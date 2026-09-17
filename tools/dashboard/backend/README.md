# backend/ — BMS Dashboard 后端

本目录存放 Python 后端服务代码与运行数据,入口为 `app.py`(Flask + SocketIO):

| 文件 | 职责 |
|---|---|
| `app.py` | Flask 主程序(路由/登录/API/WebSocket/OTA 服务) |
| `database.py` | SQLite 数据访问(`bms_history.db` 在本目录) |
| `iotda_client.py` | 华为云 IoTDA REST 客户端(AK/SK 签名轮询设备影子) |
| `iotda_rules.py` | 云端联动规则引擎 |
| `alert_push.py` | 告警推送(企业微信/Server酱/PushPlus) |
| `mqtt_client.py` | MQTT 订阅(当前生产未启用, 数据走 IoTDA REST) |
| `utils.py` | 通用工具函数 |
| `backup_db.py` | 数据库自动备份(输出到 `../backups/`) |
| `aging_monitor.py` | 老化测试监控(辅助工具) |
| `firmware_aging_test.py` | 固件老化测试脚本(辅助) |
| `test_fixes.py` / `tests/` | 单元测试 |
| `requirements*.txt` | Python 依赖清单 |
| `dashboard.env` | 运行配置(华为云凭据/告警 webhook, 不入库) |
| `.secret_key` | 会话密钥(自动生成, 勿删) |
| `rules_config.json` / `iotda_product_model.json` | 规则与物模型配置 |
| `bms_history.db` | 历史数据数据库(SQLite) |

> 单实例锁 `.bms_dashboard.lock` 与运行日志已归入 `../logs/`(2026-08-14 归类)。

启动方式(由上层脚本调用, 勿直接双击运行):
- `python backend/app.py` 或经由 `../scripts/start_dashboard.vbs` / `../scripts/watchdog.ps1` 托管
