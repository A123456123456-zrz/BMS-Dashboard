@echo off
REM BMS Dashboard 启动脚本 (无窗口)
REM 同时启动 Flask 服务和 Cloudflare 隧道
REM 修复 2026-08-07: 增加 Watchdog 守护(自动拉起 + 堆积清理)
cd /d "d:\esp32project\BMS\BMS System\tools\dashboard"

REM 启动 Flask Dashboard (pythonw 无窗口)
taskkill /F /FI "IMAGENAME eq pythonw.exe" /FI "WINDOWTITLE eq *app.py*" >nul 2>&1
start "" /B "C:\Espressif\tools\python\pythonw.exe" "d:\esp32project\BMS\BMS System\tools\dashboard\backend\app.py"

REM 等2秒让Flask起来
timeout /t 2 /nobreak >nul

REM 启动 Cloudflare 隧道 (如果没在运行)
tasklist /FI "IMAGENAME eq cloudflared.exe" 2>nul | find "cloudflared" >nul
if %errorlevel% neq 0 (
    start "" /B "C:\Users\ASUS\.cloudflared\cloudflared.exe" --config "d:\esp32project\BMS\BMS System\tools\dashboard\tunnel\config.yml" --logfile "d:\esp32project\BMS\BMS System\tools\dashboard\logs\cloudflared.log" tunnel run bms
)

REM 启动 Watchdog 守护 (如果没在运行)
tasklist /FI "IMAGENAME eq powershell.exe" 2>nul | find "powershell" >nul
powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command "if (-not (Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | Where-Object { $_.CommandLine -like '*watchdog.ps1*' })) { Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File','d:\esp32project\BMS\BMS System\tools\dashboard\scripts\watchdog.ps1' }"
