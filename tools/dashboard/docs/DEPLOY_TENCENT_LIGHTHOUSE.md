# BMS Dashboard → 阿里云服务器部署指南（你的服务器 <YOUR_ECS_IP>）

> **适用场景**：你新买的阿里云服务器（IP <YOUR_ECS_IP>，已装宝塔 Linux 面板 9.2.0）  
> **目标**：一条命令起全套服务——仪表盘 + IoTDA桥接 + 自建MQTT broker  
> **链路不变**：ESP32 → 华为云IoTDA → (桥接拉取) → 你的阿里云服务器 → 浏览器/手机

---

## 第0步：登录方式（阿里云 ECS）

本服务器 `<YOUR_ECS_IP>` 主机名 `iZ...` 为 **阿里云 ECS**。公网 22 端口可能被安全组/前端拦截（`ssh`/`scp` 报 `Connection reset`），
因此用 **阿里云控制台的「Workbench / 云助手」网页终端** 登录（走内网代理，不依赖公网 22）；若安全组已放行 22 也可直接 SSH。

> 后续所有命令都在 **Workbench 网页终端** 里粘贴执行，不在你电脑的终端执行。

---

> ⚠️ **80 端口冲突预警**：本机已装 nginx（`/etc/nginx/conf.d`、`/etc/nginx/sites-enabled` 存在），默认会占用 :80。
> 而 `docker-compose.yml` 把仪表盘映射到宿主机 `80:5000`，若 nginx 正在运行，`docker compose up` 会报 `port is already allocated`。
> 部署前先确认 80 空闲：`ss -ltnp | grep ':80'`；若被 nginx 占用且只是默认欢迎页，停掉它：
> `systemctl stop nginx && systemctl disable nginx`（专用 BMS 服务器可放心停）。需要 HTTPS 时再让 nginx 反代 `127.0.0.1:5000`。

## 第1步：装 Docker（如果没有）

登录后先检查：

```bash
docker --version && docker compose version
```

如果有版本号输出就跳过本步。没有的话执行：

```bash
# 一键安装 Docker + Compose v2（国内源，速度快）
curl -fsSL https://get.docker.com | sh
systemctl enable docker && systemctl start docker
docker --version    # 应输出 Docker version xx.x.x
```

---

## 第2步：上传代码（经控制台/OrcaTerm，不用 scp）

部署包已在本机打好：`D:\bms-dashboard.tar.gz`（顶层目录为 `dashboard/`，含 `dashboard.env` 与 `setup.sh`）。
用 **OrcaTerm / 宝塔面板** 的文件上传功能（拖拽或上传按钮）把它传到服务器 `/opt/`。

然后在 **OrcaTerm 终端** 里解压并摆到工作目录：

```bash
mkdir -p /opt/bms-dashboard && cd /opt/bms-dashboard
tar xzf /opt/bms-dashboard.tar.gz        # 解出 dashboard/ 子目录
cp -r dashboard/* . && cp dashboard/.dockerignore . 2>/dev/null; true
ls deploy/docker-compose.yml backend/app.py backend/dashboard.env setup.sh   # ✅ 四个都在就对了
```

> 💡 若控制台不支持大文件上传，可改用宝塔面板「文件」→ `/opt` → 上传 `bms-dashboard.tar.gz`（同样拖拽即可）。

---

## 第3步：运行一键部署脚本（推荐）

`setup.sh` 已自动完成「建密钥 + 建数据库 + 建 MQTT 密码 + 构建启动 + 验证」，**一条命令即可**：

```bash
cd /opt/bms-dashboard
bash setup.sh
```

脚本结束会打印三条链路的验证结果（容器状态 / `login` HTTP 码 / 桥接日志 / 1883 监听）。
看到三个容器 `Up`、且 `bms-bridge` 每 10s 一条「已发布」即成功。

> ⚠️ `dashboard.env` 已随包上传（含华为云 AK/SK）。若报错找不到，检查 `backend/dashboard.env` 是否存在。
> ⚠️ **不要手动建 `deploy/mosquitto/config/passwd`**——它由脚本用 `mosquitto_passwd` 生成；手写会导致 Mosquitto 启动即崩。

---

## 第4步：创建 MQTT broker 用户（已由 setup.sh 自动完成，下面为等效手动命令）

```bash
# 用 mosquitto 容器的命令行工具创建用户（student / 你的密码）
# 先临时启动容器生成密码文件：
docker run --rm -v "$(pwd)/deploy/mosquitto/config:/mosquitto/config" \
  eclipse-mosquitto:2.0 \
  mosquitto_passwd -c /mosquitto/config/passwd student '<BROKER_PASSWORD>'

# 验证密码文件已生成
cat deploy/mosquitto/config/passwd    # 应看到一行加密后的用户条目
```

> 🔒 **上线后请改密码**：把上面命令里的 `'<BROKER_PASSWORD>'` 换成强密码，  
> 同时改 `dashboard.env` 或 docker-compose.yml 里的 `BMS_BRIDGE_MQTT_PASS`。

---

## 第5步：构建并启动全套服务（已由 setup.sh 自动完成）

```bash
cd /opt/bms-dashboard

# ⚠️ 部署前必检：密码文件必须已存在（第4步生成），否则 mosquitto 容器会启动即崩
ls -l deploy/mosquitto/config/passwd   # 应能看到 student:xxxx 一行加密条目

# 构建镜像 + 启动三个容器（仪表盘 + 桥接 + broker）
docker compose -f deploy/docker-compose.yml up -d --build
```

首次构建约需 2-5 分钟（下载 Python 基础镜像 + Mosquitto 镜像 + pip 装依赖）。

看到以下输出就成功了：

```
 ✔ Container bms-mosquitto    Started    ✓
 ✔ Container bms-dashboard    Started    ✓
 ✔ Container bms-bridge       Started    ✓
```

---

## 第6步：验证三条链路

### 6.1 检查容器状态

```bash
docker ps              # 应看到 3 个容器都是 Up
docker compose -f deploy/docker-compose.yml logs -f   # 实时看所有日志
```

### 6.2 验证仪表盘

```bash
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:80/login
# 输出 200 或 302 = 正常
```

浏览器访问：**<http://<YOUR_ECS_IP>/>** （或 \*\*<http://<YOUR_ECS_IP>/login\*\*）>

- 能打开登录页 → ✅ 仪表盘 OK
- 输入密码（默认 `bms123`，在 dashboard.env 里改过就用改后的）→ 能看到数据 → ✅ IoTDA 链路通

### 6.3 验证 MQTT broker + 桥接

```bash
# 看桥接日志（应每10s发布一次数据）
docker logs bms-bridge --tail 20

# 用自带的 mosquitto 客户端订阅验证（另开一个 SSH 窗口）
docker exec -it bms-mosquitto mosquitto_sub -h localhost -u student -P '<BROKER_PASSWORD>' -t 'bms' -v
# 应每 10 秒收到一行 JSON 数据（soc, current, pack_v 等）
```

### 6.4 手机/外部验证

手机连 **同一个 WiFi 或 4G**，用 [MQTTX](https://mqttx.app/)（免费 App）：

- Host: `<YOUR_ECS_IP>`
- Port: `1883`
- Username: `student`
- Password: `<BROKER_PASSWORD>`
- Topic: `bms`
- 点连接 → 订阅 → 每 10 秒收到数据 = ✅ 全套打通！

> ⚠️ 手机连时确保**阿里云防火墙放行了 1883 端口**：  
> 控制台 → 轻量服务器 → 防火墙 → 添加规则 → TCP / 1883 / 全部 IPv4

---

## 第7步：安全收尾（必做！）

| # | 操作            | 命令/路径                                                                                                                                                                 |
| - | ------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 | **改仪表盘登录密码**  | 编辑 `backend/dashboard.env` → `BMS_DASH_PASSWORD=强密码` → `docker compose restart bms-dashboard`                                                                         |
| 2 | **改 MQTT 密码** | `docker exec -it bms-mosquitto mosquitto_passwd /mosquitto/config/passwd student '新密码'` → 同步改 compose 里的 `BMS_BRIDGE_MQTT_PASS` → `docker compose restart bms-bridge` |
| 3 | **关掉不需要的端口**  | 阿里云防火墙只留 **22(SSH) + 80(HTTP) + 1883(MQTT)**，其余删掉                                                                                                                     |
| 4 | **宝塔安全设置**    | 宝塔面板 → 安全 → 改面板端口(8888→随机大端口) + 改面板用户名密码 + 绑定授权IP                                                                                                                     |

---

## 常用运维命令速查

```bash
# ===== 日常操作 =====
docker compose -f deploy/docker-compose.yml logs -f          # 看日志(Ctrl+C退出)
docker compose -f deploy/docker-compose.yml restart          # 重启全部
docker compose -f deploy/docker-compose.yml down              # 停止全部
docker compose -f deploy/docker-compose.yml up -d             # 启动

# ===== 单独重启某个服务 =====
docker restart bms-dashboard      # 重启仪表盘
docker restart bms-bridge         # 重启桥接
docker restart bms-mosquitto      # 重启broker

# ===== 更新代码后重新部署 =====
# 电脑上重新上传 → 服务器上:
docker compose -f deploy/docker-compose.yml up -d --build

# ===== 备份数据库 =====
cp /opt/bms-dashboard/backend/bms_history.db /root/backups/bms_$(date +%F).db

# ===== 一键诊断 =====
echo "== 容器 =="; docker ps --format "table {{.Names}}\t{{.Status}}"
echo "== 端口 =="; ss -tlnp | grep -E '80|1883|5000'
echo "== 仪表盘 =="; curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:80/login
echo "== 桥接日志 =="; docker logs bms-bridge --tail 5
echo "== 磁盘 =="; df -h / | tail -1
```

---

## 架构总览（部署完成后）

```
  手机/平板/其他PC
       │
       ├── http://<YOUR_ECS_IP>  ──→  Nginx/直连 :80 ──→  bms-dashboard(Flask:5000)
       │                                                    │
       │                                            华为云IoTDA API(轮询设备影子)
       │                                                    ↑
       │                                              ESP32 设备(MQTT上报)
       │
       └── mqtt://<YOUR_ECS_IP>:1883 ──→  bms-mosquitto(broker)
                                                ↑
                                          bms-bridge(每10s从IoTDA拉取→publish到bms主题)
```

**你电脑现在可以关机了** —— 所有服务都在阿里云轻量上 24 小时运行。

---

## 故障排查

| 症状        | 排查命令                                      | 常见原因            |
| --------- | ----------------------------------------- | --------------- |
| 页面打不开     | `docker ps` 看 bms-dashboard 是否 Up         | 容器没起来 / 80端口没放行 |
| 页面无数据     | `docker logs bms-dashboard \| grep IoTDA` | AK/SK错 / 设备离线   |
| MQTT没数据   | `docker logs bms-bridge --tail 20`        | broker没启动 / 密码错 |
| 手机连不上MQTT | 阿里云防火墙检查1883                              | 防火墙没放行1883      |

---

*文档日期：2026-08-14 · 基于阿里云轻量 2C2G + 宝塔Linux 9.2.0 实测*
