@echo off
REM ============================================================
REM BMS CI smoke test (Windows / ESP-IDF)
REM 2026-08-09: validate firmware build + backend syntax + frontend syntax
REM Run: tools\ci\smoke_test.bat
REM Exit: 0=all pass  1=has failure
REM NOTE: keep this file ASCII-only (cmd GBK vs UTF-8 comment issue)
REM ============================================================
setlocal enabledelayedexpansion
set FAIL=0
set ROOT=%~dp0..\..

echo ============================================================
echo [CI] BMS smoke test start
echo ============================================================

REM ---------- 1. Frontend JS syntax ----------
echo.
echo [CI 1/4] frontend JS check (node --check)
where node >nul 2>&1
if %errorlevel%==0 (
    node --check "%ROOT%\tools\dashboard\frontend\static\app.js" && (echo   [OK] app.js syntax ok) || (echo   [FAIL] app.js syntax error & set FAIL=1)
) else (
    echo   [SKIP] node not found
)

REM ---------- 2. Backend Python syntax ----------
echo.
echo [CI 2/4] backend Python check (py_compile)
where py >nul 2>&1
if %errorlevel%==0 (
    cd /d "%ROOT%\tools\dashboard\backend"
    py -m py_compile app.py database.py iotda_client.py iotda_rules.py alert_push.py mqtt_client.py aging_monitor.py firmware_aging_test.py
    if %errorlevel%==0 (echo   [OK] Python all pass) else (echo   [FAIL] Python syntax error & set FAIL=1)
) else (
    echo   [SKIP] py not found
)

REM ---------- 3. JSON validity ----------
echo.
echo [CI 3/4] JSON validity
cd /d "%ROOT%\tools\dashboard"
py -c "import json;[json.load(open(f,encoding='utf-8')) for f in ['frontend/static/manifest.json','backend/iotda_product_model.json']];print('  [OK] JSON all valid')" 2>nul || (echo   [FAIL] JSON invalid & set FAIL=1)

REM ---------- 4. Firmware build ----------
echo.
echo [CI 4/4] firmware build (idf.py build)
set IDF_PATH=D:\v5.2.7\esp-idf
set IDF_TOOLS_PATH=C:\Espressif
set PATH=C:\Espressif\python_env\idf5.2_py3.10_env\Scripts;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\xtensa-esp-elf\esp-13.2.0_20250707\xtensa-esp-elf\bin;%PATH%
cd /d "%ROOT%"
py D:\v5.2.7\esp-idf\tools\idf.py build > build\ci_build.log 2>&1
if %errorlevel%==0 (echo   [OK] firmware build ok) else (
    echo   [FAIL] firmware build failed, see build\ci_build.log
    findstr /i "error" build\ci_build.log | more
    set FAIL=1
)

echo.
echo ============================================================
if %FAIL%==0 (echo [CI] ALL PASS) else (echo [CI] HAS FAILURE)
echo ============================================================
exit /b %FAIL%
