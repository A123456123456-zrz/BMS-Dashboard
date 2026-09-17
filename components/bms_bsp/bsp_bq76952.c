/**
 * @file    bsp_bq76952.c
 * @brief   BQ76952 AFE 驱动实现(3~16串电压采集 + 内置被动均衡 + TS温度 + 库仑计电流 + FET控制)
 * @author  BMS Team
 * @date    2026-08
 * @note    I2C 总线(I2C_NUM_0, 2026-08-25 起总线初始化迁至此驱动), 地址 0x08
 *          电压/温度寄存器单位 = 1 mV(TI 数据手册 Table 2: Cell Voltage Unit=mV;
 *            TS 引脚须配置为 ADCIN 模式, 0x70/0x72/0x74 才返回 mV, 否则返回 0.1K 温度)
 *          均衡: 写子命令 CB_ACTIVE_CELLS(0x0083) 控制内置均衡 MOS
 *          电流: 库仑计 CC2(0x3A, 16-bit signed, 单位 µA), 替代 INA181 外置 ADC
 *          FET : 子命令 FET_CONTROL(0x0097), 替代继电器 GPIO 控制功率开关
 *          参考: TI BQ76952 TRM (SLUUBY2B)
 *          代码风格与 bsp_ltc6804.c / bsp_bq34z100.c 对齐(Doxygen + bms_err_t + 硬件屏蔽)
 */
#include "bsp_bq76952.h"
#include "bms_pinmap.h"
#include "bms_config.h"
#include "driver/i2c.h"                  // i2c_master_write_to_device / i2c_master_write_read_device 便捷封装声明(与 bsp_bq34z100.c 对齐)
#include "driver/i2c_master.h"          // 新版 I2C 主机 API(总线初始化用)
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP_BQ76952";

/* ====== BQ76952 寄存器/子命令(参考 TI SLUUBY2B) ====== */
#define BQ76952_ADDR            0x08            // 7-bit I2C 地址(BMS-C1 仿制板 ADDR 接法)
#define BQ76952_I2C_PORT        I2C_NUM_0       // 独立使用(原 OLED/BQ34Z100 已屏蔽)

#define BQ_I2C_FREQ_HZ          400000          // 总线速率(与 bsp_oled 原配置一致)

#define BQ_REG_VC1_LO           0x14            // Cell1 Voltage 低字节(每 cell 2 字节, VC1..VC16 = 0x14~0x33)
                                                // TI 数据手册 Table 2: Cell Voltage 单位 = mV (1 LSB = 1 mV)
#define BQ_REG_TS1_CMD          0x70            // TS1 Temperature 命令(TS1~TS3 = 0x70/0x72/0x74, TRM Table 4-8)
                                                // 注意: TS 引脚须配置为 ADCIN 模式, 此命令才返回 mV;
                                                //       默认 thermistor 模式返回 0.1K 温度, 不能直接当电压用
#define BQ_REG_CC2_CURRENT      0x3A            // CC2 库仑计电流(16-bit signed, 单位 µA; 正=放电 负=充电)
#define BQ_SUB_CMD_CB_ACTIVE    0x0083          // 主机控制均衡子命令: 写位掩码启动均衡, 写 0x0000 关闭
#define BQ_SUB_CMD_FET_CTRL     0x0097          // FET_CONTROL 子命令: 1字节阻塞掩码(bit0=DSG, bit3=CHG, 置1=阻塞关断)

/* ====== CFGUPDATE 模式(data memory 写回前必须进入) ======
 * 参考 Zephyr bq769x2 驱动: SET_CFGUPDATE(0x0090) → 写 data memory → EXIT_CFGUPDATE(0x0092) */
#define BQ_SUB_CMD_SET_CFGUPDATE    0x0090
#define BQ_SUB_CMD_EXIT_CFGUPDATE   0x0092
#define BQ_CMD_BATTERY_STATUS       0x12        // BATTERY_STATUS 寄存器(bit3=CFGUPDATE, 参考 TRM Table 9-2)

/* ====== data memory 寄存器(写回 C1 仿制板硬件配置, 地址参考 bq769x2_registers.h) ====== */
#define BQ_DM_CFETOFF_CONFIG    0x92FA          // CFETOFF 引脚配置(thermistor 18k=0x07)
#define BQ_DM_DFETOFF_CONFIG    0x92FB          // DFETOFF 引脚配置(thermistor 18k=0x07, v0.4 分流 NTC)
#define BQ_DM_TS1_CONFIG        0x92FD          // TS1 引脚配置(thermistor 18k=0x07)
#define BQ_DM_TS2_CONFIG        0x92FE          // TS2 引脚配置
#define BQ_DM_TS3_CONFIG        0x92FF          // TS3 引脚配置(thermistor 18k=0x07)
#define BQ_DM_DCHG_CONFIG       0x9301          // DCHG 引脚配置(FET 温度 ADC=0x0F)
#define BQ_DM_DDSG_CONFIG       0x9302          // DDSG 引脚配置
#define BQ_DM_VCELL_MODE        0x9304          // 电压采集模式(16S 差分=0x0040)

/* BMS-C1 仿制板引脚功能配置值(与 bms_c1.dts / bms_c1_0_4_0.overlay 一致) */
#define BQ_CFG_TS_NTC_18K       0x07            // TS 引脚 = NTC thermistor 模式(内部 18k 上拉)
#define BQ_CFG_DCHG_FET_TEMP    0x0F            // DCHG 引脚 = FET 温度 ADC 输入
#define BQ_CFG_DFETOFF_SHUNT    0x07            // DFETOFF 引脚 = 分流温度 NTC(v0.4)

/* 电压/温度寄存器单位 = 1 mV, raw 值即 mV(TI 数据手册 Table 2) */
#define BQ_VREG_SUB_CMD_LO      0x3E            // 子命令寄存器低字节(写子命令地址)
#define BQ_VREG_SUB_CMD_DATA    0x40            // 子命令数据寄存器(写数据)
#define BQ_VREG_CHECKSUM        0x60            // 校验和/长度寄存器(写 [checksum, length])

/* 转换用: 1 LSB = 1 mV, 直接取整(定点 ×10000 仅用于与项目其它驱动风格统一) */
#define BQ_VCELL_LSB_MV         10000           // = 1.0 mV × 10000
#define BQ_VCELL_LSB_DIV        10000

#define BQ_I2C_TIMEOUT          pdMS_TO_TICKS(100)

static bool s_inited = false;

/* BQ76952 ALERT 事件标志(2026-08-25 新增)
 * ALERT 引脚(开漏, 上拉 10k): 高电平=有保护/事件(与 C1 dts GPIO_ACTIVE_HIGH 一致)
 * 中断置位, 应用层可轮询查询(采集任务已含轮询, 中断为双保险) */
static volatile bool s_alert_flag = false;

/* ====== 内部函数声明 ====== */
static bms_err_t bq_read(uint8_t reg, uint8_t *data, uint8_t len);
static bms_err_t bq_read_u16(uint8_t reg, uint16_t *out);
static bms_err_t bq_write_subcmd(uint16_t cmd, const uint8_t *data, uint8_t len);

/* ====== ALERT 引脚中断回调(高电平=有事件, 仅置标志) ====== */
static void IRAM_ATTR bq_alert_isr(void *arg)
{
    (void)arg;
    s_alert_flag = true;
}

static bms_err_t bq_read(uint8_t reg, uint8_t *data, uint8_t len)
{
    esp_err_t ret = i2c_master_write_read_device(BQ76952_I2C_PORT, BQ76952_ADDR,
                                                 &reg, 1, data, len, BQ_I2C_TIMEOUT);
    return (ret == ESP_OK) ? BMS_OK : BMS_ERR_I2C;
}

/* 读 16-bit 寄存器(低字节在前, ESP32 小端直接映射) */
static bms_err_t bq_read_u16(uint8_t reg, uint16_t *out)
{
    uint8_t buf[2] = {0};
    bms_err_t err = bq_read(reg, buf, 2);
    if (err != BMS_OK) {
        return err;
    }
    *out = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return BMS_OK;
}

/* 写子命令(参考 TI bq769x2 协议: 0x3E←子命令地址, 0x40←数据, 0x60←[校验和,长度]) */
static bms_err_t bq_write_subcmd(uint16_t cmd, const uint8_t *data, uint8_t len)
{
    /* I2C 写缓冲首字节为寄存器地址; 子命令地址须写入寄存器 0x3E
     * 缓冲: [0x3E][子命令低][子命令高][数据...] → 0x3E/0x3F 写子命令, 0x40/0x41 写数据 */
    uint8_t buf[10];
    uint8_t total = (uint8_t)(1 + 2 + len);
    if (total > sizeof(buf)) {
        return BMS_ERR_PARAM_INVALID;
    }
    buf[0] = (uint8_t)BQ_VREG_SUB_CMD_LO;            // 0x3E 子命令寄存器
    buf[1] = (uint8_t)(cmd & 0xFF);
    buf[2] = (uint8_t)((cmd >> 8) & 0xFF);
    for (uint8_t i = 0; i < len; i++) {
        buf[3 + i] = data[i];
    }
    esp_err_t ret = i2c_master_write_to_device(BQ76952_I2C_PORT, BQ76952_ADDR,
                                               buf, total, BQ_I2C_TIMEOUT);
    if (ret != ESP_OK) {
        return BMS_ERR_I2C;
    }
    /* 校验和 = ~(子命令地址 + 数据) 按字节求和(不含 0x3E 寄存器字节); 长度 = (2 + len) + 2 */
    uint8_t sum = 0;
    for (uint8_t i = 0; i < len + 2; i++) {
        sum += buf[1 + i];
    }
    uint8_t cl[3] = { (uint8_t)BQ_VREG_CHECKSUM, (uint8_t)(~sum), (uint8_t)(len + 4) };
    ret = i2c_master_write_to_device(BQ76952_I2C_PORT, BQ76952_ADDR,
                                     cl, 3, BQ_I2C_TIMEOUT);
    if (ret != ESP_OK) {
        return BMS_ERR_I2C;
    }
    return BMS_OK;
}

bms_err_t bsp_bq76952_init(void)
{
    /* 硬件屏蔽: 缺 BQ76952 时直接返回, 不阻塞系统 */
    if (!HW_ENABLE_BQ76952) {
        ESP_LOGW(TAG, "BQ76952 屏蔽 (HW_ENABLE_BQ76952=0)");
        return BMS_ERR_NOT_INIT;
    }

    /* 2026-08-25: I2C 总线初始化迁至此(原 bsp_oled_init 负责, OLED 已屏蔽)
     * 幂等: 若已被其他模块初始化则容忍 ESP_ERR_INVALID_STATE */
    i2c_config_t i2c_cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_I2C_SDA,
        .scl_io_num       = PIN_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BQ_I2C_FREQ_HZ,
    };
    esp_err_t iret = i2c_param_config(BQ76952_I2C_PORT, &i2c_cfg);
    if (iret != ESP_OK) {
        ESP_LOGE(TAG, "i2c config fail: %s", esp_err_to_name(iret));
        return BMS_ERR_I2C;
    }
    iret = i2c_driver_install(BQ76952_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (iret != ESP_OK && iret != ESP_ERR_INVALID_STATE) {
        /* 2026-09-09: AFE 读取失败重初始化路径——旧驱动仍占用端口时
         * install 返回 ESP_FAIL(日志实测), 先删旧驱动再重装一次 */
        ESP_LOGW(TAG, "i2c install fail(%s), 尝试删旧驱动后重装", esp_err_to_name(iret));
        i2c_driver_delete(BQ76952_I2C_PORT);
        iret = i2c_param_config(BQ76952_I2C_PORT, &i2c_cfg);
        if (iret == ESP_OK) {
            iret = i2c_driver_install(BQ76952_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
        }
        if (iret != ESP_OK && iret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "i2c reinstall fail: %s", esp_err_to_name(iret));
            return BMS_ERR_I2C;
        }
    }

    /* WAKE(TS2 唤醒) 引脚配置(若已定义): 2026-09-15 飞线后启用
     * 开漏输出(OD): 只能对地导通/悬空 — 拉低 TS2 唤醒, 悬空时完全不影响按键;
     * 严禁推挽: 推挽输出高会把 TS2 钳在 3.3V, 干扰按键检测与 AFE 采样 */
#if PIN_BQ76952_WAKE >= 0
    gpio_reset_pin(PIN_BQ76952_WAKE);
    gpio_set_direction(PIN_BQ76952_WAKE, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(PIN_BQ76952_WAKE, 1);                 /* 释放态=悬空 */
#endif

    /* ALERT 引脚(2026-08-25 新增, 仿 C1: 开漏输出+外部10k上拉, 高电平=有事件)
     * 输入+上拉+上升沿中断置标志; 采集任务轮询为双保险 */
#if PIN_BQ76952_ALERT >= 0
    {
        gpio_config_t alert_cfg = {
            .pin_bit_mask  = (1ULL << PIN_BQ76952_ALERT),
            .mode          = GPIO_MODE_INPUT,
            .pull_up_en    = GPIO_PULLUP_ENABLE,
            .pull_down_en  = GPIO_PULLDOWN_DISABLE,
            .intr_type     = GPIO_INTR_POSEDGE,
        };
        gpio_config(&alert_cfg);
        /* ISR 服务可能已由 bsp_button 安装, 幂等 */
        esp_err_t isr_ret = gpio_install_isr_service(0);
        if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "gpio_install_isr_service fail: %s", esp_err_to_name(isr_ret));
        }
        esp_err_t h_ret = gpio_isr_handler_add(PIN_BQ76952_ALERT, bq_alert_isr, NULL);
        if (h_ret != ESP_OK) {
            ESP_LOGW(TAG, "ALERT isr add fail: %s", esp_err_to_name(h_ret));
        }
        ESP_LOGI(TAG, "ALERT 引脚已配置 (GPIO%d, 高有效, 中断置标志)", PIN_BQ76952_ALERT);
    }
#endif

    /* 尝试唤醒(若处于 SHUTDOWN, 首次 I2C 可能无响应) */
    bsp_bq76952_wakeup();

    /* 探测: 读取 VC1 寄存器, 验证设备在线 */
    uint16_t vc = 0;
    esp_err_t ret = i2c_master_write_read_device(BQ76952_I2C_PORT, BQ76952_ADDR,
                                                 (uint8_t[]){(uint8_t)BQ_REG_VC1_LO}, 1,
                                                 (uint8_t *)&vc, 2, BQ_I2C_TIMEOUT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "bq76952 not found (addr=0x%02X): %s", BQ76952_ADDR, esp_err_to_name(ret));
        return BMS_ERR_I2C;
    }

    s_inited = true;
    ESP_LOGI(TAG, "bq76952 init ok (addr=0x%02X, 支持 3~16S)", BQ76952_ADDR);
    return BMS_OK;
}

void bsp_bq76952_wakeup(void)
{
#if PIN_BQ76952_WAKE >= 0
    /* 2026-09-15 飞线唤醒: GPIO15 → TS2 焊盘(R25 上端), 开漏拉低 50ms
     * 复刻 SW1 按键下降沿(芯片内部 4.6MΩ 弱上拉 TS2 至 ~5V, 拉到 <0.7V 触发唤醒).
     * 开漏必须: 只往地拉, 绝不推高 — 推 3.3V 上去会顶住 TS2 干扰按键检测与 AFE. */
    gpio_set_level(PIN_BQ76952_WAKE, 0);                 /* 输出低 = 对地导通, 拉低 TS2 */
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_BQ76952_WAKE, 1);                 /* 释放 = 开漏悬空, 不再影响 TS2 */
#else
    /* 引脚未接: I2C 访问本身可触发自唤醒(部分固件配置下有效) */
    ESP_LOGD(TAG, "WAKE 引脚未定义, 依赖 I2C 自唤醒");
#endif
}

bms_err_t bsp_bq76952_read_voltages(uint16_t *cell_mv)
{
    if (cell_mv == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    if (!s_inited) {
        return BMS_ERR_NOT_INIT;
    }

    uint8_t series = (uint8_t)BMS_CELL_SERIES_NUM;
    if (series < 1) series = 1;
    if (series > 16) series = 16;             // BQ76952 硬件最多 16 串, 钳制防读越界

    for (uint8_t i = 0; i < series; i++) {
        uint8_t reg = (uint8_t)(BQ_REG_VC1_LO + i * 2);   // VC(i+1) 低字节
        uint16_t raw = 0;
        bms_err_t err = bq_read_u16(reg, &raw);
        if (err != BMS_OK) {
            return err;
        }
        /* 16-bit 电压寄存器, 单位 = 1 mV(TI 数据手册 Table 2), raw 即 mV */
        uint32_t mv = (uint32_t)raw * BQ_VCELL_LSB_MV + (BQ_VCELL_LSB_DIV / 2);
        mv /= BQ_VCELL_LSB_DIV;
        cell_mv[i] = (uint16_t)mv;
    }
    return BMS_OK;
}

bms_err_t bsp_bq76952_read_gpio(uint16_t *gpio_mv)
{
    if (gpio_mv == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    if (!s_inited) {
        return BMS_ERR_NOT_INIT;
    }

    /* TS1~TS3 共 3 路温度通道(命令 0x70/0x72/0x74)
     * 前提: TS 引脚须配置为 ADCIN 模式(TSx Config 寄存器 = no pull-up),
     *       否则命令返回 0.1K 温度而非 mV, 此处结果将错误
     * BMS-C1 仿制板(ts1/ts3-pin-config=0x07, thermistor 模式): 请使用 bsp_bq76952_read_temp */
    for (uint8_t i = 0; i < 3; i++) {
        uint8_t reg = (uint8_t)(BQ_REG_TS1_CMD + i * 2);
        uint16_t raw = 0;
        bms_err_t err = bq_read_u16(reg, &raw);
        if (err != BMS_OK) {
            return err;
        }
        /* ADCIN 模式下, TS 命令返回引脚电压, 单位 = 1 mV; raw 即 mV */
        uint32_t mv = (uint32_t)raw * BQ_VCELL_LSB_MV + (BQ_VCELL_LSB_DIV / 2);
        mv /= BQ_VCELL_LSB_DIV;
        gpio_mv[i] = (uint16_t)mv;
    }
    return BMS_OK;
}

bms_err_t bsp_bq76952_read_temp(int16_t *temp_dc)
{
    if (temp_dc == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    if (!s_inited) {
        return BMS_ERR_NOT_INIT;
    }

    /* TS1~TS3 温度命令(0x70/0x72/0x74): thermistor 模式下返回 0.1K
     * 换算: T(℃) = raw*0.1 - 273.15 → temp_dc(0.1℃) = raw - 2731.5 ≈ raw - 2732
     * (BMS-C1 仿制板 TS1/TS3 接 18k 上拉 NTC, ts1/ts3-pin-config=0x07) */
    for (uint8_t i = 0; i < 3; i++) {
        uint8_t reg = (uint8_t)(BQ_REG_TS1_CMD + i * 2);
        uint16_t raw = 0;
        bms_err_t err = bq_read_u16(reg, &raw);
        if (err != BMS_OK) {
            return err;
        }
        int32_t k0p1 = (int32_t)(int16_t)raw;        /* 0.1K */
        int32_t c0p1 = k0p1 - 2732;                  /* 0.1℃ (273.15K ≈ 2732 个 0.1K) */
        temp_dc[i] = (int16_t)c0p1;
    }
    return BMS_OK;
}

bms_err_t bsp_bq76952_set_balance(uint32_t mask)
{
    if (!s_inited) {
        return BMS_ERR_NOT_INIT;
    }
    /* CB_ACTIVE_CELLS(0x0083) 子命令: bit0~bit15 = cell1~cell16 均衡 MOS 使能
     * 写 0x0000 关闭全部均衡(参考 TRM Table 10-1) */
    uint8_t data[2] = {
        (uint8_t)(mask & 0xFF),
        (uint8_t)((mask >> 8) & 0xFF),
    };
    return bq_write_subcmd(BQ_SUB_CMD_CB_ACTIVE, data, 2);
}

bms_err_t bsp_bq76952_read_current_ma(int16_t *ma)
{
    if (ma == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    if (!s_inited) {
        return BMS_ERR_NOT_INIT;
    }
    /* CC2 Current(0x3A): 16-bit signed, 单位 µA; 正=放电 负=充电(与 bsp_current 约定一致) */
    uint16_t raw = 0;
    bms_err_t err = bq_read_u16(BQ_REG_CC2_CURRENT, &raw);
    if (err != BMS_OK) {
        return err;
    }
    int32_t ua = (int32_t)(int16_t)raw;          /* µA */
    *ma = (int16_t)(ua / 1000);                  /* µA -> mA */
    return BMS_OK;
}

bms_err_t bsp_bq76952_set_fet(bool chg_on, bool dsg_on)
{
    if (!s_inited) {
        return BMS_ERR_NOT_INIT;
    }
    /* FET_CONTROL(0x0097) 子命令: 1 字节阻塞掩码, 置 1 = 对应 FET 禁止使能
     * bit0 = DSG(放电), bit3 = CHG(充电)(参考 TI E2E 澄清 + TRM Table 12-29) */
    uint8_t block = 0;
    if (!chg_on) block |= 0x08;
    if (!dsg_on) block |= 0x01;
    return bq_write_subcmd(BQ_SUB_CMD_FET_CTRL, &block, 1);
}

/* ====== CFGUPDATE 模式切换(写 data memory 前必须) ======
 * 参考 Zephyr bq769x2 驱动: SET_CFGUPDATE(0x0090) → 等 BATTERY_STATUS.CFGUPDATE=1 */
static bms_err_t bq_set_cfgupdate(bool enable)
{
    uint16_t cmd = enable ? BQ_SUB_CMD_SET_CFGUPDATE : BQ_SUB_CMD_EXIT_CFGUPDATE;
    /* 纯指令子命令: 只写 0x3E/0x3F, 无数据/无校验和 */
    uint8_t buf[3] = { (uint8_t)BQ_VREG_SUB_CMD_LO, (uint8_t)(cmd & 0xFF), (uint8_t)(cmd >> 8) };
    esp_err_t ret = i2c_master_write_to_device(BQ76952_I2C_PORT, BQ76952_ADDR,
                                               buf, 3, BQ_I2C_TIMEOUT);
    if (ret != ESP_OK) {
        return BMS_ERR_I2C;
    }
    vTaskDelay(pdMS_TO_TICKS(2));               /* TRM Table 9-2: 模式切换需 ~2ms */

    /* 确认 BATTERY_STATUS.CFGUPDATE 位(bit3)到位 */
    uint8_t st = 0;
    bms_err_t err = bq_read(BQ_CMD_BATTERY_STATUS, &st, 1);
    if (err != BMS_OK) {
        return err;
    }
    bool in_cfg = (st & 0x08) != 0;
    return (in_cfg == enable) ? BMS_OK : BMS_ERR_TIMEOUT;
}

/**
 * @brief   写回 BMS-C1 仿制板硬件配置(2026-08-25 新增)
 * @note    与 LibreSolar bms-c1.dts + bms_c1_0_4_0.overlay 一致:
 *          - TS1/TS3 = NTC thermistor 模式(18k 上拉, 0x07)
 *          - DCHG    = FET 温度 ADC 输入(0x0F)
 *          - DFETOFF = 分流温度 NTC(0x07, v0.4)
 *          - VCELL_MODE = 16S 差分模式(0x0040)
 *          仅当 BQ76952 已初始化成功时调用; 失败仅告警不阻塞(出厂可能已配置)
 * @retval  BMS_OK 成功
 */
bms_err_t bsp_bq76952_apply_config(void)
{
    if (!s_inited) {
        return BMS_ERR_NOT_INIT;
    }

    bms_err_t err = bq_set_cfgupdate(true);
    if (err != BMS_OK) {
        ESP_LOGW(TAG, "进入 CFGUPDATE 失败(%d), 跳过配置写回(出厂配置可能已生效)", (int)err);
        return err;
    }

    /* 逐项写回(data memory 写: 0x3E/0x3F 地址 + 0x40 数据 + 0x60 校验和,
     * 由 bq_write_subcmd 实现, 与 Zephyr datamem_write_u1 协议一致) */
    err = BMS_OK;
    uint8_t ts_cfg   = BQ_CFG_TS_NTC_18K;
    uint8_t dchg_cfg = BQ_CFG_DCHG_FET_TEMP;
    uint8_t dfet_cfg = BQ_CFG_DFETOFF_SHUNT;
    uint8_t vcell[2] = { 0x40, 0x00 };          /* 16S 差分模式 */

    err |= bq_write_subcmd(BQ_DM_TS1_CONFIG,  &ts_cfg, 1);
    err |= bq_write_subcmd(BQ_DM_TS3_CONFIG,  &ts_cfg, 1);
    err |= bq_write_subcmd(BQ_DM_DCHG_CONFIG, &dchg_cfg, 1);
    err |= bq_write_subcmd(BQ_DM_DFETOFF_CONFIG, &dfet_cfg, 1);
    err |= bq_write_subcmd(BQ_DM_VCELL_MODE,  vcell, 2);

    /* 退出 CFGUPDATE 模式 */
    bms_err_t err2 = bq_set_cfgupdate(false);
    if (err2 != BMS_OK) {
        ESP_LOGW(TAG, "退出 CFGUPDATE 失败(%d)", (int)err2);
        return err2;
    }

    if (err != BMS_OK) {
        ESP_LOGW(TAG, "部分配置写回失败(%d)", (int)err);
        return err;
    }
    ESP_LOGI(TAG, "BQ76952 C1 配置写回完成 (TS1/TS3=NTC 18k, DCHG=FET温度, DFETOFF=分流温度, 16S)");
    return BMS_OK;
}

/* ====== ALERT 事件查询(2026-08-25 新增, 仿 C1) ======
 * 返回是否有待处理的 ALERT 事件(高电平触发置位), 查询后自动清零.
 * 采集任务可周期调用(轮询)作为中断的双保险. */
bool bsp_bq76952_alert_pending(void)
{
    if (!s_inited || PIN_BQ76952_ALERT < 0) {
        return false;
    }
    if (s_alert_flag) {
        s_alert_flag = false;
        return true;
    }
    return false;
}
