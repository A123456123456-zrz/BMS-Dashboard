# OTA 固件发布与回滚操作说明

> 适用项目:主项目 `D:\esp32project\BMS\BMS System`
> 更新日期:2026-08-13
> 关联代码:`components/bms_config/include/bms_config.h`(版本宏)、
> `components/bms_middleware/sys_params.c`(NVS 版本刷新)、
> `tools/dashboard/backend/app.py`(OTA 服务路由)

---

## 一、firmware/ 目录结构

```
tools/dashboard/firmware/
├── bms.bin              ← 当前发布版固件(保持最新, 根目录只留这一个)
├── bms_1.0.1.bin        ← 历史版本(可保留, 用 download 路由指定下载)
├── bms_1.0.2.bin
├── archive/             ← (可选)历史版本归档, 扫描不递归, 最干净
└── version.json         ← 版本发布配置(设备拉取它决定是否升级)
```

> ⚠️ 根目录**只放 `bms.bin`**。历史版本 bin 直接堆根目录时,
> `_find_firmware_bin()` 会按文件名倒序自动挑一个(可能误选),
> 导致设备下载错固件。历史版本请用 `download/` 路由显式指定,
> 或放进 `firmware/archive/` 子目录。

---

## 二、OTA 服务的两条升级路径

| 路径 | 触发方式 | 比较版本? | 能否降级 |
|---|---|---|---|
| **自动检查升级** | 设备每 12h 拉 version.json(或手动 ota_check) | ✅ 远程 > 本地才升 | ❌ 不能 |
| **强制升级** | 前端"固件下载 URL"框(admin)下发 ota_upgrade | ❌ 不比较, 直接刷 | ✅ 能 |

### 服务端接口(均允许匿名, ESP32 无登录态)

| 接口 | 作用 |
|---|---|
| `GET /api/ota/version.json` | 版本信息 `{version, url, size, build_time}` |
| `GET /api/ota/firmware.bin` | 下载当前发布固件(firmware/bms.bin) |
| `GET /api/ota/download/<filename>` | **下载指定历史版本**(firmware/ 下任意 .bin, 防目录穿越) |
| `GET /api/ota/info` | 调试信息(当前固件路径/版本/大小) |

---

## 三、发布新版本(自动升级, 正常流程)

**四步, 缺一不可:**

### 第 1 步:改固件版本号

编辑 `components/bms_config/include/bms_config.h`:

```c
#define BMS_FIRMWARE_VERSION  "1.0.2"   ← 改成新版本, 如 "1.0.3"
```

> 这决定**设备升级后自己上报的版本**。不改的话, 设备升级完仍报旧版本,
> 下次检查会误判"有新版本"重复下载。

### 第 2 步:编译固件(主项目用 py3.14 venv)

```bash
cd "D:/esp32project/BMS/BMS System"
cmd //c "set MSYSTEM=& set PATH=C:\Espressif\python_env\idf5.2_py3.14_env\Scripts;%PATH% & call D:\v5.2.7\esp-idf\export.bat >nul 2>&1 & idf.py build"
```

产物:`build/bms.bin`

### 第 3 步:替换发布固件

```bash
copy /Y "build\bms.bin" "tools\dashboard\firmware\bms.bin"
```

### 第 4 步:更新 version.json

编辑 `tools/dashboard/firmware/version.json`:

```json
{"version": "1.0.3", "url": "/api/ota/firmware.bin"}
```

> `version` **必须比客户设备当前版本大**, 且只增不减;
> `url` 一般保持 `/api/ota/firmware.bin`(指向当前发布版)。

### 生效时机

- 设备每 12h 自动检查;或前端点"检查更新"(下发 ota_check)立即触发;
- 远程版本 > 本地版本 → 自动下载升级 → 重启切换分区。

---

## 四、发布指定历史版本 / 定点推送

把 version.json 的 `url` 指向具体文件即可(设备按 url 下载):

```json
{"version": "1.0.1", "url": "/api/ota/download/bms_1.0.1.bin"}
```

- 适合:某台设备需要旧版行为、灰度验证历史版本。
- 前提:对应文件已存在于 `tools/dashboard/firmware/` 下。

---

## 五、回滚(降级到旧版本)

> 自动检查**不能**降级(远程 ≤ 本地视为"已是最新")。
> 回滚必须走**强制升级** + **同步降 version.json**, 两步配合:

### 第 1 步:前端 URL 框强制刷旧固件

1. 用 **admin 账号**登录 dashboard(`/api/ota/...` 匿名, 但 ota_upgrade 命令限 admin);
2. 打开 OTA 区块 → "固件下载 URL"输入框;
3. 粘贴历史版本直链:

   ```
   http://<YOUR_ECS_IP>:5000/api/ota/down<BROKER_PASSWORD>load/bms_1.0.1.bin
   ```

4. 点"升级"→ 确认弹窗 → 设备直接下载该固件写入重启(不比较版本)。

### 第 2 步:同步修改 version.json(防止自动升回)⚠️

回滚后设备是旧版本, 若 version.json 仍写更高版本,
设备下次自动检查会发现"有新版本"**又自动升回去**。

回滚后把 version.json 改成与设备一致(或临时移除):

```json
{"version": "1.0.1", "url": "/api/ota/firmware.bin"}
```

### 回滚注意

- 回滚只用于紧急情况(新版本有严重 bug 等);
- 回滚后再发布, 版本号应继续增大(如 1.0.3), 不要复用旧号;
- NVS 版本刷新逻辑(2026-08-13 新增)会在新固件首启时把上报版本号
  同步为编译宏 `BMS_FIRMWARE_VERSION`, 无需手动处理。

---

## 六、发布红线清单

- [ ] 发布前确认 **ECS 后端在线**(客户设备靠 `http://<YOUR_ECS_IP>:5000` 下载);
- [ ] `bms_config.h` 版本号已改大并编译;
- [ ] `firmware/bms.bin` 已是新编译产物;
- [ ] version.json 版本号与固件一致、url 正确;
- [ ] 新固件先在自己设备上强制升级验证(灰度), 再放开自动升级;
- [ ] 版本号只增不减, 不向下覆盖;
- [ ] 历史 bin 保留用 `download/` 路由或 `archive/` 子目录, 不污染根目录。
