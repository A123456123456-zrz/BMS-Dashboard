#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
模拟 ESP32 BMS 设备 → 华为云 IoTDA 属性上报链路测试
========================================================
验证: 设备鉴权(HMAC-SHA256 动态密码) + 物模型属性上报 + 影子更新 通不通。

与真实固件行为对齐(见 components/bms_middleware/sys_mqtt.c):
  - 接入点:  BMS_MQTT_URI        (默认 mqtts://...iotda-device.cn-south-4.myhuaweicloud.com:8883)
  - 设备ID:  BMS_MQTT_CLIENT_ID  (默认 <IOTDA_DEVICE_ID>_BMS001)
  - 设备密钥: BMS_MQTT_DEVICE_SECRET (默认与 bms_config.h 一致)
  - ClientID = {device_id}_0_0_{YYYYMMDDHH}   (身份类型0, 签名类型0=不校验时间戳)
  - Username = {device_id}
  - Password = HMAC-SHA256(key=时间戳, msg=设备密钥) → 64 位 hex 小写
  - 上报 topic: $oc/devices/{device_id}/sys/properties/report
  - payload:   {"services":[{"service_id":"BMS","properties":{...}}]}

用法:
  python simulate_iotda_device.py                 # 默认宏值, 连续上报(10 轮/10s 间隔)
  python simulate_iotda_device.py --once          # 只上报一轮
  python simulate_iotda_device.py --uri mqtts://... --client-id XXX --secret YYY
  python simulate_iotda_device.py --count 1 --interval 0
  python simulate_iotda_device.py --no-tls        # 若平台开放 1883 明文端口

输出: 每步打印 PASS/FAIL, 最后一行总结链路通不通。
"""
import argparse
import datetime
import hashlib
import hmac
import json
import re
import sys
import time

import paho.mqtt.client as mqtt

try:
    import certifi
except ImportError:
    certifi = None

# 默认值与固件 bms_config.h 保持同步(可用 --uri/--client-id/--secret 覆盖)
DEFAULT_URI = "mqtts://6a3ff62aab.iotda-device.cn-south-4.myhuaweicloud.com:8883"
DEFAULT_CLIENT_ID = "<IOTDA_DEVICE_ID>_BMS001"
DEFAULT_SECRET = "<IOTDA_DEVICE_SECRET>"


# ---------------------------------------------------------------
# 物模型属性模板(BMS.json, property_id 与华为云控制台一致)
# ---------------------------------------------------------------
def build_properties(series=6):
    """构造一轮合理的模拟上报属性(物模型定义的全部 R 属性 + 动态数组)。"""
    cells = [4100 + i * 3 for i in range(series)]          # 每串电压 mV
    temps = [350 + i for i in range(series)]               # 每串温度 0.1℃
    return {
        "soc": 82.5,                # decimal %
        "soh": 95.2,
        "v_max": max(cells),
        "v_min": min(cells),
        "pack_v": sum(cells),       # int mV
        "current": -1250,           # mA (负=充电)
        "temp_max": max(temps),
        "temp_min": min(temps),
        "fault": 0,                 # 位掩码
        "cells": json.dumps(cells),  # string: JSON 数组字符串(与固件一致)
        "balance_mask": 0,
        "cycle_count": 12,
        "timestamp": int(time.time() * 1000),
        "charge_mos": 0,
        "discharge_mos": 1,
        "balance_on": 0,
        "charge_mode": 1,
        "insulation_rp": 1200000,
        "insulation_rn": 1150000,
        "insulation_ohm_per_v": 25000,
        "crc": 0,                   # 模拟脚本不做 CRC, 平台不校验
        "crc32": 0,
        "commStatus": 3,
    }


def parse_uri(uri):
    """mqtts://host:port → (host, port, use_tls)"""
    m = re.match(r"^(mqtts?)://([^:/]+)(?::(\d+))?", uri)
    if not m:
        raise ValueError("无法解析 MQTT URI: %s" % uri)
    scheme, host, port = m.group(1), m.group(2), m.group(3)
    use_tls = scheme == "mqtts"
    port = int(port) if port else (8883 if use_tls else 1883)
    return host, port, use_tls


def iotda_password(device_secret, ts):
    """华为云 IoTDA 动态密码: HMAC-SHA256(key=时间戳, msg=设备密钥) hex 小写
    官方规范与固件 sys_mqtt.c 一致: key=YYYYMMDDHH, msg=device_secret"""
    return hmac.new(ts.encode(), device_secret.encode(), hashlib.sha256).hexdigest()


class SimDevice:
    def __init__(self, uri, device_id, secret, count, interval, verbose=False):
        self.device_id = device_id
        self.secret = secret
        self.count = count
        self.interval = interval
        self.verbose = verbose
        self.host, self.port, self.use_tls = parse_uri(uri)
        self.client = None
        self.connected = False
        self.published = 0
        self.commands = []

    # ---- paho 回调(VERSION2 兼容) ----
    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        rc = reason_code if isinstance(reason_code, int) else (reason_code.value if hasattr(reason_code, "value") else reason_code)
        self.connected = (rc == 0)
        if self.connected:
            print("[PASS] MQTT 连接成功 (CONNACK rc=0), 设备鉴权通过")
        else:
            print("[FAIL] 连接被拒绝 rc=%s (1=协议错误 2=标识符被拒 3=服务器不可达 4/5=鉴权失败)" % rc)

    def _on_disconnect(self, client, userdata, flags, reason_code=None, properties=None):
        rc = reason_code if isinstance(reason_code, int) else (reason_code.value if hasattr(reason_code, "value") else reason_code)
        print("[INFO] 断开连接 rc=%s" % rc)

    def _on_publish(self, client, userdata, mid, reason_code=None, properties=None):
        self.published += 1

    def _on_message(self, client, userdata, msg):
        text = msg.payload.decode("utf-8", "replace")
        self.commands.append((msg.topic, text))
        print("[INFO] 收到下行命令: %s → %s" % (msg.topic, text[:200]))

    # ---- 主流程 ----
    def run(self):
        ts = datetime.datetime.utcnow().strftime("%Y%m%d%H")
        client_id = "%s_0_0_%s" % (self.device_id, ts)
        username = self.device_id
        password = iotda_password(self.secret, ts)

        print("=" * 70)
        print(" 模拟设备上报华为云 IoTDA")
        print("  接入点   : %s://%s:%d" % ("mqtts" if self.use_tls else "mqtt", self.host, self.port))
        print("  设备ID   : %s" % self.device_id)
        print("  ClientID : %s" % client_id)
        print("  Username : %s" % username)
        print("  Password : %s..." % password[:16])
        print("  上报轮数 : %d  间隔: %ds" % (self.count, self.interval))
        print("=" * 70)

        # 建连
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
        self.client.username_pw_set(username, password)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_publish = self._on_publish
        self.client.on_message = self._on_message

        if self.use_tls:
            ca = certifi.where() if certifi else None
            if self.verbose:
                print("[INFO] 使用 TLS, CA=%s" % ca)
            try:
                if ca:
                    self.client.tls_set(ca_certs=ca)
                else:
                    self.client.tls_set()  # 系统默认 CA
            except Exception as e:
                print("[FAIL] TLS 配置失败: %s (可尝试 --no-tls 或安装 certifi)" % e)
                return 1

        # 订阅命令下发(验证下行链路, 非必须)
        self.client.subscribe("$oc/devices/%s/sys/commands/#" % self.device_id, qos=1)

        try:
            self.client.connect(self.host, self.port, keepalive=60)
        except Exception as e:
            print("[FAIL] TCP/TLS 连接异常: %s" % e)
            return 1
        self.client.loop_start()

        # 等连接结果(最多 15s)
        deadline = time.time() + 15
        while time.time() < deadline and not self.connected:
            time.sleep(0.1)
        if not self.connected:
            print("[FAIL] 15s 内未建立连接(鉴权失败或平台不可达)")
            return 1

        # 上报
        topic = "$oc/devices/%s/sys/properties/report" % self.device_id
        for i in range(self.count):
            props = build_properties(series=6)
            payload = {"services": [{"service_id": "BMS", "properties": props}]}
            body = json.dumps(payload, ensure_ascii=False)
            info = self.client.publish(topic, body, qos=1)
            if self.verbose:
                print("[INFO] 上报 #%d: %s" % (i + 1, body[:160]))
            if i < self.count - 1 and self.interval > 0:
                time.sleep(self.interval)

        # 短暂等待 PUBACK + 下行命令
        time.sleep(2.0)
        self.client.loop_stop()
        self.client.disconnect()

        print("=" * 70)
        if self.connected and self.published >= self.count:
            print("[PASS] 链路畅通: 鉴权通过 + 属性上报 %d 条全部发出 (paho 确认 mid 回调 %d 次)"
                  % (self.count, self.published))
            print("  → 到华为云 IoTDA 控制台「设备详情 → 设备影子 / 消息跟踪」应能看到本轮数据")
            if self.commands:
                print("[INFO] 下行链路也通: 收到 %d 条命令" % len(self.commands))
            else:
                print("[INFO] 未收到下行命令(正常, 无人下发)")
            return 0
        print("[FAIL] 链路异常: connected=%s published=%d/%d" % (self.connected, self.published, self.count))
        return 1


def main():
    ap = argparse.ArgumentParser(description="模拟 BMS 设备上报华为云 IoTDA(链路连通性测试)")
    ap.add_argument("--uri", default=DEFAULT_URI, help="MQTT 接入点(默认同 bms_config.h)")
    ap.add_argument("--client-id", default=DEFAULT_CLIENT_ID, help="设备ID(默认同 bms_config.h)")
    ap.add_argument("--secret", default=DEFAULT_SECRET, help="设备密钥(默认同 bms_config.h)")
    ap.add_argument("--count", type=int, default=10, help="上报轮数(默认 10)")
    ap.add_argument("--interval", type=int, default=10, help="轮间间隔秒(默认 10)")
    ap.add_argument("--once", action="store_true", help="只上报一轮(= --count 1)")
    ap.add_argument("--no-tls", action="store_true", help="强制明文(平台若开放 1883)")
    ap.add_argument("-v", "--verbose", action="store_true", help="打印每轮上报内容")
    args = ap.parse_args()

    uri = args.uri
    if args.no_tls and uri.startswith("mqtts://"):
        uri = "mqtt://" + uri[len("mqtts://"):]

    dev = SimDevice(uri, args.client_id, args.secret,
                    count=1 if args.once else args.count,
                    interval=args.interval, verbose=args.verbose)
    sys.exit(dev.run())


if __name__ == "__main__":
    main()
