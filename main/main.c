/**
 * @file    main.c
 * @brief   BMS 系统启动入口
 * @author  BMS Team
 * @date    2026-08
 * @note    App 层入口, 仅做初始化与任务启动, 不含业务逻辑
 *          分层架构: main -> App -> Middleware -> BSP -> Driver(ESP-IDF)
 */
#include "app_tasks.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    /* 2026-08-18 产品化: 启动即打印复位原因, 远程诊断异常重启(看门狗/栈溢出/断电)
     * 与 reboot_guard 幂等防护配合: 若反复 watchdog 复位, 日志首行即暴露根因 */
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:       ESP_LOGI(TAG, "复位原因: 上电"); break;
    case ESP_RST_SW:            ESP_LOGI(TAG, "复位原因: 软件复位(命令重启)"); break;
    case ESP_RST_PANIC:         ESP_LOGE(TAG, "复位原因: 异常 PANIC(崩溃)!"); break;
    case ESP_RST_INT_WDT:       ESP_LOGE(TAG, "复位原因: 中断看门狗!"); break;
    case ESP_RST_TASK_WDT:      ESP_LOGE(TAG, "复位原因: 任务看门狗超时!"); break;
    case ESP_RST_WDT:           ESP_LOGE(TAG, "复位原因: 外部看门狗!"); break;
    case ESP_RST_DEEPSLEEP:     ESP_LOGI(TAG, "复位原因: 深度睡眠唤醒"); break;
    case ESP_RST_BROWNOUT:      ESP_LOGE(TAG, "复位原因: 掉电欠压(BROWNOUT)!"); break;
    case ESP_RST_SDIO:          ESP_LOGI(TAG, "复位原因: SDIO"); break;
    default:                    ESP_LOGI(TAG, "复位原因: 未知(%d)", (int)esp_reset_reason()); break;
    }

    ESP_LOGI(TAG, "BMS 系统启动 (ESP32-S3, 3~16S 运行时串数)");

    /* 系统初始化: BSP + Middleware + 算法 + 通信 */
    if (app_system_init() != BMS_OK) {
        ESP_LOGE(TAG, "系统初始化失败, 进入降级运行");
    }

    /* 创建并启动所有 FreeRTOS 业务任务 */
    if (app_tasks_start() != BMS_OK) {
        ESP_LOGE(TAG, "任务启动失败, 系统停止");
        return;
    }

    ESP_LOGI(TAG, "BMS 系统运行中...");

    /* 2026-08-14 质量修复 P1: 启动 10s 后打印各任务栈高水位,
     * 验证 2KB 小栈(task_bal/task_alarm)余量, 防止静默栈溢出。
     * app_main 自身即一个 FreeRTOS 任务, 延迟期间正常让出 CPU。 */
    vTaskDelay(pdMS_TO_TICKS(10000));
    app_dump_task_stack_watermarks();

    /* app_main 返回后 FreeRTOS 调度器持续运行, 无需 while 循环 */
}
