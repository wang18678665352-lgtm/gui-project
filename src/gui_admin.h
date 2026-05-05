/*
 * gui_admin.h — GUI 管理员管理页面 / GUI admin management pages
 *
 * 管理员角色的 Win32 GUI 界面工厂函数。
 * 根据 viewId 创建对应的子窗口页面 (科室/医生/患者/药品/病房管理、
 * 排班管理、日志查看、数据备份恢复、统计分析、密码重置)。
 *
 * Win32 GUI page factory for admin role.
 * Creates the appropriate child window page based on viewId
 * (department/doctor/patient/drug/ward/schedule management,
 * log viewing, data backup/restore, analysis, password reset).
 */

#ifndef GUI_ADMIN_H
#define GUI_ADMIN_H

#include <windows.h>
#include "common.h"

/* 创建管理员端页面: 根据 viewId 返回对应的子窗口句柄
   viewId 取值: NAV_ADMIN_DEPT / NAV_ADMIN_DOCTOR / NAV_ADMIN_PATIENT /
   NAV_ADMIN_DRUG / NAV_ADMIN_WARD / NAV_ADMIN_SCHEDULE / NAV_ADMIN_LOG /
   NAV_ADMIN_DATA / NAV_ADMIN_ANALYSIS / NAV_ADMIN_RESETPWD
   Create admin page: returns child window handle based on viewId */
HWND CreateAdminPage(HWND hParent, int viewId, RECT *rc);

#endif /* GUI_ADMIN_H */
