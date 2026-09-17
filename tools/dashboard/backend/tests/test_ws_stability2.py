# -*- coding: utf-8 -*-
"""WebSocket 稳定性详细测试: 记录每次断开/重连时刻, 观察 60 秒"""
import sys, time, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
import socketio

URL = "http://localhost:5000"
DURATION = 60
t0 = time.time()
events = []

sio = socketio.Client(reconnection=True, reconnection_attempts=0)

@sio.event
def connect():
    events.append("+%.1fs 连接" % (time.time() - t0))

@sio.event
def disconnect():
    events.append("+%.1fs 断开" % (time.time() - t0))

try:
    sio.connect(URL, transports=["websocket"])
    events.append("+%.1fs 开始观察 %ds" % (time.time() - t0, DURATION))
    time.sleep(DURATION)
    sio.disconnect()
except Exception as e:
    events.append("异常: %s" % e)

print("\n".join(events))
disc = sum(1 for e in events if "断开" in e)
print("==> 60秒内断开 %d 次: %s" % (disc, "PASS 稳定" if disc == 0 else "FAIL 仍有断线"))
sys.exit(0 if disc == 0 else 1)
