/*
 * gui_login.c — GUI 登录与注册对话框 / GUI login & registration dialogs
 *
 * 实现 Win32 对话框形式的登录和注册界面。
 *作者：王成烨
 * 登录对话框 (LoginDlg):
 *   - 角色单选按钮 (患者/医生/管理员)
 *   - 用户名 + 密码编辑框
 *   - 手动 SHA-256 哈希验证 (不依赖 login.c 的函数)
 *   - 登录/注册/退出按钮
 *   - 模态消息循环 (禁用父窗口)
 *
 * 注册对话框 (RegisterDlg):
 *   - 用户名 (≥3字符) + 密码 (≥6字符) + 确认密码
 *   - 角色单选按钮
 *   - 姓名编辑框 + 科室下拉框 (医生必选)
 *   - 用户名唯一性检查 + SHA-256 密码哈希
 *   - 自动创建患者/医生档案
 *
 * 技术要点:
 *   - 使用自定义窗口类 + CreateWindowExA (非 DialogBox), 实现模态消息循环
 *   - 通过 EnableWindow(hParent, FALSE/TRUE) 禁用/启用父窗口
 *   - 使用 g_loginResult / g_loginResultCode 全局变量在模态循环间传递结果
 *   - SHA-256 哈希使用 sha256.h 中的 sha256_hash / sha256_hex 函数
 *
 * Implements login and registration as Win32 custom-drawn dialogs
 * with modal message loops (not DialogBox-based) for fine control.
 */

#include "gui_login.h"
#include "data_storage.h"
#include "sha256.h"
#include "public.h"
#include <commctrl.h>

/* ==================  控件 ID 常量 / Control ID Constants ================== */

/*
 * 登录对话框控件 ID — 每个子控件有唯一整数 ID
 * HMENU 参数被复用为控件 ID (通过 INT_PTR 强制转换)
 */

/* 登录对话框 / Login dialog */
#define IDC_ROLE_ADMIN    101    /* 管理员单选 — 选择管理员身份 */
#define IDC_ROLE_DOCTOR   102    /* 医生单选 — 选择医生身份 */
#define IDC_ROLE_PATIENT  103    /* 患者单选 — 选择患者身份 (默认选中) */
#define IDC_USERNAME      104    /* 用户名编辑框 — 输入登录用户名 */
#define IDC_PASSWORD      105    /* 密码编辑框 — 输入登录密码 (掩码显示) */
#define IDC_LOGIN         106    /* 登录按钮 — 触发登录验证流程 */
#define IDC_REGISTER      107    /* 注册按钮 — 打开注册对话框 */
#define IDC_CANCEL        108    /* 退出按钮 — 关闭对话框, 取消登录 */
#define IDC_STATUS        109    /* 状态/错误文本 — 显示 "密码错误" 等消息 */
#define IDC_TITLE         110    /* 标题标签 — "用户登录" (大号粗体) */

/* 注册对话框 / Register dialog */
#define IDC_REG_USERNAME  201    /* 用户名 — 注册新用户名 (唯一性校验) */
#define IDC_REG_PASSWORD  202    /* 密码 — 注册密码 (掩码显示, ≥6字符) */
#define IDC_REG_CONFIRM   203    /* 确认密码 — 二次输入密码 (必须匹配) */
#define IDC_REG_ROLE_ADMIN   204 /* 管理员单选 — 注册为管理员 */
#define IDC_REG_ROLE_DOCTOR  205 /* 医生单选 — 注册为医生 (需选科室) */
#define IDC_REG_ROLE_PATIENT 206 /* 患者单选 — 注册为患者 (默认选中) */
#define IDC_REG_DEPT      207    /* 科室下拉框 — 仅医生角色可见 */
#define IDC_REG_DEPT_LABEL 213   /* 科室标签 — "科室:" 文本, 随医生选中显示/隐藏 */
#define IDC_REG_NAME      208    /* 姓名 — 用户真实姓名 (必填) */
#define IDC_REG_TITLE     209    /* 职称 (预留) — 当前未使用 */
#define IDC_REG_OK        210    /* 注册按钮 — 提交注册信息 */
#define IDC_REG_CANCEL    211    /* 取消按钮 — 关闭注册对话框 */
#define IDC_REG_STATUS    212    /* 状态文本 — 显示验证错误或成功消息 */

/* =======================  对话框尺寸 / Dialog Dimensions ======================= */

/*
 * 对话框尺寸常量 (像素近似值)
 *
 * DLG_W 360 / DLG_H 320: 登录对话框 (宽360, 高320)
 * REG_W 400 / REG_H 400: 注册对话框 (宽400, 高400)
 *
 * 注: 这些值传递给 CreateWindowExA 的宽高参数, 但实际大小
 *     取决于系统 DPI 设置和字体。标题栏等非客户区不计入。
 */
#define DLG_W 360
#define DLG_H 320
#define REG_W 400
#define REG_H 400

/* =======================  全局登录结果 / Global Login Result ======================= */

/*
 * 全局登录结果 — 用于模态消息循环中传递登录结果
 *
 * g_loginResult:    成功登录时填充 User 结构体 (用户名/角色/密码哈希)
 * g_loginResultCode: 操作结果码 (SUCCESS=0 或错误码)
 *
 * 工作流程:
 *   1. ShowLoginDialog 初始化 g_loginResultCode = ERROR_INVALID_INPUT
 *   2. 登录对话框消息循环中, 用户点击登录 → 验证 → 设置 g_loginResultCode
 *   3. PostMessage WM_CLOSE → 消息循环检测 IsWindow 返回 FALSE → 退出
 *   4. ShowLoginDialog 读取 g_loginResultCode 和 g_loginResult 返回给调用者
 *
 * Global login result (modal loop communication)
 */
static User g_loginResult;
static int g_loginResultCode = ERROR_INVALID_INPUT;

/* ==================  控件创建工具函数 / Control Creation Helpers ================== */

/*
 * 创建静态标签控件 — 用于显示只读文本 (标题、字段名、分隔线等)
 *
 * 参数:
 *   hParent - 父窗口句柄
 *   id      - 控件 ID (通过 HMENU 参数传递)
 *   text    - 标签文本
 *   x, y, w, h - 控件位置和尺寸 (像素)
 *
 * 返回: 控件窗口句柄
 *
 * Create STATIC label
 */
static HWND CreateLabel(HWND hParent, int id, const char *text,
                        int x, int y, int w, int h) {
    return CreateWindowA("STATIC", text, WS_VISIBLE | WS_CHILD | SS_LEFT,
                         x, y, w, h, hParent, (HMENU)(INT_PTR)id,
                         GetModuleHandle(NULL), NULL);
}

/*
 * 创建按钮控件 — 用于登录/注册/取消等操作
 *
 * 参数:
 *   hParent - 父窗口句柄
 *   id      - 控件 ID (WM_COMMAND 中通过 LOWORD(wParam) 识别)
 *   text    - 按钮文本
 *   x, y, w, h - 控件位置和尺寸
 *
 * 返回: 控件窗口句柄
 *
 * 样式: BS_PUSHBUTTON 标准按钮
 *
 * Create BUTTON
 */
static HWND CreateButton(HWND hParent, int id, const char *text,
                         int x, int y, int w, int h) {
    return CreateWindowA("BUTTON", text,
                         WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                         x, y, w, h, hParent, (HMENU)(INT_PTR)id,
                         GetModuleHandle(NULL), NULL);
}

/*
 * 创建编辑框控件 — 用于文本输入 (用户名/密码/姓名等)
 *
 * 参数:
 *   hParent  - 父窗口句柄
 *   id       - 控件 ID
 *   x, y, w, h - 控件位置和尺寸
 *   password - true 则使用 ES_PASSWORD 样式 (输入字符显示为 *)
 *
 * 返回: 控件窗口句柄
 *
 * 密码框: ES_PASSWORD 样式会自动掩码显示, 但 GetDlgItemTextA 仍返回明文
 *
 * Create EDIT: ES_PASSWORD style when password=true
 */
static HWND CreateEditBox(HWND hParent, int id, int x, int y, int w, int h,
                          bool password) {
    DWORD style = WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL;
    if (password) style |= ES_PASSWORD;   /* 密码输入使用掩码样式 */
    return CreateWindowA("EDIT", "", style,
                         x, y, w, h, hParent, (HMENU)(INT_PTR)id,
                         GetModuleHandle(NULL), NULL);
}

/*
 * 创建单选按钮 — 用于角色选择 (患者/医生/管理员)
 *
 * 参数:
 *   hParent - 父窗口句柄
 *   id      - 控件 ID
 *   text    - 单选按钮文本
 *   x, y, w, h - 控件位置和尺寸
 *   checked - true 则初始状态为选中
 *
 * 返回: 控件窗口句柄
 *
 * 样式: BS_AUTORADIOBUTTON — 自动管理同组单选互斥
 *       首个单选按钮需设置 WS_GROUP 样式以标记组的开始
 *
 * Create radio button: BM_SETCHECK if checked
 */
static HWND CreateRadio(HWND hParent, int id, const char *text,
                        int x, int y, int w, int h, bool checked) {
    DWORD style = WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON;
    HWND hCtrl = CreateWindowA("BUTTON", text, style,
                               x, y, w, h, hParent, (HMENU)(INT_PTR)id,
                               GetModuleHandle(NULL), NULL);
    if (checked)
        SendMessage(hCtrl, BM_SETCHECK, BST_CHECKED, 0);  /* 设置初始选中状态 */
    return hCtrl;
}

/*
 * 创建下拉列表 — 用于科室选择 (从文件加载科室列表)
 *
 * 参数:
 *   hParent - 父窗口句柄
 *   id      - 控件 ID
 *   x, y, w, h - 控件位置和尺寸
 *
 * 返回: 控件窗口句柄
 *
 * 样式: CBS_DROPDOWNLIST — 只读下拉列表 (用户不能输入自定义文本)
 *
 * Create COMBOBOX (dropdown list)
 */
static HWND CreateComboBox(HWND hParent, int id, int x, int y, int w, int h) {
    return CreateWindowA("COMBOBOX", "",
                         WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST |
                         WS_VSCROLL | CBS_HASSTRINGS,
                         x, y, w, h, hParent, (HMENU)(INT_PTR)id,
                         GetModuleHandle(NULL), NULL);
}

/* ==================  登录对话框 / Login Dialog ================== */

/*
 * 显示模态登录对话框
 *
 * 参数:
 *   hInst   - 应用程序实例句柄
 *   hParent - 父窗口句柄 (将被禁用直到对话框关闭)
 *   user    - 输出参数, 成功登录时填充 User 结构体
 *
 * 返回: SUCCESS (0) 登录成功, 或负值错误码
 *
 * 实现流程:
 *   1. 重置全局结果变量 (g_loginResult, g_loginResultCode)
 *   2. 注册自定义窗口类 "LoginDialog"
 *   3. CreateWindowExA 创建对话框窗口
 *   4. 创建子控件: 标题/角色单选/用户名编辑框/密码编辑框/按钮/状态条
 *   5. SetWindowPos 居中对齐到屏幕
 *   6. EnableWindow(hParent, FALSE) 进入模态
 *   7. 消息循环 (GetMessage → IsDialogMessage → Translate/Dispatch)
 *   8. 直到对话框被销毁 (IsWindow 返回 FALSE) 或结果码被设置
 *   9. EnableWindow(hParent, TRUE) 恢复父窗口
 *   10. 将 g_loginResult 拷贝到 *user 参数
 *
 * Show modal login dialog: register class → create controls → center →
 * modal message loop → return result.
 */
int ShowLoginDialog(HINSTANCE hInst, HWND hParent, User *user) {
    /* 重置全局结果 / Reset global result */
    g_loginResultCode = ERROR_INVALID_INPUT;
    memset(&g_loginResult, 0, sizeof(g_loginResult));

    /* 注册自定义窗口类 — 定义窗口外观和行为 / Register custom window class */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = LoginDlgProc;        /* 窗口过程函数 */
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);  /* 标准箭头光标 */
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);   /* 系统窗口背景色 */
    wc.lpszClassName = "LoginDialog";
    RegisterClassA(&wc);

    /* 创建对话框窗口 — 弹出式, 带标题栏和系统菜单 */
    HWND hDlg = CreateWindowExA(0, "LoginDialog", "电子医疗管理系统 - 登录",
                           WS_VISIBLE | WS_POPUPWINDOW | WS_CAPTION |
                           WS_SYSMENU | DS_CENTER,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           DLG_W, DLG_H,
                           hParent, NULL, hInst, NULL);

    if (!hDlg) return ERROR_FILE_IO;        /* 窗口创建失败 */

    /* ===== 创建子控件 ===== */

    /* 标题 — 使用粗体大号字体, Microsoft YaHei UI 18pt */
    CreateLabel(hDlg, IDC_TITLE, "用户登录",
                20, 15, DLG_W - 40, 25);
    HFONT hTitleFont = CreateFontA(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, "Microsoft YaHei UI");
    SendMessage(GetDlgItem(hDlg, IDC_TITLE), WM_SETFONT, (WPARAM)hTitleFont, TRUE);

    /* 角色选择 — 三个单选按钮, 默认选中患者 / Role selection (default: patient) */
    CreateLabel(hDlg, 0, "选择角色:", 20, 50, 80, 20);
    HWND hFirstRadio = CreateRadio(hDlg, IDC_ROLE_PATIENT, "患者", 110, 48, 60, 24, TRUE);
    SetWindowLongA(hFirstRadio, GWL_STYLE, GetWindowLongA(hFirstRadio, GWL_STYLE) | WS_GROUP);
    CreateRadio(hDlg, IDC_ROLE_DOCTOR, "医生", 180, 48, 60, 24, FALSE);
    CreateRadio(hDlg, IDC_ROLE_ADMIN, "管理员", 250, 48, 70, 24, FALSE);

    /* 用户名输入 / Username input */
    CreateLabel(hDlg, 0, "用户名:", 20, 90, 60, 20);
    CreateEditBox(hDlg, IDC_USERNAME, 90, 88, 240, 24, FALSE);

    /* 密码输入 — ES_PASSWORD 样式, 掩码显示 / Password input (masked) */
    CreateLabel(hDlg, 0, "密  码:", 20, 125, 60, 20);
    CreateEditBox(hDlg, IDC_PASSWORD, 90, 123, 240, 24, TRUE);

    /* 分隔线 — SS_ETCHEDHORZ 样式 / Separator line (horizontal etched) */
    CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_ETCHEDHORZ,
                  20, 160, DLG_W - 40, 2, hDlg, NULL, hInst, NULL);

    /* 按钮行 — 登录 / 注册 / 退出 / Buttons: Login / Register / Exit */
    CreateButton(hDlg, IDC_LOGIN, "登录", 50, 180, 80, 30);
    CreateButton(hDlg, IDC_REGISTER, "注册", 145, 180, 80, 30);
    CreateButton(hDlg, IDC_CANCEL, "退出", 240, 180, 80, 30);

    /* 状态条 — 居中显示错误提示文本 (初始为空) / Status bar (initially empty) */
    CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_CENTER,
                  20, 225, DLG_W - 40, 20, hDlg, (HMENU)(INT_PTR)IDC_STATUS,
                  hInst, NULL);

    /* ===== 居中显示到屏幕 / Center on screen ===== */
    RECT rc;
    GetWindowRect(hDlg, &rc);
    int sw = GetSystemMetrics(SM_CXSCREEN);  /* 屏幕宽度 */
    int sh = GetSystemMetrics(SM_CYSCREEN);  /* 屏幕高度 */
    SetWindowPos(hDlg, NULL, (sw - (rc.right - rc.left)) / 2,
                 (sh - (rc.bottom - rc.top)) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);

    /* ===== 模态消息循环 / Modal message loop ===== */
    /*
     * 禁用父窗口: 防止用户在登录完成前与主窗口交互
     * 注意: 这不是真正的 Win32 模态 (不使用 DialogBoxModal),
     *       而是手动实现: disable → loop → re-enable
     *
     * Modal message loop: disable parent, loop until dialog destroyed or result set
     */
    EnableWindow(hParent, FALSE);
    MSG msg;
    BOOL closed = FALSE;
    while (!closed && GetMessage(&msg, NULL, 0, 0)) {
        /* IsDialogMessage: 处理 Tab 导航、默认按钮等对话框键盘操作 */
        if (!IsWindow(hDlg) || !IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);           /* 虚拟键 → 字符消息 */
            DispatchMessage(&msg);            /* 分发到窗口过程 */
        }
        if (!IsWindow(hDlg)) closed = TRUE;   /* 对话框已销毁 / dialog destroyed */
    }

    /* 恢复父窗口: 重新启用并置于前台 */
    if (IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
    }
    if (IsWindow(hDlg)) DestroyWindow(hDlg);  /* 确保对话框被销毁 */

    /* 成功登录时拷贝结果到输出参数 */
    if (g_loginResultCode == SUCCESS && user) {
        *user = g_loginResult;
    }
    return g_loginResultCode;
}

/*
 * 登录对话框窗口过程 — 处理所有 Windows 消息
 *
 * 参数 (标准 Win32 窗口过程签名):
 *   hDlg   - 对话框窗口句柄
 *   msg    - 消息 ID (WM_COMMAND, WM_CLOSE 等)
 *   wParam - 消息参数 1
 *   lParam - 消息参数 2
 *
 * 返回: TRUE=消息已处理, 其他=调用 DefWindowProcA 处理
 *
 * 处理的消息:
 *   WM_COMMAND:
 *     IDC_LOGIN    — 收集输入 → SHA-256 哈希 → 遍历用户列表验证
 *     IDC_REGISTER — 打开注册对话框 (ShowRegisterDialog)
 *     IDC_CANCEL   — 标记取消并关闭对话框
 *   WM_CLOSE — 关闭窗口 (点击 X), 仅在非成功状态下标记为取消
 *
 * Login dialog window procedure
 */
INT_PTR CALLBACK LoginDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_LOGIN: {
            /* ===== 登录验证流程 / Login verification flow ===== */

            /* 1. 收集输入: 用户名/密码/角色 / Gather input: username/password/role */
            char username[50] = {0};
            char password[100] = {0};
            char userRole[20] = {0};

            GetDlgItemTextA(hDlg, IDC_USERNAME, username, sizeof(username));
            GetDlgItemTextA(hDlg, IDC_PASSWORD, password, sizeof(password));

            /* 读取角色单选按钮状态 — 检查哪个被选中 / Read role radio button state */
            if (SendDlgItemMessage(hDlg, IDC_ROLE_ADMIN, BM_GETCHECK, 0, 0) == BST_CHECKED)
                strcpy(userRole, ROLE_ADMIN);
            else if (SendDlgItemMessage(hDlg, IDC_ROLE_DOCTOR, BM_GETCHECK, 0, 0) == BST_CHECKED)
                strcpy(userRole, ROLE_DOCTOR);
            else
                strcpy(userRole, ROLE_PATIENT);  /* 默认患者 */

            /* 2. 必填校验 / Required field check */
            if (strlen(username) == 0 || strlen(password) == 0) {
                SetDlgItemTextA(hDlg, IDC_STATUS, "请输入用户名和密码");
                return TRUE;
            }

            /* 3. 手动验证: 加载用户列表 → 三重匹配 (用户名+角色+密码)
               Manual verification: load users → match username/role/password */
            UserNode *users = load_users_list();
            if (!users) {
                SetDlgItemTextA(hDlg, IDC_STATUS, "用户数据加载失败");
                return TRUE;
            }

            UserNode *cur = users;
            int userFound = 0;              /* 用户名+角色匹配标志 */
            int passwordMatch = 0;          /* 密码匹配标志 */

            /* 对输入密码做 SHA-256 哈希 — 与存储的哈希值比对
               Hash the input password with SHA-256 for comparison */
            uint8_t hash[SHA256_DIGEST_SIZE];
            char hex[SHA256_HEX_SIZE];
            sha256_hash((const uint8_t*)password, strlen(password), hash);
            sha256_hex(hash, hex);

            /* 遍历用户链表 / Walk user list */
            while (cur) {
                /* 用户名匹配 (不区分大小写) 且 角色匹配 */
                if (stricmp(cur->data.username, username) == 0 &&
                    strcmp(cur->data.role, userRole) == 0) {
                    userFound = 1;
                    /* 密码比对 (区分大小写) — 比对哈希值 */
                    if (strcmp(cur->data.password, hex) == 0) {
                        g_loginResult = cur->data;   /* 保存完整用户信息 */
                        passwordMatch = 1;
                        break;                       /* 成功匹配，退出循环 */
                    }
                }
                cur = cur->next;
            }
            free_user_list(users);

            /* 4. 根据验证结果反馈 / Feedback based on verification result */
            if (passwordMatch) {
                g_loginResultCode = SUCCESS;
                PostMessage(hDlg, WM_CLOSE, 0, 0);   /* 触发模态循环退出 */
            } else if (userFound) {
                SetDlgItemTextA(hDlg, IDC_STATUS, "密码错误");
            } else {
                /* 构造详细错误消息: "未找到 '用户名' (角色)" */
                char msg[100];
                snprintf(msg, sizeof(msg), "未找到 '%s' (%s)", username,
                         strcmp(userRole, ROLE_ADMIN) == 0 ? "管理员" :
                         strcmp(userRole, ROLE_DOCTOR) == 0 ? "医生" : "患者");
                SetDlgItemTextA(hDlg, IDC_STATUS, msg);
            }
            return TRUE;
        }

        case IDC_REGISTER:
            /* 打开注册对话框 — 以登录对话框为父窗口 / Open register dialog (modal on top of login) */
            ShowRegisterDialog(GetModuleHandle(NULL), hDlg);
            return TRUE;

        case IDC_CANCEL:
            /* 退出按钮 — 标记取消并关闭 / Cancel button — mark as canceled and close */
            g_loginResultCode = ERROR_INVALID_INPUT;
            PostMessage(hDlg, WM_CLOSE, 0, 0);
            return TRUE;
        }
        return TRUE;

    case WM_CLOSE:
        /* 窗口关闭 — 仅在非成功状态下才标记为取消
           防止 PostMessage(WM_CLOSE) 在成功登录后覆盖结果码 */
        if (g_loginResultCode != SUCCESS)
            g_loginResultCode = ERROR_INVALID_INPUT;
        DestroyWindow(hDlg);
        return TRUE;

    default:
        return DefWindowProcA(hDlg, msg, wParam, lParam);  /* 默认处理 */
    }
}

/* ==================  注册对话框 / Register Dialog ================== */

/*
 * 显示模态注册对话框
 *
 * 参数:
 *   hInst   - 应用程序实例句柄
 *   hParent - 父窗口句柄 (登录对话框)
 *
 * 返回: SUCCESS 或错误码 (当前实现总是返回 ERROR_INVALID_INPUT)
 *
 * 实现流程:
 *   1. 注册自定义窗口类 "RegisterDialog"
 *   2. CreateWindowExA 创建窗口
 *   3. 创建子控件: 标题/用户名/密码/确认密码/角色单选/姓名/科室下拉/按钮/状态条
 *   4. 从文件加载科室列表填充下拉框
 *   5. 初始隐藏科室相关控件 (患者默认选中)
 *   6. 居中对齐 → 模态消息循环
 *
 * Show modal register dialog
 */
int ShowRegisterDialog(HINSTANCE hInst, HWND hParent) {
    /* 注册自定义窗口类 / Register custom window class */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = RegisterDlgProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "RegisterDialog";
    RegisterClassA(&wc);

    HWND hDlg = CreateWindowExA(0, "RegisterDialog", "注册新用户",
                                WS_VISIBLE | WS_POPUPWINDOW | WS_CAPTION |
                                WS_SYSMENU | DS_CENTER,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                REG_W, REG_H,
                                hParent, NULL, hInst, NULL);
    if (!hDlg) return ERROR_FILE_IO;

    /* ===== 逐行排列控件 / Layout controls row by row ===== */
    int y = 20;                           /* 当前 Y 坐标, 逐行递增 */

    /* 标题 / Title */
    CreateLabel(hDlg, 0, "注册新用户", 20, y, REG_W - 40, 25);
    y += 35;

    /* 用户名输入行 / Username row */
    CreateLabel(hDlg, 0, "用户名:", 20, y, 70, 20);
    CreateEditBox(hDlg, IDC_REG_USERNAME, 100, y, 250, 24, FALSE);
    y += 35;

    /* 密码输入行 (掩码显示) / Password row (masked) */
    CreateLabel(hDlg, 0, "密码:", 20, y, 70, 20);
    CreateEditBox(hDlg, IDC_REG_PASSWORD, 100, y, 250, 24, TRUE);
    y += 35;

    /* 确认密码行 (掩码显示) / Confirm password row (masked) */
    CreateLabel(hDlg, 0, "确认密码:", 20, y, 70, 20);
    CreateEditBox(hDlg, IDC_REG_CONFIRM, 100, y, 250, 24, TRUE);
    y += 35;

    /* 角色选择行 — 三个单选按钮, 默认患者 / Role row (default: patient) */
    CreateLabel(hDlg, 0, "角色:", 20, y, 70, 20);
    CreateRadio(hDlg, IDC_REG_ROLE_PATIENT, "患者", 100, y, 60, 24, TRUE);
    CreateRadio(hDlg, IDC_REG_ROLE_DOCTOR, "医生", 170, y, 60, 24, FALSE);
    CreateRadio(hDlg, IDC_REG_ROLE_ADMIN, "管理员", 240, y, 70, 24, FALSE);
    y += 35;

    /* 姓名输入行 / Name row */
    CreateLabel(hDlg, 0, "姓名:", 20, y, 70, 20);
    CreateEditBox(hDlg, IDC_REG_NAME, 100, y, 250, 24, FALSE);
    y += 35;

    /* 科室选择 — 下拉框, 从文件加载科室列表
       初始隐藏 (患者默认选中, 患者不需要科室)
       Department combobox: loaded from file, initially hidden */
    HWND hDeptLabel = CreateLabel(hDlg, IDC_REG_DEPT_LABEL, "科室:", 20, y, 70, 20);
    HWND hDept = CreateComboBox(hDlg, IDC_REG_DEPT, 100, y, 250, 200);
    DepartmentNode *depts = load_departments_list();
    if (depts) {
        DepartmentNode *dc = depts;
        while (dc) {
            /* 将科室名称添加到下拉框 / Add department names to combobox */
            SendMessageA(hDept, CB_ADDSTRING, 0, (LPARAM)dc->data.name);
            dc = dc->next;
        }
        SendMessage(hDept, CB_SETCURSEL, 0, 0);  /* 默认选中第一个科室 */
        free_department_list(depts);
    }
    /* 初始隐藏科室控件 (患者默认, 患者无科室) / Initially hide (default is patient) */
    ShowWindow(hDeptLabel, SW_HIDE);
    ShowWindow(hDept, SW_HIDE);
    y += 35;

    /* 分隔线 + 按钮行 / Separator + button row */
    CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_ETCHEDHORZ,
                  20, y, REG_W - 40, 2, hDlg, NULL, hInst, NULL);
    y += 15;

    CreateButton(hDlg, IDC_REG_OK, "注册", 80, y, 90, 30);
    CreateButton(hDlg, IDC_REG_CANCEL, "取消", 200, y, 90, 30);
    y += 40;

    /* 状态条 — 显示验证错误或成功消息 / Status bar for feedback */
    CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_CENTER,
                  20, y, REG_W - 40, 20, hDlg,
                  (HMENU)(INT_PTR)IDC_REG_STATUS, hInst, NULL);

    /* ===== 居中显示到屏幕 / Center on screen ===== */
    RECT rc;
    GetWindowRect(hDlg, &rc);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hDlg, NULL, (sw - (rc.right - rc.left)) / 2,
                 (sh - (rc.bottom - rc.top)) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);

    /* ===== 模态循环 / Modal loop ===== */
    EnableWindow(hParent, FALSE);         /* 禁用父窗口 */
    MSG msg;
    int result = ERROR_INVALID_INPUT;
    BOOL closed = FALSE;
    while (!closed && GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!IsWindow(hDlg)) closed = TRUE;  /* 检测对话框是否已销毁 / check if destroyed */
    }
    EnableWindow(hParent, TRUE);           /* 恢复父窗口 */
    SetForegroundWindow(hParent);

    return result;
}

/*
 * 注册对话框窗口过程 — 处理注册相关的所有 Windows 消息
 *
 * 参数 (标准 Win32 窗口过程签名):
 *   hDlg   - 对话框窗口句柄
 *   msg    - 消息 ID
 *   wParam - 消息参数 1
 *   lParam - 消息参数 2
 *
 * 处理的消息:
 *   WM_COMMAND:
 *     IDC_REG_ROLE_*  — 切换角色时显示/隐藏科室选择 (仅医生需要)
 *     IDC_REG_OK      — 收集字段 → 逐项验证 → SHA-256 → 保存用户 → 创建档案
 *     IDC_REG_CANCEL  — 关闭对话框
 *   WM_CLOSE — 销毁窗口
 *
 * Register dialog window procedure
 */
INT_PTR CALLBACK RegisterDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        /* ===== 角色切换 — 医生需要科室, 其他角色不需要 ===== */
        case IDC_REG_ROLE_DOCTOR:
        case IDC_REG_ROLE_PATIENT:
        case IDC_REG_ROLE_ADMIN: {
            /* 只有医生需要选科室 — 显示/隐藏科室标签和下拉框
               Only doctors need department selection — toggle visibility */
            int show = (LOWORD(wParam) == IDC_REG_ROLE_DOCTOR) ? SW_SHOW : SW_HIDE;
            ShowWindow(GetDlgItem(hDlg, IDC_REG_DEPT_LABEL), show);
            ShowWindow(GetDlgItem(hDlg, IDC_REG_DEPT), show);
            break;
        }
        case IDC_REG_OK: {
            /* ===== 注册提交流程 / Registration submission flow ===== */

            /* 1. 收集所有字段 / Gather all fields */
            char username[50] = {0};
            char password[100] = {0};
            char confirm[100] = {0};
            char name[100] = {0};
            char role[20] = {0};
            char deptName[100] = {0};

            GetDlgItemTextA(hDlg, IDC_REG_USERNAME, username, sizeof(username));
            GetDlgItemTextA(hDlg, IDC_REG_PASSWORD, password, sizeof(password));
            GetDlgItemTextA(hDlg, IDC_REG_CONFIRM, confirm, sizeof(confirm));
            GetDlgItemTextA(hDlg, IDC_REG_NAME, name, sizeof(name));
            GetDlgItemTextA(hDlg, IDC_REG_DEPT, deptName, sizeof(deptName));

            /* 读取角色 — 检查哪个单选按钮被选中 / Read selected role */
            if (SendDlgItemMessage(hDlg, IDC_REG_ROLE_DOCTOR, BM_GETCHECK, 0, 0) == BST_CHECKED)
                strcpy(role, ROLE_DOCTOR);
            else if (SendDlgItemMessage(hDlg, IDC_REG_ROLE_ADMIN, BM_GETCHECK, 0, 0) == BST_CHECKED)
                strcpy(role, ROLE_ADMIN);
            else
                strcpy(role, ROLE_PATIENT);  /* 默认患者 */

            /* 2. 字段级验证 — 逐项检查, 失败即反馈并返回 / Field validation */

            /* 必填字段检查 / Required fields */
            if (strlen(username) == 0 || strlen(password) == 0 || strlen(name) == 0) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "请填写必填字段");
                return TRUE;
            }
            /* 用户名最小长度 / Username minimum length */
            if (strlen(username) < 3) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "用户名至少3个字符");
                return TRUE;
            }
            /* 密码最小长度 / Password minimum length */
            if (strlen(password) < 6) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "密码至少6个字符");
                return TRUE;
            }
            /* 两次密码一致性 / Password confirmation match */
            if (strcmp(password, confirm) != 0) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "两次密码不一致");
                return TRUE;
            }

            /* 3. 用户名唯一性检查 — 遍历现有用户列表 / Username uniqueness check */
            UserNode *users = load_users_list();
            if (users) {
                UserNode *cur = users;
                while (cur) {
                    if (strcmp(cur->data.username, username) == 0) {
                        SetDlgItemTextA(hDlg, IDC_REG_STATUS, "用户名已存在");
                        free_user_list(users);
                        return TRUE;
                    }
                    cur = cur->next;
                }
                free_user_list(users);
            }

            /* 4. SHA-256 哈希密码 → 构建新用户 → 保存
               Hash password with SHA-256 → create User → save to file */
            User newUser;
            memset(&newUser, 0, sizeof(newUser));
            strcpy(newUser.username, username);
            strcpy(newUser.role, role);
            /* 计算 SHA-256 哈希 / Compute SHA-256 hash */
            uint8_t hash[SHA256_DIGEST_SIZE];
            char hex[SHA256_HEX_SIZE];
            sha256_hash((const uint8_t*)password, strlen(password), hash);
            sha256_hex(hash, hex);
            strcpy(newUser.password, hex);   /* 存储哈希值, 非明文 */

            /* 5. 追加到用户列表并保存 / Append to user list and save */
            UserNode *allUsers = load_users_list();
            UserNode *newNode = create_user_node(&newUser);
            if (!newNode) {
                free_user_list(allUsers);
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "内存不足");
                return TRUE;
            }
            newNode->next = allUsers;        /* 新节点插在链表头部 / prepend new node */
            if (save_users_list(newNode) != SUCCESS) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "保存失败");
                free_user_list(newNode);
                return TRUE;
            }
            free_user_list(newNode);

            /* 6. 根据角色创建对应档案 / Create profile based on role
               - 患者: 调用 ensure_patient_profile 创建患者记录
               - 医生: 从科室名查找科室 ID → create_doctor_profile_with_details
               - 管理员: 无需创建额外档案 */
            if (strcmp(role, ROLE_PATIENT) == 0) {
                /* 自动创建患者档案 / Auto-create patient profile */
                ensure_patient_profile(username);
            } else if (strcmp(role, ROLE_DOCTOR) == 0) {
                /* 从科室名称查找科室 ID — 遍历科室链表匹配名称
                   Look up department ID from name — walk department list */
                DepartmentNode *depts = load_departments_list();
                char deptId[20] = {0};
                if (depts) {
                    DepartmentNode *dc = depts;
                    while (dc) {
                        if (strcmp(dc->data.name, deptName) == 0) {
                            strcpy(deptId, dc->data.department_id);
                            break;
                        }
                        dc = dc->next;
                    }
                    free_department_list(depts);
                }
                /* 创建医生档案 (含姓名、职称、科室) */
                create_doctor_profile_with_details(username, name, "医师", deptId);
            }

            /* 7. 成功 → 提示用户 → 关闭对话框 / Success → notify → close */
            SetDlgItemTextA(hDlg, IDC_REG_STATUS, "注册成功！");
            MessageBoxA(hDlg, "注册成功，请返回登录", "成功", MB_OK | MB_ICONINFORMATION);
            DestroyWindow(hDlg);
            return TRUE;
        }

        case IDC_REG_CANCEL:
            /* 取消按钮 — 直接关闭对话框 / Cancel button — close dialog */
            DestroyWindow(hDlg);
            return TRUE;
        }
        return TRUE;

    case WM_CLOSE:
        /* 窗口关闭 (点击 X) — 销毁窗口 */
        DestroyWindow(hDlg);
        return TRUE;

    default:
        return DefWindowProcA(hDlg, msg, wParam, lParam);
    }
}
