# 华为云 IoTDA 物模型（产品模型）使用说明

> 文件：`tools/dashboard/backend/iotda_product_model.json`
> 目的：在华为云 IoTDA 控制台导入物模型后，**产品页面即可可视化查看/调试设备属性、命令与事件**，无需自己拼 JSON。

## 一、这个物模型覆盖了什么

| 类别 | 数量 | 内容 |
|---|---|---|
| 属性(上报) | 17 | soc / soh / v_max / v_min / pack_v / current / temp_max / temp_min / fault / cells / balance_mask / cycle_count / timestamp / charge_mos / discharge_mos / balance_on / charge_mode |
| 命令(下发) | 11 | set_charge / set_discharge / set_balance / set_relay / set_param / clear_alarm / restart / ota_check / ota_upgrade / reset_params / get_info |
| 事件(上报) | 1 | fault_report（故障事件） |

所有字段名、单位、取值范围均与固件 `sys_mqtt.c`（`sys_mqtt_report`）和 `app_tasks.c`（`mqtt_cmd_callback`）**严格对齐**。

## 二、导入步骤

1. 登录[华为云 IoTDA 控制台](https://console.huaweicloud.com/iothub) → 选择**标准版实例**。
2. 左侧「产品」→ 进入你的 BMS 产品（若还没有产品，先新建产品：
   - 所属行业：智慧能源 / 自定义
   - 设备类型：`BMS_DEVICE`
   - 协议类型：MQTT
   - 数据格式：JSON）。
3. 在产品详情页 →「模型定义」→ 右上角 **「导入模型」** → 选择 `tools/dashboard/backend/iotda_product_model.json` → 确认导入。
4. 若产品已存在旧模型，可选择「覆盖导入」（先备份旧定义）。

## 三、导入后的调试能力

### 1. 在线调试（设备已连接时）
- 产品页 →「在线调试」→ 选设备。
- **上报属性**：可看到 soc/pack_v/fault 等实时值；点「读取」可主动查询设备最新影子。
- **命令下发**：下拉选 `set_charge` 填 `{"enable":1}` 即可远程开充电；`set_param` 填 `{"key":"cell_ov_prot_mv","value":3650}` 改保护阈值——**无需任何代码，直接验证固件命令回调**。

### 2. 规则引擎联动（可选，P3 方案）
物模型导入后，IoTDA 规则引擎可基于属性值创建流转规则（如 `fault != 0` 时转发到 SMN 短信 / FunctionGraph），数据流转免费赠送额度内不额外计费。

### 3. 应用侧 SDK
导入物模型后，应用侧可直接用产品模型 SDK 的 `SetDeviceProperty` / `CreateMessage` 下发，字段与模型一致，减少手写 JSON 出错。

## 四、与现有 Dashboard 的关系

- Dashboard 走的是**原生 REST API**（`iotda_client.py` 手工 HMAC 签名），**不依赖物模型**即可工作。
- 物模型是**补充能力**：主要解决「在华为云控制台直接调试设备、可视化查看属性」的需求；导入与否不影响现有网页功能。
- 若你想用 `commands` API（`/commands` 下发）替代当前 `/messages` 下发，则必须先在控制台导入本物模型，否则会报 `IOTDA.014108`（命令名未在产品模型中定义）——当前固件/看板用 `/messages` 已规避此限制。

## 五、字段速查（调试时参考）

| 属性 | 单位 | 说明 |
|---|---|---|
| soc / soh | % | 0~100（固件内部 0.0~1.0，上报 ×100） |
| v_max / v_min / pack_v | mV | 单体最高/最低/总压 |
| current | mA | 正=放电，负=充电 |
| temp_max / temp_min | 0.1℃ | 如 250 = 25.0℃（负温为负数） |
| fault | 0x | 32 位故障掩码（`bms_types.h` FAULT_xxx） |
| cells | string | `"[3700,3680,...]"`，长度=串数（6~16 自适应） |
| balance_mask | 0x | 均衡掩码，位=串号 |
| charge_mode | - | 0=STOP 1=CC 2=CV 3=FULL |

## 六、常见问题

- **导入报「JSON 格式错误」**：确认选择了「模型定义」→「导入模型」而非「导入产品」，且文件为 UTF-8。
- **命令下发无响应**：先确认设备在线（MQTT 已连接），命令走 `messages/down` topic，固件 `mqtt_cmd_callback` 解析 `{cmd, paras}`。
- **属性显示旧值**：影子更新有秒级延迟，稍等 10s 刷新。

## 七、均衡模式说明（2026-08-07 新增：主动/被动双模式）

`set_balance` 命令现在支持 `mode` 字段，固件端 `app_balance_process` 按模式分流：

| mode | 含义 | 硬件 | 均衡电流 |
|---|---|---|---|
| `passive` | 被动均衡 | LTC6804 内部放电 MOS（电阻耗散） | <100mA |
| `active` | 主动均衡 | 外接反激/电感/电容电路 + PWM 调流（`bsp_active_balance` 驱动，`PIN_ACTIVE_BAL_EN/PWM`） | 0.5~5A |
| `off` | 关闭 | — | — |

- 前端「均衡控制」面板已有被动/主动按钮，点击即下发 `set_balance {enable, mode, mask}`。
- 固件默认 `BALANCE_MODE_PASSIVE`；未接线时主动均衡驱动自动屏蔽（`HW_ENABLE_ACTIVE_BALANCE=0`），日志提示后忽略，不影响系统。
- 主动均衡硬件接线后：将 `bms_config.h` 的 `HW_ENABLE_ACTIVE_BALANCE` 置 1，并接好 EN/PWM 引脚即可启用；转移电流在 `app_balance.c` 的 `ACTIVE_BAL_CURRENT_MA` 调整。
