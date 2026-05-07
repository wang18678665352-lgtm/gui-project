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

/*
 * 解析日期字符串 "YYYY-MM-DD" → struct tm (仅设置年月日字段)
 *
 * 参数:
 *   date_text  - 输入日期字符串, 格式 "YYYY-MM-DD"
 *   date_value - 输出 struct tm 指针, 解析成功时填入年月日
 *
 * 返回: 1 成功, 0 失败 (格式错误、范围越界或 NULL 参数)
 *
 * 说明:
 *   - 使用 sscanf 提取年/月/日三个整数
 *   - 校验范围: 年>=1900, 月 1-12, 日 1-31
 *   - struct tm 中 tm_year = 年份 - 1900, tm_mon = 月份 - 1
 *   - 此函数不校验每月具体天数, 也不设置时分秒
 *
 * Parse date string "YYYY-MM-DD" → struct tm (year/month/day only).
 * Returns 1 on success, 0 on failure.
 */
static int parse_date_value(const char *date_text, struct tm *date_value) {
    int year = 0;
    int month = 0;
    int day = 0;

    /* 空指针保护 / NULL guard */
    if (!date_text || !date_value) {
        return 0;
    }

    memset(date_value, 0, sizeof(*date_value));
    if (sscanf(date_text, "%d-%d-%d", &year, &month, &day) != 3) {
        return 0;                         /* 格式不符合 YYYY-MM-DD */
    }
    /* 基础范围校验 / Basic range validation */
    if (year < 1900 || month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }

    /* 转换为 struct tm 约定: 年从 1900 起算, 月从 0 起算 */
    date_value->tm_year = year - 1900;
    date_value->tm_mon = month - 1;
    date_value->tm_mday = day;
    date_value->tm_hour = 0;
    date_value->tm_min = 0;
    date_value->tm_sec = 0;
    return 1;
}

/*
 * 获取患者类型的报销系数乘数
 *
 * 参数:
 *   patient_type - 患者类型字符串: "普通" / "医保" / "军人" (可为 NULL)
 *
 * 返回: float 乘数
 *   普通 = 0.00 (无报销)
 *   医保 = 1.00 (基础比率 60% × 1.0 = 60%)
 *   军人 = 1.15 (基础比率 80% × 1.15 ≈ 92%, 额外 15% 优待)
 *
 * 说明: 此系数与药品的 reimbursement_ratio 相乘得到最终报销比率
 *       如果 patient_type 为 NULL 或未识别, 返回 0 (无报销)
 *
 * Get patient type multiplier for reimbursement:
 *   "普通" = 0.00 (no reimbursement)
 *   "医保" = 1.00 (base ratio 60%)
 *   "军人" = 1.15 (base ratio 80%, additional 15% benefit)
 */
static float get_patient_type_multiplier(const char *patient_type) {
    if (!patient_type) {
        return 0.0f;
    }
    if (strcmp(patient_type, "医保") == 0) {
        return 1.0f;
    }
    if (strcmp(patient_type, "军人") == 0) {
        return 1.15f;                     /* 军人额外 15% 优待 */
    }
    return 0.0f;                          /* 普通患者无报销 */
}

/* ==================  输入验证 / Input Validation ================== */

/*
 * 通用输入验证 — 根据 type 参数委托到具体校验函数
 *
 * 参数:
 *   input - 待校验的字符串 (可为 NULL/空串)
 *   type  - 校验类型标识: 1=数字, 2=日期, 其他=非空检查
 *
 * 返回: true=通过, false=失败
 *
 * 实现:
 *   - 首先检查 input 是否为 NULL 或空串, 快速失败
 *   - type=1 委托 is_valid_number
 *   - type=2 委托 is_valid_date
 *   - 其他: 仅检查长度 > 0
 *
 * Generic input validation: type 1=number, 2=date, other=non-empty
 */
bool validate_input(const char *input, int type) {
    /* 快速失败: NULL 或空串直接拒绝 / Fast-fail on NULL or empty */
    if (!input || input[0] == '\0') {
        return false;
    }

    switch (type) {
        case 1:
            return is_valid_number(input);
        case 2:
            return is_valid_date(input);
        default:
            return strlen(input) > 0;     /* 其他类型仅要求非空 */
    }
}

/*
 * 数字格式校验 — 检查字符串是否为合法整数或浮点数
 *
 * 参数:
 *   str - 待校验字符串 (可为 NULL/空串)
 *
 * 返回: true=合法数字, false=非法
 *
 * 规则:
 *   - 允许字符: '0'-'9' 和 '.' (小数点)
 *   - 小数点最多一个 (超过一个即非法)
 *   - 不允许正负号、科学计数法 (e/E)、空白字符
 *
 * Check if string is a valid number (integer or float with at most one dot)
 */
bool is_valid_number(const char *str) {
    int dot_count = 0;                    /* 小数点计数 */

    if (!str || *str == '\0') {
        return false;
    }

    /* 逐字符扫描 / Scan character by character */
    while (*str) {
        if (*str == '.') {
            dot_count++;
            if (dot_count > 1) {          /* 超过一个小数点 → 非法 / more than one dot → invalid */
                return false;
            }
        } else if (!isdigit((unsigned char)*str)) {
            /* 非数字且非小数点 → 非法 (含字母/符号/空白等) */
            return false;
        }
        str++;
    }
    return true;
}

/*
 * 日期格式校验 — 委托 parse_date_value 完成解析和范围校验
 *
 * 参数:
 *   date - 日期字符串 "YYYY-MM-DD"
 *
 * 返回: true=合法, false=非法
 *
 * Check date string format by delegating to parse_date_value
 */
bool is_valid_date(const char *date) {
    struct tm parsed_date;
    return parse_date_value(date, &parsed_date) != 0;
}

/*
 * ID 唯一性检查 — 在指定实体类型中查找 ID 是否已被占用
 *
 * 参数:
 *   id      - 待检查的 ID 字符串
 *   id_type - 实体类型标识: "patient" | "doctor" | "drug"
 *
 * 返回: true=ID 唯一, false=ID 已存在或参数无效
 *
 * 实现:
 *   根据 id_type 加载对应实体链表 → 遍历比对 ID → 找到匹配则返回 false
 *   每次加载后使用对应的 free 函数释放链表内存
 *   未知 id_type 默认返回 true (宽松策略)
 *
 * Check if ID is unique within the given entity type (patient/doctor/drug)
 */
bool is_unique_id(const char *id, const char *id_type) {
    /* 空指针保护 / NULL guard */
    if (!id || !id_type) {
        return false;
    }

    /* 患者 ID 唯一性 — 遍历患者链表比对 patient_id / Patient ID uniqueness */
    if (strcmp(id_type, "patient") == 0) {
        PatientNode *head = load_patients_list();
        PatientNode *current = head;
        while (current) {
            if (strcmp(current->data.patient_id, id) == 0) {
                free_patient_list(head);
                return false;             /* ID 冲突 */
            }
            current = current->next;
        }
        free_patient_list(head);
        return true;
    }

    /* 医生 ID 唯一性 — 遍历医生链表比对 doctor_id / Doctor ID uniqueness */
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

    /* 药品 ID 唯一性 — 遍历药品链表比对 drug_id / Drug ID uniqueness */
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

    return true;                          /* 未知类型默认通过 / unknown type → pass */
}

/* ==================  输入校验辅助函数 / Validation Helpers ================== */

/*
 * 电话号码格式校验 — 检查是否为 7-15 位纯数字字符串
 *
 * 参数:
 *   s - 电话号码字符串 (可为 NULL/空串)
 *
 * 返回: true=合法, false=非法 (含 NULL/空串/长度不符/含非数字字符)
 *
 * Validate phone number: digits only, 7-15 chars
 */
bool is_valid_phone(const char *s) {
    size_t len;
    /* 空指针和空串快速拒绝 */
    if (!s || s[0] == '\0') return false;
    len = strlen(s);
    /* 长度范围校验: 7-15 位 */
    if (len < 7 || len > 15) return false;
    /* 逐字符检查是否为数字 */
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

/*
 * 年龄范围校验 — 检查是否在 0-150 之间
 *
 * 参数:
 *   age - 年龄整数值
 *
 * 返回: true=合法, false=越界
 *
 * Validate age: 0-150
 */
bool is_valid_age(int age) {
    return age >= 0 && age <= 150;
}

/* ==================  库存预警 / Inventory Warnings ================== */

/*
 * 药品告警计数 — 统计库存 <= 警戒线的药品种数
 *
 * 返回: 预警药品数量 (int)
 *
 * 实现: 加载药品链表 → 遍历检查 stock_num <= warning_line → 计数 → 释放链表
 *
 * Get count of drugs below warning line
 */
int get_drug_warning_count(void) {
    DrugNode *head = load_drugs_list();
    DrugNode *cur = head;
    int n = 0;
    while (cur) {
        if (cur->data.stock_num <= cur->data.warning_line) n++;
        cur = cur->next;
    }
    free_drug_list(head);                 /* 释放链表避免内存泄漏 */
    return n;
}

/*
 * 病房告警计数 — 统计床位 <= 警戒线的病房数量
 *
 * 返回: 预警病房数量 (int)
 *
 * 实现: 加载病房链表 → 遍历检查 remain_beds <= warning_line → 计数 → 释放
 *
 * Get count of wards below warning line
 */
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

/*
 * 告警横幅输出 — 在菜单标题下方显示告警摘要
 *
 * 无参数, 无返回值
 *
 * 行为:
 *   1. 调用 get_drug_warning_count / get_ward_warning_count 获取告警数
 *   2. 如果两者均为 0, 直接返回 (无告警)
 *   3. 否则输出: "⚠ X种药品库存不足, Y间病房床位紧张" (黄色文字)
 *
 * Display warning banner beneath menu header with drug/ward warning summary
 */
void show_warning_banner(void) {
    int dc = get_drug_warning_count();
    int wc = get_ward_warning_count();
    if (dc == 0 && wc == 0) return;       /* 无告警则跳过 / skip if no warnings */

    printf("  " C_BOLD C_YELLOW "\xe2\x9a\xa0 " C_RESET);  /* ⚠ 符号 */
    if (dc > 0) printf(C_YELLOW "%d \xe7\xa7\x8d\xe8\x8d\xaf\xe5\x93\x81\xe5\xba\x93\xe5\xad\x98\xe4\xb8\x8d\xe8\xb6\xb3" C_RESET, dc);  /* X种药品库存不足 */
    if (dc > 0 && wc > 0) printf(C_YELLOW ", " C_RESET);   /* 药品和病房之间用逗号分隔 */
    if (wc > 0) printf(C_YELLOW "%d \xe9\x97\xb4\xe7\x97\x85\xe6\x88\xbf\xe5\xba\x8a\xe4\xbd\x8d\xe7\xb4\xa7\xe5\xbc\xa0" C_RESET, wc);  /* X间病房床位紧张 */
    printf("\n");
}

/*
 * 药品库存预警详情 — 输出每种库存不足药品的具体信息
 *
 * 无参数, 无返回值
 *
 * 输出格式: 药品 名称(ID) 库存不足: 当前 N, 预警阈值 M
 * 无预警时输出 "当前无药品库存预警"
 *
 * Detailed drug warning list
 */
void check_drug_warning(void) {
    DrugNode *head = load_drugs_list();
    DrugNode *current = head;
    int found = 0;                        /* 找到预警计数 */

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

/*
 * 病房床位预警详情 — 输出每间床位紧张病房的具体信息
 *
 * 无参数, 无返回值
 *
 * 输出格式: 病房 类型(ID) 床位紧张: 剩余 N, 预警阈值 M
 * 无预警时输出 "当前无病房床位预警"
 *
 * Detailed ward bed warning list
 */
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

/*
 * 处方总金额报销计算 — 根据患者类型返回应报销金额
 *
 * 参数:
 *   total_amount - 处方总金额 (元), <=0 时返回 0
 *   patient_type  - 患者类型字符串
 *
 * 返回: 应报销金额 (元)
 *
 * 报销比率:
 *   普通 = 0% (全部自费)
 *   医保 = 60% (报销 total_amount × 0.6)
 *   军人 = 80% (报销 total_amount × 0.8)
 *
 * 注意: 返回的是报销金额, 实付 = total_amount - 返回值
 *       此函数不涉及单种药品的 reimbursement_ratio 上限
 *
 * Calculate reimbursement amount based on patient type.
 * Returns reimbursement amount (not out-of-pocket).
 * Actual payment = total_amount - reimbursement.
 */
float calculate_reimbursement(float total_amount, const char *patient_type) {
    float ratio;

    /* 金额 <= 0 直接返回 0 */
    if (total_amount <= 0.0f) {
        return 0.0f;
    }

    /* 根据患者类型确定报销比率 */
    if (strcmp(patient_type, "医保") == 0) {
        ratio = 0.6f;                     /* 医保报销 60% */
    } else if (strcmp(patient_type, "军人") == 0) {
        ratio = 0.8f;                     /* 军人报销 80% */
    } else {
        ratio = 0.0f;                     /* 普通自费 / self-pay */
    }

    return total_amount * ratio;
}

/*
 * 单种药品报销金额计算 — 结合药品报销比率上限和患者类型系数
 *
 * 参数:
 *   drug         - 药品指针 (含 price, reimbursement_ratio), 可为 NULL
 *   quantity     - 数量, <=0 时返回 0
 *   patient_type  - 患者类型: "普通"/"医保"/"军人"
 *
 * 返回: 报销金额 (元)
 *
 * 计算步骤:
 *   1. total_amount = price × quantity
 *   2. final_ratio = drug->reimbursement_ratio × patient_multiplier
 *      其中 patient_multiplier: 普通=0, 医保=1.0, 军人=1.15
 *   3. 上限裁剪: final_ratio > 95% → 0.95, final_ratio < 0 → 0
 *   4. 报销金额 = total_amount × final_ratio
 *
 * 上限: 药品报销比率最终 ≤ 95%, 患者类型系数: 普通=0, 医保=1.0, 军人=1.15
 *
 * Calculate out-of-pocket amount for a specific drug, respecting drug's
 * reimbursement_ratio cap (max 95%) and patient type multiplier.
 */
float calculate_drug_reimbursement(const Drug *drug, int quantity, const char *patient_type) {
    float total_amount;
    float final_ratio;

    /* 空指针或数量无效 → 无报销 */
    if (!drug || quantity <= 0) {
        return 0.0f;
    }

    total_amount = drug->price * quantity;

    /* 最终报销比率 = 药品报销比率 × 患者类型系数，上限 95% */
    final_ratio = drug->reimbursement_ratio * get_patient_type_multiplier(patient_type);
    if (final_ratio > 0.95f) {
        final_ratio = 0.95f;              /* 报销上限 / reimbursement cap */
    }
    if (final_ratio < 0.0f) {
        final_ratio = 0.0f;               /* 下限 0 / floor */
    }
    return total_amount * final_ratio;     /* 返回报销金额 (非实付) */
}

/* ==================  治疗阶段推进 / Treatment Stage Progression ================== */

/*
 * 治疗阶段推进 — 获取当前阶段的下一阶段
 *
 * 参数:
 *   current_stage - 当前阶段名, 可为 NULL (视为 "初诊")
 *
 * 返回: 下一阶段名称的常量字符串指针 (无需释放)
 *
 * 治疗链条:
 *   初诊 → 检查中 → 治疗中 → 康复观察 → 已出院 (终点)
 *   NULL 或未识别的阶段名也会被映射到 "检查中"
 *   "已出院" 作为终态返回自身 (不再变化)
 *
 * Get next stage in the treatment chain.
 * "已出院" is terminal and returns itself.
 */
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
    return current_stage;                 /* 已出院等终态 / terminal state */
}

/* ==================  医生推荐算法 / Doctor Recommendation ================== */

/*
 * 查找推荐医生 — 在指定科室中选择 busy_level 最低的医生
 *
 * 参数:
 *   department_id - 科室 ID 字符串
 *
 * 返回: 堆分配的 Doctor 结构体副本 (调用者负责 free), 无医生时返回 NULL
 *
 * 算法:
 *   1. 加载所有医生链表
 *   2. 遍历筛选同科室 (department_id 匹配) 的医生
 *   3. 比较 busy_level, 保留最小值对应的医生
 *   4. malloc 拷贝该医生的数据 → 释放链表 → 返回副本
 *
 * Find doctor with lowest busy_level in department.
 * Returns heap-allocated copy (caller must free) or NULL if no doctor found.
 */
Doctor* find_recommended_doctor(const char *department_id) {
    DoctorNode *head = load_doctors_list();
    DoctorNode *current = head;
    DoctorNode *best = NULL;              /* 当前最佳医生节点 */
    Doctor *result = NULL;

    /* 遍历链表, 筛选同科室中 busy_level 最低者 */
    while (current) {
        if (strcmp(current->data.department_id, department_id) == 0) {
            if (!best || current->data.busy_level < best->data.busy_level) {
                best = current;           /* 发现更空闲的医生 */
            }
        }
        current = current->next;
    }

    /* 如果找到, 分配堆内存并深拷贝 */
    if (best) {
        result = (Doctor *)malloc(sizeof(Doctor));
        if (result) {
            *result = best->data;         /* 深拷贝 / deep copy */
        }
    }

    free_doctor_list(head);
    return result;
}

/*
 * 医生推荐 (旧版兼容接口) — 返回推荐医生的序号
 *
 * 参数:
 *   department_id - 科室 ID 字符串
 *
 * 返回: 推荐医生的 doctor_id 中序号 (int), 无医生时返回 -1
 *
 * 实现:
 *   调用 find_recommended_doctor → 提取 doctor_id[1:] → atoi 转整数 → free 副本
 *   doctor_id 格式通常为 "D<序号>", 跳过首字母 'D' 取序号部分
 *
 * Recommend doctor and return the numeric part of doctor_id (legacy compat)
 */
int recommend_doctor(const char *department_id) {
    Doctor *doctor = find_recommended_doctor(department_id);
    int result = -1;

    if (doctor) {
        if (doctor->doctor_id[0] != '\0') {
            result = atoi(doctor->doctor_id + 1);  /* 跳过科室前缀字母取序号 */
        }
        free(doctor);                     /* 释放 find_recommended_doctor 分配的副本 */
    }

    return result;
}

/* ==================  重复处方检测 / Duplicate Prescription Detection ================== */

/*
 * 重复处方风险检测 — 检查 7 天内同一患者是否已开具过同一药品
 *
 * 参数:
 *   patient_id - 患者 ID
 *   drug_id    - 药品 ID
 *
 * 返回: true=7 天内有重复风险, false=无风险
 *
 * 算法:
 *   1. 加载所有处方记录链表
 *   2. 遍历, 匹配 patient_id 且 drug_id
 *   3. 解析处方日期为 struct tm, 用 mktime 转 time_t
 *   4. difftime(now, prescribed_at) 计算天数差
 *   5. 天数差 ≤ 7 → 返回 true
 *   6. 日期解析失败 → 保守处理: 视为有风险 (安全侧, 避免遗漏)
 *
 * 安全策略: 宁可误报也不漏报 — 日期解析失败时保守返回 true
 *
 * Check if same drug was prescribed for same patient within last 7 days.
 * Treats parse failures as duplicates (fail-safe).
 */
bool is_duplicate_prescription_risk(const char *patient_id, const char *drug_id) {
    PrescriptionNode *head = load_prescriptions_list();
    PrescriptionNode *current = head;
    time_t now = time(NULL);              /* 当前时间 (Unix 时间戳) */

    while (current) {
        /* 匹配患者 ID + 药品 ID / Match patient_id and drug_id */
        if (strcmp(current->data.patient_id, patient_id) == 0 &&
            strcmp(current->data.drug_id, drug_id) == 0) {
            struct tm prescription_date;
            if (parse_date_value(current->data.prescription_date, &prescription_date)) {
                time_t prescribed_at = mktime(&prescription_date);  /* 处方日期 → Unix 时间戳 */
                /* 计算天数差值 (单位: 天) / Compute day difference */
                double day_diff = difftime(now, prescribed_at) / (60.0 * 60.0 * 24.0);
                if (day_diff <= 7.0) {    /* ≤ 7 天 → 重复风险 */
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

/*
 * 病历归档 — 将指定病历状态标记为 "已归档"
 *
 * 参数:
 *   record_id - 病历序号 (整数)
 *
 * 返回: SUCCESS (0) 成功归档, ERROR_NOT_FOUND (-2) 未找到对应病历
 *
 * 匹配逻辑:
 *   1. 构造完整 record_id: "MR<序号>" (例如 MR123)
 *   2. 遍历病历链表, 匹配两种方式:
 *      a. record_id == "MR123" (完整字符串匹配)
 *      b. atoi(record_id+2) == record_id (跳过 "MR" 前缀后数值匹配)
 *   3. 匹配成功 → 设置 status="已归档" → 保存 → 返回 SUCCESS
 *   4. 遍历结束仍未找到 → 返回 ERROR_NOT_FOUND
 *
 * Archive medical record: mark status as "已归档".
 * Supports lookup by full record_id string or by embedded sequence number.
 */
int archive_medical_record(int record_id) {
    MedicalRecordNode *head = load_medical_records_list();
    MedicalRecordNode *current = head;
    char record_key[MAX_ID];

    /* 构造完整病历 ID: "MR<序号>" */
    snprintf(record_key, sizeof(record_key), "MR%d", record_id);
    while (current) {
        /* 匹配完整 ID ("MR123") 或记录中的序号部分 */
        if (strcmp(current->data.record_id, record_key) == 0 ||
            atoi(current->data.record_id + 2) == record_id) {
            strcpy(current->data.status, "已归档");  /* 更新状态 */
            save_medical_records_list(head);          /* 持久化保存 */
            free_medical_record_list(head);
            return SUCCESS;
        }
        current = current->next;
    }

    free_medical_record_list(head);
    return ERROR_NOT_FOUND;
}
