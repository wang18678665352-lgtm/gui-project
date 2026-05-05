#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Realistic hospital data generator for medium-sized hospital MIS.
Generates all data files with consistent cross-references.
医院: 仁和综合医院 (Renhe General Hospital)
规模: ~200 doctors, ~500 patients, 59 departments, 100 wards
"""

import random
import hashlib
import os
from collections import defaultdict
from datetime import datetime, timedelta, date

random.seed(20260505)
os.chdir(os.path.dirname(os.path.abspath(__file__)))

TODAY = date.today()  # 2026-05-05
DATA_DIR = "."

# ============================================================
# Utility functions
# ============================================================

def sha256(s):
    return hashlib.sha256(s.encode()).hexdigest()

def daterange(start, end):
    for n in range((end - start).days + 1):
        yield start + timedelta(days=n)

def date_str(d):
    return d.strftime("%Y-%m-%d")

def datetime_str():
    now = datetime.now()
    return now.strftime("%Y-%m-%d %H:%M:%S")

def random_datetime_str(d, hour_range=(7, 20)):
    h = random.randint(*hour_range)
    m = random.randint(0, 59)
    s = random.randint(0, 59)
    return f"{date_str(d)} {h:02d}:{m:02d}:{s:02d}"

# Shared ID generation counters (matches C's static counter in generate_id)
_id_buckets = defaultdict(int)

def generate_timestamp_id(prefix, dt):
    """Simulate C's generate_id(): PREFIX + YYYYMMDDHHmmss + 3-digit-seq."""
    ts = dt.strftime("%Y%m%d%H%M%S")
    key = (prefix, ts)
    seq = _id_buckets[key]
    _id_buckets[key] += 1
    return f"{prefix}{ts}{seq % 1000:03d}"

def generate_doctor_id(dept_index, dept_seq_counters):
    """Simulate C's generate_doctor_id(): <letter><4-digit-seq>.
    Letter = chr('A' + dept_index), capped at 'Z'."""
    letter = chr(ord('A') + min(dept_index, 25))
    seq = dept_seq_counters.get(letter, 1)
    dept_seq_counters[letter] = seq + 1
    return f"{letter}{seq:04d}"

# ============================================================
# Chinese name generator
# ============================================================

SURNAMES = "赵钱孙李周吴郑王冯陈褚卫蒋沈韩杨朱秦尤许何吕施张孔曹严华金魏陶姜戚谢邹喻柏水窦章云苏潘葛奚范彭郎鲁韦昌马苗凤花方俞任袁柳酆鲍史唐费廉岑薛雷贺倪汤滕殷罗毕郝邬安常乐于时傅皮下齐康伍余元卜顾孟平黄和穆萧尹姚邵湛汪祁毛禹狄米贝明臧计伏成戴谈宋茅庞熊纪舒屈项祝董梁杜阮蓝闵席季麻强贾路娄危江童颜郭梅盛林刁钟徐邱骆高夏蔡田樊胡凌霍虞万支柯昝管卢莫经房裘缪干解应宗丁宣贲邓郁单杭洪包诸左石崔吉钮龚程嵇邢滑裴陆荣翁荀羊於惠甄麴家封芮羿储靳汲邴糜松井段富巫乌焦巴弓牧隗山谷车侯宓蓬全郗班仰秋仲伊宫宁仇栾暴甘钭厉戎祖武符刘景詹束龙叶幸司韶郜黎蓟薄印宿白怀蒲邰从鄂索咸籍赖卓蔺屠蒙池乔阴鬱胥能苍双闻莘党翟谭贡劳逄姬申扶堵冉宰郦雍卻璩桑桂濮牛寿通边扈燕冀郏浦尚农温别庄晏柴瞿阎充慕连茹习宦艾鱼容向古易慎戈廖庾终暨居衡步都耿满弘匡国文寇广禄阙东欧殳沃利蔚越夔隆师巩厍聂晁勾敖融冷訾辛阚那简饶空曾毋沙乜养鞠须丰巢关蒯相查後荆红游竺权逯盖益桓公"
GIVEN_MALE = "伟强勇军斌杰涛峰磊鹏辉宇浩志明建华国平安康宁东旭文博思远振海龙飞刚毅永昌瑞泽恩光耀宗耀祖承志守正德良仁义礼智信诚"
GIVEN_FEMALE = "芳敏娜秀英丽静娟红梅兰菊萍燕玲雪珍琴婷雯妍蓉倩怡君珊琳洁颖萱雨柔月云霞花凤娥慧"
GIVEN_UNISEX = "文博思远晨阳雨桐"

def random_name(gender=None):
    if gender == "男":
        pool = GIVEN_MALE + GIVEN_UNISEX
    elif gender == "女":
        pool = GIVEN_FEMALE + GIVEN_UNISEX
    else:
        pool = GIVEN_MALE + GIVEN_FEMALE + GIVEN_UNISEX
    surname = random.choice(SURNAMES)
    given_len = random.choices([1, 2], weights=[0.2, 0.8])[0]
    given = ''.join(random.choice(pool) for _ in range(given_len))
    return surname + given

CITIES = ["北京", "上海", "广州", "深圳", "成都", "杭州", "武汉", "南京", "重庆", "西安",
          "长沙", "青岛", "大连", "厦门", "苏州", "天津", "郑州", "济南", "合肥", "福州",
          "昆明", "贵阳", "南宁", "哈尔滨", "长春", "沈阳", "石家庄", "太原", "兰州", "乌鲁木齐"]
DISTRICTS = ["市辖区", "朝阳区", "海淀区", "浦东新区", "天河区", "武侯区", "西湖区", "武昌区",
             "鼓楼区", "渝中区", "雁塔区", "芙蓉区", "崂山区", "中山区", "思明区", "姑苏区"]

def random_address():
    return random.choice(CITIES) + random.choice(DISTRICTS)

PATIENT_TYPES = ["普通", "医保", "医保", "医保", "自费", "工伤"]

TREATMENT_STAGES = ["初诊", "检查", "治疗", "复查", "康复", "出院"]

DOCTOR_TITLES = ["住院医师", "主治医师", "副主任医师", "主任医师", "医学博士",
                "副教授", "教授", "首席专家", "返聘专家", "学科带头人",
                "副主任药师", "主任药师", "主管护师", "副主任护师", "主任护师"]

# Realistic medical diagnoses
DIAGNOSES = [
    ("急性上呼吸道感染", "患者咽部充血，扁桃体I度肿大，双肺呼吸音清。予抗病毒及对症治疗。"),
    ("原发性高血压病2级", "血压160/95mmHg，心率72次/分，律齐。建议低盐饮食，口服降压药。"),
    ("2型糖尿病", "空腹血糖8.6mmol/L，餐后2h血糖13.2mmol/L。控制饮食，口服降糖药。"),
    ("冠状动脉粥样硬化性心脏病", "心电图示ST段轻度压低。建议口服阿司匹林+他汀类药物。"),
    ("慢性胃炎", "上腹部隐痛，伴反酸嗳气。胃镜示胃窦部黏膜充血。予抑酸保护胃黏膜。"),
    ("腰椎间盘突出症", "L4-L5椎间盘突出，左侧坐骨神经痛。建议卧床休息，理疗。"),
    ("急性支气管炎", "咳嗽咳痰3天，黄痰。双肺可闻及散在湿啰音。予抗生素+化痰治疗。"),
    ("脂肪肝(中度)", "B超示肝脏回声增强。肝功能轻度异常。建议运动+饮食控制。"),
    ("过敏性鼻炎", "鼻塞流涕打喷嚏，季节性发作。鼻甲黏膜苍白水肿。予抗组胺药。"),
    ("慢性咽炎", "咽部异物感，晨起干呕。咽后壁淋巴滤泡增生。建议忌辛辣。"),
    ("甲状腺功能亢进", "心悸手抖，体重下降。T3T4升高，TSH降低。予甲巯咪唑治疗。"),
    ("泌尿系结石", "右侧腰痛伴血尿。B超示右肾结石0.6cm。多饮水，予排石药物。"),
    ("颈椎病(神经根型)", "颈肩酸痛，右上肢麻木。X线示颈椎骨质增生。针灸+理疗。"),
    ("高脂血症", "总胆固醇6.8mmol/L，甘油三酯2.9mmol/L。饮食控制+他汀类降脂。"),
    ("慢性阻塞性肺疾病", "活动后气喘，肺功能检查示FEV1/FVC<70%。予支气管扩张剂。"),
    ("类风湿性关节炎", "双手近端指间关节肿痛，晨僵>1h。RF阳性。予DMARDs治疗。"),
    ("消化性溃疡", "周期性上腹痛，空腹加重。胃镜示十二指肠球部溃疡。予PPI治疗。"),
    ("偏头痛", "反复发作性单侧头痛，伴恶心畏光。避免诱因，予曲普坦类药物。"),
    ("骨质疏松症", "骨密度测定T值-2.8。予钙剂+维生素D+双膦酸盐。"),
    ("缺铁性贫血", "血红蛋白92g/L，血清铁降低。予铁剂补充。"),
    ("湿疹(亚急性)", "四肢伸侧红斑丘疹，瘙痒明显。予外用激素+保湿剂。"),
    ("前列腺增生", "排尿困难，夜尿增多。B超示前列腺增大。予α受体阻滞剂。"),
    ("慢性胆囊炎", "右上腹隐痛，油腻饮食后加重。B超示胆囊壁毛糙。予消炎利胆。"),
    ("支气管哮喘", "反复发作性喘息，接触过敏原后加重。予吸入性激素控制。"),
    ("急性胃肠炎", "腹痛腹泻，恶心呕吐。大便常规示白细胞(+)。予补液+抗生素。"),
    ("荨麻疹", "全身散在风团，瘙痒剧烈。予抗组胺药+外用炉甘石。"),
    ("痛风急性发作", "右足第一跖趾关节红肿热痛，血尿酸520μmol/L。予消炎止痛。"),
    ("慢性肾小球肾炎", "尿蛋白(++)，镜下血尿。血压稍高。予ACEI+保肾治疗。"),
    ("子宫肌瘤", "月经量增多，B超示子宫肌壁间肌瘤3cm。定期随访观察。"),
    ("睡眠障碍", "入睡困难，早醒，日间疲乏。建议睡眠卫生教育+必要时药物。"),
]

# ============================================================
# 1. Departments (keep existing structure, regenerate with better names)
# ============================================================

DEPARTMENT_NAMES = [
    "内科", "外科", "儿科", "骨科", "心内科", "神经内科", "呼吸内科", "消化内科",
    "肾内科", "内分泌科", "妇产科", "眼科", "耳鼻喉科", "口腔科", "皮肤科",
    "肿瘤科", "急诊科", "康复科", "中医科", "感染科", "血液科", "风湿免疫科",
    "胸外科", "肝胆外科", "泌尿外科", "神经外科", "整形外科", "血管外科",
    "烧伤科", "老年病科", "病理科", "检验科", "影像科", "超声科", "核医学科",
    "放疗科", "麻醉科", "ICU", "营养科", "精神心理科", "疼痛科", "介入科",
    "透析中心", "体检中心", "生殖医学中心", "睡眠医学中心", "过敏反应科",
    "运动医学科", "性病科", "职业病科", "预防保健科", "中西医结合科",
    "全科医学科", "舒缓医学科", "分子医学中心", "干细胞治疗中心", "精准医学中心",
    "器官移植中心", "微创外科中心"
]

departments = []
dept_base_dt = datetime(2023, 11, 1, 9, 0, 0)
for i, name in enumerate(DEPARTMENT_NAMES):
    dept_dt = dept_base_dt + timedelta(seconds=i * 3600)
    dept_id = generate_timestamp_id("DEP", dept_dt)
    leader_name = random_name()
    phone = f"0755-{random.randint(1000000, 9999999)}"
    departments.append((dept_id, name, leader_name, phone))

# ============================================================
# 2. Doctors (~200, 2-5 per department)
# ============================================================

doctors = []
doctor_users = []
doctors_by_dept = {}
dept_seq_counters = {}
is_first_doctor = True

for dept_index, (dept_id, dept_name, _, _) in enumerate(departments):
    n = random.randint(2, 5)
    if dept_name in ["急诊科", "内科", "外科", "妇产科", "儿科"]:
        n = random.randint(4, 6)
    dept_doctors = []
    for _ in range(n):
        doc_id = generate_doctor_id(dept_index, dept_seq_counters)
        if is_first_doctor:
            username = "doctor1"   # linked to test account
            is_first_doctor = False
        else:
            username = f"doctor_{doc_id.lower()}"
        name = random_name()
        title = random.choice(DOCTOR_TITLES)
        busy = random.randint(0, 5)
        password_hash = sha256("123456")
        doctors.append((doc_id, username, name, dept_id, title, busy))
        doctor_users.append((username, password_hash, "doctor"))
        dept_doctors.append(doc_id)
    doctors_by_dept[dept_id] = dept_doctors

NUM_DOCTORS = len(doctors)

# ============================================================
# 3. Patients (~500)
# ============================================================

patients = []
patient_users = []
patient_base_dt = datetime(2024, 6, 1, 8, 0, 0)
is_first_patient = True

for i in range(500):
    patient_dt = patient_base_dt + timedelta(seconds=i * 86400 // 3)
    pid = generate_timestamp_id("P", patient_dt)
    if is_first_patient:
        username = "patient1"  # linked to test account
        is_first_patient = False
    else:
        username = f"patient_{pid.lower()}"
    gender = random.choice(["男", "女"])
    name = random_name(gender)
    age = random.randint(1, 90)
    phone = f"1{random.randint(30, 99):02d}{random.randint(10000000, 99999999)}"
    address = random_address()
    ptype = random.choice(PATIENT_TYPES)
    stage = random.choice(TREATMENT_STAGES)
    is_emerg = 1 if random.random() < 0.03 else 0
    password_hash = sha256("123456")
    patients.append((pid, username, name, gender, age, phone, address, ptype, stage, is_emerg))
    patient_users.append((username, password_hash, "patient"))

NUM_PATIENTS = len(patients)

# ============================================================
# 4. Users (patients + doctors + admins)
# ============================================================

admin_users = [
    ("admin", sha256("admin123"), "admin"),
    ("wcy", sha256("123456"), "admin"),
]

# These test accounts need to exist for easy login testing
test_users = [
    ("patient1", sha256("123456"), "patient"),
    ("doctor1", sha256("123456"), "doctor"),
]

all_users = list(test_users)
# Add all patient users (skip if same username as test)
existing_usernames = {"patient1", "doctor1", "admin", "wcy"}
for uname, phash, role in patient_users:
    if uname not in existing_usernames:
        all_users.append((uname, phash, role))
        existing_usernames.add(uname)
for uname, phash, role in doctor_users:
    if uname not in existing_usernames:
        all_users.append((uname, phash, role))
        existing_usernames.add(uname)
for uname, phash, role in admin_users:
    all_users.append((uname, phash, role))

# ============================================================
# 5. Drugs (generate realistic catalog)
# ============================================================

DRUG_DATA = [
    ("阿莫西林胶囊", 18.50, "西药", 300, 20, 0, 0.70, "抗生素"),
    ("布洛芬缓释片", 26.00, "西药", 200, 15, 0, 0.60, "解热镇痛"),
    ("阿司匹林肠溶片", 8.50, "西药", 400, 30, 0, 0.80, "抗血小板"),
    ("头孢克洛胶囊", 32.00, "西药", 150, 10, 1, 0.70, "抗生素"),
    ("奥美拉唑肠溶片", 45.00, "西药", 180, 12, 0, 0.65, "消化系统"),
    ("氨氯地平片", 22.00, "西药", 250, 20, 0, 0.75, "心血管"),
    ("二甲双胍缓释片", 35.00, "西药", 300, 25, 0, 0.80, "内分泌"),
    ("氯雷他定片", 12.00, "西药", 350, 30, 0, 0.50, "抗过敏"),
    ("蒙脱石散", 16.00, "西药", 200, 15, 0, 0.60, "消化系统"),
    ("对乙酰氨基酚片", 7.50, "西药", 500, 40, 0, 0.55, "解热镇痛"),
    ("左氧氟沙星片", 28.00, "西药", 180, 15, 1, 0.70, "抗生素"),
    ("辛伐他汀片", 38.00, "西药", 160, 12, 0, 0.75, "心血管"),
    ("雷贝拉唑钠肠溶片", 55.00, "西药", 100, 10, 0, 0.65, "消化系统"),
    ("苯磺酸氨氯地平片", 24.00, "西药", 200, 18, 0, 0.75, "心血管"),
    ("阿卡波糖片", 42.00, "西药", 150, 12, 0, 0.80, "内分泌"),
    ("醋酸泼尼松片", 6.00, "西药", 300, 25, 1, 0.50, "激素类"),
    ("呋塞米片", 9.00, "西药", 250, 20, 0, 0.70, "利尿剂"),
    ("硝苯地平控释片", 48.00, "西药", 120, 10, 0, 0.75, "心血管"),
    ("格列美脲片", 38.00, "西药", 130, 10, 0, 0.80, "内分泌"),
    ("异丙嗪片", 4.50, "西药", 400, 30, 0, 0.40, "抗过敏"),
    ("黄连素片", 12.00, "中成药", 250, 20, 0, 0.60, "清热解毒"),
    ("板蓝根颗粒", 15.00, "中成药", 350, 30, 0, 0.50, "清热解毒"),
    ("六味地黄丸", 28.00, "中成药", 200, 15, 0, 0.40, "补益类"),
    ("速效救心丸", 35.00, "中成药", 180, 15, 0, 0.55, "心血管"),
    ("云南白药气雾剂", 42.00, "中成药", 100, 10, 0, 0.30, "骨伤科"),
    ("藿香正气软胶囊", 18.00, "中成药", 220, 18, 0, 0.50, "解表祛暑"),
    ("牛黄解毒片", 8.00, "中成药", 300, 25, 0, 0.45, "清热解毒"),
    ("乌鸡白凤丸", 48.00, "中成药", 80, 8, 0, 0.35, "补益类"),
    ("复方丹参滴丸", 32.00, "中成药", 200, 15, 0, 0.60, "心血管"),
    ("银翘解毒丸", 14.00, "中成药", 280, 22, 0, 0.50, "解表剂"),
    ("氯化钠注射液", 5.00, "注射液", 600, 50, 0, 0.90, "输液类"),
    ("葡萄糖注射液(5%)", 5.50, "注射液", 500, 40, 0, 0.90, "输液类"),
    ("葡萄糖注射液(10%)", 5.50, "注射液", 500, 40, 0, 0.90, "输液类"),
    ("乳酸钠林格注射液", 8.00, "注射液", 300, 25, 0, 0.85, "输液类"),
    ("甲硝唑氯化钠注射液", 12.00, "注射液", 200, 18, 1, 0.70, "抗生素"),
    ("注射用青霉素钠", 3.50, "注射液", 400, 30, 1, 0.75, "抗生素"),
    ("注射用头孢曲松钠", 15.00, "注射液", 150, 12, 1, 0.70, "抗生素"),
    ("地塞米松磷酸钠注射液", 2.00, "注射液", 350, 30, 1, 0.60, "激素类"),
    ("维生素C注射液", 1.50, "注射液", 400, 30, 0, 0.50, "维生素类"),
    ("胰岛素注射液", 48.00, "注射液", 100, 8, 1, 0.80, "内分泌"),
    ("多潘立酮片", 14.00, "西药", 220, 18, 0, 0.55, "消化系统"),
    ("碳酸钙D3片", 35.00, "西药", 180, 15, 0, 0.30, "维生素类"),
    ("厄贝沙坦片", 28.00, "西药", 200, 15, 0, 0.75, "心血管"),
    ("非那雄胺片", 55.00, "西药", 90, 8, 0, 0.65, "泌尿系统"),
    ("坦索罗辛胶囊", 42.00, "西药", 100, 8, 0, 0.70, "泌尿系统"),
    ("美托洛尔缓释片", 32.00, "西药", 160, 12, 0, 0.75, "心血管"),
    ("瑞舒伐他汀钙片", 52.00, "西药", 120, 10, 0, 0.75, "心血管"),
    ("埃索美拉唑镁肠溶片", 58.00, "西药", 80, 8, 0, 0.65, "消化系统"),
    ("氟西汀胶囊", 68.00, "西药", 60, 5, 1, 0.50, "精神科"),
    ("阿普唑仑片", 15.00, "西药", 150, 12, 1, 0.40, "精神科"),
    ("氨溴索口服液", 22.00, "西药", 200, 15, 0, 0.60, "呼吸系统"),
    ("复方甘草口服液", 10.00, "西药", 280, 22, 0, 0.45, "呼吸系统"),
    ("沙丁胺醇吸入剂", 28.00, "西药", 130, 10, 0, 0.70, "呼吸系统"),
    ("布地奈德吸入剂", 85.00, "西药", 70, 5, 1, 0.65, "呼吸系统"),
    ("氯霉素滴眼液", 8.00, "西药", 300, 25, 0, 0.40, "眼科"),
    ("维生素AD滴剂", 18.00, "西药", 200, 15, 0, 0.30, "维生素类"),
    ("钙尔奇D咀嚼片", 45.00, "西药", 150, 12, 0, 0.25, "维生素类"),
    ("消旋山莨菪碱片", 5.00, "西药", 350, 30, 0, 0.60, "消化系统"),
    ("复方氨酚烷胺片", 14.00, "西药", 300, 25, 0, 0.50, "解热镇痛"),
    ("维C银翘片", 9.00, "中成药", 350, 30, 0, 0.45, "解表剂"),
]

drugs = []
drug_base_dt = datetime(2024, 5, 1, 10, 0, 0)
for i, (name, price, dtype, stock_num, warn_line, is_special, reimb, cat) in enumerate(DRUG_DATA):
    drug_dt = drug_base_dt + timedelta(seconds=i * 7200)
    drug_id = generate_timestamp_id("DRG", drug_dt)
    drugs.append((drug_id, name, price, dtype, stock_num, warn_line, is_special, reimb, cat))
NUM_DRUGS = len(drugs)

# ============================================================
# 6. Wards (100 wards)
# ============================================================

WARD_TYPES = [
    "心血管病房", "综合病房", "ICU", "儿科病房", "妇产科病房",
    "外科病房", "内科病房", "骨科病房", "神经科病房", "肿瘤科病房",
    "观察病房", "术后恢复室", "急诊留观室", "隔离病房", "VIP病房",
]

wards = []
ward_base_dt = datetime(2023, 10, 1, 10, 0, 0)
for i in range(100):
    ward_dt = ward_base_dt + timedelta(seconds=i * 3600)
    wid = generate_timestamp_id("WRD", ward_dt)
    wtype = random.choice(WARD_TYPES)
    total = random.randint(3, 12)
    remain = random.randint(0, total)
    warning = max(1, int(total * 0.2))
    wards.append((wid, wtype, total, remain, warning))

# ============================================================
# 7. Schedules (every doctor × 7 days × 2 time slots)
# ============================================================

TIME_SLOTS = ["上午(08:00-12:00)", "下午(14:00-18:00)"]

schedules = []
for doc_id, _, _, _, _, _ in doctors:
    for d in daterange(TODAY, TODAY + timedelta(days=6)):
        for slot in TIME_SLOTS:
            hour = 8 if "上午" in slot else 14
            sch_dt = datetime(d.year, d.month, d.day, hour, 0, 0)
            sid = generate_timestamp_id("SCH", sch_dt)
            status = "正常" if random.random() > 0.05 else "停诊"
            max_appt = random.randint(10, 25)
            max_onsite = random.randint(5, 10)
            schedules.append((sid, doc_id, date_str(d), slot, max_appt, max_onsite, status))

# ============================================================
# 8. Appointments (realistic mix of statuses)
# ============================================================

appointments = []
SLOTS_7 = [
    "上午(08:00-12:00)", "下午(14:00-18:00)",
    "上午(08:00-09:00)", "上午(09:00-10:00)", "上午(10:00-11:00)", "上午(11:00-12:00)",
    "下午(14:00-15:00)", "下午(15:00-16:00)", "下午(16:00-17:00)", "下午(17:00-18:00)",
]

# Past appointments (30 days back)
for d in daterange(TODAY - timedelta(days=30), TODAY - timedelta(days=1)):
    n_appts = random.randint(8, 20)
    for _ in range(n_appts):
        patient = random.choice(patients)
        pid = patient[0]
        dept_id = random.choice(departments)[0]
        doc_id = random.choice(doctors_by_dept.get(dept_id, [doctors[0][0]]))
        slot = random.choice(SLOTS_7)
        # Past statuses
        r = random.random()
        if r < 0.70:
            status = "已就诊"
        elif r < 0.85:
            status = "已取消"
        else:
            status = "已爽约"
        fee = round(random.uniform(10, 50), 2)
        paid = 1 if status != "已爽约" else random.choice([0, 1])
        create_d = d - timedelta(days=random.randint(0, 3))
        create_time = random_datetime_str(create_d)
        apt_dt = datetime.strptime(create_time, "%Y-%m-%d %H:%M:%S")
        appt_id = generate_timestamp_id("APT", apt_dt)
        appointments.append((appt_id, pid, doc_id, dept_id, date_str(d),
                            slot, status, create_time, fee, paid))

# Future appointments (today + 6 days)
for d in daterange(TODAY, TODAY + timedelta(days=6)):
    n_appts = random.randint(5, 15)
    for _ in range(n_appts):
        patient = random.choice(patients)
        pid = patient[0]
        dept_id = random.choice(departments)[0]
        doc_id = random.choice(doctors_by_dept.get(dept_id, [doctors[0][0]]))
        slot = random.choice(SLOTS_7)
        status = "待就诊"
        fee = round(random.uniform(10, 50), 2)
        paid = random.choice([0, 1])
        create_d = TODAY - timedelta(days=random.randint(0, 5))
        create_time = random_datetime_str(create_d)
        apt_dt = datetime.strptime(create_time, "%Y-%m-%d %H:%M:%S")
        appt_id = generate_timestamp_id("APT", apt_dt)
        appointments.append((appt_id, pid, doc_id, dept_id, date_str(d),
                            slot, status, create_time, fee, paid))

# ============================================================
# 9. Onsite registrations (today only)
# ============================================================

onsite_regs = []
queue_counters = {}  # (doctor_id, dept_id) -> next queue number

for doc_id, _, _, dept_id, _, _ in doctors:
    n_onsite = random.randint(0, 12)
    if (doc_id, dept_id) not in queue_counters:
        queue_counters[(doc_id, dept_id)] = 1

    for _ in range(n_onsite):
        patient = random.choice(patients)
        pid = patient[0]

        qn = queue_counters[(doc_id, dept_id)]
        queue_counters[(doc_id, dept_id)] += 1

        # Status distribution for today
        r = random.random()
        if r < 0.35:
            status = "排队中"
        elif r < 0.55:
            status = "已接诊"
        elif r < 0.75:
            status = "就诊中"
        elif r < 0.90:
            status = "已退号"
        else:
            status = "已完成"

        create_hour = random.randint(7, 17)
        create_min = random.randint(0, 59)
        create_sec = random.randint(0, 59)
        create_time = f"{date_str(TODAY)} {create_hour:02d}:{create_min:02d}:{create_sec:02d}"
        os_dt = datetime.strptime(create_time, "%Y-%m-%d %H:%M:%S")
        os_id = generate_timestamp_id("OS", os_dt)
        onsite_regs.append((os_id, pid, doc_id, dept_id, qn, status, create_time))

# ============================================================
# 10. Medical records (one per 已就诊 appointment)
# ============================================================

medical_records = []

past_apts = [a for a in appointments if a[6] == "已就诊"]
random.shuffle(past_apts)

for appt in past_apts:
    # Create a medical record for ~85% of past appointments
    if random.random() > 0.85:
        continue

    diag_info = random.choice(DIAGNOSES)
    diagnosis_text = f"{diag_info[0]} | {diag_info[1]}"
    rec_status = random.choice(["已就诊", "已就诊", "已就诊", "已归档"])
    appt_date = appt[4]
    diag_time = random_datetime_str(datetime.strptime(appt_date, "%Y-%m-%d").date() + timedelta(days=0))
    rec_dt = datetime.strptime(diag_time, "%Y-%m-%d %H:%M:%S")
    rec_id = generate_timestamp_id("MR", rec_dt)
    medical_records.append((rec_id, appt[1], appt[2], appt[0], diagnosis_text, diag_time, rec_status))

# ============================================================
# 11. Prescriptions (1-4 per medical record)
# ============================================================

prescriptions = []
drug_list_simple = [(d[0], d[1], d[2]) for d in drugs]  # drug_id, name, price

for rec in medical_records:
    rec_id = rec[0]
    patient_id = rec[1]
    doctor_id = rec[2]
    appt_id = rec[3]
    diag_time = rec[5]

    n_rx = random.choices([0, 1, 2, 3], weights=[0.25, 0.35, 0.25, 0.15])[0]
    for _ in range(n_rx):
        drug = random.choice(drug_list_simple)
        drug_id, drug_name, drug_price = drug
        qty = random.randint(1, 5)
        total = round(qty * drug_price, 2)
        rx_dt = datetime.strptime(diag_time, "%Y-%m-%d %H:%M:%S")
        rx_id = generate_timestamp_id("PR", rx_dt)
        # prescription date same as diagnosis
        prescriptions.append((rx_id, rec_id, patient_id, doctor_id, drug_id,
                             qty, total, diag_time))

# ============================================================
# 12. Ward calls (scattered over past 30 days)
# ============================================================

ward_calls = []
CALL_STATUSES = ["待响应", "已响应", "已处理", "已完成"]
CALL_MESSAGES = [
    "患者请求帮助", "输液完毕", "疼痛加剧", "呼吸困难", "血压异常升高",
    "心率异常报警", "患者摔倒求助", "需要更换敷料", "需要更换床单", "如厕求助",
    "检查通知", "家属呼叫", "饮食请求", "用药提醒", "紧急抢救通知",
]

for _ in range(150):
    ward = random.choice(wards)
    wid = ward[0]
    patient = random.choice(patients)
    pid = patient[0]
    dept_id = random.choice(departments)[0]
    msg = random.choice(CALL_MESSAGES)
    status = random.choice(CALL_STATUSES)
    call_d = TODAY - timedelta(days=random.randint(0, 30))
    create_time = random_datetime_str(call_d)
    wc_dt = datetime.strptime(create_time, "%Y-%m-%d %H:%M:%S")
    call_id = generate_timestamp_id("WC", wc_dt)
    ward_calls.append((call_id, wid, dept_id, pid, msg, status, create_time))

# ============================================================
# 13. Templates (common medical templates)
# ============================================================

TEMPLATES = [
    ("T001", "诊断", "上呼吸道感染", "急性上呼吸道感染。咽部充血，扁桃体I-II度肿大，双肺呼吸音清，未闻及干湿啰音。体温: 37.8℃。"),
    ("T002", "诊断", "高血压病", "原发性高血压病。血压160/95mmHg，心率72次/分，律齐。无头痛头晕。建议低盐低脂饮食。"),
    ("T003", "诊断", "糖尿病", "2型糖尿病。空腹血糖升高，糖化血红蛋白超标。建议控制饮食，规律运动，口服降糖药。"),
    ("T004", "诊断", "冠心病", "冠状动脉粥样硬化性心脏病。心电图示ST段轻度改变。建议口服抗血小板药物+他汀类。"),
    ("T005", "诊断", "慢性胃炎", "慢性非萎缩性胃炎。上腹部隐痛伴反酸。胃镜检查提示胃窦部黏膜充血水肿。"),
    ("T006", "诊断", "支气管炎", "急性支气管炎。咳嗽咳痰，黄白色黏痰。双肺呼吸音粗，可闻及散在干啰音。"),
    ("T007", "诊断", "颈椎病", "颈椎病（神经根型）。颈部僵硬疼痛，伴上肢放射性麻木。颈椎X线示骨质增生。"),
    ("T008", "诊断", "心律不齐", "窦性心律不齐。心电图示偶发房性早搏。建议动态心电图进一步检查。"),
    ("T009", "诊断", "过敏性鼻炎", "常年性过敏性鼻炎。鼻塞、流清涕、打喷嚏。鼻甲黏膜苍白、水肿。"),
    ("T010", "诊断", "高脂血症", "混合型高脂血症。总胆固醇及甘油三酯均升高。建议低脂饮食+运动。"),
    ("T011", "治疗", "上呼吸道感染", "1. 口服抗生素治疗3-5天\n2. 止咳化痰对症治疗\n3. 多饮温水，注意休息\n4. 体温升高可物理降温"),
    ("T012", "治疗", "高血压", "1. 口服降压药\n2. 低盐低脂饮食\n3. 每日监测血压\n4. 适当有氧运动\n5. 定期复查"),
    ("T013", "治疗", "糖尿病", "1. 口服降糖药/胰岛素治疗\n2. 控制饮食总热量\n3. 定时定量进餐\n4. 监测血糖\n5. 适量运动"),
    ("T014", "治疗", "胃炎", "1. 质子泵抑制剂口服\n2. 胃黏膜保护剂\n3. 忌辛辣刺激食物\n4. 规律进餐\n5. 复查胃镜"),
    ("T015", "治疗", "支气管炎", "1. 抗生素治疗（细菌感染）\n2. 祛痰止咳药\n3. 支气管扩张剂\n4. 多饮水，清淡饮食\n5. 戒烟戒酒"),
    ("T016", "检查", "血常规", "血常规+CRP：白细胞、中性粒细胞计数、血红蛋白、血小板、C反应蛋白。"),
    ("T017", "检查", "生化全项", "肝功能、肾功能、血糖、血脂四项、电解质。需空腹抽血。"),
    ("T018", "检查", "胸部X线", "胸部正位片。评估双肺纹理、心影大小、纵隔情况。"),
    ("T019", "检查", "心电图", "十二导联心电图。评估心率、心律、ST-T改变等。"),
    ("T020", "检查", "腹部B超", "肝、胆、胰、脾、双肾B超。评估脏器形态、回声、占位等。"),
]

# ============================================================
# 14. Logs
# ============================================================

logs = []

actions = [
    ("登录", "system"), ("退出", "system"), ("挂号", "appointment"),
    ("取消预约", "appointment"), ("接诊", "appointment"), ("开药", "prescription"),
    ("修改密码", "user"), ("现场挂号", "onsite"), ("退号", "onsite"),
    ("完成接诊", "onsite"), ("响应呼叫", "ward_call"), ("更新阶段", "patient"),
    ("紧急标记", "patient"),
]

for _ in range(100):
    action, target = random.choice(actions)
    operator = random.choice(["patient1", "doctor1", "admin"] + [u[0] for u in all_users[:50]])
    target_id = f"ID{random.randint(10000, 99999)}"
    detail = ""
    log_d = TODAY - timedelta(days=random.randint(0, 30))
    create_time = random_datetime_str(log_d)
    log_dt = datetime.strptime(create_time, "%Y-%m-%d %H:%M:%S")
    lid = generate_timestamp_id("L", log_dt)
    logs.append((lid, operator, action, target, target_id, detail, create_time))

# ============================================================
# Write all data files
# ============================================================

def write_tsv(filename, header, data):
    """Write tab-separated file with header comment line."""
    path = os.path.join(DATA_DIR, filename)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(header + "\n")
        for row in data:
            f.write("\t".join(str(x) for x in row) + "\n")
    print(f"  {filename}: {len(data)} records")

print("Generating data files...")

# departments
write_tsv("departments.txt",
    "# department_id\tname\tleader\tphone", departments)

# doctors
write_tsv("doctors.txt",
    "# doctor_id\tusername\tname\tdepartment_id\ttitle\tbusy_level", doctors)

# patients
write_tsv("patients.txt",
    "# patient_id\tusername\tname\tgender\tage\tphone\taddress\tpatient_type\ttreatment_stage\tis_emergency", patients)

# users
write_tsv("users.txt",
    "# username\tpassword\trole", all_users)

# drugs
write_tsv("drugs.txt",
    "# drug_id\tname\tprice\tstock_num\twarning_line\tis_special\treimbursement_ratio\tcategory",
    [(d[0], d[1], d[2], d[4], d[5], d[6], d[7], d[8]) for d in drugs])  # skip d[3]=type, include d[8]=category

# wards
write_tsv("wards.txt",
    "# ward_id\ttype\ttotal_beds\tremain_beds\twarning_line", wards)

# schedules
write_tsv("schedules.txt",
    "# schedule_id\tdoctor_id\twork_date\ttime_slot\tmax_appt\tmax_onsite\tstatus", schedules)

# appointments
write_tsv("appointments.txt",
    "# appointment_id\tpatient_id\tdoctor_id\tdepartment_id\tappointment_date\tappointment_time\tstatus\tcreate_time\tfee\tpaid",
    [(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], f"{a[8]:.2f}", a[9]) for a in appointments])

# onsite registrations
write_tsv("onsite_registrations.txt",
    "# onsite_id\tpatient_id\tdoctor_id\tdepartment_id\tqueue_number\tstatus\tcreate_time", onsite_regs)

# medical records
write_tsv("medical_records.txt",
    "# record_id\tpatient_id\tdoctor_id\tappointment_id\tdiagnosis\tdiagnosis_date\tstatus", medical_records)

# prescriptions
write_tsv("prescriptions.txt",
    "# prescription_id\trecord_id\tpatient_id\tdoctor_id\tdrug_id\tquantity\ttotal_price\tprescription_date",
    [(p[0], p[1], p[2], p[3], p[4], p[5], f"{p[6]:.2f}", p[7]) for p in prescriptions])

# ward calls
write_tsv("ward_calls.txt",
    "# call_id\tward_id\tdepartment_id\tpatient_id\tmessage\tstatus\tcreate_time", ward_calls)

# templates
write_tsv("templates.txt",
    "# template_id\tcategory\tshortcut\ttext", TEMPLATES)

# logs
write_tsv("logs.txt",
    "# log_id\toperator\taction\ttarget\ttarget_id\tdetail\tcreate_time", logs)

print(f"\nDone! Generated data for:")
print(f"  {len(departments)} departments")
print(f"  {NUM_DOCTORS} doctors")
print(f"  {NUM_PATIENTS} patients")
print(f"  {len(all_users)} users")
print(f"  {NUM_DRUGS} drugs")
print(f"  {len(wards)} wards")
print(f"  {len(schedules)} schedules")
print(f"  {len(appointments)} appointments")
print(f"  {len(onsite_regs)} onsite registrations")
print(f"  {len(medical_records)} medical records")
print(f"  {len(prescriptions)} prescriptions")
print(f"  {len(ward_calls)} ward calls")
print(f"  {len(TEMPLATES)} templates")
print(f"  {len(logs)} logs")
