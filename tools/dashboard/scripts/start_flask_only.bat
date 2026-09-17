@echo off
REM BMS Dashboard 仅启动 Flask 服务 (无 Cloudflare 隧道, 局域网 IP 直连)
cd /d "d:\esp32project\BMS\BMS System\tools\dashboard"
start "" /B "C:\Espressif\tools\python\pythonw.exe" "d:\esp32project\BMS\BMS System\tools\dashboard\backend\app.py"
