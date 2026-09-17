' BMS Dashboard 自启动脚本(无窗口后台运行)
' 同时启动 Flask 服务和 Cloudflare 命名隧道(固定网址)
' 固定网址: https://bms0605.dpdns.org
' 触发方式: 启动文件夹快捷方式 / 任务计划程序 OnStart

Option Explicit

Dim ws, fso, dashboardDir, userProfile, configPath
Dim pythonwPath, pythonPath, cloudflaredPath, cfInUser
Dim waitCount, flaskRunning, tunnelRunning, procList, p
Dim netOk, testUrl, httpOk
Const HIDDEN_WINDOW = 0
Const WAIT_NO_RETURN = False

Set ws = CreateObject("Wscript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
dashboardDir = fso.GetParentFolderName(fso.GetParentFolderName(WScript.ScriptFullName))
userProfile = ws.ExpandEnvironmentStrings("%USERPROFILE%")
configPath = userProfile & "\.cloudflared\config.yml"

' ===== Python 路径 (优先 pythonw 无窗口, 找不到回退 python) =====
pythonwPath = "C:\Espressif\tools\python\v5.2.7\venv\Scripts\pythonw.exe"
pythonPath  = "C:\Espressif\tools\python\v5.2.7\venv\Scripts\python.exe"
If Not fso.FileExists(pythonwPath) Then
    pythonwPath = "C:\Espressif\tools\python\pythonw.exe"
    pythonPath  = "C:\Espressif\tools\python\python.exe"
End If
If Not fso.FileExists(pythonwPath) Then
    pythonwPath = "pythonw.exe"
    pythonPath  = "python.exe"
End If

' ===== Cloudflared 路径 (用户目录优先, 项目目录回退) =====
cfInUser = userProfile & "\.cloudflared\cloudflared.exe"
If fso.FileExists(cfInUser) Then
    cloudflaredPath = cfInUser
Else
    cloudflaredPath = dashboardDir & "\tunnel\cloudflared.exe"
End If

' ====================================================================
' 阶段1: 等待网络就绪 (混合探测, 避免 ping 被封导致等60秒)
' ====================================================================
waitCount = 0
netOk = False
' 先用轻量 DNS 探测 bms0605.dpdns.org -> 失败再退化为 nslookup 默认网关
Do While waitCount < 15
    On Error Resume Next
    ' 尝试 DNS 解析 (nslookup 不依赖 ICMP, UDP/53 基本都通)
    httpOk = ws.Run("cmd /c nslookup bms0605.dpdns.org 127.0.0.1 >nul 2>&1", HIDDEN_WINDOW, True)
    If Err.Number <> 0 Then httpOk = 1
    Err.Clear
    On Error GoTo 0
    If httpOk = 0 Then
        netOk = True
        Exit Do
    End If
    WScript.Sleep 2000
    waitCount = waitCount + 1
Loop
If Not netOk Then
    ' 再给最后一次机会: 直接探测 localhost 是否存在网卡 up (ping ::1)
    On Error Resume Next
    ws.Run "cmd /c ping -n 1 ::1 >nul 2>&1", HIDDEN_WINDOW, True
    On Error GoTo 0
End If

' ====================================================================
' 阶段2: 启动 Flask Dashboard
' 防重复: 检查 pythonw.exe 或 python.exe 的命令行中是否包含 app.py (避免误伤其他 python 进程)
' ====================================================================
flaskRunning = False
On Error Resume Next
Set procList = GetObject("winmgmts:").ExecQuery( _
    "SELECT * FROM Win32_Process " & _
    " WHERE (Name='pythonw.exe' OR Name='python.exe')" & _
    "   AND CommandLine LIKE '%app.py%'")
If Err.Number = 0 Then
    If Not procList Is Nothing Then flaskRunning = (procList.Count > 0)
End If
Err.Clear
On Error GoTo 0

If Not flaskRunning Then
    ws.CurrentDirectory = dashboardDir
    On Error Resume Next
    ws.Run """" & pythonwPath & """ """ & dashboardDir & "\backend\app.py""", HIDDEN_WINDOW, WAIT_NO_RETURN
    If Err.Number <> 0 Then
        ' pythonw 失败回退到 python (可能没有 pythonw.exe)
        Err.Clear
        ws.Run """" & pythonPath & """ """ & dashboardDir & "\backend\app.py""", HIDDEN_WINDOW, WAIT_NO_RETURN
    End If
    On Error GoTo 0
End If

' 等待 Flask 就绪 (10秒 + 探测5000端口, 比硬等待5秒更稳)
waitCount = 0
Do While waitCount < 20
    On Error Resume Next
    ' 尝试连接一下 5000 (发个空 TCP 探测, 成功返回 0 / 1)
    httpOk = ws.Run("cmd /c ""netstat -ano | findstr "":5000"" | findstr LISTENING >nul 2>&1""", HIDDEN_WINDOW, True)
    If Err.Number <> 0 Then httpOk = 1
    Err.Clear
    On Error GoTo 0
    If httpOk = 0 Then Exit Do
    WScript.Sleep 500
    waitCount = waitCount + 1
Loop

' ====================================================================
' 阶段3: 启动 Cloudflare 命名隧道 (固定网址 https://bms0605.dpdns.org)
' 防重复: 查 cloudflared.exe 进程(带 config.yml 参数)
' ====================================================================
tunnelRunning = False
On Error Resume Next
Set procList = GetObject("winmgmts:").ExecQuery( _
    "SELECT * FROM Win32_Process" & _
    " WHERE Name='cloudflared.exe'" & _
    "   AND (CommandLine LIKE '%config.yml%' OR CommandLine LIKE '%tunnel run%')")
If Err.Number = 0 Then
    If Not procList Is Nothing Then tunnelRunning = (procList.Count > 0)
End If
Err.Clear
On Error GoTo 0

If Not tunnelRunning Then
    If fso.FileExists(cloudflaredPath) And fso.FileExists(configPath) Then
        On Error Resume Next
        ws.Run "cmd /c """"" & cloudflaredPath & """ --config """ & configPath & _
               """ tunnel run bms >> """ & dashboardDir & "\logs\named_tunnel.log"" 2>&1""", _
               HIDDEN_WINDOW, WAIT_NO_RETURN
        On Error GoTo 0
    End If
End If

' ====================================================================
' 阶段4: 启动 Watchdog 守护 (自动拉起 + 堆积清理, 每180s)
'  修复 2026-08-07: 原脚本开机只起 Flask+隧道, 没有守护进程,
'  导致进程崩溃/堆积无人处理 (cloudflared 曾堆到 16 个)
' ====================================================================
Dim watchdogRunning, wdCmd
watchdogRunning = False
On Error Resume Next
Set procList = GetObject("winmgmts:").ExecQuery( _
    "SELECT * FROM Win32_Process" & _
    " WHERE Name='powershell.exe'" & _
    "   AND CommandLine LIKE '%watchdog.ps1%'")
If Err.Number = 0 Then
    If Not procList Is Nothing Then watchdogRunning = (procList.Count > 0)
End If
Err.Clear
On Error GoTo 0

If Not watchdogRunning Then
    wdCmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & dashboardDir & "\scripts\watchdog.ps1"""
    On Error Resume Next
    ws.Run wdCmd, HIDDEN_WINDOW, WAIT_NO_RETURN
    If Err.Number <> 0 Then
        Err.Clear
        ws.Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -File """ & dashboardDir & "\scripts\watchdog.ps1""", HIDDEN_WINDOW, WAIT_NO_RETURN
    End If
    On Error GoTo 0
End If

' ====================================================================
' 清理
' ====================================================================
Set procList = Nothing
Set p = Nothing
Set ws = Nothing
Set fso = Nothing
