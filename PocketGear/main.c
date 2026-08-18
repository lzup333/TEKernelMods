//
// PocketGear - 口袋装备
// NewEFMod (tefkernel / KernelLoader) 重写版，适配 Terraria 1.4.5.x (手机端 / PE)
// 原版作者: 雨鹜 https://github.com/2099123771/PocketGear
//
// 功能: 使物品栏以及各种背包(银行/保险箱/护卫保险箱/虚空保险库)中的
//       饰品与装备效果生效(翅膀除外)
//
// 注: 手机端(1.4.5.6.4)中 Player.bank/bank2/bank3/bank4 的类型是
//     InventoryStorage (内含 Item[] item 字段)，而非 PC 端的 Chest。
//
// 与 ClassicEFMod 版的差异(NewAPI):
//   - 入口从 CreateMod() 变为 create_kernel_mod(), 返回 kernel_mod_ops_t 操作表;
//   - Hook 从 registerFunctionDescriptor(替换转发函数) 变为
//     patchlib_install_prepost_hook(libffi 闭包), 使用 postfix 在
//     Player.ResetEffects 原版执行完毕后重新应用装备效果;
//   - 字段访问用 patchlib_field_get_pointer 直接取真实指针(Android);
//   - 方法调用用 patchlib_method_get_pointer 取原生函数指针直接调用
//     (il2cpp 实例方法第一个参数即 this);
//   - 日志改用 mod_logger_write。
//

#include "mod-api/mod_core.h"
#include "mod-api/mod_logger.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/struct/array.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

// ============ 状态 ============
static patch_hook_id_t g_hook_id = PATCH_HOOK_INVALID_ID;
static bool g_ready = false;

// ============ 类型句柄 ============
static patch_handle_t g_player_type = NULL;
static patch_handle_t g_storage_type = NULL;

// ============ 字段句柄 ============
static patch_handle_t g_inventory_field = NULL;   // Player.inventory      (Item[])
static patch_handle_t g_bank_field = NULL;        // Player.bank           (InventoryStorage/Chest)
static patch_handle_t g_bank2_field = NULL;       // Player.bank2          (InventoryStorage/Chest)
static patch_handle_t g_bank3_field = NULL;       // Player.bank3          (InventoryStorage/Chest)
static patch_handle_t g_bank4_field = NULL;       // Player.bank4          (InventoryStorage/Chest)
static patch_handle_t g_storageItem_field = NULL; // InventoryStorage.item / Chest.item (Item[])

// ============ 装备方法(跨平台解析) ============
static patch_handle_t g_applyEquip_method = NULL;   // Player.ApplyEquipFunctional(int, Item)
static patch_handle_t g_grantPrefix_method = NULL;  // Player.GrantPrefixBenefits(Item)
static patch_handle_t g_grantArmor_method = NULL;   // Player.GrantArmorBenefits(Item)

#if defined(__ANDROID__)
static void (*g_applyEquipFunc)(void* player, int itemSlot, void* currentItem) = NULL;
static void (*g_grantPrefixBonus)(void* player, void* item) = NULL;
static void (*g_grantArmorBonus)(void* player, void* item) = NULL;
#endif

/**
 * 读取对象字段(引用类型)并返回对象指针; 失败返回 NULL
 */
static void* GetObjField(patch_handle_t field, void* instance) {
    if (!field) return NULL;
#if defined(__ANDROID__)
    void** slot = (void**)patchlib_field_get_pointer(field, instance);
    return slot ? *slot : NULL;
#else
    void* v = NULL;
    patchlib_field_get_value(field, instance, &v);
    return v;
#endif
}

/** 三个装备方法是否都已解析 */
static bool EquipMethodsReady(void) {
#if defined(__ANDROID__)
    return g_applyEquipFunc && g_grantPrefixBonus && g_grantArmorBonus;
#else
    return g_applyEquip_method && g_grantPrefix_method && g_grantArmor_method;
#endif
}

/** 调用 ApplyEquipFunctional / GrantPrefixBenefits / GrantArmorBenefits */
static void CallEquipMethods(void* player, int eqSlot, void* item) {
    if (!player || !item) return;
#if defined(__ANDROID__)
    g_applyEquipFunc(player, eqSlot, item);
    g_grantPrefixBonus(player, item);
    g_grantArmorBonus(player, item);
#else
    int slot = eqSlot;
    void* args[2];
    args[0] = &slot;
    args[1] = &item;
    patchlib_method_invoke_args(g_applyEquip_method, player, NULL, args);
    void* args2[1];
    args2[0] = &item;
    patchlib_method_invoke_args(g_grantPrefix_method, player, NULL, args2);
    patchlib_method_invoke_args(g_grantArmor_method, player, NULL, args2);
#endif
}

/**
 * 处理单个物品栏
 * @param player       玩家实例
 * @param pItems       物品数组 (Item[])
 * @param skipLastSlot 是否跳过最后一个槽位
 *                     (主背包第58格是鼠标/垃圾槽, 不参与装备效果)
 */
static void ProcessInventory(void* player, void* pItems, bool skipLastSlot) {
    if (!player || !pItems) return;
    if (!EquipMethodsReady()) return;

    size_t count = patchlib_array_length(pItems);
    if (count == 0) return;
    if (skipLastSlot && count > 0) count -= 1;

    for (size_t i = 0; i < count; ++i) {
        void* pItem = NULL;
        if (!patchlib_array_at(pItems, i, &pItem) || !pItem) continue;

        // ApplyEquipFunctional 内部会访问 hideVisibleAccessory[itemSlot] (长度10)
        // 背包/银行槽位索引可能越界导致崩溃, 需限制在合法范围内
        int eqSlot = (int)i;
        if (eqSlot < 0) eqSlot = 0;
        if (eqSlot > 9) eqSlot = 9;

        CallEquipMethods(player, eqSlot, pItem);
    }
}

/**
 * 应用口袋装备效果
 * 处理主背包以及所有银行容器中的装备
 */
static void ApplyPocketEffects(void* player) {
    if (!g_ready || !player) return;
    if (!EquipMethodsReady()) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "PocketGear", "methods not resolved, skip");
        }
        return;
    }

    // 主背包 (跳过鼠标槽)
    ProcessInventory(player, GetObjField(g_inventory_field, player), true);

    // 各类银行容器 (Android: Player.bank 是 InventoryStorage; 桌面端: Chest)
    ProcessInventory(player, GetObjField(g_storageItem_field, GetObjField(g_bank_field, player)), false);
    ProcessInventory(player, GetObjField(g_storageItem_field, GetObjField(g_bank2_field, player)), false);
    ProcessInventory(player, GetObjField(g_storageItem_field, GetObjField(g_bank3_field, player)), false);
    ProcessInventory(player, GetObjField(g_storageItem_field, GetObjField(g_bank4_field, player)), false);
}

// ============ Hook: Player.ResetEffects (Postfix) ============
// ResetEffects 每帧会清空/重置装备效果, 因此必须在原版执行完毕后
// 重新把背包里的饰品/装备效果应用上。
static void ResetEffects_Postfix(patch_handle_t instance, void **args, void *result,
                                 const patch_method_signature_t *sig_info) {
    (void)args; (void)result; (void)sig_info;
    ApplyPocketEffects(instance);
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "PocketGear", "初始化口袋装备模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "PocketGear", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 获取类型 (Android: 银行容器为 InventoryStorage; 桌面端: Chest)
    g_player_type = patchlib_type_get_type("Terraria", "Player");
#if defined(__ANDROID__)
    g_storage_type = patchlib_type_get_type("Terraria", "InventoryStorage");
#else
    g_storage_type = patchlib_type_get_type("Terraria", "Chest");
#endif
    if (!g_player_type || !g_storage_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "PocketGear",
                             "获取类型失败 (Player/InventoryStorage)");
        }
        return;
    }

    // 2. 字段
    g_inventory_field = patchlib_type_get_field(g_player_type, "inventory");
    g_bank_field = patchlib_type_get_field(g_player_type, "bank");
    g_bank2_field = patchlib_type_get_field(g_player_type, "bank2");
    g_bank3_field = patchlib_type_get_field(g_player_type, "bank3");
    g_bank4_field = patchlib_type_get_field(g_player_type, "bank4");
    g_storageItem_field = patchlib_type_get_field(g_storage_type, "item");
    if (!g_inventory_field || !g_bank_field || !g_bank2_field ||
        !g_bank3_field || !g_bank4_field || !g_storageItem_field) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "PocketGear", "获取背包字段失败");
        }
        return;
    }

    // 3. 装备方法(跨平台解析方法句柄; Android 额外取原生函数指针)
    g_applyEquip_method = patchlib_type_get_method_by_param_count(g_player_type, "ApplyEquipFunctional", 2);
    g_grantPrefix_method = patchlib_type_get_method_by_param_count(g_player_type, "GrantPrefixBenefits", 1);
    g_grantArmor_method = patchlib_type_get_method_by_param_count(g_player_type, "GrantArmorBenefits", 1);
    if (!g_applyEquip_method) g_applyEquip_method = patchlib_type_get_method(g_player_type, "ApplyEquipFunctional");
    if (!g_grantPrefix_method) g_grantPrefix_method = patchlib_type_get_method(g_player_type, "GrantPrefixBenefits");
    if (!g_grantArmor_method) g_grantArmor_method = patchlib_type_get_method(g_player_type, "GrantArmorBenefits");

#if defined(__ANDROID__)
    g_applyEquipFunc = (void (*)(void*, int, void*))patchlib_method_get_pointer(g_applyEquip_method);
    g_grantPrefixBonus = (void (*)(void*, void*))patchlib_method_get_pointer(g_grantPrefix_method);
    g_grantArmorBonus = (void (*)(void*, void*))patchlib_method_get_pointer(g_grantArmor_method);
#endif

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "PocketGear",
                         "equip=%p prefix=%p armor=%p",
                         (void*)g_applyEquip_method, (void*)g_grantPrefix_method,
                         (void*)g_grantArmor_method);
    }

    // 4. 获取 Player.ResetEffects 方法(每帧调用, 0 参数)
    patch_handle_t reset_method = patchlib_type_get_method_by_param_count(g_player_type, "ResetEffects", 0);
    if (!reset_method) {
        reset_method = patchlib_type_get_method(g_player_type, "ResetEffects");
    }
    if (!reset_method) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "PocketGear", "获取 ResetEffects 方法失败");
        }
        return;
    }

    // 5. 安装后缀 Hook(原版执行完后再应用口袋装备效果)
    g_hook_id = patchlib_install_prepost_hook(reset_method, NULL, ResetEffects_Postfix);
    if (g_hook_id == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "PocketGear", "安装 ResetEffects Hook 失败");
        }
        return;
    }

    g_ready = true;
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "PocketGear",
                         "成功 Hook ResetEffects (hook_id=%d), 口袋装备已启用", (int)g_hook_id);
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

    g_applyEquip_method = NULL;
    g_grantPrefix_method = NULL;
    g_grantArmor_method = NULL;

#if defined(__ANDROID__)
    g_applyEquipFunc = NULL;
    g_grantPrefixBonus = NULL;
    g_grantArmorBonus = NULL;
#endif

    g_inventory_field = NULL;
    g_bank_field = NULL;
    g_bank2_field = NULL;
    g_bank3_field = NULL;
    g_bank4_field = NULL;
    g_storageItem_field = NULL;
    g_storage_type = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "PocketGear", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.pocketgear",
        .version_code = 1,
        .api_version = 1,
        .version = "1.2.0",
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
        mod_logger_write(MOD_LOG_LEVEL_INFO, "PocketGear", "口袋装备模组实例创建");
    }
    return &g_ops;
}
