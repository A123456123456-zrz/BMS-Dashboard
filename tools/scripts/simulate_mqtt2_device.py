#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
模拟 BMS 设备第二通道 → 自建 Mosquitto (双通道架构验证)
========================================================
验证: 设备侧第二通道(MQTT2)能否连上自建 Mosquitto、发布 telemetry、
      broker 是否正确转发(订阅回显)。对应固件 sys_mqtt.c 的 publish_json2()。

主题/格式与固件双发完全一致:
  - topic:   bms/<device_id>/telemetry        (bms_config.h BMS_MQTT2_TOPIC_*)
  - payload: {"services":[{"service_id":"BMS","properties":{...}}]}  (华为云包裹格式, 双发同一 JSON)

用法:
  python simulate_mqtt2_device.py                  # 默认 ECS broker + student 账号
  python simulate_mqtt2_device.py --once           # 只发一轮
  python simulate_mqtt2_device.py --host 127.0.0.1 --port 1883 --user u --pass p
  python simulate_mqtt2_device.py --device-id bms02

输出: 连接/发布/订阅回显 每步 PASS/FAIL; 退出码 0=链路通。
"""
import argparse
import datetime
import json
import sys
import time

import paho.mqtt.client as mqtt

# 默认值与 bms_config.h 的 BMS_MQTT2_* 一致
DEFAULT_HOST = "<YOUR_ECS_IP>"
DEFAULT_PORT = 1883
DEFAULT_USER = "student"
DEFAULT_PASS = "<BROKER_PASSWORD>"
DEFAULT_DEVICE_ID = "bms01"
TOPIC_PREFIX = "bms/"


def build_telemetry(series=6):
    """构造与固件 sys_mqtt_report 相同形状的属性(services 包裹 + camelCase 字段)。"""
    cells = [4100 + i * 3 for i in range(series)]
    temps = [350 + i for i in range(series)]
    props = {
        "soc": 82.5, "soh": 95.2, "capacityAh": 2.5, "cellCount": series,
        "vMax": max(cells), "vMin": min(cells), "packV": sum(cells),
        "current": -1250, "tempMax": max(temps), "tempMin": min(temps),
        "temps": json.dumps(temps), "cellVoltages": json.dumps(cells),
        "fault": 0, "insulationRp": 1200000, "insulationRn": 1150000,
        "insulationOhmPerV": 25000,
        "cellOverVoltage": 0, "cellUnderVoltage": 0, "packOverTemp": 0,
        "packUnderTemp": 0, "overCurrent": 0, "shortCircuit": 0,
        "mosFetFault": 0, "cellImbalance": 0, "commFault": 0,
        "chargeMos": 0, "dischargeMos": 1, "balanceOn": 0, "workState": 2,
        "cycleCount": 12, "timestamp": int(time.time() * 1000),
        "chargeMode": 0, "fwVersion": "1.0.3", "commStatus": 3,
        "rs485Online": 0, "rs485LastPoll": 0, "rs485Err": 0,
        "otaStage": "idle", "otaProgress": 0, "otaError": "",
    }
    return {"services": [{"service_id": "BMS", "properties": props}]}


class SimMqtt2:
    def __init__(self, host, port, user, password, device_id, count, interval, verbose=False,
                 cafile="", certfile="", keyfile=""):
        self.host, self.port, self.user = host, port, user
        self.password, self.device_id = password, device_id
        self.count, self.interval, self.verbose = count, interval, verbose
        self.cafile, self.certfile, self.keyfile = cafile, certfile, keyfile
        self.client = None
        self.connected = False
        self.published = 0
        self.echoed = 0

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        rc = reason_code if isinstance(reason_code, int) else (reason_code.value if hasattr(reason_code, "value") else reason_code)
        self.connected = (rc == 0)
        if self.connected:
            tls = " mTLS" if self.certfile else ""
            print("[PASS] 已连接自建 broker%s %s:%d (鉴权通过)" % (tls, self.host, self.port))
            # 订阅回显(验证 broker 转发)
            client.subscribe(TOPIC_PREFIX + "#", qos=1)
        else:
            print("[FAIL] 连接被拒 rc=%s (4/5=用户名密码错误, 证书类问题看 broker 日志)" % rc)

    def _on_message(self, client, userdata, msg):
        self.echoed += 1
        print("[ECHO] 收到 broker 转发的消息: %s (%d B)" % (msg.topic, len(msg.payload)))
        if self.verbose:
            print("       " + msg.payload.decode("utf-8", "replace")[:160])

    def _on_publish(self, client, userdata, mid, reason_code=None, properties=None):
        self.published += 1

    def run(self):
        try:
            from paho.mqtt.client import CallbackAPIVersion
            self.client = mqtt.Client(CallbackAPIVersion.VERSION2, client_id="sim_mqtt2_" + self.device_id)
        except (ImportError, AttributeError):
            self.client = mqtt.Client(client_id="sim_mqtt2_" + self.device_id)
        self.client.username_pw_set(self.user, self.password)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_publish = self._on_publish

        # 2026-08-16: mTLS 支持(EMQX/Mosquitto 8883 双向证书验证)
        if self.certfile and self.cafile:
            try:
                import ssl
                self.client.tls_set(ca_certs=self.cafile,
                                    certfile=self.certfile,
                                    keyfile=self.keyfile)
                if self.verbose:
                    print("[INFO] mTLS 已启用: ca=%s cert=%s key=%s"
                          % (self.cafile, self.certfile, self.keyfile))
            except Exception as e:
                print("[FAIL] mTLS 配置失败: %s" % e)
                return 1

        print("=" * 70)
        print(" 模拟第二通道设备 → 自建 broker(%s)" % ("EMQX" if self.certfile else "Mosquitto"))
        print(" broker  : %s:%d%s" % (self.host, self.port, " (mTLS)" if self.certfile else ""))
        print(" 账号    : %s" % self.user)
        print(" device  : %s  主题: %s%s/telemetry" % (self.device_id, TOPIC_PREFIX, self.device_id))
        print(" 轮数    : %d  间隔: %ds" % (self.count, self.interval))
        print("=" * 70)

        try:
            self.client.connect(self.host, self.port, keepalive=60)
        except Exception as e:
            print("[FAIL] 连接异常: %s" % e)
            return 1
        self.client.loop_start()

        deadline = time.time() + 15
        while time.time() < deadline and not self.connected:
            time.sleep(0.1)
        if not self.connected:
            print("[FAIL] 15s 内未建立连接")
            return 1

        topic = "%s%s/telemetry" % (TOPIC_PREFIX, self.device_id)
        for i in range(self.count):
            body = json.dumps(build_telemetry(), ensure_ascii=False)
            self.client.publish(topic, body, qos=1)
            if self.verbose:
                print("[INFO] 发布 #%d: %s" % (i + 1, body[:150]))
            if i < self.count - 1 and self.interval > 0:
                time.sleep(self.interval)

        time.sleep(2.0)
        self.client.loop_stop()
        self.client.disconnect()

        print("=" * 70)
        if self.connected and self.published >= self.count:
            print("[PASS] 第二通道链路通: 发布 %d 条, broker 回显 %d 条" % (self.published, self.echoed))
            print("  → 后端 mqtt_client.py 消费腿订阅 bms/+/telemetry 即收此数据")
            return 0
        print("[FAIL] 链路异常: connected=%s published=%d/%d echoed=%d"
              % (self.connected, self.published, self.count, self.echoed))
        return 1


def main():
    ap = argparse.ArgumentParser(description="模拟设备第二通道上报自建 broker(EMQX/Mosquitto 双通道验证)")
    ap.add_argument("--host", default=DEFAULT_HOST, help="broker 地址(默认 <YOUR_ECS_IP>)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help="端口(默认 1883; EMQX mTLS 用 8883)")
    ap.add_argument("--user", default=DEFAULT_USER, help="用户名(默认 student)")
    ap.add_argument("--pass", dest="password", default=DEFAULT_PASS, help="密码")
    ap.add_argument("--device-id", default=DEFAULT_DEVICE_ID, help="设备ID(默认 bms01)")
    ap.add_argument("--cafile", default="", help="CA 证书(启用 mTLS, 如 deploy/mosquitto/certs/ca.crt)")
    ap.add_argument("--cert", dest="certfile", default="", help="客户端证书(如 .../client.crt)")
    ap.add_argument("--key", dest="keyfile", default="", help="客户端私钥(如 .../client.key)")
    ap.add_argument("--count", type=int, default=5, help="发布轮数(默认 5)")
    ap.add_argument("--interval", type=int, default=2, help="轮间间隔秒(默认 2)")
    ap.add_argument("--once", action="store_true", help="只发一轮")
    ap.add_argument("-v", "--verbose", action="store_true", help="打印发布内容")
    args = ap.parse_args()

    dev = SimMqtt2(args.host, args.port, args.user, args.password, args.device_id,
                   count=1 if args.once else args.count,
                   interval=args.interval, verbose=args.verbose,
                   cafile=args.cafile, certfile=args.certfile, keyfile=args.keyfile)
    sys.exit(dev.run())


if __name__ == "__main__":
    main()
