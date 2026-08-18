//
// VeinMiner - 连锁挖矿
// NewEFMod (tefkernel / KernelLoader) 重写版，适配 Terraria 1.4.5.x (手机端 / PE)
//
// 功能: 破坏矿石/宝石方块时, 自动连锁破坏与其同类型、且相连的所有矿石/宝石。
//       例如挖掉一块铁矿, 会把连在一起的整条铁矿脉全部挖掉; 挖石头/泥土不会连锁。
//
// 连锁类型列表参考 Terraria Wiki "Ores"/"Gems" 页:
//   矿石 Tile ID: 6-9, 22, 37, 56, 58, 107-108, 111, 166-169, 204, 211, 221-223, 408
//   宝石 Tile ID: 63-68 (Sapphire/Ruby/Emerald/Topaz/Amethyst/Diamond),
//                 178 (ExposedGems) 与 566 (AmberStoneBlock, 沙漠琥珀)
//
// 实现要点(结合 PE 源码, pe/dump.cs):
//   1. 挖矿路径: 玩家挖掘走 Player.PickTile(int x, int y, int pickPower)
//      (dump.cs:62313), 其内部调用 WorldGen.KillTile 实际破坏方块。
//      Hook Player.PickTile 而不是 KillTile 的原因:
//        - 世界生成阶段 WorldGen 会高频直接调用 KillTile, 此时挂 KillTile 钩子
//          会在 libffi 闭包中反复重入, 造成栈溢出崩溃;
//        - PickTile 只在玩家实际挖掘时调用, 世界生成完全不会经过它, 天然安全。
//   2. 方块类型读取: PE 端(1.4.5.6.x) Tile 是 struct, 方块数据被拆成若干原生数组:
//        Main.maxTilesX/maxTilesY   -> static int    世界尺寸 (dump.cs:49770/49771)
//        TileData.TileLookup        -> static uint*  坐标->tile索引 (dump.cs:70236)
//        TileData.TileType          -> static ushort* tile索引->方块类型 (dump.cs:70244)
//      因此 GetTileType(x,y) = TileType[TileLookup[y*maxTilesX+x]], 索引 0xFFFFFFFF 表示空。
//      这些字段在初始化时用 patchlib_field_get_pointer 一次性解析为真实指针,
//      热路径上直接解引用, 不再调用任何 il2cpp 运行时函数。
//   3. 连锁时直接调用未 Hook 的 WorldGen.KillTile 原版函数(patchlib_method_get_pointer),
//      不会重入任何钩子, 因此不会递归。
//
// 与 ClassicEFMod 版的差异(NewAPI):
//   - 入口从 CreateMod() 变为 create_kernel_mod(), 返回 kernel_mod_ops_t 操作表;
//   - Hook 从 registerFunctionDescriptor(替换转发函数) 变为 patchlib_install_prepost_hook
//     (libffi 闭包), prefix 捕获类型, postfix 触发连锁;
//   - Hook 目标从 WorldGen.KillTile 改为 Player.PickTile, 避开世界生成期崩溃;
//   - 重要: 本 tefkernel 版本中 prefix 返回值语义与头文件注释相反
//     (实测 false = 执行原方法, true = 跳过原方法), 因此 prefix 必须返回 false;
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

// Hook 句柄与原始 KillTile 指针
static patch_hook_id_t g_hook_id = PATCH_HOOK_INVALID_ID;
static void (*g_original_KillTile)(int i, int j, bool fail, bool effectOnly, bool noItem) = NULL;

/*
 * PE 端方块数据静态字段句柄(dump.cs, 均为静态字段)
 *   Main.maxTilesX/maxTilesY  (static int)
 *   TileData.TileLookup       (static uint*)
 *   TileData.TileType         (static ushort*)
 */
static patch_handle_t g_maxTilesX_field = NULL;
static patch_handle_t g_maxTilesY_field = NULL;
static patch_handle_t g_tileLookup_field = NULL;
static patch_handle_t g_tileType_field = NULL;

#if !defined(__ANDROID__)
/*
 * 桌面端方块数据: Main.tile 是 Tile[,] (2D 数组, 元素为 Tile 类对象引用)
 * 读类型: 用 Framing.GetTileSafely(x,y) 取 Tile 对象 (内核 array_at 不支持 2D 数组),
 *         再读 Tile.type 字段
 */
static patch_handle_t g_tile_field = NULL;        // Main.tile (Tile[,])
static patch_handle_t g_tileTypeOfTile_field = NULL; // Tile.type (ushort)
static patch_handle_t g_killtile_method = NULL;   // WorldGen.KillTile (method_invoke_args)
static patch_handle_t g_getTileSafely_method = NULL; // Framing.GetTileSafely (static, 2 参)
#endif

/*
 * 字段真实指针缓存(仅 Android)。
 * 通过 patchlib_field_get_pointer 在初始化时一次性解析出各静态字段的存储地址,
 * 之后 GetTileType 直接解引用, 避免在 KillTile 热路径上反复调用
 * patchlib_field_get_value 这类 il2cpp 运行时函数(其在世界生成阶段会崩溃)。
 */
#if defined(__ANDROID__)
static int*       g_pMaxTilesX;   // &Main.maxTilesX       (static int)
static int*       g_pMaxTilesY;   // &Main.maxTilesY       (static int)
static uint32_t** g_pTileLookup;  // &TileData.TileLookup  (static uint*)
static uint16_t** g_pTileType;    // &TileData.TileType    (static ushort*)
#endif

/*
 * 矿石方块类型表
 * 参考 Terraria Wiki "Ores" 页 Tile ID, 与 PC 源码 ID/TileID.cs 核对:
 *   Copper=7 Iron=6 Silver=9 Gold=8 Tin=166 Lead=167 Tungsten=168 Platinum=169
 *   Demonite=22 Crimtane=204 Meteorite=37 Obsidian=56 Hellstone=58
 *   Cobalt=107 Palladium=221 Mythril=108 Orichalcum=222 Adamantite=111 Titanium=223
 *   Chlorophyte=211 Luminite=408
 */
static const uint16_t kOreTypes[] = {
        6, 7, 8, 9, 22, 37, 56, 58,
        107, 108, 111,
        166, 167, 168, 169,
        204, 211, 221, 222, 223,
        408
};
static const int kOreCount = (int)(sizeof(kOreTypes) / sizeof(kOreTypes[0]));

/*
 * 宝石方块类型表
 * 参考 Terraria Wiki "Gems" 页, 与 PC 源码 ID/TileID.cs 核对:
 *   63=Sapphire 64=Ruby 65=Emerald 66=Topaz 67=Amethyst 68=Diamond (洞窟中开采的宝石)
 *   178=ExposedGems (露出的放置宝石, 即 Wiki Gems 页标注的 Tile ID 178)
 *   566=AmberStoneBlock (琥珀石, 地下沙漠生成的琥珀, 掉落 item 999)
 */
static const uint16_t kGemTypes[] = {
        63, 64, 65, 66, 67, 68, 178, 566
};
static const int kGemCount = (int)(sizeof(kGemTypes) / sizeof(kGemTypes[0]));

/** 判断方块类型是否为矿石或宝石 */
static bool IsVeinType(uint16_t type) {
    for (int i = 0; i < kOreCount; ++i) {
        if (kOreTypes[i] == type) return true;
    }
    for (int i = 0; i < kGemCount; ++i) {
        if (kGemTypes[i] == type) return true;
    }
    return false;
}

// 连锁上限、搜索半径与空 tile 标记
static const int     kMaxChainTiles = 256;
static const int     kChainRadius   = 32;
static const uint32_t kNoTile       = 0xFFFFFFFFu;

// 重入深度计数: 0 = 无调用, 1 = 最外层 PickTile
static int g_killDepth = 0;
// 最外层 PickTile 所挖掘方块的类型(由 prefix hook 在原方法执行前捕获)
static uint16_t g_lastKilledType = 0;

// 四个方向的偏移
static const int kDx[4] = { 0, 0, -1, 1 };
static const int kDy[4] = { -1, 1, 0, 0 };

/**
 * 读取指定坐标的方块类型(PE 端 TileData 原生数组方案)
 * @param x 世界坐标 X
 * @param y 世界坐标 Y
 * @return 方块类型(0 表示空/越界/读取失败)
 */
static uint16_t GetTileType(int x, int y) {
#if defined(__ANDROID__)
    if (!g_pMaxTilesX || !g_pMaxTilesY || !g_pTileLookup || !g_pTileType) return 0;

    const int maxX = *g_pMaxTilesX;
    const int maxY = *g_pMaxTilesY;
    if (maxX <= 0 || maxY <= 0) return 0;
    if (x < 0 || y < 0 || x >= maxX || y >= maxY) return 0;

    const uint32_t* lookup = *g_pTileLookup;
    const uint16_t* types  = *g_pTileType;
    if (!lookup || !types) return 0;

    const uint32_t tileIndex = lookup[(size_t)y * (size_t)maxX + (size_t)x];
    if (tileIndex == kNoTile) return 0;

    return types[tileIndex];
#else
    // 桌面端: 用 Framing.GetTileSafely(x,y) 取 Tile 对象, 再读 type 字段
    if (!g_getTileSafely_method || !g_tileTypeOfTile_field ||
        !g_maxTilesX_field || !g_maxTilesY_field) return 0;

    int maxX = 0, maxY = 0;
    patchlib_field_get_value(g_maxTilesX_field, NULL, &maxX);
    patchlib_field_get_value(g_maxTilesY_field, NULL, &maxY);
    if (maxX <= 0 || maxY <= 0) return 0;
    if (x < 0 || y < 0 || x >= maxX || y >= maxY) return 0;

    int ix = x, iy = y;
    void* args[2];
    args[0] = &ix;
    args[1] = &iy;
    void* tileObj = NULL;
    if (!patchlib_method_invoke_args(g_getTileSafely_method, NULL, &tileObj, args)) return 0;
    if (!tileObj) return 0;   // 空 Tile -> 空气

    uint16_t type = 0;
    patchlib_field_get_value(g_tileTypeOfTile_field, tileObj, &type);
    return type;
#endif
}

/** 调用 WorldGen.KillTile 破坏方块 (Android: 原生函数指针; 桌面端: invoke_args) */
static void CallKillTile(int i, int j, bool fail, bool effectOnly, bool noItem) {
#if defined(__ANDROID__)
    if (g_original_KillTile) g_original_KillTile(i, j, fail, effectOnly, noItem);
#else
    if (!g_killtile_method) return;
    void* args[5];
    args[0] = &i;
    args[1] = &j;
    args[2] = &fail;
    args[3] = &effectOnly;
    args[4] = &noItem;
    patchlib_method_invoke_args(g_killtile_method, NULL, NULL, args);
#endif
}

/**
 * 实际连锁逻辑: 以 (ox, oy) 为中心做广度优先搜索,
 * 将与 origin 同类型的相连矿石/宝石用原版 KillTile 全部破坏
 */
static void ChainMine(int ox, int oy, uint16_t type, bool noItem) {
    typedef struct { int x; int y; } Pos;
    Pos queue[kMaxChainTiles];
    int count = 0;

#if defined(__ANDROID__)
    if (!g_original_KillTile) return;
#else
    if (!g_killtile_method) return;
#endif

    queue[count++] = (Pos){ ox, oy };

    for (int visited = 0; visited < count; ++visited) {
        const Pos cur = queue[visited];

        for (int dir = 0; dir < 4; ++dir) {
            const int nx = cur.x + kDx[dir];
            const int ny = cur.y + kDy[dir];

            // 限制搜索半径, 防止误挖远处方块
            if (nx < ox - kChainRadius || nx > ox + kChainRadius) continue;
            if (ny < oy - kChainRadius || ny > oy + kChainRadius) continue;

            // 已访问检查(防止死循环)
            bool seen = false;
            for (int v = 0; v < count; ++v) {
                if (queue[v].x == nx && queue[v].y == ny) { seen = true; break; }
            }
            if (seen) continue;

            // 必须与目标类型一致
            if (GetTileType(nx, ny) != type) continue;

            if (count >= kMaxChainTiles) return;   // 达到连锁上限

            queue[count++] = (Pos){ nx, ny };
            CallKillTile(nx, ny, false, false, noItem);
        }
    }
}

/**
 * Prefix Hook: 在 Player.PickTile 执行前捕获被挖方块的类型
 * @note 注意: 本 tefkernel 版本中 prefix 返回值语义与头文件注释相反!
 *      实测返回 false 才会正常执行原方法, 返回 true 会直接跳过原方法
 *      (导致玩家无法挖掘)。这里返回 false。
 */
static bool PickTile_Prefix(patch_handle_t instance, void **args,
                            const patch_method_signature_t *sig_info, void *result) {
    (void)instance; (void)sig_info; (void)result;

    const int depth = g_killDepth++;

    // 仅在最外层调用时捕获类型(此时方块还没被销毁)
    if (depth == 0 && args) {
        g_lastKilledType = GetTileType(*(int*)args[0], *(int*)args[1]);
    }

    // false = 正常执行原方法(反向语义)
    return false;
}

/**
 * Postfix Hook: 玩家成功挖掉矿石/宝石后, 对同类型的相连方块执行连锁
 */
static void PickTile_Postfix(patch_handle_t instance, void **args, void *result,
                             const patch_method_signature_t *sig_info) {
    (void)instance; (void)result; (void)sig_info;

    if (g_killDepth <= 0) return;

    if (g_killDepth == 1 && args) {
        const int i = *(int*)args[0];
        const int j = *(int*)args[1];

        // 目标方块已被成功挖掉(原版 PickTile 内部调用 KillTile 完成破坏)才连锁
        if (GetTileType(i, j) == 0) {
            const uint16_t type = g_lastKilledType;
            // 类型 0 表示空气/读取失败
            if (type != 0 && type < 4096 && IsVeinType(type)) {
                ChainMine(i, j, type, false);
            }
        }
    }

    --g_killDepth;
}

// 模块信息
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.veinminer",
        .version_code = 1,
        .api_version = 1,
        .version = "1.0.0",
};

// 模块初始化
static void init_mod(kernel_mod_handle_t *handle) {
    patch_handle_t main_type = NULL;
    patch_handle_t tiledata_type = NULL;
    patch_handle_t worldgen_type = NULL;
    patch_handle_t player_type = NULL;
    patch_handle_t killtile_method = NULL;
    patch_handle_t picktile_method = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "VeinMiner", "初始化连锁挖矿模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "VeinMiner", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 获取类型 (桌面端没有 Terraria.TileData, 方块数据走 Main.tile)
    main_type = patchlib_type_get_type("Terraria", "Main");
    worldgen_type = patchlib_type_get_type("Terraria", "WorldGen");
    player_type = patchlib_type_get_type("Terraria", "Player");
#if defined(__ANDROID__)
    tiledata_type = patchlib_type_get_type("Terraria", "TileData");
    if (!main_type || !tiledata_type || !worldgen_type || !player_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "VeinMiner", "获取类型失败 (Main/TileData/WorldGen/Player)");
        }
        return;
    }
#else
    if (!main_type || !worldgen_type || !player_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "VeinMiner", "获取类型失败 (Main/WorldGen/Player)");
        }
        return;
    }
#endif

    // 2. 解析方块数据字段
    g_maxTilesX_field = patchlib_type_get_field(main_type, "maxTilesX");
    g_maxTilesY_field = patchlib_type_get_field(main_type, "maxTilesY");

#if defined(__ANDROID__)
    g_tileLookup_field = patchlib_type_get_field(tiledata_type, "TileLookup");
    g_tileType_field = patchlib_type_get_field(tiledata_type, "TileType");

    // 一次性解析字段真实指针, 供 GetTileType 直接解引用
    g_pMaxTilesX = (int*)patchlib_field_get_pointer(g_maxTilesX_field, NULL);
    g_pMaxTilesY = (int*)patchlib_field_get_pointer(g_maxTilesY_field, NULL);
    g_pTileLookup = (uint32_t**)patchlib_field_get_pointer(g_tileLookup_field, NULL);
    g_pTileType = (uint16_t**)patchlib_field_get_pointer(g_tileType_field, NULL);

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "VeinMiner",
                         "maxTilesX=%p maxTilesY=%p lookup=%p type=%p",
                         (void*)g_pMaxTilesX, (void*)g_pMaxTilesY,
                         (void*)g_pTileLookup, (void*)g_pTileType);
    }
#else
    // 桌面端: Main.tile 是 Tile[,], 读 Tile.type 字段; 用 Framing.GetTileSafely 取 Tile 对象
    g_tile_field = patchlib_type_get_field(main_type, "tile");
    patch_handle_t tile_type = patchlib_type_get_type("Terraria", "Tile");
    if (tile_type) {
        g_tileTypeOfTile_field = patchlib_type_get_field(tile_type, "type");
    }
    patch_handle_t framing_type = patchlib_type_get_type("Terraria", "Framing");
    if (framing_type) {
        g_getTileSafely_method = patchlib_type_get_method_by_param_count(framing_type, "GetTileSafely", 2);
        if (!g_getTileSafely_method) g_getTileSafely_method = patchlib_type_get_method(framing_type, "GetTileSafely");
    }
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "VeinMiner",
                         "tileF=%p tileTypeF=%p maxXF=%p maxYF=%p",
                         (void*)g_tile_field, (void*)g_tileTypeOfTile_field,
                         (void*)g_maxTilesX_field, (void*)g_maxTilesY_field);
    }
#endif

    // 3. 获取 WorldGen.KillTile(静态方法, 5 个参数), 用于连锁时直接调用原版
    killtile_method = patchlib_type_get_method_by_param_count(worldgen_type, "KillTile", 5);
    if (!killtile_method) {
        // 参数数量匹配失败时, 尝试通过名称获取
        killtile_method = patchlib_type_get_method(worldgen_type, "KillTile");
    }
    if (!killtile_method) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "VeinMiner", "获取KillTile方法失败");
        }
        return;
    }

#if defined(__ANDROID__)
    // KillTile 不挂 Hook, 此指针即未 Hook 的原版函数, 连锁调用不会重入任何钩子
    g_original_KillTile = (void (*)(int, int, bool, bool, bool))patchlib_method_get_pointer(killtile_method);
#else
    g_killtile_method = killtile_method;
#endif
#if defined(__ANDROID__)
    if (!g_original_KillTile) {
#else
    if (!g_killtile_method) {
#endif
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "VeinMiner", "获取KillTile函数指针失败, 连锁将不可用");
        }
    }

    // 4. 获取 Player.PickTile(实例方法, 3 个参数: int x, int y, int pickPower)
    //    只在玩家挖矿时调用, 世界生成不会经过它, 因此不会干扰/崩溃世界生成
    picktile_method = patchlib_type_get_method_by_param_count(player_type, "PickTile", 3);
    if (!picktile_method) {
        // 参数数量匹配失败时, 尝试通过名称获取
        picktile_method = patchlib_type_get_method(player_type, "PickTile");
    }
    if (!picktile_method) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "VeinMiner", "获取PickTile方法失败");
        }
        return;
    }

    // 5. 安装前缀/后缀 Hook
    g_hook_id = patchlib_install_prepost_hook(picktile_method, PickTile_Prefix, PickTile_Postfix);
    if (g_hook_id == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "VeinMiner", "安装PickTile Hook失败");
        }
        return;
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "VeinMiner",
                         "成功Hook PickTile (hook_id=%d), 连锁挖矿已启用", (int)g_hook_id);
    }
}

// 模块清理
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;

    // 卸载 Hook
    if (g_hook_id != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hook_id);
        g_hook_id = PATCH_HOOK_INVALID_ID;
    }

    g_original_KillTile = NULL;
    g_killDepth = 0;

#if defined(__ANDROID__)
    g_pMaxTilesX = NULL;
    g_pMaxTilesY = NULL;
    g_pTileLookup = NULL;
    g_pTileType = NULL;
#else
    g_tile_field = NULL;
    g_tileTypeOfTile_field = NULL;
    g_killtile_method = NULL;
    g_getTileSafely_method = NULL;
#endif

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "VeinMiner", "清理模组");
    }
}

// 获取模块信息
static kernel_mod_info_t *get_info(void) {
    return &g_mod_info;
}

// 模块操作函数表
static kernel_mod_ops_t g_ops = {
        .init_mod = init_mod,
        .cleanup_mod = cleanup_mod,
        .get_info = get_info
};

// 模块入口函数
kernel_mod_ops_t *create_kernel_mod(void) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "VeinMiner", "连锁挖矿模组实例创建");
    }
    return &g_ops;
}
