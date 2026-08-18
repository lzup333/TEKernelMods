//
// ChatCommands - 聊天指令助手
// NewEFMod (tefkernel / KernelLoader) 重写版，适配 Terraria 1.4.5.x (手机端 / PE)
//
// 功能(在游戏聊天框输入指令):
//   1. /get <物品ID> [数量]     生成对应物品并给予玩家(默认 1 个, 数量上限 9999, /give 同义)
//   2. /god [on|off]            开关无敌模式(置 creativeGodMode, 免疫一切伤害)
//   3. /time <时:分>            设置游戏内时间(如 /time 12:30, 24 小时制)
//   4. /heal                    恢复全部生命与法力
//   5. /buff <buffID> [秒数]    给自己添加 buff(默认 300 秒)
//
// 注: /sp(生成生物) 已暂时移除——桌面端内核无法读取玩家 Vector2 位置,
//     无法在玩家附近生成生物, 待内核支持后恢复。
//
// 与 ClassicEFMod 版的差异(NewAPI):
//   - 入口从 CreateMod() 变为 create_kernel_mod(), 返回 kernel_mod_ops_t 操作表;
//   - Hook 从 registerFunctionDescriptor(替换转发函数) 变为
//     patchlib_install_prepost_hook(libffi 闭包);
//   - 聊天拦截: ChatCommandProcessor.ProcessIncomingMessage / ChatHelper.SendChatMessageFromClient
//     使用 postfix(原版执行后处理, 与原版后置行为一致), 消息文本通过
//     ChatMessage.Text 的 getter 读取, 失败时回退到对象内偏移 0x18;
//   - 生成 C# 字符串用 patchlib_string_create, 读取用 patchlib_string_cstr;
//   - 方法调用用 patchlib_method_get_pointer 取原生函数指针直接调用
//     (静态方法直接调用, 实例方法第一个参数即 this; byte 参数用 uint8_t);
//   - 字段访问用 patchlib_field_get_pointer 直接取真实指针(Android);
//   - 日志改用 mod_logger_write。
//

#include "mod-api/mod_core.h"
#include "mod-api/mod_logger.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/property.h"
#include "tefkernel/patchlib/struct/array.h"
#include "tefkernel/patchlib/struct/string.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

// ============ 状态 ============
static patch_hook_id_t g_hookProcessIncoming = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_hookSendChatFromClient = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_hookResetEffects = PATCH_HOOK_INVALID_ID;
static bool g_godMode = false;  // 无敌模式开关

// ============ 类型句柄 ============
static patch_handle_t g_main_type = NULL;
static patch_handle_t g_player_type = NULL;
static patch_handle_t g_item_type = NULL;
static patch_handle_t g_npc_type = NULL;

// ============ 字段句柄 ============
static patch_handle_t g_statLife_field = NULL;     // Player.statLife       (int)
static patch_handle_t g_statLifeMax_field = NULL;  // Player.statLifeMax    (int)
static patch_handle_t g_statMana_field = NULL;     // Player.statMana       (int)
static patch_handle_t g_statManaMax_field = NULL;  // Player.statManaMax    (int)
static patch_handle_t g_creativeGod_field = NULL;  // Player.creativeGodMode(bool, 旅途模式无敌)
static patch_handle_t g_mainPlayer_field = NULL;   // Main.player           (Player[], 静态)
static patch_handle_t g_dayTime_field = NULL;      // Main.dayTime          (bool, 静态)
static patch_handle_t g_time_field = NULL;         // Main.time             (double, 静态)

// ============ 方法函数指针(Android 平台) ============
#if defined(__ANDROID__)
static int  (*g_get_myPlayer)(void);                       // Main.get_myPlayer    (static int)
static int  (*g_newItem)(void*, int, int, int, int, int, int, bool, int, bool);  // Item.NewItem (static int)
static void (*g_newText)(void*, uint8_t, uint8_t, uint8_t);                        // Main.NewText (static void)
static void (*g_addBuff)(void*, int, int, bool);            // Player.AddBuff (void)
static void* (*g_getMsgText)(void*);                        // ChatMessage.get_Text
#else
// 桌面端句柄: 字段/方法用 value API / method_invoke_args 访问
static patch_handle_t g_myPlayer_field = NULL;    // Main.myPlayer        (static int 字段)
static patch_handle_t g_newItem_method = NULL;    // Item.NewItem         (static, 9 参)
static patch_handle_t g_newText_method = NULL;    // Main.NewText         (static, 4 参)
static patch_handle_t g_addBuff_method = NULL;    // Player.AddBuff       (实例, 3 参)
static patch_handle_t g_getMsgText_method = NULL; // ChatMessage.get_Text (getter 方法句柄)
static patch_handle_t g_quickSpawn_method = NULL; // Player.QuickSpawnItem (实例, 3 参, 桌面端给物品)
#endif

// ============ 对象内偏移 (PE 1.4.5.6.4 dump.cs) ============
// Entity:        position(Vector2=2float) 0x14 | velocity 0x1C | width 0x3C | height 0x40
static const size_t kPosOffset   = 0x14;
static const size_t kWidthOffset = 0x3C;
// ChatMessage:   <Text>k__BackingField 0x18 (string)
static const size_t kMsgTextOffset = 0x18;

// ============ 工具函数 ============

/** 获取本地玩家实例; 失败返回 NULL */
static void* LocalPlayer(void) {
#if defined(__ANDROID__)
    if (!g_get_myPlayer || !g_mainPlayer_field) return NULL;
    const int my = g_get_myPlayer();
    if (my < 0) return NULL;
    void** slot = (void**)patchlib_field_get_pointer(g_mainPlayer_field, NULL);
    if (!slot || !*slot) return NULL;
    void* arr = *slot;
    if ((size_t)my >= patchlib_array_length(arr)) return NULL;
    void* p = NULL;
    if (!patchlib_array_at(arr, (size_t)my, &p)) return NULL;
    return p;
#else
    if (!g_myPlayer_field || !g_mainPlayer_field) return NULL;
    int my = -1;
    patchlib_field_get_value(g_myPlayer_field, NULL, &my);
    if (my < 0) return NULL;
    void* arr = NULL;
    patchlib_field_get_value(g_mainPlayer_field, NULL, &arr);
    if (!arr) return NULL;
    if ((size_t)my >= patchlib_array_length(arr)) return NULL;
    void* p = NULL;
    if (!patchlib_array_at(arr, (size_t)my, &p)) return NULL;
    return p;
#endif
}

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

/** 玩家几何信息(位置 + 尺寸, 仅 Android 使用) */
typedef struct PlayerGeo { int x, y, w, h; } PlayerGeo;

/** 读取玩家位置与尺寸 (按对象内偏移) */
static bool PlayerGeometry(void* p, PlayerGeo* g) {
    if (!p || !g) return false;
#if defined(__ANDROID__)
    const char* base = (const char*)p;
    const float* pos = (const float*)(base + kPosOffset);
    const int* wh = (const int*)(base + kWidthOffset);
    g->x = (int)pos[0];
    g->y = (int)pos[1];
    g->w = wh[0];
    g->h = wh[1];
    if (g->w < 0) g->w = 0;
    if (g->h < 0) g->h = 0;
    return true;
#else
    (void)p; (void)g;
    return false;
#endif
}

/** 读取 ChatMessage.Text (string); 失败返回 NULL */
static void* GetMessageText(void* message) {
    if (!message) return NULL;
#if defined(__ANDROID__)
    if (g_getMsgText) return g_getMsgText(message);
    return *(void**)((char*)message + kMsgTextOffset);
#else
    if (!g_getMsgText_method) return NULL;
    void* str = NULL;
    patchlib_method_invoke_args(g_getMsgText_method, message, &str, NULL);
    return str;
#endif
}

/** 在聊天框显示一行文字(尽力而为, 失败仅降级为无反馈) */
static void ShowChat(const char* text) {
    if (!text) return;
    void* s = patchlib_string_create(text);
    if (!s) return;
#if defined(__ANDROID__)
    if (!g_newText) return;
    g_newText(s, 255, 255, 255);
#else
    if (!g_newText_method) return;
    uint8_t r = 255, g = 255, b = 255;
    void* args[4];
    args[0] = &s;
    args[1] = &r;
    args[2] = &g;
    args[3] = &b;
    patchlib_method_invoke_args(g_newText_method, NULL, NULL, args);
#endif
}

/** 去掉行首空白 */
static const char* TrimLeft(const char* s) {
    if (!s) return "";
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    return s;
}

/** 判断聊天文本是否为本 Mod 的指令 */
static bool IsModCommand(const char* t) {
    if (!t) return false;
    return strncmp(t, "/get", 4) == 0 || strncmp(t, "/give", 5) == 0 ||
           strncmp(t, "/god", 4) == 0 ||
           strncmp(t, "/time", 5) == 0 || strncmp(t, "/heal", 5) == 0 ||
           strncmp(t, "/buff", 5) == 0;
}

/**
 * 处理一条本地玩家的聊天指令
 * @param raw 玩家输入的原始文本(如 "/get 5 99")
 */
static void HandleCommand(const char* raw) {
    if (!raw) return;

    if (strncmp(raw, "/get", 4) == 0 || strncmp(raw, "/give", 5) == 0) {
        const bool isGive = strncmp(raw, "/give", 5) == 0;
        const char* rest = TrimLeft(raw + (isGive ? 5 : 4));
        if (*rest == '\0') {
            ShowChat("[指令助手] 用法: /get 或 /give <物品ID> [数量]");
            return;
        }
        char* end = NULL;
        const long id = strtol(rest, &end, 10);
        if (end == rest || id <= 0 || id >= 100000) {
            ShowChat("[指令助手] 无效的物品ID");
            return;
        }
        int stack = 1;
        if (end && *end != '\0') {
            long st = strtol(end, NULL, 10);
            if (st < 1) st = 1;
            if (st > 9999) st = 9999;
            stack = (int)st;
        }
        void* p = LocalPlayer();
        if (!p) return;
#if defined(__ANDROID__)
        if (!g_newItem) {
            ShowChat("[指令助手] Item.NewItem 解析失败");
            return;
        }
        PlayerGeo geo;
        if (!PlayerGeometry(p, &geo)) return;
        // NewItem(source=null, X, Y, Width, Height, Type, Stack, noBroadcast, pfix, noGrabDelay)
        g_newItem(NULL, geo.x, geo.y, geo.w, geo.h,
                  (int)id, stack, false, 0, false);
#else
        // 桌面端: 用实例方法 Player.QuickSpawnItem(source, item, stack) 给物品
        // (实例调用已验证可用, 且 QuickSpawnItem 链内部不使用 source, 传 NULL 安全)
        if (!g_quickSpawn_method) {
            ShowChat("[指令助手] Player.QuickSpawnItem 解析失败");
            return;
        }
        void* src = NULL;
        int itemT = (int)id;
        int stackN = stack;
        void* args[3];
        args[0] = &src;
        args[1] = &itemT;
        args[2] = &stackN;
        patchlib_method_invoke_args(g_quickSpawn_method, p, NULL, args);
#endif
        char buf[96];
        snprintf(buf, sizeof(buf), "[指令助手] 已给予物品 %ld x %d", id, stack);
        ShowChat(buf);
        return;
    }

    if (strncmp(raw, "/god", 4) == 0) {
        const char* rest = TrimLeft(raw + 4);
        if (strcmp(rest, "on") == 0 || strcmp(rest, "1") == 0 || strcmp(rest, "true") == 0) {
            g_godMode = true;
        } else if (strcmp(rest, "off") == 0 || strcmp(rest, "0") == 0 || strcmp(rest, "false") == 0) {
            g_godMode = false;
        } else {
            g_godMode = !g_godMode;   // 无参数则切换
        }
        ShowChat(g_godMode ? "[指令助手] 无敌模式已开启" : "[指令助手] 无敌模式已关闭");
        return;
    }

    if (strncmp(raw, "/time", 5) == 0) {
        const char* rest = TrimLeft(raw + 5);
        if (*rest == '\0') {
            ShowChat("[指令助手] 用法: /time <时:分>, 例如 /time 12:30");
            return;
        }
        // 解析 HH:MM(时 0-23, 分 0-59)
        char* end = NULL;
        const long hour = strtol(rest, &end, 10);
        if (end == rest || *end != ':') {
            ShowChat("[指令助手] 无效的时间, 用法: /time 12:30");
            return;
        }
        const long minute = strtol(end + 1, NULL, 10);
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
            ShowChat("[指令助手] 无效的时间(时 0-23, 分 0-59)");
            return;
        }
        // 泰拉瑞亚时间: dayLength=54000 单位 = 白天/夜晚各 12 小时
        //  -> 1 分钟 = 75 单位, 1 小时 = 4500 单位
        // 白天从 4:30(270 分钟)起, 夜晚从 16:30(990 分钟)起
        const int minOfDay = (int)(hour * 60 + minute);
        bool isDay;
        double t;
        if (minOfDay >= 270 && minOfDay < 990) {        // 4:30 ~ 16:29 = 白天
            isDay = true;
            t = (double)(minOfDay - 270) * 75.0;
        } else {                                         // 其余 = 夜晚
            isDay = false;
            const int nightMin = (minOfDay >= 990) ? (minOfDay - 990) : (minOfDay + 1440 - 990);
            t = (double)nightMin * 75.0;
        }
        if (!g_time_field || !g_dayTime_field) {
            ShowChat("[指令助手] 时间字段解析失败");
            return;
        }
#if defined(__ANDROID__)
        bool* pDay = (bool*)patchlib_field_get_pointer(g_dayTime_field, NULL);
        double* pTime = (double*)patchlib_field_get_pointer(g_time_field, NULL);
        if (pDay && pTime) {
            *pDay = isDay;
            *pTime = t;
        }
#else
        patchlib_field_set_value(g_dayTime_field, NULL, &isDay);
        patchlib_field_set_value(g_time_field, NULL, &t);
#endif

        char buf[96];
        snprintf(buf, sizeof(buf), "[指令助手] 时间已设置为 %02ld:%02ld", hour, minute);
        ShowChat(buf);
        return;
    }

    if (strncmp(raw, "/heal", 5) == 0) {
        void* p = LocalPlayer();
        if (!p) return;
        if (!g_statLife_field || !g_statLifeMax_field || !g_statMana_field || !g_statManaMax_field) {
            ShowChat("[指令助手] 生命/法力字段解析失败");
            return;
        }
#if defined(__ANDROID__)
        int* pLife = (int*)patchlib_field_get_pointer(g_statLife_field, p);
        int* pLifeMax = (int*)patchlib_field_get_pointer(g_statLifeMax_field, p);
        int* pMana = (int*)patchlib_field_get_pointer(g_statMana_field, p);
        int* pManaMax = (int*)patchlib_field_get_pointer(g_statManaMax_field, p);
        if (pLife && pLifeMax && pMana && pManaMax) {
            *pLife = *pLifeMax;
            *pMana = *pManaMax;
        }
#else
        int max = 0;
        patchlib_field_get_value(g_statLifeMax_field, p, &max);
        patchlib_field_set_value(g_statLife_field, p, &max);
        patchlib_field_get_value(g_statManaMax_field, p, &max);
        patchlib_field_set_value(g_statMana_field, p, &max);
#endif
        ShowChat("[指令助手] 已恢复全部生命与法力");
        return;
    }

    if (strncmp(raw, "/buff", 5) == 0) {
        // /buff <buffID> [秒数]  默认 300 秒
        const char* rest = TrimLeft(raw + 5);
        if (*rest == '\0') {
            ShowChat("[指令助手] 用法: /buff <buffID> [秒数]");
            return;
        }
        char* end = NULL;
        const long id = strtol(rest, &end, 10);
        if (end == rest || id < 0 || id >= 100000) {
            ShowChat("[指令助手] 无效的buffID");
            return;
        }
        long seconds = 300;
        if (end && *end != '\0') {
            const long s = strtol(end, NULL, 10);
            if (s >= 1 && s <= 36000) seconds = s;
        }
        void* p = LocalPlayer();
        if (!p) return;
#if defined(__ANDROID__)
        if (!g_addBuff) {
            ShowChat("[指令助手] Player.AddBuff 解析失败");
            return;
        }
        // AddBuff(type, time, fromNetPvP=false), 时间单位为帧(60帧=1秒)
        g_addBuff(p, (int)id, (int)(seconds * 60), false);
#else
        if (!g_addBuff_method) {
            ShowChat("[指令助手] Player.AddBuff 解析失败");
            return;
        }
        int type = (int)id, timeF = (int)(seconds * 60);
        bool fromNetPvP = false;
        void* args[3];
        args[0] = &type;
        args[1] = &timeF;
        args[2] = &fromNetPvP;
        patchlib_method_invoke_args(g_addBuff_method, p, NULL, args);
#endif
        char buf[96];
        snprintf(buf, sizeof(buf), "[指令助手] 已添加buff %ld (%ld 秒)", id, seconds);
        ShowChat(buf);
        return;
    }
}

// ============ Hook: 单机聊天消息处理 ============
static void ProcessIncomingMessage_Postfix(patch_handle_t instance, void **args, void *result,
                                           const patch_method_signature_t *sig_info) {
    (void)instance; (void)result; (void)sig_info;
    if (!args) return;
    void* message = *(void**)args[0];   // ChatMessage
    const int clientId = *(int*)args[1];
    const int myPlayer = LocalPlayerId();
    if (myPlayer < 0 || clientId != myPlayer) return;   // 只处理本地玩家

    void* str = GetMessageText(message);
    if (!str) return;
    char* text = patchlib_string_cstr(str);
    if (text) {
        if (IsModCommand(text)) {
            HandleCommand(text);
        }
        free(text);
    }
}

// ============ Hook: 多人客户端聊天消息处理 ============
static void SendChatMessageFromClient_Postfix(patch_handle_t instance, void **args, void *result,
                                              const patch_method_signature_t *sig_info) {
    (void)instance; (void)result; (void)sig_info;
    if (!args) return;
    void* message = *(void**)args[0];   // ChatMessage

    void* str = GetMessageText(message);
    if (!str) return;
    char* text = patchlib_string_cstr(str);
    if (text) {
        if (IsModCommand(text)) {
            HandleCommand(text);
        }
        free(text);
    }
}

/**
 * Hook: 每帧维持 creativeGodMode(旅途模式无敌)
 * ResetEffects 每帧会把 creativeGodMode 重置为 false (Player.cs:18607),
 * 因此在这里(原版之后)重新置为 true。置位后:
 *   - Player.Hurt 直接返回 0.0 (Player.cs:37595) —— 免疫一切伤害
 *   - Player.KillMe 直接返回 (Player.cs:38199) —— 永不死亡
 *   - Update_NPCCollision 跳过 (Player.cs:30863) —— 免疫 NPC 接触伤害
 *   与旅途模式 GodmodePower 效果完全一致, 且无需 Hook 带返回值的 Hurt。
 */
static void ResetEffects_Postfix(patch_handle_t instance, void **args, void *result,
                                 const patch_method_signature_t *sig_info) {
    (void)args; (void)result; (void)sig_info;
    if (!g_godMode || !instance) return;
    if (!g_creativeGod_field) return;
#if defined(__ANDROID__)
    bool* p = (bool*)patchlib_field_get_pointer(g_creativeGod_field, instance);
    if (p) *p = true;
#else
    bool v = true;
    patchlib_field_set_value(g_creativeGod_field, instance, &v);
#endif
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChatCommands", "初始化聊天指令模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChatCommands", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 1. 获取类型
    g_main_type = patchlib_type_get_type("Terraria", "Main");
    g_player_type = patchlib_type_get_type("Terraria", "Player");
    g_item_type = patchlib_type_get_type("Terraria", "Item");
    g_npc_type = patchlib_type_get_type("Terraria", "NPC");
    if (!g_main_type || !g_player_type || !g_item_type || !g_npc_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "ChatCommands",
                             "获取类型失败 (Main/Player/Item/NPC)");
        }
        return;
    }

    // 2. 属性 getter / 方法解析(平台相关)
#if defined(__ANDROID__)
    patch_handle_t myPlayer_prop = patchlib_type_get_property(g_main_type, "myPlayer");
    if (myPlayer_prop) {
        patch_handle_t getter = patchlib_property_get_get_method(myPlayer_prop);
        if (getter) g_get_myPlayer = (int (*)(void))patchlib_method_get_pointer(getter);
    }

    // 3. 方法函数指针
    patch_handle_t newItem_method = patchlib_type_get_method_by_param_count(g_item_type, "NewItem", 10);
    if (!newItem_method) newItem_method = patchlib_type_get_method(g_item_type, "NewItem");
    if (newItem_method) {
        g_newItem = (int (*)(void*, int, int, int, int, int, int, bool, int, bool))
                patchlib_method_get_pointer(newItem_method);
    }

    patch_handle_t newText_method = patchlib_type_get_method_by_param_count(g_main_type, "NewText", 4);
    if (!newText_method) newText_method = patchlib_type_get_method(g_main_type, "NewText");
    if (newText_method) {
        g_newText = (void (*)(void*, uint8_t, uint8_t, uint8_t))
                patchlib_method_get_pointer(newText_method);
    }

    patch_handle_t addBuff_method = patchlib_type_get_method_by_param_count(g_player_type, "AddBuff", 3);
    if (!addBuff_method) addBuff_method = patchlib_type_get_method(g_player_type, "AddBuff");
    if (addBuff_method) {
        g_addBuff = (void (*)(void*, int, int, bool))
                patchlib_method_get_pointer(addBuff_method);
    }

    // 4. ChatMessage.Text getter(读取聊天文本, 失败时回退到偏移读取)
    patch_handle_t msg_type = patchlib_type_get_type("Terraria.Chat", "ChatMessage");
    if (msg_type) {
        patch_handle_t text_prop = patchlib_type_get_property(msg_type, "Text");
        if (text_prop) {
            patch_handle_t getter = patchlib_property_get_get_method(text_prop);
            if (getter) g_getMsgText = (void* (*)(void*))patchlib_method_get_pointer(getter);
        }
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChatCommands",
                         "myPlayer=%p", (void*)g_get_myPlayer);
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChatCommands",
                         "newItem=%p newText=%p addBuff=%p getMsgText=%p",
                         (void*)g_newItem, (void*)g_newText,
                         (void*)g_addBuff, (void*)g_getMsgText);
    }
#else
    // 桌面端: 字段/方法用 value API / method_invoke_args 访问
    g_myPlayer_field = patchlib_type_get_field(g_main_type, "myPlayer");

    // 桌面端 Item.NewItem 签名为 9 参 (IEntitySource, Vector2 pos, Width, Height, Type, Stack, bool, int, bool)
    g_newItem_method = patchlib_type_get_method_by_param_count(g_item_type, "NewItem", 9);
    if (!g_newItem_method) g_newItem_method = patchlib_type_get_method(g_item_type, "NewItem");

    g_newText_method = patchlib_type_get_method_by_param_count(g_main_type, "NewText", 4);
    if (!g_newText_method) g_newText_method = patchlib_type_get_method(g_main_type, "NewText");

    g_addBuff_method = patchlib_type_get_method_by_param_count(g_player_type, "AddBuff", 3);
    if (!g_addBuff_method) g_addBuff_method = patchlib_type_get_method(g_player_type, "AddBuff");

    // QuickSpawnItem 有 (IEntitySource,int,int) 与 (IEntitySource,Item,GetItemSettings) 两个 3 参重载,
    // 用参数名区分出 (source, item, stack) 版本
    const char* quickSpawnNames[] = { "source", "item", "stack" };
    g_quickSpawn_method = patchlib_type_get_method_by_param_names(g_player_type, "QuickSpawnItem", 3, quickSpawnNames);
    if (!g_quickSpawn_method) g_quickSpawn_method = patchlib_type_get_method(g_player_type, "QuickSpawnItem");

    patch_handle_t msg_type = patchlib_type_get_type("Terraria.Chat", "ChatMessage");
    if (msg_type) {
        patch_handle_t text_prop = patchlib_type_get_property(msg_type, "Text");
        if (text_prop) {
            g_getMsgText_method = patchlib_property_get_get_method(text_prop);
        }
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChatCommands",
                         "myPlayerF=%p",
                         (void*)g_myPlayer_field);
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChatCommands",
                         "newItem=%p newText=%p addBuff=%p getMsgText=%p qs=%p",
                         (void*)g_newItem_method, (void*)g_newText_method,
                         (void*)g_addBuff_method, (void*)g_getMsgText_method,
                         (void*)g_quickSpawn_method);
    }
#endif

    // 5. 字段
    g_statLife_field = patchlib_type_get_field(g_player_type, "statLife");
    g_statLifeMax_field = patchlib_type_get_field(g_player_type, "statLifeMax");
    g_statMana_field = patchlib_type_get_field(g_player_type, "statMana");
    g_statManaMax_field = patchlib_type_get_field(g_player_type, "statManaMax");
    g_creativeGod_field = patchlib_type_get_field(g_player_type, "creativeGodMode");
    g_mainPlayer_field = patchlib_type_get_field(g_main_type, "player");
    g_dayTime_field = patchlib_type_get_field(g_main_type, "dayTime");
    g_time_field = patchlib_type_get_field(g_main_type, "time");

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChatCommands",
                         "fields: life=%p lifeMax=%p mana=%p manaMax=%p god=%p player=%p day=%p time=%p",
                         (void*)g_statLife_field, (void*)g_statLifeMax_field,
                         (void*)g_statMana_field, (void*)g_statManaMax_field,
                         (void*)g_creativeGod_field, (void*)g_mainPlayer_field,
                         (void*)g_dayTime_field, (void*)g_time_field);
    }

    // 6. 获取 Hook 目标方法并安装 postfix hook
    patch_handle_t chat_method = patchlib_type_get_method_by_param_count(
            patchlib_type_get_type("Terraria.Chat", "ChatCommandProcessor"),
            "ProcessIncomingMessage", 2);
    if (chat_method) {
        g_hookProcessIncoming = patchlib_install_prepost_hook(
                chat_method, NULL, ProcessIncomingMessage_Postfix);
    }
    if (g_hookProcessIncoming == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "ChatCommands",
                             "ProcessIncomingMessage Hook 未安装(单机指令可能不可用)");
        }
    }

    patch_handle_t chat_helper_type = patchlib_type_get_type("Terraria.Chat", "ChatHelper");
    if (chat_helper_type) {
        patch_handle_t send_method = patchlib_type_get_method_by_param_count(
                chat_helper_type, "SendChatMessageFromClient", 1);
        if (send_method) {
            g_hookSendChatFromClient = patchlib_install_prepost_hook(
                    send_method, NULL, SendChatMessageFromClient_Postfix);
        }
    }
    if (g_hookSendChatFromClient == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "ChatCommands",
                             "SendChatMessageFromClient Hook 未安装(多人指令可能不可用)");
        }
    }

    patch_handle_t reset_method = patchlib_type_get_method_by_param_count(g_player_type, "ResetEffects", 0);
    if (!reset_method) {
        reset_method = patchlib_type_get_method(g_player_type, "ResetEffects");
    }
    if (reset_method) {
        g_hookResetEffects = patchlib_install_prepost_hook(reset_method, NULL, ResetEffects_Postfix);
    }
    if (g_hookResetEffects == PATCH_HOOK_INVALID_ID) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "ChatCommands",
                             "ResetEffects Hook 未安装(无敌模式不可用)");
        }
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChatCommands",
                         "Hooks: chat=%d send=%d reset=%d",
                         (int)g_hookProcessIncoming, (int)g_hookSendChatFromClient,
                         (int)g_hookResetEffects);
    }
}

// ============ 模块清理 ============
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;

    if (g_hookProcessIncoming != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookProcessIncoming);
        g_hookProcessIncoming = PATCH_HOOK_INVALID_ID;
    }
    if (g_hookSendChatFromClient != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookSendChatFromClient);
        g_hookSendChatFromClient = PATCH_HOOK_INVALID_ID;
    }
    if (g_hookResetEffects != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookResetEffects);
        g_hookResetEffects = PATCH_HOOK_INVALID_ID;
    }
    g_godMode = false;

#if defined(__ANDROID__)
    g_get_myPlayer = NULL;
    g_newItem = NULL;
    g_newText = NULL;
    g_addBuff = NULL;
    g_getMsgText = NULL;
#else
    g_myPlayer_field = NULL;
    g_newItem_method = NULL;
    g_newText_method = NULL;
    g_addBuff_method = NULL;
    g_getMsgText_method = NULL;
    g_quickSpawn_method = NULL;
#endif

    g_statLife_field = NULL;
    g_statLifeMax_field = NULL;
    g_statMana_field = NULL;
    g_statManaMax_field = NULL;
    g_creativeGod_field = NULL;
    g_mainPlayer_field = NULL;
    g_dayTime_field = NULL;
    g_time_field = NULL;

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChatCommands", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.player.chatcommands",
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
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ChatCommands", "聊天指令模组实例创建");
    }
    return &g_ops;
}
