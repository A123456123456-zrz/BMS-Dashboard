#!/bin/bash
# ============================================================
# BMS Dashboard 本地 → 阿里云 ECS 增量同步部署脚本
# 目标: <YOUR_ECS_IP> (root, 密钥 ~/.ssh/id_ed25519)
# 用途: 把本地 frontend/ + backend/ 修改同步到 ECS 并重启服务
#       (替代逐文件手动 scp, 本会话所有改动都经此部署)
# 用法:
#   bash scripts/deploy_to_ecs.sh            # 全量同步
#   bash scripts/deploy_to_ecs.sh app.js     # 只同步前端 app.js
#   bash scripts/deploy_to_ecs.sh app.py     # 只同步后端 app.py
#   bash scripts/deploy_to_ecs.sh --restart  # 只重启服务(不同步)
# 前置: ECS SSH 可达(22 端口), 密钥 ~/.ssh/id_ed25519
# ============================================================
set -e

ECS_HOST=<YOUR_ECS_IP>
ECS_USER=root
SSH_KEY=~/.ssh/id_ed25519
# 本地 dashboard 根目录(脚本所在目录的上一级)
LOCAL_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REMOTE_ROOT=/opt/bms-dashboard
SSH_OPTS="-o ConnectTimeout=15 -o BatchMode=yes -i $SSH_KEY"

# 需要同步的文件清单(本地相对路径 -> ECS 相对路径)
FILES=(
  "frontend/static/app.js"
  "frontend/static/style.css"
  "frontend/templates/index.html"
  "frontend/templates/login.html"
  "backend/app.py"
  "backend/mqtt_client.py"
  "backend/iotda_client.py"
  "backend/database.py"
  "backend/alert_push.py"
  "backend/dashboard.env"
)

restart_only=false
[ "$1" = "--restart" ] && restart_only=true

echo "== 目标: $ECS_USER@$ECS_HOST : $REMOTE_ROOT =="

if [ "$restart_only" = false ]; then
  # 若指定了具体文件名, 只同步该文件
  target=""
  if [ -n "$1" ] && [ "$1" != "--restart" ]; then
    target="$1"
    echo "== 只同步: $target =="
    src="$LOCAL_ROOT/$target"
    [ -f "$src" ] || { echo "错误: 本地文件不存在 $src"; exit 1; }
    scp $SSH_OPTS "$src" "$ECS_USER@$ECS_HOST:$REMOTE_ROOT/$target"
  else
    echo "== 同步 $(( ${#FILES[@]} )) 个文件 =="
    for f in "${FILES[@]}"; do
      src="$LOCAL_ROOT/$f"
      [ -f "$src" ] || { echo "跳过(本地不存在): $f"; continue; }
      scp $SSH_OPTS "$src" "$ECS_USER@$ECS_HOST:$REMOTE_ROOT/$f" && echo "  ✓ $f"
    done
  fi
fi

echo "== 重启 bms-dashboard + bms-iotda-bridge 服务 =="
# SSH 偶发被重置, 重试几次
for i in 1 2 3 4 5; do
  out=$(timeout 30 ssh $SSH_OPTS "$ECS_USER@$ECS_HOST" 'systemctl restart bms-dashboard bms-iotda-bridge; sleep 3; echo "dashboard=$(systemctl is-active bms-dashboard) bridge=$(systemctl is-active bms-iotda-bridge)"' 2>&1 | grep -v "WARNING\|post-quantum\|store now\|upgraded\|openssh")
  if echo "$out" | grep -q "dashboard=active"; then
    echo "$out"
    break
  fi
  echo "SSH 重试 $i... ${out:0:80}"
  sleep 6
done

echo "== 部署完成 =="
