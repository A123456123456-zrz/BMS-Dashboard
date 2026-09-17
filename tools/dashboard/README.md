# BMS 数据可视化 Dashboard

基于 Python Flask + MQTT + ECharts 的 BMS(电池管理系统)实时监控网页。

## 功能特性

- 实时仪表盘:SOC 圆环 + 总压/电流/温度 数显
- 6 串单体电压柱状图(按电压高低着色)
- 实时曲线图(SOC/电压/电流/温度 多线,5 分钟滚动窗口)
- 故障码显示(按等级颜色标识:正常/告警/错误/严重)
- 阈值修改面板(下拉选参数 → 输入新值 → 下发到 ESP32)
- 远程命令(解除报警/重启/OTA 检查/获取参数/获取信息)
- 设备信息显示(IP/MAC/版本/运行时间/WiFi/MQTT 状态)
- SQLite 历史数据存储(自动清理 7 天前旧数据)
- **双通道架构(2026-08-16, 自建 EMQX 为主)**: 设备同一 JSON 双发自建 EMQX(mTLS 双向证书, 8883·实时主) + 华为云 IoTDA(兜底·影子/规则/告警); 后端按 device_id+timestamp 去重 + 主备反转(本地腿活跃则华为云腿不推实时, 断线自动兜底); 手机 App 直连自建 EMQX 订阅

## 目录结构(2026-08-14 归类后)

```
tools/dashboard/
├── backend/              # Flask 后端(app.py 主程序 + IoTDA 客户端 + 告警 + 数据库)
│   ├── app.py            # Flask 主程序(入口, 监听 0.0.0.0:5000)
│   ├── iotda_client.py   # 华为云 IoTDA REST 客户端(数据走 IoTDA, 非 MQTT)
│   ├── database.py       # SQLite 数据库模块
│   ├── requirements.txt  # Python 依赖
│   └── README.md         # 后端说明
├── frontend/             # 前端(templates/ + static/)
├── scripts/              # 启动/守护脚本(start_dashboard.vbs / watchdog.ps1 等)
├── tunnel/               # 隧道(cloudflared.exe + config.yml + 凭据)
├── logs/                 # 运行日志与锁文件
├── deploy/               # Docker 部署(可选)
├── docs/                 # 文档
├── backups/              # 数据库自动备份
```

## 环境要求

- Python 3.8+
- 依赖见 `backend/requirements.txt`
- 运行配置在 `backend/dashboard.env`(华为云凭据/密码,不入库)

## 启动

```bash
# 推荐:双击 scripts/start_dashboard.vbs(后台无窗口, 同时启动 Flask + 隧道 + watchdog)
wscript scripts/start_dashboard.vbs
```

启动后浏览器访问: <http://localhost:5000> 或公网 <http://<YOUR_ECS_IP>:5000>

## 配置

所有配置集中在 `app.py` 顶部常量区:

```python
MQTT_BROKER = "broker.emqx.io"   # broker 地址
MQTT_PORT   = 1883                # 端口
TOPIC_DATA  = "bms/data"          # 实时数据主题
TOPIC_FAULT = "bms/fault"         # 故障主题
TOPIC_CMD_RESP = "bms/cmd_resp"   # 命令响应主题
TOPIC_INFO  = "bms/info"          # 设备信息主题
TOPIC_CMD   = "bms/cmd"           # 命令下发主题
HOST = "0.0.0.0"
PORT = 5000
```

## MQTT 主题说明

| 主题          | 方向         | 说明                                 |
| ------------- | ------------ | ------------------------------------ |
| `bms/data`    | ESP32 → 网页 | 实时数据帧                           |
| `bms/fault`   | ESP32 → 网页 | 故障告警                             |
| `bms/cmd_resp`| ESP32 → 网页 | 命令响应                             |
| `bms/info`    | ESP32 → 网页 | 设备信息                             |
| `bms/cmd`     | 网页 → ESP32 | 命令下发(参数修改/重启/OTA 等)      |

## 数据格式

### ESP32 上报 (bms/data)

```json
{
  "soc": 85.0, "soh": 96.8,
  "v_max": 3750, "v_min": 3680,
  "pack_v": 22350, "current": 3200,
  "temp_max": 301, "temp_min": 268,
  "fault": 0,
  "cells": [3720, 3710, 3680, 3750, 3730, 3700],
  "balance_mask": 8,
  "timestamp": 12345678
}
```

### 命令下发 (bms/cmd)

```json
{"cmd":"set_param","key":"cell_ov_prot_mv","value":4250}
{"cmd":"get_params"}
{"cmd":"get_info"}
{"cmd":"clear_alarm"}
{"cmd":"restart"}
{"cmd":"ota_check"}
{"cmd":"ota_upgrade","url":"https://..."}
```

## 单位说明

| 字段        | 单位   | 说明                |
| ----------- | ------ | ------------------- |
| 电压        | mV     | 1 V = 1000 mV       |
| 电流        | mA     | 1 A = 1000 mA(正充电/负放电) |
| 温度        | 0.1℃  | 50.0℃ = 500         |
| SOC/SOH     | %      | 0~100               |

## REST API

| 接口              | 方法 | 说明                          |
| ----------------- | ---- | ----------------------------- |
| `/api/history`    | GET  | 查询历史数据(参数: minutes, limit) |
| `/api/latest`     | GET  | 查询最新一条数据              |
| `/api/faults`     | GET  | 查询故障记录                  |
| `/api/params`     | GET  | 查询参数列表                  |
| `/api/params`     | POST | 修改参数(body: {key, value}) |
| `/api/cmd`        | POST | 下发命令(body: {cmd, ...})   |
| `/api/status`     | GET  | 查询 MQTT/数据库状态          |

## 可调参数列表

```
cell_ov_prot_mv      单体过压保护      cell_uv_prot_mv      单体欠压保护
temp_ot_prot_dc      过温保护          temp_ut_prot_dc      低温保护
chg_oc_prot_ma       充电过流保护      dsg_oc_prot_ma       放电过流保护
cell_ov_warn_mv      单体过压告警      cell_uv_warn_mv      单体欠压告警
chg_oc_warn_ma       充电过流告警      dsg_oc_warn_ma       放电过流告警
temp_ot_warn_dc      过温告警          temp_ut_warn_dc      低温告警
cell_dv_warn_mv      单体压差告警      soc_low_warn_pct     低 SOC 告警
balance_threshold_mv 均衡开启阈值      balance_stop_mv      均衡停止阈值
```

## 技术栈

- 后端:Flask 3.0 + Flask-SocketIO + paho-mqtt + eventlet
- 前端:ECharts 5 + Socket.IO 4(均通过 CDN 引入)
- 数据库:SQLite(免安装,文件 `bms_history.db`)
- UI:深色主题,响应式布局

## 故障排查

1. **MQTT 一直未连接**:检查网络是否能访问 `broker.emqx.io:1883`(可 `ping broker.emqx.io` 测试)。
2. **页面无数据**:确认 ESP32 已上报数据到 `bms/data` 主题(可用 MQTTX 客户端订阅验证)。
3. **命令下发无响应**:确认 ESP32 已订阅 `bms/cmd` 主题并实现对应命令处理。
4. **端口被占用**:修改 `app.py` 中的 `PORT` 常量。
5. **eventlet 安装失败**:可改用 `pip install simple-websocket` 或注释掉 eventlet,使用默认 threading 模式。
