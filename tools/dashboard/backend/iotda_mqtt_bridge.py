#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""iotda_mqtt_bridge.py - 独立桥接进程（新增文件，不修改任何现有代码）

架构（链路保持不变）:
    [ESP32] --MQTT--> [华为云 IoTDA] --(本桥接进程)--> [MQTT broker bms 主题]
                                             （设备->IoTDA 原链路零改动）
    现有仪表盘仍直接从 IoTDA 取数；本进程只是把同一份数据旁路转发到自建 broker，
    供手机 App / 其他系统 / 测试消费。固件与现有仪表盘代码完全不动。

复用 iotda_client.IoTDAClient 的 _fetch_shadow / _shadow_to_payload（已验证的
华为云签名 + 数据解析逻辑），不实例化其轮询线程（socketio=None，不调用 _loop）。

运行（在 backend/ 目录下，与 iotda_client.py / dashboard.env 同目录）:
    pip install paho-mqtt requests
    python iotda_mqtt_bridge.py        # 自动读取同目录 dashboard.env 注入 BMS_IOTDA_* 凭证
Windows 用户直接双击 run_bridge.bat 即可(自动建 venv + 装依赖)。

Broker 凭证可用环境变量覆盖（默认已填你的阿里云 broker <YOUR_ECS_IP>:1883 / student）:
    BMS_BRIDGE_MQTT_HOST / PORT / USER / PASS / TOPIC / INTERVAL
"""
import os
import time
import json
import sys
import traceback

import paho.mqtt.client as mqtt
import iotda_client


def _load_dashboard_env():
    """若同目录存在 dashboard.env(仪表盘本地配置), 将其注入为环境变量(仅当未设置时)。
    这样桥接可独立运行, 无需手动 export BMS_IOTDA_* 。"""
    try:
        here = os.path.dirname(os.path.abspath(__file__))
        env_path = os.path.join(here, "dashboard.env")
        if not os.path.isfile(env_path):
            return
        with open(env_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" not in line:
                    continue
                k, v = line.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip())
    except Exception as e:
        print("[bridge] 加载 dashboard.env 失败(忽略): %s" % e)


_load_dashboard_env()


# ---------- 华为云 IoTDA 凭证（与仪表盘共用同一套环境变量; 由 dashboard.env 注入） ----------
AK         = os.environ.get("BMS_IOTDA_AK", "")
SK         = os.environ.get("BMS_IOTDA_SK", "")
PROJECT_ID = os.environ.get("BMS_IOTDA_PROJECT", "")
REGION     = os.environ.get("BMS_IOTDA_REGION", "cn-south-4")
DEVICE_ID  = os.environ.get("BMS_IOTDA_DEVICE", "<IOTDA_DEVICE_ID>_BMS001")

# ---------- 目标 MQTT broker（默认直连你的阿里云 broker <YOUR_ECS_IP>:1883） ----------
MQTT_HOST = os.environ.get("BMS_BRIDGE_MQTT_HOST", "<YOUR_ECS_IP>")
MQTT_PORT = int(os.environ.get("BMS_BRIDGE_MQTT_PORT", "1883"))
MQTT_USER = os.environ.get("BMS_BRIDGE_MQTT_USER", "student")
MQTT_PASS = os.environ.get("BMS_BRIDGE_MQTT_PASS", "")  # 2026-09-17 安全: 凭证不入库, 由 dashboard.env 注入
TOPIC     = os.environ.get("BMS_BRIDGE_TOPIC", "bms")
INTERVAL  = int(os.environ.get("BMS_BRIDGE_INTERVAL", "10"))


def make_mqtt_client():
    """兼容 paho-mqtt 1.x / 2.x 的 Client 构造。"""
    try:
        from paho.mqtt.client import CallbackAPIVersion
        c = mqtt.Client(CallbackAPIVersion.VERSION2)   # MQTT v5, 避免 v1 弃用警告
    except (ImportError, AttributeError):
        c = mqtt.Client()
    c.username_pw_set(MQTT_USER, MQTT_PASS)
    return c


def main():
    if not (AK and SK and PROJECT_ID):
        print("[bridge] 错误: 缺少 BMS_IOTDA_AK / BMS_IOTDA_SK / BMS_IOTDA_PROJECT 环境变量",
              file=sys.stderr)
        sys.exit(1)

    # 复用已验证的 IoTDA 取数逻辑（不启动其轮询线程，socketio=None）
    client = iotda_client.IoTDAClient(
        ak=AK, sk=SK, project_id=PROJECT_ID,
        region=REGION, device_id=DEVICE_ID, socketio=None,
    )

    mc = make_mqtt_client()

    def on_connect(c, u, f, rc, *a):
        print("[bridge] MQTT 连接 rc=%s" % rc)

    def on_disconnect(c, u, rc, *a):
        print("[bridge] MQTT 断开 rc=%s，将自动重连" % rc)

    mc.on_connect = on_connect
    mc.on_disconnect = on_disconnect
    try:
        mc.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    except Exception as e:
        print("[bridge] 首次连接 broker 失败（稍后重试）: %s" % e)
    mc.loop_start()

    print("[bridge] 启动: 每 %ds 从 IoTDA 拉取 %s -> %s (broker %s:%d)"
          % (INTERVAL, DEVICE_ID, TOPIC, MQTT_HOST, MQTT_PORT))

    sub_topic = "bms/%s/data" % DEVICE_ID
    while True:
        try:
            if not mc.is_connected():
                try:
                    mc.reconnect()
                except Exception:
                    time.sleep(2)
                    continue

            # 2026-08-20 修复: 设备离线时必须发离线心跳, 而非旧影子数据.
            #   原实现 _fetch_shadow() 对离线设备仍返回影子中最后上报的属性(华为云影子
            #   保留旧值), bridge 持续把旧数据发布到 EMQX → 前端消费腿据此误判"在线"
            #   且持续显示旧数据(设备断电后网页仍"有数据"). 先查设备真实在线状态:
            dev_status = client._fetch_device_status()
            is_online = bool(dev_status and dev_status.get("online") is True)
            if not is_online:
                # 设备离线 / 华为云 API 不可达(保守判定): 发离线心跳, 供消费方转离线
                msg = json.dumps({
                    "device_id": DEVICE_ID,
                    "device_online": False,
                    "no_data": True,
                    "cloud_accessible": bool(dev_status is not None),
                    "ts": int(time.time() * 1000),
                }, ensure_ascii=False)
                mc.publish(TOPIC, msg, qos=1)
                mc.publish(sub_topic, msg, qos=1)
                print("[bridge] 设备离线, 已发布离线心跳 -> %s / %s" % (TOPIC, sub_topic))
                time.sleep(INTERVAL)
                continue

            shadow = client._fetch_shadow()
            if not shadow:
                print("[bridge] 暂无可用的设备影子")
                payload = None
            else:
                try:
                    payload = client._shadow_to_payload(shadow)
                except Exception as _e:
                    print("[bridge] _shadow_to_payload 异常(跳过本周期): %s" % _e)
                    traceback.print_exc()
                    payload = None

            if payload is None:
                # 设备在线但影子过期: 发最小心跳, 便于消费方判离线
                msg = json.dumps({
                    "device_id": DEVICE_ID,
                    "device_online": False,
                    "no_data": True,
                    "ts": int(time.time() * 1000),
                }, ensure_ascii=False)
            else:
                payload["device_id"] = DEVICE_ID
                payload["device_online"] = True
                payload["no_data"] = False
                payload["ts"] = int(time.time() * 1000)
                msg = json.dumps(payload, ensure_ascii=False)

            mc.publish(TOPIC, msg, qos=1)
            mc.publish(sub_topic, msg, qos=1)
            print("[bridge] 已发布 %d 字节 -> %s / %s" % (len(msg), TOPIC, sub_topic))
        except Exception as e:
            print("[bridge] 取数/发布异常（下个周期重试）: %s" % e)
        time.sleep(INTERVAL)


if __name__ == "__main__":
    main()
