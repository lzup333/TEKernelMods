//
// AutoFisher - 自动钓鱼
// NewEFMod (tefkernel / KernelLoader) 重写版，适配 Terraria 1.4.5.x (手机端 / PE)
//
// 功能:
//   1. 自动收杆: 鱼上钩(浮标 ai[1] 变为负数)后自动收线, 无需点击。
//   2. 自动甩杆: 手持鱼竿且没有浮标在场上时自动抛竿(进入钓鱼状态后生效)。
//   3. 循环钓鱼: 两者结合实现全自动挂机钓鱼。
//
// 实现要点(结合 PE 1.4.5.6.4 dump.cs 与 PC 1.4.5.6 源码):
//   1. 上钩语义: 浮标等待计时结束后调用 Projectile.FishingCheck() 掷判定。
//      成功上钩时会把 ai[1] 置为负数、localAI[1] 存捕获内容(物品为正、NPC 为负)
//      (Projectile.cs:19333/19354)。随后浮标 AI 会逐帧把 ai[1] 加 1~5,
//      等到 ai[1] 归零意味着鱼挣脱(需要尽快收线)。
//      因此"鱼已上钩"的判定是: ai[0]==0 && ai[1]<0 && localAI[1]!=0。
//   2. 收杆: 玩家收线时 Player.ItemCheck -> ItemCheck_PullFishingBobbers 会把
//      ai[1] 转正、浮标飞回玩家身边并在 Kill 时把鱼交给玩家。
//      因此本 Mod 在检测到上钩后, 在 ItemCheck 的 Prefix 阶段把
//      Player.controlUseItem 置为 true, 原版 ItemCheck 会自然执行一次"收线+交鱼"。
//   3. 抛竿: 手持鱼竿且场上没有浮标时, 同样的"模拟按键"会让原版抛出新浮标。
//   4. 钓鱼状态: 玩家手动抛过一次竿(场上出现自己的浮标)后进入自动循环,
//      切走鱼竿则退出。
//
// 与 ClassicEFMod 版的差异(NewAPI):
//   - 入口从 CreateMod() 变为 create_kernel_mod(), 返回 kernel_mod_ops_t 操作表;
//   - Hook 从 registerFunctionDescriptor(替换转发函数) 变为
//     patchlib_install_prepost_hook(libffi 闭包), 在 ItemCheck 的 Prefix 阶段
//     直接改写 controlUseItem, 让原版自然执行抛竿/收线, 无需重入原函数;
//   - 字段访问用 patchlib_field_get_pointer 直接取真实指针(Android),
//     浮标 ai/localAI 是内联 struct, get_pointer 返回其存储地址, 可直接当 float* 用;
//   - 重要: 本 tefkernel 版本中 prefix 返回值语义与头文件注释相反
//     (实测 false = 执行原方法, true = 跳过原方法), 因此 prefix 必须返回 false;
//   - 日志改用 mod_logger_write。
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
static patch_hook_id_t g_hook_id = PATCH_HOOK_INVALID_ID;
static bool g_ready = false;       // 初始化是否完成
static bool g_fishing = false;     // 自动钓鱼会话是否激活
static int  g_castCooldown = 0;    // 抛竿冷却帧数

// ============ 类型句柄(仅初始化时用于获取成员) ============
static patch_handle_t g_main_type = NULL;
static patch_handle_t g_player_type = NULL;
static patch_handle_t g_item_type = NULL;
static patch_handle_t g_projectile_type = NULL;
static patch_handle_t g_entity_type = NULL;

// ============ 字段句柄 ============
static patch_handle_t g_whoAmI_field = NULL;          // Entity.whoAmI        (int)
static patch_handle_t g_inventory_field = NULL;       // Player.inventory     (Item[])
static patch_handle_t g_fishingPole_field = NULL;     // Item.fishingPole     (int)
static patch_handle_t g_controlUseItem_field = NULL;  // Player.controlUseItem(bool)
static patch_handle_t g_itemAnimation_field = NULL;   // Player.itemAnimation (int)
static patch_handle_t g_projectile_field = NULL;      // Main.projectile      (Projectile[], static)
static patch_handle_t g_active_field = NULL;          // Projectile.active    (bool)
static patch_handle_t g_owner_field = NULL;           // Projectile.owner     (int)
static patch_handle_t g_aiStyle_field = NULL;         // Projectile.aiStyle   (int)
static patch_handle_t g_ai_field = NULL;              // Projectile.ai        (Float_FixedArray_3)
static patch_handle_t g_localAI_field = NULL;         // Projectile.localAI   (Float_FixedArray_3)

// ============ 方法/属性解析(平台相关) ============
#if defined(__ANDROID__)
static int (*g_get_myPlayer)(void);              // Main.get_myPlayer      (static int)
static int (*g_get_selectedItem)(void* player);  // Player.get_selectedItem(int)
#else
static patch_handle_t g_myPlayer_field = NULL;   // Main.myPlayer          (static int 字段)
static patch_handle_t g_selectedItem_getter = NULL; // Player.get_selectedItem (getter 方法句柄)
#endif

// ============ 字段读取工具(跨平台) ============

/** 读取本地玩家编号; 失败返回 -1 */
static int LocalPlayer(void) {
#if defined(__ANDROID__)
    if (!g_get_myPlayer) return -1;
    return g_get_myPlayer();
#else
    int v = -1;
    if (g_myPlayer_field) patchlib_field_get_value(g_myPlayer_field, NULL, &v);
    return v;
#endif
}

/** 读取玩家实例的 whoAmI; 失败返回 -1 */
static int PlayerWhoAmI(void* player) {
    if (!g_whoAmI_field || !player) return -1;
#if defined(__ANDROID__)
    int* p = (int*)patchlib_field_get_pointer(g_whoAmI_field, player);
    return p ? *p : -1;
#else
    int v = -1;
    patchlib_field_get_value(g_whoAmI_field, player, &v);
    return v;
#endif
}

/** 读取手持物品栏位索引; 失败返回 -1 */
static int SelectedItemIndex(void* player) {
#if defined(__ANDROID__)
    if (!g_get_selectedItem || !player) return -1;
    return g_get_selectedItem(player);
#else
    if (!g_selectedItem_getter || !player) return -1;
    int v = -1;
    if (!patchlib_method_invoke_args(g_selectedItem_getter, player, &v, NULL)) return -1;
    return v;
#endif
}

/** 读取玩家物品栏数组对象; 失败返回 NULL */
static void* PlayerInventoryArray(void* player) {
    if (!g_inventory_field || !player) return NULL;
#if defined(__ANDROID__)
    void** storage = (void**)patchlib_field_get_pointer(g_inventory_field, player);
    return storage ? *storage : NULL;
#else
    void* arr = NULL;
    patchlib_field_get_value(g_inventory_field, player, &arr);
    return arr;
#endif
}

/** 读取物品的钓鱼竿等级; 失败返回 0 */
static int ItemFishingPole(void* item) {
    if (!g_fishingPole_field || !item) return 0;
#if defined(__ANDROID__)
    int* p = (int*)patchlib_field_get_pointer(g_fishingPole_field, item);
    return p ? *p : 0;
#else
    int v = 0;
    patchlib_field_get_value(g_fishingPole_field, item, &v);
    return v;
#endif
}

/** 把玩家 controlUseItem 置为 true; 失败返回 false */
static bool SetControlUseItem(void* player) {
    if (!g_controlUseItem_field || !player) return false;
#if defined(__ANDROID__)
    bool* p = (bool*)patchlib_field_get_pointer(g_controlUseItem_field, player);
    if (!p) return false;
    *p = true;
#else
    bool v = true;
    patchlib_field_set_value(g_controlUseItem_field, player, &v);
#endif
    return true;
}

/** 玩家是否正在播放使用动画; 失败返回 false */
static bool IsAnimating(void* player) {
    if (!g_itemAnimation_field || !player) return false;
#if defined(__ANDROID__)
    int* p = (int*)patchlib_field_get_pointer(g_itemAnimation_field, player);
    return p && *p > 0;
#else
    int v = 0;
    patchlib_field_get_value(g_itemAnimation_field, player, &v);
    return v > 0;
#endif
}

/** 读取 Main.projectile 数组对象; 失败返回 NULL */
static void* MainProjectileArray(void) {
    if (!g_projectile_field) return NULL;
#if defined(__ANDROID__)
    void** storage = (void**)patchlib_field_get_pointer(g_projectile_field, NULL);
    return storage ? *storage : NULL;
#else
    void* arr = NULL;
    patchlib_field_get_value(g_projectile_field, NULL, &arr);
    return arr;
#endif
}

/** 读取弹幕 int 字段; 失败返回 0 */
static int ProjFieldInt(patch_handle_t field, void* proj) {
    if (!field || !proj) return 0;
#if defined(__ANDROID__)
    int* p = (int*)patchlib_field_get_pointer(field, proj);
    return p ? *p : 0;
#else
    int v = 0;
    patchlib_field_get_value(field, proj, &v);
    return v;
#endif
}

/** 读取弹幕 bool 字段; 失败返回 false */
static bool ProjFieldBool(patch_handle_t field, void* proj) {
    if (!field || !proj) return false;
#if defined(__ANDROID__)
    bool* p = (bool*)patchlib_field_get_pointer(field, proj);
    return p ? *p : false;
#else
    bool v = false;
    patchlib_field_get_value(field, proj, &v);
    return v;
#endif
}

/**
 * 读取弹幕 ai/localAI 数组第 idx 个 float。
 * Android: ai/localAI 是内联 struct, get_pointer 返回其存储地址;
 * 桌面端: 是 float[] 托管数组。
 */
static bool ProjAiFloat(void* proj, patch_handle_t aiField, size_t idx, float* out) {
    if (!proj || !aiField || !out) return false;
#if defined(__ANDROID__)
    float* p = (float*)patchlib_field_get_pointer(aiField, proj);
    if (!p) return false;
    *out = p[idx];
    return true;
#else
    void* arr = NULL;
    patchlib_field_get_value(aiField, proj, &arr);
    if (!arr) return false;
    return patchlib_array_at(arr, idx, out);
#endif
}

/**
 * 扫描本地玩家的浮标, 统计数量并检测是否有鱼上钩。
 * @param myPlayer 本地玩家编号
 * @param outCount  输出的浮标数量
 * @param outBite   输出的上钩标志(ai[0]==0 && ai[1]<0 && localAI[1]!=0)
 */
static void ScanLocalBobbers(int myPlayer, int* outCount, bool* outBite) {
    *outCount = 0;
    *outBite = false;
    if (!g_aiStyle_field) return;

    void* arr = MainProjectileArray();
    if (!arr) return;

    const size_t n = patchlib_array_length(arr);
    for (size_t i = 0; i < n; ++i) {
        void* proj = NULL;
        if (!patchlib_array_at(arr, i, &proj) || !proj) continue;

        // 先按 aiStyle(浮标为 61)过滤, 绝大多数弹幕不是浮标, 尽早跳过
        if (ProjFieldInt(g_aiStyle_field, proj) != 61) continue;

        // 归属检查
        if (g_owner_field) {
            if (ProjFieldInt(g_owner_field, proj) != myPlayer) continue;
        }
        // active 检查(浮标死掉后字段会被重置, 这里作为双保险)
        if (g_active_field) {
            if (!ProjFieldBool(g_active_field, proj)) continue;
        }

        (*outCount)++;

        // 上钩检查
        if (g_ai_field && g_localAI_field) {
            float ai0 = 0.0f, ai1 = 0.0f, lai1 = 0.0f;
            if (ProjAiFloat(proj, g_ai_field, 0, &ai0) &&
                ProjAiFloat(proj, g_ai_field, 1, &ai1) &&
                ProjAiFloat(proj, g_localAI_field, 1, &lai1) &&
                ai0 == 0.0f && ai1 < 0.0f && lai1 != 0.0f) {
                *outBite = true;
            }
        }
    }
}

// ============ Hook: Player.ItemCheck (Prefix) ============
// 在 ItemCheck 执行前判定: 需要抛竿/收杆时把 controlUseItem 置 true,
// 让原版 ItemCheck 自然执行一次抛竿或收线+交鱼。prefix 必须返回 false(执行原方法)。
static bool ItemCheck_Prefix(patch_handle_t instance, void **args,
                             const patch_method_signature_t *sig_info, void *result) {
    (void)args; (void)sig_info; (void)result;
    if (!g_ready || !instance) return false;

    // 仅本地玩家
    const int myPlayer = LocalPlayer();
    if (myPlayer < 0) return false;
    if (PlayerWhoAmI(instance) != myPlayer) return false;

    // 读取手持物品, 判断是否为鱼竿
    const int sel = SelectedItemIndex(instance);
    if (sel < 0) return false;
    void* inv = PlayerInventoryArray(instance);
    if (!inv || (size_t)sel >= patchlib_array_length(inv)) return false;
    void* item = NULL;
    if (!patchlib_array_at(inv, (size_t)sel, &item) || !item) return false;
    if (ItemFishingPole(item) <= 0) {
        // 手上没有鱼竿: 结束自动钓鱼会话
        g_fishing = false;
        return false;
    }

    // 扫描自己的浮标
    int bobberCount = 0;
    bool hasBite = false;
    ScanLocalBobbers(myPlayer, &bobberCount, &hasBite);

    if (bobberCount > 0) {
        g_fishing = true;  // 玩家开始钓鱼了

        // 鱼已上钩: 模拟按下使用键, 原版 ItemCheck 会自动收线并交鱼
        if (hasBite && !IsAnimating(instance)) {
            SetControlUseItem(instance);
        }
        return false;
    }

    // 没有浮标。只有进入钓鱼状态后才自动抛竿(避免走路时乱甩)
    if (!g_fishing) return false;

    // 冷却 + 使用动画保护, 避免重复抛竿
    if (g_castCooldown > 0) { --g_castCooldown; return false; }
    g_castCooldown = 15;
    if (IsAnimating(instance)) return false;

    SetControlUseItem(instance);

    // false = 正常执行原方法(反向语义)
    return false;
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "AutoFisher", "初始化自动钓鱼模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "AutoFisher", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 获取类型
    g_main_type = patchlib_type_get_type("Terraria", "Main");
    g_player_type = patchlib_type_get_type("Terraria", "Player");
    g_item_type = patchlib_type_get_type("Terraria", "Item");
    g_projectile_type = patchlib_type_get_type("Terraria", "Projectile");
    g_entity_type = patchlib_type_get_type("Terraria", "Entity");
    if (!g_main_type || !g_player_type || !g_item_type || !g_projectile_type || !g_entity_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "AutoFisher",
                             "获取类型失败 (Main/Player/Item/Projectile/Entity)");
        }
        return;
    }

    // 2. 属性/字段解析
#if defined(__ANDROID__)
    patch_handle_t myPlayer_prop = patchlib_type_get_property(g_main_type, "myPlayer");
    if (myPlayer_prop) {
        patch_handle_t getter = patchlib_property_get_get_method(myPlayer_prop);
        if (getter) g_get_myPlayer = (int (*)(void))patchlib_method_get_pointer(getter);
    }
    patch_handle_t selectedItem_prop = patchlib_type_get_property(g_player_type, "selectedItem");
    if (selectedItem_prop) {
        patch_handle_t getter = patchlib_property_get_get_method(selectedItem_prop);
        if (getter) g_get_selectedItem = (int (*)(void*))patchlib_method_get_pointer(getter);
    }
#else
    // 桌面端: Main.myPlayer 是静态字段; Player.selectedItem 是属性 getter
    g_myPlayer_field = patchlib_type_get_field(g_main_type, "myPlayer");
    patch_handle_t selectedItem_prop = patchlib_type_get_property(g_player_type, "selectedItem");
    if (selectedItem_prop) {
        g_selectedItem_getter = patchlib_property_get_get_method(selectedItem_prop);
    }
#endif

    // 3. 字段
    g_whoAmI_field = patchlib_type_get_field(g_entity_type, "whoAmI");
    g_inventory_field = patchlib_type_get_field(g_player_type, "inventory");
    g_fishingPole_field = patchlib_type_get_field(g_item_type, "fishingPole");
    g_controlUseItem_field = patchlib_type_get_field(g_player_type, "controlUseItem");
    g_itemAnimation_field = patchlib_type_get_field(g_player_type, "itemAnimation");
    g_projectile_field = patchlib_type_get_field(g_main_type, "projectile");
    g_active_field = patchlib_type_get_field(g_projectile_type, "active");
    g_owner_field = patchlib_type_get_field(g_projectile_type, "owner");
    g_aiStyle_field = patchlib_type_get_field(g_projectile_type, "aiStyle");
    g_ai_field = patchlib_type_get_field(g_projectile_type, "ai");
    g_localAI_field = patchlib_type_get_field(g_projectile_type, "localAI");

#if defined(__ANDROID__)
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "AutoFisher",
                         "myPlayer=%p selectedItem=%p whoAmI=%p inventory=%p pole=%p use=%p anim=%p",
                         (void*)g_get_myPlayer, (void*)g_get_selectedItem,
                         (void*)g_whoAmI_field, (void*)g_inventory_field,
                         (void*)g_fishingPole_field, (void*)g_controlUseItem_field,
                         (void*)g_itemAnimation_field);
        mod_logger_write(MOD_LOG_LEVEL_INFO, "AutoFisher",
                         "projectile=%p active=%p owner=%p aiStyle=%p ai=%p localAI=%p",
                         (void*)g_projectile_field, (void*)g_active_field,
                         (void*)g_owner_field, (void*)g_aiStyle_field,
                         (void*)g_ai_field, (void*)g_localAI_field);
    }
#endif

    // 4. 获取 Player.ItemCheck 方法(每帧调用, 0 参数)
    patch_handle_t itemcheck_method = patchlib_type_get_method(g_player_type, "ItemCheck");
    if (!itemcheck_method) {
        itemcheck_method = patchlib_type_get_method_by_param_count(g_player_type, "ItemCheck", 0);
    }
    if (!itemcheck_method) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "AutoFisher", "获取 ItemCheck 方法失败");
        }
        return;
    }

    // 5. 安装前缀 Hook
    g_hook_id = patchlib_install_prepost_hook(itemcheck_method, ItemCheck_Prefix, NULL);
    if (g_hook_id == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "AutoFisher", "安装 ItemCheck Hook 失败");
        }
        return;
    }

    g_ready = true;
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "AutoFisher",
                         "成功 Hook ItemCheck (hook_id=%d), 自动钓鱼已启用", (int)g_hook_id);
    }
}

// ============ 模块清理 ============
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;

    if (g_hook_id != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hook_id);
        g_hook_id = PATCH_HOOK_INVALID_ID;
    }

    g_ready = false;
    g_fishing = false;
    g_castCooldown = 0;

#if defined(__ANDROID__)
    g_get_myPlayer = NULL;
    g_get_selectedItem = NULL;
#else
    g_myPlayer_field = NULL;
    g_selectedItem_getter = NULL;
#endif

    g_whoAmI_field = NULL;
    g_inventory_field = NULL;
    g_fishingPole_field = NULL;
    g_controlUseItem_field = NULL;
    g_itemAnimation_field = NULL;
    g_projectile_field = NULL;
    g_active_field = NULL;
    g_owner_field = NULL;
    g_aiStyle_field = NULL;
    g_ai_field = NULL;
    g_localAI_field = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "AutoFisher", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.autofisher",
        .version_code = 1,
        .api_version = 1,
        .version = "1.0.0",
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
        mod_logger_write(MOD_LOG_LEVEL_INFO, "AutoFisher", "自动钓鱼模组实例创建");
    }
    return &g_ops;
}
