# 固件 Bug 排查报告（ESP32-S3 / ESP-IDF v5.2.7 / FreeRTOS）

> 排查范围：`components/{bms_app, bms_bsp, bms_middleware, bms_config}` 全部 C/H
> 方法：全量扫描 + 关键路径逐行复核（任务/栈、共享状态竞态、ISR、内存、整数运算、缓冲边界、看门狗）
> 结论：**固件整体质量良好**，并发设计与错误处理明显经过多轮打磨；以下为仍存在的真实缺陷（按严重度）。

---

## 一、已确认缺陷

### B1【中危】MQTT client 句柄 / 连接态跨任务竞态（无 volatile、无锁）
- **位置**：`components/bms_middleware/sys_mqtt.c`
  - 定义：`s_client`（:100，普通 `static` 指针）、`s_connected`（:101，普通 `static bool`）
  - 写（MQTT 事件任务）：`:973` `s_connected=true`、`:1010/:1305/:1680` `=false`、`:1270` `s_client=esp_mqtt_client_init(...)`、`:1304` `s_client=NULL`
  - 读（`task_comm`）：`:1575/:1596/:1618/:1658/:1676` publish 前判 `s_connected||s_client==NULL`
  - **危险路径**：`cache_replay_send_func`（:132-144）在 `task_comm` 中直接用 `s_client` 发布，无锁
- **根因**：ESP32-S3 双核，这两个全局既无 `volatile` 也无互斥，`task_comm` 可能 (a) 因编译器寄存器缓存/核间不可见而读到过期 `s_connected` → 漏发或重复发；(b) 在重连 `destroy→s_client=NULL→init` 窗口内读到"非 NULL 的旧句柄"并向已销毁 client 发布 → use-after（极端时序可崩）。
- **修复**：
  1. 对 `s_client`/`s_connected` 的读写全程加一把 mutex（建议与现有双缓冲思路一致）；
  2. 或 publish 前改调 `esp_mqtt_client_is_connected(s_client)`（库内部线程安全），并对 `s_client` 切换持锁。

### B2【低危】`s_last_command_id[64]` 跨任务读写无锁
- **位置**：`sys_mqtt.c` 写于 `handle_command`/`handle_device_message`（命令下发时记录 request_id），读于 `task_comm` 经 `sys_mqtt_report_info→handle_get_info`（:432/:441）。
- **风险**：comm 任务可能读到半截 command_id → 一条错误的"命令响应"信息上报。影响极小。
- **修复**：把 command_id 作为栈参数传给 `handle_get_info`，去掉共享全局。

### B3【低危】继电器 set 的 TOCTOU（远程指令 vs 保护任务）
- **位置**：`components/bms_app/app_tasks.c`
  - `SET_CHARGE`：`:1026` `get_relay` → `:1027` `set_relay` → `:1029` `set_remote_override(true)`
  - `SET_RELAY`：`:1076` `set_relay` → `:1078` `set_remote_override(true)`
- **根因**：`get_relay` 与 `set_relay` 是两个独立加锁操作，`remote_override` 在 set **之后**才置位。二者之间的窗口内，`app_protection_process`（`app_protection.c:580`）可能调用 `sys_data_set_relay()` 覆盖本次远程设置。
- **概率**：低（保护 100ms 节拍、覆盖 30s 超时），但确属保护/远程冲突。
- **修复**：先 `set_remote_override(true)` 再 `set_relay`；或对 get+set 持一把锁。

### B4【硬件相关】`bsp_ltc6804_read_gpio` 只校验 chip0 的初始化
- **位置**：`components/bms_bsp/bsp_ltc6804.c:317` 仅 `!s_inited[0]` 判错，温度始终从 `ltc_read_cmd_chip(0,…)` 读取（注释 :320-321 已说明"多芯片扩展待做"）。
- **风险**：当 `LTC6804_CHIP_NUM>1` 且 NTC 分布在其他芯片 GPIO 上时，`pack->temp_dc[]` 对那部分电芯恒为 0 → 温度/过温保护失明。
- **处置**：取决于实际硬件拓扑。单芯片或 NTC 全在 chip0 则无影响；多芯片需按芯片循环汇总。请核对硬件接线。

### B5【防御性】`cell_dv` 无符号减法（极低概率下溢）
- **位置**：`components/bms_app/app_protection.c:442` `uint16_t cell_dv = pack->cell_mv_max - pack->cell_mv_min;`
- **分析**：正常情况下 `max/min` 来自同一 `pack` 快照（`sys_data_set_pack` 整体拷贝），不会 `min>max`，当前不触发。但若将来改为分别更新或异步刷新，回绕成 ~65535mV 会误报 `FAULT_CELL_DV_WARN`。
- **修复**（一行，廉价）：`if (min<=max) cell_dv = max-min; else cell_dv = 0;`

### B6【中危】NVS 写去抖窗口内只置脏不落盘 → 配网/set_wifi 保存后立即重启丢配置（2026-08-13 已修）
- **位置**：`components/bms_middleware/sys_params.c`（`params_commit`）、`sys_config_portal.c`（`handler_save`）、`sys_wifi.c`（`sys_wifi_save_config`）
- **根因**：`params_commit` 在 `PARAMS_NVS_COALESCE_MS=2000ms` 去抖窗口内**只置 `s_nvs_dirty`、不写 Flash** 即返回 `BMS_OK`；配网门户保存成功后仅延迟 **1s** 就 `esp_restart()`，若距上次 NVS 写不足 2s，脏数据未落盘便重启 → 新 WiFi 配置丢失，设备继续连旧路由器。MQTT `set_wifi` 路径（`sys_mqtt.c` 两处）同理。
- **修复**：新增 `sys_params_flush_now()`（无条件写活动缓冲到 Flash，全程持锁）；`handler_save` / `sys_wifi_save_config` 保存成功、重启前显式调用，保证配置真正持久化。
- **验证**：ESP-IDF v5.2.7 编译通过；详见 `08_缺陷修复执行记录.md`（2026-08-13 追加）与 `04_故障排查指南.md` §2.16。

---

## 二、已确认正确（亮点，不必改）

- **初始化顺序**：`sys_data_init()`（`app_tasks.c:1113`）在 `sys_mqtt_init()`（:1246）与 `app_tasks_start()`（:1931）之前执行 → 互斥锁恒先于任务/ISR 存在，无"加锁变 no-op"窗口。
- **`sys_data` 并发**：所有 pack/soc/fault/relay/balance/derating/charge-mode 读写均经 mutex 保护的 get/set，且 `s_state` 为 `static` 不暴露；结构体整体拷贝，无撕裂读。
- **`sys_params` 并发**：双缓冲 + mutex + 活动指针原子切换（M1 修复跨核撕裂读），NVS 合并落盘脏标记全程持锁——教科书级做法。
- **`bsp_button` ISR**：中断仅置 `s_irq_flag`（:43-47），消抖/长短按/双击状态机全在任务态（:89-174），符合 ISR 规范。
- **OLED 缓冲**：`bsp_oled.c:158-175` 对 `y<OLED_HEIGHT/8`、`x+i<OLED_WIDTH` 做边界检查，所有 `snprintf` 用 `sizeof()`，framebuffer 不会越界。
- **内存管理**：`unwrap_cmd_root` 全路径释放 cJSON 树（:514-539）；`app_ota.c:109` malloc 在错误(:130)/成功(:139)两路径均 free；`sys_config_portal.c` calloc/free 成对。
- **离线缓存**：批量上限 20 条（H22）、300KB 上限裁剪（#3）、temp-file 重放避免无限重发（#2）、成功节流（2026-08-12）——均为已记录修复。
- **看门狗**：HW TWDT 仅监控 `task_watchdog`（:1793），其余任务由 SW 看门狗 `esp_restart` 兜底；`app_tasks.c:1860` 的 `|| false` 为无害死代码（C1）。

---

## 三、纠正 2026-08-09 的相关结论

- **D2（set_param 被静默拒绝）：已修复**。固件 08-12 更新后 `handle_set_param`（`sys_mqtt.c:198-199`）改用 `cmd_get_field()` 从 `paras` 优先取值，且 `:315` 调用 `sys_params_set()` 落盘。参数（阈值/串数/容量/SOC 偏移增益/算法）下发**现在完全生效**。
- **D1（远程控制只能关不能开）：属 Python 后端 bug，非固件**。根因是 `tools/dashboard/iotda_client.py` 的 `publish_cmd` 只转发 `paras`，而前端把 `enable` 放 body 顶层 → 设备收到 `{cmd, paras:{}}`。注意：**固件侧 `cmd_get_bool`/`cmd_get_field` 同时兼容 paras 与顶层**，解析是健壮的；修 D1 只需在 `app.py /api/cmd` 把顶层非 cmd 字段并入 `paras`。

---

## 四、建议优先级

1. **B1**（中危，重连时序/双核可见性）—— 建议修，加 mutex 或改用 `esp_mqtt_client_is_connected`。
2. **B3**（低危，保护/远程冲突）—— 建议修，先置 `remote_override` 再 set_relay。
3. **B4** —— 核对硬件拓扑后决定是否扩展多芯片测温。
4. **B2 / B5** —— 顺手修，成本低。
