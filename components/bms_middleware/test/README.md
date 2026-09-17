# bms_middleware 单元测试(Unity, 宿主侧)

> **状态(2026-09-07): 已编写, 未接入构建。**
> 本目录的 `test_*.c` 使用 [Unity](https://github.com/ThrowTheSwitch/Unity) 框架,
> 目标是在 PC 上运行(不需要板子)。当前工程的 CMakeLists 未包含本目录,
> 固件编译不会编译/运行这些测试——它们不是死代码, 而是尚未挂载的测试套件。

## 覆盖范围

| 文件 | 被测模块 | 内容 |
|------|---------|------|
| `test_sys_mqtt.c` | `sys_mqtt.c` | MQTT 上报 JSON 构造、命令解析(set_param/switch_battery)、串数校验 |

## 运行方法

同 `components/bms_app/test/README.md`(Unity + 宿主 CMake + ESP-IDF 头文件 stub)。
sys_mqtt 依赖 cJSON, host 侧可引入 cjson 组件(ESP-IDF 自带)。

## 维护约定

- 改 `sys_mqtt.c` 上报格式/命令解析后, 同步更新对应用例
- 串数校验用例必须用 `BMS_HW_MAX_SERIES_NUM`(硬件上限), 不要写死 16
