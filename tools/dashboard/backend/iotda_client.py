# -*- coding: utf-8 -*-
"""
华为云 IoTDA 应用侧客户端
===================================
通过设备影子查询设备数据,并调用 REST API 下发异步命令,
最后通过 SocketIO 把数据/响应推送给前端。

⚠️ 重要:
  Flask-SocketIO 使用 eventlet 异步模式时,eventlet 会 monkeypatch ssl/urllib3,
  导致华为云 Python SDK 在 SSL context 设置阶段产生无限递归 RecursionError。
  因此本文件不使用 huaweicloudsdk*,而是直接用 requests + 手工 HMAC-SHA256 签名,
  完全绕过 SDK,从根源消除兼容性问题。

华为云 API 签名算法参考:
  https://support.huaweicloud.com/devg-apisign/api-sign-spec.html
"""
import binascii
import datetime
import hashlib
import hmac
import json
import threading
import time
import traceback
import urllib.parse

# 2026-08-10 修复: 数值解析安全 helper(根治 F1: 元素级 float 转换崩溃)
# 2026-08-19 修复(B1): 引入 safe_float —— props_to_payload 全字段安全转换,
#   坏类型字段(如 int("abc"))不再抛异常导致整帧丢弃(设备被误判离线)
from utils import safe_int, safe_float, safe_int_list

import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

import database as db
from alert_push import AlertPusher, parse_faults   # 2026-08-09: 企业微信 webhook 告警推送(未配置自动禁用); 2026-08-10: parse_faults 用于按危害等级差异化推送周期
from iotda_rules import RuleEngine_  # 2026-08-09: 本地联动规则引擎(低电量/过压/过温自动下发命令)


# ====================================================================
# 2026-08-10 D6: 上报数据 CRC 完整性校验(与固件 sys_mqtt.c 完全同算法)
#   固件对 crc_key 计算 CRC8(多项式0x07,初始0x00) + CRC32(IEEE 0xEDB88320,
#   初始0xFFFFFFFF,结果取反), 追加到上报 JSON 的 crc / crc32 字段.
#   crc_key 与上报字段一一对应(2026-08-10 起 balanceOn 布尔替代 bmask):
#     fault(4B LE) + chargeMos + dischargeMos + balanceOn + workState
#     + chargeMode + timestamp(2B LE)  —— 共 11 字节
#   后端用影子属性复算并比对, 结果输出 crc_ok: True/False/None(固件未上报)
# ====================================================================
def verify_report_crc(props):
    """复算固件 CRC8/CRC32 并与上报值比对.
    props: 影子/上报属性 dict.
    返回: (ok, crc8_recalc, crc32_recalc) —— ok=None 表示固件未上报 crc 字段"""
    def crc8_calc(data):
        crc = 0x00
        for b in data:
            crc ^= b
            for _ in range(8):
                crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
        return crc

    def crc32_calc(data):
        crc = 0xFFFFFFFF
        for b in data:
            crc ^= b
            for _ in range(8):
                crc = ((crc >> 1) ^ 0xEDB88320) & 0xFFFFFFFF if (crc & 1) else (crc >> 1)
        return (~crc) & 0xFFFFFFFF

    try:
        fault = int(props.get("fault", 0) or 0) & 0xFFFFFFFF
        chg = 1 if int(props.get("chargeMos", props.get("charge_mos", 0)) or 0) else 0
        dsg = 1 if int(props.get("dischargeMos", props.get("discharge_mos", 0)) or 0) else 0
        bal = 1 if int(props.get("balanceOn", props.get("balance_on", 0)) or 0) else 0
        wst = int(props.get("workState", props.get("work_state", 0)) or 0) & 0xFF
        cmd = int(props.get("chargeMode", props.get("charge_mode", 0)) or 0) & 0xFF
        ts  = int(props.get("timestamp", 0) or 0) & 0xFFFF
        key = (fault.to_bytes(4, "little") + bytes([chg, dsg, bal, wst, cmd])
               + ts.to_bytes(2, "little"))
        c8 = crc8_calc(key)
        c32 = crc32_calc(key)
    except Exception:
        return None, None, None
    rep_crc = props.get("crc")
    rep_crc32 = props.get("crc32")
    if rep_crc is None and rep_crc32 is None:
        return None, c8, c32          # 旧固件未上报 → 不判定
    ok = True
    if rep_crc is not None and int(rep_crc) != c8:
        ok = False
    if rep_crc32 is not None and int(rep_crc32) != c32:
        ok = False
    return ok, c8, c32


def _build_http_adapter():
    """工业标准 HTTP 会话适配器:
    - 仅对幂等方法(GET/HEAD/OPTIONS)自动重试, POST 命令绝不自动重试
      (防止消息重复下发导致设备重复执行, 例如 set_charge 双触发)
    - 指数退避 backoff_factor=1.0 → 重试间隔 1s/2s/4s, 并带少量随机抖动
    - 5xx 状态码触发重试, 4xx 不重试(4xx 重试无意义)
    - raise_on_status=False: 重试耗尽后返回最后一次响应, 由上层统一判 4xx/5xx
    """
    try:
        retry = Retry(
            total=3, connect=3, read=2,
            backoff_factor=1.0,
            status_forcelist=(500, 502, 503, 504),
            allowed_methods=frozenset(["GET", "HEAD", "OPTIONS"]),
            raise_on_status=False,
        )
    except TypeError:
        # 兼容旧版 urllib3(无 allowed_methods 关键字, 用 method_whitelist)
        retry = Retry(
            total=3, connect=3, read=2,
            backoff_factor=1.0,
            status_forcelist=(500, 502, 503, 504),
            method_whitelist=frozenset(["GET", "HEAD", "OPTIONS"]),
            raise_on_status=False,
        )
    return HTTPAdapter(max_retries=retry)


# ---------- 华为云签名工具(完全复刻官方 SDK v3 DerivationAKSKSigner + HKDF) ----------
_EMPTY_SHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
_DERIVED_ALG = "V11-HMAC-SHA256"
_DERIVED_SVC_IOTDA = "iotdm"  # IoTDA 专属实例要求派生服务名固定为 iotdm
_HKDF_HASH_LEN = 32
_HKDF_OKM_LEN = 32
_HKDF_EXPAND_CEIL = 1  # ceil(32/32)


def _hw_url_encode(s: str) -> str:
    """官方 SDK: quote(s, safe='~'), 每段 path 单独编码后再拼接"""
    return urllib.parse.quote(s, safe="~")


def _hkdf_extract(ikm: str, salt: str) -> bytes:
    if not salt:
        salt = bytes(_HKDF_HASH_LEN).decode("utf-8")
    return hmac.new(salt.encode("utf-8"), ikm.encode("utf-8"),
                    digestmod=hashlib.sha256).digest()


def _hkdf_expand_first(prk: bytes, info: bytes) -> bytes:
    t = info + bytearray((1,))
    return hmac.new(prk, t, digestmod=hashlib.sha256).digest()


def _get_der_key_sha256(access_key: str, secret_key: str, info: str) -> str:
    """对应 huaweicloudsdkcore.signer.hkdf.get_der_key_sha256"""
    tmp = _hkdf_extract(secret_key, access_key)
    der = _hkdf_expand_first(tmp, info.encode("utf-8"))
    return binascii.hexlify(der).decode()


def _hw_sign(ak: str, sk: str, method: str, uri: str, query: dict,
             headers: dict, body: bytes, region: str,
             service: str = _DERIVED_SVC_IOTDA, use_derived: bool = True) -> dict:
    """
    按官方 SDK huaweicloudsdkcore/signer/signer.py 实现:
      - use_derived=True  -> DerivationAKSKSigner (V11-HMAC-SHA256 + HKDF)
      - use_derived=False -> Signer              (SDK-HMAC-SHA256, 普通 HMAC)
    IoTDA 专属实例端点要求 V11 派生签名,默认 use_derived=True。
    """
    method = method.upper()
    now = datetime.datetime.utcnow()
    x_date = now.strftime("%Y%m%dT%H%M%SZ")
    date_stamp = now.strftime("%Y%m%d")

    # 合并头
    h = dict(headers)
    h["Host"] = headers.get("Host", "")
    h["X-Sdk-Date"] = x_date
    if body:
        h.setdefault("Content-Type", "application/json;charset=UTF-8")
        ct = h.get("Content-Type", "")
        if not ct.startswith("application/json") and not ct.startswith("application/bson"):
            h["X-Sdk-Content-Sha256"] = "UNSIGNED-PAYLOAD"

    # SignedHeaders: 所有不含下划线的 header 小写,按字典序排序
    signed_headers = sorted([k.lower() for k in h.keys() if "_" not in k])

    # CanonicalHeaders: signed_headers 顺序,key:value + \n
    canon_headers_parts = []
    for k in signed_headers:
        v = ""
        for orig_k in h.keys():
            if orig_k.lower() == k:
                v = str(h[orig_k]).strip()
                break
        canon_headers_parts.append(k + ":" + v)
    canon_headers = "\n".join(canon_headers_parts) + "\n"

    # CanonicalURI: 每段 url_encode 后用 / 拼接,最后补 /
    raw_segments = urllib.parse.unquote(uri).split("/")
    uri_segments = [_hw_url_encode(s) for s in raw_segments]
    canon_uri = "/".join(uri_segments)
    if not canon_uri.endswith("/"):
        canon_uri += "/"

    # CanonicalQueryString
    q_items = []
    for k, v in (query or {}).items():
        if isinstance(v, list):
            for vi in sorted(v):
                q_items.append((_hw_url_encode(k), _hw_url_encode(str(vi))))
        elif isinstance(v, bool):
            q_items.append((_hw_url_encode(k), _hw_url_encode(str(v).lower())))
        else:
            q_items.append((_hw_url_encode(k), _hw_url_encode(str(v))))
    q_items.sort(key=lambda x: x[0])
    canon_query = "&".join("%s=%s" % x for x in q_items)

    # Payload hash
    content_hash = h.get("X-Sdk-Content-Sha256")
    if content_hash is None:
        content_hash = hashlib.sha256(body or b"").hexdigest()

    canonical_request = "\n".join([
        method, canon_uri, canon_query, canon_headers,
        ";".join(signed_headers), content_hash,
    ])
    canon_req_hash = hashlib.sha256(canonical_request.encode("utf-8")).hexdigest()

    # 按模式选 StringToSign
    if use_derived:
        info = "%s/%s/%s" % (date_stamp, region, service)
        string_to_sign = "%s\n%s\n%s\n%s" % (
            _DERIVED_ALG, x_date, info, canon_req_hash)
        # HKDF 派生密钥 -> hex 字符串,作为签名 key
        der_key_hex = _get_der_key_sha256(ak, sk, info)
        signature = hmac.new(der_key_hex.encode("utf-8"),
                             string_to_sign.encode("utf-8"),
                             digestmod=hashlib.sha256).digest().hex()
        auth = "%s Credential=%s/%s, SignedHeaders=%s, Signature=%s" % (
            _DERIVED_ALG, ak, info,
            ";".join(signed_headers), signature)
    else:
        string_to_sign = "SDK-HMAC-SHA256\n%s\n%s" % (x_date, canon_req_hash)
        signature = hmac.new(sk.encode("utf-8"),
                             string_to_sign.encode("utf-8"),
                             digestmod=hashlib.sha256).digest().hex()
        auth = "SDK-HMAC-SHA256 Access=%s, SignedHeaders=%s, Signature=%s" % (
            ak, ";".join(signed_headers), signature)

    h["Authorization"] = auth
    return h


# ====================================================================
# 2026-08-16 双通道主备反转: 本地腿(EMQX/Mosquitto 订阅)为主, 华为云腿降级为兜底。
#   本地腿毫秒级实时推送后, 华为云影子轮询(~10s)再推同一帧会覆盖/滞后实时数据。
#   做法: 本地腿每次收到数据调用 mark_local_leg_active() 刷新活跃时间;
#         华为云腿推送 bms_data 前检查 is_local_leg_active()——本地腿活跃则跳过
#         实时推送(数据已由本地腿推过), 但 DB 入库/规则引擎/告警推送全部保留。
# ====================================================================
_LOCAL_LEG_ACTIVE_TS = 0.0

def mark_local_leg_active():
    """本地腿收到实时数据时调用, 记录活跃时间戳"""
    global _LOCAL_LEG_ACTIVE_TS
    _LOCAL_LEG_ACTIVE_TS = time.time()

def is_local_leg_active(window_sec=30.0):
    """本地腿是否在最近 window_sec 秒内活跃(有数据到达)"""
    return (time.time() - _LOCAL_LEG_ACTIVE_TS) < window_sec


# ====================================================================
# 2026-08-11 修复: 三路入口统一字段归一化(影子轮询 / IoTDA 数据转发推送 / MQTT 直连上报)
#   - 固件 sys_mqtt.c 上报为 camelCase + services[].properties 包裹(见 L1451),
#     旧 __shadow_to_payload 只认影子 reported.properties, 导致另外两路把原始
#     camelCase 直接塞进 db.insert_data(读 snake_case) → pack_v/v_max/temp_max/cells 全 0/空。
#   - 抽出 props_to_payload / extract_bms_props, 三条路径共用同一映射, 字段不再掉。
# ====================================================================
def extract_bms_props(message):
    """从多种消息形状提取 BMS 平铺属性字典(flat props)。
    支持的输入形状:
      - 直接发布(properties/report): {"services":[{"service_id":"BMS","properties":{...}}]}
      - 设备影子(shadow):            列表 [{"service_id":"BMS","reported":{"properties":{...}}}]
                                    或 {"services":[{"service_id":"BMS","reported":{...}}]}
      - 华为云转发 notify_data 包:    {"notify_data":{"body": <上述任一>}}
      - 已是平铺属性字典:             {"soc":...,"packV":...} (直接返回)
    返回 flat props dict; 无可解析属性返回 None。
    """
    # 影子形状直接是 services 列表, 统一包成 {"services": [...]} 处理
    if isinstance(message, list):
        message = {"services": message}
    if not isinstance(message, dict):
        return None
    msg = message
    # 解开华为云数据转发的 notify_data 包裹(转发消息常在 body 里再包一层)
    if isinstance(msg.get("notify_data"), dict):
        _body = msg["notify_data"].get("body")
        if isinstance(_body, dict):
            msg = _body
    # services 列表形式(直接发布 / 影子)
    services = msg.get("services")
    if isinstance(services, list):
        props = {}
        for svc in services:
            if not isinstance(svc, dict):
                continue
            # 仅取 BMS 服务(与旧 _shadow_to_payload 语义一致, 避免非 BMS 服务字段串入)
            sid = svc.get("service_id")
            if sid is not None and sid != "BMS":
                continue
            svc_props = svc.get("properties")
            if not isinstance(svc_props, dict):
                reported = svc.get("reported")
                if isinstance(reported, dict):
                    svc_props = reported.get("properties")
            if isinstance(svc_props, dict):
                props.update(svc_props)
        return props if props else None
    # 已是平铺属性字典(直接发布到 bms/data 且未包 services)
    return msg if msg else None


def props_to_payload(props, pending_series_num=0):
    """把 BMS 平铺属性(props, 兼容 camelCase + 旧 snake_case)规范化为
    入库(database.insert_data)与前端(bms_data)统一的 snake_case 负载。
    与 IoTDAClient._shadow_to_payload 共用同一映射, 保证三路字段一致。
    props 为空/无效返回 None。
    """
    if not isinstance(props, dict) or not props:
        return None

    # 单体电压数组: 新物模型字段 cellVoltages(字符串数组), 兼容旧字段 cells(JSON字符串/数组)
    cells = props.get("cellVoltages", props.get("cells", "[]"))
    if isinstance(cells, str):
        try:
            cells = json.loads(cells)
        except Exception:
            cells = []
    elif cells is None:
        cells = []
    # 串数动态化: 以设备上报 cells 长度为准; 为空时按当前串数补 0
    if isinstance(cells, list) and len(cells) > 0:
        db.update_series_num(len(cells))
        # 2026-08-09 诊断: 用户下发过串数但设备上报长度未变 → 设备未确认命令
        if pending_series_num > 0 and len(cells) != pending_series_num:
            print("[IoTDA] 诊断: 期望串数=%d 但设备上报=%d → 设备未确认串数切换 "
                  "(查固件 messages/down 处理/串口)" % (pending_series_num, len(cells)))
    else:
        cells = [0] * db.get_series_num()

    # 新物模型(BMS-V2)字段为 camelCase, 优先取新字段, 兼容旧字段
    # 2026-08-19 修复(B1): 全部用 safe_int/safe_float —— 坏类型字段不再抛异常丢整帧
    pack_v = safe_int(props.get("packV", props.get("pack_v", 0)))
    v_max = safe_int(props.get("vMax", props.get("v_max", 0)))
    v_min = safe_int(props.get("vMin", props.get("v_min", 0)))
    # cellVoltages 为字符串数组("3500","3498",...), 先转 float 再取整
    cells_int = safe_int_list(cells, default_if_bad=0)
    # pack_v / v_max / v_min 兜底: cells 有效时从 cells 计算
    if pack_v <= 0 and any(c > 0 for c in cells_int):
        pack_v = sum(cells_int)
    if v_max <= 0 and any(c > 0 for c in cells_int):
        v_max = max(cells_int)
    if v_min <= 0 and any(c > 0 for c in cells_int):
        v_min = min(c for c in cells_int if c > 0)

    soc = safe_float(props.get("soc", 0))
    soh = safe_float(props.get("soh", 0))
    current = safe_int(props.get("current", 0))
    tmax = safe_int(props.get("tempMax", props.get("temp_max", 0)))
    tmin = safe_int(props.get("tempMin", props.get("temp_min", 0)))
    # 多路温度数组(新物模型 temps, 0.1℃ 字符串数组)
    temps_raw = props.get("temps") or []
    if isinstance(temps_raw, str):
        try:
            temps_raw = json.loads(temps_raw)
        except Exception:
            temps_raw = []
    temps_int = safe_int_list(temps_raw, default_if_bad=0)
    # 绝缘检测字段(不平衡电桥法, 0=未启用/未接线)
    ins_rp = safe_int(props.get("insulationRp", props.get("insulation_rp", 0)))
    ins_rn = safe_int(props.get("insulationRn", props.get("insulation_rn", 0)))
    ins_ov = safe_int(props.get("insulationOhmPerV", props.get("insulation_ohm_per_v", 0)))

    # 2026-08-10 D6: 上报数据 CRC 完整性校验(与固件同算法复算比对)
    crc_ok, crc8_v, crc32_v = verify_report_crc(props)
    if crc_ok is False:
        print("[IoTDA] ⚠️ 数据完整性校验失败(CRC不匹配): 上报 crc=%s crc32=%s, "
              "复算 crc8=%d crc32=%d, fault=%s" % (
                  props.get("crc"), props.get("crc32"), crc8_v, crc32_v, props.get("fault")))

    payload = {
        "soc": soc,
        "soh": soh,
        "v_max": v_max,
        "v_min": v_min,
        "pack_v": pack_v,
        "current": current,
        "temp_max": tmax,
        "temp_min": tmin,
        "temps": temps_int,
        "fault": safe_int(props.get("fault", 0)),
        "insulation_rp": ins_rp,
        "insulation_rn": ins_rn,
        "insulation_ohm_per_v": ins_ov,
        "cells": cells_int,
        "cell_series_num": db.get_series_num(),
        "balance_mask": safe_int(props.get("balanceMask", props.get("balance_mask", 0))),
        "cycle_count": safe_int(props.get("cycleCount", props.get("cycle_count", 0))),
        "charge_mos": safe_int(props.get("chargeMos", props.get("charge_mos", -1)), -1),
        "discharge_mos": safe_int(props.get("dischargeMos", props.get("discharge_mos", -1)), -1),
        "balance_on": safe_int(props.get("balanceOn", props.get("balance_on",
                            1 if safe_int(props.get("balanceMask", props.get("balance_mask", 0))) != 0 else 0))),
        "charge_mode": safe_int(props.get("chargeMode", props.get("charge_mode", 0))),
        "work_state": safe_int(props.get("workState", 0)),
        "fw_version": str(props.get("fwVersion", "") or ""),
        "capacity_ah": safe_float(props.get("capacityAh", 0)),
        "timestamp": safe_int(props.get("timestamp"), default=int(time.time() * 1000)),
        "data_source": {
            "soc": "real" if soc > 0 else "none",
            "soh": "real" if soh > 0 else "none",
            "voltage": "real" if pack_v > 0 else "none",
            "current": "real" if current != 0 else "none",
            "temp": "real" if tmax > 0 else "none",
            "fault": "real",
        },
        "is_mock": False,
        "no_data": False,
        "crc_ok": crc_ok,
        # 通信健康状态位域(设备自报, 真·同步): 取代前端对 MQTT/TLS/CAN 的推断.
        # 由固件 WiFi/MQTT/CAN 真实连接态置位, 随属性上报; 前端状态卡片据此显示.
        "comm_status": safe_int(props.get("commStatus", props.get("comm_status", 0))),
        # RS485/Modbus 从站通信状态(固件自检并随属性上报):
        #   rs485Online=1 近期被主站轮询 / rs485LastPoll=距最近轮询秒数(65535=从未) / rs485Err=CRC 错误累计
        "rs485_online": safe_int(props.get("rs485Online", props.get("rs485_online", 0))),
        "rs485_last_poll": safe_int(props.get("rs485LastPoll", props.get("rs485_last_poll", 0))),
        "rs485_err": safe_int(props.get("rs485Err", props.get("rs485_err", 0))),
        # 2026-08-14: OTA 阶段回报(固件随属性上报, 设备真·确认)
        #   ota_stage: idle/downloading/verifying/rebooting/failed
        #   ota_progress: 下载进度 0~100
        #   ota_error: 失败错误码(PARAM_INVALID/DOWNLOAD_FAIL/INCOMPLETE_DATA/
        #             INVALID_IMAGE_SIZE/VALIDATE_FAILED/WRITE_FAILED), 无失败为空串
        #   前端 OTA 状态机据此把 ②设备下载 / ③校验写入 从"推测"升级为"设备已确认"
        "ota_stage": str(props.get("otaStage", props.get("ota_stage", "")) or ""),
        "ota_progress": safe_int(props.get("otaProgress", props.get("ota_progress", 0))),
        "ota_error": str(props.get("otaError", props.get("ota_error", "")) or ""),
    }
    return payload


class IoTDAClient:
    """华为云 IoTDA 应用侧客户端(后台线程轮询,原生 requests)"""

    # 专属实例应用侧端点(从 project config 拼接)
    INSTANCE_ID = "6a3ff62aab"

    def __init__(self, ak, sk, project_id, region, device_id, socketio,
                 poll_interval=60):
        self.ak = ak
        self.sk = sk
        self.project_id = project_id
        self.region = region
        self.device_id = device_id
        self.socketio = socketio
        self.poll_interval = poll_interval  # 保留但实际使用分级间隔

        self._thread = None
        self._running = False
        self._last_shadow = None
        self._last_cmp = None
        # 2026-08-11: 补传帧守卫状态——设备断网缓存恢复后经 properties/report
        #   补传会覆盖华为云影子, 轮询到的时间戳旧于已见最新帧。识别到补传帧时
        #   只回填历史库, 不推实时; 补传结束后(再次收到实时帧)通知前端刷新历史段.
        self._replay_seen = False
        # 2026-08-12: 记录已回填过的补传时间戳, 避免同一离线影子被重复轮询时反复入表/刷屏
        self._last_replay_ts = 0
        self._pending_cmds = {}
        # 区分两种连接状态:
        #   cloud_accessible: 后端 Dashboard -> 华为云 IoTDA (REST API) 是否可用
        #   device_online:   ESP32 设备 -> 华为云 IoTDA (MQTT) 是否在线
        self.connected = False                  # 兼容旧字段: 同 cloud_accessible
        self.cloud_accessible = False           # 后端访问华为云 REST API 是否正常
        self.device_online = False              # ESP32 设备 MQTT 是否在线 (status=ONLINE)
        self.device_last_status = "UNKNOWN"     # ESP32 最近一次状态字
        self._session = requests.Session()
        # 2026-08-10 工业标准加固: 挂载带指数退避的幂等重试适配器(GET 自动重试, POST 不重试)
        self._session.mount("https://", _build_http_adapter())
        self._session.mount("http://", _build_http_adapter())
        # 2026-08-10 修复: 隔离系统代理误读——requests 在 Windows 上默认会读取
        #   注册表/环境变量代理(IoTDA 日志曾出现 ProxyError/自签名证书校验失败,
        #   即流量被中间代理拦截). 本服务直连华为云公网, 关闭 trust_env 使其只走直连;
        #   若企业网络必须走代理, 请显式设置 session.proxies 后再取消本行.
        self._session.trust_env = False

        # 手动刷新控制
        import threading as _th
        self._refresh_lock = _th.Lock()
        self._force_refresh = False
        self._last_next_query = 0.0

        # 2026-08-11: 云端 API 调用计数(仅用于 /api/status 展示透明度, 不影响轮询节奏;
        #   REST API 额度为 20万/天, 远超本系统用量, 故此处不做限速, 仅统计展示)
        self._api_calls = []
        self._api_book_start = time.time()
        self._api_budget = 15000        # 与设备消息日上限一致, 仅作展示对照

        # 2026-08-09: 主动重启/OTA 后的离线推送抑制窗口(秒时间戳)
        #   用户点击"重启设备/OTA升级"后设备会短暂断线, 属预期行为,
        #   不应触发"设备离线"告警推送(避免误报)
        self._suppress_offline_until = 0.0

        # ===== 2026-08-09 多设备影子轮询 =====
        #   device_shadows[device_id] = 各设备最新 payload(由 _loop 轮询填充)
        #   device_online[device_id]  = 各设备在线状态
        self.device_shadows = {}
        self.device_online_map = {}
        # 2026-08-09: 持续故障周期提醒时间戳(上次故障推送时刻)
        #   原逻辑仅"新故障位"推送, 持续预警(如低电量)永不重推 → 加周期提醒
        self._last_fault_push_ts = 0.0
        # 2026-08-09: 期望串数(用户下发 cell_series_num 后记录, 用于诊断设备是否确认)
        self._pending_series_num = 0

        host = "%s.iotda-app.%s.myhuaweicloud.com" % (self.INSTANCE_ID, self.region)
        self._endpoint = "https://" + host
        self._host = host

        print("[IoTDA] 客户端已创建, endpoint=%s, device_id=%s" % (self._endpoint, self.device_id))

    # ---------- 基础 REST 调用 ----------
    def _call(self, method: str, path: str, query: dict = None, body: dict = None,
              timeout: tuple = (5, 10)):
        """统一封装华为云 APIG 签名 + 请求"""
        self._book_api()   # 2026-08-11: 统计云端 API 调用(展示用)
        if not path.startswith("/"):
            path = "/" + path
        url = self._endpoint + path
        body_bytes = b""
        headers = {"Host": self._host}
        if body is not None:
            body_bytes = json.dumps(body, separators=(",", ":")).encode("utf-8")
            headers["Content-Type"] = "application/json;charset=UTF-8"

        sig_headers = _hw_sign(
            ak=self.ak, sk=self.sk, method=method, uri=path,
            query=query or {}, headers=headers, body=body_bytes,
            region=self.region, service="iotdm", use_derived=True,
        )
        headers.update(sig_headers)
        try:
            resp = self._session.request(
                method=method, url=url, params=query, data=body_bytes,
                headers=headers, timeout=timeout,
            )
        except Exception as e:
            print("[IoTDA] HTTP 异常: %s %s -> %s" % (method, path, e))
            return None
        if resp.status_code >= 400:
            # Bug7 修复: 打印完整错误信息便于调试命令下发失败
            print("[IoTDA] HTTP %s %s -> %d %s" % (
                method, path, resp.status_code, resp.text[:500]))
            return None
        try:
            return resp.json()
        except Exception:
            return resp.text

    # ---------- 查询设备影子 + 在线状态 ----------
    def _fetch_shadow(self, device_id=None):
        """通过原生 REST 查询设备影子,返回 services 列表(类 SDK 格式)或 None
        2026-08-09: 支持指定 device_id(多设备轮询), 缺省用主设备"""
        dev_id = device_id or self.device_id
        path = "/v5/iot/%s/devices/%s/shadow" % (self.project_id, dev_id)
        data = self._call("GET", path)
        if not isinstance(data, dict):
            return None
        shadow = data.get("shadow")
        if shadow is None:
            shadow = data.get("device_shadow") or []
        return shadow if isinstance(shadow, list) else []

    def _fetch_device_status(self, device_id=None):
        """调用设备详情 API,返回 {"online": bool, "status": str, "event_time": str or None}
        官方字段: status=ONLINE/OFFLINE/FROZEN/INACTIVE, connection_status_update_time
        2026-08-09: 支持指定 device_id(多设备), 缺省用主设备"""
        dev_id = device_id or self.device_id
        path = "/v5/iot/%s/devices/%s" % (self.project_id, dev_id)
        data = self._call("GET", path)
        if not isinstance(data, dict):
            return None
        status = data.get("status", "UNKNOWN")
        return {
            "online": status == "ONLINE",
            "status": status,
            "connection_status_update_time": data.get("connection_status_update_time"),
            "active_time": data.get("active_time"),
        }

    @staticmethod
    def _get_event_time_ms(shadow) -> int:
        """从影子 BMS 服务的 reported.event_time 解析毫秒级时间戳,失败返回 0"""
        if not shadow:
            return 0
        for svc in shadow:
            sid = getattr(svc, "service_id", None) if not isinstance(svc, dict) else svc.get("service_id")
            if sid != "BMS":
                continue
            reported = getattr(svc, "reported", None) if not isinstance(svc, dict) else svc.get("reported")
            if not reported:
                continue
            et = reported.get("event_time") if isinstance(reported, dict) else getattr(reported, "event_time", None)
            if not et:
                return 0
            # 常见格式 20260804T131125Z
            try:
                import datetime as _dt
                t = _dt.datetime.strptime(et, "%Y%m%dT%H%M%SZ").replace(tzinfo=_dt.timezone.utc)
                return int(t.timestamp() * 1000)
            except Exception:
                pass
            try:
                import datetime as _dt
                t = _dt.datetime.fromisoformat(et.replace("Z", "+00:00"))
                return int(t.timestamp() * 1000)
            except Exception:
                return 0
        return 0

    # ---------- 内部: 取 DB 最后一条有效记录(作为 fallback, 不生成任何 sin/sine 波!) ----------
    def _last_db_fallback(self):
        """
        设备离线/影子为空时 => 不再生成正弦波(之前的波浪线bug!), 只返回本地DB最后一条静态数据
        没有DB记录 => 返回 None (交给前端 no_data 显示占位, 不画图)
        """
        try:
            import database as _db
            r = _db.query_latest()
            if r is None:
                return None
            # 过滤 "空壳上报" 无效记录(和 database.py 里判定一致)
            cells = [int(r.get("c%d" % i, 0) or 0) for i in range(1, _db.get_series_num() + 1)] if "c1" in r else (r.get("cells") or [])
            if isinstance(cells, list) and len(cells) > 0:
                pv = int(r.get("pack_v") or 0) or sum(cells)
                if pv > 0 and any(c > 0 for c in cells):
                    payload = {
                        "soc": float(r.get("soc") or 0.0),
                        "soh": float(r.get("soh") or 0.0),
                        "pack_v": pv,
                        "current": int(r.get("current") or 0),
                        "temp_max": int(r.get("temp_max") or 0),
                        "temp_min": int(r.get("temp_min") or 0),
                        "fault": int(r.get("fault") or 0),
                        "cells": cells,
                        "v_max": int(r.get("v_max") or 0) or (max(cells) if cells else 0),
                        "v_min": int(r.get("v_min") or 0) or (min(cells) if cells else 0),
                        "balance_mask": int(r.get("balance_mask") or 0),
                        "cycle_count": int(r.get("cycle_count") or 0),
                        "timestamp": int(float(r.get("timestamp") or 0) * (1000 if float(r.get("timestamp") or 0) < 1e12 else 1)),
                        "recv_time": float(r.get("recv_time") or time.time()),
                    }
                    return payload
            return None
        except Exception:
            return None

    # ---------- 2026-08-11: 云端 API 调用计数(展示用) ----------
    def _book_api(self):
        """记录一次云端 API 调用(仅统计, 用于状态页展示透明度)"""
        _now = time.time()
        self._api_calls.append(_now)
        _cut = _now - 86400
        _q = self._api_calls
        _i = 0
        while _i < len(_q) and _q[_i] < _cut:
            _i += 1
        if _i:
            self._api_calls = _q[_i:]

    def _api_used_24h(self):
        """返回最近 24h 云端 API 调用次数"""
        self._book_api()
        return len(self._api_calls)

    # ---------- 内部: 解析华为云属性格式为前端格式 ----------
    def _shadow_to_payload(self, shadow):
        """
        把华为云设备影子的 reported 属性转回前端需要的格式
        ⚠️  2026-08-06 修改: 直接返回 ESP32 上报的原始数据, 不再用 DB 缓存兜底
            传感器未接时 ESP32 上报全 0, payload 标记 no_data=True
            前端收到 no_data 后 KPI 显示"--"、曲线断线、表格空
        2026-08-11: 统一走 extract_bms_props + props_to_payload
            (与 IoTDA 推送 / MQTT 上报三路共用同一字段映射)
        """
        if shadow is None:
            return None
        props = extract_bms_props(shadow)
        if not props:
            return None
        return props_to_payload(props, getattr(self, "_pending_series_num", 0))

    # ---------- 内部: 生成空数据(ESP32 离线/影子过期) ----------
    def _gen_empty_payload(self):
        """
        设备离线或影子过期时返回纯空数据, 不再从 DB 取缓存
        前端收到 no_data=True 后 KPI 显示"--"、曲线断线、表格空
        DB 历史记录不受影响, 设备恢复后新数据正常写入
        """
        return {
            "soc": 0.0, "soh": 0.0,
            "v_max": 0, "v_min": 0, "pack_v": 0,
            "current": 0, "temp_max": 0, "temp_min": 0,
            "fault": 0, "cells": [0]*db.get_series_num(), "balance_mask": 0, "cycle_count": 0,
            "charge_mos": -1, "discharge_mos": -1, "balance_on": 0, "charge_mode": 0,
            "timestamp": int(time.time() * 1000),
            "is_mock": True,
            "no_data": True,
            "data_source": {
                "soc": "none", "soh": "none", "voltage": "none",
                "current": "none", "temp": "none", "fault": "none",
            },
        }

    # ---------- 手动刷新触发 ----------
    def trigger_refresh(self):
        """用户手动触发一次立即刷新(供前端按钮使用)"""
        with self._refresh_lock:
            self._force_refresh = True

    # ---------- 轮询线程(分级频率控制) ----------
    def _loop(self):
        # 分级间隔配置(秒)
        #   用户明确要求: 恢复网/电后 ≤3分钟 网页必须看到上线
        #   配额测算(最坏全天离线): 快扫+慢扫合计~3700条/天, 华为云20万/天额度绰绰有余
        INTERVAL_ONLINE = 5             # 设备在线: 5s (2026-08-11 提速: 配合固件上报10s, 前端≤5s内看到新数据; REST额度20万/天, 34k/天仅占17%)
        INTERVAL_FAULT_ACTIVE = 5       # 故障存在时: 5s 快速轮询(告警/恢复尽快上屏)
        # 2026-08-14 提速: 重新上电/网后前端必须尽快看到上线, 原 60s 太慢。
        #   改为多级: 刚离线前 30s 用在线节奏 5s 超快探测(立即发现恢复),
        #   之后 30s~15min 10s, 15min~1h 30s, >1h 120s(仍省额度)。
        #   配额测算: 全天真离线最坏 ~1.2万次/天, 远低于华为云20万/天上限。
        INTERVAL_OFFLINE_FAST = 10      # 离线 30s~15min: 10s 快速探测(等用户恢复上电/网)
        INTERVAL_OFFLINE_RECENT = 30    # 离线 15min~1h: 30s
        INTERVAL_OFFLINE_LONG = 120     # 离线>1h: 120s (原900s, 仍省额度)
        INTERVAL_COLD_START = 10        # 冷启动前 5 分钟: 10s 快速轮询
        COLD_START_DURATION = 300       # 冷启动持续 5 分钟
        OFFLINE_HOT_SCAN_DURATION = 30  # 2026-08-14: 刚离线前 30s 超快探测窗口(5s, 像在线一样)
        OFFLINE_FAST_SCAN_DURATION = 15 * 60  # 刚离线索引前 15 分钟快扫窗口
        NIGHT_INTERVAL_MULT = 2         # 夜间(23-7点) 所有间隔 ×2

        # 新鲜度阈值
        STALE_MS = 10 * 60 * 1000
        OFFLINE_THRESHOLD = 3600     # 1小时
        STALE_OFFLINE_MS = 60 * 1000  # 2026-08-14: 影子事件时间停滞阈值, 超过即提前判数据链路失联(不等华为云keepalive)

        online_seen = False
        offline_since = 0
        consecutive_failures = 0   # 2026-08-10: 连续失败计数(指数退避用, 成功即复位)
        loop_start = time.time()
        print("[IoTDA] 分级轮询已启动: 在线=%ss 故障=%ss 离线0-30s=%ss 30s-15min=%ss 15min-1h=%ss 1h+=%ss 冷启动=%ss(前5分钟)" %
              (INTERVAL_ONLINE, INTERVAL_FAULT_ACTIVE, INTERVAL_ONLINE,
               INTERVAL_OFFLINE_FAST, INTERVAL_OFFLINE_RECENT, INTERVAL_OFFLINE_LONG, INTERVAL_COLD_START))

        while self._running:
            now_time = time.time()
            hour = time.localtime().tm_hour
            is_night = hour >= 23 or hour < 7
            sleep_for = INTERVAL_OFFLINE_LONG

            try:
                # ---------- 计算目标间隔 ----------
                # 冷启动前 5 分钟: 快速轮询 60s, 设备上电后快速被检测到
                in_cold_start = (now_time - loop_start) < COLD_START_DURATION
                if in_cold_start and not online_seen:
                    sleep_for = INTERVAL_COLD_START
                elif online_seen and offline_since > 0:
                    offline_duration = now_time - offline_since
                    if offline_duration < OFFLINE_HOT_SCAN_DURATION:
                        # 2026-08-14: 刚离线前 30s 用在线节奏 5s 超快探测,
                        #   设备一旦恢复上电/网, 5s 内即可被发现(原 60s 太慢)
                        sleep_for = INTERVAL_ONLINE
                    elif offline_duration < OFFLINE_FAST_SCAN_DURATION:
                        # 离线索引 30s~15min: 10s 快速探测 → 恢复后最坏 10s 内发现上线
                        sleep_for = INTERVAL_OFFLINE_FAST
                    elif offline_duration < OFFLINE_THRESHOLD:
                        # 离线索引 15min~1h: 30s 探测 → 最坏 30s 内发现
                        sleep_for = INTERVAL_OFFLINE_RECENT
                    else:
                        # 离线超过 1h: 省电省配额, 120s 一次
                        sleep_for = INTERVAL_OFFLINE_LONG
                elif online_seen:
                    sleep_for = INTERVAL_ONLINE
                else:
                    # 从未在线(非冷启动): 保持 60s 快速探测, 而不是 900s
                    # 2026-08-08 修复: 设备上线后 ≤1-2min 内必须被发现,
                    #   原逻辑掉到 INTERVAL_OFFLINE_LONG(900s) 会导致:
                    #   设备已在云端 ONLINE 但后端缓存仍 OFFLINE -> 不写 DB -> 前端无数据
                    sleep_for = INTERVAL_OFFLINE_FAST

                if is_night:
                    sleep_for *= NIGHT_INTERVAL_MULT

                # 手动刷新: 立即跳过等待
                with self._refresh_lock:
                    if self._force_refresh:
                        sleep_for = 0
                        self._force_refresh = False

                # ---------- 先查设备在线状态 ----------
                dev_status = self._fetch_device_status()
                is_online = False
                dev_status_str = "UNKNOWN"
                if dev_status is not None:
                    is_online = bool(dev_status.get("online"))
                    dev_status_str = dev_status.get("status") or dev_status_str
                    # 设备详情 API 调用成功: 表示后端->华为云 REST API 可用
                    self.cloud_accessible = True
                    self.connected = True
                    # 缓存 ESP32 设备 MQTT 连接状态
                    self.device_online = is_online
                    self.device_last_status = dev_status_str
                    consecutive_failures = 0   # REST 调用成功 → 复位退避计数
                else:
                    # _fetch_device_status 返回 None (HTTP 失败/超时/签名错误等)
                    # => 后端无法访问华为云, 两种连接状态都视为不可用 (保守但避免误报在线)
                    self.cloud_accessible = False
                    self.connected = False
                    consecutive_failures += 1   # 2026-08-10: 连续失败计数(末尾统一应用退避)
                    # 保留上一次的 device_online, 仅把字串标记为未知, 避免闪烁
                    self.device_last_status = dev_status_str
                    # 2026-08-07 修复: REST 瞬时失败时沿用上次已知在线状态,
                    # 避免"设备已连 MQTT 但网页误报离线" (与上方注释语义一致)
                    is_online = self.device_online

                # ---------- 在线状态变化处理 ----------
                if is_online:
                    # 2026-08-09 修复: 设备"离线→恢复"与"重启重连"时, 必须可靠推送
                    #   (1) 后端首帧看到在线(not online_seen): 仅记录不推(防后端重启误报)
                    #   (2) 设备从离线恢复(offline_since>0): 真实上线事件 → 强制推送 + 重置故障去重
                    _was_offline = (offline_since > 0)
                    if not online_seen and not _was_offline:
                        print("[IoTDA] 设备已上线!(后端首帧, 不推送)")
                    elif _was_offline:
                        print("[IoTDA] 设备离线恢复上线!")
                        # 设备恢复: 重置故障去重, 让同故障可再次推送(警告不再丢失)
                        try:
                            AlertPusher.reset_fault_dedup()
                        except Exception:
                            pass
                        try:
                            AlertPusher.notify_online_change(True, {"last_shadow": self._last_shadow}, force=True)
                        except Exception:
                            pass
                    elif not online_seen:
                        print("[IoTDA] 设备已上线!")
                        # 后端刚启动后设备上线: 按首帧处理(不推)
                        try:
                            AlertPusher.notify_online_change(True, {"last_shadow": self._last_shadow})
                        except Exception:
                            pass
                    # Bug1 修复: 设备上线时立即推送状态, 不等下次轮询
                    try:
                        self.socketio.emit("mqtt_status", {
                            "connected": True, "no_device": False,
                            "cloud_accessible": True,
                            "device_status": "ONLINE",
                            "device_online": True,
                            "next_query_in": INTERVAL_ONLINE,
                            "poll_mode": "online",
                            "region": self.region,
                        })
                    except Exception:
                        pass
                    online_seen = True
                    offline_since = 0
                    sleep_for = INTERVAL_ONLINE
                    if is_night:
                        sleep_for *= NIGHT_INTERVAL_MULT
                elif online_seen and offline_since == 0:
                    offline_since = now_time
                    print("[IoTDA] 设备已离线,启动 %ss 快速探测(前15min),之后逐级降频" % INTERVAL_OFFLINE_FAST)
                    # 2026-08-09: 离线告警推送(企业微信)
                    #   抑制窗口内(用户刚点重启/OTA)不推送"设备离线"告警, 避免误报
                    _in_suppress = (now_time < self._suppress_offline_until)
                    try:
                        if _in_suppress:
                            print("[IoTDA] 离线发生在重启/OTA 抑制窗口内, 跳过离线推送告警")
                        else:
                            AlertPusher.notify_online_change(False, {"last_shadow": self._last_shadow})
                    except Exception:
                        pass
                    # 设备离线时立即推送状态 (注意: 前端不清零数据,显示最后一次缓存=水平直线)
                    try:
                        self.socketio.emit("mqtt_status", {
                            "connected": bool(dev_status is not None),
                            "no_device": True,
                            "cloud_accessible": bool(dev_status is not None),
                            "device_status": dev_status_str,
                            "device_online": False,
                            "next_query_in": INTERVAL_OFFLINE_FAST,
                            "poll_mode": "offline_fast",
                            "region": self.region,
                        })
                    except Exception:
                        pass

                # ---------- 获取影子 ----------
                shadow = self._fetch_shadow()
                event_time_ms = self._get_event_time_ms(shadow) if shadow else 0
                now_ms = int(time.time() * 1000)
                is_fresh = event_time_ms > 0 and (now_ms - event_time_ms) <= STALE_MS
                # 2026-08-06 修复: 信任华为云设备详情 API 的 online/offline 状态,
                #   不再用"影子新鲜"强制覆盖为 ONLINE.
                #   原因: 设备断线后 10 分钟内影子数据仍算"新鲜"(STALE_MS=10min),
                #         旧逻辑会强制判为 ONLINE, 导致设备实际断线后前端 10 分钟内仍显示"在线".
                #   现逻辑: 设备详情 API 返回 OFFLINE 即判离线, 影子新鲜度只用于决定是否使用影子数据.
                # 2026-08-14 提速: 影子事件时间停滞 > 阈值 → 提前判数据链路失联
                #   华为云 keepalive 超时(1.5×60s=90s)导致真正标 OFFLINE 偏慢;
                #   用影子 event_time 停滞作更灵敏信号(上报间隔10s, 停滞60s≈6次未报即判失联),
                #   不等华为云, 立即置 is_online=False → 下方 trust_shadow=False → payload=None
                #   → 896 分支推 device_online=False, 前端约60s内显示离线(原需等~90s)。
                #   恢复上报后 event_time 刷新, 立刻回 ONLINE(后端在线态5s高频探测)。
                #   误报风险: 需连续60s丢失上报, 概率低; 且恢复即回, 窗口短。
                if is_online and event_time_ms > 0 and (now_ms - event_time_ms) > STALE_OFFLINE_MS:
                    print("[IoTDA] 影子停滞>%ss, 提前判数据链路失联(不等华为云keepalive)" % (STALE_OFFLINE_MS // 1000))
                    is_online = False
                    dev_status_str = "LINK_STALE"
                trust_shadow = is_online and is_fresh

                if trust_shadow and shadow:
                    # 2026-08-10 修复(F1 根因): 主设备路径原未包裹异常,
                    #   一旦 _shadow_to_payload 内部解析异常(如 cells 畸形), 异常会冒泡到
                    #   外层 except 把设备误判离线 + 触发重连抖动。
                    #   副设备路径(_poll_other_devices L977)早已包裹, 此处补齐对称性。
                    try:
                        payload = self._shadow_to_payload(shadow)
                    except Exception as _e:
                        print("[IoTDA] _shadow_to_payload 异常(降级为空数据, 不判离线): %s" % _e)
                        traceback.print_exc()
                        payload = None
                else:
                    payload = None

                # ---------- 离线/影子过期: 返回空数据, 不写 DB ----------
                if payload is None:
                    payload = self._gen_empty_payload()
                    payload["device_status"] = dev_status_str
                    payload["last_event_time_ms"] = event_time_ms
                    # 保留上一条有效 pack_v, 供前端画灰色虚线(断线指示线)
                    try:
                        _prev = self._last_shadow or {}
                        _prev_pv = int(_prev.get("pack_v") or _prev.get("last_pack_v") or 0)
                        if _prev_pv > 0:
                            payload["last_pack_v"] = _prev_pv
                    except Exception:
                        pass
                    # Bug1 修复: 设备在线但影子过期时, 仍应标记为在线
                    self.connected = self.cloud_accessible
                    cmp_payload = {k: payload[k] for k in ("current", "fault", "timestamp")}
                    if self._last_shadow is None or cmp_payload != self._last_cmp:
                        self._last_shadow = payload
                        self._last_cmp = cmp_payload
                        # 离线/影子过期时不写 DB, 避免全0数据污染历史记录
                        try:
                            self.socketio.emit("bms_data", payload)
                            self.socketio.emit("mqtt_status", {
                                "connected": self.cloud_accessible,
                                "cloud_accessible": self.cloud_accessible,
                                "no_device": not is_online,
                                "device_status": dev_status_str,
                                "device_online": bool(is_online),
                                "next_query_in": sleep_for,
                                "poll_mode": "night" if is_night else ("offline" if online_seen else "cold"),
                                "region": self.region,
                                "db_count": db.query_count() if hasattr(db, "query_count") else 0,
                            })
                        except Exception:
                            pass
                    self._poll_pending_commands()
                    self._last_next_query = now_time + sleep_for
                    # 用 sleep_for 而非 self.poll_interval
                    time.sleep(max(0.1, sleep_for))
                    continue

                # ---------- ONLINE 且影子新鲜 ----------
                payload["device_status"] = "ONLINE"
                payload["last_event_time_ms"] = event_time_ms
                # 数据陈旧判定: 与 app.py DATA_STALE_MS=200s 一致
                #   超过 200s 无新数据 => no_data=True, 前端显示"--"和断线
                #   但保留 last_pack_v 供前端画灰色虚线(断线指示线)
                DATA_STALE_MS = 200 * 1000
                _data_age_ms = max(0, now_ms - event_time_ms) if event_time_ms > 0 else 0
                if _data_age_ms > DATA_STALE_MS:
                    payload["no_data"] = True
                    payload["data_age_ms"] = _data_age_ms
                    # 保留最后有效电压, 前端据此画灰色虚线
                    if payload.get("pack_v", 0) > 0:
                        payload["last_pack_v"] = payload["pack_v"]
                # connected(旧兼容字段) = cloud_accessible
                self.connected = self.cloud_accessible
                # ===== 2026-08-11: 补传帧时间戳守卫 =====
                # 设备断网缓存恢复后经 properties/report 补传会覆盖华为云影子,
                # 轮询到的新帧时间戳 < 已见最新 → 判定为补传/回退帧:
                #   只 insert_data 回填历史(recv_time 用设备真实时间戳,
                #   历史曲线自动补全断网时段), 不更新 _last_shadow、不推 WS 实时、
                #   不触发告警/规则 → 实时性零影响(企业时序数据 late-data 处理).
                _new_ts = int(payload.get("timestamp") or 0)
                _last_ts = 0
                if self._last_shadow:
                    try:
                        _last_ts = int(self._last_shadow.get("timestamp") or 0)
                    except (TypeError, ValueError):
                        _last_ts = 0
                if (_new_ts > 946684800000 and _last_ts > 946684800000
                        and _new_ts < _last_ts):
                    # 2026-08-11 去重守卫: 同一补传帧(相同时间戳)在每个轮询周期被重复命中,
                    # 若无条件 insert 会在 DB 写入大量重复行并刷海量日志(见 app.stdout.log
                    # 中"补传帧已回填历史"反复出现同一时间戳). 仅当时间戳与上次不同才回填,
                    # 避免重复写入同一断网时段数据, 也避免设备卡在补发态时刷屏.
                    if _new_ts != self._last_replay_ts:
                        try:
                            db.insert_data(payload)
                            print("[IoTDA] 补传帧(timestamp=%s)已回填历史, 不推送实时" % _new_ts)
                        except Exception as _e:
                            print("[DB] 补传帧回填失败:", _e)
                        self._last_replay_ts = _new_ts
                    self._replay_seen = True
                    self._last_next_query = now_time + sleep_for
                    time.sleep(max(0.1, sleep_for))
                    continue
                # cmp_payload 加入 no_data: no_data 状态变化时也要推送前端
                cmp_payload = {k: payload[k] for k in ("current", "fault", "timestamp", "no_data")}
                # 2026-08-08 优化: 影子上报事件时间(evt)纳入推送判定
                #   原因: 华为云收到设备新上报后 event_time 会变化, 但 current/fault/timestamp
                #         可能恰好与上一帧相同(如静止/无故障), 旧逻辑会跳过推送,
                #         导致"华为云有数据但网页不更新". 加入 evt 后, 新上报必推前端.
                cmp_payload["evt"] = event_time_ms
                # 2026-08-07: 记录旧 fault, 用于 0→非0 跳变时补发 bms_fault 告警事件
                _old_fault = int((self._last_shadow or {}).get("fault") or 0) if self._last_shadow else 0
                if self._last_shadow is None or cmp_payload != self._last_cmp:
                    self._last_shadow = payload
                    self._last_cmp = cmp_payload
                    # 只有 no_data=False 且有真实数据时才写 DB, 避免陈旧/空数据污染历史记录
                    # 2026-08-06 修复: 电流有效时也视为有真实数据,
                    #   否则 LTC6804 读取失败(pack_v=0/cells=0)但电流传感器正常时,
                    #   历史曲线完全不更新, 用户看不到任何数据变化
                    _pv = payload.get("pack_v", 0) or 0
                    _cells = payload.get("cells", [])
                    _cur = abs(int(payload.get("current", 0) or 0))
                    _has_real = (_pv > 0
                                 or (isinstance(_cells, list) and any(v > 0 for v in _cells))
                                 or _cur > 0)
                    if _has_real and not payload.get("no_data"):
                        db.insert_data(payload)
                    try:
                        # 2026-08-16 主备反转: 本地腿(EMQX 订阅)活跃时跳过华为云实时推送
                        # (数据已由本地腿毫秒级推过, 再推同一帧会滞后/覆盖); DB 入库/规则/告警保留,
                        # 本地腿 30s 无数据(断线)则华为云腿自动兜底推送
                        if not is_local_leg_active():
                            self.socketio.emit("bms_data", payload)
                        # 2026-08-07: fault 0→非0 跳变时补发告警事件, 前端秒级弹告警日志
                        # 2026-08-09 修复: 原条件 `_new_fault and not _old_fault` 只在
                        #   fault 从 0 变为非0 时触发——若设备已带其他故障(如过压位 0x00001)
                        #   又新增低电量位(0x20001), _old_fault 非 0 导致新故障永不推送.
                        #   改为检测"新出现的故障位"(_new_fault & ~_old_fault), 任何新故障都推
                        _new_fault = int(payload.get("fault") or 0)
                        _new_bits = _new_fault & ~_old_fault
                        if _new_bits:
                            self.socketio.emit("bms_fault", {"fault": _new_fault})
                            # 2026-08-09: 故障告警推送(企业微信, 同故障去重)
                            # 2026-08-10: 异常不再静默吞掉, 打印便于排查"未收到推送"
                            try:
                                AlertPusher.notify_fault(_new_fault, payload)
                            except Exception as _e:
                                print("[ALERT] 故障推送异常: %s" % _e)
                            self._last_fault_push_ts = time.time()
                        elif _new_fault and (time.time() - self._last_fault_push_ts) > 1800:
                            # 2026-08-09 修复: 持续故障周期提醒(30 分钟)——
                            #   原逻辑 `_new_bits` 仅"新出现故障位"才推, 持续预警(如低电量
                            #   0x20000)永不重推, 用户"有预警却没推送". 改为: 故障持续存在
                            #   且距上次推送 >30min 时, 重置同故障去重后重新推送提醒.
                            # 2026-08-10 修复: 按危害等级差异化周期——严重/保护级(fatal/error)
                            #   每 10 分钟重推, 预警级(warn)保持 30 分钟. 用户反馈"严重告警
                            #   也没推"实际是 30min 周期太长 + 首次推送后长时间无提醒.
                            try:
                                _lv = parse_faults(_new_fault)[0][1] if _new_fault else "warn"
                            except Exception:
                                _lv = "warn"
                            _period = 600 if _lv in ("error", "fatal") else 1800
                            if (time.time() - self._last_fault_push_ts) > _period:
                                try:
                                    AlertPusher.reset_fault_dedup()   # 清 10min 同故障去重
                                    AlertPusher.notify_fault(_new_fault, payload)
                                except Exception as _e:
                                    print("[ALERT] 周期故障推送异常: %s" % _e)
                                self._last_fault_push_ts = time.time()
                        # 2026-08-09: 本地联动规则引擎(低电量/过压/过温自动下发保护命令)
                        try:
                            RuleEngine_.evaluate(payload, self, AlertPusher)
                        except Exception:
                            pass
                        self.socketio.emit("mqtt_status", {
                            "connected": True,
                            "cloud_accessible": self.cloud_accessible,
                            "no_device": False,
                            "device_status": "ONLINE", "device_online": True,
                            "next_query_in": sleep_for,
                            "poll_mode": "night" if is_night else "online",
                            "region": self.region,
                            "db_count": db.query_count() if hasattr(db, "query_count") else 0,
                        })
                    except Exception:
                        pass

                # 2026-08-11: 补传结束通知——本轮收到的是"新于已见最新"的实时帧,
                #   说明离线缓存补传已告一段落, 通知前端刷新历史段(断网时段已回填)
                if self._replay_seen:
                    self._replay_seen = False
                    try:
                        self.socketio.emit("history_updated", {"ts": int(time.time() * 1000)})
                        print("[IoTDA] 补传结束, 已通知前端刷新历史段")
                    except Exception:
                        pass

                # 2026-08-07: 故障存在时提速轮询 → 告警/恢复尽快上屏(10s 快扫)
                try:
                    _cur_fault = int(payload.get("fault") or 0) if payload else 0
                except Exception:
                    _cur_fault = 0
                if _cur_fault and sleep_for > INTERVAL_FAULT_ACTIVE:
                    sleep_for = INTERVAL_FAULT_ACTIVE
                    if is_night:
                        sleep_for *= NIGHT_INTERVAL_MULT

                self._poll_pending_commands()
                self._last_next_query = now_time + sleep_for

                # ===== 2026-08-09 多设备影子轮询(与主设备共用轮询周期) =====
                #   轻量轮询: 遍历已注册的其他设备, 查影子 + 在线状态, 存入
                #   device_shadows/device_online_map(供 /api/devices 多设备展示与
                #   /api/status?device= 按设备查询). 仅主设备走 DB 落库/告警推送.
                self._poll_other_devices()
            except Exception as e:
                print("[IoTDA] 轮询异常: %s" % e)
                traceback.print_exc()
                # 异常时视为后端与云的连接失效, 但保留设备上次在线状态 (避免闪烁)
                self.cloud_accessible = False
                self.connected = False
                consecutive_failures += 1   # 2026-08-10: 异常同样计入连续失败
                sleep_for = INTERVAL_ONLINE

            # 2026-08-10 工业标准: 连续失败指数退避(10s→20s→40s→60s 封顶)
            #   避免云端/网络故障期间 10s 高频重试打日志; 成功一次即复位(见上方代码)
            if consecutive_failures > 0:
                sleep_for = min(60, INTERVAL_ONLINE * (2 ** (consecutive_failures - 1)))

            time.sleep(max(0.1, sleep_for))

    # ---------- 2026-08-09 多设备: 轻量轮询其他设备 ----------
    def _poll_other_devices(self):
        """轮询主设备之外的其他设备影子与在线状态, 存入 device_shadows/device_online_map.
        设备 ID 由 app.py 在启动时调用 register_device() 注入."""
        for dev_id in list(getattr(self, "_extra_device_ids", []) or []):
            try:
                shadow = self._fetch_shadow(dev_id)
                status = self._fetch_device_status(dev_id)
                online = bool(status.get("online")) if status else False
                payload = None
                if shadow:
                    try:
                        payload = self._shadow_to_payload(shadow)
                    except Exception:
                        payload = None
                self.device_shadows[dev_id] = payload
                self.device_online_map[dev_id] = online
            except Exception:
                pass

    def register_device(self, device_id):
        """2026-08-09: 注册一台从设备(如 BMS002/BMS003), 加入多设备轮询列表
        主设备(device_id)自动在列; 重复注册去重"""
        extra = list(getattr(self, "_extra_device_ids", []) or [])
        if device_id and device_id != self.device_id and device_id not in extra:
            extra.append(device_id)
        self._extra_device_ids = extra
        self.device_shadows.setdefault(device_id, None)
        self.device_online_map.setdefault(device_id, False)

    # ---------- 命令: 查询状态 + 下发 ----------
    def _poll_pending_commands(self):
        now = time.time()
        finished = []
        for cmd_id, info in list(self._pending_cmds.items()):
            if now - info["time"] > 30:
                try:
                    self.socketio.emit("cmd_resp", {
                        "command_name": info["name"],
                        "result": "timeout",
                        "command_id": cmd_id,
                    })
                except Exception:
                    pass
                finished.append(cmd_id)
                continue

            path = "/v5/iot/%s/device-commands/%s" % (self.project_id, cmd_id)
            resp = self._call("GET", path)
            if not isinstance(resp, dict):
                continue
            status = resp.get("status", "PENDING")
            if status in ("SUCCESSFUL", "FAILED", "TIMEOUT", "CANCELED"):
                resp_obj = {
                    "command_name": info["name"],
                    "command_id": cmd_id,
                    "status": status,
                    "result": resp.get("response"),
                }
                result = resp_obj.get("result")
                if isinstance(result, dict) and "result_code" in result:
                    resp_obj["ok"] = result["result_code"] == 0
                else:
                    resp_obj["ok"] = status == "SUCCESSFUL"
                try:
                    self.socketio.emit("cmd_resp", resp_obj)
                except Exception:
                    pass
                finished.append(cmd_id)
        for cmd_id in finished:
            self._pending_cmds.pop(cmd_id, None)

    def publish_cmd(self, cmd_dict):
        """下发命令到设备,返回是否成功下发
        Bug7 修复: 改用 /devices/{device_id}/messages API(设备消息下发)
        原因: /commands API 要求命令名在产品模型中定义,否则返回 IOTDA.014108
              /messages API 不校验产品模型, 直接通过 MQTT topic 下发 JSON 消息
        ESP32 端需订阅 $oc/devices/{id}/sys/messages/down topic 接收消息"""
        name = cmd_dict.get("command_name", cmd_dict.get("cmd", "unknown"))
        paras = cmd_dict.get("paras", cmd_dict.get("args", {}))
        if not isinstance(paras, dict):
            paras = {}

        # 2026-08-09: 主动重启/OTA 命令下发后, 设备将短暂断线重连
        #   设置离线推送抑制窗口(默认 120s), 期间不推送"设备离线"告警
        if name in ("restart", "reboot", "ota_upgrade", "ota_check"):
            self._suppress_offline_until = time.time() + 120
            print("[IoTDA] 已下发 %s, 未来 120s 抑制离线推送告警" % name)

        # 构造消息内容: {cmd: "set_charge", paras: {...}}
        # ESP32 端在 messages/down topic 解析此格式
        message_content = json.dumps({
            "cmd": name,
            "paras": paras,
            "timestamp": int(time.time() * 1000),
        }, ensure_ascii=False)

        # 华为云 IoTDA 设备消息下发 API (不校验产品模型)
        body = {
            "device_id": self.device_id,
            "message": {
                "content": message_content,
            },
        }
        path = "/v5/iot/%s/devices/%s/messages" % (self.project_id, self.device_id)
        resp = self._call("POST", path, body=body)
        msg_id = None
        if isinstance(resp, dict):
            msg_id = resp.get("message_id")
            if not msg_id:
                err_code = resp.get("error_code") or resp.get("code") or "UNKNOWN"
                err_msg = resp.get("error_msg") or resp.get("message") or "无响应"
                print("[IoTDA] 消息下发失败 %s: code=%s msg=%s" % (name, err_code, err_msg))
                # 透传错误详情给调用方, 区分设备离线/鉴权失败/额度超限等
                return {"ok": False, "code": err_code, "msg": err_msg}
        else:
            print("[IoTDA] 消息下发失败 %s: HTTP 无响应或设备离线" % name)
            return {"ok": False, "code": "NO_RESP", "msg": "HTTP 无响应或设备离线"}
        if msg_id:
            # /messages API 为异步消息, 无状态查询机制, 不加入 _pending_cmds
            # 避免 _poll_pending_commands 对每个命令发起无效 GET /device-commands 查询
            # 设备执行结果通过下次属性上报体现, 前端已通过 HTTP 响应获知下发状态
            # 2026-08-09 提示: msg_id 仅代表"华为云已接收", 不代表"设备已执行"——
            #   设备执行结果看下次上报(cell_series_num/cells 长度等); 若设备持续无变化,
            #   需排查设备端固件是否含 messages/down 订阅处理(烧录最新固件).
            print("[IoTDA] 已下发消息 %s, message_id=%s (异步: 设备执行结果以下次上报为准)" % (name, msg_id))
            return {"ok": True, "msg_id": msg_id, "async": True}
        return {"ok": False, "code": "UNKNOWN", "msg": "未知失败"}

    # ---------- 生命周期 ----------
    @property
    def _client(self):
        """兼容 app.py 中对 iotda_client._client is not None 的判定"""
        return object() if self.connected or self._session is not None else None

    def start(self):
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        print("[IoTDA] 客户端已启动")

    def stop(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=2)
        try:
            self._session.close()
        except Exception:
            pass
        print("[IoTDA] 客户端已停止")
