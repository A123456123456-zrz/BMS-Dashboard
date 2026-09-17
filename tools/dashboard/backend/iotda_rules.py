# -*- coding: utf-8 -*-
"""
云端联动规则引擎(2026-08-09)
============================================
对标: 华为云 IoTDA 设备联动规则/数据转发最佳实践
  - 平台规则引擎: 属性→命令联动(低电量自动保护等)
  - 订阅推送: 属性变化推送应用侧
  - 数据转发: 转 OBS/DIS/Kafka/函数计算(免本地 DB 膨胀)

本模块在后端实现"本地联动规则引擎"(不依赖云控制台):
  每次影子数据更新时评估规则, 命中即自动下发命令 + 告警推送.
  云控制台侧的数据转发/订阅推送为控制台配置, 见说明书《云端联动配置》章节.

配置(dashboard.env):
  BMS_RULE_ENABLED=1        # 1=启用联动规则(默认1)
  BMS_RULE_LOW_SOC=15       # 低电量联动阈值%(SOC<=15% 自动下发保护, 0=关闭)
  BMS_RULE_OVERVOLT=1       # 1=过压自动下发放电禁止(关闭充电MOS) 0=关闭
  BMS_RULE_TEMP_PROT=1      # 1=过温自动全断继电器 0=关闭
"""
import os
import time
import json


class RuleEngine(object):
    """本地联动规则引擎: 影子数据 → 自动命令 + 告警"""

    _CFG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rules_config.json")

    def __init__(self):
        # 默认值来自 dashboard.env, 运行时配置文件(rules_config.json)优先级更高
        _cfg = self._load_cfg()
        self._enabled = bool(_cfg.get("enabled",
                            os.environ.get("BMS_RULE_ENABLED", "1") not in ("0", "false", "False")))
        self._low_soc = int(_cfg.get("low_soc", os.environ.get("BMS_RULE_LOW_SOC", "15") or 0))
        self._overvolt = bool(_cfg.get("overvolt",
                             os.environ.get("BMS_RULE_OVERVOLT", "1") not in ("0", "false", "False")))
        self._temp_prot = bool(_cfg.get("temp_prot",
                             os.environ.get("BMS_RULE_TEMP_PROT", "1") not in ("0", "false", "False")))
        # 2026-08-18 规则扩展: 通用保护位联动总开关(欠压/过流/低温/热失控/短路, 默认开)
        self._protect = bool(_cfg.get("protect",
                             os.environ.get("BMS_RULE_PROTECT", "1") not in ("0", "false", "False")))
        self._debounce_s = int(_cfg.get("debounce_s", 300))
        # 防抖: 每个规则最后一次触发时间戳(秒), 同一规则 N 秒内不重复触发
        self._last_trigger = {}
        if self._enabled:
            print("[RULES] 联动规则引擎已启用 (低电量=%s%% 过压=%s 过温=%s 保护位=%s)" %
                  (self._low_soc, self._overvolt, self._temp_prot, self._protect))
        else:
            print("[RULES] 联动规则引擎已禁用 (enabled=0)")

    # ---------- 运行时配置(可编辑, 2026-08-10) ----------
    def _load_cfg(self):
        try:
            if os.path.exists(self._CFG_FILE):
                with open(self._CFG_FILE, "r", encoding="utf-8") as _f:
                    return json.load(_f) or {}
        except Exception:
            pass
        return {}

    def to_dict(self):
        return {
            "enabled": self._enabled,
            "low_soc": self._low_soc,
            "overvolt": self._overvolt,
            "temp_prot": self._temp_prot,
            "protect": self._protect,
            "debounce_s": self._debounce_s,
        }

    def set_config(self, d):
        """运行时更新联动规则配置并落盘; 返回 {"ok":bool,"config":dict,"msg":str}"""
        if not isinstance(d, dict):
            return {"ok": False, "config": self.to_dict(), "msg": "配置格式错误"}
        try:
            if "enabled" in d:
                self._enabled = bool(d["enabled"])
            if "low_soc" in d:
                _v = int(d["low_soc"])
                if _v < 0 or _v > 100:
                    return {"ok": False, "config": self.to_dict(), "msg": "低电量阈值需在 0~100%%"}
                self._low_soc = _v
            if "overvolt" in d:
                self._overvolt = bool(d["overvolt"])
            if "temp_prot" in d:
                self._temp_prot = bool(d["temp_prot"])
            if "protect" in d:
                self._protect = bool(d["protect"])
            if "debounce_s" in d:
                _v = int(d["debounce_s"])
                if _v < 0 or _v > 3600:
                    return {"ok": False, "config": self.to_dict(), "msg": "防抖时间需在 0~3600 秒"}
                self._debounce_s = _v
        except (TypeError, ValueError) as e:
            return {"ok": False, "config": self.to_dict(), "msg": "参数类型错误: %s" % e}
        # 同步回环境变量, 保持日志/其他读取一致
        os.environ["BMS_RULE_ENABLED"] = "1" if self._enabled else "0"
        os.environ["BMS_RULE_LOW_SOC"] = str(self._low_soc)
        os.environ["BMS_RULE_OVERVOLT"] = "1" if self._overvolt else "0"
        os.environ["BMS_RULE_TEMP_PROT"] = "1" if self._temp_prot else "0"
        try:
            with open(self._CFG_FILE, "w", encoding="utf-8") as _f:
                json.dump(self.to_dict(), _f, ensure_ascii=False, indent=2)
        except Exception as e:
            return {"ok": False, "config": self.to_dict(), "msg": "配置保存失败: %s" % e}
        return {"ok": True, "config": self.to_dict(), "msg": "已保存"}

    # ---------- 工具 ----------
    # G6(2026-08-10): 联动规则可下发的 set_param 白名单 + 边界, 与 app.PARAM_META 对齐。
    #   自包含定义(不 import app, 避免循环依赖), 防止联动绕过 /api/params 校验发非法值到设备。
    _ALLOWED_SET_PARAMS = {
        "soc_low_warn_pct": (0, 100),
    }

    def _validate_set_param(self, key, value):
        """校验联动要下发的 set_param; 返回 (ok, msg)"""
        bounds = self._ALLOWED_SET_PARAMS.get(key)
        if bounds is None:
            return False, "联动不允许下发该参数: %s" % key
        try:
            v = int(value)
        except (ValueError, TypeError):
            return False, "参数类型错误"
        if v < bounds[0] or v > bounds[1]:
            return False, "参数超出范围(%s~%s)" % (bounds[0], bounds[1])
        return True, ""

    def _debounced(self, key):
        """防抖: 返回 True 表示可以触发(距上次触发超过防抖时间)"""
        now = time.time()
        last = self._last_trigger.get(key, 0)
        if now - last < self._debounce_s:
            return False
        self._last_trigger[key] = now
        return True

    def _send_cmd(self, iotda_client, name, paras):
        """下发命令, 返回结果 dict"""
        if iotda_client is None:
            return {"ok": False, "msg": "iotda_client 未初始化"}
        try:
            return iotda_client.publish_cmd({"command_name": name, "paras": paras})
        except Exception as e:
            print("[RULES] 命令下发异常 %s: %s" % (name, e))
            return {"ok": False, "msg": str(e)}

    # ---------- 规则评估 ----------
    def evaluate(self, payload, iotda_client, alert_pusher=None):
        """每次影子数据更新时调用
        payload:      设备最新数据(_shadow_to_payload 输出)
        iotda_client: 用于下发命令
        alert_pusher: 告警推送(可选, 复用 AlertPusher)
        """
        if not self._enabled or not isinstance(payload, dict):
            return

        soc = payload.get("soc")
        fault = int(payload.get("fault") or 0)
        pack_v = int(payload.get("pack_v") or 0)
        temp_max = payload.get("temp_max")
        no_data = payload.get("no_data")
        if no_data:
            return                                  # 无有效数据不评估

        # ---- 规则1: 低电量联动(自动下发保护) ----
        if self._low_soc > 0 and isinstance(soc, (int, float)):
            soc_pct = float(soc)
            if soc_pct <= self._low_soc and self._debounced("low_soc"):
                # G6: 下发前复用边界校验(尽管 _low_soc 已由 set_config 钳制 0~100, 双保险)
                _ok, _why = self._validate_set_param("soc_low_warn_pct", self._low_soc)
                if not _ok:
                    print("[RULES] 低电量联动跳过(参数校验失败): %s" % _why)
                else:
                    print("[RULES] 低电量联动触发: SOC=%.1f%% <= %d%%" % (soc_pct, self._low_soc))
                    r = self._send_cmd(iotda_client, "set_param",
                                       {"key": "soc_low_warn_pct", "value": self._low_soc})
                    if alert_pusher:
                        alert_pusher.notify_markdown(
                            "## ⚠️ 低电量联动\n> SOC=%.1f%% 已自动下发低电量保护配置\n> 时间: %s" %
                            (soc_pct, time.strftime("%Y-%m-%d %H:%M:%S")))

        # ---- 规则2: 过压联动(自动禁止充电, 关闭充电MOS) ----
        # fault & 0x0002 = FAULT_CELL_OV_PROT(单体过压保护)
        if self._overvolt and (fault & 0x0002) and self._debounced("overvolt"):
            print("[RULES] 过压联动触发: fault=0x%X" % fault)
            r = self._send_cmd(iotda_client, "set_charge", {"enable": 0})
            if alert_pusher:
                alert_pusher.notify_markdown(
                    "## 🔴 过压联动保护\n> 单体过压已自动下发「停止充电」\n> fault=0x%X | 总压=%d mV\n> 时间: %s" %
                    (fault, pack_v, time.strftime("%Y-%m-%d %H:%M:%S")))

        # ---- 规则3: 过温联动(自动全断继电器) ----
        # fault & 0x0200 = FAULT_OT_PROT(过温保护)
        if self._temp_prot and (fault & 0x0200) and self._debounced("temp_prot"):
            print("[RULES] 过温联动触发: fault=0x%X" % fault)
            # 全断: 关闭充电+放电继电器
            self._send_cmd(iotda_client, "set_charge", {"enable": 0})
            self._send_cmd(iotda_client, "set_discharge", {"enable": 0})
            if alert_pusher:
                alert_pusher.notify_markdown(
                    "## 🔴 过温联动保护\n> 已自动全断充放电继电器\n> fault=0x%X | 最高温=%s\n> 时间: %s" %
                    (fault, temp_max, time.strftime("%Y-%m-%d %H:%M:%S")))

        # ---- 规则4(2026-08-18 扩展): 其他保护位联动 ----
        #   此前仅覆盖过压/过温; 用户指出保护场景不全, 补齐:
        #     0x0008 单体欠压保护  → 停放电 (避免深放)
        #     0x0020 充电过流保护  → 停充电
        #     0x0080 放电过流保护  → 停放电
        #     0x0100 过温预警/0x0020 已覆盖; 0x1000 低温保护 → 停充电(低温禁充)
        #     0x0400 热失控 0x0800 短路 → 全断(最高优先级)
        if self._protect and fault:
            _desc = ""
            _act = None            # "charge" / "discharge" / "all"
            if fault & 0x0400:
                _desc, _act = "热失控", "all"
            elif fault & 0x0800:
                _desc, _act = "短路", "all"
            elif fault & 0x0008:
                _desc, _act = "单体欠压", "discharge"
            elif fault & 0x0080:
                _desc, _act = "放电过流", "discharge"
            elif fault & 0x0020:
                _desc, _act = "充电过流", "charge"
            elif fault & 0x1000:
                _desc, _act = "低温", "charge"
            if _act and self._debounced("prot_" + _desc):
                print("[RULES] %s联动触发: fault=0x%X -> %s" % (_desc, fault, _act))
                if _act in ("charge", "all"):
                    self._send_cmd(iotda_client, "set_charge", {"enable": 0})
                if _act in ("discharge", "all"):
                    self._send_cmd(iotda_client, "set_discharge", {"enable": 0})
                if alert_pusher:
                    alert_pusher.notify_markdown(
                        "## 🔴 %s联动保护\n> 已自动%s\n> fault=0x%X | 总压=%d mV | 时间: %s" %
                        (_desc, "全断继电器" if _act == "all" else ("停充电" if _act == "charge" else "停放电"),
                         fault, pack_v, time.strftime("%Y-%m-%d %H:%M:%S")))


# 单例
RuleEngine_ = RuleEngine()
