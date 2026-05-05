/*
 * gui_main.c — Win32 GUI 主窗口实现 / Win32 GUI main window implementation
 *
 * 实现 GUI 版本的主窗口，提供与 console 版本相同的功能但通过 Win32 原生控件:
 *   - 左侧导航树 (TreeView) — 按角色显示不同的功能节点
 *   - 右侧内容区 — 根据导航选择动态切换页面 (患者/医生/管理员视图)
 *   - 顶部注销按钮 + 底部状态栏 (StatusBar)
 *   - 布局自适应窗口大小调整 (OnSize)
 *
 * 启动流程: 初始化数据 → 创建主窗口 → 显示登录对话框 → 填充导航树 →
 * 切换到首个页面 → 进入消息循环。
 *
 * Each role page is created by a factory function (CreatePatientPage /
 * CreateDoctorPage / CreateAdminPage) based on the selected nav item.
 *
 * Implements Win32 GUI main window with TreeView navigation, dynamic page
 * switching, status bar, and responsive layout.
 */

#include "gui_main.h"
#include "gui_login.h"
#include "gui_patient.h"
#include "gui_doctor.h"
#include "gui_admin.h"
#include "data_storage.h"
#include "login.h"
#include "public.h"
#include "sha256.h"
#include <sys/stat.h>   /* stat() — 文件修改时间轮询 / file mtime polling */

/* 轮询刷新常量 / Polling refresh constants */
#define REFRESH_TIMER_ID    10
#define REFRESH_INTERVAL_MS 3000   /* 3 秒轮询 / 3-second poll */

/* ==================  全局变量 / Global Variables ================== */

HINSTANCE g_hInst = NULL;              /* 应用实例句柄 / application instance */
User g_currentUser = {0};              /* 当前登录用户 / current logged-in user */
static HWND g_hMainWnd = NULL;         /* 主窗口句柄 / main window handle */
static HWND g_hNavTree = NULL;         /* 左侧导航树 / left nav tree */
static HWND g_hContentView = NULL;     /* 右侧内容区 / right content area */
static HWND g_hStatusBar = NULL;       /* 底部状态栏 / bottom status bar */
static HWND g_hLogoutBtn = NULL;       /* 注销按钮 / logout button */
static int g_currentView = 0;          /* 当前视图 ID / current view ID */

/* 文件修改时间缓存 — 跨实例变更检测 / Cached mtimes for cross-instance detection */
static time_t g_lastApptMtime   = 0;   /* appointments.txt */
static time_t g_lastRecordMtime = 0;   /* medical_records.txt */
static time_t g_lastRxMtime     = 0;   /* prescriptions.txt */
static time_t g_lastWardMtime   = 0;   /* wards.txt */

/* 前向声明 / Forward declarations */
static BOOL InitMainWindow(HINSTANCE hInst, int nCmdShow);
static HWND CreateNavTree(HWND hParent);
static void PopulateNavTree(void);
static void OnNavSelChange(HWND hWnd, LPARAM lParam);
static void OnSize(HWND hWnd, UINT state, int cx, int cy);

/* ==================  WinMain 入口 / WinMain Entry ================== */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    MSG msg;

    (void)hPrevInstance;
    (void)lpCmdLine;

    /* 初始化通用控件库 (TreeView, ListView, Tab, Progress 等)
       Initialize Common Controls library for TreeView, ListView, etc. */
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES |
                ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    /* 初始化数据存储与默认用户 (同 console main.c 逻辑)
       Init data storage & default users (same logic as console main.c) */
    init_console_encoding();
    if (init_data_storage() != SUCCESS) {
        MessageBoxA(NULL, "数据存储初始化失败!", "错误", MB_ICONERROR);
        return 1;
    }

    /* 创建默认用户或迁移密码 / Create defaults or migrate passwords */
    UserNode *head = load_users_list();
    if (!head) {
        /* 首次运行: admin/doctor1/patient1，密码=用户名+123 的 SHA-256 哈希
           First run: create default users with SHA-256 hashed passwords */
        User default_users[3];
        const char *usernames[3] = { "admin", "doctor1", "patient1" };
        const char *plain_pwds[3] = { "admin123", "doctor123", "patient123" };
        const char *roles[3] = { ROLE_ADMIN, ROLE_DOCTOR, ROLE_PATIENT };
        for (int i = 0; i < 3; i++) {
            memset(&default_users[i], 0, sizeof(User));
            strcpy(default_users[i].username, usernames[i]);
            strcpy(default_users[i].role, roles[i]);
            uint8_t hash[SHA256_DIGEST_SIZE];
            char hex[SHA256_HEX_SIZE];
            sha256_hash((const uint8_t*)plain_pwds[i], strlen(plain_pwds[i]), hash);
            sha256_hex(hash, hex);
            strcpy(default_users[i].password, hex);
        }
        UserNode *dh = NULL, *tail = NULL;
        for (int i = 0; i < 3; i++) {
            UserNode *node = create_user_node(&default_users[i]);
            if (!node) { free_user_list(dh); return 1; }
            if (!dh) { dh = node; tail = node; }
            else { tail->next = node; tail = node; }
        }
        save_users_list(dh);
        free_user_list(dh);
        ensure_doctor_profile("doctor1");
        ensure_patient_profile("patient1");
    } else {
        migrate_user_passwords();

        /* 确保默认账号始终存在 / Ensure default accounts always exist */
        {
            const char *defUsers[3] = { "admin", "doctor1", "patient1" };
            const char *defPwds[3]  = { "admin123", "doctor123", "patient123" };
            const char *defRoles[3] = { ROLE_ADMIN, ROLE_DOCTOR, ROLE_PATIENT };
            for (int i = 0; i < 3; i++) {
                int exists = 0;
                for (UserNode *u = head; u; u = u->next) {
                    if (strcmp(u->data.username, defUsers[i]) == 0 &&
                        strcmp(u->data.role, defRoles[i]) == 0) {
                        exists = 1; break;
                    }
                }
                if (!exists) {
                    User defU;
                    memset(&defU, 0, sizeof(defU));
                    strcpy(defU.username, defUsers[i]);
                    strcpy(defU.role, defRoles[i]);
                    uint8_t h[SHA256_DIGEST_SIZE];
                    char hx[SHA256_HEX_SIZE];
                    sha256_hash((const uint8_t*)defPwds[i], strlen(defPwds[i]), h);
                    sha256_hex(h, hx);
                    strcpy(defU.password, hx);
                    UserNode *node = create_user_node(&defU);
                    if (node) {
                        node->next = head;
                        head = node;
                        save_users_list(head);
                    }
                }
            }
        }

        /* 批量确保档案存在: 每个文件只加载一次，检查所有用户
           Batch ensure profiles: load each file once, check all users */
        batch_ensure_doctor_profiles(head);
        batch_ensure_patient_profiles(head);

        free_user_list(head);
    }
    migrate_doctor_ids();       /* 迁移旧格式医生 ID */
    ensure_default_templates(); /* 初始化 18 个默认模板 */

    g_hInst = hInstance;

    /* 初始化文件修改时间缓存，避免首次定时器误判为变更
       Init mtime caches so the first timer tick doesn't trigger a spurious refresh */
    {
        struct stat st;
        if (stat(APPOINTMENTS_FILE, &st) == 0)   g_lastApptMtime   = st.st_mtime;
        if (stat(MEDICAL_RECORDS_FILE, &st) == 0) g_lastRecordMtime = st.st_mtime;
        if (stat(PRESCRIPTIONS_FILE, &st) == 0)   g_lastRxMtime     = st.st_mtime;
        if (stat(WARDS_FILE, &st) == 0)           g_lastWardMtime   = st.st_mtime;
    }

    /* 创建主窗口 / Create main window */
    if (!InitMainWindow(hInstance, nCmdShow))
        return 1;

    /* 显示登录对话框 (模态循环) / Show login dialog (modal loop) */
    User loggedUser;
    if (ShowLoginDialog(hInstance, g_hMainWnd, &loggedUser) != SUCCESS) {
        DestroyWindow(g_hMainWnd);
        return 0;  /* 用户取消登录 → 退出 / user cancelled → exit */
    }
    g_currentUser = loggedUser;

    /* 登录成功 → 显示主窗口 / Login successful → show main window */
    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    /* 按角色填充导航树 / Populate nav tree for the logged-in role */
    PopulateNavTree();

    /* 根据角色切换到首个页面 / Switch to first page based on role */
    int firstView = 0;
    if (strcmp(loggedUser.role, ROLE_PATIENT) == 0)
        firstView = NAV_PATIENT_REGISTER;
    else if (strcmp(loggedUser.role, ROLE_DOCTOR) == 0)
        firstView = NAV_DOCTOR_REMINDER;
    else if (strcmp(loggedUser.role, ROLE_ADMIN) == 0)
        firstView = NAV_ADMIN_DEPT;
    SwitchView(g_hMainWnd, firstView);

    /* 更新状态栏 / Update status bar */
    char status[256];
    snprintf(status, sizeof(status), "  用户: %s  |  角色: %s",
             loggedUser.username, loggedUser.role);
    SetStatusText(g_hMainWnd, status);

    /* Win32 标准消息循环 / Standard Win32 message loop */
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

/* ==================  初始化主窗口 / Initialize Main Window ================== */

/* 注册窗口类 → 创建主窗口 (初始隐藏, 登录成功后才显示)
   Register window class → create → initially hidden, shown after successful login */
static BOOL InitMainWindow(HINSTANCE hInst, int nCmdShow) {
    const char CLASS_NAME[] = "EMSMainWindow";

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconA(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);  /* 系统默认背景色 */
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClassA(&wc))
        return FALSE;

    HWND hWnd = CreateWindowExA(
        0, CLASS_NAME, "电子医疗管理系统",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,    /* 可调大小的窗口 / resizable window */
        CW_USEDEFAULT, CW_USEDEFAULT,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        NULL, NULL, hInst, NULL
    );

    if (!hWnd)
        return FALSE;

    g_hMainWnd = hWnd;
    /* 初始隐藏主窗口，登录成功后再显示
       Keep main window hidden until successful login */
    (void)nCmdShow;
    return TRUE;
}

/* ==================  导航树 / Navigation Tree ================== */

/* 创建 TreeView 控件 (左侧导航栏) / Create TreeView control (left nav) */
static HWND CreateNavTree(HWND hParent) {
    HWND hTree = CreateWindowA(
        WC_TREEVIEWA, "", WS_VISIBLE | WS_CHILD | TVS_HASLINES |
        TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
        0, 0, NAV_WIDTH, 100,
        hParent, (HMENU)1, g_hInst, NULL
    );
    return hTree;
}

/* 根据当前用户角色填充导航树节点 / Populate nav tree based on current role */
static void PopulateNavTree(void) {
    if (!g_hNavTree) return;

    TreeView_DeleteAllItems(g_hNavTree);  /* 清空旧的节点 / clear old items */

    HTREEITEM hRoot = NULL;
    TV_INSERTSTRUCTA tvis = {0};
    tvis.hInsertAfter = TVI_LAST;
    tvis.item.mask = TVIF_TEXT | TVIF_PARAM;  /* 各节点通过 lParam 传递导航 ID */

    const char *role = g_currentUser.role;

    /* 患者导航: 挂号/预约查询/诊断/处方/住院/进度/资料 (7项)
       Patient nav: register/appointment/diagnosis/prescription/ward/progress/profile */
    if (strcmp(role, ROLE_PATIENT) == 0) {
        tvis.hParent = NULL;
        tvis.item.pszText = (LPSTR)"患者系统";
        tvis.item.lParam = 0;
        hRoot = TreeView_InsertItem(g_hNavTree, &tvis);

        struct { const char *name; int id; } items[] = {
            {"挂号",          NAV_PATIENT_REGISTER},
            {"预约查询",      NAV_PATIENT_APPOINTMENT},
            {"诊断结果",      NAV_PATIENT_DIAGNOSIS},
            {"处方查询",      NAV_PATIENT_PRESCRIPTION},
            {"住院信息",      NAV_PATIENT_WARD},
            {"治疗进度",      NAV_PATIENT_PROGRESS},
            {"个人信息",      NAV_PATIENT_PROFILE},
            {"修改密码",      NAV_PATIENT_CHANGE_PWD},
        };
        for (int i = 0; i < (int)(sizeof(items)/sizeof(items[0])); i++) {
            tvis.hParent = hRoot;
            tvis.item.pszText = (LPSTR)items[i].name;
            tvis.item.lParam = items[i].id;     /* 导航 ID 通过 lParam 传递 */
            TreeView_InsertItem(g_hNavTree, &tvis);
        }
        TreeView_Expand(g_hNavTree, hRoot, TVE_EXPAND);  /* 默认展开 / expand by default */

    /* 医生导航: 待接诊/接诊/病房呼叫/紧急标记/进度更新/模板/开药 (7项) */
    } else if (strcmp(role, ROLE_DOCTOR) == 0) {
        tvis.hParent = NULL;
        tvis.item.pszText = (LPSTR)"医生系统";
        tvis.item.lParam = 0;
        hRoot = TreeView_InsertItem(g_hNavTree, &tvis);

        struct { const char *name; int id; } items[] = {
            {"待接诊",          NAV_DOCTOR_REMINDER},
            {"接诊",            NAV_DOCTOR_CONSULTATION},
            {"病房呼叫",        NAV_DOCTOR_WARD_CALL},
            {"紧急标记",        NAV_DOCTOR_EMERGENCY},
            {"进度更新",        NAV_DOCTOR_PROGRESS},
            {"病历模板",        NAV_DOCTOR_TEMPLATE},
            {"开药",            NAV_DOCTOR_PRESCRIBE},
            {"修改密码",        NAV_DOCTOR_CHANGE_PWD},
        };
        for (int i = 0; i < (int)(sizeof(items)/sizeof(items[0])); i++) {
            tvis.hParent = hRoot;
            tvis.item.pszText = (LPSTR)items[i].name;
            tvis.item.lParam = items[i].id;
            TreeView_InsertItem(g_hNavTree, &tvis);
        }
        TreeView_Expand(g_hNavTree, hRoot, TVE_EXPAND);

    /* 管理员导航: 科室/医生/患者/药品/病房/排班/日志/数据/报表/密码 (10项) */
    } else if (strcmp(role, ROLE_ADMIN) == 0) {
        tvis.hParent = NULL;
        tvis.item.pszText = (LPSTR)"管理员系统";
        tvis.item.lParam = 0;
        hRoot = TreeView_InsertItem(g_hNavTree, &tvis);

        struct { const char *name; int id; } items[] = {
            {"科室管理",   NAV_ADMIN_DEPT},
            {"医生管理",   NAV_ADMIN_DOCTOR},
            {"患者管理",   NAV_ADMIN_PATIENT},
            {"药品管理",   NAV_ADMIN_DRUG},
            {"病房管理",   NAV_ADMIN_WARD},
            {"排班管理",   NAV_ADMIN_SCHEDULE},
            {"操作日志",   NAV_ADMIN_LOG},
            {"数据管理",   NAV_ADMIN_DATA},
            {"报表统计",   NAV_ADMIN_ANALYSIS},
            {"重置密码",   NAV_ADMIN_RESETPWD},
        };
        for (int i = 0; i < (int)(sizeof(items)/sizeof(items[0])); i++) {
            tvis.hParent = hRoot;
            tvis.item.pszText = (LPSTR)items[i].name;
            tvis.item.lParam = items[i].id;
            TreeView_InsertItem(g_hNavTree, &tvis);
        }
        TreeView_Expand(g_hNavTree, hRoot, TVE_EXPAND);
    }
}

/* ==================  视图切换 / View Switching ================== */

/* 切换右侧内容区: 销毁旧视图 → 根据 viewId 创建新的角色页面
   Switch content view: destroy old → create new page based on viewId */
void SwitchView(HWND hWnd, int viewId) {
    if (g_hContentView) {
        DestroyWindow(g_hContentView);
        g_hContentView = NULL;
    }
    g_currentView = viewId;

    /* 计算内容区矩形 (排除导航栏、状态栏、边距)
       Calculate content area rect (exclude nav, status bar, margins) */
    RECT rc;
    GetClientRect(hWnd, &rc);
    rc.left += NAV_WIDTH + 4;
    rc.top += 4;
    rc.right -= 4;
    rc.bottom -= STATUS_HEIGHT + 4;

    HWND hParent = hWnd;

    /* 按导航 ID 范围路由到对应的页面工厂函数
       Route to page factory based on nav ID range */
    switch (viewId) {
    /* 患者页面 (2001-2007) / Patient pages */
    case NAV_PATIENT_REGISTER:
    case NAV_PATIENT_APPOINTMENT:
    case NAV_PATIENT_DIAGNOSIS:
    case NAV_PATIENT_PRESCRIPTION:
    case NAV_PATIENT_WARD:
    case NAV_PATIENT_PROGRESS:
    case NAV_PATIENT_PROFILE:
    case NAV_PATIENT_CHANGE_PWD:
        g_hContentView = CreatePatientPage(hParent, viewId, &rc);
        break;
    /* 医生页面 (3001-3007) / Doctor pages */
    case NAV_DOCTOR_REMINDER:
    case NAV_DOCTOR_CONSULTATION:
    case NAV_DOCTOR_WARD_CALL:
    case NAV_DOCTOR_EMERGENCY:
    case NAV_DOCTOR_PROGRESS:
    case NAV_DOCTOR_TEMPLATE:
    case NAV_DOCTOR_PRESCRIBE:
    case NAV_DOCTOR_CHANGE_PWD:
        g_hContentView = CreateDoctorPage(hParent, viewId, &rc);
        break;
    /* 管理员页面 (4001-4010) / Admin pages */
    case NAV_ADMIN_DEPT:
    case NAV_ADMIN_DOCTOR:
    case NAV_ADMIN_PATIENT:
    case NAV_ADMIN_DRUG:
    case NAV_ADMIN_WARD:
    case NAV_ADMIN_SCHEDULE:
    case NAV_ADMIN_LOG:
    case NAV_ADMIN_DATA:
    case NAV_ADMIN_ANALYSIS:
    case NAV_ADMIN_RESETPWD:
        g_hContentView = CreateAdminPage(hParent, viewId, &rc);
        break;
    default:
        break;
    }
}

/* 获取内容区句柄 / Get content view handle (for external use) */
HWND GetContentView(HWND hWnd) {
    (void)hWnd;
    return g_hContentView;
}

/* 设置状态栏文本 / Set status bar text */
void SetStatusText(HWND hWnd, const char *text) {
    (void)hWnd;
    if (g_hStatusBar) {
        SendMessageA(g_hStatusBar, SB_SETTEXTA, 0, (LPARAM)text);
    }
}

/* ==================  导航选择变化 / Navigation Selection Change ================== */

/* TVN_SELCHANGEDA 消息处理: 从 lParam 提取新选中节点的 viewId → 切换视图
   Handle tree selection: extract viewId from lParam → switch view */
static void OnNavSelChange(HWND hWnd, LPARAM lParam) {
    NM_TREEVIEW *pnmtv = (NM_TREEVIEW *)lParam;
    HTREEITEM hItem = pnmtv->itemNew.hItem;
    if (!hItem) return;

    TV_ITEM item;
    item.hItem = hItem;
    item.mask = TVIF_PARAM;
    TreeView_GetItem(g_hNavTree, &item);

    int viewId = (int)item.lParam;    /* 提取节点中保存的导航 ID */
    if (viewId > 0) {
        SwitchView(hWnd, viewId);     /* 根节点 viewId=0 → 忽略 */
    }
}

/* ==================  窗口大小调整 / Window Resize ================== */

/* WM_SIZE 处理: 重新布局各子控件 (导航栏/注销按钮/状态栏/内容区)
   Handle resize: relayout nav tree, logout button, status bar, content area */
static void OnSize(HWND hWnd, UINT state, int cx, int cy) {
    (void)state;
    (void)hWnd;
    /* 导航树: 顶部→状态栏上方 / Nav tree: top → above status bar */
    if (g_hNavTree) {
        SetWindowPos(g_hNavTree, NULL, 0, 0, NAV_WIDTH, cy - STATUS_HEIGHT - 30,
                     SWP_NOZORDER);
    }
    /* 注销按钮: 导航树下方 / Logout button: below nav tree */
    if (g_hLogoutBtn) {
        SetWindowPos(g_hLogoutBtn, NULL, 0, cy - STATUS_HEIGHT - 30,
                     NAV_WIDTH, 30, SWP_NOZORDER);
    }
    /* 状态栏自动调整 / Status bar auto-sizes */
    if (g_hStatusBar) {
        SendMessageA(g_hStatusBar, WM_SIZE, 0, 0);
    }
    /* 内容区: 填充剩余空间 / Content area: fill remaining space */
    if (g_hContentView) {
        RECT rc = { NAV_WIDTH + 4, 4, cx - 4, cy - STATUS_HEIGHT - 4 };
        SetWindowPos(g_hContentView, NULL, rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER);
    }
}

/* ==================  主窗口过程 / Main Window Procedure ================== */

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        /* 创建子控件: 导航树 + 注销按钮 + 状态栏
           Create child controls: nav tree + logout button + status bar */
        g_hNavTree = CreateNavTree(hWnd);

        g_hLogoutBtn = CreateWindowA(
            "BUTTON", "退出登录",
            WS_VISIBLE | WS_CHILD | BS_FLAT,
            0, 0, NAV_WIDTH, 30,
            hWnd, (HMENU)100, g_hInst, NULL       /* ID=100: 注销按钮 */
        );

        g_hStatusBar = CreateWindowA(
            STATUSCLASSNAMEA, "", WS_VISIBLE | WS_CHILD | SBARS_SIZEGRIP,
            0, 0, 0, 0, hWnd, (HMENU)2, g_hInst, NULL
        );

        SetStatusText(hWnd, "  就绪");
        /* 启动文件轮询定时器 — 检测其他实例的数据变更 */
        SetTimer(hWnd, REFRESH_TIMER_ID, REFRESH_INTERVAL_MS, NULL);
        return 0;

    case WM_SIZE:
        OnSize(hWnd, (UINT)wParam, LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_NOTIFY:
        /* 导航树选中项变化 → 切换视图 / Tree selection change → switch view */
        if (((NMHDR *)lParam)->idFrom == 1 &&
            ((NMHDR *)lParam)->code == TVN_SELCHANGEDA) {
            OnNavSelChange(hWnd, lParam);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == 100) {
            /* 注销按钮: 清除当前用户 → 重新登录 → 重建导航树 → 切换首页
               Logout: clear user → re-login → rebuild nav → switch to first page */
            memset(&g_currentUser, 0, sizeof(User));
            User newUser;
            if (ShowLoginDialog(g_hInst, hWnd, &newUser) == SUCCESS) {
                g_currentUser = newUser;
                PopulateNavTree();
                int firstView = 0;
                if (strcmp(newUser.role, ROLE_PATIENT) == 0)
                    firstView = NAV_PATIENT_REGISTER;
                else if (strcmp(newUser.role, ROLE_DOCTOR) == 0)
                    firstView = NAV_DOCTOR_REMINDER;
                else if (strcmp(newUser.role, ROLE_ADMIN) == 0)
                    firstView = NAV_ADMIN_DEPT;
                SwitchView(hWnd, firstView);
                char status[256];
                snprintf(status, sizeof(status), "  用户: %s  |  角色: %s",
                         newUser.username, newUser.role);
                SetStatusText(hWnd, status);
            } else {
                /* 登录取消 → 关闭主窗口 / Login cancelled → close */
                DestroyWindow(hWnd);
                PostQuitMessage(0);
            }
        } else if (LOWORD(wParam) == 200) {
            /* 关于对话框 / About dialog */
            MessageBoxA(hWnd,
                "电子医疗管理系统 v1.0\n"
                "基于 Win32 API 的原生桌面应用\n\n"
                "角色: 患者 / 医生 / 管理员",
                "关于", MB_OK | MB_ICONINFORMATION);
        }
        return 0;

    case WM_APP_REFRESH:
        /* 自定义刷新消息: 子页面 CRUD 后通知刷新列表
           Custom refresh: posted by child pages after CRUD to refresh list.
           仅当 viewId 与当前一致时才刷新 (防止快速切换导致的错误)
           Only refresh if viewId matches current (prevents race condition) */
        if ((int)wParam == g_currentView)
            SwitchView(hWnd, (int)wParam);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_TIMER: {
        /* 文件轮询: 检测其他实例是否修改了数据文件
           File polling: detect if other instances modified data files */
        if (wParam == REFRESH_TIMER_ID && g_currentView > 0) {
            int dirty = 0;
            struct stat st;

            if (!dirty && stat(APPOINTMENTS_FILE, &st) == 0) {
                if (st.st_mtime != g_lastApptMtime) {
                    g_lastApptMtime = st.st_mtime;
                    if (g_currentView == NAV_PATIENT_APPOINTMENT ||
                        g_currentView == NAV_DOCTOR_REMINDER ||
                        g_currentView == NAV_DOCTOR_CONSULTATION)
                        dirty = 1;
                }
            }
            if (!dirty && stat(MEDICAL_RECORDS_FILE, &st) == 0) {
                if (st.st_mtime != g_lastRecordMtime) {
                    g_lastRecordMtime = st.st_mtime;
                    if (g_currentView == NAV_PATIENT_DIAGNOSIS)
                        dirty = 1;
                }
            }
            if (!dirty && stat(PRESCRIPTIONS_FILE, &st) == 0) {
                if (st.st_mtime != g_lastRxMtime) {
                    g_lastRxMtime = st.st_mtime;
                    if (g_currentView == NAV_PATIENT_PRESCRIPTION)
                        dirty = 1;
                }
            }
            if (!dirty && stat(WARDS_FILE, &st) == 0) {
                if (st.st_mtime != g_lastWardMtime) {
                    g_lastWardMtime = st.st_mtime;
                    if (g_currentView == NAV_PATIENT_WARD)
                        dirty = 1;
                }
            }

            if (dirty)
                SwitchView(hWnd, g_currentView);
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hWnd, REFRESH_TIMER_ID);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
}
