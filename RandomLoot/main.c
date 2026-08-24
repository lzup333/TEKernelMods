//
// OneHitLoot - 一击必爆
// NewEFMod (tefkernel / KernelLoader) 重写版，适配 Terraria 1.4.5.x (手机端 / PE)
//
// 功能: 怪物受击时额外掉落随机物品 (全局修改所有怪物)
//       每次受击必定额外掉落一件随机物品
//       物品ID完全随机 (1~6144范围), 所有物品都有可能出现
//       跳过城镇NPC (friendly/townNPC)
//
// 实现:
//   Hook NPC.checkDead() Postfix, 用 Player.QuickSpawnItem 生成随机物品
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
#include <stdlib.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

static patch_hook_id_t g_hook_id = PATCH_HOOK_INVALID_ID;

static patch_handle_t g_npc_type = NULL;
static patch_handle_t g_entity_type = NULL;
static patch_handle_t g_main_type = NULL;
static patch_handle_t g_player_type = NULL;

static patch_handle_t g_active_field = NULL;
static patch_handle_t g_friendly_field = NULL;
static patch_handle_t g_townNPC_field = NULL;
static patch_handle_t g_player_field = NULL;

#if defined(__ANDROID__)
static int (*g_get_myPlayer)(void) = NULL;
#else
static patch_handle_t g_myPlayer_field = NULL;
#endif

static patch_handle_t g_quickSpawnItem_method = NULL;
#if defined(__ANDROID__)
static void (*g_quickSpawnItem_func)(void*, void*, int, int) = NULL;
#endif

static const int kMinItemId = 1;
static const int kMaxItemId = 6144;

static int GetMyPlayer(void) {
#if defined(__ANDROID__)
    if (!g_get_myPlayer) return -1;
    return g_get_myPlayer();
#else
    if (!g_myPlayer_field) return -1;
    int v = -1;
    patchlib_field_get_value(g_myPlayer_field, NULL, &v);
    return v;
#endif
}

static void* GetPlayerObj(void) {
    int my = GetMyPlayer();
    if (my < 0) return NULL;
    void* arr = NULL;
    patchlib_field_get_value(g_player_field, NULL, &arr);
    if (!arr) return NULL;
    void* player = NULL;
    patchlib_array_at(arr, (size_t)my, &player);
    return player;
}

static bool ReadFieldBool(patch_handle_t field, void* obj) {
    if (!field || !obj) return false;
    bool v = false;
    patchlib_field_get_value(field, obj, &v);
    return v;
}

static void checkDead_Postfix(patch_handle_t instance, void **args,
                              void *result, const patch_method_signature_t *sig_info) {
    (void)args; (void)result; (void)sig_info;

    if (!instance) return;

#if defined(__ANDROID__)
    if (!g_quickSpawnItem_func) return;
#else
    if (!g_quickSpawnItem_method) return;
#endif

    if (ReadFieldBool(g_friendly_field, instance)) return;
    if (ReadFieldBool(g_townNPC_field, instance)) return;

    void* player = GetPlayerObj();
    if (!player) return;

    int itemId = kMinItemId + (rand() % (kMaxItemId - kMinItemId + 1));
    int stack = 1;

#if defined(__ANDROID__)
    void* null_src = NULL;
    g_quickSpawnItem_func(player, null_src, itemId, stack);
#else
    void* invoke_args[3];
    void* null_src = NULL;
    invoke_args[0] = &null_src;
    invoke_args[1] = &itemId;
    invoke_args[2] = &stack;
    patchlib_method_invoke_args(g_quickSpawnItem_method, player, NULL, invoke_args);
#endif
}

static void init_mod(kernel_mod_handle_t *handle) {
    g_npc_type = patchlib_type_get_type("Terraria", "NPC");
    g_entity_type = patchlib_type_get_type("Terraria", "Entity");
    g_main_type = patchlib_type_get_type("Terraria", "Main");
    g_player_type = patchlib_type_get_type("Terraria", "Player");
    if (!g_npc_type || !g_entity_type || !g_main_type || !g_player_type) return;

    g_active_field = patchlib_type_get_field(g_npc_type, "active");
    g_friendly_field = patchlib_type_get_field(g_npc_type, "friendly");
    g_townNPC_field = patchlib_type_get_field(g_npc_type, "townNPC");
    g_player_field = patchlib_type_get_field(g_main_type, "player");

#if defined(__ANDROID__)
    patch_handle_t myPlayer_prop = patchlib_type_get_property(g_main_type, "myPlayer");
    if (myPlayer_prop) {
        patch_handle_t getter = patchlib_property_get_get_method(myPlayer_prop);
        if (getter) g_get_myPlayer = (int (*)(void))patchlib_method_get_pointer(getter);
    }
#else
    g_myPlayer_field = patchlib_type_get_field(g_main_type, "myPlayer");
#endif

    const char* quickSpawnNames[] = { "source", "item", "stack" };
    g_quickSpawnItem_method = patchlib_type_get_method_by_param_names(
            g_player_type, "QuickSpawnItem", 3, quickSpawnNames);
    if (!g_quickSpawnItem_method) {
        g_quickSpawnItem_method = patchlib_type_get_method_by_param_count(
                g_player_type, "QuickSpawnItem", 3);
    }
    if (!g_quickSpawnItem_method) {
        g_quickSpawnItem_method = patchlib_type_get_method(g_player_type, "QuickSpawnItem");
    }
    if (!g_quickSpawnItem_method) return;

#if defined(__ANDROID__)
    g_quickSpawnItem_func = (void (*)(void*, void*, int, int))
            patchlib_method_get_pointer(g_quickSpawnItem_method);
    if (!g_quickSpawnItem_func) return;
#endif

    patch_handle_t checkdead_method = patchlib_type_get_method_by_param_count(
            g_npc_type, "checkDead", 0);
    if (!checkdead_method) {
        checkdead_method = patchlib_type_get_method(g_npc_type, "checkDead");
    }
    if (!checkdead_method) return;

    g_hook_id = patchlib_install_prepost_hook(checkdead_method, NULL, checkDead_Postfix);
}

static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;

    if (g_hook_id != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hook_id);
        g_hook_id = PATCH_HOOK_INVALID_ID;
    }

    g_active_field = NULL;
    g_friendly_field = NULL;
    g_townNPC_field = NULL;
    g_player_field = NULL;
#if defined(__ANDROID__)
    g_get_myPlayer = NULL;
#else
    g_myPlayer_field = NULL;
#endif
    g_quickSpawnItem_method = NULL;
#if defined(__ANDROID__)
    g_quickSpawnItem_func = NULL;
#endif
}

static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.randomloot",
        .version_code = 1,
        .api_version = 1,
        .version = "1.0.0",
};

static kernel_mod_info_t *get_info(void) {
    return &g_mod_info;
}

static kernel_mod_ops_t g_ops = {
        .init_mod = init_mod,
        .cleanup_mod = cleanup_mod,
        .get_info = get_info
};

kernel_mod_ops_t *create_kernel_mod(void) {
    return &g_ops;
}
