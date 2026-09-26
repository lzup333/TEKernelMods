//
// Copyright (C) 2026 lzup333
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

//
// Vampire - 吸血鬼
// NewEFMod (tefkernel / KernelLoader) 版, 适配 Terraria 1.4.5.x (手机端 / PE)
//
// 功能:
//   每次攻击敌对生物, 有 5% 概率回复 20 点生命。
//   不改变生命上限, 不做任何持久化。
//
// 实现要点(结合 PC 1.4.5.8 源码与 PE 1.4.5.6.4 dump.cs):
//
//   1. 命中判定: Hook Terraria.NPC.StrikeNPC(int Damage, float knockBack,
//      int hitDirection, bool crit, bool fromNet, int owner) 的 Postfix。
//      - 第 6 个参数 owner 为攻击者玩家编号(玩家/弹幕/召唤物攻击会传入,
//        环境伤害默认 -1, 网络同步为 255), 据此只认"玩家本人造成的命中";
//      - 返回值(Postfix 的 result)为本次实际造成的伤害, <=0 说明未生效
//        (目标已死/免疫等), 跳过。
//
//   2. 只对"敌对生物"生效: NPC.friendly == false && NPC.townNPC == false
//      && NPC.lifeMax > 5 (排除小动物/小虫), 避免攻击城镇 NPC 刷血。
//
//   3. 概率: 每次命中独立掷 5% (使用标准库 rand(), 初始化时用时间播种)。
//
//   4. 回血: 调用游戏自带的 Player.Heal(int amount) (Player.cs:35391)。
//      该方法内部即 statLife += amount, 并在本地玩家上触发 HealEffect
//      (绿色回血数字), 且自动钳制到 statLifeMax2 —— 正是生命之心被拾取时
//      (Player.PickupItem -> Heal(20)) 所走的同一条路径。
//      比直接改 statLife 更正确, 也自带视觉反馈。
//
// Hook 一览:
//   - Terraria.NPC.StrikeNPC  (Postfix) 命中掷概率 + Player.Heal
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
#include <time.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

// ============ 配置 ============
#define VAMPIRE_CHANCE   5    // 触发概率(百分比)
#define VAMPIRE_HEAL    20    // 触发时回复的生命值

// ============ 状态 ============
static patch_hook_id_t g_hookStrike = PATCH_HOOK_INVALID_ID;  // NPC.StrikeNPC

// ============ 类型句柄 ============
static patch_handle_t g_main_type = NULL;
static patch_handle_t g_player_type = NULL;
static patch_handle_t g_npc_type = NULL;

// ============ 方法/字段句柄 ============
#if defined(__ANDROID__)
static int  (*g_get_myPlayer)(void) = NULL;        // Main.get_myPlayer (static int 属性)
static void (*g_heal)(void*, int) = NULL;          // Player.Heal (实例, 1 参)
#else
static patch_handle_t g_myPlayer_field = NULL;     // Main.myPlayer (static int 字段)
static patch_handle_t g_heal_method = NULL;        // Player.Heal (实例, 1 参)
#endif
static patch_handle_t g_mainPlayer_field = NULL;   // Main.player (static Player[])
static patch_handle_t g_npc_lifeMax_field = NULL;  // NPC.lifeMax  (int)
static patch_handle_t g_npc_friendly_field = NULL; // NPC.friendly (bool)
static patch_handle_t g_npc_townNPC_field = NULL;  // NPC.townNPC  (bool)

// ============ 工具函数 ============

/** 获取本地玩家编号; 失败返回 -1 */
static int LocalPlayerId(void) {
#if defined(__ANDROID__)
    return g_get_myPlayer ? g_get_myPlayer() : -1;
#else
    int v = -1;
    if (g_myPlayer_field) patchlib_field_get_value(g_myPlayer_field, NULL, &v);
    return v;
#endif
}

/** 获取本地玩家实例; 失败返回 NULL */
static void* LocalPlayer(void) {
    const int my = LocalPlayerId();
    if (my < 0 || !g_mainPlayer_field) return NULL;
    void* arr = NULL;
#if defined(__ANDROID__)
    void** slot = (void**)patchlib_field_get_pointer(g_mainPlayer_field, NULL);
    if (!slot || !*slot) return NULL;
    arr = *slot;
#else
    patchlib_field_get_value(g_mainPlayer_field, NULL, &arr);
#endif
    if (!arr) return NULL;
    if ((size_t)my >= patchlib_array_length(arr)) return NULL;
    void* p = NULL;
    if (!patchlib_array_at(arr, (size_t)my, &p)) return NULL;
    return p;
}

/** 读取实例 int 字段; 失败返回 false */
static bool InstGetInt(patch_handle_t field, void* instance, int* out) {
    if (!field || !instance || !out) return false;
#if defined(__ANDROID__)
    int* p = (int*)patchlib_field_get_pointer(field, instance);
    if (!p) return false;
    *out = *p;
    return true;
#else
    patchlib_field_get_value(field, instance, out);
    return true;
#endif
}

/** 读取实例 bool 字段; 失败返回 false */
static bool InstGetBool(patch_handle_t field, void* instance, bool* out) {
    if (!field || !instance || !out) return false;
#if defined(__ANDROID__)
    bool* p = (bool*)patchlib_field_get_pointer(field, instance);
    if (!p) return false;
    *out = *p;
    return true;
#else
    patchlib_field_get_value(field, instance, out);
    return true;
#endif
}

/** 调用 Player.Heal(amount) 回复本地玩家生命 (游戏自带方法, 含回血特效) */
static void HealPlayer(void* player, int amount) {
    if (!player) return;
#if defined(__ANDROID__)
    if (g_heal) g_heal(player, amount);
#else
    if (!g_heal_method) return;
    int a = amount;
    void* args[1];
    args[0] = &a;
    patchlib_method_invoke_args(g_heal_method, player, NULL, args);
#endif
}

// ============ Postfix: NPC.StrikeNPC ============
// 参数: (int Damage, float knockBack, int hitDirection, bool crit, bool fromNet, int owner)
// result: 本次实际造成的伤害
static void StrikeNPC_Postfix(patch_handle_t instance, void **args, void *result,
                              const patch_method_signature_t *sig_info) {
    (void)sig_info;
    if (!instance || !args || !result) return;

    const int dealt = *(int*)result;
    if (dealt <= 0) return;                       // 未造成实际伤害(早退/免疫), 跳过

    const int owner = *(int*)args[5];             // 攻击者玩家编号
    const int my = LocalPlayerId();
    if (my < 0 || owner != my) return;            // 仅玩家本人造成的命中

    // 只对敌对生物生效
    bool friendly = true, townNPC = true;
    if (!InstGetBool(g_npc_friendly_field, instance, &friendly) || friendly) return;
    if (!InstGetBool(g_npc_townNPC_field, instance, &townNPC) || townNPC) return;

    int lifeMax = 0;
    if (!InstGetInt(g_npc_lifeMax_field, instance, &lifeMax)) return;
    if (lifeMax <= 5) return;                     // 排除小动物/小虫

    // 5% 概率触发
    if ((rand() % 100) >= VAMPIRE_CHANCE) return;

    void* p = LocalPlayer();
    if (!p) return;
    HealPlayer(p, VAMPIRE_HEAL);

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_DEBUG, "Vampire",
                         "吸血触发: Player.Heal(%d)", VAMPIRE_HEAL);
    }
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "Vampire", "初始化吸血鬼模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "Vampire", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    srand((unsigned)time(NULL));                  // 概率用随机数播种

    g_main_type = patchlib_type_get_type("Terraria", "Main");
    g_player_type = patchlib_type_get_type("Terraria", "Player");
    g_npc_type = patchlib_type_get_type("Terraria", "NPC");
    if (!g_main_type || !g_player_type || !g_npc_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "Vampire",
                             "获取类型失败 (Main/Player/NPC)");
        }
        return;
    }

    // 本地玩家编号 (Android 为静态属性, 桌面端为静态字段)
#if defined(__ANDROID__)
    patch_handle_t myPlayer_prop = patchlib_type_get_property(g_main_type, "myPlayer");
    if (myPlayer_prop) {
        patch_handle_t getter = patchlib_property_get_get_method(myPlayer_prop);
        if (getter) g_get_myPlayer = (int (*)(void))patchlib_method_get_pointer(getter);
    }
#else
    g_myPlayer_field = patchlib_type_get_field(g_main_type, "myPlayer");
#endif
    g_mainPlayer_field = patchlib_type_get_field(g_main_type, "player");

    // Player.Heal(int amount)
    patch_handle_t heal = patchlib_type_get_method_by_param_count(g_player_type, "Heal", 1);
    if (!heal) heal = patchlib_type_get_method(g_player_type, "Heal");
#if defined(__ANDROID__)
    if (heal) g_heal = (void (*)(void*, int))patchlib_method_get_pointer(heal);
#else
    g_heal_method = heal;
#endif

    // NPC 字段
    g_npc_lifeMax_field = patchlib_type_get_field(g_npc_type, "lifeMax");
    g_npc_friendly_field = patchlib_type_get_field(g_npc_type, "friendly");
    g_npc_townNPC_field = patchlib_type_get_field(g_npc_type, "townNPC");

    if (mod_logger_write) {
#if defined(__ANDROID__)
        mod_logger_write(MOD_LOG_LEVEL_INFO, "Vampire",
                         "heal=%p | npc lifeMax=%p friendly=%p town=%p",
                         (void*)g_heal, (void*)g_npc_lifeMax_field,
                         (void*)g_npc_friendly_field, (void*)g_npc_townNPC_field);
#else
        mod_logger_write(MOD_LOG_LEVEL_INFO, "Vampire",
                         "heal=%p | npc lifeMax=%p friendly=%p town=%p",
                         (void*)g_heal_method, (void*)g_npc_lifeMax_field,
                         (void*)g_npc_friendly_field, (void*)g_npc_townNPC_field);
#endif
    }

    // Hook: NPC.StrikeNPC (6 参重载) -> 命中吸血
    patch_handle_t strike = patchlib_type_get_method_by_param_count(g_npc_type, "StrikeNPC", 6);
    if (strike) {
        g_hookStrike = patchlib_install_prepost_hook(strike, NULL, StrikeNPC_Postfix);
    }
    if (g_hookStrike == PATCH_HOOK_INVALID_ID && mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "Vampire", "Hook NPC.StrikeNPC 失败");
    }
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "Vampire", "Hooks: strike=%d", (int)g_hookStrike);
    }
}

// ============ 模块清理 ============
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;

    if (g_hookStrike != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookStrike);
        g_hookStrike = PATCH_HOOK_INVALID_ID;
    }

#if defined(__ANDROID__)
    g_get_myPlayer = NULL;
    g_heal = NULL;
#else
    g_myPlayer_field = NULL;
    g_heal_method = NULL;
#endif
    g_mainPlayer_field = NULL;
    g_npc_lifeMax_field = NULL;
    g_npc_friendly_field = NULL;
    g_npc_townNPC_field = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "Vampire", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.vampire",
        .version_code = 5,
        .api_version = 1,
        .version = "1.3.0",
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
        mod_logger_write(MOD_LOG_LEVEL_INFO, "Vampire", "吸血鬼模组实例创建");
    }
    return &g_ops;
}
