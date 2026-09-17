/**
 * @file    bsp_buzzer.c
 * @brief   有源蜂鸣器驱动实现
 * @author  BMS Team
 * @date    2026-08
 * @note    高电平有效
 */
#include "bsp_buzzer.h"
#include "bms_pinmap.h"
#include "bms_config.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "BSP_BUZZER";

bms_err_t bsp_buzzer_init(void)
{
    /* 硬件屏蔽: 缺蜂鸣器时直接返回, 不阻塞系统 */
    if (!HW_ENABLE_BUZZER) {
        ESP_LOGW(TAG, "蜂鸣器屏蔽 (HW_ENABLE_BUZZER=0)");
        return BMS_ERR_NOT_INIT;
    }

#if HW_ENABLE_BUZZER
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_BUZZER),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,      // 修正: 原为 GPIO_PULLDOWN_DISABLE (类型错位)
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config fail: %s", esp_err_to_name(ret));
        return BMS_ERR_FAIL;
    }

    gpio_set_level(PIN_BUZZER, 0);                  // 默认静音
    ESP_LOGI(TAG, "buzzer init ok (pin=%d)", PIN_BUZZER);
#endif
    return BMS_OK;
}

void bsp_buzzer_set(bool on)
{
    /* 硬件屏蔽: 缺蜂鸣器时跳过 GPIO 操作 */
    if (!HW_ENABLE_BUZZER) {
        return;
    }
#if HW_ENABLE_BUZZER
    gpio_set_level(PIN_BUZZER, on ? 1 : 0);
#else
    (void)on;
#endif
}
