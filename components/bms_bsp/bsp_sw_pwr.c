/**
 * @file    bsp_sw_pwr.c
 * @brief   电源键驱动实现(GPIO13, 高有效, 仿 BMS-C1 SW_PWR)
 * @author  BMS Team
 * @date    2026-08
 * @note    实现机制:
 *          1. GPIO 输入+下拉, 高电平=按下(按键一端 GPIO13 一端 3.3V)
 *          2. bsp_sw_pwr_process() 在任务中周期(100ms)调用, 累计高电平时长
 *          3. 高电平持续 >=3s → 触发一次关功率(bsp_bq76952_set_fet(false,false)),
 *             同时置标志; 释放(低电平)后计时复位, 可再次长按触发
 *          4. 长按 3s 语义对齐 BMS-C1 的 SW_PWR(官方固件 button_pressed_for_3s → shutdown)
 *          5. PIN_SW_PWR<0 时初始化直接返回, process 永远返回 false
 */
#include "bsp_sw_pwr.h"
#include "bsp_bq76952.h"
#include "bms_pinmap.h"
#include "bms_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP_SW_PWR";

#define SW_PWR_LONG_PRESS_MS      3000        // 长按阈值: 3s(与 C1 button_pressed_for_3s 一致)
#define SW_PWR_CHECK_PERIOD_MS    100         // 调用方周期(用于超时累计, 与任务周期匹配)

static volatile bool s_inited = false;
static uint32_t      s_press_ms = 0;          // 当前按住累计时长(0 = 未按住)
static bool          s_fired = false;         // 本次按住是否已触发关机(防重复触发)
static volatile bool s_short_pending = false; // 短按事件(<3s 释放, 由 app 层取走)

/* ================================================================ */
bms_err_t bsp_sw_pwr_init(void)
{
    if (PIN_SW_PWR < 0) {
        ESP_LOGW(TAG, "SW_PWR 未定义引脚 (PIN_SW_PWR<0), 屏蔽");
        return BMS_ERR_NOT_INIT;
    }

    gpio_config_t cfg = {
        .pin_bit_mask  = (1ULL << PIN_SW_PWR),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_ENABLE,   /* 默认低电平=松开; 按下(接3.3V)=高 */
        .intr_type     = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config fail: %s", esp_err_to_name(ret));
        return BMS_ERR_FAIL;
    }

    s_inited = true;
    s_press_ms = 0;
    s_fired = false;
    ESP_LOGI(TAG, "SW_PWR init ok (GPIO%d, 高有效, 长按3s关功率)", PIN_SW_PWR);
    return BMS_OK;
}

/* ================================================================ */
bool bsp_sw_pwr_is_pressed(void)
{
    if (!s_inited) {
        return false;
    }
    return gpio_get_level(PIN_SW_PWR) == 1;
}

/* ================================================================ */
bool bsp_sw_pwr_process(void)
{
    if (!s_inited) {
        return false;
    }

    if (gpio_get_level(PIN_SW_PWR) == 1) {
        /* 按住: 累计时长 */
        s_press_ms += SW_PWR_CHECK_PERIOD_MS;
        if (s_press_ms >= SW_PWR_LONG_PRESS_MS && !s_fired) {
            s_fired = true;
            ESP_LOGW(TAG, "SW_PWR 长按 %u ms 触发: 关闭功率输出 (CHG/DSG FET OFF)",
                     (unsigned)s_press_ms);
            bsp_bq76952_set_fet(false, false);   /* 关闭充电+放电 FET */
            return true;
        }
    } else {
        /* 松开: 复位计时, 允许下次长按再次触发 */
        if (s_press_ms >= 200 && s_press_ms < SW_PWR_LONG_PRESS_MS) {
            s_short_pending = true;              /* 有效短按(200ms~3s, 消抖) */
        }
        s_press_ms = 0;
        s_fired = false;
    }
    return false;
}

/* ================================================================ */
bool bsp_sw_pwr_take_short_press(void)
{
    if (s_short_pending) {
        s_short_pending = false;
        return true;
    }
    return false;
}
