# -*- coding: utf-8 -*-
"""
dedup.py - 双通道去重（零停机迁移储备，2026-08-16）

背景：
  零停机双发迁移期间，同一份设备数据会经两条腿进入后端：
    腿A（华为 IoTDA 影子）:  bms-bridge  -> Mosquitto bms/<id>/data  -> _handle_data
    腿B（设备直连 ECS）:      固件直连    -> Mosquitto bms/<id>/telemetry -> _handle_telemetry
  两条腿最终都调用 db.insert_data，若不处理会双写（历史库重复行、SOH/告警双算、前端跳变）。

去重策略：
  不依赖"到达时刻"（两路时间戳基准不同：bridge 用 time.time()，固件用自己的 timestamp，
  无法对齐），改用「内容指纹 + 滑动窗口」：
    - 对归一化后的 BMS 核心数值字段计算稳定指纹（md5）。
    - 同一 (device_id, 指纹) 在窗口内再次出现 => 判定为双发重复，跳过入库与推送。
    - 另一条腿仍正常落库/推送，前端不丢数据。
  窗口默认 60s：覆盖桥接轮询(10s)与直连上报(1~10s)的到达时延差；超过窗口的正常变化会被
  视为新样本（合理，因为电池在动时两采样本就不同）。

  该模块为纯内存缓存，进程重启即清空——可接受（双发是过渡态，重启概率低且重复仅影响
  过渡期少量历史行）。
"""
import time
import json
import hashlib
import threading

# 去重窗口（秒）
DEDUP_WINDOW = 60

# 归一化 payload 中"非数值/易变"的键，计算指纹时排除（避免把心跳时刻、在线标记误判为差异）
_EXCLUDE_KEYS = {
    "device_id", "ts", "timestamp", "event_time", "device_online",
    "crc", "crc32", "seq", "_id", "_ts", "data_source",
}

# (device_id, fingerprint) -> 过期绝对时间
_cache = {}

# 线程锁: paho 的 MQTT 回调运行在独立网络线程, 与可能的其他调用方会并发读写 _cache。
# 不加锁在量产多设备下会出现 dict 并发写竞态(RuntimeError: dictionary changed size during iteration
# 或偶发双写)。2026-08-18 高级工程师评审补丁。
_lock = threading.Lock()


def _fingerprint(normalized):
    """对归一化 payload 的"值字段"计算稳定指纹（顺序无关，缺失忽略）"""
    if not isinstance(normalized, dict):
        return ""
    items = []
    for k, v in normalized.items():
        if k in _EXCLUDE_KEYS:
            continue
        items.append((k, v))
    # sort_keys 保证键顺序无关；separators 去空白，保证字符串稳定
    s = json.dumps(sorted(items), sort_keys=True, ensure_ascii=False,
                    separators=(",", ":"), default=str)
    return hashlib.md5(s.encode("utf-8")).hexdigest()


def is_duplicate(device_id, normalized, window=DEDUP_WINDOW):
    """判断是否为近期已收到的重复内容（双发）。

    返回 True  => 重复，调用方应跳过 insert/emit。
    返回 False => 新样本，调用方正常处理，并记录指纹。
    """
    if not device_id or not isinstance(normalized, dict):
        return False
    fp = _fingerprint(normalized)
    if not fp:
        return False
    key = (device_id, fp)
    now = time.time()
    with _lock:
        # 清理过期条目（避免内存无限增长）
        if len(_cache) > 2000:
            expired = [k for k, exp in _cache.items() if exp < now]
            for k in expired:
                _cache.pop(k, None)
        if key in _cache:
            _cache[key] = now + window  # 刷新窗口
            return True
        _cache[key] = now + window
        return False


def stats():
    """调试用：当前缓存条目数（加锁读，避免并发读竞态）"""
    with _lock:
        return len(_cache)
