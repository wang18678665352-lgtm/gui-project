/*
 * ui_utils.h — 控制台 UI 工具库 / Console UI utility library
 *
 * 为控制台界面提供丰富的终端 UI 组件，包括:
 *   - ANSI 转义序列颜色/样式宏定义 (前景色/背景色/粗体/组合样式)
 *   - 方向键菜单高亮: 缓存菜单项 + ANSI 光标保存/恢复实现原位高亮
 *   - UI 绘制函数: 清屏、分隔线、标题框、表头、状态信息、确认对话框
 *   - 表格列打印: UTF-8 显示宽度感知的对齐打印
 *   - 智能输入: ID 查找/患者选择/药品选择 (支持搜索自动补全)
 *   - 模板快捷输入: #编号 语法插入预定义诊断/治疗模板
 *   - 分页与列表选择: 支持搜索过滤和方向键高亮导航
 *
 * Rich terminal UI toolkit: ANSI color/style macros, arrow-key menu highlighting
 * via cursor save/restore, box drawing, table column printing with CJK width
 * awareness, smart ID/patient/drug input with search, template quick-input via
 * #number syntax, pagination, and interactive list selection with search.
 */

#ifndef UI_UTILS_H
#define UI_UTILS_H

#include "common.h"

/* =======================  ANSI 颜色定义 / ANSI Color Definitions ======================= */

/* 文字前景色 / Foreground colors */
#define C_RESET    "\033[0m"     /* 重置所有样式 / reset all */
#define C_BOLD     "\033[1m"     /* 粗体 / bold */
#define C_DIM      "\033[2m"     /* 暗淡 / dim */
#define C_RED      "\033[31m"    /* 红色 — 错误信息 / errors */
#define C_GREEN    "\033[32m"    /* 绿色 — 成功信息 / success */
#define C_YELLOW   "\033[33m"    /* 黄色 — 警告信息 / warnings */
#define C_BLUE     "\033[34m"    /* 蓝色 */
#define C_MAGENTA  "\033[35m"    /* 品红 */
#define C_CYAN     "\033[36m"    /* 青色 — 标题/标签 / titles, labels */
#define C_WHITE    "\033[97m"    /* 亮白 — 正文 / body text */

/* 背景色 / Background colors */
#define BG_RED     "\033[41m"    /* 红底 — 严重告警 / critical alerts */
#define BG_GREEN   "\033[42m"    /* 绿底 — 成功状态 / success state */
#define BG_YELLOW  "\033[43m"    /* 黄底 — 警告状态 / warning state */
#define BG_BLUE    "\033[44m"    /* 蓝底 — 选中高亮 / selection highlight */
#define BG_CYAN    "\033[46m"    /* 青底 */
#define BG_WHITE   "\033[107m"   /* 白底 */

/* 组合样式 — 语义化宏，便于统一调整 UI 风格
   Composite styles — semantic macros for consistent UI styling */
#define S_TITLE    C_BOLD C_CYAN     /* 大标题 / main title */
#define S_SUCCESS  C_BOLD C_GREEN    /* 成功操作 / operation success */
#define S_ERROR    C_BOLD C_RED      /* 操作失败 / operation error */
#define S_WARNING  C_BOLD C_YELLOW   /* 警告提示 / warning notice */
#define S_INFO     C_BOLD C_WHITE    /* 一般信息 / info message */
#define S_LABEL    C_CYAN            /* 字段标签 / field label */
#define S_VALUE    C_WHITE           /* 字段值 / field value */

/* =======================  菜单缓存 (方向键高亮) / Menu Cache (Arrow-key Highlight) ======================= */

/* 初始化菜单缓存 (每次进入菜单前调用)
   Initialize menu cache before each menu display */
void ui_menu_cache_init(void);

/* 填充菜单缓存: nums[] 选项编号, texts[] 显示文本, is_exit[] 是否退出项
   返回总行数 (用于 get_menu_choice 的 max 参数)
   Fill menu cache with item numbers, texts, and exit flags. Returns total lines. */
int  ui_menu_cache_fill(int *nums, char texts[][60], bool *is_exit, int max);

/* =======================  行跟踪 (原位高亮) / Line Tracking (In-place Highlight) ======================= */

/* 记录当前光标行位置 (在输出菜单项之前调用)
   Save current cursor line position before outputting menu items */
void ui_menu_track_line(void);

/* 获取缓存的总选项行数 / Get cached total item line count */
int  ui_menu_get_saved_total(void);

/* 获取指定索引项的光标偏移量 (用于 ANSI 光标跳转到该项)
   Get cursor offset for item at given index */
int  ui_menu_get_item_offset(int idx);

/* =======================  UI 绘制函数 / UI Drawing Functions ======================= */

void ui_init_ansi(void);                          /* 启用 Windows VT 终端处理 / enable VT processing */
void ui_clear_screen(void);                       /* ANSI 清屏 / clear screen via ANSI escape */
void ui_line(int width, const char *ch);          /* 画指定宽度水平线 / draw horizontal line */
void ui_box_top(const char *title);               /* 盒子顶部 ┌── title ──┐ / box top border with title */
void ui_box_bottom(void);                         /* 盒子底部 └──────────┘ / box bottom border */
void ui_header(const char *title);                /* 显示大标题 (居中, 双线框) / centered title with double-line */
void ui_sub_header(const char *title);            /* 显示子标题 (下划线装饰) / sub-title with underline */
void ui_divider(void);                            /* 分隔线 ────────── / divider line */
void ui_ok(const char *msg);                      /* 成功消息 (绿色 ✓) / success message with green check */
void ui_err(const char *msg);                     /* 错误消息 (红色 ✗) / error message with red cross */
void ui_warn(const char *msg);                    /* 警告消息 (黄色 !) / warning message with yellow mark */
void ui_info(const char *label, const char *value); /* 标签-值对 (label: value) / label-value pair display */
void ui_menu_item(int num, const char *text);     /* 普通菜单项 (编号 + 文本) / menu item with number */
void ui_menu_exit(int num, const char *text);     /* 退出菜单项 (灰色样式) / exit menu item with dim style */
void ui_user_badge(const char *name, const char *role); /* 用户身份徽章 / user identity badge */
void ui_step(int step, const char *desc);         /* 步骤指示器 / step indicator */

/* =======================  确认对话框 / Confirmation Dialog ======================= */

/* 模态确认: 显示提示并等待 Y/N 输入，返回 true 表示确认
   Modal confirmation: display prompt, wait for Y/N, return true on confirm */
bool ui_confirm(const char *prompt);

/* =======================  表格列打印 (显示宽度对齐) / Table Column Printing (Display-width Aligned) ======================= */

/* 计算 UTF-8 字符串的显示宽度 (CJK 字符宽 2, ASCII 宽 1)
   Calculate display width of UTF-8 string (CJK chars = 2, ASCII = 1) */
int  utf8_display_width(const char *s);

/* 按指定宽度打印字符串列 (不足补空格, 超出截断)
   Print string column with padding/truncation to fixed display width */
void ui_print_col(const char *s, int width);

/* 按指定宽度打印整数列 / Print integer column with fixed display width */
void ui_print_col_int(int val, int width);

/* 按指定宽度打印浮点数列 / Print float column with fixed display width */
void ui_print_col_float(float val, int width);

/* =======================  智能输入 / Smart Input ======================= */

/* 智能 ID 查找: 从 id_list 中选择匹配 ID，支持模糊搜索
   Smart ID lookup: select from id_list with fuzzy search support */
int  smart_id_lookup(const char *prompt, const char *id_list[], int count, char *output, int out_size);

/* 智能患者输入: 按医生关联过滤患者列表，支持名称/ID 搜索
   Smart patient input: filter patients by doctor association, search by name/ID */
int  smart_patient_input(const char *doctor_id, const char *prompt, char *output, int out_size);

/* 智能药品输入: 从药品列表中选择，显示库存和价格信息
   Smart drug input: select from drug list with stock & price display */
int  smart_drug_input(const char *prompt, char *output, int out_size);

/* =======================  模板快捷输入 / Template Quick Input ======================= */

/* 模板快捷输入: 输入 #编号 自动展开为对应模板文本
   支持 category 过滤 (diagnosis/treatment/exam)
   Quick template input: #number expands to template text.
   The category parameter filters available templates. */
int  quick_template_input(const char *category, const char *prompt, char *output, int out_size);

/* =======================  分页与搜索 / Pagination & Search ======================= */

/* 分页显示列表: items 为字符串数组, page_size 每页行数 (默认 15)
   Paginated list display with page navigation, search, and jump-to-page */
void ui_paginate(const char **items, int count, int page_size, const char *title);

/* 搜索并选择列表项: 输入关键词实时过滤，方向键导航，回车确认
   返回选中索引 (0-based)，-1 表示取消
   Search and select from list: real-time keyword filtering, arrow-key navigation,
   Enter to confirm. Returns selected index (0-based) or -1 for cancel. */
int  ui_search_list(const char *prompt, const char **items, int count);

/* 高亮列表选择: 纯方向键导航 (无搜索过滤)，回车确认
   返回选中索引 (0-based)，-1 表示取消
   Simple list selection: arrow-key navigation only, Enter to confirm.
   Returns selected index (0-based) or -1 for cancel. */
int  ui_select_list(const char *prompt, const char **items, int count);

#endif
