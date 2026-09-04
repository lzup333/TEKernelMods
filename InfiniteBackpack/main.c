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
// InfiniteBackpack - 无限背包
// NewEFMod (tefkernel / KernelLoader) 版, 适配 Terraria 1.4.5.x (手机端 / PE)
//
// 功能(游戏聊天框输入指令):
//   /store  (别名 /存)    把背包里全部非快捷栏物品(第 10~49 格)存入无限背包
//   /take <槽位>[-<槽位>] [数量] (别名 /取)  按槽位取出物品
//     单槽: /take 3 [数量](缺省 1); 范围: /take 1-5 整格全部取出;
//     槽位 = /backpack 列表序号(从 1 开始);
//   /backpack (别名 /bp)  查看无限背包内容(每行 8 个分多行显示)
//
// 原版机制(pc_source):
//   - Player.inventory (Item[58]): 0~9=快捷栏, 10~49=主背包, 50~57=饰品/时装/染料;
//     金币栏/弹药栏是独立的 coins[]/ammo[] 数组, 不受影响;
//   - 存入: 读 Item.type/Item.stack 合并进存储表, 然后把该格 type=0/stack=0 清空
//     (Item.IsAir == type==0 && stack==0, 与原版空格一致);
//   - 取出: Player.QuickSpawnItem(source, int item, int stack) (Player.cs:6790),
//     该链路内部新建 Item 并走 GetItem 拾取逻辑, source 参数未参与(传 NULL 安全,
//     NewItem 用 GetItemSource_InventoryOverflow());
//   - 反馈消息用原版聊天标签显示物品图标: 数量>1 用 [i/s数量:ID],
//     否则用 [i:ID] (原版聊天标签格式);
//     /take 按槽位(列表序号)取, 不用输物品ID。
//
// 存档(持久化):
//   - 存档为私有目录下的文本文件, 每行 "物品ID 数量"(路径不对玩家展示);
//   - 私有目录在 mod 安装时即已创建(内核 modloader 保证);
//   - 模块初始化时加载, 每次存/取后立即写回, 关闭游戏后仍保留;
//   - 行解析容错: 跳过非法行, 单格数量上限 9999(超过自动开新格,
//     同背包格行为), 总格数上限 4096。
//
// 踩坑备忘(同 VeinMiner 注释):
//   - prefix 返回 true = 跳过原方法, 本 mod 全部使用 postfix;
//   - 桌面端 field_get_value 返回句柄不能直接解引用, 用 value API;
//   - Item[] 是引用类型元素数组, array_at 可用; Item.type/stack 是实例
//     int 字段, field_set_value(桌面)/field_get_pointer(Android) 均可用。
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

// ============ 可调参数 ============
static const int   INVENTORY_FIRST = 10;      // 非快捷栏起始格
static const int   INVENTORY_LAST  = 49;      // 主背包结束格
static const int   MAX_ENTRY_STACK = 9999;   // 单格上限(与游戏内单格物品上限一致)
static const int   MAX_STORAGE_ENTRIES = 4096;
static const int   CHAT_LIST_LIMIT = 8;       // 每条聊天消息列出的条目数(分多行显示)

// ============ 状态 ============
static patch_hook_id_t g_hookProcessIncoming = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_hookSendChatFromClient = PATCH_HOOK_INVALID_ID;

// 无限背包存储表(物品ID -> 总数, 同类合并)
typedef struct StoreEntry { int type; long stack; bool merged; } StoreEntry;
static StoreEntry* g_store = NULL;
static int g_store_count = 0;
static int g_store_cap = 0;
static char g_save_path[1024] = {0};
static bool g_dirty = false;

// ============ 类型句柄 ============
static patch_handle_t g_main_type = NULL;
static patch_handle_t g_player_type = NULL;
static patch_handle_t g_item_type = NULL;

// ============ 字段句柄 ============
static patch_handle_t g_mainPlayer_field = NULL;   // Main.player (Player[], 静态)
static patch_handle_t g_myPlayer_field = NULL;     // Main.myPlayer (static int, 桌面端)
static patch_handle_t g_inventory_field = NULL;    // Player.inventory (Item[])
static patch_handle_t g_itemType_field = NULL;     // Item.type (int)
static patch_handle_t g_itemStack_field = NULL;    // Item.stack (int)

// ============ 方法句柄(桌面端) ============
static patch_handle_t g_getMsgText_method = NULL;  // ChatMessage.get_Text
static patch_handle_t g_newText_method = NULL;     // Main.NewText (static, 4 参)
static patch_handle_t g_quickSpawn_method = NULL;  // Player.QuickSpawnItem(source,int,stack)

// ============ 方法函数指针(Android 平台) ============
#if defined(__ANDROID__)
static int  (*g_get_myPlayer)(void);               // Main.get_myPlayer
// 1.4.5.8: NewItem 只有 9 参旧版 (X,Y,W,H,Type,Stack,noBroadcast,pfix,ownership)
// 与 12 参 source 版, 10 参版本已不存在
static int  (*g_newItem)(int, int, int, int, int, int, bool, int, int); // Item.NewItem
// 1.4.5.8: NewText(string, byte R, byte G, byte B, bool onlyCurrentPlayer) = 5 参
static void (*g_newText)(void*, uint8_t, uint8_t, uint8_t, bool); // Main.NewText
static void* (*g_getMsgText)(void*);               // ChatMessage.get_Text
#endif

// ============ 对象内偏移 (PE 1.4.5.6.4 dump.cs) ============
// Entity:        position(Vector2=2float) 0x14 | width 0x3C | height 0x40
// ChatMessage:   <Text>k__BackingField 0x18 (string)
static const size_t kPosOffset   = 0x14;
static const size_t kWidthOffset = 0x3C;
static const size_t kMsgTextOffset = 0x18;

// ============ 存储表操作 ============
static StoreEntry* StoreFind(int type) {
    for (int i = 0; i < g_store_count; ++i)
        if (g_store[i].type == type) return &g_store[i];
    return NULL;
}

static StoreEntry* StoreAppend(int type, long stack) {
    if (g_store_count >= MAX_STORAGE_ENTRIES) return NULL;
    if (g_store_count >= g_store_cap) {
        int newcap = g_store_cap ? g_store_cap * 2 : 64;
        if (newcap > MAX_STORAGE_ENTRIES) newcap = MAX_STORAGE_ENTRIES;
        StoreEntry* p = (StoreEntry*)realloc(g_store, (size_t)newcap * sizeof(StoreEntry));
        if (!p) return NULL;
        g_store = p; g_store_cap = newcap;
    }
    g_store[g_store_count].type = type;
    g_store[g_store_count].stack = stack;
    g_store[g_store_count].merged = false;
    return &g_store[g_store_count++];
}

/** 存入数量: 同类先填满未满格(每格最多 9999), 超出开新格; 返回 false=满 */
static bool StoreDeposit(int type, long stack) {
    if (stack > MAX_ENTRY_STACK) stack = MAX_ENTRY_STACK;
    while (stack > 0) {
        // 先找同类未满格
        StoreEntry* e = NULL;
        for (int i = 0; i < g_store_count; ++i) {
            if (g_store[i].type == type && g_store[i].stack < MAX_ENTRY_STACK) { e = &g_store[i]; break; }
        }
        if (e) {
            long add = MAX_ENTRY_STACK - e->stack;
            if (add > stack) add = stack;
            e->stack += add;
            stack -= add;
        } else {
            long add = (stack > MAX_ENTRY_STACK) ? MAX_ENTRY_STACK : stack;
            if (StoreAppend(type, add) == NULL) return false;
            stack -= add;
        }
    }
    return true;
}

// ============ 存档读写 ============
static void SaveStore(void) {
    if (!g_save_path[0]) return;
    FILE* f = fopen(g_save_path, "w");
    if (!f) {
        if (mod_logger_write)
            mod_logger_write(MOD_LOG_LEVEL_WARNING, "InfiniteBackpack",
                             "存档写入失败: %s", g_save_path);
        return;
    }
    for (int i = 0; i < g_store_count; ++i)
        fprintf(f, "%d %ld\n", g_store[i].type, g_store[i].stack);
    fclose(f);
}

static void LoadStore(const char* private_dir) {
    if (!private_dir) return;
    snprintf(g_save_path, sizeof(g_save_path), "%s/backpack.txt", private_dir);
    FILE* f = fopen(g_save_path, "r");
    if (!f) return;   // 首次使用无存档
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int type = 0; long stack = 0;
        if (sscanf(line, " %d %ld", &type, &stack) != 2) continue;
        if (type <= 0 || type >= 100000 || stack <= 0) continue;
        if (stack > MAX_ENTRY_STACK) stack = MAX_ENTRY_STACK;
        StoreAppend(type, stack);
    }
    fclose(f);
    if (mod_logger_write && g_store_count > 0)
        mod_logger_write(MOD_LOG_LEVEL_INFO, "InfiniteBackpack",
                         "存档加载: %s (%d 种物品)", g_save_path, g_store_count);
}

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

/** 读取 Player.inventory (Item[]); 失败返回 NULL */
static void* GetInventoryArray(void* player) {
    if (!player || !g_inventory_field) return NULL;
    void* arr = NULL;
#if defined(__ANDROID__)
    void** slot = (void**)patchlib_field_get_pointer(g_inventory_field, player);
    if (slot) arr = *slot;
#else
    patchlib_field_get_value(g_inventory_field, player, &arr);
#endif
    return arr;
}

/** 读 Item.type / Item.stack */
static void ItemRead(void* item, int* type, int* stack) {
    *type = 0; *stack = 0;
    if (!item) return;
#if defined(__ANDROID__)
    int* pType = (int*)patchlib_field_get_pointer(g_itemType_field, item);
    int* pStack = (int*)patchlib_field_get_pointer(g_itemStack_field, item);
    if (pType) *type = *pType;
    if (pStack) *stack = *pStack;
#else
    patchlib_field_get_value(g_itemType_field, item, type);
    patchlib_field_get_value(g_itemStack_field, item, stack);
#endif
}

/** 清空一格物品 (type=0, stack=0 => IsAir) */
static void ItemClear(void* item) {
    if (!item) return;
    int zero = 0;
#if defined(__ANDROID__)
    int* pType = (int*)patchlib_field_get_pointer(g_itemType_field, item);
    int* pStack = (int*)patchlib_field_get_pointer(g_itemStack_field, item);
    if (pType) *pType = 0;
    if (pStack) *pStack = 0;
#else
    patchlib_field_set_value(g_itemType_field, item, &zero);
    patchlib_field_set_value(g_itemStack_field, item, &zero);
#endif
}

/** 把物品发到本地玩家背包(拾取逻辑完整走原版) */
static bool GiveItem(void* player, int type, int stack) {
    if (!player) return false;
#if defined(__ANDROID__)
    if (!g_newItem) return false;
    const char* base = (const char*)player;
    const float* pos = (const float*)(base + kPosOffset);
    const int* wh = (const int*)(base + kWidthOffset);
    // NewItem(X, Y, Width, Height, Type, Stack, noBroadcast, pfix, ownership)
    g_newItem((int)pos[0], (int)pos[1], wh[0], wh[1],
              type, stack, false, 0, 0);
    return true;
#else
    if (!g_quickSpawn_method) return false;
    void* src = NULL;
    void* args[3];
    args[0] = &src;
    args[1] = &type;
    args[2] = &stack;
    patchlib_method_invoke_args(g_quickSpawn_method, player, NULL, args);
    return true;
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

/** 在聊天框显示一行文字(尽力而为) */
static void ShowChat(const char* text) {
    if (!text) return;
    void* s = patchlib_string_create(text);
    if (!s) return;
#if defined(__ANDROID__)
    if (!g_newText) return;
    g_newText(s, 255, 255, 255, false);
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

// ============ 指令处理 ============

/** 原版物品标签: 数量>1 用 [i/s数量:物品ID] 显示图标+数字, 否则 [i:物品ID] */
static void FormatItemTag(char* buf, size_t size, int id, long count) {
    if (count > 1) snprintf(buf, size, "[i/s%ld:%d]", count, id);
    else snprintf(buf, size, "[i:%d]", id);
}

/** /store: 存放全部非快捷栏物品(第 10~49 格) */
static void CmdStore(void) {
    void* p = LocalPlayer();
    if (!p) return;
    void* inv = GetInventoryArray(p);
    if (!inv) { ShowChat("[无限背包] 背包数组获取失败"); return; }
    size_t len = patchlib_array_length(inv);
    if (len < (size_t)INVENTORY_FIRST) { ShowChat("[无限背包] 背包数组过小"); return; }

    int end = INVENTORY_LAST;
    if (len - 1 < (size_t)end) end = (int)len - 1;

    long total = 0;
    int kinds = 0;
    char list[1024];
    size_t off = 0;
    off += snprintf(list + off, sizeof(list) - off, "[无限背包] 已存入:");

    // 本轮已列出的物品ID(线性小数组, 最多 40 格去重)
    int listed[64]; int listed_count = 0;
    for (int i = INVENTORY_FIRST; i <= end; ++i) {
        void* item = NULL;
        if (!patchlib_array_at(inv, (size_t)i, &item) || !item) continue;
        int type = 0, stack = 0;
        ItemRead(item, &type, &stack);
        if (type <= 0 || type >= 100000 || stack <= 0) continue;
        if (!StoreDeposit(type, (long)stack)) {
            ShowChat("[无限背包] 存储已满(4096 格), 未完全存入");
            break;
        }
        ItemClear(item);
        total += stack;
        // 列表去重: 同类只显示一次(显示本轮新增后的总量)
        bool seen = false;
        for (int j = 0; j < listed_count; ++j) if (listed[j] == type) seen = true;
        if (!seen && listed_count < 64) {
            listed[listed_count++] = type;
            StoreEntry* e = StoreFind(type);
            if (kinds < 20 && off < sizeof(list) - 64 && e) {
                char tag[32];
                FormatItemTag(tag, sizeof(tag), type, e->stack);
                off += snprintf(list + off, sizeof(list) - off, " %s", tag);
            }
            kinds++;
        }
        g_dirty = true;
    }
    if (total == 0) { ShowChat("[无限背包] 背包(快捷栏以外)没有可存入的物品"); return; }
    if (kinds > 20) {
        snprintf(list + strlen(list), sizeof(list) - strlen(list), " …等 %d 种", kinds);
    }
    snprintf(list + strlen(list), sizeof(list) - strlen(list),
             " 共 %ld 件 (存前 %d 种, 现共 %d 种)", total, kinds, g_store_count);
    ShowChat(list);
    SaveStore();
}

// 从存储表移除一段槽位 [first, last) (保持其余条目顺序不变)
static void StoreRemoveRange(int first, int last) {
    if (first < 0) first = 0;
    if (last > g_store_count) last = g_store_count;
    if (first >= last) return;
    const int keep = g_store_count - (last - first);
    memmove(&g_store[first], &g_store[last], (size_t)(keep - first) * sizeof(StoreEntry));
    g_store_count = keep;
    if (g_store_count == 0) { free(g_store); g_store = NULL; g_store_cap = 0; }
}

/**
 * /take <槽位>[-<槽位>] [数量]
 *   单槽:  /take 3 [数量]      缺省数量=1
 *   范围:  /take 1-5           整格取出 1~5 格全部物品
 */
static void CmdTake(const char* rest) {
    if (!rest || *rest == '\0') {
        ShowChat("[无限背包] 用法: /take <槽位>[-<槽位>] [数量]");
        ShowChat("[无限背包] 单槽缺省数量=1, 范围(如 1-5)整格全部取出");
        return;
    }
    char* end = NULL;
    long a = strtol(rest, &end, 10);
    if (end == rest || a < 1 || a > g_store_count) {
        char buf[96];
        snprintf(buf, sizeof(buf), "[无限背包] 无效的槽位 (1 ~ %d), 先用 /backpack 查看", g_store_count);
        ShowChat(buf);
        return;
    }
    long b = -1;
    if (end && *end == '-') {
        const char* p = end + 1;
        char* e2 = NULL;
        b = strtol(p, &e2, 10);
        if (e2 == p || b < a || b > g_store_count) {
            ShowChat("[无限背包] 无效的范围终点");
            return;
        }
        end = e2;
    }

    void* p = LocalPlayer();
    if (!p) {
        ShowChat("[无限背包] 玩家实例获取失败");
        return;
    }

    // ---- 范围模式: 整格全部取出 ----
    if (b >= 0) {
        long total = 0;
        for (int i = (int)a - 1; i < (int)b; ++i) total += g_store[i].stack;
        // 先拷贝快照, 再统一移除, 最后发放(顺序稳定)
        StoreEntry* snapshot = (StoreEntry*)malloc((size_t)(b - a + 1) * sizeof(StoreEntry));
        if (!snapshot) { ShowChat("[无限背包] 内存不足"); return; }
        memcpy(snapshot, &g_store[a - 1], (size_t)(b - a + 1) * sizeof(StoreEntry));
        StoreRemoveRange((int)(a - 1), (int)b);
        for (int i = 0; i < b - a + 1; ++i) {
            if (!GiveItem(p, snapshot[i].type, (int)snapshot[i].stack)) {
                free(snapshot);
                ShowChat("[无限背包] 发放物品失败(方法解析问题)");
                return;
            }
        }
        // 展示取出的条目(8 个一行, | 分隔), 展示完再释放快照
        char buf[96];
        snprintf(buf, sizeof(buf), "[无限背包] 已取出槽位 %ld-%ld", a, b);
        ShowChat(buf);
        char line[1024];
        int shown = 0;
        size_t off = 0;
        for (int i = 0; i < b - a + 1; ++i) {
            char tag[32];
            FormatItemTag(tag, sizeof(tag), snapshot[i].type, snapshot[i].stack);
            if (off + strlen(tag) + 8 >= sizeof(line)) {
                ShowChat(line);
                off = 0; shown = 0;
            }
            if (shown > 0) off += snprintf(line + off, sizeof(line) - off, " |");
            off += snprintf(line + off, sizeof(line) - off, " %s", tag);
            shown++;
        }
        free(snapshot);
        if (off > 0) ShowChat(line);
        SaveStore();
        return;
    }

    // ---- 单槽模式 ----
    long want = 1;
    if (end && *end != '\0') {
        long v = strtol(end, NULL, 10);
        if (v > 0) want = (v > MAX_ENTRY_STACK) ? MAX_ENTRY_STACK : v;
    }
    StoreEntry* e = &g_store[a - 1];
    const int id = e->type;
    long give = (want > e->stack) ? e->stack : want;

    // 每格最多 9999, 一次发放即可
    if (!GiveItem(p, id, (int)give)) {
        ShowChat("[无限背包] 发放物品失败(方法解析问题)");
        return;
    }
    e->stack -= give;
    const long left = e->stack;
    if (left <= 0) {
        // 移除该槽位(其余槽位顺序保持, 序号不跳变)
        StoreRemoveRange((int)(a - 1), (int)a);
    }
    char tag[32];
    FormatItemTag(tag, sizeof(tag), id, give);
    char buf[160];
    snprintf(buf, sizeof(buf), "[无限背包] 已取出槽位%ld的 %s (剩余 %ld)", a, tag, left);
    ShowChat(buf);
    SaveStore();
}

/** 把 [起,止) 范围内的槽位列表发到聊天框: 每行 8 个, | 分隔 */
static void ShowEntryLines(int first, int last) {
    char line[1024];
    for (int base = first; base < last; base += CHAT_LIST_LIMIT) {
        size_t off = 0;
        int shown = 0;
        for (int i = base; i < last && shown < CHAT_LIST_LIMIT; ++i, ++shown) {
            char tag[32];
            FormatItemTag(tag, sizeof(tag), g_store[i].type, g_store[i].stack);
            if (off + strlen(tag) + 8 >= sizeof(line)) break;
            if (shown > 0) off += snprintf(line + off, sizeof(line) - off, " |");
            off += snprintf(line + off, sizeof(line) - off, " %d %s", i + 1, tag);
        }
        ShowChat(line);
    }
}

/** /backpack: 查看无限背包内容 (首行单独前缀, 条目行 | 分隔, 每行 8 个) */
static void CmdList(void) {
    if (g_store_count == 0) { ShowChat("[无限背包] 是空的"); return; }
    ShowChat("[无限背包]");   // 标题独占一行, 不挤条目
    long total = 0;
    for (int i = 0; i < g_store_count; ++i) total += g_store[i].stack;
    ShowEntryLines(0, g_store_count);
    char buf[96];
    snprintf(buf, sizeof(buf), "[无限背包] 共 %d 格 / %ld 件", g_store_count, total);
    ShowChat(buf);
}

/** 判断聊天文本是否为本 Mod 的指令 */
static bool IsModCommand(const char* t) {
    if (!t || t[0] != '/') return false;
    return strncmp(t, "/store", 6) == 0 || strncmp(t, "/存", 2) == 0 ||
           strncmp(t, "/take", 5) == 0 || strncmp(t, "/取", 2) == 0 ||
           strncmp(t, "/backpack", 9) == 0 || strncmp(t, "/bp", 3) == 0;
}

/** 处理一条指令 */
static void HandleCommand(const char* raw) {
    if (!raw) return;
    if (strncmp(raw, "/store", 6) == 0 || strncmp(raw, "/存", 2) == 0) {
        CmdStore();
    } else if (strncmp(raw, "/take", 5) == 0 || strncmp(raw, "/取", 2) == 0) {
        const char* rest = raw;
        rest += (strncmp(raw, "/take", 5) == 0) ? 5 : 2;
        while (*rest == ' ' || *rest == '\t') ++rest;
        CmdTake(rest);
    } else if (strncmp(raw, "/backpack", 9) == 0 || strncmp(raw, "/bp", 3) == 0) {
        CmdList();
    }
}

// ============ Hook: 聊天消息处理 ============
static void ProcessIncomingMessage_Postfix(patch_handle_t instance, void **args, void *result,
                                           const patch_method_signature_t *sig_info) {
    (void)instance; (void)result; (void)sig_info;
    if (!args) return;
    void* message = *(void**)args[0];
    const int clientId = *(int*)args[1];
    const int myPlayer = LocalPlayerId();
    if (myPlayer < 0 || clientId != myPlayer) return;
    void* str = GetMessageText(message);
    if (!str) return;
    char* text = patchlib_string_cstr(str);
    if (text) {
        if (IsModCommand(text)) HandleCommand(text);
        free(text);
    }
}

static void SendChatMessageFromClient_Postfix(patch_handle_t instance, void **args, void *result,
                                              const patch_method_signature_t *sig_info) {
    (void)instance; (void)result; (void)sig_info;
    if (!args) return;
    void* message = *(void**)args[0];
    void* str = GetMessageText(message);
    if (!str) return;
    char* text = patchlib_string_cstr(str);
    if (text) {
        if (IsModCommand(text)) HandleCommand(text);
        free(text);
    }
}

// ============ 模块初始化 ============
static void init_mod(kernel_mod_handle_t *handle) {
    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "InfiniteBackpack", "初始化无限背包模组");
        mod_logger_write(MOD_LOG_LEVEL_INFO, "InfiniteBackpack", "私有目录: %s",
                         handle && handle->private_dir ? handle->private_dir : "NULL");
    }

    // 0. 加载存档
    LoadStore(handle && handle->private_dir ? handle->private_dir : NULL);

    // 1. 类型
    g_main_type = patchlib_type_get_type("Terraria", "Main");
    g_player_type = patchlib_type_get_type("Terraria", "Player");
    g_item_type = patchlib_type_get_type("Terraria", "Item");
    if (!g_main_type || !g_player_type || !g_item_type) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "InfiniteBackpack",
                             "获取类型失败 (Main/Player/Item)");
        }
        return;
    }

    // 2. 方法解析(平台相关)
#if defined(__ANDROID__)
    patch_handle_t myPlayer_prop = patchlib_type_get_property(g_main_type, "myPlayer");
    if (myPlayer_prop) {
        patch_handle_t getter = patchlib_property_get_get_method(myPlayer_prop);
        if (getter) g_get_myPlayer = (int (*)(void))patchlib_method_get_pointer(getter);
    }
    patch_handle_t newText_method = patchlib_type_get_method_by_param_count(g_main_type, "NewText", 5);
    if (newText_method) {
        g_newText = (void (*)(void*, uint8_t, uint8_t, uint8_t, bool))
                patchlib_method_get_pointer(newText_method);
    }
    patch_handle_t msg_type = patchlib_type_get_type("Terraria.Chat", "ChatMessage");
    if (msg_type) {
        patch_handle_t text_prop = patchlib_type_get_property(msg_type, "Text");
        if (text_prop) {
            patch_handle_t getter = patchlib_property_get_get_method(text_prop);
            if (getter) g_getMsgText = (void* (*)(void*))patchlib_method_get_pointer(getter);
        }
    }
    patch_handle_t newItem_method = patchlib_type_get_method_by_param_count(g_item_type, "NewItem", 9);
    if (!newItem_method) newItem_method = patchlib_type_get_method(g_item_type, "NewItem");
    if (newItem_method) {
        g_newItem = (int (*)(int, int, int, int, int, int, bool, int, int))
                patchlib_method_get_pointer(newItem_method);
    }
#else
    g_myPlayer_field = patchlib_type_get_field(g_main_type, "myPlayer");
    g_newText_method = patchlib_type_get_method_by_param_count(g_main_type, "NewText", 4);
    if (!g_newText_method) g_newText_method = patchlib_type_get_method(g_main_type, "NewText");
    patch_handle_t msg_type = patchlib_type_get_type("Terraria.Chat", "ChatMessage");
    if (msg_type) {
        patch_handle_t text_prop = patchlib_type_get_property(msg_type, "Text");
        if (text_prop) {
            g_getMsgText_method = patchlib_property_get_get_method(text_prop);
        }
    }
    // QuickSpawnItem 有两个 3 参重载, 用参数名选 (source, item, stack) 版本
    static const char* qsNames[] = { "source", "item", "stack" };
    g_quickSpawn_method = patchlib_type_get_method_by_param_names(
            g_player_type, "QuickSpawnItem", 3, qsNames);
    if (!g_quickSpawn_method) g_quickSpawn_method = patchlib_type_get_method(g_player_type, "QuickSpawnItem");
#endif

    // 3. 字段
    g_mainPlayer_field = patchlib_type_get_field(g_main_type, "player");
    g_inventory_field = patchlib_type_get_field(g_player_type, "inventory");
    g_itemType_field = patchlib_type_get_field(g_item_type, "type");
    g_itemStack_field = patchlib_type_get_field(g_item_type, "stack");
    if (!g_mainPlayer_field || !g_inventory_field || !g_itemType_field || !g_itemStack_field) {
        if (mod_logger_write) {
            mod_logger_write(MOD_LOG_LEVEL_ERROR, "InfiniteBackpack",
                             "获取字段失败: player=%p inventory=%p type=%p stack=%p",
                             (void*)g_mainPlayer_field, (void*)g_inventory_field,
                             (void*)g_itemType_field, (void*)g_itemStack_field);
        }
        return;
    }

    // 4. Hook 聊天拦截(postfix)
    patch_handle_t chat_method = patchlib_type_get_method_by_param_count(
            patchlib_type_get_type("Terraria.Chat", "ChatCommandProcessor"),
            "ProcessIncomingMessage", 2);
    if (chat_method) {
        g_hookProcessIncoming = patchlib_install_prepost_hook(
                chat_method, NULL, ProcessIncomingMessage_Postfix);
    }
    if (g_hookProcessIncoming == PATCH_HOOK_INVALID_ID && mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_WARNING, "InfiniteBackpack",
                         "ProcessIncomingMessage Hook 未安装(单机指令可能不可用)");
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
    if (g_hookSendChatFromClient == PATCH_HOOK_INVALID_ID && mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_WARNING, "InfiniteBackpack",
                         "SendChatMessageFromClient Hook 未安装(多人客户端指令可能不可用)");
    }

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "InfiniteBackpack",
                         "初始化完成: 存储=%d 种, /store /take /backpack 可用",
                         g_store_count);
    }
}

// ============ 模块清理 ============
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;
    if (g_dirty) SaveStore();   // 退出前兜底保存

    if (g_hookProcessIncoming != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookProcessIncoming);
        g_hookProcessIncoming = PATCH_HOOK_INVALID_ID;
    }
    if (g_hookSendChatFromClient != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hookSendChatFromClient);
        g_hookSendChatFromClient = PATCH_HOOK_INVALID_ID;
    }

    free(g_store);
    g_store = NULL;
    g_store_count = g_store_cap = 0;
    g_dirty = false;

    g_main_type = NULL;
    g_player_type = NULL;
    g_item_type = NULL;
    g_mainPlayer_field = NULL;
    g_myPlayer_field = NULL;
    g_inventory_field = NULL;
    g_itemType_field = NULL;
    g_itemStack_field = NULL;
    g_getMsgText_method = NULL;
    g_newText_method = NULL;
    g_quickSpawn_method = NULL;
#if defined(__ANDROID__)
    g_get_myPlayer = NULL;
    g_newText = NULL;
    g_getMsgText = NULL;
    g_newItem = NULL;
#endif

    if (mod_logger_write) {
        mod_logger_write(MOD_LOG_LEVEL_INFO, "InfiniteBackpack", "清理模组");
    }
}

// ============ 模块信息 ============
static kernel_mod_info_t g_mod_info = {
        .pkg_id = "lzup.chat.backpack",
        .version_code = 2,
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
        mod_logger_write(MOD_LOG_LEVEL_INFO, "InfiniteBackpack", "无限背包模组实例创建");
    }
    return &g_ops;
}
