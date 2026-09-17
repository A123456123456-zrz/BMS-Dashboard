/**
 * @file    bsp_active_balance.c
 * @brief   主动均衡驱动实现(LEDC PWM 调流 + GPIO 使能)
 * @author  BMS Team
 * @date    2026-08
 * @note    设计原则(企业级可维护性):
 *          1. 硬件抽象: 上层(app_balance)只依赖本接口, 不关心具体电路
 *          2. 未接线降级: HW_ENABLE_ACTIVE_BALANCE=0 时全部空操作, 不阻塞系统
 *          3. 接口预留: 使能引脚 + PWM 调流, 满足反激/开关电容等主流方案
 */
#include "bsp_active_balance.h"
#include "bms_pinmap.h"
#include "bms_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "BSP_ACTIVE_BAL";

/* LEDC 配置(14bit 分辨率, 25kHz, 电感/变压器电路常用频率)
 * 2026-08-08 修复: ESP32-S3 LEDC duty_resolution 仅支持 0~14 位,
 *   LEDC_TIMER_16_BIT 不存在会导致编译失败, 改为 14 位并同步 MAX_DUTY */
#define ACTIVE_BAL_LEDC_TIMER      LEDC_TIMER_0
#define ACTIVE_BAL_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define ACTIVE_BAL_LEDC_CHANNEL    LEDC_CHANNEL_0
#define ACTIVE_BAL_PWM_FREQ_HZ     25000
#define ACTIVE_BAL_PWM_RES_BITS    LEDC_TIMER_14_BIT
#define ACTIVE_BAL_MAX_DUTY        ((1 << 14) - 1)
/* 满电流 5000mA 对应 100% 占空比 */
#define ACTIVE_BAL_MAX_CURRENT_MA  5000

static bool s_inited = false;

bms_err_t bsp_active_balance_init(void)
{
    /* 硬件屏蔽: 未接线时直接返回, 不阻塞系统 */
    if (!HW_ENABLE_ACTIVE_BALANCE) {
        ESP_LOGW(TAG, "ACTIVE_BALANCE 屏蔽 (HW_ENABLE_ACTIVE_BALANCE=0), 使用被动均衡或关闭");
        return BMS_ERR_NOT_INIT;
    }

#if HW_ENABLE_ACTIVE_BALANCE
    /* 使能引脚: 输出, 默认关闭 */
    gpio_config_t en_cfg = {
        .pin_bit_mask = (1ULL << PIN_ACTIVE_BAL_EN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&en_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(EN) fail: %s", esp_err_to_name(ret));
        return BMS_ERR_FAIL;
    }
    gpio_set_level(PIN_ACTIVE_BAL_EN, 0);

    /* PWM 调流: LEDC 定时器 + 通道 */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = ACTIVE_BAL_LEDC_MODE,
        .timer_num       = ACTIVE_BAL_LEDC_TIMER,
        .duty_resolution = ACTIVE_BAL_PWM_RES_BITS,
        .freq_hz         = ACTIVE_BAL_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc timer config fail: %s", esp_err_to_name(ret));
        return BMS_ERR_FAIL;
    }

    ledc_channel_config_t chan_cfg = {
        .gpio_num   = PIN_ACTIVE_BAL_PWM,
        .speed_mode = ACTIVE_BAL_LEDC_MODE,
        .channel    = ACTIVE_BAL_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = ACTIVE_BAL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ret = ledc_channel_config(&chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc channel config fail: %s", esp_err_to_name(ret));
        return BMS_ERR_FAIL;
    }
    ledc_set_duty(ACTIVE_BAL_LEDC_MODE, ACTIVE_BAL_LEDC_CHANNEL, 0);
    ledc_update_duty(ACTIVE_BAL_LEDC_MODE, ACTIVE_BAL_LEDC_CHANNEL);

    s_inited = true;
    ESP_LOGI(TAG, "active balance init ok (en=%d pwm=%d %dHz)",
             PIN_ACTIVE_BAL_EN, PIN_ACTIVE_BAL_PWM, ACTIVE_BAL_PWM_FREQ_HZ);
#endif /* HW_ENABLE_ACTIVE_BALANCE */
    return BMS_OK;
}

bms_err_t bsp_active_balance_set(uint32_t mask, uint16_t current_ma)
{
    if (!HW_ENABLE_ACTIVE_BALANCE || !s_inited) {
        return BMS_ERR_NOT_INIT;
    }

#if HW_ENABLE_ACTIVE_BALANCE
    if (mask == 0 || current_ma == 0) {
        /* 关闭均衡: 使能低 + PWM 0 */
        gpio_set_level(PIN_ACTIVE_BAL_EN, 0);
        ledc_set_duty(ACTIVE_BAL_LEDC_MODE, ACTIVE_BAL_LEDC_CHANNEL, 0);
        ledc_update_duty(ACTIVE_BAL_LEDC_MODE, ACTIVE_BAL_LEDC_CHANNEL);
        return BMS_OK;
    }

    /* 按目标电流换算 PWM 占空比(线性映射, 钳位到最大) */
    uint32_t cur = current_ma;
    if (cur > ACTIVE_BAL_MAX_CURRENT_MA) {
        cur = ACTIVE_BAL_MAX_CURRENT_MA;
    }
    uint32_t duty = (uint32_t)((uint64_t)cur * ACTIVE_BAL_MAX_DUTY / ACTIVE_BAL_MAX_CURRENT_MA);

    gpio_set_level(PIN_ACTIVE_BAL_EN, 1);
    ledc_set_duty(ACTIVE_BAL_LEDC_MODE, ACTIVE_BAL_LEDC_CHANNEL, duty);
    ledc_update_duty(ACTIVE_BAL_LEDC_MODE, ACTIVE_BAL_LEDC_CHANNEL);
    ESP_LOGD(TAG, "balance on mask=0x%08X current=%dmA duty=%u", (unsigned)mask, (int)cur, (unsigned)duty);
#endif /* HW_ENABLE_ACTIVE_BALANCE */
    return BMS_OK;
}
