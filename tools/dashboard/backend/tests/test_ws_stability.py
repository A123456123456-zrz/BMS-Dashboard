# -*- coding: utf-8 -*-
"""WebSocket 连接稳定性验证脚本(重启 Dashboard 后运行)
用 python-socketio 客户端模拟前端, 连续连接观察是否断线:
  - 连接成功: 打印 connect
  - 收到 bms_data 推送: 计数
  - 断线: 打印 disconnect + 原因
退出码: 0=稳定(无断线)  1=发生断线
"""
import sys, time, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
import socketio

URL = "http://localhost:5000"
DURATION = 25   # 观察秒数
disconnect_count = 0
data_count = 0

sio = socketio.Client(reconnection=True, reconnection_attempts=0)

@sio.event
def connect():
    print("[OK] 已连接 WebSocket")

@sio.event
def disconnect():
    global disconnect_count
    disconnect_count += 1
    print("[FAIL] 连接断开! (第 %d 次)" % disconnect_count)

@sio.on("bms_data")
def on_bms_data(data):
    global data_count
    data_count += 1

try:
    sio.connect(URL, transports=["websocket"])
    print("开始观察 %d 秒, 等待数据推送..." % DURATION)
    time.sleep(DURATION)
    sio.disconnect()
    print("观察结束: 收到推送 %d 条, 断开 %d 次" % (data_count, disconnect_count))
    if disconnect_count == 0:
        print("==> PASS: WebSocket 连接稳定")
        sys.exit(0)
    else:
        print("==> FAIL: 仍存在断线")
        sys.exit(1)
except Exception as e:
    print("连接异常:", e)
    sys.exit(2)
