/*
 * common.h — 系统公共定义 / System common definitions
 *
 * 所有模块共享的基础类型、常量和工具函数声明。
 * 本文件定义了角色标识、缓冲区大小边界、错误码体系、
 * User/Session 结构体以及跨平台控制台工具函数。
 *
 * Shared foundation for all modules: role identifiers, buffer size limits,
 * error code system, User/Session structs, and cross-platform console utilities.
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

/* 角色定义 — 用于权限判断和菜单路由
   Role definitions — used for permission checks and menu routing */
#define ROLE_ADMIN  "admin"    /* 管理员: 全部权限 / full access */
#define ROLE_DOCTOR "doctor"   /* 医生: 接诊/开药/病房 / consultation/prescription/ward */
#define ROLE_PATIENT "patient" /* 患者: 挂号/查询/资料 / registration/query/profile */

/* 缓冲区大小上限 — 确保栈分配安全
   Max buffer sizes — guarantee safe stack allocation */
#define MAX_USERNAME 50    /* 用户名 / username */
#define MAX_PASSWORD 65    /* SHA-256 十六进制哈希 (64 字符 + '\0') */
#define MAX_NAME 100       /* 姓名/科室名等 / names, department names, etc. */
#define MAX_ID 20          /* 各类 ID (科室/医生/患者/药品/病历/处方等) */
#define MAX_BUFFER 256     /* 通用缓冲区 / general-purpose buffer */
#define MAX_MENU_ITEMS 30  /* 控制台菜单最大选项数 / max console menu items */

/* 状态码 — undef 避免与 windows.h (winerror.h) 中的宏冲突
   Status codes — #undef first to avoid conflict with winerror.h macros */
#ifndef SUCCESS
#define SUCCESS 0
#endif
#undef ERROR_INVALID_INPUT       /* 输入格式错误 / invalid input format */
#undef ERROR_NOT_FOUND           /* 未找到目标记录 / target record not found */
#undef ERROR_DUPLICATE           /* 重复记录 (如用户名已存在) / duplicate record */
#undef ERROR_PERMISSION_DENIED   /* 权限不足 / insufficient permission */
#undef ERROR_FILE_IO             /* 文件读写失败 / file I/O failure */
#define ERROR_INVALID_INPUT -1
#define ERROR_NOT_FOUND -2
#define ERROR_DUPLICATE -3
#define ERROR_PERMISSION_DENIED -4
#define ERROR_FILE_IO -5

/* 用户结构 — 持久化到 users.txt
   User struct — persisted to users.txt */
typedef struct {
    char username[MAX_USERNAME];    /* 登录用户名 / login username */
    char password[MAX_PASSWORD];    /* SHA-256 十六进制哈希 / SHA-256 hex hash */
    char role[20];                  /* admin / doctor / patient */
} User;

/* 会话结构 — 内存中跟踪当前登录状态
   Session struct — in-memory tracking of current login state */
typedef struct {
    User current_user;              /* 当前登录用户 / currently logged-in user */
    bool logged_in;                 /* 是否已登录 / login flag */
} Session;

/* =======================  控制台工具函数 ======================= */

/* 初始化控制台编码为 UTF-8 (SetConsoleOutputCP(65001))
   Initialize console code page to UTF-8 */
void init_console_encoding(void);

/* 清空 stdin 输入缓冲区 (防残留输入干扰后续读取)
   Flush stdin buffer to prevent stale input from interfering */
void clear_input_buffer(void);

/* 暂停等待用户按键 (用于"按任意键继续")
   Pause and wait for any key press ("press any key to continue") */
void pause_screen(void);

/* 方向键菜单选择: 支持 ↑↓ 导航 + Enter 确认 + 数字快捷键
   缓存菜单项并在原位高亮当前选中行 (通过 ANSI 光标控制)
   Arrow-key menu: ↑↓ navigation + Enter confirm + number shortcuts.
   Menu items are cached and the current selection is highlighted in-place
   using ANSI cursor save/restore sequences. */
int get_menu_choice(int min, int max);

/* 获取当前时间字符串 (格式 "YYYY-MM-DD HH:MM:SS")
   Get current time as formatted string */
void get_current_time(char *buffer, int buffer_size);

/* 生成唯一 ID: "前缀_YYYYMMDDHHMMSS_序号" (总数 19 字符以内)
   Generate unique ID: "prefix_YYYYMMDDHHMMSS_seq" (max 19 chars) */
void generate_id(char *buffer, int buffer_size, const char *prefix);

/* 从 stdin 读取一行并转换为 UTF-8 (处理 Windows GBK→UTF-8 编码转换)
   Read a line from stdin and convert to UTF-8 (handles GBK→UTF-8 on Windows) */
char* read_input_line(char *buf, size_t size);

#endif // COMMON_H
