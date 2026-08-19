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
// FreeCraft - 自由合成 (无需对应材料 / 无需工作站)
// NewEFMod (tefkernel / KernelLoader) 重写版，适配 Terraria 1.4.5.x (手机端 / PE)
//
// 功能:
//   1. 无需工作站: 所有配方的 requiredTile/needWater/needHoney/needLava/
//      生物群系等前置条件全部被清除, 不再需要任何工作台/熔炉/砧台/液体/环境;
//      同时把 Recipe.SetCraftingFilter 设为空操作, 让手机端合成界面的
//      工作站过滤器(GUICrafting.UpdateFilter)始终处于"全部"状态。
//   2. 无需对应材料: 所有配方的 requiredItemQuickLookup 被清空,
//      材料检查恒为通过, 合成列表显示全部配方。
//   3. 合成不消耗材料: 跳过原版 Recipe.GetIngredientsForOneCraft,
//      让本次要消耗的材料列表保持为空 -> 合成直接发放产物。
//
// 与 ClassicEFMod 版的差异(NewAPI):
//   - 入口从 CreateMod() 变为 create_kernel_mod(), 返回 kernel_mod_ops_t 操作表;
//   - Hook 从 registerFunctionDescriptor(替换转发函数) 变为
//     patchlib_install_prepost_hook(libffi 闭包):
//       * Recipe.FindRecipes / GetThroughDelayedFindRecipes -> postfix,
//         原版重建配方后清空各配方的"工作站/液体/环境/材料"数据;
//       * Recipe.GetIngredientsForOneCraft / SetCraftingFilter -> prefix 返回 true,
//         直接跳过原版(材料列表与工作站过滤器保持为空);
//   - 字段访问用 patchlib_field_get_pointer 直接取真实指针(Android);
//   - Main.recipe / Recipe.requiredItemQuickLookup 数组用
//     patchlib_array_length/at 访问; RequiredItemEntry 是内联 struct,
//     仍按 il2cpp 数组布局(数据区 0x20, 每项 8 字节)直接清空;
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
#include <string.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

// ============ 状态 ============
static patch_hook_id_t g_hookFindRecipes = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_hookDelayedFindRecipes = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_hookGetIngredients = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_hookSetCraftingFilter = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_hookEnvCheck = PATCH_HOOK_INVALID_ID;      // 桌面端: PlayerMeetsEnvironmentConditions
static patch_hook_id_t g_hookEnoughItems = PATCH_HOOK_INVALID_ID;   // 桌面端: CollectedEnoughItemsToCraft(RequiredItemEntry[])

// 采样验证日志(只打一次)
static bool g_verifiedOnce = false;

// ============ 类型句柄 ============
static patch_handle_t g_main_type = NULL;
static patch_handle_t g_recipe_type = NULL;
static patch_handle_t g_item_type = NULL;

// ============ 字段句柄 ============
static patch_handle_t g_mainRecipe_field = NULL;          // Main.recipe          (Recipe[], 静态)
static patch_handle_t g_recipeCreateItem_field = NULL;    // Recipe.createItem    (Item)
static patch_handle_t g_itemType_field = NULL;            // Item.type            (int)
static patch_handle_t g_requiredTile_field = NULL;        // Recipe.requiredTile  (int)
static patch_handle_t g_needWater_field = NULL;           // Recipe.needWater     (bool)
static patch_handle_t g_needHoney_field = NULL;           // Recipe.needHoney     (bool)
static patch_handle_t g_needLava_field = NULL;            // Recipe.needLava      (bool)
static patch_handle_t g_needTorch_field = NULL;           // Recipe.needTorchGodsFavor (bool)
static patch_handle_t g_needSnow_field = NULL;            // Recipe.needSnowBiome (bool)
static patch_handle_t g_needGrave_field = NULL;           // Recipe.needGraveyardBiome (bool)
static patch_handle_t g_needMech_field = NULL;            // Recipe.needMechdusa  (bool)
static patch_handle_t g_quickLookup_field = NULL;         // Recipe.requiredItemQuickLookup (RequiredItemEntry[])

/*
 * RequiredItemEntry 是内联 struct (dump.cs:68137):
 *   itemIdOrRecipeGroup: int @ 0x0
 *   stack:               int @ 0x4
 * Recipe.requiredItemQuickLookup 字段的值是指向 RequiredItemEntry[] 数组的引用。
 * IL2CPP 数组数据区在 0x20 处(对象头 16 字节 + bounds 8 字节 + max_length 4 字节
 * + 对齐 4 字节)。为避免误写, 先校验 max_length(应为 15)再清空。
 */
static const size_t kArrayDataOffset = 0x20;

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

/** 检查配方是否已是"零前置"状态 */
static bool IsRecipeFree(void* recipe) {
    if (!recipe) return true;
#if defined(__ANDROID__)
    if (g_requiredTile_field) {
        int* pTile = (int*)patchlib_field_get_pointer(g_requiredTile_field, recipe);
        if (pTile && *pTile != -1) return false;
    }
    if (g_quickLookup_field) {
        void* arr = GetObjField(g_quickLookup_field, recipe);
        if (arr) {
            const size_t len = patchlib_array_length(arr);
            if (len > 0) {
                const int32_t first = *(const int32_t*)((const char*)arr + kArrayDataOffset);
                if (first != 0) return false;
            }
        }
    }
#else
    if (g_requiredTile_field) {
        int tile = -1;
        patchlib_field_get_value(g_requiredTile_field, recipe, &tile);
        if (tile != -1) return false;
    }
#endif
    return true;
}

/** 清空配方的工作站/液体/环境前置条件 */
static void ClearEnvironmentRequirements(void* recipe) {
#if defined(__ANDROID__)
    int* pTile = (int*)patchlib_field_get_pointer(g_requiredTile_field, recipe);
    if (pTile) *pTile = -1;
    bool* pWater = (bool*)patchlib_field_get_pointer(g_needWater_field, recipe);
    if (pWater) *pWater = false;
    bool* pHoney = (bool*)patchlib_field_get_pointer(g_needHoney_field, recipe);
    if (pHoney) *pHoney = false;
    bool* pLava = (bool*)patchlib_field_get_pointer(g_needLava_field, recipe);
    if (pLava) *pLava = false;
    bool* pTorch = (bool*)patchlib_field_get_pointer(g_needTorch_field, recipe);
    if (pTorch) *pTorch = false;
    bool* pSnow = (bool*)patchlib_field_get_pointer(g_needSnow_field, recipe);
    if (pSnow) *pSnow = false;
    bool* pGrave = (bool*)patchlib_field_get_pointer(g_needGrave_field, recipe);
    if (pGrave) *pGrave = false;
    bool* pMech = (bool*)patchlib_field_get_pointer(g_needMech_field, recipe);
    if (pMech) *pMech = false;
#else
    int noTile = -1;
    bool no = false;
    patchlib_field_set_value(g_requiredTile_field, recipe, &noTile);
    patchlib_field_set_value(g_needWater_field, recipe, &no);
    patchlib_field_set_value(g_needHoney_field, recipe, &no);
    patchlib_field_set_value(g_needLava_field, recipe, &no);
    patchlib_field_set_value(g_needTorch_field, recipe, &no);
    patchlib_field_set_value(g_needSnow_field, recipe, &no);
    patchlib_field_set_value(g_needGrave_field, recipe, &no);
    patchlib_field_set_value(g_needMech_field, recipe, &no);
#endif
}

/** 清空配方材料表 (requiredItemQuickLookup) */
static void ClearMaterials(void* recipe) {
    if (!g_quickLookup_field) return;
    void* arr = GetObjField(g_quickLookup_field, recipe);
    if (!arr) return;

#if defined(__ANDROID__)
    const size_t len = patchlib_array_length(arr);
    if (len > 15) return;  // 安全校验, 防止误写野内存
    memset((char*)arr + kArrayDataOffset, 0, len * 8);
#else
    const size_t len = patchlib_array_length(arr);
    if (len > 15) return;
    // RequiredItemEntry 是内联 struct {int itemIdOrRecipeGroup; int stack;} (8 字节)
    unsigned char zeroEntry[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < len; ++i) {
        patchlib_array_set(arr, i, zeroEntry);
    }
#endif
}

/** 把单个配方改成"零前置" */
static void MakeRecipeFree(void* recipe) {
    if (!recipe) return;
    if (IsRecipeFree(recipe)) return;  // 已经处理过, 跳过(降低每帧开销)
    ClearEnvironmentRequirements(recipe);
    ClearMaterials(recipe);
}

/** 把所有有效配方改成"零前置"并统计 */
static int PatchAllRecipes(void* recipes) {
    if (!recipes) return 0;
    const size_t total = patchlib_array_length(recipes);
    int count = 0;
    for (size_t i = 0; i < total; ++i) {
        void* recipe = NULL;
        if (!patchlib_array_at(recipes, i, &recipe) || !recipe) continue;

        // 只处理"产物类型非空"的有效配方
        if (g_recipeCreateItem_field && g_itemType_field) {
            void* item = GetObjField(g_recipeCreateItem_field, recipe);
            if (!item) continue;
#if defined(__ANDROID__)
            int* pType = (int*)patchlib_field_get_pointer(g_itemType_field, item);
            if (pType && *pType <= 0) continue;
#else
            int type = 0;
            patchlib_field_get_value(g_itemType_field, item, &type);
            if (type <= 0) continue;
#endif
        }

        MakeRecipeFree(recipe);
        ++count;
    }
    return count;
}

// ============ 配方数据修改 ============
/*
 * 说明: 不再手动覆盖 Main.availableRecipe。
 * 原版 Recipe.FindRecipes 会依次检查 PlayerMeetsTileRequirements /
 * PlayerMeetsEnvironmentConditions / CollectedEnoughItemsToCraft,
 * 而本 Mod 已把每个配方的 requiredTile / need 系列布尔字段 / 材料表全部清除,
 * 这三项检查恒为通过, 原版会自然地(按配方下标顺序)把所有配方加入列表,
 * 并正确维护 focusRecipe, 避免合成后选中项跳变。
 */

static void PatchRecipes(void) {
    if (!g_mainRecipe_field) return;
    void* recipes = GetObjField(g_mainRecipe_field, NULL);
    if (!recipes) return;

    PatchAllRecipes(recipes);

    // 采样验证日志(只打一次)
    if (g_verifiedOnce) return;
    g_verifiedOnce = true;
    if (patchlib_array_length(recipes) == 0) return;
    void* r0 = NULL;
    if (!patchlib_array_at(recipes, 0, &r0) || !r0) return;

    int tile = -2;
    int firstReq = -1;
#if defined(__ANDROID__)
    int* pTile = (int*)patchlib_field_get_pointer(g_requiredTile_field, r0);
    if (pTile) tile = *pTile;
    void* arr = GetObjField(g_quickLookup_field, r0);
    if (arr && patchlib_array_length(arr) > 0) {
        firstReq = *(const int32_t*)((const char*)arr + kArrayDataOffset);
    }
#else
    patchlib_field_get_value(g_requiredTile_field, r0, &tile);
#endif
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "FreeCraft",
                         "verify: recipe[0] requiredTile=%d firstRequiredItem=%d",
                         tile, firstReq);
    }
}

// ============ Hook: 合成配方表刷新 (无需工作站 / 无需材料) ============
static void FindRecipes_Postfix(patch_handle_t instance, void **args, void *result,
                                const patch_method_signature_t *sig_info) {
    (void)instance; (void)args; (void)result; (void)sig_info;
    PatchRecipes();
}

static void DelayedFindRecipes_Postfix(patch_handle_t instance, void **args, void *result,
                                       const patch_method_signature_t *sig_info) {
    (void)instance; (void)args; (void)result; (void)sig_info;
    PatchRecipes();
}

// ============ Hook: 合成时材料列表 (免费合成, 不消耗材料) ============
// 关键: 跳过原版, 让本次要消耗的材料列表保持为空 -> 消耗循环不执行 -> 免费发放产物
static bool GetIngredientsForOneCraft_Prefix(patch_handle_t instance, void **args,
                                             const patch_method_signature_t *sig_info, void *result) {
    (void)instance; (void)args; (void)sig_info; (void)result;
    // true = 跳过原方法(本 tefkernel 版本前缀语义与头文件注释相反)
    return true;
}

// ============ Hook: 工作站过滤器 (阻止按工作站过滤, 使所有配方可见) ============
// 关键: 跳过原版, 阻止设置"工作站过滤器"。
// 手机端合成界面(GUICrafting.UpdateFilter)按 requiredTile == 当前工作站
// 过滤配方; 让 SetCraftingFilter 空操作, 过滤器始终为空 ->
// get_TileFilter 返回 -1 -> 所有配方都被显示, 不再受工作站限制。
static bool SetCraftingFilter_Prefix(patch_handle_t instance, void **args,
                                     const patch_method_signature_t *sig_info, void *result) {
    (void)instance; (void)args; (void)sig_info; (void)result;
    // true = 跳过原方法
    return true;
}

// ============ Hook: 桌面端配方检查强制通过 (他法: 不逐配方改数据, 直接改检查结果) ============
// PC 的 Recipe.UpdateRecipeList 对每个配方调用:
//   recipe.PlayerMeetsEnvironmentConditions(localPlayer)   (工作站/液体/环境)
//   CollectedEnoughItemsToCraft(recipe)                    (材料是否足够)
// 两个 postfix 都强制返回 true => 所有配方恒满足条件, 无需工作站/材料, 全部显示。

/** Recipe.PlayerMeetsEnvironmentConditions 后置: 强制通过 */
static void EnvironmentCheck_Postfix(patch_handle_t instance, void **args, void *result,
                                     const patch_method_signature_t *sig_info) {
    (void)instance; (void)args; (void)sig_info;
    if (result) *(bool*)result = true;
}

/** Recipe.CollectedEnoughItemsToCraft(RequiredItemEntry[]) 后置: 强制通过 */
static void EnoughItems_Postfix(patch_handle_t instance, void **args, void *result,
                                const patch_method_signature_t *sig_info) {
    (void)instance; (void)args; (void)sig_info;
    if (result) *(bool*)result = true;
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "FreeCraft", "初始化自由合成模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "FreeCraft", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 获取类型
    g_main_type = patchlib_type_get_type("Terraria", "Main");
    g_recipe_type = patchlib_type_get_type("Terraria", "Recipe");
    g_item_type = patchlib_type_get_type("Terraria", "Item");
    if (!g_main_type || !g_recipe_type || !g_item_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "FreeCraft", "获取类型失败 (Main/Recipe/Item)");
        }
        return;
    }

    // 2. 字段
    g_mainRecipe_field = patchlib_type_get_field(g_main_type, "recipe");
    g_recipeCreateItem_field = patchlib_type_get_field(g_recipe_type, "createItem");
    g_itemType_field = patchlib_type_get_field(g_item_type, "type");
    g_requiredTile_field = patchlib_type_get_field(g_recipe_type, "requiredTile");
    g_needWater_field = patchlib_type_get_field(g_recipe_type, "needWater");
    g_needHoney_field = patchlib_type_get_field(g_recipe_type, "needHoney");
    g_needLava_field = patchlib_type_get_field(g_recipe_type, "needLava");
    g_needTorch_field = patchlib_type_get_field(g_recipe_type, "needTorchGodsFavor");
    g_needSnow_field = patchlib_type_get_field(g_recipe_type, "needSnowBiome");
    g_needGrave_field = patchlib_type_get_field(g_recipe_type, "needGraveyardBiome");
    g_needMech_field = patchlib_type_get_field(g_recipe_type, "needMechdusa");
    g_quickLookup_field = patchlib_type_get_field(g_recipe_type, "requiredItemQuickLookup");

    if (!g_mainRecipe_field || !g_recipeCreateItem_field || !g_itemType_field ||
        !g_requiredTile_field || !g_needWater_field || !g_needHoney_field ||
        !g_needLava_field || !g_needTorch_field || !g_needSnow_field ||
        !g_needGrave_field || !g_needMech_field || !g_quickLookup_field) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "FreeCraft", "获取配方字段失败");
        }
        return;
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "FreeCraft",
                         "fields: recipe=%p createItem=%p itemType=%p requiredTile=%p quickLookup=%p",
                         (void*)g_mainRecipe_field, (void*)g_recipeCreateItem_field,
                         (void*)g_itemType_field, (void*)g_requiredTile_field,
                         (void*)g_quickLookup_field);
    }

    // 3. Hook: Recipe.FindRecipes(bool canDelayCheck) 静态, 1 参 (仅 Android 存在)
#if defined(__ANDROID__)
    patch_handle_t find_method = patchlib_type_get_method_by_param_count(g_recipe_type, "FindRecipes", 1);
    if (!find_method) find_method = patchlib_type_get_method(g_recipe_type, "FindRecipes");
    if (find_method) {
        g_hookFindRecipes = patchlib_install_prepost_hook(find_method, NULL, FindRecipes_Postfix);
    }
    if (g_hookFindRecipes == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "FreeCraft", "FindRecipes Hook 未安装");
        }
    }
#endif

    // 4. Hook: 配方表刷新检查 (平台方案不同)
#if defined(__ANDROID__)
    patch_handle_t delayed_method = patchlib_type_get_method_by_param_count(
            g_recipe_type, "GetThroughDelayedFindRecipes", 0);
    if (!delayed_method) delayed_method = patchlib_type_get_method(g_recipe_type, "GetThroughDelayedFindRecipes");
    if (delayed_method) {
        g_hookDelayedFindRecipes = patchlib_install_prepost_hook(
                delayed_method, NULL, DelayedFindRecipes_Postfix);
    }
    if (g_hookDelayedFindRecipes == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "FreeCraft", "GetThroughDelayedFindRecipes Hook 未安装");
        }
    }
#else
    // 桌面端他法: 直接 Hook 两个配方检查方法, 强制返回 true,
    // 无需逐配方修改 requiredTile/need*/材料数据。
    // 1) Recipe.PlayerMeetsEnvironmentConditions(Player, List<string>) 实例, 2 参
    patch_handle_t env_method = patchlib_type_get_method_by_param_count(
            g_recipe_type, "PlayerMeetsEnvironmentConditions", 2);
    if (!env_method) env_method = patchlib_type_get_method(g_recipe_type, "PlayerMeetsEnvironmentConditions");
    if (env_method) {
        g_hookEnvCheck = patchlib_install_prepost_hook(env_method, NULL, EnvironmentCheck_Postfix);
    }
    if (g_hookEnvCheck == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "FreeCraft", "PlayerMeetsEnvironmentConditions Hook 未安装");
        }
    }

    // 2) Recipe.CollectedEnoughItemsToCraft(RequiredItemEntry[]) 静态, 1 参
    //    (该重载参数名为 requiredItems, 用于区分同名的 Recipe 重载)
    const char* requiredItemsNames[] = { "requiredItems" };
    patch_handle_t enough_method = patchlib_type_get_method_by_param_names(
            g_recipe_type, "CollectedEnoughItemsToCraft", 1, requiredItemsNames);
    if (!enough_method) enough_method = patchlib_type_get_method(g_recipe_type, "CollectedEnoughItemsToCraft");
    if (enough_method) {
        g_hookEnoughItems = patchlib_install_prepost_hook(enough_method, NULL, EnoughItems_Postfix);
    }
    if (g_hookEnoughItems == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "FreeCraft", "CollectedEnoughItemsToCraft Hook 未安装(材料检查无法绕过)");
        }
    }
#endif

    // 5. Hook: Recipe.GetIngredientsForOneCraft(Player, List) 实例, 2 参 (跳过原版, 免费合成)
    patch_handle_t ingredients_method = patchlib_type_get_method_by_param_count(
            g_recipe_type, "GetIngredientsForOneCraft", 2);
    if (!ingredients_method) ingredients_method = patchlib_type_get_method(g_recipe_type, "GetIngredientsForOneCraft");
    if (ingredients_method) {
        g_hookGetIngredients = patchlib_install_prepost_hook(
                ingredients_method, GetIngredientsForOneCraft_Prefix, NULL);
    }
    if (g_hookGetIngredients == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "FreeCraft", "GetIngredientsForOneCraft Hook 未安装(免费合成不可用)");
        }
    }

    // 6. Hook: Recipe.SetCraftingFilter(int, int, int) 静态, 3 参 (跳过原版, 仅 Android)
#if defined(__ANDROID__)
    patch_handle_t setFilter_method = patchlib_type_get_method_by_param_count(
            g_recipe_type, "SetCraftingFilter", 3);
    if (!setFilter_method) setFilter_method = patchlib_type_get_method(g_recipe_type, "SetCraftingFilter");
    if (setFilter_method) {
        g_hookSetCraftingFilter = patchlib_install_prepost_hook(
                setFilter_method, SetCraftingFilter_Prefix, NULL);
    }
    if (g_hookSetCraftingFilter == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "FreeCraft", "SetCraftingFilter Hook 未安装(全部配方显示可能不可用)");
        }
    }
#endif

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "FreeCraft",
                         "Hooks: find=%d delayed=%d env=%d enough=%d ingredients=%d filter=%d",
                         (int)g_hookFindRecipes, (int)g_hookDelayedFindRecipes,
                         (int)g_hookEnvCheck, (int)g_hookEnoughItems,
                         (int)g_hookGetIngredients, (int)g_hookSetCraftingFilter);
    }
}

// ============ 模块清理 ============
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;

    if (g_hookFindRecipes != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookFindRecipes);
        g_hookFindRecipes = PATCH_HOOK_INVALID_ID;
    }
    if (g_hookDelayedFindRecipes != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookDelayedFindRecipes);
        g_hookDelayedFindRecipes = PATCH_HOOK_INVALID_ID;
    }
    if (g_hookGetIngredients != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookGetIngredients);
        g_hookGetIngredients = PATCH_HOOK_INVALID_ID;
    }
    if (g_hookSetCraftingFilter != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookSetCraftingFilter);
        g_hookSetCraftingFilter = PATCH_HOOK_INVALID_ID;
    }
    if (g_hookEnvCheck != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookEnvCheck);
        g_hookEnvCheck = PATCH_HOOK_INVALID_ID;
    }
    if (g_hookEnoughItems != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookEnoughItems);
        g_hookEnoughItems = PATCH_HOOK_INVALID_ID;
    }

    g_verifiedOnce = false;

    g_mainRecipe_field = NULL;
    g_recipeCreateItem_field = NULL;
    g_itemType_field = NULL;
    g_requiredTile_field = NULL;
    g_needWater_field = NULL;
    g_needHoney_field = NULL;
    g_needLava_field = NULL;
    g_needTorch_field = NULL;
    g_needSnow_field = NULL;
    g_needGrave_field = NULL;
    g_needMech_field = NULL;
    g_quickLookup_field = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "FreeCraft", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.freecraft",
        .version_code = 1,
        .api_version = 1,
        .version = "1.0.1",
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
        mod_logger_write(MOD_LOG_LEVEL_INFO, "FreeCraft", "自由合成模组实例创建");
    }
    return &g_ops;
}
