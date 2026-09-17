# BMS Dashboard 容器镜像
# 用法: 在 tools/dashboard/ 目录执行  docker compose up -d --build
FROM python:3.11-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    TZ=Asia/Shanghai

WORKDIR /app

# 1) 先装依赖, 利用 Docker 层缓存(改业务代码不会触发重装)
COPY backend/requirements.prod.txt /app/backend/requirements.prod.txt
RUN pip install --no-cache-dir -r /app/backend/requirements.prod.txt -i https://mirrors.aliyun.com/pypi/simple/

# 2) 拷贝代码(frontend 由 backend/app.py 通过 ../frontend 引用, 见 app.py L147-149)
COPY backend/ /app/backend/
COPY frontend/ /app/frontend/
# 3) 固件与版本配置(firmware/version.json + bms.bin, 供 /api/ota/* 匿名接口)
COPY firmware/ /app/firmware/

# 4) 密钥与数据库通过 bind mount 注入(.dockerignore 已排除, 不会写进镜像层)
WORKDIR /app/backend
EXPOSE 5000
# app.py 内部已 socketio.run(host="0.0.0.0", port=5000) 且会自动读取同目录 dashboard.env / .secret_key
CMD ["python", "app.py"]
