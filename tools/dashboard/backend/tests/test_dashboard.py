# -*- coding: utf-8 -*-
"""
BMS Dashboard 看板侧单元测试
================================
P0-4 补充测试: 故障码解析(_shadow_to_payload) + 串数自适应 + 数据库读写

运行方式(无需安装第三方依赖, 标准库 unittest):
    cd tools/dashboard
    python -m unittest discover -s tests -v
"""
import json
import os
import sys
import tempfile
import unittest

# 允许直接 import 看板模块(以本文件所在目录为基准定位 dashboard 目录)
_DASH_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _DASH_DIR not in sys.path:
    sys.path.insert(0, _DASH_DIR)

import database as db
from iotda_client import IoTDAClient


def _make_client():
    """构造最小 IoTDAClient(不影响真实连接, 仅测 _shadow_to_payload)"""
    return IoTDAClient(
        ak="test-ak", sk="test-sk", project_id="test-project",
        region="cn-south-4", device_id="test-device",
        socketio=None, poll_interval=60,
    )


def _shadow(props: dict):
    """构造华为云影子格式(单服务 BMS)"""
    return [{"service_id": "BMS", "reported": {"properties": props}}]


class TestShadowToPayload(unittest.TestCase):
    """故障码解析 / 影子→前端 payload 转换"""

    def setUp(self):
        self.client = _make_client()

    def test_none_returns_none(self):
        self.assertIsNone(self.client._shadow_to_payload(None))

    def test_no_bms_service_returns_none(self):
        shadow = [{"service_id": "OTHER", "reported": {"properties": {"soc": 0.5}}}]
        self.assertIsNone(self.client._shadow_to_payload(shadow))

    def test_empty_props_returns_none(self):
        self.assertIsNone(self.client._shadow_to_payload([{"service_id": "BMS", "reported": {"properties": {}}}]))

    def test_fault_mask_passthrough(self):
        """故障码透传: fault=0x20000 (LOW_SOC_WARN)"""
        payload = self.client._shadow_to_payload(_shadow({"fault": 0x20000}))
        self.assertEqual(payload["fault"], 0x20000)
        self.assertEqual(payload["data_source"]["fault"], "real")

    def test_cells_string_parsed_to_list(self):
        """cells 云端以字符串存储, 需还原为 int 列表"""
        payload = self.client._shadow_to_payload(_shadow({"cells": "[3700,3680,3660,3640,3620,3600]"}))
        self.assertEqual(payload["cells"], [3700, 3680, 3660, 3640, 3620, 3600])
        self.assertEqual(payload["pack_v"], sum([3700, 3680, 3660, 3640, 3620, 3600]))

    def test_cells_corrupt_string_falls_back(self):
        payload = self.client._shadow_to_payload(_shadow({"cells": "not-json"}))
        # 解析失败回退到当前串数补 0
        self.assertEqual(len(payload["cells"]), db.get_series_num())

    def test_online_no_data_false_even_all_zero(self):
        """P0-3 修复回归: 在线且影子新鲜时, 即使 LTC6804 未接(全0)也不置 no_data"""
        payload = self.client._shadow_to_payload(_shadow({"soc": 0, "pack_v": 0, "current": 0}))
        self.assertFalse(payload["no_data"])
        self.assertFalse(payload["is_mock"])

    def test_pack_v_fallback_from_cells(self):
        """pack_v<=0 时从 cells 求和兜底"""
        payload = self.client._shadow_to_payload(_shadow({"cells": "[3700,3680]", "pack_v": 0}))
        self.assertEqual(payload["pack_v"], 7380)
        self.assertEqual(payload["v_max"], 3700)
        self.assertEqual(payload["v_min"], 3680)

    def test_series_num_updated_from_cells(self):
        """串数自适应: cells 长度 8 → 系统串数更新为 8"""
        old = db.get_series_num()
        self.client._shadow_to_payload(_shadow({"cells": "[1,2,3,4,5,6,7,8]"}))
        self.assertEqual(db.get_series_num(), 8)
        # 恢复原串数, 避免污染后续用例
        db.update_series_num(old)

    def test_timestamp_default(self):
        payload = self.client._shadow_to_payload(_shadow({"soc": 0.5}))
        self.assertIsInstance(payload["timestamp"], int)


class TestNormalizeReportPaths(unittest.TestCase):
    """2026-08-11 回归: IoTDA 数据转发推送 / MQTT 直连上报 三路入口统一归一化
    固件 sys_mqtt.c 上报为 camelCase + services[].properties 包裹, 旧代码把原始
    camelCase 直接喂给 db.insert_data(读 snake_case) → pack_v/v_max/temp_max/cells 全 0/空.
    修复后 api_iotda_push 与 mqtt_client._handle_data 都经 extract_bms_props +
    props_to_payload 归一化, 与影子轮询路径共用同一映射.
    """

    def _firmware_msg(self, props):
        """模拟固件 properties/report 直发(JSON 形状)"""
        return {"services": [{"service_id": "BMS", "properties": props}]}

    def _normalize(self, msg):
        from iotda_client import extract_bms_props, props_to_payload
        return props_to_payload(extract_bms_props(msg))

    def test_direct_publish_camelcase_normalized(self):
        """直发 camelCase 经归一化后字段正确(修复核心断言)"""
        msg = self._firmware_msg({
            "soc": 88.5, "soh": 99.0, "vMax": 4200, "vMin": 4000, "packV": 24600,
            "current": -1500, "tempMax": 350, "tempMin": 300,
            "cellVoltages": ["4100", "4080", "4070", "4090", "4060", "4050"],
            "fault": 0, "cycleCount": 120, "chargeMos": 1, "dischargeMos": 1,
        })
        payload = self._normalize(msg)
        self.assertIsNotNone(payload)
        self.assertEqual(payload["pack_v"], 24600)
        self.assertEqual(payload["v_max"], 4200)
        self.assertEqual(payload["v_min"], 4000)
        self.assertEqual(payload["temp_max"], 350)
        self.assertEqual(payload["cells"], [4100, 4080, 4070, 4090, 4060, 4050])
        self.assertEqual(payload["fault"], 0)

    def test_notify_data_wrapper_normalized(self):
        """华为云转发 notify_data.body 包裹也能解析"""
        msg = {"notify_data": {"body": self._firmware_msg(
            {"packV": 9999, "cellVoltages": ["1000"]})}}
        payload = self._normalize(msg)
        self.assertEqual(payload["pack_v"], 9999)
        self.assertEqual(payload["cells"], [1000])

    def test_flat_dict_normalized(self):
        """已是平铺属性字典(本地 MQTT bms/data 可能直接发 flat)也能归一化"""
        payload = self._normalize({"soc": 50, "packV": 12345,
                                    "vMax": 2100, "cellVoltages": ["2000", "2100"]})
        self.assertEqual(payload["pack_v"], 12345)
        self.assertEqual(payload["cells"], [2000, 2100])

    def test_non_bms_service_ignored(self):
        """非 BMS 服务的属性不应串入(与旧 _shadow_to_payload 语义一致)"""
        msg = {"services": [
            {"service_id": "OTHER", "properties": {"packV": 12345}},
            {"service_id": "BMS", "properties": {"packV": 24600, "cellVoltages": ["4100"]}},
        ]}
        payload = self._normalize(msg)
        self.assertEqual(payload["pack_v"], 24600)

    def test_shadow_path_still_works(self):
        """影子轮询路径(_shadow_to_payload)修复后行为不变"""
        from iotda_client import IoTDAClient
        c = IoTDAClient(ak="a", sk="b", project_id="p", region="r",
                        device_id="d", socketio=None, poll_interval=60)
        shadow = [{"service_id": "BMS", "reported": {"properties": {
            "packV": 5555, "cellVoltages": ["1100", "1200"]}}}]
        payload = c._shadow_to_payload(shadow)
        self.assertEqual(payload["pack_v"], 5555)
        self.assertEqual(payload["cells"], [1100, 1200])


class TestDatabase(unittest.TestCase):
    """数据库读写(隔离临时库, 不碰真实 bms_history.db)"""

    @classmethod
    def setUpClass(cls):
        cls._tmpdir = tempfile.mkdtemp(prefix="bms_test_")
        cls._real_path = db.DB_PATH
        db.DB_PATH = os.path.join(cls._tmpdir, "test_bms_history.db")
        db.init_db()

    @classmethod
    def tearDownClass(cls):
        db.DB_PATH = cls._real_path
        import shutil
        shutil.rmtree(cls._tmpdir, ignore_errors=True)

    def test_insert_and_query_latest(self):
        payload = {
            "timestamp": 1700000000000, "soc": 0.85, "soh": 0.99,
            "v_max": 4200, "v_min": 4000, "pack_v": 24600,
            "current": -1200, "temp_max": 28, "temp_min": 25,
            "fault": 0x20000, "cells": [4200, 4100, 4100, 4000, 4100, 4100],
            "cycle_count": 12, "balance_mask": 0,
        }
        db.insert_data(payload)
        row = db.query_latest()
        self.assertIsNotNone(row)
        self.assertEqual(row["soc"], 0.85)
        self.assertEqual(row["fault"], 0x20000)
        # 串数自适应: 6 格 cells 完整落库
        self.assertEqual(len(json.loads(row["cells_json"])), 6)

    def test_insert_cells_series_adaptation(self):
        """8 串数据落库后 cells_json 应为 8 格"""
        old = db.get_series_num()
        db.update_series_num(8)
        payload = {"timestamp": 1700000001000, "cells": [3800]*8,
                   "soc": 0.9, "current": 0, "fault": 0}
        db.insert_data(payload)
        row = db.query_latest()
        self.assertEqual(len(json.loads(row["cells_json"])), 8)
        db.update_series_num(old)

    def test_query_faults_filters(self):
        db.insert_data({"timestamp": 1700000002000, "cells": [0]*6,
                        "soc": 0.1, "current": 0, "fault": 0x20000})
        faults = db.query_faults()
        self.assertGreaterEqual(len(faults), 1)
        self.assertTrue(any(f["fault"] == 0x20000 for f in faults))


if __name__ == "__main__":
    unittest.main(verbosity=2)
