#include "mod-api/mod_core.h"
#include "mod-api/mod_logger.h"
#include "stddef.h"
#include "stdlib.h"
#include "time.h"
#include "tefkernel/patchlib/method.h"

//Code is cheap,Show your prompt.
void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;
// 全局补丁句柄
static patch_handle_t GetWeaponDamage_method = 0;

// 模块信息

static kernel_mod_info_t g_mod_info = {
    .pkg_id = "lzup.player.random_damage",  // 修改为random_damage
    .version_code = 1,
    .api_version = 1,
    .version = "1.0.0",
};

// 随机数生成器初始化标志
static int random_initialized = 0;

// 初始化随机数生成器
static void init_random(void) {
    if (!random_initialized) {
        srand((unsigned int)time(NULL));
        random_initialized = 1;
    }
}

// 生成指定范围内的随机伤害值
static int generate_random_damage(int min_damage, int max_damage) {


    // 生成[min_damage, max_damage]范围内的随机整数
    int range = max_damage - min_damage + 1;
    return min_damage + (rand() % range);
}

// GetWeaponDamage 方法的钩子函数
void GetWeaponDamage_hook(patch_handle_t instance, void **args, void *result,
                          const patch_method_signature_t *sig_info) {
    if (result) {
        // 初始化随机数生成器
        init_random();

        // 修改返回值
        int* pResult = (int*)result;

        // 生成随机伤害值（例如：1-100之间的随机数）
        int originalDamage = *pResult;
        int randomDamage = generate_random_damage(1, 100);
        (void)originalDamage;

        *pResult = randomDamage;
    }
}

// 模块初始化
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomDamage", "初始化随机伤害模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomDamage", "私有目录: %s",
                         handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 获取Player类型
    patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");
    if (!player_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "RandomDamage", "获取Player类型失败");
        }
        return;
    }

    // 2. 获取GetWeaponDamage方法 - 带1个参数（Item）
    GetWeaponDamage_method = patchlib_type_get_method_by_param_count(player_type, "GetWeaponDamage", 1);

    if (!GetWeaponDamage_method) {
        // 如果参数数量匹配失败，尝试通过名称获取
        GetWeaponDamage_method = patchlib_type_get_method(player_type, "GetWeaponDamage");
    }

    if (!GetWeaponDamage_method) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "RandomDamage", "获取GetWeaponDamage方法失败");
        }
        patchlib_free(player_type);
        return;
    }

    // 3. 安装后置钩子
    patchlib_install_prepost_hook(GetWeaponDamage_method, NULL, GetWeaponDamage_hook);

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomDamage",
                        "成功Hook GetWeaponDamage方法，武器伤害将变为随机值");
    }

    // 4. 释放类型句柄
    patchlib_free(player_type);
}

// 模块清理
static void cleanup_mod(kernel_mod_handle_t* handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomDamage", "清理模组");
    }

    // 释放资源
    if (GetWeaponDamage_method) {
        patchlib_free(GetWeaponDamage_method);
        GetWeaponDamage_method = 0;
    }

    random_initialized = 0;
}

// 获取模块信息
static kernel_mod_info_t* get_info(void) {
    return &g_mod_info;
}

// 模块操作函数表
static kernel_mod_ops_t g_ops = {
    .init_mod = init_mod,
    .cleanup_mod = cleanup_mod,
    .get_info = get_info
};

// 模块入口函数
kernel_mod_ops_t* create_kernel_mod(void) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomDamage", "随机伤害模组实例创建");
    }
    return &g_ops;
}