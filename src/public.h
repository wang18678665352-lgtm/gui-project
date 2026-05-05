/*
 * public.h — 公共业务逻辑函数 / Public business logic functions
 *
 * 跨模块共享的业务工具函数，包括:
 *   - 输入验证 (数字/日期/唯一ID/电话/年龄)
 *   - 药品与病房库存警戒检测 (低于警戒线时输出告警横幅)
 *   - 医保/军人报销比例计算 (医保 60%, 军人 80%, 药品比率上限 95%)
 *   - 治疗阶段推进链 (初诊→检查→治疗→复查→康复→出院)
 *   - 医生推荐算法 (同科室内选 busy_level 最低者)
 *   - 重复处方风险检测 (7 天内同患者同药品)
 *   - 病历归档
 *
 * Shared business utilities: input validation, inventory warning checks,
 * reimbursement calculation, treatment stage progression, doctor recommendation,
 * duplicate prescription detection, and medical record archiving.
 */

#ifndef PUBLIC_H
#define PUBLIC_H

#include "common.h"
#include "data_storage.h"

/* =======================  输入校验 / Input Validation ======================= */

/* 通用输入验证: type 指定校验规则 (如 number/date/id 等)
   Generic input validation based on type */
bool validate_input(const char *input, int type);

/* 是否为合法数字 (整数或浮点数) / Check if string is a valid number */
bool is_valid_number(const char *str);

/* 是否为合法日期 (YYYY-MM-DD 格式, 含月份/天数范围校验)
   Check if string is a valid date in YYYY-MM-DD format */
bool is_valid_date(const char *date);

/* 检查 ID 是否唯一: id_type 指定实体类型 (patient/doctor/drug/ward/department)
   Check if ID is unique for the given entity type */
bool is_unique_id(const char *id, const char *id_type);

/* 验证电话号码格式 (7-15 位数字) / Validate phone number format (7-15 digits) */
bool is_valid_phone(const char *s);

/* 验证年龄范围 (0-150) / Validate age range (0-150) */
bool is_valid_age(int age);

/* =======================  库存告警 / Inventory Warnings ======================= */

/* 检查所有药品库存: 当前库存 <= 警戒线则输出告警
   Check all drug stocks: warn if any drug's stock <= its warning line */
void check_drug_warning(void);

/* 检查所有病房床位: 剩余床位 <= 警戒线则输出告警
   Check all ward beds: warn if remaining beds <= warning line */
void check_ward_warning(void);

/* 获取库存告警数量 (用于在标题栏显示告警徽标)
   Get count of items below warning line (for badge display) */
int  get_drug_warning_count(void);
int  get_ward_warning_count(void);

/* 输出告警横幅: 汇总药品+病房告警信息
   Display warning banner summarizing drug & ward warnings */
void show_warning_banner(void);

/* =======================  报销计算 / Reimbursement Calculation ======================= */

/* 计算处方总金额的报销后实际支付金额
   patient_type: "普通" → 无报销, "医保" → 60%, "军人" → 80%
   报销比率受药品 reimbursement_ratio 上限约束 (如药品上限 95% 则最多报 95%)
   Calculate actual payment after reimbursement based on patient type.
   Reimbursement is capped by the drug's reimbursement_ratio. */
float calculate_reimbursement(float total_amount, const char *patient_type);

/* 计算单种药品的报销后金额 (结合药品报销比率上限)
   Calculate reimbursement for a specific drug (respects drug's ratio cap) */
float calculate_drug_reimbursement(const Drug *drug, int quantity, const char *patient_type);

/* =======================  治疗进程 / Treatment Progress ======================= */

/* 获取当前治疗阶段的下一阶段
   链条: 初诊 → 检查 → 治疗 → 复查 → 康复 → 出院 (终点)
   Get next stage in treatment chain. Discharged is terminal. */
const char* get_next_stage(const char *current_stage);

/* =======================  医生推荐 / Doctor Recommendation ======================= */

/* 推荐指定科室中 busy_level 最低的医生 (返回医生索引或 SUCCESS/ERROR)
   Recommend doctor with lowest busy_level in given department */
int recommend_doctor(const char *department_id);

/* 返回推荐医生的指针 (或 NULL) / Return pointer to recommended doctor (or NULL) */
Doctor* find_recommended_doctor(const char *department_id);

/* 检测 7 天内同一患者是否已开具过同一药品 (防重复用药)
   Check if same drug was prescribed for same patient within last 7 days */
bool is_duplicate_prescription_risk(const char *patient_id, const char *drug_id);

/* =======================  病历管理 / Record Management ======================= */

/* 归档病历 (标记为已归档状态) / Archive a medical record (mark as archived) */
int archive_medical_record(int record_id);

#endif // PUBLIC_H
