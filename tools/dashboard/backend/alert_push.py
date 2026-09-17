# -*- coding: utf-8 -*-
"""
告警推送模块(多通道, 2026-08-09)
============================================
支持三个通道(配置哪个用哪个, 可并存):
  1. 企业微信 webhook (BMS_ALERT_WEBHOOK)
     - 群"消息推送"(原群机器人), markdown 卡片, 限 20 条/分钟
  2. Server酱 (BMS_ALERT_SERVERCHAN_KEY)  ★ 推个人微信
     - sct.ftqq.com 扫码登录拿 SendKey, 通过"方糖"公众号收到, 免费限 5 条/天
  3. PushPlus (BMS_ALERT_PUSHPLUS_TOKEN)   ★ 推个人微信
     - pushplus.plus 扫码关注公众号拿 token, 免费限 200 条/天
功能:
  - 设备故障告警(fault 0→非0 跳变时推送)
  - 设备离线/上线通知(可选)
  - 推送频率限流(2s 节流) + 同故障 10 分钟去重
配置(dashboard.env, 全部留空则禁用):
  BMS_ALERT_WEBHOOK=https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=xxx
  BMS_ALERT_SERVERCHAN_KEY=sctpxxx
  BMS_ALERT_PUSHPLUS_TOKEN=xxx
  BMS_ALERT_ONLINE_NOTIFY=1   # 1=离线/上线也推送 0=只推故障(默认1)
用法:
  from alert_push import AlertPusher
  AlertPusher.notify_fault(fault_code, status_dict)
  AlertPusher.notify_text("自定义消息")
"""
import os
import time
import threading
import urllib.request
import json
import urllib.parse

try:
    import requests
    _HAS_REQUESTS = True
except Exception:
    _HAS_REQUESTS = False


def _load_env_file():
    """加载本目录 dashboard.env(与 app.py 相同逻辑, 环境变量优先)
    2026-08-09 修复: alert_push.py 单独自测/被 iotda_client import 时,
    dashboard.env 可能尚未加载, 导致单例创建时读不到 webhook 密钥."""
    try:
        _env_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.env")
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
    except Exception as _e:
        # 2026-09-07: 不再静默吞异常 — env 加载失败(缺文件/编码错)会让 webhook
        # 密钥丢失, 排障无迹可寻; 打印一行告警(不改变降级行为)
        print("[ALERT] dashboard.env 加载失败: %s" % _e)


_load_env_file()


# ============================================================
# 故障码映射表(名称 + 危害等级, 与 components/bms_config/include/bms_types.h
# 及前端 static/app.js FAULT_MAP 完全一致, 2026-08-09 新增)
# 等级: warn=预警级(不切断,降额+提示,自动恢复) / error=保护级(切断继电器)
#       fatal=严重级(全断继电器,急促报警,需人工复位)
# ============================================================
FAULT_DEFS = {
    0x00001:   {"name": "单体过压预警",         "level": "warn"},
    0x00002:   {"name": "单体过压保护",         "level": "error"},
    0x00004:   {"name": "单体欠压预警",         "level": "warn"},
    0x00008:   {"name": "单体欠压保护",         "level": "error"},
    0x02000:   {"name": "单体压差过大",         "level": "warn"},
    0x00010:   {"name": "充电过流预警",         "level": "warn"},
    0x00020:   {"name": "充电过流保护",         "level": "error"},
    0x00040:   {"name": "放电过流预警",         "level": "warn"},
    0x00080:   {"name": "放电过流保护",         "level": "error"},
    0x10000:   {"name": "持续过载预警",         "level": "warn"},
    0x00100:   {"name": "过温预警",             "level": "warn"},
    0x00200:   {"name": "过温保护",             "level": "error"},
    0x04000:   {"name": "低温预警",             "level": "warn"},
    0x01000:   {"name": "低温保护",             "level": "error"},
    0x08000:   {"name": "温升速率过快",         "level": "warn"},
    0x00400:   {"name": "热失控报警",           "level": "fatal"},
    0x00800:   {"name": "短路",                 "level": "fatal"},
    0x20000:   {"name": "电量过低预警",         "level": "warn"},
    0x40000:   {"name": "健康度衰减预警",       "level": "warn"},
    0x80000:   {"name": "通信/采样异常",        "level": "warn"},
    0x100000:  {"name": "电流采样失效",         "level": "fatal"},
    0x200000:  {"name": "绝缘电阻预警",         "level": "warn"},
    0x400000:  {"name": "绝缘电阻保护",         "level": "error"},
}
# 等级图标/中文(推送展示用)
FAULT_LEVEL_ICON = {"warn": "⚠️ 预警", "error": "🔴 保护", "fatal": "🚨 严重"}


def parse_faults(fault_code):
    """解析故障码 -> [(name, level, hex), ...], 按危害程度排序(fatal>error>warn)
    fault_code: 位掩码(可多个故障同时存在)"""
    code = int(fault_code or 0)
    if code == 0:
        return []
    hits = []
    for bit, info in FAULT_DEFS.items():
        if code & bit:
            hits.append((info["name"], info["level"], bit))
    # 排序: fatal(2) > error(1) > warn(0)
    _rank = {"fatal": 2, "error": 1, "warn": 0}
    hits.sort(key=lambda x: _rank.get(x[1], 0), reverse=True)
    return hits


class _AlertPusher(object):
    """多通道告警推送(单例)"""

    _CFG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "push_config.json")

    def __init__(self):
        _cfg = self._load_cfg()
        self._webhook = (_cfg.get("webhook") or os.environ.get("BMS_ALERT_WEBHOOK", "")).strip()
        self._sct_key = (_cfg.get("sct_key") or os.environ.get("BMS_ALERT_SERVERCHAN_KEY", "")).strip()
        self._pp_token = (_cfg.get("pp_token") or os.environ.get("BMS_ALERT_PUSHPLUS_TOKEN", "")).strip()
        self._online_notify = bool(_cfg.get("online_notify",
                                os.environ.get("BMS_ALERT_ONLINE_NOTIFY", "1") not in ("0", "false", "False")))
        self._enabled = bool(self._webhook or self._sct_key or self._pp_token)
        self._lock = threading.Lock()
        self._last_send_ts = 0.0
        self._last_fault = 0          # 上一次推送的故障码(去重: 同一故障只推一次)
        self._last_fault_ts = 0.0
        self._last_online = None      # 上一次在线状态(None=未知)
        self._min_interval = 2.0      # 最小推送间隔(秒), 防刷屏(企业微信上限20条/分)

        if self._enabled:
            _chans = []
            if self._webhook:      _chans.append("企业微信")
            if self._sct_key:      _chans.append("Server酱→个人微信")
            if self._pp_token:     _chans.append("PushPlus→个人微信")
            print("[ALERT] 告警推送已启用 (%s, 在线通知=%s)" %
                  ("+".join(_chans), "开" if self._online_notify else "关"))
        else:
            print("[ALERT] 告警推送未配置 (BMS_ALERT_WEBHOOK/SERVERCHAN_KEY/PUSHPLUS_TOKEN 均为空, 已禁用)")

    # ---------- 运行时配置(可编辑, 2026-08-10) ----------
    def _load_cfg(self):
        try:
            if os.path.exists(self._CFG_FILE):
                with open(self._CFG_FILE, "r", encoding="utf-8") as _f:
                    return json.load(_f) or {}
        except Exception as _e:
            # 2026-09-07: 配置 JSON 损坏时提示(返回 {} 降级行为不变)
            print("[ALERT] 运行配置 %s 加载失败, 用默认: %s" % (self._CFG_FILE, _e))
        return {}

    @staticmethod
    def _mask(v):
        v = (v or "").strip()
        if not v:
            return ""
        return v[:6] + "***" if len(v) > 6 else "***"

    def to_dict(self):
        return {
            "webhook": self._mask(self._webhook),
            "sct_key": self._mask(self._sct_key),
            "pp_token": self._mask(self._pp_token),
            "online_notify": self._online_notify,
            "enabled": self._enabled,
        }

    def set_config(self, d):
        """运行时更新推送配置并落盘; 返回 {"ok":bool,"config":dict,"msg":str}
        说明: webhook/sct_key/pp_token 若传空字符串表示清空该通道;
              出于安全, to_dict 返回的是脱敏值, 编辑时只需填入要变更的项,
              未变更项保持原值(前端传 '__keep__' 或不传该键)。"""
        if not isinstance(d, dict):
            return {"ok": False, "config": self.to_dict(), "msg": "配置格式错误"}
        try:
            if "webhook" in d and d["webhook"] not in ("", "__keep__"):
                self._webhook = str(d["webhook"]).strip()
            if "sct_key" in d and d["sct_key"] not in ("", "__keep__"):
                self._sct_key = str(d["sct_key"]).strip()
            if "pp_token" in d and d["pp_token"] not in ("", "__keep__"):
                self._pp_token = str(d["pp_token"]).strip()
            if "online_notify" in d:
                self._online_notify = bool(d["online_notify"])
        except (TypeError, ValueError) as e:
            return {"ok": False, "config": self.to_dict(), "msg": "参数类型错误: %s" % e}
        self._enabled = bool(self._webhook or self._sct_key or self._pp_token)
        # 同步环境变量
        os.environ["BMS_ALERT_WEBHOOK"] = self._webhook
        os.environ["BMS_ALERT_SERVERCHAN_KEY"] = self._sct_key
        os.environ["BMS_ALERT_PUSHPLUS_TOKEN"] = self._pp_token
        os.environ["BMS_ALERT_ONLINE_NOTIFY"] = "1" if self._online_notify else "0"
        try:
            with open(self._CFG_FILE, "w", encoding="utf-8") as _f:
                json.dump({
                    "webhook": self._webhook, "sct_key": self._sct_key,
                    "pp_token": self._pp_token, "online_notify": self._online_notify,
                }, _f, ensure_ascii=False, indent=2)
        except Exception as e:
            return {"ok": False, "config": self.to_dict(), "msg": "配置保存失败: %s" % e}
        return {"ok": True, "config": self.to_dict(), "msg": "已保存"}

    # ---------- 发送 ----------
    def _send(self, payload, bypass_throttle=False):
        """企业微信 webhook 发送(带节流), 成功返回 True
        2026-08-11 修复: bypass_throttle=True 供故障告警使用——
        设备恢复上线时"上线通知"与同轮"故障推送"几乎同时发出,
        2s 全局节流会把紧跟其后的故障推送吞掉(日志表现: 上线推送成功,
        故障推送"全部失败/未配置"且无 HTTP 错误明细). 故障已由
        10 分钟同故障去重 + 分级周期提醒限频, 绕过 2s 节流不会刷屏."""
        if not self._webhook:
            return False
        if not bypass_throttle and not self._throttle():
            return False
        try:
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            if _HAS_REQUESTS:
                r = requests.post(self._webhook, data=data, timeout=5)
                ok = (r.status_code == 200 and "errcode" in r.text)
                if not ok:
                    print("[ALERT] 企微推送失败: HTTP %s %s" % (r.status_code, r.text[:200]))
                return ok
            req = urllib.request.Request(self._webhook, data=data,
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=5) as resp:
                body = resp.read().decode("utf-8", "ignore")
                ok = ('"errcode":0' in body or '"errcode": 0' in body)
                if not ok:
                    print("[ALERT] 企微推送失败: %s" % body[:200])
                return ok
        except Exception as e:
            print("[ALERT] 企微推送异常: %s" % e)
            return False

    def _send_serverchan(self, title, md, bypass_throttle=False):
        """Server酱(推个人微信): GET https://sctapi.ftqq.com/{key}.send?title=&desp="""
        if not self._sct_key:
            return False
        if not bypass_throttle and not self._throttle():
            return False
        url = "https://sctapi.ftqq.com/%s.send?%s" % (
            self._sct_key,
            urllib.parse.urlencode({"title": title, "desp": md}))
        try:
            if _HAS_REQUESTS:
                r = requests.get(url, timeout=5)
                ok = (r.status_code == 200 and '"code":0' in r.text)
                if not ok:
                    print("[ALERT] Server酱推送失败: HTTP %s %s" % (r.status_code, r.text[:200]))
                return ok
            with urllib.request.urlopen(url, timeout=5) as resp:
                body = resp.read().decode("utf-8", "ignore")
                ok = ('"code":0' in body or '"code": 0' in body)
                if not ok:
                    print("[ALERT] Server酱推送失败: %s" % body[:200])
                return ok
        except Exception as e:
            print("[ALERT] Server酱推送异常: %s" % e)
            return False

    def _send_pushplus(self, title, md, bypass_throttle=False):
        """PushPlus(推个人微信): POST http://www.pushplus.plus/send {token,title,content,template=markdown}"""
        if not self._pp_token:
            return False
        if not bypass_throttle and not self._throttle():
            return False
        try:
            body = json.dumps({
                "token": self._pp_token,
                "title": title,
                "content": md,
                "template": "markdown",
            }, ensure_ascii=False).encode("utf-8")
            url = "https://www.pushplus.plus/send"
            if _HAS_REQUESTS:
                r = requests.post(url, data=body,
                                  headers={"Content-Type": "application/json"}, timeout=5)
                ok = (r.status_code == 200 and '"code":200' in r.text)
                if not ok:
                    print("[ALERT] PushPlus推送失败: HTTP %s %s" % (r.status_code, r.text[:200]))
                return ok
            req = urllib.request.Request(url, data=body,
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=5) as resp:
                text = resp.read().decode("utf-8", "ignore")
                ok = ('"code":200' in text)
                if not ok:
                    print("[ALERT] PushPlus推送失败: %s" % text[:200])
                return ok
        except Exception as e:
            print("[ALERT] PushPlus推送异常: %s" % e)
            return False

    def _throttle(self):
        """全局节流: 2s 内只允许一次真实发送(多通道共享)"""
        with self._lock:
            now = time.time()
            if now - self._last_send_ts < self._min_interval:
                return False
            self._last_send_ts = now
        return True

    # ---------- 对外接口 ----------
    def reset_fault_dedup(self):
        """2026-08-09: 设备重启/重连后重置故障去重, 让同故障可再次推送.
        设备重启后重新触发相同故障(如再次低电量)时, 原 10 分钟去重会拦截,
        导致"设备重启后警告不再推送". 在设备重连恢复时调用此方法."""
        with self._lock:
            self._last_fault = 0
            self._last_fault_ts = 0.0
    def _broadcast(self, title, md, bypass_throttle=False):
        """向所有已配置通道推送, 返回是否至少成功一个
        2026-08-10: 记录各通道结果, 便于确认"推送是否真的发出"(排查未收到微信)
        2026-08-11: 新增 bypass_throttle 透传(故障告警跳过 2s 节流, 防止被上线通知吞掉)
        2026-08-20 修复(#5 推送速度): 原实现在调用线程(通常是 IoTDA 轮询线程)里
          串行 POST 三个通道(各 timeout=5s), 企微/Server酱任一网络慢会阻塞轮询
          数秒~十几秒 → 数据延迟、故障检测延迟、前端掉线误判。改为后台守护线程
          异步发送, 调用立即返回(受理即 True); 故障去重(notify_fault 内 10min 去重)
          仍在调用线程同步判定, 不丢推送。多线程下 _throttle 由 _lock 保护."""
        def _async_send():
            try:
                r1 = self._send({"msgtype": "markdown", "markdown": {"content": md}},
                                bypass_throttle=bypass_throttle)
                r2 = self._send_serverchan(title, md, bypass_throttle=bypass_throttle)
                r3 = self._send_pushplus(title, md, bypass_throttle=bypass_throttle)
                print("[ALERT] 推送结果: 企业微信=%s Server酱=%s PushPlus=%s (%s)" %
                      (r1, r2, r3, "成功" if any([r1, r2, r3]) else "全部失败/未配置"))
            except Exception as _e:
                print("[ALERT] 异步推送异常: %s" % _e)
        t = threading.Thread(target=_async_send, daemon=True)
        t.start()
        return True

    def notify_text(self, msg):
        """推送纯文本(全部通道)"""
        return self._broadcast("BMS 通知", msg)

    def notify_markdown(self, md, bypass_throttle=False):
        """推送 markdown(全部通道)
        2026-08-11: bypass_throttle 透传(故障告警用 True 跳过 2s 节流)"""
        # 提取首行标题供 Server酱/PushPlus 使用
        _title = "BMS 告警"
        for line in (md or "").split("\n"):
            _t = line.strip().lstrip("#").strip()
            if _t:
                _title = _t[:30]
                break
        return self._broadcast(_title, md, bypass_throttle=bypass_throttle)

    def notify_fault(self, fault_code, status=None):
        """故障告警: 同故障去重(恢复前不重复推), 故障码非 0 才推
        fault_code: 设备上报故障掩码(0=无故障)
        status:     {soc, pack_v, current, temp_max, device_online, ...} 附带状态
        2026-08-09: 输出故障原因文字 + 危害等级图标 + 多故障列表(不再只有裸故障码)"""
        fault_code = int(fault_code or 0)
        if fault_code == 0 or not self._enabled:
            self._last_fault = 0      # 故障恢复, 允许下次新故障再推
            return False
        with self._lock:
            now = time.time()
            if fault_code == self._last_fault and (now - self._last_fault_ts) < 600:
                return False          # 同一故障 10 分钟内不重复推
            self._last_fault = fault_code
            self._last_fault_ts = now

        # 解析故障原因与危害等级(可多个同时存在)
        faults = parse_faults(fault_code)
        if not faults:
            faults = [("未知故障 0x%X" % fault_code, "warn", fault_code)]
        top_level = faults[0][1]                      # 最高危害等级(fatal>error>warn)
        top_icon = FAULT_LEVEL_ICON.get(top_level, "⚠️ 预警")

        st = status or {}
        soc = st.get("soc", "--")
        pack_v = st.get("pack_v", "--")
        cur = st.get("current", "--")
        # 2026-08-09 修复: 固件 temp_max 单位是 0.1℃(如 280=28.0℃),
        #   推送必须换算成 ℃, 否则显示 "280℃" 误导
        #   0 值(传感器未接/未上报)显示 "--" 而非 "0℃", 避免误导
        tmax = st.get("temp_max", "--")
        try:
            tmax = float(tmax)
            if tmax <= 0:
                tmax_str = "--"
            else:
                tmax = tmax / 10.0
                tmax_str = ("%.1f℃" % tmax) if (tmax * 10) % 10 != 0 else ("%d℃" % int(tmax))
        except (TypeError, ValueError):
            tmax_str = "--"
        # pack_v 单位 mV -> 显示 V 更直观(如 16500 -> 16.5V); 0 值显示 "--"
        try:
            pack_v_f = float(pack_v)
            pack_v_str = ("%.2fV" % (pack_v_f / 1000.0)) if pack_v_f > 0 else "--"
        except (TypeError, ValueError):
            pack_v_str = "--"
        # 电流 mA -> A(超过 1A 显示 A, 否则保留 mA 更精细)
        try:
            cur_f = float(cur)
            cur_str = ("%.2fA" % (cur_f / 1000.0)) if abs(cur_f) >= 1000 else ("%dmA" % int(cur_f))
        except (TypeError, ValueError):
            cur_str = "--"
        # 等级中文名 + 处置建议(让推送更可读、可行动)
        _LV_CN = {"warn": "预警", "error": "保护", "fatal": "严重"}
        _LV_TIP = {
            "warn":  "请关注, 持续观察是否升级",
            "error": "请立即检查, 必要时降低负载",
            "fatal": "请立即断电检修!",
        }
        top_cn = _LV_CN.get(top_level, "预警")
        top_tip = _LV_TIP.get(top_level, "")

        # ===== 2026-08-10 优化: 推送消息结构化 + 等级醒目 + 处置建议 =====
        #   原格式"故障原因: A、B"一长串, 多故障时难读; 现逐行列出每个故障,
        #   标题带等级图标, 补充处置建议与 emoji 分隔, 企业微信/Server酱均友好
        md = "## %s BMS 设备%s\n" % (top_icon, top_cn)
        md += "> "
        md += "、".join("**%s** (%s)" % (name, FAULT_LEVEL_ICON.get(lv, "⚠️"))
                        for name, lv, _bit in faults)
        md += "\n\n"
        md += "> 📟 故障码: `0x%X`\n" % fault_code
        md += "> 📊 SOC: **%s%%** | 总压: **%s** | 电流: **%s** | 最高温: **%s**\n" % (
            soc, pack_v_str, cur_str, tmax_str)
        if top_tip:
            md += "> ⚠️ 处置建议: **%s**\n" % top_tip
        md += "> ⏰ %s\n" % time.strftime("%Y-%m-%d %H:%M:%S")
        # 2026-08-11 修复: 故障告警绕过 2s 全局节流(bypass_throttle=True),
        #   否则设备恢复上线时"上线通知"先消耗节流窗口, 紧跟的故障推送会被
        #   _throttle 直接丢弃(日志见: 上线成功 + 故障"全部失败/未配置"无 HTTP 错误).
        return self.notify_markdown(md, bypass_throttle=True)

    def notify_online_change(self, online, status=None, force=False):
        """设备离线/上线通知(可选开启)
        force=True: 2026-08-09 设备重启/重连恢复时强制推送上线——
          正常路径 `_last_online is None` 首帧不推(防后端重启误报),
          但设备重启恢复属于真实上线事件, 由调用方传 force=True 确保推送."""
        if not self._enabled or not self._online_notify:
            self._last_online = online
            return False
        # 2026-08-09 修复: 后端刚启动时 _last_online=None(状态未知),
        #   首帧只记录状态不推送——否则每次后端重启都会误报一次"设备上线"
        #   (此前 220 次重启 -> 67 次"上线"误报, 设备本身一直在线)
        if self._last_online is None and not force:
            self._last_online = online
            return False
        if online == self._last_online and not force:
            return False              # 状态未变化不推
        self._last_online = online
        icon = "🟢" if online else "🔴"
        title = "设备上线" if online else "设备离线"
        # ===== 2026-08-10 优化: 在线/离线通知结构化, 与故障消息风格统一 =====
        #   原格式仅时间+原始 dict, 手机上看不清; 现补充关键状态摘要(电压/温度/SOC)
        md = "## %s BMS %s\n" % (icon, title)
        md += "> ⏰ %s\n" % time.strftime("%Y-%m-%d %H:%M:%S")
        if status:
            sh = status.get("last_shadow") if isinstance(status, dict) else None
            if isinstance(sh, dict):
                # 摘要关键字段(单位换算与故障消息一致: 电压 mV->V, 温度 0.1℃->℃)
                _soc = sh.get("soc", "--")
                _pv = sh.get("pack_v", 0)
                try:
                    _pv_str = ("%.2fV" % (float(_pv) / 1000.0)) if float(_pv) > 0 else "--"
                except (TypeError, ValueError):
                    _pv_str = "--"
                _tm = sh.get("temp_max", 0)
                try:
                    _tmf = float(_tm)
                    _tm_str = ("%.1f℃" % (_tmf / 10.0)) if _tmf > 0 else "--"
                except (TypeError, ValueError):
                    _tm_str = "--"
                _cur = sh.get("current", 0)
                try:
                    _curf = float(_cur)
                    _cur_str = ("%.2fA" % (_curf / 1000.0)) if abs(_curf) >= 1000 else ("%dmA" % int(_curf))
                except (TypeError, ValueError):
                    _cur_str = "--"
                md += "> 📊 SOC: **%s%%** | 总压: **%s** | 电流: **%s** | 最高温: **%s**\n" % (
                    _soc, _pv_str, _cur_str, _tm_str)
            elif status.get("last_shadow"):
                md += "> 最近数据: %s\n" % str(status["last_shadow"])[:200]
        return self.notify_markdown(md)


# 单例
AlertPusher = _AlertPusher()


def _self_test():
    """简单自测: python alert_push.py"""
    print("企微 webhook:", AlertPusher._webhook or "(未配置)")
    print("Server酱 key:", (AlertPusher._sct_key[:6] + "***") if AlertPusher._sct_key else "(未配置)")
    print("PushPlus token:", (AlertPusher._pp_token[:6] + "***") if AlertPusher._pp_token else "(未配置)")
    print("enabled:", AlertPusher._enabled)
    print("notify_text:", AlertPusher.notify_text("BMS 告警模块自检 OK"))


if __name__ == "__main__":
    _self_test()
