# -*- coding: utf-8 -*-
"""WebSocket 稳定性测试 v3: 主动断开不计入失败(避免 disconnect() 触发事件误判)"""
import sys, time, io, threading
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
import socketio

URL = "http://localhost:5000"
DURATION = 70
t0 = time.time()
unexpected_disc = []      # 观察期内非主动断开的记录
saw_connect = False
_stop = threading.Event()

sio = socketio.Client(reconnection=True, reconnection_attempts=0)

@sio.event
def connect():
    global saw_connect
    saw_connect = True
    print("+%.1fs 连接成功" % (time.time() - t0))

@sio.event
def disconnect():
    # 主动调用 disconnect() 时也会触发本事件; 用标志区分
    if not _stop.is_set():
        unexpected_disc.append(time.time() - t0)
        print("+%.1fs ⚠ 意外断开" % (time.time() - t0))

try:
    sio.connect(URL, transports=["websocket"])
    print("观察 %d 秒..." % DURATION)
    time.sleep(DURATION)
    _stop.set()          # 先置标志, 再主动断开(不计入失败)
    sio.disconnect()
except Exception as e:
    print("异常:", e)
    sys.exit(2)

print("连接成功=%s | 意外断开 %d 次: %s" % (saw_connect, len(unexpected_disc), unexpected_disc or "无"))
if saw_connect and not unexpected_disc:
    print("==> PASS: WebSocket 连接稳定")
    sys.exit(0)
else:
    print("==> FAIL")
    sys.exit(1)
