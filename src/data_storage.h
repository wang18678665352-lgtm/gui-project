/*
 * data_storage.h — 数据持久化层 / Data persistence layer
 *
 * 本文件定义了系统中所有业务实体的数据结构、链表节点类型和
 * 文件持久化操作接口。数据以制表符分隔的文本文件形式存储在
 * data/ 目录下，内存中使用单向链表表示。
 *
 * 核心设计:
 *   - 每个实体类型有: struct 定义 + Node 链表节点 + 文件路径宏
 *   - 加载函数从文件读取 → 构建链表
 *   - 保存函数将链表写回文件 (含表头注释行)
 *   - 查找函数遍历链表按 ID/username 检索
 *   - 现场挂号使用队列结构 (支持急诊前插)
 *
 * Defines all business entity structs, linked-list node types, and
 * file persistence operations. Data is stored as tab-separated text
 * files under data/ and represented in memory as singly-linked lists.
 *
 * Core design:
 *   - Each entity type: struct + Node + file path macro
 *   - Load functions: read from file → build linked list
 *   - Save functions: write linked list back to file (with header line)
 *   - Find functions: traverse list by ID/username
 *   - Onsite registrations use a queue (supports emergency front-insert)
 */

#ifndef DATA_STORAGE_H
#define DATA_STORAGE_H

#include "common.h"

/* =======================  数据文件路径 / Data File Paths ======================= */
/* 所有数据存储在 data/ 子目录下，每个文件第一行为列名注释行
   All data files under data/ directory. First line of each file is a column header comment. */

#define DATA_DIR "data"
#define USERS_FILE DATA_DIR "/users.txt"                    /* 用户 (用户名/密码哈希/角色) */
#define PATIENTS_FILE DATA_DIR "/patients.txt"               /* 患者档案 */
#define DOCTORS_FILE DATA_DIR "/doctors.txt"                 /* 医生档案 */
#define DEPARTMENTS_FILE DATA_DIR "/departments.txt"         /* 科室信息 */
#define DRUGS_FILE DATA_DIR "/drugs.txt"                     /* 药品库存 */
#define WARDS_FILE DATA_DIR "/wards.txt"                     /* 病房信息 */
#define APPOINTMENTS_FILE DATA_DIR "/appointments.txt"       /* 预约挂号记录 */
#define ONSITE_REGISTRATIONS_FILE DATA_DIR "/onsite_registrations.txt"  /* 现场挂号记录 */
#define WARD_CALLS_FILE DATA_DIR "/ward_calls.txt"           /* 病房呼叫记录 */
#define MEDICAL_RECORDS_FILE DATA_DIR "/medical_records.txt" /* 病历记录 */
#define PRESCRIPTIONS_FILE DATA_DIR "/prescriptions.txt"     /* 处方记录 */
#define TEMPLATES_FILE    DATA_DIR "/templates.txt"          /* 诊断/治疗/检查快捷模板 */
#define SCHEDULES_FILE    DATA_DIR "/schedules.txt"          /* 医生排班表 */
#define LOGS_FILE         DATA_DIR "/logs.txt"               /* 系统操作日志 */

/* =======================  业务实体结构定义 / Business Entity Structs ======================= */

/* 患者 — 持久化到 patients.txt
   Patient — persisted to patients.txt */
typedef struct {
    char patient_id[MAX_ID];        /* 患者编号 (P_yyyyMMddHHmmss_序号) */
    char username[MAX_USERNAME];    /* 关联的用户名 / linked username */
    char name[MAX_NAME];            /* 姓名 / name */
    char gender[10];                /* 性别: 男/女 */
    int age;                        /* 年龄 (0-150) */
    char phone[20];                 /* 电话 (7-15 位数字) */
    char address[200];              /* 地址 */
    char patient_type[20];          /* 患者类型: 普通/医保/军人 (影响报销比例) */
    char treatment_stage[20];       /* 治疗阶段: 初诊→检查→治疗→复查→康复→出院 */
    bool is_emergency;              /* 是否急诊 (急诊患者自动创建病房呼叫) */
} Patient;

/* 医生 — 持久化到 doctors.txt
   Doctor — persisted to doctors.txt */
typedef struct {
    char doctor_id[MAX_ID];         /* 医生编号 (科室前缀字母+序号, 如 N0001) */
    char username[MAX_USERNAME];    /* 关联的用户名 / linked username */
    char name[MAX_NAME];            /* 姓名 / name */
    char department_id[MAX_ID];     /* 所属科室 ID / department ID */
    char title[50];                 /* 职称 (22 种中文职称可选) */
    int busy_level;                 /* 繁忙度 (推荐算法选最低者, 挂号+1/退号-1) */
} Doctor;

/* 科室 — 持久化到 departments.txt
   Department — persisted to departments.txt */
typedef struct {
    char department_id[MAX_ID];     /* 科室编号 (D_yyyyMMddHHmmss_序号) */
    char name[MAX_NAME];            /* 科室名称 / department name */
    char leader[MAX_NAME];          /* 科室主任姓名 / department head */
    char phone[20];                 /* 科室电话 / department phone */
} Department;

/* 药品 — 持久化到 drugs.txt
   Drug — persisted to drugs.txt */
typedef struct {
    char drug_id[MAX_ID];           /* 药品编号 (M_yyyyMMddHHmmss_序号) */
    char name[MAX_NAME];            /* 药品名称 / drug name */
    float price;                    /* 单价 / unit price */
    int stock_num;                  /* 当前库存 / current stock */
    int warning_line;               /* 库存警戒线 (<= 此值触发告警) */
    bool is_special;                /* 是否特殊药品 (管制药品等) */
    float reimbursement_ratio;      /* 报销比率上限 (如 0.95 表示最多报 95%) */
    char category[20];              /* 药品分类 / drug category */
} Drug;

/* 病房 — 持久化到 wards.txt
   Ward — persisted to wards.txt */
typedef struct {
    char ward_id[MAX_ID];           /* 病房编号 (W_yyyyMMddHHmmss_序号) */
    char type[50];                  /* 病房类型 / ward type (e.g. 普通病房/ICU) */
    int total_beds;                 /* 总床位数 / total beds */
    int remain_beds;                /* 剩余可用床位 / remaining available beds */
    int warning_line;               /* 床位警戒线 (<= 此值触发告警) */
} Ward;

/* 预约挂号 — 持久化到 appointments.txt
   Appointment — persisted to appointments.txt */
typedef struct {
    char appointment_id[MAX_ID];    /* 挂号编号 (A_yyyyMMddHHmmss_序号) */
    char patient_id[MAX_ID];        /* 患者 ID */
    char doctor_id[MAX_ID];         /* 医生 ID */
    char department_id[MAX_ID];     /* 科室 ID */
    char appointment_date[20];      /* 预约日期 (YYYY-MM-DD, 限未来 7 天) */
    char appointment_time[20];      /* 预约时段 (如 "上午(08:00-12:00)") */
    char status[20];                /* 状态: 待就诊/已就诊/已取消/已爽约 */
    char create_time[30];           /* 创建时间戳 */
    float fee;                      /* 挂号费 (元) */
    int paid;                       /* 是否已缴费 (0=未缴, 1=已缴) */
} Appointment;

/* 现场挂号 — 持久化到 onsite_registrations.txt
   Onsite Registration — persisted to onsite_registrations.txt */
typedef struct {
    char onsite_id[MAX_ID];         /* 现场挂号编号 (O_yyyyMMddHHmmss_序号) */
    char patient_id[MAX_ID];        /* 患者 ID */
    char doctor_id[MAX_ID];         /* 医生 ID */
    char department_id[MAX_ID];     /* 科室 ID */
    int queue_number;               /* 排队号码 (按医生+科室递增) */
    char status[20];                /* 状态: 排队中/已接诊/已取消 */
    char create_time[30];           /* 创建时间戳 */
} OnsiteRegistration;

/* 病房呼叫 — 持久化到 ward_calls.txt
   Ward Call — persisted to ward_calls.txt */
typedef struct {
    char call_id[MAX_ID];           /* 呼叫编号 (WC_yyyyMMddHHmmss_序号) */
    char ward_id[MAX_ID];           /* 发起呼叫的病房 ID */
    char department_id[MAX_ID];     /* 目标科室 ID */
    char patient_id[MAX_ID];        /* 相关患者 ID */
    char message[200];              /* 呼叫消息 / call message */
    char status[20];                /* 状态: 待响应/已响应/已处理 */
    char create_time[30];           /* 创建时间戳 */
} WardCall;

/* 病历 — 持久化到 medical_records.txt
   Medical Record — persisted to medical_records.txt */
typedef struct {
    char record_id[MAX_ID];         /* 病历编号 (R_yyyyMMddHHmmss_序号) */
    char patient_id[MAX_ID];        /* 患者 ID */
    char doctor_id[MAX_ID];         /* 接诊医生 ID */
    char appointment_id[MAX_ID];    /* 关联挂号 ID */
    char diagnosis[500];            /* 诊断内容 / diagnosis text */
    char diagnosis_date[20];        /* 诊断日期 (YYYY-MM-DD) */
    char status[20];                /* 状态: 初诊/复诊/已归档 */
} MedicalRecord;

/* 处方 — 持久化到 prescriptions.txt
   Prescription — persisted to prescriptions.txt */
typedef struct {
    char prescription_id[MAX_ID];   /* 处方编号 (P_yyyyMMddHHmmss_序号) */
    char record_id[MAX_ID];         /* 关联病历 ID */
    char patient_id[MAX_ID];        /* 患者 ID */
    char doctor_id[MAX_ID];         /* 开具医生 ID */
    char drug_id[MAX_ID];           /* 药品 ID */
    int quantity;                   /* 数量 / quantity */
    float total_price;              /* 总价 (单价×数量) / total price */
    char prescription_date[20];     /* 处方日期 (YYYY-MM-DD) */
} Prescription;

/* 诊断模板 — 持久化到 templates.txt
   Medical Template — persisted to templates.txt */
typedef struct {
    char template_id[MAX_ID];       /* 模板编号 (T_yyyyMMddHHmmss_序号) */
    char category[20];              /* 类别: diagnosis/treatment/exam */
    char shortcut[60];              /* 快捷调用文本 (如 "#1 感冒诊断") */
    char text[500];                 /* 模板正文 / template body text */
} MedicalTemplate;

/* 医生排班 — 持久化到 schedules.txt
   Schedule — persisted to schedules.txt */
typedef struct {
    char schedule_id[MAX_ID];       /* 排班编号 (S_yyyyMMddHHmmss_序号) */
    char doctor_id[MAX_ID];         /* 医生 ID */
    char work_date[12];             /* 工作日期 (YYYY-MM-DD) */
    char time_slot[16];             /* 时段 (如 "上午(08-12)"/"下午(14-18)") */
    int max_appt;                   /* 预约最大人数 / max appointments this slot */
    int max_onsite;                 /* 现场挂号最大人数 / max onsite patients this slot */
    char status[10];                /* 状态: 生效中/已取消 */
} Schedule;

/* 操作日志 — 持久化到 logs.txt
   Log Entry — persisted to logs.txt */
typedef struct {
    char log_id[MAX_ID];            /* 日志编号 (L_yyyyMMddHHmmss_序号) */
    char operator_name[MAX_USERNAME]; /* 操作者用户名 / operator username */
    char action[20];                /* 操作类型: 新增/修改/删除/登录/注销 */
    char target[20];                /* 操作目标实体 / target entity type */
    char target_id[MAX_ID];         /* 目标实体 ID */
    char detail[200];               /* 操作详情 / detail description */
    char create_time[30];           /* 操作时间戳 */
} LogEntry;

/* =======================  链表节点结构 / Linked List Node Types ======================= */
/* 每种实体对应一个链表节点类型，包含数据体和 next 指针。
   Each entity has a corresponding node type with data payload and next pointer. */

typedef struct UserNode {
    User data;
    struct UserNode *next;
} UserNode;

typedef struct PatientNode {
    Patient data;
    struct PatientNode *next;
} PatientNode;

typedef struct DoctorNode {
    Doctor data;
    struct DoctorNode *next;
} DoctorNode;

typedef struct DepartmentNode {
    Department data;
    struct DepartmentNode *next;
} DepartmentNode;

typedef struct DrugNode {
    Drug data;
    struct DrugNode *next;
} DrugNode;

typedef struct WardNode {
    Ward data;
    struct WardNode *next;
} WardNode;

typedef struct AppointmentNode {
    Appointment data;
    struct AppointmentNode *next;
} AppointmentNode;

typedef struct OnsiteRegistrationNode {
    OnsiteRegistration data;
    struct OnsiteRegistrationNode *next;
} OnsiteRegistrationNode;

/* 现场挂号队列 — front 用于出队, rear 用于入队, 急诊患者从 front 插入
   Onsite registration queue — front for dequeue, rear for enqueue.
   Emergency patients are inserted at the front (queue-jumping). */
typedef struct {
    OnsiteRegistrationNode *front;  /* 队首 (出队端) / front (dequeue end) */
    OnsiteRegistrationNode *rear;   /* 队尾 (入队端) / rear (enqueue end) */
    int size;                       /* 队列大小 / queue size */
} OnsiteRegistrationQueue;

typedef struct WardCallNode {
    WardCall data;
    struct WardCallNode *next;
} WardCallNode;

typedef struct ScheduleNode {
    Schedule data;
    struct ScheduleNode *next;
} ScheduleNode;

typedef struct LogEntryNode {
    LogEntry data;
    struct LogEntryNode *next;
} LogEntryNode;

typedef struct MedicalRecordNode {
    MedicalRecord data;
    struct MedicalRecordNode *next;
} MedicalRecordNode;

typedef struct PrescriptionNode {
    Prescription data;
    struct PrescriptionNode *next;
} PrescriptionNode;

typedef struct TemplateNode {
    MedicalTemplate data;
    struct TemplateNode *next;
} TemplateNode;

/* =======================  初始化 / Initialization ======================= */

/* 创建 data/ 目录 (如不存在) / Create data/ directory if not exists */
int init_data_storage(void);

/* =======================  链表操作 / Linked List Operations ======================= */
/* 每种实体有 3 个基本操作: create (分配+初始化节点)、free (释放链表)、count (节点计数) */

UserNode* create_user_node(const User *user);
void free_user_list(UserNode *head);
int count_user_list(UserNode *head);

PatientNode* create_patient_node(const Patient *patient);
void free_patient_list(PatientNode *head);
int count_patient_list(PatientNode *head);

DoctorNode* create_doctor_node(const Doctor *doctor);
void free_doctor_list(DoctorNode *head);
int count_doctor_list(DoctorNode *head);

DepartmentNode* create_department_node(const Department *department);
void free_department_list(DepartmentNode *head);
int count_department_list(DepartmentNode *head);

DrugNode* create_drug_node(const Drug *drug);
void free_drug_list(DrugNode *head);
int count_drug_list(DrugNode *head);

WardNode* create_ward_node(const Ward *ward);
void free_ward_list(WardNode *head);
int count_ward_list(WardNode *head);

AppointmentNode* create_appointment_node(const Appointment *appointment);
void free_appointment_list(AppointmentNode *head);
int count_appointment_list(AppointmentNode *head);

OnsiteRegistrationNode* create_onsite_registration_node(const OnsiteRegistration *registration);
void free_onsite_registration_list(OnsiteRegistrationNode *head);
int count_onsite_registration_list(OnsiteRegistrationNode *head);

/* 队列操作 (现场挂号专用 / onsite registration queue specific) */
void init_onsite_registration_queue(OnsiteRegistrationQueue *queue);      /* 初始化空队列 */
int enqueue_onsite_registration(OnsiteRegistrationQueue *queue,
                                const OnsiteRegistration *registration,
                                bool front);  /* front=true 时队首插入 (急诊用) */
int dequeue_onsite_registration(OnsiteRegistrationQueue *queue,
                                OnsiteRegistration *registration);        /* 出队 (接诊时使用) */
void free_onsite_registration_queue(OnsiteRegistrationQueue *queue);

WardCallNode* create_ward_call_node(const WardCall *call);
void free_ward_call_list(WardCallNode *head);
int count_ward_call_list(WardCallNode *head);

MedicalRecordNode* create_medical_record_node(const MedicalRecord *record);
void free_medical_record_list(MedicalRecordNode *head);
int count_medical_record_list(MedicalRecordNode *head);

PrescriptionNode* create_prescription_node(const Prescription *prescription);
void free_prescription_list(PrescriptionNode *head);
int count_prescription_list(PrescriptionNode *head);

/* =======================  数据加载与保存 / Data Load & Save ======================= */
/* 每个实体有 load (文件→链表) 和 save (链表→文件) 两个核心函数
   Each entity has load (file→list) and save (list→file) functions */

UserNode* load_users_list(void);
int save_users_list(UserNode *head);

PatientNode* load_patients_list(void);
int save_patients_list(PatientNode *head);

/* 患者查找 / Patient lookup */
Patient* find_patient_by_username(const char *username);   /* 按用户名查找 */
Patient* find_patient_by_id(const char *patient_id);       /* 按患者 ID 查找 */

/* 确保患者档案存在: 如果不存在则创建默认档案并写回文件
   Ensure patient profile exists; create default if missing */
int ensure_patient_profile(const char *username);

DoctorNode* load_doctors_list(void);
int save_doctors_list(DoctorNode *head);

/* 医生查找 / Doctor lookup */
Doctor* find_doctor_by_username(const char *username);     /* 按用户名查找 */
Doctor* find_doctor_by_id(const char *doctor_id);          /* 按医生 ID 查找 */

/* 确保医生档案存在 / Ensure doctor profile exists */
int ensure_doctor_profile(const char *username);

/* 批量确保医生/患者档案存在: 一次性加载数据文件，检查和追加所有缺失档案
   Batch ensure doctor/patient profiles: load data file once, check & append all missing */
void batch_ensure_doctor_profiles(UserNode *user_list);
void batch_ensure_patient_profiles(UserNode *user_list);

/* 创建医生档案 (含姓名/职称/科室) / Create doctor profile with full details */
int create_doctor_profile_with_details(const char *username, const char *name,
                                       const char *title, const char *department_id);

/* 生成医生 ID: 科室首字母 + 4 位序号 (如 N0001)
   Generate doctor ID: department initial + 4-digit sequence (e.g. N0001) */
void generate_doctor_id(const char *department_id, char *out_buf, int buf_size);

/* 迁移旧格式医生 ID 到新格式 (X0001) 并更新所有关联文件
   Migrate old-format doctor IDs to new format and update all related files */
void migrate_doctor_ids(void);

/* 更新指定医生 ID 在所有关联文件中的引用 (科室变更时调用)
   Update references to old doctor ID across all files (used when dept changes) */
void update_doctor_id_across_files(const char *old_id, const char *new_id);

DepartmentNode* load_departments_list(void);
int save_departments_list(DepartmentNode *head);

DrugNode* load_drugs_list(void);
int save_drugs_list(DrugNode *head);
Drug* find_drug_by_id(const char *drug_id);                /* 按药品 ID 查找 */

WardNode* load_wards_list(void);
int save_wards_list(WardNode *head);

AppointmentNode* load_appointments_list(void);
int save_appointments_list(AppointmentNode *head);

/* 查找指定患者/医生的所有挂号记录
   Find all appointments for a specific patient or doctor */
Appointment* find_appointments_by_patient(const char *patient_id);
Appointment* find_appointments_by_doctor(const char *doctor_id);

/* 现场挂号队列: 返回整个队列副本 (从文件加载)
   Load onsite registration queue from file (returns full queue copy) */
OnsiteRegistrationQueue load_onsite_registration_queue(void);
int save_onsite_registration_queue(const OnsiteRegistrationQueue *queue);

/* 获取指定医生+科室的下一个排队号码 / Get next queue number for doctor+dept */
int get_next_onsite_queue_number(const char *doctor_id, const char *department_id);

WardCallNode* load_ward_calls_list(void);
int save_ward_calls_list(WardCallNode *head);

MedicalRecordNode* load_medical_records_list(void);
int save_medical_records_list(MedicalRecordNode *head);

/* 按患者 ID 查找病历 / Find medical records by patient ID */
MedicalRecord* find_records_by_patient(const char *patient_id);

PrescriptionNode* load_prescriptions_list(void);
int save_prescriptions_list(PrescriptionNode *head);

/* =======================  排班操作 / Schedule Operations ======================= */

ScheduleNode* create_schedule_node(const Schedule *schedule);
void free_schedule_list(ScheduleNode *head);
int count_schedule_list(ScheduleNode *head);
ScheduleNode* load_schedules_list(void);
int save_schedules_list(ScheduleNode *head);

/* 检查医生在指定日期是否已有排班 / Check if doctor already has schedule on date */
int has_doctor_schedule(const char *doctor_id, const char *date);

/* =======================  日志操作 / Log Operations ======================= */

LogEntryNode* create_log_entry_node(const LogEntry *entry);
void free_log_entry_list(LogEntryNode *head);
int count_log_entry_list(LogEntryNode *head);
LogEntryNode* load_logs_list(void);

/* 追加一条操作日志 (自动生成 ID 和时间戳)
   Append an operation log entry (auto-generates ID and timestamp) */
int append_log(const char *operator_name, const char *action, const char *target,
               const char *target_id, const char *detail);

/* =======================  数据备份与恢复 / Data Backup & Restore ======================= */

/* 备份所有数据到 data/backup_YYYYMMDD_HHMMSS/ 目录
   自动删除超过 10 个的旧备份
   Backup all data to timestamped directory; auto-cleanup oldest if >10 backups */
int backup_data(void);

/* 从指定备份目录恢复数据 (覆盖当前 data/ 文件)
   Restore data from specified backup directory (overwrites current data/) */
int restore_data(const char *backup_dir);

/* 列出所有备份目录名 (返回动态分配的字符串数组)
   List all backup directory names (returns dynamically allocated string array) */
int list_backups(const char ***out_names, int *out_count);

/* 释放 list_backups 返回的字符串数组 / Free the string array from list_backups */
void free_backups_list(const char **names, int count);

/* =======================  模板操作 / Template Operations ======================= */

TemplateNode* load_templates_list(void);
int save_templates_list(TemplateNode *head);
TemplateNode* create_template_node(const MedicalTemplate *tmpl);
void free_template_list(TemplateNode *head);

/* 确保默认模板存在: 初始化 18 个预设诊断/治疗/检查模板
   Ensure default templates exist: initialize 18 preset templates */
int ensure_default_templates(void);

#endif // DATA_STORAGE_H
