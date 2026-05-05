/*
 * gui_login.c — GUI 登录与注册对话框 / GUI login & registration dialogs
 *
 * 实现 Win32 对话框形式的登录和注册界面。
 *
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
 * Implements login and registration as Win32 custom-drawn dialogs
 * with modal message loops (not DialogBox-based) for fine control.
 */

#include "gui_login.h"
#include "data_storage.h"
#include "sha256.h"
#include "public.h"
#include <commctrl.h>

/* ==================  控件 ID 常量 / Control ID Constants ================== */

/* 登录对话框 / Login dialog */
#define IDC_ROLE_ADMIN    101    /* 管理员单选 */
#define IDC_ROLE_DOCTOR   102    /* 医生单选 */
#define IDC_ROLE_PATIENT  103    /* 患者单选 */
#define IDC_USERNAME      104    /* 用户名编辑框 */
#define IDC_PASSWORD      105    /* 密码编辑框 */
#define IDC_LOGIN         106    /* 登录按钮 */
#define IDC_REGISTER      107    /* 注册按钮 */
#define IDC_CANCEL        108    /* 退出按钮 */
#define IDC_STATUS        109    /* 状态/错误文本 */
#define IDC_TITLE         110    /* 标题标签 */

/* 注册对话框 / Register dialog */
#define IDC_REG_USERNAME  201    /* 用户名 */
#define IDC_REG_PASSWORD  202    /* 密码 */
#define IDC_REG_CONFIRM   203    /* 确认密码 */
#define IDC_REG_ROLE_ADMIN   204 /* 管理员单选 */
#define IDC_REG_ROLE_DOCTOR  205 /* 医生单选 */
#define IDC_REG_ROLE_PATIENT 206 /* 患者单选 */
#define IDC_REG_DEPT      207    /* 科室下拉框 */
#define IDC_REG_DEPT_LABEL 213   /* 科室标签 */
#define IDC_REG_NAME      208    /* 姓名 */
#define IDC_REG_TITLE     209    /* 职称 (预留) */
#define IDC_REG_OK        210    /* 注册按钮 */
#define IDC_REG_CANCEL    211    /* 取消按钮 */
#define IDC_REG_STATUS    212    /* 状态文本 */

/* 对话框尺寸 / Dialog dimensions (pixel approximations) */
#define DLG_W 360
#define DLG_H 320
#define REG_W 400
#define REG_H 400

/* 全局登录结果 (模态循环通信) / Global login result (modal loop communication) */
static User g_loginResult;
static int g_loginResultCode = ERROR_INVALID_INPUT;

/* ==================  控件创建工具函数 / Control Creation Helpers ================== */

/* 创建静态标签 / Create STATIC label */
static HWND CreateLabel(HWND hParent, int id, const char *text,
                        int x, int y, int w, int h) {
    return CreateWindowA("STATIC", text, WS_VISIBLE | WS_CHILD | SS_LEFT,
                         x, y, w, h, hParent, (HMENU)(INT_PTR)id,
                         GetModuleHandle(NULL), NULL);
}

/* 创建按钮 / Create BUTTON */
static HWND CreateButton(HWND hParent, int id, const char *text,
                         int x, int y, int w, int h) {
    return CreateWindowA("BUTTON", text,
                         WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                         x, y, w, h, hParent, (HMENU)(INT_PTR)id,
                         GetModuleHandle(NULL), NULL);
}

/* 创建编辑框: password=true 则使用 ES_PASSWORD 掩码样式
   Create EDIT: ES_PASSWORD style when password=true */
static HWND CreateEditBox(HWND hParent, int id, int x, int y, int w, int h,
                          bool password) {
    DWORD style = WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL;
    if (password) style |= ES_PASSWORD;
    return CreateWindowA("EDIT", "", style,
                         x, y, w, h, hParent, (HMENU)(INT_PTR)id,
                         GetModuleHandle(NULL), NULL);
}

/* 创建单选按钮: checked=true 则初始选中
   Create radio button: BM_SETCHECK if checked */
static HWND CreateRadio(HWND hParent, int id, const char *text,
                        int x, int y, int w, int h, bool checked) {
    DWORD style = WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON;
    HWND hCtrl = CreateWindowA("BUTTON", text, style,
                               x, y, w, h, hParent, (HMENU)(INT_PTR)id,
                               GetModuleHandle(NULL), NULL);
    if (checked)
        SendMessage(hCtrl, BM_SETCHECK, BST_CHECKED, 0);
    return hCtrl;
}

/* 创建下拉框 / Create COMBOBOX (dropdown list) */
static HWND CreateComboBox(HWND hParent, int id, int x, int y, int w, int h) {
    return CreateWindowA("COMBOBOX", "",
                         WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST |
                         WS_VSCROLL | CBS_HASSTRINGS,
                         x, y, w, h, hParent, (HMENU)(INT_PTR)id,
                         GetModuleHandle(NULL), NULL);
}

/* ==================  登录对话框 / Login Dialog ================== */

/* 显示模态登录对话框:
   注册自定义窗口类 → 创建控件 → 居中 → 模态消息循环 → 返回结果
   Show modal login dialog: register class → create controls → center →
   modal message loop → return result. */
int ShowLoginDialog(HINSTANCE hInst, HWND hParent, User *user) {
    g_loginResultCode = ERROR_INVALID_INPUT;
    memset(&g_loginResult, 0, sizeof(g_loginResult));

    /* 注册自定义窗口类 / Register custom window class */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = LoginDlgProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "LoginDialog";
    RegisterClassA(&wc);

    HWND hDlg = CreateWindowExA(0, "LoginDialog", "电子医疗管理系统 - 登录",
                           WS_VISIBLE | WS_POPUPWINDOW | WS_CAPTION |
                           WS_SYSMENU | DS_CENTER,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           DLG_W, DLG_H,
                           hParent, NULL, hInst, NULL);

    if (!hDlg) return ERROR_FILE_IO;

    /* 标题 — 使用粗体大号字体 / Title with bold large font */
    CreateLabel(hDlg, IDC_TITLE, "用户登录",
                20, 15, DLG_W - 40, 25);
    HFONT hTitleFont = CreateFontA(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, "Microsoft YaHei UI");
    SendMessage(GetDlgItem(hDlg, IDC_TITLE), WM_SETFONT, (WPARAM)hTitleFont, TRUE);

    /* 角色选择 / Role selection (默认选中患者) */
    CreateLabel(hDlg, 0, "选择角色:", 20, 50, 80, 20);
    CreateRadio(hDlg, IDC_ROLE_PATIENT, "患者", 110, 48, 60, 24, TRUE);
    CreateRadio(hDlg, IDC_ROLE_DOCTOR, "医生", 180, 48, 60, 24, FALSE);
    CreateRadio(hDlg, IDC_ROLE_ADMIN, "管理员", 250, 48, 70, 24, FALSE);

    /* 用户名 / Username */
    CreateLabel(hDlg, 0, "用户名:", 20, 90, 60, 20);
    CreateEditBox(hDlg, IDC_USERNAME, 90, 88, 240, 24, FALSE);

    /* 密码 / Password */
    CreateLabel(hDlg, 0, "密  码:", 20, 125, 60, 20);
    CreateEditBox(hDlg, IDC_PASSWORD, 90, 123, 240, 24, TRUE);

    /* 分隔线 / Separator line */
    CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_ETCHEDHORZ,
                  20, 160, DLG_W - 40, 2, hDlg, NULL, hInst, NULL);

    /* 按钮 / Buttons */
    CreateButton(hDlg, IDC_LOGIN, "登录", 50, 180, 80, 30);
    CreateButton(hDlg, IDC_REGISTER, "注册", 145, 180, 80, 30);
    CreateButton(hDlg, IDC_CANCEL, "退出", 240, 180, 80, 30);

    /* 状态条 (显示错误信息) / Status bar (shows error messages) */
    CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_CENTER,
                  20, 225, DLG_W - 40, 20, hDlg, (HMENU)(INT_PTR)IDC_STATUS,
                  hInst, NULL);

    /* 居中显示 / Center on screen */
    RECT rc;
    GetWindowRect(hDlg, &rc);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hDlg, NULL, (sw - (rc.right - rc.left)) / 2,
                 (sh - (rc.bottom - rc.top)) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);

    /* 模态消息循环: 禁用父窗口，直到对话框被销毁或结果码变更
       Modal message loop: disable parent, loop until dialog destroyed or result set */
    EnableWindow(hParent, FALSE);
    MSG msg;
    BOOL closed = FALSE;
    while (!closed && GetMessage(&msg, NULL, 0, 0)) {
        if (!IsWindow(hDlg) || !IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!IsWindow(hDlg)) closed = TRUE;  /* 对话框已销毁 / dialog destroyed */
    }

    if (IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
    }
    if (IsWindow(hDlg)) DestroyWindow(hDlg);

    if (g_loginResultCode == SUCCESS && user) {
        *user = g_loginResult;
    }
    return g_loginResultCode;
}

/* 登录对话框窗口过程 / Login dialog window procedure */
INT_PTR CALLBACK LoginDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_LOGIN: {
            /* 收集输入: 用户名/密码/角色 / Gather input */
            char username[50] = {0};
            char password[100] = {0};
            char userRole[20] = {0};

            GetDlgItemTextA(hDlg, IDC_USERNAME, username, sizeof(username));
            GetDlgItemTextA(hDlg, IDC_PASSWORD, password, sizeof(password));

            /* 读取角色单选按钮状态 / Read role radio button state */
            if (SendDlgItemMessage(hDlg, IDC_ROLE_ADMIN, BM_GETCHECK, 0, 0) == BST_CHECKED)
                strcpy(userRole, ROLE_ADMIN);
            else if (SendDlgItemMessage(hDlg, IDC_ROLE_DOCTOR, BM_GETCHECK, 0, 0) == BST_CHECKED)
                strcpy(userRole, ROLE_DOCTOR);
            else
                strcpy(userRole, ROLE_PATIENT);

            if (strlen(username) == 0 || strlen(password) == 0) {
                SetDlgItemTextA(hDlg, IDC_STATUS, "请输入用户名和密码");
                return TRUE;
            }

            /* 手动验证: 加载用户列表 → SHA-256 哈希密码 → 三重匹配
               Manual verification: load users → hash → triple match */
            UserNode *users = load_users_list();
            if (!users) {
                SetDlgItemTextA(hDlg, IDC_STATUS, "用户数据加载失败");
                return TRUE;
            }

            UserNode *cur = users;
            int found = 0;
            while (cur) {
                if (strcmp(cur->data.username, username) == 0 &&
                    strcmp(cur->data.role, userRole) == 0) {
                    /* 对输入密码做 SHA-256 → 与存储哈希比对 */
                    uint8_t hash[SHA256_DIGEST_SIZE];
                    char hex[SHA256_HEX_SIZE];
                    sha256_hash((const uint8_t*)password, strlen(password), hash);
                    sha256_hex(hash, hex);

                    if (strcmp(cur->data.password, hex) == 0) {
                        g_loginResult = cur->data;
                        found = 1;
                    }
                    break;
                }
                cur = cur->next;
            }
            free_user_list(users);

            if (found) {
                g_loginResultCode = SUCCESS;
                PostMessage(hDlg, WM_CLOSE, 0, 0);  /* 触发模态循环退出 */
            } else {
                SetDlgItemTextA(hDlg, IDC_STATUS,
                    "用户名或密码错误，请重试");
            }
            return TRUE;
        }

        case IDC_REGISTER:
            /* 打开注册对话框 / Open register dialog (modal on top of login) */
            ShowRegisterDialog(GetModuleHandle(NULL), hDlg);
            return TRUE;

        case IDC_CANCEL:
            g_loginResultCode = ERROR_INVALID_INPUT;
            PostMessage(hDlg, WM_CLOSE, 0, 0);
            return TRUE;
        }
        return TRUE;

    case WM_CLOSE:
        /* 仅在非成功状态下才标记为取消 / Only mark as cancel if not already successful */
        if (g_loginResultCode != SUCCESS)
            g_loginResultCode = ERROR_INVALID_INPUT;
        DestroyWindow(hDlg);
        return TRUE;

    default:
        return DefWindowProcA(hDlg, msg, wParam, lParam);
    }
}

/* ==================  注册对话框 / Register Dialog ================== */

/* 显示模态注册对话框 / Show modal register dialog */
int ShowRegisterDialog(HINSTANCE hInst, HWND hParent) {
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

    /* 逐行排列控件 / Layout controls row by row */
    int y = 20;
    CreateLabel(hDlg, 0, "注册新用户", 20, y, REG_W - 40, 25);
    y += 35;

    /* 用户名 / Username */
    CreateLabel(hDlg, 0, "用户名:", 20, y, 70, 20);
    CreateEditBox(hDlg, IDC_REG_USERNAME, 100, y, 250, 24, FALSE);
    y += 35;

    /* 密码 / Password */
    CreateLabel(hDlg, 0, "密码:", 20, y, 70, 20);
    CreateEditBox(hDlg, IDC_REG_PASSWORD, 100, y, 250, 24, TRUE);
    y += 35;

    /* 确认密码 / Confirm password */
    CreateLabel(hDlg, 0, "确认密码:", 20, y, 70, 20);
    CreateEditBox(hDlg, IDC_REG_CONFIRM, 100, y, 250, 24, TRUE);
    y += 35;

    /* 角色 / Role */
    CreateLabel(hDlg, 0, "角色:", 20, y, 70, 20);
    CreateRadio(hDlg, IDC_REG_ROLE_PATIENT, "患者", 100, y, 60, 24, TRUE);
    CreateRadio(hDlg, IDC_REG_ROLE_DOCTOR, "医生", 170, y, 60, 24, FALSE);
    CreateRadio(hDlg, IDC_REG_ROLE_ADMIN, "管理员", 240, y, 70, 24, FALSE);
    y += 35;

    /* 姓名 / Name */
    CreateLabel(hDlg, 0, "姓名:", 20, y, 70, 20);
    CreateEditBox(hDlg, IDC_REG_NAME, 100, y, 250, 24, FALSE);
    y += 35;

    /* 科室 (下拉框，从文件加载) / Department (combo box, loaded from file) */
    HWND hDeptLabel = CreateLabel(hDlg, IDC_REG_DEPT_LABEL, "科室:", 20, y, 70, 20);
    HWND hDept = CreateComboBox(hDlg, IDC_REG_DEPT, 100, y, 250, 200);
    DepartmentNode *depts = load_departments_list();
    if (depts) {
        DepartmentNode *dc = depts;
        while (dc) {
            SendMessageA(hDept, CB_ADDSTRING, 0, (LPARAM)dc->data.name);
            dc = dc->next;
        }
        SendMessage(hDept, CB_SETCURSEL, 0, 0);  /* 默认选中第一个 */
        free_department_list(depts);
    }
    ShowWindow(hDeptLabel, SW_HIDE);
    ShowWindow(hDept, SW_HIDE);
    y += 35;

    /* 分隔线 + 按钮 / Separator + buttons */
    CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_ETCHEDHORZ,
                  20, y, REG_W - 40, 2, hDlg, NULL, hInst, NULL);
    y += 15;

    CreateButton(hDlg, IDC_REG_OK, "注册", 80, y, 90, 30);
    CreateButton(hDlg, IDC_REG_CANCEL, "取消", 200, y, 90, 30);
    y += 40;

    /* 状态条 / Status bar */
    CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_CENTER,
                  20, y, REG_W - 40, 20, hDlg,
                  (HMENU)(INT_PTR)IDC_REG_STATUS, hInst, NULL);

    /* 居中 / Center on screen */
    RECT rc;
    GetWindowRect(hDlg, &rc);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hDlg, NULL, (sw - (rc.right - rc.left)) / 2,
                 (sh - (rc.bottom - rc.top)) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);

    /* 模态循环 / Modal loop */
    EnableWindow(hParent, FALSE);
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
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);

    return result;
}

/* 注册对话框窗口过程 / Register dialog window procedure */
INT_PTR CALLBACK RegisterDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_REG_ROLE_DOCTOR:
        case IDC_REG_ROLE_PATIENT:
        case IDC_REG_ROLE_ADMIN: {
            /* 只有医生需要选科室 / Only doctors need department selection */
            int show = (LOWORD(wParam) == IDC_REG_ROLE_DOCTOR) ? SW_SHOW : SW_HIDE;
            ShowWindow(GetDlgItem(hDlg, IDC_REG_DEPT_LABEL), show);
            ShowWindow(GetDlgItem(hDlg, IDC_REG_DEPT), show);
            break;
        }
        case IDC_REG_OK: {
            /* 收集所有字段 / Gather all fields */
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

            /* 读取角色 / Read role */
            if (SendDlgItemMessage(hDlg, IDC_REG_ROLE_DOCTOR, BM_GETCHECK, 0, 0) == BST_CHECKED)
                strcpy(role, ROLE_DOCTOR);
            else if (SendDlgItemMessage(hDlg, IDC_REG_ROLE_ADMIN, BM_GETCHECK, 0, 0) == BST_CHECKED)
                strcpy(role, ROLE_ADMIN);
            else
                strcpy(role, ROLE_PATIENT);

            /* 字段级验证 / Field validation */
            if (strlen(username) == 0 || strlen(password) == 0 || strlen(name) == 0) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "请填写必填字段");
                return TRUE;
            }
            if (strlen(username) < 3) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "用户名至少3个字符");
                return TRUE;
            }
            if (strlen(password) < 6) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "密码至少6个字符");
                return TRUE;
            }
            if (strcmp(password, confirm) != 0) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "两次密码不一致");
                return TRUE;
            }

            /* 用户名唯一性检查 / Username uniqueness check */
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

            /* SHA-256 哈希密码 → 创建用户 → 保存
               Hash password → create user → save */
            User newUser;
            memset(&newUser, 0, sizeof(newUser));
            strcpy(newUser.username, username);
            strcpy(newUser.role, role);
            uint8_t hash[SHA256_DIGEST_SIZE];
            char hex[SHA256_HEX_SIZE];
            sha256_hash((const uint8_t*)password, strlen(password), hash);
            sha256_hex(hash, hex);
            strcpy(newUser.password, hex);

            /* 追加到用户列表并保存 / Append to user list and save */
            UserNode *allUsers = load_users_list();
            UserNode *newNode = create_user_node(&newUser);
            if (!newNode) {
                free_user_list(allUsers);
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "内存不足");
                return TRUE;
            }
            newNode->next = allUsers;     /* 新节点插在链表头部 / prepend new node */
            if (save_users_list(newNode) != SUCCESS) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "保存失败");
                free_user_list(newNode);
                return TRUE;
            }
            free_user_list(newNode);

            /* 根据角色创建对应档案 / Create profile based on role */
            if (strcmp(role, ROLE_PATIENT) == 0) {
                ensure_patient_profile(username);
            } else if (strcmp(role, ROLE_DOCTOR) == 0) {
                /* 从科室名称查找科室 ID / Look up department ID from name */
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
                create_doctor_profile_with_details(username, name, "医师", deptId);
            }

            /* 成功 → 提示 → 关闭 / Success → notify → close */
            SetDlgItemTextA(hDlg, IDC_REG_STATUS, "注册成功！");
            MessageBoxA(hDlg, "注册成功，请返回登录", "成功", MB_OK | MB_ICONINFORMATION);
            DestroyWindow(hDlg);
            return TRUE;
        }

        case IDC_REG_CANCEL:
            DestroyWindow(hDlg);
            return TRUE;
        }
        return TRUE;

    case WM_CLOSE:
        DestroyWindow(hDlg);
        return TRUE;

    default:
        return DefWindowProcA(hDlg, msg, wParam, lParam);
    }
}
