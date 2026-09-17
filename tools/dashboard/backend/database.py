# -*- coding: utf-8 -*-
"""
SQLite 数据库模块
负责 BMS 历史数据存储与查询
"""
import sqlite3
import threading
import time
import os
import json
from datetime import datetime

# 2026-08-10 修复: 数值解析安全 helper(根治 F1-ext: 元素级 int 转换崩溃)
from utils import safe_int, safe_int_list

# 数据库文件路径(放在 dashboard 目录下)
DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bms_history.db")

# 线程锁,保证多线程写入安全
_db_lock = threading.Lock()

# #7: query_count 的 TTL 计数缓存, 避免高频轮询时对 bms_data 全表 COUNT(*) 扫描.
# 插入/清理后必须 invalidate, 否则会读到过期总数.
_COUNT_TTL = 30.0  # 缓存有效期(秒)
_count_cache = {"ts": 0.0, "value": 0}

# ====================================================================
# 电池串联数(动态): 默认 16S (2026-09-07: 由 6 改 16, 与 bms_config.h
# BMS_CELL_SERIES_NUM=16 一致) 设备未上线时历史查询按 16 路渲染
# 由上报数据/DB 记录自动更新, 供 app.py / iotda_client.py / 前端动态适配
# ====================================================================
_series_num = 16
_series_lock = threading.Lock()

def get_series_num():
    """获取当前电池串联数(1~32)"""
    with _series_lock:
        return _series_num

def update_series_num(n):
    """根据上报数据/参数更新串数(1~32 合法, 非法值忽略; 与设备端 BMS_MAX_CELL_SERIES_NUM=32 一致)
    2026-08-09 修复: 上限 16→32(原 16 会拒绝 17~32 串记录, 与固件 1~32S 冲突)
    2026-09-07: 加 _series_lock 保护多线程读写(WS 线程写 / 查询线程读)"""
    global _series_num
    try:
        n = int(n)
    except (TypeError, ValueError):
        return
    if 1 <= n <= 32:
        with _series_lock:
            _series_num = n

def _parse_series_from_payload(payload):
    """从上报 payload 解析串数:
    1) cell_series_num 字段(设备参数/用户下发)
    2) cells 数组实际长度(最权威: 设备采了几串就报几格)
    2026-08-08 修复: cells 全 0(LTC6804 未接)时不覆盖配置串数,
    否则用户切到 16 串后, 设备全 0 上报会把全局串数拉回 6, 前端无法查询 >6 串"""
    if not isinstance(payload, dict):
        return
    csn = payload.get("cell_series_num")
    if csn is not None:
        update_series_num(csn)
    cells = payload.get("cells")
    # 2026-08-10 修复(F1-ext): 元素可能含畸形字符串("N/A"/""/None),
    #   直接 `v > 0` 会在 str vs int 比较时抛 TypeError; 用安全转换后再判定有效串数。
    if isinstance(cells, list) and len(cells) > 0 and any(safe_int(v, 0) > 0 for v in cells):
        update_series_num(len(cells))

def get_conn():
    """获取数据库连接(每次调用新建,用完即关)"""
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row  # 以字典方式访问列
    return conn


def init_db():
    """初始化数据库表结构"""
    with _db_lock:
        conn = get_conn()
        try:
            # #6: WAL 模式提升读写并发(读不阻塞写, 写不阻塞读),
            # 配合 cleanup_old 的 VACUUM 回收空闲页, 避免文件只增不减.
            try:
                conn.execute("PRAGMA journal_mode=WAL")
            except Exception:
                pass
            conn.execute("""
                CREATE TABLE IF NOT EXISTS bms_data (
                    id            INTEGER PRIMARY KEY AUTOINCREMENT,
                    timestamp     INTEGER,           -- 设备时间戳(秒)
                    recv_time     REAL,              -- 服务端接收时间(unix 秒)
                    soc           REAL,              -- 剩余电量 %
                    soh           REAL,              -- 健康度 %
                    v_max         INTEGER,           -- 单体最高电压 mV
                    v_min         INTEGER,           -- 单体最低电压 mV
                    pack_v        INTEGER,           -- 总压 mV
                    current       INTEGER,           -- 电流 mA(正充电/负放电)
                    temp_max      REAL,              -- 最高温度 0.1℃
                    temp_min      REAL,              -- 最低温度 0.1℃
                    fault         INTEGER,           -- 故障码
                    c1            INTEGER,           -- 单体 1 电压 mV
                    c2            INTEGER,
                    c3            INTEGER,
                    c4            INTEGER,
                    c5            INTEGER,
                    c6            INTEGER,
                    cycle_count   INTEGER DEFAULT 0,
                    balance_mask  INTEGER,           -- 均衡掩码
                    charge_mos    INTEGER DEFAULT -1,-- QoL: 充电MOS回读(1=开 0=关 -1=老固件未知)
                    discharge_mos INTEGER DEFAULT -1,-- QoL: 放电MOS回读(1=开 0=关 -1=未知)
                    balance_on    INTEGER DEFAULT 0, -- QoL: 均衡进行中(1=是 0=否)
                    charge_mode   INTEGER DEFAULT 0, -- QoL: 充电模式(0=待机1=充2=放3=均衡)
                    cells_json    TEXT,              -- 完整单体数组 JSON(串数自适应, 兼容 c1~c6)
                    insulation_rp INTEGER DEFAULT 0, -- 正极对地绝缘电阻 Ω(不平衡电桥法, 0=未启用/未接线)
                    insulation_rn INTEGER DEFAULT 0, -- 负极对地绝缘电阻 Ω(不平衡电桥法, 0=未启用/未接线)
                    insulation_ohm_per_v INTEGER DEFAULT 0, -- 综合绝缘电阻率 Ω/V(=min(Rp,Rn)/总压, GB/T 18384.1 要求≥100合格)
                    rs485_online     INTEGER DEFAULT 0, -- RS485/Modbus 从站在线(1=近期被主站轮询 0=离线/未启用)
                    rs485_last_poll INTEGER DEFAULT 0, -- 距最近一次被主站轮询的秒数(65535=从未)
                    rs485_err       INTEGER DEFAULT 0  -- RS485 CRC/帧错误累计计数
                )
            """)
            # ----- 兼容升级: 旧库没有 charge_mos/.. 四列时, 用 ADD COLUMN (SQLite 2021+安全支持) -----
            for _col, _typ in [("charge_mos", "INTEGER DEFAULT -1"),
                               ("discharge_mos", "INTEGER DEFAULT -1"),
                               ("balance_on", "INTEGER DEFAULT 0"),
                               ("charge_mode", "INTEGER DEFAULT 0"),
                               ("cycle_count", "INTEGER DEFAULT 0"),
                               ("cells_json", "TEXT"),
                               ("insulation_rp", "INTEGER DEFAULT 0"),
                               ("insulation_rn", "INTEGER DEFAULT 0"),
                               ("insulation_ohm_per_v", "INTEGER DEFAULT 0"),
                               ("rs485_online", "INTEGER DEFAULT 0"),
                               ("rs485_last_poll", "INTEGER DEFAULT 0"),
                               ("rs485_err", "INTEGER DEFAULT 0")]:
                try:
                    conn.execute("ALTER TABLE bms_data ADD COLUMN %s %s" % (_col, _typ))
                except Exception:
                    pass  # 列已存在 -> 忽略 (SQLite 没有 ADD COLUMN IF NOT EXISTS)
            # 索引加速时间范围查询
            conn.execute("CREATE INDEX IF NOT EXISTS idx_recv_time ON bms_data(recv_time)")
            conn.execute("CREATE INDEX IF NOT EXISTS idx_fault ON bms_data(fault)")
            conn.execute("CREATE INDEX IF NOT EXISTS idx_timestamp ON bms_data(timestamp)")

            # 操作审计日志表(记录所有命令下发操作)
            conn.execute("""
                CREATE TABLE IF NOT EXISTS audit_log (
                    id          INTEGER PRIMARY KEY AUTOINCREMENT,
                    recv_time   REAL,               -- 操作时间(unix 秒)
                    cmd         TEXT,               -- 命令名称
                    paras       TEXT,               -- 命令参数(JSON 字符串)
                    source      TEXT,               -- 操作来源(ip 或 session)
                    result      TEXT                -- 下发结果(ok/fail)
                )
            """)
            conn.execute("CREATE INDEX IF NOT EXISTS idx_audit_time ON audit_log(recv_time)")

            # 前端运行日志持久化表(2026-08-13: 防止前端清空/篡改后无法审计)
            conn.execute("""
                CREATE TABLE IF NOT EXISTS operation_log (
                    id          INTEGER PRIMARY KEY AUTOINCREMENT,
                    recv_time   REAL,               -- 日志时间(unix 秒)
                    level       TEXT,               -- 等级: info/success/warn/error
                    msg         TEXT,               -- 日志内容
                    user        TEXT,               -- 当前登录用户
                    source      TEXT                -- 操作来源(ip)
                )
            """)
            conn.execute("CREATE INDEX IF NOT EXISTS idx_oplog_time ON operation_log(recv_time)")
            conn.execute("CREATE INDEX IF NOT EXISTS idx_oplog_level ON operation_log(level)")

            # 用户表(多用户账号/角色权限, 2026-08-08 企业级账号体系)
            conn.execute("""
                CREATE TABLE IF NOT EXISTS users (
                    id          INTEGER PRIMARY KEY AUTOINCREMENT,
                    username    TEXT UNIQUE NOT NULL,   -- 登录用户名
                    password    TEXT NOT NULL,          -- 密码(SHA-256 哈希, 加盐)
                    salt        TEXT NOT NULL,          -- 随机盐
                    role        TEXT NOT NULL DEFAULT 'viewer',  -- admin/operator/viewer
                    display_name TEXT DEFAULT '',
                    created_at  REAL DEFAULT 0,
                    last_login  REAL DEFAULT 0,
                    password_changed_at REAL DEFAULT 0  -- 密码变更时间(改密后强制旧会话失效)
                )
            """)
            conn.execute("CREATE INDEX IF NOT EXISTS idx_users_name ON users(username)")
            # ----- 兼容升级: 旧库没有 password_changed_at 列时补齐(支持改密强退旧会话) -----
            try:
                conn.execute("ALTER TABLE users ADD COLUMN password_changed_at REAL DEFAULT 0")
            except Exception:
                pass
            conn.commit()
        finally:
            conn.close()


# ====================================================================
# 用户管理(多用户账号/角色权限, 2026-08-08 企业级账号体系)
# 角色: admin=管理员(全部权限) operator=操作员(控制+配置) viewer=只读
# ====================================================================
import hashlib as _hashlib
import secrets as _secrets

# 2026-08-21 修复(B9 遗漏): PBKDF2 迭代次数常量 — 之前只有引用无定义,
# _hash_pw/reset_password/change_password 调用即 NameError, 登录改密不可用.
_PBKDF2_ITER = 200000

def _hash_pw(password, salt):
    """PBKDF2-HMAC-SHA256(加盐迭代) 哈希 — 2026-08-19 修复(B9):
    原单轮 SHA-256 可被离线高速爆破; 200k 迭代显著提高破解成本.
    格式: pbkdf2$<iter>$<hex>, 便于识别迭代次数与未来升级.
    (create_user/change_password 写入新格式; verify_user 校验时自动迁移存量)"""
    _dk = _hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"),
                               salt.encode("utf-8"), _PBKDF2_ITER)
    return "pbkdf2$%d$%s" % (_PBKDF2_ITER, _dk.hex())


def _verify_pw(password, salt, stored):
    """校验密码哈希, 兼容旧单轮 SHA-256 存量.
    返回 (ok: bool, new_hash: str|None) — new_hash 非 None 表示旧格式校验通过,
    调用方应将其迁移为新 PBKDF2 哈希落库. 恒定时间比较防时序侧信道."""
    if isinstance(stored, str) and stored.startswith("pbkdf2$"):
        try:
            _it = int(stored.split("$")[1])
        except (ValueError, IndexError):
            _it = _PBKDF2_ITER
        _dk = _hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"),
                                   salt.encode("utf-8"), _it)
        _expect = "pbkdf2$%d$%s" % (_it, _dk.hex())
        return _secrets.compare_digest(_expect, stored), None
    # 旧格式: 单轮 SHA-256(salt+password) — 校验通过则返回新哈希供迁移
    _old = _hashlib.sha256((salt + password).encode("utf-8")).hexdigest()
    if _secrets.compare_digest(_old, stored or ""):
        return True, _hash_pw(password, salt)
    return False, None

def find_user(username):
    """按用户名查用户, 返回 dict 或 None"""
    with _db_lock:
        conn = get_conn()
        try:
            r = conn.execute("SELECT * FROM users WHERE username=?", (username,)).fetchone()
            return dict(r) if r else None
        finally:
            conn.close()

def find_user_by_id(user_id):
    """按用户 ID 查用户(供 login_required 校验密码变更时间), 返回 dict 或 None"""
    with _db_lock:
        conn = get_conn()
        try:
            r = conn.execute("SELECT * FROM users WHERE id=?", (user_id,)).fetchone()
            return dict(r) if r else None
        finally:
            conn.close()

def verify_user(username, password):
    """校验用户名密码, 成功返回 {id, username, role, display_name}, 失败返回 None"""
    u = find_user(username)
    if not u:
        return None
    # G2(2026-08-10): 哈希比较改用恒定时间比较, 防止时序侧信道攻击
    # 2026-08-19 修复(B9): 校验兼容新旧格式, 旧 SHA-256 校验通过后自动迁移 PBKDF2
    _ok, _new_hash = _verify_pw(password, u["salt"], u["password"])
    if not _ok:
        return None
    if _new_hash:
        try:
            with _db_lock:
                c = get_conn()
                try:
                    c.execute("UPDATE users SET password=? WHERE id=?", (_new_hash, u["id"]))
                    c.commit()
                finally:
                    c.close()
            print("[AUTH] 用户 %s 密码哈希已迁移至 PBKDF2" % username)
        except Exception:
            pass
    # 更新最后登录时间
    try:
        with _db_lock:
            c = get_conn()
            try:
                c.execute("UPDATE users SET last_login=? WHERE id=?", (time.time(), u["id"]))
                c.commit()
            finally:
                c.close()
    except Exception:
        pass
    return {"id": u["id"], "username": u["username"], "role": u["role"],
            "display_name": u.get("display_name") or u["username"]}

def create_user(username, password, role="viewer", display_name=""):
    """创建用户(已存在则跳过), 返回 True/False"""
    if find_user(username):
        return False
    salt = _secrets.token_hex(8)
    with _db_lock:
        conn = get_conn()
        try:
            conn.execute(
                "INSERT INTO users(username,password,salt,role,display_name,created_at) VALUES (?,?,?,?,?,?)",
                (username, _hash_pw(password, salt), salt, role, display_name, time.time()))
            conn.commit()
            return True
        finally:
            conn.close()

def list_users():
    """列出全部用户(不含密码哈希, 含角色)"""
    with _db_lock:
        conn = get_conn()
        try:
            rows = conn.execute(
                "SELECT id, username, role, display_name, created_at, last_login FROM users ORDER BY id").fetchall()
            return [dict(r) for r in rows]
        finally:
            conn.close()

def ensure_default_admin(username="admin", password="admin123"):
    """确保至少有一个管理员账号(首次启动默认 admin/admin123)

    2026-09-16 安全加固: 默认密码创建时 password_changed_at 置 0 标记为"未改密",
    登录路由据此强制跳转改密页; 用户手动改密后由 change_password 更新该时间戳.
    """
    admins = [u for u in list_users() if u["role"] == "admin"]
    if not admins:
        create_user(username, password, role="admin", display_name="系统管理员")
        _mark_default_password(username)
        return True
    return False


def _mark_default_password(username):
    """给指定用户打'仍在用默认密码'标记(password_changed_at=0)"""
    conn = sqlite3.connect(DB_PATH)
    try:
        conn.execute("UPDATE users SET password_changed_at = 0 WHERE username = ?", (username,))
        conn.commit()
    finally:
        conn.close()


def is_default_password(username):
    """判断用户是否仍在用初始密码(password_changed_at==0 视为默认密码未改)"""
    conn = sqlite3.connect(DB_PATH)
    try:
        row = conn.execute(
            "SELECT password_changed_at FROM users WHERE username = ?", (username,)
        ).fetchone()
        return row is not None and float(row[0] or 0) == 0
    finally:
        conn.close()

def change_password(user_id, old_password, new_password):
    """修改用户密码: 验证旧密码正确则更新为新密码
    返回: (ok: bool, msg: str)"""
    with _db_lock:
        conn = get_conn()
        try:
            r = conn.execute("SELECT * FROM users WHERE id=?", (user_id,)).fetchone()
            if not r:
                return False, "用户不存在"
            u = dict(r)
        finally:
            conn.close()
    # 校验旧密码(G2: 恒定时间比较)
    # 2026-08-19 修复(B9): 兼容旧 SHA-256 存量(校验通过即更新为新格式)
    _ok, _ = _verify_pw(old_password, u["salt"], u["password"])
    if not _ok:
        return False, "旧密码错误"
    # G4(2026-08-10): 新密码最小长度由 4 提升到 8, 增强暴力破解抵抗
    if len(new_password) < 8:
        return False, "新密码至少 8 位"
    # 更新新密码(重新生成盐) + 记录密码变更时间(用于强制旧会话失效)
    salt = _secrets.token_hex(8)
    with _db_lock:
        conn = get_conn()
        try:
            conn.execute("UPDATE users SET password=?, salt=?, password_changed_at=? WHERE id=?",
                         (_hash_pw(new_password, salt), salt, time.time(), user_id))
            conn.commit()
        finally:
            conn.close()
    return True, "密码已修改"


def reset_password(username, new_password):
    """管理员重置用户密码(无需旧密码), 成功返回 True. 用于成员忘密/入职初始化.
    新密码至少 8 位(与 change_password 一致)."""
    if len(new_password) < 8:
        return False
    u = find_user(username)
    if not u:
        return False
    salt = _secrets.token_hex(8)
    with _db_lock:
        conn = get_conn()
        try:
            conn.execute(
                "UPDATE users SET password=?, salt=?, password_changed_at=? WHERE id=?",
                (_hash_pw(new_password, salt), salt, time.time(), u["id"]))
            conn.commit()
            return True
        finally:
            conn.close()


def delete_user(username):
    """删除用户(离职禁用). 审计日志以用户名文本留存, 不受影响.
    保护: 不能删除最后一个管理员账号(否则无人可管)."""
    u = find_user(username)
    if not u:
        return False, "用户不存在"
    if u["role"] == "admin":
        admins = [x for x in list_users() if x["role"] == "admin"]
        if len(admins) <= 1:
            return False, "不能删除最后一个管理员账号"
    with _db_lock:
        conn = get_conn()
        try:
            conn.execute("DELETE FROM users WHERE id=?", (u["id"],))
            conn.commit()
            return True, "已删除"
        finally:
            conn.close()


# ====================================================================
# 2026-08-16 双通道去重: 本地腿(Mosquitto 订阅)与华为云腿(影子轮询)会先后
#   到达同一帧数据(固件双发同一 JSON, 相同 timestamp)。若不处理会重复落库,
#   历史曲线/报表数据翻倍。此处按 (device_id, timestamp) 做内存窗口去重,
#   窗口内(60s)同 key 只落库一次。多进程部署需换 Redis 等共享缓存。
# ====================================================================
_DEDUP_WINDOW_SEC = 60.0          # 两腿到达时间差上限(本地腿毫秒级, 华为云腿~10s轮询)
_dedup_cache = {}                 # key=(device_id, timestamp) -> last_seen_ts

def _is_dup_frame(payload):
    """同一帧(device_id+timestamp)在窗口内已入库过则返回 True"""
    try:
        ts = int(payload.get("timestamp") or 0)
    except (TypeError, ValueError):
        return False
    if ts <= 0:
        return False              # 无有效时间戳, 不去重(原样入库)
    dev = str(payload.get("device_id") or "bms01")   # 单设备兜底, 与固件 client_id 一致
    key = (dev, ts)
    now = time.time()
    last = _dedup_cache.get(key)
    if last is not None and (now - last) < _DEDUP_WINDOW_SEC:
        return True               # 窗口内重复帧 → 丢弃
    _dedup_cache[key] = now
    # 简单清理: 超过窗口的旧条目删除, 防缓存无限增长
    if len(_dedup_cache) > 8192:
        for k in list(_dedup_cache):
            if (now - _dedup_cache[k]) > _DEDUP_WINDOW_SEC:
                del _dedup_cache[k]
    return False


def insert_data(payload):
    """
    插入一条 bms/data 数据
    payload: dict,与 ESP32 上报 JSON 一致
    """
    # 2026-08-16 双通道去重: 本地腿/华为云腿同一帧只落库一次
    if _is_dup_frame(payload):
        return
    cells = payload.get("cells", [])
    # 上报时先解析串数(供全系统动态适配)
    _parse_series_from_payload(payload)
    if not isinstance(cells, list):
        cells = []
    # 2026-08-10 修复(F1-ext): 安全解析, 畸形元素兜底 0, 绝不抛异常,
    #   避免坏数据冒泡出 insert_data 把设备误判离线 + 静默丢数据点。
    cells_int = safe_int_list(cells, default_if_bad=0)
    # c1~c6 兼容列: 补齐到 6 格(旧库/旧查询仍可用)
    #   顺带修复潜在 IndexError: 串数 <6 时原代码 cells_int 长度不足, 下方
    #   int(cells_int[0..5]) 会越界; 改为对 cells_int 补齐后再取。
    while len(cells_int) < 6:
        cells_int.append(0)
    # 完整数组存 cells_json(串数自适应, 多少串存多少) —— 必须在补齐之后计算
    cells_json_str = json.dumps(cells_int)

    with _db_lock:
        conn = get_conn()
        try:
            # 2026-08-08: recv_time 优先用数据真实上报时间(UTC 毫秒),
            #   断网补发数据带真实时间戳, 可放回断网时段, 历史曲线缺段补全;
            #   无效/缺失时回退为后端接收时刻.
            # 2026-08-08 修复: 设备时间超前(快几分钟)时钳制为后端时间,
            #   否则历史曲线横坐标比真实时间快.
            _recv_sec = time.time()
            try:
                _ts = int(payload.get("timestamp") or 0)
                if _ts > 946684800000:   # > 2000-01-01 视为有效 UTC 毫秒
                    _dev = _ts / 1000.0
                    # 设备时间超前后端超过 60s => 视为时钟异常, 用后端时间
                    if _dev <= time.time() + 60:
                        _recv_sec = _dev
            except (TypeError, ValueError):
                pass
            conn.execute("""
                INSERT INTO bms_data (
                    timestamp, recv_time, soc, soh, v_max, v_min, pack_v,
                    current, temp_max, temp_min, fault,
                    c1, c2, c3, c4, c5, c6, cycle_count, balance_mask,
                    charge_mos, discharge_mos, balance_on, charge_mode, cells_json,
                    insulation_rp, insulation_rn, insulation_ohm_per_v,
                    rs485_online, rs485_last_poll, rs485_err
                ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """, (
                int(payload.get("timestamp", 0)),
                _recv_sec,
                float(payload.get("soc", 0)),
                float(payload.get("soh", 0)),
                int(payload.get("v_max", 0)),
                int(payload.get("v_min", 0)),
                int(payload.get("pack_v", 0)),
                int(payload.get("current", 0)),
                float(payload.get("temp_max", 0)),
                float(payload.get("temp_min", 0)),
                int(payload.get("fault", 0)),
                int(cells_int[0]), int(cells_int[1]), int(cells_int[2]),
                int(cells_int[3]), int(cells_int[4]), int(cells_int[5]),
                int(payload.get("cycle_count", 0)),
                int(payload.get("balance_mask", 0)),
                int(payload.get("charge_mos", -1)),
                int(payload.get("discharge_mos", -1)),
                int(payload.get("balance_on", 0)),
                int(payload.get("charge_mode", 0)),
                cells_json_str,
                int(payload.get("insulation_rp", 0)),
                int(payload.get("insulation_rn", 0)),
                int(payload.get("insulation_ohm_per_v", 0)),
                int(payload.get("rs485_online", 0)),
                int(payload.get("rs485_last_poll", 0)),
                int(payload.get("rs485_err", 0)),
            ))
            conn.commit()
            invalidate_count_cache()  # #7: 新数据入表, 计数缓存立即失效
        except Exception as e:
            print("[DB] insert_data 失败:", e)
        finally:
            conn.close()


def _cells_from_row(r):
    """从 DB 行提取完整单体数组:
    1) 优先 cells_json(串数自适应, 存多少串读多少)
    2) 回退 c1~c6(旧库/旧记录兼容)"""
    j = r.get("cells_json")
    if j:
        try:
            parsed = json.loads(j)
            if isinstance(parsed, list) and len(parsed) > 0:
                return [int(x or 0) for x in parsed]
        except Exception:
            pass
    return [int(r.get("c%d" % i, 0) or 0) for i in range(1, 7)]


# #8: query_history 显式列裁剪. 去掉冗余的 c1~c6(已由 cells_json 覆盖,
#   前端 _cells_from_row 优先读 cells_json, c1~c6 仅作旧库回退, 此处不拉取可减小大查询负载);
#   保留 cells_json 以重建自适应串数 cells 数组.
_HISTORY_COLS = (
    "id, timestamp, recv_time, soc, soh, v_max, v_min, pack_v, current, "
    "temp_max, temp_min, fault, cycle_count, balance_mask, charge_mos, "
    "discharge_mos, balance_on, charge_mode, cells_json"
)

def query_history(minutes=5, limit=2000, start=None, end=None):
    """
    查询历史数据(用于曲线绘制 / 报表 / 导出)
    返回 list[dict] (每条附带 cells=[c1..c6] 数组, 供前端复用)

    时间范围三选一(优先级: start/end 同时给出时 > 仅 minutes):
      - start/end : epoch 秒(闭开区间 [start, end)); 给出后可拉取任意历史区间
      - minutes   : 仅给 minutes 时, 取最近 N 分钟(默认 5)

    2026-08-09 修复: limit 钳制上限 50000, 防止前端误传超大值(如 200000)
    拖慢查询/撑爆内存
    2026-08-20 修复(#4): 上限 50000→100000 —— 7d 历史按 7s 上报约 8.6 万行,
    limit 50000 只取末端 ~4 天, 聚合后 7d 曲线前半为空看不出趋势; SQLite fetchall
    10 万行(cells 已裁剪)内存约几十 MB, 仍可控.
    2026-08-10 优化(#8): SELECT * 改为显式列裁剪, 不拉取冗余的 c1~c6 列.
    2026-08-10 #27: 新增 start/end 任意范围查询(自定义日期/季/年导出与报表).
    """
    try:
        limit = min(max(int(limit), 1), 100000)
    except (TypeError, ValueError):
        limit = 2000

    # 优先 start/end 任意范围查询(自定义日期/季/年导出与报表)
    # 2026-08-19 修复(B7): ORDER BY recv_time DESC 取区间**末端** N 行再反转——
    #   原 ASC LIMIT 在区间行数超限(月报 52 万行/7d 约 6 万帧)时只取区间**开头**,
    #   报表统计错误且 7d 曲线尾部缺失被前端误判离线. 取末端保证"最新数据完整".
    if start is not None or end is not None:
        clauses, params = [], []
        if start is not None:
            clauses.append("recv_time >= ?")
            params.append(float(start))
        if end is not None:
            clauses.append("recv_time < ?")
            params.append(float(end))
        where = " AND ".join(clauses)
        sql = "SELECT %s FROM bms_data WHERE %s ORDER BY recv_time DESC LIMIT ?" % (_HISTORY_COLS, where)
        params.append(limit)
        with _db_lock:
            conn = get_conn()
            try:
                rows = conn.execute(sql, tuple(params)).fetchall()
            finally:
                conn.close()
        rows.reverse()   # 倒序取回后反转, 保持调用方期望的升序
    else:
        cutoff = time.time() - minutes * 60
        with _db_lock:
            conn = get_conn()
            try:
                rows = conn.execute(
                    "SELECT %s FROM bms_data WHERE recv_time >= ? ORDER BY recv_time DESC LIMIT ?"
                    % _HISTORY_COLS,
                    (cutoff, limit)
                ).fetchall()
            finally:
                conn.close()
        rows.reverse()   # 倒序取回后反转, 保持调用方期望的升序
    out = []
    for r in rows:
        d = dict(r)
        # 优先用 param_snapshot 里的串数, 其次 cells 数组长度
        snap = d.get("param_snapshot")
        if isinstance(snap, dict) and snap.get("cell_series_num"):
            update_series_num(snap["cell_series_num"])
        cells = _cells_from_row(d)
        if len(cells) > 0:
            update_series_num(len(cells))
        d["cells"] = cells
        out.append(d)
    return out


# 2026-08-11: 服务端历史聚合(根治"反应慢")——
#   7d 历史原样返回 1 万行(含 cells_json)经 Cloudflare 隧道传输 ~2MB, 浏览器解析+聚合卡顿;
#   改为服务端按范围桶聚合, 仅回传小包(buckets), 前端直接渲染.
#   桶口径/维度与前端 _aggregateHistoryRows 一致: 1h=60s/6h=600s/24h=900s/7d=1800s,
#   总电压(mV→V)/最高温度(0.1℃→℃)/总电流(mA→A)/各单体(mV→V); 同时返回设备最后有效电压时间.
# 2026-08-20 修复(#4 曲线趋势): 24h 原每小时 1 点(仅 24 点)、7d 原每小时 1 点(168 点),
#   且前端 limit 10000 按 7s 上报只覆盖 ~19 小时 → 7d 曲线后面大半为空, 看不出趋势.
#   桶加细: 24h=每 15 分钟 1 点(96 点)、7d=每 30 分钟 1 点(336 点), 趋势更平滑.
HIST_BUCKET = {"1h": 60, "6h": 600, "24h": 900, "7d": 1800}
HIST_RANGE_SEC = {"1h": 3600, "6h": 21600, "24h": 86400, "7d": 604800}


def aggregate_history(rows, range_key):
    """按范围把原始 bms_data 行聚合为固定桶序列(等效前端 _aggregateHistoryRows).
    返回 {labels,volt,temp,curr,cells,bucketTs,off,dayMarkers,last_voltage_ts}."""
    bucket = HIST_BUCKET.get(range_key, 60)
    range_sec = HIST_RANGE_SEC.get(range_key, 3600)
    end_ts = int(time.time())
    start_ts = int((end_ts - range_sec) // bucket) * bucket
    end_bucket = int(end_ts // bucket) * bucket

    cell_count = 0
    for r in (rows or []):
        c = len(r.get("cells") or [])
        if c > cell_count:
            cell_count = c

    bucket_map = {}
    for row in (rows or []):
        ts = float(row.get("recv_time") or 0)
        if not ts:
            continue
        b = int(ts // bucket) * bucket
        if b < start_ts or b > end_bucket:
            continue
        agg = bucket_map.get(b)
        if agg is None:
            agg = {"sumV": 0.0, "vCnt": 0, "sumT": 0.0, "tCnt": 0,
                   "sumC": 0.0, "cCnt": 0,
                   "cellSum": [0.0] * cell_count, "cellN": [0] * cell_count}
            bucket_map[b] = agg
        pv = float(row.get("pack_v") or 0) / 1000.0
        if pv > 0:
            agg["sumV"] += pv
            agg["vCnt"] += 1
        tm = float(row.get("temp_max") or 0) / 10.0
        if tm > 0:
            agg["sumT"] += tm
            agg["tCnt"] += 1
        agg["sumC"] += float(row.get("current") or 0) / 1000.0
        agg["cCnt"] += 1
        cells = row.get("cells") or []
        for i in range(cell_count):
            cv = float(cells[i]) / 1000.0 if (i < len(cells) and cells[i] is not None) else 0.0
            if cv > 0:
                agg["cellSum"][i] += cv
                agg["cellN"][i] += 1

    out = {"labels": [], "volt": [], "temp": [], "curr": [], "cells": [],
           "bucketTs": [], "off": [], "dayMarkers": []}
    prev_day = None
    bucket_idx = 0
    total_buckets = max(1, int((end_bucket - start_ts) // bucket) + 1)
    b = start_ts
    while b <= end_bucket:
        d = datetime.fromtimestamp(b)
        mo = "%02d" % (d.month)
        da = "%02d" % d.day
        hh = "%02d" % d.hour
        mm = "%02d" % d.minute
        day = mo + "-" + da
        if range_key == "7d":
            label = day + " " + hh + ":00"
        else:
            label = (day + " " + hh + ":" + mm) if (prev_day is not None and day != prev_day) else (hh + ":" + mm)
        out["labels"].append(label)
        out["bucketTs"].append(b)
        agg = bucket_map.get(b)
        out["volt"].append(round(agg["sumV"] / agg["vCnt"], 2) if agg and agg["vCnt"] > 0 else None)
        out["temp"].append(round(agg["sumT"] / agg["tCnt"], 1) if agg and agg["tCnt"] > 0 else None)
        out["curr"].append(round(agg["sumC"] / agg["cCnt"], 2) if agg and agg["cCnt"] > 0 else None)
        if agg:
            cells_out = [round(agg["cellSum"][i] / agg["cellN"][i], 3) if agg["cellN"][i] > 0 else None
                         for i in range(cell_count)]
        else:
            cells_out = [None] * cell_count
        out["cells"].append(cells_out)
        if prev_day is not None and day != prev_day:
            out["dayMarkers"].append({"x": bucket_idx / (total_buckets - 1), "day": day})
        prev_day = day
        b += bucket
        bucket_idx += 1

    # 无数据区段(Offline)指示: 相邻桶间隔 > 桶长 → 用上一桶有效电压画水平虚线
    for i in range(len(out["bucketTs"])):
        if i == 0:
            out["off"].append(None)
            continue
        dt = out["bucketTs"][i] - out["bucketTs"][i - 1]
        if dt > bucket and out["volt"][i - 1] is not None and out["volt"][i - 1] > 0:
            out["off"].append(out["volt"][i - 1])
        else:
            out["off"].append(None)

    # 设备最后一条有效电压时间(全局查询, 用于前端准确提示数据缺口)
    last_voltage_ts = 0
    try:
        with _db_lock:
            conn = get_conn()
            try:
                row = conn.execute("SELECT MAX(recv_time) FROM bms_data WHERE pack_v > 0").fetchone()
                if row and row[0]:
                    last_voltage_ts = row[0]
            finally:
                conn.close()
    except Exception:
        last_voltage_ts = 0
    out["last_voltage_ts"] = last_voltage_ts
    return out


def _is_empty_record(r):
    """判断记录是否为"空壳上报"(ESP32 刚启动字段尚未填充).
    判定标准: cells 全0 且 pack_v <=0 且 soc <=0,但 current 可能有值
    => 这种记录会导致 KPI 显示 0-, 不应该作为最新数据展示"""
    cells = _cells_from_row(r)
    pack_v = int(r.get("pack_v") or 0)
    soc = float(r.get("soc") or 0.0)
    cells_zero = all(v == 0 for v in cells)
    pack_zero = pack_v <= 0
    soc_zero = soc <= 0.001
    return cells_zero and pack_zero and soc_zero


def query_latest():
    """查询最新一条数据(向前跳过空壳上报).
    同时把 c1~c6 列重组为 cells=[...] 数组, 便于前端直接使用"""
    with _db_lock:
        conn = get_conn()
        try:
            rows = conn.execute(
                "SELECT * FROM bms_data ORDER BY id DESC LIMIT 50"
            ).fetchall()
        finally:
            conn.close()
    if not rows:
        return None
    # 优先: 找最近一条非空有效记录(最近50条内)
    target = None
    for r in rows:
        if not _is_empty_record(dict(r)):
            target = r
            break
    if target is None:
        target = rows[0]  # 没有有效记录时回退到最新一条
    r = dict(target)
    # 优先用 param_snapshot 里的串数, 其次 cells 数组长度
    snap = r.get("param_snapshot")
    if isinstance(snap, dict) and snap.get("cell_series_num"):
        update_series_num(snap["cell_series_num"])
    # cells 数组: 优先 cells_json(串数自适应), 回退 c1~c6
    cells = _cells_from_row(r)
    if len(cells) > 0:
        update_series_num(len(cells))
    r["cells"] = cells
    # v_max/v_min/pack_v 双重兜底(DB字段 + cells 推导)
    if not r.get("v_max") or int(r.get("v_max") or 0) <= 0:
        r["v_max"] = max(cells) if any(cells) else 0
    if not r.get("v_min") or int(r.get("v_min") or 0) <= 0:
        pos = [v for v in cells if v > 0]
        r["v_min"] = min(pos) if pos else (min(cells) if any(cells) else 0)
    if not r.get("pack_v") or int(r.get("pack_v") or 0) <= 0:
        r["pack_v"] = sum(cells)
    # SOH 单位契约: 设备按百分比(0~100)上报并存库(与 soc 一致).
    # 旧注释里的"主控首次启动会上报 0 -> 强制 95%"已不适用: 当前固件每周期计算 soh
    # (R0 初值=R_NEW -> 100%), 不会上报 0; 且强制 95 会把真实低 SOH(如 80%)掩盖成 95%,
    # 也曾在旧固件 0~1 上报时把 85% 误显为 95%. 此处仅做类型兜底, 不再改写数值.
    try:
        soh_v = float(r.get("soh") or 0)
    except (TypeError, ValueError):
        soh_v = 0.0
    r["soh"] = soh_v
    # cycle_count 保留为 0 (真实初始值)
    return r


def query_faults(limit=100):
    """查询最近故障记录"""
    with _db_lock:
        conn = get_conn()
        try:
            rows = conn.execute(
                "SELECT * FROM bms_data WHERE fault != 0 ORDER BY id DESC LIMIT ?",
                (limit,)
            ).fetchall()
        finally:
            conn.close()
    return [dict(r) for r in rows]


def query_faults_range(start=None, end=None, limit=200000):
    """按时间区间查询故障记录(后端导出用, 审计级完整原始记录)。
    start/end: unix 秒(REAL), 闭区间 [start, end]; 任一为 None 表示不限制该端。
    返回 fault != 0 且 recv_time 落在区间内的全部行, 按 recv_time DESC。
    注意: 返回的是 DB 原始行(含完整 fault 位掩码), 不做前端式单 bit 去重,
    以保证审计完整性(每一帧上报都保留)。"""
    sql = "SELECT * FROM bms_data WHERE fault != 0"
    params = []
    if start is not None:
        sql += " AND recv_time >= ?"
        params.append(float(start))
    if end is not None:
        sql += " AND recv_time <= ?"
        params.append(float(end))
    sql += " ORDER BY recv_time DESC LIMIT ?"
    params.append(int(limit))
    with _db_lock:
        conn = get_conn()
        try:
            rows = conn.execute(sql, params).fetchall()
        finally:
            conn.close()
    return [dict(r) for r in rows]


def query_count():
    """查询总记录数(#7: 加 TTL 缓存, 避免每轮询全表 COUNT(*) 扫描)"""
    now = time.time()
    if now - _count_cache["ts"] < _COUNT_TTL:
        return _count_cache["value"]
    with _db_lock:
        conn = get_conn()
        try:
            row = conn.execute("SELECT COUNT(*) AS c FROM bms_data").fetchone()
        finally:
            conn.close()
    val = row["c"] if row else 0
    _count_cache["value"] = val
    _count_cache["ts"] = now
    return val


def invalidate_count_cache():
    """使计数缓存失效(插入/清理后调用, 保证下次查询重算)"""
    _count_cache["ts"] = 0.0


def cleanup_old(days=7):
    """清理超过 N 天的旧数据,避免数据库无限增长
    #6: 删除后 VACUUM 回收空闲页, 防止 SQLite 文件只增不减(DELETE 仅标空闲);
        并在 init_db 启用 WAL 提升并发. 仅在确有删除时 VACUUM, 避免无谓开销.
    2026-08-19 修复(B10): 同时清理 operation_log(运行日志)——
      原实现 prune_operation_log 已定义但无调用方, 运行日志表无限增长."""
    cutoff = time.time() - days * 86400
    with _db_lock:
        conn = get_conn()
        try:
            cur = conn.execute("DELETE FROM bms_data WHERE recv_time < ?", (cutoff,))
            conn.execute("DELETE FROM audit_log WHERE recv_time < ?", (cutoff,))
            conn.execute("DELETE FROM operation_log WHERE recv_time < ?", (cutoff,))
            deleted = cur.rowcount
            conn.commit()
            if deleted > 0:
                conn.execute("VACUUM")   # 回收空间, 文件不再只增不减
            invalidate_count_cache()
        finally:
            conn.close()


# ====================================================================
# 操作审计日志
# ====================================================================
def insert_audit_log(cmd, paras, source, result):
    """记录一条操作审计日志
    cmd:    命令名称
    paras:  命令参数(dict 或 JSON 字符串)
    source: 操作来源(IP 地址)
    result: 下发结果("ok" 或 "fail")
    """
    import json as _json
    if isinstance(paras, dict):
        paras_str = _json.dumps(paras, ensure_ascii=False)
    else:
        paras_str = str(paras)

    with _db_lock:
        conn = get_conn()
        try:
            conn.execute(
                "INSERT INTO audit_log (recv_time, cmd, paras, source, result) VALUES (?,?,?,?,?)",
                (time.time(), str(cmd), paras_str, str(source), str(result))
            )
            conn.commit()
        except Exception as e:
            print("[DB] insert_audit_log 失败:", e)
        finally:
            conn.close()


def query_audit_log(limit=100, cmd_filter=None):
    """查询操作审计日志
    limit:      最多返回条数
    cmd_filter: 按命令名过滤(可选)
    返回 list[dict]
    """
    with _db_lock:
        conn = get_conn()
        try:
            if cmd_filter:
                rows = conn.execute(
                    "SELECT * FROM audit_log WHERE cmd = ? ORDER BY id DESC LIMIT ?",
                    (cmd_filter, limit)
                ).fetchall()
            else:
                rows = conn.execute(
                    "SELECT * FROM audit_log ORDER BY id DESC LIMIT ?",
                    (limit,)
                ).fetchall()
        finally:
            conn.close()
    return [dict(r) for r in rows]


def insert_operation_log(entries):
    """批量写入前端运行日志。
    entries: list[dict], 每个 dict 需含 recv_time(float), level(str), msg(str),
             可选 user/source(若缺省则在此函数外用 session 填充)。
    返回成功写入条数。"""
    if not entries:
        return 0
    now = time.time()
    with _db_lock:
        conn = get_conn()
        try:
            cur = conn.executemany(
                "INSERT INTO operation_log (recv_time, level, msg, user, source) VALUES (?,?,?,?,?)",
                [
                    (
                        float(e.get("recv_time") or now),
                        str(e.get("level") or "info"),
                        str(e.get("msg") or ""),
                        str(e.get("user") or ""),
                        str(e.get("source") or ""),
                    )
                    for e in entries
                ],
            )
            conn.commit()
            return cur.rowcount
        finally:
            conn.close()


def query_operation_log_range(start=None, end=None, level=None, limit=200000):
    """按时间区间查询前端运行日志(后端导出/回填显示用)。
    start/end: unix 秒(REAL) 闭区间; level: 可选过滤; 按 recv_time DESC。"""
    sql = "SELECT * FROM operation_log WHERE 1=1"
    params = []
    if start is not None:
        sql += " AND recv_time >= ?"
        params.append(float(start))
    if end is not None:
        sql += " AND recv_time <= ?"
        params.append(float(end))
    if level and level != "all":
        sql += " AND level = ?"
        params.append(level)
    sql += " ORDER BY recv_time DESC LIMIT ?"
    params.append(int(limit))
    with _db_lock:
        conn = get_conn()
        try:
            rows = conn.execute(sql, params).fetchall()
        finally:
            conn.close()
    return [dict(r) for r in rows]


def prune_operation_log(keep_days=90):
    """清理超过 keep_days 天的运行日志(默认保留 90 天), 防止表无限增长。"""
    cutoff = time.time() - keep_days * 86400
    with _db_lock:
        conn = get_conn()
        try:
            cur = conn.execute("DELETE FROM operation_log WHERE recv_time < ?", (cutoff,))
            conn.commit()
            return cur.rowcount
        finally:
            conn.close()
