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
// MultiProjectile - 多倍弹幕 (多重射击)
// NewEFMod (tefkernel / KernelLoader) 重写版，适配 Terraria 1.4.5.x (PC / PE)
//
// 功能:
//   玩家手持武器射击(产生弹幕)时, 每一发弹幕额外复制 N-1 份(默认 5 倍),
//   复制弹道按小角度扇形展开(散射), 让每次射击打出多颗弹幕。
//
// 实现要点(结合 PC 源码 Projectile.cs 与 PE dump.cs):
//   1. 射击路径: 所有武器弹幕最终都经由 Projectile.NewProjectile 创建。
//      PC/PE 端存在两个重载:
//        - NewProjectile(IEntitySource, Vector2, Vector2, int, int, float, int, ...)   (11 参)
//        - NewProjectile(IEntitySource, float, float, float, float, int, int, float, ...) (13 参)
//      其中 Vector2 重载内部委托给 float 重载(Projectile.cs:10194), 因此
//      只 Hook float 重载即可覆盖所有弹幕创建, 不会重复复制。
//   2. 过滤条件(仅复制"玩家+武器弹幕"):
//        - Owner 必须为本地玩家(Main.myPlayer), 排除 NPC/环境弹幕;
//        - 排除非武器弹幕: 钓鱼浮标(bobber), 钩爪(aiStyle==7),
//          召唤物(minion) 与 哨兵(sentry)。
//   3. 复制方式: 在 float 重载的 postfix 里读取原始参数(起点/速度/类型/伤害等),
//      把速度向量按 -~+ 角度旋转后, 用同一个方法句柄再次调用 NewProjectile 生成副本。
//      副本的 Owner / 类型 / 伤害 / 击退 / ai 均与原弹幕一致。
//   4. 重入保护: 复制时置 g_duplicating, 副本再次触发 postfix 时直接返回,
//      避免无限递归。无论 patchlib_method_invoke_args 走的是钩子还是原函数,
//      该保护都能保证安全。
//
// 与 ClassicEFMod 版的差异(NewAPI):
//   - 入口从 CreateMod() 变为 create_kernel_mod(), 返回 kernel_mod_ops_t 操作表;
//   - Hook 从 registerFunctionDescriptor(替换转发函数) 变为
//     patchlib_install_prepost_hook(libffi 闭包), postfix 读取 args/result;
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
#include <string.h>
#include <math.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

// ============ 配置 ============
#define kExtraCopies 4                 // 每发额外复制 4 份 => 总共 5 倍
#define kSpreadStep  0.13f             // 相邻复制弹道夹角(弧度, 约 7.4°), 总扇形约 ±11°
#define kProjArgs    13                // float 重载 NewProjectile 参数个数

// ============ 状态 ============
static patch_hook_id_t g_hookNewProjectile = PATCH_HOOK_INVALID_ID;
static bool g_ready = false;           // 初始化是否完成
static bool g_duplicating = false;     // 正在复制副本(重入保护)
static bool g_loggedOnce = false;      // 复制日志只打一次

// ============ 类型句柄 ============
static patch_handle_t g_main_type = NULL;
static patch_handle_t g_projectile_type = NULL;

// ============ 方法句柄 ============
static patch_handle_t g_newProjectile_method = NULL;  // Projectile.NewProjectile (float 重载, 13 参)

// ============ 字段句柄 ============
static patch_handle_t g_mainProjectile_field = NULL;  // Main.projectile   (Projectile[], 静态)
static patch_handle_t g_aiStyle_field = NULL;         // Projectile.aiStyle (int)
static patch_handle_t g_minion_field = NULL;          // Projectile.minion  (bool)
static patch_handle_t g_sentry_field = NULL;          // Projectile.sentry  (bool)
static patch_handle_t g_bobber_field = NULL;          // Projectile.bobber  (bool)

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

/** 读取静态引用字段(引用类型)返回对象指针; 失败返回 NULL */
static void* GetStaticObjField(patch_handle_t field) {
    if (!field) return NULL;
#if defined(__ANDROID__)
    void** slot = (void**)patchlib_field_get_pointer(field, NULL);
    return slot ? *slot : NULL;
#else
    void* v = NULL;
    patchlib_field_get_value(field, NULL, &v);
    return v;
#endif
}

/** 读取弹幕 int 字段; 失败返回 0 */
static int ProjInt(patch_handle_t field, void* proj) {
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
static bool ProjBool(patch_handle_t field, void* proj) {
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

/** 从 Main.projectile 取指定索引的弹幕对象; 失败返回 NULL */
static void* GetProjectileAt(int idx) {
    void* arr = GetStaticObjField(g_mainProjectile_field);
    if (!arr || idx < 0) return NULL;
    void* proj = NULL;
    if (!patchlib_array_at(arr, (size_t)idx, &proj)) return NULL;
    return proj;
}

/**
 * 判断该弹幕是否为"武器弹幕"(可被复制)。
 * 过滤掉非武器来源的玩家弹幕: 钓鱼浮标 / 钩爪 / 召唤物 / 哨兵。
 */
static bool IsMultiplyCandidate(void* proj) {
    if (g_bobber_field && ProjBool(g_bobber_field, proj)) return false;  // 钓鱼浮标
    if (g_minion_field && ProjBool(g_minion_field, proj)) return false;  // 召唤物
    if (g_sentry_field && ProjBool(g_sentry_field, proj)) return false;  // 哨兵
    if (g_aiStyle_field && ProjInt(g_aiStyle_field, proj) == 7) return false;  // 钩爪
    return true;
}

// ============ Hook: Projectile.NewProjectile (float 重载, 13 参) postfix ============
static void NewProjectile_Postfix(patch_handle_t instance, void **args, void *result,
                                  const patch_method_signature_t *sig_info) {
    (void)instance; (void)sig_info;
    if (!g_ready || g_duplicating) return;
    if (!args || !result) return;

    // args[8] = int Owner
    const int owner = *(int*)args[8];
    const int myPlayer = LocalPlayer();
    if (myPlayer < 0 || owner != myPlayer) return;  // 仅本地玩家弹幕

    // result 为弹幕索引, 通过 Main.projectile 取对象做非武器过滤
    const int idx = *(int*)result;
    void* proj = GetProjectileAt(idx);
    if (!proj || !IsMultiplyCandidate(proj)) return;

    // args[3]/args[4] = SpeedX/SpeedY
    const float sx = *(float*)args[3];
    const float sy = *(float*)args[4];
    const float mag2 = sx * sx + sy * sy;
    if (mag2 <= 0.0001f) return;  // 无初速的弹幕(如原地生成的)不散射复制

    g_duplicating = true;

    for (int j = 0; j < kExtraCopies; ++j) {
        // 对称扇形: 偏移角 (j - (N-1)/2) * step, N=4 => -1.5,-0.5,+0.5,+1.5
        const float angle = ((float)j - (float)(kExtraCopies - 1) * 0.5f) * kSpreadStep;
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
            mod_logger_write(MOD_LOG_LEVEL_INFO, "MultiProjectile",
                             "5x 多倍弹幕已生效 (owner=%d type=%d)", owner, *(int*)args[5]);
        }
    }
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "MultiProjectile", "初始化多倍弹幕模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "MultiProjectile", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 类型
    g_main_type = patchlib_type_get_type("Terraria", "Main");
    g_projectile_type = patchlib_type_get_type("Terraria", "Projectile");
    if (!g_main_type || !g_projectile_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "MultiProjectile", "获取类型失败 (Main/Projectile)");
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
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "MultiProjectile", "解析 Main.myPlayer getter 失败");
        }
        return;
    }
#else
    g_myPlayer_field = patchlib_type_get_field(g_main_type, "myPlayer");
    if (!g_myPlayer_field) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "MultiProjectile", "获取 Main.myPlayer 字段失败");
        }
        return;
    }
#endif

    // 3. 字段
    g_mainProjectile_field = patchlib_type_get_field(g_main_type, "projectile");
    g_aiStyle_field = patchlib_type_get_field(g_projectile_type, "aiStyle");
    g_minion_field = patchlib_type_get_field(g_projectile_type, "minion");
    g_sentry_field = patchlib_type_get_field(g_projectile_type, "sentry");
    g_bobber_field = patchlib_type_get_field(g_projectile_type, "bobber");
    if (!g_mainProjectile_field || !g_aiStyle_field || !g_minion_field ||
        !g_sentry_field || !g_bobber_field) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "MultiProjectile", "获取字段失败");
        }
        return;
    }

    // 4. 方法: Projectile.NewProjectile 的 float 重载(13 参)
    g_newProjectile_method = patchlib_type_get_method_by_param_count(g_projectile_type, "NewProjectile", kProjArgs);
    if (!g_newProjectile_method) {
        g_newProjectile_method = patchlib_type_get_method(g_projectile_type, "NewProjectile");
    }
    if (!g_newProjectile_method) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "MultiProjectile", "获取 NewProjectile 方法失败");
        }
        return;
    }

    // 5. 安装 postfix Hook
    g_hookNewProjectile = patchlib_install_prepost_hook(g_newProjectile_method, NULL, NewProjectile_Postfix);
    if (g_hookNewProjectile == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "MultiProjectile", "安装 NewProjectile Hook 失败");
        }
        return;
    }

    g_ready = true;
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "MultiProjectile",
                         "成功 Hook NewProjectile (hook_id=%d), 多倍弹幕已启用 (5x)", (int)g_hookNewProjectile);
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

    g_mainProjectile_field = NULL;
    g_aiStyle_field = NULL;
    g_minion_field = NULL;
    g_sentry_field = NULL;
    g_bobber_field = NULL;
    g_newProjectile_method = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "MultiProjectile", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.multiprojectile",
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
        mod_logger_write(MOD_LOG_LEVEL_INFO, "MultiProjectile", "多倍弹幕模组实例创建");
    }
    return &g_ops;
}
