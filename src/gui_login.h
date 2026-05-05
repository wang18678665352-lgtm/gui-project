/*
 * gui_login.h — GUI 登录/注册对话框 / GUI login & registration dialogs
 *
 * 提供 Win32 对话框形式的登录和注册界面:
 *   - 登录: 角色单选按钮 (患者/医生/管理员) + 用户名/密码编辑框
 *   - 注册: 用户名/密码/确认/角色/姓名/科室 (医生) 等字段
 * 密码使用 SHA-256 哈希存储，登录时手动计算哈希进行比对验证。
 *
 * Provides Win32 dialog-based login and registration UI.
 * Login: role radio buttons + username/password edit boxes.
 * Register: username/password/confirm/role/name/department fields.
 * Passwords are SHA-256 hashed; verification compares computed hashes.
 */

#ifndef GUI_LOGIN_H
#define GUI_LOGIN_H

#include <windows.h>
#include "common.h"

/* 显示登录对话框 (模态消息循环)
 * 参数: hInst 实例句柄, hParent 父窗口, user 输出参数
 * 返回: SUCCESS(0) 且 user 被填充，或错误码
 * Show modal login dialog. Returns SUCCESS with user filled on success. */
int ShowLoginDialog(HINSTANCE hInst, HWND hParent, User *user);

/* 显示注册对话框 (模态消息循环)
 * 返回: SUCCESS(0) 注册成功，或错误码
 * Show modal register dialog. Returns SUCCESS or error code. */
int ShowRegisterDialog(HINSTANCE hInst, HWND hParent);

/* 登录对话框过程: 处理 WM_INITDIALOG (初始化控件)/WM_COMMAND (按钮点击) 等
   Login dialog proc: handles control init and button commands */
INT_PTR CALLBACK LoginDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

/* 注册对话框过程: 处理科室下拉框联动、字段验证、用户创建等
   Register dialog proc: handles department combo, field validation, user creation */
INT_PTR CALLBACK RegisterDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

#endif /* GUI_LOGIN_H */
