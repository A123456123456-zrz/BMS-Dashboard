@echo off
cd /d "%~dp0"
REM BMS 桥接一键启动(Windows): 自动建 venv + 装依赖 + 连阿里云 broker
if not exist venv (
    echo [run_bridge] 创建虚拟环境...
    python -m venv venv
    call venv\Scripts\activate.bat
    python -m pip install --upgrade pip
    pip install paho-mqtt requests
) else (
    call venv\Scripts\activate.bat
)
set BMS_BRIDGE_MQTT_HOST=<YOUR_ECS_IP>
echo [run_bridge] 启动桥接(连 <YOUR_ECS_IP>:1883, 发布到 bms 主题)...
python iotda_mqtt_bridge.py
pause
