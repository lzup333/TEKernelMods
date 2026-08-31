//
// LightPenetration - 光源穿透
// NewEFMod (tefkernel / KernelLoader) 版, 适配 Terraria 1.4.5.x (手机端 / PE)
//
// 功能:
//   降低光照衰减系数, 让光源照得更远。
//   仅彩色照明模式生效(新光照引擎 LightingEngine)。
//   穿透强度由 PENETRATION_FACTOR 控制(0~1), 当前为 0.3:
//     衰减降到原来的 30%, 光照范围约扩大 3 倍。
//
// 实现:
//   - Hook LightingEngine.UpdateLightDecay (LightingEngine.cs:137) 的 Postfix;
//   - 该方法每帧把衰减系数写入工作光照图 _workingLightMap:
//     LightDecayThroughAir / LightDecayThroughSolid;
//   - LightMap 传播是乘法: light *= decay (LightMap.cs:182/196),
//     Postfix 把衰减系数改为 1 - (1 - old) * PENETRATION_FACTOR,
//     即每格光照损失缩小为原来的 0.3 倍;
//   - LightMap.LightDecayThroughAir/Solid 是自动属性,
//     用 getter 读原值、setter 写新值; _workingLightMap 每帧可能被换新,
//     所以必须每帧重新取;
//   - Postfix 不改变原方法行为, hook 失败不影响游戏运行。
//

#include "mod-api/mod_core.h"
#include "mod-api/mod_logger.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/property.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

// ============ 穿透强度(0.0 ~ 1.0, 衰减系数被缩放为原值的该倍数) ============
static const float PENETRATION_FACTOR = 0.3f;

// ============ 状态 ============
static patch_hook_id_t g_hook_id = PATCH_HOOK_INVALID_ID;

// ============ 类型句柄 ============
static patch_handle_t g_engine_type = NULL;           // Terraria.Graphics.Light.LightingEngine
static patch_handle_t g_workingLightMap_field = NULL; // LightingEngine._workingLightMap (LightMap)

// ============ 属性 getter/setter (LightMap 上的自动属性) ============
static patch_handle_t g_decayAir_getter = NULL;       // LightMap.get_LightDecayThroughAir
static patch_handle_t g_decayAir_setter = NULL;       // LightMap.set_LightDecayThroughAir
static patch_handle_t g_decaySolid_getter = NULL;     // LightMap.get_LightDecayThroughSolid
static patch_handle_t g_decaySolid_setter = NULL;     // LightMap.set_LightDecayThroughSolid

// ============ Postfix: LightingEngine.UpdateLightDecay ============
// LightMap 传播是乘法(light *= decay), decay 越接近 1 衰减越弱。
// 这里把"每格损失 (1 - decay)"缩小为原来的 PENETRATION_FACTOR 倍:
//   new = 1 - (1 - old) * PENETRATION_FACTOR
// 例如原 decay=0.91, factor=0.3 => new=0.973, 光照传播距离约扩大 3 倍。
// _workingLightMap 每帧可能被换新, 所以必须每帧重新取。
static void UpdateLightDecay_Postfix(patch_handle_t instance, void **args,
                                     void *result, const patch_method_signature_t *sig_info) {
    (void)args; (void)result; (void)sig_info;
    if (!instance) return;

    void* lightMap = NULL;
    patchlib_field_get_value(g_workingLightMap_field, instance, &lightMap);
    if (!lightMap) return;

    float old_v = 0.0f;
    if (g_decayAir_getter && g_decayAir_setter) {
        patchlib_method_invoke_args(g_decayAir_getter, lightMap, &old_v, NULL);
        float new_v = 1.0f - (1.0f - old_v) * PENETRATION_FACTOR;
        void* setter_args[1] = { &new_v };
        patchlib_method_invoke_args(g_decayAir_setter, lightMap, NULL, setter_args);
    }
    if (g_decaySolid_getter && g_decaySolid_setter) {
        patchlib_method_invoke_args(g_decaySolid_getter, lightMap, &old_v, NULL);
        float new_v = 1.0f - (1.0f - old_v) * PENETRATION_FACTOR;
        void* setter_args[1] = { &new_v };
        patchlib_method_invoke_args(g_decaySolid_setter, lightMap, NULL, setter_args);
    }
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "LightPenetration",
                         "初始化光源穿透模组 (factor=%.2f)", PENETRATION_FACTOR);
        mod_logger_write(MOD_LOG_LEVEL_INFO, "LightPenetration", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 类型与字段
    g_engine_type = patchlib_type_get_type("Terraria.Graphics.Light", "LightingEngine");
    if (!g_engine_type) {
        if (mod_logger_write)
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "LightPenetration",
                             "获取类型失败 (Terraria.Graphics.Light.LightingEngine)");
        return;
    }
    g_workingLightMap_field = patchlib_type_get_field(g_engine_type, "_workingLightMap");
    if (!g_workingLightMap_field) {
        if (mod_logger_write)
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "LightPenetration",
                             "获取 _workingLightMap 字段失败");
        return;
    }

    // 2. LightMap 的两个自动属性 getter/setter
    patch_handle_t lm_type = patchlib_type_get_type("Terraria.Graphics.Light", "LightMap");
    if (lm_type) {
        patch_handle_t p;
        p = patchlib_type_get_property(lm_type, "LightDecayThroughAir");
        if (p) {
            g_decayAir_getter = patchlib_property_get_get_method(p);
            g_decayAir_setter = patchlib_property_get_set_method(p);
        }
        p = patchlib_type_get_property(lm_type, "LightDecayThroughSolid");
        if (p) {
            g_decaySolid_getter = patchlib_property_get_get_method(p);
            g_decaySolid_setter = patchlib_property_get_set_method(p);
        }
    }
    if (!g_decayAir_getter || !g_decayAir_setter || !g_decaySolid_getter || !g_decaySolid_setter) {
        if (mod_logger_write)
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "LightPenetration",
                             "获取 LightMap 衰减属性失败: getAir=%p setAir=%p getSolid=%p setSolid=%p",
                             (void*)g_decayAir_getter, (void*)g_decayAir_setter,
                             (void*)g_decaySolid_getter, (void*)g_decaySolid_setter);
        return;
    }

    // 3. Hook LightingEngine.UpdateLightDecay(每帧调用, 0 参数)
    patch_handle_t m = patchlib_type_get_method(g_engine_type, "UpdateLightDecay");
    if (!m) m = patchlib_type_get_method_by_param_count(g_engine_type, "UpdateLightDecay", 0);
    if (!m) {
        if (mod_logger_write)
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "LightPenetration",
                             "获取 UpdateLightDecay 方法失败");
        return;
    }

    g_hook_id = patchlib_install_prepost_hook(m, NULL, UpdateLightDecay_Postfix);
    if (g_hook_id == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write)
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "LightPenetration",
                             "安装 UpdateLightDecay Hook 失败");
        return;
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "LightPenetration",
                         "成功 Hook UpdateLightDecay (hook_id=%d), 光源穿透已启用",
                         (int)g_hook_id);
    }
}

// ============ 模块清理 ============
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;

    if (g_hook_id != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hook_id);
        g_hook_id = PATCH_HOOK_INVALID_ID;
    }

    g_engine_type = NULL;
    g_workingLightMap_field = NULL;
    g_decayAir_getter = NULL;
    g_decayAir_setter = NULL;
    g_decaySolid_getter = NULL;
    g_decaySolid_setter = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "LightPenetration", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.lightpenetration",
        .version_code = 2,
        .api_version = 1,
        .version = "1.1.0",
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
        mod_logger_write(MOD_LOG_LEVEL_INFO, "LightPenetration", "光源穿透模组实例创建");
    }
    return &g_ops;
}
