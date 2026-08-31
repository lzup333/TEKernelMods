//
// NightVision - 完全夜视
// NewEFMod (tefkernel / KernelLoader) 版, 适配 Terraria 1.4.5.x (手机端 / PE)
//
// 功能:
//   全图完全明亮 (Full Bright): 无论地表黑夜、洞穴多深, 画面与白天无异。
//   原版夜视(夜视头盔/药水)只把光照衰减乘 1.03, 效果微弱;
//   本 Mod 直接把光照引擎的"衰减系数"置 0, 光在空气中/固体中不再衰减,
//   从而实现完全夜视, 且不占用饰品栏与 Buff 栏。
//
// 实现要点(结合 PE 1.4.5.6.4 dump.cs 与 PC 1.4.5.6 源码):
//   游戏有两套光照引擎, 按画面光照模式选择, 两条路都处理:
//
//   1. 传统光照 LegacyLighting (Retro / 快速 / 普通模式):
//      - LegacyLighting.DoColors() (LegacyLighting.cs:845) 每帧根据环境
//        计算衰减系数 _negLight/_negLight2/_negLight3
//        (黑暗时约 0.04/0.16/0.08, LegacyLighting.cs:969-971);
//      - 光照传播时亮度按这些系数逐格衰减, 系数为 0 => 光不衰减 => 全图满亮度。
//      - Postfix DoColors() 在其执行完(已计入夜视/盲眼/黑幕等修正)后,
//        把三个字段全部写为 0。
//
//   2. 新光照引擎 LightingEngine (彩色 / 白色高级光照模式):
//      - LightingEngine.UpdateLightDecay() (LightingEngine.cs:137) 每帧把
//        衰减系数写入工作光照图 _workingLightMap (LightMap 对象):
//        LightDecayThroughAir / LightDecayThroughSolid (LightingEngine.cs:182-183 附近,
//        原版夜视仅 *1.03);
//      - LightMap 内部传播时按这两个系数逐格衰减 (LightMap.cs:182/196);
//      - Postfix UpdateLightDecay() 把实例 _workingLightMap 的
//        LightDecayThroughAir/LightDecayThroughSolid 两个属性写为 0。
//
//   3. 同时保留 Player.ResetEffects Postfix 把 nightVision 置 true:
//      原版多处(如某些 UI/怪物生成判定)读取该字段, 保持行为一致,
//      也作为两套引擎之外路径的兜底。
//
// Hook 一览:
//   - Terraria.Player.ResetEffects                       (Postfix) nightVision=true
//   - Terraria.Graphics.Light.LegacyLighting.DoColors    (Postfix) _negLight*=0
//   - Terraria.Graphics.Light.LightingEngine.UpdateLightDecay (Postfix) Decay 属性=0
//
// 注意:
//   - LightMap.LightDecayThroughAir/Solid 是自动属性(auto-property),
//     需要通过 patchlib_property_get_set_method 拿 setter 再 invoke;
//   - _negLight/_negLight2/_negLight3 与 _workingLightMap 是私有字段,
//     patchlib_type_get_field 可以按名字取到;
//   - Postfix 不改变原方法行为, 就算某条路径 hook 失败也不影响游戏运行。
//

#include "mod-api/mod_core.h"
#include "mod-api/mod_logger.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/property.h"
#include "tefkernel/patchlib/struct/array.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

// ============ 状态 ============
#define MAX_HOOKS 4
static patch_hook_id_t g_hooks[MAX_HOOKS];
static int g_hook_count = 0;
static bool g_ready = false;

// ============ 类型句柄 ============
static patch_handle_t g_player_type = NULL;         // Terraria.Player
static patch_handle_t g_entity_type = NULL;         // Terraria.Entity
static patch_handle_t g_legacy_type = NULL;         // Terraria.Graphics.Light.LegacyLighting
static patch_handle_t g_engine_type = NULL;         // Terraria.Graphics.Light.LightingEngine

// ============ 字段句柄 ============
static patch_handle_t g_nightVision_field = NULL;   // Player.nightVision (bool)
static patch_handle_t g_position_field = NULL;      // Entity.position (Vector2)
static patch_handle_t g_negLight_field = NULL;      // LegacyLighting._negLight  (float)
static patch_handle_t g_negLight2_field = NULL;     // LegacyLighting._negLight2 (float)
static patch_handle_t g_negLight3_field = NULL;     // LegacyLighting._negLight3 (float)
static patch_handle_t g_workingLightMap_field = NULL; // LightingEngine._workingLightMap (LightMap)

// ============ 方法句柄 ============
static patch_handle_t g_addlight_method = NULL;     // Lighting.AddLight(int,int,float,float,float)

// ============ 属性 setter (LightMap 上的自动属性) ============
static patch_handle_t g_decayAir_setter = NULL;     // LightMap.set_LightDecayThroughAir
static patch_handle_t g_decaySolid_setter = NULL;   // LightMap.set_LightDecayThroughSolid

static bool InstallHook(patch_handle_t method, void* prefix, void* postfix) {
    if (!method) return false;
    patch_hook_id_t id = patchlib_install_prepost_hook(method,
                        (prefix_callback_t)prefix, (postfix_callback_t)postfix);
    if (id == PATCH_HOOK_INVALID_ID) return false;
    if (g_hook_count < MAX_HOOKS) g_hooks[g_hook_count++] = id;
    return true;
}

// ============ Postfix 1: Player.ResetEffects ============
// 1. nightVision 置 true 兜底;
// 2. 在玩家所在格子注入一个白色光源(Lighting.AddLight 1,1,1):
//    "衰减=0/1(不衰减)"只保证已有光不消失, 夜晚没有任何光源时依然全黑;
//    以玩家为中心种光 + 光不衰减 => 从玩家扩散至全屏 => 彻底明亮。
static void ResetEffects_Postfix(patch_handle_t instance, void **args,
                                 void *result, const patch_method_signature_t *sig_info) {
    (void)args; (void)result; (void)sig_info;
    if (!instance) return;
    bool v = true;
    patchlib_field_set_value(g_nightVision_field, instance, &v);

    if (!g_addlight_method || !g_position_field) return;

    // 读取 Entity.position (Vector2: x,y 连续存储)
    float px = 0.0f, py = 0.0f;
#if defined(__ANDROID__)
    float* p = (float*)patchlib_field_get_pointer(g_position_field, instance);
    if (!p) return;
    px = p[0]; py = p[1];
#else
    float pos[2] = {0.0f, 0.0f};
    patchlib_field_get_value(g_position_field, instance, pos);
    px = pos[0]; py = pos[1];
#endif

    int tx = (int)(px / 16.0f);
    int ty = (int)(py / 16.0f);
    float one = 1.0f;
    void* light_args[5] = { &tx, &ty, &one, &one, &one };
    patchlib_method_invoke_args(g_addlight_method, NULL, NULL, light_args);
}

// ============ Postfix 2: LegacyLighting.DoColors ============
// DoColors 结束后衰减系数已经定稿(夜视/盲眼等修正均已计入),
// 此处统一覆盖为 0 => 光照不再随距离衰减 => 全图完全明亮。
static void DoColors_Postfix(patch_handle_t instance, void **args,
                             void *result, const patch_method_signature_t *sig_info) {
    (void)args; (void)result; (void)sig_info;
    if (!instance) return;
    float zero = 0.0f;
    patchlib_field_set_value(g_negLight_field, instance, &zero);
    patchlib_field_set_value(g_negLight2_field, instance, &zero);
    patchlib_field_set_value(g_negLight3_field, instance, &zero);
}

// ============ Postfix 3: LightingEngine.UpdateLightDecay ============
// UpdateLightDecay 结束后把工作光照图的衰减系数改为 1.0。
// 注意: LightMap 传播是乘法(light *= decay, LightMap.cs:182/196),
// decay=1.0 才是"不衰减"; 置 0 会把光照本身清零(表现为全黑)。
// _workingLightMap 每帧可能被换新, 所以必须每帧重新取。
static void UpdateLightDecay_Postfix(patch_handle_t instance, void **args,
                                     void *result, const patch_method_signature_t *sig_info) {
    (void)args; (void)result; (void)sig_info;
    if (!instance) return;

    void* lightMap = NULL;
    patchlib_field_get_value(g_workingLightMap_field, instance, &lightMap);
    if (!lightMap) return;

    float one = 1.0f;
    void* setter_args[1] = { &one };
    if (g_decayAir_setter)
        patchlib_method_invoke_args(g_decayAir_setter, lightMap, NULL, setter_args);
    if (g_decaySolid_setter)
        patchlib_method_invoke_args(g_decaySolid_setter, lightMap, NULL, setter_args);
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "NightVision", "初始化完全夜视模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "NightVision", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 类型
    g_player_type  = patchlib_type_get_type("Terraria", "Player");
    g_legacy_type  = patchlib_type_get_type("Terraria.Graphics.Light", "LegacyLighting");
    g_engine_type  = patchlib_type_get_type("Terraria.Graphics.Light", "LightingEngine");
    if (!g_player_type) {
        if (mod_logger_write)
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "NightVision", "获取类型失败 (Terraria.Player)");
        return;
    }
    if (!g_legacy_type && !g_engine_type) {
        if (mod_logger_write)
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "NightVision",
                             "获取光照引擎类型失败 (LegacyLighting/LightingEngine 均为空)");
        return;
    }

    // 2. Player.nightVision + ResetEffects + 种光
    g_nightVision_field = patchlib_type_get_field(g_player_type, "nightVision");

    g_entity_type = patchlib_type_get_type("Terraria", "Entity");
    if (g_entity_type) g_position_field = patchlib_type_get_field(g_entity_type, "position");

    patch_handle_t lighting_type = patchlib_type_get_type("Terraria", "Lighting");
    if (lighting_type) {
        // AddLight 有多个重载, 取 5 参数版本 (int,int,float,float,float)
        g_addlight_method = patchlib_type_get_method_by_param_count(lighting_type, "AddLight", 5);
    }
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "NightVision",
                         "AddLight=%p position=%p", (void*)g_addlight_method, (void*)g_position_field);
    }

    if (g_nightVision_field) {
        patch_handle_t m = patchlib_type_get_method(g_player_type, "ResetEffects");
        if (!m) m = patchlib_type_get_method_by_param_count(g_player_type, "ResetEffects", 0);
        if (!InstallHook(m, NULL, (void*)ResetEffects_Postfix) && mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "NightVision", "Hook ResetEffects 失败(非致命)");
        }
    } else if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_WARNING, "NightVision", "获取 nightVision 字段失败(非致命)");
    }

    // 3. 传统光照: LegacyLighting.DoColors + _negLight*
    if (g_legacy_type) {
        g_negLight_field  = patchlib_type_get_field(g_legacy_type, "_negLight");
        g_negLight2_field = patchlib_type_get_field(g_legacy_type, "_negLight2");
        g_negLight3_field = patchlib_type_get_field(g_legacy_type, "_negLight3");
        patch_handle_t m = patchlib_type_get_method(g_legacy_type, "DoColors");
        if (!m) m = patchlib_type_get_method_by_param_count(g_legacy_type, "DoColors", 0);
        if (m && g_negLight_field && g_negLight2_field && g_negLight3_field) {
            if (!InstallHook(m, NULL, (void*)DoColors_Postfix) && mod_logger_write) {
                mod_logger_write(MOD_LOG_LEVEL_WARNING, "NightVision", "Hook DoColors 失败(非致命)");
            }
        } else if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "NightVision",
                             "LegacyLighting 解析失败: method=%p f1=%p f2=%p f3=%p",
                             (void*)m, (void*)g_negLight_field, (void*)g_negLight2_field,
                             (void*)g_negLight3_field);
        }
    }

    // 4. 新光照引擎: LightingEngine.UpdateLightDecay + _workingLightMap
    if (g_engine_type) {
        g_workingLightMap_field = patchlib_type_get_field(g_engine_type, "_workingLightMap");

        // LightMap 的两个自动属性 setter
        patch_handle_t lm_type = patchlib_type_get_type("Terraria.Graphics.Light", "LightMap");
        if (lm_type) {
            patch_handle_t p;
            p = patchlib_type_get_property(lm_type, "LightDecayThroughAir");
            if (p) g_decayAir_setter = patchlib_property_get_set_method(p);
            p = patchlib_type_get_property(lm_type, "LightDecayThroughSolid");
            if (p) g_decaySolid_setter = patchlib_property_get_set_method(p);
        }

        patch_handle_t m = patchlib_type_get_method(g_engine_type, "UpdateLightDecay");
        if (!m) m = patchlib_type_get_method_by_param_count(g_engine_type, "UpdateLightDecay", 0);
        if (m && g_workingLightMap_field && g_decayAir_setter && g_decaySolid_setter) {
            if (!InstallHook(m, NULL, (void*)UpdateLightDecay_Postfix) && mod_logger_write) {
                mod_logger_write(MOD_LOG_LEVEL_WARNING, "NightVision",
                                 "Hook UpdateLightDecay 失败(非致命)");
            }
        } else if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "NightVision",
                             "LightingEngine 解析失败: method=%p map=%p setAir=%p setSolid=%p",
                             (void*)m, (void*)g_workingLightMap_field,
                             (void*)g_decayAir_setter, (void*)g_decaySolid_setter);
        }
    }

    if (g_hook_count > 0) {
        g_ready = true;
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_INFO, "NightVision",
                             "成功安装 %d 个 Hook, 完全夜视已启用", g_hook_count);
        }
    } else if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "NightVision", "所有 Hook 均安装失败");
    }
}

// ============ 模块清理 ============
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;

    for (int i = 0; i < g_hook_count; ++i) {
        if (g_hooks[i] != PATCH_HOOK_INVALID_ID) patchlib_uninstall_hook(g_hooks[i]);
    }
    g_hook_count = 0;

    g_ready = false;
    g_player_type = NULL;
    g_entity_type = NULL;
    g_legacy_type = NULL;
    g_engine_type = NULL;
    g_nightVision_field = NULL;
    g_position_field = NULL;
    g_addlight_method = NULL;
    g_negLight_field = NULL;
    g_negLight2_field = NULL;
    g_negLight3_field = NULL;
    g_workingLightMap_field = NULL;
    g_decayAir_setter = NULL;
    g_decaySolid_setter = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "NightVision", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.nightvision",
        .version_code = 2,
        .api_version = 1,
        .version = "2.1.0",
};

static kernel_mod_info_t *get_info(void) {
    return &g_mod_info;
}

// ============ 模块操作函数表 ============
static kernel_mod_ops_t g_ops = {
        .init_mod = init_mod,
        .cleanup_mod = cleanup_mod,
        .get_info = get_info
};

// ============ 模块入口函数 ============
kernel_mod_ops_t *create_kernel_mod(void) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "NightVision", "完全夜视模组实例创建");
    }
    return &g_ops;
}
