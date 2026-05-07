/*
 * gui_main.c — Win32 GUI 主窗口实现 / Win32 GUI main window implementation
 *作者：王成烨
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
 *
 * ===================== 导航系统架构 / Navigation Architecture =====================
 *
 * 导航 ID 分配方案:
 *   NAV_PATIENT_BASE = 2000  →  患者页面: 2001-2008 (挂号/预约/诊断/缴费/病房/进度/资料/密码)
 *   NAV_DOCTOR_BASE  = 3000  →  医生页面: 3001-3008 (待接诊/接诊/病房呼叫/急诊/进度/模板/开药/密码)
 *   NAV_ADMIN_BASE   = 4000  →  管理员页面: 4001-4010 (科室/医生/患者/药品/病房/排班/日志/数据/报表/密码)
 *
 * 视图切换流程:
 *   用户点击导航树节点 → WM_NOTIFY/TVN_SELCHANGEDA → OnNavSelChange
 *   → 从 TVITEM.lParam 提取导航 ID → SwitchView(hWnd, viewId)
 *   → 销毁旧内容区 → 根据 viewId 范围路由到 Patient/Doctor/Admin 工厂函数
 *   → 创建新的子窗口作为内容区
 *
 * 消息流:
 *   Windows 消息循环 (GetMessage/DispatchMessage) → MainWndProc
 *   → 分别处理 WM_CREATE(子控件创建), WM_SIZE(布局), WM_NOTIFY(导航选择),
 *      WM_COMMAND(按钮点击), WM_TIMER(文件轮询), WM_APP_REFRESH(子页面刷新通知)
 *
 * ===================== 导航 ID 与页面对照表 / View ID to Page Mapping =====================
 *
 *   NAV_PATIENT_REGISTER  (2001) → 患者挂号页面     → CreatePatientPage → gui_patient.c
 *   NAV_PATIENT_APPOINTMENT(2002) → 患者预约查询      → CreatePatientPage → gui_patient.c
 *   NAV_PATIENT_DIAGNOSIS (2003)  → 患者诊断结果      → CreatePatientPage → gui_patient.c
 *   NAV_PATIENT_PRESCRIPTION(2004)→ 患者缴费          → CreatePatientPage → gui_patient.c
 *   NAV_PATIENT_WARD      (2005) → 患者住院信息      → CreatePatientPage → gui_patient.c
 *   NAV_PATIENT_PROGRESS  (2006) → 患者治疗进度      → CreatePatientPage → gui_patient.c
 *   NAV_PATIENT_PROFILE   (2007) → 患者个人信息      → CreatePatientPage → gui_patient.c
 *   NAV_PATIENT_CHANGE_PWD(2008) → 患者修改密码      → CreatePatientPage → gui_patient.c
 *
 *   NAV_DOCTOR_REMINDER   (3001) → 医生待接诊列表    → CreateDoctorPage  → gui_doctor.c
 *   NAV_DOCTOR_CONSULTATION(3002)→ 医生接诊问诊      → CreateDoctorPage  → gui_doctor.c
 *   NAV_DOCTOR_WARD_CALL  (3003) → 医生病房呼叫      → CreateDoctorPage  → gui_doctor.c
 *   NAV_DOCTOR_EMERGENCY  (3004) → 医生急诊管理      → CreateDoctorPage  → gui_doctor.c
 *   NAV_DOCTOR_PROGRESS   (3005) → 医生进度更新      → CreateDoctorPage  → gui_doctor.c
 *   NAV_DOCTOR_TEMPLATE   (3006) → 医生病历模板      → CreateDoctorPage  → gui_doctor.c
 *   NAV_DOCTOR_PRESCRIBE  (3007) → 医生开具处方      → CreateDoctorPage  → gui_doctor.c
 *   NAV_DOCTOR_CHANGE_PWD (3008) → 医生修改密码      → CreateDoctorPage  → gui_doctor.c
 *
 *   NAV_ADMIN_DEPT        (4001) → 管理员科室管理    → CreateAdminPage   → gui_admin.c
 *   NAV_ADMIN_DOCTOR      (4002) → 管理员医生管理    → CreateAdminPage   → gui_admin.c
 *   NAV_ADMIN_PATIENT     (4003) → 管理员患者管理    → CreateAdminPage   → gui_admin.c
 *   NAV_ADMIN_DRUG        (4004) → 管理员药品管理    → CreateAdminPage   → gui_admin.c
 *   NAV_ADMIN_WARD        (4005) → 管理员病房管理    → CreateAdminPage   → gui_admin.c
 *   NAV_ADMIN_SCHEDULE    (4006) → 管理员排班管理    → CreateAdminPage   → gui_admin.c
 *   NAV_ADMIN_LOG         (4007) → 管理员操作日志    → CreateAdminPage   → gui_admin.c
 *   NAV_ADMIN_DATA        (4008) → 管理员数据管理    → CreateAdminPage   → gui_admin.c
 *   NAV_ADMIN_ANALYSIS    (4009) → 管理员报表统计    → CreateAdminPage   → gui_admin.c
 *   NAV_ADMIN_RESETPWD    (4010)→  管理员重置密码    → CreateAdminPage   → gui_admin.c
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
static time_t g_lastWardCallMtime = 0; /* ward_calls.txt */
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

    /* ========== 步骤 1: 初始化通用控件库 (TreeView, ListView, Tab, Progress 等) ==========
       必须在使用 TreeView/ListView 等公共控件之前调用, 否则 CreateWindow 会失败。
       InitCommonControlsEx 注册所需的所有控件窗口类。
       Initialize Common Controls library for TreeView, ListView, etc. */
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES |
                ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    /* ========== 步骤 2: 初始化数据存储与默认用户 ==========
       加载或创建用户数据文件, 确保默认账号 (admin/doctor1/patient1) 存在。
       同 console 版本 main.c 的逻辑, 包括首次运行时的默认用户创建和密码迁移。
       Init data storage & default users (same logic as console main.c) */
    init_console_encoding();
    if (init_data_storage() != SUCCESS) {
        MessageBoxA(NULL, "数据存储初始化失败!", "错误", MB_ICONERROR);
        return 1;
    }

    /* 创建默认用户或迁移密码 / Create defaults or migrate passwords */

    /* ========== 步骤 2a: 尝试加载用户列表, 检测是否首次运行 ==========
       如果 load_users_list 返回 NULL, 说明 users.txt 不存在 (首次运行)。
       首次运行: 创建 3 个默认用户 (admin/doctor1/patient1),
       密码 = 用户名+123 的 SHA-256 哈希值。
       First run: create default users with SHA-256 hashed passwords */
    UserNode *head = load_users_list();
    if (!head) {
        /* 首次运行: 硬编码创建 admin/doctor1/patient1 三个默认用户
           密码分别为: admin123, doctor123, patient123 的 SHA-256 哈希
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
        /* 首次运行: 同时为 doctor1 和 patient1 创建默认档案 (姓名=用户名)
           Ensure default profiles exist for doctor1 and patient1 */
        ensure_doctor_profile("doctor1");
        ensure_patient_profile("patient1");
    } else {
        /* ========== 步骤 2b: 非首次运行 — 迁移和补全 ==========
           1. 迁移旧格式密码 (明文 → SHA-256 哈希)
           2. 确保三个默认账号始终存在 (防止用户误删默认账号)
           3. 批量确保所有医生和患者的档案文件存在
           4. 释放用户列表
           Non-first run: migrate passwords, ensure defaults, batch ensure profiles */
        migrate_user_passwords();

        /* 确保默认账号始终存在 — 遍历三个默认账号, 检查是否在用户列表中
           如果某个默认账号已被删除, 则重新创建并插入链表头部
           Ensure default accounts always exist */
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
           避免逐用户加载/save 导致的 O(n^2) 性能问题
           Batch ensure profiles: load each file once, check all users */
        batch_ensure_doctor_profiles(head);
        batch_ensure_patient_profiles(head);

        free_user_list(head);
    }
    migrate_doctor_ids();       /* 迁移旧格式医生 ID / Migrate legacy doctor ID format */
    ensure_default_templates(); /* 初始化 18 个默认病历模板 / Initialize 18 default medical templates */

    g_hInst = hInstance;

    /* ========== 步骤 3: 初始化文件修改时间缓存 ==========
       通过 stat() 读取各数据文件的 mtime, 作为轮询检测的基准值。
       避免首次定时器触发时因 mtime 为 0 而误判所有文件为新变更。
       Init mtime caches so the first timer tick doesn't trigger a spurious refresh */
    {
        struct stat st;
        if (stat(APPOINTMENTS_FILE, &st) == 0)   g_lastApptMtime   = st.st_mtime;
        if (stat(MEDICAL_RECORDS_FILE, &st) == 0) g_lastRecordMtime = st.st_mtime;
        if (stat(PRESCRIPTIONS_FILE, &st) == 0)   g_lastRxMtime     = st.st_mtime;
        if (stat(WARD_CALLS_FILE, &st) == 0)    g_lastWardCallMtime = st.st_mtime;
        if (stat(WARDS_FILE, &st) == 0)           g_lastWardMtime   = st.st_mtime;
    }

    /* ========== 步骤 4: 创建主窗口 (初始隐藏) ==========
       窗口在 InitMainWindow 中创建但保持隐藏状态。
       等登录成功后再通过 ShowWindow 显示, 避免先看到空白窗口再弹登录框。
       Create main window (initially hidden, shown after successful login) */
    if (!InitMainWindow(hInstance, nCmdShow))
        return 1;

    /* ========== 步骤 5: 显示登录对话框 (模态) ==========
       ShowLoginDialog 内部运行模态消息循环, 阻塞直到用户登录成功或取消。
       登录成功: 将用户信息存入 g_currentUser, 继续执行。
       登录取消: 销毁主窗口, 返回 0 (干净退出)。
       Show login dialog (modal loop) */
    User loggedUser;
    if (ShowLoginDialog(hInstance, g_hMainWnd, &loggedUser) != SUCCESS) {
        DestroyWindow(g_hMainWnd);
        return 0;  /* 用户取消登录 → 退出 / user cancelled → exit */
    }
    g_currentUser = loggedUser;

    /* ========== 步骤 6: 登录成功后显示主窗口 ==========
       此时窗口内容 (导航树/状态栏) 已在 WM_CREATE 中创建完毕,
       ShowWindow 让用户看到完整的界面。
       Login successful → show main window */
    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    /* ========== 步骤 7: 按角色填充导航树 ==========
       根据 g_currentUser.role 动态创建导航树节点:
       - 患者 → 8 个子节点 (挂号..修改密码)
       - 医生 → 8 个子节点 (待接诊..修改密码)
       - 管理员 → 10 个子节点 (科室管理..重置密码)
       Populate nav tree for the logged-in role */
    PopulateNavTree();

    /* ========== 步骤 8: 根据角色切换到首个页面 ==========
       不同角色的默认首页:
       - 患者 → NAV_PATIENT_REGISTER (2001, 挂号)
       - 医生 → NAV_DOCTOR_REMINDER (3001, 待接诊)
       - 管理员 → NAV_ADMIN_DEPT (4001, 科室管理)
       Switch to first page based on role */
    int firstView = 0;
    if (strcmp(loggedUser.role, ROLE_PATIENT) == 0)
        firstView = NAV_PATIENT_REGISTER;
    else if (strcmp(loggedUser.role, ROLE_DOCTOR) == 0)
        firstView = NAV_DOCTOR_REMINDER;
    else if (strcmp(loggedUser.role, ROLE_ADMIN) == 0)
        firstView = NAV_ADMIN_DEPT;
    SwitchView(g_hMainWnd, firstView);

    /* ========== 步骤 9: 更新状态栏显示当前用户信息 ==========
       医生角色: 显示 用户名/姓名/角色/医生ID
       患者角色: 显示 用户名/姓名/角色/患者ID
       管理员角色: 显示 用户名/角色
       Update status bar with current user info */
    char status[256];
    if (strcmp(loggedUser.role, ROLE_DOCTOR) == 0) {
        Doctor *d = find_doctor_by_username(loggedUser.username);
        snprintf(status, sizeof(status), "  用户: %s (%s)  |  角色: %s  |  医生ID: %s",
                 loggedUser.username, d ? d->name : "", loggedUser.role, d ? d->doctor_id : "未知");
        if (d) free(d);
    } else if (strcmp(loggedUser.role, ROLE_PATIENT) == 0) {
        Patient *p = find_patient_by_username(loggedUser.username);
        snprintf(status, sizeof(status), "  用户: %s (%s)  |  角色: %s  |  患者ID: %s",
                 loggedUser.username, p ? p->name : "", loggedUser.role, p ? p->patient_id : "未知");
        if (p) free(p);
    } else {
        snprintf(status, sizeof(status), "  用户: %s  |  角色: %s",
                 loggedUser.username, loggedUser.role);
    }
    SetStatusText(g_hMainWnd, status);

    /* ========== 步骤 10: 进入 Win32 标准消息循环 ==========
       GetMessage 阻塞等待消息, TranslateMessage 处理键盘消息 (WM_KEYDOWN→WM_CHAR),
       DispatchMessage 调用 MainWndProc 处理消息。
       循环在收到 WM_QUIT 时 GetMessage 返回 0, 退出循环。
       Standard Win32 message loop */
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

/* ==================  初始化主窗口 / Initialize Main Window ================== */

/*
 * InitMainWindow — 注册窗口类并创建主窗口
 *
 * 流程:
 *   1. 定义窗口类属性 (图标/光标/背景色/窗口过程)
 *   2. 调用 RegisterClassA 注册窗口类
 *   3. 调用 CreateWindowExA 创建窗口实例 (初始隐藏, 等登录成功后显示)
 *
 * 窗口样式 WS_OVERLAPPEDWINDOW 提供标题栏/系统菜单/最小化/最大化/可调边框。
 * WS_CLIPCHILDREN 防止父窗口在子控件区域绘制, 减少闪烁。
 *
 * 返回值: 成功返回 TRUE, 失败返回 FALSE
 *
 * Register window class → create → initially hidden, shown after successful login
 */
static BOOL InitMainWindow(HINSTANCE hInst, int nCmdShow) {
    const char CLASS_NAME[] = "EMSMainWindow";

    /* 填充窗口类结构 / Fill window class structure */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = MainWndProc;                /* 窗口过程 → 处理所有消息 */
    wc.hInstance     = hInst;                      /* 应用实例句柄 */
    wc.hIcon         = LoadIconA(NULL, IDI_APPLICATION);  /* 默认应用图标 */
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);       /* 默认箭头光标 */
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);  /* 系统默认背景色 */
    wc.lpszClassName = CLASS_NAME;

    /* 注册窗口类 — 失败则返回 FALSE 导致程序退出 */
    if (!RegisterClassA(&wc))
        return FALSE;

    /* 创建主窗口实例 — 初始隐藏, 等登录成功后再 ShowWindow 显示
       WS_CLIPCHILDREN 非常重要: 避免父窗口绘制覆盖子控件区域,
       同时也是无闪烁布局的前提条件 */
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

/*
 * CreateNavTree — 创建 TreeView 控件作为左侧导航栏
 *
 * TreeView 样式说明:
 *   WS_VISIBLE          — 控件初始可见
 *   WS_CHILD            — 子窗口控件
 *   TVS_HASLINES        — 显示兄弟节点之间的虚线连接
 *   TVS_LINESATROOT     — 显示根节点到子节点的连接线
 *   TVS_HASBUTTONS      — 每个可展开节点显示 +/- 按钮
 *   TVS_SHOWSELALWAYS   — 选中项始终高亮, 即使 TreeView 失去焦点
 *
 * 控件 ID 为 1, 在 WM_NOTIFY 中通过 idFrom == 1 来识别来自导航树的通知消息。
 *
 * Create TreeView control (left nav)
 */
static HWND CreateNavTree(HWND hParent) {
    HWND hTree = CreateWindowA(
        WC_TREEVIEWA, "", WS_VISIBLE | WS_CHILD | TVS_HASLINES |
        TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
        0, 0, NAV_WIDTH, 100,
        hParent, (HMENU)1, g_hInst, NULL
    );
    return hTree;
}

/*
 * PopulateNavTree — 根据当前用户角色动态填充导航树节点
 *
 * 导航树结构 (单层, 每个角色一个根节点 + N 个子节点):
 *
 *   患者角色 (ROLE_PATIENT):
 *     根节点: "患者系统" (lParam=0, 不可点击切换)
 *       └── 挂号         (lParam=2001, NAV_PATIENT_REGISTER)
 *       └── 预约查询     (lParam=2002, NAV_PATIENT_APPOINTMENT)
 *       └── 诊断结果     (lParam=2003, NAV_PATIENT_DIAGNOSIS)
 *       └── 缴费         (lParam=2004, NAV_PATIENT_PRESCRIPTION)
 *       └── 住院信息     (lParam=2005, NAV_PATIENT_WARD)
 *       └── 治疗进度     (lParam=2006, NAV_PATIENT_PROGRESS)
 *       └── 个人信息     (lParam=2007, NAV_PATIENT_PROFILE)
 *       └── 修改密码     (lParam=2008, NAV_PATIENT_CHANGE_PWD)
 *
 *   医生角色 (ROLE_DOCTOR):
 *     根节点: "医生系统" (lParam=0)
 *       └── 待接诊       (lParam=3001, NAV_DOCTOR_REMINDER)
 *       └── 接诊         (lParam=3002, NAV_DOCTOR_CONSULTATION)
 *       └── 病房呼叫     (lParam=3003, NAV_DOCTOR_WARD_CALL)
 *       └── 紧急标记     (lParam=3004, NAV_DOCTOR_EMERGENCY)
 *       └── 进度更新     (lParam=3005, NAV_DOCTOR_PROGRESS)
 *       └── 病历模板     (lParam=3006, NAV_DOCTOR_TEMPLATE)
 *       └── 后续医疗活动 (lParam=3007, NAV_DOCTOR_PRESCRIBE)
 *       └── 修改密码     (lParam=3008, NAV_DOCTOR_CHANGE_PWD)
 *
 *   管理员角色 (ROLE_ADMIN):
 *     根节点: "管理员系统" (lParam=0)
 *       └── 科室管理     (lParam=4001, NAV_ADMIN_DEPT)
 *       └── 医生管理     (lParam=4002, NAV_ADMIN_DOCTOR)
 *       └── 患者管理     (lParam=4003, NAV_ADMIN_PATIENT)
 *       └── 药品管理     (lParam=4004, NAV_ADMIN_DRUG)
 *       └── 病房管理     (lParam=4005, NAV_ADMIN_WARD)
 *       └── 排班管理     (lParam=4006, NAV_ADMIN_SCHEDULE)
 *       └── 操作日志     (lParam=4007, NAV_ADMIN_LOG)
 *       └── 数据管理     (lParam=4008, NAV_ADMIN_DATA)
 *       └── 报表统计     (lParam=4009, NAV_ADMIN_ANALYSIS)
 *       └── 重置密码     (lParam=4010, NAV_ADMIN_RESETPWD)
 *
 * 关键设计:
 *   - 每个子节点的 lParam 存储导航 ID (如 2001, 3001...),
 *     在 OnNavSelChange 中通过 TreeView_GetItem 提取,
 *     然后传给 SwitchView 进行页面路由。
 *   - 根节点的 lParam=0, 在 OnNavSelChange 中被忽略 (viewId > 0 检查),
 *     因此点击根节点不会触发视图切换。
 *   - 创建完毕后调用 TreeView_Expand 展开根节点, 子节点立即可见。
 *
 * Populate nav tree based on current role
 */
static void PopulateNavTree(void) {
    if (!g_hNavTree) return;

    TreeView_DeleteAllItems(g_hNavTree);  /* 清空旧的节点 / clear old items */

    HTREEITEM hRoot = NULL;
    /* TV_INSERTSTRUCT 设置:
       - hInsertAfter = TVI_LAST → 在最后位置插入 (追加)
       - item.mask = TVIF_TEXT | TVIF_PARAM → 设置节点文本和 lParam */
    TV_INSERTSTRUCTA tvis = {0};
    tvis.hInsertAfter = TVI_LAST;
    tvis.item.mask = TVIF_TEXT | TVIF_PARAM;  /* 各节点通过 lParam 传递导航 ID */

    const char *role = g_currentUser.role;

    /* 患者导航: 挂号/预约查询/诊断/处方/住院/进度/资料 (7项)
       Patient nav: register/appointment/diagnosis/prescription/ward/progress/profile */
    if (strcmp(role, ROLE_PATIENT) == 0) {
        /* 创建根节点 "患者系统", lParam=0 表示不可导航 */
        tvis.hParent = NULL;
        tvis.item.pszText = (LPSTR)"患者系统";
        tvis.item.lParam = 0;
        hRoot = TreeView_InsertItem(g_hNavTree, &tvis);

        /* 创建 8 个子节点, 每个的 lParam 对应一个导航 ID
           当用户点击子节点时, OnNavSelChange 提取 lParam → SwitchView 切换到对应页面 */
        struct { const char *name; int id; } items[] = {
            {"挂号",          NAV_PATIENT_REGISTER},
            {"预约查询",      NAV_PATIENT_APPOINTMENT},
            {"诊断结果",      NAV_PATIENT_DIAGNOSIS},
            {"缴费",          NAV_PATIENT_PRESCRIPTION},
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
            {"后续医疗活动",    NAV_DOCTOR_PRESCRIBE},
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

/*
 * SwitchView — 切换右侧内容区的显示
 *
 * 这是导航系统的核心函数。当用户在导航树中选择不同节点时被调用,
 * 负责销毁旧的内容页并创建新的内容页。
 *
 * 工作流程:
 *   1. 销毁当前内容区 (DestroyWindow), 释放 g_hContentView 句柄
 *   2. 更新 g_currentView 为新的 viewId
 *   3. 计算内容区的客户端矩形 (排除左侧导航栏、边距和底部状态栏)
 *   4. 根据 viewId 范围进行路由:
 *      2001-2008 (患者) → CreatePatientPage(hParent, viewId, &rc)  → 返回子窗口句柄
 *      3001-3008 (医生) → CreateDoctorPage(hParent, viewId, &rc)   → 返回子窗口句柄
 *      4001-4010 (管理员)→ CreateAdminPage(hParent, viewId, &rc)   → 返回子窗口句柄
 *   5. 新创建的子窗口句柄存入 g_hContentView
 *
 * 路由策略:
 *   使用 switch-case 的 fall-through 模式: 同角色的所有 viewId 共享同一个
 *   case body, 避免了在每个 case 中重复调用相同的工厂函数。
 *
 * 调用来源:
 *   - WinMain: 登录成功后切换到角色默认首页
 *   - OnNavSelChange: 用户点击导航树节点
 *   - MainWndProc/WM_COMMAND: 注销后重新登录切换首页
 *   - MainWndProc/WM_TIMER: 文件轮询检测到变更时刷新当前视图
 *   - MainWndProc/WM_APP_REFRESH: 子页面 CRUD 后主动请求刷新
 *
 * Switch content view: destroy old → create new based on viewId
 */
void SwitchView(HWND hWnd, int viewId) {
    /* 步骤 1: 销毁旧内容区 / destroy old content window */
    if (g_hContentView) {
        DestroyWindow(g_hContentView);
        g_hContentView = NULL;
    }
    g_currentView = viewId;

    /* 步骤 2: 计算内容区矩形 / calculate content area rect
       左侧: NAV_WIDTH + 4 (导航树宽度 + 边距)
       顶部: 4 (边距)
       右侧: 窗口宽度 - 4 (右边距)
       底部: 窗口高度 - STATUS_HEIGHT - 4 (状态栏高度 + 边距)
       Calculate content area rect (exclude nav, status bar, margins) */
    RECT rc;
    GetClientRect(hWnd, &rc);
    rc.left += NAV_WIDTH + 4;
    rc.top += 4;
    rc.right -= 4;
    rc.bottom -= STATUS_HEIGHT + 4;

    HWND hParent = hWnd;

    /* 步骤 3: 按导航 ID 范围路由到对应的页面工厂函数
       患者页面 (2001-2008): gui_patient.c → CreatePatientPage
       医生页面 (3001-3008): gui_doctor.c  → CreateDoctorPage
       管理员页面(4001-4010):gui_admin.c   → CreateAdminPage
       每个工厂函数内部根据 viewId 二次分发到具体的页面创建逻辑
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

/*
 * OnNavSelChange — 处理 TreeView 节点选择变更通知
 *
 * 消息流:
 *   用户点击导航树节点 → TreeView 发送 WM_NOTIFY 给父窗口 (主窗口)
 *   → MainWndProc 检查 ((NMHDR*)lParam)->code == TVN_SELCHANGEDA
 *   → 调用 OnNavSelChange(hWnd, lParam)
 *   → 提取 NM_TREEVIEW 中的 itemNew (新选中的节点)
 *   → TreeView_GetItem 获取该节点的 lParam (即导航 ID)
 *   → 如果 viewId > 0 (非根节点), 调用 SwitchView 切换页面
 *
 * 安全性:
 *   - hItem == NULL 检查: 防止取消选中时 itemNew.hItem 为 NULL
 *   - viewId > 0 检查: 根节点的 lParam=0, 点击根节点不做任何操作
 *
 * Handle tree selection: extract viewId from lParam → switch view
 */
static void OnNavSelChange(HWND hWnd, LPARAM lParam) {
    /* 从 lParam 提取 NM_TREEVIEW 结构, 获取新选中的节点句柄 */
    NM_TREEVIEW *pnmtv = (NM_TREEVIEW *)lParam;
    HTREEITEM hItem = pnmtv->itemNew.hItem;
    if (!hItem) return;  /* 取消选中 → 不处理 */

    /* 获取节点存储的导航 ID (存储在 TVITEM.lParam 中)
       在 PopulateNavTree 中, 每个子节点的 lParam 被设置为对应的 NAV_* 枚举值 */
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

/*
 * OnSize — WM_SIZE 消息处理: 重新布局所有子控件
 *
 * 布局方案 (自适应窗口大小):
 *
 *   ┌──────────┬─────────────────────────────────────┐
 *   │  导航树   │                                     │
 *   │  (200px) │         内容区 (填充剩余空间)         │
 *   │          │                                     │
 *   │          │                                     │
 *   ├──────────┤                                     │
 *   │ 注销按钮  │                                     │
 *   │  (30px)  │                                     │
 *   ├──────────┴─────────────────────────────────────┤
 *   │              状态栏 (25px, 全宽)                │
 *   └────────────────────────────────────────────────┘
 *
 * 各控件定位:
 *   导航树:   x=0, y=0, w=NAV_WIDTH, h=cy - STATUS_HEIGHT - 30
 *   注销按钮:  x=0, y=cy - STATUS_HEIGHT - 30, w=NAV_WIDTH, h=30
 *   内容区:   x=NAV_WIDTH+4, y=4, w=cx-NAV_WIDTH-8, h=cy-STATUS_HEIGHT-8
 *   状态栏:   自动调整 (发送 WM_SIZE 给状态栏即可)
 *
 * Handle resize: relayout nav tree, logout button, status bar, content area
 */
static void OnSize(HWND hWnd, UINT state, int cx, int cy) {
    (void)state;
    (void)hWnd;
    /* 导航树: 顶部→状态栏上方 / Nav tree: top → above status bar */
    if (g_hNavTree) {
        SetWindowPos(g_hNavTree, NULL, 0, 0, NAV_WIDTH, cy - STATUS_HEIGHT - 30,
                     SWP_NOZORDER);
    }
    /* 注销按钮: 导航树下方, 高度 30px / Logout button: below nav tree, 30px tall */
    if (g_hLogoutBtn) {
        SetWindowPos(g_hLogoutBtn, NULL, 0, cy - STATUS_HEIGHT - 30,
                     NAV_WIDTH, 30, SWP_NOZORDER);
    }
    /* 状态栏自动调整 — SBARS_SIZEGRIP 样式使状态栏自动响应 WM_SIZE */
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

/*
 * MainWndProc — 主窗口消息处理过程
 *
 * 处理的消息类型及流程:
 *
 *   WM_CREATE      — 窗口创建时触发, 创建子控件 (导航树/注销按钮/状态栏),
 *                    启动文件轮询定时器
 *   WM_SIZE        — 窗口大小变化时触发, 调用 OnSize 重新布局子控件
 *   WM_NOTIFY      — 子控件通知消息 (导航树选择变更), 调用 OnNavSelChange
 *   WM_COMMAND     — 按钮点击等命令消息:
 *                     - LOWORD(wParam)==100 → 注销按钮 → 重新登录
 *                     - LOWORD(wParam)==200 → 关于对话框
 *   WM_APP_REFRESH — 自定义消息: 子页面 CRUD 完成后通知主窗口刷新当前视图
 *   WM_TIMER       — 定时器消息: 每 3 秒检查数据文件是否被其他实例修改,
 *                    如果有变更则自动刷新当前视图
 *   WM_CLOSE       — 窗口关闭请求
 *   WM_DESTROY     — 窗口销毁时清理定时器并发送 WM_QUIT 退出消息循环
 *
 * 默认处理: 调用 DefWindowProcA 处理未显式处理的消息
 *
 * Window procedure: handles all messages for the main window
 */
LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    /* ---------- WM_CREATE: 窗口创建 (在 CreateWindowEx 内部触发) ----------
       此时创建所有子控件:
       - 导航树 (TreeView, ID=1)
       - 注销按钮 (Button, ID=100)
       - 状态栏 (StatusBar, ID=2)
       同时设置初始状态栏文本 "就绪" 并启动文件轮询定时器。
       Create child controls: nav tree + logout button + status bar */
    case WM_CREATE:
        g_hNavTree = CreateNavTree(hWnd);

        /* 创建注销按钮: 普通按钮样式, 扁平外观 (BS_FLAT)
           ID=100 用于在 WM_COMMAND 中识别来自注销按钮的命令 */
        g_hLogoutBtn = CreateWindowA(
            "BUTTON", "退出登录",
            WS_VISIBLE | WS_CHILD | BS_FLAT,
            0, 0, NAV_WIDTH, 30,
            hWnd, (HMENU)100, g_hInst, NULL       /* ID=100: 注销按钮 */
        );

        /* 创建状态栏: 系统预定义的 STATUSCLASSNAME 类
           SBARS_SIZEGRIP 在右下角添加尺寸调节手柄 */
        g_hStatusBar = CreateWindowA(
            STATUSCLASSNAMEA, "", WS_VISIBLE | WS_CHILD | SBARS_SIZEGRIP,
            0, 0, 0, 0, hWnd, (HMENU)2, g_hInst, NULL
        );

        /* 初始状态栏文本 / initial status text */
        SetStatusText(hWnd, "  就绪");
        /* 启动文件轮询定时器 — 每 3 秒检测其他实例是否修改了数据文件
           Start file polling timer to detect external data file changes */
        SetTimer(hWnd, REFRESH_TIMER_ID, REFRESH_INTERVAL_MS, NULL);
        return 0;

    /* ---------- WM_SIZE: 窗口大小变化 ----------
       调用 OnSize 重新布局所有子控件, 保证界面在任意窗口大小下都能正常显示。 */
    case WM_SIZE:
        OnSize(hWnd, (UINT)wParam, LOWORD(lParam), HIWORD(lParam));
        return 0;

    /* ---------- WM_NOTIFY: 子控件通知消息 ----------
       处理来自 TreeView 的选择变更通知:
       - idFrom == 1  → 确认消息来自导航树 (导航树的 HMENU=1)
       - code == TVN_SELCHANGEDA → 节点选择已变更 (不同于 TVN_SELCHANGING)
       满足两个条件后调用 OnNavSelChange 进行页面切换。
       Tree selection change → switch view */
    case WM_NOTIFY:
        if (((NMHDR *)lParam)->idFrom == 1 &&
            ((NMHDR *)lParam)->code == TVN_SELCHANGEDA) {
            OnNavSelChange(hWnd, lParam);
        }
        return 0;

    /* ---------- WM_COMMAND: 控件命令消息 ----------
       处理来自按钮等控件的命令消息, 根据控件 ID (wParam 低 16 位) 分发:

       case ID=100 (注销按钮):
         1. 清除当前用户数据 (memset)
         2. 显示登录对话框 (模态)
         3. 登录成功 → 重建导航树 → 切换到角色默认首页 → 更新状态栏
         4. 登录取消 → 销毁主窗口 → 发送 WM_QUIT 退出程序

       case ID=200 (关于按钮, 保留供未来使用):
         显示"关于"对话框, 展示版本和角色信息 */
    case WM_COMMAND:
        if (LOWORD(wParam) == 100) {
            /* ========== 注销流程 / Logout Flow ==========
               注销→重新登录的核心流程, 模拟了 WinMain 中登录成功后的初始化步骤:

               步骤 1: 清除当前用户
                 将 g_currentUser 清零, 防止旧的用户数据残留影响新登录。
               Logout: clear user → re-login → rebuild nav → switch to first page */

            memset(&g_currentUser, 0, sizeof(User));

            /* 步骤 2: 显示登录对话框 (模态)
               如果用户取消登录 → DestroyWindow 关闭主窗口 → PostQuitMessage 退出 */
            User newUser;
            if (ShowLoginDialog(g_hInst, hWnd, &newUser) == SUCCESS) {
                /* 步骤 3: 登录成功 — 更新全局用户状态 */
                g_currentUser = newUser;

                /* 步骤 4: 重建导航树 (新用户可能有不同的角色, 导航项不同)
                   先清除旧树所有节点, 再根据新角色的枚举创建对应的子节点 */
                PopulateNavTree();

                /* 步骤 5: 确定新用户角色的默认首页并切换
                   患者→挂号(2001), 医生→待接诊(3001), 管理员→科室管理(4001) */
                int firstView = 0;
                if (strcmp(newUser.role, ROLE_PATIENT) == 0)
                    firstView = NAV_PATIENT_REGISTER;
                else if (strcmp(newUser.role, ROLE_DOCTOR) == 0)
                    firstView = NAV_DOCTOR_REMINDER;
                else if (strcmp(newUser.role, ROLE_ADMIN) == 0)
                    firstView = NAV_ADMIN_DEPT;
                SwitchView(hWnd, firstView);

                /* 步骤 6: 更新状态栏, 显示新用户信息 */
                char status[256];
                snprintf(status, sizeof(status), "  用户: %s  |  角色: %s",
                         newUser.username, newUser.role);
                SetStatusText(hWnd, status);
            } else {
                /* 注销后用户取消了重新登录 → 关闭应用
                   相当于彻底退出 (也可以改为返回登录界面, 当前设计选择退出) */
                DestroyWindow(hWnd);
                PostQuitMessage(0);
            }
        } else if (LOWORD(wParam) == 200) {
            /* ========== 关于对话框 / About Dialog ==========
               显示应用程序的名称、版本和技术信息。
               使用 MB_OK 单按钮 + 信息图标。
               保留控件 ID=200, 可绑定到菜单或按钮 */
            MessageBoxA(hWnd,
                "电子医疗管理系统 v1.0\n"
                "基于 Win32 API 的原生桌面应用\n\n"
                "角色: 患者 / 医生 / 管理员",
                "关于", MB_OK | MB_ICONINFORMATION);
        }
        return 0;

    /* ---------- WM_APP_REFRESH: 自定义刷新消息 ----------
       由子页面 (gui_patient.c/gui_doctor.c/gui_admin.c) 在完成 CRUD 操作后通过
       PostMessage(hMainWnd, WM_APP_REFRESH, viewId, 0) 发送。

       作用: 通知主窗口销毁当前内容页并重新创建, 实现列表/数据的实时刷新。
       例如: 医生在"接诊"页面完成诊断后, 发送 WM_APP_REFRESH 让页面重新加载
       最新的挂号列表, 将已接诊的患者从列表中移除。

       安全机制: 仅当 wParam (请求的 viewId) 与 g_currentView 一致时才刷新,
       防止用户快速切换页面时, 旧页面的刷新请求错误地刷新新页面。

       消息格式: wParam = 请求刷新的 viewId, lParam = 未使用 (保留为 0)
       Custom refresh: posted by child pages after CRUD to refresh list.
       Only refresh if viewId matches current (prevents race condition) */
    case WM_APP_REFRESH:
        if ((int)wParam == g_currentView)
            SwitchView(hWnd, (int)wParam);
        return 0;

    /* ---------- WM_CLOSE: 窗口关闭请求 ----------
       用户点击标题栏关闭按钮或按 Alt+F4 时触发。
       直接调用 DestroyWindow 进入销毁流程 → 触发 WM_DESTROY。 */
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    /* ---------- WM_TIMER: 定时器消息 (文件轮询) ----------
       每 REFRESH_INTERVAL_MS (3 秒) 触发一次。

       目的: 检测其他进程实例 (如另一个同时运行的 GUI) 是否修改了数据文件,
       如果有变更则自动刷新当前视图, 实现多实例间的数据同步。

       检查策略 (短路求值, 找到第一个变更后立即停止):
         1. appointments.txt     → 影响 挂号记录页 / 待接诊页 / 接诊页
         2. medical_records.txt  → 影响 诊断结果页
         3. prescriptions.txt   → 影响 缴费页 / 处方页
         4. ward_calls.txt      → 影响 缴费页 (病房呼叫与缴费关联)
         5. wards.txt           → 影响 住院信息页

       对每个文件: stat() 获取当前 mtime → 与缓存值比较 → 不等则标记 dirty=1
       如果当前页面受该文件影响且发生了变更, 则标记 dirty=1 并立即退出检查。

       最后: 如果 dirty=1, 调用 SwitchView 重建当前视图 (刷新数据)。
       安全条件: g_currentView > 0 (已显示页面时才轮询, 登录界面不轮询)

       File polling: detect if other instances modified data files */
    case WM_TIMER: {
        if (wParam == REFRESH_TIMER_ID && g_currentView > 0) {
            int dirty = 0;
            struct stat st;

            /* 检查 appointments.txt — 影响挂号/待接诊/接诊页面 */
            if (!dirty && stat(APPOINTMENTS_FILE, &st) == 0) {
                if (st.st_mtime != g_lastApptMtime) {
                    g_lastApptMtime = st.st_mtime;
                    if (g_currentView == NAV_PATIENT_APPOINTMENT ||
                        g_currentView == NAV_DOCTOR_REMINDER ||
                        g_currentView == NAV_DOCTOR_CONSULTATION)
                        dirty = 1;
                }
            }
            /* 检查 medical_records.txt — 影响诊断结果页面 */
            if (!dirty && stat(MEDICAL_RECORDS_FILE, &st) == 0) {
                if (st.st_mtime != g_lastRecordMtime) {
                    g_lastRecordMtime = st.st_mtime;
                    if (g_currentView == NAV_PATIENT_DIAGNOSIS)
                        dirty = 1;
                }
            }
            /* 检查 prescriptions.txt — 影响缴费页面 */
            if (!dirty && stat(PRESCRIPTIONS_FILE, &st) == 0) {
                if (st.st_mtime != g_lastRxMtime) {
                    g_lastRxMtime = st.st_mtime;
                    if (g_currentView == NAV_PATIENT_PRESCRIPTION)
                        dirty = 1;
                }
            }
            /* 检查 ward_calls.txt — 影响缴费页面 (病房呼叫关联缴费) */
            if (!dirty && stat(WARD_CALLS_FILE, &st) == 0) {
                if (st.st_mtime != g_lastWardCallMtime) {
                    g_lastWardCallMtime = st.st_mtime;
                    if (g_currentView == NAV_PATIENT_PRESCRIPTION)
                        dirty = 1;
                }
            }
            /* 检查 wards.txt — 影响住院信息页面 */
            if (!dirty && stat(WARDS_FILE, &st) == 0) {
                if (st.st_mtime != g_lastWardMtime) {
                    g_lastWardMtime = st.st_mtime;
                    if (g_currentView == NAV_PATIENT_WARD)
                        dirty = 1;
                }
            }

            /* 如果有变更则重建当前视图, 重新从文件加载最新数据 */
            if (dirty)
                SwitchView(hWnd, g_currentView);
        }
        return 0;
    }

    /* ---------- WM_DESTROY: 窗口销毁消息 ----------
       在 DestroyWindow 被调用后、窗口彻底销毁前触发。
       执行清理:
       1. KillTimer 停止文件轮询定时器 (防止窗口句柄失效后继续触发)
       2. PostQuitMessage 向消息队列发送 WM_QUIT, 使 GetMessage 返回 0 退出消息循环
       Cleanup: kill timer, post quit message */
    case WM_DESTROY:
        KillTimer(hWnd, REFRESH_TIMER_ID);
        PostQuitMessage(0);
        return 0;

    /* ---------- 默认消息处理 ----------
       所有未显式处理的消息 (如 WM_PAINT, WM_KEYDOWN, WM_SETFOCUS 等)
       交由系统默认处理 (DefWindowProcA), 保证窗口的正常行为。 */
    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
}
