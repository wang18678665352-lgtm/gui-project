/*
 * login.h — 用户登录与注册模块 / User login & registration module
 *
 * 负责系统的身份认证流程: 用户登录 (角色验证 + 密码 SHA-256 校验)、
 * 新用户注册 (用户名唯一性检查 + 密码确认 + 角色选择)、注销、权限检查、
 * 密码修改与重置、以及将旧明码密码迁移到 SHA-256 哈希的兼容逻辑。
 *
 * Handles authentication: login (role verification + SHA-256 password check),
 * registration (username uniqueness + password confirmation + role selection),
 * logout, permission checks, password change/reset, and migration of legacy
 * plaintext passwords to SHA-256 hashes.
 */

#ifndef LOGIN_H
#define LOGIN_H

#include "common.h"
#include "data_storage.h"

/* 用户登录: 交互式输入用户名/密码/角色，验证成功后填充 logged_user 结构
   返回值: SUCCESS(0) 成功, 或错误码
   Interactive login: prompts for username/password/role, fills logged_user on success.
   Returns: SUCCESS(0) or error code */
int login(User *logged_user);

/* 用户注册: 交互式输入用户名/密码/确认/角色等信息，创建新用户并写入文件
   返回值: SUCCESS(0) 成功, 或错误码
   Register new user interactively, write to users.txt. Returns SUCCESS or error code */
int register_user(User *new_user);

/* 注销当前会话: 清除 session 中的用户信息和登录状态
   Logout: clear user info and login state from session */
void logout(Session *session);

/* 权限检查: 验证用户是否拥有 required_role 指定的角色权限
   admin 拥有所有权限; doctor/patient 仅限自身角色
   Check if user has the required role. Admin has full access. */
bool has_permission(const User *user, const char *required_role);

/* 修改当前用户密码: 验证旧密码后更新为新密码 (SHA-256 哈希存储)
   Change own password: verify old password, then update with new SHA-256 hash */
int change_password(const User *user);

/* 管理员重置任意用户密码: 选择用户后直接设置新密码 (不验证旧密码)
   Admin reset any user's password without verifying old password */
int admin_reset_password(const User *admin_user);

/* 密码迁移: 扫描所有用户，将明码密码自动转换为 SHA-256 哈希
   判断依据: 如果密码字段恰好是 64 位十六进制字符则视为已哈希，否则为明码
   Migrate all plaintext passwords to SHA-256. Skips already-hashed entries
   (identified by being exactly 64 hex characters). */
int migrate_user_passwords(void);

#endif // LOGIN_H
