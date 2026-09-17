/**
 * @file    bsp_can.c
 * @brief   TWAI CAN 通信驱动实现(ESP32-S3 内置 TWAI 控制器 + SN65HVD230 收发器)
 * @author  BMS Team
 * @date    2026-08
 * @note    2026-08-25: 由 MCP2515(SPI) 方案改为 ESP32-S3 内置 TWAI 控制器,
 *          外接 SN65HVD230 收发器(STB=GPIO48, 低=正常). 标准帧 11-bit ID, 500kbps.
 *          接口(bsp_can.h)保持不变, 上层 sys_can.c 无需改动.
 */
#include "bsp_can.h"
#include "bms_pinmap.h"
#include "bms_config.h"
#include "driver/twai.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "BSP_CAN";

static bool s_inited = false;
static twai_message_t s_pending;               /* has_msg/recv 之间的暂存报文 */

/* ====== 内部函数声明 ====== */
static void can_stb_init(void);

/* ====== 总线离线自愈: Bus-Off→recovery, STOPPED→start; 1s 限频防高频重入 ====== */
static void can_try_recover(void)
{
    static uint32_t last_recover_ms = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now - last_recover_ms < 1000) {
        return;                                   /* 1s 内只尝试一次 */
    }
    last_recover_ms = now;

    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) {
        return;
    }
    if (st.state == TWAI_STATE_BUS_OFF) {
        ESP_LOGW(TAG, "CAN Bus-Off, 发起总线恢复");
        twai_initiate_recovery();
    } else if (st.state == TWAI_STATE_STOPPED) {
        /* RECOVERING 状态由恢复流程自动转 RUNNING, 无需干预 */
        ESP_LOGW(TAG, "CAN 已停止(%d), 重新 start", (int)st.state);
        twai_start();
    }
}

bms_err_t bsp_can_init(void)
{
    /* 硬件屏蔽: 缺 TWAI 时直接返回, 不阻塞系统 */
    if (!HW_ENABLE_TWAI) {
        ESP_LOGW(TAG, "TWAI 屏蔽 (HW_ENABLE_TWAI=0)");
        return BMS_ERR_NOT_INIT;
    }

    /* 收发器待机引脚: 拉低进入正常模式 */
    can_stb_init();

    /* TWAI 控制器配置 */
    twai_general_config_t gcfg = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)PIN_CAN_TX, (gpio_num_t)PIN_CAN_RX, TWAI_MODE_NORMAL);
    gcfg.clkout_io = TWAI_IO_UNUSED;
    gcfg.bus_off_io = TWAI_IO_UNUSED;

    twai_timing_config_t tcfg = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t fcfg = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t ret = twai_driver_install(&gcfg, &tcfg, &fcfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "twai install fail: %s", esp_err_to_name(ret));
        return BMS_ERR_FAIL;
    }
    ret = twai_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "twai start fail: %s", esp_err_to_name(ret));
        return BMS_ERR_FAIL;
    }

    s_inited = true;
    ESP_LOGI(TAG, "twai can init ok (%ld bps, tx=GPIO%d rx=GPIO%d stb=GPIO%d)",
             (long)CAN_BAUDRATE, PIN_CAN_TX, PIN_CAN_RX, PIN_CAN_STB);
    return BMS_OK;
}

static void can_stb_init(void)
{
    gpio_config_t stb = {
        .pin_bit_mask = (1ULL << PIN_CAN_STB),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&stb);
    gpio_set_level(PIN_CAN_STB, 0);              /* 低=正常模式(SN65HVD230) */
}

bms_err_t bsp_can_send(const bms_can_msg_t *msg)
{
    if (!s_inited || msg == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }

    twai_message_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.identifier        = msg->id & 0x7FF;   /* 标准 11-bit ID */
    frame.data_length_code  = msg->dlc & 0x0F;
    if (frame.data_length_code > 8) {
        frame.data_length_code = 8;
    }
    memcpy(frame.data, msg->data, frame.data_length_code);

    esp_err_t ret = twai_transmit(&frame, pdMS_TO_TICKS(20));
    if (ret != ESP_OK) {
        static uint16_t fail_cnt = 0;            /* 限频: 总线离线时 30 次打 1 次 */
        if ((++fail_cnt % 30) == 1) {
            ESP_LOGE(TAG, "twai transmit fail: %s (累计 %u 次)",
                     esp_err_to_name(ret), fail_cnt);
        }
        if (ret == ESP_ERR_INVALID_STATE) {
            can_try_recover();                   /* 控制器停止/Bus-Off, 自愈 */
        }
        return BMS_ERR_TIMEOUT;
    }
    return BMS_OK;
}

bool bsp_can_has_msg(void)
{
    if (!s_inited) {
        return false;
    }
    /* 非阻塞尝试接收一帧, 成功后暂存, 供 bsp_can_recv 取出 */
    twai_message_t msg;
    if (twai_receive(&msg, 0) == ESP_OK) {
        s_pending = msg;
        return true;
    }
    return false;
}

bms_err_t bsp_can_recv(bms_can_msg_t *msg)
{
    if (!s_inited || msg == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }

    twai_message_t frame;
    /* 优先取 has_msg 暂存的帧 */
    if (s_pending.data_length_code != 0 || s_pending.identifier != 0) {
        frame = s_pending;
        memset(&s_pending, 0, sizeof(s_pending));
    } else {
        if (twai_receive(&frame, 0) != ESP_OK) {
            return BMS_ERR_FAIL;                 /* 无报文 */
        }
    }

    msg->id  = frame.identifier;
    msg->dlc = frame.data_length_code;
    if (msg->dlc > 8) {
        msg->dlc = 8;
    }
    memcpy(msg->data, frame.data, msg->dlc);
    return BMS_OK;
}
