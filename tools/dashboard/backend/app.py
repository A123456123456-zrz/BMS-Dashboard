# -*- coding: utf-8 -*-
"""
BMS Dashboard 后端服务
===================================
基于 Flask + SocketIO + eventlet, 提供:
  1. Web 界面(实时仪表盘 + 历史曲线 + 控制面板)
  2. REST API(设备状态/命令下发/参数设置/数据导出/报表)
  3. WebSocket 实时推送(bms_data/mqtt_status/bms_fault/cmd_resp)
  4. 华为云 IoTDA 设备影子轮询 + 命令下发

启动: python app.py
访问: http://localhost:5000/
"""
# ====================================================================
# 2026-08-08 修复: WebSocket 频繁断线
#   async_mode="eventlet" 模式下必须最先调用 monkey_patch():
#   否则 threading/socket/select 仍是原生阻塞实现, 后台轮询线程
#   (华为云 HTTP 请求, 最长可阻塞数秒)会卡住 eventlet 心跳调度,
#   导致 SocketIO ping/pong 超时, 前端 WebSocket 反复断开降级轮询.
#   必须在 import threading/socket/requests 之前执行.
# ====================================================================
import eventlet
eventlet.monkey_patch()

import os
import time
import json
import datetime
import threading
import secrets as _secrets  # P0-2: SECRET_KEY 未配置时生成随机值

# 2026-08-10 修复: 安全数值解析 helper(根治 F1 系列: 设备畸形数据不抛异常)
from utils import safe_int
# 2026-08-10 #25/#26: 联动规则引擎 / 告警推送单例(供 /api/rules, /api/push_config 读写)
from iotda_rules import RuleEngine, RuleEngine_
from alert_push import AlertPusher, parse_faults
from functools import wraps

from flask import Flask, render_template, request, redirect, session, jsonify, Response, make_response
from flask_socketio import SocketIO

import database as db
from iotda_client import IoTDAClient, props_to_payload, extract_bms_props
# 2026-08-16: 双通道架构的"自建 Mosquitto 消费腿"(与 iotda 双路并存, 互为冗余)
from mqtt_client import MqttClient

# ====================================================================
# 登录密码保护(简单单用户认证)
# 2026-08-07 P0 安全加固: 密码/密钥支持环境变量注入, 不硬编码泄露
#   优先读取环境变量(部署/云环境), 未设置时回退到本地开发默认值(并打警告)
#   Linux:  export BMS_DASH_PASSWORD="yourpass"
#   Windows: set BMS_DASH_PASSWORD=yourpass
# ====================================================================
import os as _os

# ====================================================================
# 2026-08-07 修复: 支持从本地 dashboard.env 加载配置(替代手动设环境变量)
#   背景: AK/SK 移出源码后若未设环境变量, 华为云 REST 链路失效,
#        导致"设备已连 MQTT 但网页仍显示离线"。dashboard.env 已加入
#        .gitignore 不提交仓库; 系统环境变量优先, 文件仅兜底。
#   格式: KEY=VALUE 每行一条, # 开头为注释, 值可带引号
# ====================================================================
def _load_env_file():
    _env_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.env")
    try:
        with open(_env_path, "r", encoding="utf-8") as _f:
            for _line in _f:
                _line = _line.strip()
                if not _line or _line.startswith("#") or "=" not in _line:
                    continue
                _k, _, _v = _line.partition("=")
                _k = _k.strip()
                _v = _v.strip().strip('"').strip("'")
                if _k and _v and _k not in os.environ:
                    os.environ[_k] = _v
    except FileNotFoundError:
        pass

_load_env_file()

DASHBOARD_PASSWORD = _os.environ.get("BMS_DASH_PASSWORD", "bms123")  # 登录密码(环境变量优先)
LOGIN_REQUIRED = _os.environ.get("BMS_LOGIN_REQUIRED", "1") not in ("0", "false", "False")  # True=开启密码保护

if DASHBOARD_PASSWORD == "bms123" and not _os.environ.get("BMS_DASH_PASSWORD"):
    print("[安全警告] 正在使用默认密码 bms123, 建议设置环境变量 BMS_DASH_PASSWORD!")


def login_required(f):
    """API 登录验证装饰器, 未登录或会话超时返回 401
    双重检查: 1.session有logged_in  2.last_activity未超过10分钟
    2026-08-08 修复: 3. 密码变更时间校验 - 改密后旧会话强制失效"""
    @wraps(f)
    def decorated(*args, **kwargs):
        if not LOGIN_REQUIRED:
            return f(*args, **kwargs)
        if "logged_in" not in session:
            return jsonify({"ok": False, "error": "未登录", "need_login": True}), 401
        # 服务端会话超时检查: 超过10分钟无活动强制重新登录
        last = session.get("last_activity", 0)
        if time.time() - last > SESSION_TIMEOUT_SECONDS:
            session.clear()
            return jsonify({"ok": False, "error": "会话超时,请重新登录", "need_login": True}), 401
        # 2026-08-08 修复: 密码变更时间校验(改密后旧会话 cookie 立即失效)
        #   Flask session 是客户端签名 cookie, session.clear() 无法使已发放的旧 cookie 失效,
        #   若用户改密后旧 cookie 仍带着 login_time, 会被误认为有效 -> 需比对密码变更时间.
        _uid = session.get("user_id")
        if _uid is not None:
            try:
                _u = db.find_user_by_id(int(_uid))
                if _u:
                    _pwd_changed = float(_u.get("password_changed_at") or 0)
                    _login_at = float(session.get("login_time") or 0)
                    if _pwd_changed > 0 and _login_at < _pwd_changed:
                        session.clear()
                        return jsonify({"ok": False, "error": "密码已修改,请重新登录", "need_login": True}), 401
            except Exception:
                pass
        # ---- 2026-09-16 安全加固: 默认密码未改则拦截数据接口(403) ----
        # 强制用户先改密; 只放行改密接口自身与登出, 其余 API 一律拒绝
        if session.get("must_change_pw"):
            ep = request.path
            if ep != "/api/change_password" and not ep.startswith("/api/logout"):
                return jsonify({"ok": False,
                                "error": "仍在使用默认密码, 请先修改密码",
                                "must_change_password": True}), 403
        # 刷新活动时间(每次请求都刷新, 实现"无活动10分钟超时")
        session["last_activity"] = time.time()
        return f(*args, **kwargs)
    return decorated


def role_required(*roles):
    """角色权限装饰器(2026-08-08 企业级 RBAC)
    用法: @role_required("admin", "operator")
    权限: admin=全部  operator=控制+配置  viewer=只读
    未登录/无权限返回 403; 旧单密码会话(无 role)默认按 admin 放行(兼容)"""
    def _wrap(f):
        @wraps(f)
        def _decorated(*args, **kwargs):
            if not LOGIN_REQUIRED:
                return f(*args, **kwargs)
            if "logged_in" not in session:
                return jsonify({"ok": False, "error": "未登录", "need_login": True}), 401
            role = session.get("role")
            if role is None:
                # 旧会话/兼容: 无 role 字段时按 admin 放行
                role = "admin"
            if role not in roles:
                return jsonify({"ok": False, "error": "权限不足(需要 %s 角色)" % "/".join(roles)}), 403
            return f(*args, **kwargs)
        return _decorated
    return _wrap

# ====================================================================
# Flask 应用初始化
# ====================================================================
template_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "frontend", "templates")
static_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "frontend", "static")
app = Flask(__name__, template_folder=template_dir, static_folder=static_dir)
# 2026-08-07 P0 安全加固: SECRET_KEY 支持环境变量注入(生产环境务必设置随机值)
# P0-2 加固: 未设置环境变量时自动生成随机密钥, 避免固定默认值被利用伪造会话
# 2026-08-08 修复: 随机密钥持久化到本地文件, 避免每次重启重新生成
#   -> 重启后所有旧会话 cookie 全部失效(前端轮询 401 -> 曲线/历史停更)
_secret_key = _os.environ.get("BMS_DASH_SECRET")
if not _secret_key:
    _secret_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".secret_key")
    try:
        if os.path.isfile(_secret_file):
            with open(_secret_file, "r", encoding="utf-8") as _f:
                _secret_key = _f.read().strip()
        if not _secret_key:
            _secret_key = _secrets.token_hex(24)
            with open(_secret_file, "w", encoding="utf-8") as _f:
                _f.write(_secret_key)
    except Exception:
        _secret_key = _secrets.token_hex(24)   # 文件不可写时退回随机(会话重启后失效, 但可正常登录)
app.config["SECRET_KEY"] = _secret_key
# 禁用模板和静态文件缓存(开发调试用)
app.config["TEMPLATES_AUTO_RELOAD"] = True
app.config["SEND_FILE_MAX_AGE_DEFAULT"] = 0
# Session 配置: 关闭浏览器后重新打开需重新登录
# 双重机制确保失效:
#   1. Cookie 层: 短 lifetime(10分钟) + 每次请求刷新, 关闭浏览器10分钟后 cookie 过期
#   2. 服务端层: 存储 last_activity 时间戳, 超过 10 分钟无活动强制重新登录
#   这样即使 Chrome "恢复会话" 保留了 session cookie, 服务端也会拒绝
from datetime import timedelta
app.config["SESSION_PERMANENT"] = True
app.config["PERMANENT_SESSION_LIFETIME"] = timedelta(minutes=10)  # 10分钟无活动过期
app.config["SESSION_REFRESH_EACH_REQUEST"] = True                 # 每次请求刷新 cookie 过期时间
app.config["SESSION_COOKIE_PATH"] = "/"
app.config["SESSION_COOKIE_HTTPONLY"] = True
app.config["SESSION_COOKIE_SAMESITE"] = "Lax"
# 2026-08-07 P0 安全加固: HTTPS 部署(cloudflared/反代)时开启 Secure Cookie
app.config["SESSION_COOKIE_SECURE"] = _os.environ.get("BMS_COOKIE_SECURE", "0") == "1"
# G3(2026-08-10): 反向代理(Cloudflare / cloudflared / nginx)终止 TLS 时,
#   客户端实际走 https, 但 Flask 请求 scheme 仍为 http, 上面静态判断会漏开 Secure。
#   这里改为 per-request 动态判定: 显式开关 BMS_COOKIE_SECURE=1 优先,
#   或请求头 X-Forwarded-Proto=https 时自动开启, 避免明文 http 下会话 cookie 被窃取。
@app.before_request
def _apply_secure_cookie():
    _secure = (_os.environ.get("BMS_COOKIE_SECURE", "0") == "1") or \
              (request.headers.get("X-Forwarded-Proto", "").lower() == "https")
    app.config["SESSION_COOKIE_SECURE"] = _secure

# ====== 2026-09-07: 轻量令牌桶限流(不依赖 flask-limiter) ======
#   高频 API(/api/status, /api/history, /api/params) 单 IP 30 次/10s,
#   防恶意刷接口打垮 SQLite。登录用户按 session.user_id, 未登录按 remote_addr。
_RATE_BUCKETS = {}   # key -> {"tokens": float, "last": float}
_RATE_LOCK = threading.Lock()
_RATE_LIMIT = 30     # 10 秒内最大请求数
_RATE_WINDOW = 10.0  # 窗口秒数

def _rate_check(key):
    now = time.time()
    with _RATE_LOCK:
        b = _RATE_BUCKETS.get(key)
        if not b:
            b = {"tokens": _RATE_LIMIT, "last": now}
            _RATE_BUCKETS[key] = b
        elapsed = now - b["last"]
        b["tokens"] = min(_RATE_LIMIT, b["tokens"] + elapsed * (_RATE_LIMIT / _RATE_WINDOW))
        b["last"] = now
        if b["tokens"] >= 1.0:
            b["tokens"] -= 1.0
            return True
        return False

@app.before_request
def _apply_rate_limit():
    if request.path.startswith("/api/") and request.path not in ("/api/me",):
        key = session.get("user_id") or request.remote_addr or "unknown"
        if not _rate_check(key):
            return jsonify({"ok": False, "msg": "请求过于频繁, 请稍后再试"}), 429
# 服务端会话超时(秒): 与 PERMANENT_SESSION_LIFETIME 一致
SESSION_TIMEOUT_SECONDS = 1800  # 2026-08-22 改进(A3): 10分钟 → 30分钟无活动超时(减少频繁重登)

# 使用 eventlet 作为异步模式(支持长连接 + SocketIO)
# 2026-08-08 修复: 显式放宽心跳超时(默认 ping_interval=25s 过紧,
#   华为云 HTTP 轮询偶发阻塞时 ping/pong 易超时导致 WebSocket 断线)
socketio = SocketIO(app, async_mode="eventlet", cors_allowed_origins="*",
                    ping_interval=30, ping_timeout=60, max_http_buffer_size=1024 * 1024)

# ====================================================================
# 2026-08-07 安全加固: 全局安全响应头(所有 HTTP 响应)
#   - nosniff          : 禁止 MIME 类型嗅探(防 XSS 变体)
#   - SAMEORIGIN       : 禁止被 iframe 嵌入(防点击劫持)
#   - no-referrer      : 不泄露来源地址
#   - default-src 'self': 基础 CSP(控制台调试时可临时放宽)
# ====================================================================
@app.after_request
def add_security_headers(resp):
    resp.headers.setdefault("X-Content-Type-Options", "nosniff")
    resp.headers.setdefault("X-Frame-Options", "SAMEORIGIN")
    resp.headers.setdefault("Referrer-Policy", "no-referrer")
    resp.headers.setdefault("X-XSS-Protection", "1; mode=block")
    resp.headers.setdefault(
        "Content-Security-Policy",
        "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; "
        "script-src 'self' 'unsafe-inline' 'unsafe-eval'; connect-src 'self' ws: wss:",
    )
    # 2026-08-22 修复(#3 缓存根因): 静态资源强制 no-store——
    #   原 SEND_FILE_MAX_AGE_DEFAULT=0 只发 no-cache, Cloudflare 边缘对 .js 仍
    #   按 max-age=14400(4h)缓存, 用户经隧道访问时浏览器一直加载旧版 app.js,
    #   导致网页端所有修复不生效("联动规则/告警推送无法设置"等).
    #   no-store 让 Cloudflare 与浏览器都绝不缓存, 配合 index.html 的 ?v= mtime
    #   版本戳, 每次部署即生效.
    if request.path.startswith("/static/"):
        resp.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
        resp.headers["Pragma"] = "no-cache"
    return resp

# ====================================================================
# 2026-08-07 安全加固: 敏感操作 CSRF 来源校验
#   对 /api/cmd /api/params 等写操作, 校验 Origin/Referer 必须为本站,
#   防止跨站请求伪造(浏览器跨站发起的请求会被拒绝)
# ====================================================================
_CSRF_PROTECTED = {"/api/cmd", "/api/params", "/api/refresh", "/api/ota_check", "/api/change_password", "/api/ota/publish"}

@app.before_request
def csrf_protect():
    if request.method != "POST" or request.path not in _CSRF_PROTECTED:
        return None
    # 允许本机/隧道/局域网访问: Origin 为空(同源/工具)或为本站来源
    origin = request.headers.get("Origin")
    referer = request.headers.get("Referer")
    if origin:
        # Origin 必须与请求 Host 同源, 或属于本机/隧道域名
        try:
            from urllib.parse import urlparse
            o = urlparse(origin)
            _localhost = ("localhost", "127.0.0.1", "localhost:5000", "127.0.0.1:5000")
            # 2026-08-10 修复(F3): TLS 部署下拒绝明文(http)跨站来源, 防中间人降级攻击。
            #   仅当服务器本身为 https 时才强制; 局域网 http 部署不打扰普通 API 客户端。
            if request.scheme == "https" and o.scheme == "http" and o.netloc not in _localhost:
                return jsonify({"ok": False, "msg": "CSRF: 禁止明文(http)来源下发命令"}), 403
            if o.netloc == request.host:
                return None
            if o.netloc in _localhost:
                return None
            if "dpdns.org" in o.netloc:   # cloudflared 隧道域名
                return None
        except Exception:
            pass
        return jsonify({"ok": False, "msg": "CSRF: 来源不合法"}), 403
    if referer:
        try:
            from urllib.parse import urlparse
            r = urlparse(referer)
            if r.netloc == request.host or "dpdns.org" in r.netloc:
                return None
        except Exception:
            pass
        return jsonify({"ok": False, "msg": "CSRF: 来源不合法"}), 403
    return None

# ====================================================================
# 华为云 IoTDA 配置
# ====================================================================
# 2026-08-07 P0 安全加固: 华为云 IoTDA 凭证改为环境变量注入,
#   严禁把真实 AK/SK 提交进仓库. 未设置时打印警告并以空值运行(启动即报错).
#   Linux:   export BMS_IOTDA_AK=xxx  BMS_IOTDA_SK=xxx  BMS_IOTDA_PROJECT=xxx
#   Windows: set BMS_IOTDA_AK=xxx ...
# ====================================================================
AK         = _os.environ.get("BMS_IOTDA_AK", "")
SK         = _os.environ.get("BMS_IOTDA_SK", "")
PROJECT_ID = _os.environ.get("BMS_IOTDA_PROJECT", "")
REGION     = _os.environ.get("BMS_IOTDA_REGION", "cn-south-4")
DEVICE_ID  = _os.environ.get("BMS_IOTDA_DEVICE", "<IOTDA_DEVICE_ID>_BMS001")
# 2026-08-11: 固件上报间隔(秒). 设备消息日上限 15000, 10s 上报 => 8640/天(余量42%).
#   改小可提升实时性但更耗额度; 下限由固件 clamp 保证 ≤15000/天(间隔≥6s).
DEVICE_REPORT_INTERVAL_SEC = int(os.environ.get("BMS_REPORT_INTERVAL_SEC", "10"))

if not AK or not SK or not PROJECT_ID:
    print("[安全警告] 未设置华为云 IoTDA 环境变量(BMS_IOTDA_AK/SK/PROJECT), 云连接将不可用!")

# IoTDA 客户端全局实例(在 main 中创建)
iotda_client = None

# 2026-08-18 离线判定增强: 本地 EMQX 消费腿全局引用(main 中创建, /api/status 读取其
#   connected / last_msg_ts 判定"EMQX 直连新鲜度" —— 设备断电后 ~60s 内前端即可判离线,
#   不再依赖华为云影子 60s+ 延迟)
local_mqtt = None

# ====================================================================
# P2-2 工程化: /api/status 短缓存(3s)
#   前端轮询 /api/status 频率高于 IoTDA 影子刷新频率(10s/15s),
#   3s 内重复请求直接返回缓存, 减少 DB 查询与不必要的轮询负担
# ====================================================================
_STATUS_CACHE = {"ts": 0.0, "data": None}
_STATUS_CACHE_TTL = 3.0   # 秒

# ====================================================================
# P2-3 工程化: /api/report 短缓存(60s, 按参数key)
#   报表统计量级大(百万行), 前端切换日期/刷新会重复触发;
#   相同参数 60s 内直接返回缓存, 避免重复全量统计阻塞请求线程
# ====================================================================
_REPORT_CACHE = {}        # key -> {"ts": float, "data": dict}
_REPORT_CACHE_TTL = 60.0  # 秒

# ====================================================================
# 命令白名单(允许通过 /api/cmd 下发的命令)
# ====================================================================
ALLOWED_CMDS = {
    "set_charge", "set_discharge", "set_balance", "set_relay",
    "clear_alarm", "restart", "get_info", "get_params",
    "set_param", "ota_check", "ota_upgrade", "reset_params",
    "switch_battery",   # 多电池组切换(网页 deviceSelect 下拉), 设备端实时适配串数/容量
}

# 2026-08-10 修复(F2): 破坏性命令分级 —— 仅 admin 可执行。
#   operator 可运行常规控制/配置(set_charge/discharge/balance/relay/clear_alarm/
#   get_info/get_params/set_param/switch_battery/ota_check), 但 restart/ota_upgrade/
#   reset_params 会重启设备/刷机/清空参数, 误触发危害大, 限 admin-only。
ADMIN_ONLY_CMDS = {"restart", "ota_upgrade", "reset_params"}

# ====== 电池组配置表(网页 deviceSelect 下拉项 ↔ 下发 switch_battery 的参数) ======
# group       下发到设备的电池组编号(BMS-001/002/003 → 1/2/3)
# series      该组目标串数(1~BMS_MAX_CELL_SERIES_NUM, 设备端会校验)
# capacity_mah 该组单体容量 mAh(设备端同步恒流充电电流)
# 新增电池组: 在此追加一项 + 前端 deviceSelect 加一个 <option> 即可
BATTERY_GROUPS = [
    {"group": 1, "name": "BMS-001 主电池组", "short": "BMS-001", "series": 6,  "capacity_mah": 2500},
    {"group": 2, "name": "BMS-002 备用电池组", "short": "BMS-002", "series": 8,  "capacity_mah": 3000},
    {"group": 3, "name": "BMS-003 充电电池组", "short": "BMS-003", "series": 16, "capacity_mah": 2000},
]

# 参数元数据(用于 /api/params 参数校验 + 类型转换)
# 与 components/bms_config/include/bms_config.h 的宏值一一对应:
#   default = 主控宏值            | min/max = 允许用户调整的合理范围
#   unit    = 显示单位(给前端)   | label   = 中文描述(给前端)
PARAM_META = {
    # ====== 电压类 ======
    "cell_ov_prot_mv":   {"type": int, "min": 3800, "max": 4400, "default": 4250, "unit": "mV", "label": "单体过压保护"},
    "cell_ov_warn_mv":   {"type": int, "min": 3900, "max": 4300, "default": 4150, "unit": "mV", "label": "单体过压预警"},
    "cell_ov_recover_mv":{"type": int, "min": 3800, "max": 4350, "default": 4100, "unit": "mV", "label": "单体过压恢复"},
    "cell_uv_prot_mv":   {"type": int, "min": 2500, "max": 3100, "default": 2800, "unit": "mV", "label": "单体欠压保护"},
    "cell_uv_warn_mv":   {"type": int, "min": 2700, "max": 3300, "default": 2900, "unit": "mV", "label": "单体欠压预警"},
    "cell_uv_recover_mv":{"type": int, "min": 2750, "max": 3350, "default": 3000, "unit": "mV", "label": "单体欠压恢复"},
    "cell_dv_warn_mv":   {"type": int, "min": 10,   "max": 500,  "default": 100,  "unit": "mV", "label": "单体压差过大预警"},
    "cell_dv_recover_mv":{"type": int, "min": 5,    "max": 200,  "default": 60,   "unit": "mV", "label": "单体压差恢复"},
    # ====== 电流类 ======
    "chg_oc_prot_ma":    {"type": int, "min": 1000, "max": 30000,"default": 5000, "unit": "mA", "label": "充电过流保护"},
    "chg_oc_warn_ma":    {"type": int, "min": 500,  "max": 20000,"default": 4000, "unit": "mA", "label": "充电过流预警"},
    "dsg_oc_prot_ma":    {"type": int, "min": 1000, "max": 30000,"default": 10000,"unit": "mA", "label": "放电过流保护"},
    "dsg_oc_warn_ma":    {"type": int, "min": 500,  "max": 20000,"default": 8000, "unit": "mA", "label": "放电过流预警"},
    "rated_current_ma":  {"type": int, "min": 500,  "max": 20000,"default": 2500, "unit": "mA", "label": "额定电流 (1C)"},
    "overload_warn_ratio":{"type": float, "min": 1.0,"max": 2.0, "default": 1.05, "unit": "倍", "label": "过载预警倍率"},
    "overload_prot_ratio":{"type": float, "min": 1.1,"max": 3.0, "default": 1.30, "unit": "倍", "label": "过载保护倍率"},
    # ====== 温度类 ======
    "temp_ot_prot_dc":   {"type": int, "min": 400,  "max": 800,  "default": 600,  "unit": "0.1℃", "label": "过温保护"},
    "temp_ot_warn_dc":   {"type": int, "min": 350,  "max": 700,  "default": 500,  "unit": "0.1℃", "label": "过温预警"},
    "temp_ot_recover_dc":{"type": int, "min": 300,  "max": 700,  "default": 450,  "unit": "0.1℃", "label": "过温恢复"},
    "temp_ut_prot_dc":   {"type": int, "min": -300, "max": 100,  "default": -200, "unit": "0.1℃", "label": "低温保护"},
    "temp_ut_warn_dc":   {"type": int, "min": -200, "max": 200,  "default": -100, "unit": "0.1℃", "label": "低温预警"},
    "temp_ut_recover_dc":{"type": int, "min": -100, "max": 300,  "default": 0,    "unit": "0.1℃", "label": "低温恢复"},
    "temp_dtdt_warn":    {"type": float,"min": 0.5, "max": 10.0, "default": 2.0,  "unit": "℃/min", "label": "热失控温升预警"},
    "dv_dt_warn_mvps":   {"type": float,"min": -200.0,"max": -10.0,"default": -50.0,"unit": "mV/s",  "label": "电压降速率预警"},
    # ====== SOC/SOH ======
    "soc_low_warn_pct":  {"type": int, "min": 5,    "max": 50,   "default": 15,   "unit": "%",  "label": "电量过低预警"},
    "soc_low_recover_pct":{"type": int,"min": 10,   "max": 70,   "default": 20,   "unit": "%",  "label": "电量恢复"},
    "soh_low_warn_pct":  {"type": int, "min": 30,   "max": 95,   "default": 80,   "unit": "%",  "label": "健康度衰减预警"},
    # ====== 均衡 ======
    "balance_threshold_mv": {"type": int, "min": 5, "max": 100,  "default": 30,   "unit": "mV", "label": "均衡启动阈值"},
    "balance_stop_mv":   {"type": int, "min": 1,    "max": 50,   "default": 10,   "unit": "mV", "label": "均衡停止阈值"},
    "balance_timeout_min":{"type": int, "min": 5,   "max": 240,  "default": 30,   "unit": "min","label": "均衡超时时间"},
    # ====== 充电策略 ======
    "charge_cc_current_ma":{"type": int,"min": 100,  "max": 10000,"default": 2000, "unit": "mA", "label": "恒流充电电流"},
    "charge_cc_soc_thr":  {"type": float,"min": 0.5,"max": 0.95, "default": 0.80, "unit": "1",  "label": "CC→CV SOC 切换点"},
    "charge_cv_soc_thr":  {"type": float,"min": 0.8,"max": 1.0,  "default": 0.95, "unit": "1",  "label": "充满停止 SOC"},
    # ====== 硬件总体 ======
    "cell_series_num":   {"type": int, "min": 1,    "max": 32,   "default": 6,    "unit": "S",  "label": "串联数"},
    "cell_capacity_mah": {"type": int, "min": 500,  "max": 50000,"default": 2500, "unit": "mAh","label": "单体容量"},
    "nominal_voltage_mv":{"type": int, "min": 3000, "max": 150000,"default": 21600,"unit": "mV", "label": "标称总压"},
    "full_voltage_mv":   {"type": int, "min": 4000, "max": 150000,"default": 25200,"unit": "mV", "label": "满充总压"},
    "cutoff_voltage_mv": {"type": int, "min": 3000, "max": 150000,"default": 16800,"unit": "mV", "label": "放电截止总压"},
    # ====== v8: 电池类型 / SOC 算法选择(网页电池参数配置下发, 设备端实时切换算法) ======
    "battery_type":      {"type": int, "min": 0,    "max": 3,     "default": 1,    "unit": "1",  "label": "电池类型(0=LFP 1=NCM 2=LTO 3=铅酸)"},
    "soc_algo":          {"type": int, "min": 0,    "max": 4,     "default": 3,    "unit": "1",  "label": "SOC算法(0=AEKF 1=纯安时积分 2=OCV查表 3=MCC-EKF默认 4=UKF)"},
    # ====== v9: 均衡策略 / 起始SOC(网页均衡控制下发) ======
    "balance_start_soc_pct": {"type": int, "min": 0, "max": 100, "default": 0, "unit": "%", "label": "均衡起始SOC门槛(0=不限制)"},
    "balance_strategy":      {"type": int, "min": 0, "max": 2,   "default": 0, "unit": "1", "label": "均衡策略(0=电压差触发 1=容量差触发 2=定时均衡)"},
    # ====== v10: 属性上报间隔(网页实时性调节, 受15000/天消息上限约束, 最小7s) ======
    "report_interval":       {"type": int, "min": 7, "max": 3600, "default": 10, "unit": "s", "label": "属性上报间隔(实时性/15000条天上限)"},
}


def _init_param_history_table():
    """用户参数历史表(存储用户通过 Dashboard 修改过的参数值)
    当 ESP32 未上报 param_snapshot 时, /api/params GET 用用户保存的值优先兜底."""
    with db._db_lock:
        conn = db.get_conn()
        try:
            conn.execute("""
                CREATE TABLE IF NOT EXISTS param_history (
                    id          INTEGER PRIMARY KEY AUTOINCREMENT,
                    key         TEXT NOT NULL,
                    value       TEXT NOT NULL,
                    recv_time   REAL NOT NULL,
                    source      TEXT
                )
            """)
            conn.execute("CREATE INDEX IF NOT EXISTS idx_param_key ON param_history(key)")
            conn.commit()
        finally:
            conn.close()


def _load_user_params():
    """加载用户最近一次保存的参数 (key -> 字符串 value)"""
    try:
        with db._db_lock:
            conn = db.get_conn()
            try:
                rows = conn.execute("""
                    SELECT key, value FROM param_history t
                    WHERE id = (SELECT MAX(id) FROM param_history WHERE key = t.key)
                """).fetchall()
                return {r["key"]: r["value"] for r in rows}
            finally:
                conn.close()
    except Exception:
        return {}


def _save_user_param(key, value, source):
    try:
        with db._db_lock:
            conn = db.get_conn()
            try:
                conn.execute(
                    "INSERT INTO param_history(key, value, recv_time, source) VALUES (?,?,?,?)",
                    (key, str(value), time.time(), source or "")
                )
                conn.commit()
            finally:
                conn.close()
    except Exception:
        pass

# ====================================================================
# 登录/登出路由
# ====================================================================
# 2026-08-07 安全加固: 登录暴力破解防护(连续失败锁定)
# 2026-08-08 修复: 失败计数改为按 IP 隔离 + 加锁保护.
#   原因: 原 _LOGIN_FAILS 为全局单例, 任一用户连续输错 N 次密码
#         会锁定全站(所有用户 10 分钟无法登录) —— 多用户场景下的 DoS.
#   新逻辑: 每个 IP 独立计数/锁定, 互不影响; 定期清理过期条目防内存增长.
_LOGIN_FAILS_LOCK = threading.Lock()
_LOGIN_FAILS_BY_IP = {}      # ip -> {"count": n, "locked_until": ts}
_LOGIN_LOCK_THRESHOLD = 5      # 连续失败 N 次触发锁定
_LOGIN_LOCK_SECONDS  = 600     # 锁定 10 分钟

def _login_fail_state(ip):
    """获取某 IP 的失败状态(不存在则初始化), 顺手清理过期条目"""
    now = time.time()
    with _LOGIN_FAILS_LOCK:
        # 清理已过期的条目, 防止 IP 池无限增长
        if len(_LOGIN_FAILS_BY_IP) > 256:
            for k in [k for k, v in _LOGIN_FAILS_BY_IP.items()
                      if v["locked_until"] < now and v["count"] == 0]:
                _LOGIN_FAILS_BY_IP.pop(k, None)
        st = _LOGIN_FAILS_BY_IP.get(ip)
        if st is None:
            st = {"count": 0, "locked_until": 0.0}
            _LOGIN_FAILS_BY_IP[ip] = st
        # 锁定已过期则清零重计
        if st["locked_until"] < now and st["count"] >= _LOGIN_LOCK_THRESHOLD:
            st["count"] = 0
            st["locked_until"] = 0.0
        return st


def _client_ip():
    """获取真实客户端 IP(2026-08-10 修复):
    兼容 cloudflared 隧道/反向代理场景——它们把原始 remote_addr 统一改写为
    127.0.0.1/代理内网 IP, 导致所有访问共享同一个失败计数(一人触发锁定全站挂).
    优先取 X-Forwarded-For 首个地址 / X-Real-IP, 兜底 request.remote_addr.
    2026-08-19 修复(B4): 仅当显式配置 BMS_TRUST_PROXY=1 时才信任 XFF——
      原实现无条件信任, 攻击者伪造 X-Forwarded-For 即可获得全新失败计数,
      使 5 次登录锁定形同虚设(在线暴力破解无限). 服务直连(无代理)时 XFF 不可信."""
    if _os.environ.get("BMS_TRUST_PROXY", "0") == "1":
        for _h in ("X-Forwarded-For", "X-Real-IP"):
            _v = request.headers.get(_h)
            if _v:
                _v = _v.split(",")[0].strip()
                if _v:
                    return _v
    return request.remote_addr or "unknown"

@app.route("/login", methods=["GET", "POST"])
def login():
    """登录页: POST 验证密码, 成功后写入 session + 记录登录时间"""
    if not LOGIN_REQUIRED:
        return redirect("/")

    ip = _client_ip()
    st = _login_fail_state(ip)

    # ---- 安全加固: 锁定检查(仅拦截 POST 登录尝试) ----
    # 2026-08-10 修复: 锁定检查移到 POST 分支内——
    #   原逻辑 GET 打开登录页也被锁定拦截, 用户"没登录进网页就看到已锁定",
    #   且 127.0.0.1 在 cloudflared 隧道场景下是共享地址(所有访问同一 IP),
    #   任一人触发锁定会让所有访问者打开页面即见锁屏. GET 打开页面本身无害,
    #   锁定只需阻止继续尝试登录(POST).
    if request.method == "POST" and time.time() < st["locked_until"]:
        remain = int(st["locked_until"] - time.time())
        return render_template("login.html",
                               error="登录失败次数过多, 已锁定, 请 %d 秒后再试" % remain)

    if request.method == "POST":
        username = request.form.get("username", "").strip()
        password = request.form.get("password", "")
        # 2026-08-08: 多用户账号体系 - 优先查用户表
        user = None
        try:
            user = db.verify_user(username, password)
        except Exception:
            user = None
        # 2026-08-08 修复: 旧密码绕过漏洞 - 兼容回退仅当"用户表完全为空"(首次启动)时启用;
        #   否则一律以用户表为准, 防止用户改密后旧密码(DASHBOARD_PASSWORD)仍能登录
        try:
            _user_count = len(db.list_users())
        except Exception:
            _user_count = 0
        # 2026-08-19 修复(B3): 移除共享密码 bms123 兜底——
        #   原实现用户表为空时任何请求可用 admin+bms123(默认口令)登录, 服务暴露公网即可
        #   获得 admin 全权(下发命令/改保护参数/发恶意固件). ensure_default_admin 已保证
        #   首启必有 admin 账号, 用户表为空窗口几乎不存在; 真为空时应拒绝而非放行.
        # if user is None and _user_count == 0 and password == DASHBOARD_PASSWORD and username in ("", "admin"):
        #     user = {"id": 0, "username": "admin", "role": "admin", "display_name": "管理员"}
        if user is not None:
            with _LOGIN_FAILS_LOCK:
                st["count"] = 0                # 成功登录清零失败计数
                st["locked_until"] = 0.0
            # ---- 安全加固: 会话固定攻击防护(登录前先清空旧会话再重建) ----
            session.clear()
            session["logged_in"] = True
            session["user_id"] = user["id"]
            session["username"] = user["username"]
            session["role"] = user["role"]
            session["display_name"] = user.get("display_name") or user["username"]
            session["login_time"] = time.time()
            session["last_activity"] = time.time()
            session.permanent = True                  # 启用 cookie lifetime 刷新
            # ---- 2026-09-16 安全加固: 默认密码强制首登修改 ----
            # 仍用初始密码(password_changed_at==0)则打会话标记, 数据接口一律 403,
            # 前端据 must_change_password 弹改密框(见 static/app.js)
            try:
                session["must_change_pw"] = db.is_default_password(username)
            except Exception:
                session["must_change_pw"] = False
            # ---- 安全加固: 登录成功写入审计日志 ----
            try:
                db.insert_audit_log("login", {"ok": True, "user": username}, ip, "ok")
            except Exception:
                pass
            return redirect("/")
        # ---- 安全加固: 失败计数 + 锁定(仅该 IP) ----
        with _LOGIN_FAILS_LOCK:
            st["count"] += 1
        try:
            db.insert_audit_log("login", {"ok": False, "user": username}, ip, "fail")
        except Exception:
            pass
        if st["count"] >= _LOGIN_LOCK_THRESHOLD:
            with _LOGIN_FAILS_LOCK:
                st["locked_until"] = time.time() + _LOGIN_LOCK_SECONDS
            return render_template("login.html",
                                   error="登录失败次数过多, 已锁定 10 分钟")
        remain = _LOGIN_LOCK_THRESHOLD - st["count"]
        return render_template("login.html",
                               error="密码错误(还可尝试 %d 次)" % remain)
    return render_template("login.html", error=None)

@app.route("/logout")
def logout():
    """登出: 清除 session"""
    session.clear()
    return redirect("/login")


@app.route("/api/change_password", methods=["POST"])
@login_required
@role_required("admin", "operator", "viewer")
def api_change_password():
    """修改当前登录用户密码(需验证旧密码)"""
    payload = request.get_json(silent=True) or {}
    old_pw = payload.get("old_password", "")
    new_pw = payload.get("new_password", "")
    user_id = session.get("user_id")
    if user_id is None:
        return jsonify({"ok": False, "msg": "未登录"}), 401
    ok, msg = db.change_password(int(user_id), old_pw, new_pw)
    if ok:
        db.insert_audit_log("change_password", {"user": session.get("username", "")},
                            request.remote_addr or "unknown", "ok")
        # 2026-08-08 修复: 改密成功后强制退出, 必须用新密码重新登录
        session.clear()
        return jsonify({"ok": True, "msg": "密码已修改, 请重新登录", "relogin": True})
    else:
        db.insert_audit_log("change_password", {"user": session.get("username", "")},
                            request.remote_addr or "unknown", "fail")
        return jsonify({"ok": ok, "msg": msg})


@app.route("/")
def index():
    """主页:实时仪表盘(需登录)
    页面访问也做会话超时检查, 与 login_required 装饰器逻辑一致"""
    if LOGIN_REQUIRED:
        if "logged_in" not in session:
            return redirect("/login")
        # 服务端会话超时检查(与 login_required 一致)
        last = session.get("last_activity", 0)
        if time.time() - last > SESSION_TIMEOUT_SECONDS:
            session.clear()
            return redirect("/login")
        session["last_activity"] = time.time()
    # 2026-08-21 版本戳(#3): 基于 app.js/style.css 文件 mtime 动态生成 ?v=,
    #   前端文件一旦更新 mtime 即变化 → 浏览器自动拉新, 无需手动改模板版本号.
    #   相比旧硬编码 ?v=1787202631(每次改文件都要手改模板), 部署即生效.
    # 2026-08-22 修复: asset_ver 加入 index.html 的 mtime —— 原实现只取 app.js/style.css,
    #   index.html 的改动不反映到版本戳, 用户无法从徽章版本号判断是否加载最新页面
    #   (导致"选择文件按钮已改但用户看到的还是旧版"无法定位).
    _asset_ver = ""
    try:
        _app_js = os.path.join(static_dir, "app.js")
        _style_css = os.path.join(static_dir, "style.css")
        _idx_html = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "..", "frontend", "templates", "index.html")
        _m = max(os.path.getmtime(_app_js), os.path.getmtime(_style_css),
                 os.path.getmtime(_idx_html))
        _asset_ver = str(int(_m))
    except Exception:
        _asset_ver = str(int(time.time()))
    resp = make_response(render_template("index.html", asset_ver=_asset_ver))
    # 禁止浏览器/Cloudflare 缓存仪表盘 HTML, 确保前端改动即时生效(解决"页面没变"问题)
    resp.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
    resp.headers["Pragma"] = "no-cache"
    resp.headers["Expires"] = "0"
    return resp


# ====================================================================
# REST API (全部需要登录验证)
# ====================================================================

@app.route("/api/status")
@login_required
def api_status():
    """设备实时状态(从数据库取最新一条 + IoTDA 在线状态)
    P2-2: 3s 短缓存, 前端高频轮询时直接命中缓存, 降低 DB 压力
    2026-08-09: 支持 ?device=<device_id> 查询其他设备(从 iotda_client.device_shadows 返回)"""
    # ---- 2026-08-09 多设备: 指定设备(非主)时从多设备轮询缓存直接返回 ----
    _dev_param = (request.args.get("device") or "").strip()
    if _dev_param and iotda_client and _dev_param != iotda_client.device_id:
        # 短名兼容: 前端可能传 BMS002(短名), 而缓存 key 是完整 device_id,
        #   映射到完整 ID(6a718e..._BMS002) 再查 device_shadows
        _full_id = _dev_param
        if not _dev_param.startswith(iotda_client.device_id.split("_")[0]):
            _prefix = iotda_client.device_id.split("_")[0]
            _full_id = "%s_%s" % (_prefix, _dev_param)
        _ds = getattr(iotda_client, "device_shadows", {}) or {}
        _payload = _ds.get(_full_id)
        if not isinstance(_payload, dict):
            _payload = {
                "device_online": bool(getattr(iotda_client, "device_online_map", {}).get(_full_id, False)),
                "no_data": True, "soc": None, "pack_v": 0, "current": 0,
                "fault": 0, "cells": [], "device_status": "OFFLINE",
            }
        _payload["device_id"] = _full_id
        return jsonify(_payload)

    # ---- P2-2 缓存命中: 3s 内返回上次结果 ----
    if _STATUS_CACHE["data"] is not None:
        if (time.time() - _STATUS_CACHE["ts"]) < _STATUS_CACHE_TTL:
            return jsonify(_STATUS_CACHE["data"])
    """双格式兼容:
      - 新格式: pack_v/soc/current/... 直接在顶层 (前端更简单)
      - 老格式: data = {...} 保留, 兼容老代码 (s.data.pack_v)
    同时平铺:  cells/v_max/v_min/temp_max/temp_min/soh/... 与前端期望严格一致"""
    latest = db.query_latest()
    # 兼容旧字段:
    #   iotda_connected = 后端 Dashboard -> 华为云 IoTDA REST API 是否可用
    #   device_online   = ESP32 设备 MQTT -> 华为云 IoTDA 是否在线
    cloud_ok = iotda_client.cloud_accessible if iotda_client else False
    dev_online = iotda_client.device_online if iotda_client else False
    dev_status = (iotda_client.device_last_status if iotda_client else "UNKNOWN") or "UNKNOWN"
    region = (iotda_client.region if iotda_client else "") or ""
    try:
        db_count = db.query_count()
    except Exception:
        db_count = 0
    # 下次查询倒计时
    next_in = 0
    if iotda_client and hasattr(iotda_client, "_last_next_query"):
        try:
            next_in = max(0, int(iotda_client._last_next_query - time.time()))
        except Exception:
            next_in = 0

    # ---------- 检查设备是否有效数据 (从 iotda_client._last_shadow 获取) ----------
    shadow_no_data = False
    shadow_age_ms = 0
    try:
        if iotda_client and hasattr(iotda_client, "_last_shadow"):
            lsh = getattr(iotda_client, "_last_shadow")
            if isinstance(lsh, dict) and lsh:
                shadow_no_data = bool(lsh.get("no_data"))
                # 从影子获取最后上报时间, 计算数据年龄
                let_ms = int(lsh.get("last_event_time_ms") or 0)
                if let_ms > 946684800000:   # > 2000-01-01 视为有效 Unix ms
                    shadow_age_ms = max(0, int(time.time() * 1000) - let_ms)
    except Exception:
        pass
    # 也从 DB 记录计算数据年龄(Flask刚重启时 _last_shadow 可能为 None)
    db_age_ms = 0
    event_age_ms = 0   # ESP32 上报时间年龄(比 DB 入库时间更准确反映数据新鲜度)
    if isinstance(latest, dict):
        try:
            recv_s = float(latest.get("recv_time") or 0)
            if recv_s > 946684800:  # > 2000-01-01
                db_age_ms = max(0, int(time.time() * 1000 - recv_s * 1000))
        except Exception:
            pass
        # ESP32 上报时间(last_event_time_ms): 数据本身的时间戳, 比 DB 入库时间更准确
        try:
            evt_ms = int(latest.get("last_event_time_ms") or 0)
            if evt_ms > 946684800000:  # > 2000-01-01
                event_age_ms = max(0, int(time.time() * 1000) - evt_ms)
        except Exception:
            pass
    # 数据年龄判定(2026-08-06 修复):
    #   旧逻辑: 取影子/DB/ESP32上报年龄的最大值 => DB 记录很老时(如 ESP32 上报 pack_v=0 不写 DB),
    #           db_age_ms 很大, 导致即使影子新鲜也判 no_data=true, 前端永远显示"等待数据"
    #   新逻辑: 影子年龄优先(最权威, 直接反映 ESP32 上报时间),
    #           影子不可用时(Flask 刚重启 _last_shadow=None)才用 DB 年龄 fallback
    if shadow_age_ms > 0:
        effective_age_ms = shadow_age_ms
    elif event_age_ms > 0:
        effective_age_ms = event_age_ms
    else:
        effective_age_ms = db_age_ms
    # 设备离线 / 传感器未接 / 数据陈旧(>200s无新数据) 时标记 no_data
    #   华为云设备详情API有1~2min延迟, 即使ESP32已断开, status可能仍返回ONLINE
    #   因此用数据年龄(200s)作为补充判定, 确保设备实际断开后前端显示"无数据"
    # 2026-08-21 实时性修复: 60s→10s(用户要求: EMQX 与华为云同步时间判定)。
    #   设备正常 7s telemetry/100ms fast 上报; EMQX 无数据 10s + 华为云影子
    #   数据 10s 无新帧 → 双路都确认离线。EMQX 断帧但华为云 10s 内仍有数据
    #   (影子新鲜) → 仍判在线(华为云兜底, 与 OFFLINE_MS=10s 同步)。
    DATA_STALE_MS = 10 * 1000
    # 2026-08-06 修复: Dashboard 刚重启时 _last_shadow=None, shadow_age_ms=0,
    #   fallback 到 db_age_ms(可能很大, 因 pack_v=0 不写 DB), 误判 no_data=True.
    #   新逻辑: 设备在线且影子未标记 no_data 时, 不用 DB 年龄判定:
    #     - 影子年龄已知(>0) → 用影子年龄判定(最准确)
    #     - 影子年龄未知(=0, 刚启动未轮询) → 暂不判 no_data, 等轮询完成后自动更新
    #     - 设备离线 → no_data=True
    #     - 影子标记 no_data → no_data=True
    if dev_online and not shadow_no_data:
        if shadow_age_ms > 0:
            no_data = (shadow_age_ms > DATA_STALE_MS)
        else:
            no_data = False   # 影子未获取但设备在线, 等首次轮询
    else:
        no_data = shadow_no_data or (not dev_online) or (effective_age_ms > DATA_STALE_MS)
    # 临时调试: 暴露 no_data 判定的各分量(定位前端"等待数据"问题)
    _debug_no_data = {
        "shadow_no_data": shadow_no_data,
        "dev_online": dev_online,
        "effective_age_ms": effective_age_ms,
        "shadow_age_ms": shadow_age_ms,
        "db_age_ms": db_age_ms,
        "event_age_ms": event_age_ms,
        "DATA_STALE_MS": DATA_STALE_MS,
        "no_data_result": no_data,
    }

    # ---------- 平铺 latest 到顶层 (前端无需 s.data.*, 直接 s.pack_v) ----------
    resp = {
        "ok": True,
        "iotda_connected": cloud_ok,         # 后端->云端 REST API 可用性(原字段,兼容)
        "cloud_accessible": cloud_ok,         # 同上,显式命名
        "device_online": dev_online,          # 设备端 MQTT 是否在线(用户真正关心的)
        "device_status": dev_status,          # ONLINE/OFFLINE/FROZEN/...
        "online": dev_online,                 # 兼容: 前端原来用 online 表示设备是否在线
        "region": region,
        "db_count": db_count,
        "next_query_in": next_in,
        "no_data": no_data,                   # 设备离线/传感器未接时 True
        "series_num": db.get_series_num(),    # 当前电池串联数(前端据此动态渲染)
        "_debug": _debug_no_data,
        "device_msg_day_estimate": (int(86400 / DEVICE_REPORT_INTERVAL_SEC) if DEVICE_REPORT_INTERVAL_SEC > 0 else 0),
        "device_msg_budget": 15000,
        "cloud_api_used_24h": (iotda_client._api_used_24h() if iotda_client else 0),
        "cloud_api_budget": (iotda_client._api_budget if iotda_client else 15000),
        "report_interval_sec": DEVICE_REPORT_INTERVAL_SEC,             # 临时调试: no_data 判定分量
    }

    # 2026-08-18 离线判定增强: 暴露 EMQX 直连消费腿状态与新鲜度。
    #   emqx_direct_online = 后端->EMQX 本地消费腿在线(双链路融合状态卡片用)
    #   emqx_age_ms        = 距最近一帧设备数据(telemetry/fast/data 任一)的毫秒数
    #   设备断电瞬间 MQTT 连接断开, 6s keepalive + 重连探测 → 至多 ~10s 内不再有新帧;
    #   前端据此可在 ~60s 内判离线, 远快于华为云影子(60~120s)。
    try:
        _lm = local_mqtt
        _lm_connected = bool(getattr(_lm, "connected", False))
        _lm_last = float(getattr(_lm, "last_msg_ts", 0.0) or 0.0)
        resp["emqx_direct_online"] = _lm_connected
        resp["emqx_age_ms"] = max(0, int(time.time() * 1000 - _lm_last * 1000)) if _lm_last > 0 else -1
    except Exception:
        resp["emqx_direct_online"] = False
        resp["emqx_age_ms"] = -1

    # no_data 时仍返回 DB 最后有效电压, 供前端画灰色虚线(断线指示线)
    if no_data and isinstance(latest, dict):
        try:
            _pv = int(latest.get("pack_v") or 0)
            if _pv > 0:
                resp["last_pack_v"] = _pv
        except Exception:
            pass

    # no_data 时不返回 DB 历史数据, 前端显示"--"和断线
    # 2026-08-06 修复: 优先用 iotda_client._last_shadow 实时影子数据
    #   旧逻辑: 始终用 db.query_latest() → ESP32 上报 pack_v=0 不写 DB 时,
    #           /api/status 一直返回 DB 旧数据, 前端永远不更新
    #   新逻辑: 影子有值时优先用影子(实时), 影子不可用时 fallback 到 DB
    if not no_data:
        try:
            if iotda_client and hasattr(iotda_client, "_last_shadow"):
                _lsh = getattr(iotda_client, "_last_shadow")
                if isinstance(_lsh, dict) and _lsh and not _lsh.get("no_data"):
                    latest = _lsh   # 用影子实时数据覆盖 DB 旧数据
        except Exception:
            pass
    if not no_data and isinstance(latest, dict):
        # ---- 先把 latest 保存为 data (副本), 再同步平铺到顶层 ----
        #   修复: 原 resp["data"] = latest 在兜底之后执行, 导致 data 的兜底被覆盖
        data_copy = dict(latest)
        resp["data"] = data_copy
        # 平铺所有数据列到顶层 (pack_v/soc/current/.../c1..c6/cells/balance_mask)
        for k, v in latest.items():
            if k not in resp:
                resp[k] = v
        # ---------- 问题1修复: timestamp 有效性过滤 + last_event_time_ms / data_age_ms 预计算 ----------
        #   判定: timestamp 若 < 2000-01-01 (946684800) 视为无效 (如 ESP32 boot tick 364780ms)
        MIN_UNIX_SEC = 946684800  # 2000-01-01 00:00 UTC
        # 原始 last_event_time_ms 优先 (新鲜影子的真实上报UTC)
        let_ms = 0
        try:
            let_raw = latest.get("last_event_time_ms") or resp.get("last_event_time_ms") or 0
            let_ms = int(let_raw or 0)
        except Exception:
            let_ms = 0
        recv_ts_s = 0
        try:
            # recv_time: DB 入库时的 Unix 秒 (若存在)
            recv_ts_s = float(latest.get("recv_time") or 0)
        except Exception:
            recv_ts_s = 0
        # 判定: 数据新鲜度 => 供前端直接使用,不再让前端自己乱算
        now_ms = int(time.time() * 1000)
        real_event_ms = 0
        if let_ms > 0 and let_ms > (MIN_UNIX_SEC * 1000):
            real_event_ms = let_ms
        elif recv_ts_s >= MIN_UNIX_SEC:
            real_event_ms = int(recv_ts_s * 1000)
        age_ms = 0
        if real_event_ms > 0:
            age_ms = max(0, now_ms - real_event_ms)
        resp["last_event_time_ms"] = real_event_ms if real_event_ms > 0 else None
        data_copy["last_event_time_ms"] = resp["last_event_time_ms"]
        resp["data_age_ms"] = age_ms if age_ms > 0 else None
        data_copy["data_age_ms"] = resp["data_age_ms"]
        # 过滤无效 timestamp (如 ESP32 boot tick = 364780 之类)
        for key_node in (resp, data_copy):
            try:
                ts_raw = key_node.get("timestamp")
                if ts_raw is not None:
                    ts_v = int(ts_raw or 0)
                    # 若为秒, 换算 ms 后仍小于 MIN*1000 则视为无效
                    check_v = ts_v * 1000 if ts_v < 1e12 else ts_v
                    if check_v < MIN_UNIX_SEC * 1000:
                        key_node["timestamp"] = None
            except Exception:
                pass
        # ---------- 问题4修复: 数据来源标注 (让前端知道是实时还是DB缓存) ----------
        # 如果 iotda_client._last_shadow 存在且 event_time 对得上 => 新鲜影子
        # 否则 => DB 历史缓存
        is_realtime = False
        try:
            if iotda_client and hasattr(iotda_client, "_last_shadow"):
                lsh = getattr(iotda_client, "_last_shadow")
                if isinstance(lsh, dict) and lsh:
                    sh_ts = lsh.get("last_event_time_ms") or 0
                    if sh_ts > 0 and real_event_ms > 0 and abs(sh_ts - real_event_ms) <= 60 * 1000:
                        is_realtime = True
        except Exception:
            is_realtime = False
        # 方案3修复: 必须年龄<=180s才算实时; 否则华为云device_online=True也可能只是详情API尚未刷新(有1~2min延迟)
        #          => 避免 data_realtime=True 但 data_age=460s 的矛盾状态
        is_realtime = is_realtime and (age_ms > 0) and (age_ms <= 180 * 1000)
        if not is_realtime:
            is_realtime = (dev_online and (0 < age_ms <= 180 * 1000))
        resp["data_realtime"] = bool(is_realtime)
        data_copy["data_realtime"] = resp["data_realtime"]
        # data_source 字段 (与前端 updateDataSource 参数对应)
        existing_src = resp.get("data_source") or None
        if existing_src is None:
            if is_realtime:
                resp["data_source"] = {"pack_v":"real","soc":"real","current":"real","temp":"real","soh":"real","voltage":"real","fault":"real"}
            else:
                resp["data_source"] = {"pack_v":"db_cache","soc":"db_cache","current":"db_cache","temp":"db_cache","soh":"db_cache","voltage":"db_cache","fault":"db_cache"}
            data_copy["data_source"] = resp["data_source"]
        # ---------- 兜底逻辑: no_data 时全部跳过, 前端显示"无数据" ----------
        _skip_fallback = no_data

        if not _skip_fallback:
            # 1) soh: 默认 95% (新出厂电池), 主控第一次启动还没安时积分会返回 0
            try:
                soh_v = float(resp.get("soh") or 0)
            except (TypeError, ValueError):
                soh_v = 0.0
            if soh_v <= 1:
                resp["soh"] = 95.0
                data_copy["soh"] = 95.0     # 同步写入 data
            # 2) cycle_count: None -> 0 (DB 中 NULL 值, 或安时积分尚未累计)
            try:
                cyc_raw = resp.get("cycle_count")
                cyc_v = int(cyc_raw or 0)
            except (TypeError, ValueError):
                cyc_v = 0
            if (cyc_raw is None) or (cyc_v < 0):
                cyc_v = max(0, cyc_v)
                resp["cycle_count"] = cyc_v
                data_copy["cycle_count"] = cyc_v
            # 3) cells / pack_v / v_max / v_min 二次兜底(与 database.py query_latest 对齐)
            #    串数动态化: 以 cells 实际长度 / 设备上报串数为准, 不再写死 6
            cs = resp.get("cells")
            series_n = db.get_series_num()
            if isinstance(cs, list) and len(cs) > 0:
                db.update_series_num(len(cs))
                series_n = len(cs)
            if not isinstance(cs, list) or len(cs) < series_n:
                cs = [int(resp.get("c%d" % i, 0) or 0) for i in range(1, series_n + 1)]
                resp["cells"] = cs
                data_copy["cells"] = cs
            if isinstance(cs, list) and any(v > 0 for v in cs):
                # pack_v 兜底
                try:
                    pv = int(resp.get("pack_v") or 0)
                except (TypeError, ValueError):
                    pv = 0
                if pv <= 0:
                    pv = sum(int(x or 0) for x in cs)
                    resp["pack_v"] = pv
                    data_copy["pack_v"] = pv
                # v_max / v_min 兜底
                try:
                    vmax = int(resp.get("v_max") or 0)
                except (TypeError, ValueError):
                    vmax = 0
                if vmax <= 0:
                    vmax = max(int(x or 0) for x in cs)
                    resp["v_max"] = vmax
                    data_copy["v_max"] = vmax
                try:
                    vmin = int(resp.get("v_min") or 0)
                except (TypeError, ValueError):
                    vmin = 0
                positive_cells = [int(x or 0) for x in cs if int(x or 0) > 0]
                if vmin <= 0 and positive_cells:
                    vmin = min(positive_cells)
                    resp["v_min"] = vmin
                    data_copy["v_min"] = vmin
    else:
        resp["data"] = None

    # ---- P2-2 写入短缓存(下次 3s 内请求直接命中) ----
    _STATUS_CACHE["ts"] = time.time()
    _STATUS_CACHE["data"] = resp
    return jsonify(resp)


@app.route("/api/history")
@login_required
def api_history():
    """历史数据查询(默认最近60分钟)
    2026-08-09 跨天统计: 响应附加 span_days(起止时间跨越自然天数),
    前端据此优先判断坐标轴是否进入"带日期"渲染模式, 避免曲线刷新闪烁
    2026-08-10 #27: 支持 start/end(epoch 秒)任意历史区间拉取(自定义日期导出/曲线)"""
    minutes = request.args.get("minutes", 60, type=int)
    start = request.args.get("start", type=float)
    end = request.args.get("end", type=float)
    if start is not None or end is not None:
        limit = min(request.args.get("limit", 50000, type=int), 100000)
    else:
        # 2026-08-20 修复(#4): 原钳 50000 —— 7d 按 7s 上报约 8.6 万行, 前端传 100000
        #   覆盖 7 天全范围(曲线后半不再为空); 与 database.query_history 上限对齐.
        limit = min(request.args.get("limit", 2000, type=int), 100000)
    rows = db.query_history(minutes=minutes, limit=limit, start=start, end=end)
    # 2026-08-11: 服务端聚合(agg=1)——历史视图直接回传按范围桶聚合后的小包(~168 点),
    #   替代前端拉取 1 万行原始数据后再聚合, 根治 7d 历史"反应慢". 仅对 minutes 模式生效
    #   (start/end 自定义区间仍走原始数据, 供导出/报表使用).
    agg = request.args.get("agg", 0, type=int)
    if agg == 1 and start is None and end is None:
        _min_to_range = {60: "1h", 360: "6h", 1440: "24h", 10080: "7d"}
        range_key = _min_to_range.get(minutes, "1h")
        agg_data = db.aggregate_history(rows, range_key)
        return jsonify({"ok": True, "agg": agg_data, "range": range_key})
    # 计算时间跨度(跨天数): 首尾 recv_time 的日期差+1(单日=1, 跨零点=2, 跨3天=3, 跨月按日计)
    span_days = 1
    if rows:
        try:
            t0 = float(rows[0].get("recv_time") or 0)
            t1 = float(rows[-1].get("recv_time") or 0)
            if t0 > 0 and t1 > 0:
                import datetime as _dt
                d0 = _dt.datetime.fromtimestamp(t0).date()
                d1 = _dt.datetime.fromtimestamp(t1).date()
                span_days = max(1, (d1 - d0).days + 1)
        except Exception:
            span_days = 1
    return jsonify({"ok": True, "data": rows, "span_days": span_days})


@app.route("/api/faults")
@login_required
def api_faults():
    """故障记录查询"""
    limit = request.args.get("limit", 100, type=int)
    rows = db.query_faults(limit=limit)
    return jsonify({"ok": True, "data": rows})


@app.route("/api/faults/export")
@login_required
def api_faults_export():
    """按时间区间导出故障记录(审计级, 服务端从数据库原始记录生成 CSV)。
    直接读取 bms_data 中 fault != 0 的原始行, 不受前端"删除/隐藏"影响,
    防止有人篡改前端展示制造"无故障"假象。
    Query: start/end = unix 秒(epoch, REAL), 闭区间; 缺省 end=now, start=30天前。
    返回 text/csv (UTF-8-SIG, Excel 中文友好) + Content-Disposition: attachment。
    """
    import csv, io
    try:
        start = request.args.get("start", type=int)
        end = request.args.get("end", type=int)
    except (TypeError, ValueError):
        return jsonify({"ok": False, "msg": "start/end 需为 epoch 秒"}), 400
    if end is None:
        end = int(time.time())
    if start is None:
        start = end - 30 * 86400
    rows = db.query_faults_range(start=start, end=end)

    # 数据为中国 BMS, 统一按 UTC+8 展示时间
    def fmt(ts):
        try:
            return (datetime.datetime.fromtimestamp(float(ts), datetime.timezone.utc)
                    .astimezone(datetime.timezone(datetime.timedelta(hours=8)))
                    .strftime("%Y-%m-%d %H:%M:%S"))
        except Exception:
            return str(ts)

    buf = io.StringIO()
    w = csv.writer(buf)
    w.writerow([
        "时间(UTC+8)", "recv_time_epoch", "故障码(hex)",
        "故障位(hex)", "最高等级", "故障描述(全部)",
        "SOC(%)", "SOH(%)", "总压(mV)", "电流(mA)",
        "温度max(0.1C)", "温度min(0.1C)", "循环次数", "均衡掩码", "数据行ID",
    ])
    for r in rows:
        recv = r.get("recv_time") or 0
        fault = int(r.get("fault") or 0)
        faults = parse_faults(fault)  # [(name, level, bit), ...] 已按危害排序
        if faults:
            bits = ";".join("0x%X" % b for _, _, b in faults)
            names = ";".join(n for n, _, _ in faults)
            level = faults[0][1]  # 最高危害等级在前
        else:
            bits, names, level = "", "", ""
        w.writerow([
            fmt(recv), recv, "0x%X" % fault, bits, level, names,
            r.get("soc"), r.get("soh"), r.get("pack_v"), r.get("current"),
            r.get("temp_max"), r.get("temp_min"), r.get("cycle_count"),
            r.get("balance_mask"), r.get("id"),
        ])
    data = buf.getvalue().encode("utf-8-sig")
    resp = make_response(data)
    resp.headers["Content-Type"] = "text/csv; charset=utf-8"
    fname = "BMS_历史故障_%s_%s.csv" % (
        datetime.datetime.fromtimestamp(start, datetime.timezone.utc).strftime("%Y%m%d"),
        datetime.datetime.fromtimestamp(end, datetime.timezone.utc).strftime("%Y%m%d"),
    )
    resp.headers["Content-Disposition"] = "attachment; filename=%s" % fname
    return resp


@app.route("/api/refresh", methods=["POST"])
@login_required
@role_required("admin", "operator")
def api_refresh():
    """手动立即刷新一次(前端 refreshBtn 按钮调用)
    绕过 180s/60s 轮询等待, 0 延迟触发一次华为云查询, 适合恢复网/电后立刻看结果"""
    if iotda_client is None:
        return jsonify({"ok": False, "msg": "IoTDA 客户端未初始化"})
    iotda_client.trigger_refresh()
    db.insert_audit_log("manual_refresh", {"user_ip": request.remote_addr or "unknown"},
                        request.remote_addr or "unknown", "ok")
    return jsonify({"ok": True, "msg": "已触发立即刷新,3秒内更新界面"})


@app.route("/api/iotda/push", methods=["POST"])
def api_iotda_push():
    """华为云 IoTDA 数据转发接收端点(2026-08-09 新增, 订阅推送)
    ─────────────────────────────────────────────────────────
    用途: 华为云 IoTDA「数据转发」→ 第三方应用服务(HTTP推送) → 本端点
          设备数据由"后端轮询影子"升级为"云端主动推送", 实时性更高,
          且可转发至 OBS/DIS/Kafka 免本地 DB 膨胀(配置见说明书).
    鉴权: 需在 dashboard.env 配置 BMS_IOTDA_PUSH_TOKEN,
          请求头 X-Push-Token 携带该令牌; 未配置则端点关闭(返回 404).
    格式: 华为云数据转发消息体(JSON), 属性字段映射同设备上报(soc/pack_v/...)
    """
    _push_token = os.environ.get("BMS_IOTDA_PUSH_TOKEN", "").strip()
    if not _push_token:
        return jsonify({"ok": False, "error": "push 端点未启用(BMS_IOTDA_PUSH_TOKEN 未配置)"}), 404
    # 令牌校验(请求头或 query)
    _tok = request.headers.get("X-Push-Token") or request.args.get("token") or ""
    if _tok != _push_token:
        return jsonify({"ok": False, "error": "鉴权失败"}), 401
    try:
        data = request.get_json(force=True, silent=True) or {}
    except Exception:
        data = {}
    if not isinstance(data, dict):
        return jsonify({"ok": False, "error": "格式错误"}), 400

    # 华为云转发消息可能是嵌套结构(notify_data/body), 尽量提取设备属性
    payload = data
    if isinstance(data.get("notify_data"), dict):
        _nd = data["notify_data"]
        payload = _nd.get("body") if isinstance(_nd.get("body"), dict) else _nd
    # 忽略非本设备消息
    _dev = (payload.get("device_id") or os.environ.get("BMS_IOTDA_DEVICE", ""))
    if isinstance(payload.get("device_id"), str) and iotda_client:
        if payload["device_id"] != iotda_client.device_id:
            return jsonify({"ok": True, "msg": "非本设备消息,已忽略"})

    # 2026-08-11 修复: IoTDA 数据转发推送的是固件原始 camelCase + services 包裹(payload),
    #   必须归一化为 snake_case 后再入库/广播, 否则 db.insert_data(读 pack_v/v_max/temp_max/
    #   cells) 与前端全部拿到 0/空, 网页端电压/温度/单体全掉(与轮询路径 _shadow_to_payload 同映射)
    _props = extract_bms_props(payload)
    if _props:
        _normalized = props_to_payload(_props)
        if _normalized is not None:
            payload = _normalized

    # 落库(与轮询共用同一写库, 空壳过滤一致)
    try:
        db.insert_data(payload)
    except Exception as e:
        print("[PUSH] 写库失败: %s" % e)
    # 广播到前端 + 告警推送 + 联动规则
    try:
        socketio.emit("bms_data", payload)
        _f = int(payload.get("fault") or 0)
        if _f:
            socketio.emit("bms_fault", {"fault": _f})
    except Exception:
        pass
    try:
        from alert_push import AlertPusher
        from iotda_rules import RuleEngine_
        if _f:
            AlertPusher.notify_fault(_f, payload)
        RuleEngine_.evaluate(payload, iotda_client, AlertPusher)
    except Exception:
        pass
    print("[PUSH] 已接收云端转发数据: soc=%s pack_v=%s fault=0x%X" %
          (payload.get("soc"), payload.get("pack_v"),
           safe_int(payload.get("fault"))))  # 2026-08-19 修复(B1): 坏类型字段不抛异常
    return jsonify({"ok": True, "msg": "received"})


@app.route("/api/debug/shadow")
@login_required
def api_debug_shadow():
    """临时调试端点: 查看 iotda_client 内部状态(排查 no_data 问题)"""
    info = {"ok": True}
    if iotda_client:
        lsh = getattr(iotda_client, "_last_shadow", None)
        info["last_shadow"] = lsh
        info["last_cmp"] = getattr(iotda_client, "_last_cmp", None)
        info["cloud_accessible"] = iotda_client.cloud_accessible
        info["device_online"] = iotda_client.device_online
        info["device_last_status"] = iotda_client.device_last_status
        info["last_next_query"] = getattr(iotda_client, "_last_next_query", 0)
        # 直接实时查一次影子
        try:
            shadow = iotda_client._fetch_shadow()
            status = iotda_client._fetch_device_status()
            et_ms = iotda_client._get_event_time_ms(shadow) if shadow else 0
            import time as _t
            now_ms = int(_t.time() * 1000)
            info["live_shadow"] = shadow
            info["live_status"] = status
            info["live_event_time_ms"] = et_ms
            info["live_age_ms"] = max(0, now_ms - et_ms) if et_ms > 0 else 0
            info["now_ms"] = now_ms
        except Exception as e:
            info["live_error"] = str(e)
    else:
        info["error"] = "iotda_client is None"
    return jsonify(info)


@app.route("/api/diag")
@login_required
def api_diag():
    """网页实时诊断: 一键检查设备状态/上报新鲜度/字段新旧/packV值
    供日常维护排查使用(对应 BMS-V2 物模型)"""
    import time as _t
    now = _t.time()
    diag = {
        "ok": True,
        "ts": now,
        "time_str": _t.strftime("%Y-%m-%d %H:%M:%S", _t.localtime(now)),
    }
    if not iotda_client:
        diag["error"] = "iotda_client 未初始化"
        return jsonify(diag)

    # ---- 1. 后端访问华为云状态 ----
    diag["cloud_accessible"] = iotda_client.cloud_accessible
    diag["device_online"] = iotda_client.device_online
    diag["device_last_status"] = iotda_client.device_last_status
    diag["db_count"] = db.query_count() if hasattr(db, "query_count") else 0

    # ---- 2. 实时拉一次设备详情 + 影子 ----
    try:
        status = iotda_client._fetch_device_status()
        diag["status_api"] = status
        diag["status_online"] = bool(status and status.get("online"))
        # 2026-08-08 修复: 实时拉取成功时用实时结果覆盖后台缓存(_loop 轮询间隔
        #   在"从未在线"时最长 900s, 缓存可能过期; 若 API 调用失败则保留缓存值)
        if status is not None:
            diag["device_online"] = diag["status_online"]
            diag["device_last_status"] = status.get("status") or diag["device_last_status"]
        if status:
            upd = status.get("connection_status_update_time")
            if upd:
                try:
                    t = _t.mktime(_t.strptime(str(upd)[:19], "%Y-%m-%dT%H:%M:%S"))
                    diag["status_update_age_min"] = round((now - t) / 60, 1)
                except Exception:
                    diag["status_update_age_min"] = None
    except Exception as e:
        diag["status_error"] = str(e)

    shadow = None
    try:
        shadow = iotda_client._fetch_shadow()
    except Exception as e:
        diag["shadow_error"] = str(e)

    # ---- 3. 影子属性: 新鲜度 + 字段新旧 + 关键值 ----
    diag["shadow_has_data"] = bool(shadow)
    props = {}
    if shadow:
        for svc in shadow:
            sid = svc.get("service_id") if isinstance(svc, dict) else getattr(svc, "service_id", None)
            if sid != "BMS":
                continue
            reported = svc.get("reported") if isinstance(svc, dict) else getattr(svc, "reported", None)
            if isinstance(reported, dict):
                p = reported.get("properties") or {}
                if isinstance(p, dict):
                    props.update(p)
        diag["shadow_prop_count"] = len(props)
        diag["shadow_keys"] = sorted(props.keys())

        # 上报新鲜度
        et_ms = iotda_client._get_event_time_ms(shadow)
        now_ms = int(now * 1000)
        if et_ms > 0:
            diag["shadow_last_report_age_sec"] = max(0, (now_ms - et_ms) // 1000)
            diag["shadow_last_report_age_hint"] = _fmt_age(now_ms - et_ms)
        else:
            diag["shadow_last_report_age_sec"] = None
            diag["shadow_last_report_age_hint"] = "未知(影子无有效 event_time)"

    # ---- 4. 字段新旧判定(BMS-V2 新字段 vs V1 旧字段) ----
    new_keys = ["vMax", "vMin", "packV", "tempMax", "tempMin", "cellVoltages",
                "balanceMask", "cycleCount", "chargeMos", "dischargeMos",
                "balanceOn", "chargeMode", "workState", "fwVersion", "capacityAh", "cellCount"]
    old_keys = ["v_max", "v_min", "pack_v", "temp_max", "temp_min", "cells",
                "balance_mask", "cycle_count", "charge_mos", "discharge_mos",
                "balance_on", "charge_mode"]
    has_new = [k for k in new_keys if k in props]
    has_old = [k for k in old_keys if k in props]
    diag["fields_new"] = has_new
    diag["fields_old"] = has_old
    if has_new and not has_old:
        diag["field_version"] = "new"      # 新固件 + 新物模型
    elif has_old and not has_new:
        diag["field_version"] = "old"      # 旧固件(需烧录新固件)
    elif has_new and has_old:
        diag["field_version"] = "mixed"    # 混合(过渡期)
    else:
        diag["field_version"] = "unknown"

    # ---- 5. 关键值: packV/pack_v + cellVoltages + 全 0 判定 ----
    pv = props.get("packV") or props.get("pack_v") or 0
    cv = props.get("cellVoltages") or props.get("cells") or []
    try:
        pv_int = int(pv or 0)
    except Exception:
        pv_int = 0
    try:
        cv_list = list(cv) if isinstance(cv, list) else []
    except Exception:
        cv_list = []
    diag["pack_v"] = pv_int
    diag["cell_voltages"] = cv_list
    # 2026-08-10 修复(F1 一致性): cv_list 元素可能含畸形字符串("N/A"),
    #   用 safe_int 兜底, 避免诊断接口 500。
    diag["cell_voltages_all_zero"] = bool(cv_list) and all(safe_int(c, 0) == 0 for c in cv_list)
    diag["soc"] = props.get("soc")
    diag["current"] = props.get("current")

    # ---- 6. 综合结论(便于直接定位问题) ----
    problems = []
    if not diag.get("cloud_accessible"):
        problems.append("后端连不上华为云 REST API(检查网络/证书/AK-SK)")
    if not diag.get("device_online"):
        problems.append("华为云显示设备离线(检查 ESP32 MQTT 连接/设备密钥)")
    if diag.get("shadow_has_data") and not diag.get("shadow_prop_count"):
        problems.append("影子存在但无 BMS 服务属性(检查产品模型 service_id)")
    if diag.get("field_version") == "old":
        problems.append("设备仍上报旧字段名(v_max/pack_v): 需烧录新固件(上报 vMax/packV)")
    if diag.get("field_version") == "unknown" and diag.get("shadow_has_data"):
        problems.append("影子字段既非新物模型也非旧字段(检查产品模型/上报内容)")
    if diag.get("shadow_last_report_age_sec") is not None and diag.get("shadow_last_report_age_sec", 99999) > 300:
        problems.append("设备已超过 5 分钟未上报(影子陈旧, 检查 ESP32 上报任务/网络)")
    if diag.get("cell_voltages_all_zero"):
        problems.append("单体电压全为 0(检查 LTC6804 接线 / HW_ENABLE_LTC6804 配置)")
    if diag.get("pack_v", 0) <= 0 and not diag.get("cell_voltages_all_zero"):
        problems.append("packV 为 0 但存在有效单体(检查整组电压计算/上报)")
    diag["problems"] = problems
    diag["health"] = "OK" if not problems else "ISSUES"

    # 最近 DB 记录时间(数据是否在写库)
    try:
        latest = db.query_latest()
        if isinstance(latest, dict):
            rt = latest.get("recv_time") or 0
            if rt:
                diag["db_last_write_ago_sec"] = int(now - float(rt))
            else:
                diag["db_last_write_ago_sec"] = None
    except Exception:
        diag["db_last_write_ago_sec"] = None
    return jsonify(diag)


def _fmt_age(ms):
    """毫秒 -> 可读时长"""
    s = max(0, int(ms // 1000))
    if s < 60:
        return "%ds 前" % s
    m = s // 60
    if m < 60:
        return "%d 分 %ds 前" % (m, s % 60)
    h = m // 60
    return "%d 小时 %d 分前" % (h, m % 60)


@app.route("/api/cmd", methods=["POST"])
@login_required
@role_required("admin", "operator")
def api_cmd():
    """命令下发到设备(通过 IoTDA /messages API)
    请求体: {"cmd": "set_charge", "enable": true, ...}
    返回: {"ok": bool, "msg": "已下发" 或 "下发失败: <错误详情>"}"""
    payload = request.get_json(silent=True) or {}
    cmd = payload.get("cmd") or payload.get("command_name")
    if not cmd:
        return jsonify({"ok": False, "msg": "缺少 cmd 字段"}), 400

    # 命令白名单校验(防止非法命令)
    if cmd not in ALLOWED_CMDS:
        return jsonify({"ok": False, "msg": "不允许的命令: " + str(cmd)}), 400

    # 2026-08-10 修复(F2): 破坏性命令限 admin-only。
    #   兼容旧会话(无 role 字段按 admin 放行); 正常会话取 session.role。
    if cmd in ADMIN_ONLY_CMDS:
        _role = session.get("role") or "admin"
        if _role != "admin":
            return jsonify({
                "ok": False,
                "msg": "权限不足: 命令 %s 需要 admin 角色(当前角色 %s)" % (cmd, _role)
            }), 403

    # D1 修复(2026-08-10): 前端历史上以顶层平铺传参({cmd, enable:true}),
    #   而 iotda_client.publish_cmd 只透传 paras/args → enable/charge/mask 等
    #   顶层参数被丢弃, 导致控制命令"只能关、不能开"(enable 恒 false)。
    #   这里把顶层非元字段并入 paras, 同时兼容 paras 子对象与顶层平铺两种格式;
    #   前端 sendDeviceCmd 已同步改为规范 paras 格式, 双保险互不依赖。
    meta_fields = {"cmd", "command_name", "timestamp", "paras", "args"}
    extra = {k: v for k, v in payload.items() if k not in meta_fields}
    if extra:
        merged = dict(payload.get("paras") or {})
        merged.update(extra)
        payload = dict(payload)
        payload["paras"] = merged

    # ===== 2026-08-18 安全互锁(服务端兜底, 防止绕过前端直接调 API) =====
    #   1) 设备处于保护/故障状态(error 级)时禁止开启充电/放电 MOS 与主继电器
    #   2) 充电 MOS 与放电 MOS 互斥: 开启一方前若另一方仍吸合则拒绝(避免同时充放)
    _paras = payload.get("paras") or {}
    _want_on = bool(_paras.get("enable", _paras.get("charge", False)) or
                    (_paras.get("charge") is True) or (_paras.get("discharge") is True))
    if cmd in ("set_charge", "set_discharge", "set_relay") and _want_on:
        # 从 DB 取最近一帧的故障与 MOS 状态做互锁判定(无数据时保守放行)
        # 2026-08-19 修复(B8): 优先按命令目标设备取影子帧做互锁——
        #   原实现用 db.query_latest() 取全库最新一条, 多设备时设备 A 的 fault/MOS
        #   会误判设备 B 的命令(可绕过保护互锁). bms_data 表无 device_id 列,
        #   iotda_client.device_shadows 按设备缓存最新 payload(fault/mos 齐全).
        _lk = None
        try:
            _dev_target = _paras.get("device") or payload.get("device") or ""
            if _dev_target and iotda_client:
                _ds_map = getattr(iotda_client, "device_shadows", {}) or {}
                # 短名兼容(BMS002 → 6a718e..._BMS002), 与 /api/status 一致
                _full = _dev_target
                if iotda_client.device_id and not _dev_target.startswith(iotda_client.device_id.split("_")[0]):
                    _full = "%s_%s" % (iotda_client.device_id.split("_")[0], _dev_target)
                _lk = _ds_map.get(_full)
            if not _lk:
                _lk = db.query_latest()
        except Exception:
            _lk = None
        _lkf = int((_lk or {}).get("fault") or 0) if isinstance(_lk, dict) else 0
        # error 级保护位(与前端 FAULT_PROT_MASK 一致): 单体过压/过流/欠压/短路/过温/低温/热失控
        _PROT_MASK = 0x00002 | 0x00020 | 0x00008 | 0x00080 | 0x00200 | 0x01000 | 0x00800 | 0x00400
        if _lkf & _PROT_MASK:
            return jsonify({"ok": False, "msg": "安全互锁: 设备保护中(fault=0x%X), 禁止开启 %s" % (_lkf, cmd)}), 409
        # 充放互斥: 依据最近帧的 charge_mos / discharge_mos 回读状态
        if cmd == "set_charge" and bool((_lk or {}).get("discharge_mos") or 0):
            return jsonify({"ok": False, "msg": "安全互锁: 放电MOS仍在吸合, 请先关闭再开启充电"}), 409
        if cmd == "set_discharge" and bool((_lk or {}).get("charge_mos") or 0):
            return jsonify({"ok": False, "msg": "安全互锁: 充电MOS仍在吸合, 请先关闭再开启放电"}), 409

    # ===== 2026-08-19 修复(B5): set_param 复用 PARAM_META 校验 =====
    #   原实现 api_cmd 只做命令白名单, paras.key/value 完全不校验, 可绕过 /api/params
    #   的 min/max/类型校验下发任意越界值(如把过压保护调到 99999), 危害电池安全.
    if cmd == "set_param":
        _sk = _paras.get("key")
        _sv = _paras.get("value")
        if not _sk or _sv is None:
            return jsonify({"ok": False, "msg": "set_param 缺少 key 或 value"}), 400
        _meta = PARAM_META.get(_sk)
        if not _meta:
            return jsonify({"ok": False, "msg": "未知参数: " + str(_sk)}), 400
        try:
            if _meta["type"] is int:
                _fv = float(_sv)
                if abs(_fv - round(_fv)) > 1e-9:
                    return jsonify({"ok": False, "msg": "参数 %s 必须为整数" % _sk}), 400
                _cv = int(round(_fv))
            else:
                _cv = _meta["type"](_sv)
        except (ValueError, TypeError):
            return jsonify({"ok": False, "msg": "参数类型错误"}), 400
        if _cv < _meta["min"] or _cv > _meta["max"]:
            return jsonify({"ok": False,
                            "msg": "参数超出范围(%s~%s)" % (_meta["min"], _meta["max"])}), 400
        payload["paras"] = dict(_paras)
        payload["paras"]["value"] = _cv   # 用规范化后的值下发(与 /api/params 一致)

    # 2026-08-21 EMQX 主通道命令下行(用户架构要求: EMQX 为主, 华为云备胎):
    #   优先经本地消费腿发布到 bms/<id>/cmd(EMQX 直连设备, 实时),
    #   失败(设备 EMQX 腿离线/未订阅)再回退华为云 /messages 兜底.
    result = None
    try:
        if local_mqtt is not None and local_mqtt.publish_cmd(payload):
            result = {"ok": True, "channel": "emqx", "msg": "已下发(EMQX)"}
            print("[CMD] 已通过 EMQX 下发: %s" % cmd)
    except Exception as _e:
        print("[CMD] EMQX 下发异常, 回退华为云: %s" % _e)
    if result is None:
        result = iotda_client.publish_cmd(payload) if iotda_client else {"ok": False, "code": "NO_CLIENT", "msg": "客户端未初始化"}
        if isinstance(result, dict):
            result["channel"] = "huawei"
    # 兼容旧式布尔返回(防御性处理)
    if isinstance(result, bool):
        result = {"ok": result, "msg": "已下发" if result else "下发失败"}
    ok = bool(result.get("ok"))
    # 操作审计日志(记录所有命令下发操作)
    db.insert_audit_log(cmd, payload.get("paras", {}), request.remote_addr or "unknown",
                        "ok" if ok else "fail")
    # 透传错误详情, 前端可据此显示具体失败原因
    msg = "已下发" if ok else ("下发失败: " + result.get("code", "") + " " + result.get("msg", ""))
    return jsonify({"ok": ok, "msg": msg, "payload": payload, "channel": result.get("channel", "")})


@app.route("/api/params", methods=["POST"])
@login_required
@role_required("admin", "operator")
def api_params():
    """参数设置(通过 IoTDA 下发 set_param 命令)
    请求体: {"key": "cell_ov_prot_mv", "value": 3650}"""
    payload = request.get_json(silent=True) or {}
    key = payload.get("key")
    value = payload.get("value")
    if not key or value is None:
        return jsonify({"ok": False, "msg": "缺少 key 或 value"}), 400

    # 参数元数据校验
    meta = PARAM_META.get(key)
    if not meta:
        return jsonify({"ok": False, "msg": "未知参数: " + str(key)}), 400
    meta_type = meta["type"]
    if meta_type is int:
        # 2026-08-10 修复(F4): 拒绝带小数部分的输入, 避免 int(4250.9)→4250 静默截断
        try:
            fval = float(value)
        except (ValueError, TypeError):
            return jsonify({"ok": False, "msg": "参数类型错误"}), 400
        if abs(fval - round(fval)) > 1e-9:
            return jsonify({"ok": False, "msg": "参数 %s 必须为整数(收到 %.3f)" % (key, fval)}), 400
        value = int(round(fval))
    else:
        try:
            value = meta_type(value)
        except (ValueError, TypeError):
            return jsonify({"ok": False, "msg": "参数类型错误"}), 400
    if value < meta["min"] or value > meta["max"]:
        return jsonify({"ok": False, "msg": "参数超出范围(%s~%s)" % (meta["min"], meta["max"])}), 400

    # 立即落盘到 param_history(即使设备离线, 下次打开网页也能看到/重新下发)
    _save_user_param(key, value, request.remote_addr or "unknown")

    # 2026-08-08 修复: 串数立即同步到全局(即使设备尚未上报新串数)
    #   否则 GET /api/params 仍返回旧串数, 前端 loadParams 无法跟随切换
    if key == "cell_series_num":
        db.update_series_num(int(value))
        print("[PARAMS] cell_series_num 已同步全局 -> %d" % int(value))
        # 2026-08-09 诊断: 记录期望串数, iotda_client 轮询时对比设备上报长度,
        #   若设备未确认(固件无 messages/down 处理/消息未达), 日志会明确提示
        try:
            iotda_client._pending_series_num = int(value)
        except Exception:
            pass

    # 2026-08-11: 上报间隔同步到全局, 使 /api/status 的"设备消息/天"估算实时准确
    if key == "report_interval":
        global DEVICE_REPORT_INTERVAL_SEC
        DEVICE_REPORT_INTERVAL_SEC = int(value)
        print("[PARAMS] report_interval 已同步全局 -> %ds (估算 %d 条/天)"
              % (int(value), int(86400 / max(1, int(value)))))

    # 2026-08-21 EMQX 主通道命令下行(与 /api/cmd 一致): 先经本地消费腿发 EMQX,
    #   失败再回退华为云 /messages 兜底(用户架构要求: EMQX 为主, 华为云备胎).
    cmd_payload = {"cmd": "set_param", "paras": {"key": key, "value": value}}
    result = None
    try:
        if local_mqtt is not None and local_mqtt.publish_cmd(cmd_payload):
            result = {"ok": True, "channel": "emqx", "msg": "已下发(EMQX)"}
            print("[PARAMS] 已通过 EMQX 下发 %s=%s" % (key, value))
    except Exception as _e:
        print("[PARAMS] EMQX 下发异常, 回退华为云: %s" % _e)
    if result is None:
        result = iotda_client.publish_cmd(cmd_payload) if iotda_client else {"ok": False, "msg": "客户端未初始化"}
        if isinstance(result, dict):
            result["channel"] = "huawei"
    if isinstance(result, bool):
        result = {"ok": result}
    ok = bool(result.get("ok"))
    db.insert_audit_log("set_param", {"key": key, "value": value}, request.remote_addr or "unknown",
                        "ok" if ok else "fail")
    return jsonify({"ok": ok, "msg": ("参数已下发" if ok else "下发失败") + (
        ("(" + result.get("msg", "") + ")") if not ok and result.get("msg") else ""),
        "channel": result.get("channel", "")})


@app.route("/api/params", methods=["GET"])
@login_required
def api_params_get():
    """参数查询(GET). 优先级:
      1) 最近一条 DB 记录的 param_snapshot 字段(如 ESP32 端上报过)
      2) 用户通过 Dashboard 保存过的 param_history (用户自定义值)
      3) PARAM_META.default (与 bms_config.h 宏值一致)
    返回格式: {key: {value, min, max, type, default, unit, label, source}}"""
    latest = db.query_latest() or {}
    snap = latest.get("param_snapshot") if isinstance(latest, dict) else None
    if not isinstance(snap, dict):
        snap = {}
    user_p = _load_user_params()
    result = {}
    for key, meta in PARAM_META.items():
        T = meta["type"]
        source = "default"
        val = meta["default"]
        if key in snap:
            try:
                val = T(snap[key]); source = "device"
            except Exception:
                pass
        if source != "device" and key in user_p:
            try:
                val = T(user_p[key]); source = "user"
            except Exception:
                pass
        result[key] = {
            "value":   T(val),
            "min":     meta["min"],
            "max":     meta["max"],
            "type":    T.__name__,
            "default": meta["default"],
            "unit":    meta.get("unit", ""),
            "label":   meta.get("label", key),
            "source":  source,   # device = 来自设备上报 | user = 用户修改过 | default = bms_config.h 宏值
        }
    return jsonify(result)


# ====================================================================
# 全局错误处理器: 确保所有 /api/* 错误统一返回 JSON, 避免前端出现 Unexpected token '<'
# ====================================================================
@app.errorhandler(401)
def handle_401(e):
    if request.path.startswith("/api/") or request.path.startswith("/socket.io/"):
        return jsonify({"ok": False, "error": "未登录", "need_login": True}), 401
    return redirect("/login")

@app.errorhandler(403)
def handle_403(e):
    if request.path.startswith("/api/"):
        return jsonify({"ok": False, "error": "禁止访问"}), 403
    return e

@app.errorhandler(404)
def handle_404(e):
    if request.path.startswith("/api/"):
        return jsonify({"ok": False, "error": "接口不存在: " + request.path}), 404
    # 非 API 路径: 主页不存在的路径重定向到首页(SPA 风格), 避免乱跳
    return redirect("/")

@app.errorhandler(500)
def handle_500(e):
    import traceback
    err_msg = str(e) + "\n" + traceback.format_exc(limit=5)
    print("[Server] 500 Error:", err_msg)
    if request.path.startswith("/api/"):
        return jsonify({"ok": False, "error": "服务器内部错误: " + str(e)}), 500
    return "Server Error: " + str(e), 500


# ====================================================================
# OTA 固件升级服务 (ESP32 通过 HTTP 拉取 version.json + bms.bin)
# 注意: /api/ota/* 允许匿名访问(ESP32 升级没有登录态)
# ====================================================================
# OTA 固件查找优先级:
#   1) <dashboard>/firmware/bms.bin      (用户把固件放这里, 便于发布)
#   2) <project>/build/bms.bin           (ESP-IDF 编译产物, 开发期自动读取)
#   3) 找不到 → 返回 404 (ESP32 会 WARN 跳过, 属正常)
import glob as _glob
import hashlib as _hashlib

def _find_firmware_bin():
    """查找最新的 bms.bin, 返回绝对路径或 None"""
    dash_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # tools/dashboard 根
    proj_root = os.path.dirname(os.path.dirname(dash_dir))                  # 项目根(BMS System)
    candidates = [
        os.path.join(dash_dir, "firmware", "bms.bin"),
        os.path.join(proj_root, "build", "bms.bin"),
    ]
    # 额外: firmware/ 目录下所有最新的 bin
    fw_dir = os.path.join(dash_dir, "firmware")
    if os.path.isdir(fw_dir):
        for fn in sorted(os.listdir(fw_dir), reverse=True):
            if fn.lower().endswith(".bin") and ("bms" in fn.lower() or "firmware" in fn.lower()):
                candidates.insert(0, os.path.join(fw_dir, fn))
                break
    for p in candidates:
        if os.path.isfile(p) and os.path.getsize(p) > 1024 * 100:  # 至少 >100KB
            return p
    return None


# 版本号: 读取固件目录下的 version.json (优先级1), 否则自动从固件 mtime 生成
def _read_version_meta():
    dash_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    proj_root = os.path.dirname(os.path.dirname(dash_dir))
    fw_bin = _find_firmware_bin()

    # 1. 尝试 firmware/version.json 或 build/ 目录下 version.json
    for vp in [os.path.join(dash_dir, "firmware", "version.json"),
               os.path.join(proj_root, "build", "version.json")]:
        if os.path.isfile(vp):
            try:
                with open(vp, "r", encoding="utf-8") as f:
                    data = json.load(f)
                if "version" in data and "url" in data:
                    return data
            except Exception:
                pass

    # 2. 自动生成: 版本 = 固件大小时间戳哈希, URL = /api/ota/firmware.bin
    #    bms_config.h 里的 BMS_FIRMWARE_VERSION 默认 1.0.1, 这里把 build mtime 当构建号
    if fw_bin is None:
        return {"version": "1.0.0", "url": ""}

    st = os.stat(fw_bin)
    # 简单版本号: 1.0.${日数自20260101}
    import datetime as _dt
    base = _dt.datetime(2026, 1, 1)
    day_idx = max(1, int((_dt.datetime.fromtimestamp(st.st_mtime) - base).total_seconds() // 86400) + 1)
    auto_version = "1.0.%d" % day_idx
    return {
        "version": auto_version,
        "url": "/api/ota/firmware.bin",       # 相对路径, 让 ESP32 自己拼 host
        "size": st.st_size,
        # 2026-08-09 修复: open 用 with 确保关闭(原一次性读取未关文件句柄)
        "md5": _hashlib.md5(open(fw_bin, "rb").read()).hexdigest() if st.st_size < 20 * 1024 * 1024 else "",
        "build_time": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(st.st_mtime)),
    }


@app.route("/api/ota/version.json", methods=["GET"])
def ota_version_json():
    """OTA 版本信息: 供 ESP32 拉取, 返回 {version, url, size, build_time}
    注意: 这个接口允许匿名访问 (ESP32 没有 Dashboard session)"""
    meta = _read_version_meta()
    # ESP32 没有 session, url 需要完整 http://host:port/...
    # 2026-08-19 修复(B6): 不再信任 Host/X-Forwarded-Host 拼接完整 URL——
    #   原实现攻击者发一条带 Host: evil.com 的请求即可获得指向恶意服务器的版本响应,
    #   配合明文 HTTP 可将 ESP32 固件下载重定向到攻击者服务器(刷入恶意固件).
    #   现优先用 BMS_OTA_BASE_URL 环境变量固定 base(生产配 https://emqx.bms0605.dpdns.org);
    #   未配置则返回相对路径, 由设备端用自身配置的服务器地址拼接, 不采纳请求头.
    _ota_base = (_os.environ.get("BMS_OTA_BASE_URL") or "").rstrip("/")
    if meta.get("url", "").startswith("/") and _ota_base:
        meta["url"] = _ota_base + meta["url"]
    # 未配置 BMS_OTA_BASE_URL: 保持相对路径, 不拼任何请求头
    resp = jsonify(meta)
    # 禁止缓存, 每次重新拉
    resp.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
    return resp


@app.route("/api/ota/firmware.bin", methods=["GET"])
def ota_firmware_bin():
    """OTA 固件二进制文件: ESP32 下载 bms.bin"""
    fw_bin = _find_firmware_bin()
    if fw_bin is None:
        return jsonify({"ok": False, "error": "未找到固件文件, 请先 idf.py build"}), 404

    st = os.stat(fw_bin)
    def generate():
        with open(fw_bin, "rb") as f:
            while True:
                chunk = f.read(64 * 1024)
                if not chunk:
                    break
                yield chunk
    fname = os.path.basename(fw_bin)
    resp = Response(generate(), mimetype="application/octet-stream",
                    direct_passthrough=True)
    resp.headers["Content-Length"] = st.st_size
    resp.headers["Content-Disposition"] = "attachment; filename=%s" % fname
    resp.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
    return resp


@app.route("/api/ota/download/<filename>", methods=["GET"])
def ota_download_file(filename):
    """多版本固件下载: /api/ota/download/<filename>
    从 firmware/ 目录下载指定固件(支持历史版本保留在 firmware/ 下,
    version.json 的 url 可写成 /api/ota/download/bms_1.0.1.bin 指向任意版本)
    防目录穿越: 仅接受纯文件名(basename 原样), 拒绝路径分隔符/../"""
    dash_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    fw_dir = os.path.join(dash_dir, "firmware")
    # 防目录穿越: basename 后必须与原始一致 → 不允许包含路径分隔符
    safe_name = os.path.basename(filename)
    if safe_name != filename or not safe_name:
        return jsonify({"ok": False, "error": "非法文件名"}), 400
    fw_path = os.path.join(fw_dir, safe_name)
    if not os.path.isfile(fw_path) or os.path.getsize(fw_path) < 1024 * 100:
        return jsonify({"ok": False, "error": "固件不存在: %s" % safe_name}), 404

    st = os.stat(fw_path)
    def generate():
        with open(fw_path, "rb") as f:
            while True:
                chunk = f.read(64 * 1024)
                if not chunk:
                    break
                yield chunk
    resp = Response(generate(), mimetype="application/octet-stream",
                    direct_passthrough=True)
    resp.headers["Content-Length"] = st.st_size
    resp.headers["Content-Disposition"] = "attachment; filename=%s" % safe_name
    resp.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
    return resp


# (可选: 让 ESP32 能知道自己的 Dashboard 的 IP/域名, 便于拼 OTA URL)
@app.route("/api/ota/info", methods=["GET"])
def ota_info():
    meta = _read_version_meta()
    fw_bin = _find_firmware_bin()
    return jsonify({
        "ok": True,
        "version": meta.get("version"),
        "size": meta.get("size", (fw_bin and os.path.getsize(fw_bin))),
        "build_time": meta.get("build_time"),
        "firmware_exists": fw_bin is not None,
        "firmware_path": fw_bin,
        "ota_url_hint": "设备固件 bms_config.h 的 BMS_OTA_VERSION_URL 默认已指向隧道 https://bms0605.dpdns.org(已部署到 ECS), 无需改动; 前端/强制升级 URL 填 https://bms0605.dpdns.org/api/ota/firmware.bin",
    })


@app.route("/api/ota/publish", methods=["POST"])
@login_required
def ota_publish():
    """网页上传固件并发布(2026-08-16 新增, admin-only):
    请求体: multipart/form-data, 字段 file=固件.bin + version=版本号(如 1.0.3)
    行为: 保存为 <dashboard>/firmware/bms.bin, 同步写入 firmware/version.json,
          设备下次检查(12h 自动 / 前端"检查更新")即发现新版本升级。
    注意: ECS docker 部署下 firmware 目录以 volume 挂载, 上传即落盘、容器重建不丢。"""
    if session.get("role") != "admin":
        return jsonify({"ok": False, "msg": "仅管理员可发布固件(当前角色非 admin)"}), 403
    f = request.files.get("file")
    if f is None or not f.filename:
        return jsonify({"ok": False, "msg": "请选择固件文件(bms.bin)"}), 400
    if not f.filename.lower().endswith(".bin"):
        return jsonify({"ok": False, "msg": "固件必须是 .bin 文件"}), 400
    version = (request.form.get("version") or "").strip()
    if not version:
        return jsonify({"ok": False, "msg": "请填写版本号(如 1.0.3)"}), 400
    data = f.read()
    if len(data) < 1024 * 100:
        return jsonify({"ok": False, "msg": "固件过小(<100KB), 拒绝发布"}), 400
    if len(data) > 20 * 1024 * 1024:
        return jsonify({"ok": False, "msg": "固件过大(>20MB), 拒绝发布"}), 400

    dash_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    fw_dir = os.path.join(dash_dir, "firmware")
    try:
        os.makedirs(fw_dir, exist_ok=True)
        fw_path = os.path.join(fw_dir, "bms.bin")
        with open(fw_path, "wb") as out:
            out.write(data)
        vp = os.path.join(fw_dir, "version.json")
        with open(vp, "w", encoding="utf-8") as vf:
            json.dump({"version": version, "url": "/api/ota/firmware.bin"},
                      vf, ensure_ascii=False)
    except Exception as e:
        return jsonify({"ok": False, "msg": "固件保存失败: %s" % e}), 500
    return jsonify({
        "ok": True,
        "version": version,
        "size": len(data),
        "url": "/api/ota/firmware.bin",
        "msg": "固件 v%s 已发布, 设备下次检查将自动升级" % version,
    })


@app.route("/api/devices")
@login_required
def api_devices():
    """设备列表(单设备,参数与 bms_config.h 主控严格一致).
    参数来源:
      - 型号/串数/容量/标称/满充/截止电压: bms_config.h 硬件总体宏
      - 固件版本: BMS_FIRMWARE_VERSION = 1.0.1
      - 区域: 华为云 cn-south-4 (设备部署位置: 实验室/深圳)
      - 最后上报: DB 最新一条 recv_time (若有)
      - 在线状态: iotda_client.device_online
    """
    # 从 DB 取最后上报时间与当前容量/SOC/循环次数(从最新记录展示)
    latest = db.query_latest() or {}
    last_seen = latest.get("recv_time") if isinstance(latest, dict) else None
    cur_soc   = latest.get("soc", 0) if isinstance(latest, dict) else 0
    cur_cycle = latest.get("cycle_count", 0) if isinstance(latest, dict) else 0

    online = bool(iotda_client.device_online) if iotda_client else False
    status = (iotda_client.device_last_status if iotda_client else "") or ("ONLINE" if online else "OFFLINE")

    # 从设备实际上报(影子)读取真实参数, 替代硬编码:
    #   影子 payload 含 capacity_ah(capacityAh)/fw_version(fwVersion), 设备参数下发后实时更新
    _shadow_now = None
    try:
        if iotda_client and isinstance(getattr(iotda_client, "_last_shadow", None), dict):
            _shadow_now = iotda_client._last_shadow
    except Exception:
        _shadow_now = None
    _cap_ah   = float((_shadow_now or {}).get("capacity_ah") or 0) or 2.5
    _fw_ver   = str((_shadow_now or {}).get("fw_version") or "") or "1.0.1"

    # ===== 2026-08-10 修复: 串数同步(与电芯类型同优先级链) =====
    #   原实现 db.get_series_num() 会被设备上报 cells 长度每帧覆盖
    #   (iotda_client 收到 16 串上报即 update_series_num(16)),
    #   导致"下发 32 串后设备管理仍显示 16". 现改为:
    #     1) 最近一条 DB 记录 param_snapshot(设备端上报过)
    #     2) 用户通过 Dashboard 保存的 param_history(下发即写入, 立即可见)
    #     3) db.get_series_num() 兜底
    _series_n = db.get_series_num()
    _snap_cs = latest.get("param_snapshot") if isinstance(latest, dict) else None
    if isinstance(_snap_cs, dict) and _snap_cs.get("cell_series_num") is not None:
        try:
            _sn = int(_snap_cs["cell_series_num"])
            if 1 <= _sn <= 32:
                _series_n = _sn
        except (TypeError, ValueError):
            pass
    _usr_cs = _load_user_params().get("cell_series_num")
    if _usr_cs is not None:
        try:
            _sn = int(_usr_cs)
            if 1 <= _sn <= 32:
                _series_n = _sn
        except (TypeError, ValueError):
            pass
    # ===== 2026-08-10 修复: 电芯类型读取来源 =====
    #   原实现从 iotda_client._last_shadow 读 battery_type, 但设备影子 payload
    #   只含 fw_version/capacity_ah 等字段, 从不包含 battery_type → 永远回退"三元锂",
    #   导致"下发电池参数后设备管理表格电芯类型不变".
    #   现改用与 /api/params GET 相同的优先级链:
    #     1) 最近一条 DB 记录 param_snapshot(设备端上报过)
    #     2) 用户通过 Dashboard 保存的 param_history(下发即写入, 立即可见)
    #     3) PARAM_META.default(与 bms_config.h 一致)
    _BT_NAMES = {0: "磷酸铁锂(LFP)", 1: "三元锂(NCM)", 2: "钛酸锂(LTO)", 3: "铅酸"}
    _bt_raw = None
    _snap = latest.get("param_snapshot") if isinstance(latest, dict) else None
    if isinstance(_snap, dict) and _snap.get("battery_type") is not None:
        _bt_raw = _snap.get("battery_type")
    if _bt_raw is None:
        _bt_raw = _load_user_params().get("battery_type")
    try:
        _cell_type = _BT_NAMES.get(int(_bt_raw), "三元锂(NCM)")
    except (TypeError, ValueError):
        _cell_type = "三元锂(NCM)"    # 无记录/非法时回退默认(与 bms_config.h 一致)
    HARDWARE_SPEC = {
        "series":       _series_n,   # BMS_CELL_SERIES_NUM (动态)
        "parallel":     1,       # BMS_CELL_PARALLEL_NUM (1P)
        "capacity_mah": int(_cap_ah * 1000),   # 从设备实际上报(回退 2500mAh)
        "capacity_ah":  _cap_ah,               # 从设备实际上报
        "nominal_mv":   3600 * _series_n,  # 标称总压 = 3.6V x 串数
        "full_mv":      4200 * _series_n,  # 满充总压 = 4.2V x 串数
        "cutoff_mv":    2800 * _series_n,  # 放电截止 = 2.8V x 串数
        "cell_type":    _cell_type,   # DB快照→用户保存→默认(与 /api/params 一致)
        "firmware":     _fw_ver,          # 从设备实际上报(回退 1.0.1)
        "esp32_chip":   "ESP32-WROOM-32",
        "model":        "BMS-%dS%dAh-V1" % (_series_n, int(_cap_ah)),  # 产品型号 = BMS-{S}S{Ah}-V{fw_ver}
        "sn":           "BMS001-20260701-001",  # 序列号 (出厂号)
        "manufacturer": "自研BMS (实验室)",
        "deploy_area":  "深圳·实验室",    # 设备部署位置(对应表格"区域"列)
        "region":       "cn-south-4",     # 华为云区域
        "bms_version":  _fw_ver,          # 从设备实际上报
        "protocol":     "MQTT+TLS 1.3",
    }

    dev = {
        # 表格显示字段
        "name":         "智能电池监控 · BMS-001",
        "device_id":    DEVICE_ID,
        "region":       HARDWARE_SPEC["deploy_area"],   # "区域"列 = 部署位置
        "cloud_region": HARDWARE_SPEC["region"],        # 华为云区域(补充)
        "status":       status,
        "online":       online,
        "series":       HARDWARE_SPEC["series"],
        "parallel":     HARDWARE_SPEC["parallel"],
        "capacity_ah":  HARDWARE_SPEC["capacity_ah"],
        "capacity_mah": HARDWARE_SPEC["capacity_mah"],
        "nominal_v":    round(HARDWARE_SPEC["nominal_mv"] / 1000.0, 2),
        "full_v":       round(HARDWARE_SPEC["full_mv"] / 1000.0, 2),
        "cutoff_v":     round(HARDWARE_SPEC["cutoff_mv"] / 1000.0, 2),
        "firmware":     HARDWARE_SPEC["firmware"],
        "bms_version":  HARDWARE_SPEC["bms_version"],
        "hardware":     HARDWARE_SPEC["model"],
        "sn":           HARDWARE_SPEC["sn"],
        "manufacturer": HARDWARE_SPEC["manufacturer"],
        "chip":         HARDWARE_SPEC["esp32_chip"],
        "protocol":     HARDWARE_SPEC["protocol"],
        "cell_type":    HARDWARE_SPEC["cell_type"],
        # 运行态
        "last_seen":    (last_seen * 1000) if last_seen else None,
        "cur_soc":      round(float(cur_soc), 1) if cur_soc else 0,
        "cycle_count":  int(cur_cycle or 0),
        # 本机 Dashboard 电脑 IP/MAC (非 ESP32 的,便于运维定位)
        "server_ip":    request.host.split(":")[0] if request.host else "",
    }

    # 尝试补 ESP32 的 IP/MAC(若 iotda_client 从影子里拿到了 info 上报)
    if isinstance(latest, dict) and latest.get("ip"):
        dev["device_ip"] = latest.get("ip")
    if isinstance(latest, dict) and latest.get("mac"):
        dev["device_mac"] = latest.get("mac")

    # ===== 2026-08-09 多设备管理: 由 BATTERY_GROUPS 生成多设备列表 =====
    #   主设备(BMS-001)使用真实在线状态与硬件参数;
    #   其他组设备从 iotda_client 多设备轮询结果取真实状态(device_shadows/device_online_map),
    #   未接入设备显示 OFFLINE 占位.
    multi_devices = [dev]
    for g in BATTERY_GROUPS:
        if g["group"] == 1 or g["short"] == "BMS-001":
            continue   # 主设备已在上方生成
        _s = int(g.get("series") or 6)
        _did = DEVICE_ID.replace("BMS001", "BMS%03d" % g["group"])
        # 从 iotda_client 多设备缓存取真实状态(轮询填充; 未注册/未接入为 False)
        _online = False
        _shadow = None
        if iotda_client:
            try:
                _online = bool(iotda_client.device_online_map.get(_did, False))
                _shadow = iotda_client.device_shadows.get(_did)
            except Exception:
                pass
        _soc = 0
        _last_seen = None
        if isinstance(_shadow, dict):
            try:
                _soc = round(float(_shadow.get("soc") or 0), 1)
                _let = int(_shadow.get("last_event_time_ms") or 0)
                if _let > 946684800000:
                    _last_seen = _let
            except Exception:
                pass
        # ===== 2026-08-10 修复: 从设备电芯类型 1对1 独立 =====
        #   原逻辑复用主设备 HARDWARE_SPEC["cell_type"], 主设备下发 LFP 后
        #   BMS-002/003 也跟着变(用户只改了一台却三台全变).
        #   现改为从**该设备自己的影子**读取 battery_type(无则回退默认),
        #   与主设备完全独立, 各显示各的.
        _slv_bt = None
        if isinstance(_shadow, dict):
            _slv_bt = _shadow.get("battery_type")
            if _slv_bt is None:
                _slv_bt = _shadow.get("batteryType")   # 兼容物模型驼峰字段名
        try:
            _slave_cell_type = _BT_NAMES.get(int(_slv_bt), "三元锂(NCM)")
        except (TypeError, ValueError):
            _slave_cell_type = "三元锂(NCM)"   # 影子无该字段/非法时回退默认
        multi_devices.append({
            "name":         "智能电池监控 · %s" % g["short"],
            "device_id":    _did,
            "region":       "深圳·实验室",
            "cloud_region": HARDWARE_SPEC["region"],
            "status":       "ONLINE" if _online else "OFFLINE",
            "online":       _online,
            "series":       _s,
            "parallel":     1,
            "capacity_ah":  round(float(g.get("capacity_mah", 2500)) / 1000.0, 2),
            "capacity_mah": int(g.get("capacity_mah", 2500)),
            "nominal_v":    round(3.6 * _s, 2),
            "full_v":       round(4.2 * _s, 2),
            "cutoff_v":     round(2.8 * _s, 2),
            "firmware":     HARDWARE_SPEC["firmware"],
            "bms_version":  HARDWARE_SPEC["bms_version"],
            "hardware":     "BMS-%dS%dmAh-V1" % (_s, int(g.get("capacity_mah", 2500))),
            "sn":           "BMS%03d-20260701-001" % g["group"],
            "manufacturer": HARDWARE_SPEC["manufacturer"],
            "chip":         HARDWARE_SPEC["esp32_chip"],
            "protocol":     HARDWARE_SPEC["protocol"],
            "cell_type":    _slave_cell_type,   # 2026-08-10: 从设备独立(不复用主设备)
            "last_seen":    _last_seen,
            "cur_soc":      _soc,
            "cycle_count":  0,
            "server_ip":    request.host.split(":")[0] if request.host else "",
        })

    return jsonify({
        "ok":     True,
        "count":  len(multi_devices),
        "spec":   HARDWARE_SPEC,
        "devices": multi_devices,
        # 电池组配置表: 前端 deviceSelect 下拉项 ↔ switch_battery 下发参数
        "groups": BATTERY_GROUPS,
    })


@app.route("/api/export.csv")
@login_required
def api_export_csv():
    """导出历史数据为 CSV
    2026-08-10 P2 修复: 支持前端 range 参数(1h/6h/24h/7d)与 limit,
    原实现只读 minutes(默认60), 导致选择 6h/24h/7d 导出的永远是最近 1 小时"""
    RANGE_TO_MINUTES = {"1h": 60, "6h": 360, "24h": 1440, "7d": 10080}
    range_str = request.args.get("range", "")
    start_s = (request.args.get("start") or "").strip()
    end_s = (request.args.get("end") or "").strip()
    start_ts = end_ts = None
    if start_s and end_s:
        try:
            start_ts = time.mktime(datetime.datetime.strptime(start_s, "%Y-%m-%d").timetuple())
            _end_date = datetime.datetime.strptime(end_s, "%Y-%m-%d").date()
            end_ts = time.mktime((_end_date + datetime.timedelta(days=1)).timetuple())
        except ValueError:
            return jsonify({"ok": False, "msg": "日期格式错误(应为 YYYY-MM-DD)"}), 400
    if range_str in RANGE_TO_MINUTES:
        minutes = RANGE_TO_MINUTES[range_str]
    else:
        minutes = request.args.get("minutes", 60, type=int)
    limit = min(request.args.get("limit", 10000, type=int), 50000)
    rows = db.query_history(minutes=minutes, limit=limit, start=start_ts, end=end_ts)

    def generate():
        # CSV 表头: 串数列随当前串数动态生成
        series_n = db.get_series_num()
        headers = ["timestamp","soc","soh","pack_v","current","temp_max","temp_min","fault","v_max","v_min"]
        headers += ["c%d" % i for i in range(1, series_n + 1)]
        headers.append("balance_mask")
        yield ",".join(headers) + "\n"
        for r in rows:
            vals = []
            for h in headers:
                if h.startswith("c"):
                    idx = int(h[1:]) - 1
                    cells_arr = r.get("cells") or []
                    if idx < len(cells_arr):
                        vals.append(str(cells_arr[idx]))
                    else:
                        vals.append(str(r.get(h, "") or ""))
                else:
                    vals.append(str(r.get(h, "") or ""))
            yield ",".join(vals) + "\n"
    return Response(generate(), mimetype="text/csv",
                    headers={"Content-Disposition": "attachment; filename=bms_history.csv"})


@app.route("/api/report")
@login_required
def api_report():
    """生成日报/周报/月报/季报/年报(范围统计)
    参数:
      type    daily(日) | weekly(周) | monthly(月) | quarterly(季) | yearly(年) | custom(自定义)
      date    对 daily: YYYY-MM-DD (默认今天)
              对 weekly: YYYY-MM-DD (会取所在周, 周一~周日)
              对 monthly: YYYY-MM   (默认本月)
              对 quarterly: YYYY-Qn (如 2026-Q3) 或 YYYY-MM (取所在季, 默认本季)
              对 yearly: YYYY        (默认今年)
              对 custom: 用 start/end
      start   custom 范围: YYYY-MM-DD (含)
      end     custom 范围: YYYY-MM-DD (含)
    返回字段(与前端 renderReport 期望一致):
      summary.count/ soc_avg/ soc_min/ soc_max/ soh_avg
             pack_v_avg/min/max/ current_avg/min/max
             temp_min/temp_max(°C)/ fault_count / charge_ah/discharge_ah
      buckets[]  = [{time, soc_avg, pack_v_avg, current_avg, temp_max}]
      alerts[]   = [{code, name, count}]  (code=bit 位, 0..N; name 与 FAULT_MAP 一致)
    """
    report_type = request.args.get("type", "daily").strip().lower()
    target_date = request.args.get("date", "").strip()
    start_s = request.args.get("start", "").strip()
    end_s = request.args.get("end", "").strip()

    # ---- P2-3 缓存命中: 相同参数 60s 内直接返回上次统计结果 ----
    _cache_key = "%s|%s|%s|%s" % (report_type, target_date, start_s, end_s)
    _cached = _REPORT_CACHE.get(_cache_key)
    if _cached and (time.time() - _cached["ts"]) < _REPORT_CACHE_TTL:
        return jsonify(_cached["data"])

    now = datetime.date.today()
    start = None
    end_exclusive = None  # [start, end_exclusive)

    try:
        if report_type == "custom":
            if not start_s or not end_s:
                return jsonify({"ok": False, "msg": "自定义范围需要 start 和 end (YYYY-MM-DD)"}), 400
            start = datetime.datetime.strptime(start_s, "%Y-%m-%d").date()
            end = datetime.datetime.strptime(end_s, "%Y-%m-%d").date()
            if end < start:
                return jsonify({"ok": False, "msg": "end 不能早于 start"}), 400
            end_exclusive = end + datetime.timedelta(days=1)
        elif report_type == "monthly":
            if not target_date:
                y, m = now.year, now.month
            else:
                if len(target_date) >= 7:
                    y, m = int(target_date[:4]), int(target_date[5:7])
                else:
                    return jsonify({"ok": False, "msg": "monthly 日期格式: YYYY-MM"}), 400
            # 当月 1 号 ~ 下月 1 号
            if m == 12:
                next_month = datetime.date(y + 1, 1, 1)
            else:
                next_month = datetime.date(y, m + 1, 1)
            start = datetime.date(y, m, 1)
            end_exclusive = next_month
        elif report_type == "quarterly":
            # YYYY-Qn 或 YYYY-MM(取所在季), 默认本季
            if not target_date:
                y, m = now.year, now.month
            else:
                _td = target_date.upper()
                if "-Q" in _td:
                    try:
                        y = int(_td.split("-Q")[0]); q = int(_td.split("-Q")[1])
                    except ValueError:
                        return jsonify({"ok": False, "msg": "quarterly 日期格式: YYYY-Qn"}), 400
                    if not (1 <= q <= 4):
                        return jsonify({"ok": False, "msg": "季度 Q 取值 1~4"}), 400
                    m = (q - 1) * 3 + 1
                elif len(target_date) >= 7:
                    y, m = int(target_date[:4]), int(target_date[5:7])
                else:
                    return jsonify({"ok": False, "msg": "quarterly 日期格式: YYYY-Qn 或 YYYY-MM"}), 400
            q_start = ((m - 1) // 3) * 3 + 1
            start = datetime.date(y, q_start, 1)
            next_q = q_start + 3
            if next_q > 12:
                end_exclusive = datetime.date(y + 1, next_q - 12, 1)
            else:
                end_exclusive = datetime.date(y, next_q, 1)
        elif report_type == "yearly":
            if not target_date:
                y = now.year
            else:
                try:
                    y = int(target_date[:4])
                except ValueError:
                    return jsonify({"ok": False, "msg": "yearly 日期格式: YYYY"}), 400
            start = datetime.date(y, 1, 1)
            end_exclusive = datetime.date(y + 1, 1, 1)
        elif report_type == "weekly":
            dt = datetime.datetime.strptime(target_date, "%Y-%m-%d").date() if target_date else now
            start = dt - datetime.timedelta(days=dt.weekday())  # 周一
            end_exclusive = start + datetime.timedelta(days=7)
        else:  # daily (默认)
            dt = datetime.datetime.strptime(target_date, "%Y-%m-%d").date() if target_date else now
            start = dt
            end_exclusive = start + datetime.timedelta(days=1)
    except ValueError as e:
        return jsonify({"ok": False, "msg": "日期格式错误: " + str(e)}), 400

    start_ts = time.mktime(start.timetuple())
    end_ts = time.mktime(end_exclusive.timetuple())

    # 查询 (以分钟为单位, 让 DB 拉取足够范围)
    minutes = max(1, int((end_ts - start_ts) // 60))
    # 2026-08-10 #27: 用 start/end 精确范围查询(避免 LIMIT 截断大区间),
    #   rep_limit 按区间天数放大, 上限 500000(年度整年约 52万点, 采样间隔越大越省)
    _total_days = (end_exclusive - start).days
    rep_limit = min(500000, max(50000, _total_days * 3000))
    rows = db.query_history(minutes=minutes, limit=rep_limit, start=start_ts, end=end_ts)
    rows = [r for r in rows if start_ts <= r.get("recv_time", 0) < end_ts]

    if not rows:
        # 空数据: 仍返回完整卡片, 显示"时段无数据" (而非前端 catch 弹出 error toast)
        return jsonify({
            "ok": True,
            "type": report_type,
            "date": target_date or start.isoformat(),
            "start_date": start.isoformat(),
            "end_date": (end_exclusive - datetime.timedelta(days=1)).isoformat(),
            "empty": True,
            "msg": "该时段无数据",
            "summary": None,
            "buckets": [],
            "alerts": [],
        })

    # ---------- 汇总统计 ----------
    soc_vals     = [float(r.get("soc") or 0) for r in rows]
    soh_vals     = [float(r.get("soh") or 0) for r in rows]
    pack_v_vals  = [float(r.get("pack_v") or 0) for r in rows]
    current_vals = [float(r.get("current") or 0) for r in rows]
    t_max_vals   = [float(r.get("temp_max") or 0) for r in rows]
    t_min_vals   = [float(r.get("temp_min") or 0) for r in rows]
    fault_count  = sum(1 for r in rows if int(r.get("fault") or 0) != 0)

    # ---------- 时段分桶 ----------
    # daily: 按小时(00..23)
    # weekly/monthly/quarterly/custom(3~100天): 按天 (YYYY-MM-DD)
    # yearly(>100天): 按月 (YYYY-MM) 避免 365 个点过密
    total_days = (end_exclusive - start).days
    by_hour = total_days <= 2  # 1~2 天的范围也按小时更细
    by_month = total_days > 100  # 年度(365天)按月; 季度(90天)仍按天
    buckets_ord = []
    buckets_idx = {}
    def _bucket_key(ts):
        d = datetime.datetime.fromtimestamp(ts)
        if by_hour:  return d.strftime("%Y-%m-%d %H:00")
        if by_month: return d.strftime("%Y-%m")
        return d.strftime("%Y-%m-%d")

    # 预生成连续桶 (避免缺小时/缺天/缺月导致图断裂)
    if by_hour:
        t = datetime.datetime(start.year, start.month, start.day, 0, 0, 0)
        end_t = datetime.datetime.fromtimestamp(time.mktime(end_exclusive.timetuple()))
        while t < end_t:
            k = t.strftime("%Y-%m-%d %H:00")
            buckets_idx[k] = len(buckets_ord)
            buckets_ord.append({"key": k, "soc":[], "pack_v":[], "current":[], "temp_max":[]})
            t += datetime.timedelta(hours=1)
    elif by_month:
        _y, _m = start.year, start.month
        while datetime.date(_y, _m, 1) < end_exclusive:
            k = "%04d-%02d" % (_y, _m)
            buckets_idx[k] = len(buckets_ord)
            buckets_ord.append({"key": k, "soc":[], "pack_v":[], "current":[], "temp_max":[]})
            _m += 1
            if _m > 12:
                _m = 1; _y += 1
    else:
        d = start
        while d < end_exclusive:
            k = d.isoformat()
            buckets_idx[k] = len(buckets_ord)
            buckets_ord.append({"key": k, "soc":[], "pack_v":[], "current":[], "temp_max":[]})
            d += datetime.timedelta(days=1)

    for r in rows:
        ts = float(r.get("recv_time") or 0)
        if ts <= 0:
            continue
        k = _bucket_key(ts)
        if k not in buckets_idx:
            continue
        b = buckets_ord[buckets_idx[k]]
        if r.get("soc") is not None: b["soc"].append(float(r["soc"]))
        if r.get("pack_v") is not None: b["pack_v"].append(float(r["pack_v"]))
        if r.get("current") is not None: b["current"].append(float(r["current"]))
        if r.get("temp_max") is not None: b["temp_max"].append(float(r["temp_max"]))

    # 压缩展示 label: daily 去掉 YYYY-MM-DD 前缀(仅 HH:00); weekly/monthly 保留日(MM-DD); yearly 保留月(YYYY-MM)
    buckets = []
    for b in buckets_ord:
        raw_k = b["key"]
        if by_hour:
            label = raw_k.split(" ")[1] if " " in raw_k else raw_k
        elif by_month:
            label = raw_k  # "YYYY-MM"
        else:
            _parts = raw_k.split("-")  # YYYY-MM-DD
            label = "%s-%s" % (_parts[1], _parts[2])
        def _avg(arr, nd=1):
            # 2026-08-09 修复: 空桶(未到时间/设备断网无数据)返回 None 而非 0,
            #   避免"未来时段/断网段"在报表图上显示为 0 值实线(误导);
            #   前端对 None 显示空白/跳过该点.
            return round(sum(arr)/len(arr), nd) if arr else None
        def _ext(arr, nd=2, fn=None):
            return round(fn(arr), nd) if arr else None
        buckets.append({
            "time": label,
            "key":  raw_k,
            "soc_avg":     _avg(b["soc"], 1),
            "soc_min":     _ext(b["soc"], 1, min),
            "soc_max":     _ext(b["soc"], 1, max),
            "pack_v_avg":  _avg(b["pack_v"], 0),
            "pack_v_min":  _ext(b["pack_v"], 0, min),
            "pack_v_max":  _ext(b["pack_v"], 0, max),
            "current_avg": _avg(b["current"], 0),
            "current_min": _ext(b["current"], 0, min),
            "current_max": _ext(b["current"], 0, max),
            "temp_max":    (max(b["temp_max"]) / 10.0) if b["temp_max"] else None,
            "count": len(b["soc"]),
        })

    # ---------- 故障统计(使用与 bms_types.h / 前端 FAULT_MAP 严格对齐的 bit->描述) ----------
    # 对齐依据: app.js FAULT_MAP = { 0x00001(bit0):"单体过压预警", ... , 0x100000(bit20):"电流采样失效" }
    FAULT_NAMES = {
        # ---- 电压类 bit0~3 ----
        0:  "单体过压预警", 1: "单体过压保护", 2: "单体欠压预警", 3: "单体欠压保护",
        # ---- 电流类 bit4~7,16 ----
        4:  "充电过流预警", 5: "充电过流保护", 6:  "放电过流预警", 7:  "放电过流保护",
        16: "持续过载预警",
        # ---- 温度类 bit8~11,15 ----
        8:  "过温预警",     9: "过温保护",
        10: "热失控报警",   11: "短路",
        15: "温升速率预警",
        # ---- 状态类 bit12~14,17~18 ----
        12: "低温保护",     13: "单体压差过大", 14: "低温预警",
        17: "电量过低预警", 18: "健康度衰减预警",
        # ---- 系统类 bit19,20 ----
        19: "通信/采样异常",
        20: "电流采样失效",
    }
    fault_stats = {}
    for r in rows:
        fault = int(r.get("fault") or 0)
        if fault == 0:
            continue
        for bit in range(32):
            if fault & (1 << bit):
                code = 1 << bit
                name = FAULT_NAMES.get(bit, ("未知(bit %d, 0x%X)" % (bit, code)))
                key_tup = (bit, name)
                fault_stats[key_tup] = fault_stats.get(key_tup, 0) + 1
    alerts = []
    for (bit, name), cnt in sorted(fault_stats.items(), key=lambda x: -x[1]):
        alerts.append({"code": bit, "hex": ("0x%X" % (1 << bit)), "name": name, "count": cnt})

    # ---------- 充放电量(Ah) ----------
    charge_ah = 0.0
    discharge_ah = 0.0
    prev = None
    for r in sorted(rows, key=lambda x: float(x.get("recv_time") or 0)):
        if prev:
            dt_hours = max(0.0, (float(r.get("recv_time") or 0) - float(prev.get("recv_time") or 0)) / 3600.0)
            cur = float(r.get("current") or 0)
            if cur > 0:      charge_ah    += cur * dt_hours / 1000.0
            elif cur < 0:    discharge_ah += abs(cur) * dt_hours / 1000.0
        prev = r

    # ---------- 响应 ----------
    summary = {
        "count":         len(rows),
        "soc_avg":       round(sum(soc_vals)/len(soc_vals), 2) if soc_vals else 0,
        "soc_min":       round(min(soc_vals), 2) if soc_vals else 0,
        "soc_max":       round(max(soc_vals), 2) if soc_vals else 0,
        "soh_avg":       round(sum(soh_vals)/len(soh_vals), 2) if soh_vals else 0,
        "pack_v_avg":    round(sum(pack_v_vals)/len(pack_v_vals), 0) if pack_v_vals else 0,
        "pack_v_min":    round(min(pack_v_vals), 0) if pack_v_vals else 0,
        "pack_v_max":    round(max(pack_v_vals), 0) if pack_v_vals else 0,
        "current_avg":   round(sum(current_vals)/len(current_vals), 1) if current_vals else 0,
        "current_min":   round(min(current_vals), 1) if current_vals else 0,
        "current_max":   round(max(current_vals), 1) if current_vals else 0,
        "charge_ah":     round(charge_ah, 3),
        "discharge_ah":  round(discharge_ah, 3),
        "temp_min":      (min(t_min_vals) / 10.0) if t_min_vals else 0,
        "temp_max":      (max(t_max_vals) / 10.0) if t_max_vals else 0,
        "fault_count":   fault_count,
    }
    _report_resp = {
        "ok": True,
        "empty": False,
        "type": report_type,
        "date": target_date or start.isoformat(),
        "start_date": start.isoformat(),
        "end_date": (end_exclusive - datetime.timedelta(days=1)).isoformat(),
        "summary": summary,
        "buckets": buckets,
        "alerts": alerts,
    }
    # P2-3 写缓存: 相同参数 60s 内直接命中, 避免重复全量统计阻塞
    # 2026-08-19 修复(B11): 写入时淘汰过期条目——原实现只覆盖同 key, 过期 key
    #   永不删除, 异常前端用不同日期参数连续请求会让缓存字典无限增长(OOM).
    _now_c = time.time()
    if len(_REPORT_CACHE) > 64:
        for _k in [k for k, v in _REPORT_CACHE.items()
                   if (_now_c - v["ts"]) > _REPORT_CACHE_TTL]:
            _REPORT_CACHE.pop(_k, None)
    _REPORT_CACHE[_cache_key] = {"ts": _now_c, "data": _report_resp}
    return jsonify(_report_resp)


@app.route("/api/audit_log")
@login_required
def api_audit_log():
    """操作审计日志查询"""
    limit = request.args.get("limit", 100, type=int)
    cmd_filter = request.args.get("cmd", None)
    rows = db.query_audit_log(limit=limit, cmd_filter=cmd_filter)
    return jsonify({"ok": True, "data": rows})


@app.route("/api/operation_log", methods=["POST"])
@login_required
def api_operation_log_post():
    """批量写入前端运行日志(持久化到 DB, 防篡改/防刷新丢失)。
    Body: {entries: [{recv_time, level, msg}]}
    后端自动补全 user / source(IP)。"""
    data = request.get_json(silent=True) or {}
    entries = data.get("entries") or []
    if not isinstance(entries, list):
        return jsonify({"ok": False, "msg": "entries 必须是数组"}), 400
    if len(entries) > 1000:
        return jsonify({"ok": False, "msg": "单次最多写入 1000 条"}), 400
    user = session.get("username", "")
    ip = request.remote_addr or request.headers.get("X-Forwarded-For", "").split(",")[0].strip() or "unknown"
    for e in entries:
        if not isinstance(e, dict):
            continue
        e.setdefault("user", user)
        e.setdefault("source", ip)
    n = db.insert_operation_log(entries)
    return jsonify({"ok": True, "count": n})


@app.route("/api/operation_log")
@login_required
def api_operation_log_get():
    """查询前端运行日志(用于页面加载回填)。
    Query: start/end(epoch秒), level, limit(默认500)"""
    start = request.args.get("start", type=int)
    end = request.args.get("end", type=int)
    level = request.args.get("level") or None
    limit = request.args.get("limit", 500, type=int)
    if end is None:
        end = int(time.time())
    if start is None:
        start = end - 7 * 86400
    rows = db.query_operation_log_range(start=start, end=end, level=level, limit=limit)
    return jsonify({"ok": True, "data": rows})


@app.route("/api/operation_log/export")
@login_required
def api_operation_log_export():
    """按时间区间导出前端运行日志 CSV(审计级, 服务端 DB 直接出)。
    Query: start/end(epoch秒, 缺省 end=now/start=7天前), level(可选)"""
    import csv, io
    try:
        start = request.args.get("start", type=int)
        end = request.args.get("end", type=int)
        level = request.args.get("level") or None
    except (TypeError, ValueError):
        return jsonify({"ok": False, "msg": "start/end 需为 epoch 秒"}), 400
    if end is None:
        end = int(time.time())
    if start is None:
        start = end - 7 * 86400
    rows = db.query_operation_log_range(start=start, end=end, level=level, limit=200000)

    def fmt(ts):
        try:
            return (datetime.datetime.fromtimestamp(float(ts), datetime.timezone.utc)
                    .astimezone(datetime.timezone(datetime.timedelta(hours=8)))
                    .strftime("%Y-%m-%d %H:%M:%S"))
        except Exception:
            return str(ts)

    buf = io.StringIO(); w = csv.writer(buf)
    w.writerow(["时间(UTC+8)", "recv_time_epoch", "等级", "用户", "来源", "消息"])
    for r in rows:
        w.writerow([
            fmt(r.get("recv_time")), r.get("recv_time"), r.get("level"),
            r.get("user"), r.get("source"), r.get("msg"),
        ])
    data = buf.getvalue().encode("utf-8-sig")
    resp = make_response(data)
    resp.headers["Content-Type"] = "text/csv; charset=utf-8"
    fname = "BMS_运行日志_%s_%s.csv" % (
        datetime.datetime.fromtimestamp(start, datetime.timezone.utc).strftime("%Y%m%d"),
        datetime.datetime.fromtimestamp(end, datetime.timezone.utc).strftime("%Y%m%d"),
    )
    resp.headers["Content-Disposition"] = "attachment; filename=%s" % fname
    return resp


@app.route("/api/rules", methods=["GET", "POST"])
@login_required
def api_rules():
    """联动规则配置(读/写); 写操作仅管理员"""
    if request.method == "POST":
        if session.get("role") != "admin":
            return jsonify({"ok": False, "msg": "仅管理员可修改联动规则"}), 403
        data = request.get_json(silent=True) or {}
        res = RuleEngine_.set_config(data)
        return jsonify(res), (200 if res.get("ok") else 400)
    return jsonify({"ok": True, "config": RuleEngine_.to_dict()})


@app.route("/api/push_config", methods=["GET", "POST"])
@login_required
def api_push_config():
    """告警推送配置(读/写); 写操作仅管理员"""
    if request.method == "POST":
        if session.get("role") != "admin":
            return jsonify({"ok": False, "msg": "仅管理员可修改告警推送"}), 403
        data = request.get_json(silent=True) or {}
        res = AlertPusher.set_config(data)
        return jsonify(res), (200 if res.get("ok") else 400)
    return jsonify({"ok": True, "config": AlertPusher.to_dict()})


@app.route("/api/me")
@login_required
def api_me():
    """返回当前登录用户角色(前端据此门控管理员可编辑配置)
    2026-08-22 修复(#2): 登录时写入的是 session["username"], 原代码误取
    session.get("user") → 永远返回默认 "admin", 导致操作员登录后右上角仍显示 admin"""
    return jsonify({
        "ok": True,
        "user": session.get("username", session.get("user", "admin")),
        "role": session.get("role", "guest"),
        "is_admin": session.get("role") == "admin",
    })


# ============================================================
# 2026-08-21 用户管理 API(仅管理员) —— 前端用户管理界面配套
#   此前用户只能命令行 manage_users.py 管理, 网页端无管理界面.
# ============================================================
@app.route("/api/users", methods=["GET"])
@role_required("admin")
def api_users_list():
    """用户列表(不含密码哈希)"""
    try:
        rows = db.list_users()
    except Exception as _e:
        return jsonify({"ok": False, "msg": "查询用户失败: %s" % _e}), 500
    return jsonify({"ok": True, "users": rows})


@app.route("/api/users", methods=["POST"])
@role_required("admin")
def api_users_create():
    """新增用户: {username, password, role, display_name?}"""
    d = request.get_json(silent=True) or {}
    username = (d.get("username") or "").strip()
    password = d.get("password") or ""
    role = d.get("role") or "viewer"
    display_name = (d.get("display_name") or "").strip()
    if not username or len(username) < 3:
        return jsonify({"ok": False, "msg": "用户名至少 3 个字符"}), 400
    if len(password) < 8:
        return jsonify({"ok": False, "msg": "密码至少 8 位"}), 400
    if role not in ("admin", "operator", "viewer"):
        return jsonify({"ok": False, "msg": "角色必须是 admin/operator/viewer"}), 400
    if db.find_user(username):
        return jsonify({"ok": False, "msg": "用户名已存在"}), 409
    try:
        ok = db.create_user(username, password, role=role, display_name=display_name)
    except Exception as _e:
        return jsonify({"ok": False, "msg": "创建失败: %s" % _e}), 500
    if not ok:
        return jsonify({"ok": False, "msg": "创建失败(用户可能已存在)"}), 409
    db.insert_audit_log("create_user", {"user": username, "role": role},
                        request.remote_addr or "unknown", "ok")
    return jsonify({"ok": True, "msg": "用户 %s 已创建" % username})


@app.route("/api/users/<username>", methods=["PUT"])
@role_required("admin")
def api_users_update(username):
    """更新用户: {role?, password?, display_name?}(至少一项)"""
    d = request.get_json(silent=True) or {}
    if not db.find_user(username):
        return jsonify({"ok": False, "msg": "用户不存在"}), 404
    changes = []
    if "role" in d:
        role = d["role"]
        if role not in ("admin", "operator", "viewer"):
            return jsonify({"ok": False, "msg": "角色必须是 admin/operator/viewer"}), 400
        # 保护: 不能把最后一个管理员降级
        if role != "admin" and username == session.get("username"):
            return jsonify({"ok": False, "msg": "不能降级当前登录的最后一个管理员"}), 403
        try:
            conn = db.get_conn()
            conn.execute("UPDATE users SET role=? WHERE username=?", (role, username))
            conn.commit()
        except Exception as _e:
            return jsonify({"ok": False, "msg": "更新角色失败: %s" % _e}), 500
        changes.append("角色=%s" % role)
    if "password" in d and d["password"]:
        if len(d["password"]) < 8:
            return jsonify({"ok": False, "msg": "密码至少 8 位"}), 400
        try:
            ok = db.reset_password(username, d["password"])
        except Exception as _e:
            return jsonify({"ok": False, "msg": "重置密码失败: %s" % _e}), 500
        if not ok:
            return jsonify({"ok": False, "msg": "重置密码失败"}), 400
        changes.append("重置密码")
    if "display_name" in d:
        try:
            conn = db.get_conn()
            conn.execute("UPDATE users SET display_name=? WHERE username=?", (d["display_name"], username))
            conn.commit()
        except Exception as _e:
            return jsonify({"ok": False, "msg": "更新显示名失败: %s" % _e}), 500
        changes.append("显示名")
    if not changes:
        return jsonify({"ok": False, "msg": "无可更新字段(role/password/display_name)"}), 400
    db.insert_audit_log("update_user", {"user": username, "changes": ",".join(changes)},
                        request.remote_addr or "unknown", "ok")
    return jsonify({"ok": True, "msg": "用户 %s 已更新: %s" % (username, ",".join(changes))})


@app.route("/api/users/<username>", methods=["DELETE"])
@role_required("admin")
def api_users_delete(username):
    """删除用户(不能删除自己/最后一个管理员)"""
    if username == session.get("username"):
        return jsonify({"ok": False, "msg": "不能删除当前登录账号"}), 403
    try:
        ok, msg = db.delete_user(username)
    except Exception as _e:
        return jsonify({"ok": False, "msg": "删除失败: %s" % _e}), 500
    if not ok:
        return jsonify({"ok": False, "msg": msg}), 400
    db.insert_audit_log("delete_user", {"user": username},
                        request.remote_addr or "unknown", "ok")
    return jsonify({"ok": True, "msg": "用户 %s 已删除" % username})


# ====================================================================
# WebSocket 事件
# ====================================================================
# ===== 2026-08-09 工业加固: 单设备最大 WebSocket 连接数限制 =====
#   防止异常客户端(重连风暴/多开页面)挤占连接, 超限拒绝新连接并记录日志
_WS_MAX_CONN = 5                      # 单设备允许的最大并发连接数
_ws_conn_count = 0
_WS_CONN_LOCK = threading.Lock()

@socketio.on("connect")
def on_connect():
    """客户端建立 WebSocket 连接(限制最大并发连接数)"""
    global _ws_conn_count
    with _WS_CONN_LOCK:
        if _ws_conn_count >= _WS_MAX_CONN:
            print("[WS] 拒绝连接: 并发连接数已达上限 %d" % _WS_MAX_CONN)
            return False              # 拒绝本次连接(SocketIO 返回 False 即断开)
        _ws_conn_count += 1
    print("[WS] 客户端已连接 (当前 %d/%d)" % (_ws_conn_count, _WS_MAX_CONN))

@socketio.on("disconnect")
def on_disconnect():
    """客户端断开 WebSocket 连接"""
    global _ws_conn_count
    with _WS_CONN_LOCK:
        if _ws_conn_count > 0:
            _ws_conn_count -= 1
    print("[WS] 客户端已断开 (当前 %d/%d)" % (_ws_conn_count, _WS_MAX_CONN))

@socketio.on("manual_refresh")
def on_manual_refresh():
    """前端请求手动刷新设备数据"""
    if iotda_client:
        iotda_client._force_refresh = True
        print("[WS] 收到手动刷新请求")


# ====================================================================
# 定时任务: 数据库清理(每天清理超过 90 天的旧数据)
# ====================================================================
def cleanup_thread():
    """后台清理线程: 每 6 小时清理一次旧数据"""
    while True:
        time.sleep(6 * 3600)
        try:
            db.cleanup_old(days=90)
            print("[DB] 旧数据清理完成")
        except Exception as e:
            print("[DB] 清理失败:", e)


def backup_thread():
    """2026-08-09: 数据库每日自动备份线程
    每日 03:30 执行一次压缩备份(backup_db.backup_db_now),
    保留最近 N 份(BMS_DB_BACKUP_KEEP, 默认 14), 可被 BMS_DB_BACKUP_ENABLED=0 关闭"""
    try:
        from backup_db import _auto_backup_once
    except Exception as e:
        print("[BACKUP] 备份模块加载失败:", e)
        return
    while True:
        try:
            # 计算到下一个 03:30 的等待秒数
            now = datetime.datetime.now()
            target = now.replace(hour=3, minute=30, second=0, microsecond=0)
            if now >= target:
                target += datetime.timedelta(days=1)
            wait_s = (target - now).total_seconds()
            print("[BACKUP] 每日备份线程已启动, 下次备份: %s" % target.strftime("%Y-%m-%d %H:%M"))
            time.sleep(wait_s)
            _auto_backup_once()
        except Exception as e:
            print("[BACKUP] 备份线程异常:", e)
            time.sleep(3600)


# ====================================================================
# 启动预检: 端口占用检测 + 单实例保护(2026-08-10, 防 WinError 10048)
# ====================================================================
DASH_LISTEN_HOST = "0.0.0.0"
DASH_LISTEN_PORT = 5000
_SINGLE_INSTANCE_LOCK = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                     "logs", ".bms_dashboard.lock")


def _pid_alive(pid):
    """检查进程是否存活(跨平台: os.kill(pid, 0) 仅探测存在性, 不发信号)"""
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False
    except Exception:
        return False


def _release_single_instance():
    """进程退出时删除 PID 锁文件"""
    try:
        if os.path.exists(_SINGLE_INSTANCE_LOCK):
            os.remove(_SINGLE_INSTANCE_LOCK)
    except Exception:
        pass


def _preflight_startup():
    """启动前预检(在任何初始化/副作用之前调用):
    1) 端口占用探测: 提前 bind 检查, 占用时输出可读错误并退出,
       避免 socketio.run 抛出 WinError 10048 的晦涩堆栈;
    2) 单实例 PID 锁: 锁文件存在且旧进程仍存活 → 拒绝启动,
       防止 watchdog/手动重复拉起产生多个轮询实例互相干扰(重复轮询/抢端口/刷告警).
    任一检查失败即 sys.exit(1), 不进入数据库初始化等流程."""
    import socket as _sock
    import sys as _sys
    import errno as _errno
    import atexit as _atexit

    # ---- 1) 端口占用探测 ----
    probe = _sock.socket(_sock.AF_INET, _sock.SOCK_STREAM)
    # 2026-08-18 修复: 探测 socket 必须设 SO_REUSEADDR —— 否则前一个实例刚退出、
    #   端口处于 TIME_WAIT(60s) 时, bind 会误报 EADDRINUSE, 导致 systemd 反复重启失败
    #   (日志见"端口 5000 已被占用"但 ss 实际无监听)。真实监听(Flask/eventlet)本身
    #   也复用该选项, 这里设 REUSEADDR 只影响探测, 不影响占用的真实性。
    try:
        probe.setsockopt(_sock.SOL_SOCKET, _sock.SO_REUSEADDR, 1)
    except Exception:
        pass
    try:
        probe.bind((DASH_LISTEN_HOST, DASH_LISTEN_PORT))
    except OSError as e:
        if e.errno in (_errno.EADDRINUSE, _errno.EACCES):
            print("[启动] 端口 %d 已被占用(%s)。" % (DASH_LISTEN_PORT, e))
            print("[启动] 可能已有 BMS Dashboard 实例在运行:")
            print("        1) 任务管理器结束旧的 python.exe/app.py 进程")
            print("        2) 或等待 watchdog 重启(仅保留单实例)")
            _sys.exit(1)
        raise
    finally:
        probe.close()

    # ---- 2) 单实例 PID 锁 ----
    if os.path.exists(_SINGLE_INSTANCE_LOCK):
        old_pid = 0
        try:
            with open(_SINGLE_INSTANCE_LOCK, "r", encoding="utf-8") as f:
                old_pid = int((f.read() or "0").strip() or 0)
        except Exception:
            old_pid = 0
        if old_pid > 0 and old_pid != os.getpid() and _pid_alive(old_pid):
            print("[启动] 检测到旧实例仍在运行(PID=%d), 拒绝重复启动。" % old_pid)
            print("[启动] 如需强制重启, 请先结束旧进程或删除 %s" % _SINGLE_INSTANCE_LOCK)
            _sys.exit(1)
    try:
        with open(_SINGLE_INSTANCE_LOCK, "w", encoding="utf-8") as f:
            f.write(str(os.getpid()))
    except Exception as e:
        print("[启动] 写 PID 锁文件失败(%s), 继续启动..." % e)
    _atexit.register(_release_single_instance)


# 2026-08-18 离线推送增强: EMQX 直连超时监测线程
#   EMQX 直连消费腿在设备断电瞬间即不再收到帧(keepalive 6s 断开),
#   本线程每 15s 检查一次: 距最近一帧 > EMQX_OFFLINE_ALERT_MS 则判定设备离线,
#   并触发企业微信离线推送 —— 远快于华为云影子(60~120s)的离线通知。
_EMQX_OFFLINE_ALERT_MS = 70 * 1000   # 略大于前端 60s 判离线阈值, 留余量避免瞬时抖动误报
_emqx_offline_notified = False        # 避免重复推送(设备恢复后再离线才再推)


def _emqx_offline_monitor():
    """监测 EMQX 直连新鲜度, 超过阈值时推送企业微信离线; 恢复后复位。"""
    global _emqx_offline_notified
    while True:
        time.sleep(15)
        try:
            if local_mqtt is None:
                continue
            _last = float(getattr(local_mqtt, "last_msg_ts", 0.0) or 0.0)
            if _last > 0:
                age = (time.time() - _last) * 1000
                if age > _EMQX_OFFLINE_ALERT_MS:
                    if not _emqx_offline_notified:
                        _emqx_offline_notified = True
                        try:
                            AlertPusher.notify_online_change(
                                False, {"last_shadow": None}, force=True)
                            print("[EMQX监控] 设备离线(EMQX 直连 %ds 无新帧), 已推送企微" % int(age / 1000))
                        except Exception as e:
                            print("[EMQX监控] 离线推送失败: %s" % e)
                else:
                    _emqx_offline_notified = False   # 帧恢复 → 复位, 允许下次再告警
        except Exception as _e:
            print("[EMQX监控] 异常(已忽略): %s" % _e)


# ====================================================================
# 主入口
# ====================================================================
def main():
    global iotda_client

    # 2026-08-10: 端口预检 + 单实例保护(失败即退出, 防止多实例/端口冲突)
    _preflight_startup()

    # 初始化数据库
    db.init_db()
    print("[DB] 数据库已初始化")

    # 用户参数历史表(存储用户修改过的阈值
    _init_param_history_table()
    print("[DB] 参数历史表已就绪")

    # 2026-08-08: 多用户账号体系 - 确保默认管理员存在(admin/admin123, 生产请改密)
    try:
        if db.ensure_default_admin("admin", "admin123"):
            print("[安全] 已创建默认管理员账号 admin/admin123 (生产环境请立即修改密码!)")
    except Exception as e:
        print("[安全] 默认管理员创建失败:", e)

    # 创建 IoTDA 客户端并启动后台轮询
    iotda_client = IoTDAClient(
        ak=AK, sk=SK,
        project_id=PROJECT_ID,
        region=REGION,
        device_id=DEVICE_ID,
        socketio=socketio,
        poll_interval=60,
    )
    # ===== 2026-08-09 多设备管理: 注册同一产品下的从设备 =====
    #   华为云已注册 BMS001(主)/BMS002/BMS003 三台设备(同一产品),
    #   从设备加入轻量轮询(影子+在线状态), 供 /api/devices 与 /api/status?device= 查询
    try:
        for _g in BATTERY_GROUPS:
            if _g.get("group", 1) <= 1:
                continue   # group1 = 主设备(已由 DEVICE_ID 覆盖)
            _did = DEVICE_ID.replace("BMS001", "BMS%03d" % _g["group"])
            iotda_client.register_device(_did)
            print("[IoTDA] 已注册从设备: %s" % _did)
    except Exception as e:
        print("[IoTDA] 从设备注册失败(不影响主设备):", e)
    iotda_client.start()
    print("[IoTDA] 客户端已启动, device_id=%s" % DEVICE_ID)

    # ===== 2026-08-16: 双通道架构 — 本地消费腿(自建 EMQX/Mosquitto, 与 IoTDA 双路并存, 主备反转) =====
    #   设备双发: 同一份 JSON 同时发华为云(影子) + 自建 broker(bms/<id>/telemetry)。
    #   本腿订阅本地 broker, 事件驱动入库+推送(实时主); IoTDA 腿保留影子/规则/告警/兜底。
    #   配置: dashboard.env 或环境变量 BMS_BRIDGE_MQTT_HOST/PORT/USER/PASS(与 docker-compose bms-bridge 一致)
    try:
        from mqtt_client import MqttClient as _MqttClient
        _m_host = os.environ.get("BMS_BRIDGE_MQTT_HOST", "127.0.0.1")
        _m_port = int(os.environ.get("BMS_BRIDGE_MQTT_PORT", "1883"))
        _m_user = os.environ.get("BMS_BRIDGE_MQTT_USER", "")
        _m_pass = os.environ.get("BMS_BRIDGE_MQTT_PASS", "")
        # 2026-08-18 离线判定增强: 绑定到模块级全局 local_mqtt, /api/status 据此
        #   暴露 EMQX 直连新鲜度(设备断电即时离线判定, 不依赖华为云影子延迟)
        global local_mqtt
        local_mqtt = _MqttClient(
            broker=_m_host, port=_m_port,
            # 2026-08-18 实时性整改: 补收 /fast 高速帧(100ms 精简帧, 仅喂曲线不落库)
            # 与 /data 桥接主题(华为云影子桥接腿), 一次收齐所有数据通道
            topics=["bms/+/telemetry", "bms/+/event", "bms/+/status", "bms/+/cmd/ack",
                    "bms/+/fast", "bms/+/data"],
            # 2026-08-21 EMQX 主通道命令下行: 发布到 bms/<device_id>/cmd,
            #   与固件 EMQX 腿订阅主题一致(bms/bms01/cmd, 见 sys_mqtt.c mqtt2_event_handler).
            #   网页命令经 EMQX 直达设备(实时, 不依赖华为云 /messages 兜底).
            cmd_topic=("bms/" + _os.environ.get("BMS_MQTT2_CLIENT_ID", "bms01") + "/cmd"),
            socketio=socketio,
            client_id="bms_dashboard_local",
            username=_m_user, password=_m_pass,
        )
        local_mqtt.start()
        print("[MQTT-Local] 本地消费腿已启动 %s:%d (双通道并存, 实时主)" % (_m_host, _m_port))
    except Exception as _e:
        print("[MQTT-Local] 本地 MQTT 消费腿启动失败(不影响 IoTDA 主链路): %s" % _e)
        local_mqtt = None

    # 启动数据库清理线程
    t = threading.Thread(target=cleanup_thread, daemon=True)
    t.start()

    # 2026-08-09: 启动每日数据库备份线程(每日 03:30 压缩备份, 保留最近 N 份)
    try:
        tb = threading.Thread(target=backup_thread, daemon=True)
        tb.start()
    except Exception as e:
        print("[BACKUP] 备份线程启动失败:", e)

    # 2026-08-18 离线推送增强: 启动 EMQX 直连超时监测线程
    #   (设备断电后 ~70s 触发企业微信离线推送, 远快于华为云影子 60~120s)
    try:
        threading.Thread(target=_emqx_offline_monitor, daemon=True).start()
        print("[EMQX监控] 直连超时监测线程已启动(阈值 %ds)" % (_EMQX_OFFLINE_ALERT_MS // 1000))
    except Exception as _e:
        print("[EMQX监控] 监测线程启动失败(不影响主服务): %s" % _e)

    # 启动 Flask + SocketIO 服务
    print("=" * 60)
    print("  BMS Dashboard 服务已启动")
    print("  访问地址: http://localhost:5000/")
    # 2026-08-19 修复(B12): 不再明文打印登录密码(日志可被运维/日志平台读取);
    #   密码通过 dashboard.env 的 BMS_DASH_PASSWORD 管理, 不落地日志.
    print("  (登录密码: 见 BMS_DASH_PASSWORD 环境变量, 不输出到日志)")
    print("=" * 60)
    socketio.run(app, host="0.0.0.0", port=5000, debug=False, use_reloader=False)


if __name__ == "__main__":
    main()
