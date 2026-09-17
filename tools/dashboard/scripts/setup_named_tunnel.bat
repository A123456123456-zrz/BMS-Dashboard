@echo off
chcp 65001 >nul
REM ============================================================
REM BMS Dashboard 固定网址配置脚本(命名隧道)
REM 域名: bms0605.dpdns.org
REM 前提: 域名已托管到 Cloudflare(NS 已切)
REM 注意: 此脚本为重置/复用脚本,完整流程已在本电脑执行过
REM ============================================================

echo ============================================================
echo  BMS 固定网址配置脚本
echo  域名: bms0605.dpdns.org
echo  隧道名称: bms
echo ============================================================
echo.

REM 第1步: 登录 Cloudflare(会打开浏览器授权,选择对应域名)
echo [1/4] 登录 Cloudflare 账号...
echo      (浏览器会打开,选择 bms0605.dpdns.org 域名授权)
cloudflared.exe tunnel login
if %ERRORLEVEL% NEQ 0 (
    echo [错误] 登录失败,请重试
    pause
    exit /b 1
)
echo [成功] 登录完成
echo.

REM 第2步: 创建命名隧道(如已存在则跳过)
echo [2/4] 创建命名隧道 bms...
cloudflared.exe tunnel create bms
if %ERRORLEVEL% NEQ 0 (
    echo [警告] 隧道可能已存在,尝试继续...
) else (
    echo [成功] 隧道创建完成
)
echo.

REM 第3步: 绑定域名(创建 DNS CNAME 记录)
echo [3/4] 绑定域名 bms0605.dpdns.org 到隧道...
cloudflared.exe tunnel route dns bms bms0605.dpdns.org
if %ERRORLEVEL% NEQ 0 (
    echo [警告] 域名可能已绑定,跳过...
) else (
    echo [成功] 域名绑定完成
)
echo.

REM 第4步: 生成配置文件
echo [4/4] 生成配置文件...
REM 获取隧道 ID
for /f "tokens=*" %%i in ('cloudflared.exe tunnel list ^| findstr /r "^bms "') do (
    for /f "tokens=2" %%j in ("%%i") do set TUNNEL_ID=%%j
)
if "%TUNNEL_ID%"=="" (
    for /f "tokens=*" %%i in ('cloudflared.exe tunnel list ^| findstr "bms"') do (
        for /f "tokens=1" %%j in ("%%i") do set TUNNEL_ID=%%j
    )
)

REM 写入 config.yml
if not "%TUNNEL_ID%"=="" (
    (
        echo tunnel: %TUNNEL_ID%
        echo credentials-file: %USERPROFILE%\.cloudflared\%TUNNEL_ID%.json
        echo protocol: http2
        echo.
        echo ingress:
        echo   - hostname: bms0605.dpdns.org
        echo     service: http://127.0.0.1:5000
        echo   - service: http_status:404
    ) > "%USERPROFILE%\.cloudflared\config.yml"
    echo [成功] 配置文件已生成: %USERPROFILE%\.cloudflared\config.yml
    echo       tunnel_id = %TUNNEL_ID%
) else (
    echo [警告] 未获取到隧道 ID,请确认隧道创建成功
)
echo.

echo ============================================================
echo  配置完成! 固定网址: https://bms0605.dpdns.org
echo ============================================================
echo.
echo  启动方式(三选一):
echo  1. 双击 start_named_tunnel.bat  (前台带日志,推荐日常使用)
echo  2. 双击 start_dashboard.vbs     (后台无窗口,开机自启用)
echo  3. 运行 install_autostart.bat   (安装开机自启)
echo.
pause
