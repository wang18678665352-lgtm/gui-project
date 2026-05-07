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

/* =======================  角色定义 / Role Definitions ======================= */

/*
 * 角色定义 — 用于权限判断和菜单路由
 *
 * ROLE_ADMIN  - 管理员: 拥有全部权限 (用户管理、系统配置、所有数据查看)
 * ROLE_DOCTOR - 医生:   接诊/开药/病房管理/患者信息查看
 * ROLE_PATIENT- 患者:   挂号/查询药品和医生/编辑个人资料
 *
 * Role definitions — used for permission checks and menu routing
 */
#define ROLE_ADMIN  "admin"    /* 管理员: 全部权限 / full access */
#define ROLE_DOCTOR "doctor"   /* 医生: 接诊/开药/病房 / consultation/prescription/ward */
#define ROLE_PATIENT "patient" /* 患者: 挂号/查询/资料 / registration/query/profile */

/* =======================  缓冲区大小 / Buffer Size Limits ======================= */

/*
 * 缓冲区大小上限 — 确保栈分配安全, 防止缓冲区溢出
 *
 * MAX_USERNAME 50:   用户名最大长度 (用于 login 用户名输入)
 * MAX_PASSWORD 65:   SHA-256 哈希的十六进制字符串 (64 字符 + '\0')
 * MAX_NAME 100:      姓名、科室名等中文/英文名称
 * MAX_ID 20:         各类 ID (科室/医生/患者/药品/病历/处方/病房等)
 * MAX_BUFFER 256:    通用临时缓冲区 (格式化字符串、中间结果等)
 * MAX_MENU_ITEMS 30: 控制台菜单最大选项数 (get_menu_choice 方向键导航上限)
 *
 * Max buffer sizes — guarantee safe stack allocation
 */
#define MAX_USERNAME 50    /* 用户名 / username */
#define MAX_PASSWORD 65    /* SHA-256 十六进制哈希 (64 字符 + '\0') */
#define MAX_NAME 100       /* 姓名/科室名等 / names, department names, etc. */
#define MAX_ID 20          /* 各类 ID (科室/医生/患者/药品/病历/处方等) */
#define MAX_BUFFER 256     /* 通用缓冲区 / general-purpose buffer */
#define MAX_MENU_ITEMS 30  /* 控制台菜单最大选项数 / max console menu items */

/* =======================  状态码 / Status Codes ======================= */

/*
 * 状态码体系 — 函数返回值, 用于标识操作结果
 *
 * 注意: 使用 #undef 避免与 windows.h (winerror.h) 中的同名宏冲突
 *       这些宏在 windows.h 中已定义为不同的错误值
 *
 * SUCCESS                (0):  操作成功
 * ERROR_INVALID_INPUT   (-1):  输入格式错误、缺少必填字段
 * ERROR_NOT_FOUND       (-2):  未找到目标记录 (如用户不存在)
 * ERROR_DUPLICATE       (-3):  重复记录 (如用户名已存在)
 * ERROR_PERMISSION_DENIED(-4): 权限不足 (如患者尝试访问管理功能)
 * ERROR_FILE_IO         (-5):  文件读写失败 (如 users.txt 不可读)
 *
 * Status codes — #undef first to avoid conflict with winerror.h macros
 */
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

/* =======================  用户结构 / User Struct ======================= */

/*
 * 用户结构 — 代表一个系统用户, 持久化到 users.txt
 *
 * 字段:
 *   username - 登录用户名 (唯一标识, 不超过 MAX_USERNAME)
 *   password - SHA-256 十六进制哈希值 (固定 64 字符 + '\0')
 *   role     - 角色: "admin" | "doctor" | "patient"
 *
 * 注意: 密码以哈希存储, 永不明文保存
 *
 * User struct — persisted to users.txt
 */
typedef struct {
    char username[MAX_USERNAME];    /* 登录用户名 / login username */
    char password[MAX_PASSWORD];    /* SHA-256 十六进制哈希 / SHA-256 hex hash */
    char role[20];                  /* admin / doctor / patient */
} User;

/* =======================  会话结构 / Session Struct ======================= */

/*
 * 会话结构 — 内存中跟踪当前登录状态
 *
 * 字段:
 *   current_user - 当前登录用户的完整信息 (User 结构体)
 *   logged_in    - 登录标志: true=已登录, false=未登录
 *
 * 用途:
 *   - 权限检查: 根据 current_user.role 判断操作权限
 *   - 页面显示: 在标题栏显示 current_user.username
 *   - 会话保持: logged_in 控制菜单流程 (未登录时跳回登录页)
 *
 * Session struct — in-memory tracking of current login state
 */
typedef struct {
    User current_user;              /* 当前登录用户 / currently logged-in user */
    bool logged_in;                 /* 是否已登录 / login flag */
} Session;

/* =======================  控制台工具函数 / Console Utilities ======================= */

/*
 * 初始化控制台编码为 UTF-8 (SetConsoleOutputCP(65001))
 *
 * 调用时机: 程序启动时, 在任何 printf 输出之前
 * 作用: 使 UTF-8 编码的中文字符能在 Windows 控制台正常显示
 *   如果不调用, GBK 环境下的控制台将无法正确渲染 UTF-8 字符
 *
 * Initialize console code page to UTF-8
 */
void init_console_encoding(void);

/*
 * 清空 stdin 输入缓冲区 — 防止残留输入干扰后续读取
 *
 * 调用时机: 在每次输入操作之前, 尤其是在 scanf 后调用 getchar 之前
 * 行为: 循环读取并丢弃 stdin 中的所有字符直到 '\n' 或 EOF
 *
 * Flush stdin buffer to prevent stale input from interfering
 */
void clear_input_buffer(void);

/*
 * 暂停等待用户按键 — 用于 "按任意键继续" 场景
 *
 * 行为: 输出提示信息, 等待用户按任意键 + 回车, 清空缓冲区后返回
 *
 * Pause and wait for any key press ("press any key to continue")
 */
void pause_screen(void);

/*
 * 方向键菜单选择 — 支持 ↑↓ 导航 + Enter 确认 + 数字快捷键
 *
 * 参数:
 *   min - 菜单项最小编号
 *   max - 菜单项最大编号
 *
 * 返回: 用户选择的菜单项编号 (min ~ max), 或 -1 表示退出
 *
 * 实现: 缓存菜单项并在原位高亮当前选中行 (通过 ANSI 光标控制)
 *   依赖 ui_utils.c 中的菜单项缓存 (g_menu_nums, g_menu_texts 等)
 *   使用 DECSC (\0337) / DECRC (\0338) 保存和恢复光标位置
 *   支持: ↑↓ 方向键 / 数字快捷选择 / Enter 确认 / q 退出
 *
 * Arrow-key menu: ↑↓ navigation + Enter confirm + number shortcuts.
 * Menu items are cached and the current selection is highlighted in-place
 * using ANSI cursor save/restore sequences.
 */
int get_menu_choice(int min, int max);

/*
 * 获取当前时间字符串 — 格式 "YYYY-MM-DD HH:MM:SS"
 *
 * 参数:
 *   buffer      - 输出缓冲区
 *   buffer_size - 缓冲区大小
 *
 * 行为: 调用 time() + localtime() 获取本地时间, 用 strftime 格式化
 *
 * Get current time as formatted string
 */
void get_current_time(char *buffer, int buffer_size);

/*
 * 生成唯一 ID — 格式 "前缀_YYYYMMDDHHMMSS_序号"
 *
 * 参数:
 *   buffer      - 输出缓冲区 (至少 20 字节)
 *   buffer_size - 缓冲区大小
 *   prefix      - ID 前缀字符串 (如 "P" 表示患者)
 *
 * 生成规则: 前缀 + '_' + 时间戳(14位) + '_' + 序号(自增)
 * 示例: "P_20260507143025_001" (共 19 字符)
 * 唯一性保证: 时间戳精确到秒 + 自增序号, 避免同秒内冲突
 *
 * Generate unique ID: "prefix_YYYYMMDDHHMMSS_seq" (max 19 chars)
 */
void generate_id(char *buffer, int buffer_size, const char *prefix);

/*
 * 从 stdin 读取一行并处理编码 — 处理 Windows GBK→UTF-8 编码转换
 *
 * 参数:
 *   buf  - 输出缓冲区
 *   size - 缓冲区大小
 *
 * 返回: buf (成功) 或 NULL (失败/EOF)
 *
 * 实现: 在 Windows 上读取多字节 (可能是 GBK), 转换为 UTF-8
 *   在 Unix 上直接使用 fgets 读取
 *
 * Read a line from stdin and convert to UTF-8 (handles GBK→UTF-8 on Windows)
 */
char* read_input_line(char *buf, size_t size);

#endif // COMMON_H
