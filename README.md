# BMS 电池管理系统（全栈）

> ESP32-S3 固件 + Flask Web 监控台 + 华为云 IoTDA 云端链路 —— 从 AFE 芯片到浏览器页面的完整电池管理方案

![CI](https://github.com/A123456123456-zrz/BMS-Dashboard/actions/workflows/ci.yml/badge.svg)

## 在线演示

| 项 | 值 |
|---|---|
| 监控台地址 | https://bms0605.dpdns.org |
| 演示账号 | `viewer`（只读，可看全部数据与图表，无控制权限） |

## 项目亮点

- **5 种 SOC 估算算法可运行时切换**：AEKF / EKF / UKF / OCV 查表 / 安时积分，带收敛状态可视化
- **BQ76952 AFE 完整驱动**：I2C 采样、被动均衡、FET 控制、SHUTDOWN 唤醒（开漏飞线 + 自动重试链路）
- **数据完整性双校验**：CRC8 + CRC32 与后端同算法复核，篡改/位翻转在前端即标红
- **断网自愈**：MQTT HMAC 动态鉴权失败自动重连；离线期间数据落 SPIFFS，恢复后按序补传
- **故障黑匣子**：复位原因/死前现场存 NVS，云端可追溯"上次为什么重启"
- **军工级可靠性工程实践**（高可靠加固）：命令幂等查漏、WS 无限重连、pagehide 资源释放
- **安全**：PBKDF2 口令哈希 + 恒定时间比较防时序侧信道、RBAC 三级角色 + 审计日志、默认口令强制改密、XSS 全量审计
- **双主题响应式 UI**：夜视/日光主题 + 桌面/移动端双布局（移动端底部 Tab 栏，类 App 体验）

## 架构

```
┌─────────────┐   MQTT/TLS   ┌──────────────┐   HTTPS    ┌─────────────┐
│  BMS 设备    │ ───────────▶ │  华为云 IoTDA │ ◀──────── │  Flask 后端  │
│  ESP32-S3   │   HMAC 鉴权   │  (物模型/影子) │   桥接     │  (Flask)    │
│  BQ76952    │              └──────────────┘            │  SQLite     │
└─────────────┘                                          └──────┬──────┘
      │ OTA 固件升级 ◀──────── cloudflared 隧道 ────────────────▶ │ WebSocket
      └──────────────────── https://bms0605.dpdns.org ◀─────── ┌───────┐
                                                               │ 浏览器  │
                                                               └───────┘
```

## 仓库结构

```
├── components/        # ESP-IDF 组件: AFE 驱动/MQTT/参数/算法
│   ├── bms_bsp/       #   板级: BQ76952、蜂鸣器、按键
│   ├── bms_middleware/#   中间件: sys_mqtt(HMAC+CRC)、sys_params(NVS黑匣子)
│   ├── bms_app/       #   应用任务: 采集/保护/SOC/告警
│   └── bms_config/    #   引脚映射与系统配置
├── main/              # 固件入口
├── tools/dashboard/   # Flask 监控台 (后端 + 前端 + 部署脚本)
│   ├── backend/       #   app.py / database.py / iotda 桥接 / 告警推送
│   ├── frontend/      #   Chart.js 双主题 UI (index.html + app.js + style.css)
│   └── deploy/        #   EMQX/mosquitto 配置、docker-compose、部署文档
├── 说明书/            # 12 篇中文文档: 架构/接线/故障排查/OTA/质量评估
├── hardware/          # 硬件资料
└── .github/workflows/ # CI: 固件编译(ESP-IDF) + 后端语法 + 前端语法 三阶段冒烟
```

## 技术栈

| 层 | 技术 |
|---|---|
| 固件 | ESP-IDF v5.2 (FreeRTOS)、BQ76952 (I2C)、mbedTLS (HMAC/TLS)、NV(S) |
| 算法 | AEKF/EKF/UKF SOC 融合、SOH 双法、OCV 查表、dT/dt 热失控预警 |
| 后端 | Python Flask、SQLite、paho-mqtt、Socket.IO、华为云 IoTDA API |
| 前端 | 原生 JS + Chart.js（零框架）、CSS 变量双主题、Service Worker PWA |
| 运维 | GitHub Actions CI、cloudflared 隧道、EMQX/mosquitto、systemd 部署 |

## 快速开始

```bash
# 后端
cd tools/dashboard/backend
pip install -r requirements.txt   # flask / paho-mqtt / flask-socketio
python app.py                     # http://localhost:5000

# 固件 (ESP-IDF v5.2)
idf.py build && idf.py -p COM6 flash monitor
```

首次启动自动创建 `admin/admin123`，登录后强制修改密码。

## 文档

完整的 12 篇中文文档见 [说明书/](说明书/) —— 从硬件接线图到固件 bug 逐条排查记录，以及 [tools/dashboard/docs/](tools/dashboard/docs/) 下的云端部署指南。

## 说明

- 仓库中所有服务器 IP、设备 ID、密钥均已占位符化；真实凭证通过环境变量注入
- `viewer` 演示账号为只读角色，控制指令/参数修改仅限管理员
