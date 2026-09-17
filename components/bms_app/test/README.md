# bms_app 单元测试(Unity, 宿主侧)

> **状态(2026-09-07): 已编写, 未接入构建。**
> 本目录的 3 个 `test_*.c` 使用 [Unity](https://github.com/ThrowTheSwitch/Unity) 框架,
> 目标是在 PC 上运行(不需要板子)。当前工程的 CMakeLists 未包含本目录,
> 固件编译不会编译/运行这些测试——它们不是死代码, 而是尚未挂载的测试套件。

## 覆盖范围

| 文件 | 被测模块 | 内容 |
|------|---------|------|
| `test_app_protection.c` | `app_protection.c` | 过/欠压、过温、过流保护判定与滞回、故障掩码 |
| `test_app_soc.c` | `app_soc.c` | SOC 算法输入校验、边界条件 |
| `test_app_soc_algo.c` | `app_soc.c` | SOC 核心算法(AEKF 相关)数值验证 |

注意: `TEST_CELL_CNT` 取 `BMS_CELL_SERIES_NUM`(运行时默认串数),
改串数配置后测试会自动跟随, 无需手改。

## 如何运行(待补, 建议方案)

方案 A — 独立 CMake(host):
1. `git clone https://github.com/ThrowTheSwitch/Unity test/Unity`
2. 在本目录加 `CMakeLists.txt`, 编译 `../..` 被测源文件 + 本目录测试 + Unity,
   需 stub: `driver/i2c.h`、`esp_log.h`、`freertos/task.h` 等(仅编译期替身, 函数体空)
3. `cmake -B build && cmake --build build && ./build/bms_app_tests`

方案 B — ESP-IDF host 工具链:
1. `idf.py set-target linux` 后 `idf.py build` 跑不了(S3 目标不支持),
   需单独建 `test_host/` 工程, 用 `component_add_dependency("unity")` 引入官方 Unity 组件
   (ESP-IDF 自带, 无需联网)

## 维护约定

- 改 `app_protection.c` / `app_soc.c` 的保护/算法逻辑后, 同步更新对应测试用例
- 测试断言阈值必须与 `bms_config.h` 宏一致(用宏引用, 不写死数值)
