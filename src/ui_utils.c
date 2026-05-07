/*
 * ui_utils.c — 控制台 UI 工具库实现 / Console UI utility library implementation
 *作者：王成烨
 * 实现丰富的控制台终端 UI 组件:
 *   - UTF-8 显示宽度计算 (CJK=2, ASCII=1) — 用于表格列对齐
 *   - ANSI/VT 终端初始化 (Windows EnableVirtualTerminalProcessing)
 *   - UI 绘制: 清屏、水平线、盒子边框、标题、分隔线、状态消息
 *   - 菜单项缓存: 记录每项行号偏移，配合 common.c 的 get_menu_choice
 *     实现方向键原位高亮 (通过 ANSI DECSC/DECRC 光标控制)
 *   - 确认对话框
 *   - 表格列打印 (显示宽度感知的对齐)
 *   - 智能输入: ID 查找、患者选择 (按医生过滤)、药品选择
 *   - 模板快捷输入: #n 语法展开诊断模板
 *   - 分页显示 (搜索/跳页/翻页)
 *   - 搜索列表选择 (实时过滤 + 方向键高亮)
 *   - 普通列表选择 (方向键高亮)
 *
 * Implements terminal UI: UTF-8 display width, ANSI init, box/title/divider
 * drawing, menu item caching for in-place arrow-key highlighting, confirmation
 * dialogs, width-aware table columns, smart input helpers, template quick-input,
 * pagination with search, and interactive list selection.
 */

#include "ui_utils.h"
#include "data_storage.h"

#ifdef _WIN32
#include <windows.h>
#include <conio.h>                        /* _getch() — 无回显读取按键, 用于方向键检测 */
#endif

/* ==================  行跟踪状态 (原位高亮用) / Line Tracking State ================== */

/*
 * 行跟踪状态变量 — 用于菜单项原位高亮功能
 *
 * 这些静态变量跟踪输出的菜单项行号，以便在方向键切换时通过 ANSI
 * 光标控制精确定位到每个菜单项进行重绘 (高亮/取消高亮)。
 *
 * 工作流程:
 *   1. ui_box_top / ui_menu_cache_init 重置 g_line_counter
 *   2. 每次 ui_menu_item 调用递增 g_line_counter, 并记录 g_item_offsets[i]
 *   3. ui_menu_cache_fill 将当前数据固化到 g_saved_* 快照
 *   4. get_menu_choice (common.c) 通过 ui_menu_get_item_offset 获取行偏移
 *   5. 方向键导航时使用 \033[NA 上移 N 行精确定位到目标行
 *
 * These static variables track menu item line positions so arrow-key
 * navigation can precisely reposition the cursor for in-place redraw
 * (highlight/unhighlight) using ANSI cursor control sequences. */

static int   g_line_counter      = 0;        /* 当前菜单段落累计行数 / cumulative line count */
static int   g_item_offsets[MAX_MENU_ITEMS]; /* 每个菜单项的行号 / line number per item */
static int   g_saved_total       = 0;        /* 固化的总行数 / snapshot total lines */
static int   g_saved_offsets[MAX_MENU_ITEMS];/* 固化的偏移量 / snapshot offsets */
static int   g_saved_count       = 0;        /* 固化的项数 / snapshot item count */

/* ==================  UTF-8 显示宽度 / UTF-8 Display Width ================== */

/*
 * 计算 UTF-8 字符串的终端显示宽度
 *
 * 参数:
 *   s - UTF-8 编码的字符串 (可为 NULL)
 *
 * 返回: 终端显示列数 (整数), NULL 返回 0
 *
 * 规则 (基于 UTF-8 首字节高位特征推断):
 *   单字节 (0x00-0x7F):  ASCII 字符, 终端占 1 列
 *   3 字节 (0xE0-0xEF):  CJK 汉字,   终端占 2 列 (全角)
 *   2 字节 (0xC0-0xDF):  拉丁扩展,   终端占 1 列
 *   4 字节 (0xF0-0xF7):  不常见符号, 终端占 1 列 (保守)
 *   其他:               终端占 1 列
 *
 * 用途: printf %s 对中文字符的占位计算不准确 — printf 按字节数计数,
 *       但终端实际显示 CJK 字符占 2 列。此函数用于正确计算表格列对齐。
 *
 * Calculate terminal display width of UTF-8 string:
 *   - 1-byte (ASCII) → width 1
 *   - 3-byte (CJK, 0xE0+) → width 2
 *   - 2-byte (Latin-ext, 0xC0+) → width 1
 * Used for table column alignment where printf %s miscounts CJK width. */
int utf8_display_width(const char *s) {
    int w = 0;
    if (!s) return 0;                     /* NULL 字符串 → 宽度 0 */
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c <= 0x7F)      { w += 1; s += 1; }  /* ASCII — 1 字节, 1 列宽 */
        else if (c >= 0xE0) { w += 2; s += 3; }  /* CJK 汉字 — 3 字节, 2 列宽 */
        else if (c >= 0xC0) { w += 1; s += 2; }  /* 拉丁扩展 — 2 字节, 1 列宽 */
        else                { w += 1; s += 1; }  /* 其他 / other */
    }
    return w;
}

/* ==================  ANSI / VT 终端初始化 / ANSI/VT Terminal Init ================== */

/*
 * 启用 ANSI/VT 虚拟终端处理
 *
 * 无参数, 无返回值
 *
 * 作用: 在 Windows 控制台上启用 ENABLE_VIRTUAL_TERMINAL_PROCESSING 标志,
 *       使 ANSI 转义序列 (颜色代码 \033[...m, 光标控制 \033[nA 等)
 *       能被控制台正确解释和渲染。
 *
 * 注意: 没有这一步, 所有颜色代码都会以原始文本形式输出到屏幕
 *       (例如 "\033[31m" 会直接作为可见字符显示)。
 *       在 Unix/Linux 终端上此函数无操作 (ANSI 天生支持)。
 *
 * Enable virtual terminal processing on Windows so ANSI escape sequences
 * (colors, cursor control) are interpreted by the console. */
void ui_init_ansi(void) {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);  /* 获取标准输出设备句柄 */
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {           /* 读取当前控制台模式 */
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;  /* 添加 VT 处理标志 */
            SetConsoleMode(hOut, mode);              /* 写回新模式 */
        }
    }
#endif
}

/*
 * ANSI 清屏 — 清除终端所有内容并将光标移至左上角
 *
 * 无参数, 无返回值
 *
 * 转义序列说明:
 *   \033[2J — ED (Erase Display): 清除整个屏幕
 *   \033[H  — CUP (Cursor Position): 将光标定位到 (1,1) 左上角
 *
 * ANSI clear: \033[2J erase display, \033[H cursor home */
void ui_clear_screen(void) {
    printf(C_RESET "\033[2J\033[H");       /* 先重置颜色属性再清屏 */
}

/* ==================  UI 绘制函数 / UI Drawing Functions ================== */

/*
 * 画水平线 — 使用指定字符或默认的 "─" (U+2500)
 *
 * 参数:
 *   width - 线的长度 (字符数), <=0 时默认为 60
 *   ch    - 线的字符 (UTF-8 编码), NULL 或空串时使用默认字符 "─"
 *
 * 使用场景: 分隔不同内容区域, 例如表格表头下方、菜单区块之间
 *
 * Draw horizontal line with specified char or default "─" */
void ui_line(int width, const char *ch) {
    if (width <= 0) width = 60;           /* 默认宽度 60 */
    printf(C_RESET);                      /* 确保颜色属性已重置 */
    for (int i = 0; i < width; i++) {
        printf("%s", ch && ch[0] ? ch : "\xe2\x94\x80");  /* ─ 的 UTF-8 编码 */
    }
    printf("\n");
}

/*
 * 盒子顶部边框 — 双线框顶部, 居中显示标题
 *
 * 参数:
 *   title - 标题字符串 (显示在边框中间位置)
 *
 * 绘制效果示例:
 *   ╔══ 药品库存管理 ═══════════╗
 *
 * 说明:
 *   - 自动调用 ui_menu_cache_init 重置菜单缓存
 *   - 使用 utf8_display_width 计算标题显示宽度以正确居中
 *   - 边框字符: ╔ (U+2554) ═ (U+2550) ╗ (U+2557)
 *   - 总宽度 58 字符, 标题居中放置
 *
 * Box top border: ╔══ title ═══════╗ with centered title. */
void ui_box_top(const char *title) {
    ui_menu_cache_init();                 /* 每次新菜单时重置缓存 / fresh cache for each menu */
    int len = title ? utf8_display_width(title) : 0;
    int total = 58;                       /* 边框总宽度 (双线框) */
    /* 计算标题左右各需要多少 ═ 来填充剩余空间 */
    int left = (total - len) / 2;
    int right = total - len - left;

    printf(C_BOLD C_CYAN);
    printf("\xe2\x95\x94");                             /* ╔ 左上角 */
    for (int i = 0; i < left; i++) printf("\xe2\x95\x90"); /* ═ 水平双线 (左侧) */
    if (title && len > 0) {
        printf(" ");                      /* 标题前留一个空格 */
        printf(C_WHITE);                  /* 标题用白色高亮 */
        printf("%s", title);
        printf(C_BOLD C_CYAN);            /* 恢复青色双线颜色 */
        printf(" ");                      /* 标题后留一个空格 */
    }
    for (int i = 0; i < right; i++) printf("\xe2\x95\x90"); /* ═ 水平双线 (右侧) */
    printf("\xe2\x95\x97");                              /* ╗ 右上角 */
    printf(C_RESET "\n");
    g_line_counter++;                     /* 跟踪此行号, 供原位高亮定位使用 */
}

/*
 * 盒子底部边框 — 双线框底部闭合
 *
 * 绘制效果:
 *   ╚══════════════════════════════╝
 *
 * Box bottom border: ╚══════════════╝ */
void ui_box_bottom(void) {
    printf(C_BOLD C_CYAN);
    printf("\xe2\x95\x9a");                               /* ╚ 左下角 */
    for (int i = 0; i < 58; i++) printf("\xe2\x95\x90"); /* ═ 58 个双线字符 */
    printf("\xe2\x95\x9d");                               /* ╝ 右下角 */
    printf(C_RESET "\n");
    g_line_counter++;
}

/*
 * 主标题 — 带双线装饰的大标题
 *
 * 参数:
 *   title - 标题字符串
 *
 * 绘制效果:
 *
 *   ━━━ 用户管理 ━━━
 *
 * Main header: ═══ title ═══ with double-line decoration */
void ui_header(const char *title) {
    printf("\n");
    printf(C_BOLD C_CYAN);
    printf("  \xe2\x94\x81\xe2\x94\x81\xe2\x94\x81  ");   /* ━━━ 左侧装饰 (U+2501) */
    printf(C_WHITE);
    printf("%s", title);                  /* 标题正文 (白色加粗) */
    printf(C_BOLD C_CYAN);
    printf("  \xe2\x94\x81\xe2\x94\x81\xe2\x94\x81");      /* ━━━ 右侧装饰 */
    printf(C_RESET "\n");
    g_line_counter += 2;                  /* 空行 + 标题行共 2 行 */
}

/*
 * 子标题 — 带青色 ▸ 符号的子标题
 *
 * 参数:
 *   title - 子标题字符串
 *
 * 绘制效果:
 *
 *     ▸ 药品库存列表
 *
 * 青色 ▸ 符号作为视觉引导, 标题正文加粗显示
 *
 * Sub-header: ▸ title with cyan label */
void ui_sub_header(const char *title) {
    printf("\n");
    printf(S_LABEL);                      /* 青色 (标签色) */
    printf("  \xe2\x96\xb8 ");                             /* ▸ 符号 (U+25B8) */
    printf(C_RESET);
    printf(C_BOLD);                       /* 标题正文加粗 */
    printf("%s", title);
    printf(C_RESET "\n");
    g_line_counter += 2;                  /* 空行 + 标题行 */
}

/*
 * 分隔线 — 暗淡样式的水平分隔线
 *
 * 绘制效果:
 *     ──────────────────────────────────────────  (58 个 ─)
 *
 * 使用 C_DIM 暗淡样式, 视觉上作为次要分隔, 不抢夺注意力
 *
 * Divider line: ───────────────── */
void ui_divider(void) {
    printf(C_DIM);                        /* 暗淡样式 (低对比度) */
    printf("  ");
    for (int i = 0; i < 58; i++) printf("\xe2\x94\x80");    /* ─ (U+2500) × 58 */
    printf(C_RESET "\n");
    g_line_counter++;
}

/*
 * 成功消息 — 绿色 ✓ 前缀
 *
 * 参数:
 *   msg - 成功消息文本
 *
 * 绘制效果:
 *     ✓ 操作成功
 *
 * Success message: ✓ msg in green */
void ui_ok(const char *msg) {
    printf(S_SUCCESS);                    /* 绿色 (成功色) */
    printf("  \xe2\x9c\x93 %s", msg);                       /* ✓ (U+2713) + 消息 */
    printf(C_RESET "\n");
}

/*
 * 错误消息 — 红色 ✗ 前缀
 *
 * 参数:
 *   msg - 错误消息文本
 *
 * 绘制效果:
 *     ✗ 操作失败: 用户不存在
 *
 * Error message: ✗ msg in red */
void ui_err(const char *msg) {
    printf(S_ERROR);                      /* 红色 (错误色) */
    printf("  \xe2\x9c\x97 %s", msg);                       /* ✗ (U+2717) + 错误信息 */
    printf(C_RESET "\n");
}

/*
 * 警告消息 — 黄色 ⚠ 前缀
 *
 * 参数:
 *   msg - 警告消息文本
 *
 * 绘制效果:
 *     ⚠ 库存不足
 *
 * Warning message: ⚠ msg in yellow */
void ui_warn(const char *msg) {
    printf(S_WARNING);                    /* 黄色 (警告色) */
    printf("  \xe2\x9a\xa0 %s", msg);                       /* ⚠ (U+26A0) + 警告信息 */
    printf(C_RESET "\n");
}

/*
 * 标签-值信息对 — 用于显示键值对信息 (如患者详情)
 *
 * 参数:
 *   label - 标签文本 (青色显示, 如 "姓名:")
 *   value - 值文本 (白色显示, 可为 NULL 显示为空)
 *
 * 绘制效果 (标签后至少 20 个显示宽度间距):
 *     姓名:              张三
 *     身份证号:          320123199001011234
 *
 * 说明: 使用 utf8_display_width 计算标签的实际显示宽度,
 *       补足空格使值文本从第 20 列开始对齐
 *
 * Info label-value pair: label in cyan, value in white, min 20-width spacing */
void ui_info(const char *label, const char *value) {
    int vw = utf8_display_width(label);   /* 标签的终端显示宽度 */
    int pad = 20 - vw;                    /* 需要的填充空格数 */
    if (pad < 1) pad = 1;                /* 至少保留一个空格间隔 */

    printf("  ");
    printf(S_LABEL);                      /* 青色标签 */
    printf("%s", label);
    printf(C_RESET);
    for (int i = 0; i < pad; i++) printf(" ");  /* 填充空格对齐值列 */
    printf("%s", value ? value : "");      /* 输出值 (NULL 时显示空字符串) */
    printf("\n");
}

/*
 * 用户身份徽章 — 显示当前登录用户信息
 *
 * 参数:
 *   name - 用户名
 *   role - 角色字符串: "admin" / "doctor" / "patient"
 *
 * 绘制效果:
 *     👤 张三  管理员
 *
 * 角色标签使用青色背景高亮, 根据角色显示不同中文标签:
 *   admin   → "管理员"
 *   doctor  → "医生"
 *   patient → "患者"
 *
 * User badge: 👤 username [role] with background-colored tag */
void ui_user_badge(const char *name, const char *role) {
    /* 根据角色字符串确定中文标签文本 */
    const char *role_label = "";
    if (strcmp(role, "admin") == 0) role_label = "\xe7\xae\xa1 \xe7\x90\x86 \xe5\x91\x98";    /* 管理员 */
    else if (strcmp(role, "doctor") == 0) role_label = "\xe5\x8c\xbb    \xe7\x94\x9f";         /* 医  生 */
    else if (strcmp(role, "patient") == 0) role_label = "\xe6\x82\xa3    \xe8\x80\x85";        /* 患  者 */

    printf(C_BOLD C_CYAN);
    printf("  \xf0\x9f\x91\xa4 ");                          /* 👤 图标 (U+1F464, UTF-8 4 字节) */
    printf(C_WHITE);
    printf("%s", name);                   /* 用户名 (白色加粗) */
    printf(C_RESET);
    printf("  ");
    printf(BG_CYAN C_BOLD);               /* 青色背景 + 加粗 */
    printf(" %s ", role_label);           /* 角色标签 (如 "管理员") */
    printf(C_RESET "\n");
    g_line_counter++;                     /* 跟踪此行号 */
}

/*
 * 步骤指示器 — 显示步骤编号和描述 (预留接口)
 *
 * 参数:
 *   step - 步骤编号 (如 1, 2, 3)
 *   desc - 步骤描述文本
 *
 * 绘制效果:
 *     [1] 填写基本信息
 *
 * 注: 当前代码库中未使用此函数, 保留供未来向导式流程使用
 *
 * Step indicator: [step] desc (currently unused, reserved for future wizards) */
void ui_step(int step, const char *desc) {
    printf("  [%d] %s\n", step, desc);
}

/* ==================  表格列打印 (显示宽度对齐) / Table Column Printing ================== */

/*
 * 打印字符串列 — 按显示宽度填充空格对齐
 *
 * 参数:
 *   s     - 待打印的字符串
 *   width - 目标列宽 (显示宽度单位, 非字节数)
 *
 * 行为:
 *   1. 先输出字符串 s
 *   2. 使用 utf8_display_width(s) 计算已占用的终端列数
 *   3. 填充 width - 已占用列数 个空格
 *   4. 最后额外输出一个空格作为列间分隔符
 *
 * 用途: 配合 ui_print_col_int / ui_print_col_float 构建对齐的表格输出
 *
 * Print string column: output s, pad with spaces to display width */
void ui_print_col(const char *s, int width) {
    int w = utf8_display_width(s);        /* 实际显示宽度 */
    printf("%s", s);
    for (int i = w; i < width; i++) printf(" ");  /* 补空格到目标宽度 */
    printf(" ");                          /* 列间分隔空格 */
}

/*
 * 打印整数列 — 将整数格式化为字符串后委托 ui_print_col 对齐输出
 *
 * 参数:
 *   val   - 整数值
 *   width - 目标列宽 (显示宽度)
 *
 * Print integer column */
void ui_print_col_int(int val, int width) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    ui_print_col(buf, width);             /* 委托字符串列打印 */
}

/*
 * 打印浮点数列 (保留 2 位小数) — 委托 ui_print_col
 *
 * 参数:
 *   val   - 浮点数值 (如药品价格)
 *   width - 目标列宽
 *
 * 格式: 保留 2 位小数 (如 "12.50")
 *
 * Print float column (2 decimal places) */
void ui_print_col_float(float val, int width) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", val);
    ui_print_col(buf, width);
}

/* ==================  字符串不区分大小写搜索 / Case-Insensitive Substring Search ================== */

/*
 * 不区分大小写的子串匹配 — 朴素搜索算法 (O(n*m))
 *
 * 参数:
 *   haystack - 被搜索的字符串 (干草堆)
 *   needle   - 要搜索的子串 (针)
 *
 * 返回: 1=找到匹配, 0=未找到
 *
 * 算法:
 *   对 haystack 中每个字符位置, 尝试从该位置开始与 needle 逐字符比较
 *   比较时手动将大写字母 (A-Z) 转为小写 (a-z) 以实现不区分大小写
 *   找到完整匹配返回 1, 遍历完所有位置仍未找到返回 0
 *
 * 注意: 此函数的复杂度为 O(n*m), 对于大量数据的搜索建议使用
 *       ui_search_list 的缓存过滤机制减少调用次数
 *
 * Case-insensitive substring match using naive O(n*m) algorithm */
static int str_contains_icase(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return 0;  /* 参数无效或 needle 为空 */
    const char *h = haystack;
    while (*h) {
        const char *n = needle;
        const char *start = h;            /* 匹配起点 (每次 h 前进, start 从 h 开始) */
        while (*n && *start) {
            char hc = *start, nc = *n;
            /* 手动转小写比较: A-Z → a-z, 其他字符不变 */
            if (hc >= 'A' && hc <= 'Z') hc += 32;    /* 大写转小写 (ASCII: +32) */
            if (nc >= 'A' && nc <= 'Z') nc += 32;
            if (hc != nc) break;          /* 某字符不匹配, 退出内循环尝试下一位置 */
            start++; n++;                 /* 匹配成功, 继续比较下一个字符 */
        }
        if (!*n) return 1;                /* needle 已全部匹配 / full match found */
        h++;                              /* 滑动到下一位置继续搜索 */
    }
    return 0;
}

/* ==================  智能输入 / Smart Input ================== */

/*
 * 智能 ID 查找 — 提供选择或直接输入的灵活输入方式
 *
 * 参数:
 *   prompt    - 输入提示文本 (如 "请选择药品")
 *   id_list   - 候选 ID 字符串数组 (可为空)
 *   count     - 候选数组长度
 *   output    - 输出缓冲区 (存放最终选中的 ID 或用户直接输入的文本)
 *   out_size  - 输出缓冲区大小
 *
 * 返回: 1=成功选择了/输入了一个 ID, -1=用户取消操作
 *
 * 行为分两种:
 *   - count <= 0 (无候选): 直接显示提示 + 让用户自由输入
 *   - count > 0 (有候选): 调用 ui_search_list 进入交互式搜索选择
 *
 * Smart ID lookup: direct input if list empty, otherwise interactive search */
int smart_id_lookup(const char *prompt, const char *id_list[], int count, char *output, int out_size) {
    if (count <= 0) {
        /* 无候选 → 直接自由输入 / No candidates → direct free input */
        char input[64];
        printf(S_LABEL "  %s: " C_RESET, prompt);
        if (read_input_line(input, sizeof(input)) == NULL) return -1;
        if (input[0] == 0) return -1;     /* 空输入视为取消 */
        strncpy(output, input, out_size - 1);
        output[out_size - 1] = 0;         /* 确保字符串以 '\0' 结尾 */
        return 1;
    }

    /* 有候选 → 交互式搜索选择 / Has candidates → interactive search selection */
    int idx = ui_search_list(prompt, id_list, count);
    if (idx < 0) return -1;               /* 用户取消 */
    strncpy(output, id_list[idx], out_size - 1);
    output[out_size - 1] = 0;
    return 1;
}

/*
 * 智能患者输入 — 收集与指定医生关联的患者 ID 进行交互式选择
 *
 * 参数:
 *   doctor_id - 医生 ID, 用于过滤关联患者
 *   prompt    - 提示文本
 *   output    - 输出缓冲区 (存放选中的患者 ID)
 *   out_size  - 输出缓冲区大小
 *
 * 返回: 1=成功选择, -1=用户取消
 *
 * 数据来源 (两处合并, 自动去重):
 *   1. 预约挂号记录 (appointments.txt): 匹配 doctor_id, 收集 patient_id
 *   2. 现场挂号队列 (onsite_queue.txt): 匹配 doctor_id, 收集 patient_id
 *
 * 去重策略: 遍历已有候选数组, 若发现相同 patient_id 则跳过
 *
 * Smart patient input: collect patient IDs linked to doctor (from appointments
 * and onsite registrations, deduplicated), then interactive selection. */
int smart_patient_input(const char *doctor_id, const char *prompt, char *output, int out_size) {
    const char *candidates[500];          /* 候选患者 ID 数组 (最多 500) */
    int count = 0;

    /* 从预约记录收集 (限制最多 150 个来自预约) / Collect from appointments */
    {
        AppointmentNode *ah = load_appointments_list();
        AppointmentNode *ac = ah;
        while (ac && count < 150) {
            if (strcmp(ac->data.doctor_id, doctor_id) == 0) {
                /* 去重检查: 检查当前 candidate 是否已在数组中 / Deduplication check */
                int dup = 0;
                int j;
                for (j = 0; j < count; j++)
                    if (strcmp(candidates[j], ac->data.patient_id) == 0) { dup = 1; break; }
                if (!dup) candidates[count++] = ac->data.patient_id;
            }
            ac = ac->next;
        }
        free_appointment_list(ah);
    }
    /* 从现场挂号收集 (限制总候选数 250) / Collect from onsite registrations */
    {
        OnsiteRegistrationQueue oq = load_onsite_registration_queue();
        OnsiteRegistrationNode *oc = oq.front;
        while (oc && count < 250) {
            if (strcmp(oc->data.doctor_id, doctor_id) == 0) {
                /* 去重检查 / Deduplication check */
                int dup = 0;
                int j;
                for (j = 0; j < count; j++)
                    if (strcmp(candidates[j], oc->data.patient_id) == 0) { dup = 1; break; }
                if (!dup) candidates[count++] = oc->data.patient_id;
            }
            oc = oc->next;
        }
        free_onsite_registration_queue(&oq);
    }

    return smart_id_lookup(prompt, candidates, count, output, out_size);
}

/*
 * 智能药品输入 — 加载所有药品构建 "ID (名称)" 列表后交互式选择
 *
 * 参数:
 *   prompt   - 提示文本 (如 "请选择药品")
 *   output   - 输出缓冲区 (存放选中的药品 ID)
 *   out_size - 输出缓冲区大小
 *
 * 返回: 1=成功选择, -1=用户取消
 *
 * 算法:
 *   1. 从文件加载所有药品链表
 *   2. 构建 "drug_id (药品名称)" 格式的候选列表
 *   3. 调用 smart_id_lookup → ui_search_list 进行交互式选择
 *   4. 用户选择后, 从输出中截取纯药品 ID:
 *      在第一个空格处截断 (去掉 " (名称)" 后缀)
 *
 * Smart drug input: load all drugs → "ID (name)" list → interactive selector.
 * Extracts drug ID from selection (strips parenthesized name). */
int smart_drug_input(const char *prompt, char *output, int out_size) {
    DrugNode *dh = load_drugs_list();
    DrugNode *dc = dh;
    char *drug_list[300];                 /* 候选药品显示文本指针数组 */
    char  drug_buf[300][MAX_ID + MAX_NAME + 4];  /* 候选药品显示文本缓冲区 */
    int count = 0;

    /* 构建 "药物ID (药品名称)" 候选列表 / Build "ID (name)" candidate list */
    while (dc && count < 200) {
        snprintf(drug_buf[count], sizeof(drug_buf[0]), "%s (%s)", dc->data.drug_id, dc->data.name);
        drug_list[count] = drug_buf[count];
        count++;
        dc = dc->next;
    }
    free_drug_list(dh);

    int result = smart_id_lookup(prompt, (const char **)drug_list, count, output, out_size);

    /* 提取纯药品 ID — 在第一个空格处截断, 去掉 " (名称)" 尾部
       例如: 用户选择了 "D001 (阿莫西林)" → 提取 "D001"
       Extract ID: truncate at first space to remove parenthesized name */
    if (result == 1) {
        char *paren = strchr(output, ' ');
        if (paren) *paren = 0;            /* 在空格位置截断字符串 */
    }
    return result;
}

/* ==================  模板快捷输入 / Template Quick Input ================== */

/*
 * 模板快捷输入 — 支持直接输入、#n 模板引用、数字选择和查看模板列表
 *
 * 参数:
 *   category - 模板类别 (如 "诊断"、"处方" 等, 对应 templates.txt 中的 category 字段)
 *   prompt   - 输入提示文本
 *   output   - 输出缓冲区 (存放用户最终选择的文本)
 *   out_size - 输出缓冲区大小
 *
 * 返回: 1=有输入 (直接文本或模板内容), -1=用户取消
 *
 * 支持的四种输入方式:
 *   1. 直接输入文本 — 用户直接敲入任意文本, 回车确认
 *   2. #n 模板语法 — 输入 "#3" 引用第 3 个模板的内容
 *   3. 数字快捷键 — 输入纯数字 "3" 也引用第 3 个模板
 *   4. v 命令 — 显示该类别所有模板列表, 供用户浏览后选择
 *
 * 模板来源: 从 templates.txt 文件加载, 按 category 字段过滤
 *
 * Quick template input: supports direct text, #n syntax for template reference,
 * numeric selection, and v for viewing all templates. */
int quick_template_input(const char *category, const char *prompt, char *output, int out_size) {
    TemplateNode *head = load_templates_list();
    TemplateNode *cur = head;

    const char *tlist[200];               /* 模板显示文本数组 (供列表展示用) */
    char ttext[200][600];                 /* 模板显示文本缓冲区 */
    int tcount = 0;

    /* 收集指定类别的模板 — 按 category 字段过滤 / Collect templates of the given category */
    while (cur && tcount < 100) {
        if (strcmp(cur->data.category, category) == 0) {
            /* 格式化: "快捷码 | 模板内容" / Format: "shortcut | text" */
            snprintf(ttext[tcount], sizeof(ttext[0]), "%s | %s",
                     cur->data.shortcut, cur->data.text);
            tlist[tcount] = ttext[tcount];
            tcount++;
        }
        cur = cur->next;
    }
    free_template_list(head);

    /* 无模板 → 直接自由输入模式 / No templates → direct free input */
    if (tcount == 0) {
        printf(S_LABEL "  %s" C_RESET " (直接输入): ", prompt);
        if (read_input_line(output, out_size) == NULL) return -1;
        if (output[0] == 0) return -1;
        return 1;
    }

    /* 显示提示信息和操作方法 / Show prompt and usage instructions */
    printf("\n" S_LABEL "  %s" C_RESET "\n", prompt);
    printf(C_DIM "  [直接输入] / [#n=模板] / [v=查看全部模板]" C_RESET "\n");

    if (read_input_line(output, out_size) == NULL) return -1;

    if (output[0] == 0) return -1;        /* 空输入 → 取消 */

    /* v 命令 — 显示该类别所有模板列表, 然后重新请求输入
       v command: show template list, then re-input */
    if (strcmp(output, "v") == 0 || strcmp(output, "V") == 0) {
        printf("\n" S_LABEL "  %s 模板列表:" C_RESET "\n", category);
        ui_divider();
        int i;
        for (i = 0; i < tcount; i++) {
            printf("  " C_BOLD C_YELLOW "%2d." C_RESET " %s\n", i + 1, tlist[i]);
        }
        printf(S_LABEL "  请输入内容或选序号: " C_RESET);
        if (read_input_line(output, out_size) == NULL) return -1;
        if (output[0] == 0) return -1;
    }

    /* #n 模板语法 — 如输入 "#3" → 引用第 3 个模板的内容
       验证: 首字符为 '#' 且后续字符全部为数字
       #n template syntax: e.g. "#3" → reference 3rd template */
    if (output[0] == '#') {
        char *num_str = output + 1;       /* 跳过 '#' 获取数字部分 */
        int is_digits = 1;
        for (char *p = num_str; *p; p++) {
            if (*p < '0' || *p > '9') { is_digits = 0; break; }
        }
        if (is_digits && num_str[0] != '\0') {
            int idx = atoi(num_str) - 1;  /* 转换为 0-based 索引 */
            if (idx >= 0 && idx < tcount) {
                /* 重新加载模板链表, 按类别过滤后取第 idx 个 */
                TemplateNode *th = load_templates_list();
                TemplateNode *tc = th;
                int cnt = 0;
                while (tc) {
                    if (strcmp(tc->data.category, category) == 0) {
                        if (cnt == idx) {
                            strncpy(output, tc->data.text, out_size - 1);
                            output[out_size - 1] = '\0';
                            free_template_list(th);
                            return 1;
                        }
                        cnt++;
                    }
                    tc = tc->next;
                }
                free_template_list(th);
            }
        }
        return 1;
    }

    /* 纯数字输入 — 如输入 "3" → 引用第 3 个模板
       验证: 输入的全部字符都是数字
       Pure numeric: e.g. "3" → reference 3rd template */
    {
        int is_all_digits = 1;
        char *p = output;
        while (*p) { if (*p < '0' || *p > '9') { is_all_digits = 0; break; } p++; }
        if (is_all_digits && output[0]) {
            int idx = atoi(output) - 1;
            if (idx >= 0 && idx < tcount) {
                /* 重新加载模板链表, 按类别过滤后取第 idx 个 */
                TemplateNode *th = load_templates_list();
                TemplateNode *tc = th;
                int cnt = 0;
                while (tc) {
                    if (strcmp(tc->data.category, category) == 0) {
                        if (cnt == idx) {
                            strncpy(output, tc->data.text, out_size - 1);
                            output[out_size - 1] = 0;
                            free_template_list(th);
                            return 1;
                        }
                        cnt++;
                    }
                    tc = tc->next;
                }
                free_template_list(th);
            }
        }
    }

    return 1;                             /* 直接输入的文本已存在于 output 中 */
}

/* ==================  菜单项缓存 / Menu Item Cache ================== */

/*
 * 菜单缓存数据结构 — 供 get_menu_choice (common.c) 使用
 *
 * 这些静态数组存储当前页面的菜单项信息:
 *   g_menu_nums[]  — 菜单项编号 (如 1, 2, 3 ...)
 *   g_menu_texts[] — 菜单项文本 (每项最多 59 字符 + '\0')
 *   g_menu_exit[]  — 是否为退出项 (退出项用暗淡样式显示, 如 "返回上级")
 *   g_menu_count   — 当前菜单项总数
 *
 * Menu cache data structures used by get_menu_choice in common.c */
static int   g_menu_nums[MAX_MENU_ITEMS];
static char  g_menu_texts[MAX_MENU_ITEMS][60];
static bool  g_menu_exit[MAX_MENU_ITEMS];
static int   g_menu_count = 0;

/*
 * 初始化菜单缓存 — 每次进入新菜单前必须调用
 *
 * 行为: 将 g_menu_count 和 g_line_counter 重置为 0,
 *       确保下一个菜单从干净的状态开始
 *
 * 调用时机: ui_box_top 内部自动调用, 也可手动调用
 *
 * Reset cache before each new menu */
void ui_menu_cache_init(void) {
    g_menu_count = 0;
    g_line_counter = 0;
}

/*
 * 填充缓存数组并固化行跟踪快照
 *
 * 参数:
 *   nums    - 输出数组: 菜单项编号
 *   texts   - 输出数组: 菜单项文本 (每项 60 字节)
 *   is_exit - 输出数组: 是否为退出项
 *   max     - 输出数组的最大容量
 *
 * 返回: 实际填充的菜单项数量
 *
 * 操作步骤:
 *   1. 将 g_menu_nums/texts/exit 复制到输出参数数组中
 *   2. 固化行跟踪快照: 保存 g_line_counter → g_saved_total,
 *      保存 g_item_offsets → g_saved_offsets
 *   3. 调用 ui_menu_cache_init 重置所有缓存为下一个菜单做准备
 *
 * Fill cache arrays for get_menu_choice to read, snapshot line tracking state,
 * then reset cache for next menu. */
int ui_menu_cache_fill(int *nums, char texts[][60], bool *is_exit, int max) {
    int n = (g_menu_count < max) ? g_menu_count : max;  /* 不超过输出容量 */
    /* 复制菜单项数据 / Copy menu item data to output arrays */
    for (int i = 0; i < n; i++) {
        nums[i] = g_menu_nums[i];
        strncpy(texts[i], g_menu_texts[i], 59);
        texts[i][59] = 0;                 /* 确保以 '\0' 结尾 */
        is_exit[i] = g_menu_exit[i];
    }

    /* 固化行跟踪快照 (在重置前保存, 因为重置会清零)
       Save line tracking snapshot before reset */
    g_saved_total = g_line_counter;
    for (int i = 0; i < n && i < MAX_MENU_ITEMS; i++) {
        g_saved_offsets[i] = g_item_offsets[i];
    }
    g_saved_count = n;

    ui_menu_cache_init();                 /* 为下一个菜单重置 / reset for next menu */
    return n;
}

/*
 * 输出普通菜单项 — 带黄色编号的菜单条目
 *
 * 参数:
 *   num  - 菜单项编号 (1-based, 如 1, 2, 3)
 *   text - 菜单项文本 (如 "患者管理")
 *
 * 行为:
 *   1. 将菜单项信息缓存到 g_menu_* 静态数组中
 *   2. 记录当前 g_line_counter 到 g_item_offsets (供原位高亮定位)
 *   3. 输出格式: "  1. 患者管理" (编号黄色加粗, 文本默认色)
 *   4. 递增 g_line_counter
 *
 * Output normal menu item: cache num+text+line, render as "1. text" with yellow number */
void ui_menu_item(int num, const char *text) {
    if (g_menu_count < MAX_MENU_ITEMS) {
        g_item_offsets[g_menu_count] = g_line_counter;  /* 记录该菜单项所在的行号 */
        g_menu_nums[g_menu_count] = num;
        strncpy(g_menu_texts[g_menu_count], text, 59);
        g_menu_texts[g_menu_count][59] = 0;
        g_menu_exit[g_menu_count] = false; /* 普通菜单项 */
        g_menu_count++;
    }
    printf("  ");
    printf(C_BOLD C_YELLOW);
    printf("%d.", num);                   /* 黄色加粗编号 */
    printf(C_RESET);
    printf(" %s\n", text);                /* 菜单文本 */
    g_line_counter++;
}

/*
 * 输出退出菜单项 — 带退出样式 (暗淡) 的菜单条目
 *
 * 参数:
 *   num  - 菜单项编号 (通常为最大编号, 如 "0")
 *   text - 菜单项文本 (如 "退出系统"、"返回上级菜单")
 *
 * 行为: 与 ui_menu_item 类似, 但两个区别:
 *   - g_menu_exit 标记为 true (get_menu_choice 据此判断是否退出)
 *   - 使用 C_DIM (暗淡/灰色) 样式输出, 从视觉上与普通选项区分
 *
 * Output exit menu item: dimmed style to distinguish from normal items */
void ui_menu_exit(int num, const char *text) {
    if (g_menu_count < MAX_MENU_ITEMS) {
        g_item_offsets[g_menu_count] = g_line_counter;
        g_menu_nums[g_menu_count] = num;
        strncpy(g_menu_texts[g_menu_count], text, 59);
        g_menu_texts[g_menu_count][59] = 0;
        g_menu_exit[g_menu_count] = true; /* 标记为退出项 */
        g_menu_count++;
    }
    printf("  ");
    printf(C_DIM);                        /* 暗淡/灰色样式 */
    printf("%d.", num);
    printf(" %s", text);
    printf(C_RESET "\n");
    g_line_counter++;
}

/* ==================  确认对话框 / Confirmation Dialog ================== */

/*
 * 模态确认对话框 — 向用户确认危险/不可逆操作
 *
 * 参数:
 *   prompt - 确认提示文本 (如 "确认删除该记录？")
 *
 * 返回: true=用户确认执行, false=用户取消
 *
 * 行为:
 *   1. 输出确认提示: "  ? 确认执行 [Y/n]: "
 *   2. 读取用户单次按键 (getchar, 无回显)
 *   3. 判断: 回车键 / 'y' / 'Y' → 返回 true (确认)
 *           其他按键 (包括 'n' / 'N') → 返回 false (取消)
 *   4. 清空输入缓冲区 (防止残留字符干扰后续输入)
 *
 * Modal confirm: show "? prompt [Y/n]", Enter or Y confirms, n cancels */
bool ui_confirm(const char *prompt) {
    printf("\n" S_WARNING "  ? %s" C_RESET " [" C_BOLD C_GREEN "Y" C_RESET "/" C_DIM "n" C_RESET "]: ",
           prompt ? prompt : "确认执行");
    fflush(stdout);                       /* 确保提示立即刷新到屏幕 */
    int c = getchar();
    /* 清空输入缓冲区: 用户可能输入了多个字符, 丢弃多余的 */
    if (c != '\n' && c != '\r') clear_input_buffer();
    return (c == '\n' || c == '\r' || c == 'y' || c == 'Y');
}

/* ==================  行跟踪接口 / Line Tracking Interface ================== */

/*
 * 手动增加一行计数 — 用于非菜单项的输出
 *
 * 用途: 当在菜单项之间插入了非菜单内容的输出 (如空行、提示信息等),
 *       需要手动调用此函数, 以保持行号跟踪的准确性,
 *       确保 get_menu_choice 的高亮光标移动距离计算正确
 *
 * Increment line counter for non-menu-item output (e.g. blank lines) */
void ui_menu_track_line(void) {
    g_line_counter++;
}

/*
 * 获取固化的总行数 — 供 get_menu_choice 使用
 *
 * 返回: 最后一个菜单段落的固化总行数
 *
 * Get saved total line count */
int ui_menu_get_saved_total(void) {
    return g_saved_total;
}

/*
 * 获取指定索引菜单项的固化行偏移 — 供 get_menu_choice 原位高亮定位
 *
 * 参数:
 *   idx - 菜单项索引 (0-based)
 *
 * 返回: 该项的行偏移量, 索引无效时返回 0
 *
 * Get saved offset for item at index */
int ui_menu_get_item_offset(int idx) {
    if (idx >= 0 && idx < g_saved_count) return g_saved_offsets[idx];
    return 0;
}

/* ==================  分页显示 / Pagination ================== */

/*
 * 分页显示列表 — 支持翻页、跳页、搜索、退出
 *
 * 参数:
 *   items     - 字符串数组 (每项为一个列表条目)
 *   count     - 数组长度 (列表项总数)
 *   page_size - 每页显示条数 (<=0 时默认 15)
 *   title     - 列表标题 (可为 NULL)
 *
 * 支持的操作 (单键触发):
 *   n / Enter - 下一页
 *   p         - 上一页
 *   g         - 跳转到指定页 (输入页码)
 *   s         - 搜索过滤 (不区分大小写子串匹配, 结果在新分页中显示)
 *   q         - 退出分页
 *
 * 搜索行为: 对全部 items 执行 str_contains_icase 匹配,
 *           匹配结果收集到 filtered 数组, 递归调用 ui_paginate 展示
 *
 * Paginate a list: next/prev page (n/p), goto page (g), search (s), quit (q).
 * Search does case-insensitive substring matching across items. */
void ui_paginate(const char **items, int count, int page_size, const char *title) {
    if (count <= 0) {
        ui_warn("暂无数据。");
        return;
    }

    if (page_size <= 0) page_size = 15;   /* 默认每页 15 条 / default 15 per page */

    /* 计算总页数 (向上取整) / Calculate total pages (ceiling division) */
    int total_pages = (count + page_size - 1) / page_size;
    int page = 0;                         /* 当前页索引 (0-based) */

    while (page < total_pages) {
        /* 显示标题 (含总数统计) / Show title with total count */
        if (title) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s (共%d条)", title, count);
            ui_sub_header(buf);
        }

        /* 计算当前页的起止索引 / Calculate start/end index for current page */
        int start = page * page_size;
        int end = start + page_size;
        if (end > count) end = count;     /* 最后一页可能不满一页 */

        /* 输出当前页所有条目 (带黄色编号) / Output items with yellow numbering */
        for (int i = start; i < end; i++) {
            printf("  " C_BOLD C_YELLOW "%d." C_RESET " %s\n", i + 1, items[i]);
        }

        /* 多页时显示导航栏 / Show navigation bar (only if multiple pages) */
        if (total_pages > 1) {
            printf(C_DIM "  ─── 第 %d/%d 页", page + 1, total_pages);
            printf("  n下一页 p上一页 g跳页 q返回 s搜索" C_RESET "\n");

            int c = getchar();
            if (c != '\n') clear_input_buffer();  /* 非回车键时清空缓冲区 */

            /* === 命令分派 / Command dispatch === */

            if (c == 'q' || c == 'Q') break;      /* 退出分页 */

            if (c == 's' || c == 'S') {
                /* ===== 搜索子流程 / Search sub-flow ===== */
                char keyword[64];
                printf(S_LABEL "  输入关键字: " C_RESET);
                if (read_input_line(keyword, sizeof(keyword)) == NULL) continue;
                if (keyword[0] == '\0') continue;  /* 空关键字 → 跳过搜索 */

                /* 遍历所有条目执行不区分大小写子串搜索 */
                int match_idx[count];             /* 匹配项的索引数组 */
                int match_cnt = 0;
                for (int i = 0; i < count; i++) {
                    if (str_contains_icase(items[i], keyword)) {
                        match_idx[match_cnt++] = i;
                    }
                }

                if (match_cnt == 0) {
                    ui_warn("未匹配到结果。");
                    continue;             /* 无匹配结果, 继续当前页 */
                }

                /* 构建过滤后的列表, 递归进入分页显示 / Build filtered list and recurse */
                const char *filtered[match_cnt];
                for (int i = 0; i < match_cnt; i++) {
                    filtered[i] = items[match_idx[i]];
                }

                char search_title[128];
                if (title) {
                    snprintf(search_title, sizeof(search_title), "%s 搜索结果", title);
                } else {
                    snprintf(search_title, sizeof(search_title), "搜索结果");
                }
                ui_paginate(filtered, match_cnt, page_size, search_title);
                continue;
            }

            /* 上一页 / Previous page */
            if (c == 'p' || c == 'P') {
                if (page > 0) { page -= 2; continue; }  /* -2 补偿后面 page++ */
                printf(C_DIM "  已是第一页\n" C_RESET);
                continue;
            }

            /* 跳页 / Goto page */
            if (c == 'g' || c == 'G') {
                printf(S_LABEL "  输入页码 (1-%d): " C_RESET, total_pages);
                char pg_buf[16];
                if (read_input_line(pg_buf, sizeof(pg_buf)) && pg_buf[0]) {
                    int goto_page = atoi(pg_buf) - 1;    /* 转为 0-based */
                    if (goto_page >= 0 && goto_page < total_pages) { page = goto_page - 1; }
                    else { printf(C_DIM "  无效页码\n" C_RESET); continue; }
                } else { continue; }
                continue;
            }

            /* 下一页 / Next page (Enter 或 n) */
            if (c == '\n' || c == 'n' || c == 'N') {
                if (page >= total_pages - 1) break;       /* 已是最后一页 */
            } else if (c != '\n' && c != 'n' && c != 'N') {
                if (page >= total_pages - 1) break;
            }
        }

        page++;                           /* 前进到下一页 */
    }
}

/* ==================  搜索列表选择 / Search List Selection ================== */

/*
 * 搜索并选择列表项 — 支持实时搜索过滤 + 方向键导航原位高亮
 *
 * 参数:
 *   prompt - 提示文本 (如 "请选择药品")
 *   items  - 候选字符串数组
 *   count  - 候选数组长度
 *
 * 返回: 选中项的 0-based 索引, -1 表示用户取消
 *
 * 交互方式:
 *   - 显示前 20 条候选项 (带黄色编号)
 *   - ↑↓ 方向键: 移动高亮 (原位重绘, 不刷新整个列表)
 *   - 1-9 数字键: 直接选择对应编号的项
 *   - s 键: 进入搜索过滤模式 (不区分大小写子串匹配)
 *           唯一匹配时自动选中, 无需用户再确认
 *   - Enter: 确认当前高亮项
 *   - q 键: 取消选择
 *
 * 原位高亮实现原理:
 *   使用 ANSI DECSC (\0337) 保存光标位置 → CUU (\033[nA) 向上移动
 *   → EL (\033[K) 清除当前行 → 重绘文本 (高亮或取消高亮)
 *   → DECRC (\0338) 恢复光标位置
 *   这样终端看起来就是"原地"改变了某行的颜色, 而非刷屏
 *
 * Search and select from list: arrow-key highlight, search filter, Enter to select.
 * Uses ANSI cursor in-place highlight with DECSC/DECRC sequences. */
int ui_search_list(const char *prompt, const char **items, int count) {
    if (count <= 0) {
        ui_warn("暂无数据。");
        return -1;
    }

    int matches[500];                     /* 当前匹配的索引数组 (最多 500 项) */
    int mcount = count > 500 ? 500 : count;
    for (int i = 0; i < mcount; i++) matches[i] = i;  /* 初始匹配全部 / initially match all */

    char input[64];                       /* 搜索关键字缓冲区 */

    /* 外层循环: 每次搜索过滤后重新绘制列表 */
    while (1) {
        /* 计算显示数量: 最多 20 项 / Calculate visible count: max 20 */
        int show_count = mcount > 20 ? 20 : mcount;
        int more_lines = mcount > 20 ? 1 : 0;  /* 是否有更多未显示的项 */

        /* 输出提示和分隔线 / Show prompt and divider */
        printf("\n" S_LABEL "  %s:" C_RESET "\n", prompt ? prompt : "请选择");
        ui_divider();

        /* 输出候选项 (黄色编号 + 文本) / Show candidate items */
        for (int i = 0; i < show_count; i++) {
            printf("  " C_BOLD C_YELLOW "%2d." C_RESET " %s\n", i + 1, items[matches[i]]);
        }
        if (more_lines) {
            printf(C_DIM "  ... 还有 %d 项" C_RESET "\n", mcount - 20);
        }

        /* 操作提示 / Operation hints */
        printf(C_DIM "  ↑↓选择 回车确认 s搜索 q取消" C_RESET "\n");
        fflush(stdout);

        int highlight = 0;                /* 当前高亮项索引 (0-based) */
        int prev_highlight = -1;          /* 上一次高亮项索引 (用于取消旧高亮) */

        /* 内层循环: 处理方向键 / 搜索 / 快捷键 */
        while (1) {
            int ch = _getch();            /* 无回显读取按键 (conio.h) */

            /* ---- 方向键处理 (双字节序列: 0xE0/0x00 + 方向码) ---- */
            if (ch == 0xE0 || ch == 0x00) {
                ch = _getch();            /* 读取方向键第二字节 */
                if (ch == 72)      { highlight--; if (highlight < 0) highlight = show_count - 1; }
                else if (ch == 80) { highlight++; if (highlight >= show_count) highlight = 0; }
                else continue;            /* 非方向键 → 忽略 */
            }
            /* ---- 方向键处理 (VT 终端三字节序列: ESC [ A/B) ---- */
            else if (ch == 0x1B) {        /* ESC (0x1B) */
                ch = _getch();
                if (ch == '[') {          /* CSI 起始符 */
                    ch = _getch();
                    if (ch == 'A')      { highlight--; if (highlight < 0) highlight = show_count - 1; }
                    else if (ch == 'B') { highlight++; if (highlight >= show_count) highlight = 0; }
                    else continue;
                } else continue;
            }
            /* ---- 回车确认 / Enter to confirm ---- */
            else if (ch == '\r' || ch == '\n') {
                printf("\r\033[K\n");     /* 清除当前行并换行 */
                return matches[highlight];
            }
            /* ---- s 搜索过滤 / Search filter ---- */
            else if (ch == 's' || ch == 'S') {
                printf("\r\033[K");       /* 清除当前行 */
                printf(S_LABEL "  搜索: " C_RESET);
                fflush(stdout);
                if (read_input_line(input, sizeof(input)) == NULL) return -1;

                if (input[0] == '\0') {
                    /* 空搜索 → 复位显示全部匹配 / Reset to show all matches */
                    mcount = count > 500 ? 500 : count;
                    for (int i = 0; i < mcount; i++) matches[i] = i;
                    break;                /* 跳出内层循环, 重新绘制列表 */
                }

                /* 执行搜索过滤 / Execute search filter */
                int new_matches[500];
                int new_mcount = 0;
                for (int i = 0; i < count && new_mcount < 500; i++) {
                    if (str_contains_icase(items[i], input)) {
                        new_matches[new_mcount++] = i;
                    }
                }

                if (new_mcount == 0) {
                    ui_err("未匹配到任何结果。");
                    break;                /* 无结果, 重新绘制原列表 */
                }

                mcount = new_mcount;
                memcpy(matches, new_matches, mcount * sizeof(int));
                /* 唯一匹配 → 自动选中, 无需用户再操作
                   Single match → auto-select, skip user confirmation */
                if (mcount == 1) {
                    printf("\r\033[K");
                    printf(S_SUCCESS "  %s" C_RESET "\n", items[matches[0]]);
                    return matches[0];
                }
                break;                    /* 重新绘制过滤后的列表 */
            }
            /* ---- q 取消 / q to cancel ---- */
            else if (ch == 'q' || ch == 'Q') {
                printf("\r\033[K");
                return -1;
            }
            /* ---- 数字快捷键 (1-9) / Digit shortcut ---- */
            else if (ch >= '1' && ch <= '9') {
                int n = ch - '0';         /* 字符转数字 (1-9) */
                if (n >= 1 && n <= show_count) {
                    printf("\n");
                    return matches[n - 1];  /* 转为 0-based 索引 */
                }
                continue;
            }
            else {
                continue;                 /* 其他按键忽略 */
            }

            /* 高亮未变化则跳过重绘 (性能优化) */
            if (highlight == prev_highlight) continue;

            /* ===== 原位高亮切换 / In-place highlight swap ===== */

            /* 计算光标到目标行的距离 (光标在导航栏这行)
               dist: 从光标位置向上数几行到目标项 */
            int dist = show_count + more_lines - highlight;

            /* 取消旧项高亮: 保存光标 → 上移 → 清行 → 重绘 (普通样式) → 恢复光标
               Un-highlight previous item: save cursor → move up → clear → redraw → restore */
            if (prev_highlight >= 0) {
                int old_dist = show_count + more_lines - prev_highlight;
                printf("\0337");                             /* DECSC — 保存光标位置 / save cursor */
                printf("\033[%dA", old_dist);                /* CUU — 向上移动 old_dist 行 / move up */
                printf("\r\033[K");                          /* CR + EL — 清除整行 / clear line */
                printf("  " C_BOLD C_YELLOW "%2d." C_RESET " %s\n",
                       prev_highlight + 1, items[matches[prev_highlight]]);
                printf("\0338");                             /* DECRC — 恢复光标位置 / restore cursor */
            }

            /* 高亮新项: 保存光标 → 上移 → 清行 → 高亮重绘 → 恢复光标
               Highlight new item: save → move up → clear → highlight → restore */
            printf("\0337");                                 /* 保存光标 */
            printf("\033[%dA", dist);                        /* 上移 dist 行 */
            printf("\r\033[K");                              /* 清行 */
            printf("  " BG_CYAN C_WHITE C_BOLD "%2d. %s" C_RESET "\n",
                   highlight + 1, items[matches[highlight]]);
            printf("\0338");                                 /* 恢复光标 */
            fflush(stdout);                                  /* 确保立即渲染, 减少闪烁 */

            prev_highlight = highlight;    /* 更新上一个高亮索引供下次取消 */
        }
    }
}

/* ==================  普通列表选择 / Simple List Selection ================== */

/*
 * 普通列表选择 (无搜索过滤) — 方向键导航高亮, 回车确认选择
 *
 * 参数:
 *   prompt - 提示文本 (如 "请选择科室")
 *   items  - 候选字符串数组
 *   count  - 候选数组长度
 *
 * 返回: 选中项的 0-based 索引, -1 表示用户取消
 *
 * 实现:
 *   复用 ui_menu_item 的缓存和行跟踪机制,
 *   然后委托 get_menu_choice (common.c) 处理方向键导航
 *   最多显示 MAX_MENU_ITEMS (30) 项, 超出部分显示省略提示
 *
 * Simple list selection (no search): arrow-key highlight, Enter confirm.
 * Reuses ui_menu_item caching + get_menu_choice. */
int ui_select_list(const char *prompt, const char **items, int count) {
    if (count <= 0) {
        ui_warn("暂无数据。");
        return -1;
    }

    ui_menu_cache_init();
    int n = count > MAX_MENU_ITEMS ? MAX_MENU_ITEMS : count;  /* 限制最大显示数 */
    /* 通过 ui_menu_item 输出并缓存所有候选项 */
    for (int i = 0; i < n; i++) {
        ui_menu_item(i + 1, items[i]);    /* 编号从 1 开始 (1-based) */
    }
    /* 超出显示上限时给出提示 / Truncation notice when exceeding MAX_MENU_ITEMS */
    if (count > MAX_MENU_ITEMS) {
        printf(C_DIM "  ... 共 %d 项（仅显示前 %d 项）" C_RESET "\n", count, MAX_MENU_ITEMS);
        ui_menu_track_line();
    }
    ui_menu_track_line();

    printf(S_LABEL "  %s" C_RESET "\n", prompt ? prompt : "请选择");
    ui_menu_track_line();

    /* 委托 get_menu_choice 处理方向键导航和确认 */
    int sel = get_menu_choice(1, n);
    if (sel < 1) return -1;               /* 用户取消 (选择 < 1) */
    return sel - 1;                        /* 从 1-based 转为 0-based 索引 */
}
