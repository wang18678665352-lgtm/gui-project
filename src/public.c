/*
 * public.c — 公共业务逻辑函数实现 / Public business logic implementation
 *
 * 实现跨模块共享的业务工具函数:
 *   - 日期解析与患者类型系数
 *   - 输入验证 (数字/日期/唯一ID/电话/年龄)
 *   - 药品与病房库存预警 (计数 + 输出)
 *   - 医保/军人报销计算 (带药品报销比率上限约束)
 *   - 治疗阶段推进 (初诊→检查→治疗→康复→出院)
 *   - 医生推荐 (同科室最低 busy_level)
 *   - 7 天内重复处方风险检测
 *   - 病历归档
 *
 * Implements shared business utilities: date parsing, patient type multipliers,
 * input validation, inventory warnings, reimbursement calculation with drug ratio
 * caps, treatment stage progression, doctor recommendation, duplicate prescription
 * detection (7-day window), and medical record archiving.
 */

#include "public.h"
#include "ui_utils.h"
#include <ctype.h>

/* ==================  内部辅助函数 / Internal Helpers ================== */

/* 解析日期字符串 "YYYY-MM-DD" → struct tm (仅设置年月日字段)
   返回 1 成功, 0 失败 (格式错误或范围越界)
   Parse date string "YYYY-MM-DD" → struct tm (year/month/day only).
   Returns 1 on success, 0 on failure. */
static int parse_date_value(const char *date_text, struct tm *date_value) {
    int year = 0;
    int month = 0;
    int day = 0;

    if (!date_text || !date_value) {
        return 0;
    }

    memset(date_value, 0, sizeof(*date_value));
    if (sscanf(date_text, "%d-%d-%d", &year, &month, &day) != 3) {
        return 0;
    }
    /* 基础范围校验 / Basic range validation */
    if (year < 1900 || month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }

    date_value->tm_year = year - 1900;
    date_value->tm_mon = month - 1;
    date_value->tm_mday = day;
    date_value->tm_hour = 0;
    date_value->tm_min = 0;
    date_value->tm_sec = 0;
    return 1;
}

/* 获取患者类型的报销系数乘数:
     普通 = 0.00 (无报销)
     医保 = 1.00 (基础比率 60%)
     军人 = 1.15 (基础比率 80%, 额外优待 15%)
   此系数乘以药品的 reimbursement_ratio 得到最终报销比率
   Get patient type multiplier for reimbursement:
     "普通" = 0.00 (no reimbursement)
     "医保" = 1.00 (base ratio 60%)
     "军人" = 1.15 (base ratio 80%, additional 15% benefit) */
static float get_patient_type_multiplier(const char *patient_type) {
    if (!patient_type) {
        return 0.0f;
    }
    if (strcmp(patient_type, "医保") == 0) {
        return 1.0f;
    }
    if (strcmp(patient_type, "军人") == 0) {
        return 1.15f;
    }
    return 0.0f;
}

/* ==================  输入验证 / Input Validation ================== */

/* 通用输入验证: type=1 数字, type=2 日期, 其他检查非空
   Generic input validation: type 1=number, 2=date, other=non-empty */
bool validate_input(const char *input, int type) {
    if (!input || input[0] == '\0') {
        return false;
    }

    switch (type) {
        case 1:
            return is_valid_number(input);
        case 2:
            return is_valid_date(input);
        default:
            return strlen(input) > 0;
    }
}

/* 检查字符串是否为合法数字 (整数或最多一个小数点的浮点数)
   Check if string is a valid number (integer or float with at most one dot) */
bool is_valid_number(const char *str) {
    int dot_count = 0;

    if (!str || *str == '\0') {
        return false;
    }

    while (*str) {
        if (*str == '.') {
            dot_count++;
            if (dot_count > 1) {     /* 超过一个小数点 → 非法 / more than one dot → invalid */
                return false;
            }
        } else if (!isdigit((unsigned char)*str)) {
            return false;
        }
        str++;
    }
    return true;
}

/* 检查日期字符串格式 / Check date string format */
bool is_valid_date(const char *date) {
    struct tm parsed_date;
    return parse_date_value(date, &parsed_date) != 0;
}

/* 检查 ID 在指定实体类型中是否唯一
   Check if ID is unique within the given entity type (patient/doctor/drug) */
bool is_unique_id(const char *id, const char *id_type) {
    if (!id || !id_type) {
        return false;
    }

    /* 患者 ID 唯一性 / Patient ID uniqueness */
    if (strcmp(id_type, "patient") == 0) {
        PatientNode *head = load_patients_list();
        PatientNode *current = head;
        while (current) {
            if (strcmp(current->data.patient_id, id) == 0) {
                free_patient_list(head);
                return false;
            }
            current = current->next;
        }
        free_patient_list(head);
        return true;
    }

    /* 医生 ID 唯一性 / Doctor ID uniqueness */
    if (strcmp(id_type, "doctor") == 0) {
        DoctorNode *head = load_doctors_list();
        DoctorNode *current = head;
        while (current) {
            if (strcmp(current->data.doctor_id, id) == 0) {
                free_doctor_list(head);
                return false;
            }
            current = current->next;
        }
        free_doctor_list(head);
        return true;
    }

    /* 药品 ID 唯一性 / Drug ID uniqueness */
    if (strcmp(id_type, "drug") == 0) {
        DrugNode *head = load_drugs_list();
        DrugNode *current = head;
        while (current) {
            if (strcmp(current->data.drug_id, id) == 0) {
                free_drug_list(head);
                return false;
            }
            current = current->next;
        }
        free_drug_list(head);
        return true;
    }

    return true;   /* 未知类型默认通过 / unknown type → pass */
}

/* ==================  输入校验辅助函数 / Validation Helpers ================== */

/* 验证电话号码: 纯数字, 7-15 位
   Validate phone number: digits only, 7-15 chars */
bool is_valid_phone(const char *s) {
    size_t len;
    if (!s || s[0] == '\0') return false;
    len = strlen(s);
    if (len < 7 || len > 15) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

/* 验证年龄: 0-150 / Validate age: 0-150 */
bool is_valid_age(int age) {
    return age >= 0 && age <= 150;
}

/* ==================  库存预警 / Inventory Warnings ================== */

/* 获取库存不足的药品数量 / Get count of drugs below warning line */
int get_drug_warning_count(void) {
    DrugNode *head = load_drugs_list();
    DrugNode *cur = head;
    int n = 0;
    while (cur) {
        if (cur->data.stock_num <= cur->data.warning_line) n++;
        cur = cur->next;
    }
    free_drug_list(head);
    return n;
}

/* 获取床位紧张的病房数量 / Get count of wards below warning line */
int get_ward_warning_count(void) {
    WardNode *head = load_wards_list();
    WardNode *cur = head;
    int n = 0;
    while (cur) {
        if (cur->data.remain_beds <= cur->data.warning_line) n++;
        cur = cur->next;
    }
    free_ward_list(head);
    return n;
}

/* 输出告警横幅: 在菜单标题下方显示药品/病房告警摘要
   Display warning banner beneath menu header with drug/ward warning summary */
void show_warning_banner(void) {
    int dc = get_drug_warning_count();
    int wc = get_ward_warning_count();
    if (dc == 0 && wc == 0) return;    /* 无告警则跳过 / skip if no warnings */

    printf("  " C_BOLD C_YELLOW "\xe2\x9a\xa0 " C_RESET);  /* ⚠ 符号 */
    if (dc > 0) printf(C_YELLOW "%d \xe7\xa7\x8d\xe8\x8d\xaf\xe5\x93\x81\xe5\xba\x93\xe5\xad\x98\xe4\xb8\x8d\xe8\xb6\xb3" C_RESET, dc);  /* X种药品库存不足 */
    if (dc > 0 && wc > 0) printf(C_YELLOW ", " C_RESET);
    if (wc > 0) printf(C_YELLOW "%d \xe9\x97\xb4\xe7\x97\x85\xe6\x88\xbf\xe5\xba\x8a\xe4\xbd\x8d\xe7\xb4\xa7\xe5\xbc\xa0" C_RESET, wc);  /* X间病房床位紧张 */
    printf("\n");
}

/* 详细药品库存预警列表 / Detailed drug warning list */
void check_drug_warning(void) {
    DrugNode *head = load_drugs_list();
    DrugNode *current = head;
    int found = 0;

    printf("\n========== 药品库存预警 ==========\n");
    while (current) {
        if (current->data.stock_num <= current->data.warning_line) {
            printf("药品 %s(%s) 库存不足: 当前 %d, 预警阈值 %d\n",
                   current->data.name,
                   current->data.drug_id,
                   current->data.stock_num,
                   current->data.warning_line);
            found++;
        }
        current = current->next;
    }

    if (!found) {
        printf("当前无药品库存预警。\n");
    }

    free_drug_list(head);
}

/* 详细病房床位预警列表 / Detailed ward bed warning list */
void check_ward_warning(void) {
    WardNode *head = load_wards_list();
    WardNode *current = head;
    int found = 0;

    printf("\n========== 病房床位预警 ==========\n");
    while (current) {
        if (current->data.remain_beds <= current->data.warning_line) {
            printf("病房 %s(%s) 床位紧张: 剩余 %d, 预警阈值 %d\n",
                   current->data.type,
                   current->data.ward_id,
                   current->data.remain_beds,
                   current->data.warning_line);
            found++;
        }
        current = current->next;
    }

    if (!found) {
        printf("当前无病房床位预警。\n");
    }

    free_ward_list(head);
}

/* ==================  报销计算 / Reimbursement Calculation ================== */

/* 计算处方总金额的报销金额
   报销比率: 普通 0% / 医保 60% / 军人 80%
   返回报销金额 (而非实付金额) → 实付 = total_amount - 报销金额
   Calculate reimbursement amount based on patient type.
   Returns reimbursement amount (not out-of-pocket).
   Actual payment = total_amount - reimbursement. */
float calculate_reimbursement(float total_amount, const char *patient_type) {
    float ratio;

    if (total_amount <= 0.0f) {
        return 0.0f;
    }

    if (strcmp(patient_type, "医保") == 0) {
        ratio = 0.6f;                /* 医保报销 60% */
    } else if (strcmp(patient_type, "军人") == 0) {
        ratio = 0.8f;                /* 军人报销 80% */
    } else {
        ratio = 0.0f;                /* 普通自费 / self-pay */
    }

    return total_amount * ratio;
}

/* 计算单种药品的报销后实付金额 (考虑药品报销比率上限)
   公式: 实付 = 原价×数量 - (原价×数量 × 药品报销比率 × 患者类型系数)
   上限: 药品报销比率最终 ≤ 95%, 患者类型系数: 普通=0, 医保=1.0, 军人=1.15
   Calculate out-of-pocket amount for a specific drug, respecting drug's
   reimbursement_ratio cap (max 95%) and patient type multiplier. */
float calculate_drug_reimbursement(const Drug *drug, int quantity, const char *patient_type) {
    float total_amount;
    float final_ratio;

    if (!drug || quantity <= 0) {
        return 0.0f;
    }

    total_amount = drug->price * quantity;

    /* 最终报销比率 = 药品报销比率 × 患者类型系数，上限 95% */
    final_ratio = drug->reimbursement_ratio * get_patient_type_multiplier(patient_type);
    if (final_ratio > 0.95f) {
        final_ratio = 0.95f;           /* 报销上限 / reimbursement cap */
    }
    if (final_ratio < 0.0f) {
        final_ratio = 0.0f;            /* 下限 0 / floor */
    }
    return total_amount * final_ratio;  /* 返回报销金额 (非实付) */
}

/* ==================  治疗阶段推进 / Treatment Stage Progression ================== */

/* 获取当前阶段的下一阶段
   链条: 初诊 → 检查中 → 治疗中 → 康复观察 → 已出院 (终点，不再变化)
   Get next stage in the treatment chain.
   "已出院" is terminal and returns itself. */
const char* get_next_stage(const char *current_stage) {
    if (!current_stage || strcmp(current_stage, "初诊") == 0) {
        return "检查中";
    }
    if (strcmp(current_stage, "检查中") == 0) {
        return "治疗中";
    }
    if (strcmp(current_stage, "治疗中") == 0) {
        return "康复观察";
    }
    if (strcmp(current_stage, "康复观察") == 0) {
        return "已出院";
    }
    return current_stage;  /* 已出院等终态 / terminal state */
}

/* ==================  医生推荐算法 / Doctor Recommendation ================== */

/* 查找指定科室中 busy_level 最低的医生 (返回堆分配的副本或 NULL)
   用于患者挂号时自动推荐最空闲的医生
   Find doctor with lowest busy_level in department.
   Returns heap-allocated copy (caller must free) or NULL if no doctor found. */
Doctor* find_recommended_doctor(const char *department_id) {
    DoctorNode *head = load_doctors_list();
    DoctorNode *current = head;
    DoctorNode *best = NULL;
    Doctor *result = NULL;

    while (current) {
        if (strcmp(current->data.department_id, department_id) == 0) {
            if (!best || current->data.busy_level < best->data.busy_level) {
                best = current;
            }
        }
        current = current->next;
    }

    if (best) {
        result = (Doctor *)malloc(sizeof(Doctor));
        if (result) {
            *result = best->data;    /* 深拷贝 / deep copy */
        }
    }

    free_doctor_list(head);
    return result;
}

/* 推荐医生并返回其 ID 中的序号部分 (用于旧版接口兼容)
   Recommend doctor and return the numeric part of doctor_id (legacy compat) */
int recommend_doctor(const char *department_id) {
    Doctor *doctor = find_recommended_doctor(department_id);
    int result = -1;

    if (doctor) {
        if (doctor->doctor_id[0] != '\0') {
            result = atoi(doctor->doctor_id + 1);  /* 跳过科室前缀字母取序号 */
        }
        free(doctor);
    }

    return result;
}

/* ==================  重复处方检测 / Duplicate Prescription Detection ================== */

/* 检测 7 天内同一患者是否已开具过同一药品
   遍历所有处方 → 匹配 patient_id + drug_id → 比较日期差值
   日期解析失败则保守地视为重复 (安全侧)
   Check if same drug was prescribed for same patient within last 7 days.
   Treats parse failures as duplicates (fail-safe). */
bool is_duplicate_prescription_risk(const char *patient_id, const char *drug_id) {
    PrescriptionNode *head = load_prescriptions_list();
    PrescriptionNode *current = head;
    time_t now = time(NULL);

    while (current) {
        if (strcmp(current->data.patient_id, patient_id) == 0 &&
            strcmp(current->data.drug_id, drug_id) == 0) {
            struct tm prescription_date;
            if (parse_date_value(current->data.prescription_date, &prescription_date)) {
                time_t prescribed_at = mktime(&prescription_date);
                double day_diff = difftime(now, prescribed_at) / (60.0 * 60.0 * 24.0);
                if (day_diff <= 7.0) {                         /* ≤ 7 天 → 重复风险 */
                    free_prescription_list(head);
                    return true;
                }
            } else {
                /* 日期解析失败 → 保守处理: 视为有风险 (避免遗漏)
                   Parse failure → conservative: treat as risk to avoid missing duplicates */
                free_prescription_list(head);
                return true;
            }
        }
        current = current->next;
    }

    free_prescription_list(head);
    return false;
}

/* ==================  病历管理 / Record Management ================== */

/* 归档病历: 将指定病历状态标记为 "已归档"
   支持按 record_id (字符串) 或 record_id 中的序号查找
   Archive medical record: mark status as "已归档".
   Supports lookup by full record_id string or by embedded sequence number. */
int archive_medical_record(int record_id) {
    MedicalRecordNode *head = load_medical_records_list();
    MedicalRecordNode *current = head;
    char record_key[MAX_ID];

    snprintf(record_key, sizeof(record_key), "MR%d", record_id);
    while (current) {
        /* 匹配完整 ID ("MR123") 或记录中的序号部分 */
        if (strcmp(current->data.record_id, record_key) == 0 ||
            atoi(current->data.record_id + 2) == record_id) {
            strcpy(current->data.status, "已归档");
            save_medical_records_list(head);
            free_medical_record_list(head);
            return SUCCESS;
        }
        current = current->next;
    }

    free_medical_record_list(head);
    return ERROR_NOT_FOUND;
}
