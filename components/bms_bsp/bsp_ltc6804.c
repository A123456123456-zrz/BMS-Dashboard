/**
 * @file    bsp_ltc6804.c
 * @brief   LTC6804-2 AFE 驱动实现(12串电压采集 + 被动均衡 + GPIO温度)
 * @author  BMS Team
 * @date    2026-08
 * @note    SPI Mode3(CPOL=1,CPHA=1) 1MHz, PEC15 校验
 *          采集流程: STCVAD -> 等12ms -> RDCVA/B/C/D -> 解析12位电压
 *          均衡: WRCFG 写配置寄存器 DCC 位
 * 2026-08-09: 升级为多芯片驱动(方案A: 共享 SPI2 + 独立片选, 支持 1~32S)
 *             LTC6804_CHIP_NUM 控制芯片数: 16S=2 / 24S=2 / 32S=3
 *             多芯片 ADC 转换并发启动, 总耗时仍 ~17ms, 不影响 100ms 采集周期
 */
#include "bsp_ltc6804.h"
#include "bms_pinmap.h"
#include "bms_config.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 2026-08-25: LTC6804 方案弃用(AFE=BQ76952), 且 bms_pinmap.h 已移除 SPI2 引脚宏;
 * 整个实现按 HW_ENABLE_LTC6804 编译, 关闭时本文件为空翻译单元, 保证可编译 */
#if HW_ENABLE_LTC6804

static const char *TAG = "BSP_LTC6804";

/* ====== LTC6804 命令字 ====== */
#define LTC_CMD_WRCFG       0x0001      // 写配置寄存器
#define LTC_CMD_RDCFG       0x0002      // 读配置寄存器
#define LTC_CMD_RDCVA       0x0004      // 读电压组 A (C1~C3)
#define LTC_CMD_RDCVB       0x0006      // 读电压组 B (C4~C6)
#define LTC_CMD_RDCVC       0x0008      // 读电压组 C (C7~C9)
#define LTC_CMD_RDCVD       0x000A      // 读电压组 D (C10~C12)
#define LTC_CMD_RDAUXA      0x000C      // 读辅助组 A (GPIO1~GPIO3)
#define LTC_CMD_RDAUXB      0x000E      // 读辅助组 B (GPIO4~GPIO5)
#define LTC_CMD_STCVAD      0x0010      // 启动 ADC 转换(标准)
#define LTC_CMD_STCVDC      0x0011      // 启动 ADC 转换(允许放电)

/* ====== PEC15 查表 ====== */
static const uint16_t s_pec15_table[256] = {
    0x0000,0x4599,0x4eab,0x0b32,0x58cf,0x1d56,0x1664,0x53fd,0x7407,0x319e,0x3aac,0x7f35,0x2cc8,0x6951,0x6263,0x27fa,
    0x2d97,0x680e,0x633c,0x26a5,0x7558,0x30c1,0x3bf3,0x7e6a,0x5990,0x1c09,0x173b,0x52a2,0x015f,0x44c6,0x4ff4,0x0a6d,
    0x5b2e,0x1eb7,0x1585,0x501c,0x03e1,0x4678,0x4d4a,0x08d3,0x2f29,0x6ab0,0x6182,0x241b,0x77e6,0x327f,0x394d,0x7cd4,
    0x76b9,0x3320,0x3812,0x7d8b,0x2e76,0x6bef,0x60dd,0x2544,0x02be,0x4727,0x4c15,0x098c,0x5a71,0x1fe8,0x14da,0x5143,
    0x73c5,0x365c,0x3d6e,0x78f7,0x2b0a,0x6e93,0x65a1,0x2038,0x07c2,0x425b,0x4969,0x0cf0,0x5f0d,0x1a94,0x11a6,0x543f,
    0x5e52,0x1bcb,0x10f9,0x5560,0x069d,0x4304,0x4836,0x0daf,0x2a55,0x6fcc,0x64fe,0x2167,0x729a,0x3703,0x3c31,0x79a8,
    0x28eb,0x6d72,0x6640,0x23d9,0x7024,0x35bd,0x3e8f,0x7b16,0x5cec,0x1975,0x1247,0x57de,0x0423,0x41ba,0x4a88,0x0f11,
    0x057c,0x40e5,0x4bd7,0x0e4e,0x5db3,0x182a,0x1318,0x5681,0x717b,0x34e2,0x3fd0,0x7a49,0x29b4,0x6c2d,0x671f,0x2286,
    0x2213,0x678a,0x6cb8,0x2921,0x7adc,0x3f45,0x3477,0x71ee,0x5614,0x138d,0x18bf,0x5d26,0x0edb,0x4b42,0x4070,0x05e9,
    0x0f84,0x4a1d,0x412f,0x04b6,0x574b,0x12d2,0x19e0,0x5c79,0x7b83,0x3e1a,0x3528,0x70b1,0x234c,0x66d5,0x6de7,0x287e,
    0x793d,0x3ca4,0x3796,0x720f,0x21f2,0x646b,0x6f59,0x2ac0,0x0d3a,0x48a3,0x4391,0x0608,0x55f5,0x106c,0x1b5e,0x5ec7,
    0x54aa,0x1133,0x1a01,0x5f98,0x0c65,0x49fc,0x42ce,0x0757,0x20ad,0x6534,0x6e06,0x2b9f,0x7862,0x3dfb,0x36c9,0x7350,
    0x51d6,0x144f,0x1f7d,0x5ae4,0x0919,0x4c80,0x47b2,0x022b,0x25d1,0x6048,0x6b7a,0x2ee3,0x7d1e,0x3887,0x33b5,0x762c,
    0x7c41,0x39d8,0x32ea,0x7773,0x248e,0x6117,0x6a25,0x2fbc,0x0846,0x4ddf,0x46ed,0x0374,0x5089,0x1510,0x1e22,0x5bbb,
    0x0af8,0x4f61,0x4453,0x01ca,0x5237,0x17ae,0x1c9c,0x5905,0x7eff,0x3b66,0x3054,0x75cd,0x2630,0x63a9,0x689b,0x2d02,
    0x276f,0x62f6,0x69c4,0x2c5d,0x7fa0,0x3a39,0x310b,0x7492,0x5368,0x16f1,0x1dc3,0x585a,0x0ba7,0x4e3e,0x450c,0x0095,
};

/* ====== 多芯片支持(方案A: 共享 SPI2 总线 + 每芯片独立片选) ====== */
static spi_device_handle_t s_spi[LTC6804_CHIP_NUM];     /* 每芯片一个设备句柄 */
static bool               s_inited[LTC6804_CHIP_NUM];   /* 每芯片初始化状态 */

/* 片选引脚表(与芯片号对应, 定义于 bms_pinmap.h)
 * 2026-08-22: 原 PIN_LTC6804_CS=GPIO27 不在排针上已移除, 改用排针内 PIN_LTC6804_CS1(GPIO2);
 *   LTC6804 方案已弃用(AFE=BQ76952, HW_ENABLE_LTC6804=0), 此处仅保证驱动可编译. */
static const int s_cs_pin[LTC6804_CHIP_NUM] = {
    PIN_LTC6804_CS1,
#if LTC6804_CHIP_NUM >= 2
    PIN_LTC6804_CS1,
#endif
#if LTC6804_CHIP_NUM >= 3
    PIN_LTC6804_CS2,
#endif
};

/* ====== 内部函数声明 ====== */
static uint16_t pec15_calc(const uint8_t *data, uint8_t len);
static bms_err_t spi_xfer_chip(uint8_t chip, const uint8_t *tx, uint8_t *rx, uint16_t len);
static bms_err_t ltc_write_cmd_chip(uint8_t chip, uint16_t cmd);
static bms_err_t ltc_read_cmd_chip(uint8_t chip, uint16_t cmd, uint8_t *buf, uint8_t len);

/**
 * @brief   计算 PEC15 校验值
 * @param   data  数据缓冲区
 * @param   len    数据长度
 * @return  16 位 PEC15 值
 */
static uint16_t pec15_calc(const uint8_t *data, uint8_t len)
{
    uint16_t pec = 0x0010;                          // 初始值
    for (uint8_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)(pec >> 7) ^ data[i];
        pec = (uint16_t)((pec << 8) ^ s_pec15_table[idx]);
    }
    /* LTC6804 datasheet: PEC 为 15 位, 发送时左对齐进 16 位字(末位补 0),
     * 即 PEC16 = PEC15 << 1. 芯片按此校验命令/数据, 缺此左移会导致命令被拒.
     * 已用权威值校验: WRCFG(0x0001)=0x3D6E, RDCFG(0x0002)=0x2B0A. */
    return (uint16_t)(pec << 1);
}

/**
 * @brief   SPI 读写交换(带芯片号片选)
 * @param   chip  芯片号(0 ~ LTC6804_CHIP_NUM-1), 越界/未初始化返回参数错误
 */
static bms_err_t spi_xfer_chip(uint8_t chip, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    if (chip >= LTC6804_CHIP_NUM || s_spi[chip] == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    spi_transaction_t t = {0};
    t.length    = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    t.flags     = 0;

    esp_err_t ret = spi_device_polling_transmit(s_spi[chip], &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi xfer fail (chip=%u): %s", chip, esp_err_to_name(ret));
        return BMS_ERR_SPI;
    }
    return BMS_OK;
}

/**
 * @brief   发送命令字(带 PEC)
 * @param   chip  目标芯片号
 */
static bms_err_t ltc_write_cmd_chip(uint8_t chip, uint16_t cmd)
{
    uint8_t tx[4];
    tx[0] = (uint8_t)(cmd >> 8);
    tx[1] = (uint8_t)(cmd & 0xFF);
    uint16_t pec = pec15_calc(tx, 2);
    tx[2] = (uint8_t)(pec >> 8);
    tx[3] = (uint8_t)(pec & 0xFF);
    return spi_xfer_chip(chip, tx, NULL, 4);
}

/**
 * @brief   发送命令并读取数据(命令 + PEC + 数据 + PEC)
 * @param   chip  目标芯片号
 * @param   buf  输出缓冲区, 长度 = len(不含命令和PEC)
 */
static bms_err_t ltc_read_cmd_chip(uint8_t chip, uint16_t cmd, uint8_t *buf, uint8_t len)
{
    /* 2026-08-10 安全加固: len 上限校验(tx/rx 固定 4+32),
     * 防止调用方传入超长 len 导致栈越界读写 */
    if (len > 30) {
        return BMS_ERR_PARAM_INVALID;
    }
    uint8_t tx[4 + 32] = {0};
    uint8_t rx[4 + 32] = {0};

    /* 组装命令 */
    tx[0] = (uint8_t)(cmd >> 8);
    tx[1] = (uint8_t)(cmd & 0xFF);
    uint16_t cmd_pec = pec15_calc(tx, 2);
    tx[2] = (uint8_t)(cmd_pec >> 8);
    tx[3] = (uint8_t)(cmd_pec & 0xFF);

    bms_err_t err = spi_xfer_chip(chip, tx, rx, 4 + len + 2);
    if (err != BMS_OK) {
        return err;
    }

    /* 校验数据段 PEC(每芯片独立 PEC, 直接复用) */
    uint8_t  *data   = &rx[4];
    uint16_t  recv_pec = ((uint16_t)data[len] << 8) | data[len + 1];
    uint16_t  calc_pec = pec15_calc(data, len);
    if (recv_pec != calc_pec) {
        ESP_LOGW(TAG, "pec mismatch (chip=%u): recv=0x%04X calc=0x%04X", chip, recv_pec, calc_pec);
        return BMS_ERR_CRC;
    }

    for (uint8_t i = 0; i < len; i++) {
        buf[i] = data[i];
    }
    return BMS_OK;
}

bms_err_t bsp_ltc6804_init(void)
{
    /* 硬件屏蔽: 缺 LTC6804 时直接返回, 不阻塞系统 */
    if (!HW_ENABLE_LTC6804) {
        ESP_LOGW(TAG, "LTC6804 屏蔽 (HW_ENABLE_LTC6804=0)");
        return BMS_ERR_NOT_INIT;
    }

    /* 配置 SPI 总线(SPI2, Mode3) —— 只初始化一次, 多芯片共享 */
    spi_bus_config_t buscfg = {
        .mosi_io_num     = PIN_SPI2_MOSI,
        .miso_io_num     = PIN_SPI2_MISO,
        .sclk_io_num     = PIN_SPI2_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi bus init fail: %s", esp_err_to_name(ret));
        return BMS_ERR_SPI;
    }

    /* 逐芯片添加设备(每芯片独立片选)并唤醒 */
    for (uint8_t chip = 0; chip < LTC6804_CHIP_NUM; chip++) {
        spi_device_interface_config_t devcfg = {
            .clock_speed_hz = LTC6804_SPI_HZ,
            .mode           = 3,
            .spics_io_num   = s_cs_pin[chip],
            .queue_size     = 4,
            /* H6 修复: 原 SPI_DEVICE_HALFDUPLEX 会把事务拆成"先发后收"两阶段,
             * 而 LTC6804 的响应是在主机继续发送 dummy 的同时从 MISO 移出,
             * 半双工 RX 阶段读到的已是响应之后的无效数据, 导致读电压必失败.
             * 移除该标志使用全双工(边发边收), 与 ltc_read_cmd_chip 的
             * 单事务 4+len+2 字节收发设计匹配. */
            .flags          = 0,
        };
        ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi[chip]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi add device fail (chip=%u, cs=%d): %s",
                     chip, s_cs_pin[chip], esp_err_to_name(ret));
            s_spi[chip] = NULL;
            s_inited[chip] = false;
            continue;                      /* 该芯片失败不影响其他芯片 */
        }

        /* 唤醒该芯片(发送 dummy 字节) */
        uint8_t dummy = 0xFF;
        spi_xfer_chip(chip, &dummy, NULL, 1);
        vTaskDelay(pdMS_TO_TICKS(10));

        s_inited[chip] = true;
        ESP_LOGI(TAG, "ltc6804 chip %u init ok (cs=%d, spi=%dHz mode3)",
                 chip, s_cs_pin[chip], LTC6804_SPI_HZ);
    }

    /* 2026-08-10 修复: 统计成功片数, 全部失败时返回错误(原恒返回 BMS_OK,
     * 上层误判初始化成功, 后续读取才发现 BMS_ERR_NOT_INIT, 排查困难) */
    uint8_t ok_cnt = 0;
    for (uint8_t ch = 0; ch < LTC6804_CHIP_NUM; ch++) {
        if (s_inited[ch]) ok_cnt++;
    }
    if (ok_cnt == 0) {
        ESP_LOGE(TAG, "LTC6804 全部 %u 片初始化失败", (unsigned)LTC6804_CHIP_NUM);
        return BMS_ERR_SPI;
    }
    if (ok_cnt < LTC6804_CHIP_NUM) {
        ESP_LOGW(TAG, "LTC6804 仅 %u/%u 片初始化成功(降级运行)", ok_cnt, (unsigned)LTC6804_CHIP_NUM);
    }

    return BMS_OK;
}

void bsp_ltc6804_wakeup(void)
{
    /* 唤醒所有已初始化的芯片 */
    uint8_t dummy = 0xFF;
    for (uint8_t chip = 0; chip < LTC6804_CHIP_NUM; chip++) {
        if (s_inited[chip]) {
            spi_xfer_chip(chip, &dummy, NULL, 1);
        }
    }
}

bms_err_t bsp_ltc6804_read_voltages(uint16_t *cell_mv)
{
    if (cell_mv == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }

    /* 至少一片芯片就绪才可读取 */
    bool any_ready = false;
    for (uint8_t ch = 0; ch < LTC6804_CHIP_NUM; ch++) {
        if (s_inited[ch]) { any_ready = true; break; }
    }
    if (!any_ready) {
        return BMS_ERR_NOT_INIT;
    }

    /* 并发启动所有芯片 ADC 转换(多芯片并行, 总耗时仍 ~17ms) */
    for (uint8_t ch = 0; ch < LTC6804_CHIP_NUM; ch++) {
        if (!s_inited[ch]) continue;
        ltc_write_cmd_chip(ch, LTC_CMD_STCVAD);
    }

    /* 等待转换完成(标准模式 12ms, 多留余量) */
    vTaskDelay(pdMS_TO_TICKS(LTC6804_ADC_CONV_MS + 5));

    /* 逐芯片读取 4 组电压寄存器, 每组 6 字节(C1~C3 各2字节)
     * 全局串号 = chip * 12 + 组内序号, 由上层 BMS_CELL_SERIES_NUM 截断 */
    uint8_t  grp[6];
    uint16_t cmd_list[4] = {LTC_CMD_RDCVA, LTC_CMD_RDCVB, LTC_CMD_RDCVC, LTC_CMD_RDCVD};

    for (uint8_t ch = 0; ch < LTC6804_CHIP_NUM; ch++) {
        if (!s_inited[ch]) continue;
        for (uint8_t g = 0; g < 4; g++) {
            bms_err_t err = ltc_read_cmd_chip(ch, cmd_list[g], grp, 6);
            if (err != BMS_OK) {
                return err;
            }
            /* 每组 3 个串电压, 12 位有效 */
            for (uint8_t c = 0; c < 3; c++) {
                uint8_t idx = g * 3 + c;
                if (idx >= LTC6804_CELL_NUM) {
                    break;
                }
                uint16_t raw = (uint16_t)grp[c * 2] | ((uint16_t)grp[c * 2 + 1] << 8);
                raw &= 0x0FFF;                          // 低 12 位有效
                uint8_t gidx = (uint8_t)(ch * LTC6804_CELL_NUM + idx);   /* 全局串号 */
                if (gidx < BMS_CELL_SERIES_NUM) {
                    /* raw * 100μV / 1000 = raw / 10, 单位 mV */
                    cell_mv[gidx] = (uint16_t)((uint32_t)raw * LTC6804_VLSB_UV / 1000);
                }
            }
        }
    }
    return BMS_OK;
}

bms_err_t bsp_ltc6804_read_gpio(uint16_t *gpio_mv)
{
    if (gpio_mv == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    /* B4 修复: 遍历所有已初始化芯片, 每片 GPIO1~GPIO5 写入 gpio_mv[ch*5 + i],
     * 支持多芯片级联(每片独立 NTC 测温). 单芯片(LTC6804_CHIP_NUM=1)时行为
     * 与原版完全一致(仅 chip0). 调用方需提供 >= LTC6804_CHIP_NUM*5 的缓冲区. */
    bool any = false;
    for (uint8_t ch = 0; ch < LTC6804_CHIP_NUM; ch++) {
        if (!s_inited[ch]) {
            continue;
        }
        any = true;
        uint8_t grp[6];
        /* 辅助组 A: GPIO1~GPIO3 */
        bms_err_t err = ltc_read_cmd_chip(ch, LTC_CMD_RDAUXA, grp, 6);
        if (err != BMS_OK) {
            return err;
        }
        for (uint8_t i = 0; i < 3; i++) {
            uint16_t raw = (uint16_t)grp[i * 2] | ((uint16_t)grp[i * 2 + 1] << 8);
            raw &= 0x0FFF;
            gpio_mv[ch * 5 + i] = (uint16_t)((uint32_t)raw * LTC6804_VLSB_UV / 1000); // 单位 mV
        }

        /* 辅助组 B: GPIO4~GPIO5 */
        err = ltc_read_cmd_chip(ch, LTC_CMD_RDAUXB, grp, 6);
        if (err != BMS_OK) {
            return err;
        }
        for (uint8_t i = 0; i < 2; i++) {
            uint16_t raw = (uint16_t)grp[i * 2] | ((uint16_t)grp[i * 2 + 1] << 8);
            raw &= 0x0FFF;
            gpio_mv[ch * 5 + 3 + i] = (uint16_t)((uint32_t)raw * LTC6804_VLSB_UV / 1000);
        }
    }
    if (!any) {
        return BMS_ERR_FAIL;   // 无已初始化芯片, 无法测温
    }
    return BMS_OK;
}

bms_err_t bsp_ltc6804_set_balance(uint32_t mask)
{
    /* 写配置寄存器: 6 字节 CFGR0~CFGR5
     * CFGR0: GPIO/REF/DTEN 位
     * CFGR1~CFGR2: UV/OV 比较阈值(此处保持默认)
     * CFGR3: DCC1~DCC8
     * CFGR4: DCC9~DCC12 + DCTO
     * CFGR5: DCTO(放电超时)
     * 2026-08-09: 多芯片时按每芯片 12 位拆分掩码, 逐芯片写入 */
    bool any_ready = false;
    for (uint8_t ch = 0; ch < LTC6804_CHIP_NUM; ch++) {
        if (s_inited[ch]) { any_ready = true; break; }
    }
    if (!any_ready) {
        return BMS_ERR_NOT_INIT;
    }

    for (uint8_t chip = 0; chip < LTC6804_CHIP_NUM; chip++) {
        if (!s_inited[chip]) continue;

        uint32_t chip_mask = (mask >> (chip * LTC6804_CELL_NUM)) & 0x0FFF;  /* 每芯片 12 位 */

        uint8_t cfg[6] = {0};
        cfg[0] = 0xF8;                                   // GPIO5~GPIO1 输入, 关闭 REF/DTEN
        cfg[3] = (uint8_t)(chip_mask & 0x00FF);          // DCC1~DCC8
        cfg[4] = (uint8_t)((chip_mask >> 8) & 0x000F);   // DCC9~DCC12, DCTO=0
        cfg[5] = 0x00;

        /* 组装写命令: cmd(2) + cmd_pec(2) + data(6) + data_pec(2) */
        uint8_t tx[12];
        tx[0] = (uint8_t)(LTC_CMD_WRCFG >> 8);
        tx[1] = (uint8_t)(LTC_CMD_WRCFG & 0xFF);
        uint16_t cmd_pec = pec15_calc(tx, 2);
        tx[2] = (uint8_t)(cmd_pec >> 8);
        tx[3] = (uint8_t)(cmd_pec & 0xFF);
        for (uint8_t i = 0; i < 6; i++) {
            tx[4 + i] = cfg[i];
        }
        uint16_t data_pec = pec15_calc(cfg, 6);
        tx[10] = (uint8_t)(data_pec >> 8);
        tx[11] = (uint8_t)(data_pec & 0xFF);

        bms_err_t err = spi_xfer_chip(chip, tx, NULL, 12);
        if (err != BMS_OK) {
            ESP_LOGE(TAG, "set balance fail (chip=%u)", chip);
            return err;
        }
    }
    return BMS_OK;
}

#endif // HW_ENABLE_LTC6804
