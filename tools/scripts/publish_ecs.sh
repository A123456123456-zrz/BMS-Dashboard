#!/usr/bin/env bash
# ============================================================================
# BMS 一键发布 + 清理脚本(在你本地电脑的 Git Bash 里运行)
# ----------------------------------------------------------------------------
# 功能: 上传固件/前端 → ECS 重启服务 → 清理旧镜像/日志/历史固件/备份
# 以后每次发布: 先编译固件, 再运行本脚本, 一步完成"上传+重启+清理"。
#
# 用法:
#   bash publish_ecs.sh           # 发布全部(固件+前端+重启+清理)
#   bash publish_ecs.sh clean     # 只清理, 不发布
#   bash publish_ecs.sh fw        # 只发布固件+清理
#   bash publish_ecs.sh web       # 只发布前端+清理
#
# 前置: 本地已能用 SSH 密钥免密登录 ECS(首次先手动 ssh 一次确认)
# ============================================================================

# ---------- 配置区(第一次使用, 按你的实际情况改) ----------
ECS_HOST="root@<YOUR_ECS_IP>"                  # ECS 登录 (user@ip)
LOCAL_PROJ="/d/esp32project/BMS/BMS System"     # 本地项目根 (Git Bash 路径)
DASH_DIR=""                                     # ECS 上 dashboard 目录 (留空=自动探测)
KEEP_FW=2                                       # 保留最近 N 个历史固件(bms_1.0.x.bin)
KEEP_BACKUP=5                                   # 保留最近 N 份数据库备份
# --------------------------------------------------------

set -e
MODE="${1:-all}"

echo "=============================================================="
echo " BMS 发布脚本  模式: $MODE   目标: $ECS_HOST"
echo "=============================================================="

# ---------- 0) 自动探测 ECS 上的 dashboard 目录 ----------
if [ -z "$DASH_DIR" ]; then
  echo "[1/6] 探测 ECS 上 dashboard 目录 ..."
  DASH_DIR=$(ssh "$ECS_HOST" \
    'find / -maxdepth 6 -name app.py -path "*dashboard*" 2>/dev/null | head -1' \
    | sed 's#/backend/app.py##' | tr -d '\r')
  if [ -z "$DASH_DIR" ]; then
    echo "❌ 自动探测失败。请 SSH 登录 ECS 手动确认, 然后在本脚本配置区填 DASH_DIR="
    exit 1
  fi
  echo "     探测到: $DASH_DIR"
fi

# ---------- 1) 上传固件 ----------
if [ "$MODE" = "all" ] || [ "$MODE" = "fw" ]; then
  echo "[2/6] 上传固件 ..."
  test -f "$LOCAL_PROJ/build/bms.bin" || { echo "❌ 本地没有 build/bms.bin, 请先编译"; exit 1; }
  ssh "$ECS_HOST" "mkdir -p $DASH_DIR/firmware"
  scp "$LOCAL_PROJ/build/bms.bin"                       "$ECS_HOST:$DASH_DIR/firmware/bms.bin"
  scp "$LOCAL_PROJ/tools/dashboard/firmware/version.json" "$ECS_HOST:$DASH_DIR/firmware/version.json"
  echo "     ✅ 固件 + version.json 已上传"
fi

# ---------- 2) 上传前端 ----------
if [ "$MODE" = "all" ] || [ "$MODE" = "web" ]; then
  echo "[3/6] 上传前端 ..."
  scp -r "$LOCAL_PROJ/tools/dashboard/frontend/." "$ECS_HOST:$DASH_DIR/frontend/"
  echo "     ✅ 前端已上传"
fi

# ---------- 3) 重启服务 ----------
if [ "$MODE" != "clean" ]; then
  echo "[4/6] 重启 ECS 服务 ..."
  ssh "$ECS_HOST" "
    cd $DASH_DIR || exit 1
    if command -v docker >/dev/null 2>&1 && [ -f deploy/docker-compose.yml ]; then
      echo '   [Docker 部署] 重建并重启容器...'
      docker compose -f deploy/docker-compose.yml up -d --build
    elif command -v systemctl >/dev/null 2>&1 && systemctl list-unit-files 2>/dev/null | grep -q bms-dashboard; then
      echo '   [systemd 部署] 重启服务...'
      systemctl restart bms-dashboard
    else
      echo '   [venv 部署] 重启 Flask...'
      pkill -f 'backend/app.py' 2>/dev/null || true
      sleep 1
      nohup python3 backend/app.py >> logs/app.stdout.log 2>&1 &
    fi
  "
  echo "     ✅ 服务已重启"
fi

# ---------- 4) 清理 ----------
echo "[5/6] 清理旧文件 ..."
ssh "$ECS_HOST" "
  # 4.1 Docker 悬空镜像(每次构建累积, 最大头)
  if command -v docker >/dev/null 2>&1; then
    docker image prune -f 2>/dev/null | tail -1
  fi
  # 4.2 历史固件: 保留最新 $KEEP_FW 个 bms_1.0.x.bin(bms.bin 是当前版, 永不删)
  if [ -d $DASH_DIR/firmware ]; then
    cd $DASH_DIR/firmware
    ls -t bms_*.bin 2>/dev/null | tail -n +$((KEEP_FW+1)) | xargs -r rm -f
  fi
  # 4.3 日志: 超过 1MB 的日志清空(保留文件, 释放空间)
  if [ -d $DASH_DIR/logs ]; then
    find $DASH_DIR/logs -name '*.log' -size +1M -exec truncate -s 0 {} \; 2>/dev/null || true
  fi
  # 4.4 数据库备份: 保留最新 $KEEP_BACKUP 份
  if [ -d $DASH_DIR/backups ]; then
    cd $DASH_DIR/backups
    ls -t *.gz 2>/dev/null | tail -n +$((KEEP_BACKUP+1)) | xargs -r rm -f
  fi
"

# ---------- 5) 验证 ----------
echo "[6/6] 验证 ..."
sleep 2
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" --connect-timeout 8 "http://<YOUR_ECS_IP>:5000/api/status" 2>/dev/null || echo "000")
echo "     后端 /api/status → HTTP $HTTP_CODE (200/401 都算正常)"
ssh "$ECS_HOST" "docker system df 2>/dev/null | head -5 || true"

echo "=============================================================="
echo " ✅ 完成! 浏览器验证: http://<YOUR_ECS_IP>:5000"
echo "=============================================================="
