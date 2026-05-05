/*
 * ui_utils.c — 控制台 UI 工具库实现 / Console UI utility library implementation
 *
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
#include <conio.h>
#endif

/* ==================  行跟踪状态 (原位高亮用) / Line Tracking State ================== */
/* 这些静态变量跟踪输出的菜单项行号，以便在方向键切换时通过 ANSI
   光标控制精确定位到每个菜单项进行重绘 (高亮/取消高亮)。

   These static variables track menu item line positions so arrow-key
   navigation can precisely reposition the cursor for in-place redraw
   (highlight/unhighlight) using ANSI cursor control sequences. */

static int   g_line_counter      = 0;        /* 当前菜单段落累计行数 / cumulative line count */
static int   g_item_offsets[MAX_MENU_ITEMS]; /* 每个菜单项的行号 / line number per item */
static int   g_saved_total       = 0;        /* 固化的总行数 / snapshot total lines */
static int   g_saved_offsets[MAX_MENU_ITEMS];/* 固化的偏移量 / snapshot offsets */
static int   g_saved_count       = 0;        /* 固化的项数 / snapshot item count */

/* ==================  UTF-8 显示宽度 / UTF-8 Display Width ================== */

/* 计算 UTF-8 字符串的终端显示宽度
   规则: 单字节 (0x00-0x7F) → 宽 1
         3 字节 (0xE0+) 中文/CJK → 宽 2
         2 字节 (0xC0+) 拉丁扩展 → 宽 1
   用于表格列对齐 (printf %s 对中文字符占位不准确)

   Calculate terminal display width of UTF-8 string:
     - 1-byte (ASCII) → width 1
     - 3-byte (CJK, 0xE0+) → width 2
     - 2-byte (Latin-ext, 0xC0+) → width 1
   Used for table column alignment where printf %s miscounts CJK width. */
int utf8_display_width(const char *s) {
    int w = 0;
    if (!s) return 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c <= 0x7F)      { w += 1; s += 1; }  /* ASCII */
        else if (c >= 0xE0) { w += 2; s += 3; }  /* CJK 汉字 (3 字节) */
        else if (c >= 0xC0) { w += 1; s += 2; }  /* 拉丁扩展 (2 字节) */
        else                { w += 1; s += 1; }  /* 其他 / other */
    }
    return w;
}

/* ==================  ANSI / VT 终端初始化 / ANSI/VT Terminal Init ================== */

/* 在 Windows 上启用虚拟终端处理 (使 ANSI 转义序列生效)
   没有这一步，颜色代码和光标控制序列都不会被解释。
   Enable virtual terminal processing on Windows so ANSI escape sequences
   (colors, cursor control) are interpreted by the console. */
void ui_init_ansi(void) {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, mode);
        }
    }
#endif
}

/* ANSI 清屏: \033[2J 清屏, \033[H 光标归位
   ANSI clear: \033[2J erase display, \033[H cursor home */
void ui_clear_screen(void) {
    printf(C_RESET "\033[2J\033[H");
}

/* ==================  UI 绘制函数 / UI Drawing Functions ================== */

/* 画水平线: 使用指定字符或默认的 "─" (U+2500)
   Draw horizontal line with specified char or default "─" */
void ui_line(int width, const char *ch) {
    if (width <= 0) width = 60;
    printf(C_RESET);
    for (int i = 0; i < width; i++) {
        printf("%s", ch && ch[0] ? ch : "\xe2\x94\x80");
    }
    printf("\n");
}

/* 盒子上边框: ╔══ title ═══════╗ (双线框)
   居中显示标题，左右用 ═ 填充。
   Box top border: ╔══ title ═══════╗ with centered title. */
void ui_box_top(const char *title) {
    ui_menu_cache_init();  /* 每次新菜单时重置缓存 / fresh cache for each menu */
    int len = title ? utf8_display_width(title) : 0;
    int total = 58;
    int left = (total - len) / 2;
    int right = total - len - left;

    printf(C_BOLD C_CYAN);
    printf("\xe2\x95\x94");                             /* ╔ */
    for (int i = 0; i < left; i++) printf("\xe2\x95\x90"); /* ═ */
    if (title && len > 0) {
        printf(" ");
        printf(C_WHITE);
        printf("%s", title);
        printf(C_BOLD C_CYAN);
        printf(" ");
    }
    for (int i = 0; i < right; i++) printf("\xe2\x95\x90");
    printf("\xe2\x95\x97");                              /* ╗ */
    printf(C_RESET "\n");
    g_line_counter++;
}

/* 盒子下边框: ╚══════════════╝
   Box bottom border: ╚══════════════╝ */
void ui_box_bottom(void) {
    printf(C_BOLD C_CYAN);
    printf("\xe2\x95\x9a");                               /* ╚ */
    for (int i = 0; i < 58; i++) printf("\xe2\x95\x90"); /* ═ */
    printf("\xe2\x95\x9d");                               /* ╝ */
    printf(C_RESET "\n");
    g_line_counter++;
}

/* 大标题: ═══ title ═══ (双线装饰)
   Main header: ═══ title ═══ with double-line decoration */
void ui_header(const char *title) {
    printf("\n");
    printf(C_BOLD C_CYAN);
    printf("  \xe2\x94\x81\xe2\x94\x81\xe2\x94\x81  ");   /* ━━━  */
    printf(C_WHITE);
    printf("%s", title);
    printf(C_BOLD C_CYAN);
    printf("  \xe2\x94\x81\xe2\x94\x81\xe2\x94\x81");      /* ━━━ */
    printf(C_RESET "\n");
    g_line_counter += 2;
}

/* 子标题: ▸ title (青色标签)
   Sub-header: ▸ title with cyan label */
void ui_sub_header(const char *title) {
    printf("\n");
    printf(S_LABEL);
    printf("  \xe2\x96\xb8 ");                             /* ▸ */
    printf(C_RESET);
    printf(C_BOLD);
    printf("%s", title);
    printf(C_RESET "\n");
    g_line_counter += 2;
}

/* 分隔线: ─────────────────
   Divider line: ───────────────── */
void ui_divider(void) {
    printf(C_DIM);
    printf("  ");
    for (int i = 0; i < 58; i++) printf("\xe2\x94\x80");    /* ─ */
    printf(C_RESET "\n");
    g_line_counter++;
}

/* 成功消息: ✓ msg (绿色)
   Success message: ✓ msg in green */
void ui_ok(const char *msg) {
    printf(S_SUCCESS);
    printf("  \xe2\x9c\x93 %s", msg);                       /* ✓ */
    printf(C_RESET "\n");
}

/* 错误消息: ✗ msg (红色)
   Error message: ✗ msg in red */
void ui_err(const char *msg) {
    printf(S_ERROR);
    printf("  \xe2\x9c\x97 %s", msg);                       /* ✗ */
    printf(C_RESET "\n");
}

/* 警告消息: ⚠ msg (黄色)
   Warning message: ⚠ msg in yellow */
void ui_warn(const char *msg) {
    printf(S_WARNING);
    printf("  \xe2\x9a\xa0 %s", msg);                       /* ⚠ */
    printf(C_RESET "\n");
}

/* 标签-值对: label:        value (标签青色, 值白色, 至少间距 20 显示宽度)
   Info label-value pair: label in cyan, value in white, min 20-width spacing */
void ui_info(const char *label, const char *value) {
    int vw = utf8_display_width(label);
    int pad = 20 - vw;
    if (pad < 1) pad = 1;

    printf("  ");
    printf(S_LABEL);
    printf("%s", label);
    printf(C_RESET);
    for (int i = 0; i < pad; i++) printf(" ");
    printf("%s", value ? value : "");
    printf("\n");
}

/* 用户身份徽章: 👤 用户名 [角色] (带背景色标签)
   User badge: 👤 username [role] with background-colored tag */
void ui_user_badge(const char *name, const char *role) {
    const char *role_label = "";
    if (strcmp(role, "admin") == 0) role_label = "\xe7\xae\xa1 \xe7\x90\x86 \xe5\x91\x98";    /* 管理员 */
    else if (strcmp(role, "doctor") == 0) role_label = "\xe5\x8c\xbb    \xe7\x94\x9f";         /* 医  生 */
    else if (strcmp(role, "patient") == 0) role_label = "\xe6\x82\xa3    \xe8\x80\x85";        /* 患  者 */

    printf(C_BOLD C_CYAN);
    printf("  \xf0\x9f\x91\xa4 ");                          /* 👤 */
    printf(C_WHITE);
    printf("%s", name);
    printf(C_RESET);
    printf("  ");
    printf(BG_CYAN C_BOLD);
    printf(" %s ", role_label);
    printf(C_RESET "\n");
    g_line_counter++;
}

/* 步骤指示器: [step] desc
   Step indicator: [step] desc (currently unused in codebase, defined for consistency) */
void ui_step(int step, const char *desc) {
    printf("  [%d] %s\n", step, desc);
}

/* ==================  表格列打印 (显示宽度对齐) / Table Column Printing ================== */

/* 打印字符串列: 输出 s 后补空格至显示宽度 width
   Print string column: output s, pad with spaces to display width */
void ui_print_col(const char *s, int width) {
    int w = utf8_display_width(s);
    printf("%s", s);
    for (int i = w; i < width; i++) printf(" ");
    printf(" ");
}

/* 打印整数列 / Print integer column */
void ui_print_col_int(int val, int width) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    ui_print_col(buf, width);
}

/* 打印浮点数 (2 位小数) 列 / Print float column (2 decimal places) */
void ui_print_col_float(float val, int width) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", val);
    ui_print_col(buf, width);
}

/* ==================  字符串不区分大小写搜索 / Case-Insensitive Substring Search ================== */

static int str_contains_icase(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return 0;
    const char *h = haystack;
    while (*h) {
        const char *n = needle;
        const char *start = h;
        while (*n && *start) {
            char hc = *start, nc = *n;
            if (hc >= 'A' && hc <= 'Z') hc += 32;    /* 大写转小写 */
            if (nc >= 'A' && nc <= 'Z') nc += 32;
            if (hc != nc) break;
            start++; n++;
        }
        if (!*n) return 1;   /* needle 完全匹配 / full match */
        h++;
    }
    return 0;
}

/* ==================  智能输入 / Smart Input ================== */

/* 智能 ID 查找: 若列表为空则直接输入，否则调用搜索选择
   Smart ID lookup: direct input if list empty, otherwise interactive search */
int smart_id_lookup(const char *prompt, const char *id_list[], int count, char *output, int out_size) {
    if (count <= 0) {
        char input[64];
        printf(S_LABEL "  %s: " C_RESET, prompt);
        if (read_input_line(input, sizeof(input)) == NULL) return -1;
        if (input[0] == 0) return -1;
        strncpy(output, input, out_size - 1);
        output[out_size - 1] = 0;
        return 1;
    }

    int idx = ui_search_list(prompt, id_list, count);
    if (idx < 0) return -1;
    strncpy(output, id_list[idx], out_size - 1);
    output[out_size - 1] = 0;
    return 1;
}

/* 智能患者输入: 收集与指定医生关联的患者 ID → 交互式选择
   关联来源: 预约挂号 + 现场挂号 (去重)
   Smart patient input: collect patient IDs linked to doctor (from appointments
   and onsite registrations, deduplicated), then interactive selection. */
int smart_patient_input(const char *doctor_id, const char *prompt, char *output, int out_size) {
    const char *candidates[500];
    int count = 0;

    /* 从预约记录收集 / Collect from appointments */
    {
        AppointmentNode *ah = load_appointments_list();
        AppointmentNode *ac = ah;
        while (ac && count < 150) {
            if (strcmp(ac->data.doctor_id, doctor_id) == 0) {
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
    /* 从现场挂号收集 / Collect from onsite registrations */
    {
        OnsiteRegistrationQueue oq = load_onsite_registration_queue();
        OnsiteRegistrationNode *oc = oq.front;
        while (oc && count < 250) {
            if (strcmp(oc->data.doctor_id, doctor_id) == 0) {
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

/* 智能药品输入: 加载所有药品 → 构建 "ID (名称)" 列表 → 交互式选择
   选择后从输出中提取药品 ID (裁掉括号内容)
   Smart drug input: load all drugs → "ID (name)" list → interactive selector.
   Extracts drug ID from selection (strips parenthesized name). */
int smart_drug_input(const char *prompt, char *output, int out_size) {
    DrugNode *dh = load_drugs_list();
    DrugNode *dc = dh;
    char *drug_list[300];
    char  drug_buf[300][MAX_ID + MAX_NAME + 4];
    int count = 0;

    while (dc && count < 200) {
        snprintf(drug_buf[count], sizeof(drug_buf[0]), "%s (%s)", dc->data.drug_id, dc->data.name);
        drug_list[count] = drug_buf[count];
        count++;
        dc = dc->next;
    }
    free_drug_list(dh);

    int result = smart_id_lookup(prompt, (const char **)drug_list, count, output, out_size);

    /* 提取 ID: 截断在第一个空格处 (去掉 " (名称)" 部分) */
    if (result == 1) {
        char *paren = strchr(output, ' ');
        if (paren) *paren = 0;
    }
    return result;
}

/* ==================  模板快捷输入 / Template Quick Input ================== */

/* 模板快捷输入: 支持三种输入方式
   1. 直接输入文本
   2. #n 引用第 n 个模板
   3. 纯数字 n 引用第 n 个模板
   4. v 查看全部模板列表
   Quick template input: supports direct text, #n syntax for template reference,
   numeric selection, and v for viewing all templates. */
int quick_template_input(const char *category, const char *prompt, char *output, int out_size) {
    TemplateNode *head = load_templates_list();
    TemplateNode *cur = head;

    const char *tlist[200];
    char ttext[200][600];
    int tcount = 0;

    /* 收集指定类别的模板 / Collect templates of the given category */
    while (cur && tcount < 100) {
        if (strcmp(cur->data.category, category) == 0) {
            snprintf(ttext[tcount], sizeof(ttext[0]), "%s | %s",
                     cur->data.shortcut, cur->data.text);
            tlist[tcount] = ttext[tcount];
            tcount++;
        }
        cur = cur->next;
    }
    free_template_list(head);

    /* 无模板 → 直接输入 / No templates → direct input */
    if (tcount == 0) {
        printf(S_LABEL "  %s" C_RESET " (直接输入): ", prompt);
        if (read_input_line(output, out_size) == NULL) return -1;
        if (output[0] == 0) return -1;
        return 1;
    }

    printf("\n" S_LABEL "  %s" C_RESET "\n", prompt);
    printf(C_DIM "  [直接输入] / [#n=模板] / [v=查看全部模板]" C_RESET "\n");

    if (read_input_line(output, out_size) == NULL) return -1;

    if (output[0] == 0) return -1;

    /* v 命令: 显示模板列表后重新输入 / v command: show templates, then re-input */
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

    /* #n 模板语法: 如 #3 → 引用第 3 个模板
       #n template syntax: e.g. #3 → reference 3rd template */
    if (output[0] == '#') {
        char *num_str = output + 1;
        int is_digits = 1;
        for (char *p = num_str; *p; p++) {
            if (*p < '0' || *p > '9') { is_digits = 0; break; }
        }
        if (is_digits && num_str[0] != '\0') {
            int idx = atoi(num_str) - 1;
            if (idx >= 0 && idx < tcount) {
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

    /* 纯数字输入: 如 3 → 引用第 3 个模板
       Pure numeric: e.g. 3 → reference 3rd template */
    {
        int is_all_digits = 1;
        char *p = output;
        while (*p) { if (*p < '0' || *p > '9') { is_all_digits = 0; break; } p++; }
        if (is_all_digits && output[0]) {
            int idx = atoi(output) - 1;
            if (idx >= 0 && idx < tcount) {
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

    return 1;
}

/* ==================  菜单项缓存 / Menu Item Cache ================== */

/* 菜单缓存数据结构 (供 get_menu_choice 使用)
   Menu cache data structures used by get_menu_choice in common.c */
static int   g_menu_nums[MAX_MENU_ITEMS];
static char  g_menu_texts[MAX_MENU_ITEMS][60];
static bool  g_menu_exit[MAX_MENU_ITEMS];
static int   g_menu_count = 0;

/* 初始化缓存: 每次进入新菜单前调用 / Reset cache before each new menu */
void ui_menu_cache_init(void) {
    g_menu_count = 0;
    g_line_counter = 0;
}

/* 填充缓存数组供 get_menu_choice 读取，同时固化行跟踪快照
   填充后重置缓存为下次菜单准备
   Fill cache arrays for get_menu_choice to read, snapshot line tracking state,
   then reset cache for next menu. */
int ui_menu_cache_fill(int *nums, char texts[][60], bool *is_exit, int max) {
    int n = (g_menu_count < max) ? g_menu_count : max;
    for (int i = 0; i < n; i++) {
        nums[i] = g_menu_nums[i];
        strncpy(texts[i], g_menu_texts[i], 59);
        texts[i][59] = 0;
        is_exit[i] = g_menu_exit[i];
    }

    /* 固化行跟踪快照 (在重置前保存) / Save line tracking snapshot before reset */
    g_saved_total = g_line_counter;
    for (int i = 0; i < n && i < MAX_MENU_ITEMS; i++) {
        g_saved_offsets[i] = g_item_offsets[i];
    }
    g_saved_count = n;

    ui_menu_cache_init();  /* 为下一个菜单重置 / reset for next menu */
    return n;
}

/* 输出普通菜单项: 缓存编号+文本+行号，输出为 "1. 文本" 的黄色编号样式
   Output normal menu item: cache num+text+line, render as "1. text" with yellow number */
void ui_menu_item(int num, const char *text) {
    if (g_menu_count < MAX_MENU_ITEMS) {
        g_item_offsets[g_menu_count] = g_line_counter;
        g_menu_nums[g_menu_count] = num;
        strncpy(g_menu_texts[g_menu_count], text, 59);
        g_menu_texts[g_menu_count][59] = 0;
        g_menu_exit[g_menu_count] = false;
        g_menu_count++;
    }
    printf("  ");
    printf(C_BOLD C_YELLOW);
    printf("%d.", num);
    printf(C_RESET);
    printf(" %s\n", text);
    g_line_counter++;
}

/* 输出退出菜单项: 用暗淡样式显示 (区别于普通选项)
   Output exit menu item: dimmed style to distinguish from normal items */
void ui_menu_exit(int num, const char *text) {
    if (g_menu_count < MAX_MENU_ITEMS) {
        g_item_offsets[g_menu_count] = g_line_counter;
        g_menu_nums[g_menu_count] = num;
        strncpy(g_menu_texts[g_menu_count], text, 59);
        g_menu_texts[g_menu_count][59] = 0;
        g_menu_exit[g_menu_count] = true;
        g_menu_count++;
    }
    printf("  ");
    printf(C_DIM);
    printf("%d.", num);
    printf(" %s", text);
    printf(C_RESET "\n");
    g_line_counter++;
}

/* ==================  确认对话框 / Confirmation Dialog ================== */

/* 模态确认: 显示 "? prompt [Y/n]"，回车或 Y 确认，n 取消
   Modal confirm: show "? prompt [Y/n]", Enter or Y confirms, n cancels */
bool ui_confirm(const char *prompt) {
    printf("\n" S_WARNING "  ? %s" C_RESET " [" C_BOLD C_GREEN "Y" C_RESET "/" C_DIM "n" C_RESET "]: ", prompt ? prompt : "确认执行");
    fflush(stdout);
    int c = getchar();
    if (c != '\n' && c != '\r') clear_input_buffer();
    return (c == '\n' || c == '\r' || c == 'y' || c == 'Y');
}

/* ==================  行跟踪接口 / Line Tracking Interface ================== */

/* 增加一行 (用于非菜单项的额外输出，如空行)
   Increment line counter for non-menu-item output (e.g. blank lines) */
void ui_menu_track_line(void) {
    g_line_counter++;
}

/* 获取固化的总行数 / Get saved total line count */
int ui_menu_get_saved_total(void) {
    return g_saved_total;
}

/* 获取指定索引项的固化偏移 / Get saved offset for item at index */
int ui_menu_get_item_offset(int idx) {
    if (idx >= 0 && idx < g_saved_count) return g_saved_offsets[idx];
    return 0;
}

/* ==================  分页显示 / Pagination ================== */

/* 分页显示列表: 支持翻页 (n/p)、跳页 (g)、搜索 (s)、退出 (q)
   搜索在当前列表中进行不区分大小写的子串匹配，结果显示在新的分页视图中
   Paginate a list: next/prev page (n/p), goto page (g), search (s), quit (q).
   Search does case-insensitive substring matching across items. */
void ui_paginate(const char **items, int count, int page_size, const char *title) {
    if (count <= 0) {
        ui_warn("暂无数据。");
        return;
    }

    if (page_size <= 0) page_size = 15;   /* 默认每页 15 条 / default 15 per page */

    int total_pages = (count + page_size - 1) / page_size;
    int page = 0;

    while (page < total_pages) {
        if (title) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s (共%d条)", title, count);
            ui_sub_header(buf);
        }

        int start = page * page_size;
        int end = start + page_size;
        if (end > count) end = count;

        for (int i = start; i < end; i++) {
            printf("  " C_BOLD C_YELLOW "%d." C_RESET " %s\n", i + 1, items[i]);
        }

        if (total_pages > 1) {
            printf(C_DIM "  ─── 第 %d/%d 页", page + 1, total_pages);
            printf("  n下一页 p上一页 g跳页 q返回 s搜索" C_RESET "\n");

            int c = getchar();
            if (c != '\n') clear_input_buffer();

            if (c == 'q' || c == 'Q') break;

            if (c == 's' || c == 'S') {
                /* 搜索子流程 / Search sub-flow */
                char keyword[64];
                printf(S_LABEL "  输入关键字: " C_RESET);
                if (read_input_line(keyword, sizeof(keyword)) == NULL) continue;
                if (keyword[0] == '\0') continue;

                int match_idx[count];
                int match_cnt = 0;
                for (int i = 0; i < count; i++) {
                    if (str_contains_icase(items[i], keyword)) {
                        match_idx[match_cnt++] = i;
                    }
                }

                if (match_cnt == 0) {
                    ui_warn("未匹配到结果。");
                    continue;
                }

                /* 构建过滤后的列表并递归分页 / Build filtered list and paginate recursively */
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

            if (c == 'p' || c == 'P') {
                if (page > 0) { page -= 2; continue; }   /* -2 因为后面 page++ */
                printf(C_DIM "  已是第一页\n" C_RESET);
                continue;
            }

            if (c == 'g' || c == 'G') {
                printf(S_LABEL "  输入页码 (1-%d): " C_RESET, total_pages);
                char pg_buf[16];
                if (read_input_line(pg_buf, sizeof(pg_buf)) && pg_buf[0]) {
                    int goto_page = atoi(pg_buf) - 1;
                    if (goto_page >= 0 && goto_page < total_pages) { page = goto_page - 1; }
                    else { printf(C_DIM "  无效页码\n" C_RESET); continue; }
                } else { continue; }
                continue;
            }

            if (c == '\n' || c == 'n' || c == 'N') {
                if (page >= total_pages - 1) break;
            } else if (c != '\n' && c != 'n' && c != 'N') {
                if (page >= total_pages - 1) break;
            }
        }

        page++;
    }
}

/* ==================  搜索列表选择 / Search List Selection ================== */

/* 搜索并选择列表项: 显示前 20 条，方向键导航高亮，搜索过滤，回车选中
   使用与 get_menu_choice 相同的 ANSI DECSC/DECRC 原位高亮技术
   Search and select from list: show first 20 items, arrow-key highlight,
   search filter, Enter to select. Uses same ANSI cursor in-place highlight
   technique as get_menu_choice. */
int ui_search_list(const char *prompt, const char **items, int count) {
    if (count <= 0) {
        ui_warn("暂无数据。");
        return -1;
    }

    int matches[500];
    int mcount = count > 500 ? 500 : count;
    for (int i = 0; i < mcount; i++) matches[i] = i;  /* 初始匹配全部 / initially match all */

    char input[64];

    while (1) {
        int show_count = mcount > 20 ? 20 : mcount;     /* 最多显示 20 项 */
        int more_lines = mcount > 20 ? 1 : 0;

        printf("\n" S_LABEL "  %s:" C_RESET "\n", prompt ? prompt : "请选择");
        ui_divider();
        for (int i = 0; i < show_count; i++) {
            printf("  " C_BOLD C_YELLOW "%2d." C_RESET " %s\n", i + 1, items[matches[i]]);
        }
        if (more_lines) {
            printf(C_DIM "  ... 还有 %d 项" C_RESET "\n", mcount - 20);
        }

        printf(C_DIM "  ↑↓选择 回车确认 s搜索 q取消" C_RESET "\n");
        fflush(stdout);

        int highlight = 0;
        int prev_highlight = -1;

        while (1) {
            int ch = _getch();

            /* 方向键处理 / Arrow key handling */
            if (ch == 0xE0 || ch == 0x00) {
                ch = _getch();
                if (ch == 72)      { highlight--; if (highlight < 0) highlight = show_count - 1; }
                else if (ch == 80) { highlight++; if (highlight >= show_count) highlight = 0; }
                else continue;
            }
            else if (ch == 0x1B) {
                ch = _getch();
                if (ch == '[') {
                    ch = _getch();
                    if (ch == 'A')      { highlight--; if (highlight < 0) highlight = show_count - 1; }
                    else if (ch == 'B') { highlight++; if (highlight >= show_count) highlight = 0; }
                    else continue;
                } else continue;
            }
            else if (ch == '\r' || ch == '\n') {
                printf("\r\033[K\n");
                return matches[highlight];
            }
            else if (ch == 's' || ch == 'S') {
                /* 搜索过滤 / Search filter */
                printf("\r\033[K");
                printf(S_LABEL "  搜索: " C_RESET);
                fflush(stdout);
                if (read_input_line(input, sizeof(input)) == NULL) return -1;

                if (input[0] == '\0') {
                    /* 清空搜索 → 复位全部匹配 / Reset to all matches */
                    mcount = count > 500 ? 500 : count;
                    for (int i = 0; i < mcount; i++) matches[i] = i;
                    break;
                }

                int new_matches[500];
                int new_mcount = 0;
                for (int i = 0; i < count && new_mcount < 500; i++) {
                    if (str_contains_icase(items[i], input)) {
                        new_matches[new_mcount++] = i;
                    }
                }

                if (new_mcount == 0) {
                    ui_err("未匹配到任何结果。");
                    break;
                }

                mcount = new_mcount;
                memcpy(matches, new_matches, mcount * sizeof(int));
                /* 唯一匹配 → 自动选中 / Single match → auto-select */
                if (mcount == 1) {
                    printf("\r\033[K");
                    printf(S_SUCCESS "  %s" C_RESET "\n", items[matches[0]]);
                    return matches[0];
                }
                break;
            }
            else if (ch == 'q' || ch == 'Q') {
                printf("\r\033[K");
                return -1;
            }
            else if (ch >= '1' && ch <= '9') {
                /* 数字快捷键 / Digit shortcut */
                int n = ch - '0';
                if (n >= 1 && n <= show_count) {
                    printf("\n");
                    return matches[n - 1];
                }
                continue;
            }
            else {
                continue;
            }

            if (highlight == prev_highlight) continue;

            /* 原位高亮切换 / In-place highlight swap */
            int dist = show_count + more_lines - highlight;

            /* 取消旧项高亮 / Un-highlight previous item */
            if (prev_highlight >= 0) {
                int old_dist = show_count + more_lines - prev_highlight;
                printf("\0337");                             /* 保存光标 / save cursor */
                printf("\033[%dA", old_dist);                /* 上移到旧项 / move up to old */
                printf("\r\033[K");                          /* 清行 / clear line */
                printf("  " C_BOLD C_YELLOW "%2d." C_RESET " %s\n", prev_highlight + 1, items[matches[prev_highlight]]);
                printf("\0338");                             /* 恢复光标 / restore cursor */
            }

            /* 高亮新项 / Highlight new item */
            printf("\0337");
            printf("\033[%dA", dist);
            printf("\r\033[K");
            printf("  " BG_CYAN C_WHITE C_BOLD "%2d. %s" C_RESET "\n", highlight + 1, items[matches[highlight]]);
            printf("\0338");
            fflush(stdout);

            prev_highlight = highlight;
        }
    }
}

/* ==================  普通列表选择 / Simple List Selection ================== */

/* 列表选择 (无搜索过滤): 方向键导航高亮，回车确认
   复用 ui_menu_item 的缓存机制 + get_menu_choice
   Simple list selection (no search): arrow-key highlight, Enter confirm.
   Reuses ui_menu_item caching + get_menu_choice. */
int ui_select_list(const char *prompt, const char **items, int count) {
    if (count <= 0) {
        ui_warn("暂无数据。");
        return -1;
    }

    ui_menu_cache_init();
    int n = count > MAX_MENU_ITEMS ? MAX_MENU_ITEMS : count;
    for (int i = 0; i < n; i++) {
        ui_menu_item(i + 1, items[i]);
    }
    if (count > MAX_MENU_ITEMS) {
        printf(C_DIM "  ... 共 %d 项（仅显示前 %d 项）" C_RESET "\n", count, MAX_MENU_ITEMS);
        ui_menu_track_line();
    }
    ui_menu_track_line();

    printf(S_LABEL "  %s" C_RESET "\n", prompt ? prompt : "请选择");
    ui_menu_track_line();

    int sel = get_menu_choice(1, n);
    if (sel < 1) return -1;
    return sel - 1;   /* 转换为 0-based 索引 / convert to 0-based index */
}
