/**
 * @file    bsp_bq34z100.c
 * @brief   BQ34Z100-G1 电量计驱动实现(I2C, 可选交叉验证)
 * @author  BMS Team
 * @date    2026-08
 * @note    与 OLED 共用 I2C 总线(I2C_NUM_0)
 *          设备地址 0x55, 提供 SOC/电压读取用于 AEKF 交叉验证
 */
#include "bsp_bq34z100.h"
#include "bms_pinmap.h"
#include "bms_config.h"
#include "driver/i2c.h"                  // i2c_master_write_to_device / i2c_master_write_read_device 便捷封装声明(注: 非冗余)
#include "driver/i2c_master.h"            // 新版 I2C 主机 API
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP_BQ34Z100";

#define BQ34Z100_ADDR       0x55            // 7-bit I2C 地址
#define BQ34Z100_I2C_PORT   I2C_NUM_0       // 与 OLED 共用

/* BQ34Z100 子命令(16-bit, 通过 0x00 寄存器写入后从 0x00 块读, 首字节为长度) */
#define BQ_SUBCMD_SOC       0x1C            // 相对 SOC
#define BQ_SUBCMD_VOLTAGE   0x08            // 电压, 单位 mV

static bool s_inited = false;

/* ====== 内部函数 ====== */
static bms_err_t bq_read_subcmd(uint16_t subcmd, int16_t *out);
static bms_err_t bq_write(uint8_t reg, const uint8_t *data, uint8_t len);
static bms_err_t bq_read(uint8_t reg, uint8_t *data, uint8_t len);

static bms_err_t bq_write(uint8_t reg, const uint8_t *data, uint8_t len)
{
    uint8_t buf[8];
    if (len + 1 > sizeof(buf)) {
        return BMS_ERR_PARAM_INVALID;
    }
    buf[0] = reg;
    for (uint8_t i = 0; i < len; i++) {
        buf[1 + i] = data[i];
    }
    esp_err_t ret = i2c_master_write_to_device(BQ34Z100_I2C_PORT, BQ34Z100_ADDR,
                                               buf, len + 1, pdMS_TO_TICKS(100));
    return (ret == ESP_OK) ? BMS_OK : BMS_ERR_I2C;
}

static bms_err_t bq_read(uint8_t reg, uint8_t *data, uint8_t len)
{
    esp_err_t ret = i2c_master_write_read_device(BQ34Z100_I2C_PORT, BQ34Z100_ADDR,
                                                    &reg, 1, data, len, pdMS_TO_TICKS(100));
    return (ret == ESP_OK) ? BMS_OK : BMS_ERR_I2C;
}

/**
 * @brief   通过子命令读取 2 字节数据
 * @note    流程(对照 BQ34Z100-G1 TRM MAC Data 读时序):
 *          写子命令到 0x00 -> 等 5ms -> 从 0x00 块读
 *          块读首字节为数据长度, 其后为数据(小端), 末尾为校验和
 * H7 修复: 原代码从 0x04 用普通 2 字节读, 地址与"长度字节"协议均不符,
 *          读数必然错位; 现改为从 0x00 读 1+2 字节并跳过长度字节.
 */
static bms_err_t bq_read_subcmd(uint16_t subcmd, int16_t *out)
{
    uint8_t cmd[2] = {(uint8_t)(subcmd & 0xFF), (uint8_t)(subcmd >> 8)};
    bms_err_t err = bq_write(0x00, cmd, 2);
    if (err != BMS_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(5));                    // 等待芯片处理

    uint8_t buf[4] = {0};                            /* [0]=长度, [1..2]=数据(小端), [3]=校验和 */
    err = bq_read(0x00, buf, 3);
    if (err != BMS_OK) {
        return err;
    }
    if (buf[0] < 2) {                                /* 长度字节须 >= 2 才够一个 16 位数据 */
        ESP_LOGW(TAG, "bq subcmd 0x%04X 长度异常 len=%u", (unsigned)subcmd, (unsigned)buf[0]);
        return BMS_ERR_CRC;
    }
    *out = (int16_t)((uint16_t)buf[1] | ((uint16_t)buf[2] << 8));
    return BMS_OK;
}

bms_err_t bsp_bq34z100_init(void)
{
    /* 硬件屏蔽: 缺 BQ34Z100 时直接返回, 不阻塞系统 */
    if (!HW_ENABLE_BQ34Z100) {
        ESP_LOGW(TAG, "BQ34Z100 屏蔽 (HW_ENABLE_BQ34Z100=0)");
        return BMS_ERR_NOT_INIT;
    }

    /* I2C 总线由 bsp_oled_init 初始化, 此处仅探测设备是否在线 */
    uint8_t cmd[2] = {0x00, 0x1C};                   // 读取 SOC 子命令
    esp_err_t ret = i2c_master_write_to_device(BQ34Z100_I2C_PORT, BQ34Z100_ADDR,
                                               cmd, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "bq34z100 not found (addr=0x%02X): %s", BQ34Z100_ADDR, esp_err_to_name(ret));
        return BMS_ERR_I2C;                          // 设备可选, 失败不阻塞
    }

    s_inited = true;
    ESP_LOGI(TAG, "bq34z100 init ok (addr=0x%02X)", BQ34Z100_ADDR);
    return BMS_OK;
}

int8_t bsp_bq34z100_read_soc(void)
{
    if (!s_inited) {
        return -1;
    }
    int16_t soc = 0;
    if (bq_read_subcmd(BQ_SUBCMD_SOC, &soc) != BMS_OK) {
        ESP_LOGW(TAG, "read soc fail");
        return -1;
    }
    return (int8_t)soc;
}

int16_t bsp_bq34z100_read_voltage_mv(void)
{
    if (!s_inited) {
        return -1;
    }
    int16_t volt = 0;
    if (bq_read_subcmd(BQ_SUBCMD_VOLTAGE, &volt) != BMS_OK) {
        ESP_LOGW(TAG, "read voltage fail");
        return -1;
    }
    return volt;
}
