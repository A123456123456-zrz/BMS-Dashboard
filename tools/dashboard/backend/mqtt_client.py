# -*- coding: utf-8 -*-
"""
MQTT 订阅线程模块（2026-08-16 生产化：双通道架构的"自建 Mosquitto 消费腿"）
负责连接自建 Mosquitto broker，订阅 BMS 五类协议主题，
将数据归一化后写入数据库并通过 SocketIO 推送给前端。

协议主题（与固件双发保持一致, 见文档《迁自建服务器实施步骤》3.1 节）:
  bms/<device_id>/telemetry   遥测(复用华为云 services 包裹格式, 双发同一 JSON)
  bms/<device_id>/event       事件/故障
  bms/<device_id>/status      在线状态(LWT retained)
  bms/<device_id>/cmd/ack     命令回执
兼容旧 EMQX 主题: bms/data、bms/fault、bms/cmd_resp、bms/info
"""
import json
import re
import threading
import time
import paho.mqtt.client as mqtt

import database as db
import dedup

# 2026-08-20 修复(#2 防闪): 离线判定新鲜度阈值(与前端 OFFLINE_MS 一致)。
#   设备采样: EMQX fast 帧 100ms / telemetry 7s; 断电后 EMQX keepalive 15s 内
#   检测断开并发 LWT 遗嘱。消费腿据此判断"EMQX 直连是否还有新鲜帧":
#   - 直连新鲜(<=OFFLINE_MS) = 设备在线 → 华为云侧离线心跳/retained 遗嘱属误报, 丢弃
#   - 直连过期(>OFFLINE_MS)  = 设备真离线 → 转发离线帧, 前端转离线
# 2026-08-21 实时性修复: 60s→20s→10s —— 用户要求 10 秒内判定离线:
#   EMQX 无数据开始计时, 10s 后仍无帧即判离线; 固件 EMQX 腿 keepalive=15s,
#   断电后 ~22.5s broker 发 LWT 遗嘱, 遗嘱到达时距最后帧已 >10s → 生效。
#   100ms fast 帧正常时 10s 无帧即确认断线, 误判风险可忽略。
OFFLINE_MS = 10 * 1000


class MqttClient:
    """MQTT 客户端封装(后台线程运行)"""

    def __init__(self, broker, port, topics, cmd_topic, socketio, client_id="bms_dashboard",
                 username="", password=""):
        """
        :param broker:    broker 地址
        :param port:      端口
        :param topics:    订阅主题列表
        :param cmd_topic: 命令下发主题
        :param socketio:  Flask-SocketIO 实例
        :param client_id: MQTT client id
        :param username:  broker 用户名(自建 Mosquitto 必填, 空则不启用认证)
        :param password:  broker 密码
        """
        self.broker = broker
        self.port = port
        self.topics = topics
        self.cmd_topic = cmd_topic
        self.socketio = socketio
        self.client_id = client_id

        # paho-mqtt 1.x / 2.x 兼容构造(2.x 要求 CallbackAPIVersion)
        try:
            from paho.mqtt.client import CallbackAPIVersion
            self._client = mqtt.Client(CallbackAPIVersion.VERSION2, client_id=client_id, clean_session=True)
        except (ImportError, AttributeError):
            self._client = mqtt.Client(client_id=client_id, clean_session=True)
        if username:
            self._client.username_pw_set(username, password)
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message
        self._thread = None
        self._running = False
        # 2026-08-18 离线判定增强: 记录 EMQX 直连消费腿状态与最近消息时间,
        #   供 /api/status 暴露给前端做"设备断电即时离线"判定(不依赖华为云 60s 影子)
        self.connected = False
        self.last_msg_ts = 0.0

    # ---------- 回调 ----------
    # 2026-08-16 修复: paho-mqtt 2.x (CallbackAPIVersion.VERSION2) 下 on_connect 回调为
    #   (client, userdata, flags, reason_code, properties) 6 参, 旧 1.x 写法 5 参会 TypeError,
    #   导致连接成功后订阅未执行(EMQX subscriptions=0). 兼容写法: 尾部加 properties=None.
    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        # paho 2.x 传 ReasonCode 对象(.value), 1.x 传 int —— 统一取 int
        rc = getattr(reason_code, "value", reason_code)
        if rc == 0:
            print("[MQTT] 已连接 broker %s:%d" % (self.broker, self.port))
            self.connected = True
            for t in self.topics:
                client.subscribe(t)
                print("[MQTT] 已订阅: %s" % t)
            # 通知前端连接状态
            self.socketio.emit("mqtt_status", {"connected": True})
        else:
            print("[MQTT] 连接失败, rc=%d" % rc)
            self.connected = False
            self.socketio.emit("mqtt_status", {"connected": False, "rc": rc})

    def _on_disconnect(self, client, userdata, *args):
        # paho 2.x VERSION2: (client, userdata, disconnect_flags, reason_code, properties)
        # paho 1.x:         (client, userdata, rc)
        if len(args) >= 2:
            rc = getattr(args[1], "value", args[1])   # 2.x: reason_code 在 args[1]
        else:
            rc = args[0] if args else 0               # 1.x: rc 在 args[0]
        print("[MQTT] 断开连接, rc=%d" % rc)
        self.connected = False
        try:
            self.socketio.emit("mqtt_status", {"connected": False})
        except Exception:
            pass

    @staticmethod
    def _device_id_from_topic(topic):
        """从 bms/<device_id>/... 提取设备ID, 无则返回 None"""
        m = re.match(r"^bms/([^/]+)/(?:telemetry|event|status|cmd/ack|fast|data)$", topic)
        return m.group(1) if m else None

    def _on_message(self, client, userdata, msg):
        topic = msg.topic
        # 2026-08-18 离线判定增强: 设备帧到达刷新 last_msg_ts,
        #   /api/status 据此计算 EMQX 直连新鲜度(设备断电后 ~60s 内前端即可判离线)
        # 2026-08-20 修复: 离线心跳帧(device_online=False / no_data=True)不刷新
        #   last_msg_ts —— 否则桥接每 10s 发一次离线心跳, emqx_age_ms 恒小,
        #   前端永不判离线、持续显示"在线+有数据"。仅真实数据帧刷新新鲜度。
        try:
            _p = json.loads(msg.payload.decode("utf-8"))
        except Exception:
            _p = None
        _is_offline_hb = isinstance(_p, dict) and (
            _p.get("device_online") is False or _p.get("no_data") is True)
        # 2026-08-20 修复(#3 华为云影子): 桥接转发帧(bms/<id>/data, 华为云影子数据)
        #   不刷新 last_msg_ts —— 华为云影子在设备断电后 1~2min 内仍保留旧数据,
        #   桥接每 10s 持续转发, 会让 EMQX 直连新鲜度恒小, 前端永不判离线、
        #   断电后网页仍持续刷新数据。只有 EMQX 直连设备帧(telemetry/fast)才
        #   代表设备真实在线, 才刷新新鲜度。
        _is_bridge_frame = bool(re.match(r"^bms/[^/]+/data$", topic))
        if not _is_offline_hb and not _is_bridge_frame:
            self.last_msg_ts = time.time()
        # 2026-08-19 修复(B2): 顶层 try/except 兜底——paho loop_start 网络线程中
        #   回调抛异常会终止该线程, 消费腿静默失效且无告警无恢复(重启进程才能恢复).
        #   单条消息异常只记录, 绝不允许中断订阅循环.
        try:
            self._on_message_inner(msg, topic)
        except Exception as e:
            print("[MQTT] 消息处理异常(已忽略, 不影响订阅): %s" % e)

    def _on_message_inner(self, msg, topic):
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except Exception as e:
            print("[MQTT] JSON 解析失败 (%s): %s" % (topic, e))
            return

        # ---- 新协议(自建 Mosquitto 双通道, 2026-08-16) ----
        if re.match(r"^bms/[^/]+/telemetry$", topic):
            self._handle_telemetry(payload, self._device_id_from_topic(topic))
        elif re.match(r"^bms/[^/]+/fast$", topic):
            self._handle_fast(payload, self._device_id_from_topic(topic))
        elif re.match(r"^bms/[^/]+/event$", topic):
            self._handle_event(payload, self._device_id_from_topic(topic))
        elif re.match(r"^bms/[^/]+/status$", topic):
            self._handle_status(payload, self._device_id_from_topic(topic))
        elif re.match(r"^bms/[^/]+/cmd/ack$", topic):
            self._handle_cmd_ack(payload, self._device_id_from_topic(topic))
        # ---- 旧 EMQX 主题兼容(保留, 勿删) ----
        elif topic.endswith("/data"):
            self._handle_data(payload, topic)
        elif topic.endswith("/fault"):
            self._handle_fault(payload)
        elif topic.endswith("/cmd_resp"):
            self._handle_cmd_resp(payload)
        elif topic.endswith("/info"):
            self._handle_info(payload)
        else:
            print("[MQTT] 未知主题: %s" % topic)

    # ---------- 新协议处理(2026-08-16) ----------
    def _handle_telemetry(self, payload, device_id):
        """bms/<id>/telemetry: 归一化入库 + 推送前端(与 IoTDA 腿共用同一映射)"""
        from iotda_client import props_to_payload, extract_bms_props
        try:
            from iotda_client import mark_local_leg_active
        except ImportError:
            mark_local_leg_active = None
        normalized = None
        _props = extract_bms_props(payload)
        if _props:
            normalized = props_to_payload(_props)
        if normalized is None:
            # 已是平铺字典或空 → 原样(至少补 device_id)
            normalized = payload if isinstance(payload, dict) else {}
        if device_id:
            normalized.setdefault("device_id", device_id)
        # 双通道去重(零停机迁移储备): 华为桥接腿与直连腿内容一致时跳过
        if dedup.is_duplicate(device_id, normalized):
            print("[MQTT] 去重跳过(双发重复): %s" % device_id)
            return
        # 2026-08-16 主备反转: 本地腿(EMQX)为主 —— 刷新活跃时间, 华为云腿据此跳过重复实时推送
        if mark_local_leg_active is not None:
            try:
                mark_local_leg_active()
            except Exception:
                pass
        try:
            db.insert_data(normalized)
        except Exception as e:
            print("[MQTT] 入库失败(已忽略, 不影响订阅): %s" % e)
        # 2026-08-21 修复(#2 跳离线根因): 补权威在线字段——消费腿刚收到设备帧,
        #   设备必然在线。此前实时帧缺 device_online/emqx_age_ms, 前端只能用
        #   设备时钟 timestamp 算 ageMs(设备时钟与服务器偏差 10~30s 时误超
        #   OFFLINE_MS=20s → 每帧判离线、轮询拉回在线循环跳)。
        #   现由后端权威标注: 前端 apiOnline 直接为 true, 不再依赖设备时钟。
        if isinstance(normalized, dict):
            normalized.setdefault("device_online", True)
            normalized.setdefault("emqx_age_ms", 0)
        self.socketio.emit("bms_data", normalized)

    def _handle_fast(self, payload, device_id):
        """bms/<id>/fast: 100ms 高速精简帧 → 只喂前端实时曲线, 不写库

        2026-08-18 实时性整改(文档第 5.3 节): N 台设备 × 10 条/秒的高频帧若同步
        写库会拖垮 DB, 故快帧只 emit, 由 7s 全量帧负责落库/历史查询。
        注: 高频帧不逐帧打日志(100ms×N 台会淹没日志), 仅失败/降频计数时打印。
        """
        if device_id and isinstance(payload, dict):
            payload.setdefault("device_id", device_id)
        # 2026-08-21 修复(#2 跳离线根因): fast 帧同样补权威在线字段——
        #   前端 bms_fast 在线恢复逻辑据此秒级转在线, 不依赖设备时钟。
        if isinstance(payload, dict):
            payload.setdefault("device_online", True)
            payload.setdefault("emqx_age_ms", 0)
        # 高频帧不做双通道去重(去重只针对全量帧入库路径), 100ms 一帧去重窗口会误伤
        try:
            self.socketio.emit("bms_fast", payload)
        except Exception as e:
            print("[MQTT] fast 推送失败(已忽略): %s" % e)

    def _handle_event(self, payload, device_id):
        """bms/<id>/event: 事件/故障 → 推送前端"""
        print("[MQTT] 收到事件: %s" % payload)
        if device_id and isinstance(payload, dict):
            payload.setdefault("device_id", device_id)
        self.socketio.emit("bms_fault", payload)

    def _handle_status(self, payload, device_id):
        """bms/<id>/status: 在线状态(LWT retained) → 推送前端
        2026-08-20 修复(#2 断电第一时间离线): 固件 EMQX 通道已配 LWT 遗嘱,
        设备断电/断网时 broker 立即发布 {device_online:false,no_data:true} 到此主题。
        消费腿收到离线遗嘱时:
          - 把该设备在线标记置 False, 供 /api/status 的 emqx_age_ms 立即判离线
          - 原样 emit bms_status(前端监听到即秒级转离线, 不等 15s 轮询)
          - 并触发企微离线推送(notify_online_change)"""
        print("[MQTT] 收到状态: %s" % payload)
        if device_id and isinstance(payload, dict):
            payload.setdefault("device_id", device_id)
            if payload.get("device_online") is False or payload.get("no_data") is True:
                # 2026-08-20 修复(#2 防闪): 该离线遗嘱可能是 EMQX 上 retained 的旧遗嘱
                #   (固件 LWT retain=true, 设备每次重连订阅都会重新收到最后一次遗嘱,
                #   即使设备已恢复在线) 或桥接对华为云状态的误报。
                #   守卫: 仅当 EMQX 直连也确认无新帧(>OFFLINE_MS)时才当真——
                #   设备还在发真实帧(100ms fast/7s telemetry)即视为在线。
                if (time.time() - self.last_msg_ts) <= OFFLINE_MS:
                    print("[MQTT] 忽略离线遗嘱(EMQX 直连仍有新帧, 设备在线): %s" % device_id)
                    return
                # 离线遗嘱: 立即更新设备在线状态(消费腿侧), 前端/轮询即时可见
                self.device_online = False
                # 不刷新 last_msg_ts —— 让 emqx_age_ms 增长, /api/status 判离线
                try:
                    from alert_push import AlertPusher
                    AlertPusher.notify_online_change(
                        False, {"last_shadow": None}, force=True)
                except Exception as e:
                    print("[MQTT] 离线企微推送异常: %s" % e)
        self.socketio.emit("bms_status", payload)

    def _handle_cmd_ack(self, payload, device_id):
        """bms/<id>/cmd/ack: 命令回执(req_id 关联) → 推送前端"""
        print("[MQTT] 命令回执: %s" % payload)
        if device_id and isinstance(payload, dict):
            payload.setdefault("device_id", device_id)
        self.socketio.emit("cmd_resp", payload)

    # ---------- 旧主题处理(兼容保留) ----------
    def _handle_data(self, payload, topic=None):
        """处理 bms/data: 入库 + 推送前端"""
        # 2026-08-20 修复(#3 华为云影子): 桥接转发帧(bms/<id>/data, 华为云影子)。
        #   华为云影子在设备断电后 1~2min 内仍保留旧数据(设备详情 API 延迟),
        #   桥接每 10s 持续转发 → 若按原逻辑归一化/入库/emit, 前端会收到
        #   "在线+旧数据"帧: 断电后网页仍持续刷新、永不转离线。
        #   处理原则(前端实时数据只认 EMQX 直连 telemetry/fast):
        #   - 离线心跳帧(device_online=False / no_data=True): EMQX 直连仍新鲜
        #     = 华为云误报(设备在线) → 丢弃; EMQX 直连也过期 = 设备真离线 → 转发
        #   - 正常影子数据帧: 一律丢弃不推前端(避免断电后旧影子数据让前端"复活")
        _is_bridge = bool(topic and re.match(r"^bms/[^/]+/data$", topic))
        if _is_bridge:
            _is_hb = isinstance(payload, dict) and (
                payload.get("device_online") is False or payload.get("no_data") is True)
            if _is_hb:
                if (time.time() - self.last_msg_ts) <= OFFLINE_MS:
                    print("[MQTT] 忽略华为云侧离线心跳(EMQX 直连仍有新帧, 设备在线): %s"
                          % payload.get("device_id"))
                    return
                print("[MQTT] 收到离线心跳帧(设备离线): %s" % payload.get("device_id"))
                self.socketio.emit("bms_data", payload)
                return
            print("[MQTT] 忽略桥接影子数据帧(前端实时数据以 EMQX 直连为准): %s" % topic)
            return
        # 2026-08-11 修复: 固件 MQTT 直连上报为 camelCase + services 包裹, 必须归一化为
        #   snake_case 后再入库/广播, 否则与 IoTDA 推送同样掉 pack_v/v_max/temp_max/cells。
        #   复用 iotda_client.props_to_payload(与轮询路径同映射), 三方字段一致。
        from iotda_client import props_to_payload, extract_bms_props
        _props = extract_bms_props(payload)
        if _props:
            _normalized = props_to_payload(_props)
            if _normalized is not None:
                payload = _normalized
        # G5(2026-08-10): 入库异常单独兜底, 避免坏数据抛出后拖垮整个 MQTT 订阅线程
        #   (否则一条异常会中断 on_message 回调, 后续消息全丢)
        # 双通道去重(零停机迁移储备): 华为桥接腿(bms/<id>/data) 与直连腿重复内容跳过
        _did = payload.get("device_id") if isinstance(payload, dict) else None
        if dedup.is_duplicate(_did, payload):
            print("[MQTT] 去重跳过(双发重复): %s" % _did)
            return
        try:
            db.insert_data(payload)
        except Exception as e:
            print("[MQTT] 入库失败(已忽略, 不影响订阅): %s" % e)
        # 推送实时数据给前端
        self.socketio.emit("bms_data", payload)

    def _handle_fault(self, payload):
        """处理 bms/fault: 推送故障告警"""
        print("[MQTT] 收到故障: %s" % payload)
        self.socketio.emit("bms_fault", payload)

    def _handle_cmd_resp(self, payload):
        """处理 bms/cmd_resp: 推送命令响应"""
        print("[MQTT] 命令响应: %s" % payload)
        self.socketio.emit("cmd_resp", payload)

    def _handle_info(self, payload):
        """处理 bms/info: 推送设备信息"""
        print("[MQTT] 设备信息: %s" % payload)
        self.socketio.emit("bms_info", payload)

    # ---------- 发布命令 ----------
    def publish_cmd(self, cmd_dict):
        """向 bms/cmd 发布命令"""
        try:
            payload = json.dumps(cmd_dict)
            self._client.publish(self.cmd_topic, payload, qos=1)
            print("[MQTT] 已下发命令: %s" % payload)
            return True
        except Exception as e:
            print("[MQTT] 发布命令失败: %s" % e)
            return False

    # ---------- 生命周期 ----------
    def start(self):
        """启动 MQTT 客户端(非阻塞 loop_start)"""
        try:
            self._client.connect(self.broker, self.port, keepalive=60)
        except Exception as e:
            print("[MQTT] 连接异常: %s,将启用重连循环" % e)
        self._client.loop_start()
        self._running = True
        # 启动守护线程做断线重连检测
        self._thread = threading.Thread(target=self._watchdog, daemon=True)
        self._thread.start()
        print("[MQTT] 客户端已启动")

    def _watchdog(self):
        """看门狗: 检测连接状态,断线则重连"""
        while self._running:
            time.sleep(5)
            if not self._client.is_connected():
                print("[MQTT] 检测到断线,尝试重连...")
                try:
                    self._client.reconnect()
                except Exception as e:
                    print("[MQTT] 重连失败: %s" % e)
            # 2026-08-19 修复(B2): paho loop_start 网络线程若因回调异常退出,
            #   is_connected() 仍返回 True 但订阅已死; 检测线程存活, 死亡则重启 loop.
            try:
                _th = getattr(self._client, "_thread", None)
                if _th is not None and not _th.is_alive():
                    print("[MQTT] loop 线程已退出(订阅失效), 重新 loop_start()...")
                    self._client.loop_start()
            except Exception as e:
                print("[MQTT] loop 线程检测异常: %s" % e)

    def stop(self):
        """停止客户端"""
        self._running = False
        try:
            self._client.loop_stop()
            self._client.disconnect()
        except Exception:
            pass
        print("[MQTT] 客户端已停止")
