# BMS Dashboard · Docker 部署变体（与 DEPLOY_CLOUD.md 二选一）

> 本文是 `DEPLOY_CLOUD.md` 的**补充**，提供**容器化**部署路径。
> 如果你更习惯 venv + systemd + Nginx 的传统方式，请直接看 `DEPLOY_CLOUD.md`（那套 21 章文档更详尽）。
> 两条路最终效果相同：7×24 公网可访问、数据来自华为云 IoTDA、ESP32 一行不用改。

## 为什么用 Docker 版
- 一条 `docker compose up -d --build` 完成部署，不污染服务器 Python 环境；
- 数据库/密钥用 bind mount 挂进去，**备份 = 拷文件**（对应你之前关心的“能否完全还原”）；
- 换服务器：把 `tools/dashboard/` 整目录拷走，`docker compose up` 即恢复。

## 前置条件
- 一台 Linux VPS（推荐 Ubuntu 22.04），已装 **Docker + docker compose v2**；
- 云控制台安全组入方向放行 **TCP 80**（源站 HTTP；TLS 由 Cloudflare 边缘终止，无需放行 443/5000）；
- `tools/dashboard/` 整目录（含 `backend/`、`frontend/`）已传到服务器。

## 本文提供的文件（实际位于 `deploy/` 子目录）
- `deploy/Dockerfile`：基于 `python:3.11-slim` 构建 Flask 镜像，安装 `backend/requirements.prod.txt`；
- `deploy/docker-compose.yml`：映射 `80:5000`、`restart: unless-stopped`、挂载密钥与数据库（构建上下文为 `tools/dashboard/`）；
- `.dockerignore`：位于 `tools/dashboard/` 根，排除密钥/数据库/缓存，避免写进镜像层。

## 部署步骤
1. 上传代码：
   ```bash
   scp -r tools/dashboard/ user@<VPS公网IP>:~/bms-dashboard
   ```
2. 进入目录：`cd ~/bms-dashboard`
3. 确保数据库文件存在（**全新部署**）：`touch backend/bms_history.db`
   - 若要**保留历史**，把原来的 `backend/bms_history.db` 拷过来覆盖即可。
4. 构建并启动：`docker compose -f deploy/docker-compose.yml up -d --build`
   （compose 已移到 `deploy/`，后续命令都带 `-f deploy/docker-compose.yml`）
5. 看日志：`docker compose logs -f`
6. 本机验证：`curl -s http://127.0.0.1/ | head` 应返回登录页 HTML。

## Cloudflare 配置（已有域名 bms0605.dpdns.org）
- **DNS**：把 A 记录指向 **VPS 公网 IP**，橙色云（代理）打开；
- **SSL/TLS → 概述**：模式设为 **Flexible**（边缘 HTTPS，源站 HTTP:80，无需源站证书）；
- 不需要 certbot、不需要源站 Nginx（`DEPLOY_CLOUD.md` 第 12 章可整体跳过）。

## 华为云 IoTDA 推送地址
- 若启用 IoTDA 主动推送（`api_iotda_push`），把推送/订阅端点改为：
  `http://<YOUR_ECS_IP>:5000/api/iota_push`（或 `http://<VPS公网IP>/api/iota_push`）；
- **仅轮询影子模式则不用改**，云服务器能出公网访问华为云 API 即可（ESP32 上报链路完全不变）。

## 备份与还原
- **备份**：`docker compose -f deploy/docker-compose.yml stop` 后拷 `backend/bms_history.db` + `backend/dashboard.env` + `backend/.secret_key`；
- **还原**：拷回同路径，`docker compose -f deploy/docker-compose.yml up -d` 即恢复历史与凭据。
- 这与你之前问的“整目录复制能否完全还原”一致——容器化让这一点更干净、不漏隐藏文件。

## 成本（年付参考，2026）
- 腾讯云 / 阿里云 **轻量应用服务器·香港** 2C2G/50G ≈ **¥300–600/年**（免备案、国内低延迟）；
- 华为云 **轻量·香港/新加坡** 类似（与 IoTDA 同生态，控制台统一）；
- Oracle Always Free = **¥0**（需信用卡、区域选首尔/东京，延迟中等）。
- **多项目共享（团队场景）**：规格升到 **4C8G / 80–100G SSD** 起步，约 **¥800–1500/年**；每多 2–3 个项目再 +2C4G。
- 选“包年”而非“按需”：年付通常比月付便宜 20–40%，且本就是 24/7 服务。

## 团队共用与多项目管理（进阶 · 多项目 + 独立账号 + Traefik）

> 你已确认的三项决策：**①服务器除 BMS 看板外还要跑团队其他项目（多项目共享）；②登录方式选 B 独立账号（每人独立账号+角色）；③反向代理用 Traefik（按子域名自动路由）**。下面把最终形态定下来。

### 1. 最终服务器规格（年付）
- 单看板：2C4G/50G ≈ ¥300–600/年；
- **多项目（当前选择）**：**4C8G / 80–100G SSD 起步 ≈ ¥800–1500/年**，项目每多 2–3 个再 +2C4G；
- 推荐：腾讯云 / 阿里云 轻量·香港 4C8G；华为云同生态；Oracle Always Free 4C24G ARM（仍可选，需信用卡）。

### 2. 架构拓扑
```
                 ┌──────────────────────────────────────────────────┐
   公网 ───────► │  VPS (Ubuntu 22.04 + Docker)                      │
   (Cloudflare  │                                                   │
    Flexible)   │   Traefik :80  (按 Host 头路由 + 访问日志)         │
                │        │              │              │             │
                │        ▼              ▼              ▼             │
                │  bms.xxx.com    projb.xxx.com   proje.xxx.com     │
                │  bms-dashboard    项目B容器       项目C容器         │
                │  (本次 Docker 版) (各自镜像)    (各自镜像)          │
                │  资源上限 1C/512M  ...                            │
                └──────────────────────────────────────────────────┘
```
每个项目一个容器：独立端口（容器内部）、独立 volume（DB/配置）、独立 `deploy.resources` 资源上限，互不拖垮。

### 3. Traefik 反代配置（示例）
> 单项目时直接用原 `deploy/docker-compose.yml`（`80:5000`）即可；**多项目时改用下面的 Traefik 栈**，二者都占主机 80，二选一、别同时跑。

保存为 `deploy/traefik-stack.yml`（与 `deploy/docker-compose.yml` 分开放，避免混淆）：
```yaml
# deploy/traefik-stack.yml —— 多项目共享用(替代单项目 docker-compose.yml)
# 用法: docker compose -f deploy/traefik-stack.yml up -d --build
# 注意: 本文件占用主机 80, 与原 deploy/docker-compose.yml 的 "80:5000" 冲突, 二选一!
services:
  traefik:
    image: traefik:v3.1
    container_name: traefik
    restart: unless-stopped
    command:
      - "--providers.docker=true"
      - "--providers.docker.exposedbydefault=false"
      - "--entrypoints.web.address=:80"
      - "--accesslog=true"
    ports:
      - "80:80"                       # 仅 80; TLS 由 Cloudflare 边缘(Flexible)终止
    volumes:
      - "/var/run/docker.sock:/var/run/docker.sock:ro"
    networks: [proxy]

  bms-dashboard:                     # 复用在 deploy/Dockerfile 构建的镜像
    build:
      context: ..
      dockerfile: deploy/Dockerfile
    container_name: bms-dashboard
    restart: unless-stopped
    environment: [TZ=Asia/Shanghai]
    volumes:
      - ../backend/dashboard.env:/app/backend/dashboard.env:ro
      - ../backend/.secret_key:/app/backend/.secret_key:ro
      - ../backend/bms_history.db:/app/backend/bms_history.db
    # 不再 map 80:5000 —— 端口由 Traefik 按 Host 头内部路由
    labels:
      - "traefik.enable=true"
      - "traefik.http.routers.bms.rule=Host(`bms.example.com`)"
      - "traefik.http.services.bms.loadbalancer.server.port=5000"
    deploy:
      resources:
        limits: { cpus: "1.0", memory: 512M }   # 资源上限, 防单项目吃满整机
    networks: [proxy]

  # 项目 B / C: 复制上面 bms-dashboard 块, 改 container_name / build 上下文 / Host 子域名 / 资源上限
  # projb:
  #   ...

networks:
  proxy:
    driver: bridge
```
- 把 `bms.example.com` 换成你的真实子域名；Cloudflare 里给该子域名加 A 记录指向 VPS IP（橙色云 + Flexible，和现在一样）。
- **新增项目 = 复制一个 service 块 + 一个新的子域名 label，零改 Traefik 配置。**
- （可选）要端到端 TLS，可给 Traefik 加 `websecure` entrypoint + ACME（Cloudflare DNS 校验），但 Flexible 模式下不是必须。

### 4. 团队安全加固（OS 层 · 必做）
- **独立账号**：每人一个 sudo 账号，**禁止 root 直登 SSH**；看板本身已按角色门控（见下）。
- **SSH 仅密钥登录**：`/etc/ssh/sshd_config` 设 `PasswordAuthentication no` + `PubkeyAuthentication yes`，改完 `systemctl restart sshd`。
- **防火墙**：`ufw allow 22,80`（Cloudflare 场景源站只暴露 22+80，443 由边缘终结）；Cloudflare 可设「仅允许 Cloudflare IP」进一步收敛 80。
- **fail2ban**：防护 SSH 爆破；看板登录已有 per-IP 失败锁定（5 次/10 分钟，`app.py`）。
- **自动安全更新**：`unattended-upgrades`。
- 上述 OS 层步骤详见 `DEPLOY_CLOUD.md` 第 5/13/16 章。

### 5. 登录模型 B：独立账号（落地步骤）
看板后端已具备完整多用户框架：`users` 表 + 角色(`admin`/`operator`/`viewer`) + 审计日志；首次启动 `ensure_default_admin()` 自动建 `admin/admin123`（`app.py` L2397）。**但当前 Web UI 还没有「管理员建账号」页面**，所以先用随仓库提供的服务端脚本 `backend/manage_users.py` 批量建/管：

```bash
cd tools/dashboard/backend
python manage_users.py list                                   # 列出账号(首次会自动建 admin)
python manage_users.py add <用户名> <密码(>=8位)> <admin|operator|viewer> [显示名]
python manage_users.py reset <用户名> <新密码(>=8位)>          # 成员忘密/入职初始化
python manage_users.py delete <用户名>                         # 离职禁用(不能删最后一个 admin)
```

落地顺序：
1. 部署后首次登录用 `admin/admin123`，**立刻改密**（设置页或 `manage_users.py reset admin <新密码>`）；
2. 为每位成员 `add` 独立账号，按职责给 `operator`(可下发指令) / `viewer`(只读) / `admin`(全权)；
3. **建完任意账号后，旧「共享密码(DASHBOARD_PASSWORD)回退登录」自动失效**（代码逻辑：仅当用户表为空才允许回退），无需手动关闭；
4. 成员离职：`delete <用户名>`。

- 角色权限：`admin` 全权（含下发指令、改系统设置、管账号）；`operator` 可查看+下发指令，不能改设置/管账号；`viewer` 只读。所有登录/改密/操作均写入审计日志（设置页「操作审计日志」可见）。
- **后续增强（待做）**：把 `manage_users.py` 的能力做成 Web 端「用户管理」页面（admin 可见），免去 SSH 进服务器建账号。

### 6. 备份 / 监控 / 单点风险
- **备份**：除 `backend/bms_history.db` + `dashboard.env` + `.secret_key`，Traefik 模式还需备份 Traefik 的 `acme.json`（若启用 ACME）；
- **监控**：`docker compose -f deploy/traefik-stack.yml logs -f` + 各容器 healthcheck；可加一个 `uptime-kuma` 容器做存活探测；
- **单点故障**：一台 VPS 是 SPOF——用每日自动备份（`backup_db.py` 已存在）+ 看门狗缓解；关键项目未来可迁 Oracle 双活（超纲，按需再议）。

## 自建 MQTT broker 桥接（可选 · 链路不变）
若要把真实 BMS 数据也推到自己的 MQTT broker（例如阿里云 `<YOUR_ECS_IP>:1883`，
账号 `student`），新增的 `bms-bridge` 服务会周期从华为云 IoTDA 拉取设备影子，
转发到 broker 的 `bms` 主题（及 `bms/<device_id>/data`）。
- **不改固件、不改现有仪表盘**：只是把同一份 IoTDA 数据旁路输出到 broker，前端链路完全不变；
- 复用 `dashboard.env` 的 `BMS_IOTDA_*` 凭证；broker 默认连 `127.0.0.1:1883`（与 broker 同机最快）；
- 启动：`docker compose -f deploy/docker-compose.yml up -d --build`（bms-bridge 随栈一起起来）；
- 验证：用 MQTTX 连 broker、订阅 `bms`，应每 10s 收到与前端一致的 JSON；
- 改 broker 地址/账号：在 `dashboard.env` 加 `BMS_BRIDGE_MQTT_HOST/PORT/USER/PASS`（默认已填 student / 你的密码）；
- 依赖已加回 `requirements.prod.txt` 的 `paho-mqtt`，无需额外安装。

## 与 DEPLOY_CLOUD.md 的关系
- 两套**二选一**，不要混用（避免 5000/80 端口冲突）。
- Docker 版：容器 5000 → 主机 80；DEPLOY_CLOUD.md 版：Nginx 80/443 → 5000。
- 服务器初始化、安全加固（改 SSH 端口/密钥登录/防火墙）、监控日志等内容，仍以 `DEPLOY_CLOUD.md` 第 5/13/16 章为准。
