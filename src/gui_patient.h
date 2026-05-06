/*
 * gui_patient.h — GUI 患者功能页面 / GUI patient function pages
 *
 * 患者角色的 Win32 GUI 界面工厂函数。
 * 根据 viewId 创建对应的子窗口页面 (预约挂号/挂号记录/诊断记录/
 * 处方记录/病房信息/治疗进度/个人资料)。
 *
 * Win32 GUI page factory for patient role.
 * Creates the appropriate child window page based on viewId
 * (register/appointment/diagnosis/prescription/ward/progress/profile).
 */

#ifndef GUI_PATIENT_H
#define GUI_PATIENT_H

#include <windows.h>
#include "common.h"

/* 创建患者端页面: 根据 viewId 返回对应的子窗口句柄
   viewId 取值: NAV_PATIENT_REGISTER / NAV_PATIENT_APPOINTMENT /
   NAV_PATIENT_DIAGNOSIS / NAV_PATIENT_PRESCRIPTION(缴费) / NAV_PATIENT_WARD /
   NAV_PATIENT_PROGRESS / NAV_PATIENT_PROFILE
   Create patient page: returns child window handle based on viewId */
HWND CreatePatientPage(HWND hParent, int viewId, RECT *rc);

#endif /* GUI_PATIENT_H */
