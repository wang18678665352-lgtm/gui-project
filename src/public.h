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

/*
 * 通用输入验证 — 根据 type 参数对输入字符串执行不同的校验规则
 *
 * 参数:
 *   input  - 待校验的字符串指针 (可为 NULL)
 *   type   - 校验类型: 1=数字, 2=日期格式, 其他值=非空检查
 *
 * 返回: true=通过校验, false=校验失败 (含 input 为 NULL/空串的情况)
 *
 * 说明: 此函数作为统一入口，内部委托 is_valid_number / is_valid_date
 * Generic input validation based on type
 */
bool validate_input(const char *input, int type);

/*
 * 数字格式校验 — 检查字符串是否表示合法的整数或浮点数
 *
 * 参数:
 *   str - 待校验的字符串 (可为 NULL)
 *
 * 返回: true=合法数字, false=非法 (含 NULL/空串/多个小数点)
 *
 * 规则: 允许 0-9 和至多一个小数点, 不允许正负号/科学计数法
 * Check if string is a valid number (integer or float with at most one dot)
 */
bool is_valid_number(const char *str);

/*
 * 日期格式校验 — 检查字符串是否符合 YYYY-MM-DD 格式且在合法范围内
 *
 * 参数:
 *   date - 日期字符串, 格式 "YYYY-MM-DD"
 *
 * 返回: true=合法日期, false=格式错误或范围越界
 *
 * 校验: 年>=1900, 月 1-12, 日 1-31 (不校验每月具体天数如 2 月 30 日)
 * Check if string is a valid date in YYYY-MM-DD format
 */
bool is_valid_date(const char *date);

/*
 * ID 唯一性检查 — 在指定实体类型的数据文件中查找 ID 是否已存在
 *
 * 参数:
 *   id      - 待检查的 ID 字符串
 *   id_type - 实体类型: "patient" | "doctor" | "drug" | "ward" | "department"
 *
 * 返回: true=ID 唯一 (未被占用), false=ID 已存在或参数无效
 *
 * 实现: 分别加载对应实体链表，遍历比对 ID 字段
 * Check if ID is unique for the given entity type
 */
bool is_unique_id(const char *id, const char *id_type);

/*
 * 电话号码格式校验 — 检查是否为 7-15 位纯数字
 *
 * 参数:
 *   s - 电话号码字符串 (可为 NULL)
 *
 * 返回: true=合法电话号码, false=非法
 *
 * 校验: 长度 7~15, 全部为数字字符 '0'-'9'
 * Validate phone number format (7-15 digits)
 */
bool is_valid_phone(const char *s);

/*
 * 年龄范围校验 — 检查年龄是否在 0-150 之间
 *
 * 参数:
 *   age - 年龄整数值
 *
 * 返回: true=合法年龄, false=越界
 * Validate age range (0-150)
 */
bool is_valid_age(int age);

/* =======================  库存告警 / Inventory Warnings ======================= */

/*
 * 药品库存预警 — 遍历所有药品, 输出库存不足的明细
 *
 * 无参数, 无返回值
 *
 * 行为: 对每种药品检查 stock_num <= warning_line，满足则打印告警行
 *       若无预警则输出 "当前无药品库存预警"
 * Check all drug stocks: warn if any drug's stock <= its warning line
 */
void check_drug_warning(void);

/*
 * 病房床位预警 — 遍历所有病房, 输出床位紧张的明细
 *
 * 无参数, 无返回值
 *
 * 行为: 对每间病房检查 remain_beds <= warning_line，满足则打印告警行
 * Check all ward beds: warn if remaining beds <= warning line
 */
void check_ward_warning(void);

/*
 * 药品告警计数 — 统计库存不足的药品数量 (用于标题栏徽标)
 *
 * 返回: 库存 <= 警戒线的药品种数
 * Get count of drugs below warning line (for badge display)
 */
int  get_drug_warning_count(void);

/*
 * 病房告警计数 — 统计床位紧张的病房数量 (用于标题栏徽标)
 *
 * 返回: 剩余床位 <= 警戒线的病房数
 * Get count of wards below warning line (for badge display)
 */
int  get_ward_warning_count(void);

/*
 * 告警横幅输出 — 在菜单标题下方汇总显示药品+病房告警
 *
 * 无参数, 无返回值
 *
 * 行为: 如无告警则跳过; 有告警时显示 "⚠ X种药品库存不足, Y间病房床位紧张"
 * Display warning banner summarizing drug & ward warnings
 */
void show_warning_banner(void);

/* =======================  报销计算 / Reimbursement Calculation ======================= */

/*
 * 处方总金额报销计算 — 根据患者类型计算总报销金额
 *
 * 参数:
 *   total_amount - 处方总金额 (元)
 *   patient_type  - 患者类型: "普通"=0% | "医保"=60% | "军人"=80%
 *
 * 返回: 报销金额 (元), 非实付金额; 实付 = total_amount - 返回值
 *
 * 注意: 此函数返回的是报销金额, 且不涉及单种药品的报销比率上限
 * Calculate reimbursement amount based on patient type.
 * Returns reimbursement amount (not out-of-pocket).
 */
float calculate_reimbursement(float total_amount, const char *patient_type);

/*
 * 单种药品报销计算 — 结合药品报销比率上限和患者类型系数
 *
 * 参数:
 *   drug         - 药品结构体指针 (含 price, reimbursement_ratio)
 *   quantity     - 数量
 *   patient_type  - 患者类型: "普通"/"医保"/"军人"
 *
 * 返回: 报销金额 (元)
 *
 * 公式: 报销金额 = price × quantity × drug->reimbursement_ratio × patient_multiplier
 *       最终报销比率上限 95%, 下限 0%
 * Calculate reimbursement for a specific drug (respects drug's ratio cap)
 */
float calculate_drug_reimbursement(const Drug *drug, int quantity, const char *patient_type);

/* =======================  治疗进程 / Treatment Progress ======================= */

/*
 * 治疗阶段推进 — 获取当前阶段的下一阶段
 *
 * 参数:
 *   current_stage - 当前治疗阶段名 (可为 NULL, 视为 "初诊")
 *
 * 返回: 下一阶段名称的字符串指针 (静态/常量, 无需释放)
 *
 * 链条: 初诊 → 检查中 → 治疗中 → 康复观察 → 已出院 (终点, 返回自身)
 * Get next stage in treatment chain. Discharged is terminal.
 */
const char* get_next_stage(const char *current_stage);

/* =======================  医生推荐 / Doctor Recommendation ======================= */

/*
 * 医生推荐 (旧版接口) — 返回推荐医生的序号部分
 *
 * 参数:
 *   department_id - 科室 ID 字符串
 *
 * 返回: 推荐医生的 doctor_id 中序号字段 (atoi), 无医生时返回 -1
 *
 * 算法: 同科室中 busy_level 最低的医生
 * Recommend doctor with lowest busy_level in given department.
 * Returns numeric index from doctor_id, or -1.
 */
int recommend_doctor(const char *department_id);

/*
 * 查找推荐医生 (新版接口) — 返回堆分配的结构体副本
 *
 * 参数:
 *   department_id - 科室 ID 字符串
 *
 * 返回: 堆分配的 Doctor 结构体指针 (调用者负责 free), 无医生时返回 NULL
 *
 * 算法: 遍历同科室所有医生, 比较 busy_level, 取最小值
 * Find doctor with lowest busy_level in department.
 * Returns heap-allocated copy (caller must free) or NULL.
 */
Doctor* find_recommended_doctor(const char *department_id);

/*
 * 重复处方风险检测 — 检查 7 天内同一患者是否已开具过同一药品
 *
 * 参数:
 *   patient_id - 患者 ID
 *   drug_id    - 药品 ID
 *
 * 返回: true=7 天内有重复处方 (含日期解析失败时保守处理), false=无风险
 *
 * 算法: 遍历处方记录, 匹配 patient_id+drug_id, 用 difftime 计算日期差
 *       若日期解析失败, 保守地返回 true (避免遗漏风险)
 * Check if same drug was prescribed for same patient within last 7 days
 */
bool is_duplicate_prescription_risk(const char *patient_id, const char *drug_id);

/* =======================  病历管理 / Record Management ======================= */

/*
 * 病历归档 — 将指定病历的状态标记为 "已归档"
 *
 * 参数:
 *   record_id - 病历序号 (整数, 匹配 "MR<序号>" 或嵌入序号)
 *
 * 返回: SUCCESS (0) 成功, ERROR_NOT_FOUND (-2) 未找到病历
 *
 * 行为: 加载病历列表 → 查找匹配记录 → 设置 status="已归档" → 保存
 * Archive a medical record (mark as archived).
 * Returns SUCCESS or ERROR_NOT_FOUND.
 */
int archive_medical_record(int record_id);

#endif // PUBLIC_H
