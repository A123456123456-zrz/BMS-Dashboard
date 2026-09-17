@echo off
chcp 65001 >nul
REM ============================================================
REM BMS Dashboard 固定网址启动脚本(命名隧道)
REM 启动 Flask + 命名隧道,固定网址: https://bms0605.dpdns.org
REM ============================================================

cd /d "%~dp0.."

REM 启动 Flask Dashboard(后台无窗口)
start "" /B pythonw backend\app.py

REM 等待 Flask 启动
timeout /t 5 /nobreak >nul

REM 启动命名隧道(前台运行,关闭窗口即停止)
echo ============================================================
echo  BMS Dashboard 已启动
echo  固定网址: https://bms0605.dpdns.org
echo  本机访问: http://localhost:5000
echo  按 Ctrl+C 停止隧道(Dashboard 仍会运行)
echo ============================================================
echo.

"%USERPROFILE%\.cloudflared\cloudflared.exe" --config "d:\esp32project\BMS\BMS System\tools\dashboard\tunnel\config.yml" --logfile "d:\esp32project\BMS\BMS System\tools\dashboard\logs\cloudflared.log" tunnel run bms

pause
