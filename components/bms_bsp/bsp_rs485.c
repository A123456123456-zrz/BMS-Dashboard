/**
 * @file    bsp_rs485.c
 * @brief   RS485 半双工通信驱动实现(MAX3485 + UART1)
 * @author  BMS Team
 * @date    2026-08
 * @note    行业标准要点:
 *          - Modbus RTU 物理层: EIA-485 半双工, 总线空闲 = 接收态(DE 低)
 *          - 发送完必须等 UART 移位寄存器排空再释放总线, 防止最后一字节截断
 *          - 波特率可运行时修改(Modbus 保持寄存器 40002 支持)
 */
#include "bsp_rs485.h"
#include "bms_config.h"
#include "bms_pinmap.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"                              /* esp_rom_delay_us */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "BSP_RS485";

#define RS485_UART_NUM      UART_NUM_1          /* UART0 归日志 console, UART1 空闲可用 */
#define RS485_UART_TX        PIN_RS485_TX       /* GPIO14 (UART1, GPIO matrix) */
#define RS485_UART_RX        PIN_RS485_RX       /* GPIO21 (UART1, GPIO matrix) */
#define RS485_DE_GPIO        PIN_RS485_DE       /* GPIO47: 高=发送 (DE/RE 短接) */
#define RS485_RE_GPIO        PIN_RS485_RE       /* GPIO47: 低=接收使能 (与 DE 同脚短接) */

#define RS485_BUF_SIZE       256                /* 收发缓冲 */
#define RS485_EVENT_QUEUE     16                 /* 事件队列(预留) */

static uint32_t s_baud = BMS_RS485_BAUD_DEFAULT;

/* ================================================================ */
bms_err_t bsp_rs485_init(void)
{
    /* 硬件屏蔽: 缺 RS485 时直接返回, 不阻塞系统 */
    if (!HW_ENABLE_RS485) {
        ESP_LOGW(TAG, "RS485 屏蔽 (HW_ENABLE_RS485=0)");
        return BMS_ERR_NOT_INIT;
    }

    /* 1. 方向控制引脚: DE/RE 短接共用 GPIO47, 初始为接收态(低电平) */
    gpio_config_t de_cfg = {
        .pin_bit_mask = (1ULL << RS485_DE_GPIO) | (1ULL << RS485_RE_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,    /* 上电默认接收态, 防误发送 */
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&de_cfg));
    gpio_set_level(RS485_DE_GPIO, 0);            /* 接收态 */
    gpio_set_level(RS485_RE_GPIO, 0);            /* 接收使能 */

    /* 2. UART1 配置: 9600-8-N-1 (可宏调) */
    uart_config_t uart_cfg = {
        .baud_rate           = BMS_RS485_BAUD_DEFAULT,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .source_clk          = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(RS485_UART_NUM, RS485_BUF_SIZE,
                                        RS485_BUF_SIZE, RS485_EVENT_QUEUE,
                                        NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install fail: %s", esp_err_to_name(err));
        return BMS_ERR_FAIL;
    }
    err = uart_param_config(RS485_UART_NUM, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config fail: %s", esp_err_to_name(err));
        return BMS_ERR_FAIL;
    }
    err = uart_set_pin(RS485_UART_NUM, RS485_UART_TX, RS485_UART_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin fail: %s", esp_err_to_name(err));
        return BMS_ERR_FAIL;
    }

    s_baud = BMS_RS485_BAUD_DEFAULT;
    ESP_LOGI(TAG, "RS485 初始化完成: UART1 %lu-8N1, TX=GPIO%d RX=GPIO%d DE=GPIO%d ~RE=GPIO%d",
             (unsigned long)s_baud, RS485_UART_TX, RS485_UART_RX, RS485_DE_GPIO, RS485_RE_GPIO);
    return BMS_OK;
}

/* ================================================================ */
bms_err_t bsp_rs485_set_baud(uint32_t baud)
{
    if (!HW_ENABLE_RS485) {
        return BMS_ERR_NOT_INIT;
    }
    static const uint32_t valid[] = {1200, 2400, 4800, 9600, 19200,
                                     38400, 57600, 115200};
    bool ok = false;
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (valid[i] == baud) { ok = true; break; }
    }
    if (!ok) {
        ESP_LOGW(TAG, "非法波特率 %lu, 忽略", (unsigned long)baud);
        return BMS_ERR_PARAM_INVALID;
    }
    esp_err_t err = uart_set_baudrate(RS485_UART_NUM, baud);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_baudrate fail: %s", esp_err_to_name(err));
        return BMS_ERR_FAIL;
    }
    s_baud = baud;
    ESP_LOGI(TAG, "RS485 波特率改为 %lu", (unsigned long)baud);
    return BMS_OK;
}

/* ================================================================ */
uint32_t bsp_rs485_get_baud(void)
{
    return s_baud;
}

/* ================================================================ */
bms_err_t bsp_rs485_send(const uint8_t *data, size_t len)
{
    if (!HW_ENABLE_RS485) {
        return BMS_ERR_NOT_INIT;
    }
    if (data == NULL || len == 0 || len > RS485_BUF_SIZE) {
        return BMS_ERR_PARAM_INVALID;
    }

    /* 1. 拉高 DE/RE(短接) → 发送态: DE=1 使能发送, ~RE=1 禁用接收 */
    gpio_set_level(RS485_DE_GPIO, 1);
    gpio_set_level(RS485_RE_GPIO, 1);
    /* 等待总线从接收切到发送的电气稳定时间(半字节周期即可, 保守 100us) */
    esp_rom_delay_us(100);

    /* 2. 写入 UART */
    int written = uart_write_bytes(RS485_UART_NUM, data, len);
    if (written < 0 || (size_t)written != len) {
        ESP_LOGE(TAG, "uart_write_bytes fail: %d/%u", written, (unsigned)len);
        gpio_set_level(RS485_DE_GPIO, 0);
        gpio_set_level(RS485_RE_GPIO, 0);
        return BMS_ERR_FAIL;
    }

    /* 3. 等待移位寄存器排空(行业标准: 否则最后一字节被 DE 拉低截断,
     *    从站收到 CRC 错误帧) */
    esp_err_t err = uart_wait_tx_done(RS485_UART_NUM, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_wait_tx_done timeout");
    }

    /* 4. 拉低 DE/RE(短接) → 回到接收态: DE=0 禁用发送, ~RE=0 使能接收 */
    gpio_set_level(RS485_DE_GPIO, 0);
    gpio_set_level(RS485_RE_GPIO, 0);
    return BMS_OK;
}

/* ================================================================ */
bms_err_t bsp_rs485_recv(uint8_t *buf, size_t buf_len, size_t *out_len,
                         uint32_t timeout_ms)
{
    if (!HW_ENABLE_RS485) {
        return BMS_ERR_NOT_INIT;
    }
    if (buf == NULL || buf_len == 0 || out_len == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    *out_len = 0;

    int n = uart_read_bytes(RS485_UART_NUM, buf, buf_len,
                            timeout_ms ? pdMS_TO_TICKS(timeout_ms) : 0);
    if (n <= 0) {
        return BMS_ERR_TIMEOUT;
    }
    *out_len = (size_t)n;
    return BMS_OK;
}

/* ================================================================ */
size_t bsp_rs485_available(void)
{
    if (!HW_ENABLE_RS485) {
        return 0;
    }
    size_t size = 0;
    uart_get_buffered_data_len(RS485_UART_NUM, &size);
    return size;
}
