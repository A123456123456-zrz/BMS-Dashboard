# BMS Dashboard 云服务器部署方案（超级详细版）

> 适用对象：BMS System 项目的 `tools/dashboard/`（Flask + SocketIO + 华为云 IoTDA 的电池监控网页）
> 目标：让网页 **7×24 小时** 可访问，不再依赖你的电脑开机/联网
> 作者日期：2026-08-07 · 基于当前代码库实测确认

---

## 目录

1. [架构总览：本地模式 vs 云部署模式](#1-架构总览本地模式-vs-云部署模式)
2. [数据流分析（云部署后数据怎么走）](#2-数据流分析)
3. [前置条件清单](#3-前置条件清单)
4. [云服务器选型与购买](#4-云服务器选型与购买)
5. [服务器初始化（安全第一步）](#5-服务器初始化)
6. [系统环境准备（Python + 依赖）](#6-系统环境准备)
7. [上传项目代码](#7-上传项目代码)
8. [安装 Python 依赖](#8-安装-python-依赖)
9. [修改配置（密码 / 华为云凭据 / 端口）](#9-修改配置)
10. [首次启动与功能验证](#10-首次启动与功能验证)
11. [systemd 守护：开机自启 + 崩溃自动重启](#11-systemd-守护)
12. [Nginx 反向代理 + HTTPS 证书](#12-nginx-反向代理--https-证书)
13. [安全加固](#13-安全加固)
14. [数据备份与恢复（SQLite）](#14-数据备份与恢复)
15. [升级 / 回滚 / 迁移](#15-升级--回滚--迁移)
16. [监控与日志](#16-监控与日志)
17. [ESP32 固件侧调整（OTA 地址）](#17-esp32-固件侧调整)
18. [故障排查手册](#18-故障排查手册)
19. [成本估算](#19-成本估算)
20. [部署前后检查清单](#20-部署前后检查清单)
21. [附录：常用运维命令速查](#21-附录常用运维命令速查)

---

## 1. 架构总览（本地模式 vs 云部署模式）

### 1.1 现状：本地模式（依赖你的电脑）

```
其他设备(手机/平板/PC)
      │  http://<YOUR_ECS_IP>:5000
      ▼
Cloudflare 边缘节点
      │  cloudflared 出站隧道
      ▼
你的电脑（必须开机+联网）
      ├─ cloudflared.exe 隧道进程
      └─ Dashboard (Flask, 127.0.0.1:5000)
              │  华为云 IoTDA REST API(轮询影子/下发命令)
              ▼
        华为云 IoTDA ←──────── ESP32 (MQTT 上报)
```

**缺点**：你电脑关机 / 断网 / 进程崩溃 → 网页不可用。这正是你遇到的问题。

### 1.2 目标：云部署模式（7×24）

```
其他设备(手机/平板/PC)
      │  https://bms.example.com（或 http://云服务器公网IP:5000）
      ▼
Nginx (云服务器, 443/80 → 5000)  ←── HTTPS 证书
      ▼
Dashboard (Flask, 云服务器 127.0.0.1:5000)
      │  华为云 IoTDA REST API（公网可达）
      ▼
华为云 IoTDA ←──────── ESP32 (MQTT 上报，不用改)
```

**优点**：
- 你的电脑关机/断网都不影响；
- 云服务器有公网 IP + 固定带宽，访问更稳定；
- 不需要 cloudflared 隧道（云服务器本身就是公网入口）；
- 网页登录密码、数据照常工作。

**关键结论（实测确认）**：Dashboard 的所有数据来自**华为云 IoTDA REST API**（`iotda_client.py` 手工 HMAC-SHA256 签名轮询设备影子），**没有直接依赖 ESP32 的 MQTT broker**（`mqtt_client.py` 虽然存在于项目里，但 `app.py` 并未调用它）。所以只要云服务器能访问公网（访问华为云 API 域名），数据链路就通。这是整个方案可行性的基础。

### 1.3 数据流细节（部署后需要验证的 3 条链路）

| # | 链路 | 方向 | 验证方法 |
|---|------|------|----------|
| 1 | 浏览器 → 云服务器 443/80 | 公网入 | 访问域名/公网 IP 能打开登录页 |
| 2 | 云服务器 → 华为云 IoTDA | 公网出 | 登录后能看到实时数据、设备在线状态 |
| 3 | ESP32 → 云服务器（仅 OTA 需要） | 公网入 | ESP32 能从云服务器拉取固件版本/升级包 |

> ⚠️ 链路 3 只有在你需要**远程 OTA 升级 ESP32** 时才需要。如果 ESP32 和 Dashboard 不在同一内网，ESP32 必须能访问到云服务器的 OTA 地址（见第 17 章）。

---

## 2. 数据流分析

### 2.1 数据上报（ESP32 → 华为云 → Dashboard）

1. ESP32 固件（`sys_mqtt.c`）通过 MQTT + TLS 上报到**华为云 IoTDA**（`BMS_MQTT_SERVICE_ID="BMS"`），属性包含 `soc/soh/pack_v/current/cells/fault` 等；
2. 云服务器上的 Dashboard 通过 `iotda_client.py` 以 AK/SK 签名调用 IoTDA REST API，**轮询设备影子**（默认 60s，离线自动降频）；
3. 数据写入 SQLite（`bms_history.db`），并通过 SocketIO 实时推送到浏览器。

### 2.2 命令下发（浏览器 → 云服务器 → IoTDA → ESP32）

1. 网页点击按钮 → `POST /api/cmd` 或 `/api/params`；
2. `iotda_client.py` 调用 IoTDA 命令下发 API（`/v5/iot/{project_id}/devices/{device_id}/messages`）；
3. ESP32 收到命令执行（重启/清告警/改阈值/OTA 等），通过影子/响应主题回报结果。

### 2.3 部署后不需要的东西

- ❌ cloudflared 隧道：不需要（云服务器自带公网 IP）；
- ❌ 本地 watchdog/自启脚本（bat/vbs/ps1）：不需要（改用 systemd）；
- ❌ 本机 SQLite 历史数据：可选迁移（见 14 章），不迁移也能从零开始采集。

---

## 3. 前置条件清单

开始前请准备好以下内容（避免中途卡壳）：

- [ ] **云服务器**：一台公网可访问的 Linux 服务器（推荐华为云 ECS / 轻量云，见第 4 章）
- [ ] **SSH 客户端**：电脑上能连服务器（Windows 自带 `ssh` 命令即可）
- [ ] **代码包**：`D:\esp32project\BMS\BMS System\tools\dashboard\` 整个目录（约几 MB）
- [ ] **华为云凭据**（在 `app.py` 第 84~88 行）：`AK / SK / PROJECT_ID / REGION / DEVICE_ID`，部署时要用
- [ ] **（可选）域名**：如 `bms.example.com`，用于 HTTPS；没有域名可用公网 IP + HTTP 先跑起来
- [ ] **（可选）邮箱**：申请 Let's Encrypt 免费 HTTPS 证书用
- [ ] **ESP32 侧改动**（仅 OTA 需要）：`bms_config.h` 中 OTA 版本 URL 改成云服务器地址（见第 17 章）

> 无需准备：MQTT broker（EMQX 等）——本 Dashboard 不直连 MQTT，数据走华为云。

---

## 4. 云服务器选型与购买

### 4.1 规格建议（最低配置即可）

| 配置项 | 推荐值 | 说明 |
|--------|--------|------|
| 系统 | Ubuntu 22.04 LTS 或 Debian 12 | 教程命令基于 Debian/Ubuntu；CentOS 需自行转换 yum 命令 |
| CPU | 1~2 核 | Flask + SocketIO 足够 |
| 内存 | 2 GB（最低 1 GB） | eventlet + SQLite 内存占用约 200~400MB |
| 磁盘 | 40 GB 系统盘即可 | 历史数据是 SQLite 单文件，7 天自动清理 |
| 带宽 | 1~3 Mbps 起步 | 网页是轻量 JSON + 图表，够用；人多再升 |
| 公网 IP | 必须（弹性公网 IP） | 没有公网 IP 就白搭 |
| 安全组 | 放行 22 / 80 / 443（或 5000） | 见 5.4 |

### 4.2 推荐厂商与购买入口（任选其一）

1. **华为云 ECS**（与你的 IoTDA 同生态，内网互通方便）：控制台 → 弹性云服务器 → 购买，选 `通用计算型 s6 1核2G` + Ubuntu 22.04，付费方式选"按需"（小时计费，先跑通再转包年）。
2. **华为云 轻量应用服务器**（最省事）：控制台 → 轻量应用服务器 → 购买，选 Ubuntu 22.04，自带固定公网 IP + 应用防火墙，适合新手。
3. 腾讯云轻量 / 阿里云轻量：同样支持 Ubuntu 22.04，教程命令通用。

> 💡 建议：第一次先用"按需付费"，跑通 + 验证数据链路后，再考虑包年（便宜 60%+）。

### 4.3 购买后记下 4 个信息

```
公网 IP：        例如 123.123.123.123
root 密码：      （或你创建的密钥）
SSH 端口：       22（默认）
系统：           Ubuntu 22.04
```

---

## 5. 服务器初始化

### 5.1 SSH 登录

在你的电脑（Windows）上打开 PowerShell 或 CMD，执行：

```bash
ssh root@123.123.123.123
```

- 首次连接提示 `Are you sure you want to continue connecting?` 输入 `yes`；
- 然后输入 root 密码（输入时不显示字符是正常的）。

> 如果没有 `ssh` 命令（老 Windows），装个 Git Bash 即可自带，或用 `Windows 终端`。

### 5.2 立刻修改 root 密码（首次必做）

```bash
passwd root
```

### 5.3 更新系统

```bash
apt update && apt upgrade -y
```

（Ubuntu 首次可能耗时 3~10 分钟，正常。）

### 5.4 配置防火墙（重要！）

云服务器一般有两层：**云控制台安全组** + **系统防火墙**。两层都要放行。

**系统防火墙（ufw）**：

```bash
apt install -y ufw
ufw allow 22/tcp      # SSH（先放行，避免把自己锁外面）
ufw allow 80/tcp      # HTTP（Nginx）
ufw allow 443/tcp     # HTTPS（Nginx）
ufw allow 5000/tcp    # 备选：直接访问 5000（安全起见后期可关，见第12章）
ufw --force enable
ufw status            # 查看放行规则
```

**云控制台安全组**：登录华为云控制台 → 你的服务器实例 → 安全组 → 入方向规则，添加：
- 22 (TCP) 来源 0.0.0.0/0（或只放行你电脑的公网 IP，更安全）
- 80 (TCP)、443 (TCP) 来源 0.0.0.0/0
- 5000 (TCP) 来源 0.0.0.0/0（临时调试用）

### 5.5 创建普通用户（可选但推荐，避免一直用 root）

```bash
adduser bms           # 按提示设置密码
usermod -aG sudo bms  # 给 sudo 权限
su - bms              # 切到该用户
```

> 后续命令在 `bms` 用户下执行，需要管理员权限时命令前加 `sudo`。为简洁，本文档多数命令以 root 执行。

---

## 6. 系统环境准备（Python + 依赖）

### 6.1 安装 Python 3（Ubuntu 22.04 自带 3.10，满足要求）

```bash
python3 --version     # 应输出 Python 3.10.x 或更高（项目要求 3.8+）
```

如果版本低于 3.8（老系统）：

```bash
apt install -y python3 python3-pip python3-venv
```

### 6.2 安装 pip / venv（若缺失）

```bash
apt install -y python3-pip python3-venv
```

### 6.3 （可选但强烈推荐）创建虚拟环境，隔离依赖

```bash
cd /opt
mkdir -p bms-dashboard && cd bms-dashboard
python3 -m venv venv
source venv/bin/activate        # 激活后命令行前缀出现 (venv)
python --version                # 确认用的是 venv 里的 Python
```

> 不建 venv 也能跑，但全局装依赖容易和你服务器上其他 Python 项目打架。教程按 venv 写。

---

## 7. 上传项目代码

### 7.1 准备代码包（在你电脑上操作）

把整个 `tools/dashboard` 目录打成 zip（**只打 dashboard 目录内容，不含外层 BMS System**）：

在你电脑的 PowerShell 中：

```powershell
cd "D:\esp32project\BMS\BMS System\tools"
Compress-Archive -Path dashboard -DestinationPath dashboard.zip
```

> 压缩前先删除无用文件，减小体积、避免上传卡顿：
> `dashboard.zip` 里可删：`*.log`、`__pycache__`、`_tmp_app.js`、`bms_history.db`（历史数据可选保留，见 14 章）。

### 7.2 上传到服务器（在你电脑上操作）

方法 A：`scp`（最简单）

```bash
scp "D:\esp32project\BMS\BMS System\tools\dashboard.zip" root@123.123.123.123:/tmp/
```

方法 B：`rsync`（Windows Git Bash 自带，增量同步，以后升级用这个最爽）

```bash
rsync -avz "D:/esp32project/BMS/BMS System/tools/dashboard/" root@123.123.123.123:/opt/bms-dashboard/app/
```

### 7.3 服务器上解压（在服务器上操作）

```bash
cd /opt/bms-dashboard
mkdir -p app
unzip /tmp/dashboard.zip -d app        # 解压后 app/ 下就是 app.py、static、templates 等
ls app                                  # 确认看到 app.py / requirements.txt / templates / static
```

> 没装 unzip 就先：`apt install -y unzip`
> 若用 rsync 同步过，则跳过 7.2 方法 A 与 7.3，直接进入第 8 章。

---

## 8. 安装 Python 依赖

### 8.1 激活 venv 并安装

```bash
cd /opt/bms-dashboard
source venv/bin/activate
cd app
pip install -r requirements.txt
```

### 8.2 常见坑与解决方案

| 报错 | 原因 | 解决 |
|------|------|------|
| `externally-managed-environment`（Ubuntu 23+） | 系统限制 pip 全局安装 | 确认已用 venv（上一步 `source venv/bin/activate`） |
| `error: externally-managed` 仍出现 | venv 没激活 | 检查命令行前缀是否带 `(venv)` |
| `greenlet` / `eventlet` 编译失败 | 缺 gcc/头文件 | `apt install -y build-essential python3-dev` 后重装 |
| `No matching distribution found for flask==3.0.0` | 网络无法访问 PyPI | 换镜像源：`pip install -r requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple` |

### 8.3 验证依赖是否完整

```bash
python -c "import flask, flask_socketio, eventlet, requests; print('deps OK')"
```

> ✅ 应该输出 `deps OK`。`requests` 是 iotda_client 手工签名必需的（requirements.txt 里没列，但代码在用——若上面命令报 `No module named 'requests'`，请执行 `pip install requests`）。

---

## 9. 修改配置

### 9.1 编辑 `app.py` 顶部配置区

```bash
nano /opt/bms-dashboard/app/app.py
```

（不会 nano 就装：`apt install -y nano`；或 `vim` 也一样。）

需要重点核对/修改 4 处：

**① 登录密码（第 31 行附近）——必须改！**

```python
DASHBOARD_PASSWORD = "bms123"   # ← 改成强密码，例如 "Bms@2026!Lab"
```

**② 华为云凭据（第 84~88 行附近）——与你的华为云账号一致**

```python
# P0-2 安全加固: 严禁把真实 AK/SK 写进仓库! 通过环境变量注入
AK         = os.environ.get("HUAWEI_AK", "")      # 华为云 Access Key（云上部署建议换专用子账号的 AK/SK，见 13 章）
SK         = os.environ.get("HUAWEI_SK", "")
PROJECT_ID = os.environ.get("HUAWEI_PROJECT_ID", "")
REGION     = os.environ.get("HUAWEI_REGION", "cn-south-4")
DEVICE_ID  = os.environ.get("HUAWEI_DEVICE_ID", "")
```

> ⚠️ 这些是**敏感凭据**！上传代码包前请确认不会把含 SK 的 app.py 公开到 git/网盘。生产建议：创建华为云 IAM 子账号，仅授予该设备相关权限，用子账号 AK/SK 部署（见 13.3）。

**③ 端口（第 1319 行附近）——默认 5000，不用改**

```python
socketio.run(app, host="0.0.0.0", port=5000, debug=False, use_reloader=False)
```

**④ 设备信息（第 881~906 行 HARDWARE_SPEC）——可选，改产品型号/部署区域等展示信息**

```python
"deploy_area": "深圳·实验室",   # 可改成 "云服务器·华为云"
```

改完保存：nano 中按 `Ctrl+O` 回车保存，`Ctrl+X` 退出。

### 9.2 验证配置语法

```bash
cd /opt/bms-dashboard/app
python -m py_compile app.py database.py iotda_client.py mqtt_client.py && echo "语法 OK"
```

### 9.3 确认数据库目录可写

Dashboard 会在当前目录生成 `bms_history.db`：

```bash
chmod -R u+rw /opt/bms-dashboard/app
touch /opt/bms-dashboard/app/.write_test && rm /opt/bms-dashboard/app/.write_test && echo "目录可写"
```

---

## 10. 首次启动与功能验证

### 10.1 前台启动（看日志，确认无报错）

```bash
cd /opt/bms-dashboard
source venv/bin/activate
cd app
python app.py
```

预期输出（类似）：

```
[DB] 数据库已初始化
[DB] 参数历史表已就绪
[IoTDA] 客户端已启动, device_id=<IOTDA_DEVICE_ID>_BMS001
============================================================
  BMS Dashboard 服务已启动
  访问地址: http://localhost:5000/
  登录密码: bms123
============================================================
```

### 10.2 服务器本机验证

另开一个 SSH 窗口（或先 Ctrl+C 停掉前台，再按 10.3 后台跑）：

```bash
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:5000/login
```

- 输出 `200`：服务正常；
- 输出 `302`：服务正常（未登录重定向到 /login，属正常）；
- 无输出/拒绝连接：看 10.1 的报错日志排查（见第 18 章）。

### 10.3 公网验证（临时直连 5000）

在你电脑浏览器访问：`http://123.123.123.123:5000/`（前提：安全组 + ufw 都放行了 5000）。

能看到**登录页**即成功；输入密码后能看到实时数据、设备在线状态，说明 IoTDA 链路也通了。

> 验证完建议立即关闭 5000 端口对外（只留 80/443 走 Nginx），见 12 章末尾。

### 10.4 三个关键页面验证清单

| 页面/功能 | 预期结果 |
|-----------|----------|
| `/login` 登录 | 能登录，密码错误有提示 |
| 总览 KPI（SOC/总压/电流/温度） | 有数据或显示"无数据"（取决于设备是否在线） |
| 单体电芯表格 | 按串数显示（6 串默认；设备上报几串显示几串） |
| 设备管理页 | 显示设备在线状态、规格信息 |
| 告警中心 / 历史曲线 / 报表 | 页面可打开、无报错 |
| 命令下发（重启/清告警等） | 返回"已下发"（设备在线时） |

---

## 11. systemd 守护

用 systemd 把 Dashboard 变成系统服务：开机自启、崩溃自动重启、日志统一管理。**替换掉本地的 watchdog/bat/vbs 方案**。

### 11.1 创建服务单元文件

```bash
nano /etc/systemd/system/bms-dashboard.service
```

粘贴以下内容（`bms` 用户或 root 均可，这里按 root 写）：

```ini
[Unit]
Description=BMS Dashboard (Flask + SocketIO)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
# 关键：必须在 app 目录启动（SQLite 相对路径、templates/static 都在这里）
WorkingDirectory=/opt/bms-dashboard/app
# 用 venv 里的 python 执行
ExecStart=/opt/bms-dashboard/venv/bin/python app.py
Restart=always
RestartSec=5
# 环境变量（可选：以后把 AK/SK 挪到这里读取，见 13.3）
# Environment=HUAWEI_AK=xxx
# Environment=HUAWEI_SK=xxx
# 日志限制，防止无限增长
StandardOutput=journal
StandardError=journal
SyslogIdentifier=bms-dashboard

[Install]
WantedBy=multi-user.target
```

保存（`Ctrl+O` 回车，`Ctrl+X`）。

### 11.2 启动并设为开机自启

```bash
systemctl daemon-reload
systemctl enable bms-dashboard     # 开机自启
systemctl start bms-dashboard      # 启动
systemctl status bms-dashboard     # 查看状态（按 q 退出）
```

预期输出里有 `Active: active (running)` 即成功。

### 11.3 常用管理命令

```bash
systemctl restart bms-dashboard    # 重启（改代码后用它）
systemctl stop bms-dashboard       # 停止
systemctl start bms-dashboard      # 启动
journalctl -u bms-dashboard -f     # 实时看日志（Ctrl+C 退出）
journalctl -u bms-dashboard -n 50  # 看最近 50 行日志
```

### 11.4 崩溃自愈验证（可选）

```bash
kill -9 $(pgrep -f "app.py")       # 模拟崩溃
sleep 10
systemctl is-active bms-dashboard  # 应输出 active（Restart=always 自动拉起）
```

---

## 12. Nginx 反向代理 + HTTPS 证书

目标：用户访问 `https://bms.example.com`（或 `http://公网IP`），Nginx 转发到 `127.0.0.1:5000`，并关掉 5000 直接暴露。

### 12.1 安装 Nginx

```bash
apt install -y nginx
systemctl enable nginx
systemctl start nginx
```

### 12.2 方案 A：有域名 + 想要 HTTPS（推荐）

把域名解析到云服务器公网 IP（云厂商 DNS 控制台添加 A 记录，例如 `bms` → `123.123.123.123`）。

**创建 Nginx 配置：**

```bash
nano /etc/nginx/sites-available/bms-dashboard
```

```nginx
server {
    listen 80;
    server_name bms.example.com;   # ← 改成你的域名

    # WebSocket (SocketIO 实时推送必须)
    location /socket.io/ {
        proxy_pass http://127.0.0.1:5000;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_read_timeout 3600s;
    }

    # 普通 HTTP 请求
    location / {
        proxy_pass http://127.0.0.1:5000;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }
}
```

**启用并测试：**

```bash
ln -s /etc/nginx/sites-available/bms-dashboard /etc/nginx/sites-enabled/
rm -f /etc/nginx/sites-enabled/default        # 删掉默认站点，避免冲突
nginx -t                                       # 配置语法检查，输出 ok 再继续
systemctl reload nginx
```

**申请免费 HTTPS 证书（Let's Encrypt）：**

```bash
apt install -y certbot python3-certbot-nginx
certbot --nginx -d bms.example.com            # 按提示填邮箱，同意条款
```

certbot 会自动改 Nginx 配置并配置自动续期。验证续期任务：

```bash
certbot renew --dry-run                        # 应显示 renew success / no failures
```

### 12.3 方案 B：没有域名，先用 HTTP（公网 IP 直连）

```bash
nano /etc/nginx/sites-available/bms-dashboard
```

```nginx
server {
    listen 80;
    server_name _;    # 匹配任意 Host

    location /socket.io/ {
        proxy_pass http://127.0.0.1:5000;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_read_timeout 3600s;
    }

    location / {
        proxy_pass http://127.0.0.1:5000;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

启用命令同上（12.2 的 ln -s / nginx -t / reload 三步）。

### 12.4 关掉 5000 直接暴露（安全收尾）

```bash
ufw delete allow 5000/tcp
```

同时在云控制台安全组删除 5000 入方向规则。之后只允许 80/443 进来，5000 仅本机访问。

### 12.5 最终验证（在你电脑浏览器）

```
https://bms.example.com/login      → 能打开登录页（方案 A）
http://123.123.123.123/login       → 能打开登录页（方案 B）
```

打开网页后按 `F12` → Console 无红色报错、网络面板能看到 `/socket.io/` 有 200/101 响应（WebSocket 升级成功），说明实时推送正常。

---

## 13. 安全加固

### 13.1 必做项

| 项 | 做法 |
|----|------|
| 改登录密码 | `DASHBOARD_PASSWORD` 改成强密码（9.1） |
| 关 5000 直连 | 只留 80/443（12.4） |
| SSH 加固 | `apt install -y fail2ban`，配置防暴力破解；有条件改用密钥登录并禁密码 |
| HTTPS | 有域名就上 certbot（12.2）；没域名至少用 HTTP + 改密码 |
| 更新系统 | 定期 `apt update && apt upgrade -y` |

### 13.2 华为云凭据保护（重要）

`app.py` 里明文写了 AK/SK。风险与对策：

- 风险：代码包外传、截图、git 提交 → SK 泄露 = 别人能读你的设备数据/下发命令；
- 对策 A（最简单）：AK/SK 只存在于服务器本地的 app.py，**不要**把 app.py 发到任何公开渠道；
- 对策 B（推荐）：创建华为云 **IAM 子账号**（仅授予 IoTDA 该设备相关权限，最小授权），用子账号的 AK/SK 部署，主账号 SK 不动。参考华为云文档"创建 IAM 用户 + 授权 IoTDA 只读/命令权限"；
- 对策 C（进阶）：把 AK/SK 放到环境变量或 `/opt/bms-dashboard/.env`（chmod 600），app.py 改为 `os.environ.get(...)` 读取（需要改少量代码，方法见附录 A）。

### 13.3 定期检查

```bash
fail2ban-client status sshd      # 看有没有被爆破
journalctl -u bms-dashboard -n 100 | grep -i error   # 看服务错误
uptime                          # 服务器负载
```

---

## 14. 数据备份与恢复（SQLite）

### 14.1 数据在哪

- 数据文件：`/opt/bms-dashboard/app/bms_history.db`（SQLite 单文件，历史数据 + 参数记录 + 审计日志）；
- 代码改动：`/opt/bms-dashboard/app/*.py`（改过配置就值得备份）。

### 14.2 用 sqlite3 在线备份（推荐，避免文件锁损坏）

```bash
apt install -y sqlite3
cd /opt/bms-dashboard/app
sqlite3 bms_history.db ".backup /root/backups/bms_history_$(date +%F).db"
```

> 不要直接 `cp bms_history.db`——服务运行中直接拷可能拿到半写入状态；`.backup` 是 SQLite 官方在线备份接口，安全。

### 14.3 定时自动备份（cron，每天凌晨 3 点）

```bash
mkdir -p /root/backups
crontab -e
```

粘贴（第一次会选编辑器，选 nano）：

```cron
# 每天 03:00 备份数据库，保留最近 14 份
0 3 * * * cd /opt/bms-dashboard/app && sqlite3 bms_history.db ".backup /root/backups/bms_history_$(date +\%F).db" && find /root/backups -name "bms_history_*.db" -mtime +14 -delete
```

验证 cron 配置：

```bash
crontab -l                    # 应显示刚才那行
```

### 14.4 恢复数据（搬家/事故后）

```bash
systemctl stop bms-dashboard
cd /opt/bms-dashboard/app
cp /root/backups/bms_history_2026-08-07.db bms_history.db
chmod 644 bms_history.db
systemctl start bms-dashboard
```

### 14.5 从本机迁移历史数据（可选）

想把电脑上的旧 `bms_history.db` 带到云上：本地电脑执行

```bash
scp "D:\esp32project\BMS\BMS System\tools\dashboard\bms_history.db" root@123.123.123.123:/tmp/
```

服务器上（先停服务）：

```bash
systemctl stop bms-dashboard
cp /tmp/bms_history.db /opt/bms-dashboard/app/bms_history.db
chmod 644 /opt/bms-dashboard/app/bms_history.db
systemctl start bms-dashboard
```

> 注意：旧库表结构若缺新列（如 `cells_json`），服务启动时会自动 `ALTER TABLE` 补齐（database.py 的 init_db 兼容逻辑），无需手动处理。迁移后建议用第 10.4 的清单验证一遍。

---

## 15. 升级 / 回滚 / 迁移

### 15.1 代码升级流程（日常改代码后）

```bash
# 1) 备份当前版本（可选但推荐）
cp -r /opt/bms-dashboard/app /root/backups/app_$(date +%F)

# 2) 备份数据库
sqlite3 /opt/bms-dashboard/app/bms_history.db ".backup /root/backups/bms_history_$(date +%F).db"

# 3) 同步新代码（你电脑上执行；rsync 增量最快）
rsync -avz "D:/esp32project/BMS/BMS System/tools/dashboard/" root@123.123.123.123:/opt/bms-dashboard/app/

# 4) 服务器上装依赖（如果有新增依赖）
cd /opt/bms-dashboard && source venv/bin/activate && pip install -r app/requirements.txt

# 5) 语法检查 + 重启
python -m py_compile app/app.py app/database.py app/iotda_client.py app/mqtt_client.py && \
systemctl restart bms-dashboard && \
systemctl status bms-dashboard
```

### 15.2 回滚

```bash
rm -rf /opt/bms-dashboard/app
cp -r /root/backups/app_2026-08-07 /opt/bms-dashboard/app
systemctl restart bms-dashboard
```

### 15.3 整机迁移（换服务器）

1. 新服务器按第 5~8 章初始化；
2. 拷贝代码 + 数据库 + venv（或重新装依赖）：
   ```bash
   # 老服务器打包
   cd /opt && tar czf bms-dashboard.tgz bms-dashboard --exclude='bms-dashboard/venv'
   scp bms-dashboard.tgz root@新IP:/opt/
   # 新服务器解压 + 建 venv 装依赖 + 按第 11/12 章配置 systemd 与 Nginx
   cd /opt && tar xzf bms-dashboard.tgz && cd bms-dashboard && python3 -m venv venv && source venv/bin/activate && pip install -r app/requirements.txt
   ```

---

## 16. 监控与日志

### 16.1 日志查看

```bash
journalctl -u bms-dashboard -f            # 实时跟踪
journalctl -u bms-dashboard --since today # 今天的日志
journalctl -u bms-dashboard -p err        # 只看错误级
```

### 16.2 防止日志无限增长

systemd journal 默认已限制（约 10% 磁盘）。确认：

```bash
journalctl --disk-usage
journalctl --vacuum-size=200M             # 手动清理到 200MB
```

### 16.3 服务健康自检脚本（可选，替代本地的 watchdog）

新建 `/root/check_bms.sh`：

```bash
#!/bin/bash
# BMS Dashboard 健康检查: 服务状态 + 本地 5000 + 公网(可选)
if ! systemctl is-active --quiet bms-dashboard; then
    echo "$(date) dashboard 挂了, 重启" >> /root/check_bms.log
    systemctl restart bms-dashboard
fi
if ! curl -sf -o /dev/null http://127.0.0.1:5000/login; then
    echo "$(date) 5000 无响应, 重启" >> /root/check_bms.log
    systemctl restart bms-dashboard
fi
```

```bash
chmod +x /root/check_bms.sh
crontab -e    # 每 5 分钟跑一次
# */5 * * * * /root/check_bms.sh
```

> systemd `Restart=always` 已覆盖进程崩溃；这个脚本额外兜底"假死"（进程在但端口不响应）的情况。

### 16.4 磁盘/内存告警（可选）

```bash
apt install -y htop
htop     # 看内存/CPU
df -h    # 看磁盘
```

---

## 17. ESP32 固件侧调整

### 17.1 数据上报：无需改动 ✅

ESP32 的数据是上报到华为云 IoTDA 的（`sys_mqtt.c`），Dashboard 从华为云拉取。**云部署后这条链路完全不变**，ESP32 的 MQTT 配置（broker/TLS/凭据）一行都不用动。

### 17.2 OTA 远程升级：需要改 OTA 地址 ⚠️（仅当你想远程升级 ESP32）

当前 `bms_config.h` 里 OTA 版本 URL 指向 Dashboard 的地址。云部署后要改成云服务器：

```c
// bms_config.h (ESP32 工程)
// 原来(局域网)可能类似:
// #define BMS_OTA_VERSION_URL "http://192.168.x.x:5000/api/ota/version.json"
// 改成云服务器:
#define BMS_OTA_VERSION_URL "https://bms.example.com/api/ota/version.json"
// 无域名用公网 IP:
// #define BMS_OTA_VERSION_URL "http://123.123.123.123:5000/api/ota/version.json"
```

**前提条件**：
1. ESP32 能访问公网（能连华为云 IoTDA 就说明可以）；
2. 云服务器 5000 端口对 ESP32 可达：若走 Nginx，需给 OTA 路径也配好反代（第 12 章配置里的 `location /` 已覆盖 `/api/ota/*`，✅ 无需额外配置）；若 ESP32 直连 5000，则安全组必须放行 5000（与 12.4 冲突，二选一：要么 ESP32 走 80/443 反代，要么放行 5000）。

**固件文件放哪**：Dashboard 从两个位置找固件（`app.py` 的 `_find_firmware_bin()`）：
1. `/opt/bms-dashboard/app/firmware/bms.bin`（推荐，放这里）
2. `/opt/bms-dashboard/../build/bms.bin`（ESP-IDF 编译产物，云上一般没有）

```bash
mkdir -p /opt/bms-dashboard/app/firmware
# 把编译好的 bms.bin 传到服务器
scp "D:\esp32project\BMS\BMS System\build\bms.bin" root@123.123.123.123:/opt/bms-dashboard/app/firmware/
```

3. 改完重新编译烧录 ESP32（OTA 地址变更需要本地烧录一次，之后才能远程 OTA）。

### 17.3 云部署对固件的其他影响

| 功能 | 影响 |
|------|------|
| 实时数据上报 | 无影响 |
| 参数下发/命令 | 无影响（Dashboard → IoTDA → ESP32） |
| OTA | 需按 17.2 改地址 + 传固件 |
| 设备信息(IP/MAC) | Dashboard 展示的是 ESP32 上报的 IP/MAC，无影响 |

---

## 18. 故障排查手册

按"症状 → 检查 → 解决"组织，遇到问题按顺序查。

### 18.1 网页打不开（连接超时/拒绝）

| # | 检查 | 命令/方法 | 解决办法 |
|---|------|-----------|----------|
| 1 | 服务是否在跑 | `systemctl status bms-dashboard` | 没跑则 `systemctl start bms-dashboard` |
| 2 | 本地 5000 是否通 | `curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:5000/login` | 不通看日志 `journalctl -u bms-dashboard -n 50` |
| 3 | Nginx 是否在跑 | `systemctl status nginx` | `systemctl start nginx` |
| 4 | 防火墙是否放行 | `ufw status` + 云控制台安全组 | 放行 80/443（或 5000 临时调试） |
| 5 | 域名解析是否正确 | 电脑上 `nslookup bms.example.com` | 解析到公网 IP；A 记录别填错 |
| 6 | 端口是否被占用 | `ss -tlnp \| grep -E '5000|80|443'` | 杀掉占用进程或换端口 |

### 18.2 网页能打开但一直"等待数据 / 无数据"

| # | 检查 | 命令/方法 | 解决办法 |
|---|------|-----------|----------|
| 1 | IoTDA API 是否可访问 | 看启动日志有无 `[IoTDA] 客户端已启动` | 服务器能出网吗：`curl -s -o /dev/null -w "%{http_code}" https://iotda.cn-south-4.myhuaweicloud.com` |
| 2 | 凭据是否正确 | 对照 app.py 的 AK/SK/PROJECT_ID/REGION/DEVICE_ID | 从华为云控制台核对（重点：SK 是否被误改/截断） |
| 3 | 设备是否在线 | 华为云 IoTDA 控制台 → 设备列表 | 设备离线则数据就是无；确认 ESP32 在线 |
| 4 | 数据年龄判定 | 页面 `F12` → Network → `/api/status` 响应里的 `_debug.no_data_result` | 设备离线/数据陈旧 >200s 判定无数据 |
| 5 | 影子数据是否为空 | `curl http://127.0.0.1:5000/api/debug/shadow`（登录后可访问） | 看 `live_shadow` 字段是否含数据 |

### 18.3 WebSocket 不实时（页面要手动刷新才有数据）

| # | 检查 | 解决 |
|---|------|------|
| 1 | Nginx 是否配置了 `/socket.io/` 的 Upgrade 头 | 用第 12 章的配置（`proxy_set_header Upgrade/Connection`） |
| 2 | 浏览器 Console 是否有 400/403 | 检查 Nginx 日志 `tail -f /var/log/nginx/error.log` |
| 3 | eventlet 是否正常 | 启动日志无 eventlet 报错；`pip show eventlet` 版本 ≥ 0.35 |

### 18.4 500 错误 / 页面报服务器错误

```bash
journalctl -u bms-dashboard -n 200 | grep -A 10 "Error"
```

常见原因与处理：
- **SQLite 锁**（`database is locked`）：并发写冲突，一般自愈；若频繁出现，检查是否多个 Dashboard 实例在跑（`pgrep -f app.py` 应只有 1 个）。
- **表结构旧**：`cells_json` 列缺失 → 重启服务自动 ALTER（database.py init_db 兼容逻辑），若失败手动加列：
  ```bash
  sqlite3 /opt/bms-dashboard/app/bms_history.db "ALTER TABLE bms_data ADD COLUMN cells_json TEXT;"
  ```
- **依赖缺失**：`ModuleNotFoundError` → 按第 8 章补装。

### 18.5 登录不了 / 密码错误

- 检查 app.py `DASHBOARD_PASSWORD` 是否为改后的值（注意引号/空格）；
- 改完要 `systemctl restart bms-dashboard`；
- 会话 10 分钟无活动会自动过期，重新登录即可。

### 18.6 服务器重启后网页又挂

```bash
systemctl enable bms-dashboard    # 确认已设置开机自启
systemctl is-enabled bms-dashboard  # 应输出 enabled
```

### 18.7 一键诊断脚本（按顺序贴，输出贴给 AI/同事）

```bash
echo "== 1. 服务状态 =="; systemctl status bms-dashboard --no-pager | head -12
echo "== 2. 端口 =="; ss -tlnp | grep -E '5000|80|443'
echo "== 3. 最近日志 =="; journalctl -u bms-dashboard -n 30 --no-pager
echo "== 4. 本地 HTTP =="; curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:5000/login
echo "== 5. IoTDA 出网 =="; curl -s -o /dev/null -w "%{http_code}\n" --max-time 10 https://iotda.cn-south-4.myhuaweicloud.com
echo "== 6. 磁盘/内存 =="; df -h / | tail -1; free -m | head -2
```

---

## 19. 成本估算

| 项目 | 月成本（约） | 说明 |
|------|--------------|------|
| 华为云轻量 1核2G（按需→包年） | 约 ¥30~60/月（包年约 ¥300~600/年） | 不同规格差异大，以官网为准 |
| 域名（可选） | 约 ¥30~80/年 | 如 `bms.example.com`，.com/.cn 等 |
| HTTPS 证书 | ¥0 | Let's Encrypt 免费 |
| 带宽流量 | 含在套餐内（轻量）或按量 | 网页流量小，基本可忽略 |
| 华为云 IoTDA | 免费额度内（设备数少） | 超出按消息量计费，参考官方价 |

> 结论：一年总成本约 ¥400~700，换来 7×24 稳定访问，比一直开着电脑省心省钱（电费都不止）。

---

## 20. 部署前后检查清单

### 部署前（✅ 打勾核对）

- [ ] 已购买云服务器并记下公网 IP/root 密码
- [ ] 已 SSH 登录成功
- [ ] 已修改 root 密码
- [ ] ufw + 安全组已放行 22/80/443
- [ ] Python 3.8+ 已确认
- [ ] 代码包已上传并解压到 `/opt/bms-dashboard/app`
- [ ] venv 已建、依赖已装、`deps OK`
- [ ] app.py 密码已改强密码
- [ ] AK/SK/PROJECT_ID/REGION/DEVICE_ID 已核对
- [ ] `python -m py_compile` 语法通过
- [ ] 前台启动无报错、本地 `/login` 返回 200/302

### 部署后（上线验收）

- [ ] systemd 服务 `active (running)` 且 `enabled`
- [ ] 公网能打开登录页（域名或 IP）
- [ ] 登录后能看到实时数据/设备状态（IoTDA 链路通）
- [ ] 单体表格按串数显示正常
- [ ] 命令下发返回"已下发"
- [ ] `/socket.io/` WebSocket 升级成功（F12 确认）
- [ ] 5000 端口已关闭外网直连（只留 80/443）
- [ ] 每日备份 cron 已配置（`crontab -l` 确认）
- [ ] fail2ban 已装（可选）
- [ ] ESP32 OTA 地址已改（如需远程 OTA）并传固件

---

## 21. 附录：常用运维命令速查

### 服务管理

```bash
systemctl status bms-dashboard     # 状态
systemctl restart bms-dashboard    # 重启（改代码后）
systemctl stop/start bms-dashboard
journalctl -u bms-dashboard -f     # 实时日志
```

### Nginx

```bash
nginx -t                           # 配置语法检查
systemctl reload nginx             # 平滑重载
tail -f /var/log/nginx/error.log   # 错误日志
```

### 备份

```bash
sqlite3 /opt/bms-dashboard/app/bms_history.db ".backup /root/backups/bms_history_$(date +%F).db"
```

### 同步代码（你电脑上）

```bash
rsync -avz "D:/esp32project/BMS/BMS System/tools/dashboard/" root@123.123.123.123:/opt/bms-dashboard/app/
```

### 附录 A：把 AK/SK 改为环境变量读取（安全增强，可选）

1. 修改 `app.py` 顶部（约 84~88 行）：

```python
import os
# P0-2 安全加固: 严禁把真实 AK/SK 写进仓库! 以下默认值为空, 必须通过环境变量注入
AK         = os.environ.get("HUAWEI_AK", "")
SK         = os.environ.get("HUAWEI_SK", "")
PROJECT_ID = os.environ.get("HUAWEI_PROJECT_ID", "")
REGION     = os.environ.get("HUAWEI_REGION", "cn-south-4")
DEVICE_ID  = os.environ.get("HUAWEI_DEVICE_ID", "")
```

2. 创建环境变量文件（权限收紧）：

```bash
echo 'HUAWEI_AK=你的AK' >> /etc/bms-dashboard.env
echo 'HUAWEI_SK=你的SK' >> /etc/bms-dashboard.env
chmod 600 /etc/bms-dashboard.env
```

3. systemd 服务里引用（编辑 `/etc/systemd/system/bms-dashboard.service`，在 `[Service]` 加）：

```ini
EnvironmentFile=/etc/bms-dashboard.env
```

4. 重载生效：

```bash
systemctl daemon-reload && systemctl restart bms-dashboard
```

---

*文档结束 · 遇到任何一步报错，把报错原文 + 18.7 诊断脚本输出发出来，我可以帮你定位。*

---
