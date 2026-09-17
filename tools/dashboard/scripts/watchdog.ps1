# ============================================================
# BMS Dashboard + Cloudflare Tunnel Watchdog (Health Check)
#   - Check 2 processes every 180s: Python Dashboard, cloudflared
#   - HTTP test: local:5000 / login+api / public HTTPS
#   - Auto-restart on failure (after 2 consecutive failures)
#   - Verify config.yml integrity + NTFS ReadOnly attribute
# ============================================================
$ErrorActionPreference = "SilentlyContinue"
# Fix: $MyInvocation.MyCommand.Path is empty when run via Task Scheduler
$RootDir  = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrWhiteSpace($RootDir)) { $RootDir = "d:\esp32project\BMS\BMS System\tools\dashboard" }
$Py       = "C:\Espressif\tools\python\python.exe"
$CfExe    = Join-Path $RootDir "tunnel\cloudflared.exe"
$Cfg      = Join-Path $RootDir "tunnel\config.yml"
$AppPy    = Join-Path $RootDir "backend\app.py"
$LogFile  = Join-Path $RootDir "logs\watchdog.log"
$ExpectedCfgContent = @"
tunnel: 27c0e102-74d6-4b5b-91cd-50b021f09ba4
credentials-file: d:\esp32project\BMS\BMS System\tools\dashboard\tunnel\27c0e102-74d6-4b5b-91cd-50b021f09ba4.json

ingress:
  - hostname: bms0605.dpdns.org
    service: http://127.0.0.1:5000
  - service: http_status:404
"@

function wlog($s) {
    # 2026-08-08 可靠性修复: 日志轮转 - 超过 1MB 时保留最后 300 行, 防止无限增长
    if (Test-Path $LogFile) {
        $sz = (Get-Item $LogFile).Length
        if ($sz -gt 1MB) {
            Get-Content $LogFile -Tail 300 | Set-Content $LogFile -Encoding UTF8
            Add-Content -Path $LogFile -Value ("[{0}] [LOG-ROTATE] 日志已轮转(原 {1} 字节)" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $sz) -Encoding UTF8
        }
    }
    $line = ("[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $s)
    Add-Content -Path $LogFile -Value $line -Encoding UTF8
    Write-Host $line -ForegroundColor Cyan
}
function Assert-Config {
    $needFix = $false
    if (-not (Test-Path $Cfg)) { $needFix = $true }
    else {
        $txt = Get-Content $Cfg -Raw -Encoding UTF8
        if ($txt.Length -lt 200 -or -not ($txt -match 'tunnel:\s*\w{8}-') -or -not ($txt -match 'ingress:') -or -not ($txt -match 'bms0605\.dpdns\.org')) {
            $needFix = $true
        }
    }
    if ($needFix) {
        wlog "[WARN] config.yml corrupted or empty! Restoring + setting ReadOnly"
        if (Test-Path $Cfg) { (Get-Item $Cfg).Attributes = (Get-Item $Cfg).Attributes -band -bnot [IO.FileAttributes]::ReadOnly }
        Set-Content -Path $Cfg -Value $ExpectedCfgContent -Encoding UTF8 -Force
    }
    $item = Get-Item $Cfg
    if (($item.Attributes -band [IO.FileAttributes]::ReadOnly) -eq 0) {
        wlog "[FIX] Adding NTFS ReadOnly attribute to config.yml"
        $item.Attributes = $item.Attributes -bor [IO.FileAttributes]::ReadOnly
    }
}
function Proc-Alive($nameMatch, $cmdMatch) {
    # 返回匹配进程数量(0=无, 1=正常, >1=有堆积)
    # 修复: 旧版 @($found).Count 在无进程时(@($null).Count=1)误判为"活着",
    #       且不清理旧实例导致进程无限堆积(cloudflared 曾堆到 16 个)
    try {
        $procs = @(Get-CimInstance Win32_Process -Filter "Name like '$nameMatch'" -ErrorAction Stop)
        if ($procs.Count -eq 0) { return 0 }
        if ([string]::IsNullOrEmpty($cmdMatch)) { return $procs.Count }
        $n = 0
        foreach ($p in $procs) {
            if ($p.CommandLine -and ($p.CommandLine -match $cmdMatch)) { $n++ }
        }
        return $n
    } catch {
        return 0
    }
}
function Stop-All($nameMatch, $cmdMatch) {
    # 杀掉所有匹配进程(启动新实例前调用, 防止堆积)
    Get-CimInstance Win32_Process -Filter "Name like '$nameMatch'" |
        Where-Object { [string]::IsNullOrEmpty($cmdMatch) -or ($_.CommandLine -and ($_.CommandLine -match $cmdMatch)) } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
function Http-Test($url, $body = $null) {
    try {
        if ($body) {
            $r = Invoke-WebRequest -Uri $url -Method POST -Body $body -TimeoutSec 8 -UseBasicParsing -SessionVariable s
            # 2026-08-09 修复: 登录成功返回 302 重定向(FOUND→首页), 属正常成功,
            #   原判定 `-ne 200` 把 302 误判为失败 → watchdog 每 180s 强制重启 Dashboard
            #   → WebSocket 反复断开重连. 改为接受 2xx/3xx(重定向=登录成功).
            if ($r.StatusCode -lt 200 -or $r.StatusCode -ge 400) { return $false, $null }
            $r2 = Invoke-WebRequest -Uri ($url -replace '/login','/api/status') -WebSession $s -TimeoutSec 10 -UseBasicParsing
            # 2026-08-09 修复: 健康检查语义=服务是否"活着"——401(未授权)说明服务在运行
            #   只是会话未建立(login body 缺 username 导致 WebSession 无有效 cookie),
            #   不应判为失败. 仅 5xx/连接异常才算服务故障.
            if ($r2.StatusCode -ge 500) { return $false, $r2 }
            return $true, $r2
        } else {
            $r = Invoke-WebRequest -Uri $url -TimeoutSec 8 -UseBasicParsing
            return ($r.StatusCode -lt 500), $r
        }
    } catch {
        # 2026-08-09 修复(关键): PowerShell Invoke-WebRequest 对 4xx/5xx 会抛异常而非返回状态码.
        #   原 catch 一律返回 false → login 后 api/status 返回 401(未授权)被误判为服务故障
        #   → watchdog 每 180s 强制重启 Dashboard → WebSocket 反复断开重连.
        #   修复: 有 HTTP 响应且状态码 <500(如 401/403/404)= 服务在运行, 判健康;
        #         仅无响应(连接失败)或 5xx(服务异常) 才判故障.
        if ($_.Exception.Response) {
            try { $code = [int]$_.Exception.Response.StatusCode } catch { $code = 599 }
            if ($code -lt 500) { return $true, $null }
        }
        return $false, $null
    }
}
function Start-Dashboard {
    wlog "[START] Dashboard: $Py $AppPy"
    $pi = New-Object System.Diagnostics.ProcessStartInfo
    $pi.FileName = $Py; $pi.Arguments = '-u "' + $AppPy + '"'
    $pi.UseShellExecute = $false
    $pi.RedirectStandardOutput = $true; $pi.RedirectStandardError = $true
    $pi.WorkingDirectory = $RootDir
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $pi
    $logFile = Join-Path $RootDir "logs\app.stdout.log"
    $errFile = Join-Path $RootDir "logs\app.stderr.log"
    Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
        param($s,$e); if ($e.Data) { Add-Content "d:\esp32project\BMS\BMS System\tools\dashboard\logs\app.stdout.log" $e.Data }
    } | Out-Null
    Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
        param($s,$e); if ($e.Data) { Add-Content "d:\esp32project\BMS\BMS System\tools\dashboard\logs\app.stderr.log" $e.Data }
    } | Out-Null
    $proc.Start() | Out-Null
    $proc.BeginOutputReadLine(); $proc.BeginErrorReadLine()
    return $proc.Id
}
function Start-Cloudflared {
    wlog "[START] Cloudflare Tunnel: $CfExe"
    if (-not (Test-Path $CfExe)) { wlog "[ERROR] $CfExe not found"; return 0 }
    $pi = New-Object System.Diagnostics.ProcessStartInfo
    $pi.FileName = $CfExe
    $pi.Arguments = 'tunnel --config "' + $Cfg + '" --no-prechecks run'
    $pi.UseShellExecute = $false
    $pi.WorkingDirectory = $RootDir
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $pi
    $proc.Start() | Out-Null
    Start-Sleep -Seconds 4
    if ($proc.HasExited) { wlog "[ERROR] cloudflared exited immediately code=$($proc.ExitCode)" }
    return $proc.Id
}

wlog "============================================================"
wlog "[WATCHDOG] Started (poll interval 180s)"
wlog "============================================================"
$fail5000 = 0; $failWan = 0; $wanCounter = 0

while ($true) {
    Assert-Config
    # -------- Process alive check (数量检测: 0=启动, >1=清理堆积再启动) --------
    $dCount = Proc-Alive "python%" "app\.py"
    $cCount = Proc-Alive "cloudflared%" ""
    wlog ("Status: Dashboard={0}  Cloudflared={1}" -f $dCount, $cCount)
    if ($dCount -eq 0) { $id = Start-Dashboard; wlog "Dashboard restarted, PID=$id"; Start-Sleep -Seconds 8 }
    elseif ($dCount -gt 1) {
        wlog "[CLEANUP] Dashboard 堆积 $dCount 个, 清理后重启"
        Stop-All "python%" "app\.py"
        Start-Sleep -Seconds 3
        $id = Start-Dashboard; wlog "Dashboard restarted, PID=$id"; Start-Sleep -Seconds 8
    }
    if ($cCount -eq 0) { $id = Start-Cloudflared; wlog "Cloudflared restarted, PID=$id" }
    elseif ($cCount -gt 1) {
        wlog "[CLEANUP] Cloudflared 堆积 $cCount 个, 清理后重启"
        Stop-All "cloudflared%" ""
        Start-Sleep -Seconds 3
        $id = Start-Cloudflared; wlog "Cloudflared restarted, PID=$id"
    }
    # -------- HTTP check: local 5000 --------
    # 修复(2026-08-10): 去掉 password=bms123 的登录式探测, 改为仅 GET /login 验证存活.
    #   原 POST 错误密码会污染失败计数(隧道场景下 remote_addr 被统一改写为 127.0.0.1),
    #   且公网探测会锁定用户公网 IP(见下方 WAN 段说明). 现 GET /login 返回 200 即判存活,
    #   完全不触发任何登录/失败计数逻辑.
    $okLocal, $_ = Http-Test "http://localhost:5000/login"
    if (-not $okLocal) {
        $fail5000++
        wlog ("[FAIL] local 5000 failed ({0} consecutive)" -f $fail5000)
        if ($fail5000 -ge 2) {
            wlog "[RESTART] Force restarting Dashboard ..."
            Stop-All "python%" "app\.py"
            Start-Sleep -Seconds 3
            Start-Dashboard | Out-Null
            $fail5000 = 0
        }
    } else { $fail5000 = 0; wlog "[OK] local 5000 HTTP OK" }
    # -------- HTTP check: public HTTPS (every 5 rounds) --------
    # 修复(2026-08-10): 改为 GET /api/status 验证隧道+服务存活, 完全不做登录.
    #   根因: 旧逻辑每 5 轮向 https://bms0605.dpdns.org/login POST password=bms123.
    #   但仪表盘真实密码由用户管理(用户表非空时 bms123 已失效), 每次都是"失败登录";
    #   请求经 Cloudflare 回源, 服务端按 X-Forwarded-For 的公网 IP 累计失败次数,
    #   满 5 次即锁定该公网 IP 10 分钟 —— 用户/同网设备一打开网页就见"已锁定",
    #   而 watchdog 自身浑然不觉(它只看 /api/status 是否 200, 登错后拿不到 200 反而会
    #   误重启 cloudflared). 现 GET /api/status: 401=需登录但服务正常, 200=正常,
    #   403=被拦截但服务在, 均判健康; 仅 5xx/连接失败=真正故障. 该探测绝不触发登录计数.
    $wanCounter++
    if (($wanCounter % 5) -eq 0) {
        try {
            Add-Type -AssemblyName System.Net.Http
            $hc = New-Object System.Net.Http.HttpClient
            $hc.Timeout = [TimeSpan]::FromSeconds(22)
            $r = $hc.GetAsync('https://bms0605.dpdns.org/api/status').Result
            $code = [int]$r.StatusCode
            # 2xx/3xx/401/403 = 服务存活(4xx 说明鉴权/路由在工作); 仅 5xx/连接失败=故障
            if ($code -lt 500) { wlog ("[OK] public HTTPS OK: https://bms0605.dpdns.org (status {0})" -f $code); $failWan = 0 }
            else { throw "api/status HTTP $code" }
            $hc.Dispose()
        } catch {
            $failWan++
            wlog ("[WARN] public HTTPS failed ({0} consecutive): {1}" -f $failWan, $_.Exception.Message)
            if ($failWan -ge 3) {
                wlog "[RESTART] Restarting Cloudflared ..."
                Stop-All "cloudflared%" ""
                Start-Sleep -Seconds 3
                Start-Cloudflared | Out-Null
                $failWan = 0
            }
        }
    }
    wlog "--- next round 180s ---"
    Start-Sleep -Seconds 180
}
