/*
 * ============================================================================
 * data_storage.c — 医院管理系统数据持久化层 (Data Persistence Layer)
 * ============================================================================
 *
 * [中文]
 * 本文件是医院管理系统的数据持久化核心，采用 C99 标准实现。所有数据以
 * 制表符分隔的纯文本文件（.txt）存储在 data/ 目录下。文件格式特点：
 *   - # 开头的行为注释/列标题行，读取时自动跳过
 *   - 空白行自动跳过
 *   - 字段间使用 Tab (\t) 分隔
 *   - 字段值中如果包含 Tab、换行等特殊字符，使用 C 风格转义（\\, \t, \n, \r）
 *     存储和读取时分别调用 escape_field / unescape_field_inplace 处理
 *
 * 数据组织方式：
 *   - 运行时使用单向链表（Singly Linked List）在内存中管理实体
 *   - 每种实体各有 create/free/count 三个链表操作函数
 *   - 每种实体各有 load/save 两个文件读写函数，带列标题行
 *   - 现场挂号（OnsiteRegistration）使用队列（Queue）结构，支持紧急插队
 *
 * 支持的实体类型（共14种）：
 *   User, Patient, Doctor, Department, Drug, Ward, Appointment,
 *   OnsiteRegistration, WardCall, MedicalRecord, Prescription,
 *   Schedule, LogEntry, MedicalTemplate
 *
 * 额外功能：
 *   - 医生 ID 生成：科室字母前缀 + 4位序号（如 A0001）
 *   - 医生 ID 迁移：将旧版长 ID 转换为新版短格式，同步更新所有关联文件
 *   - 操作日志：追加式记录所有增删改操作
 *   - 数据备份/还原：时间戳命名备份目录，超过限制自动清理最旧备份
 *
 * [English]
 * This file is the data persistence core of a hospital management system,
 * implemented in C99. All data is stored as tab-delimited plain text files
 * (.txt) under the data/ directory. File format characteristics:
 *   - Lines starting with # are comments/column headers, skipped during load
 *   - Blank lines are automatically skipped
 *   - Fields are separated by Tab (\t)
 *   - Fields containing Tab, newline, or other special characters use C-style
 *     escaping (\\, \t, \n, \r) via escape_field / unescape_field_inplace
 *
 * Data organization:
 *   - Singly linked lists are used for in-memory entity management at runtime
 *   - Each entity type has create/free/count linked-list helper functions
 *   - Each entity type has load/save file I/O functions with column header lines
 *   - OnsiteRegistration uses a Queue structure, supporting emergency front-insert
 *
 * Supported entity types (14 total):
 *   User, Patient, Doctor, Department, Drug, Ward, Appointment,
 *   OnsiteRegistration, WardCall, MedicalRecord, Prescription,
 *   Schedule, LogEntry, MedicalTemplate
 *
 * Additional features:
 *   - Doctor ID generation: department letter prefix + 4-digit sequence (e.g., A0001)
 *   - Doctor ID migration: converts old long IDs to new short format, updating all
 *     cross-referenced files
 *   - Operation log: append-only recording of all CRUD operations
 *   - Data backup/restore: timestamped backup directories with auto-cleanup
 *     of oldest backups when exceeding the limit
 * ============================================================================
 */

#include "data_storage.h"
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* 文本行最大缓冲区大小 / Maximum text line buffer size */
#define TEXT_LINE_SIZE 4096

/* ==========================================================================
 * 第一部分：基础文件 I/O 辅助函数
 * Part 1: Basic File I/O Helper Functions
 * ========================================================================== */

/*
 * [中文] 从文件中读取一行有效数据（跳过空白行和 # 注释行）
 * [English] Read one valid data line from file (skip blank lines and # comments)
 *
 * 工作流程 / Workflow:
 *   1. fgets 读取一行
 *   2. 去除行尾的 \n 和 \r
 *   3. 如果是空白行或以 # 开头 → 跳过，继续读下一行
 *   4. 返回 1 表示成功读取，0 表示 EOF
 */
static int read_data_line(FILE *fp, char *buffer, size_t size) {
    while (fgets(buffer, (int)size, fp)) {
        size_t len = strlen(buffer);
        /* 去除行尾换行符 / Strip trailing newline characters */
        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
            buffer[--len] = '\0';
        }
        /* 跳过空白行和 # 注释行 / Skip blank lines and # comment lines */
        if (buffer[0] == '\0' || buffer[0] == '#') {
            continue;
        }
        return 1;
    }
    return 0;
}

/*
 * [中文] 制表符分隔的字段解析器 — 每次调用返回下一个 Tab 分隔的字段
 * [English] Tab-delimited token parser — each call returns the next tab-separated field
 *
 * 工作机制 / Mechanism:
 *   - 在 *cursor 指向的字符串中查找下一个 \t
 *   - 将 \t 替换为 \0（字符串终止符），返回当前字段起始地址
 *   - 更新 *cursor 指向下一个字段的起始位置
 *   - 如果没有更多 \t，则将 *cursor 置为 NULL（后续调用返回空字符串）
 *
 * 注意：这会修改原始字符串（将 Tab 替换为 NUL），但这对我们的一次性解析场景是安全的。
 * Note: This modifies the original string (replacing Tab with NUL), which is safe
 * for our one-pass parsing scenario.
 */
static char *next_token(char **cursor) {
    static char empty_buf[1] = {0};
    char *start = *cursor;
    char *sep;

    if (!start) {
        return empty_buf;
    }

    sep = strchr(start, '\t');
    if (sep) {
        *sep = '\0';           /* 将 Tab 替换为字符串终止符 / Replace Tab with NUL terminator */
        *cursor = sep + 1;     /* 游标前进到下一个字段 / Advance cursor to next field */
    } else {
        *cursor = NULL;        /* 无更多字段 / No more fields */
    }

    return start;
}

/* ==========================================================================
 * 第二部分：字段转义/反转义 — 防止数据中的特殊字符破坏文件格式
 * Part 2: Field Escape/Unescape — Protect file format from embedded special chars
 * ==========================================================================
 *
 * [中文]
 * 由于数据文件使用 Tab(\t) 和换行(\n) 作为结构分隔符，如果字段值本身包含
 * 这些字符（例如诊断描述中有换行），就会破坏文件格式。因此引入 C 风格转义：
 *
 *   原始字符 → 转义后      说明
 *   --------   --------    ----
 *   \          \\          反斜杠自身
 *   Tab        \t          制表符
 *   \n         \n          换行符
 *   \r         \r          回车符
 *
 * 写入时调用 escape_field（转义），读取后调用 unescape_field_inplace（反转义）。
 *
 * [English]
 * Since data files use Tab(\t) and newline(\n) as structural delimiters, if a
 * field value itself contains these characters (e.g., a diagnosis description
 * with line breaks), it would corrupt the file format. C-style escaping is used:
 *
 *   Original → Escaped    Description
 *   --------   --------   -----------
 *   \          \\         Backslash itself
 *   Tab        \t         Tab character
 *   \n         \n         Newline
 *   \r         \r         Carriage return
 *
 * escape_field is called when writing, unescape_field_inplace when reading.
 */

/*
 * [中文] 将输入字符串中的特殊字符转义后写入输出缓冲区
 * [English] Escape special characters from input string into output buffer
 *
 * j + 4 < output_size 确保即使下一个字符需要转义为2字节，也有足够的空间放
 * 转义序列 + NUL 终止符。
 * j + 4 < output_size ensures room for a 2-char escape sequence + safety margin + NUL.
 */
static void escape_field(const char *input, char *output, size_t output_size) {
    size_t j = 0;
    for (size_t i = 0; input[i] != '\0' && j + 4 < output_size; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '\\') {
            output[j++] = '\\'; output[j++] = '\\';
        } else if (c == '\t') {
            output[j++] = '\\'; output[j++] = 't';
        } else if (c == '\n') {
            output[j++] = '\\'; output[j++] = 'n';
        } else if (c == '\r') {
            output[j++] = '\\'; output[j++] = 'r';
        } else {
            output[j++] = c;
        }
    }
    output[j] = '\0';
}

/*
 * [中文] 原地反转义 — 将字符串中的 \n, \t, \r, \\ 还原为真实字符
 * [English] In-place unescape — restore \n, \t, \r, \\ to their real characters
 *
 * 算法说明 / Algorithm:
 *   使用两个索引 i（读指针）和 j（写指针）在同一缓冲区中操作。
 *   当读指针遇到 \ 时，检查下一个字符：
 *     \\ → 写入一个 \
 *     \t → 写入一个 Tab
 *     \n → 写入一个换行
 *     \r → 写入一个回车
 *     否则 → 原样写入 \
 *   j 始终 ≤ i，因此原地操作是安全的。
 *   Uses two indices: i (read pointer) and j (write pointer) in the same buffer.
 *   When read pointer encounters \, check the next character for the escape code.
 *   Since j ≤ i always, in-place operation is safe.
 */
static void unescape_field_inplace(char *str) {
    size_t j = 0;
    for (size_t i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\\' && str[i+1] != '\0') {
            if (str[i+1] == '\\') { str[j++] = '\\'; i++; }
            else if (str[i+1] == 't') { str[j++] = '\t'; i++; }
            else if (str[i+1] == 'n') { str[j++] = '\n'; i++; }
            else if (str[i+1] == 'r') { str[j++] = '\r'; i++; }
            else { str[j++] = str[i]; }
        } else {
            str[j++] = str[i];
        }
    }
    str[j] = '\0';
}

/*
 * [中文] 安全地写一个字符串字段到文件（自动转义特殊字符）
 * [English] Safely write a string field to file (auto-escape special characters)
 */
static void fprintf_escaped(FILE *fp, const char *str) {
    char buf[4096];
    escape_field(str, buf, sizeof(buf));
    fprintf(fp, "%s", buf);
}

/* ==========================================================================
 * 第三部分：字段类型解析工具
 * Part 3: Token Parsing Utilities (int, float, bool)
 * ========================================================================== */

/* 解析下一个 token 为 int / Parse next token as int */
static int parse_int_token(char **cursor) {
    return atoi(next_token(cursor));
}

/* 解析下一个 token 为 float / Parse next token as float */
static float parse_float_token(char **cursor) {
    return (float)atof(next_token(cursor));
}

/*
 * [中文] 解析下一个 token 为 bool — 支持 "1", "true", "TRUE" 三种形式
 * [English] Parse next token as bool — accepts "1", "true", or "TRUE"
 */
static bool parse_bool_token(char **cursor) {
    const char *token = next_token(cursor);
    return strcmp(token, "1") == 0 || strcmp(token, "true") == 0 || strcmp(token, "TRUE") == 0;
}

/* ==========================================================================
 * 第四部分：数据目录初始化
 * Part 4: Data Directory Initialization
 * ========================================================================== */

/*
 * [中文] 初始化数据存储 — 确保 data/ 目录存在，不存在则创建
 * [English] Initialize data storage — ensure data/ directory exists, create if not
 *
 * Windows 使用 CreateDirectoryA，Unix 使用 mkdir(0755)。
 * Windows uses CreateDirectoryA, Unix uses mkdir(0755).
 */
int init_data_storage(void) {
#ifdef _WIN32
    DWORD dwAttrib = GetFileAttributesA(DATA_DIR);
    if (dwAttrib == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryA(DATA_DIR, NULL)) {
            printf("创建数据目录失败!\n");
            return ERROR_FILE_IO;
        }
    }
#else
    struct stat st = {0};
    if (stat(DATA_DIR, &st) == -1) {
        if (mkdir(DATA_DIR, 0755) != 0) {
            printf("创建数据目录失败: %s\n", strerror(errno));
            return ERROR_FILE_IO;
        }
    }
#endif
    return SUCCESS;
}

/* ==========================================================================
 * 第五部分：用户(User)链表操作 — 创建/释放/计数
 * Part 5: User Linked List Operations — Create/Free/Count
 * ========================================================================== */

UserNode* create_user_node(const User *user) {
    UserNode *node = (UserNode *)malloc(sizeof(UserNode));
    if (node) {
        node->data = *user;
        node->next = NULL;
    }
    return node;
}

/* [中文] 释放整个用户链表 / [English] Free entire user linked list */
void free_user_list(UserNode *head) {
    UserNode *current = head;
    while (current) {
        UserNode *next = current->next;
        free(current);
        current = next;
    }
}

/* [中文] 统计用户链表节点数 / [English] Count nodes in user linked list */
int count_user_list(UserNode *head) {
    int count = 0;
    UserNode *current = head;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/* ==========================================================================
 * 第六部分：患者(Patient)链表操作 — 创建/释放/计数
 * Part 6: Patient Linked List Operations — Create/Free/Count
 * ========================================================================== */

PatientNode* create_patient_node(const Patient *patient) {
    PatientNode *node = (PatientNode *)malloc(sizeof(PatientNode));
    if (node) {
        node->data = *patient;
        node->next = NULL;
    }
    return node;
}

void free_patient_list(PatientNode *head) {
    PatientNode *current = head;
    while (current) {
        PatientNode *next = current->next;
        free(current);
        current = next;
    }
}

int count_patient_list(PatientNode *head) {
    int count = 0;
    PatientNode *current = head;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/* ==========================================================================
 * 第七部分：医生(Doctor)链表操作 — 创建/释放/计数
 * Part 7: Doctor Linked List Operations — Create/Free/Count
 * ========================================================================== */

DoctorNode* create_doctor_node(const Doctor *doctor) {
    DoctorNode *node = (DoctorNode *)malloc(sizeof(DoctorNode));
    if (node) {
        node->data = *doctor;
        node->next = NULL;
    }
    return node;
}

void free_doctor_list(DoctorNode *head) {
    DoctorNode *current = head;
    while (current) {
        DoctorNode *next = current->next;
        free(current);
        current = next;
    }
}

int count_doctor_list(DoctorNode *head) {
    int count = 0;
    DoctorNode *current = head;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/* ==========================================================================
 * 第八部分：科室(Department)链表操作 — 创建/释放/计数
 * Part 8: Department Linked List Operations — Create/Free/Count
 * ========================================================================== */

DepartmentNode* create_department_node(const Department *department) {
    DepartmentNode *node = (DepartmentNode *)malloc(sizeof(DepartmentNode));
    if (node) {
        node->data = *department;
        node->next = NULL;
    }
    return node;
}

void free_department_list(DepartmentNode *head) {
    DepartmentNode *current = head;
    while (current) {
        DepartmentNode *next = current->next;
        free(current);
        current = next;
    }
}

int count_department_list(DepartmentNode *head) {
    int count = 0;
    DepartmentNode *current = head;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/* ==========================================================================
 * 第九部分：药品(Drug)链表操作 — 创建/释放/计数
 * Part 9: Drug Linked List Operations — Create/Free/Count
 * ========================================================================== */

DrugNode* create_drug_node(const Drug *drug) {
    DrugNode *node = (DrugNode *)malloc(sizeof(DrugNode));
    if (node) {
        node->data = *drug;
        node->next = NULL;
    }
    return node;
}

void free_drug_list(DrugNode *head) {
    DrugNode *current = head;
    while (current) {
        DrugNode *next = current->next;
        free(current);
        current = next;
    }
}

int count_drug_list(DrugNode *head) {
    int count = 0;
    DrugNode *current = head;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/* ==========================================================================
 * 第十部分：病房(Ward)链表操作 — 创建/释放/计数
 * Part 10: Ward Linked List Operations — Create/Free/Count
 * ========================================================================== */

WardNode* create_ward_node(const Ward *ward) {
    WardNode *node = (WardNode *)malloc(sizeof(WardNode));
    if (node) {
        node->data = *ward;
        node->next = NULL;
    }
    return node;
}

void free_ward_list(WardNode *head) {
    WardNode *current = head;
    while (current) {
        WardNode *next = current->next;
        free(current);
        current = next;
    }
}

int count_ward_list(WardNode *head) {
    int count = 0;
    WardNode *current = head;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/* ==========================================================================
 * 第十一部分：预约(Appointment)链表操作 — 创建/释放/计数
 * Part 11: Appointment Linked List Operations — Create/Free/Count
 * ========================================================================== */

AppointmentNode* create_appointment_node(const Appointment *appointment) {
    AppointmentNode *node = (AppointmentNode *)malloc(sizeof(AppointmentNode));
    if (node) {
        node->data = *appointment;
        node->next = NULL;
    }
    return node;
}

void free_appointment_list(AppointmentNode *head) {
    AppointmentNode *current = head;
    while (current) {
        AppointmentNode *next = current->next;
        free(current);
        current = next;
    }
}

int count_appointment_list(AppointmentNode *head) {
    int count = 0;
    AppointmentNode *current = head;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/* ==========================================================================
 * 第十二部分：现场挂号(OnsiteRegistration)队列操作
 * Part 12: OnsiteRegistration Queue Operations
 * ==========================================================================
 *
 * [中文]
 * 现场挂号使用队列（Queue）结构而非普通链表。这是因为现场挂号需要 FIFO
 * （先进先出）的就诊顺序，同时支持急诊患者插队到队首。
 *
 * enqueue_onsite_registration 的 front 参数：
 *   - front=true  → 插入队首（急诊患者优先）
 *   - front=false → 插入队尾（正常排队）
 *
 * [English]
 * Onsite registration uses a Queue structure instead of a plain linked list,
 * because it requires FIFO (first-in-first-out) visit ordering, with support
 * for emergency patients to jump to the front of the queue.
 *
 * enqueue_onsite_registration front parameter:
 *   - front=true  → insert at front (emergency patient priority)
 *   - front=false → insert at rear (normal queue)
 */

OnsiteRegistrationNode* create_onsite_registration_node(const OnsiteRegistration *registration) {
    OnsiteRegistrationNode *node = (OnsiteRegistrationNode *)malloc(sizeof(OnsiteRegistrationNode));
    if (node) {
        node->data = *registration;
        node->next = NULL;
    }
    return node;
}

void free_onsite_registration_list(OnsiteRegistrationNode *head) {
    OnsiteRegistrationNode *current = head;
    while (current) {
        OnsiteRegistrationNode *next = current->next;
        free(current);
        current = next;
    }
}

int count_onsite_registration_list(OnsiteRegistrationNode *head) {
    int count = 0;
    OnsiteRegistrationNode *current = head;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/* [中文] 初始化空队列 / [English] Initialize an empty queue */
void init_onsite_registration_queue(OnsiteRegistrationQueue *queue) {
    if (!queue) {
        return;
    }
    queue->front = NULL;
    queue->rear = NULL;
    queue->size = 0;
}

/*
 * [中文] 入队操作 — 支持队首插入（急诊）和队尾插入（普通）
 * [English] Enqueue operation — supports front insertion (emergency) and rear insertion (normal)
 *
 * 队首插入逻辑 / Front insertion logic:
 *   新节点指向原队首 → 更新队首指针 → 如果队列原为空则同时更新队尾
 *   New node points to old front → update front pointer → also update rear if was empty
 *
 * 队尾插入逻辑 / Rear insertion logic:
 *   如果队列为空 → 队首队尾都指向新节点
 *   如果队列非空 → 原队尾的 next 指向新节点 → 更新队尾指针
 *   If empty → both front and rear point to new node
 *   If not empty → old rear->next = new node → update rear pointer
 */
int enqueue_onsite_registration(OnsiteRegistrationQueue *queue, const OnsiteRegistration *registration, bool front) {
    OnsiteRegistrationNode *node;

    if (!queue || !registration) {
        return ERROR_INVALID_INPUT;
    }

    node = create_onsite_registration_node(registration);
    if (!node) {
        return ERROR_FILE_IO;
    }

    if (front) {
        /* 急诊：插入队首 / Emergency: insert at front */
        node->next = queue->front;
        queue->front = node;
        if (!queue->rear) queue->rear = node;
    } else {
        /* 普通：插入队尾 / Normal: insert at rear */
        if (!queue->rear) {
            queue->front = node;
            queue->rear = node;
        } else {
            queue->rear->next = node;
            queue->rear = node;
        }
    }

    queue->size++;
    return SUCCESS;
}

/*
 * [中文] 出队操作 — 从队首取出一个挂号记录
 * [English] Dequeue operation — remove a registration from the front of the queue
 */
int dequeue_onsite_registration(OnsiteRegistrationQueue *queue, OnsiteRegistration *registration) {
    OnsiteRegistrationNode *node;

    if (!queue || !queue->front) {
        return ERROR_NOT_FOUND;
    }

    node = queue->front;
    if (registration) {
        *registration = node->data;
    }

    queue->front = node->next;
    if (!queue->front) {
        queue->rear = NULL;  /* 队列已空 / Queue is now empty */
    }

    queue->size--;
    free(node);
    return SUCCESS;
}

/* [中文] 释放整个队列（包括所有节点） / [English] Free entire queue (including all nodes) */
void free_onsite_registration_queue(OnsiteRegistrationQueue *queue) {
    if (!queue) {
        return;
    }
    free_onsite_registration_list(queue->front);
    init_onsite_registration_queue(queue);
}

/* ==========================================================================
 * 第十三部分：病房呼叫(WardCall)链表操作 — 创建/释放/计数
 * Part 13: WardCall Linked List Operations — Create/Free/Count
 * ========================================================================== */

WardCallNode* create_ward_call_node(const WardCall *call) {
    WardCallNode *node = (WardCallNode *)malloc(sizeof(WardCallNode));
    if (node) {
        node->data = *call;
        node->next = NULL;
    }
    return node;
}

void free_ward_call_list(WardCallNode *head) {
    WardCallNode *current = head;
    while (current) {
        WardCallNode *next = current->next;
        free(current);
        current = next;
    }
}

int count_ward_call_list(WardCallNode *head) {
    int count = 0;
    WardCallNode *current = head;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/* ==========================================================================
 * 第十四部分：病历(MedicalRecord)链表操作 — 创建/释放/计数
 * Part 14: MedicalRecord Linked List Operations — Create/Free/Count
 * ========================================================================== */

MedicalRecordNode* create_medical_record_node(const MedicalRecord *record) {
    MedicalRecordNode *node = (MedicalRecordNode *)malloc(sizeof(MedicalRecordNode));
    if (node) {
        node->data = *record;
        node->next = NULL;
    }
    return node;
}

void free_medical_record_list(MedicalRecordNode *head) {
    MedicalRecordNode *current = head;
    while (current) {
        MedicalRecordNode *next = current->next;
        free(current);
        current = next;
    }
}

int count_medical_record_list(MedicalRecordNode *head) {
    int count = 0;
    MedicalRecordNode *current = head;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/* ==========================================================================
 * 第十五部分：处方(Prescription)链表操作 — 创建/释放/计数
 * Part 15: Prescription Linked List Operations — Create/Free/Count
 * ========================================================================== */

PrescriptionNode* create_prescription_node(const Prescription *prescription) {
    PrescriptionNode *node = (PrescriptionNode *)malloc(sizeof(PrescriptionNode));
    if (node) {
        node->data = *prescription;
        node->next = NULL;
    }
    return node;
}

void free_prescription_list(PrescriptionNode *head) {
    PrescriptionNode *current = head;
    while (current) {
        PrescriptionNode *next = current->next;
        free(current);
        current = next;
    }
}

int count_prescription_list(PrescriptionNode *head) {
    int count = 0;
    PrescriptionNode *current = head;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/* ==========================================================================
 * 第十六部分：数据加载与保存 — 用户(User)
 * Part 16: Data Load/Save — User
 * ==========================================================================
 *
 * [中文]
 * 加载模式 / Loading pattern:
 *   1. 打开文件，逐行读取有效数据行（跳过注释/空行）
 *   2. 使用 next_token 按 Tab 分割字段
 *   3. 字符串字段在赋值前调用 unescape_field_inplace 反转义
 *   4. 数值字段通过 parse_*_token 解析
 *   5. 构建链表节点，追加到链表尾部（head/tail 指针跟踪）
 *   6. 如果内存分配失败，释放已构建的链表并返回 NULL
 *
 * 保存模式 / Saving pattern:
 *   1. 先写入 # 开头的列标题行
 *   2. 遍历链表，每个字段用 fprintf_escaped 安全写入（自动转义特殊字符）
 *   3. 字段间用 Tab 分隔，记录间用换行分隔
 *
 * [English]
 * Loading pattern:
 *   1. Open file, read valid data lines (skip comments/blanks)
 *   2. Use next_token to split fields by Tab
 *   3. String fields: call unescape_field_inplace before assigning
 *   4. Numeric fields: parse via parse_*_token
 *   5. Build linked list nodes, append to tail (head/tail pointer tracking)
 *   6. On allocation failure, free the partial list and return NULL
 *
 * Saving pattern:
 *   1. Write #-prefixed column header line first
 *   2. Iterate list, write each field via fprintf_escaped (auto-escape)
 *   3. Tab between fields, newline between records
 */

UserNode* load_users_list(void) {
    FILE *fp = fopen(USERS_FILE, "r");
    if (!fp) {
        return NULL;
    }

    UserNode *head = NULL;
    UserNode *tail = NULL;
    char line[TEXT_LINE_SIZE];

    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        User user;
        memset(&user, 0, sizeof(user));
        /* 用户名字段不需要反转义（用户名不含特殊字符） / Username needs no unescape */
        strncpy(user.username, next_token(&cursor), sizeof(user.username) - 1);
        user.username[sizeof(user.username) - 1] = '\0';
        strncpy(user.password, next_token(&cursor), sizeof(user.password) - 1);
        user.password[sizeof(user.password) - 1] = '\0';
        strncpy(user.role, next_token(&cursor), sizeof(user.role) - 1);
        user.role[sizeof(user.role) - 1] = '\0';

        UserNode *node = create_user_node(&user);
        if (!node) {
            free_user_list(head);  /* 回滚已分配内存 / Rollback allocated memory */
            fclose(fp);
            return NULL;
        }

        /* 链表尾部追加 / Append to tail of linked list */
        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
    }

    fclose(fp);
    return head;
}

int save_users_list(UserNode *head) {
    FILE *fp = fopen(USERS_FILE, "w");
    if (!fp) {
        return ERROR_FILE_IO;
    }

    /* 列标题行（# 开头，加载时自动跳过） / Column header line (# prefix, auto-skipped on load) */
    fprintf(fp, "# username\tpassword\trole\n");
    UserNode *current = head;
    while (current) {
        fprintf_escaped(fp, current->data.username); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.password); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.role); fprintf(fp, "\n");
        current = current->next;
    }

    fclose(fp);
    return SUCCESS;
}

/* ==========================================================================
 * 第十七部分：数据加载与保存 — 患者(Patient)
 * Part 17: Data Load/Save — Patient
 * ========================================================================== */

PatientNode* load_patients_list(void) {
    FILE *fp = fopen(PATIENTS_FILE, "r");
    if (!fp) {
        return NULL;
    }

    PatientNode *head = NULL;
    PatientNode *tail = NULL;
    char line[TEXT_LINE_SIZE];

    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        Patient patient;
        memset(&patient, 0, sizeof(patient));
        /* 患者数据可能含特殊字符（地址等），需要反转义 / Patient data may contain special chars, need unescape */
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(patient.patient_id, tk, sizeof(patient.patient_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(patient.username, tk, sizeof(patient.username) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(patient.name, tk, sizeof(patient.name) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(patient.gender, tk, sizeof(patient.gender) - 1); }
        patient.age = parse_int_token(&cursor);
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(patient.phone, tk, sizeof(patient.phone) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(patient.address, tk, sizeof(patient.address) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(patient.patient_type, tk, sizeof(patient.patient_type) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(patient.treatment_stage, tk, sizeof(patient.treatment_stage) - 1); }
        patient.is_emergency = parse_bool_token(&cursor);

        PatientNode *node = create_patient_node(&patient);
        if (!node) {
            free_patient_list(head);
            fclose(fp);
            return NULL;
        }

        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
    }

    fclose(fp);
    return head;
}

int save_patients_list(PatientNode *head) {
    FILE *fp = fopen(PATIENTS_FILE, "w");
    if (!fp) {
        return ERROR_FILE_IO;
    }

    fprintf(fp, "# patient_id\tusername\tname\tgender\tage\tphone\taddress\tpatient_type\ttreatment_stage\tis_emergency\n");
    PatientNode *current = head;
    while (current) {
        fprintf_escaped(fp, current->data.patient_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.username); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.name); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.gender); fprintf(fp, "\t");
        fprintf(fp, "%d\t", current->data.age);              /* 数值字段无需转义 / Numeric field needs no escaping */
        fprintf_escaped(fp, current->data.phone); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.address); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.patient_type); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.treatment_stage); fprintf(fp, "\t");
        fprintf(fp, "%d\n", current->data.is_emergency ? 1 : 0);  /* bool 存为 0/1 / bool stored as 0/1 */
        current = current->next;
    }

    fclose(fp);
    return SUCCESS;
}

/* ==========================================================================
 * 第十八部分：数据加载与保存 — 医生(Doctor)
 * Part 18: Data Load/Save — Doctor
 * ========================================================================== */

DoctorNode* load_doctors_list(void) {
    FILE *fp = fopen(DOCTORS_FILE, "r");
    if (!fp) {
        return NULL;
    }

    DoctorNode *head = NULL;
    DoctorNode *tail = NULL;
    char line[TEXT_LINE_SIZE];

    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        Doctor doctor;
        memset(&doctor, 0, sizeof(doctor));
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(doctor.doctor_id, tk, sizeof(doctor.doctor_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(doctor.username, tk, sizeof(doctor.username) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(doctor.name, tk, sizeof(doctor.name) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(doctor.department_id, tk, sizeof(doctor.department_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(doctor.title, tk, sizeof(doctor.title) - 1); }
        doctor.busy_level = parse_int_token(&cursor);

        DoctorNode *node = create_doctor_node(&doctor);
        if (!node) {
            free_doctor_list(head);
            fclose(fp);
            return NULL;
        }

        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
    }

    fclose(fp);
    return head;
}

int save_doctors_list(DoctorNode *head) {
    FILE *fp = fopen(DOCTORS_FILE, "w");
    if (!fp) {
        return ERROR_FILE_IO;
    }

    fprintf(fp, "# doctor_id\tusername\tname\tdepartment_id\ttitle\tbusy_level\n");
    DoctorNode *current = head;
    while (current) {
        fprintf_escaped(fp, current->data.doctor_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.username); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.name); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.department_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.title); fprintf(fp, "\t");
        fprintf(fp, "%d\n", current->data.busy_level);
        current = current->next;
    }

    fclose(fp);
    return SUCCESS;
}

/* ==========================================================================
 * 第十九部分：数据加载与保存 — 科室(Department)
 * Part 19: Data Load/Save — Department
 * ========================================================================== */

DepartmentNode* load_departments_list(void) {
    FILE *fp = fopen(DEPARTMENTS_FILE, "r");
    if (!fp) {
        return NULL;
    }

    DepartmentNode *head = NULL;
    DepartmentNode *tail = NULL;
    char line[TEXT_LINE_SIZE];

    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        Department department;
        memset(&department, 0, sizeof(department));
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(department.department_id, tk, sizeof(department.department_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(department.name, tk, sizeof(department.name) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(department.leader, tk, sizeof(department.leader) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(department.phone, tk, sizeof(department.phone) - 1); }

        DepartmentNode *node = create_department_node(&department);
        if (!node) {
            free_department_list(head);
            fclose(fp);
            return NULL;
        }

        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
    }

    fclose(fp);
    return head;
}

int save_departments_list(DepartmentNode *head) {
    FILE *fp = fopen(DEPARTMENTS_FILE, "w");
    if (!fp) {
        return ERROR_FILE_IO;
    }

    fprintf(fp, "# department_id\tname\tleader\tphone\n");
    DepartmentNode *current = head;
    while (current) {
        fprintf_escaped(fp, current->data.department_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.name); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.leader); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.phone); fprintf(fp, "\n");
        current = current->next;
    }

    fclose(fp);
    return SUCCESS;
}

/* ==========================================================================
 * 第二十部分：数据加载与保存 — 药品(Drug)
 * Part 20: Data Load/Save — Drug
 * ========================================================================== */

DrugNode* load_drugs_list(void) {
    FILE *fp = fopen(DRUGS_FILE, "r");
    if (!fp) {
        return NULL;
    }

    DrugNode *head = NULL;
    DrugNode *tail = NULL;
    char line[TEXT_LINE_SIZE];

    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        Drug drug;
        memset(&drug, 0, sizeof(drug));
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(drug.drug_id, tk, sizeof(drug.drug_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(drug.name, tk, sizeof(drug.name) - 1); }
        drug.price = parse_float_token(&cursor);
        drug.stock_num = parse_int_token(&cursor);
        drug.warning_line = parse_int_token(&cursor);
        drug.is_special = parse_bool_token(&cursor);
        drug.reimbursement_ratio = parse_float_token(&cursor);
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(drug.category, tk, sizeof(drug.category) - 1); }
        /* 如果旧数据没有 category 字段，默认设为 "西药" */
        /* If old data has no category field, default to "西药" (Western medicine) */
        if (drug.category[0] == '\0') strcpy(drug.category, "西药");

        DrugNode *node = create_drug_node(&drug);
        if (!node) {
            free_drug_list(head);
            fclose(fp);
            return NULL;
        }

        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
    }

    fclose(fp);
    return head;
}

int save_drugs_list(DrugNode *head) {
    FILE *fp = fopen(DRUGS_FILE, "w");
    if (!fp) {
        return ERROR_FILE_IO;
    }

    fprintf(fp, "# drug_id\tname\tprice\tstock_num\twarning_line\tis_special\treimbursement_ratio\tcategory\n");
    DrugNode *current = head;
    while (current) {
        fprintf_escaped(fp, current->data.drug_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.name); fprintf(fp, "\t");
        fprintf(fp, "%.2f\t", current->data.price);           /* 价格保留2位小数 / Price with 2 decimal places */
        fprintf(fp, "%d\t", current->data.stock_num);
        fprintf(fp, "%d\t", current->data.warning_line);
        fprintf(fp, "%d\t", current->data.is_special ? 1 : 0);
        fprintf(fp, "%.2f\t", current->data.reimbursement_ratio); /* 报销比例2位小数 / Reimbursement ratio 2 decimal */
        fprintf_escaped(fp, current->data.category); fprintf(fp, "\n");
        current = current->next;
    }

    fclose(fp);
    return SUCCESS;
}

/* ==========================================================================
 * 第二十一部分：数据加载与保存 — 病房(Ward)
 * Part 21: Data Load/Save — Ward
 * ========================================================================== */

WardNode* load_wards_list(void) {
    FILE *fp = fopen(WARDS_FILE, "r");
    if (!fp) {
        return NULL;
    }

    WardNode *head = NULL;
    WardNode *tail = NULL;
    char line[TEXT_LINE_SIZE];

    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        Ward ward;
        memset(&ward, 0, sizeof(ward));
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(ward.ward_id, tk, sizeof(ward.ward_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(ward.type, tk, sizeof(ward.type) - 1); }
        ward.total_beds = parse_int_token(&cursor);
        ward.remain_beds = parse_int_token(&cursor);
        ward.warning_line = parse_int_token(&cursor);

        WardNode *node = create_ward_node(&ward);
        if (!node) {
            free_ward_list(head);
            fclose(fp);
            return NULL;
        }

        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
    }

    fclose(fp);
    return head;
}

int save_wards_list(WardNode *head) {
    FILE *fp = fopen(WARDS_FILE, "w");
    if (!fp) {
        return ERROR_FILE_IO;
    }

    fprintf(fp, "# ward_id\ttype\ttotal_beds\tremain_beds\twarning_line\n");
    WardNode *current = head;
    while (current) {
        fprintf_escaped(fp, current->data.ward_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.type); fprintf(fp, "\t");
        fprintf(fp, "%d\t%d\t%d\n",
                current->data.total_beds,
                current->data.remain_beds,
                current->data.warning_line);
        current = current->next;
    }

    fclose(fp);
    return SUCCESS;
}

/* ==========================================================================
 * 第二十二部分：数据加载与保存 — 预约(Appointment)
 * Part 22: Data Load/Save — Appointment
 * ========================================================================== */

AppointmentNode* load_appointments_list(void) {
    FILE *fp = fopen(APPOINTMENTS_FILE, "r");
    if (!fp) {
        return NULL;
    }

    AppointmentNode *head = NULL;
    AppointmentNode *tail = NULL;
    char line[TEXT_LINE_SIZE];

    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        Appointment appointment;
        memset(&appointment, 0, sizeof(appointment));
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(appointment.appointment_id, tk, sizeof(appointment.appointment_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(appointment.patient_id, tk, sizeof(appointment.patient_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(appointment.doctor_id, tk, sizeof(appointment.doctor_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(appointment.department_id, tk, sizeof(appointment.department_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(appointment.appointment_date, tk, sizeof(appointment.appointment_date) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(appointment.appointment_time, tk, sizeof(appointment.appointment_time) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(appointment.status, tk, sizeof(appointment.status) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(appointment.create_time, tk, sizeof(appointment.create_time) - 1); }
        /* fee 和 paid 为后加字段, 旧数据可能没有, 给默认值
           fee & paid are backward-compat fields, default if missing */
        { char *tk = next_token(&cursor); appointment.fee = (tk && tk[0]) ? (float)atof(tk) : 0.0f; }
        { char *tk = next_token(&cursor); appointment.paid = (tk && tk[0]) ? atoi(tk) : 0; }

        AppointmentNode *node = create_appointment_node(&appointment);
        if (!node) {
            free_appointment_list(head);
            fclose(fp);
            return NULL;
        }

        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
    }

    fclose(fp);
    return head;
}

int save_appointments_list(AppointmentNode *head) {
    FILE *fp = fopen(APPOINTMENTS_FILE, "w");
    if (!fp) {
        return ERROR_FILE_IO;
    }

    fprintf(fp, "# appointment_id\tpatient_id\tdoctor_id\tdepartment_id\tappointment_date\tappointment_time\tstatus\tcreate_time\tfee\tpaid\n");
    AppointmentNode *current = head;
    while (current) {
        fprintf_escaped(fp, current->data.appointment_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.patient_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.doctor_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.department_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.appointment_date); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.appointment_time); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.status); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.create_time); fprintf(fp, "\t");
        fprintf(fp, "%.2f\t%d\n", current->data.fee, current->data.paid);
        current = current->next;
    }

    fclose(fp);
    return SUCCESS;
}

/* ==========================================================================
 * 第二十三部分：数据加载与保存 — 现场挂号(OnsiteRegistration)队列
 * Part 23: Data Load/Save — OnsiteRegistration Queue
 * ==========================================================================
 *
 * [中文]
 * 现场挂号以队列形式从文件加载。加载时按照文件顺序逐个入队（队尾追加），
 * 因此文件中记录的顺序即反映原始排队顺序。
 * [English]
 * Onsite registrations are loaded as a queue from file. Each record is enqueued
 * in file order (rear append), so the file order reflects the original queue order.
 */

OnsiteRegistrationQueue load_onsite_registration_queue(void) {
    FILE *fp = fopen(ONSITE_REGISTRATIONS_FILE, "r");
    OnsiteRegistrationQueue queue;
    char line[TEXT_LINE_SIZE];

    init_onsite_registration_queue(&queue);
    if (!fp) {
        return queue;
    }

    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        OnsiteRegistration registration;
        memset(&registration, 0, sizeof(registration));
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(registration.onsite_id, tk, sizeof(registration.onsite_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(registration.patient_id, tk, sizeof(registration.patient_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(registration.doctor_id, tk, sizeof(registration.doctor_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(registration.department_id, tk, sizeof(registration.department_id) - 1); }
        registration.queue_number = parse_int_token(&cursor);
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(registration.status, tk, sizeof(registration.status) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(registration.create_time, tk, sizeof(registration.create_time) - 1); }

        /* 按文件顺序追加入队（队尾） / Enqueue in file order (rear append) */
        if (enqueue_onsite_registration(&queue, &registration, false) != SUCCESS) {
            free_onsite_registration_queue(&queue);
            break;
        }
    }

    fclose(fp);
    return queue;
}

int save_onsite_registration_queue(const OnsiteRegistrationQueue *queue) {
    FILE *fp = fopen(ONSITE_REGISTRATIONS_FILE, "w");
    OnsiteRegistrationNode *current;

    if (!fp) {
        return ERROR_FILE_IO;
    }

    fprintf(fp, "# onsite_id\tpatient_id\tdoctor_id\tdepartment_id\tqueue_number\tstatus\tcreate_time\n");
    /* 从队首开始遍历，保持队列顺序 / Traverse from front to preserve queue order */
    current = queue ? queue->front : NULL;
    while (current) {
        fprintf_escaped(fp, current->data.onsite_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.patient_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.doctor_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.department_id); fprintf(fp, "\t");
        fprintf(fp, "%d\t", current->data.queue_number);
        fprintf_escaped(fp, current->data.status); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.create_time); fprintf(fp, "\n");
        current = current->next;
    }

    fclose(fp);
    return SUCCESS;
}

/*
 * [中文] 获取指定医生的下一个现场排队号
 * [English] Get the next onsite queue number for a specific doctor
 *
 * 算法 / Algorithm:
 *   遍历当前所有现场挂号记录，找到同一医生的最大排队号，
 *   返回 max + 1 作为新排队号。
 *   Iterate all current onsite registrations, find the max queue_number
 *   for the same doctor, return max + 1 as the new queue number.
 */
int get_next_onsite_queue_number(const char *doctor_id, const char *department_id) {
    OnsiteRegistrationQueue queue = load_onsite_registration_queue();
    OnsiteRegistrationNode *current = queue.front;
    int max_queue_number = 0;

    while (current) {
        if (strcmp(current->data.doctor_id, doctor_id) == 0 &&
            strcmp(current->data.department_id, department_id) == 0 &&
            current->data.queue_number > max_queue_number) {
            max_queue_number = current->data.queue_number;
        }
        current = current->next;
    }

    free_onsite_registration_queue(&queue);
    return max_queue_number + 1;
}

/* ==========================================================================
 * 第二十四部分：数据加载与保存 — 病房呼叫(WardCall)
 * Part 24: Data Load/Save — WardCall
 * ========================================================================== */

WardCallNode* load_ward_calls_list(void) {
    FILE *fp = fopen(WARD_CALLS_FILE, "r");
    WardCallNode *head = NULL;
    WardCallNode *tail = NULL;
    char line[TEXT_LINE_SIZE];

    if (!fp) {
        return NULL;
    }

    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        WardCall call;
        memset(&call, 0, sizeof(call));
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(call.call_id, tk, sizeof(call.call_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(call.ward_id, tk, sizeof(call.ward_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(call.department_id, tk, sizeof(call.department_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(call.patient_id, tk, sizeof(call.patient_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(call.message, tk, sizeof(call.message) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(call.status, tk, sizeof(call.status) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(call.create_time, tk, sizeof(call.create_time) - 1); }

        WardCallNode *node = create_ward_call_node(&call);
        if (!node) {
            free_ward_call_list(head);
            fclose(fp);
            return NULL;
        }

        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
    }

    fclose(fp);
    return head;
}

int save_ward_calls_list(WardCallNode *head) {
    FILE *fp = fopen(WARD_CALLS_FILE, "w");
    WardCallNode *current = head;

    if (!fp) {
        return ERROR_FILE_IO;
    }

    fprintf(fp, "# call_id\tward_id\tdepartment_id\tpatient_id\tmessage\tstatus\tcreate_time\n");
    while (current) {
        fprintf_escaped(fp, current->data.call_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.ward_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.department_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.patient_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.message); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.status); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.create_time); fprintf(fp, "\n");
        current = current->next;
    }

    fclose(fp);
    return SUCCESS;
}

/* ==========================================================================
 * 第二十五部分：排班(Schedule)链表操作与数据加载保存
 * Part 25: Schedule Linked List Operations & Data Load/Save
 * ========================================================================== */

ScheduleNode* create_schedule_node(const Schedule *schedule) {
    ScheduleNode *node = (ScheduleNode *)malloc(sizeof(ScheduleNode));
    if (node) {
        node->data = *schedule;
        node->next = NULL;
    }
    return node;
}

void free_schedule_list(ScheduleNode *head) {
    ScheduleNode *current = head;
    while (current) {
        ScheduleNode *next = current->next;
        free(current);
        current = next;
    }
}

int count_schedule_list(ScheduleNode *head) {
    int count = 0;
    ScheduleNode *current = head;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

ScheduleNode* load_schedules_list(void) {
    FILE *fp = fopen(SCHEDULES_FILE, "r");
    if (!fp) {
        return NULL;
    }

    ScheduleNode *head = NULL;
    ScheduleNode *tail = NULL;
    char line[TEXT_LINE_SIZE];

    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        Schedule schedule;
        memset(&schedule, 0, sizeof(schedule));
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(schedule.schedule_id, tk, sizeof(schedule.schedule_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(schedule.doctor_id, tk, sizeof(schedule.doctor_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(schedule.work_date, tk, sizeof(schedule.work_date) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(schedule.time_slot, tk, sizeof(schedule.time_slot) - 1); }
        schedule.max_appt = parse_int_token(&cursor);    /* 最大预约数 / Max appointments */
        schedule.max_onsite = parse_int_token(&cursor);  /* 最大现场号 / Max onsite registrations */
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(schedule.status, tk, sizeof(schedule.status) - 1); }

        ScheduleNode *node = create_schedule_node(&schedule);
        if (!node) {
            free_schedule_list(head);
            fclose(fp);
            return NULL;
        }

        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
    }

    fclose(fp);
    return head;
}

int save_schedules_list(ScheduleNode *head) {
    FILE *fp = fopen(SCHEDULES_FILE, "w");
    if (!fp) {
        return ERROR_FILE_IO;
    }

    fprintf(fp, "# schedule_id\tdoctor_id\twork_date\ttime_slot\tmax_appt\tmax_onsite\tstatus\n");
    ScheduleNode *current = head;
    while (current) {
        fprintf_escaped(fp, current->data.schedule_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.doctor_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.work_date); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.time_slot); fprintf(fp, "\t");
        fprintf(fp, "%d\t%d\t", current->data.max_appt, current->data.max_onsite);
        fprintf_escaped(fp, current->data.status); fprintf(fp, "\n");
        current = current->next;
    }

    fclose(fp);
    return SUCCESS;
}

/*
 * [中文] 检查指定医生在指定日期是否有正常排班
 * [English] Check if a specified doctor has normal schedule on a given date
 *
 * 匹配条件 / Match conditions:
 *   doctor_id 匹配 AND work_date 匹配 AND status == "正常"
 */
int has_doctor_schedule(const char *doctor_id, const char *date) {
    ScheduleNode *head = load_schedules_list();
    if (!head) return 0;

    ScheduleNode *current = head;
    while (current) {
        if (strcmp(current->data.doctor_id, doctor_id) == 0 &&
            strcmp(current->data.work_date, date) == 0 &&
            strcmp(current->data.status, "正常") == 0) {
            free_schedule_list(head);
            return 1;
        }
        current = current->next;
    }

    free_schedule_list(head);
    return 0;
}

/* ==========================================================================
 * 第二十六部分：数据加载与保存 — 病历(MedicalRecord)
 * Part 26: Data Load/Save — MedicalRecord
 * ========================================================================== */

MedicalRecordNode* load_medical_records_list(void) {
    FILE *fp = fopen(MEDICAL_RECORDS_FILE, "r");
    if (!fp) {
        return NULL;
    }

    MedicalRecordNode *head = NULL;
    MedicalRecordNode *tail = NULL;
    char line[TEXT_LINE_SIZE];

    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        MedicalRecord record;
        memset(&record, 0, sizeof(record));
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(record.record_id, tk, sizeof(record.record_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(record.patient_id, tk, sizeof(record.patient_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(record.doctor_id, tk, sizeof(record.doctor_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(record.appointment_id, tk, sizeof(record.appointment_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(record.diagnosis, tk, sizeof(record.diagnosis) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(record.diagnosis_date, tk, sizeof(record.diagnosis_date) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(record.status, tk, sizeof(record.status) - 1); }

        MedicalRecordNode *node = create_medical_record_node(&record);
        if (!node) {
            free_medical_record_list(head);
            fclose(fp);
            return NULL;
        }

        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
    }

    fclose(fp);
    return head;
}

int save_medical_records_list(MedicalRecordNode *head) {
    FILE *fp = fopen(MEDICAL_RECORDS_FILE, "w");
    if (!fp) {
        return ERROR_FILE_IO;
    }

    fprintf(fp, "# record_id\tpatient_id\tdoctor_id\tappointment_id\tdiagnosis\tdiagnosis_date\tstatus\n");
    MedicalRecordNode *current = head;
    while (current) {
        fprintf_escaped(fp, current->data.record_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.patient_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.doctor_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.appointment_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.diagnosis); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.diagnosis_date); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.status); fprintf(fp, "\n");
        current = current->next;
    }

    fclose(fp);
    return SUCCESS;
}

/* ==========================================================================
 * 第二十七部分：数据加载与保存 — 处方(Prescription)
 * Part 27: Data Load/Save — Prescription
 * ========================================================================== */

PrescriptionNode* load_prescriptions_list(void) {
    FILE *fp = fopen(PRESCRIPTIONS_FILE, "r");
    if (!fp) {
        return NULL;
    }

    PrescriptionNode *head = NULL;
    PrescriptionNode *tail = NULL;
    char line[TEXT_LINE_SIZE];

    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        Prescription prescription;
        memset(&prescription, 0, sizeof(prescription));
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(prescription.prescription_id, tk, sizeof(prescription.prescription_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(prescription.record_id, tk, sizeof(prescription.record_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(prescription.patient_id, tk, sizeof(prescription.patient_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(prescription.doctor_id, tk, sizeof(prescription.doctor_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(prescription.drug_id, tk, sizeof(prescription.drug_id) - 1); }
        prescription.quantity = parse_int_token(&cursor);
        prescription.total_price = parse_float_token(&cursor);
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(prescription.prescription_date, tk, sizeof(prescription.prescription_date) - 1); }

        PrescriptionNode *node = create_prescription_node(&prescription);
        if (!node) {
            free_prescription_list(head);
            fclose(fp);
            return NULL;
        }

        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
    }

    fclose(fp);
    return head;
}

int save_prescriptions_list(PrescriptionNode *head) {
    FILE *fp = fopen(PRESCRIPTIONS_FILE, "w");
    if (!fp) {
        return ERROR_FILE_IO;
    }

    fprintf(fp, "# prescription_id\trecord_id\tpatient_id\tdoctor_id\tdrug_id\tquantity\ttotal_price\tprescription_date\n");
    PrescriptionNode *current = head;
    while (current) {
        fprintf_escaped(fp, current->data.prescription_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.record_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.patient_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.doctor_id); fprintf(fp, "\t");
        fprintf_escaped(fp, current->data.drug_id); fprintf(fp, "\t");
        fprintf(fp, "%d\t%.2f\t", current->data.quantity, current->data.total_price);
        fprintf_escaped(fp, current->data.prescription_date); fprintf(fp, "\n");
        current = current->next;
    }

    fclose(fp);
    return SUCCESS;
}

/* ==========================================================================
 * 第二十八部分：查找与查询辅助函数
 * Part 28: Find & Query Helper Functions
 * ==========================================================================
 *
 * [中文]
 * 这些函数从文件加载数据链表，遍历查找匹配项，找到后分配新内存复制实体数据
 * 并返回（调用者负责释放），然后释放整个链表。返回的指针是独立副本，
 * 不受后续文件修改影响。
 *
 * [English]
 * These functions load the data linked list from file, traverse to find matches,
 * allocate new memory to copy the entity data (caller is responsible for freeing),
 * then free the entire linked list. The returned pointer is an independent copy,
 * unaffected by subsequent file modifications.
 */

/*
 * [中文] 根据用户名查找患者 / [English] Find patient by username
 */
Patient* find_patient_by_username(const char *username) {
    PatientNode *head = load_patients_list();
    if (!head) {
        return NULL;
    }

    PatientNode *current = head;
    while (current) {
        if (strcmp(current->data.username, username) == 0) {
            Patient *patient = (Patient *)malloc(sizeof(Patient));
            if (patient) {
                *patient = current->data;  /* 复制实体数据 / Copy entity data */
            }
            free_patient_list(head);
            return patient;
        }
        current = current->next;
    }

    free_patient_list(head);
    return NULL;
}

/*
 * [中文] 根据患者ID查找患者 / [English] Find patient by patient ID
 */
Patient* find_patient_by_id(const char *patient_id) {
    PatientNode *head = load_patients_list();
    if (!head) {
        return NULL;
    }

    PatientNode *current = head;
    while (current) {
        if (strcmp(current->data.patient_id, patient_id) == 0) {
            Patient *patient = (Patient *)malloc(sizeof(Patient));
            if (patient) {
                *patient = current->data;
            }
            free_patient_list(head);
            return patient;
        }
        current = current->next;
    }

    free_patient_list(head);
    return NULL;
}

/*
 * [中文] 确保患者档案存在 — 如果不存在则创建默认档案
 * [English] Ensure patient profile exists — create default profile if not found
 *
 * 处理三种情况 / Handles three cases:
 *   1. 患者文件为空 → 创建新患者并保存
 *   2. 患者文件有数据但无此用户 → 追加新患者到链表尾部并保存
 *   3. 患者文件已有此用户 → 直接返回成功，无需任何操作
 */
int ensure_patient_profile(const char *username) {
    PatientNode *head = load_patients_list();
    if (!head) {
        /* 情况1：尚无任何患者数据 / Case 1: No patient data yet */
        Patient new_patient = {0};
        generate_id(new_patient.patient_id, MAX_ID, "P");
        strcpy(new_patient.username, username);
        strcpy(new_patient.name, username);
        strcpy(new_patient.gender, "未知");
        strcpy(new_patient.patient_type, "普通");
        strcpy(new_patient.treatment_stage, "初诊");
        new_patient.is_emergency = false;

        PatientNode *node = create_patient_node(&new_patient);
        if (!node) {
            return ERROR_FILE_IO;
        }

        int result = save_patients_list(node);
        free_patient_list(node);
        return result;
    }

    /* 情况3：查找是否已存在该用户 / Case 3: Check if user already exists */
    PatientNode *current = head;
    while (current) {
        if (strcmp(current->data.username, username) == 0) {
            free_patient_list(head);
            return SUCCESS;
        }
        current = current->next;
    }

    /* 情况2：不存在，追加新患者 / Case 2: Not found, append new patient */
    Patient new_patient = {0};
    generate_id(new_patient.patient_id, MAX_ID, "P");
    strcpy(new_patient.username, username);
    strcpy(new_patient.name, username);
    strcpy(new_patient.gender, "未知");
    strcpy(new_patient.patient_type, "普通");
    strcpy(new_patient.treatment_stage, "初诊");
    new_patient.is_emergency = false;

    PatientNode *node = create_patient_node(&new_patient);
    if (!node) {
        free_patient_list(head);
        return ERROR_FILE_IO;
    }

    /* 找到链表尾部，追加新节点 / Find tail of linked list, append new node */
    PatientNode *tail = head;
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = node;

    int result = save_patients_list(head);
    free_patient_list(head);
    return result;
}

/*
 * [中文] 根据用户名查找医生 / [English] Find doctor by username
 */
Doctor* find_doctor_by_username(const char *username) {
    DoctorNode *head = load_doctors_list();
    if (!head) {
        return NULL;
    }

    DoctorNode *current = head;
    while (current) {
        if (strcmp(current->data.username, username) == 0) {
            Doctor *doctor = (Doctor *)malloc(sizeof(Doctor));
            if (doctor) {
                *doctor = current->data;
            }
            free_doctor_list(head);
            return doctor;
        }
        current = current->next;
    }

    free_doctor_list(head);
    return NULL;
}

/*
 * [中文] 根据医生ID查找医生 / [English] Find doctor by doctor ID
 */
Doctor* find_doctor_by_id(const char *doctor_id) {
    DoctorNode *head = load_doctors_list();
    if (!head) {
        return NULL;
    }

    DoctorNode *current = head;
    while (current) {
        if (strcmp(current->data.doctor_id, doctor_id) == 0) {
            Doctor *doctor = (Doctor *)malloc(sizeof(Doctor));
            if (doctor) {
                *doctor = current->data;
            }
            free_doctor_list(head);
            return doctor;
        }
        current = current->next;
    }

    free_doctor_list(head);
    return NULL;
}

/* ==========================================================================
 * 第二十九部分：医生 ID 生成 —— 科室字母前缀 + 4位序号
 * Part 29: Doctor ID Generation — Department Letter Prefix + 4-Digit Sequence
 * ==========================================================================
 *
 * [中文]
 * 医生 ID 格式：一个字母 + 4位数字（如 A0001, B0003, X0001）
 *
 * 生成算法 / Generation algorithm:
 *   1. 根据 department_id 在科室列表中查找对应科室
 *   2. 科室在列表中的索引决定字母：第0个→A，第1个→B，...，最多到Z（25）
 *   3. 查找所有已存在的医生，统计同字母前缀的最大序号
 *   4. 新 ID = 字母 + (max_seq + 1) 格式化为4位零填充
 *   5. 如果 department_id 无效或科室不在列表中，默认使用字母 'X'
 *
 * [English]
 * Doctor ID format: one letter + 4 digits (e.g., A0001, B0003, X0001)
 *
 * Generation algorithm:
 *   1. Find the department matching department_id in the department list
 *   2. Department index in list determines the letter: 0→A, 1→B, ..., max Z (25)
 *   3. Scan all existing doctors, find max sequence number with the same letter prefix
 *   4. New ID = letter + (max_seq + 1) formatted as 4-digit zero-padded
 *   5. If department_id is invalid or department not in list, default to letter 'X'
 */
void generate_doctor_id(const char *department_id, char *out_buf, int buf_size) {
    char dept_letter = 'X';  /* 默认字母 / Default letter */
    int max_seq = 0;

    /* 步骤1-2：根据科室确定字母前缀 / Steps 1-2: Determine letter prefix from department */
    if (department_id && department_id[0] != '\0') {
        DepartmentNode *dept_head = load_departments_list();
        DepartmentNode *dc = dept_head;
        int letter_idx = 0;
        while (dc) {
            if (strcmp(dc->data.department_id, department_id) == 0) {
                dept_letter = (char)('A' + letter_idx);  /* 索引映射为字母 / Map index to letter */
                break;
            }
            letter_idx++;
            if (letter_idx > 25) letter_idx = 25;  /* 最多 Z / Cap at Z */
            dc = dc->next;
        }
        free_department_list(dept_head);
    }

    /* 步骤3：查找同字母前缀的最大序号 / Step 3: Find max sequence with same letter prefix */
    {
        DoctorNode *dh = load_doctors_list();
        DoctorNode *dcur = dh;
        while (dcur) {
            if (dcur->data.doctor_id[0] == dept_letter) {
                int seq = (int)strtol(dcur->data.doctor_id + 1, NULL, 10);  /* 跳过首字母解析数字 / Skip first letter, parse digits */
                if (seq > max_seq) max_seq = seq;
            }
            dcur = dcur->next;
        }
        free_doctor_list(dh);
    }

    /* 步骤4：格式化为 X0001 格式 / Step 4: Format as X0001 */
    snprintf(out_buf, buf_size, "%c%04d", dept_letter, max_seq + 1);
}

/* ==========================================================================
 * 第三十部分：医生 ID 迁移 — 旧版长 ID 转换为新版短格式
 * Part 30: Doctor ID Migration — Convert Old Long IDs to New Short Format
 * ==========================================================================
 *
 * [中文]
 * ID 迁移算法 / ID Migration Algorithm:
 *
 * 背景：
 *   旧版系统中医生 ID 可能是任意长度的字符串。新版统一使用 "字母+4位数字"
 *   格式（如 A0001）。此函数将旧版长 ID 转换为新版短格式，并同步更新所有
 *   引用该医生 ID 的其他数据文件。
 *
 * 步骤概览 / Step overview:
 *   1. 遍历所有医生，找出 ID 长度 > 5 的（即旧版长 ID）
 *   2. 对每个需要迁移的医生：
 *      a. 根据其所属科室确定字母前缀（同 generate_doctor_id 逻辑）
 *      b. 查找同字母前缀的最大序号，生成新 ID
 *      c. 将 (旧ID → 新ID) 映射关系存入 old_to_new 表
 *      d. 更新医生自身的 doctor_id
 *   3. 保存更新后的医生列表
 *   4. 遍历所有关联文件（预约、现场挂号、病历、处方），查找引用旧 ID 的
 *      记录，替换为新 ID，然后保存。
 *
 * 关联文件 / Cross-referenced files updated:
 *   - appointments.txt（预约）
 *   - onsite_registrations.txt（现场挂号）
 *   - medical_records.txt（病历）
 *   - prescriptions.txt（处方）
 *
 * [English]
 * ID Migration Algorithm:
 *
 * Background:
 *   Old system may have doctor IDs of arbitrary length. New system uniformly uses
 *   "letter + 4-digit" format (e.g., A0001). This function converts old long IDs
 *   to new short format and updates all cross-referenced data files.
 *
 * Step overview:
 *   1. Iterate all doctors, identify those with ID length > 5 (old long IDs)
 *   2. For each doctor needing migration:
 *      a. Determine letter prefix from their department (same logic as generate_doctor_id)
 *      b. Find max sequence with same letter prefix, generate new ID
 *      c. Store (old ID → new ID) mapping in old_to_new table
 *      d. Update the doctor's own doctor_id
 *   3. Save the updated doctor list
 *   4. Iterate all cross-referenced files (appointments, onsite registrations,
 *      medical records, prescriptions), find records referencing old IDs,
 *      replace with new IDs, and save.
 */
void migrate_doctor_ids(void) {
    DoctorNode *dh = load_doctors_list();
    DoctorNode *doc = dh;
    int changed = 0;
    /* 旧→新 ID 映射表，最多500条 / Old→New ID mapping table, max 500 entries */
    char old_to_new[500][2][MAX_ID];
    int map_count = 0;

    if (!dh) return;

    /* 步骤1-2：遍历医生，迁移长 ID / Steps 1-2: Iterate doctors, migrate long IDs */
    while (doc) {
        int id_len = (int)strlen(doc->data.doctor_id);
        if (id_len > 5) {  /* 旧版长 ID 判断 / Old long ID detection */
            char new_id[MAX_ID];

            /* 步骤2a：确定字母前缀 / Step 2a: Determine letter prefix */
            DoctorNode *dh2 = load_doctors_list();
            DoctorNode *tmp = dh2;
            int dept_idx = 0;
            if (doc->data.department_id[0] != '\0') {
                DepartmentNode *dept_head = load_departments_list();
                DepartmentNode *dc = dept_head;
                while (dc) {
                    if (strcmp(dc->data.department_id, doc->data.department_id) == 0) break;
                    dept_idx++;
                    if (dept_idx > 25) dept_idx = 25;
                    dc = dc->next;
                }
                free_department_list(dept_head);
            }

            /* 步骤2b：查找最大序号 / Step 2b: Find max sequence number */
            char letter = doc->data.department_id[0] ? (char)('A' + dept_idx) : 'X';
            int max_s = 0;
            while (tmp) {
                if (strcmp(tmp->data.doctor_id, doc->data.doctor_id) == 0) {
                    tmp = tmp->next;
                    continue;  /* 跳过自身 / Skip self */
                }
                if (tmp->data.doctor_id[0] == letter) {
                    int s = (int)strtol(tmp->data.doctor_id + 1, NULL, 10);
                    if (s > max_s) max_s = s;
                }
                tmp = tmp->next;
            }
            free_doctor_list(dh2);

            snprintf(new_id, sizeof(new_id), "%c%04d", letter, max_s + 1);

            /* 步骤2c：记录映射关系 / Step 2c: Record mapping */
            if (map_count < 200) {
                strcpy(old_to_new[map_count][0], doc->data.doctor_id);
                strcpy(old_to_new[map_count][1], new_id);
                map_count++;
            }

            /* 步骤2d：更新医生自身 ID / Step 2d: Update doctor's own ID */
            strcpy(doc->data.doctor_id, new_id);
            changed = 1;
        }
        doc = doc->next;
    }

    if (changed) {
        /* 步骤3：保存医生列表 / Step 3: Save doctor list */
        save_doctors_list(dh);

        /* 步骤4a：更新预约文件中的医生 ID 引用 / Step 4a: Update doctor ID refs in appointments */
        {
            AppointmentNode *ah = load_appointments_list();
            AppointmentNode *ac = ah;
            while (ac) {
                int i;
                for (i = 0; i < map_count; i++) {
                    if (strcmp(ac->data.doctor_id, old_to_new[i][0]) == 0) {
                        strcpy(ac->data.doctor_id, old_to_new[i][1]);
                        break;
                    }
                }
                ac = ac->next;
            }
            save_appointments_list(ah);
            free_appointment_list(ah);
        }

        /* 步骤4b：更新现场挂号文件中的医生 ID 引用 / Step 4b: Update doctor ID refs in onsite registrations */
        {
            OnsiteRegistrationQueue oq = load_onsite_registration_queue();
            OnsiteRegistrationNode *oc = oq.front;
            while (oc) {
                int i;
                for (i = 0; i < map_count; i++) {
                    if (strcmp(oc->data.doctor_id, old_to_new[i][0]) == 0) {
                        strcpy(oc->data.doctor_id, old_to_new[i][1]);
                        break;
                    }
                }
                oc = oc->next;
            }
            save_onsite_registration_queue(&oq);
            free_onsite_registration_queue(&oq);
        }

        /* 步骤4c：更新病历文件中的医生 ID 引用 / Step 4c: Update doctor ID refs in medical records */
        {
            MedicalRecordNode *mh = load_medical_records_list();
            MedicalRecordNode *mc = mh;
            while (mc) {
                int i;
                for (i = 0; i < map_count; i++) {
                    if (strcmp(mc->data.doctor_id, old_to_new[i][0]) == 0) {
                        strcpy(mc->data.doctor_id, old_to_new[i][1]);
                        break;
                    }
                }
                mc = mc->next;
            }
            save_medical_records_list(mh);
            free_medical_record_list(mh);
        }

        /* 步骤4d：更新处方文件中的医生 ID 引用 / Step 4d: Update doctor ID refs in prescriptions */
        {
            PrescriptionNode *ph = load_prescriptions_list();
            PrescriptionNode *pc = ph;
            while (pc) {
                int i;
                for (i = 0; i < map_count; i++) {
                    if (strcmp(pc->data.doctor_id, old_to_new[i][0]) == 0) {
                        strcpy(pc->data.doctor_id, old_to_new[i][1]);
                        break;
                    }
                }
                pc = pc->next;
            }
            save_prescriptions_list(ph);
            free_prescription_list(ph);
        }
    }

    free_doctor_list(dh);
}

/*
 * [中文] 跨文件更新医生 ID — 当医生更换科室时，更新所有关联文件中的引用
 * [English] Update doctor ID across files — when a doctor changes department,
 *          update references in all related files
 *
 * 与 migrate_doctor_ids 的区别 / Difference from migrate_doctor_ids:
 *   - migrate_doctor_ids: 批量迁移所有旧版长 ID
 *   - update_doctor_id_across_files: 单个医生的 ID 变更（如科室变动导致 ID 变化）
 *
 * 更新的文件 / Files updated:
 *   - appointments.txt, onsite_registrations.txt, medical_records.txt, prescriptions.txt
 */
void update_doctor_id_across_files(const char *old_id, const char *new_id) {
    {
        AppointmentNode *ah = load_appointments_list();
        AppointmentNode *ac = ah;
        while (ac) {
            if (strcmp(ac->data.doctor_id, old_id) == 0)
                strcpy(ac->data.doctor_id, new_id);
            ac = ac->next;
        }
        save_appointments_list(ah);
        free_appointment_list(ah);
    }
    {
        OnsiteRegistrationQueue oq = load_onsite_registration_queue();
        OnsiteRegistrationNode *oc = oq.front;
        while (oc) {
            if (strcmp(oc->data.doctor_id, old_id) == 0)
                strcpy(oc->data.doctor_id, new_id);
            oc = oc->next;
        }
        save_onsite_registration_queue(&oq);
        free_onsite_registration_queue(&oq);
    }
    {
        MedicalRecordNode *mh = load_medical_records_list();
        MedicalRecordNode *mc = mh;
        while (mc) {
            if (strcmp(mc->data.doctor_id, old_id) == 0)
                strcpy(mc->data.doctor_id, new_id);
            mc = mc->next;
        }
        save_medical_records_list(mh);
        free_medical_record_list(mh);
    }
    {
        PrescriptionNode *ph = load_prescriptions_list();
        PrescriptionNode *pc = ph;
        while (pc) {
            if (strcmp(pc->data.doctor_id, old_id) == 0)
                strcpy(pc->data.doctor_id, new_id);
            pc = pc->next;
        }
        save_prescriptions_list(ph);
        free_prescription_list(ph);
    }
}

/*
 * [中文] 确保医生档案存在 — 如果不存在则创建默认档案
 * [English] Ensure doctor profile exists — create default profile if not found
 */
int ensure_doctor_profile(const char *username) {
    DoctorNode *head = load_doctors_list();
    if (!head) {
        /* 尚无任何医生数据 / No doctor data yet */
        Doctor new_doctor = {0};
        generate_doctor_id("", new_doctor.doctor_id, MAX_ID);  /* 无科室 → 默认 X 开头 / No dept → defaults to X prefix */
        strcpy(new_doctor.username, username);
        strcpy(new_doctor.name, username);
        strcpy(new_doctor.title, "医生");

        DoctorNode *node = create_doctor_node(&new_doctor);
        if (!node) {
            return ERROR_FILE_IO;
        }

        int result = save_doctors_list(node);
        free_doctor_list(node);
        return result;
    }

    /* 检查是否已存在 / Check if already exists */
    DoctorNode *current = head;
    while (current) {
        if (strcmp(current->data.username, username) == 0) {
            free_doctor_list(head);
            return SUCCESS;
        }
        current = current->next;
    }

    /* 追加新医生 / Append new doctor */
    Doctor new_doctor = {0};
    generate_doctor_id("", new_doctor.doctor_id, MAX_ID);
    strcpy(new_doctor.username, username);
    strcpy(new_doctor.name, username);
    strcpy(new_doctor.title, "医生");

    DoctorNode *node = create_doctor_node(&new_doctor);
    if (!node) {
        free_doctor_list(head);
        return ERROR_FILE_IO;
    }

    /* 找到链表尾部，追加 / Find tail, append */
    DoctorNode *tail = head;
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = node;

    int result = save_doctors_list(head);
    free_doctor_list(head);
    return result;
}

/* 批量确保医生档案: 一次加载数据，检查所有医生用户，追加缺失项
   Batch ensure doctor profiles: load once, check all doc users, append missing */
void batch_ensure_doctor_profiles(UserNode *user_list) {
    DoctorNode *head = load_doctors_list();

    int changed = 0;
    for (UserNode *u = user_list; u; u = u->next) {
        if (strcmp(u->data.role, ROLE_DOCTOR) != 0)
            continue;

        int found = 0;
        for (DoctorNode *d = head; d; d = d->next) {
            if (strcmp(d->data.username, u->data.username) == 0) {
                found = 1; break;
            }
        }
        if (found) continue;

        Doctor new_doctor = {0};
        generate_doctor_id("", new_doctor.doctor_id, MAX_ID);
        strcpy(new_doctor.username, u->data.username);
        strcpy(new_doctor.name, u->data.username);
        strcpy(new_doctor.title, "医生");

        DoctorNode *node = create_doctor_node(&new_doctor);
        if (!node) continue;

        if (!head) {
            head = node;
        } else {
            DoctorNode *tail = head;
            while (tail->next) tail = tail->next;
            tail->next = node;
        }
        changed = 1;
    }

    if (changed && head) save_doctors_list(head);
    if (head) free_doctor_list(head);
}

/* 批量确保患者档案 / Batch ensure patient profiles */
void batch_ensure_patient_profiles(UserNode *user_list) {
    PatientNode *head = load_patients_list();

    int changed = 0;
    for (UserNode *u = user_list; u; u = u->next) {
        if (strcmp(u->data.role, ROLE_PATIENT) != 0)
            continue;

        int found = 0;
        for (PatientNode *p = head; p; p = p->next) {
            if (strcmp(p->data.username, u->data.username) == 0) {
                found = 1; break;
            }
        }
        if (found) continue;

        Patient new_patient = {0};
        generate_id(new_patient.patient_id, MAX_ID, "P");
        strcpy(new_patient.username, u->data.username);
        strcpy(new_patient.name, u->data.username);
        strcpy(new_patient.gender, "未知");
        strcpy(new_patient.patient_type, "普通");
        strcpy(new_patient.treatment_stage, "初诊");
        new_patient.is_emergency = false;

        PatientNode *node = create_patient_node(&new_patient);
        if (!node) continue;

        if (!head) {
            head = node;
        } else {
            PatientNode *tail = head;
            while (tail->next) tail = tail->next;
            tail->next = node;
        }
        changed = 1;
    }

    if (changed && head) save_patients_list(head);
    if (head) free_patient_list(head);
}

/*
 * [中文] 创建带详细信息的医生档案（用户名、姓名、职称、科室）
 * [English] Create doctor profile with detailed info (username, name, title, department)
 *
 * 与 ensure_doctor_profile 的区别 / Difference from ensure_doctor_profile:
 *   此函数允许在创建时指定完整的医生信息（姓名、职称、科室），
 *   而 ensure_doctor_profile 只使用默认值。
 */
int create_doctor_profile_with_details(const char *username, const char *name, const char *title, const char *department_id) {
    DoctorNode *head = load_doctors_list();
    DoctorNode *current = head;

    /* 如果已存在则直接返回 / Return if already exists */
    while (current) {
        if (strcmp(current->data.username, username) == 0) {
            free_doctor_list(head);
            return SUCCESS;
        }
        current = current->next;
    }

    /* 创建新医生 / Create new doctor */
    Doctor new_doctor = {0};
    generate_doctor_id(department_id, new_doctor.doctor_id, MAX_ID);  /* 基于科室生成 ID / Generate ID based on department */
    strcpy(new_doctor.username, username);
    strcpy(new_doctor.name, name);
    strcpy(new_doctor.title, title);
    if (department_id && department_id[0] != '\0') {
        strcpy(new_doctor.department_id, department_id);
    }

    DoctorNode *node = create_doctor_node(&new_doctor);
    if (!node) {
        if (head) free_doctor_list(head);
        return ERROR_FILE_IO;
    }

    if (!head) {
        int result = save_doctors_list(node);
        free_doctor_list(node);
        return result;
    }

    /* 追加到链表尾部 / Append to tail of list */
    DoctorNode *tail = head;
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = node;

    int result = save_doctors_list(head);
    free_doctor_list(head);
    return result;
}

/*
 * [中文] 根据药品ID查找药品 / [English] Find drug by drug ID
 */
Drug* find_drug_by_id(const char *drug_id) {
    DrugNode *head = load_drugs_list();
    if (!head) {
        return NULL;
    }

    DrugNode *current = head;
    while (current) {
        if (strcmp(current->data.drug_id, drug_id) == 0) {
            Drug *drug = (Drug *)malloc(sizeof(Drug));
            if (drug) {
                *drug = current->data;
            }
            free_drug_list(head);
            return drug;
        }
        current = current->next;
    }

    free_drug_list(head);
    return NULL;
}

/*
 * [中文] 根据患者ID查找预约（返回第一个匹配项）
 * [English] Find appointment by patient ID (returns first match)
 */
Appointment* find_appointments_by_patient(const char *patient_id) {
    AppointmentNode *head = load_appointments_list();
    if (!head) {
        return NULL;
    }

    AppointmentNode *current = head;
    while (current) {
        if (strcmp(current->data.patient_id, patient_id) == 0) {
            Appointment *appointment = (Appointment *)malloc(sizeof(Appointment));
            if (appointment) {
                *appointment = current->data;
            }
            free_appointment_list(head);
            return appointment;
        }
        current = current->next;
    }

    free_appointment_list(head);
    return NULL;
}

/*
 * [中文] 根据医生ID查找预约（返回第一个匹配项）
 * [English] Find appointment by doctor ID (returns first match)
 */
Appointment* find_appointments_by_doctor(const char *doctor_id) {
    AppointmentNode *head = load_appointments_list();
    if (!head) {
        return NULL;
    }

    AppointmentNode *current = head;
    while (current) {
        if (strcmp(current->data.doctor_id, doctor_id) == 0) {
            Appointment *appointment = (Appointment *)malloc(sizeof(Appointment));
            if (appointment) {
                *appointment = current->data;
            }
            free_appointment_list(head);
            return appointment;
        }
        current = current->next;
    }

    free_appointment_list(head);
    return NULL;
}

/*
 * [中文] 根据患者ID查找病历（返回第一个匹配项）
 * [English] Find medical record by patient ID (returns first match)
 */
MedicalRecord* find_records_by_patient(const char *patient_id) {
    MedicalRecordNode *head = load_medical_records_list();
    if (!head) {
        return NULL;
    }

    MedicalRecordNode *current = head;
    while (current) {
        if (strcmp(current->data.patient_id, patient_id) == 0) {
            MedicalRecord *record = (MedicalRecord *)malloc(sizeof(MedicalRecord));
            if (record) {
                *record = current->data;
            }
            free_medical_record_list(head);
            return record;
        }
        current = current->next;
    }

    free_medical_record_list(head);
    return NULL;
}

/* ==========================================================================
 * 第三十一部分：医疗模板(Template)操作 — 创建/释放/加载/保存/默认初始化
 * Part 31: Medical Template Operations — Create/Free/Load/Save/Default Init
 * ==========================================================================
 *
 * [中文]
 * 医疗模板用于快速填充诊断、治疗方案、检查项目等。系统预置18个默认模板，
 * 覆盖6种常见疾病（上呼吸道感染、急性胃肠炎、高血压、糖尿病、肺炎、皮炎），
 * 每种疾病包含诊断、治疗、检查三类模板。
 *
 * [English]
 * Medical templates are used for quick-filling diagnosis, treatment plans,
 * exam items, etc. The system preloads 18 default templates covering 6 common
 * conditions (URI, acute gastroenteritis, hypertension, diabetes, pneumonia,
 * dermatitis), each with diagnosis, treatment, and exam templates.
 */

TemplateNode* create_template_node(const MedicalTemplate *tmpl) {
    TemplateNode *node = (TemplateNode *)malloc(sizeof(TemplateNode));
    if (node) {
        node->data = *tmpl;
        node->next = NULL;
    }
    return node;
}

void free_template_list(TemplateNode *head) {
    TemplateNode *current = head;
    while (current) {
        TemplateNode *next = current->next;
        free(current);
        current = next;
    }
}

TemplateNode* load_templates_list(void) {
    FILE *fp = fopen(TEMPLATES_FILE, "r");
    if (!fp) return NULL;
    TemplateNode *head = NULL, *tail = NULL;
    char line[TEXT_LINE_SIZE];
    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        MedicalTemplate tmpl;
        memset(&tmpl, 0, sizeof(tmpl));
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(tmpl.template_id, tk, sizeof(tmpl.template_id) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(tmpl.category, tk, sizeof(tmpl.category) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(tmpl.shortcut, tk, sizeof(tmpl.shortcut) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(tmpl.text, tk, sizeof(tmpl.text) - 1); }
        TemplateNode *node = create_template_node(&tmpl);
        if (!node) { free_template_list(head); fclose(fp); return NULL; }
        if (!head) { head = node; tail = node; }
        else { tail->next = node; tail = node; }
    }
    fclose(fp);
    return head;
}

int save_templates_list(TemplateNode *head) {
    FILE *fp = fopen(TEMPLATES_FILE, "w");
    if (!fp) return ERROR_FILE_IO;
    fprintf(fp, "# template_id\tcategory\tshortcut\ttext\n");
    TemplateNode *cur = head;
    while (cur) {
        fprintf_escaped(fp, cur->data.template_id); fprintf(fp, "\t");
        fprintf_escaped(fp, cur->data.category); fprintf(fp, "\t");
        fprintf_escaped(fp, cur->data.shortcut); fprintf(fp, "\t");
        fprintf_escaped(fp, cur->data.text); fprintf(fp, "\n");
        cur = cur->next;
    }
    fclose(fp);
    return SUCCESS;
}

/*
 * [中文] 初始化默认模板 — 仅当模板文件为空时写入18个预置模板
 * [English] Initialize default templates — write 18 preset templates only if file is empty
 *
 * 模板编号 / Template IDs:
 *   T001-T003: 上呼吸道感染（诊断、治疗、检查） / URI (diagnosis, treatment, exam)
 *   T004-T006: 急性胃肠炎 / Acute gastroenteritis
 *   T007-T009: 高血压 / Hypertension
 *   T010-T012: 糖尿病 / Diabetes
 *   T013-T015: 肺炎 / Pneumonia
 *   T016-T018: 皮炎 / Dermatitis
 */
int ensure_default_templates(void) {
    TemplateNode *existing = load_templates_list();
    if (existing) { free_template_list(existing); return SUCCESS; }  /* 已有模板，跳过 / Already have templates, skip */

    MedicalTemplate defaults[] = {
        {"T001", "诊断", "上呼吸道感染", "急性上呼吸道感染，伴发热、咳嗽、咽痛"},
        {"T002", "治疗", "上呼吸道感染", "口服抗生素3天，多饮水，注意休息，复查体温"},
        {"T003", "检查", "上感套餐", "血常规、CRP"},
        {"T004", "诊断", "急性胃肠炎", "急性胃肠炎，伴腹痛、腹泻、恶心呕吐"},
        {"T005", "治疗", "急性胃肠炎", "口服补液盐，蒙脱石散，清淡饮食3天"},
        {"T006", "检查", "胃肠套餐", "血常规、粪便常规"},
        {"T007", "诊断", "高血压", "原发性高血压，测血压升高"},
        {"T008", "治疗", "高血压", "降压药物治疗，低盐低脂饮食，定期监测血压"},
        {"T009", "检查", "高血压套餐", "血压动态监测、血脂四项、心电图"},
        {"T010", "诊断", "糖尿病", "2型糖尿病，血糖控制不佳"},
        {"T011", "治疗", "糖尿病", "口服降糖药/胰岛素，控制饮食，适量运动"},
        {"T012", "检查", "糖尿病套餐", "空腹血糖、糖化血红蛋白、尿微量白蛋白"},
        {"T013", "诊断", "肺炎", "肺部感染，伴咳嗽咳痰、发热"},
        {"T014", "治疗", "肺炎", "抗生素治疗7天，祛痰止咳，必要时住院"},
        {"T015", "检查", "肺部套餐", "胸片、血常规、CRP、痰培养"},
        {"T016", "诊断", "皮炎", "皮肤炎症，局部红肿瘙痒"},
        {"T017", "治疗", "皮炎", "外用激素药膏，口服抗组胺药，避免刺激"},
        {"T018", "检查", "皮肤套餐", "皮损检查、过敏原筛查"},
    };
    int count = sizeof(defaults) / sizeof(defaults[0]);
    TemplateNode *head = NULL, *tail = NULL;
    int i;
    for (i = 0; i < count; i++) {
        TemplateNode *node = create_template_node(&defaults[i]);
        if (!node) { free_template_list(head); return ERROR_FILE_IO; }
        if (!head) { head = node; tail = node; }
        else { tail->next = node; tail = node; }
    }
    int result = save_templates_list(head);
    free_template_list(head);
    return result;
}

/* ==========================================================================
 * 第三十二部分：操作日志(LogEntry) — 追加式操作记录
 * Part 32: Operation Log (LogEntry) — Append-Only Operation Recording
 * ==========================================================================
 *
 * [中文]
 * 操作日志用于记录所有增删改操作，采用追加写入模式（fopen(..., "a")）。
 * 每条日志包含：操作人、操作类型、操作目标、目标ID、详情、时间戳。
 * 日志文件首次写入时自动添加列标题行。
 *
 * [English]
 * Operation log records all CRUD operations using append mode (fopen(..., "a")).
 * Each log entry contains: operator, action, target, target ID, detail, timestamp.
 * The column header line is automatically added on first write.
 */

LogEntryNode* create_log_entry_node(const LogEntry *entry) {
    LogEntryNode *node = malloc(sizeof(LogEntryNode));
    if (!node) return NULL;
    node->data = *entry;
    node->next = NULL;
    return node;
}

void free_log_entry_list(LogEntryNode *head) {
    while (head) {
        LogEntryNode *next = head->next;
        free(head);
        head = next;
    }
}

int count_log_entry_list(LogEntryNode *head) {
    int count = 0;
    while (head) { count++; head = head->next; }
    return count;
}

LogEntryNode* load_logs_list(void) {
    FILE *fp = fopen(LOGS_FILE, "r");
    if (!fp) return NULL;

    LogEntryNode *head = NULL, *tail = NULL;
    char line[TEXT_LINE_SIZE];
    while (read_data_line(fp, line, sizeof(line))) {
        char *cursor = line;
        LogEntry entry;
        memset(&entry, 0, sizeof(entry));
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(entry.log_id, tk, MAX_ID - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(entry.operator_name, tk, MAX_USERNAME - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(entry.action, tk, sizeof(entry.action) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(entry.target, tk, sizeof(entry.target) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(entry.target_id, tk, MAX_ID - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(entry.detail, tk, sizeof(entry.detail) - 1); }
        { char *tk = next_token(&cursor); unescape_field_inplace(tk); strncpy(entry.create_time, tk, sizeof(entry.create_time) - 1); }

        LogEntryNode *node = create_log_entry_node(&entry);
        if (!node) { free_log_entry_list(head); fclose(fp); return NULL; }
        if (!tail) { head = node; tail = node; }
        else { tail->next = node; tail = node; }
    }

    fclose(fp);
    return head;
}

/*
 * [中文] 追加一条操作日志到日志文件
 * [English] Append one operation log entry to the log file
 *
 * 实现说明 / Implementation notes:
 *   - 以追加模式 "a" 打开文件（不清除已有内容） / Open in append mode "a" (preserves existing content)
 *   - fseek + ftell 检查文件是否为空，空文件则写入列标题 / Check if file is empty via fseek+ftell, write header if so
 *   - 自动生成日志 ID（L 前缀）和当前时间戳 / Auto-generate log ID (L prefix) and current timestamp
 */
int append_log(const char *operator_name, const char *action, const char *target,
               const char *target_id, const char *detail) {
    FILE *fp = fopen(LOGS_FILE, "a");
    if (!fp) return ERROR_FILE_IO;

    /* 如果文件为空（首次写入），先写列标题行 */
    /* If file is empty (first write), write column header line first */
    fseek(fp, 0, SEEK_END);
    if (ftell(fp) == 0) {
        fprintf(fp, "# log_id\toperator\taction\ttarget\ttarget_id\tdetail\tcreate_time\n");
    }

    char log_id[MAX_ID];
    generate_id(log_id, MAX_ID, "L");  /* 日志ID以L开头 / Log ID prefixed with L */
    char create_time[30];
    get_current_time(create_time, sizeof(create_time));

    fprintf_escaped(fp, log_id); fprintf(fp, "\t");
    fprintf_escaped(fp, operator_name); fprintf(fp, "\t");
    fprintf_escaped(fp, action); fprintf(fp, "\t");
    fprintf_escaped(fp, target); fprintf(fp, "\t");
    fprintf_escaped(fp, target_id); fprintf(fp, "\t");
    fprintf_escaped(fp, detail); fprintf(fp, "\t");
    fprintf_escaped(fp, create_time); fprintf(fp, "\n");
    fclose(fp);
    return SUCCESS;
}

/* ==========================================================================
 * 第三十三部分：数据备份与恢复
 * Part 33: Data Backup and Restore
 * ==========================================================================
 *
 * [中文]
 * 备份机制 / Backup Mechanism:
 *
 *   1. 备份时创建一个以时间戳命名的子目录（格式：YYYYMMDD_HHMMSS）
 *     位于 data/backup/ 下
 *   2. 将所有14个数据文件逐一复制到备份目录
 *   3. 备份完成后自动检查备份数量；如果超过 MAX_BACKUPS(20) 个，
 *      删除最旧的备份目录（按名称字典序最小）
 *
 *   还原机制 / Restore Mechanism:
 *   1. 指定备份目录名，将目录中的所有文件逐一复制回 data/ 目录
 *   2. 覆盖当前数据文件
 *
 *   文件列表 / File list (14 files):
 *     users.txt, patients.txt, doctors.txt, departments.txt,
 *     drugs.txt, wards.txt, appointments.txt, onsite_registrations.txt,
 *     ward_calls.txt, medical_records.txt, prescriptions.txt,
 *     templates.txt, schedules.txt, logs.txt
 *
 * [English]
 * Backup Mechanism:
 *
 *   1. Creates a timestamped subdirectory (format: YYYYMMDD_HHMMSS)
 *      under data/backup/
 *   2. Copies all 14 data files one by one into the backup directory
 *   3. After backup, checks backup count; if exceeding MAX_BACKUPS(20),
 *      deletes the oldest backup directory (smallest lexicographic name)
 *
 * Restore Mechanism:
 *   1. Given a backup directory name, copies all files back to data/ directory
 *   2. Overwrites current data files
 */

/* 需要备份的所有数据文件名列表 / List of all data file names to back up */
static const char *backup_data_files[] = {
    "users.txt", "patients.txt", "doctors.txt", "departments.txt",
    "drugs.txt", "wards.txt", "appointments.txt", "onsite_registrations.txt",
    "ward_calls.txt", "medical_records.txt", "prescriptions.txt",
    "templates.txt", "schedules.txt", "logs.txt"
};
static const int backup_file_count = sizeof(backup_data_files) / sizeof(backup_data_files[0]);
#define MAX_BACKUPS 20  /* 最大备份保留数 / Maximum number of backups to retain */

/*
 * [中文] 清理最旧的备份 — 当备份数量超过 MAX_BACKUPS 时删除最旧的
 * [English] Remove oldest backup — when backup count exceeds MAX_BACKUPS
 *
 * 算法 / Algorithm:
 *   1. 遍历 data/backup/ 下的所有子目录
 *   2. 统计子目录数量
 *   3. 记录名称字典序最小的目录（即时间戳最早，因为命名格式是 YYYYMMDD_HHMMSS）
 *   4. 如果 count > MAX_BACKUPS，删除最旧备份目录中的所有文件，然后删除目录
 *
 * 注意：Windows 和 Unix 实现分开；Windows 使用 _findfirst/_findnext，
 *       Unix 使用 opendir/readdir（此处为简化版本）。
 */
#ifdef _WIN32
#include <io.h>
static void remove_oldest_backup(void) {
    struct _finddata_t fd;
    intptr_t handle = _findfirst(DATA_DIR "/backup/*", &fd);
    if (handle == -1L) return;

    char oldest[64] = "";
    int count = 0;
    /* 遍历备份目录，统计数量并找到最旧的 / Iterate backups, count and find oldest */
    do {
        if (fd.attrib & _A_SUBDIR) {
            if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
            count++;
            /* 字典序最小 = 时间戳最早 / Lexicographically smallest = earliest timestamp */
            if (oldest[0] == '\0' || strcmp(fd.name, oldest) < 0)
                strncpy(oldest, fd.name, sizeof(oldest) - 1);
        }
    } while (_findnext(handle, &fd) == 0);
    _findclose(handle);

    /* 超过上限，删除最旧的 / Exceeded limit, remove oldest */
    if (count > MAX_BACKUPS && oldest[0]) {
        char path[256];
        snprintf(path, sizeof(path), DATA_DIR "/backup/%s", oldest);

        /* 先删除备份目录内的所有文件 / First delete all files in the backup directory */
        char search[256];
        snprintf(search, sizeof(search), "%s/*", path);
        struct _finddata_t ffd;
        intptr_t fh = _findfirst(search, &ffd);
        if (fh != -1L) {
            do {
                if (strcmp(ffd.name, ".") != 0 && strcmp(ffd.name, "..") != 0) {
                    char fp[512];
                    snprintf(fp, sizeof(fp), "%s/%s", path, ffd.name);
                    remove(fp);
                }
            } while (_findnext(fh, &ffd) == 0);
            _findclose(fh);
        }
        _rmdir(path);  /* 删除空目录 / Remove the empty directory */
        printf("  ⚠ 已清理最旧备份: %s (超过 %d 个限制)\n", oldest, MAX_BACKUPS);
    }
}
#else
static void remove_oldest_backup(void) {
    /* Unix 版本 — 简化实现 / Unix version — simplified implementation */
    (void)0;
}
#endif

/*
 * [中文] 二进制文件复制 — 逐块读取源文件写入目标文件
 * [English] Binary file copy — read source in chunks and write to destination
 *
 * 使用 4096 字节缓冲区逐块复制，适用于任意大小的文件。
 * Uses a 4096-byte buffer for chunked copying, works for files of any size.
 */
static int copy_file(const char *src, const char *dst) {
    FILE *fs = fopen(src, "rb");
    if (!fs) return ERROR_FILE_IO;
    FILE *fd = fopen(dst, "wb");
    if (!fd) { fclose(fs); return ERROR_FILE_IO; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fs)) > 0) {
        if (fwrite(buf, 1, n, fd) != n) { fclose(fs); fclose(fd); return ERROR_FILE_IO; }
    }
    fclose(fs);
    fclose(fd);
    return SUCCESS;
}

/*
 * [中文] 执行数据备份
 * [English] Perform data backup
 *
 * 完整流程 / Full workflow:
 *   1. 确保 data/backup/ 目录存在
 *   2. 创建以当前时间戳命名的子目录（如 data/backup/20260505_143020/）
 *   3. 逐一复制14个数据文件到备份目录
 *   4. 打印备份结果（成功文件数 / 总文件数）
 *   5. 如果备份了至少1个文件，触发最旧备份清理
 */
int backup_data(void) {
    /* 步骤1：确保备份根目录存在 / Step 1: Ensure backup root directory exists */
#ifdef _WIN32
    _mkdir(DATA_DIR "/backup");
#else
    mkdir(DATA_DIR "/backup", 0755);
#endif

    /* 步骤2：创建时间戳子目录 / Step 2: Create timestamped subdirectory */
    char dir_name[64];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(dir_name, sizeof(dir_name), DATA_DIR "/backup/%Y%m%d_%H%M%S", tm);

#ifdef _WIN32
    if (_mkdir(dir_name) != 0) return ERROR_FILE_IO;
#else
    if (mkdir(dir_name, 0755) != 0) return ERROR_FILE_IO;
#endif

    /* 步骤3：逐一复制数据文件 / Step 3: Copy data files one by one */
    int success_count = 0;
    for (int i = 0; i < backup_file_count; i++) {
        char src[256], dst[256];
        snprintf(src, sizeof(src), DATA_DIR "/%s", backup_data_files[i]);
        snprintf(dst, sizeof(dst), "%s/%s", dir_name, backup_data_files[i]);
        if (copy_file(src, dst) == SUCCESS) success_count++;
    }

    printf("备份完成: %s (%d/%d 文件)\n", dir_name, success_count, backup_file_count);

    /* 步骤5：清理超量备份 / Step 5: Cleanup excess backups */
    if (success_count > 0) {
        remove_oldest_backup();
    }

    return SUCCESS;
}

/*
 * [中文] 列出所有备份目录名
 * [English] List all backup directory names
 *
 * 返回值通过 out_names（字符串数组）和 out_count（数量）返回。
 * 调用者使用完毕后必须调用 free_backups_list 释放内存。
 *
 * Results returned via out_names (string array) and out_count (count).
 * Caller must call free_backups_list to free the memory after use.
 *
 * 使用动态扩容策略：初始容量16，满时翻倍 / Uses dynamic growth: initial capacity 16, doubles when full.
 */
int list_backups(const char ***out_names, int *out_count) {
    *out_names = NULL;
    *out_count = 0;

#ifdef _WIN32
    struct _finddata_t fd;
    intptr_t handle = _findfirst(DATA_DIR "/backup/*", &fd);
    if (handle == -1L) return SUCCESS;

    int capacity = 0, count = 0;
    char **names = NULL;
    do {
        if (fd.attrib & _A_SUBDIR) {
            if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
            /* 动态扩容 / Dynamic resizing */
            if (count >= capacity) {
                capacity = capacity ? capacity * 2 : 16;  /* 初始16，之后翻倍 / Start with 16, double thereafter */
                char **tmp = realloc(names, capacity * sizeof(char *));
                if (!tmp) { free_backups_list((const char **)names, count); _findclose(handle); return ERROR_FILE_IO; }
                names = tmp;
            }
            names[count] = malloc(strlen(fd.name) + 1);
            if (!names[count]) { free_backups_list((const char **)names, count); _findclose(handle); return ERROR_FILE_IO; }
            strcpy(names[count], fd.name);
            count++;
        }
    } while (_findnext(handle, &fd) == 0);
    _findclose(handle);

    *out_names = (const char **)names;
    *out_count = count;
#endif
    return SUCCESS;
}

/*
 * [中文] 释放备份列表内存
 * [English] Free backup list memory
 */
void free_backups_list(const char **names, int count) {
    if (!names) return;
    for (int i = 0; i < count; i++) {
        free((void *)names[i]);
    }
    free(names);
}

/*
 * [中文] 从指定备份还原数据
 * [English] Restore data from a specified backup
 *
 * 将所有14个数据文件从备份目录复制回 data/ 目录，覆盖当前文件。
 * 打印还原结果（成功文件数 / 总文件数）。
 *
 * Copies all 14 data files from the backup directory back to data/,
 * overwriting current files. Prints restore results (successful / total).
 */
int restore_data(const char *backup_dir) {
    int success_count = 0;

    for (int i = 0; i < backup_file_count; i++) {
        char src[256], dst[256];
        snprintf(src, sizeof(src), DATA_DIR "/backup/%s/%s", backup_dir, backup_data_files[i]);
        snprintf(dst, sizeof(dst), DATA_DIR "/%s", backup_data_files[i]);
        if (copy_file(src, dst) == SUCCESS) success_count++;
    }

    printf("还原完成: %s (%d/%d 文件)\n", backup_dir, success_count, backup_file_count);
    return SUCCESS;
}
