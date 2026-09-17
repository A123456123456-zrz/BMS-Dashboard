#!/usr/bin/env bash
# ============================================================
# BMS Dashboard 一键部署脚本（腾讯云轻量 / 阿里云 ECS 通用）
# 用法：把 bms-dashboard.tar.gz 上传到服务器后
#   mkdir -p /opt/bms-dashboard && cd /opt/bms-dashboard
#   tar xzf bms-dashboard.tar.gz
#   cp -r dashboard/* . && cp dashboard/.dockerignore . 2>/dev/null; true
#   bash setup.sh
# 脚本会自动：装 Docker → 建密钥/数据库 → 建 MQTT 密码 → 起三容器 → 验证
# ============================================================
set -uo pipefail

cd "$(dirname "$0")" || { echo "无法定位脚本目录"; exit 1; }

# 兼容 docker compose 插件 / docker-compose 二进制
if docker compose version >/dev/null 2>&1; then DC="docker compose"; else DC="docker-compose"; fi

MQTT_USER="student"
MQTT_PASS="<BROKER_PASSWORD>"      # 与 iotda_mqtt_bridge.py 默认值一致；改密码时同步 dashboard.env 的 BMS_BRIDGE_MQTT_PASS

echo "=================================================="
echo "[1/6] 检查并安装 Docker"
echo "=================================================="
if ! command -v docker >/dev/null 2>&1; then
  echo "未检测到 Docker，开始安装..."
  curl -fsSL https://get.docker.com | sh
  systemctl enable --now docker 2>/dev/null || service docker start 2>/dev/null || true
fi
docker --version
$DC version 2>/dev/null | head -1 || true

echo
echo "=================================================="
echo "[2/6] 准备密钥与数据库文件（宿主机 bind mount 用）"
echo "=================================================="
mkdir -p backend
if [ ! -f backend/dashboard.env ]; then
  echo "ERROR: backend/dashboard.env 缺失！请确认 tar 包含该文件，或手动上传。"
  exit 1
fi
echo "dashboard.env: OK"
# Flask session 加密密钥（不存在则随机生成，避免重启掉登录）
if [ ! -f backend/.secret_key ]; then
  head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > backend/.secret_key
  echo ".secret_key: 已生成随机值"
else
  echo ".secret_key: 已存在"
fi
# 历史数据库（不存在则 touch；bind mount 指向文件而非目录）
touch backend/bms_history.db
echo "bms_history.db: 已就绪（云端将从 IoTDA 重新采集）"

echo
echo "=================================================="
echo "[3/6] 创建 Mosquitto 密码文件 (${MQTT_USER})"
echo "=================================================="
mkdir -p deploy/mosquitto/config
# 用官方镜像一次性生成 passwd，写入宿主机目录（compose 以 :ro 挂载）
docker run --rm -v "$PWD/deploy/mosquitto/config:/mosquitto/config" \
  eclipse-mosquitto:2.0 mosquitto_passwd -c -b /mosquitto/config/passwd "$MQTT_USER" "$MQTT_PASS"
if [ -s deploy/mosquitto/config/passwd ]; then
  echo "passwd: 已生成 ->"; ls -l deploy/mosquitto/config/passwd
else
  echo "ERROR: passwd 生成失败"; exit 1
fi

echo
echo "=================================================="
echo "[4/6] 构建并启动全套（仪表盘 + 桥接 + broker）"
echo "=================================================="
$DC -f deploy/docker-compose.yml up -d --build
echo "启动命令已下发。"

echo
echo "=================================================="
echo "[5/6] 等待 15s 让容器就绪..."
echo "=================================================="
sleep 15

echo
echo "=================================================="
echo "[6/6] 验证三条链路"
echo "=================================================="
echo "--- 容器状态 ---"
$DC -f deploy/docker-compose.yml ps 2>/dev/null || docker ps --format 'table {{.Names}}\t{{.Status}}\t{{.Ports}}'

echo "--- HTTP 80/login ---"
curl -s -o /dev/null -w "login HTTP %{http_code}\n" http://127.0.0.1:80/login || echo "curl 失败（80 未响应，查容器日志）"

echo "--- 桥接日志（应每 10s 一条『已发布』）---"
docker logs bms-bridge --tail 6 2>/dev/null || echo "bms-bridge 无日志"

echo "--- broker 监听 1883 ---"
(ss -ltnp 2>/dev/null | grep ':1883' || netstat -ltn 2>/dev/null | grep ':1883' || echo "未检测到 1883 监听（查 mosquitto 日志）")

echo
echo "=================================================="
echo "部署完成。浏览器访问 http://<你的公网IP>/"
echo "登录密码见 backend/dashboard.env 的 BMS_DASH_PASSWORD"
echo "若外网打不开：去云控制台防火墙放行 22 / 80 / 1883"
echo "=================================================="
