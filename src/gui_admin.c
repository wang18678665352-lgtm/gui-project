/*
 * gui_admin.c — Win32 GUI 管理员界面实现 / Win32 GUI admin page implementation
 *
 * 实现管理员角色的所有 GUI 页面 (10 个页面):
 *   - 科室管理 (CreateDeptPage) — 科室 CRUD (新增/编辑/删除)
 *   - 医生管理 (CreateDoctorMgmtPage) — 医生 CRUD, 含繁忙度字段
 *   - 患者管理 (CreatePatientMgmtPage) — 患者 CRUD + 刷新
 *   - 药品管理 (CreateDrugPage) — 药品 CRUD + 补货
 *   - 病房管理 (CreateWardPage) — 病房 CRUD (类型/总床位/剩余/预警)
 *   - 排班管理 (CreateSchedulePage) — 生成排班 + 停诊
 *   - 操作日志 (CreateLogPage) — 审计日志列表 (只读)
 *   - 数据管理 (CreateDataPage) — 数据备份/恢复 + 备份列表
 *   - 报表统计 (CreateAnalysisPage) — TabControl 三页: 概览/医生负载/财务统计
 *   - 重置密码 (CreateResetPwdPage) — 用户列表→选中→SHA-256 重置密码
 *
 * 所有 CRUD 页面共享 AdminPageWndProc, 通过 GWLP_USERDATA 存储 viewId 路由。
 * 通用 FieldDlgProc 模态对话框支持最多 16 字段的动态表单。
 * 数据填充函数 (PopulateXxxList) 统一从文件加载 → 填充 ListView → 释放链表。
 *
 * Implements all 10 admin-role GUI pages with generic CRUD via FieldDlgProc,
 * backup/restore with auto-cleanup (max 10 backups), TabControl-based
 * analysis dashboard, and SHA-256 password reset with audit logging.
 */

#include "gui_admin.h"
#include "gui_main.h"
#include "data_storage.h"
#include "public.h"
#include "sha256.h"
#include <time.h>
#include <stdio.h>

static int ShowResetPwdDialog(HWND hParent, const char *username);
static LRESULT CALLBACK ResetPwdDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
static int g_pwdResult = 0;

/* ─── ListView 工具 / ListView Utilities ──────────────────────────────── */

static HWND CreateListView(HWND hParent, int id, int x, int y, int w, int h) {
    HWND hLV = CreateWindowA(WC_LISTVIEWA, "",
        WS_VISIBLE | WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS |
        LVS_SINGLESEL | WS_BORDER,
        x, y, w, h, hParent, (HMENU)(INT_PTR)id, g_hInst, NULL);
    ListView_SetExtendedListViewStyle(hLV, LVS_EX_FULLROWSELECT |
                                      LVS_EX_GRIDLINES | LVS_EX_ALTERNATINGROWCOLORS);
    return hLV;
}

static void AddCol(HWND hLV, int idx, const char *text, int width) {
    LV_COLUMNA col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = (char *)text;
    col.cx = width;
    ListView_InsertColumn(hLV, idx, &col);
}

static void AddRow(HWND hLV, int row, int cols, const char **items) {
    LV_ITEMA li = {0};
    li.mask = LVIF_TEXT;
    li.pszText = (char *)items[0];
    li.iItem = row;
    ListView_InsertItem(hLV, &li);
    for (int c = 1; c < cols; c++) {
        li.iSubItem = c;
        li.pszText = (char *)items[c];
        ListView_SetItem(hLV, &li);
    }
}

static void ClearLV(HWND hLV) {
    ListView_DeleteAllItems(hLV);
}

/* 获取选中的 ListView 指定列文本 (无选中时返回空串)
   Get text from selected ListView row at column, or empty if none selected */
static void GetSelectedItemText(HWND hLV, int col, char *buf, int size) {
    int sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
    if (sel >= 0) {
        ListView_GetItemText(hLV, sel, col, buf, size);
    } else {
        buf[0] = 0;
    }
}

/* ─── 字体管理 / Font Management ─────────────────────────────────────── */

/* GetAdminFont — 创建/缓存 Microsoft YaHei 14pt 字体 (单例)
   Create/cache a Microsoft YaHei 14pt font (singleton pattern) */

static HFONT g_hAdminFont = NULL;

static HFONT GetAdminFont(void) {
    if (!g_hAdminFont) {
        g_hAdminFont = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Microsoft YaHei");
    }
    return g_hAdminFont;
}

/* EnumChildWindows 回调: 为每个子控件设置字体 / Callback to set font on each child */
static BOOL CALLBACK SetChildFontEnum(HWND hChild, LPARAM lParam) {
    SendMessage(hChild, WM_SETFONT, lParam, TRUE);
    return TRUE;
}

static void ApplyDefaultFont(HWND hWnd) {
    HFONT hFont = GetAdminFont();
    if (hFont) {
        SendMessage(hWnd, WM_SETFONT, (WPARAM)hFont, TRUE);
        EnumChildWindows(hWnd, SetChildFontEnum, (LPARAM)hFont);
    }
}

/* ─── 字段编辑对话框 (通用) / Generic Field Edit Dialog ─────────────── */

/* FieldDlgProc + ShowFieldDialog: 通用多字段编辑对话框
   支持最多 FD_FIELDS_MAX (16) 个字段, 每个字段可设 read_only
   Generic multi-field edit dialog supporting up to 16 fields with read_only option */

#define FD_FIELDS_MAX  16

typedef struct {
    char label[32];
    char value[256];
    int  max_len;
    BOOL read_only;
} FieldDef;

static int g_fdResult = 0; /* -1=running / 运行中, 0=cancel / 取消, 1=ok / 确定 */

static LRESULT CALLBACK FieldDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
        FieldDef *fields = (FieldDef *)cs->lpCreateParams;
        SetWindowLongPtrA(hDlg, GWLP_USERDATA, (LONG_PTR)fields);

        int count = 0;
        while (count < FD_FIELDS_MAX && fields[count].label[0]) count++;

        RECT rc;
        GetClientRect(hDlg, &rc);
        int w = rc.right - rc.left;
        int y = 15;

        for (int i = 0; i < count; i++) {
            CreateWindowA("STATIC", fields[i].label,
                WS_VISIBLE | WS_CHILD | SS_RIGHT,
                10, y + 2, 80, 20, hDlg, NULL, g_hInst, NULL);
            DWORD style = WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL;
            if (fields[i].read_only) style |= ES_READONLY;
            HWND hEdit = CreateWindowA("EDIT", fields[i].value, style,
                100, y, w - 120, 22, hDlg, (HMENU)(1000 + i), g_hInst, NULL);
            SendMessageA(hEdit, EM_SETLIMITTEXT, fields[i].max_len - 1, 0);
            y += 30;
        }

        y += 10;
        CreateWindowA("BUTTON", "确定", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            w / 2 - 95, y, 80, 28, hDlg, (HMENU)1, g_hInst, NULL);
        CreateWindowA("BUTTON", "取消", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            w / 2 + 15, y, 80, 28, hDlg, (HMENU)2, g_hInst, NULL);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == 1) {
            FieldDef *fields = (FieldDef *)GetWindowLongPtrA(hDlg, GWLP_USERDATA);
            if (fields) {
                for (int i = 0; i < FD_FIELDS_MAX && fields[i].label[0]; i++)
                    GetDlgItemTextA(hDlg, 1000 + i, fields[i].value, fields[i].max_len);
            }
            g_fdResult = 1;
            DestroyWindow(hDlg);
        } else if (id == 2) {
            g_fdResult = 0;
            DestroyWindow(hDlg);
        }
        return 0;
    }
    case WM_CLOSE:
        g_fdResult = 0;
        DestroyWindow(hDlg);
        return 0;
    default:
        return DefWindowProcA(hDlg, msg, wParam, lParam);
    }
}

static int ShowFieldDialog(HWND hParent, const char *title,
                           FieldDef *fields, int field_count) {
    if (field_count <= 0 || field_count > FD_FIELDS_MAX) return 0;
    fields[field_count].label[0] = 0;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = FieldDlgProc;
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "FieldDialog";
    RegisterClassA(&wc);

    int w = 400, h = 50 + field_count * 30 + 55;

    HWND hDlg = CreateWindowExA(0, "FieldDialog", title,
        WS_VISIBLE | WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        hParent, NULL, g_hInst, (LPVOID)fields);
    if (!hDlg) return 0;

    RECT pr, rc;
    GetWindowRect(hParent, &pr);
    GetWindowRect(hDlg, &rc);
    SetWindowPos(hDlg, NULL,
        pr.left + (pr.right - pr.left - (rc.right - rc.left)) / 2,
        pr.top + (pr.bottom - pr.top - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(hParent, FALSE);
    g_fdResult = -1;
    MSG msg;
    while (g_fdResult == -1 && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);

    return g_fdResult == 1;
}

/* ─── 刷新各页面 ListView / Refresh Each Page's ListView ────────────── */

/* 各 PopulateXxxList 函数: 从文件加载链表 → 清空 ListView → 逐行填充 → 释放链表
   Each Populate function: load from file → clear ListView → populate rows → free list */

static void PopulateDeptList(HWND hLV) {
    ClearLV(hLV);
    DepartmentNode *list = load_departments_list();
    int row = 0;
    for (DepartmentNode *cur = list; cur; cur = cur->next) {
        const char *items[4] = { cur->data.department_id, cur->data.name,
                                 cur->data.leader, cur->data.phone };
        AddRow(hLV, row++, 4, items);
    }
    free_department_list(list);
}

static void PopulateDocList(HWND hLV) {
    ClearLV(hLV);
    DoctorNode *list = load_doctors_list();
    int row = 0;
    for (DoctorNode *cur = list; cur; cur = cur->next) {
        char busy[16];
        snprintf(busy, sizeof(busy), "%d", cur->data.busy_level);
        const char *items[5] = { cur->data.doctor_id, cur->data.name,
                                 cur->data.department_id, cur->data.title, busy };
        AddRow(hLV, row++, 5, items);
    }
    free_doctor_list(list);
}

static void PopulatePatList(HWND hLV) {
    ClearLV(hLV);
    PatientNode *list = load_patients_list();
    int row = 0;
    for (PatientNode *cur = list; cur; cur = cur->next) {
        char age[16];
        snprintf(age, sizeof(age), "%d", cur->data.age);
        const char *items[8] = { cur->data.patient_id, cur->data.name,
            cur->data.gender, age, cur->data.phone,
            cur->data.patient_type, cur->data.treatment_stage,
            cur->data.is_emergency ? "是" : "否" };
        AddRow(hLV, row++, 8, items);
    }
    free_patient_list(list);
}

static void PopulateDrugList(HWND hLV) {
    ClearLV(hLV);
    DrugNode *list = load_drugs_list();
    int row = 0;
    for (DrugNode *cur = list; cur; cur = cur->next) {
        char price[16], stock[16], warn[16], ratio[16];
        snprintf(price, sizeof(price), "%.2f", cur->data.price);
        if (cur->data.stock_num <= cur->data.warning_line)
            snprintf(stock, sizeof(stock), "[!] %d", cur->data.stock_num);
        else
            snprintf(stock, sizeof(stock), "%d", cur->data.stock_num);
        snprintf(warn, sizeof(warn), "%d", cur->data.warning_line);
        snprintf(ratio, sizeof(ratio), "%.0f%%", cur->data.reimbursement_ratio * 100);
        const char *items[7] = { cur->data.drug_id, cur->data.name,
            price, stock, warn, ratio, cur->data.category };
        AddRow(hLV, row++, 7, items);
    }
    free_drug_list(list);
}

static void PopulateWardList(HWND hLV) {
    ClearLV(hLV);
    WardNode *list = load_wards_list();
    int row = 0;
    for (WardNode *cur = list; cur; cur = cur->next) {
        char total[16], remain[16], warn[16];
        snprintf(total, sizeof(total), "%d", cur->data.total_beds);
        snprintf(remain, sizeof(remain), "%d", cur->data.remain_beds);
        snprintf(warn, sizeof(warn), "%d", cur->data.warning_line);
        const char *items[5] = { cur->data.ward_id, cur->data.type,
                                 total, remain, warn };
        AddRow(hLV, row++, 5, items);
    }
    free_ward_list(list);
}

static void PopulateSchedList(HWND hLV) {
    ClearLV(hLV);
    ScheduleNode *list = load_schedules_list();
    int row = 0;
    for (ScheduleNode *cur = list; cur; cur = cur->next) {
        const char *items[5] = { cur->data.schedule_id, cur->data.doctor_id,
            cur->data.work_date, cur->data.time_slot, cur->data.status };
        AddRow(hLV, row++, 5, items);
    }
    free_schedule_list(list);
}

/* ─── 读取选中行到 FieldDef / Get Selected Row into FieldDef Array ─── */

/* GetSelectedRow: 将 ListView 选中行的各列值读出到 FieldDef 数组
   Reads column values from the selected ListView row into FieldDef array */

static void GetSelectedRow(HWND hLV, int col_count, FieldDef *fields) {
    int sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
    if (sel < 0) return;
    for (int c = 0; c < col_count && c < FD_FIELDS_MAX; c++) {
        ListView_GetItemText(hLV, sel, c, fields[c].value, fields[c].max_len);
    }
}

/* ─── 通用管理页面 WndProc / Generic Admin Page WndProc ──────────────── */

/* AdminPageWndProc — 所有管理员 CRUD 页面的统一消息处理
   WM_CREATE: 通过 GWLP_USERDATA 存储 viewId
   WM_COMMAND: 按 viewId 路由到各模块的 CRUD 分支 (科室/医生/患者/药品/病房/排班/数据)
   WM_NOTIFY: 处理分析页面的 Tab 切换 (TCN_SELCHANGE) */

static HWND g_analysisTabPages[3];

/* ─── 生成排班对话框 / Generate Schedule Dialog ─────────────────────── */

static int g_genSchedResult = 0;

static LRESULT CALLBACK GenSchedDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        int y = 20, w = 350;

        CreateWindowA("STATIC", "选择医生:", WS_VISIBLE | WS_CHILD, 20, y + 2, 80, 20, hDlg, NULL, g_hInst, NULL);
        HWND hDoc = CreateWindowA("COMBOBOX", "", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
                                  110, y, 200, 200, hDlg, (HMENU)100, g_hInst, NULL);
        DoctorNode *docs = load_doctors_list();
        for (DoctorNode *d = docs; d; d = d->next) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s (%s)", d->data.name, d->data.doctor_id);
            SendMessageA(hDoc, CB_ADDSTRING, 0, (LPARAM)buf);
        }
        free_doctor_list(docs);
        SendMessage(hDoc, CB_SETCURSEL, 0, 0);
        y += 35;

        CreateWindowA("STATIC", "选择日期:", WS_VISIBLE | WS_CHILD, 20, y + 2, 80, 20, hDlg, NULL, g_hInst, NULL);
        HWND hDate = CreateWindowA("COMBOBOX", "", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
                                   110, y, 200, 200, hDlg, (HMENU)101, g_hInst, NULL);
        time_t now = time(NULL);
        for (int i = 0; i < 30; i++) {
            struct tm *tm = localtime(&now);
            char buf[20];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
            SendMessageA(hDate, CB_ADDSTRING, 0, (LPARAM)buf);
            now += 86400;
        }
        SendMessage(hDate, CB_SETCURSEL, 0, 0);
        y += 35;

        CreateWindowA("STATIC", "选择时段:", WS_VISIBLE | WS_CHILD, 20, y + 2, 80, 20, hDlg, NULL, g_hInst, NULL);
        HWND hTime = CreateWindowA("COMBOBOX", "", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
                                   110, y, 200, 200, hDlg, (HMENU)102, g_hInst, NULL);
        const char *slots[] = {"上午(08:00-12:00)", "下午(14:00-17:00)", "晚上(18:00-21:00)"};
        for (int i = 0; i < 3; i++) SendMessageA(hTime, CB_ADDSTRING, 0, (LPARAM)slots[i]);
        SendMessage(hTime, CB_SETCURSEL, 0, 0);
        y += 45;

        CreateWindowA("STATIC", "预约号源上限:", WS_VISIBLE | WS_CHILD, 20, y + 2, 100, 20, hDlg, NULL, g_hInst, NULL);
        HWND hMaxAppt = CreateWindowA("EDIT", "20", WS_VISIBLE | WS_CHILD | WS_BORDER,
                                      130, y, 60, 22, hDlg, (HMENU)103, g_hInst, NULL);
        y += 30;

        CreateWindowA("STATIC", "现场号源上限:", WS_VISIBLE | WS_CHILD, 20, y + 2, 100, 20, hDlg, NULL, g_hInst, NULL);
        HWND hMaxOnsite = CreateWindowA("EDIT", "10", WS_VISIBLE | WS_CHILD | WS_BORDER,
                                        130, y, 60, 22, hDlg, (HMENU)104, g_hInst, NULL);
        y += 40;

        CreateWindowA("BUTTON", "确认生成", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 60, y, 100, 30, hDlg, (HMENU)IDOK, g_hInst, NULL);
        CreateWindowA("BUTTON", "取消", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 180, y, 100, 30, hDlg, (HMENU)IDCANCEL, g_hInst, NULL);
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDOK) {
            char docBuf[128], dateBuf[20], timeBuf[64];
            GetDlgItemTextA(hDlg, 100, docBuf, sizeof(docBuf));
            GetDlgItemTextA(hDlg, 101, dateBuf, sizeof(dateBuf));
            GetDlgItemTextA(hDlg, 102, timeBuf, sizeof(timeBuf));

            char maxApptBuf[16] = "20", maxOnsiteBuf[16] = "10";
            GetDlgItemTextA(hDlg, 103, maxApptBuf, sizeof(maxApptBuf));
            GetDlgItemTextA(hDlg, 104, maxOnsiteBuf, sizeof(maxOnsiteBuf));
            int maxApptVal = atoi(maxApptBuf);
            int maxOnsiteVal = atoi(maxOnsiteBuf);
            if (maxApptVal <= 0) maxApptVal = 20;
            if (maxOnsiteVal <= 0) maxOnsiteVal = 10;

            char *paren = strchr(docBuf, '(');
            if (!paren) return 0;
            char docId[MAX_ID] = "";
            strncpy(docId, paren + 1, strlen(paren) - 2);

            Schedule s;
            generate_id(s.schedule_id, sizeof(s.schedule_id), "SCH");
            strcpy(s.doctor_id, docId);
            strcpy(s.work_date, dateBuf);
            strcpy(s.time_slot, timeBuf);
            strcpy(s.status, "正常");
            s.max_appt = maxApptVal; s.max_onsite = maxOnsiteVal;

            ScheduleNode *head = load_schedules_list();
            ScheduleNode *node = create_schedule_node(&s);
            node->next = head;
            save_schedules_list(node);
            free_schedule_list(node);

            g_genSchedResult = 1;
            DestroyWindow(hDlg);
        } else if (LOWORD(wParam) == IDCANCEL) {
            g_genSchedResult = 0;
            DestroyWindow(hDlg);
        }
        return 0;
    }
    case WM_CLOSE:
        g_genSchedResult = 0;
        DestroyWindow(hDlg);
        return 0;
    default:
        return DefWindowProcA(hDlg, msg, wParam, lParam);
    }
}

static int ShowGenSchedDialog(HWND hParent) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = GenSchedDlgProc;
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "GenSchedDialog";
    RegisterClassA(&wc);

    HWND hDlg = CreateWindowExA(0, "GenSchedDialog", "生成排班",
        WS_VISIBLE | WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 380, 310,
        hParent, NULL, g_hInst, NULL);
    
    EnableWindow(hParent, FALSE);
    g_genSchedResult = -1;
    MSG msg;
    while (g_genSchedResult == -1 && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
    return g_genSchedResult == 1;
}

static LRESULT CALLBACK AdminPageWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    int viewId = (int)GetWindowLongPtrA(hWnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_SIZE:
        return 0;

    case WM_COMMAND: {
        int cmd = LOWORD(wParam);
        HWND hLV;
        int sel;

        switch (viewId) {

        /* ── 科室管理 ─────────────────────────────────────────── */
        case NAV_ADMIN_DEPT:
            hLV = GetDlgItem(hWnd, 4001);
            switch (cmd) {
            case 4101: { /* 新增 */
                FieldDef f[4] = {
                    {"名称", "", MAX_NAME, FALSE},
                    {"负责人", "", MAX_NAME, FALSE},
                    {"电话", "", 20, FALSE},
                };
                if (ShowFieldDialog(hWnd, "新增科室", f, 3)) {
                    Department d;
                    generate_id(d.department_id, sizeof(d.department_id), "DEP");
                    strcpy(d.name, f[0].value);
                    strcpy(d.leader, f[1].value);
                    strcpy(d.phone, f[2].value);
                    DepartmentNode *head = load_departments_list();
                    DepartmentNode *node = create_department_node(&d);
                    node->next = head;
                    save_departments_list(node);
                    free_department_list(node);
                    PopulateDeptList(hLV);
                }
                return 0;
            }
            case 4102: { /* 编辑 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一行", "提示", MB_OK); return 0; }
                FieldDef f[5] = {
                    {"科室ID", "", MAX_ID, TRUE},
                    {"名称", "", MAX_NAME, FALSE},
                    {"负责人", "", MAX_NAME, FALSE},
                    {"电话", "", 20, FALSE},
                    {"", "", 0, FALSE}
                };
                GetSelectedRow(hLV, 4, f);
                if (ShowFieldDialog(hWnd, "编辑科室", f, 4)) {
                    DepartmentNode *head = load_departments_list();
                    DepartmentNode *cur = head;
                    char targetId[32];
                    strcpy(targetId, f[0].value);
                    while (cur) {
                        if (strcmp(cur->data.department_id, targetId) == 0) {
                            strcpy(cur->data.name, f[1].value);
                            strcpy(cur->data.leader, f[2].value);
                            strcpy(cur->data.phone, f[3].value);
                            break;
                        }
                        cur = cur->next;
                    }
                    save_departments_list(head);
                    free_department_list(head);
                    PopulateDeptList(hLV);
                }
                return 0;
            }
            case 4103: { /* 删除 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一行", "提示", MB_OK); return 0; }
                if (MessageBoxA(hWnd, "确定要删除该科室吗？", "确认", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
                char targetId[32];
                ListView_GetItemText(hLV, sel, 0, targetId, sizeof(targetId));
                DepartmentNode *head = load_departments_list();
                DepartmentNode *prev = NULL, *cur = head;
                while (cur) {
                    if (strcmp(cur->data.department_id, targetId) == 0) {
                        if (prev) prev->next = cur->next;
                        else head = cur->next;
                        free(cur);
                        break;
                    }
                    prev = cur;
                    cur = cur->next;
                }
                save_departments_list(head);
                free_department_list(head);
                PopulateDeptList(hLV);
                return 0;
            }
            }
            return 0;

        /* ── 医生管理 ─────────────────────────────────────────── */
        case NAV_ADMIN_DOCTOR:
            hLV = GetDlgItem(hWnd, 4002);
            switch (cmd) {
            case 4201: { /* 新增 */
                FieldDef f[4] = {
                    {"姓名", "", MAX_NAME, FALSE},
                    {"科室", "", MAX_ID, FALSE},
                    {"职称", "", 50, FALSE},
                    {"繁忙度", "", 16, FALSE},
                };
                if (ShowFieldDialog(hWnd, "新增医生", f, 4)) {
                    Doctor d;
                    generate_id(d.doctor_id, sizeof(d.doctor_id), "DOC");
                    strcpy(d.name, f[0].value);
                    strcpy(d.department_id, f[1].value);
                    strcpy(d.title, f[2].value);
                    d.busy_level = atoi(f[3].value);
                    DoctorNode *head = load_doctors_list();
                    DoctorNode *node = create_doctor_node(&d);
                    node->next = head;
                    save_doctors_list(node);
                    free_doctor_list(node);
                    PopulateDocList(hLV);
                }
                return 0;
            }
            case 4202: { /* 编辑 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一行", "提示", MB_OK); return 0; }
                FieldDef f[6] = {
                    {"医生ID", "", MAX_ID, TRUE},
                    {"姓名", "", MAX_NAME, FALSE},
                    {"科室", "", MAX_ID, FALSE},
                    {"职称", "", 50, FALSE},
                    {"繁忙度", "", 16, FALSE},
                    {"", "", 0, FALSE}
                };
                GetSelectedRow(hLV, 5, f);
                if (ShowFieldDialog(hWnd, "编辑医生", f, 5)) {
                    char targetId[32];
                    strcpy(targetId, f[0].value);
                    DoctorNode *head = load_doctors_list();
                    for (DoctorNode *cur = head; cur; cur = cur->next) {
                        if (strcmp(cur->data.doctor_id, targetId) == 0) {
                            strcpy(cur->data.name, f[1].value);
                            strcpy(cur->data.department_id, f[2].value);
                            strcpy(cur->data.title, f[3].value);
                            cur->data.busy_level = atoi(f[4].value);
                            break;
                        }
                    }
                    save_doctors_list(head);
                    free_doctor_list(head);
                    PopulateDocList(hLV);
                }
                return 0;
            }
            case 4203: { /* 删除 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一行", "提示", MB_OK); return 0; }
                if (MessageBoxA(hWnd, "确定要删除该医生吗？", "确认", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
                char targetId[32];
                ListView_GetItemText(hLV, sel, 0, targetId, sizeof(targetId));
                DoctorNode *head = load_doctors_list();
                DoctorNode *prev = NULL, *cur = head;
                while (cur) {
                    if (strcmp(cur->data.doctor_id, targetId) == 0) {
                        if (prev) prev->next = cur->next;
                        else head = cur->next;
                        free(cur);
                        break;
                    }
                    prev = cur;
                    cur = cur->next;
                }
                save_doctors_list(head);
                free_doctor_list(head);
                PopulateDocList(hLV);
                return 0;
            }
            case 4204: { /* 重置密码 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一位医生", "提示", MB_OK); return 0; }
                char targetId[32];
                ListView_GetItemText(hLV, sel, 0, targetId, sizeof(targetId));
                char username[MAX_USERNAME] = "";
                DoctorNode *head = load_doctors_list();
                for (DoctorNode *cur = head; cur; cur = cur->next) {
                    if (strcmp(cur->data.doctor_id, targetId) == 0) {
                        strcpy(username, cur->data.username);
                        break;
                    }
                }
                free_doctor_list(head);
                if (username[0]) ShowResetPwdDialog(GetParent(hWnd), username);
                return 0;
            }
            }
            return 0;

        /* ── 患者管理 ─────────────────────────────────────────── */
        case NAV_ADMIN_PATIENT:
            hLV = GetDlgItem(hWnd, 4003);
            switch (cmd) {
            case 4301: { /* 新增 */
                FieldDef f[7] = {
                    {"用户名", "", MAX_USERNAME, FALSE},
                    {"姓名", "", MAX_NAME, FALSE},
                    {"性别", "", 10, FALSE},
                    {"年龄", "", 16, FALSE},
                    {"电话", "", 20, FALSE},
                    {"患者类型", "", 20, FALSE},
                    {"治疗阶段", "", 20, FALSE},
                };
                if (ShowFieldDialog(hWnd, "新增患者", f, 7)) {
                    Patient p;
                    generate_id(p.patient_id, sizeof(p.patient_id), "PAT");
                    strcpy(p.username, f[0].value);
                    strcpy(p.name, f[1].value);
                    strcpy(p.gender, f[2].value);
                    p.age = atoi(f[3].value);
                    strcpy(p.phone, f[4].value);
                    strcpy(p.patient_type, f[5].value);
                    strcpy(p.treatment_stage, f[6].value);
                    p.is_emergency = 0;
                    PatientNode *head = load_patients_list();
                    PatientNode *node = create_patient_node(&p);
                    node->next = head;
                    save_patients_list(node);
                    free_patient_list(node);
                    PopulatePatList(hLV);
                }
                return 0;
            }
            case 4302: { /* 编辑 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一行", "提示", MB_OK); return 0; }
                FieldDef f[7] = {
                    {"用户名", "", MAX_USERNAME, FALSE},
                    {"姓名", "", MAX_NAME, FALSE},
                    {"性别", "", 10, FALSE},
                    {"年龄", "", 16, FALSE},
                    {"电话", "", 20, FALSE},
                    {"患者类型", "", 20, FALSE},
                    {"治疗阶段", "", 20, FALSE},
                };
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                char idBuf[MAX_ID] = {0};
                ListView_GetItemText(hLV, sel, 0, idBuf, sizeof(idBuf));
                /* 从患者数据加载用户名（ListView 无此列） */
                {
                    PatientNode *ph = load_patients_list();
                    for (PatientNode *pc = ph; pc; pc = pc->next) {
                        if (strcmp(pc->data.patient_id, idBuf) == 0) {
                            strcpy(f[0].value, pc->data.username);
                            break;
                        }
                    }
                    free_patient_list(ph);
                }
                /* 从 ListView 读取，col 1-6 对应 f[1]-f[6] */
                ListView_GetItemText(hLV, sel, 1, f[1].value, f[1].max_len);
                ListView_GetItemText(hLV, sel, 2, f[2].value, f[2].max_len);
                ListView_GetItemText(hLV, sel, 3, f[3].value, f[3].max_len);
                ListView_GetItemText(hLV, sel, 4, f[4].value, f[4].max_len);
                ListView_GetItemText(hLV, sel, 5, f[5].value, f[5].max_len);
                ListView_GetItemText(hLV, sel, 6, f[6].value, f[6].max_len);
                if (ShowFieldDialog(hWnd, "编辑患者", f, 7)) {
                    PatientNode *head = load_patients_list();
                    for (PatientNode *cur = head; cur; cur = cur->next) {
                        if (strcmp(cur->data.patient_id, idBuf) == 0) {
                            strcpy(cur->data.username, f[0].value);
                            strcpy(cur->data.name, f[1].value);
                            strcpy(cur->data.gender, f[2].value);
                            cur->data.age = atoi(f[3].value);
                            strcpy(cur->data.phone, f[4].value);
                            strcpy(cur->data.patient_type, f[5].value);
                            strcpy(cur->data.treatment_stage, f[6].value);
                            break;
                        }
                    }
                    save_patients_list(head);
                    free_patient_list(head);
                    PopulatePatList(hLV);
                }
                return 0;
            }
            case 4305: { /* 重置密码 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一位患者", "提示", MB_OK); return 0; }
                char targetId[32];
                ListView_GetItemText(hLV, sel, 0, targetId, sizeof(targetId));
                char username[MAX_USERNAME] = "";
                PatientNode *head = load_patients_list();
                for (PatientNode *cur = head; cur; cur = cur->next) {
                    if (strcmp(cur->data.patient_id, targetId) == 0) {
                        strcpy(username, cur->data.username);
                        break;
                    }
                }
                free_patient_list(head);
                if (username[0]) ShowResetPwdDialog(GetParent(hWnd), username);
                return 0;
            }
            case 4303: { /* 删除 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一行", "提示", MB_OK); return 0; }
                if (MessageBoxA(hWnd, "确定要删除该患者吗？", "确认", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
                char targetId[32];
                ListView_GetItemText(hLV, sel, 0, targetId, sizeof(targetId));
                PatientNode *head = load_patients_list();
                PatientNode *prev = NULL, *cur = head;
                while (cur) {
                    if (strcmp(cur->data.patient_id, targetId) == 0) {
                        if (prev) prev->next = cur->next;
                        else head = cur->next;
                        free(cur);
                        break;
                    }
                    prev = cur;
                    cur = cur->next;
                }
                save_patients_list(head);
                free_patient_list(head);
                PopulatePatList(hLV);
                return 0;
            }
            case 4304: /* 刷新 */
                PopulatePatList(hLV);
                return 0;
            }
            return 0;

        /* ── 药品管理 ─────────────────────────────────────────── */
        case NAV_ADMIN_DRUG:
            hLV = GetDlgItem(hWnd, 4004);
            switch (cmd) {
            case 4401: { /* 新增 */
                FieldDef f[6] = {
                    {"名称", "", MAX_NAME, FALSE},
                    {"价格", "", 16, FALSE},
                    {"库存", "", 16, FALSE},
                    {"预警线", "", 16, FALSE},
                    {"报销比例%", "", 16, FALSE},
                    {"分类", "", 20, FALSE},
                };
                if (ShowFieldDialog(hWnd, "新增药品", f, 6)) {
                    Drug d;
                    generate_id(d.drug_id, sizeof(d.drug_id), "DRG");
                    strcpy(d.name, f[0].value);
                    d.price = (float)atof(f[1].value);
                    d.stock_num = atoi(f[2].value);
                    d.warning_line = atoi(f[3].value);
                    d.reimbursement_ratio = (float)atof(f[4].value) / 100.0f;
                    strcpy(d.category, f[5].value);
                    d.is_special = 0;
                    DrugNode *head = load_drugs_list();
                    DrugNode *node = create_drug_node(&d);
                    node->next = head;
                    save_drugs_list(node);
                    free_drug_list(node);
                    PopulateDrugList(hLV);
                }
                return 0;
            }
            case 4402: { /* 编辑 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一行", "提示", MB_OK); return 0; }
                FieldDef f[6] = {
                    {"名称", "", MAX_NAME, FALSE},
                    {"价格", "", 16, FALSE},
                    {"库存", "", 16, FALSE},
                    {"预警线", "", 16, FALSE},
                    {"报销比例%", "", 16, FALSE},
                    {"分类", "", 20, FALSE},
                };
                char idBuf[MAX_ID] = {0};
                ListView_GetItemText(hLV, sel, 0, idBuf, sizeof(idBuf));
                ListView_GetItemText(hLV, sel, 1, f[0].value, f[0].max_len);
                ListView_GetItemText(hLV, sel, 2, f[1].value, f[1].max_len);
                ListView_GetItemText(hLV, sel, 3, f[2].value, f[2].max_len);
                ListView_GetItemText(hLV, sel, 4, f[3].value, f[3].max_len);
                /* 报销比例列显示 "XX%"，去掉 % 符号后再解析
                   Strip % suffix from reimbursement ratio column before parsing */
                {
                    char ratioCol[32];
                    ListView_GetItemText(hLV, sel, 5, ratioCol, sizeof(ratioCol));
                    char *pct = strchr(ratioCol, '%');
                    if (pct) *pct = 0;
                    strcpy(f[4].value, ratioCol);
                }
                ListView_GetItemText(hLV, sel, 6, f[5].value, f[5].max_len);
                if (ShowFieldDialog(hWnd, "编辑药品", f, 6)) {
                    DrugNode *head = load_drugs_list();
                    for (DrugNode *cur = head; cur; cur = cur->next) {
                        if (strcmp(cur->data.drug_id, idBuf) == 0) {
                            strcpy(cur->data.name, f[0].value);
                            cur->data.price = (float)atof(f[1].value);
                            cur->data.stock_num = atoi(f[2].value);
                            cur->data.warning_line = atoi(f[3].value);
                            cur->data.reimbursement_ratio = (float)atof(f[4].value) / 100.0f;
                            strcpy(cur->data.category, f[5].value);
                            break;
                        }
                    }
                    save_drugs_list(head);
                    free_drug_list(head);
                    PopulateDrugList(hLV);
                }
                return 0;
            }
            case 4403: { /* 删除 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一行", "提示", MB_OK); return 0; }
                if (MessageBoxA(hWnd, "确定要删除该药品吗？", "确认", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
                char targetId[32];
                ListView_GetItemText(hLV, sel, 0, targetId, sizeof(targetId));
                DrugNode *head = load_drugs_list();
                DrugNode *prev = NULL, *cur = head;
                while (cur) {
                    if (strcmp(cur->data.drug_id, targetId) == 0) {
                        if (prev) prev->next = cur->next;
                        else head = cur->next;
                        free(cur);
                        break;
                    }
                    prev = cur;
                    cur = cur->next;
                }
                save_drugs_list(head);
                free_drug_list(head);
                PopulateDrugList(hLV);
                return 0;
            }
            case 4404: { /* 补货 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一行", "提示", MB_OK); return 0; }
                char targetId[32];
                ListView_GetItemText(hLV, sel, 0, targetId, sizeof(targetId));
                FieldDef f[1] = {{"增加数量", "", 16, FALSE}};
                if (ShowFieldDialog(hWnd, "补货", f, 1)) {
                    int add = atoi(f[0].value);
                    if (add <= 0) { MessageBoxA(hWnd, "数量必须大于 0", "错误", MB_OK); return 0; }
                    DrugNode *head = load_drugs_list();
                    for (DrugNode *cur = head; cur; cur = cur->next) {
                        if (strcmp(cur->data.drug_id, targetId) == 0) {
                            cur->data.stock_num += add;
                            break;
                        }
                    }
                    save_drugs_list(head);
                    free_drug_list(head);
                    PopulateDrugList(hLV);
                }
                return 0;
            }
            }
            return 0;

        /* ── 病房管理 ─────────────────────────────────────────── */
        case NAV_ADMIN_WARD:
            hLV = GetDlgItem(hWnd, 4005);
            switch (cmd) {
            case 4501: { /* 新增 */
                FieldDef f[4] = {
                    {"类型", "", 50, FALSE},
                    {"总床位", "", 16, FALSE},
                    {"剩余床位", "", 16, FALSE},
                    {"预警线", "", 16, FALSE},
                };
                if (ShowFieldDialog(hWnd, "新增病房", f, 4)) {
                    Ward w;
                    generate_id(w.ward_id, sizeof(w.ward_id), "WRD");
                    strcpy(w.type, f[0].value);
                    w.total_beds = atoi(f[1].value);
                    w.remain_beds = atoi(f[2].value);
                    w.warning_line = atoi(f[3].value);
                    WardNode *head = load_wards_list();
                    WardNode *node = create_ward_node(&w);
                    node->next = head;
                    save_wards_list(node);
                    free_ward_list(node);
                    PopulateWardList(hLV);
                }
                return 0;
            }
            case 4502: { /* 编辑 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一行", "提示", MB_OK); return 0; }
                FieldDef f[4] = {
                    {"类型", "", 50, FALSE},
                    {"总床位", "", 16, FALSE},
                    {"剩余床位", "", 16, FALSE},
                    {"预警线", "", 16, FALSE},
                };
                char idBuf[MAX_ID] = {0};
                ListView_GetItemText(hLV, sel, 0, idBuf, sizeof(idBuf));
                ListView_GetItemText(hLV, sel, 1, f[0].value, f[0].max_len);
                ListView_GetItemText(hLV, sel, 2, f[1].value, f[1].max_len);
                ListView_GetItemText(hLV, sel, 3, f[2].value, f[2].max_len);
                ListView_GetItemText(hLV, sel, 4, f[3].value, f[3].max_len);
                if (ShowFieldDialog(hWnd, "编辑病房", f, 4)) {
                    WardNode *head = load_wards_list();
                    for (WardNode *cur = head; cur; cur = cur->next) {
                        if (strcmp(cur->data.ward_id, idBuf) == 0) {
                            strcpy(cur->data.type, f[0].value);
                            cur->data.total_beds = atoi(f[1].value);
                            cur->data.remain_beds = atoi(f[2].value);
                            cur->data.warning_line = atoi(f[3].value);
                            break;
                        }
                    }
                    save_wards_list(head);
                    free_ward_list(head);
                    PopulateWardList(hLV);
                }
                return 0;
            }
            case 4503: { /* 删除 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一行", "提示", MB_OK); return 0; }
                if (MessageBoxA(hWnd, "确定要删除该病房吗？", "确认", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
                char targetId[32];
                ListView_GetItemText(hLV, sel, 0, targetId, sizeof(targetId));
                WardNode *head = load_wards_list();
                WardNode *prev = NULL, *cur = head;
                while (cur) {
                    if (strcmp(cur->data.ward_id, targetId) == 0) {
                        if (prev) prev->next = cur->next;
                        else head = cur->next;
                        free(cur);
                        break;
                    }
                    prev = cur;
                    cur = cur->next;
                }
                save_wards_list(head);
                free_ward_list(head);
                PopulateWardList(hLV);
                return 0;
            }
            }
            return 0;

        /* ── 排班管理 ─────────────────────────────────────────── */
        case NAV_ADMIN_SCHEDULE:
            hLV = GetDlgItem(hWnd, 4006);
            switch (cmd) {
            case 4601: { /* 生成排班 */
                if (ShowGenSchedDialog(hWnd)) {
                    PopulateSchedList(hLV);
                }
                return 0;
            }
            case 4602: { /* 停诊 */
                sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                if (sel < 0) { MessageBoxA(hWnd, "请先选择一行", "提示", MB_OK); return 0; }
                char targetId[32];
                ListView_GetItemText(hLV, sel, 0, targetId, sizeof(targetId));
                ScheduleNode *head = load_schedules_list();
                char schedDocId[20] = {0}, schedDate[12] = {0}, schedSlot[16] = {0};
                for (ScheduleNode *cur = head; cur; cur = cur->next) {
                    if (strcmp(cur->data.schedule_id, targetId) == 0) {
                        strcpy(cur->data.status, "停诊");
                        strcpy(schedDocId, cur->data.doctor_id);
                        strcpy(schedDate, cur->data.work_date);
                        strcpy(schedSlot, cur->data.time_slot);
                        break;
                    }
                }
                save_schedules_list(head);
                free_schedule_list(head);

                /* 联动取消该排班下所有待就诊预约 / Auto-cancel appointments */
                if (strlen(schedDocId) > 0) {
                    AppointmentNode *apps = load_appointments_list();
                    int canceledCount = 0;
                    if (apps) {
                        for (AppointmentNode *a = apps; a; a = a->next) {
                            if (strcmp(a->data.doctor_id, schedDocId) == 0 &&
                                strcmp(a->data.appointment_date, schedDate) == 0 &&
                                strcmp(a->data.appointment_time, schedSlot) == 0 &&
                                strcmp(a->data.status, "待就诊") == 0) {
                                strcpy(a->data.status, "已取消");
                                a->data.paid = 0;  /* 退费 */
                                canceledCount++;
                            }
                        }
                        if (canceledCount > 0)
                            save_appointments_list(apps);
                        free_appointment_list(apps);
                    }
                    if (canceledCount > 0) {
                        char logMsg[128];
                        snprintf(logMsg, sizeof(logMsg),
                            "停诊联动取消 %d 个预约", canceledCount);
                        append_log(g_currentUser.username, "停诊联动", "schedule",
                                   targetId, logMsg);
                        char mbMsg[128];
                        snprintf(mbMsg, sizeof(mbMsg),
                            "已停诊, 同时取消了 %d 个关联预约并退费。", canceledCount);
                        MessageBoxA(hWnd, mbMsg, "提示", MB_OK | MB_ICONINFORMATION);
                    }
                }
                PopulateSchedList(hLV);
                return 0;
            }
            }
            return 0;

        /* ── 数据管理 ─────────────────────────────────────────── */
        case NAV_ADMIN_DATA:
            switch (cmd) {
            case 4801: { /* 备份数据 / Backup */
                if (backup_data() >= 0) {
                    append_log(g_currentUser.username, "备份", "数据", "all", "手动数据备份");
                    MessageBoxA(hWnd, "数据备份完成！\n备份文件保存在 data/backup/ 目录。", "提示", MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxA(hWnd, "备份失败！", "错误", MB_OK | MB_ICONERROR);
                }
                return 0;
            }
            case 4802: { /* 恢复数据 / Restore */
                if (MessageBoxA(hWnd, "恢复数据将覆盖当前所有数据，确定继续？", "确认",
                    MB_YESNO | MB_ICONWARNING) != IDYES) return 0;
                FieldDef f[1] = {{"备份目录名", "", 64, FALSE}};
                const char **names = NULL;
                int count = 0;
                list_backups(&names, &count);
                if (count == 0) {
                    MessageBoxA(hWnd, "没有找到备份，请先备份数据。", "提示", MB_OK | MB_ICONINFORMATION);
                    free_backups_list(names, count);
                    return 0;
                }
                if (ShowFieldDialog(hWnd, "输入备份目录名进行恢复", f, 1)) {
                    if (restore_data(f[0].value) >= 0) {
                        append_log(g_currentUser.username, "恢复", "数据", "all", "数据恢复");
                        MessageBoxA(hWnd, "数据恢复完成！请刷新各页面查看最新数据。", "提示", MB_OK | MB_ICONINFORMATION);
                    } else {
                        MessageBoxA(hWnd, "恢复失败！请检查备份目录名是否正确。", "错误", MB_OK | MB_ICONERROR);
                    }
                }
                free_backups_list(names, count);
                return 0;
            }
            }
            return 0;
        }
        return 0;
    }

    case WM_NOTIFY:
        if (viewId == NAV_ADMIN_ANALYSIS && ((NMHDR *)lParam)->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(((NMHDR *)lParam)->hwndFrom);
            for (int i = 0; i < 3; i++) {
                if (g_analysisTabPages[i])
                    ShowWindow(g_analysisTabPages[i], i == sel ? SW_SHOW : SW_HIDE);
            }
            return 0;
        }
        break;

    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
    return 0;
}

/* ─── 各页面创建函数 / Page Creation Functions ──────────────────────── */

static HWND CreateDeptPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = AdminPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "AdminDeptPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "AdminDeptPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_ADMIN_DEPT);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 20;
    int h = (rc->bottom - rc->top) - 60;

    HWND hLV = CreateListView(hPage, 4001, 10, 10, w, h);
    AddCol(hLV, 0, "科室ID", 80);
    AddCol(hLV, 1, "名称", 120);
    AddCol(hLV, 2, "负责人", 100);
    AddCol(hLV, 3, "电话", 100);
    PopulateDeptList(hLV);

    int by = rc->bottom - rc->top - 45;
    CreateWindowA("BUTTON", "新增", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  10, by, 70, 30, hPage, (HMENU)4101, g_hInst, NULL);
    CreateWindowA("BUTTON", "编辑", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  90, by, 70, 30, hPage, (HMENU)4102, g_hInst, NULL);
    CreateWindowA("BUTTON", "删除", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  170, by, 70, 30, hPage, (HMENU)4103, g_hInst, NULL);

    ApplyDefaultFont(hPage);
    return hPage;
}

static HWND CreateDoctorMgmtPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = AdminPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "AdminDocPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "AdminDocPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_ADMIN_DOCTOR);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 20;
    int h = (rc->bottom - rc->top) - 60;

    HWND hLV = CreateListView(hPage, 4002, 10, 10, w, h);
    AddCol(hLV, 0, "医生ID", 80);
    AddCol(hLV, 1, "姓名", 80);
    AddCol(hLV, 2, "科室", 80);
    AddCol(hLV, 3, "职称", 80);
    AddCol(hLV, 4, "繁忙度", 60);
    PopulateDocList(hLV);

    int by = rc->bottom - rc->top - 45;
    CreateWindowA("BUTTON", "新增", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  10, by, 70, 30, hPage, (HMENU)4201, g_hInst, NULL);
    CreateWindowA("BUTTON", "编辑", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  90, by, 70, 30, hPage, (HMENU)4202, g_hInst, NULL);
    CreateWindowA("BUTTON", "删除", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  170, by, 70, 30, hPage, (HMENU)4203, g_hInst, NULL);
    CreateWindowA("BUTTON", "重置密码", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  250, by, 80, 30, hPage, (HMENU)4204, g_hInst, NULL);

    ApplyDefaultFont(hPage);
    return hPage;
}

static HWND CreatePatientMgmtPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = AdminPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "AdminPatPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "AdminPatPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_ADMIN_PATIENT);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 20;
    int h = (rc->bottom - rc->top) - 60;

    HWND hLV = CreateListView(hPage, 4003, 10, 10, w, h);
    AddCol(hLV, 0, "患者ID", 80);
    AddCol(hLV, 1, "姓名", 80);
    AddCol(hLV, 2, "性别", 40);
    AddCol(hLV, 3, "年龄", 40);
    AddCol(hLV, 4, "电话", 100);
    AddCol(hLV, 5, "类型", 80);
    AddCol(hLV, 6, "阶段", 100);
    AddCol(hLV, 7, "急诊", 60);

    PopulatePatList(hLV);
    int by = rc->bottom - rc->top - 45;
    CreateWindowA("BUTTON", "新增", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  10, by, 70, 30, hPage, (HMENU)4301, g_hInst, NULL);
    CreateWindowA("BUTTON", "编辑", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  90, by, 70, 30, hPage, (HMENU)4302, g_hInst, NULL);
    CreateWindowA("BUTTON", "删除", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  170, by, 70, 30, hPage, (HMENU)4303, g_hInst, NULL);
    CreateWindowA("BUTTON", "刷新", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  250, by, 70, 30, hPage, (HMENU)4304, g_hInst, NULL);
    CreateWindowA("BUTTON", "重置密码", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  330, by, 80, 30, hPage, (HMENU)4305, g_hInst, NULL);

    ApplyDefaultFont(hPage);
    return hPage;
}

static HWND CreateDrugPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = AdminPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "AdminDrugPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "AdminDrugPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_ADMIN_DRUG);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 20;
    int h = (rc->bottom - rc->top) - 60;

    HWND hLV = CreateListView(hPage, 4004, 10, 10, w, h);
    AddCol(hLV, 0, "药品ID", 80);
    AddCol(hLV, 1, "名称", 120);
    AddCol(hLV, 2, "价格", 60);
    AddCol(hLV, 3, "库存", 50);
    AddCol(hLV, 4, "预警线", 50);
    AddCol(hLV, 5, "报销比例", 70);
    AddCol(hLV, 6, "分类", 60);
    PopulateDrugList(hLV);

    int by = rc->bottom - rc->top - 45;
    CreateWindowA("BUTTON", "新增", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  10, by, 70, 30, hPage, (HMENU)4401, g_hInst, NULL);
    CreateWindowA("BUTTON", "编辑", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  90, by, 70, 30, hPage, (HMENU)4402, g_hInst, NULL);
    CreateWindowA("BUTTON", "删除", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  170, by, 70, 30, hPage, (HMENU)4403, g_hInst, NULL);
    CreateWindowA("BUTTON", "补货", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  250, by, 70, 30, hPage, (HMENU)4404, g_hInst, NULL);

    ApplyDefaultFont(hPage);
    return hPage;
}

static HWND CreateWardPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = AdminPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "AdminWardPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "AdminWardPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_ADMIN_WARD);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 20;
    int h = (rc->bottom - rc->top) - 60;

    HWND hLV = CreateListView(hPage, 4005, 10, 10, w, h);
    AddCol(hLV, 0, "病房ID", 80);
    AddCol(hLV, 1, "类型", 100);
    AddCol(hLV, 2, "总床位", 60);
    AddCol(hLV, 3, "剩余床位", 60);
    AddCol(hLV, 4, "预警线", 60);
    PopulateWardList(hLV);

    int by = rc->bottom - rc->top - 45;
    CreateWindowA("BUTTON", "新增", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  10, by, 70, 30, hPage, (HMENU)4501, g_hInst, NULL);
    CreateWindowA("BUTTON", "编辑", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  90, by, 70, 30, hPage, (HMENU)4502, g_hInst, NULL);
    CreateWindowA("BUTTON", "删除", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  170, by, 70, 30, hPage, (HMENU)4503, g_hInst, NULL);

    ApplyDefaultFont(hPage);
    return hPage;
}

static HWND CreateSchedulePage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = AdminPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "AdminSchedPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "AdminSchedPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_ADMIN_SCHEDULE);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 20;
    int h = (rc->bottom - rc->top) - 60;

    HWND hLV = CreateListView(hPage, 4006, 10, 10, w, h);
    AddCol(hLV, 0, "排班ID", 100);
    AddCol(hLV, 1, "医生ID", 80);
    AddCol(hLV, 2, "日期", 100);
    AddCol(hLV, 3, "时段", 60);
    AddCol(hLV, 4, "状态", 60);
    PopulateSchedList(hLV);

    int by = rc->bottom - rc->top - 45;
    CreateWindowA("BUTTON", "生成排班", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  10, by, 90, 30, hPage, (HMENU)4601, g_hInst, NULL);
    CreateWindowA("BUTTON", "停诊", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                  110, by, 70, 30, hPage, (HMENU)4602, g_hInst, NULL);

    ApplyDefaultFont(hPage);
    return hPage;
}

/* ─── 操作日志页面 / Operation Log Page ──────────────────────────────── */

static HWND CreateLogPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = AdminPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "AdminLogPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "AdminLogPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_ADMIN_LOG);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 20;
    int h = (rc->bottom - rc->top) - 20;

    HWND hLV = CreateListView(hPage, 4007, 10, 10, w, h);
    AddCol(hLV, 0, "日志ID", 80);
    AddCol(hLV, 1, "操作人", 80);
    AddCol(hLV, 2, "操作", 60);
    AddCol(hLV, 3, "对象", 60);
    AddCol(hLV, 4, "对象ID", 80);
    AddCol(hLV, 5, "详情", 200);
    AddCol(hLV, 6, "时间", 120);

    LogEntryNode *logs = load_logs_list();
    int row = 0;
    if (logs) {
        LogEntryNode *cur = logs;
        while (cur) {
            const char *items[7] = {
                cur->data.log_id, cur->data.operator_name,
                cur->data.action, cur->data.target,
                cur->data.target_id, cur->data.detail, cur->data.create_time
            };
            AddRow(hLV, row++, 7, items);
            cur = cur->next;
        }
        free_log_entry_list(logs);
    }

    ApplyDefaultFont(hPage);
    return hPage;
}

/* ─── 数据管理页面 / Data Management Page ───────────────────────────── */

static HWND CreateDataPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = AdminPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "AdminDataPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "AdminDataPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_ADMIN_DATA);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 40;

    CreateWindowA("STATIC", "数据备份与恢复",
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        20, 15, 200, 22, hPage, NULL, g_hInst, NULL);

    CreateWindowA("BUTTON", "备份数据",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        20, 45, 120, 32, hPage, (HMENU)4801, g_hInst, NULL);

    CreateWindowA("BUTTON", "恢复数据",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        150, 45, 120, 32, hPage, (HMENU)4802, g_hInst, NULL);

    CreateWindowA("STATIC", "备份操作将 data/ 下所有数据文件复制到 data/backup/ 目录，以时间戳命名。",
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        20, 85, w, 20, hPage, NULL, g_hInst, NULL);

    /* 备份列表 */
    int lvY = 115, lvH = (rc->bottom - rc->top) - lvY - 20;
    HWND hLV = CreateListView(hPage, 4803, 20, lvY, w, lvH > 50 ? lvH : 100);
    AddCol(hLV, 0, "备份目录", 150);
    AddCol(hLV, 1, "说明", 300);

    const char **names = NULL;
    int count = 0;
    list_backups(&names, &count);
    if (names) {
        for (int i = count - 1; i >= 0; i--) {
            const char *items[2] = { names[i], "可用" };
            AddRow(hLV, count - 1 - i, 2, items);
        }
        free_backups_list(names, count);
    }

    ApplyDefaultFont(hPage);
    return hPage;
}

/* ─── 报表统计页面 / Analysis & Reports Page ────────────────────────── */

/* PopulateAnalysisOverview: 汇总关键运营指标 (预约/收入/床位/药品/患者)
   Aggregate key operational metrics: appointments, revenue, beds, drugs, patients */

static void PopulateAnalysisOverview(HWND hLV) {
    ClearLV(hLV);
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char today[16];
    strftime(today, sizeof(today), "%Y-%m-%d", tm);
    char thisMonth[8];
    strftime(thisMonth, sizeof(thisMonth), "%Y-%m", tm);

    /* 预约/挂号统计 */
    AppointmentNode *appts = load_appointments_list();
    int totalAppts = 0, todayAppts = 0, completedAppts = 0;
    for (AppointmentNode *cur = appts; cur; cur = cur->next) {
        totalAppts++;
        if (strncmp(cur->data.appointment_date, today, 10) == 0)
            todayAppts++;
        if (strcmp(cur->data.status, "已完成") == 0 || strcmp(cur->data.status, "已就诊") == 0)
            completedAppts++;
    }

    /* 处方统计与财务统计 / Prescription & Financial Statistics */
    PrescriptionNode *prescs = load_prescriptions_list();
    double totalRevenue = 0, monthRevenue = 0;
    int totalPrescs = 0;
    for (PrescriptionNode *cur = prescs; cur; cur = cur->next) {
        if (cur->data.paid) {
            totalRevenue += cur->data.total_price;
            if (strncmp(cur->data.prescription_date, thisMonth, 7) == 0)
                monthRevenue += cur->data.total_price;
        }
        totalPrescs++;
    }

    /* 加上已缴费挂号费 / Add paid registration fees */
    for (AppointmentNode *cur = appts; cur; cur = cur->next) {
        if (cur->data.paid) totalRevenue += cur->data.fee;
    }
    OnsiteRegistrationQueue onQ = load_onsite_registration_queue();
    for (OnsiteRegistrationNode *cur = onQ.front; cur; cur = cur->next) {
        if (cur->data.paid) totalRevenue += cur->data.fee;
    }
    free_onsite_registration_queue(&onQ);

    /* 床位统计 */
    WardNode *wards = load_wards_list();
    int totalBeds = 0, usedBeds = 0;
    for (WardNode *cur = wards; cur; cur = cur->next) {
        totalBeds += cur->data.total_beds;
        usedBeds += cur->data.total_beds - cur->data.remain_beds;
    }

    /* 药品统计 */
    DrugNode *drugs = load_drugs_list();
    int lowStockDrugs = 0, totalDrugTypes = 0;
    for (DrugNode *cur = drugs; cur; cur = cur->next) {
        totalDrugTypes++;
        if (cur->data.stock_num < cur->data.warning_line)
            lowStockDrugs++;
    }

    /* 患者统计 */
    PatientNode *pats = load_patients_list();
    int totalPats = 0, emgPats = 0;
    for (PatientNode *cur = pats; cur; cur = cur->next) {
        totalPats++;
        if (cur->data.is_emergency) emgPats++;
    }

    char buf[64];
    int row = 0;

    snprintf(buf, sizeof(buf), "%d 次", totalAppts);
    AddRow(hLV, row++, 2, (const char *[]){"累计预约数", buf});
    snprintf(buf, sizeof(buf), "%d 次", todayAppts);
    AddRow(hLV, row++, 2, (const char *[]){"今日预约数", buf});
    snprintf(buf, sizeof(buf), "%d 次", completedAppts);
    AddRow(hLV, row++, 2, (const char *[]){"已完成就诊", buf});
    snprintf(buf, sizeof(buf), "%.2f 元", totalRevenue);
    AddRow(hLV, row++, 2, (const char *[]){"累计收入", buf});
    snprintf(buf, sizeof(buf), "%.2f 元", monthRevenue);
    AddRow(hLV, row++, 2, (const char *[]){"本月收入", buf});
    snprintf(buf, sizeof(buf), "%d 条", totalPrescs);
    AddRow(hLV, row++, 2, (const char *[]){"处方总数", buf});
    double bedRate = totalBeds > 0 ? 100.0 * usedBeds / totalBeds : 0;
    snprintf(buf, sizeof(buf), "%.1f%% (%d/%d)", bedRate, usedBeds, totalBeds);
    AddRow(hLV, row++, 2, (const char *[]){"床位使用率", buf});
    snprintf(buf, sizeof(buf), "%d 种", totalDrugTypes);
    AddRow(hLV, row++, 2, (const char *[]){"药品品种数", buf});
    snprintf(buf, sizeof(buf), "%d 种", lowStockDrugs);
    AddRow(hLV, row++, 2, (const char *[]){"库存预警药品", buf});
    snprintf(buf, sizeof(buf), "%d 人", totalPats);
    AddRow(hLV, row++, 2, (const char *[]){"患者总数", buf});
    snprintf(buf, sizeof(buf), "%d 人", emgPats);
    AddRow(hLV, row++, 2, (const char *[]){"紧急患者", buf});

    free_appointment_list(appts);
    free_prescription_list(prescs);
    free_ward_list(wards);
    free_drug_list(drugs);
    free_patient_list(pats);
}

static void PopulateAnalysisDoctorLoad(HWND hLV) {
    /* 统计每位医生的接诊/处方数, 按病历数分档
       Per-doctor workload: appointment count, prescription count, load classification */
    ClearLV(hLV);

    DoctorNode *docs = load_doctors_list();
    AppointmentNode *appts = load_appointments_list();
    PrescriptionNode *prescs = load_prescriptions_list();
    MedicalRecordNode *records = load_medical_records_list();

    int row = 0;
    for (DoctorNode *cur = docs; cur; cur = cur->next) {
        int apptCount = 0, prescCount = 0;
        for (AppointmentNode *a = appts; a; a = a->next)
            if (strcmp(a->data.doctor_id, cur->data.doctor_id) == 0) apptCount++;
        for (PrescriptionNode *p = prescs; p; p = p->next)
            if (strcmp(p->data.doctor_id, cur->data.doctor_id) == 0) prescCount++;

        char apptStr[16], prescStr[16], status[20];
        snprintf(apptStr, sizeof(apptStr), "%d", apptCount);
        snprintf(prescStr, sizeof(prescStr), "%d", prescCount);

        /* Count medical records for workload */
        int recCount = 0;
        for (MedicalRecordNode *r = records; r; r = r->next)
            if (strcmp(r->data.doctor_id, cur->data.doctor_id) == 0) recCount++;

        if (recCount >= 20) strcpy(status, "高负荷");
        else if (recCount >= 10) strcpy(status, "正常");
        else strcpy(status, "较轻松");

        const char *items[6] = {
            cur->data.doctor_id, cur->data.name,
            cur->data.department_id, apptStr, prescStr, status
        };
        AddRow(hLV, row++, 6, items);
    }

    free_doctor_list(docs);
    free_appointment_list(appts);
    free_prescription_list(prescs);
    free_medical_record_list(records);
}

static void PopulateAnalysisFinancial(HWND hLV) {
    /* 按月份汇总处方收入 + 按药品/患者类型计算报销额
       Monthly revenue aggregation with drug-specific & patient-type reimbursement */
    ClearLV(hLV);

    PrescriptionNode *prescs = load_prescriptions_list();
    DrugNode *drugs = load_drugs_list();
    PatientNode *patients = load_patients_list();

    /* 按月份聚合 / Aggregate by month */
    struct {
        char month[8];
        double total;
        double reimb;
        int count;
    } months[256];
    int monthCount = 0;

    for (PrescriptionNode *cur = prescs; cur; cur = cur->next) {
        char m[8];
        strncpy(m, cur->data.prescription_date, 7);
        m[7] = 0;
        int found = -1;
        for (int i = 0; i < monthCount; i++) {
            if (strcmp(months[i].month, m) == 0) { found = i; break; }
        }
        if (found < 0) {
            found = monthCount++;
            strcpy(months[found].month, m);
            months[found].total = 0;
            months[found].reimb = 0;
            months[found].count = 0;
        }
        months[found].total += cur->data.total_price;
        months[found].count++;

        /* 按药品报销比例 + 患者类型计算 / Drug-specific reimbursement */
        DrugNode *dn = drugs;
        while (dn) {
            if (strcmp(dn->data.drug_id, cur->data.drug_id) == 0) break;
            dn = dn->next;
        }
        if (dn) {
            PatientNode *pn = patients;
            while (pn) {
                if (strcmp(pn->data.patient_id, cur->data.patient_id) == 0) break;
                pn = pn->next;
            }
            const char *ptype = pn ? pn->data.patient_type : "普通";
            months[found].reimb += calculate_drug_reimbursement(&dn->data, cur->data.quantity, ptype);
        }
    }

    /* 按月份降序排列 / Sort months descending (bubble sort) */
    for (int i = 0; i < monthCount - 1; i++) {
        for (int j = i + 1; j < monthCount; j++) {
            if (strcmp(months[i].month, months[j].month) < 0) {
                char tmp_m[8]; double tmp_total, tmp_reimb; int tmp_count;
                strcpy(tmp_m, months[i].month);
                tmp_total = months[i].total;
                tmp_reimb = months[i].reimb;
                tmp_count = months[i].count;
                strcpy(months[i].month, months[j].month);
                months[i].total = months[j].total;
                months[i].reimb = months[j].reimb;
                months[i].count = months[j].count;
                strcpy(months[j].month, tmp_m);
                months[j].total = tmp_total;
                months[j].reimb = tmp_reimb;
                months[j].count = tmp_count;
            }
        }
    }

    int row = 0;
    for (int i = 0; i < monthCount; i++) {
        double selfPay = months[i].total - months[i].reimb;
        char totalStr[32], reimbStr[32], selfStr[32];
        snprintf(totalStr, sizeof(totalStr), "%.2f", months[i].total);
        snprintf(reimbStr, sizeof(reimbStr), "%.2f", months[i].reimb);
        snprintf(selfStr, sizeof(selfStr), "%.2f", selfPay);

        char monthLabel[32];
        snprintf(monthLabel, sizeof(monthLabel), "%.7s (%d笔)", months[i].month, months[i].count);

        const char *items[4] = { monthLabel, totalStr, reimbStr, selfStr };
        AddRow(hLV, row++, 4, items);
    }

    /* Show total row */
    if (monthCount > 0) {
        double grandTotal = 0, grandReimb = 0;
        for (int i = 0; i < monthCount; i++) {
            grandTotal += months[i].total;
            grandReimb += months[i].reimb;
        }
        char totalStr[32], reimbStr[32], selfStr[32];
        snprintf(totalStr, sizeof(totalStr), "%.2f", grandTotal);
        snprintf(reimbStr, sizeof(reimbStr), "%.2f", grandReimb);
        snprintf(selfStr, sizeof(selfStr), "%.2f", grandTotal - grandReimb);
        const char *items[4] = {"--- 合计 ---", totalStr, reimbStr, selfStr};
        AddRow(hLV, row++, 4, items);
    }

    free_prescription_list(prescs);
    free_drug_list(drugs);
    free_patient_list(patients);
}

static HWND CreateAnalysisPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = AdminPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "AdminAnalPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "AdminAnalPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_ADMIN_ANALYSIS);
    if (!hPage) return NULL;

    int pw = (rc->right - rc->left);
    int ph = (rc->bottom - rc->top);

    HWND hTab = CreateWindowA(WC_TABCONTROLA, "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        10, 10, pw - 20, ph - 20,
        hPage, NULL, g_hInst, NULL);

    TC_ITEMA tci = {0};
    tci.mask = TCIF_TEXT;
    tci.pszText = "概览";
    TabCtrl_InsertItem(hTab, 0, &tci);
    tci.pszText = "医生负载";
    TabCtrl_InsertItem(hTab, 1, &tci);
    tci.pszText = "财务统计";
    TabCtrl_InsertItem(hTab, 2, &tci);

    /* Tab content area */
    RECT tabRc;
    GetClientRect(hTab, &tabRc);
    tabRc.top += 26;
    tabRc.left += 4;
    tabRc.right -= 4;
    tabRc.bottom -= 4;
    int cx = tabRc.right - tabRc.left;
    int cy = tabRc.bottom - tabRc.top;

    /* Create tab pages */
    g_analysisTabPages[0] = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_CLIPCHILDREN, tabRc.left, tabRc.top, cx, cy,
        hPage, NULL, g_hInst, NULL);
    HWND hLV0 = CreateListView(g_analysisTabPages[0], 4701, 0, 0, cx, cy);
    AddCol(hLV0, 0, "指标", 150);
    AddCol(hLV0, 1, "数值", pw - 200);
    PopulateAnalysisOverview(hLV0);

    g_analysisTabPages[1] = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_CLIPCHILDREN, tabRc.left, tabRc.top, cx, cy,
        hPage, NULL, g_hInst, NULL);
    HWND hLV1 = CreateListView(g_analysisTabPages[1], 4711, 0, 0, cx, cy);
    AddCol(hLV1, 0, "医生ID", 80);
    AddCol(hLV1, 1, "姓名", 80);
    AddCol(hLV1, 2, "科室", 80);
    AddCol(hLV1, 3, "接诊数", 70);
    AddCol(hLV1, 4, "处方数", 70);
    AddCol(hLV1, 5, "工作状态", 80);
    PopulateAnalysisDoctorLoad(hLV1);

    g_analysisTabPages[2] = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_CLIPCHILDREN, tabRc.left, tabRc.top, cx, cy,
        hPage, NULL, g_hInst, NULL);
    HWND hLV2 = CreateListView(g_analysisTabPages[2], 4721, 0, 0, cx, cy);
    AddCol(hLV2, 0, "月份", 130);
    AddCol(hLV2, 1, "总收入", 120);
    AddCol(hLV2, 2, "报销金额", 120);
    AddCol(hLV2, 3, "自费金额", 120);
    PopulateAnalysisFinancial(hLV2);

    /* Show first tab */
    ShowWindow(g_analysisTabPages[0], SW_SHOW);
    ShowWindow(g_analysisTabPages[1], SW_HIDE);
    ShowWindow(g_analysisTabPages[2], SW_HIDE);

    ApplyDefaultFont(hPage);
    return hPage;
}

/* ─── 重置密码页面 / Reset Password Page ───────────────────────────── */

static LRESULT CALLBACK ResetPwdDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
        SetWindowLongPtrA(hDlg, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        RECT rc; GetClientRect(hDlg, &rc);
        int w = rc.right - rc.left;
        int y = 20;

        CreateWindowA("STATIC", "目标用户:",
            WS_VISIBLE | WS_CHILD | SS_RIGHT, 10, y + 2, 80, 20, hDlg, NULL, g_hInst, NULL);
        HWND hUser = CreateWindowA("EDIT", (const char *)cs->lpCreateParams,
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_READONLY,
            100, y, w - 130, 22, hDlg, (HMENU)4901, g_hInst, NULL);
        SendMessageA(hUser, EM_SETLIMITTEXT, 49, 0);
        y += 35;

        CreateWindowA("STATIC", "新密码:",
            WS_VISIBLE | WS_CHILD | SS_RIGHT, 10, y + 2, 80, 20, hDlg, NULL, g_hInst, NULL);
        CreateWindowA("EDIT", "123456",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
            100, y, w - 130, 22, hDlg, (HMENU)4902, g_hInst, NULL);
        y += 35;

        CreateWindowA("STATIC", "确认密码:",
            WS_VISIBLE | WS_CHILD | SS_RIGHT, 10, y + 2, 80, 20, hDlg, NULL, g_hInst, NULL);
        CreateWindowA("EDIT", "123456",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
            100, y, w - 130, 22, hDlg, (HMENU)4903, g_hInst, NULL);
        y += 45;

        CreateWindowA("BUTTON", "确定",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            w / 2 - 110, y, 100, 28, hDlg, (HMENU)4904, g_hInst, NULL);
        CreateWindowA("BUTTON", "取消",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            w / 2 + 10, y, 100, 28, hDlg, (HMENU)4905, g_hInst, NULL);
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == 4904) {
            char newPwd[256], confirmPwd[256];
            GetDlgItemTextA(hDlg, 4902, newPwd, sizeof(newPwd));
            GetDlgItemTextA(hDlg, 4903, confirmPwd, sizeof(confirmPwd));
            if (strlen(newPwd) == 0) {
                MessageBoxA(hDlg, "密码不能为空", "提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (strcmp(newPwd, confirmPwd) != 0) {
                MessageBoxA(hDlg, "两次输入的密码不一致", "提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            char username[256];
            GetDlgItemTextA(hDlg, 4901, username, sizeof(username));

            UserNode *head = load_users_list();
            UserNode *target = NULL;
            for (UserNode *cur = head; cur; cur = cur->next) {
                if (strcmp(cur->data.username, username) == 0) {
                    target = cur;
                    break;
                }
            }
            if (!target) {
                free_user_list(head);
                MessageBoxA(hDlg, "未找到该用户", "错误", MB_OK | MB_ICONERROR);
                return 0;
            }

            uint8_t hash[SHA256_DIGEST_SIZE];
            char hex[SHA256_HEX_SIZE];
            sha256_hash((const uint8_t *)newPwd, strlen(newPwd), hash);
            sha256_hex(hash, hex);
            strcpy(target->data.password, hex);

            int ret = save_users_list(head);
            free_user_list(head);
            if (ret == SUCCESS) {
                append_log(g_currentUser.username, "重置密码", "user", username, "");
                MessageBoxA(hDlg, "密码重置成功", "成功", MB_OK);
                g_pwdResult = 1;
                DestroyWindow(hDlg);
            } else {
                MessageBoxA(hDlg, "保存失败", "错误", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (LOWORD(wParam) == 4905) {
            g_pwdResult = 0;
            DestroyWindow(hDlg);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        g_pwdResult = 0;
        DestroyWindow(hDlg);
        return 0;
    default:
        return DefWindowProcA(hDlg, msg, wParam, lParam);
    }
}

static int ShowResetPwdDialog(HWND hParent, const char *username) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = ResetPwdDlgProc;
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "ResetPwdDialog";
    RegisterClassA(&wc);

    HWND hDlg = CreateWindowExA(0, "ResetPwdDialog", "重置密码",
        WS_VISIBLE | WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 350, 220,
        hParent, NULL, g_hInst, (LPVOID)username);
    if (!hDlg) return 0;

    RECT pr, rc;
    GetWindowRect(hParent, &pr);
    GetWindowRect(hDlg, &rc);
    SetWindowPos(hDlg, NULL,
        pr.left + (pr.right - pr.left - (rc.right - rc.left)) / 2,
        pr.top + (pr.bottom - pr.top - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(hParent, FALSE);
    g_pwdResult = -1;
    MSG msg;
    while (g_pwdResult == -1 && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
    return g_pwdResult == 1;
}

static LRESULT CALLBACK ResetPwdPageWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCT *)lParam)->lpCreateParams);
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lParam) - 10;
        int h = HIWORD(lParam) - 10;
        HWND hLV = GetDlgItem(hWnd, 4805);
        if (hLV) SetWindowPos(hLV, NULL, 5, 5, w - 10, h - 50, SWP_NOZORDER);
        HWND hBtn = GetDlgItem(hWnd, 4806);
        if (hBtn) SetWindowPos(hBtn, NULL, 5, h - 40, 120, 30, SWP_NOZORDER);
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == 4806) {
            HWND hLV = GetDlgItem(hWnd, 4805);
            if (!hLV) return 0;
            char username[256] = "";
            GetSelectedItemText(hLV, 0, username, sizeof(username));
            if (username[0] == 0) {
                MessageBoxA(GetParent(hWnd), "请先选择一个用户", "提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            ShowResetPwdDialog(GetParent(hWnd), username);
            PostMessage(GetParent(hWnd), WM_APP_REFRESH, NAV_ADMIN_RESETPWD, 0);
        }
        return 0;
    }
    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
}

static HWND CreateResetPwdPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = ResetPwdPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "AdminPwdPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "AdminPwdPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_ADMIN_RESETPWD);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 10;
    int h = (rc->bottom - rc->top) - 10;

    HWND hLV = CreateListView(hPage, 4805, 5, 5, w - 10, h - 50);
    AddCol(hLV, 0, "用户名", 150);
    AddCol(hLV, 1, "角色", 100);

    UserNode *users = load_users_list();
    if (users) {
        int row = 0;
        for (UserNode *cur = users; cur; cur = cur->next) {
            const char *items[2] = { cur->data.username, cur->data.role };
            AddRow(hLV, row++, 2, items);
        }
        free_user_list(users);
    }

    CreateWindowA("BUTTON", "重置密码",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        5, h - 40, 120, 30, hPage, (HMENU)4806, g_hInst, NULL);

    return hPage;
}

/* ─── 公开接口 / Public Interface ────────────────────────────────────── */

/* CreateAdminPage — 工厂函数, 按 viewId 路由到各管理员页面创建函数
   Factory function routing viewId to the appropriate admin page creator */

HWND CreateAdminPage(HWND hParent, int viewId, RECT *rc) {
    switch (viewId) {
    case NAV_ADMIN_DEPT:     return CreateDeptPage(hParent, rc);
    case NAV_ADMIN_DOCTOR:   return CreateDoctorMgmtPage(hParent, rc);
    case NAV_ADMIN_PATIENT:  return CreatePatientMgmtPage(hParent, rc);
    case NAV_ADMIN_DRUG:     return CreateDrugPage(hParent, rc);
    case NAV_ADMIN_WARD:     return CreateWardPage(hParent, rc);
    case NAV_ADMIN_SCHEDULE: return CreateSchedulePage(hParent, rc);
    case NAV_ADMIN_LOG:      return CreateLogPage(hParent, rc);
    case NAV_ADMIN_DATA:     return CreateDataPage(hParent, rc);
    case NAV_ADMIN_ANALYSIS: return CreateAnalysisPage(hParent, rc);
    case NAV_ADMIN_RESETPWD: return CreateResetPwdPage(hParent, rc);
    default: return NULL;
    }
}
