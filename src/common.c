/*
 * common.c — 跨平台控制台工具函数 / Cross-platform console utility functions
 *
 * 提供系统底层的控制台交互功能:
 *   - read_input_line(): 从 stdin 读取一行并做 GBK→UTF-8 编码转换 (Windows)
 *   - init_console_encoding(): 设置控制台输出代码页为 UTF-8
 *   - clear_input_buffer(): 清空输入缓冲区 (防残留输入干扰)
 *   - pause_screen(): "按任意键继续"
 *   - _getch(): 跨平台无缓冲单字符输入 (Windows: _getch; Unix: termios)
 *   - get_menu_choice(): 方向键菜单选择 — 支持 ↑↓ 导航、Enter 确认、数字快捷键
 *     使用 ANSI DECSC/DECRC 光标保存/恢复实现原位高亮切换
 *   - get_current_time(): 格式化当前时间为 "YYYY-MM-DD HH:MM:SS"
 *   - generate_id(): 生成唯一 ID (时间戳 + 毫秒级计数器 + 随机偏移防冲突)
 *
 * Provides low-level console interaction: UTF-8 encoding init, line input with
 * code page conversion, arrow-key menu navigation with in-place highlighting
 * via ANSI cursor save/restore, time formatting, and unique ID generation.
 */

#include "common.h"
#include "ui_utils.h"

#ifdef _WIN32
#include <windows.h>
#include <locale.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

/* 从 stdin 读取一行并转换为 UTF-8
   在 Windows 上: stdin 默认按系统代码页 (如 GBK) 编码，需转换为 UTF-8
   在 Unix 上: 直接使用 fgets
   Read line from stdin and convert to UTF-8.
   On Windows: stdin uses system code page (e.g. GBK), must convert to UTF-8. */
char* read_input_line(char *buf, size_t size) {
    if (!buf || size == 0) return NULL;
    if (fgets(buf, (int)size, stdin) == NULL) return NULL;
    buf[strcspn(buf, "\n")] = '\0';      /* 去除结尾换行符 / strip trailing newline */

#ifdef _WIN32
    UINT acp = GetACP();                  /* 获取系统默认代码页 / get system ANSI code page */
    if (acp != CP_UTF8) {
        /* GBK/ANSI → UTF-16 → UTF-8 双步转换 / Two-step conversion */
        int wlen = MultiByteToWideChar(acp, 0, buf, -1, NULL, 0);
        if (wlen > 0) {
            wchar_t *wide = (wchar_t*)malloc(wlen * sizeof(wchar_t));
            if (wide) {
                MultiByteToWideChar(acp, 0, buf, -1, wide, wlen);
                int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
                if (utf8_len > 0 && (size_t)utf8_len <= size)
                    WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, utf8_len, NULL, NULL);
                free(wide);
            }
        }
    }
#endif
    return buf;
}

/* 初始化控制台编码: 输出代码页 → UTF-8, C 区域 → .UTF-8
   同时启用 ANSI/VT 终端处理 (用于颜色/光标控制)
   Init console: output CP → UTF-8, locale → .UTF-8, enable ANSI/VT processing */
void init_console_encoding(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);               /* 控制台输出使用 UTF-8 */
    setlocale(LC_ALL, ".UTF-8");               /* C 标准库也使用 UTF-8 区域 */
#endif
    ui_init_ansi();                             /* 启用 VT 终端 ANSI 转义序列处理 */
}

/* 清空 stdin 缓冲区: 丢弃直到遇到 \n 或 EOF
   Flush stdin: discard all chars until newline or EOF */
void clear_input_buffer(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

/* 暂停并等待用户按回车 / Pause and wait for Enter key */
void pause_screen(void) {
    printf("\n按回车键继续...");
    getchar();
}

/* ==================  跨平台无缓冲单字符输入 ================== */

#ifndef _WIN32
/* Unix 实现: 使用 termios 禁用行缓冲和回显
   Unix implementation: disable canonical mode and echo via termios */
static int _getch(void) {
    struct termios oldt, newt;
    int ch;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);       /* 关闭行缓冲 + 回显 / disable line buffering & echo */
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return ch;
}
#endif

/* ==================  方向键菜单选择 (核心交互函数) ================== */

/* 方向键菜单选择: ↑↓ 切换选项，Enter 确认，数字键快捷选择
   使用缓存菜单项 + ANSI 光标保存/恢复实现原位高亮 (避免全屏重绘闪烁)

   光标控制逻辑:
   - 先输出完整菜单 (每项后跟 \n)
   - 记录总行数 total 和每项的偏移量
   - 用户按键切换时:
     a. \0337 (DECSC) 保存当前光标 (在提示行)
     b. \033[NA 上移 N 行到旧高亮项 → 重绘为普通样式
     c. \0338 (DECRC) 恢复光标到提示行
     d. \033[MA 上移 M 行到新高亮项 → 重绘为反色高亮样式
     e. \0338 再次恢复光标
     f. 更新提示行显示

   Arrow-key menu selection: ↑↓ navigate, Enter confirm, digit shortcut.
   Uses cached menu items + ANSI cursor save/restore for in-place highlight
   (avoids full-screen redraw flicker). */
int get_menu_choice(int min, int max) {
    /* 读取缓存的菜单项 / Retrieve cached menu items */
    int   item_nums[MAX_MENU_ITEMS];
    char  item_texts[MAX_MENU_ITEMS][60];
    bool  item_exit[MAX_MENU_ITEMS];
    int   item_count = ui_menu_cache_fill(item_nums, item_texts, item_exit, MAX_MENU_ITEMS);

    int current_index = 0;
    int current;

    /* 确定初始选中项: 优先选 min 对应的菜单项
       Determine initial selection: prefer item matching min */
    if (item_count > 0) {
        current = item_nums[0];
        if (min >= 1) {
            for (int i = 0; i < item_count; i++) {
                if (item_nums[i] == min) {
                    current_index = i;
                    current = item_nums[i];
                    break;
                }
            }
        }
    } else {
        current = (min >= 1) ? min : (min + 1);
        if (current < min) current = min;
        if (current > max) current = max;
    }

    /* 显示初始提示 / Show initial prompt */
    if (item_count > 0) {
        printf("\n请选择 (\xe2\x86\x91\xe2\x86\x93 切换, 回车确认): ");
        printf(C_BOLD C_CYAN "\xe2\x86\x92 " C_RESET);
        printf(C_BOLD C_WHITE "\033[7m %s \033[0m" C_RESET, item_texts[current_index]);
    } else {
        printf("\n请选择 (\xe2\x86\x91\xe2\x86\x93 切换, 回车确认): ");
        printf("\033[7m %d \033[0m", current);
    }
    fflush(stdout);

    int prev_index = current_index;
    int prev_current = current;

    while (1) {
        int ch = _getch();

        if (ch == 0xE0 || ch == 0x00) {
            /* Windows 方向键: 前缀 0xE0 + 扫描码 (↑=72, ↓=80)
               Windows arrow key: prefix 0xE0 + scan code (↑=72, ↓=80) */
            ch = _getch();
            if (ch == 72) {        /* ↑ / Up */
                if (item_count > 0) {
                    current_index--;
                    if (current_index < 0) current_index = item_count - 1;
                    current = item_nums[current_index];
                } else {
                    current--;
                    if (current < min) current = max;
                }
            } else if (ch == 80) {  /* ↓ / Down */
                if (item_count > 0) {
                    current_index++;
                    if (current_index >= item_count) current_index = 0;
                    current = item_nums[current_index];
                } else {
                    current++;
                    if (current > max) current = min;
                }
            } else {
                continue;
            }
        }
        else if (ch == 0x1B) {
            /* Unix/ANSI 转义序列: ESC [ A (↑) / ESC [ B (↓)
               Unix/ANSI escape sequence: ESC [ A (↑) / ESC [ B (↓) */
            ch = _getch();
            if (ch == '[') {
                ch = _getch();
                if (ch == 'A') {        /* ↑ / Up */
                    if (item_count > 0) {
                        current_index--;
                        if (current_index < 0) current_index = item_count - 1;
                        current = item_nums[current_index];
                    } else {
                        current--;
                        if (current < min) current = max;
                    }
                } else if (ch == 'B') {  /* ↓ / Down */
                    if (item_count > 0) {
                        current_index++;
                        if (current_index >= item_count) current_index = 0;
                        current = item_nums[current_index];
                    } else {
                        current++;
                        if (current > max) current = min;
                    }
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }
        else if (ch == '\r' || ch == '\n') {
            /* Enter — 确认选择 / confirm selection */
            printf("\n");
            return current;
        } else if (ch >= '0' && ch <= '9') {
            /* 数字键快速选择 (仅在合法范围内生效) / Digit shortcut (valid range only) */
            int num = ch - '0';
            if (num >= min && num <= max) {
                printf("\n");
                return num;
            }
            continue;
        } else {
            continue;
        }

        if (item_count > 0) {
            if (current_index == prev_index) continue;  /* 未变化则跳过 / skip if unchanged */

            /* ===== 原位高亮切换 (ANSI DECSC/DECRC 光标控制) ===== */
            {
                int total = ui_menu_get_saved_total();          /* 菜单总行数 (含边框和空行) */
                int old_idx = prev_index;
                int new_idx = current_index;
                int old_off = ui_menu_get_item_offset(old_idx); /* 旧项所在行号 */
                int new_off = ui_menu_get_item_offset(new_idx); /* 新项所在行号 */

                /* \0337 (DECSC): 保存当前光标位置 / save cursor */
                printf("\0337");

                /* 上移到旧高亮项 → 重绘为普通样式 → 恢复光标 */
                printf("\033[%dA", total - old_off + 1);
                printf("\r\033[K");                              /* 清除整行 / clear line */
                if (item_exit[old_idx]) {
                    printf("  " C_DIM "%d. %s" C_RESET "\n", item_nums[old_idx], item_texts[old_idx]);
                } else {
                    printf("  " C_BOLD C_YELLOW "%d." C_RESET " %s\n", item_nums[old_idx], item_texts[old_idx]);
                }

                /* \0338 (DECRC): 恢复光标到提示行 / restore cursor to prompt */
                printf("\0338");

                /* 上移到新高亮项 → 重绘为反色高亮 → 恢复光标 */
                printf("\033[%dA", total - new_off + 1);
                printf("\r\033[K");
                printf("  " BG_CYAN C_WHITE C_BOLD "%d. %s" C_RESET "\n", item_nums[new_idx], item_texts[new_idx]);

                /* 再次恢复光标 */
                printf("\0338");
            }

            prev_index = current_index;
        } else {
            if (current == prev_current) continue;
            prev_current = current;
        }

        /* 更新底部提示行: 清空当前行 → 重绘 / Update prompt line: clear → redraw */
        printf("\r\033[K请选择 (\xe2\x86\x91\xe2\x86\x93 切换, 回车确认): ");
        if (item_count > 0) {
            printf(C_BOLD C_CYAN "\xe2\x86\x92 " C_RESET);
            printf(C_BOLD C_WHITE "\033[7m %s \033[0m" C_RESET, item_texts[current_index]);
        } else {
            printf("\033[7m %d \033[0m", current);
        }
        fflush(stdout);
    }
}

/* 获取当前时间字符串 (格式: "YYYY-MM-DD HH:MM:SS")
   Get current time formatted as "YYYY-MM-DD HH:MM:SS" */
void get_current_time(char *buffer, int buffer_size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", tm_info);
}

/* 生成唯一 ID: "前缀YYYYMMDDHHMMSS序号" (总长 ≤ 19 字符)
   使用静态计数器在同一秒内递增，并以随机偏移初始化减少重启冲突。
   格式: 前缀(变长) + YYYY(4) + MM(2) + DD(2) + HH(2) + MM(2) + SS(2) + 序号(3)

   Generate unique ID: "prefix_YYYYMMDDHHMMSS_seq" (max 19 chars).
   Uses static counter that increments within same second, initialized with
   random offset to reduce collision risk on restart. */
void generate_id(char *buffer, int buffer_size, const char *prefix) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    static time_t last_sec = 0;
    static int counter = 0;
    static int initialized = 0;

    /* 首次调用: 使用随机偏移初始化计数器，减少重启后 ID 碰撞风险
       First call: init counter with random offset to reduce post-restart collisions */
    if (!initialized) {
        srand((unsigned int)(now ^ (unsigned long long)&counter));
        counter = rand() % 7919;   /* 随机偏移 (质数) / random offset (prime) */
        initialized = 1;
    }

    /* 新的一秒: 重置计数器 / New second: reset counter */
    if (now != last_sec) {
        last_sec = now;
        counter = 0;
    }
    int seq = counter++;
    snprintf(buffer, buffer_size, "%s%04d%02d%02d%02d%02d%02d%03d",
             prefix,
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec,
             seq % 1000);          /* 模 1000 防溢出 / modulo 1000 to prevent overflow */
}
