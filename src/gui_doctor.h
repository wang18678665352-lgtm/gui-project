/*
 * gui_doctor.h — GUI 医生工作页面 / GUI doctor workflow pages
 *
 * 医生角色的 Win32 GUI 界面工厂函数。
 * 根据 viewId 创建对应的子窗口页面 (挂号提醒/接诊问诊/病房呼叫/
 * 急诊管理/治疗进度/诊断模板/开具处方)。
 *
 * Win32 GUI page factory for doctor role.
 * Creates the appropriate child window page based on viewId
 * (reminder/consultation/ward call/emergency/progress/template/prescribe).
 */

#ifndef GUI_DOCTOR_H
#define GUI_DOCTOR_H

#include <windows.h>
#include "common.h"

/* 创建医生端页面: 根据 viewId 返回对应的子窗口句柄
   viewId 取值: NAV_DOCTOR_REMINDER / NAV_DOCTOR_CONSULTATION /
   NAV_DOCTOR_WARD_CALL / NAV_DOCTOR_EMERGENCY / NAV_DOCTOR_PROGRESS /
   NAV_DOCTOR_TEMPLATE / NAV_DOCTOR_PRESCRIBE
   Create doctor page: returns child window handle based on viewId */
HWND CreateDoctorPage(HWND hParent, int viewId, RECT *rc);

#endif /* GUI_DOCTOR_H */
