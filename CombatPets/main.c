//
// CombatPets - 战斗宠物
// NewEFMod (tefkernel / KernelLoader) 版, 适配 Terraria 1.4.5.x (手机端 / PE)
//
// 功能:
//   宠物保留各自的 AI 类型与行为(AI 完全不动), 在此基础上:
//   1. 附近(500px)有敌怪时, 宠物主动跑向敌怪(直接位移), 靠近后
//      靠弹幕接触伤害结算;
//   2. 伤害随世界进程(时期)成长, 参考 wiki 各宠物页面;
//   3. 宠物召唤物品标记为召唤伤害物品(summon = true)并同步
//      item.damage, 物品提示可见。
//
// 时期(阶段倍率见 g_stage_mult):
//   0 初始 → 1 克眼/世吞/克脑/史王/蜂王 → 2 骷髅王 → 3 困难模式(肉山)
//   → 4 任意机械Boss → 5 世纪之花 → 6 石巨人 → 7 拜月教徒 → 8 月总
//
// 实现要点(结合 PE 1.4.5.6.4 dump.cs 与 PC 1.4.5.6 源码):
//   1. 识别宠物: Main.projPet (静态 bool[]), 122 个;
//      755(光之蝙蝠)/946(光之刃) 本身是仆从, 跳过。
//   2. 追击: Hook Projectile.AI 的 Postfix, 宠物原 AI 执行完(位置自然)
//      之后, 若 500px 内有敌怪, 直接把宠物位置向敌怪移动(每帧 8px),
//      宠物本体保持各自 AI 行为不变。
//   3. 伤害结算: 原版 Damage_CanDealDamage() (Projectile.cs:11810) 对
//      Main.projPet 弹幕一律禁伤(755 在 ai[0]!=0 冲刺状态除外)。
//      Hook Projectile.Damage 的 Prefix/Postfix: 执行期间把 type 伪装成
//      755, 且 ai[0]==0 时临时置 1(保存恢复), 绕过禁伤检查;
//      pet 弹幕不是 minion, 不走 originalDamage 每帧重算
//      (Projectile.cs:15365), 直接写 proj.damage 即可生效。
//      宠物多数 penetrate=-1, 接触后按 NPC 全局免疫帧重复结算。
//   4. 敌怪有效性近似 CanBeChasedBy: active && !friendly &&
//      !townNPC && !dontTakeDamage && life > 0。
//   5. 物品: Item.SetDefaults Postfix 里, 若 item.shoot 是宠物弹幕,
//      设 item.summon = true(与原版召唤武器一致, Item.cs:294),
//      并把 item.damage 设为当前时期伤害(物品提示可见)。
//   6. 仅处理本地玩家的宠物(owner == Main.myPlayer), 避免联机不同步。
//
// 与 AutoFisher 示例的差异:
//   - prefix 返回值语义与头文件注释相反: false = 执行原方法;
//   - Main.projPet/Main.npc 是托管数组, 用 patchlib_array_at 访问;
//   - NPC.downed* / Main.hardMode 是静态字段,
//     patchlib_field_get_value 传 instance=NULL 读取。
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

// Collision.SolidTiles(int,int,int,int) (Collision.cs:3468) — 全 int 参数,
// 用于墙体检测(视线/下一步阻挡), 避免直接位移穿墙
static patch_handle_t g_solidTiles_method = NULL;

#define PROJ_BAT_OF_LIGHT 755
#define PROJ_EMPRESS_BLADE 946
#define CHASE_RANGE_SQ (500.0f * 500.0f)
#define CHASE_SPEED 8.0f
#define PET_KNOCKBACK 2.0f

// ============ 宠物弹幕表 (取自 Main.projPet, 除去 755/946) ============
static const int g_pet_projs[] = {
    492, 499, 653, 701, 703, 702, 764, 765, 319, 334, 324, 266,
    313, 314, 317, 175, 111, 112, 127, 191, 192, 193, 194, 197,
    198, 199, 200, 208, 209, 210, 211, 236, 268, 269, 353, 373,
    375, 380, 387, 388, 390, 391, 392, 393, 394, 395, 1093, 1094,
    398, 407, 423, 533, 613, 623, 625, 626, 627, 628, 758, 759,
    774, 815, 816, 817, 821, 825, 831, 833, 834, 835, 854, 858,
    859, 860, 864, 875, 951, 963, 970, 1022, 881, 882, 883, 884,
    885, 886, 887, 888, 889, 890, 891, 892, 893, 894, 895, 896,
    897, 898, 899, 900, 901, 934, 956, 957, 958, 959, 960, 994,
    998, 1003, 1004, 1018, 1056, 1090, 1027, 1046, 1050, 1095, 1096
};
#define PET_PROJ_COUNT (sizeof(g_pet_projs) / sizeof(g_pet_projs[0]))

// ============ 宠物基础伤害表 (键值对, 近似 wiki 1.4.5 数值) ============
static const struct { int proj; int dmg; } g_pet_dmg[] = {
    { 236, 12 }, { 266, 14 }, { 268, 15 }, { 269, 15 }, { 313, 14 },
    { 314, 14 }, { 317, 14 }, { 319, 15 }, { 324, 15 }, { 334, 13 },
    { 373, 14 }, { 375, 16 }, { 380, 20 }, { 387, 16 }, { 395, 18 },
    { 398, 18 }, { 407, 20 }, { 423, 22 }, { 533, 24 }, { 613, 25 },
    { 623, 26 }, { 625, 26 }, { 626, 26 }, { 627, 26 }, { 628, 26 },
    { 653, 22 }, { 701, 24 }, { 702, 24 }, { 703, 24 }, { 758, 30 },
    { 759, 30 }, { 764, 28 }, { 765, 28 }, { 774, 28 }, { 815, 32 },
    { 816, 32 }, { 817, 32 }, { 821, 32 }, { 825, 34 }, { 831, 34 },
    { 833, 34 }, { 834, 34 }, { 835, 34 }, { 854, 35 }, { 858, 35 },
    { 859, 35 }, { 860, 35 }, { 864, 36 }, { 875, 38 }, { 881, 30 },
    { 882, 30 }, { 883, 30 }, { 884, 30 }, { 885, 30 }, { 886, 30 },
    { 887, 30 }, { 888, 30 }, { 889, 30 }, { 890, 30 }, { 891, 30 },
    { 892, 30 }, { 893, 30 }, { 894, 30 }, { 895, 30 }, { 896, 30 },
    { 897, 30 }, { 898, 30 }, { 899, 30 }, { 900, 30 }, { 901, 30 },
    { 934, 38 }, { 951, 40 }, { 956, 42 }, { 957, 42 }, { 958, 42 },
    { 959, 42 }, { 960, 42 }, { 963, 45 }, { 970, 45 }, { 994, 48 },
    { 998, 48 }, { 1003, 48 }, { 1004, 48 }, { 1018, 50 }, { 1022, 50 },
    { 1027, 52 }, { 1046, 55 }, { 1050, 55 }, { 1056, 52 }, { 1090, 52 },
    { 1093, 30 }, { 1094, 30 }, { 1095, 55 }, { 1096, 55 }, { 175, 10 },
    { 111, 10 }, { 112, 10 }, { 127, 10 }, { 191, 10 }, { 192, 10 },
    { 193, 10 }, { 194, 10 }, { 197, 10 }, { 198, 10 }, { 199, 10 },
    { 200, 10 }, { 208, 10 }, { 209, 10 }, { 210, 10 }, { 211, 10 },
    { 353, 12 }, { 492, 18 }, { 499, 18 }
};
#define PET_DMG_COUNT (sizeof(g_pet_dmg) / sizeof(g_pet_dmg[0]))

// ============ 阶段倍率 (下标 0~8, 见文件头说明) ============
// 倍率控制终盘伤害在 60~100 之间(封顶 100), 叠加玩家召唤加成后的
// 实际值与原版仆从相当。
static const float g_stage_mult[9] =
    { 1.0f, 1.2f, 1.5f, 2.0f, 2.4f, 2.8f, 3.2f, 3.6f, 4.0f };
#define DAMAGE_CAP 100

// ============ 状态 ============
static patch_hook_id_t g_hook_ai = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_hook_item = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_hook_dmg = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_hook_addbuff = PATCH_HOOK_INVALID_ID;
static bool g_ready = false;

// 追击状态: 按弹幕 whoAmI 记录当前追击目标 (Main.npc 下标, -1 无)
static int g_chase_target[1000];
// 按弹幕 whoAmI 记录上次全量扫描的帧号(限频, 避免每帧扫 1000 NPC)
static int g_chase_scan_tick[1000];
static bool g_skip_ai = false;   // 本帧跳过宠物原 AI(冲刺接管)

// Main.projPet 查询表(初始化时从 g_pet_projs 构建, 避免热路径反射)
#define PROJ_TYPE_MAX 1110
static bool g_pet_lookup[PROJ_TYPE_MAX];

// 世界阶段缓存(每 120 个 AI 帧刷新一次)
static int g_stage_cache = 0;
static int g_stage_tick = 0;

// Damage 单线程逐个调用, 伪装上下文用静态变量保存
static bool g_dmg_spoofing = false;
static int g_origType = 0;
static float g_origAi0 = 0.0f;
static bool g_ai0_modified = false;

// ============ 类型句柄 ============
static patch_handle_t g_main_type = NULL;        // Terraria.Main
static patch_handle_t g_npc_type = NULL;         // Terraria.NPC
static patch_handle_t g_player_type = NULL;      // Terraria.Player
static patch_handle_t g_item_type = NULL;        // Terraria.Item
static patch_handle_t g_projectile_type = NULL;  // Terraria.Projectile
static patch_handle_t g_entity_type = NULL;      // Terraria.Entity

// ============ 字段句柄 ============
static patch_handle_t g_projPet_field = NULL;    // Main.projPet (static bool[])
static patch_handle_t g_npcArray_field = NULL;   // Main.npc (static NPC[])
static patch_handle_t g_hardMode_field = NULL;   // Main.hardMode (static bool)
static patch_handle_t g_proj_active_field = NULL; // Projectile.active (bool)
static patch_handle_t g_proj_type_field = NULL;  // Projectile.type (int)
static patch_handle_t g_proj_owner_field = NULL; // Projectile.owner (int)
static patch_handle_t g_proj_damage_field = NULL;  // Projectile.damage (int)
static patch_handle_t g_proj_friendly_field = NULL; // Projectile.friendly (bool)
static patch_handle_t g_proj_knockback_field = NULL; // Projectile.knockBack (float)
static patch_handle_t g_proj_ai_field = NULL;       // Projectile.ai (float x3)
static patch_handle_t g_proj_vel_field = NULL;      // Entity.velocity (Vector2)
static patch_handle_t g_proj_timeLeft_field = NULL; // Projectile.timeLeft (int)
static patch_handle_t g_proj_spriteDir_field = NULL; // Projectile.spriteDirection (int)
static patch_handle_t g_proj_whoAmI_field = NULL;   // Entity.whoAmI (int)
static patch_handle_t g_proj_pos_field = NULL;      // Entity.position (Vector2)
static patch_handle_t g_proj_width_field = NULL;    // Entity.width (int)
static patch_handle_t g_proj_height_field = NULL;   // Entity.height (int)
static patch_handle_t g_npc_active_field = NULL;    // NPC.active (bool)
static patch_handle_t g_npc_friendly_field = NULL;  // NPC.friendly (bool)
static patch_handle_t g_npc_townNPC_field = NULL;   // NPC.townNPC (bool)
static patch_handle_t g_npc_dontTake_field = NULL;  // NPC.dontTakeDamage (bool)
static patch_handle_t g_npc_life_field = NULL;      // NPC.life (int)
static patch_handle_t g_npc_lifeMax_field = NULL;   // NPC.lifeMax (int)
static patch_handle_t g_npc_chaseable_field = NULL; // NPC.chaseable (bool)
static patch_handle_t g_npc_immortal_field = NULL;  // NPC.immortal (bool)
static patch_handle_t g_item_shoot_field = NULL;    // Item.shoot (int)
static patch_handle_t g_item_damage_field = NULL;   // Item.damage (int)
static patch_handle_t g_item_summon_field = NULL;   // Item.summon (bool)

// ============ 本地玩家编号(平台相关) ============
#if defined(__ANDROID__)
static int (*g_get_myPlayer)(void);                  // Main.get_myPlayer
#else
static patch_handle_t g_myPlayer_field = NULL;       // Main.myPlayer (static int)
#endif

// NPC.downed* 静态字段
static patch_handle_t g_downed1 = NULL;
static patch_handle_t g_downed2 = NULL;
static patch_handle_t g_downed3 = NULL;
static patch_handle_t g_downedSlimeKing = NULL;
static patch_handle_t g_downedQueenBee = NULL;
static patch_handle_t g_downedMechAny = NULL;
static patch_handle_t g_downedPlant = NULL;
static patch_handle_t g_downedGolem = NULL;
static patch_handle_t g_downedCultist = NULL;
static patch_handle_t g_downedMoonlord = NULL;

// ============ 工具 ============
static bool ReadStaticBool(patch_handle_t field) {
    bool v = false;
    if (field) patchlib_field_get_value(field, NULL, &v);
    return v;
}

static int ObjInt(patch_handle_t field, void* obj) {
    if (!field || !obj) return 0;
#if defined(__ANDROID__)
    int* p = (int*)patchlib_field_get_pointer(field, obj);
    return p ? *p : 0;
#else
    int v = 0;
    patchlib_field_get_value(field, obj, &v);
    return v;
#endif
}

static void ObjSetInt(patch_handle_t field, void* obj, int v) {
    if (!field || !obj) return;
#if defined(__ANDROID__)
    int* p = (int*)patchlib_field_get_pointer(field, obj);
    if (p) *p = v;
#else
    patchlib_field_set_value(field, obj, &v);
#endif
}

static void ObjSetFloat(patch_handle_t field, void* obj, float v) {
    if (!field || !obj) return;
#if defined(__ANDROID__)
    float* p = (float*)patchlib_field_get_pointer(field, obj);
    if (p) *p = v;
#else
    patchlib_field_set_value(field, obj, &v);
#endif
}

/** 读取弹幕 ai 数组第 idx 个 float (Android 内联 struct / 桌面托管数组) */
static void ObjSetBool(patch_handle_t field, void* obj, bool v) {
    if (!field || !obj) return;
#if defined(__ANDROID__)
    bool* p = (bool*)patchlib_field_get_pointer(field, obj);
    if (p) *p = v;
#else
    patchlib_field_set_value(field, obj, &v);
#endif
}

static bool ProjAiGet(void* proj, size_t idx, float* out) {
    if (!proj || !g_proj_ai_field || !out) return false;
#if defined(__ANDROID__)
    float* p = (float*)patchlib_field_get_pointer(g_proj_ai_field, proj);
    if (!p) return false;
    *out = p[idx];
    return true;
#else
    void* arr = NULL;
    patchlib_field_get_value(g_proj_ai_field, proj, &arr);
    if (!arr) return false;
    return patchlib_array_at(arr, idx, out);
#endif
}

static bool EntityPosGet(void* ent, float* x, float* y) {
    if (!ent || !g_proj_pos_field) return false;
#if defined(__ANDROID__)
    float* p = (float*)patchlib_field_get_pointer(g_proj_pos_field, ent);
    if (!p) return false;
    *x = p[0]; *y = p[1];
    return true;
#else
    float pos[2] = { 0.0f, 0.0f };
    patchlib_field_get_value(g_proj_pos_field, ent, pos);
    *x = pos[0]; *y = pos[1];
    return true;
#endif
}

static void EntityPosAdd(void* ent, float dx, float dy) {
    if (!ent || !g_proj_pos_field) return;
#if defined(__ANDROID__)
    float* p = (float*)patchlib_field_get_pointer(g_proj_pos_field, ent);
    if (!p) return;
    p[0] += dx; p[1] += dy;
#else
    float pos[2] = { 0.0f, 0.0f };
    patchlib_field_get_value(g_proj_pos_field, ent, pos);
    pos[0] += dx; pos[1] += dy;
    patchlib_field_set_value(g_proj_pos_field, ent, pos);
#endif
}

static void EntityVelGet(void* ent, float* x, float* y) {
    if (!ent || !g_proj_vel_field) return;
#if defined(__ANDROID__)
    float* p = (float*)patchlib_field_get_pointer(g_proj_vel_field, ent);
    if (!p) return;
    *x = p[0]; *y = p[1];
#else
    float v[2] = { 0.0f, 0.0f };
    patchlib_field_get_value(g_proj_vel_field, ent, v);
    *x = v[0]; *y = v[1];
#endif
}

static void EntityVelSet(void* ent, float x, float y) {
    if (!ent || !g_proj_vel_field) return;
#if defined(__ANDROID__)
    float* p = (float*)patchlib_field_get_pointer(g_proj_vel_field, ent);
    if (!p) return;
    p[0] = x; p[1] = y;
#else
    float v[2] = { x, y };
    patchlib_field_set_value(g_proj_vel_field, ent, v);
#endif
}

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

static bool IsPetProj(int type) {
    return type >= 0 && type < PROJ_TYPE_MAX && g_pet_lookup[type];
}

/** 计算当前世界进程阶段 (0~8), 见文件头 */
static int CurrentStage(void) {
    if (ReadStaticBool(g_downedMoonlord)) return 8;
    if (ReadStaticBool(g_downedCultist)) return 7;
    if (ReadStaticBool(g_downedGolem)) return 6;
    if (ReadStaticBool(g_downedPlant)) return 5;
    if (ReadStaticBool(g_downedMechAny)) return 4;
    if (ReadStaticBool(g_hardMode_field)) return 3;
    if (ReadStaticBool(g_downed3)) return 2;
    if (ReadStaticBool(g_downed1) || ReadStaticBool(g_downed2) ||
        ReadStaticBool(g_downedSlimeKing) || ReadStaticBool(g_downedQueenBee)) return 1;
    return 0;
}

/** 查表: 宠物基础伤害 */
static int PetBaseDmg(int projType) {
    for (size_t i = 0; i < PET_DMG_COUNT; ++i) {
        if (g_pet_dmg[i].proj == projType) return g_pet_dmg[i].dmg;
    }
    return 10;
}

/** 宠物在当前时期的伤害 (阶段每 120 帧刷新一次缓存) */
static int PetDamageNow(int projType) {
    if (++g_stage_tick >= 120) {
        g_stage_tick = 0;
        g_stage_cache = CurrentStage();
    }
    int stage = g_stage_cache;
    if (stage > 8) stage = 8;
    float d = (float)PetBaseDmg(projType) * g_stage_mult[stage];
    if (d > DAMAGE_CAP) d = DAMAGE_CAP;
    return (int)(d + 0.5f);
}

static void ProjAiSet(void* proj, size_t idx, float v) {
    if (!proj || !g_proj_ai_field) return;
#if defined(__ANDROID__)
    float* p = (float*)patchlib_field_get_pointer(g_proj_ai_field, proj);
    if (p) p[idx] = v;
#else
    void* arr = NULL;
    patchlib_field_get_value(g_proj_ai_field, proj, &arr);
    if (arr) patchlib_array_set(arr, idx, &v);
#endif
}

/** 读取实体中心点 */
static bool EntityCenter(void* ent, float* cx, float* cy) {
    float x, y;
    if (!EntityPosGet(ent, &x, &y)) return false;
    *cx = x + (float)ObjInt(g_proj_width_field, ent) * 0.5f;
    *cy = y + (float)ObjInt(g_proj_height_field, ent) * 0.5f;
    return true;
}

/** 敌怪有效性 (对齐 NPC.CanBeChasedBy, NPC.cs:91070)
 *  active && chaseable && lifeMax > 5 && !dontTakeDamage && !friendly && !immortal
 *  chaseable 排除训练假人/小动物等; lifeMax > 5 排除史莱姆幼体等弱小生物 */
static bool IsChaseableEnemy(void* npc) {
    if (!ObjInt(g_npc_active_field, npc)) return false;
    if (ObjInt(g_npc_lifeMax_field, npc) <= 5) return false;
    if (ObjInt(g_npc_life_field, npc) <= 0) return false;
#if defined(__ANDROID__)
    bool* c = (bool*)patchlib_field_get_pointer(g_npc_chaseable_field, npc);
    if (c && !*c) return false;
    bool* f = (bool*)patchlib_field_get_pointer(g_npc_friendly_field, npc);
    if (f && *f) return false;
    bool* d = (bool*)patchlib_field_get_pointer(g_npc_dontTake_field, npc);
    if (d && *d) return false;
    bool* m = (bool*)patchlib_field_get_pointer(g_npc_immortal_field, npc);
    if (m && *m) return false;
#else
    bool cv = false, fv = false, dv = false, mv = false;
    patchlib_field_get_value(g_npc_chaseable_field, npc, &cv);
    patchlib_field_get_value(g_npc_friendly_field, npc, &fv);
    patchlib_field_get_value(g_npc_dontTake_field, npc, &dv);
    patchlib_field_get_value(g_npc_immortal_field, npc, &mv);
    if (!cv || fv || dv || mv) return false;
#endif
    return true;
}

/** 扫描宠物附近 (500px) 的最近敌怪, 返回 Main.npc 下标, 无则 -1 */
static int FindNearestEnemy(void* pet) {
    if (!g_npcArray_field) return -1;
    float px, py;
    if (!EntityCenter(pet, &px, &py)) return -1;

    void* arr = NULL;
    patchlib_field_get_value(g_npcArray_field, NULL, &arr);
    if (!arr) return -1;

    int best = -1;
    float bestD2 = CHASE_RANGE_SQ;
    const size_t n = patchlib_array_length(arr);
    for (size_t i = 0; i < n; ++i) {
        void* npc = NULL;
        if (!patchlib_array_at(arr, i, &npc) || !npc) continue;
        if (!IsChaseableEnemy(npc)) continue;
        float nx, ny;
        if (!EntityCenter(npc, &nx, &ny)) continue;
        const float dx = nx - px, dy = ny - py;
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestD2) {
            bestD2 = d2;
            best = (int)i;
        }
    }
    return best;
}

/** 查询世界坐标所在图格是否为实心方块 */
static bool TileSolidAt(float wx, float wy) {
    if (!g_solidTiles_method) return false;
    const int tx = (int)(wx / 16.0f);
    const int ty = (int)(wy / 16.0f);
    int r = 0;
    void* args[4] = { &tx, &tx, &ty, &ty };
    if (!patchlib_method_invoke_args(g_solidTiles_method, NULL, &r, args)) return false;
    return r != 0;
}

/** 宠物中心到目标中心的直线路径是否通畅(按图格采样, 步长 16px) */
static bool PathClear(float x1, float y1, float x2, float y2) {
    const float dx = x2 - x1, dy = y2 - y1;
    const float dist = sqrtf(dx * dx + dy * dy);
    const int steps = (int)(dist / 16.0f);
    for (int k = 1; k <= steps; ++k) {
        const float t = (float)k / (float)(steps + 1);
        if (TileSolidAt(x1 + dx * t, y1 + dy * t)) return false;
    }
    return true;
}

/** 校验追击目标是否仍有效(存活且未远离且路径通畅) */
static bool ChaseTargetValid(void* pet, int target) {
    if (target < 0 || !g_npcArray_field) return false;
    void* arr = NULL;
    patchlib_field_get_value(g_npcArray_field, NULL, &arr);
    if (!arr) return false;
    void* npc = NULL;
    if (!patchlib_array_at(arr, (size_t)target, &npc) || !npc) return false;
    if (!IsChaseableEnemy(npc)) return false;
    // 目标远离宠物 700px 以上则放弃
    float px, py, nx, ny;
    if (!EntityCenter(pet, &px, &py) || !EntityCenter(npc, &nx, &ny)) return false;
    const float dx = nx - px, dy = ny - py;
    if (dx * dx + dy * dy > 700.0f * 700.0f) return false;
    // 视线被墙挡住则放弃追击(原地跟随玩家), 避免穿墙
    return PathClear(px, py, nx, ny);
}

/** 冲刺一步: 直线冲向目标(速度 10px/帧), 返回是否仍在追击 */
static bool ChaseStep(void* pet, int target) {
    void* arr = NULL;
    patchlib_field_get_value(g_npcArray_field, NULL, &arr);
    if (!arr) return false;
    void* npc = NULL;
    if (!patchlib_array_at(arr, (size_t)target, &npc) || !npc) return false;

    float px, py, nx, ny;
    if (!EntityCenter(pet, &px, &py) || !EntityCenter(npc, &nx, &ny)) return false;
    const float dx = nx - px, dy = ny - py;
    const float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 0.001f) return false;

    // 贴身判定按双方碰撞箱计算, 保证接触(硬编码 24px 会导致
    // 小型敌怪碰不到碰撞箱, 宠物悬停卡死且无接触伤害)
    const float pw = (float)ObjInt(g_proj_width_field, pet);
    const float nw = (float)ObjInt(g_proj_width_field, npc);
    const float stopDist = (pw + nw) * 0.25f > 8.0f ? (pw + nw) * 0.25f : 8.0f;

    if (dist <= stopDist) {
        // 保持小幅压向目标, 维持重叠以持续触发接触伤害,
        // 围绕中心来回微动而不会彻底停死
        EntityVelSet(pet, dx / dist * 1.5f, dy / dist * 1.5f);
        return true;
    }

    // 速度平滑: 当前速度向目标方向缓转(0.25/帧), 冲刺呈柔和弧线
    float cvx = 0.0f, cvy = 0.0f;
    EntityVelGet(pet, &cvx, &cvy);
    const float wantX = dx / dist * CHASE_SPEED;
    const float wantY = dy / dist * CHASE_SPEED;
    float vx = cvx + (wantX - cvx) * 0.25f;
    float vy = cvy + (wantY - cvy) * 0.25f;

    // 下一位置被方块挡住: 停止移动并放弃追击(交给原 AI), 不穿墙
    if (TileSolidAt(px + vx, py + vy)) {
        EntityVelSet(pet, 0.0f, 0.0f);
        return false;
    }

    EntityPosAdd(pet, vx, vy);
    EntityVelSet(pet, vx, vy);
    // 朝向
    if (g_proj_spriteDir_field)
        ObjSetInt(g_proj_spriteDir_field, pet, vx >= 0.0f ? 1 : -1);
    return true;
}

// ============ Hook: Projectile.AI (Prefix + Postfix) ============
// 冲刺状态下跳过宠物原 AI(完全接管, 路径笔直), 其余帧原 AI 照常执行。
// 注意 prefix 返回值语义与头文件注释相反: true = 跳过原方法。
static bool AI_Prefix(patch_handle_t instance, void **args,
                      const patch_method_signature_t *sig_info, void *result) {
    (void)args; (void)sig_info; (void)result;
    if (!g_ready || g_skip_ai || !instance) return false;

    const int type = ObjInt(g_proj_type_field, instance);
    if (!IsPetProj(type)) return false;
    if (!ObjInt(g_proj_active_field, instance)) return false;

    const int myPlayer = LocalPlayer();
    if (myPlayer < 0 || ObjInt(g_proj_owner_field, instance) != myPlayer) return false;

    const int whoAmI = ObjInt(g_proj_whoAmI_field, instance);
    if (whoAmI < 0 || whoAmI >= 1000) return false;

    // 伤害随时期成长(pet 非 minion, 不会被 originalDamage 重算覆盖)
    const int dmg = PetDamageNow(type);
    ObjSetInt(g_proj_damage_field, instance, dmg);
    ObjSetFloat(g_proj_knockback_field, instance, PET_KNOCKBACK);
    // 接触伤害需要 friendly
    ObjSetBool(g_proj_friendly_field, instance, true);

    // 追击状态机: 有效目标 => 接管; 无 => 归还原版 AI
    // 无目标时限频扫描(每 ~20 帧一次), 避免每帧全量扫 1000 个 NPC
    static int ai_tick = 0;
    const int tick = ++ai_tick;
    int target = g_chase_target[whoAmI];
    if (!ChaseTargetValid(instance, target)) {
        if (tick - g_chase_scan_tick[whoAmI] >= 20) {
            g_chase_scan_tick[whoAmI] = tick;
            target = FindNearestEnemy(instance);
            g_chase_target[whoAmI] = target;
        } else {
            target = -1;
        }
    }
    if (target >= 0) {
        // 接管: 跳过原 AI。原 AI 的存活刷新(timeLeft=2)不会执行, 补上
        ObjSetInt(g_proj_timeLeft_field, instance, 2);
        g_skip_ai = true;
        return true;
    }
    return false;
}

static void AI_Postfix(patch_handle_t instance, void **args,
                       void *result, const patch_method_signature_t *sig_info) {
    (void)args; (void)result; (void)sig_info;
    if (!g_skip_ai || !instance) return;
    g_skip_ai = false;

    const int whoAmI = ObjInt(g_proj_whoAmI_field, instance);
    if (whoAmI < 0 || whoAmI >= 1000) return;
    const int target = g_chase_target[whoAmI];
    if (target < 0) return;
    if (!ChaseStep(instance, target)) g_chase_target[whoAmI] = -1;
}

// ============ Hook: Projectile.Damage (Prefix/Postfix) ============
// 原版 Damage_CanDealDamage() (Projectile.cs:11810) 对 Main.projPet
// 弹幕一律禁伤(755 在 ai[0]!=0 冲刺状态除外)。
// Damage 执行期间把 type 伪装成 755, ai[0] 临时置 1, 绕过禁伤检查。
static bool Damage_Prefix(patch_handle_t instance, void **args,
                          const patch_method_signature_t *sig_info, void *result) {
    (void)args; (void)sig_info; (void)result;
    if (!g_ready || g_dmg_spoofing || !instance) return false;

    const int type = ObjInt(g_proj_type_field, instance);
    if (!IsPetProj(type)) return false;
    if (!ObjInt(g_proj_active_field, instance)) return false;

    const int myPlayer = LocalPlayer();
    if (myPlayer < 0 || ObjInt(g_proj_owner_field, instance) != myPlayer) return false;

    g_origType = type;
    ObjSetInt(g_proj_type_field, instance, PROJ_BAT_OF_LIGHT);

    // 755 的放行条件是 ai[0]!=0, 宠物待机时 ai[0] 多为 0, 临时置 1
    g_ai0_modified = false;
    float a0 = 0.0f;
    if (ProjAiGet(instance, 0, &a0) && a0 == 0.0f) {
        ProjAiSet(instance, 0, 1.0f);
        g_ai0_modified = true;
    }
    g_dmg_spoofing = true;
    return false;
}

static void Damage_Postfix(patch_handle_t instance, void **args,
                           void *result, const patch_method_signature_t *sig_info) {
    (void)args; (void)result; (void)sig_info;
    if (!g_dmg_spoofing || !instance) return;
    g_dmg_spoofing = false;
    ObjSetInt(g_proj_type_field, instance, g_origType);
    if (g_ai0_modified) {
        ProjAiSet(instance, 0, g_origAi0);
        g_ai0_modified = false;
    }
}

// ============ Hook: Player.AddBuff_RemoveOldPetBuffsOfMatchingType (Prefix) ====
// 原版宠物互斥的唯一入口 (Player.cs:5154): 新宠物 Buff 加入前删除所有
// 已存在的 lightPet/vanityPet Buff。跳过它即可让多只宠物共存:
//   - 宠物的生成/存续按 Buff 独立驱动 (BuffHandle_SpawnPetIfNeededAndSetTime,
//     Player.cs:10634 起, 每种宠物有自己的玩家标志位与弹幕);
//   - 同种宠物重复召唤走 AddBuff_TryUpdatingExistingBuffTime 刷新时长,
//     不会走到这里, 不会产生重复弹幕;
//   - 宠物栏(装备)仍是单槽, 属原版行为。
// 注意 prefix 返回值语义与头文件注释相反: true = 跳过原方法。
static bool RemoveOldPetBuffs_Prefix(patch_handle_t instance, void **args,
                                     const patch_method_signature_t *sig_info, void *result) {
    (void)instance; (void)args; (void)sig_info; (void)result;
    return g_ready;
}

// ============ Hook: Item.SetDefaults (Postfix) ============
// 宠物召唤物品: 标记为召唤伤害物品(summon = true, 与原版召唤武器一致),
// 并把 item.damage 设为当前时期伤害(物品提示可见)。
static void SetDefaults_Postfix(patch_handle_t instance, void **args,
                                void *result, const patch_method_signature_t *sig_info) {
    (void)args; (void)result; (void)sig_info;
    if (!g_ready || !instance) return;

    const int shoot = ObjInt(g_item_shoot_field, instance);
    if (!IsPetProj(shoot)) return;

    ObjSetBool(g_item_summon_field, instance, true);
    ObjSetInt(g_item_damage_field, instance, PetDamageNow(shoot));
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write)
        mod_logger_write(MOD_LOG_LEVEL_INFO, "CombatPets", "初始化战斗宠物模组");

    // 0. 宠物弹幕查询表(热路径免反射)
    for (size_t i = 0; i < PET_PROJ_COUNT; ++i) {
        int t = g_pet_projs[i];
        if (t >= 0 && t < PROJ_TYPE_MAX) g_pet_lookup[t] = true;
    }
    g_stage_cache = 0;
    g_stage_tick = 0;

    // 1. 类型
    g_main_type = patchlib_type_get_type("Terraria", "Main");
    g_npc_type = patchlib_type_get_type("Terraria", "NPC");
    g_player_type = patchlib_type_get_type("Terraria", "Player");
    g_item_type = patchlib_type_get_type("Terraria", "Item");
    g_projectile_type = patchlib_type_get_type("Terraria", "Projectile");
    g_entity_type = patchlib_type_get_type("Terraria", "Entity");
    if (!g_main_type || !g_npc_type || !g_player_type || !g_item_type ||
        !g_projectile_type || !g_entity_type) {
        return;
    }

    // 2. 本地玩家编号
#if defined(__ANDROID__)
    patch_handle_t myPlayer_prop = patchlib_type_get_property(g_main_type, "myPlayer");
    if (myPlayer_prop) {
        patch_handle_t getter = patchlib_property_get_get_method(myPlayer_prop);
        if (getter) g_get_myPlayer = (int (*)(void))patchlib_method_get_pointer(getter);
    }
    if (!g_get_myPlayer) {
        return;
    }
#else
    g_myPlayer_field = patchlib_type_get_field(g_main_type, "myPlayer");
    if (!g_myPlayer_field) {
        return;
    }
#endif

    // 3. 静态字段
    g_projPet_field = patchlib_type_get_field(g_main_type, "projPet");
    g_npcArray_field = patchlib_type_get_field(g_main_type, "npc");
    g_hardMode_field = patchlib_type_get_field(g_main_type, "hardMode");
    g_downed1 = patchlib_type_get_field(g_npc_type, "downedBoss1");
    g_downed2 = patchlib_type_get_field(g_npc_type, "downedBoss2");
    g_downed3 = patchlib_type_get_field(g_npc_type, "downedBoss3");
    g_downedSlimeKing = patchlib_type_get_field(g_npc_type, "downedSlimeKing");
    g_downedQueenBee = patchlib_type_get_field(g_npc_type, "downedQueenBee");
    g_downedMechAny = patchlib_type_get_field(g_npc_type, "downedMechBossAny");
    g_downedPlant = patchlib_type_get_field(g_npc_type, "downedPlantBoss");
    g_downedGolem = patchlib_type_get_field(g_npc_type, "downedGolemBoss");
    g_downedCultist = patchlib_type_get_field(g_npc_type, "downedAncientCultist");
    g_downedMoonlord = patchlib_type_get_field(g_npc_type, "downedMoonlord");
    if (!g_projPet_field || !g_npcArray_field || !g_hardMode_field ||
        !g_downed1 || !g_downed2 || !g_downed3 || !g_downedSlimeKing ||
        !g_downedQueenBee || !g_downedMechAny || !g_downedPlant ||
        !g_downedGolem || !g_downedCultist || !g_downedMoonlord) {
        return;
    }

    // 4. 实例字段
    // 注意: active 不是 Entity 的字段, Projectile/NPC 各自单独声明
    g_proj_active_field = patchlib_type_get_field(g_projectile_type, "active");
    g_npc_active_field = patchlib_type_get_field(g_npc_type, "active");
    g_proj_type_field = patchlib_type_get_field(g_projectile_type, "type");
    g_proj_owner_field = patchlib_type_get_field(g_projectile_type, "owner");
    g_proj_damage_field = patchlib_type_get_field(g_projectile_type, "damage");
    g_proj_friendly_field = patchlib_type_get_field(g_projectile_type, "friendly");
    g_proj_knockback_field = patchlib_type_get_field(g_projectile_type, "knockBack");
    g_proj_ai_field = patchlib_type_get_field(g_projectile_type, "ai");
    g_proj_vel_field = patchlib_type_get_field(g_entity_type, "velocity");
    g_proj_timeLeft_field = patchlib_type_get_field(g_projectile_type, "timeLeft");
    g_proj_spriteDir_field = patchlib_type_get_field(g_projectile_type, "spriteDirection");
    g_proj_whoAmI_field = patchlib_type_get_field(g_entity_type, "whoAmI");
    g_proj_pos_field = patchlib_type_get_field(g_entity_type, "position");
    g_proj_width_field = patchlib_type_get_field(g_entity_type, "width");
    g_proj_height_field = patchlib_type_get_field(g_entity_type, "height");
    g_npc_friendly_field = patchlib_type_get_field(g_npc_type, "friendly");
    g_npc_townNPC_field = patchlib_type_get_field(g_npc_type, "townNPC");
    g_npc_dontTake_field = patchlib_type_get_field(g_npc_type, "dontTakeDamage");
    g_npc_life_field = patchlib_type_get_field(g_npc_type, "life");
    g_npc_lifeMax_field = patchlib_type_get_field(g_npc_type, "lifeMax");
    g_npc_chaseable_field = patchlib_type_get_field(g_npc_type, "chaseable");
    g_npc_immortal_field = patchlib_type_get_field(g_npc_type, "immortal");
    g_item_shoot_field = patchlib_type_get_field(g_item_type, "shoot");
    g_item_damage_field = patchlib_type_get_field(g_item_type, "damage");
    g_item_summon_field = patchlib_type_get_field(g_item_type, "summon");
    if (!g_proj_active_field || !g_proj_type_field || !g_proj_owner_field ||
        !g_proj_damage_field || !g_proj_friendly_field || !g_proj_knockback_field || !g_proj_ai_field ||
        !g_proj_vel_field || !g_proj_timeLeft_field || !g_proj_spriteDir_field ||
        !g_proj_whoAmI_field ||
        !g_proj_pos_field || !g_proj_width_field || !g_proj_height_field ||
        !g_npc_active_field || !g_npc_friendly_field || !g_npc_townNPC_field ||
        !g_npc_dontTake_field || !g_npc_life_field || !g_npc_lifeMax_field ||
        !g_npc_chaseable_field || !g_npc_immortal_field || !g_item_shoot_field ||
        !g_item_damage_field || !g_item_summon_field) {
        return;
    }

    // 5. Hook Projectile.AI (Postfix)、Projectile.Damage (Prefix+Postfix)
    //    与 Item.SetDefaults (Postfix)
    patch_handle_t ai_method = patchlib_type_get_method(g_projectile_type, "AI");
    if (!ai_method)
        ai_method = patchlib_type_get_method_by_param_count(g_projectile_type, "AI", 0);
    if (!ai_method) {
        return;
    }
    g_hook_ai = patchlib_install_prepost_hook(ai_method, AI_Prefix, AI_Postfix);
    if (g_hook_ai == PATCH_HOOK_INVALID_ID) {
        return;
    }

    // 宠物禁伤检查在 Damage_CanDealDamage 里按 type 判定,
    // Damage 执行期间保持 755 伪装
    patch_handle_t dmg_method = patchlib_type_get_method(g_projectile_type, "Damage");
    if (!dmg_method)
        dmg_method = patchlib_type_get_method_by_param_count(g_projectile_type, "Damage", 0);
    if (dmg_method) {
        g_hook_dmg = patchlib_install_prepost_hook(dmg_method, Damage_Prefix, Damage_Postfix);
    }

    {
        patch_handle_t collision_type = patchlib_type_get_type("Terraria", "Collision");
        if (collision_type) {
            g_solidTiles_method = patchlib_type_get_method_by_param_count(
                collision_type, "SolidTiles", 4);
        }
        if (!g_solidTiles_method && mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "CombatPets",
                             "获取 Collision.SolidTiles 失败(穿墙保护不可用)");
        }
    }

    // 6. 多宠物共存: 跳过宠物 Buff 互斥清理 (Player.cs:5154)
    patch_handle_t removePet_method = patchlib_type_get_method(
        g_player_type, "AddBuff_RemoveOldPetBuffsOfMatchingType");
    if (!removePet_method)
        removePet_method = patchlib_type_get_method_by_param_count(
            g_player_type, "AddBuff_RemoveOldPetBuffsOfMatchingType", 1);
    if (removePet_method) {
        g_hook_addbuff = patchlib_install_prepost_hook(
            removePet_method, RemoveOldPetBuffs_Prefix, NULL);
    }

    // SetDefaults(int, ItemVariant = null): 可选参数计入参数个数,
    // 实际为 2 参; 兼容两种取法
    patch_handle_t setdef_method =
        patchlib_type_get_method_by_param_count(g_item_type, "SetDefaults", 2);
    if (!setdef_method)
        setdef_method = patchlib_type_get_method_by_param_count(g_item_type, "SetDefaults", 1);
    if (setdef_method) {
        g_hook_item = patchlib_install_prepost_hook(setdef_method, NULL, SetDefaults_Postfix);
    }

    g_ready = true;
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "CombatPets",
                         "初始化完成: AI=%d Damage=%d Item=%d 多宠物=%d",
                         (int)g_hook_ai, (int)g_hook_dmg, (int)g_hook_item,
                         (int)g_hook_addbuff);
    }
}

// ============ 模块清理 ============
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;

    if (g_hook_ai != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hook_ai);
        g_hook_ai = PATCH_HOOK_INVALID_ID;
    }
    if (g_hook_item != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hook_item);
        g_hook_item = PATCH_HOOK_INVALID_ID;
    }
    if (g_hook_dmg != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hook_dmg);
        g_hook_dmg = PATCH_HOOK_INVALID_ID;
    }
    if (g_hook_addbuff != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hook_addbuff);
        g_hook_addbuff = PATCH_HOOK_INVALID_ID;
    }

    g_ready = false;
    g_skip_ai = false;
    for (int i = 0; i < 1000; ++i) g_chase_target[i] = -1;
    g_dmg_spoofing = false;
    g_ai0_modified = false;
    g_main_type = NULL;
    g_npc_type = NULL;
    g_player_type = NULL;
    g_item_type = NULL;
    g_projectile_type = NULL;
    g_entity_type = NULL;
    g_projPet_field = NULL;
    g_npcArray_field = NULL;
    g_hardMode_field = NULL;
    g_proj_active_field = NULL;
    g_proj_type_field = NULL;
    g_proj_owner_field = NULL;
    g_proj_damage_field = NULL;
    g_proj_friendly_field = NULL;
    g_proj_knockback_field = NULL;
    g_proj_ai_field = NULL;
    g_proj_vel_field = NULL;
    g_proj_timeLeft_field = NULL;
    g_proj_spriteDir_field = NULL;
    g_proj_whoAmI_field = NULL;
    g_proj_pos_field = NULL;
    g_proj_width_field = NULL;
    g_proj_height_field = NULL;
    g_npc_active_field = NULL;
    g_npc_friendly_field = NULL;
    g_npc_townNPC_field = NULL;
    g_npc_dontTake_field = NULL;
    g_npc_life_field = NULL;
    g_npc_lifeMax_field = NULL;
    g_npc_chaseable_field = NULL;
    g_npc_immortal_field = NULL;
    g_item_shoot_field = NULL;
    g_item_damage_field = NULL;
    g_item_summon_field = NULL;
    g_downed1 = NULL;
    g_downed2 = NULL;
    g_downed3 = NULL;
    g_downedSlimeKing = NULL;
    g_downedQueenBee = NULL;
    g_downedMechAny = NULL;
    g_downedPlant = NULL;
    g_downedGolem = NULL;
    g_downedCultist = NULL;
    g_downedMoonlord = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "CombatPets", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.projectile.combatpets",
        .version_code = 14,
        .api_version = 1,
        .version = "4.2.0",
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
    return &g_ops;
}
