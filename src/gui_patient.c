/*
 * gui_patient.c — Win32 GUI 患者界面实现 / Win32 GUI patient page implementation
 *
 * 实现患者角色的所有 GUI 页面 (7 个页面 + 2 个模态对话框):
 *   - 预约挂号 (CreateRegisterPage) — 选择科室→医生→日期→时段, 校验排班后创建预约
 *   - 预约查询 (CreateAppointmentPage) — ListView 展示当前患者所有预约, 支持取消
 *   - 诊断结果 (CreateDiagnosisPage) — 查看病史记录列表, 选中查看诊断详情
 *   - 缴费 (CreatePaymentPage) — 支付处方/挂号/病房及其他医疗服务费用
 *   - 住院信息 (CreateWardPage) — 查看病房类型/总床位/剩余床位
 *   - 治疗进度 (CreateProgressPage) — 显示当前治疗阶段与紧急状态
 *   - 个人信息 (CreateProfilePage) — 查看与编辑个人资料 (姓名/性别/年龄/电话/地址)
 *
 * 每个页面使用独立的窗口类, 通过 CreatePatientPage() 工厂函数按 viewId 路由。
 * 模态对话框 (挂号 RegDlgProc, 编辑资料 PatFieldDlgProc) 使用自定义窗口类 +
 * 本地模态消息循环 (EnableWindow 禁用父窗口 + GetMessage 循环)。
 *
 * Implements all 7 patient-role GUI pages with ListView data display,
 * modal dialogs for registration and profile editing, appointment
 * management with schedule validation, and medical record browsing.
 */

#include "gui_patient.h"
#include "gui_main.h"
#include "data_storage.h"
#include "public.h"
#include "sha256.h"

/* ─── ListView 工具函数 / ListView Utility Functions ────────────────── */

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

/* 获取当前患者 ID (通过用户名查找患者档案)
   Get current patient ID by looking up username in patient records */
static const char* GetPatientId(void) {
    Patient *p = find_patient_by_username(g_currentUser.username);
    return p ? p->patient_id : "";
}

/* ─── 字段编辑对话框 (给个人信息用) / Profile Field Edit Dialog ───── */

#define PF_FIELDS_MAX 8

typedef struct {
    char label[32];
    char value[256];
    int  max_len;
    BOOL read_only;
} PatFieldDef;

static int g_pfResult = 0;

static LRESULT CALLBACK PatFieldDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
        SetWindowLongPtrA(hDlg, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        PatFieldDef *fields = (PatFieldDef *)cs->lpCreateParams;
        int count = 0;
        while (count < PF_FIELDS_MAX && fields[count].label[0]) count++;
        RECT rc;
        GetClientRect(hDlg, &rc);
        int w = rc.right - rc.left, y = 15;
        for (int i = 0; i < count; i++) {
            CreateWindowA("STATIC", fields[i].label,
                WS_VISIBLE|WS_CHILD|SS_RIGHT, 10, y+2, 80, 20,
                hDlg, NULL, g_hInst, NULL);
            DWORD style = WS_VISIBLE|WS_CHILD|WS_BORDER|ES_AUTOHSCROLL;
            if (fields[i].read_only) style |= ES_READONLY;
            HWND hEdit = CreateWindowA("EDIT", fields[i].value, style,
                100, y, w - 120, 22, hDlg, (HMENU)(2000+i), g_hInst, NULL);
            SendMessageA(hEdit, EM_SETLIMITTEXT, fields[i].max_len-1, 0);
            y += 30;
        }
        y += 10;
        CreateWindowA("BUTTON", "确定", WS_VISIBLE|WS_CHILD|BS_PUSHBUTTON,
            w/2-95, y, 80, 28, hDlg, (HMENU)1, g_hInst, NULL);
        CreateWindowA("BUTTON", "取消", WS_VISIBLE|WS_CHILD|BS_PUSHBUTTON,
            w/2+15, y, 80, 28, hDlg, (HMENU)2, g_hInst, NULL);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == 1) {
            PatFieldDef *fields = (PatFieldDef *)GetWindowLongPtrA(hDlg, GWLP_USERDATA);
            if (fields) {
                for (int i = 0; i < PF_FIELDS_MAX && fields[i].label[0]; i++)
                    GetDlgItemTextA(hDlg, 2000+i, fields[i].value, fields[i].max_len);
            }
            g_pfResult = 1; DestroyWindow(hDlg);
        } else if (id == 2) {
            g_pfResult = 0; DestroyWindow(hDlg);
        }
        return 0;
    }
    case WM_CLOSE:
        g_pfResult = 0; DestroyWindow(hDlg); return 0;
    default:
        return DefWindowProcA(hDlg, msg, wParam, lParam);
    }
}

static int ShowPatFieldDialog(HWND hParent, const char *title,
                              PatFieldDef *fields, int field_count) {
    if (field_count <= 0 || field_count > PF_FIELDS_MAX) return 0;
    fields[field_count].label[0] = 0;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = PatFieldDlgProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = "PatFieldDialog";
    RegisterClassA(&wc);
    int w = 400, h = 50 + field_count * 30 + 55;
    HWND hDlg = CreateWindowExA(0, "PatFieldDialog", title,
        WS_VISIBLE|WS_POPUPWINDOW|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        hParent, NULL, g_hInst, (LPVOID)fields);
    if (!hDlg) return 0;
    RECT pr, rc;
    GetWindowRect(hParent, &pr);
    GetWindowRect(hDlg, &rc);
    SetWindowPos(hDlg, NULL,
        pr.left+(pr.right-pr.left-(rc.right-rc.left))/2,
        pr.top+(pr.bottom-pr.top-(rc.bottom-rc.top))/2,
        0, 0, SWP_NOSIZE|SWP_NOZORDER);
    EnableWindow(hParent, FALSE);
    g_pfResult = -1;
    MSG msg;
    while (g_pfResult == -1 && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
    return g_pfResult == 1;
}

/* 检查 dateStr (YYYY-MM-DD) 是否在 [today, today+maxDays] 范围内
   Check if dateStr (YYYY-MM-DD) is within [today, today+maxDays] */
static int is_date_in_range(const char *dateStr, int maxDays) {
    time_t now = time(NULL);
    struct tm tmNow;
    memcpy(&tmNow, localtime(&now), sizeof(tmNow));

    struct tm tmTarget = {0};
    if (sscanf(dateStr, "%d-%d-%d", &tmTarget.tm_year, &tmTarget.tm_mon, &tmTarget.tm_mday) != 3)
        return 0;
    tmTarget.tm_year -= 1900;
    tmTarget.tm_mon  -= 1;
    tmTarget.tm_hour = 12; /* midday to avoid DST edge issues */
    time_t tTarget = mktime(&tmTarget);
    if (tTarget == (time_t)-1) return 0;

    /* 当天 00:00 */
    tmNow.tm_hour = 0; tmNow.tm_min = 0; tmNow.tm_sec = 0;
    time_t tNow = mktime(&tmNow);

    if (tTarget < tNow) return 0; /* 不能在过去 */
    if (tTarget > tNow + maxDays * 86400LL) return 0; /* 超出最大天数 */
    return 1;
}

/* 检查医生在指定日期时段是否有排班 (状态=正常)
   Check if doctor has an active schedule for the given date+timeslot */
static int doctor_has_schedule(const char *docId, const char *date, const char *timeSlot) {
    ScheduleNode *sched = load_schedules_list();
    if (!sched) return 1; /* 无排班数据则放行 */
    int found = 0;
    for (ScheduleNode *cur = sched; cur; cur = cur->next) {
        if (strcmp(cur->data.doctor_id, docId) == 0 &&
            strcmp(cur->data.work_date, date) == 0 &&
            strcmp(cur->data.time_slot, timeSlot) == 0 &&
            strcmp(cur->data.status, "正常") == 0) {
            found = 1;
            break;
        }
    }
    free_schedule_list(sched);
    return found;
}

/* ─── 挂号辅助函数 / Registration Helpers ──────────────────────────── */

/* 从排班表获取指定医生+日期+时段的预约号源上限
   Get max appointments for doctor+date+timeslot from schedule */
static int get_schedule_max_appt(const char *docId, const char *date, const char *timeSlot) {
    ScheduleNode *sched = load_schedules_list();
    if (!sched) return 20;
    int max_appt = 20;
    for (ScheduleNode *cur = sched; cur; cur = cur->next) {
        if (strcmp(cur->data.doctor_id, docId) == 0 &&
            strcmp(cur->data.work_date, date) == 0 &&
            strcmp(cur->data.time_slot, timeSlot) == 0 &&
            strcmp(cur->data.status, "正常") == 0) {
            max_appt = cur->data.max_appt;
            if (max_appt <= 0) max_appt = 20;
            break;
        }
    }
    free_schedule_list(sched);
    return max_appt;
}

/* 根据医生职称返回挂号费 / Return registration fee by doctor title */
static float get_registration_fee(const char *title) {
    if (strstr(title, "主任")) return 50.0f;
    if (strstr(title, "副主任")) return 30.0f;
    return 15.0f;  /* 医师/其他 */
}

/* 统计某医生某日期某时段排队人数 (不含已就诊/已取消/已爽约)
   Count waiting appointments for doctor+date+timeslot */
static int count_slot_appointments(const char *doctor_id, const char *date,
                                   const char *timeSlot) {
    int count = 0;
    time_t now = time(NULL);
    struct tm tmNow;
    memcpy(&tmNow, localtime(&now), sizeof(tmNow));
    char todayStr[12];
    snprintf(todayStr, sizeof(todayStr), "%04d-%02d-%02d",
             tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday);

    AppointmentNode *apps = load_appointments_list();
    if (apps) {
        AppointmentNode *cur = apps;
        while (cur) {
            if (strcmp(cur->data.doctor_id, doctor_id) == 0 &&
                strcmp(cur->data.appointment_date, date) == 0 &&
                strcmp(cur->data.appointment_time, timeSlot) == 0 &&
                strcmp(cur->data.status, "已就诊") != 0 &&
                strcmp(cur->data.status, "已完成") != 0 &&
                strcmp(cur->data.status, "已取消") != 0 &&
                strcmp(cur->data.status, "已爽约") != 0) {
                if (strcmp(date, todayStr) < 0 &&
                    strcmp(cur->data.status, "待就诊") == 0) {
                    strcpy(cur->data.status, "已爽约");
                } else {
                    count++;
                }
            }
            cur = cur->next;
        }
        /* 如果有爽约被标记, 保存 / Save if any no-shows were marked */
        AppointmentNode *c2 = apps;
        int modified = 0;
        while (c2) {
            if (strcmp(c2->data.status, "已爽约") == 0) modified = 1;
            c2 = c2->next;
        }
        if (modified) save_appointments_list(apps);
        free_appointment_list(apps);
    }
    return count;
}

/* 统计某患者的爽约次数 / Count no-shows for a patient */
static int count_patient_no_shows(const char *patient_id) {
    int count = 0;
    AppointmentNode *apps = load_appointments_list();
    if (apps) {
        for (AppointmentNode *cur = apps; cur; cur = cur->next) {
            if (strcmp(cur->data.patient_id, patient_id) == 0 &&
                strcmp(cur->data.status, "已爽约") == 0)
                count++;
        }
        free_appointment_list(apps);
    }
    return count;
}

/* 检查同一患者是否已在同科室同时段有预约 / Check duplicate by patient+dept+date+timeslot */
static int has_duplicate_appointment(const char *patient_id,
                                     const char *department_id,
                                     const char *date,
                                     const char *timeSlot) {
    AppointmentNode *apps = load_appointments_list();
    if (!apps) return 0;
    int found = 0;
    for (AppointmentNode *cur = apps; cur; cur = cur->next) {
        if (strcmp(cur->data.patient_id, patient_id) == 0 &&
            strcmp(cur->data.department_id, department_id) == 0 &&
            strcmp(cur->data.appointment_date, date) == 0 &&
            strcmp(cur->data.appointment_time, timeSlot) == 0 &&
            strcmp(cur->data.status, "待就诊") == 0) {
            found = 1;
            break;
        }
    }
    free_appointment_list(apps);
    return found;
}

/* 统计患者待就诊预约总数 / Count active pending appointments for patient */
static int count_patient_active_appointments(const char *patient_id) {
    int count = 0;
    AppointmentNode *apps = load_appointments_list();
    if (!apps) return 0;
    for (AppointmentNode *cur = apps; cur; cur = cur->next) {
        if (strcmp(cur->data.patient_id, patient_id) == 0 &&
            strcmp(cur->data.status, "待就诊") == 0) {
            count++;
        }
    }
    free_appointment_list(apps);
    return count;
}

/* ─── 现场挂号对话框 / Onsite Registration Dialog ───────────────────── */

#define IDC_ONSITE_DEPT     3111
#define IDC_ONSITE_DOCLIST  3112
#define IDC_ONSITE_OK       3113
#define IDC_ONSITE_CANCEL   3114
#define IDC_ONSITE_STATUS   3115

static int g_onsiteResult = 0;

/* 从排班表获取指定医生当日的现场号源上限
   Get max onsite slots for doctor today from schedule */
static int get_today_schedule_max_onsite(const char *doctor_id) {
    char today[12];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    snprintf(today, sizeof(today), "%04d-%02d-%02d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday);

    ScheduleNode *sched = load_schedules_list();
    if (!sched) return 8;
    int max_onsite = 8;
    for (ScheduleNode *cur = sched; cur; cur = cur->next) {
        if (strcmp(cur->data.doctor_id, doctor_id) == 0 &&
            strcmp(cur->data.work_date, today) == 0 &&
            strcmp(cur->data.status, "正常") == 0) {
            max_onsite = cur->data.max_onsite;
            if (max_onsite <= 0) max_onsite = 8;
            break;
        }
    }
    free_schedule_list(sched);
    return max_onsite;
}

/* 统计某医生当日有效现场挂号数 / Count today's valid onsite registrations for a doctor */
static int count_onsite_today_gui(const char *doctor_id) {
    OnsiteRegistrationQueue queue = load_onsite_registration_queue();
    OnsiteRegistrationNode *cur = queue.front;
    int count = 0;
    while (cur) {
        if (strcmp(cur->data.doctor_id, doctor_id) == 0 &&
            strcmp(cur->data.status, "已完成") != 0 &&
            strcmp(cur->data.status, "已退号") != 0) {
            count++;
        }
        cur = cur->next;
    }
    free_onsite_registration_queue(&queue);
    return count;
}

/* 检查是否存在有效挂号冲突（预约+现场） / Check for active registration conflict */
static int has_registration_conflict_gui(const char *patient_id, const char *doctor_id, const char *department_id) {
    AppointmentNode *apps = load_appointments_list();
    for (AppointmentNode *cur = apps; cur; cur = cur->next) {
        if (strcmp(cur->data.patient_id, patient_id) == 0 &&
            strcmp(cur->data.doctor_id, doctor_id) == 0 &&
            strcmp(cur->data.department_id, department_id) == 0 &&
            strcmp(cur->data.status, "已就诊") != 0 &&
            strcmp(cur->data.status, "已完成") != 0 &&
            strcmp(cur->data.status, "已取消") != 0 &&
            strcmp(cur->data.status, "已爽约") != 0) {
            free_appointment_list(apps);
            return 1;
        }
    }
    free_appointment_list(apps);

    OnsiteRegistrationQueue queue = load_onsite_registration_queue();
    for (OnsiteRegistrationNode *cur = queue.front; cur; cur = cur->next) {
        if (strcmp(cur->data.patient_id, patient_id) == 0 &&
            strcmp(cur->data.doctor_id, doctor_id) == 0 &&
            strcmp(cur->data.department_id, department_id) == 0 &&
            strcmp(cur->data.status, "已完成") != 0 &&
            strcmp(cur->data.status, "已退号") != 0) {
            free_onsite_registration_queue(&queue);
            return 1;
        }
    }
    free_onsite_registration_queue(&queue);
    return 0;
}

static int has_any_schedule_today(void) {
    char today[12];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    snprintf(today, sizeof(today), "%04d-%02d-%02d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday);

    ScheduleNode *schedules = load_schedules_list();
    if (!schedules) return 0;
    int found = 0;
    for (ScheduleNode *s = schedules; s; s = s->next) {
        if (strcmp(s->data.work_date, today) == 0 &&
            strcmp(s->data.status, "正常") == 0) {
            found = 1;
            break;
        }
    }
    free_schedule_list(schedules);
    return found;
}

static void RefreshOnsiteDoctorList(HWND hDlg, const char *deptId) {
    HWND hLV = GetDlgItem(hDlg, IDC_ONSITE_DOCLIST);
    if (!hLV) return;
    ListView_DeleteAllItems(hLV);

    char today[12];
    {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        snprintf(today, sizeof(today), "%04d-%02d-%02d",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday);
    }

    int useSchedFilter = has_any_schedule_today();

    DoctorNode *docs = load_doctors_list();
    int row = 0;
    for (DoctorNode *d = docs; d; d = d->next) {
        if (strcmp(d->data.department_id, deptId) != 0) continue;
        if (useSchedFilter && !has_doctor_schedule(d->data.doctor_id, today)) continue;

        int maxOnsite = get_today_schedule_max_onsite(d->data.doctor_id);
        int used = count_onsite_today_gui(d->data.doctor_id);
        int remain = maxOnsite - used;
        if (remain < 0) remain = 0;

        char remText[20];
        snprintf(remText, sizeof(remText), "余%d/%d", remain, maxOnsite);
        const char *items[4] = {
            d->data.doctor_id, d->data.name,
            d->data.title[0] ? d->data.title : "未设置",
            remText
        };
        AddRow(hLV, row++, 4, items);
        if (remain == 0) {
            ListView_SetItemState(hLV, row - 1,
                LVIS_CUT, LVIS_CUT);  /* gray out full slots */
        }
    }
    free_doctor_list(docs);
}

static LRESULT CALLBACK OnsiteRegDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        int x = 20, y = 15;

        CreateWindowA("STATIC", "现场挂号（当日）",
            WS_VISIBLE|WS_CHILD|SS_LEFT,
            x, y, 400, 25, hDlg, NULL, g_hInst, NULL);
        y += 30;

        CreateWindowA("STATIC", "提示: 现场挂号仅限当天，挂号后自动进入候诊队列。",
            WS_VISIBLE|WS_CHILD|SS_LEFT,
            x, y, 450, 20, hDlg, NULL, g_hInst, NULL);
        y += 28;

        CreateWindowA("STATIC", "选择科室:",
            WS_VISIBLE|WS_CHILD|SS_LEFT,
            x, y + 2, 80, 20, hDlg, NULL, g_hInst, NULL);
        HWND hDept = CreateWindowA("COMBOBOX", "",
            WS_VISIBLE|WS_CHILD|CBS_DROPDOWNLIST|WS_VSCROLL|CBS_HASSTRINGS,
            x + 80, y, 250, 200, hDlg, (HMENU)IDC_ONSITE_DEPT, g_hInst, NULL);
        DepartmentNode *depts = load_departments_list();
        if (depts) {
            for (DepartmentNode *d = depts; d; d = d->next)
                SendMessageA(hDept, CB_ADDSTRING, 0, (LPARAM)d->data.name);
            SendMessage(hDept, CB_SETCURSEL, 0, 0);
            free_department_list(depts);
        }
        y += 32;

        CreateWindowA("STATIC", "选择医生:",
            WS_VISIBLE|WS_CHILD|SS_LEFT,
            x, y + 2, 80, 20, hDlg, NULL, g_hInst, NULL);
        y += 22;

        HWND hLV = CreateListView(hDlg, IDC_ONSITE_DOCLIST,
            x, y, 460, 160);
        AddCol(hLV, 0, "医生ID", 90);
        AddCol(hLV, 1, "姓名", 120);
        AddCol(hLV, 2, "职称", 100);
        AddCol(hLV, 3, "剩余名额", 100);
        y += 170;

        /* 初始化医生列表: 用第一个科室 */
        {
            DepartmentNode *dl = load_departments_list();
            if (dl) {
                /* 默认选择第一个科室 */
                DepartmentNode *first = dl;
                char firstDeptId[20] = "";
                strcpy(firstDeptId, first->data.department_id);
                free_department_list(dl);
                RefreshOnsiteDoctorList(hDlg, firstDeptId);
            }
        }

        CreateWindowA("STATIC", "", WS_VISIBLE|WS_CHILD|SS_CENTER,
            x, y, 420, 20, hDlg, (HMENU)IDC_ONSITE_STATUS, g_hInst, NULL);
        y += 30;

        CreateWindowA("BUTTON", "确认挂号", WS_VISIBLE|WS_CHILD|BS_PUSHBUTTON,
            x + 80, y, 120, 30, hDlg, (HMENU)IDC_ONSITE_OK, g_hInst, NULL);
        CreateWindowA("BUTTON", "取消", WS_VISIBLE|WS_CHILD|BS_PUSHBUTTON,
            x + 220, y, 120, 30, hDlg, (HMENU)IDC_ONSITE_CANCEL, g_hInst, NULL);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_ONSITE_DEPT && code == CBN_SELCHANGE) {
            char deptName[100] = {0};
            HWND hDept = GetDlgItem(hDlg, IDC_ONSITE_DEPT);
            int sel = (int)SendMessageA(hDept, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR) {
                SendMessageA(hDept, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)deptName);
                DepartmentNode *depts = load_departments_list();
                for (DepartmentNode *d = depts; d; d = d->next) {
                    if (strcmp(d->data.name, deptName) == 0) {
                        RefreshOnsiteDoctorList(hDlg, d->data.department_id);
                        break;
                    }
                }
                free_department_list(depts);
            }
            return 0;
        }

        if (id == IDC_ONSITE_OK && code == BN_CLICKED) {
            /* 1. 获取选择的科室和医生 / Get selected department and doctor */
            HWND hDept = GetDlgItem(hDlg, IDC_ONSITE_DEPT);
            HWND hLV   = GetDlgItem(hDlg, IDC_ONSITE_DOCLIST);

            int deptSel = (int)SendMessageA(hDept, CB_GETCURSEL, 0, 0);
            if (deptSel == CB_ERR) {
                SetDlgItemTextA(hDlg, IDC_ONSITE_STATUS, "请选择科室");
                return 0;
            }

            int docSel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
            if (docSel < 0) {
                SetDlgItemTextA(hDlg, IDC_ONSITE_STATUS, "请选择一名医生");
                return 0;
            }

            char deptName[100] = {0}, docId[MAX_ID] = {0};
            SendMessageA(hDept, CB_GETLBTEXT, (WPARAM)deptSel, (LPARAM)deptName);
            ListView_GetItemText(hLV, docSel, 0, docId, sizeof(docId));

            /* 获取科室 ID / Get department ID */
            char deptId[MAX_ID] = {0};
            DepartmentNode *depts = load_departments_list();
            for (DepartmentNode *d = depts; d; d = d->next) {
                if (strcmp(d->data.name, deptName) == 0) {
                    strcpy(deptId, d->data.department_id);
                    break;
                }
            }
            free_department_list(depts);

            if (deptId[0] == 0 || docId[0] == 0) {
                SetDlgItemTextA(hDlg, IDC_ONSITE_STATUS, "选择无效");
                return 0;
            }

            /* 2. 获取患者 ID / Get patient ID */
            const char *pid = GetPatientId();
            if (!pid || pid[0] == 0) {
                SetDlgItemTextA(hDlg, IDC_ONSITE_STATUS, "未找到患者信息");
                return 0;
            }

            /* 3. 爽约检查 / No-show check */
            int noShowCount = count_patient_no_shows(pid);
            if (noShowCount >= 3) {
                SetDlgItemTextA(hDlg, IDC_ONSITE_STATUS,
                    "您有3次以上爽约记录, 暂无法挂号");
                return 0;
            }

            /* 4. 活跃记录数检查 (预约+现场) / Active registrations check */
            {
                int activeAppt = count_patient_active_appointments(pid);
                OnsiteRegistrationQueue q = load_onsite_registration_queue();
                int activeOnsite = 0;
                for (OnsiteRegistrationNode *c = q.front; c; c = c->next) {
                    if (strcmp(c->data.patient_id, pid) == 0 &&
                        strcmp(c->data.status, "排队中") == 0)
                        activeOnsite++;
                }
                free_onsite_registration_queue(&q);
                if (activeAppt + activeOnsite >= 3) {
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                        "您已有 %d 条活跃挂号记录 (最多 3 条)", activeAppt + activeOnsite);
                    SetDlgItemTextA(hDlg, IDC_ONSITE_STATUS, msg);
                    return 0;
                }
            }

            /* 5. 重复冲突检查 / Duplicate check */
            if (has_registration_conflict_gui(pid, docId, deptId)) {
                SetDlgItemTextA(hDlg, IDC_ONSITE_STATUS,
                    "您已有该医生的有效挂号记录, 请勿重复挂号");
                return 0;
            }

            /* 6. 医生现场号满额检查 / Doctor onsite slots full check */
            {
                int maxOnsite = get_today_schedule_max_onsite(docId);
                if (count_onsite_today_gui(docId) >= maxOnsite) {
                    SetDlgItemTextA(hDlg, IDC_ONSITE_STATUS,
                        "该医生今日现场号已满, 请选择其他医生或预约挂号");
                    return 0;
                }
            }

            /* 7. 确认 / Confirm */
            float fee = 0;
            DoctorNode *allDocs = load_doctors_list();
            for (DoctorNode *ad = allDocs; ad; ad = ad->next) {
                if (strcmp(ad->data.doctor_id, docId) == 0) {
                    fee = get_registration_fee(ad->data.title);
                    break;
                }
            }
            free_doctor_list(allDocs);

            int queuedAhead = count_onsite_today_gui(docId);
            char confirmMsg[256];
            snprintf(confirmMsg, sizeof(confirmMsg),
                "确认现场挂号?\n\n科室: %s\n医生: %s\n前面还有 %d 人排队\n挂号费: %.2f 元\n\n挂号后将进入排队队列并缴费。",
                deptName, docId, queuedAhead, fee);
            if (MessageBoxA(hDlg, confirmMsg, "确认挂号",
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;

            /* 8. 创建现场号 / Create onsite registration */
            OnsiteRegistration reg;
            memset(&reg, 0, sizeof(reg));
            generate_id(reg.onsite_id, MAX_ID, "OS");
            strcpy(reg.patient_id, pid);
            strcpy(reg.doctor_id, docId);
            strcpy(reg.department_id, deptId);
            reg.queue_number = get_next_onsite_queue_number(docId, deptId);
            strcpy(reg.status, "排队中");
            get_current_time(reg.create_time, sizeof(reg.create_time));
            reg.fee = fee;
            reg.paid = 1;

            /* 加载患者信息判断是否急诊 / Check if emergency patient */
            int is_emergency = 0;
            PatientNode *pts = load_patients_list();
            if (pts) {
                for (PatientNode *p = pts; p; p = p->next) {
                    if (strcmp(p->data.patient_id, pid) == 0) {
                        is_emergency = p->data.is_emergency;
                        break;
                    }
                }
                free_patient_list(pts);
            }

            OnsiteRegistrationQueue queue = load_onsite_registration_queue();
            int ret = enqueue_onsite_registration(&queue, &reg, is_emergency);
            if (ret != SUCCESS) {
                free_onsite_registration_queue(&queue);
                SetDlgItemTextA(hDlg, IDC_ONSITE_STATUS, "入队失败");
                return 0;
            }
            save_onsite_registration_queue(&queue);
            free_onsite_registration_queue(&queue);

            /* 增加医生繁忙度 / Increase doctor busy level */
            DoctorNode *docs = load_doctors_list();
            for (DoctorNode *d = docs; d; d = d->next) {
                if (strcmp(d->data.doctor_id, docId) == 0) {
                    d->data.busy_level++;
                    break;
                }
            }
            save_doctors_list(docs);
            free_doctor_list(docs);

            append_log(g_currentUser.username, "现场挂号", "onsite",
                       reg.onsite_id, deptName);

            char successMsg[256];
            snprintf(successMsg, sizeof(successMsg),
                "现场挂号成功!\n\n现场单号: %s\n排队号: %d\n挂号费: %.2f 元 (已缴)\n\n请前往「预约查询」查看排队状态。",
                reg.onsite_id, reg.queue_number, fee);
            MessageBoxA(hDlg, successMsg, "成功", MB_OK | MB_ICONINFORMATION);

            g_onsiteResult = 1;
            DestroyWindow(hDlg);
            return 0;
        }

        if (id == IDC_ONSITE_CANCEL && code == BN_CLICKED) {
            g_onsiteResult = 0;
            DestroyWindow(hDlg);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        g_onsiteResult = 0;
        DestroyWindow(hDlg);
        return 0;
    default:
        return DefWindowProcA(hDlg, msg, wParam, lParam);
    }
}

static int ShowOnsiteRegDialog(HWND hParent) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = OnsiteRegDlgProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PatientOnsiteRegDialog";
    RegisterClassA(&wc);

    HWND hDlg = CreateWindowExA(0, "PatientOnsiteRegDialog", "现场挂号（当日）",
        WS_VISIBLE|WS_POPUPWINDOW|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 420,
        hParent, NULL, g_hInst, NULL);
    if (!hDlg) return 0;

    RECT pr, rc;
    GetWindowRect(hParent, &pr);
    GetWindowRect(hDlg, &rc);
    SetWindowPos(hDlg, NULL,
        pr.left+(pr.right-pr.left-(rc.right-rc.left))/2,
        pr.top+(pr.bottom-pr.top-(rc.bottom-rc.top))/2,
        0, 0, SWP_NOSIZE|SWP_NOZORDER);

    EnableWindow(hParent, FALSE);
    g_onsiteResult = -1;
    MSG msg;
    while (g_onsiteResult == -1 && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
    return g_onsiteResult == 1;
}

/* ─── 挂号对话框 / Registration Dialog ──────────────────────────────── */

/* 挂号对话框: 科室下拉→联动过滤医生→日期下拉(今天~+6天)→时段下拉(7个时段)
   确认挂号时校验排班 → 创建预约 → 更新医生繁忙度
   Registration: dept combo → filtered doctor combo → date (today~+6d) →
   time slot (7 slots) → validate schedule → create appointment → update busy level */

#define IDC_REG_DEPT    3101
#define IDC_REG_DOCTOR  3102
#define IDC_REG_DATE    3103
#define IDC_REG_TIME    3104
#define IDC_REG_OK      3105
#define IDC_REG_STATUS  3106
#define IDC_REG_QUEUE   3107

static int g_regResult = 0;

/* 动态刷新可用日期 / Refresh available dates based on schedule */
static void RefreshRegDates(HWND hDlg, const char *docId) {
    HWND hDate = GetDlgItem(hDlg, IDC_REG_DATE);
    SendMessage(hDate, CB_RESETCONTENT, 0, 0);
    if (!docId || !docId[0]) return;

    /* 获取今天的日期，用于过滤已过的日期 */
    time_t now = time(NULL);
    struct tm tmNow;
    memcpy(&tmNow, localtime(&now), sizeof(tmNow));
    char todayStr[12];
    snprintf(todayStr, sizeof(todayStr), "%04d-%02d-%02d",
             tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday);

    ScheduleNode *scheds = load_schedules_list();
    char lastDate[20] = "";
    int count = 0;
    for (ScheduleNode *cur = scheds; cur; cur = cur->next) {
        if (strcmp(cur->data.doctor_id, docId) == 0 && strcmp(cur->data.status, "正常") == 0) {
            /* 过滤已过的日期，不显示比今天早的排班 */
            if (strcmp(cur->data.work_date, todayStr) < 0) continue;
            if (strcmp(cur->data.work_date, lastDate) != 0) {
                SendMessageA(hDate, CB_ADDSTRING, 0, (LPARAM)cur->data.work_date);
                strcpy(lastDate, cur->data.work_date);
                count++;
            }
        }
    }
    free_schedule_list(scheds);
    if (count > 0) SendMessage(hDate, CB_SETCURSEL, 0, 0);
}

/* 动态刷新可用时段 / Refresh available time slots based on selected date and doctor */
static void RefreshRegSlots(HWND hDlg, const char *docId, const char *dateStr) {
    HWND hTime = GetDlgItem(hDlg, IDC_REG_TIME);
    SendMessage(hTime, CB_RESETCONTENT, 0, 0);
    if (!docId || !docId[0] || !dateStr || !dateStr[0]) return;

    ScheduleNode *scheds = load_schedules_list();
    int count = 0;
    for (ScheduleNode *cur = scheds; cur; cur = cur->next) {
        if (strcmp(cur->data.doctor_id, docId) == 0 &&
            strcmp(cur->data.work_date, dateStr) == 0 &&
            strcmp(cur->data.status, "正常") == 0) {
            SendMessageA(hTime, CB_ADDSTRING, 0, (LPARAM)cur->data.time_slot);
            count++;
        }
    }
    free_schedule_list(scheds);
    if (count > 0) SendMessage(hTime, CB_SETCURSEL, 0, 0);
}

static LRESULT CALLBACK RegDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        int y = 15;
        CreateWindowA("STATIC", "选择科室:", WS_VISIBLE|WS_CHILD,
            20, y+2, 80, 20, hDlg, NULL, g_hInst, NULL);
        HWND hDept = CreateWindowA("COMBOBOX", "",
            WS_VISIBLE|WS_CHILD|CBS_DROPDOWNLIST|WS_VSCROLL|CBS_HASSTRINGS,
            110, y, 250, 200, hDlg, (HMENU)IDC_REG_DEPT, g_hInst, NULL);

        DepartmentNode *depts = load_departments_list();
        if (depts) {
            for (DepartmentNode *d = depts; d; d = d->next)
                SendMessageA(hDept, CB_ADDSTRING, 0, (LPARAM)d->data.name);
            SendMessage(hDept, CB_SETCURSEL, 0, 0);
            free_department_list(depts);
        }
        y += 30;

        CreateWindowA("STATIC", "选择医生:", WS_VISIBLE|WS_CHILD,
            20, y+2, 80, 20, hDlg, NULL, g_hInst, NULL);
        HWND hDoc = CreateWindowA("COMBOBOX", "",
            WS_VISIBLE|WS_CHILD|CBS_DROPDOWNLIST|WS_VSCROLL|CBS_HASSTRINGS,
            110, y, 250, 200, hDlg, (HMENU)IDC_REG_DOCTOR, g_hInst, NULL);

        DoctorNode *docs = load_doctors_list();
        if (docs) {
            for (DoctorNode *d = docs; d; d = d->next) {
                char label[150];
                snprintf(label, sizeof(label), "%s - %s", d->data.name, d->data.title);
                SendMessageA(hDoc, CB_ADDSTRING, 0, (LPARAM)label);
            }
            SendMessage(hDoc, CB_SETCURSEL, 0, 0);
            free_doctor_list(docs);
        }
        y += 30;
        CreateWindowA("STATIC", "日期:", WS_VISIBLE|WS_CHILD,
            20, y+2, 80, 20, hDlg, NULL, g_hInst, NULL);
        HWND hDate = CreateWindowA("COMBOBOX", "",
            WS_VISIBLE|WS_CHILD|CBS_DROPDOWNLIST|WS_VSCROLL|CBS_HASSTRINGS,
            110, y, 150, 150, hDlg, (HMENU)IDC_REG_DATE, g_hInst, NULL);
        y += 30;

        CreateWindowA("STATIC", "时段:", WS_VISIBLE|WS_CHILD,
            20, y+2, 80, 20, hDlg, NULL, g_hInst, NULL);
        HWND hTime = CreateWindowA("COMBOBOX", "",
            WS_VISIBLE|WS_CHILD|CBS_DROPDOWNLIST|WS_VSCROLL|CBS_HASSTRINGS,
            110, y, 250, 100, hDlg, (HMENU)IDC_REG_TIME, g_hInst, NULL);
        y += 35;

        /* 排队信息显示 / Queue info display */
        CreateWindowA("STATIC", "选择医生、日期和时段后将显示排队人数",
            WS_VISIBLE|WS_CHILD|SS_LEFT,
            20, y, 340, 20, hDlg, (HMENU)IDC_REG_QUEUE, g_hInst, NULL);
        y += 28;

        CreateWindowA("BUTTON", "确认挂号", WS_VISIBLE|WS_CHILD|BS_PUSHBUTTON,
            50, y, 120, 30, hDlg, (HMENU)IDC_REG_OK, g_hInst, NULL);
        CreateWindowA("STATIC", "", WS_VISIBLE|WS_CHILD|SS_CENTER,
            50, y+35, 300, 20, hDlg, (HMENU)IDC_REG_STATUS, g_hInst, NULL);
        return 0;
    }

    case WM_COMMAND: {
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_REG_DEPT) {
            HWND hDept = GetDlgItem(hDlg, IDC_REG_DEPT);
            HWND hDoc  = GetDlgItem(hDlg, IDC_REG_DOCTOR);
            int sel = (int)SendMessage(hDept, CB_GETCURSEL, 0, 0);
            if (sel == CB_ERR) return 0;
            char deptName[100] = {0};
            SendMessageA(hDept, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)deptName);

            char deptId[20] = {0};
            DepartmentNode *depts = load_departments_list();
            for (DepartmentNode *d = depts; d; d = d->next) {
                if (strcmp(d->data.name, deptName) == 0) {
                    strcpy(deptId, d->data.department_id);
                    break;
                }
            }
            free_department_list(depts);

            SendMessage(hDoc, CB_RESETCONTENT, 0, 0);
            if (strlen(deptId) > 0) {
                DoctorNode *docs = load_doctors_list();
                for (DoctorNode *d = docs; d; d = d->next) {
                    if (strcmp(d->data.department_id, deptId) == 0) {
                        char label[150];
                        snprintf(label, sizeof(label), "%s - %s", d->data.name, d->data.title);
                        SendMessageA(hDoc, CB_ADDSTRING, 0, (LPARAM)label);
                    }
                }
                free_doctor_list(docs);
            }
            SendMessage(hDoc, CB_SETCURSEL, 0, 0);
            
            /* Trigger doctor change logic */
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDC_REG_DOCTOR, CBN_SELCHANGE), (LPARAM)hDoc);
            return 0;
        }

        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_REG_DOCTOR) {
            HWND hDoc = (HWND)lParam;
            int sel = (int)SendMessage(hDoc, CB_GETCURSEL, 0, 0);
            if (sel == CB_ERR) return 0;

            char label[150] = {0};
            SendMessageA(hDoc, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)label);
            char docName[100] = {0};
            char *dash = strstr(label, " - ");
            if (dash) {
                size_t len = dash - label;
                if (len >= sizeof(docName)) len = sizeof(docName)-1;
                memcpy(docName, label, len);
                docName[len] = 0;
            } else strcpy(docName, label);

            DoctorNode *docs = load_doctors_list();
            char docId[20] = "";
            for (DoctorNode *d = docs; d; d = d->next) {
                if (strcmp(d->data.name, docName) == 0) {
                    strcpy(docId, d->data.doctor_id);
                    break;
                }
            }
            free_doctor_list(docs);

            RefreshRegDates(hDlg, docId);
            /* Trigger date change logic */
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDC_REG_DATE, CBN_SELCHANGE), (LPARAM)GetDlgItem(hDlg, IDC_REG_DATE));
            return 0;
        }

        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_REG_DATE) {
            HWND hDoc = GetDlgItem(hDlg, IDC_REG_DOCTOR);
            HWND hDate = (HWND)lParam;
            int docSel = (int)SendMessage(hDoc, CB_GETCURSEL, 0, 0);
            int dateSel = (int)SendMessage(hDate, CB_GETCURSEL, 0, 0);
            if (docSel == CB_ERR || dateSel == CB_ERR) return 0;

            char label[150] = {0}, dateStr[20] = {0};
            SendMessageA(hDoc, CB_GETLBTEXT, (WPARAM)docSel, (LPARAM)label);
            SendMessageA(hDate, CB_GETLBTEXT, (WPARAM)dateSel, (LPARAM)dateStr);

            char docName[100] = {0};
            char *dash = strstr(label, " - ");
            if (dash) {
                size_t len = dash - label;
                if (len >= sizeof(docName)) len = sizeof(docName)-1;
                memcpy(docName, label, len);
                docName[len] = 0;
            } else strcpy(docName, label);

            DoctorNode *docs = load_doctors_list();
            char docId[20] = "";
            for (DoctorNode *d = docs; d; d = d->next) {
                if (strcmp(d->data.name, docName) == 0) {
                    strcpy(docId, d->data.doctor_id);
                    break;
                }
            }
            free_doctor_list(docs);

            RefreshRegSlots(hDlg, docId, dateStr);
            /* Trigger time change logic to refresh queue info */
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDC_REG_TIME, CBN_SELCHANGE), (LPARAM)GetDlgItem(hDlg, IDC_REG_TIME));
            return 0;
        }

        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_REG_TIME) {
            HWND hDoc = GetDlgItem(hDlg, IDC_REG_DOCTOR);
            HWND hDate = GetDlgItem(hDlg, IDC_REG_DATE);
            HWND hTime = (HWND)lParam;
            int docSel = (int)SendMessage(hDoc, CB_GETCURSEL, 0, 0);
            int dateSel = (int)SendMessage(hDate, CB_GETCURSEL, 0, 0);
            int timeSel = (int)SendMessage(hTime, CB_GETCURSEL, 0, 0);
            
            if (docSel != CB_ERR && dateSel != CB_ERR && timeSel != CB_ERR) {
                char label[150] = {0}, dateStr[20] = {0}, timeStr[64] = {0};
                SendMessageA(hDoc, CB_GETLBTEXT, (WPARAM)docSel, (LPARAM)label);
                SendMessageA(hDate, CB_GETLBTEXT, (WPARAM)dateSel, (LPARAM)dateStr);
                SendMessageA(hTime, CB_GETLBTEXT, (WPARAM)timeSel, (LPARAM)timeStr);

                char docName[100] = {0};
                char *dash = strstr(label, " - ");
                if (dash) {
                    size_t len = dash - label;
                    if (len >= sizeof(docName)) len = sizeof(docName)-1;
                    memcpy(docName, label, len);
                    docName[len] = 0;
                } else strcpy(docName, label);

                DoctorNode *docs = load_doctors_list();
                char docId[20] = "";
                for (DoctorNode *d = docs; d; d = d->next) {
                    if (strcmp(d->data.name, docName) == 0) {
                        strcpy(docId, d->data.doctor_id);
                        break;
                    }
                }
                free_doctor_list(docs);

                int qty = count_slot_appointments(docId, dateStr, timeStr);
                int maxSlot = get_schedule_max_appt(docId, dateStr, timeStr);
                char qInfo[128];
                snprintf(qInfo, sizeof(qInfo), "号源限量: %d/%d 人 | %s - %s",
                         qty, maxSlot, docName, timeStr);
                SetDlgItemTextA(hDlg, IDC_REG_QUEUE, qInfo);
            } else {
                SetDlgItemTextA(hDlg, IDC_REG_QUEUE, "该医生当前无排班可用");
            }
            return 0;
        }

        if (LOWORD(wParam) == IDC_REG_OK) {
            HWND hDept = GetDlgItem(hDlg, IDC_REG_DEPT);
            HWND hDoc  = GetDlgItem(hDlg, IDC_REG_DOCTOR);

            int deptSel = (int)SendMessage(hDept, CB_GETCURSEL, 0, 0);
            int docSel  = (int)SendMessage(hDoc, CB_GETCURSEL, 0, 0);
            if (deptSel == CB_ERR || docSel == CB_ERR) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "请选择科室和医生");
                return 0;
            }

            char deptName[100] = {0};
            SendMessageA(hDept, CB_GETLBTEXT, (WPARAM)deptSel, (LPARAM)deptName);
            char docLabel[150] = {0};
            SendMessageA(hDoc, CB_GETLBTEXT, (WPARAM)docSel, (LPARAM)docLabel);
            
            char dateStr[20] = {0};
            HWND hDate = GetDlgItem(hDlg, IDC_REG_DATE);
            int dateSel = (int)SendMessage(hDate, CB_GETCURSEL, 0, 0);
            if (dateSel == CB_ERR) { SetDlgItemTextA(hDlg, IDC_REG_STATUS, "请选择日期"); return 0; }
            SendMessageA(hDate, CB_GETLBTEXT, (WPARAM)dateSel, (LPARAM)dateStr);

            char timeSlot[64] = {0};
            HWND hTime = GetDlgItem(hDlg, IDC_REG_TIME);
            int timeSel = (int)SendMessage(hTime, CB_GETCURSEL, 0, 0);
            if (timeSel == CB_ERR) { SetDlgItemTextA(hDlg, IDC_REG_STATUS, "请选择时段"); return 0; }
            SendMessageA(hTime, CB_GETLBTEXT, (WPARAM)timeSel, (LPARAM)timeSlot);

            char deptId[20] = {0};
            DepartmentNode *depts = load_departments_list();
            for (DepartmentNode *d = depts; d; d = d->next) {
                if (strcmp(d->data.name, deptName) == 0) {
                    strcpy(deptId, d->data.department_id);
                    break;
                }
            }
            free_department_list(depts);

            char docName[100] = {0};
            char *dash = strstr(docLabel, " - ");
            if (dash) {
                size_t len = dash - docLabel;
                if (len >= sizeof(docName)) len = sizeof(docName)-1;
                memcpy(docName, docLabel, len);
                docName[len] = 0;
            } else {
                strcpy(docName, docLabel);
            }

            char docId[20] = {0};
            char docTitle[50] = {0};
            DoctorNode *docs = load_doctors_list();
            for (DoctorNode *d = docs; d; d = d->next) {
                if (strcmp(d->data.name, docName) == 0) {
                    strcpy(docId, d->data.doctor_id);
                    strcpy(docTitle, d->data.title);
                    if (d->data.busy_level < 10) d->data.busy_level++;
                    break;
                }
            }
            save_doctors_list(docs);
            free_doctor_list(docs);

            if (strlen(docId) == 0) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "未找到医生信息");
                return 0;
            }

            /* 爽约检查: >=3 次禁止挂号 / No-show check: block if >=3 */
            const char *pid = GetPatientId();
            int noShowCount = count_patient_no_shows(pid);
            if (noShowCount >= 3) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS,
                    "您有3次以上爽约记录, 暂无法挂号");
                return 0;
            }

            /* 活跃预约数检查: 最多 3 条待就诊 / Max 3 active appointments */
            {
                int activeCount = count_patient_active_appointments(pid);
                if (activeCount >= 3) {
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                        "您已有 %d 条待就诊预约 (最多 3 条), 请先取消部分预约后再挂号",
                        activeCount);
                    SetDlgItemTextA(hDlg, IDC_REG_STATUS, msg);
                    return 0;
                }
            }

            /* 同科室同时段重复检查 / Same-dept same-slot duplicate check */
            if (has_duplicate_appointment(pid, deptId, dateStr, timeSlot)) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS,
                    "您已在该科室此时段有预约 (含其他医生), 请勿重复挂号");
                return 0;
            }

            /* 排班校验 / Schedule check */
            if (!doctor_has_schedule(docId, dateStr, timeSlot)) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "该医生此时段无排班");
                return 0;
            }

            /* 号源容量检查 / Capacity check */
            int queued = count_slot_appointments(docId, dateStr, timeSlot);
            int maxSlot = get_schedule_max_appt(docId, dateStr, timeSlot);
            if (queued >= maxSlot) {
                SetDlgItemTextA(hDlg, IDC_REG_STATUS, "该时段已约满, 请选择其他时段");
                return 0;
            }

            /* 挂号费 / Registration fee */
            float fee = get_registration_fee(docTitle);
            char confirmMsg[256];
            snprintf(confirmMsg, sizeof(confirmMsg),
                "医生: %s (%s)\n日期: %s\n时段: %s\n号源限量: %d/%d 人\n挂号费: %.0f 元\n\n确认挂号并缴费?",
                docName, docTitle, dateStr, timeSlot, queued + 1, maxSlot, fee);
            if (MessageBoxA(hDlg, confirmMsg, "确认挂号",
                           MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;

            /* 创建预约 */
            Appointment apt;
            memset(&apt, 0, sizeof(apt));
            generate_id(apt.appointment_id, sizeof(apt.appointment_id), "APT");
            strcpy(apt.patient_id, pid);
            strcpy(apt.doctor_id, docId);
            strcpy(apt.department_id, deptId);
            strcpy(apt.appointment_date, dateStr);
            strcpy(apt.appointment_time, timeSlot);
            strcpy(apt.status, "待就诊");
            get_current_time(apt.create_time, sizeof(apt.create_time));
            apt.fee = fee;
            apt.paid = 1;

            AppointmentNode *head = load_appointments_list();
            AppointmentNode *node = create_appointment_node(&apt);
            node->next = head;
            save_appointments_list(node);
            free_appointment_list(node);

            append_log(g_currentUser.username, "挂号缴费", "appointment",
                       apt.appointment_id, "");

            char doneMsg[128];
            snprintf(doneMsg, sizeof(doneMsg),
                "挂号成功! 排队第 %d 位, 挂号费 %.0f 元已缴。", queued + 1, fee);
            MessageBoxA(hDlg, doneMsg, "成功", MB_OK | MB_ICONINFORMATION);

            g_regResult = 1;
            DestroyWindow(hDlg);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        g_regResult = 0;
        DestroyWindow(hDlg);
        return 0;
    default:
        return DefWindowProcA(hDlg, msg, wParam, lParam);
    }
}

static int ShowRegDialog(HWND hParent) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = RegDlgProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = "PatientRegDialog";
    RegisterClassA(&wc);

    HWND hDlg = CreateWindowExA(0, "PatientRegDialog", "预约挂号",
        WS_VISIBLE|WS_POPUPWINDOW|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 330,
        hParent, NULL, g_hInst, NULL);
    if (!hDlg) return 0;

    RECT pr, rc;
    GetWindowRect(hParent, &pr);
    GetWindowRect(hDlg, &rc);
    SetWindowPos(hDlg, NULL,
        pr.left+(pr.right-pr.left-(rc.right-rc.left))/2,
        pr.top+(pr.bottom-pr.top-(rc.bottom-rc.top))/2,
        0, 0, SWP_NOSIZE|SWP_NOZORDER);

    EnableWindow(hParent, FALSE);
    g_regResult = -1;
    MSG msg;
    while (g_regResult == -1 && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
    return g_regResult == 1;
}

/* ─── 页面窗口过程 / Patient Page Window Procedure ──────────────────── */

/* 获取选中的 ListView 指定列文本 */
static void GetSelectedItemText(HWND hLV, int col, char *buf, int size) {
    int sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
    if (sel >= 0) {
        ListView_GetItemText(hLV, sel, col, buf, size);
    } else {
        buf[0] = 0;
    }
}

static void PopulatePaymentList(HWND hLV);

/* PatientPageWndProc — 患者页面的通用消息处理
   WM_CREATE: 存储 viewId 到 GWLP_USERDATA
   WM_SIZE:   撑满所有子控件
   WM_COMMAND: 处理挂号(1010)/取消预约(1002)/编辑资料(1020)按钮
   WM_NOTIFY: 诊断页面 ListView 选中变化 → 显示诊断详情 */

static LRESULT CALLBACK PatientPageWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    int viewId = (int)GetWindowLongPtrA(hWnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_SIZE: {
        int cx = LOWORD(lParam), cy = HIWORD(lParam);

        if (viewId == NAV_PATIENT_APPOINTMENT) {
            HWND hAptLV  = GetDlgItem(hWnd, 2001);
            HWND hOnsLV  = GetDlgItem(hWnd, 2011);
            HWND hCancel = GetDlgItem(hWnd, 1002);
            HWND hBack   = GetDlgItem(hWnd, 1003);
            int lvY = 30, lvH = cy - 75;
            if (hAptLV)  SetWindowPos(hAptLV, NULL, 5, lvY, cx - 10, lvH, SWP_NOZORDER);
            if (hOnsLV)  SetWindowPos(hOnsLV, NULL, 5, lvY, cx - 10, lvH, SWP_NOZORDER);
            if (hCancel) SetWindowPos(hCancel, NULL, cx - 100, cy - 40, 90, 30, SWP_NOZORDER);
            if (hBack)   SetWindowPos(hBack, NULL, cx - 100, cy - 40, 90, 30, SWP_NOZORDER);
        } else if (viewId == NAV_PATIENT_PRESCRIPTION) {
            HWND hLV = GetDlgItem(hWnd, 4001);
            if (hLV) SetWindowPos(hLV, NULL, 5, 5, cx - 10, cy - 90, SWP_NOZORDER);
            HWND hBtn = GetDlgItem(hWnd, 4002);
            if (hBtn) SetWindowPos(hBtn, NULL, cx - 105, cy - 40, 100, 30, SWP_NOZORDER);
            HWND hCheck = GetDlgItem(hWnd, 4003);
            if (hCheck) SetWindowPos(hCheck, NULL, 5, cy - 40, 150, 20, SWP_NOZORDER);
        } else {
            HWND hChild = GetWindow(hWnd, GW_CHILD);
            while (hChild) {
                SetWindowPos(hChild, NULL, 0, 0, cx, cy - 40, SWP_NOZORDER | SWP_NOMOVE);
                hChild = GetWindow(hChild, GW_HWNDNEXT);
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == 4002 && code == BN_CLICKED) { /* 立即缴费 */
            HWND hLV = GetDlgItem(hWnd, 4001);
            char targetId[MAX_ID] = "";
            GetSelectedItemText(hLV, 0, targetId, sizeof(targetId));
            if (targetId[0] == 0) {
                MessageBoxA(hWnd, "请先选择一个待缴费项目", "提示", MB_OK);
                return 0;
            }

            int useInsurance = (IsDlgButtonChecked(hWnd, 4003) == BST_CHECKED);

            if (strncmp(targetId, "PR", 2) == 0) {
                PrescriptionNode *list = load_prescriptions_list();
                for (PrescriptionNode *cur = list; cur; cur = cur->next) {
                    if (strcmp(cur->data.prescription_id, targetId) == 0) {
                        float finalPrice = cur->data.total_price;
                        float reimbAmount = 0.0f;
                        char reimbInfo[64] = "";

                        if (useInsurance) {
                            Drug *drug = find_drug_by_id(cur->data.drug_id);
                            Patient *patient = find_patient_by_id(cur->data.patient_id);
                            if (drug && patient) {
                                reimbAmount = calculate_drug_reimbursement(drug, cur->data.quantity, patient->patient_type);
                                finalPrice = cur->data.total_price - reimbAmount;
                                if (finalPrice < 0.0f) finalPrice = 0.0f;
                                snprintf(reimbInfo, sizeof(reimbInfo),
                                         " (医保报销: %.2f 元, 药品比例: %.0f%%)",
                                         reimbAmount, drug->reimbursement_ratio * 100);
                            } else {
                                snprintf(reimbInfo, sizeof(reimbInfo), " (无法获取医保信息)");
                            }
                            free(drug);
                            free(patient);
                        }

                        char msg[256];
                        snprintf(msg, sizeof(msg), "应付金额: %.2f 元%s\n确认支付?",
                                 finalPrice, reimbInfo);
                        if (MessageBoxA(hWnd, msg, "支付确认", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                            cur->data.paid = 1;
                            save_prescriptions_list(list);
                            append_log(g_currentUser.username, "缴纳处方费", "prescription", targetId, useInsurance ? "医保" : "自费");
                            MessageBoxA(hWnd, "支付成功", "成功", MB_OK);
                        }
                        break;
                    }
                }
                free_prescription_list(list);
            } else if (strncmp(targetId, "APT", 3) == 0) {
                /* ─── 挂号缴费（支持医保报销） / Appointment Payment (insurance supported) ─── */
                AppointmentNode *list = load_appointments_list();
                for (AppointmentNode *cur = list; cur; cur = cur->next) {
                    if (strcmp(cur->data.appointment_id, targetId) == 0) {
                        /* 实付金额初始为挂号费原价 / final price starts at original fee */
                        float finalPrice = cur->data.fee;
                        float reimbAmount = 0.0f;
                        char reimbInfo[64] = "";

                        /* 勾选"使用医保报销"时，按患者类型计算报销金额
                           When insurance checkbox is checked, calculate reimbursement by patient type:
                             - 医保患者报销 60% / insured patients get 60% reimbursement
                             - 军人患者报销 80% / military patients get 80% reimbursement
                             - 普通/自费患者不报销 / ordinary/self-pay patients get 0% */
                        if (useInsurance) {
                            /* 通过预约记录中的 patient_id 查找患者档案，获取患者类型
                               Look up patient profile by patient_id from appointment to get patient_type */
                            Patient *patient = find_patient_by_id(cur->data.patient_id);
                            if (patient) {
                                /* 计算报销金额 (基于挂号费原价 × 患者类型报销比率)
                                   Calculate reimbursement amount based on original fee × patient type ratio */
                                reimbAmount = calculate_reimbursement(cur->data.fee, patient->patient_type);
                                /* 实付金额 = 原价 − 报销金额 (不低于 0)
                                   Final price = original fee − reimbursement (floor at 0) */
                                finalPrice = cur->data.fee - reimbAmount;
                                if (finalPrice < 0.0f) finalPrice = 0.0f;
                                /* 拼接报销提示信息 / build reimbursement info string */
                                snprintf(reimbInfo, sizeof(reimbInfo),
                                         " (医保报销: %.2f 元)", reimbAmount);
                                free(patient);
                            } else {
                                /* 找不到患者档案时的兜底提示 / fallback when patient record not found */
                                snprintf(reimbInfo, sizeof(reimbInfo), " (无法获取医保信息)");
                            }
                        }

                        /* 弹出确认对话框，展示实付金额和报销明细，用户确认后才扣款
                           Show confirmation dialog with final price and reimbursement breakdown */
                        char msg[256];
                        snprintf(msg, sizeof(msg), "应付金额: %.2f 元%s\n确认支付?",
                                 finalPrice, reimbInfo);
                        if (MessageBoxA(hWnd, msg, "支付确认", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                            /* 标记为已缴费并保存 / Mark as paid and persist */
                            cur->data.paid = 1;
                            save_appointments_list(list);
                            /* 记录操作日志 (操作类型: 缴纳挂号费, 支付方式: 医保/自费)
                               Log the payment action with payment method (insurance/self-pay) */
                            append_log(g_currentUser.username, "缴纳挂号费", "appointment", targetId, useInsurance ? "医保" : "自费");
                            MessageBoxA(hWnd, "支付成功", "成功", MB_OK);
                        }
                        break;
                    }
                }
                free_appointment_list(list);
            } else if (strncmp(targetId, "WC", 2) == 0) {
                /* 病房服务缴费 / Ward service payment */
                WardCallNode *list = load_ward_calls_list();
                for (WardCallNode *cur = list; cur; cur = cur->next) {
                    if (strcmp(cur->data.call_id, targetId) == 0) {
                        float finalPrice = cur->data.fee;
                        float reimbAmount = 0.0f;
                        char reimbInfo[64] = "";

                        if (useInsurance) {
                            Patient *patient = find_patient_by_id(cur->data.patient_id);
                            if (patient) {
                                reimbAmount = calculate_reimbursement(cur->data.fee, patient->patient_type);
                                finalPrice = cur->data.fee - reimbAmount;
                                if (finalPrice < 0.0f) finalPrice = 0.0f;
                                snprintf(reimbInfo, sizeof(reimbInfo),
                                         " (医保报销: %.2f 元)", reimbAmount);
                                free(patient);
                            } else {
                                snprintf(reimbInfo, sizeof(reimbInfo), " (无法获取医保信息)");
                            }
                        }

                        char msg[256];
                        snprintf(msg, sizeof(msg), "应付金额: %.2f 元%s\n确认支付?",
                                 finalPrice, reimbInfo);
                        if (MessageBoxA(hWnd, msg, "支付确认", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                            cur->data.paid = 1;
                            save_ward_calls_list(list);
                            append_log(g_currentUser.username, "缴纳病房费", "ward_call", targetId, useInsurance ? "医保" : "自费");
                            MessageBoxA(hWnd, "支付成功", "成功", MB_OK);
                        }
                        break;
                    }
                }
                free_ward_call_list(list);
            } else if (strncmp(targetId, "MS", 2) == 0) {
                /* 其他医疗服务缴费 / Other medical service payment */
                OtherServiceNode *list = load_other_services_list();
                for (OtherServiceNode *cur = list; cur; cur = cur->next) {
                    if (strcmp(cur->data.service_id, targetId) == 0) {
                        float finalPrice = cur->data.total_price;
                        float reimbAmount = 0.0f;
                        char reimbInfo[64] = "";

                        if (useInsurance) {
                            Patient *patient = find_patient_by_id(cur->data.patient_id);
                            if (patient) {
                                reimbAmount = calculate_reimbursement(cur->data.total_price, patient->patient_type);
                                finalPrice = cur->data.total_price - reimbAmount;
                                if (finalPrice < 0.0f) finalPrice = 0.0f;
                                snprintf(reimbInfo, sizeof(reimbInfo),
                                         " (医保报销: %.2f 元)", reimbAmount);
                                free(patient);
                            } else {
                                snprintf(reimbInfo, sizeof(reimbInfo), " (无法获取医保信息)");
                            }
                        }

                        char msg[256];
                        snprintf(msg, sizeof(msg), "%s\r\n应付金额: %.2f 元%s\r\n确认支付?",
                                 cur->data.service_name, finalPrice, reimbInfo);
                        if (MessageBoxA(hWnd, msg, "支付确认", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                            cur->data.paid = 1;
                            save_other_services_list(list);
                            append_log(g_currentUser.username, "缴纳医疗服务费", "other_service", targetId, useInsurance ? "医保" : "自费");
                            MessageBoxA(hWnd, "支付成功", "成功", MB_OK);
                        }
                        break;
                    }
                }
                free_other_service_list(list);
            }
            PopulatePaymentList(hLV);
            return 0;
        }

        if (code != BN_CLICKED) return 0;
        switch (id) {

        case 1010: { /* 预约挂号（从挂号页面发起） */
            if (ShowRegDialog(hWnd)) {
                MessageBoxA(hWnd, "挂号成功！请前往「预约查询」查看。", "成功", MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }

        case 1011: { /* 现场挂号（从挂号页面发起） */
            if (ShowOnsiteRegDialog(hWnd)) {
                MessageBoxA(hWnd, "现场挂号成功！请前往「预约查询」查看排队状态。", "成功", MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }

        case 2020: { /* 预约挂号 tab */
            HWND hAptLV = GetDlgItem(hWnd, 2001);
            HWND hOnsLV = GetDlgItem(hWnd, 2011);
            HWND hCancel = GetDlgItem(hWnd, 1002);
            HWND hBack   = GetDlgItem(hWnd, 1003);
            if (hAptLV)  ShowWindow(hAptLV, SW_SHOW);
            if (hOnsLV)  ShowWindow(hOnsLV, SW_HIDE);
            if (hCancel) ShowWindow(hCancel, SW_SHOW);
            if (hBack)   ShowWindow(hBack, SW_HIDE);
            return 0;
        }

        case 2021: { /* 现场挂号 tab */
            HWND hAptLV = GetDlgItem(hWnd, 2001);
            HWND hOnsLV = GetDlgItem(hWnd, 2011);
            HWND hCancel = GetDlgItem(hWnd, 1002);
            HWND hBack   = GetDlgItem(hWnd, 1003);
            if (hAptLV)  ShowWindow(hAptLV, SW_HIDE);
            if (hOnsLV)  ShowWindow(hOnsLV, SW_SHOW);
            if (hCancel) ShowWindow(hCancel, SW_HIDE);
            if (hBack)   ShowWindow(hBack, SW_SHOW);
            return 0;
        }

        case 1003: { /* 退号（现场挂号记录页） */
            HWND hLV = GetDlgItem(hWnd, 2011);
            if (!hLV) return 0;
            int sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
            if (sel < 0) {
                MessageBoxA(hWnd, "请先选择一个现场挂号", "提示", MB_OK);
                return 0;
            }
            char onsId[MAX_ID] = "";
            char status[20] = "";
            ListView_GetItemText(hLV, sel, 0, onsId, sizeof(onsId));
            ListView_GetItemText(hLV, sel, 4, status, sizeof(status));
            if (strcmp(status, "排队中") != 0) {
                MessageBoxA(hWnd, "当前状态无法退号", "提示", MB_OK);
                return 0;
            }
            if (MessageBoxA(hWnd, "确定退号？退号后需重新排队", "确认",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                OnsiteRegistrationQueue onQ = load_onsite_registration_queue();
                OnsiteRegistrationNode *on = onQ.front;
                while (on) {
                    if (strcmp(on->data.onsite_id, onsId) == 0) {
                        strcpy(on->data.status, "已退号");
                        break;
                    }
                    on = on->next;
                }
                save_onsite_registration_queue(&onQ);
                free_onsite_registration_queue(&onQ);
                append_log(g_currentUser.username, "退号", "onsite", onsId, "");
                MessageBoxA(hWnd, "已退号", "成功", MB_OK);
                PostMessage(GetParent(hWnd), WM_APP + 1, NAV_PATIENT_APPOINTMENT, 0);
            }
            return 0;
        }

        case 1002: { /* 取消预约（预约查询页面） */
            HWND hLV = GetDlgItem(hWnd, 2001);
            int sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
            if (sel < 0) {
                MessageBoxA(hWnd, "请先选择一个预约", "提示", MB_OK);
                return 0;
            }
            char apptId[20] = {0};
            ListView_GetItemText(hLV, sel, 0, apptId, sizeof(apptId));
            AppointmentNode *apps = load_appointments_list();
            float refundFee = 0.0f;
            int canRefund = 0;
            for (AppointmentNode *cur = apps; cur; cur = cur->next) {
                if (strcmp(cur->data.appointment_id, apptId) == 0) {
                    if (strcmp(cur->data.status, "待就诊") == 0 &&
                        cur->data.paid && cur->data.fee > 0) {
                        refundFee = cur->data.fee;
                        canRefund = 1;
                    }
                    break;
                }
            }
            char cancelMsg[256];
            if (canRefund)
                snprintf(cancelMsg, sizeof(cancelMsg),
                    "确定取消该预约吗？\n将退还挂号费 %.0f 元。", refundFee);
            else
                snprintf(cancelMsg, sizeof(cancelMsg), "确定取消该预约吗？");
            if (MessageBoxA(hWnd, cancelMsg, "确认",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                for (AppointmentNode *cur = apps; cur; cur = cur->next) {
                    if (strcmp(cur->data.appointment_id, apptId) == 0) {
                        strcpy(cur->data.status, "已取消");
                        cur->data.paid = 0;  /* 退费 */
                        break;
                    }
                }
                save_appointments_list(apps);
                if (canRefund)
                    append_log(g_currentUser.username, "退号退费", "appointment",
                               apptId, "");
                MessageBoxA(hWnd, "预约已取消, 号源已释放", "成功", MB_OK);
                PostMessage(GetParent(hWnd), WM_APP + 1, NAV_PATIENT_APPOINTMENT, 0);
            }
            free_appointment_list(apps);
            return 0;
        }

        case 1020: { /* 编辑个人信息 */
            const char *pid = GetPatientId();
            if (strlen(pid) == 0) {
                MessageBoxA(hWnd, "未找到患者信息", "错误", MB_OK);
                return 0;
            }
            Patient *p = find_patient_by_id(pid);
            if (!p) {
                MessageBoxA(hWnd, "未找到患者信息", "错误", MB_OK);
                return 0;
            }

            char ageStr[16];
            snprintf(ageStr, sizeof(ageStr), "%d", p->age);
            PatFieldDef f[6] = {
                {"姓名", "", MAX_NAME, FALSE},
                {"性别", "", 10, FALSE},
                {"年龄", "", 16, FALSE},
                {"电话", "", 20, FALSE},
                {"地址", "", 200, FALSE},
            };
            strcpy(f[0].value, p->name);
            strcpy(f[1].value, p->gender);
            strcpy(f[2].value, ageStr);
            strcpy(f[3].value, p->phone);
            strcpy(f[4].value, p->address);

            if (ShowPatFieldDialog(hWnd, "编辑个人信息", f, 5)) {
                PatientNode *head = load_patients_list();
                for (PatientNode *cur = head; cur; cur = cur->next) {
                    if (strcmp(cur->data.patient_id, pid) == 0) {
                        strcpy(cur->data.name, f[0].value);
                        strcpy(cur->data.gender, f[1].value);
                        cur->data.age = atoi(f[2].value);
                        strcpy(cur->data.phone, f[3].value);
                        strcpy(cur->data.address, f[4].value);
                        break;
                    }
                }
                save_patients_list(head);
                free_patient_list(head);
                MessageBoxA(hWnd, "个人信息已更新", "成功", MB_OK);
                PostMessage(GetParent(hWnd), WM_APP + 1, NAV_PATIENT_PROFILE, 0);
            }
            return 0;
        }
        }
        return 0;
    }

    case WM_NOTIFY: {
        if (viewId == NAV_PATIENT_DIAGNOSIS) {
            NMHDR *nm = (NMHDR *)lParam;
            if (nm->idFrom == 2002 && nm->code == LVN_ITEMCHANGED) {
                NMLISTVIEW *nmlv = (NMLISTVIEW *)lParam;
                if ((nmlv->uChanged & LVIF_STATE) &&
                    (nmlv->uNewState & LVIS_SELECTED) &&
                    !(nmlv->uOldState & LVIS_SELECTED)) {
                    char recId[20] = "";
                    ListView_GetItemText(nm->hwndFrom, nmlv->iItem, 0, recId, sizeof(recId));
                    if (recId[0] == 0) return 0;

                    MedicalRecordNode *recs = load_medical_records_list();
                    if (recs) {
                        MedicalRecordNode *cur = recs;
                        while (cur) {
                            if (strcmp(cur->data.record_id, recId) == 0) {
                                char diagPart[512] = "", advicePart[512] = "";
                                char *p = strstr(cur->data.diagnosis, " | 治疗建议: ");
                                if (p) {
                                    size_t diagLen = p - cur->data.diagnosis;
                                    if (diagLen >= sizeof(diagPart)) diagLen = sizeof(diagPart) - 1;
                                    strncpy(diagPart, cur->data.diagnosis, diagLen);
                                    diagPart[diagLen] = 0;
                                    strcpy(advicePart, p + 13); /* Skip " | 治疗建议: " */
                                } else {
                                    strcpy(diagPart, cur->data.diagnosis);
                                }

                                SetDlgItemTextA(hWnd, 3000, diagPart);
                                SetDlgItemTextA(hWnd, 3001, advicePart);
                                break;
                            }
                            cur = cur->next;
                        }
                        free_medical_record_list(recs);
                    }
                }
            }
        }
        return 0;
    }

    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
}

/* ─── 挂号页面 / Registration Page ──────────────────────────────────── */

static HWND CreateRegisterPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = PatientPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PatientRegisterPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "PatientRegisterPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_PATIENT_REGISTER);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 40;
    int y = 20;

    CreateWindowA("STATIC", "请选择挂号方式",
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        20, y, 200, 25, hPage, NULL, g_hInst, NULL);
    y += 35;

    CreateWindowA("STATIC", "预约挂号: 选择未来7天内日期和时段",
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        20, y, 400, 20, hPage, NULL, g_hInst, NULL);
    y += 25;

    CreateWindowA("BUTTON", "预约挂号",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        20, y, 160, 35, hPage, (HMENU)1010, g_hInst, NULL);
    y += 50;

    CreateWindowA("STATIC", "现场挂号: 当日挂号，自动进入排队队列",
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        20, y, 400, 20, hPage, NULL, g_hInst, NULL);
    y += 25;

    CreateWindowA("BUTTON", "现场挂号 (当日)",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        20, y, 160, 35, hPage, (HMENU)1011, g_hInst, NULL);

    return hPage;
}

/* ─── 预约查询页面 / Appointment Query Page ─────────────────────────── */

static HWND CreateAppointmentPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = PatientPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PatientApptPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "PatientApptPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_PATIENT_APPOINTMENT);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 10;
    int h = (rc->bottom - rc->top) - 10;

    /* Radio buttons for tab switching */
    CreateWindowA("BUTTON", "预约挂号",
        WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON | WS_GROUP,
        10, 5, 80, 22, hPage, (HMENU)2020, g_hInst, NULL);
    CreateWindowA("BUTTON", "现场挂号",
        WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON,
        100, 5, 80, 22, hPage, (HMENU)2021, g_hInst, NULL);
    CheckRadioButton(hPage, 2020, 2021, 2020);

    int lvY = 30, lvH = h - 75;
    const char *pid = GetPatientId();

    /* ── 预约挂号 ListView / Appointment ListView ── */
    HWND hAptLV = CreateListView(hPage, 2001, 5, lvY, w, lvH);
    AddCol(hAptLV, 0, "预约ID", 100);
    AddCol(hAptLV, 1, "医生", 80);
    AddCol(hAptLV, 2, "日期", 100);
    AddCol(hAptLV, 3, "时段", 60);
    AddCol(hAptLV, 4, "状态", 60);

    DoctorNode *docList = load_doctors_list();
    DepartmentNode *aptDepts = load_departments_list();
    int aptRow = 0;
    AppointmentNode *apps = load_appointments_list();
    if (apps && strlen(pid) > 0) {
        AppointmentNode *cur = apps;
        while (cur) {
            if (strcmp(cur->data.patient_id, pid) == 0) {
                const char *docName = cur->data.doctor_id;
                for (DoctorNode *dn = docList; dn; dn = dn->next) {
                    if (strcmp(dn->data.doctor_id, cur->data.doctor_id) == 0) {
                        docName = dn->data.name; break;
                    }
                }
                const char *items[5] = {
                    cur->data.appointment_id, docName,
                    cur->data.appointment_date, cur->data.appointment_time,
                    cur->data.status
                };
                AddRow(hAptLV, aptRow++, 5, items);
            }
            cur = cur->next;
        }
    }
    free_appointment_list(apps);

    /* ── 现场挂号 ListView / Onsite Registration ListView ── */
    HWND hOnsLV = CreateListView(hPage, 2011, 5, lvY, w, lvH);
    AddCol(hOnsLV, 0, "现场单号", 130);
    AddCol(hOnsLV, 1, "医生", 80);
    AddCol(hOnsLV, 2, "科室", 80);
    AddCol(hOnsLV, 3, "排队号", 60);
    AddCol(hOnsLV, 4, "状态", 60);
    AddCol(hOnsLV, 5, "排队位置", 60);

    int onsRow = 0;
    if (strlen(pid) > 0) {
        OnsiteRegistrationQueue onQ = load_onsite_registration_queue();
        OnsiteRegistrationNode *on = onQ.front;
        while (on) {
            if (strcmp(on->data.patient_id, pid) == 0) {
                /* 计算排队位置: 同医生同科室中排队号更小的"排队中"数量
                   Calc queue position: count "排队中" entries for same
                   doctor+dept with smaller queue_number */
                int ahead = 0;
                if (strcmp(on->data.status, "排队中") == 0) {
                    OnsiteRegistrationNode *o2 = onQ.front;
                    while (o2) {
                        if (strcmp(o2->data.doctor_id, on->data.doctor_id) == 0 &&
                            strcmp(o2->data.department_id, on->data.department_id) == 0 &&
                            strcmp(o2->data.status, "排队中") == 0 &&
                            o2->data.queue_number < on->data.queue_number) {
                            ahead++;
                        }
                        o2 = o2->next;
                    }
                }
                char qn[12], aheadStr[20];
                snprintf(qn, sizeof(qn), "%d", on->data.queue_number);
                if (strcmp(on->data.status, "排队中") == 0) {
                    snprintf(aheadStr, sizeof(aheadStr), "还剩%d人", ahead);
                } else {
                    aheadStr[0] = 0;
                }
                const char *docName2 = on->data.doctor_id;
                for (DoctorNode *dn = docList; dn; dn = dn->next) {
                    if (strcmp(dn->data.doctor_id, on->data.doctor_id) == 0) {
                        docName2 = dn->data.name; break;
                    }
                }
                const char *deptName2 = on->data.department_id;
                if (aptDepts) {
                    DepartmentNode *dn = aptDepts;
                    while (dn) {
                        if (strcmp(dn->data.department_id, on->data.department_id) == 0)
                        { deptName2 = dn->data.name; break; }
                        dn = dn->next;
                    }
                }
                const char *items[6] = {
                    on->data.onsite_id, docName2,
                    deptName2, qn, on->data.status,
                    aheadStr
                };
                AddRow(hOnsLV, onsRow++, 6, items);
            }
            on = on->next;
        }
        free_onsite_registration_queue(&onQ);
    }
    free_doctor_list(docList);
    if (aptDepts) free_department_list(aptDepts);
    ShowWindow(hOnsLV, SW_HIDE);

    /* Action buttons */
    CreateWindowA("BUTTON", "取消预约",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        w - 100, h - 40, 90, 30, hPage, (HMENU)1002, g_hInst, NULL);
    CreateWindowA("BUTTON", "退号",
        WS_CHILD | BS_PUSHBUTTON,  /* hidden initially */
        w - 100, h - 40, 90, 30, hPage, (HMENU)1003, g_hInst, NULL);

    return hPage;
}

/* ─── 诊断结果页面 / Diagnosis Results Page ─────────────────────────── */

static HWND CreateDiagnosisPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = PatientPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PatientDiagPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "PatientDiagPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_PATIENT_DIAGNOSIS);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 10;

    HWND hLV = CreateListView(hPage, 2002, 5, 5, w - 10, 150);
    AddCol(hLV, 0, "病历ID", 100);
    AddCol(hLV, 1, "医生", 80);
    AddCol(hLV, 2, "日期", 100);
    AddCol(hLV, 3, "状态", 60);

    const char *pid = GetPatientId();
    MedicalRecordNode *recs = load_medical_records_list();
    DoctorNode *diagDocs = load_doctors_list();
    int row = 0;
    if (recs && strlen(pid) > 0) {
        MedicalRecordNode *cur = recs;
        while (cur) {
            if (strcmp(cur->data.patient_id, pid) == 0) {
                const char *docName = cur->data.doctor_id;
                if (diagDocs) {
                    DoctorNode *dn = diagDocs;
                    while (dn) {
                        if (strcmp(dn->data.doctor_id, cur->data.doctor_id) == 0)
                        { docName = dn->data.name; break; }
                        dn = dn->next;
                    }
                }
                const char *items[4] = {
                    cur->data.record_id,
                    docName,
                    cur->data.diagnosis_date,
                    cur->data.status
                };
                AddRow(hLV, row++, 4, items);
            }
            cur = cur->next;
        }
    }
    free_medical_record_list(recs);
    if (diagDocs) free_doctor_list(diagDocs);

    CreateWindowA("STATIC", "诊断:",
        WS_VISIBLE | WS_CHILD, 5, 165, 100, 20,
        hPage, NULL, g_hInst, NULL);
    CreateWindowA("EDIT", "",
        WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_READONLY |
        WS_BORDER | WS_VSCROLL,
        5, 190, w - 10, 60,
        hPage, (HMENU)3000, g_hInst, NULL);

    CreateWindowA("STATIC", "治疗建议:",
        WS_VISIBLE | WS_CHILD, 5, 255, 100, 20,
        hPage, NULL, g_hInst, NULL);
    CreateWindowA("EDIT", "",
        WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_READONLY |
        WS_BORDER | WS_VSCROLL,
        5, 280, w - 10, (rc->bottom - rc->top) - 290,
        hPage, (HMENU)3001, g_hInst, NULL);

    return hPage;
}

/* ─── 缴费页面 / Payment Page ────────────────────────── */
/* 支持多种医疗服务: 处方/挂号/病房/其他, 按ID前缀自动识别
   Service types: PR=处方药, APT/O_=挂号, WC=病房, MS=其他医疗服务 */

static void PopulatePaymentList(HWND hLV) {
    ListView_DeleteAllItems(hLV);
    const char *pid = GetPatientId();
    if (!pid || !pid[0]) return;

    int row = 0;

    /* 1. 待缴费处方 / Unpaid Prescriptions */
    PrescriptionNode *rxList = load_prescriptions_list();
    DrugNode *payDrugs = load_drugs_list();
    for (PrescriptionNode *cur = rxList; cur; cur = cur->next) {
        if (strcmp(cur->data.patient_id, pid) == 0 && !cur->data.paid) {
            char price[20], drugType[80];
            snprintf(price, sizeof(price), "%.2f", cur->data.total_price);
            snprintf(drugType, sizeof(drugType), "处方药");
            if (payDrugs) {
                DrugNode *dn = payDrugs;
                while (dn) {
                    if (strcmp(dn->data.drug_id, cur->data.drug_id) == 0) {
                        snprintf(drugType, sizeof(drugType), "%s", dn->data.name);
                        break;
                    }
                    dn = dn->next;
                }
            }
            const char *items[5] = { cur->data.prescription_id, drugType, price, cur->data.prescription_date, "未缴费" };
            AddRow(hLV, row++, 5, items);
        }
    }
    free_prescription_list(rxList);
    if (payDrugs) free_drug_list(payDrugs);

    /* 2. 待缴费挂号 / Unpaid Registrations */
    AppointmentNode *appts = load_appointments_list();
    for (AppointmentNode *cur = appts; cur; cur = cur->next) {
        if (strcmp(cur->data.patient_id, pid) == 0 && !cur->data.paid && cur->data.fee > 0) {
            char price[20];
            snprintf(price, sizeof(price), "%.2f", cur->data.fee);
            const char *items[5] = { cur->data.appointment_id, "预约挂号", price, cur->data.appointment_date, "未缴费" };
            AddRow(hLV, row++, 5, items);
        }
    }
    free_appointment_list(appts);

    /* 3. 待缴费病房 / Unpaid Ward Services */
    WardCallNode *wardCalls = load_ward_calls_list();
    WardNode *wards = load_wards_list();
    for (WardCallNode *cur = wardCalls; cur; cur = cur->next) {
        if (strcmp(cur->data.patient_id, pid) == 0 && !cur->data.paid) {
            /* 未设置费用时从病房日费读取 */
            if (cur->data.fee <= 0.0f && wards) {
                WardNode *wn = wards;
                while (wn) {
                    if (strcmp(wn->data.ward_id, cur->data.ward_id) == 0) {
                        cur->data.fee = wn->data.price_per_day;
                        break;
                    }
                    wn = wn->next;
                }
            }
            if (cur->data.fee <= 0.0f) continue; /* 无法确定费用则跳过 */
            char price[20];
            snprintf(price, sizeof(price), "%.2f", cur->data.fee);
            char type[80];
            snprintf(type, sizeof(type), "病房服务");
            const char *items[5] = { cur->data.call_id, type, price, cur->data.create_time, "未缴费" };
            AddRow(hLV, row++, 5, items);
        }
    }
    free_ward_call_list(wardCalls);
    if (wards) free_ward_list(wards);

    /* 4. 待缴费其他医疗服务 / Unpaid Other Medical Services */
    OtherServiceNode *svcList = load_other_services_list();
    for (OtherServiceNode *cur = svcList; cur; cur = cur->next) {
        if (strcmp(cur->data.patient_id, pid) == 0 && !cur->data.paid) {
            char price[20];
            snprintf(price, sizeof(price), "%.2f", cur->data.total_price);
            char type[80];
            snprintf(type, sizeof(type), "%s", cur->data.service_name);
            const char *items[5] = { cur->data.service_id, type, price, cur->data.service_date, "未缴费" };
            AddRow(hLV, row++, 5, items);
        }
    }
    free_other_service_list(svcList);
}

static HWND CreatePaymentPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = PatientPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PatPayPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "PatPayPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_PATIENT_PRESCRIPTION);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 10;
    int h = (rc->bottom - rc->top) - 10;

    HWND hLV = CreateListView(hPage, 4001, 5, 5, w - 10, h - 90);
    AddCol(hLV, 0, "项目ID", 130);
    AddCol(hLV, 1, "类型", 100);
    AddCol(hLV, 2, "金额", 80);
    AddCol(hLV, 3, "日期", 100);
    AddCol(hLV, 4, "状态", 80);

    CreateWindowA("BUTTON", "立即缴费",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        w - 105, h - 40, 100, 30, hPage, (HMENU)4002, g_hInst, NULL);

    CreateWindowA("BUTTON", "使用医保报销（按药品比例）",
        WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
        5, h - 40, 180, 20, hPage, (HMENU)4003, g_hInst, NULL);

    PopulatePaymentList(hLV);
    return hPage;
}

/* ─── 住院信息页面 / Ward Information & Call Page ────────────────────── */

#define IDC_WARD_CALL_BTN    2101
#define IDC_WARD_CALL_LV     2102
#define IDC_WARD_INFO_LV     2103

/* 呼叫护士对话框 / Nurse Call Dialog */
static int g_wardCallResult = 0;

static LRESULT CALLBACK WardCallDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        int y = 15;

        CreateWindowA("STATIC", "呼叫原因:",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            20, y + 2, 80, 20, hDlg, NULL, g_hInst, NULL);
        CreateWindowA("EDIT", "",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_MULTILINE | WS_VSCROLL,
            110, y, 250, 60, hDlg, (HMENU)102, g_hInst, NULL);
        y += 75;

        CreateWindowA("STATIC", "请输入呼叫原因，护士将尽快响应。",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            20, y, 300, 20, hDlg, NULL, g_hInst, NULL);
        y += 30;

        CreateWindowA("BUTTON", "呼叫护士", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            80, y, 100, 30, hDlg, (HMENU)IDOK, g_hInst, NULL);
        CreateWindowA("BUTTON", "取消", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            200, y, 100, 30, hDlg, (HMENU)IDCANCEL, g_hInst, NULL);
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDOK) {
            char message[200] = {0};
            GetDlgItemTextA(hDlg, 102, message, sizeof(message));

            if (message[0] == 0) {
                MessageBoxA(hDlg, "请输入呼叫原因", "提示", MB_OK);
                return 0;
            }

            /* 确定患者所在病房：从患者历史呼叫记录中查找最近一次使用的病房 */
            char wardId[MAX_ID] = "";
            WardCallNode *oldCalls = load_ward_calls_list();
            if (oldCalls) {
                WardCallNode *cur = oldCalls;
                while (cur) {
                    if (strcmp(cur->data.patient_id, GetPatientId()) == 0
                        && cur->data.ward_id[0]) {
                        strcpy(wardId, cur->data.ward_id);
                        break;
                    }
                    cur = cur->next;
                }
                free_ward_call_list(oldCalls);
            }

            char deptId[MAX_ID] = "";
            DepartmentNode *depts = load_departments_list();
            if (depts) {
                strcpy(deptId, depts->data.department_id);
                free_department_list(depts);
            }

            WardCall call;
            memset(&call, 0, sizeof(call));
            generate_id(call.call_id, sizeof(call.call_id), "WC");
            strcpy(call.ward_id, wardId);
            strcpy(call.department_id, deptId);
            strcpy(call.patient_id, GetPatientId());
            strcpy(call.message, message);
            strcpy(call.status, "待响应");
            get_current_time(call.create_time, sizeof(call.create_time));
            call.paid = 0;
            /* 从病房信息中获取日费用 */
            {
                WardNode *wards = load_wards_list();
                if (wards) {
                    WardNode *wn = wards;
                    while (wn) {
                        if (wardId[0] && strcmp(wn->data.ward_id, wardId) == 0) {
                            call.fee = wn->data.price_per_day;
                            break;
                        }
                        wn = wn->next;
                    }
                    free_ward_list(wards);
                }
            }

            WardCallNode *calls = load_ward_calls_list();
            WardCallNode *node = create_ward_call_node(&call);
            node->next = calls;
            save_ward_calls_list(node);
            free_ward_call_list(node);

            append_log(g_currentUser.username, "呼叫护士", "ward_call", call.call_id, message);
            g_wardCallResult = 1;
            DestroyWindow(hDlg);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            g_wardCallResult = 0;
            DestroyWindow(hDlg);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        g_wardCallResult = 0;
        DestroyWindow(hDlg);
        return 0;
    default:
        return DefWindowProcA(hDlg, msg, wParam, lParam);
    }
}

static int ShowWardCallDialog(HWND hParent) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WardCallDlgProc;
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PatientWardCallDlg";
    RegisterClassA(&wc);

    HWND hDlg = CreateWindowExA(0, "PatientWardCallDlg", "呼叫护士",
        WS_VISIBLE | WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 220,
        hParent, NULL, g_hInst, NULL);

    RECT pr, rc;
    GetWindowRect(hParent, &pr);
    GetWindowRect(hDlg, &rc);
    SetWindowPos(hDlg, NULL,
        pr.left + (pr.right - pr.left - (rc.right - rc.left)) / 2,
        pr.top + (pr.bottom - pr.top - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(hParent, FALSE);
    g_wardCallResult = -1;
    MSG msg;
    while (g_wardCallResult == -1 && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
    return g_wardCallResult == 1;
}

/* 病房页面窗口过程 / Ward Page WndProc */
static LRESULT CALLBACK WardPatientPageWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lParam), h = HIWORD(lParam);
        int halfH = (h - 55) / 2;
        if (halfH < 80) halfH = 80;
        HWND hCallLV  = GetDlgItem(hWnd, IDC_WARD_CALL_LV);
        HWND hCallBtn = GetDlgItem(hWnd, IDC_WARD_CALL_BTN);
        HWND hInfoLV  = GetDlgItem(hWnd, IDC_WARD_INFO_LV);
        int y = 5;
        if (hCallLV)  SetWindowPos(hCallLV, NULL, 5, y, w - 10, halfH, SWP_NOZORDER);
        if (hCallBtn) SetWindowPos(hCallBtn, NULL, w - 110, halfH + 8, 100, 30, SWP_NOZORDER);
        y = halfH + 48;
        if (hInfoLV)  SetWindowPos(hInfoLV, NULL, 5, y, w - 10, h - y - 5, SWP_NOZORDER);
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDC_WARD_CALL_BTN) {
            if (ShowWardCallDialog(GetParent(hWnd))) {
                MessageBoxA(GetParent(hWnd), "呼叫已发送，护士将尽快响应", "成功", MB_OK | MB_ICONINFORMATION);
                PostMessage(GetParent(hWnd), WM_APP_REFRESH, NAV_PATIENT_WARD, 0);
            }
        }
        return 0;
    }
    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
}

static HWND CreateWardPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WardPatientPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PatientWardPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "PatientWardPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_PATIENT_WARD);
    if (!hPage) return NULL;

    int w = (rc->right - rc->left) - 10;
    int h = (rc->bottom - rc->top) - 10;
    int halfH = (h - 55) / 2;
    if (halfH < 80) halfH = 80;
    const char *pid = GetPatientId();

    /* ── 我的呼叫记录 / My Call History ── */
    CreateWindowA("STATIC", "我的呼叫记录:",
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        5, 5, 200, 20, hPage, NULL, g_hInst, NULL);

    HWND hCallLV = CreateListView(hPage, IDC_WARD_CALL_LV, 5, 22, w - 10, halfH - 22);
    AddCol(hCallLV, 0, "呼叫ID", 130);
    AddCol(hCallLV, 1, "病房", 100);
    AddCol(hCallLV, 2, "消息", 200);
    AddCol(hCallLV, 3, "状态", 60);
    AddCol(hCallLV, 4, "时间", 160);

    int callRow = 0;
    WardCallNode *calls = load_ward_calls_list();
    WardNode *patWards = load_wards_list();
    if (calls && strlen(pid) > 0) {
        WardCallNode *cur = calls;
        while (cur) {
            if (strcmp(cur->data.patient_id, pid) == 0) {
                const char *wardType = cur->data.ward_id;
                if (patWards) {
                    WardNode *w = patWards;
                    while (w) {
                        if (strcmp(w->data.ward_id, cur->data.ward_id) == 0)
                        { wardType = w->data.type; break; }
                        w = w->next;
                    }
                }
                const char *items[5] = {
                    cur->data.call_id, wardType,
                    cur->data.message, cur->data.status,
                    cur->data.create_time
                };
                AddRow(hCallLV, callRow++, 5, items);
            }
            cur = cur->next;
        }
        free_ward_call_list(calls);
    }
    if (patWards) free_ward_list(patWards);

    CreateWindowA("BUTTON", "呼叫护士",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        w - 110, halfH + 8, 100, 30,
        hPage, (HMENU)IDC_WARD_CALL_BTN, g_hInst, NULL);

    /* ── 病房信息 / Ward Info ── */
    CreateWindowA("STATIC", "病房信息:",
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        5, halfH + 48, 200, 20, hPage, NULL, g_hInst, NULL);

    int infoY = halfH + 68;
    HWND hInfoLV = CreateListView(hPage, IDC_WARD_INFO_LV, 5, infoY, w - 10,
                                  h - infoY - 5);
    AddCol(hInfoLV, 0, "病房ID", 80);
    AddCol(hInfoLV, 1, "类型", 120);
    AddCol(hInfoLV, 2, "总床位", 60);
    AddCol(hInfoLV, 3, "剩余床位", 60);

    WardNode *wards = load_wards_list();
    int row = 0;
    if (wards) {
        WardNode *cur = wards;
        while (cur) {
            char totalStr[16], remainStr[16];
            snprintf(totalStr, sizeof(totalStr), "%d", cur->data.total_beds);
            snprintf(remainStr, sizeof(remainStr), "%d", cur->data.remain_beds);
            const char *items[4] = {
                cur->data.ward_id, cur->data.type, totalStr, remainStr
            };
            AddRow(hInfoLV, row++, 4, items);
            cur = cur->next;
        }
        free_ward_list(wards);
    }
    return hPage;
}

/* ─── 治疗进度页面 / Treatment Progress Page ────────────────────────── */

static HWND CreateProgressPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = PatientPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PatientProgPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "PatientProgPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_PATIENT_PROGRESS);
    if (!hPage) return NULL;

    const char *pid = GetPatientId();
    Patient *p = NULL;
    if (strlen(pid) > 0) p = find_patient_by_id(pid);

    char info[512];
    if (p) {
        snprintf(info, sizeof(info),
            "患者: %s\n"
            "治疗阶段: %s\n"
            "紧急状态: %s\n\n"
            "下一阶段请咨询主治医生。",
            p->name,
            p->treatment_stage,
            p->is_emergency ? "是" : "否");
    } else {
        snprintf(info, sizeof(info), "未找到患者信息");
    }

    CreateWindowA("STATIC", info,
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        20, 20, 400, 200, hPage, NULL, g_hInst, NULL);

    return hPage;
}

/* ─── 修改密码页面 / Change Password Page ──────────────────────────── */

static LRESULT CALLBACK ChangePwdPageWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) != BN_CLICKED) return 0;
        if (LOWORD(wParam) == 1030) { /* 确认修改 */
            char oldPwd[100] = "", newPwd[100] = "", confirmPwd[100] = "";
            GetDlgItemTextA(hWnd, 1032, oldPwd, sizeof(oldPwd));
            GetDlgItemTextA(hWnd, 1033, newPwd, sizeof(newPwd));
            GetDlgItemTextA(hWnd, 1034, confirmPwd, sizeof(confirmPwd));

            if (oldPwd[0] == 0 || newPwd[0] == 0 || confirmPwd[0] == 0) {
                MessageBoxA(hWnd, "请填写所有密码字段", "提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (strcmp(newPwd, confirmPwd) != 0) {
                MessageBoxA(hWnd, "两次输入的新密码不一致", "错误", MB_OK | MB_ICONERROR);
                return 0;
            }

            uint8_t hashBytes[SHA256_DIGEST_SIZE];
            char hashHex[SHA256_HEX_SIZE];
            sha256_hash((const uint8_t *)oldPwd, strlen(oldPwd), hashBytes);
            sha256_hex(hashBytes, hashHex);

            if (strcmp(g_currentUser.password, hashHex) != 0) {
                MessageBoxA(hWnd, "旧密码错误", "错误", MB_OK | MB_ICONERROR);
                return 0;
            }

            /* 更新用户密码 / Update user password */
            sha256_hash((const uint8_t *)newPwd, strlen(newPwd), hashBytes);
            sha256_hex(hashBytes, hashHex);

            UserNode *users = load_users_list();
            UserNode *cur = users;
            while (cur) {
                if (strcmp(cur->data.username, g_currentUser.username) == 0 &&
                    strcmp(cur->data.role, g_currentUser.role) == 0) {
                    strcpy(cur->data.password, hashHex);
                    strcpy(g_currentUser.password, hashHex);
                    break;
                }
                cur = cur->next;
            }
            save_users_list(users);
            free_user_list(users);

            append_log(g_currentUser.username, "修改密码", "user", g_currentUser.username, "");
            MessageBoxA(hWnd, "密码修改成功", "成功", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        return 0;
    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
}

static HWND CreateChangePwdPage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = ChangePwdPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PatientChgPwdPage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "PatientChgPwdPage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_PATIENT_CHANGE_PWD);
    if (!hPage) return NULL;

    int y = 20;
    CreateWindowA("STATIC", "修改密码",
        WS_VISIBLE | WS_CHILD | SS_CENTER,
        100, y, 200, 24, hPage, NULL, g_hInst, NULL);

    y += 35;
    CreateWindowA("STATIC", "旧密码:",
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        80, y + 2, 70, 20, hPage, NULL, g_hInst, NULL);
    CreateWindowA("EDIT", "",
        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL,
        155, y, 180, 22, hPage, (HMENU)1032, g_hInst, NULL);

    y += 32;
    CreateWindowA("STATIC", "新密码:",
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        80, y + 2, 70, 20, hPage, NULL, g_hInst, NULL);
    CreateWindowA("EDIT", "",
        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL,
        155, y, 180, 22, hPage, (HMENU)1033, g_hInst, NULL);

    y += 32;
    CreateWindowA("STATIC", "确认密码:",
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        80, y + 2, 70, 20, hPage, NULL, g_hInst, NULL);
    CreateWindowA("EDIT", "",
        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL,
        155, y, 180, 22, hPage, (HMENU)1034, g_hInst, NULL);

    y += 45;
    CreateWindowA("BUTTON", "确认修改",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        160, y, 90, 30, hPage, (HMENU)1030, g_hInst, NULL);

    return hPage;
}

/* ─── 个人信息页面 / Profile Page ───────────────────────────────────── */

static HWND CreateProfilePage(HWND hParent, RECT *rc) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = PatientPageWndProc;
    wc.hInstance     = g_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PatientProfilePage";
    RegisterClassA(&wc);

    HWND hPage = CreateWindowExA(0, "PatientProfilePage", "",
        WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        hParent, NULL, g_hInst, (LPVOID)(INT_PTR)NAV_PATIENT_PROFILE);
    if (!hPage) return NULL;

    const char *pid = GetPatientId();
    Patient *p = (strlen(pid) > 0) ? find_patient_by_id(pid) : NULL;

    int y = 20;
    char buf[256];
    if (p) {
        snprintf(buf, sizeof(buf), "姓名: %s", p->name);
        CreateWindowA("STATIC", buf, WS_VISIBLE | WS_CHILD | SS_LEFT,
                      20, y, 300, 20, hPage, NULL, g_hInst, NULL);
        y += 28;

        snprintf(buf, sizeof(buf), "性别: %s", p->gender);
        CreateWindowA("STATIC", buf, WS_VISIBLE | WS_CHILD | SS_LEFT,
                      20, y, 300, 20, hPage, NULL, g_hInst, NULL);
        y += 28;

        snprintf(buf, sizeof(buf), "年龄: %d", p->age);
        CreateWindowA("STATIC", buf, WS_VISIBLE | WS_CHILD | SS_LEFT,
                      20, y, 300, 20, hPage, NULL, g_hInst, NULL);
        y += 28;

        snprintf(buf, sizeof(buf), "电话: %s", p->phone);
        CreateWindowA("STATIC", buf, WS_VISIBLE | WS_CHILD | SS_LEFT,
                      20, y, 300, 20, hPage, NULL, g_hInst, NULL);
        y += 28;

        snprintf(buf, sizeof(buf), "地址: %s", p->address);
        CreateWindowA("STATIC", buf, WS_VISIBLE | WS_CHILD | SS_LEFT,
                      20, y, 500, 20, hPage, NULL, g_hInst, NULL);
        y += 28;

        snprintf(buf, sizeof(buf), "患者类型: %s", p->patient_type);
        CreateWindowA("STATIC", buf, WS_VISIBLE | WS_CHILD | SS_LEFT,
                      20, y, 300, 20, hPage, NULL, g_hInst, NULL);
        y += 28;

        snprintf(buf, sizeof(buf), "治疗阶段: %s", p->treatment_stage);
        CreateWindowA("STATIC", buf, WS_VISIBLE | WS_CHILD | SS_LEFT,
                      20, y, 300, 20, hPage, NULL, g_hInst, NULL);
        y += 35;

        CreateWindowA("BUTTON", "编辑资料",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            20, y, 90, 30, hPage, (HMENU)1020, g_hInst, NULL);
    } else {
        CreateWindowA("STATIC", "未找到患者信息",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            20, 20, 200, 20, hPage, NULL, g_hInst, NULL);
    }
    return hPage;
}

/* ─── 公开接口 / Public Interface ───────────────────────────────────── */

/* CreatePatientPage — 工厂函数, 按 viewId 路由到各页面创建函数
   Factory function routing viewId to the appropriate page creator */

HWND CreatePatientPage(HWND hParent, int viewId, RECT *rc) {
    switch (viewId) {
    case NAV_PATIENT_REGISTER:     return CreateRegisterPage(hParent, rc);
    case NAV_PATIENT_APPOINTMENT:  return CreateAppointmentPage(hParent, rc);
    case NAV_PATIENT_DIAGNOSIS:    return CreateDiagnosisPage(hParent, rc);
    case NAV_PATIENT_PRESCRIPTION: return CreatePaymentPage(hParent, rc);
    case NAV_PATIENT_WARD:        return CreateWardPage(hParent, rc);
    case NAV_PATIENT_PROGRESS:     return CreateProgressPage(hParent, rc);
    case NAV_PATIENT_PROFILE:      return CreateProfilePage(hParent, rc);
    case NAV_PATIENT_CHANGE_PWD:  return CreateChangePwdPage(hParent, rc);
    default: return NULL;
    }
}
