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
// ChaosGen - 混沌生成
// NewEFMod (tefkernel / KernelLoader) 重写版，适配 Terraria 1.4.5.x (手机端 / PE)
//
// 功能: 每次新建世界时, 在世界生成完成后自动把全世界所有"实心方块"
//       随机化为任意"安全"图格(0 ~ TileID.Count-1 中非 frameImportant 的全部
//       类型, 地形/矿石/宝石/草/沙/冰……一切皆有可能, 但排除箱/门/牌/床/家具
//       等需要特殊数据或多格结构的图格, 避免渲染/更新时读取孤儿数据崩溃)。
//       全图随机化, 不保留任何区域。
//
// 与 ClassicEFMod 版的差异(NewAPI):
//   - 入口从 CreateMod() 变为 create_kernel_mod(), 返回 kernel_mod_ops_t 操作表;
//   - Hook 从 registerFunctionDescriptor(替换转发函数) 变为
//     patchlib_install_prepost_hook(libffi 闭包):
//       * WorldGen.Reset    -> postfix 置"新世界生成中"标记
//       * Player.ResetEffects -> postfix 作为每帧 tick, 生成完成后分帧随机化
//   - 方块数据静态字段用 patchlib_field_get_pointer 一次性解析为真实指针,
//     热路径上直接解引用(与 VeinMiner 同方案);
//   - tileFrameImportant(bool[]) 用 patchlib_array_length/at 遍历;
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
#include <time.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

// ============ 状态 ============
static patch_hook_id_t g_hookReset_id = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_hookResetEffects_id = PATCH_HOOK_INVALID_ID;

static bool g_inWorldGen = false;   // 正在生成新世界
static bool g_done = false;         // 本次生成的随机化已完成
static bool g_workStarted = false;  // 是否已初始化(种子 + 安全表)
static int  g_progressY = 0;        // 已处理到的行
static int  g_workMaxY = 0;         // 世界高度快照
static uint64_t g_totalChanged = 0; // 已随机化的方块数

// ============ 类型/字段句柄 ============
static patch_handle_t g_main_type = NULL;
static patch_handle_t g_worldgen_type = NULL;
static patch_handle_t g_tiledata_type = NULL;
static patch_handle_t g_player_type = NULL;

static patch_handle_t g_maxTilesX_field = NULL;           // Main.maxTilesX       (static int)
static patch_handle_t g_maxTilesY_field = NULL;           // Main.maxTilesY       (static int)
static patch_handle_t g_generatingWorld_field = NULL;     // WorldGen.generatingWorld (static bool)
static patch_handle_t g_tileLookup_field = NULL;          // TileData.TileLookup  (static uint*)
static patch_handle_t g_tileType_field = NULL;            // TileData.TileType    (static ushort*)
static patch_handle_t g_tileSHeader_field = NULL;         // TileData.TileSHeader (static short*)
static patch_handle_t g_tileFrameX_field = NULL;          // TileData.TileFrameX  (static short*)
static patch_handle_t g_tileFrameY_field = NULL;          // TileData.TileFrameY  (static short*)
static patch_handle_t g_tileFrameImportant_field = NULL;  // Main.tileFrameImportant (static bool[])

/*
 * 字段真实指针缓存(仅 Android)。
 * 通过 patchlib_field_get_pointer 在初始化时一次性解析出各静态字段的存储地址,
 * 之后直接解引用。其中数组指针(TileLookup 等)的世界数据指针在换世界时可能被
 * 重新分配, 因此每次随机化时重新解引用取值。
 */
#if defined(__ANDROID__)
static int*       g_pMaxTilesX;   // &Main.maxTilesX       (static int)
static int*       g_pMaxTilesY;   // &Main.maxTilesY       (static int)
static bool*      g_pGeneratingWorld;   // &WorldGen.generatingWorld (static bool)
static uint32_t** g_pTileLookup;  // &TileData.TileLookup  (static uint*)
static uint16_t** g_pTileType;    // &TileData.TileType    (static ushort*)
static int16_t**  g_pTileSHeader; // &TileData.TileSHeader (static short*)
static int16_t**  g_pTileFrameX;  // &TileData.TileFrameX  (static short*)
static int16_t**  g_pTileFrameY;  // &TileData.TileFrameY  (static short*)
#endif

#if !defined(__ANDROID__)
/*
 * 桌面端方块数据: Main.tile 是 Tile[,] (2D 数组, 元素为 Tile 类对象引用)
 * 通过 Tile 对象字段读写类型/实心标志/帧
 */
static patch_handle_t g_tile_field = NULL;               // Main.tile (Tile[,])
static patch_handle_t g_tileTypeOfTile_field = NULL;     // Tile.type (ushort)
static patch_handle_t g_sTileHeader_field = NULL;        // Tile.sTileHeader (ushort)
static patch_handle_t g_frameXOfTile_field = NULL;       // Tile.frameX (short)
static patch_handle_t g_frameYOfTile_field = NULL;       // Tile.frameY (short)
static patch_handle_t g_tileFrameImportantArr_field = NULL; // Main.tileFrameImportant (bool[])
static patch_handle_t g_getTileSafely_method = NULL;   // Framing.GetTileSafely (static, 2 参)
// 桌面端全图随机化: 线性游标 + 每帧处理格数上限(优化, 防止卡顿)
static long g_workTileTotal = 0;
static long g_workTilePos = 0;
#endif

// ============ 随机范围 ============
// 所有合法图格类型 [0, TileID.Count), PE 端 TileID.Count = 753 (dump.cs:103101)
static const uint32_t kTileTypeCount = 753u;

// 随机候选类型列表(排除 tileFrameImportant 的结构/数据图格), 运行期构建
static uint16_t g_safeTypes[1024];
static int g_safeCount = 0;

static const uint32_t kNoTile     = 0xFFFFFFFFu;
static const int16_t  kActiveMask = 0x20;  // TileSHeader 实心标志 (PC Tile.cs:619)

// ============ 随机数 (xorshift32) ============
static uint32_t s_rng = 0x13579BDFu;

static void SeedRng(void) {
    s_rng = (uint32_t)time(NULL)
            ^ (uint32_t)(uintptr_t)&s_rng
            ^ 0x9E3779B9u;
}

static uint32_t Rand(void) {
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

static uint16_t RandomTileType(void) {
    if (g_safeCount > 0) {
        return g_safeTypes[Rand() % (uint32_t)g_safeCount];
    }
    return (uint16_t)(Rand() % kTileTypeCount);
}

/**
 * 构建安全随机候选表: 排除 Main.tileFrameImportant 图格
 * (箱/门/牌/床/梳妆台等, 其 frameX 被当作数据索引, 随机后无对应数据会崩溃)。
 * 返回候选数, 0 表示失败(解析不到 tileFrameImportant, 调用方应跳过随机化)。
 */
static int BuildSafeTypes(void) {
    g_safeCount = 0;
#if defined(__ANDROID__)
    if (!g_tileFrameImportant_field) return 0;
    void** slot = (void**)patchlib_field_get_pointer(g_tileFrameImportant_field, NULL);
    if (!slot || !*slot) return 0;
    void* fi = *slot;
    const size_t count = patchlib_array_length(fi);
    for (size_t i = 0; i < kTileTypeCount && i < count; ++i) {
        bool important = false;
        if (!patchlib_array_at(fi, i, &important)) continue;
        if (!important) {
            if (g_safeCount < (int)(sizeof(g_safeTypes) / sizeof(g_safeTypes[0]))) {
                g_safeTypes[g_safeCount++] = (uint16_t)i;
            }
        }
    }
#else
    // 桌面端: Main.tileFrameImportant 是 bool[] 静态字段
    if (!g_tileFrameImportantArr_field) return 0;
    void* fi = NULL;
    patchlib_field_get_value(g_tileFrameImportantArr_field, NULL, &fi);
    if (!fi) return 0;
    const size_t count = patchlib_array_length(fi);
    for (size_t i = 0; i < kTileTypeCount && i < count; ++i) {
        bool important = false;
        if (!patchlib_array_at(fi, i, &important)) continue;
        if (!important) {
            if (g_safeCount < (int)(sizeof(g_safeTypes) / sizeof(g_safeTypes[0]))) {
                g_safeTypes[g_safeCount++] = (uint16_t)i;
            }
        }
    }
#endif
    return g_safeCount;
}

// ============ 方块读写 (PE TileData 原生数组方案) ============

static uint32_t TileIndex(int x, int y) {
#if defined(__ANDROID__)
    if (!g_pMaxTilesX || !g_pMaxTilesY || !g_pTileLookup) return kNoTile;
    const int maxX = *g_pMaxTilesX;
    const int maxY = *g_pMaxTilesY;
    if (maxX <= 0 || maxY <= 0) return kNoTile;
    if (x < 0 || y < 0 || x >= maxX || y >= maxY) return kNoTile;
    const uint32_t* lookup = *g_pTileLookup;
    if (!lookup) return kNoTile;
    return lookup[(size_t)y * (size_t)maxX + (size_t)x];
#else
    (void)x; (void)y;
    return kNoTile;
#endif
}

#if !defined(__ANDROID__)
/** 桌面端: 用 Framing.GetTileSafely(x,y) 取 Tile 类对象 (内核 array_at 不支持 2D 数组); 失败返回 NULL */
static void* GetTileObject(int x, int y) {
    if (!g_getTileSafely_method || !g_maxTilesX_field || !g_maxTilesY_field) return NULL;
    int maxX = 0, maxY = 0;
    patchlib_field_get_value(g_maxTilesX_field, NULL, &maxX);
    patchlib_field_get_value(g_maxTilesY_field, NULL, &maxY);
    if (maxX <= 0 || maxY <= 0) return NULL;
    if (x < 0 || y < 0 || x >= maxX || y >= maxY) return NULL;
    int ix = x, iy = y;
    void* args[2];
    args[0] = &ix;
    args[1] = &iy;
    void* t = NULL;
    if (!patchlib_method_invoke_args(g_getTileSafely_method, NULL, &t, args)) return NULL;
    return t;
}
#endif

// ============ 全图随机化 Pass(分帧执行, 每帧处理一段) ============
static void Pass_RandomizeBand(int yFrom, int yTo) {
#if defined(__ANDROID__)
    if (!g_pMaxTilesX || !g_pMaxTilesY || !g_pTileType || !g_pTileSHeader) return;
    const int maxX = *g_pMaxTilesX;
    const int maxY = *g_pMaxTilesY;
    if (maxX <= 0 || maxY <= 0) return;
    if (yTo > maxY) yTo = maxY;
    if (yFrom < 0) yFrom = 0;

    uint16_t* types = *g_pTileType;
    int16_t*  sHead = *g_pTileSHeader;
    int16_t*  fx = g_pTileFrameX ? *g_pTileFrameX : NULL;
    int16_t*  fy = g_pTileFrameY ? *g_pTileFrameY : NULL;
    if (!types || !sHead) return;

    for (int y = yFrom; y < yTo; ++y) {
        for (int x = 0; x < maxX; ++x) {
            const uint32_t idx = TileIndex(x, y);
            if (idx == kNoTile) continue;
            // 只随机化实心方块: 空气/墙/液体所在格保持原样
            if ((sHead[idx] & kActiveMask) == 0) continue;

            types[idx] = RandomTileType();
            if (fx) fx[idx] = 0;
            if (fy) fy[idx] = 0;
            ++g_totalChanged;
        }
    }
#else
    // 桌面端不使用行带式分帧, 由 ResetEffects 线性游标调用 DesktopRandomizeTile
    (void)yFrom; (void)yTo;
    return;
#endif
}

#if !defined(__ANDROID__)
/** 桌面端: 随机化单个坐标的方块 (GetTileSafely 取 Tile 对象, 只处理实心方块) */
static void DesktopRandomizeTile(int x, int y) {
    if (!g_tile_field || !g_tileTypeOfTile_field || !g_sTileHeader_field) return;
    void* tile = GetTileObject(x, y);
    if (!tile) return;
    // 只随机化实心方块 (sTileHeader 的 bit5 == active)
    uint16_t sh = 0;
    patchlib_field_get_value(g_sTileHeader_field, tile, &sh);
    if ((sh & kActiveMask) == 0) return;

    const uint16_t rndType = RandomTileType();
    uint16_t t = rndType;
    patchlib_field_set_value(g_tileTypeOfTile_field, tile, &t);
    if (g_frameXOfTile_field) {
        short zero = 0;
        patchlib_field_set_value(g_frameXOfTile_field, tile, &zero);
    }
    if (g_frameYOfTile_field) {
        short zero = 0;
        patchlib_field_set_value(g_frameYOfTile_field, tile, &zero);
    }
    ++g_totalChanged;
}
#endif

// ============ 分帧参数 ============
#if defined(__ANDROID__)
static const int kRowsPerFrame = 48;   // 每帧处理的世界行数 (Android: 原生内存, 快)
#else
static const int kRowsPerFrame = 48;   // 桌面端不使用行带式, 该值仅占位
// 桌面端每帧处理的格数上限 (Framing.GetTileSafely 逐格托管调用较慢,
// 限制每帧工作量以分摊到多帧, 避免进入世界时卡死)
static const long kDesktopTilesPerFrame = 4000;
#endif

// ============ Hook 逻辑 ============

/** Hook: WorldGen.Reset -> 新世界开始生成 */
static void Reset_Postfix(patch_handle_t instance, void **args, void *result,
                          const patch_method_signature_t *sig_info) {
    (void)instance; (void)args; (void)result; (void)sig_info;
    g_inWorldGen = true;
    g_done = false;
    g_workStarted = false;
    g_progressY = 0;
    g_totalChanged = 0;
}

/** Hook: Player.ResetEffects(每帧) -> 生成完成后分帧执行全图随机化 */
static void ResetEffects_Postfix(patch_handle_t instance, void **args, void *result,
                                 const patch_method_signature_t *sig_info) {
    (void)instance; (void)args; (void)result; (void)sig_info;
    if (!g_inWorldGen || g_done) return;
    // generatingWorld 仍为 true 说明后台还在生成, 等待
#if defined(__ANDROID__)
    if (g_pGeneratingWorld && *g_pGeneratingWorld) return;
#endif

    // 首次调用: 初始化随机源与安全候选表
    if (!g_workStarted) {
        g_workStarted = true;
        g_progressY = 0;
#if defined(__ANDROID__)
        g_workMaxY = (g_pMaxTilesY && g_pMaxTilesX) ? *g_pMaxTilesY : 0;
#else
        // 桌面端: 全图随机化, 用线性游标分帧处理
        g_workMaxY = 0;
        g_workTileTotal = 0;
        g_workTilePos = 0;
        {
            int maxX = 0, maxY = 0;
            if (g_maxTilesX_field) patchlib_field_get_value(g_maxTilesX_field, NULL, &maxX);
            if (g_maxTilesY_field) patchlib_field_get_value(g_maxTilesY_field, NULL, &maxY);
            if (maxX < 1) maxX = 4200;
            if (maxY < 1) maxY = 1200;
            g_workTileTotal = (long)maxX * (long)maxY;
            g_workMaxY = maxY;
            if (mod_logger_write) {
                mod_logger_write(MOD_LOG_LEVEL_INFO, "ChaosGen",
                                 "desktop full-map randomize tiles=%ld", g_workTileTotal);
            }
        }
#endif
#if defined(__ANDROID__)
        if (g_workMaxY <= 0 || !g_pTileType || !g_pTileSHeader) {
#else
        if (g_workMaxY <= 0) {
#endif
            if (mod_logger_write) {
                mod_logger_write(MOD_LOG_LEVEL_ERROR, "ChaosGen",
                                 "tile fields not ready, skip randomization");
            }
            g_done = true;
            g_inWorldGen = false;
            return;
        }
        SeedRng();
        const int safeCount = BuildSafeTypes();
        if (safeCount <= 0) {
            if (mod_logger_write) {
                mod_logger_write(MOD_LOG_LEVEL_ERROR, "ChaosGen",
                                 "tileFrameImportant unavailable, skip randomization");
            }
            g_done = true;
            g_inWorldGen = false;
            return;
        }
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_INFO, "ChaosGen",
                             "== chaos generation start (safe types=%d) ==", safeCount);
        }
    }

    // 每帧处理一段 (Android: 行带式; 桌面端: 线性游标 + 每帧格数上限, 防止卡顿)
#if defined(__ANDROID__)
    const int from = g_progressY;
    int to = from + kRowsPerFrame;
    if (to > g_workMaxY) to = g_workMaxY;
    Pass_RandomizeBand(from, to);
    g_progressY = to;
    if (g_progressY >= g_workMaxY) {
#else
    // 桌面端优化: 每帧最多处理 kDesktopTilesPerFrame 格, 分摊到多帧避免卡死
    {
        int maxX = 0;
        if (g_maxTilesX_field) patchlib_field_get_value(g_maxTilesX_field, NULL, &maxX);
        if (maxX < 1) maxX = 4200;
        const long budget = kDesktopTilesPerFrame;
        long done = 0;
        while (done < budget && g_workTilePos < g_workTileTotal) {
            const long idx = g_workTilePos++;
            DesktopRandomizeTile((int)(idx % maxX), (int)(idx / maxX));
            ++done;
        }
        g_progressY = (int)(g_workTilePos / maxX);
        g_workMaxY = (int)(g_workTileTotal / maxX);
    }
    if (g_workTilePos >= g_workTileTotal) {
#endif
        g_done = true;
        g_inWorldGen = false;
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_INFO, "ChaosGen",
                             "== chaos generation done (changed=%llu) ==",
                             (unsigned long long)g_totalChanged);
        }
    }
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChaosGen", "初始化混沌生成模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChaosGen", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 获取类型 (桌面端没有 Terraria.TileData, 方块数据走 Main.tile)
    g_main_type = patchlib_type_get_type("Terraria", "Main");
    g_worldgen_type = patchlib_type_get_type("Terraria", "WorldGen");
    g_player_type = patchlib_type_get_type("Terraria", "Player");
#if defined(__ANDROID__)
    g_tiledata_type = patchlib_type_get_type("Terraria", "TileData");
    if (!g_main_type || !g_worldgen_type || !g_tiledata_type || !g_player_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ChaosGen",
                             "获取类型失败 (Main/WorldGen/TileData/Player)");
        }
        return;
    }
#else
    if (!g_main_type || !g_worldgen_type || !g_player_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ChaosGen",
                             "获取类型失败 (Main/WorldGen/Player)");
        }
        return;
    }
#endif

    // 2. 解析方块数据字段 (Android: TileData 原生数组; 桌面端: Main.tile + Framing.GetTileSafely)
    g_maxTilesX_field = patchlib_type_get_field(g_main_type, "maxTilesX");
    g_maxTilesY_field = patchlib_type_get_field(g_main_type, "maxTilesY");
    g_generatingWorld_field = patchlib_type_get_field(g_worldgen_type, "generatingWorld");

#if defined(__ANDROID__)
    g_tileLookup_field = patchlib_type_get_field(g_tiledata_type, "TileLookup");
    g_tileType_field = patchlib_type_get_field(g_tiledata_type, "TileType");
    g_tileSHeader_field = patchlib_type_get_field(g_tiledata_type, "TileSHeader");
    g_tileFrameX_field = patchlib_type_get_field(g_tiledata_type, "TileFrameX");
    g_tileFrameY_field = patchlib_type_get_field(g_tiledata_type, "TileFrameY");
    g_tileFrameImportant_field = patchlib_type_get_field(g_main_type, "tileFrameImportant");

    if (!g_maxTilesX_field || !g_maxTilesY_field || !g_generatingWorld_field ||
        !g_tileLookup_field || !g_tileType_field || !g_tileSHeader_field ||
        !g_tileFrameX_field || !g_tileFrameY_field || !g_tileFrameImportant_field) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ChaosGen", "获取方块字段失败");
        }
        return;
    }

    // 一次性解析字段真实指针, 供 TileIndex/Pass_RandomizeBand 直接解引用
    g_pMaxTilesX = (int*)patchlib_field_get_pointer(g_maxTilesX_field, NULL);
    g_pMaxTilesY = (int*)patchlib_field_get_pointer(g_maxTilesY_field, NULL);
    g_pGeneratingWorld = (bool*)patchlib_field_get_pointer(g_generatingWorld_field, NULL);
    g_pTileLookup = (uint32_t**)patchlib_field_get_pointer(g_tileLookup_field, NULL);
    g_pTileType = (uint16_t**)patchlib_field_get_pointer(g_tileType_field, NULL);
    g_pTileSHeader = (int16_t**)patchlib_field_get_pointer(g_tileSHeader_field, NULL);
    g_pTileFrameX = (int16_t**)patchlib_field_get_pointer(g_tileFrameX_field, NULL);
    g_pTileFrameY = (int16_t**)patchlib_field_get_pointer(g_tileFrameY_field, NULL);

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChaosGen",
                         "maxTilesX=%p maxTilesY=%p generatingWorld=%p",
                         (void*)g_pMaxTilesX, (void*)g_pMaxTilesY,
                         (void*)g_pGeneratingWorld);
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChaosGen",
                         "lookup=%p type=%p sHeader=%p frameX=%p frameY=%p",
                         (void*)g_pTileLookup, (void*)g_pTileType,
                         (void*)g_pTileSHeader, (void*)g_pTileFrameX,
                         (void*)g_pTileFrameY);
    }
#else
    // 桌面端: Main.tile 是 Tile[,], 读 Tile 类对象字段
    g_tile_field = patchlib_type_get_field(g_main_type, "tile");
    patch_handle_t tile_type = patchlib_type_get_type("Terraria", "Tile");
    if (tile_type) {
        g_tileTypeOfTile_field = patchlib_type_get_field(tile_type, "type");
        g_sTileHeader_field = patchlib_type_get_field(tile_type, "sTileHeader");
        g_frameXOfTile_field = patchlib_type_get_field(tile_type, "frameX");
        g_frameYOfTile_field = patchlib_type_get_field(tile_type, "frameY");
    }
    g_tileFrameImportantArr_field = patchlib_type_get_field(g_main_type, "tileFrameImportant");
    patch_handle_t framing_type = patchlib_type_get_type("Terraria", "Framing");
    if (framing_type) {
        g_getTileSafely_method = patchlib_type_get_method_by_param_count(framing_type, "GetTileSafely", 2);
        if (!g_getTileSafely_method) g_getTileSafely_method = patchlib_type_get_method(framing_type, "GetTileSafely");
    }

    if (!g_tile_field || !g_tileTypeOfTile_field || !g_sTileHeader_field ||
        !g_tileFrameImportantArr_field || !g_getTileSafely_method) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ChaosGen", "获取桌面端方块字段失败");
        }
        return;
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChaosGen",
                         "tileF=%p typeF=%p headerF=%p fxF=%p fyF=%p tfiF=%p safely=%p",
                         (void*)g_tile_field, (void*)g_tileTypeOfTile_field,
                         (void*)g_sTileHeader_field, (void*)g_frameXOfTile_field,
                         (void*)g_frameYOfTile_field, (void*)g_tileFrameImportantArr_field,
                         (void*)g_getTileSafely_method);
    }
#endif

    // 3. 获取 WorldGen.Reset(静态方法, 0 参数)
    patch_handle_t reset_method = patchlib_type_get_method_by_param_count(g_worldgen_type, "Reset", 0);
    if (!reset_method) {
        reset_method = patchlib_type_get_method(g_worldgen_type, "Reset");
    }
    if (!reset_method) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ChaosGen", "获取 WorldGen.Reset 方法失败");
        }
        return;
    }

    // 4. 安装后缀 Hook(生成开始标记)
    g_hookReset_id = patchlib_install_prepost_hook(reset_method, NULL, Reset_Postfix);
    if (g_hookReset_id == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ChaosGen", "安装 Reset Hook 失败");
        }
        return;
    }

    // 5. 获取 Player.ResetEffects(实例方法, 0 参数), 用作每帧 tick
    patch_handle_t resetEffects_method = patchlib_type_get_method_by_param_count(g_player_type, "ResetEffects", 0);
    if (!resetEffects_method) {
        resetEffects_method = patchlib_type_get_method(g_player_type, "ResetEffects");
    }
    if (!resetEffects_method) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ChaosGen", "获取 ResetEffects 方法失败");
        }
        return;
    }

    // 6. 安装后缀 Hook(每帧检查并分帧随机化)
    g_hookResetEffects_id = patchlib_install_prepost_hook(resetEffects_method, NULL, ResetEffects_Postfix);
    if (g_hookResetEffects_id == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ChaosGen", "安装 ResetEffects Hook 失败");
        }
        return;
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChaosGen",
                         "成功 Hook Reset (hook_id=%d) / ResetEffects (hook_id=%d), 混沌生成已启用",
                         (int)g_hookReset_id, (int)g_hookResetEffects_id);
    }
}

// ============ 模块清理 ============
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;

    if (g_hookReset_id != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookReset_id);
        g_hookReset_id = PATCH_HOOK_INVALID_ID;
    }
    if (g_hookResetEffects_id != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookResetEffects_id);
        g_hookResetEffects_id = PATCH_HOOK_INVALID_ID;
    }

    g_inWorldGen = false;
    g_done = false;
    g_workStarted = false;
    g_progressY = 0;
    g_workMaxY = 0;
    g_totalChanged = 0;
    g_safeCount = 0;

#if defined(__ANDROID__)
    g_pMaxTilesX = NULL;
    g_pMaxTilesY = NULL;
    g_pGeneratingWorld = NULL;
    g_pTileLookup = NULL;
    g_pTileType = NULL;
    g_pTileSHeader = NULL;
    g_pTileFrameX = NULL;
    g_pTileFrameY = NULL;
#else
    g_tile_field = NULL;
    g_tileTypeOfTile_field = NULL;
    g_sTileHeader_field = NULL;
    g_frameXOfTile_field = NULL;
    g_frameYOfTile_field = NULL;
    g_tileFrameImportantArr_field = NULL;
    g_getTileSafely_method = NULL;
    g_workTileTotal = 0;
    g_workTilePos = 0;
#endif

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChaosGen", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.chaosgen",
        .version_code = 1,
        .api_version = 1,
        .version = "1.1.0",
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
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChaosGen", "混沌生成模组实例创建");
    }
    return &g_ops;
}
