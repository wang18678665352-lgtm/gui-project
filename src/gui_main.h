/*
 * gui_main.h — Win32 GUI 主窗口定义 / Win32 GUI main window definitions
 *
 * 定义了 GUI 模式下的主窗口布局常量、导航项 ID 和枚举值、
 * 窗口过程函数声明以及全局变量。
 *
 * 窗口布局: 左侧导航树 (200px) + 右侧内容区 (剩余宽度)
 *          + 底部状态栏 (25px) + 右上角注销按钮
 *
 * 导航系统: 每个角色 (患者/医生/管理员) 有独立的导航枚举，
 *           通过 WM_COMMAND 消息的 wParam 低字节传递导航 ID。
 *
 * Defines GUI main window layout constants, navigation item IDs & enums,
 * window procedure declarations, and global variables.
 *
 * Layout: left nav tree (200px) + right content area (fill)
 *          + bottom status bar (25px) + top-right logout button
 *
 * Navigation: each role (patient/doctor/admin) has its own nav enum.
 * Navigation IDs are passed via WM_COMMAND wParam low word.
 */

#ifndef GUI_MAIN_H
#define GUI_MAIN_H

#include <windows.h>
#include <commctrl.h>
#include "common.h"

/* MinGW 兼容: 旧版 commctrl.h 缺少的 ListView 扩展样式定义
   MinGW compatibility: define missing extended ListView styles */
#ifndef LVS_EX_ALTERNATINGROWCOLORS
#define LVS_EX_ALTERNATINGROWCOLORS 0x00000040  /* 交替行颜色 / alternating row colors */
#endif
#ifndef LVS_EX_DOUBLEBUFFER
#define LVS_EX_DOUBLEBUFFER 0x00010000           /* 双缓冲绘制 (防闪烁) / double-buffered painting */
#endif

/* =======================  窗口尺寸 / Window Dimensions ======================= */
#define WINDOW_WIDTH  1024       /* 默认窗口宽度 / default window width */
#define WINDOW_HEIGHT 768        /* 默认窗口高度 / default window height */
#define WINDOW_MIN_WIDTH  800    /* 最小宽度 (OnSize 中约束) / minimum width */
#define WINDOW_MIN_HEIGHT 600    /* 最小高度 (OnSize 中约束) / minimum height */
#define NAV_WIDTH     200        /* 左侧导航树宽度 / left nav tree width */
#define STATUS_HEIGHT 25         /* 底部状态栏高度 / bottom status bar height */

/* =======================  导航项 ID 基础值 / Navigation ID Base Values ======================= */
/* 每个角色的导航 ID = 角色基础值 + 偏移。通过 wParam 低 16 位传递。
   Each role's nav ID = base + offset. Passed via wParam low word. */
#define NAV_BASE          1000   /* 通用 / shared */
#define NAV_PATIENT_BASE  2000   /* 患者角色 / patient role */
#define NAV_DOCTOR_BASE   3000   /* 医生角色 / doctor role */
#define NAV_ADMIN_BASE    4000   /* 管理员角色 / admin role */

/* =======================  患者导航项 / Patient Navigation Items ======================= */
enum {
    NAV_PATIENT_REGISTER = NAV_PATIENT_BASE + 1,    /* 预约挂号 / book appointment */
    NAV_PATIENT_APPOINTMENT,                         /* 挂号记录 / appointment records */
    NAV_PATIENT_DIAGNOSIS,                           /* 诊断记录 / diagnosis records */
    NAV_PATIENT_PRESCRIPTION,                        /* 缴费 / payment */
    NAV_PATIENT_WARD,                                /* 病房信息 / ward info */
    NAV_PATIENT_PROGRESS,                            /* 治疗进度 / treatment progress */
    NAV_PATIENT_PROFILE,                             /* 个人资料 / profile */
    NAV_PATIENT_CHANGE_PWD,                          /* 修改密码 / change password */
};

/* =======================  医生导航项 / Doctor Navigation Items ======================= */
enum {
    NAV_DOCTOR_REMINDER = NAV_DOCTOR_BASE + 1,       /* 挂号提醒 / appointment reminder */
    NAV_DOCTOR_CONSULTATION,                          /* 接诊问诊 / consultation */
    NAV_DOCTOR_WARD_CALL,                             /* 病房呼叫 / ward calls */
    NAV_DOCTOR_EMERGENCY,                             /* 急诊管理 / emergency management */
    NAV_DOCTOR_PROGRESS,                              /* 治疗进度 / treatment progress */
    NAV_DOCTOR_TEMPLATE,                              /* 诊断模板 / diagnosis templates */
    NAV_DOCTOR_PRESCRIBE,                             /* 开具处方 / prescribe */
    NAV_DOCTOR_CHANGE_PWD,                            /* 修改密码 / change password */
};

/* =======================  管理员导航项 / Admin Navigation Items ======================= */
enum {
    NAV_ADMIN_DEPT = NAV_ADMIN_BASE + 1,              /* 科室管理 / department management */
    NAV_ADMIN_DOCTOR,                                  /* 医生管理 / doctor management */
    NAV_ADMIN_PATIENT,                                 /* 患者管理 / patient management */
    NAV_ADMIN_DRUG,                                    /* 药品管理 / drug management */
    NAV_ADMIN_WARD,                                    /* 病房管理 / ward management */
    NAV_ADMIN_SCHEDULE,                                /* 排班管理 / schedule management */
    NAV_ADMIN_LOG,                                     /* 日志查看 / log viewing */
    NAV_ADMIN_DATA,                                    /* 数据管理 / data management */
    NAV_ADMIN_ANALYSIS,                                /* 统计分析 / analysis */
    NAV_ADMIN_RESETPWD,                                /* 密码重置 / password reset */
};

/* =======================  主窗口函数 / Main Window Functions ======================= */

/* 创建主窗口并进入消息循环 / Create main window and enter message loop */
int RunMainWindow(HINSTANCE hInstance);

/* 主窗口过程: 处理 WM_CREATE/WM_SIZE/WM_NOTIFY(TreeView 选择变更)/
   WM_COMMAND(注销按钮)/WM_APP_REFRESH 等消息
   Main window proc: handles creation, sizing, tree selection, logout, refresh */
LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* 切换右侧内容区的显示: 销毁旧视图 → 创建新视图 (根据 viewId 选择角色+页面)
   Switch right-side content view: destroy old → create new based on viewId */
void SwitchView(HWND hWnd, int viewId);

/* 获取右侧内容区子窗口句柄 / Get content area child window handle */
HWND GetContentView(HWND hWnd);

/* 设置底部状态栏文本 / Set bottom status bar text */
void SetStatusText(HWND hWnd, const char *text);

/* 自定义刷新消息: 各子页面通过 PostMessage(hWnd, WM_APP_REFRESH, viewId, 0)
   通知主窗口刷新当前视图 (常用于 CRUD 操作后自动刷新列表)
   Custom refresh message: child pages post this to trigger view refresh
   after CRUD operations (e.g. re-populate list after add/edit/delete) */
#define WM_APP_REFRESH (WM_APP + 1)

/* 全局实例句柄和当前用户 (各模块通过 extern 访问)
   Global instance handle and current user (accessible by all modules) */
extern HINSTANCE g_hInst;
extern User g_currentUser;

#endif /* GUI_MAIN_H */
