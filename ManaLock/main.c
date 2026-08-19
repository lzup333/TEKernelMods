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
// ManaLock - 魔力锁定
// NewEFMod (tefkernel / KernelLoader) 重写版，适配 Terraria 1.4.5.x (手机端 / PE)
//
// 功能: 每帧把玩家的当前魔力值锁定为最大值, 实现无限魔力。
//
// 与 ClassicEFMod 版的差异(NewAPI):
//   - 入口从 CreateMod() 变为 create_kernel_mod(), 返回 kernel_mod_ops_t 操作表;
//   - Hook 从 registerFunctionDescriptor(替换转发函数) 变为
//     patchlib_install_prepost_hook(libffi 闭包), 使用 postfix 在
//     Player.ResetEffects 原版执行完毕后再把 statMana 置为 statManaMax
//     (ResetEffects 每帧会把 statMana 恢复为合法状态, 因此必须后置);
//   - 字段访问用 patchlib_field_get_pointer 直接取真实指针(Android),
//     避免在每帧热路径上反复调用 il2cpp 运行时函数;
//   - 日志改用 mod_logger_write。
//

#include "mod-api/mod_core.h"
#include "mod-api/mod_logger.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/field.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

// ============ 状态 ============
static patch_hook_id_t g_hook_id = PATCH_HOOK_INVALID_ID;
static bool g_ready = false;

// ============ 类型/字段句柄 ============
static patch_handle_t g_player_type = NULL;
static patch_handle_t g_statMana_field = NULL;     // Player.statMana    (int)
static patch_handle_t g_statManaMax_field = NULL;  // Player.statManaMax (int)

// ============ Hook: Player.ResetEffects (Postfix) ============
// ResetEffects 每帧把当前魔力恢复为合法状态, 因此必须在原版执行完毕后
// 再把 statMana 置为 statManaMax。
static void ResetEffects_Postfix(patch_handle_t instance, void **args, void *result,
                                 const patch_method_signature_t *sig_info) {
    (void)args; (void)result; (void)sig_info;
    if (!g_ready || !instance) return;

#if defined(__ANDROID__)
    int* pMana = (int*)patchlib_field_get_pointer(g_statMana_field, instance);
    int* pMax  = (int*)patchlib_field_get_pointer(g_statManaMax_field, instance);
    if (pMana && pMax) {
        *pMana = *pMax;
    }
#else
    int maxMana = 0;
    patchlib_field_get_value(g_statManaMax_field, instance, &maxMana);
    patchlib_field_set_value(g_statMana_field, instance, &maxMana);
#endif
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ManaLock", "初始化魔力锁定模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ManaLock", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 获取类型
    g_player_type = patchlib_type_get_type("Terraria", "Player");
    if (!g_player_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ManaLock", "获取 Player 类型失败");
        }
        return;
    }

    // 2. 字段
    g_statMana_field = patchlib_type_get_field(g_player_type, "statMana");
    g_statManaMax_field = patchlib_type_get_field(g_player_type, "statManaMax");
    if (!g_statMana_field || !g_statManaMax_field) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ManaLock",
                             "获取 statMana/statManaMax 字段失败");
        }
        return;
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ManaLock",
                         "statMana=%p statManaMax=%p",
                         (void*)g_statMana_field, (void*)g_statManaMax_field);
    }

    // 3. 获取 Player.ResetEffects 方法(每帧调用, 0 参数)
    patch_handle_t reset_method = patchlib_type_get_method_by_param_count(g_player_type, "ResetEffects", 0);
    if (!reset_method) {
        reset_method = patchlib_type_get_method(g_player_type, "ResetEffects");
    }
    if (!reset_method) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ManaLock", "获取 ResetEffects 方法失败");
        }
        return;
    }

    // 4. 安装后缀 Hook(原版执行完后再锁定魔力)
    g_hook_id = patchlib_install_prepost_hook(reset_method, NULL, ResetEffects_Postfix);
    if (g_hook_id == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ManaLock", "安装 ResetEffects Hook 失败");
        }
        return;
    }

    g_ready = true;
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ManaLock",
                         "成功 Hook ResetEffects (hook_id=%d), 魔力锁定已启用", (int)g_hook_id);
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
    g_statMana_field = NULL;
    g_statManaMax_field = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ManaLock", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.manalock",
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
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ManaLock", "魔力锁定模组实例创建");
    }
    return &g_ops;
}
