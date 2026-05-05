/*
 * login.c — 用户认证与密码管理 / User authentication & password management
 *
 * 实现系统身份认证的核心功能:
 *   - register_user(): 注册新用户 (角色选择 → 用户名/密码输入 → 重复检查 →
 *      SHA-256 哈希 → 保存 → 医生信息完善/患者档案创建)
 *   - login(): 用户登录 (角色选择 → 用户名/密码 → SHA-256 哈希比对 → 验证)
 *   - logout(): 注销会话 (清除 session 状态)
 *   - has_permission(): 权限检查 (admin 全权限, 其他角色自身权限)
 *   - change_password(): 自主修改密码 (验证旧密码 → 输入新密码 ×2 → 一致性检查)
 *   - admin_reset_password(): 管理员重置密码 (无需旧密码, 操作记录日志)
 *   - migrate_user_passwords(): 明码→SHA-256 迁移 (识别 64 位十六进制哈希)
 *
 * Implements authentication: registration with role-specific profile creation,
 * login with SHA-256 verification, logout, permission checks, password change
 * with old-password verification, admin password reset with audit logging,
 * and automatic plaintext-to-SHA-256 password migration.
 */

#include "login.h"
#include "data_storage.h"
#include "ui_utils.h"
#include "sha256.h"
#include <ctype.h>
#include <stdbool.h>

/* ==================  用户注册 / User Registration ================== */

/* 注册新用户:
   1. 选择角色 (管理员/医生/患者)
   2. 输入用户名 → 检查是否为空 → 检查唯一性
   3. 输入密码 ×2 → 检查一致性
   4. SHA-256 哈希密码 → 创建 UserNode → 追加到链表 → 保存到文件
   5. 医生角色: 额外收集姓名/职称 (22 种选择)/科室 → 创建医生档案
   6. 患者角色: 自动创建患者档案
   返回 SUCCESS(0) 或错误码 */
int register_user(User *new_user) {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
    char confirm_password[MAX_PASSWORD];
    char role[20];

    /* 步骤 1: 角色选择 / Role selection */
    printf("\n");
    ui_box_top("用 户 注 册");
    ui_menu_item(1, "注册管理员");
    ui_menu_item(2, "注册医生");
    ui_menu_item(3, "注册患者");
    printf(C_BOLD C_CYAN "  ║" C_RESET "                                                    " C_BOLD C_CYAN "║\n" C_RESET);
    ui_menu_track_line();
    ui_menu_exit(0, "返回");
    ui_box_bottom();

    int role_choice = get_menu_choice(0, 3);
    if (role_choice == 0) return ERROR_INVALID_INPUT;

    switch (role_choice) {
        case 1: strcpy(role, ROLE_ADMIN); break;
        case 2: strcpy(role, ROLE_DOCTOR); break;
        case 3: strcpy(role, ROLE_PATIENT); break;
        default: return ERROR_INVALID_INPUT;
    }

    /* 步骤 2: 输入用户名 / Input username */
    ui_sub_header("用户信息");
    printf(S_LABEL "  请输入用户名: " C_RESET);
    if (fgets(username, MAX_USERNAME, stdin) == NULL) return ERROR_INVALID_INPUT;
    username[strcspn(username, "\n")] = 0;

    if (strlen(username) == 0) {
        ui_err("用户名不能为空!");
        return ERROR_INVALID_INPUT;
    }

    /* 步骤 3: 输入密码 + 确认 / Input password + confirm */
    printf(S_LABEL "  请输入密码: " C_RESET);
    if (fgets(password, MAX_PASSWORD, stdin) == NULL) return ERROR_INVALID_INPUT;
    password[strcspn(password, "\n")] = 0;

    if (strlen(password) == 0) {
        ui_err("密码不能为空!");
        return ERROR_INVALID_INPUT;
    }

    printf(S_LABEL "  请再次输入密码: " C_RESET);
    if (fgets(confirm_password, MAX_PASSWORD, stdin) == NULL) return ERROR_INVALID_INPUT;
    confirm_password[strcspn(confirm_password, "\n")] = 0;

    if (strcmp(password, confirm_password) != 0) {
        ui_err("两次输入的密码不一致!");
        return ERROR_INVALID_INPUT;
    }

    /* 步骤 4: 检查用户名唯一性 / Check username uniqueness */
    UserNode *head = load_users_list();

    UserNode *current = head;
    while (current) {
        if (strcmp(current->data.username, username) == 0) {
            free_user_list(head);
            ui_err("用户名已存在!");
            return ERROR_DUPLICATE;
        }
        current = current->next;
    }

    /* 步骤 5: SHA-256 哈希密码 → 创建 User → 追加到链表 → 保存
       Hash password → create user → append to list → save to file */
    User user;
    uint8_t hash_bytes[SHA256_DIGEST_SIZE];
    char hash_hex[SHA256_HEX_SIZE];
    sha256_hash((const uint8_t*)password, strlen(password), hash_bytes);
    sha256_hex(hash_bytes, hash_hex);
    strcpy(user.username, username);
    strcpy(user.password, hash_hex);
    strcpy(user.role, role);

    UserNode *new_node = create_user_node(&user);
    if (!new_node) {
        free_user_list(head);
        ui_err("内存分配失败!");
        return ERROR_FILE_IO;
    }

    /* 追加到链表尾部 / Append to end of list */
    if (!head) {
        head = new_node;
    } else {
        current = head;
        while (current->next) {
            current = current->next;
        }
        current->next = new_node;
    }

    int result = save_users_list(head);
    free_user_list(head);

    if (result != SUCCESS) {
        ui_err("保存用户数据失败!");
        return result;
    }

    ui_ok("注册成功!");
    ui_info("用户名", username);
    ui_info("角色", role);

    /* 步骤 6: 根据角色创建对应档案 / Create profile based on role */
    if (strcmp(role, ROLE_PATIENT) == 0) {
        /* 患者: 自动创建空白患者档案 / Patient: auto-create profile */
        ensure_patient_profile(username);
    } else if (strcmp(role, ROLE_DOCTOR) == 0) {
        /* 医生: 收集姓名/职称/科室后创建完整档案
           Doctor: collect name/title/department → create full profile */
        char doctor_name[MAX_NAME];
        char doctor_title[50];
        char doctor_dept[MAX_ID];

        ui_header("医生信息完善");

        /* 输入姓名 / Input name */
        printf(S_LABEL "  请输入医生姓名: " C_RESET);
        if (fgets(doctor_name, sizeof(doctor_name), stdin) == NULL) {
            doctor_name[0] = '\0';
        } else {
            doctor_name[strcspn(doctor_name, "\n")] = 0;
        }
        if (doctor_name[0] == '\0') {
            strcpy(doctor_name, username);  /* 默认使用用户名 / default to username */
        }

        /* 选择职称 (22 种中国医生职称) / Select title from 22 Chinese medical titles */
        const char *title_options[] = {
            "主任医师", "副主任医师", "主治医师", "住院医师", "医士",
            "主任药师", "副主任药师", "主管药师", "药师",
            "主任护师", "副主任护师", "主管护师", "护师", "护士",
            "教授", "副教授", "研究员", "副研究员",
            "返聘专家", "首席专家", "医学博士", "实习医生", "医师"
        };
        int title_count = sizeof(title_options) / sizeof(title_options[0]);
        int title_sel = ui_select_list("选择职称（↑↓切换, 回车确认）", title_options, title_count);
        if (title_sel >= 0) {
            strcpy(doctor_title, title_options[title_sel]);
        } else {
            strcpy(doctor_title, "医生");
        }

        /* 选择科室 / Select department */
        doctor_dept[0] = '\0';
        DepartmentNode *dept_head = load_departments_list();
        if (dept_head) {
            int dc = count_department_list(dept_head);
            if (dc > 0) {
                /* 构建科室选择列表 / Build department selection list */
                const char **di = malloc(dc * sizeof(const char *));
                char (*db)[70] = malloc(dc * sizeof(*db));
                int idx = 0;
                DepartmentNode *dp = dept_head;
                while (dp) {
                    snprintf(db[idx], 70, "%s - %s", dp->data.department_id, dp->data.name);
                    di[idx] = db[idx]; idx++; dp = dp->next;
                }
                int sel = ui_select_list("选择科室（↑↓切换, 回车确认）", (const char **)di, dc);
                if (sel >= 0) {
                    dp = dept_head;
                    for (int j = 0; j < sel; j++) dp = dp->next;
                    strcpy(doctor_dept, dp->data.department_id);
                } else {
                    ui_warn("未选择科室，可在管理员系统中修改。");
                }
                free((void*)di); free(db);
            } else {
                ui_warn("暂无科室数据，请联系管理员添加。");
            }
        } else {
            ui_warn("暂无科室数据，请联系管理员添加。");
        }
        free_department_list(dept_head);

        /* 创建医生档案 / Create doctor profile */
        create_doctor_profile_with_details(username, doctor_name, doctor_title, doctor_dept);
    }

    if (new_user) *new_user = user;  /* 输出已创建的用户数据 / output created user */
    return SUCCESS;
}

/* ==================  用户登录 / User Login ================== */

/* 用户登录:
   1. 选择角色 (管理员/医生/患者)
   2. 输入用户名和密码
   3. 将输入的密码做 SHA-256 哈希
   4. 遍历用户列表: 匹配用户名 + 密码哈希 + 角色
   5. 匹配成功 → 填充 logged_user 并返回 SUCCESS
   返回 SUCCESS(0) 或错误码 */
int login(User *logged_user) {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
    char role[20];

    /* 步骤 1: 角色选择 / Role selection */
    printf("\n");
    ui_box_top("登 录 系 统");
    ui_menu_item(1, "管理员登录");
    ui_menu_item(2, "医生登录");
    ui_menu_item(3, "患者登录");
    printf(C_BOLD C_CYAN "  ║" C_RESET "                                                    " C_BOLD C_CYAN "║\n" C_RESET);
    ui_menu_track_line();
    ui_menu_exit(0, "返回");
    ui_box_bottom();

    int role_choice = get_menu_choice(0, 3);
    if (role_choice == 0) return ERROR_INVALID_INPUT;

    switch (role_choice) {
        case 1: strcpy(role, ROLE_ADMIN); break;
        case 2: strcpy(role, ROLE_DOCTOR); break;
        case 3: strcpy(role, ROLE_PATIENT); break;
        default: return ERROR_INVALID_INPUT;
    }

    /* 步骤 2: 输入凭证 / Input credentials */
    ui_sub_header("请输入登录信息");
    printf(S_LABEL "  用户名: " C_RESET);
    if (fgets(username, MAX_USERNAME, stdin) == NULL) return ERROR_INVALID_INPUT;
    username[strcspn(username, "\n")] = 0;

    printf(S_LABEL "  密码: " C_RESET);
    if (fgets(password, MAX_PASSWORD, stdin) == NULL) return ERROR_INVALID_INPUT;
    password[strcspn(password, "\n")] = 0;

    UserNode *head = load_users_list();

    if (!head) {
        ui_err("加载用户数据失败!");
        return ERROR_FILE_IO;
    }

    /* 步骤 3: 对输入密码做 SHA-256 哈希 (与存储的哈希比对)
       Hash the input password for comparison */
    uint8_t hash_bytes[SHA256_DIGEST_SIZE];
    char hash_hex[SHA256_HEX_SIZE];
    sha256_hash((const uint8_t*)password, strlen(password), hash_bytes);
    sha256_hex(hash_bytes, hash_hex);

    /* 步骤 4: 遍历用户列表查找匹配项 (用户名 + 密码哈希 + 角色三重匹配)
       Search user list for triple match: username + hash + role */
    UserNode *current = head;
    while (current) {
        if (strcmp(current->data.username, username) == 0 &&
            strcmp(current->data.password, hash_hex) == 0 &&
            strcmp(current->data.role, role) == 0) {
            *logged_user = current->data;    /* 复制用户数据到输出参数 */
            free_user_list(head);
            ui_ok("登录成功!");
            return SUCCESS;
        }
        current = current->next;
    }

    free_user_list(head);
    ui_err("用户名、密码或角色类型错误!");
    return ERROR_NOT_FOUND;
}

/* ==================  注销 / Logout ================== */

/* 注销当前会话: 清除登录标记和用户信息
   Logout: clear login flag and wipe user info */
void logout(Session *session) {
    if (session && session->logged_in) {
        printf(C_DIM "  ← 用户 %s 已退出登录" C_RESET "\n", session->current_user.username);
        session->logged_in = false;
        memset(&session->current_user, 0, sizeof(User));  /* 安全擦除 / secure wipe */
    }
}

/* ==================  权限检查 / Permission Check ================== */

/* 检查用户是否拥有指定角色权限
   admin 拥有所有角色权限 (超级用户)
   其他角色仅拥有自身角色权限
   Check if user has required role. Admin has universal access. */
bool has_permission(const User *user, const char *required_role) {
    if (!user || !required_role) return false;

    if (strcmp(user->role, ROLE_ADMIN) == 0) {
        return true;   /* admin 全权限 / admin has full access */
    }

    return strcmp(user->role, required_role) == 0;
}

/* ==================  修改密码 / Change Password ================== */

/* 用户自主修改密码:
   1. 验证旧密码 (SHA-256 比对)
   2. 输入新密码 ×2 → 检查一致性
   3. 更新内存中的密码哈希 → 保存到文件
   Change own password: verify old → new ×2 confirm → update file */
int change_password(const User *user) {
    char old_password[MAX_PASSWORD];
    char new_password[MAX_PASSWORD];
    char confirm_password[MAX_PASSWORD];

    ui_sub_header("修改密码");

    /* 验证旧密码 / Verify old password */
    printf(S_LABEL "  请输入原密码: " C_RESET);
    if (fgets(old_password, MAX_PASSWORD, stdin) == NULL) return ERROR_INVALID_INPUT;
    old_password[strcspn(old_password, "\n")] = 0;

    uint8_t hash_bytes[SHA256_DIGEST_SIZE];
    char hash_hex[SHA256_HEX_SIZE];
    sha256_hash((const uint8_t*)old_password, strlen(old_password), hash_bytes);
    sha256_hex(hash_bytes, hash_hex);

    if (strcmp(user->password, hash_hex) != 0) {
        ui_err("原密码错误!");
        return ERROR_PERMISSION_DENIED;
    }

    /* 输入新密码 / Input new password */
    printf(S_LABEL "  请输入新密码: " C_RESET);
    if (fgets(new_password, MAX_PASSWORD, stdin) == NULL) return ERROR_INVALID_INPUT;
    new_password[strcspn(new_password, "\n")] = 0;

    if (strlen(new_password) == 0) {
        ui_err("密码不能为空!");
        return ERROR_INVALID_INPUT;
    }

    /* 确认新密码 / Confirm new password */
    printf(S_LABEL "  请再次输入新密码: " C_RESET);
    if (fgets(confirm_password, MAX_PASSWORD, stdin) == NULL) return ERROR_INVALID_INPUT;
    confirm_password[strcspn(confirm_password, "\n")] = 0;

    if (strcmp(new_password, confirm_password) != 0) {
        ui_err("两次输入的新密码不一致!");
        return ERROR_INVALID_INPUT;
    }

    /* 查找用户记录 → 更新密码哈希 → 保存 / Find user → update hash → save */
    UserNode *head = load_users_list();
    if (!head) {
        ui_err("加载用户数据失败!");
        return ERROR_FILE_IO;
    }

    UserNode *current = head;
    while (current) {
        if (strcmp(current->data.username, user->username) == 0 &&
            strcmp(current->data.role, user->role) == 0) {
            sha256_hash((const uint8_t*)new_password, strlen(new_password), hash_bytes);
            sha256_hex(hash_bytes, hash_hex);
            strcpy(current->data.password, hash_hex);
            break;
        }
        current = current->next;
    }

    if (!current) {
        free_user_list(head);
        ui_err("用户数据异常，未找到匹配用户!");
        return ERROR_NOT_FOUND;
    }

    int result = save_users_list(head);
    free_user_list(head);

    if (result == SUCCESS) {
        ui_ok("密码修改成功!");
    } else {
        ui_err("保存用户数据失败!");
    }
    return result;
}

/* ==================  管理员重置密码 / Admin Password Reset ================== */

/* 管理员重置任意用户密码:
   1. 输入目标用户名
   2. 查找用户 (存在性检查)
   3. 输入新密码 ×2 → 一致性检查
   4. 更新密码哈希 → 保存 → 记录操作日志
   Admin resets any user's password: lookup username → new password ×2 →
   update hash → save → audit log. */
int admin_reset_password(const User *admin_user) {
    (void)admin_user;   /* admin_user 用于日志记录 / used in log */
    char target_username[MAX_USERNAME];

    ui_header("重置用户密码");

    /* 输入目标用户名 / Input target username */
    printf(S_LABEL "  请输入要重置密码的用户名: " C_RESET);
    if (fgets(target_username, MAX_USERNAME, stdin) == NULL) return ERROR_INVALID_INPUT;
    target_username[strcspn(target_username, "\n")] = 0;

    if (strlen(target_username) == 0) {
        ui_err("用户名不能为空!");
        return ERROR_INVALID_INPUT;
    }

    /* 查找目标用户 / Find target user */
    UserNode *head = load_users_list();
    if (!head) {
        ui_err("加载用户数据失败!");
        return ERROR_FILE_IO;
    }

    UserNode *target = NULL;
    UserNode *current = head;
    while (current) {
        if (strcmp(current->data.username, target_username) == 0) {
            target = current;
            break;
        }
        current = current->next;
    }

    if (!target) {
        free_user_list(head);
        char buf[256];
        snprintf(buf, sizeof(buf), "未找到用户: %s", target_username);
        ui_err(buf);
        return ERROR_NOT_FOUND;
    }

    /* 输入新密码 + 确认 / Input new password + confirm */
    char new_password[MAX_PASSWORD];
    char confirm_password[MAX_PASSWORD];

    printf(S_LABEL "  用户 %s（角色: %s）\n", target_username, target->data.role);
    printf(S_LABEL "  请输入新密码: " C_RESET);
    if (fgets(new_password, MAX_PASSWORD, stdin) == NULL) { free_user_list(head); return ERROR_INVALID_INPUT; }
    new_password[strcspn(new_password, "\n")] = 0;

    if (strlen(new_password) == 0) {
        free_user_list(head);
        ui_err("密码不能为空!");
        return ERROR_INVALID_INPUT;
    }

    printf(S_LABEL "  请再次输入新密码: " C_RESET);
    if (fgets(confirm_password, MAX_PASSWORD, stdin) == NULL) { free_user_list(head); return ERROR_INVALID_INPUT; }
    confirm_password[strcspn(confirm_password, "\n")] = 0;

    if (strcmp(new_password, confirm_password) != 0) {
        free_user_list(head);
        ui_err("两次输入的密码不一致!");
        return ERROR_INVALID_INPUT;
    }

    /* 哈希新密码 → 更新 → 保存 / Hash → update → save */
    uint8_t hash_bytes[SHA256_DIGEST_SIZE];
    char hash_hex[SHA256_HEX_SIZE];
    sha256_hash((const uint8_t*)new_password, strlen(new_password), hash_bytes);
    sha256_hex(hash_bytes, hash_hex);
    strcpy(target->data.password, hash_hex);

    int result = save_users_list(head);
    free_user_list(head);

    if (result == SUCCESS) {
        char buf[256];
        snprintf(buf, sizeof(buf), "用户 %s 的密码已重置!", target_username);
        ui_ok(buf);
        /* 记录操作日志 / Record audit log */
        append_log(admin_user->username, "重置密码", "用户", target_username, "管理员重置密码");
    } else {
        ui_err("保存用户数据失败!");
    }
    return result;
}

/* ==================  密码迁移 (明码 → SHA-256) / Password Migration ================== */

/* 将旧的明码密码 (或非 SHA-256 格式) 自动转换为 SHA-256 哈希
   识别规则:
   - 密码长度 == 64 且全部为十六进制字符 → 视为已哈希 (跳过)
   - 否则 → 视为明码 → 计算 SHA-256 → 替换
   只有实际修改时才写回文件

   Migrate plaintext passwords to SHA-256 hashes.
   Detection: if password is 64 hex characters → already hashed (skip).
   Otherwise treat as plaintext → hash → replace.
   Only writes back if any password was changed. */
int migrate_user_passwords(void) {
    UserNode *head = load_users_list();
    if (!head) return SUCCESS;   /* 无用户则无需迁移 / no users → nothing to do */
    UserNode *cur = head;
    int changed = 0;
    while (cur) {
        size_t len = strlen(cur->data.password);
        int needs_hash = 0;
        /* 长度不是 64 → 一定是明码 / not 64 chars → definitely plaintext */
        if (len != 64) {
            needs_hash = 1;
        } else {
            /* 长度为 64 → 检查是否全为十六进制字符
               Is 64 chars → verify all are hex digits */
            for (size_t i = 0; i < len; i++) {
                if (!isxdigit((unsigned char)cur->data.password[i])) {
                    needs_hash = 1;
                    break;
                }
            }
        }
        if (needs_hash) {
            /* 将当前密码字段作为明码源文本进行哈希
               Treat current password field as plaintext source and hash it */
            uint8_t hash[SHA256_DIGEST_SIZE];
            char hex[SHA256_HEX_SIZE];
            sha256_hash((const uint8_t*)cur->data.password, len, hash);
            sha256_hex(hash, hex);
            strcpy(cur->data.password, hex);
            changed = 1;
        }
        cur = cur->next;
    }
    int result = changed ? save_users_list(head) : SUCCESS;
    free_user_list(head);
    return result;
}
