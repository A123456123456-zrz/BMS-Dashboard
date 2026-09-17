/**
 * @file    bsp_led.c
 * @brief   LED 三灯状态指示驱动实现
 *          GPIO1=红, GPIO2=绿, GPIO38=板载白色RGB(共阳, 低=亮高=灭)
 * @note    状态编码:
 *            关机      → 全灭
 *            正常      → 绿灯亮
 *            预警      → 白灯慢闪(1Hz)
 *            保护      → 红灯快闪(5Hz) + 白灯快闪
 *            严重      → 红灯常亮 + 白灯常亮
 *            充电中    → 白灯恒亮
 *            WiFi连接中 → 白灯快闪(3Hz)
 */
#include "bsp_led.h"
#include "bms_pinmap.h"
#include "bms_config.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "BSP_LED";

/* 白灯是共阳接法: GPIO38=低电平点亮, GPIO38=高电平熄灭 */
static inline void white_on(void)  { gpio_set_level(PIN_LED_WHITE, 0); }
static inline void white_off(void) { gpio_set_level(PIN_LED_WHITE, 1); }

bms_err_t bsp_led_init(void)
{
    if (!HW_ENABLE_LED) {
        ESP_LOGW(TAG, "LED 屏蔽");
        return BMS_ERR_NOT_INIT;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LED_RED) | (1ULL << PIN_LED_GREEN) | (1ULL << PIN_LED_WHITE),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config fail: %s", esp_err_to_name(ret));
        return BMS_ERR_FAIL;
    }

    gpio_set_level(PIN_LED_RED, 0);
    gpio_set_level(PIN_LED_GREEN, 0);
    white_off();
    ESP_LOGI(TAG, "LED init ok (R=%d G=%d W=%d)", PIN_LED_RED, PIN_LED_GREEN, PIN_LED_WHITE);
    return BMS_OK;
}

void bsp_led_set(bool on)
{
    if (!HW_ENABLE_LED) return;
    gpio_set_level(PIN_LED_GREEN, on ? 1 : 0);
}

void bsp_led_set_color(bsp_led_color_e color)
{
    if (!HW_ENABLE_LED) return;
    gpio_set_level(PIN_LED_RED,   0);
    gpio_set_level(PIN_LED_GREEN, 0);
    white_off();
    switch (color) {
    case BSP_LED_GREEN:    gpio_set_level(PIN_LED_GREEN, 1); break;
    case BSP_LED_RED:      gpio_set_level(PIN_LED_RED, 1);   break;
    case BSP_LED_YELLOW:   gpio_set_level(PIN_LED_RED, 1); gpio_set_level(PIN_LED_GREEN, 1); break;
    default: break;
    }
}

void bsp_led_white(bool on)
{
    if (!HW_ENABLE_LED) return;
    on ? white_on() : white_off();
}

void bsp_led_toggle(void)
{
    if (!HW_ENABLE_LED) return;
    gpio_set_level(PIN_LED_GREEN, gpio_get_level(PIN_LED_GREEN) ? 0 : 1);
}

void bsp_led_white_toggle(void)
{
    if (!HW_ENABLE_LED) return;
    gpio_set_level(PIN_LED_WHITE, gpio_get_level(PIN_LED_WHITE) ? 0 : 1);
}
