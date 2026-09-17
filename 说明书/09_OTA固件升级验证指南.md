# OTA 固件升级验证指南（说明书第 09 篇）

> 目的：验证 ESP32-S3 通过 HTTPS 从 Dashboard 后端拉取新固件并升级（A/B 双分区 + 回滚保护）的功能链路是否打通。
> 适用：BMS 项目（ESP-IDF v5.2.7 / ESP32-S3 / 16MB Flash，OTA 分区 ota_0/ota_1 各 7MB）。

---

## 一、OTA 工作原理（固件侧）

```
┌────────────┐  HTTPS GET /api/ota/version.json   ┌──────────────────┐
│  ESP32-S3  │ ──────────────────────────────────► │  Dashboard 后端  │
│  (task_ota)│                                     │  (backend/app.py)│
│            │ ◄────────────────────────────────── │  {version, url,  │
│            │      {"version":"1.0.226","url":...}│   size, md5}     │
│            │                                     │                  │
│            │  HTTPS GET /api/ota/firmware.bin    │                  │
│            │ ──────────────────────────────────► │  读取 bms.bin    │
│            │ ◄────────────────────────────────── │  (流式下发)      │
│            │      bms.bin (写入 ota_1 分区)      │                  │
└────────────┘                                     └──────────────────┘
```

### 1.1 关键代码位置
| 文件 | 作用 |
|------|------|
| `components/bms_app/app_ota.c` | OTA 核心：`app_ota_init`（校验分区/防回滚）、`app_ota_check_and_upgrade`（自动检查升级）、`app_ota_upgrade_with_url`（强制 URL 升级）、`fetch_remote_version`、`compare_version`、`download_and_flash` |
| `components/bms_app/app_tasks.c` | `task_ota` 任务（事件触发 + 12h 周期保底检查）+ `ota_scheduler_request_check/upgrade`（MQTT 命令异步调度） |
| `components/bms_config/include/bms_config.h` | `BMS_OTA_VERSION_URL` / `BMS_OTA_FIRMWARE_URL` / `BMS_OTA_CHECK_PERIOD_MS` / `BMS_FIRMWARE_VERSION` |
| `tools/dashboard/backend/app.py` | 后端 OTA 服务：`/api/ota/version.json`、`/api/ota/firmware.bin` |

### 1.2 升级流程（两段式：检查 → 确认 → 升级，2026-08-13 起）

> 行业惯例：**默认人工确认**——检查到新版本只上报，等用户在前端确认后才升级。
> 无人值守设备可把 NVS 开关 `ota_auto_confirm` 设为 1 恢复全自动。

**阶段 A：检查（只查不升）**
1. `task_ota` 触发（启动后 ~10s 首次 / 每 12h 保底 / MQTT `ota_check` / 前端"检查更新"按钮）
2. `app_ota_check_and_upgrade()` 拉取 `version.json`（URL 来自 NVS `ota_version_url`）
3. `compare_version(local, remote)` 三段式版本比较（`主.次.修`）
4. **远程版本 > 本地版本** → 缓存待升级信息（`sys_ota_status`：版本号 + URL）→ 经 MQTT `get_info`/`bms_info` 上报 `ota_new_version` 给前端 → **不下载、不重启**
5. 远程版本 ≤ 本地 → 清空待升级缓存，不动作

**阶段 B：确认（用户在前端决定）**
6. 前端收到 `ota_new_version` → 显示"发现新版本 vX.X.X"→ 弹窗询问"是否立即升级？"
7. 用户点"确定" → 前端下发 `ota_upgrade`（带固件 URL）；点"取消" → 不动，下次检查再提示

**阶段 C：升级（仅确认后执行）**
8. `app_ota_upgrade_with_url()` 流式下载 bms.bin 写入**非运行**的 OTA 分区
9. 校验通过（镜像大小 > 0 + `esp_https_ota_finish` 内部校验）→ 写 `ota_upgraded` 标记（保留 WiFi 配置）→ 3s 后 `esp_restart()`
10. 新固件启动 `app_ota_init()` 检测 `PENDING_VERIFY` → `esp_ota_mark_app_valid_cancel_rollback()` 标记 valid，防止下次启动回滚

> **开关**：`sys_params_set_ota_auto_confirm(true)` 后，阶段 A 的第 4 步改为直接进入阶段 C（全自动升级），跳过用户确认。

### 1.3 版本号比较规则
- `"1.0.1"` vs `"1.0.2"` → 远程大 → 升级
- `"1.0.1"` vs `"1.0.1"` → 相同 → 不升级（`<= 0` 跳过）
- 远程 `version` 字段缺失/格式错 → WARN 跳过（非 ERROR）

---

## 二、后端 OTA 服务（Dashboard 侧）

### 2.1 固件查找优先级（`_find_firmware_bin`）
| 优先级 | 路径 | 说明 |
|--------|------|------|
| 1 | `tools/dashboard/firmware/bms.bin` | 正式发布目录（推荐） |
| 2 | `tools/dashboard/firmware/*.bin` | 该目录下最新匹配 bms/firmware 的 bin |
| 3 | `<项目根>/build/bms.bin` | 开发期自动读取编译产物 |

### 2.2 version.json 生成规则（`_read_version_meta`）
- **手动优先**：`tools/dashboard/firmware/version.json` 或 `build/version.json`（需含 `version` + `url` 字段）
- **自动生成**：无手动文件时，版本 = `1.0.${自2026-01-01天数}`，url = 自动拼 `http(s)://<host>/api/ota/firmware.bin`，附 `size`/`md5`/`build_time`

```json
// 自动生成示例（GET http://<YOUR_ECS_IP>:5000/api/ota/version.json）
{
  "version": "1.0.226",
  "url": "http://<YOUR_ECS_IP>:5000/api/ota/firmware.bin",
  "size": 1206880,
  "md5": "…",
  "build_time": "2026-08-13 13:51:55"
}
```

### 2.3 端点
| 端点 | 说明 |
|------|------|
| `GET /api/ota/version.json` | 版本信息（**匿名可访问**，ESP32 无登录态） |
| `GET /api/ota/firmware.bin` | 固件二进制（流式下发，64KB chunk） |
| `GET /api/ota_check` | 前端"检查升级"按钮（受 CSRF 保护，走 `ota_check` 命令） |

---

## 三、触发 OTA 的 3 种方式

| 方式 | 操作 | 行为(默认人工确认) |
|------|------|---------------------|
| **① 启动后自动检查** | 设备开机 ~10s 后 `task_ota` 自动拉一次 version.json | 只检查上报，等确认 |
| **② 周期保底检查** | 每 `BMS_OTA_CHECK_PERIOD_MS`（默认 12h）自动检查一次 | 只检查上报，等确认 |
| **③ 前端/MQTT 命令（推荐验证用）** | Dashboard "🔍 检查更新" 或华为云下发 `ota_check` / `ota_upgrade` | 检查→弹窗确认→升级 |

MQTT 命令格式（华为云 IoTDA 控制台 / Dashboard 网页）：
```json
// ota_check：触发版本检查（人工确认模式下只上报新版本, 不升级）
{"command_name": "ota_check", "paras": {}}

// ota_upgrade：用户确认后强制用指定 URL 升级（跳过版本比较）
{"command_name": "ota_upgrade", "paras": {"url": "http://<YOUR_ECS_IP>:5000/api/ota/firmware.bin"}}
```

---

## 四、验证步骤（完整流程）

### 前置条件
- [ ] Dashboard 后端已启动（`backend/app.py`，端口 5000）
- [ ] 公网可达：`http://<YOUR_ECS_IP>:5000/api/ota/version.json` 浏览器能打开
- [ ] 设备已配网 + 能连华为云 IoTDA（MQTT 在线）
- [ ] `idf.py monitor` 串口窗口已打开（观察日志）

### Step 1：确认后端能提供固件
```bash
# 1. 确认 bms.bin 存在（build/ 或 firmware/ 至少一处）
ls -la "D:\esp32project\BMS\BMS System\build\bms.bin"

# 2. 本地验证 version.json（后端需在运行）
curl http://localhost:5000/api/ota/version.json
# 应返回 {"version":"1.0.xxx","url":"http://localhost:5000/api/ota/firmware.bin",...}

# 3. 公网验证（经 cloudflared 隧道）
curl http://<YOUR_ECS_IP>:5000/api/ota/version.json
```

### Step 2：确认设备本地版本
串口日志（`idf.py monitor`）应看到：
```
[APP_OTA] OTA 初始化, 当前固件版本=1.0.1, 运行分区=ota_0
```
> 注意：本地版本取 NVS `firmware_version`，与 `bms_config.h` 的 `BMS_FIRMWARE_VERSION` 一致。

### Step 3：确保远程版本 > 本地版本
后端自动生成的版本 `1.0.${天数}`（2026-08-13 ≈ 1.0.226）**远大于**固件内 1.0.1，**通常无需手动改**。
若需要手动指定版本，创建 `tools/dashboard/firmware/version.json`：
```json
{"version": "2.0.0", "url": "/api/ota/firmware.bin"}
```
（改后重启后端生效）

### Step 4：触发 OTA 检查
推荐方式（任选其一）：
- **前端（推荐）**：Dashboard → "🚀 OTA 远程固件升级" → 点 **🔍 检查更新**（前端会先下发 `ota_check`，再拉 version.json 对比版本，发现新版本弹窗确认）
- **串口最快**：设备刚开机 ~10s 会自动检查一次（日志见 Step 5），或等 12h 保底
- **MQTT 命令**：华为云控制台 → 设备 → 命令下发 → `ota_check`

### Step 5：观察检查日志（两段式·阶段 A：只查不升）
```
[APP_OTA] OTA 检查, version_url=http://<YOUR_ECS_IP>:5000/api/ota/version.json
[APP_OTA] version.json: {"version":"1.0.226","url":"https://.../firmware.bin",...}
[APP_OTA] 本地版本=1.0.1 远程版本=1.0.226
[APP_OTA] 发现新版本 1.0.226
[APP_OTA] 新版本 1.0.226 待用户确认(人工确认模式), 已上报前端由用户决定升级
```
> 到这里**升级尚未发生**——固件只缓存了新版本并经 MQTT 上报 `ota_new_version`。

### Step 5b：前端确认（两段式·阶段 B）
前端应弹出：`🔔 发现新版本 v1.0.226 ... 是否立即升级?`
- 点 **确定** → 前端自动下发 `ota_upgrade`（带固件 URL）→ 进入阶段 C
- 点 **取消** → 不升级，下次检查再提示

### Step 6：观察升级日志（阶段 C：确认后执行）
```
[APP_OTA] 自动确认模式? 或 [OTA] 触发强制升级: https://.../firmware.bin
[APP_OTA] 开始下载: https://.../api/ota/firmware.bin
[APP_OTA] OTA 进度: 10% ... 100%
[APP_OTA] OTA 镜像大小 xxx 字节, 通过合理性校验
[APP_OTA] OTA 下载完成, 即将重启
[APP_OTA] OTA 升级成功, 3 秒后重启
```

### Step 7：确认升级成功（重启后）
```
[APP_OTA] OTA 初始化, 当前固件版本=1.0.226, 运行分区=ota_1   ← 版本+分区都变了!
[APP_OTA] OTA 新固件验证通过, 已标记 valid (防回滚)
[SYS_WIFI] OTA 升级后首次启动, 保留 WiFi 配置 (标记已消费)    ← WiFi 不掉线
```
- ✅ 设备自动连回原 WiFi（MQTT 恢复在线）
- ✅ 网页 Dashboard 显示新版本号（属性上报 firmware_version）

---

## 五、验证结果判断

| 现象 | 结论 |
|------|------|
| 检查日志出现"待用户确认(人工确认模式)"，前端弹窗确认后下载升级，重启后版本/分区更新 | ✅ **OTA 功能正常（两段式）** |
| 检查日志出现"已是最新版本" | 正常，本地已是远程版本 |
| 前端弹窗"发现新版本"但用户点取消 | 正常，不升级（行业惯例人工确认） |
| 日志直接出现"发现新版本 → 开始升级"（无确认） | 设备 `ota_auto_confirm=1`（无人值守自动模式） |
| `version.json` 404 / HTTP 非 200 | ⚠️ 后端未部署或路径错，WARN 跳过属正常（开发期占位） |
| 下载后 `OTA 镜像校验失败` | 固件损坏/被截断，检查 URL 与文件完整性 |
| 重启后回到旧分区（回滚） | 新固件启动未及时 `mark valid`（正常 30s 内标记，看 app_ota_init 日志） |

---

## 六、常见问题排查

### 6.1 version.json 拉取失败（HTTP open fail / 404）
- 后端是否运行：`Get-NetTCPConnection -LocalPort 5000` 应 LISTENING
- 公网是否可达：浏览器打开 `http://<YOUR_ECS_IP>:5000/api/ota/version.json`
- 隧道是否健康：`tools/dashboard/logs/cloudflared.log` / `logs/named_tunnel.log`
- **注意**：隧道 ingress 的 service 必须写 `http://127.0.0.1:5000`（不能写 localhost，否则 IPv6 ::1 解析导致 502）

### 6.2 提示"已是最新版本"不升级
- 远程 version ≤ 本地版本 → 手动在 `firmware/version.json` 写更大版本号（如 2.0.0）后重启后端
- 或用 `ota_upgrade` 命令带 `url` 强制升级（跳过版本比较）

### 6.3 升级后 WiFi 掉线/重新配网
- 正常应保留 WiFi（OTA 成功路径写 `ota_upgraded` 标记）
- 若误触发重新配网：确认烧录的是含 B7 修复的最新固件；串口应有 `OTA 升级后首次启动, 保留 WiFi 配置`

### 6.4 升级后回滚（自动回到旧版本）
- 新固件启动后 `app_ota_init` 必须在 `PENDING_VERIFY` 时调用 `esp_ota_mark_app_valid_cancel_rollback()`
- 若新固件崩溃/看门狗复位，会回滚旧分区——这是 A/B 分区的**正常保护机制**

### 6.5 日志刷 ERROR（开发期）
- OTA 服务器未部署时 `HTTP open fail` 是 WARN（设计如此），部署后自然消失
- 12h 保底检查周期内无事件时每秒 `sec_cnt` 累加无日志

---

## 七、常见命令速查

```bash
# 后端本地验证
curl http://localhost:5000/api/ota/version.json
curl -o /dev/null -w "%{http_code} %{size_download}B" http://localhost:5000/api/ota/firmware.bin

# 公网验证（cloudflared 隧道）
curl http://<YOUR_ECS_IP>:5000/api/ota/version.json

# 手动放置正式固件（推荐发布方式）
mkdir -p tools/dashboard/firmware
cp build/bms.bin tools/dashboard/firmware/bms.bin
# 可选：指定版本
echo '{"version":"2.0.0","url":"/api/ota/firmware.bin"}' > tools/dashboard/firmware/version.json
# 重启后端使生效

# 设备端触发（华为云控制台命令下发 / Dashboard 按钮）
#   ota_check   → 版本检查（人工确认模式: 只上报新版本, 不升级; 前端弹窗确认后才升）
#   ota_upgrade → {"url":"http://<YOUR_ECS_IP>:5000/api/ota/firmware.bin"} 用户确认后强制升级
```

---

*生成日期：2026-08-13。对应代码：`app_ota.c`（固件）、`backend/app.py` §OTA 服务（后端）、`app_tasks.c` task_ota（调度）。*
