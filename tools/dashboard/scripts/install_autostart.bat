@echo off
chcp 65001 >nul
REM ============================================================
REM BMS Dashboard 开机自启动安装工具
REM   - 方式1 (推荐管理员): 任务计划程序 OnStart -> 开机即启动,无需登录
REM   - 方式2 (普通用户)  : %APPDATA%\启动 文件夹 -> 登录后启动
REM 固定网址: https://bms0605.dpdns.org
REM ============================================================

setlocal
set "VBS_PATH=%~dp0start_dashboard.vbs"
set "STARTUP_DIR=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"

echo ========================================
echo   BMS Dashboard 开机自启动安装
echo   固定网址: https://bms0605.dpdns.org
echo   VBS: %VBS_PATH%
echo ========================================
echo.

REM ============================================================
REM 方式1: 任务计划程序 (需要管理员权限 -> 开机即启动 无需登录)
REM ============================================================
echo [1/3] 尝试注册"任务计划程序"(开机即启动)...
REM  /sc onstart        -> 系统启动时触发 (比 登录 触发更早)
REM  /rl highest        -> 最高权限 (避免 eventlet/端口 绑定失败)
REM  /f                 -> 已存在就覆盖
schtasks /create /tn "BMS_Dashboard" ^
    /tr "wscript.exe \"%VBS_PATH%\"" ^
    /sc onstart /rl highest /f >nul 2>&1
if %errorlevel% equ 0 (
    echo [OK] 任务计划程序注册成功! 下次开机无需登录即可自启
    goto :after_install
)

REM ============================================================
REM 方式2: 启动文件夹 (不需要管理员, 但需要登录)
REM ============================================================
echo [1/3] 任务计划程序未获得管理员权限, 改用"启动文件夹"方式...
if not exist "%STARTUP_DIR%" mkdir "%STARTUP_DIR%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ws = New-Object -ComObject WScript.Shell; $sc = $ws.CreateShortcut('%STARTUP_DIR%\BMS_Dashboard.lnk'); $sc.TargetPath = 'wscript.exe'; $sc.Arguments = '\"%VBS_PATH%\"'; $sc.WorkingDirectory = '%~dp0'; $sc.Description = 'BMS Dashboard 自启动 (启动文件夹)'; $sc.WindowStyle = 7; $sc.Save()"
if exist "%STARTUP_DIR%\BMS_Dashboard.lnk" (
    echo [OK] 启动文件夹快捷方式已创建! (你登录账号后自动启动)
    echo      提示: 想要"无需登录就启动",请以"管理员身份"重新运行本脚本
) else (
    echo [失败] 启动文件夹快捷方式创建失败
    pause
    exit /b 1
)

:after_install
echo.
echo [2/3] 双保险: 无论以上哪种方式成功,都再确认一下两种渠道都就位
REM 两种方式都再打一遍(幂等),不会坏
if not exist "%STARTUP_DIR%\BMS_Dashboard.lnk" (
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$ws = New-Object -ComObject WScript.Shell; $sc = $ws.CreateShortcut('%STARTUP_DIR%\BMS_Dashboard.lnk'); $sc.TargetPath = 'wscript.exe'; $sc.Arguments = '\"%VBS_PATH%\"'; $sc.WorkingDirectory = '%~dp0'; $sc.Description = 'BMS Dashboard 自启动'; $sc.Save()" >nul 2>&1
)
schtasks /query /tn "BMS_Dashboard" >nul 2>&1
if %errorlevel% neq 0 (
    REM 再试一次无管理员版的任务计划程序 (登录触发 也比没有好)
    schtasks /create /tn "BMS_Dashboard" /tr "wscript.exe \"%VBS_PATH%\"" /sc onlogon /f >nul 2>&1
)

echo.
echo [3/3] 立即启动 Dashboard 服务 (模拟开机效果)...
REM 先杀掉旧的(如果存在, 干净环境)
taskkill /F /FI "IMAGENAME eq pythonw.exe" /FI "COMMANDLINE eq *app.py*" >nul 2>&1
taskkill /F /FI "IMAGENAME eq cloudflared.exe" /FI "COMMANDLINE eq *tunnel run*" >nul 2>&1
timeout /t 2 /nobreak >nul

wscript.exe "%VBS_PATH%"

echo   等待服务启动(最长20秒, 端口监听就提前结束)...
set /a i=0
:wait_loop
netstat -ano | findstr ":5000" | findstr LISTENING >nul 2>&1
if %errorlevel% equ 0 (
    echo [OK] 服务已启动 (监听 :5000)
    goto :done
)
timeout /t 1 /nobreak >nul
set /a i+=1
if %i% lss 20 goto wait_loop
echo [WARN] 20秒内未检测到 :5000 监听, 请检查 named_tunnel.log / cf_stderr.log

:done
echo.
echo ========================================
echo   安装/启动完成
echo ========================================
echo.
echo   本机访问:   http://localhost:5000
echo   公网访问:   https://bms0605.dpdns.org
echo   登录密码:   bms123
echo.
echo   当前自启方式(两种都会尝试):
schtasks /query /tn "BMS_Dashboard" /fo list 2>nul | findstr /i "TaskName To Run Schedule"
if exist "%STARTUP_DIR%\BMS_Dashboard.lnk" echo   启动文件夹: 已启用 ^( %STARTUP_DIR%\BMS_Dashboard.lnk ^)
echo.
pause
endlocal
