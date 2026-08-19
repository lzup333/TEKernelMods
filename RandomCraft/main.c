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
// RandomCraft - 随机合成 (随机所需材料 + 随机产物)
// NewEFMod (tefkernel / KernelLoader) 重写版，适配 Terraria 1.4.5.x (PC / PE)
//
// 功能:
//   1. 随机所需材料: 配方表构建完成后(Recipe.SetupRecipes 后置)，把每个配方的
//      材料表 requiredItem / requiredItemQuickLookup 整体随机替换为 1~3 种
//      随机原版材料 + 随机堆叠数(1~9)。合成界面显示、可合成判断与消耗逻辑
//      都会按"新材料"走。
//   2. 随机产物: Hook CraftingRequests.CreateResult 后置，每次合成时把产物
//      替换为另一个随机可合成物品(类型从产物池随机，数量固定为 1)。
//   3. 材料池 / 产物池在首次初始化时从 Main.recipe 收集，整个进程内只随机化
//      一次，避免配方列表反复变动。
//
// 说明:
//   - 产物与材料均来自原版配方表(可合成产物池 / 材料池)，不会生成不存在的物品。
//   - 材料条目为 RecipeGroup 的(itemIdOrRecipeGroup >= 1000000)会被忽略，
//     只取真实物品 id (0 < id < 1000000)。
//   - CreateResult 同时被本地合成与网络合成(请求服务器)复用，故两种模式都生效。
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
#include <stdlib.h>
#include <string.h>
#include <time.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

// ============ 状态 ============
static patch_hook_id_t g_hookSetupRecipes = PATCH_HOOK_INVALID_ID;    // Recipe.SetupRecipes
static patch_hook_id_t g_hookCreateResult = PATCH_HOOK_INVALID_ID;    // CraftingRequests.CreateResult
static bool g_initialized = false;                                    // 是否已完成一次随机化
static bool g_loggedSample = false;                                   // 产物采样日志只打一次

// ============ 类型句柄 ============
static patch_handle_t g_main_type = NULL;
static patch_handle_t g_recipe_type = NULL;
static patch_handle_t g_item_type = NULL;
static patch_handle_t g_crafting_type = NULL;

// ============ 字段句柄 ============
static patch_handle_t g_mainRecipe_field = NULL;          // Main.recipe                (Recipe[], 静态)
static patch_handle_t g_recipeCreateItem_field = NULL;    // Recipe.createItem          (Item)
static patch_handle_t g_recipeRequiredItem_field = NULL;  // Recipe.requiredItem        (Item[])
static patch_handle_t g_recipeQuickLookup_field = NULL;   // Recipe.requiredItemQuickLookup (RequiredItemEntry[])
static patch_handle_t g_itemType_field = NULL;            // Item.type                  (int)
static patch_handle_t g_itemStack_field = NULL;           // Item.stack                 (int)

// ============ 方法句柄 ============
static patch_handle_t g_setupRecipes_method = NULL;       // Recipe.SetupRecipes()                 (static)
static patch_handle_t g_createResult_method = NULL;       // CraftingRequests.CreateResult(Recipe) (static)
static patch_handle_t g_setDefaults2_method = NULL;       // Item.SetDefaults(int, ItemVariant)
static patch_handle_t g_setDefaults1_method = NULL;       // Item.SetDefaults(int)

// ============ 随机池 ============
#define kPoolMax 2048
static int32_t g_material_types[kPoolMax];
static int g_material_count = 0;
static int32_t g_product_types[kPoolMax];
static int g_product_count = 0;

// RequiredItemEntry { int itemIdOrRecipeGroup; int stack; } 内联 struct，共 8 字节
#define kLookupEntrySize 8
// IL2CPP 数组数据区偏移(对象头 16B + bounds 8B + max_length 4B + 对齐 4B)
static const size_t kArrayDataOffset = 0x20;
// RecipeGroup.FakeItemIdOffset，真实物品 id 恒小于该值
#define kFakeItemIdOffset 1000000

// ============ 基础工具 ============

/** 读取对象字段(引用类型)并返回对象指针; 失败返回 NULL */
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

static int32_t ReadItemType(void* item) {
    if (!item || !g_itemType_field) return 0;
#if defined(__ANDROID__)
    int32_t* p = (int32_t*)patchlib_field_get_pointer(g_itemType_field, item);
    return p ? *p : 0;
#else
    int32_t t = 0;
    patchlib_field_get_value(g_itemType_field, item, &t);
    return t;
#endif
}

static void SetItemField(patch_handle_t field, void* item, int32_t v) {
    if (!item || !field) return;
#if defined(__ANDROID__)
    int32_t* p = (int32_t*)patchlib_field_get_pointer(field, item);
    if (p) *p = v;
#else
    int32_t tmp = v;
    patchlib_field_set_value(field, item, &tmp);
#endif
}

static void SetItemType(void* item, int32_t v) { SetItemField(g_itemType_field, item, v); }
static void SetItemStack(void* item, int32_t v) { SetItemField(g_itemStack_field, item, v); }

/** 读取材料表第 idx 项的 itemIdOrRecipeGroup */
static int32_t ReadLookupEntryType(void* arr, size_t idx) {
    if (!arr) return 0;
#if defined(__ANDROID__)
    if (idx >= patchlib_array_length(arr)) return 0;
    return *(const int32_t*)((const char*)arr + kArrayDataOffset + idx * kLookupEntrySize);
#else
    unsigned char buf[kLookupEntrySize] = {0};
    if (!patchlib_array_at(arr, idx, buf)) return 0;
    int32_t t;
    memcpy(&t, buf, sizeof(t));
    return t;
#endif
}

/** 设置材料表第 idx 项 (RequiredItemEntry: type + stack) */
static void SetLookupEntry(void* arr, size_t idx, int32_t type, int32_t stack) {
    if (!arr) return;
#if defined(__ANDROID__)
    if (idx >= patchlib_array_length(arr)) return;
    int32_t* p = (int32_t*)((char*)arr + kArrayDataOffset + idx * kLookupEntrySize);
    p[0] = type;
    p[1] = stack;
#else
    unsigned char buf[kLookupEntrySize];
    memcpy(buf, &type, sizeof(type));
    memcpy(buf + sizeof(type), &stack, sizeof(stack));
    patchlib_array_set(arr, idx, buf);
#endif
}

/** 设置 requiredItem[ idx ] 这个 Item 的 type/stack */
static void SetRequiredItemSlot(void* arr, size_t idx, int32_t type, int32_t stack) {
    if (!arr) return;
    void* item = NULL;
    if (!patchlib_array_at(arr, idx, &item) || !item) return;
    SetItemType(item, type);
    SetItemStack(item, stack);
}

// ============ 随机数 ============
static int g_random_initialized = 0;

static void init_random(void) {
    if (!g_random_initialized) {
        srand((unsigned int)time(NULL));
        g_random_initialized = 1;
    }
}

static int Rand(int range) {
    init_random();
    return range > 0 ? (rand() % range) : 0;
}

// ============ 随机池构建 ============
static void PoolAdd(int32_t* pool, int* count, int32_t t) {
    if (t <= 0) return;
    if (*count >= kPoolMax) return;
    for (int i = 0; i < *count; ++i) {
        if (pool[i] == t) return;  // 去重
    }
    pool[(*count)++] = t;
}

static void BuildPools(void) {
    g_material_count = 0;
    g_product_count = 0;
    if (!g_mainRecipe_field) return;

    void* recipes = GetObjField(g_mainRecipe_field, NULL);
    if (!recipes) return;
    const size_t total = patchlib_array_length(recipes);

    for (size_t i = 0; i < total; ++i) {
        void* recipe = NULL;
        if (!patchlib_array_at(recipes, i, &recipe) || !recipe) continue;

        // 产物池: createItem.type
        void* item = GetObjField(g_recipeCreateItem_field, recipe);
        if (item) PoolAdd(g_product_types, &g_product_count, ReadItemType(item));

        // 材料池: requiredItemQuickLookup 中的真实物品 id
        void* quick = GetObjField(g_recipeQuickLookup_field, recipe);
        if (quick) {
            const size_t len = patchlib_array_length(quick);
            for (size_t j = 0; j < len; ++j) {
                const int32_t t = ReadLookupEntryType(quick, j);
                if (t > 0 && t < kFakeItemIdOffset) PoolAdd(g_material_types, &g_material_count, t);
            }
        }
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomCraft",
                         "pools: materials=%d products=%d", g_material_count, g_product_count);
    }
}

// ============ 单个配方材料随机化 ============
static void RandomizeRecipeMaterials(void* recipe) {
    if (!recipe || g_material_count <= 0) return;

    void* reqItemArr = GetObjField(g_recipeRequiredItem_field, recipe);
    void* quickArr = GetObjField(g_recipeQuickLookup_field, recipe);

    const size_t reqLen = reqItemArr ? patchlib_array_length(reqItemArr) : 0;
    const size_t qLen = quickArr ? patchlib_array_length(quickArr) : 0;
    const size_t cap = 15;  // maxRequirements

    // 随机 1~3 种材料
    const int n = 1 + Rand(3);
    for (int i = 0; i < n; ++i) {
        const int32_t t = g_material_types[Rand(g_material_count)];
        const int32_t stack = 1 + Rand(9);
        if ((size_t)i < qLen) SetLookupEntry(quickArr, (size_t)i, t, stack);
        if ((size_t)i < reqLen) SetRequiredItemSlot(reqItemArr, (size_t)i, t, stack);
    }

    // 其余置空，防止残留旧材料
    for (size_t i = (size_t)n; i < cap; ++i) {
        if (i < qLen) SetLookupEntry(quickArr, i, 0, 0);
        if (i < reqLen) SetRequiredItemSlot(reqItemArr, i, 0, 0);
    }
}

// ============ 全部配方随机化(只执行一次) ============
static void RandomizeAllRecipes(void) {
    if (g_initialized) return;
    g_initialized = true;

    BuildPools();
    if (g_material_count <= 0) return;

    void* recipes = GetObjField(g_mainRecipe_field, NULL);
    if (!recipes) return;
    const size_t total = patchlib_array_length(recipes);

    int count = 0;
    for (size_t i = 0; i < total; ++i) {
        void* recipe = NULL;
        if (!patchlib_array_at(recipes, i, &recipe) || !recipe) continue;
        void* item = GetObjField(g_recipeCreateItem_field, recipe);
        if (!item || ReadItemType(item) <= 0) continue;  // 只处理有效配方
        RandomizeRecipeMaterials(recipe);
        ++count;
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomCraft",
                         "randomized materials for %d recipes", count);
    }
}

// ============ 产物应用 ============
/** 调用 Item.SetDefaults(int) 让物品按随机类型正确初始化 */
static void ApplyItemDefaults(void* item, int32_t type) {
    if (!item || type <= 0) return;
    const int32_t t = type;
    if (g_setDefaults2_method) {
        void* nullVariant = NULL;
        void* args2[2] = { (void*)&t, (void*)&nullVariant };
        patchlib_method_invoke_args(g_setDefaults2_method, item, NULL, args2);
    } else if (g_setDefaults1_method) {
        void* args1[1] = { (void*)&t };
        patchlib_method_invoke_args(g_setDefaults1_method, item, NULL, args1);
    } else {
        // 找不到 SetDefaults 时退化为直接改 type（最少能随机出图标/名称）
        SetItemType(item, type);
    }
}

// ============ Hook: 配方表构建完成 (随机材料) ============
static void SetupRecipes_Postfix(patch_handle_t instance, void **args, void *result,
                                 const patch_method_signature_t *sig_info) {
    (void)instance; (void)args; (void)result; (void)sig_info;
    RandomizeAllRecipes();
}

// ============ Hook: 每次合成 (随机产物) ============
static void CreateResult_Postfix(patch_handle_t instance, void **args, void *result,
                                 const patch_method_signature_t *sig_info) {
    (void)instance; (void)args; (void)sig_info;
    if (!result) return;

    void** ppItem = (void**)result;
    void* item = ppItem ? *ppItem : NULL;
    if (!item) return;

    // 兜底: 若 SetupRecipes Hook 未触发，则在此完成首次随机化
    RandomizeAllRecipes();
    if (g_product_count <= 0) return;

    const int32_t randomType = g_product_types[Rand(g_product_count)];
    if (randomType <= 0) return;

    ApplyItemDefaults(item, randomType);
    SetItemStack(item, 1);

    if (!g_loggedSample) {
        g_loggedSample = true;
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomCraft",
                             "sample craft -> random item type=%d", randomType);
        }
    }
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomCraft", "初始化随机合成模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomCraft", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 类型
    g_main_type = patchlib_type_get_type("Terraria", "Main");
    g_recipe_type = patchlib_type_get_type("Terraria", "Recipe");
    g_item_type = patchlib_type_get_type("Terraria", "Item");
    g_crafting_type = patchlib_type_get_type("Terraria.GameContent", "CraftingRequests");
    if (!g_main_type || !g_recipe_type || !g_item_type || !g_crafting_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "RandomCraft",
                             "获取类型失败 (Main/Recipe/Item/CraftingRequests)");
        }
        return;
    }

    // 2. 字段
    g_mainRecipe_field = patchlib_type_get_field(g_main_type, "recipe");
    g_recipeCreateItem_field = patchlib_type_get_field(g_recipe_type, "createItem");
    g_recipeRequiredItem_field = patchlib_type_get_field(g_recipe_type, "requiredItem");
    g_recipeQuickLookup_field = patchlib_type_get_field(g_recipe_type, "requiredItemQuickLookup");
    g_itemType_field = patchlib_type_get_field(g_item_type, "type");
    g_itemStack_field = patchlib_type_get_field(g_item_type, "stack");
    if (!g_mainRecipe_field || !g_recipeCreateItem_field || !g_recipeRequiredItem_field ||
        !g_recipeQuickLookup_field || !g_itemType_field || !g_itemStack_field) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "RandomCraft", "获取字段失败");
        }
        return;
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomCraft",
                         "fields: recipe=%p createItem=%p requiredItem=%p quickLookup=%p itemType=%p itemStack=%p",
                         (void*)g_mainRecipe_field, (void*)g_recipeCreateItem_field,
                         (void*)g_recipeRequiredItem_field, (void*)g_recipeQuickLookup_field,
                         (void*)g_itemType_field, (void*)g_itemStack_field);
    }

    // 3. 方法
    g_setupRecipes_method = patchlib_type_get_method_by_param_count(g_recipe_type, "SetupRecipes", 0);
    if (!g_setupRecipes_method) g_setupRecipes_method = patchlib_type_get_method(g_recipe_type, "SetupRecipes");

    g_createResult_method = patchlib_type_get_method_by_param_count(g_crafting_type, "CreateResult", 1);
    if (!g_createResult_method) g_createResult_method = patchlib_type_get_method(g_crafting_type, "CreateResult");

    g_setDefaults2_method = patchlib_type_get_method_by_param_count(g_item_type, "SetDefaults", 2);
    g_setDefaults1_method = patchlib_type_get_method_by_param_count(g_item_type, "SetDefaults", 1);
    if (!g_setDefaults2_method && !g_setDefaults1_method) {
        g_setDefaults1_method = patchlib_type_get_method(g_item_type, "SetDefaults");
    }

    // 4. Hook: Recipe.SetupRecipes() 后置 -> 随机材料
    if (g_setupRecipes_method) {
        g_hookSetupRecipes = patchlib_install_prepost_hook(g_setupRecipes_method, NULL, SetupRecipes_Postfix);
    }
    if (g_hookSetupRecipes == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "RandomCraft", "SetupRecipes Hook 未安装(随机材料不可用)");
        }
    }

    // 5. Hook: CraftingRequests.CreateResult(Recipe) 后置 -> 随机产物
    if (g_createResult_method) {
        g_hookCreateResult = patchlib_install_prepost_hook(g_createResult_method, NULL, CreateResult_Postfix);
    }
    if (g_hookCreateResult == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "RandomCraft", "CreateResult Hook 未安装(随机产物不可用)");
        }
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomCraft",
                         "Hooks: setup=%d createResult=%d setDefaults=%p/%p",
                         (int)g_hookSetupRecipes, (int)g_hookCreateResult,
                         (void*)g_setDefaults2_method, (void*)g_setDefaults1_method);
    }
}

// ============ 模块清理 ============
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;

    if (g_hookSetupRecipes != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookSetupRecipes);
        g_hookSetupRecipes = PATCH_HOOK_INVALID_ID;
    }
    if (g_hookCreateResult != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookCreateResult);
        g_hookCreateResult = PATCH_HOOK_INVALID_ID;
    }

    g_initialized = false;
    g_loggedSample = false;
    g_material_count = 0;
    g_product_count = 0;

    g_mainRecipe_field = NULL;
    g_recipeCreateItem_field = NULL;
    g_recipeRequiredItem_field = NULL;
    g_recipeQuickLookup_field = NULL;
    g_itemType_field = NULL;
    g_itemStack_field = NULL;
    g_setupRecipes_method = NULL;
    g_createResult_method = NULL;
    g_setDefaults2_method = NULL;
    g_setDefaults1_method = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomCraft", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.randomcraft",
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
        mod_logger_write(MOD_LOG_LEVEL_INFO, "RandomCraft", "随机合成模组实例创建");
    }
    return &g_ops;
}
