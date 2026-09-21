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
// StarFall - 星坠 (坠落之星雨)
// NewEFMod (tefkernel / KernelLoader) 重写版，适配 Terraria 1.4.5.x (PC / PE)
//
// 功能:
//   夜晚原版每下落一颗坠落之星(ProjectileID.FallingStar = 12)时,
//   额外复制 299 颗(共 300 倍), 形成密集流星雨。
//
// 实现要点(结合 PC 源码 Projectile.cs 与 PE dump.cs):
//   1. 坠落之星(类型 12)是弹幕。原版夜晚由弹幕 720(FallingStarSpawner)
//      飞行一段时间后在自身位置生成类型 12(Projectile.cs AI_148_StarSpawner),
//      因此所有坠落之星的创建最终都经过 Projectile.NewProjectile 的
//      float 重载(13 参)。
//   2. 本 Mod 完全仿照 MultiProjectile(多倍弹幕)的做法:
//      Hook Projectile.NewProjectile(float 重载, 13 参) 的 Postfix,
//      当 args[5](Type) == 12 且 Owner 为本地玩家时, 读取原始参数,
//      把速度按扇形角度小幅旋转后用同一方法句柄再调用 299 次, 生成 300 倍星星。
//      - 只处理本地玩家的弹幕;
//      - 只复制坠落之星(type==12), 其余弹幕一律不动;
//      - 用 g_duplicating 做重入保护, 避免副本再次触发 Postfix 造成无限递归。
//   3. 由于原版只在夜晚生成 720 -> 12, 本 Mod 自然只会在夜晚生效。
//
// 注意:
//   本 Mod 与「多倍弹幕 (MultiProjectile)」都会复制弹幕, 同时启用会相乘,
//   导致星星数量爆炸并可能触顶 1000 弹幕上限。不建议同时启用。
//
// 与 ClassicEFMod 版的差异(NewAPI):
//   - 入口从 CreateMod() 变为 create_kernel_mod(), 返回 kernel_mod_ops_t 操作表;
//   - Hook 从 registerFunctionDescriptor 变为 patchlib_install_prepost_hook;
//   - 字段访问用 patchlib_field_get_pointer 直接取真实指针(Android),
//     桌面端用 patchlib_field_get_value;
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
#include <math.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

// ============ 配置 ============
#define kFallingStarType   12       // ProjectileID.FallingStar
#define kExtraCopies       299      // 每颗坠落之星额外复制 299 份 => 总共 300 倍
#define kFanSpreadRadians  1.2f     // 复制体速度扇形总张角(弧度, 约 ±34°)
#define kProjArgs          13       // float 重载 NewProjectile 参数个数

// ============ 状态 ============
static patch_hook_id_t g_hookNewProjectile = PATCH_HOOK_INVALID_ID;
static bool g_ready = false;        // 初始化是否完成
static bool g_duplicating = false;  // 正在复制副本(重入保护)
static bool g_loggedOnce = false;   // 复制日志只打一次

// ============ 类型句柄 ============
static patch_handle_t g_main_type = NULL;
static patch_handle_t g_projectile_type = NULL;

// ============ 方法句柄 ============
static patch_handle_t g_newProjectile_method = NULL;  // Projectile.NewProjectile (float 重载, 13 参)

// ============ 本地玩家解析(平台相关) ============
#if defined(__ANDROID__)
static int (*g_get_myPlayer)(void);            // Main.get_myPlayer (getter 方法指针, 静态)
#else
static patch_handle_t g_myPlayer_field = NULL; // Main.myPlayer (static int 字段)
#endif

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

// ============ Hook: Projectile.NewProjectile (float 重载, 13 参) postfix ============
static void NewProjectile_Postfix(patch_handle_t instance, void **args, void *result,
                                  const patch_method_signature_t *sig_info) {
    (void)instance; (void)sig_info;
    if (!g_ready || g_duplicating) return;
    if (!args || !result) return;

    // args[5] = int Type; 只处理坠落之星
    const int type = *(int*)args[5];
    if (type != kFallingStarType) return;

    // args[8] = int Owner; 仅本地玩家的坠落之星
    const int owner = *(int*)args[8];
    const int myPlayer = LocalPlayer();
    if (myPlayer < 0 || owner != myPlayer) return;

    // args[3]/args[4] = SpeedX/SpeedY
    const float sx = *(float*)args[3];
    const float sy = *(float*)args[4];
    if (sx * sx + sy * sy <= 0.0001f) return;  // 无初速不散射复制

    g_duplicating = true;

    for (int j = 0; j < kExtraCopies; ++j) {
        // 对称扇形: 复制体速度在 ±kFanSpreadRadians/2 范围内均匀展开
        const float angle = ((float)j - (float)(kExtraCopies - 1) * 0.5f) /
                            (float)(kExtraCopies - 1) * kFanSpreadRadians;
        const float c = cosf(angle);
        const float s = sinf(angle);
        const float nsx = sx * c - sy * s;
        const float nsy = sx * s + sy * c;

        // 复制参数表, 只替换速度
        void* dup_args[kProjArgs];
        for (int k = 0; k < kProjArgs; ++k) dup_args[k] = args[k];
        dup_args[3] = (void*)&nsx;
        dup_args[4] = (void*)&nsy;

        int unused = 0;
        if (!patchlib_method_invoke_args(g_newProjectile_method, NULL, &unused, dup_args)) break;
    }

    g_duplicating = false;

    if (!g_loggedOnce) {
        g_loggedOnce = true;
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_INFO, "StarFall",
                             "300x 星坠已生效 (owner=%d)", owner);
        }
    }
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "StarFall", "初始化星坠模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "StarFall", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 类型
    g_main_type = patchlib_type_get_type("Terraria", "Main");
    g_projectile_type = patchlib_type_get_type("Terraria", "Projectile");
    if (!g_main_type || !g_projectile_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "StarFall", "获取类型失败 (Main/Projectile)");
        }
        return;
    }

    // 2. 本地玩家解析
#if defined(__ANDROID__)
    patch_handle_t myPlayer_prop = patchlib_type_get_property(g_main_type, "myPlayer");
    if (myPlayer_prop) {
        patch_handle_t getter = patchlib_property_get_get_method(myPlayer_prop);
        if (getter) g_get_myPlayer = (int (*)(void))patchlib_method_get_pointer(getter);
    }
    if (!g_get_myPlayer) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "StarFall", "解析 Main.myPlayer getter 失败");
        }
        return;
    }
#else
    g_myPlayer_field = patchlib_type_get_field(g_main_type, "myPlayer");
    if (!g_myPlayer_field) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "StarFall", "获取 Main.myPlayer 字段失败");
        }
        return;
    }
#endif

    // 3. 方法: Projectile.NewProjectile 的 float 重载(13 参)
    g_newProjectile_method = patchlib_type_get_method_by_param_count(g_projectile_type, "NewProjectile", kProjArgs);
    if (!g_newProjectile_method) {
        g_newProjectile_method = patchlib_type_get_method(g_projectile_type, "NewProjectile");
    }
    if (!g_newProjectile_method) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "StarFall", "获取 NewProjectile 方法失败");
        }
        return;
    }

    // 4. 安装 postfix Hook
    g_hookNewProjectile = patchlib_install_prepost_hook(g_newProjectile_method, NULL, NewProjectile_Postfix);
    if (g_hookNewProjectile == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "StarFall", "安装 NewProjectile Hook 失败");
        }
        return;
    }

    g_ready = true;
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "StarFall",
                         "成功 Hook NewProjectile (hook_id=%d), 星坠已启用 (300x)", (int)g_hookNewProjectile);
    }
}

// ============ 模块清理 ============
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;

    if (g_hookNewProjectile != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookNewProjectile);
        g_hookNewProjectile = PATCH_HOOK_INVALID_ID;
    }

    g_ready = false;
    g_duplicating = false;
    g_loggedOnce = false;

#if defined(__ANDROID__)
    g_get_myPlayer = NULL;
#else
    g_myPlayer_field = NULL;
#endif

    g_newProjectile_method = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "StarFall", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.starfall",
        .version_code = 4,
        .api_version = 1,
        .version = "2.0.0",
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
        mod_logger_write(MOD_LOG_LEVEL_INFO, "StarFall", "星坠模组实例创建");
    }
    return &g_ops;
}
