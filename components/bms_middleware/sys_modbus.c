/**
 * @file    sys_modbus.c
 * @brief   Modbus RTU Slave 协议栈实现(行业标准, 符合 modbus.org V1.1b3)
 * @author  BMS Team
 * @date    2026-08
 * @note    实现要点:
 *          - CRC16-MODBUS: 多项式 0xA001, 初始 0xFFFF, 低字节在前
 *          - RTU 帧: [addr(1)][func(1)][data(N)][crc_lo(1)][crc_hi(1)]
 *          - 3.5 字符间隔判帧: 接收间隔 >3.5 字符(9600bps≈4ms) 即认为一帧结束
 *          - 功能码: 03 读保持 / 04 读输入 / 06 写单保持 / 10 写多保持
 *          - 异常码: 01 非法功能 / 02 非法地址 / 03 非法数据
 *          - 从站地址不匹配或 CRC 错 → 静默丢弃(不回复)
 *          - 广播地址 0: 执行但不回复
 */
#include "sys_modbus.h"
#include "sys_data.h"
#include "sys_params.h"
#include "bsp_rs485.h"
#include "bms_config.h"
#include "bms_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "SYS_MODBUS";

/* ====== 功能码 ====== */
#define MB_FC_READ_HOLDING     0x03   /* 读保持寄存器 */
#define MB_FC_READ_INPUT       0x04   /* 读输入寄存器 */
#define MB_FC_WRITE_SINGLE     0x06   /* 写单保持寄存器 */
#define MB_FC_WRITE_MULTI      0x10   /* 写多保持寄存器 */

/* ====== 异常码 ====== */
#define MB_EX_ILLEGAL_FUNC     0x01
#define MB_EX_ILLEGAL_ADDR     0x02
#define MB_EX_ILLEGAL_DATA     0x03

/* ====== 寄存器区(Modbus 1 基址) ====== */
#define MB_HOLD_START          40001  /* 保持寄存器起始(1 基址) */
#define MB_HOLD_COUNT          50     /* 保持寄存器数量 */
#define MB_INPUT_START         30001  /* 输入寄存器起始(1 基址) */
#define MB_INPUT_COUNT         60     /* 输入寄存器数量 */

/* 帧缓冲 */
#define MB_RX_BUF               256
#define MB_TX_BUF               256

static uint16_t s_slave_addr = BMS_MODBUS_SLAVE_ADDR;   /* 运行期从站地址(可经 40001 修改) */
static uint32_t s_baud       = BMS_RS485_BAUD_DEFAULT;

/* 通信统计(供 sys_modbus_get_report → 属性上报/状态卡片) */
static uint32_t s_last_poll_ms = 0;   /* 最近一次收到 CRC 正确请求的时间(ms, esp_timer) */
static uint32_t s_err_total    = 0;   /* CRC/帧错误累计 */
static uint32_t s_rx_total     = 0;   /* 有效帧累计 */

/* 保持寄存器 RAM 缓存(写入后同步 NVS; 复位后从 NVS 重新加载) */
static uint16_t s_hold[MB_HOLD_COUNT];

/* ================================================================
 *                        CRC16 (MODBUS 多项式 0xA001)
 * ================================================================ */
static uint16_t mb_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;   /* 发送时低字节在前 */
}

/* ================================================================
 *                      寄存器读写(1 基址 → 索引)
 * ================================================================ */
/* 保持寄存器 40001~40050: 站地址/波特率/保护阈值(可读写, 写后落盘 NVS) */
static bool hold_read(uint16_t addr, uint16_t *val)
{
    uint16_t idx = addr - MB_HOLD_START;
    if (addr < MB_HOLD_START || addr >= MB_HOLD_START + MB_HOLD_COUNT) {
        return false;
    }
    *val = s_hold[idx];
    return true;
}

static bool hold_write(uint16_t addr, uint16_t val)
{
    uint16_t idx = addr - MB_HOLD_START;
    if (addr < MB_HOLD_START || addr >= MB_HOLD_START + MB_HOLD_COUNT) {
        return false;
    }

    /* 2026-08-19 修复(F10): 保护阈值/电池规格寄存器写前范围校验(与云端 set_param F3、
     * 启动 POST 自检一致)。原实现除 40001/40002 外无任何校验, 主站一条越界写
     * (如欠压保护写 0 / 过压写 99999)即可让电池保护全部失效。非法值拒绝写入。 */
    switch (addr) {
    case 40003: /* 过压保护 mV */
        if (val < 3000 || val > 5000) return false;
        break;
    case 40004: /* 欠压保护 mV */
        if (val < 2000 || val > 3500) return false;
        break;
    case 40005: /* 过温保护 0.1℃ (int16 解释) */
        if ((int16_t)val < -50 || (int16_t)val > 150) return false;
        break;
    case 40006: /* 低温保护 0.1℃ (int16 解释) */
        if ((int16_t)val < -50 || (int16_t)val > 100) return false;
        break;
    case 40007: /* 充电过流 mA */
        if (val > 50000) return false;
        break;
    case 40008: /* 放电过流 mA */
        if (val > 50000) return false;
        break;
    case 40009: /* 过压预警 mV */
        if (val < 3000 || val > 5000) return false;
        break;
    case 40010: /* 欠压预警 mV */
        if (val < 2000 || val > 3500) return false;
        break;
    case 40011: /* 压差预警 mV */
        if (val > 2000) return false;
        break;
    case 40012: /* 低SOC预警 % */
        if (val > 100) return false;
        break;
    case 40013: /* 均衡触发阈值 mV */
        if (val > 500) return false;
        break;
    case 40014: /* 均衡停止阈值 mV */
        if (val > 500) return false;
        break;
    case 40015: /* 串联数 (F6: 上限=硬件能力) */
        if (val < 1 || val > BMS_HW_MAX_SERIES_NUM) return false;
        break;
    case 40016: /* 容量 mAh */
        if (val < 500 || val > 60000) return false;
        break;
    default:
        break;
    }

    s_hold[idx] = val;

    /* 特殊寄存器即时生效 */
    switch (addr) {
    case 40001:  /* 从站地址 1~247 */
        if (val >= 1 && val <= 247) {
            s_slave_addr = val;
        }
        break;
    case 40002:  /* 波特率 */
        if (bsp_rs485_set_baud(val) == BMS_OK) {
            s_baud = val;
        }
        break;
    default:
        break;
    }
    return true;
}

/* 输入寄存器 30001~30060: BMS 实时数据(只读, 每次动态读取) */
static bool input_read(uint16_t addr, uint16_t *val)
{
    if (addr < MB_INPUT_START || addr >= MB_INPUT_START + MB_INPUT_COUNT) {
        return false;
    }
    uint16_t idx = addr - MB_INPUT_START;

    bms_pack_data_t pack;
    sys_data_get_pack(&pack);
    bms_soc_data_t soc;
    sys_data_get_soc(&soc);
    bms_fault_mask_t fault = sys_data_get_fault();
    bool chg_on = false, dsg_on = false;
    sys_data_get_relay(&chg_on, &dsg_on);

    switch (idx) {
    case 0:  *val = (uint16_t)(pack.pack_mv & 0xFFFF); return true;  /* 总压低16位 mV */
    case 1:  *val = (uint16_t)(pack.pack_mv >> 16);    return true;  /* 总压高16位(32S 需) */
    case 2:  *val = (uint16_t)pack.current_ma;         return true;  /* 总电流 mA(有符号) */
    case 3:  *val = (uint16_t)(soc.soc * 10000.0f);    return true;  /* SOC ×10000 */
    case 4:  *val = (uint16_t)(soc.soh * 10000.0f);    return true;  /* SOH ×10000 */
    case 5:  *val = pack.cell_mv_max;                  return true;  /* 最高单体 mV */
    case 6:  *val = pack.cell_mv_min;                  return true;  /* 最低单体 mV */
    case 7:  *val = (uint16_t)(soc.cycle_count & 0xFFFF); return true; /* 循环次数低16位 */
    case 8:  *val = (uint16_t)(soc.cycle_count >> 16); return true;  /* 循环次数高16位 */
    case 9:  *val = (uint16_t)fault;                   return true;  /* 故障掩码 */
    case 10: *val = chg_on ? 1 : 0;                    return true;  /* 充电继电器 */
    case 11: *val = dsg_on ? 1 : 0;                    return true;  /* 放电继电器 */
    case 12: *val = (uint16_t)sys_data_get_charge_mode(); return true; /* 充电模式 */
    default:
        if (idx >= 13 && idx < 13 + BMS_MAX_CELL_SERIES_NUM) {
            *val = pack.cell_mv[idx - 13];             return true;  /* 单体电压 */
        }
        /* 温度(若通道数允许) */
        if (idx >= 13 + BMS_MAX_CELL_SERIES_NUM) {
            uint16_t t = idx - 13 - BMS_MAX_CELL_SERIES_NUM;
            if (t < BMS_MAX_CELL_SERIES_NUM) {
                *val = (uint16_t)pack.temp_dc[t];      return true;  /* 温度 0.1℃ */
            }
        }
        return false;
    }
}

/* ================================================================
 *                    保持寄存器缓存初始化(从 NVS 加载)
 * ================================================================ */
static void hold_cache_load(void)
{
    const bms_params_t *p = sys_params_get();
    memset(s_hold, 0, sizeof(s_hold));

    s_hold[0]  = BMS_MODBUS_SLAVE_ADDR;                       /* 40001 从站地址 */
    s_hold[1]  = (uint16_t)BMS_RS485_BAUD_DEFAULT;            /* 40002 波特率 */
    s_hold[2]  = p->cell_ov_prot_mv;                          /* 40003 过压保护 mV */
    s_hold[3]  = p->cell_uv_prot_mv;                          /* 40004 欠压保护 mV */
    s_hold[4]  = (uint16_t)p->temp_ot_prot_dc;                /* 40005 过温保护 0.1℃ */
    s_hold[5]  = (uint16_t)p->temp_ut_prot_dc;                /* 40006 低温保护 0.1℃ */
    s_hold[6]  = (uint16_t)p->chg_oc_prot_ma;                 /* 40007 充电过流 mA */
    s_hold[7]  = (uint16_t)p->dsg_oc_prot_ma;                 /* 40008 放电过流 mA */
    s_hold[8]  = p->cell_ov_warn_mv;                          /* 40009 过压预警 mV */
    s_hold[9]  = p->cell_uv_warn_mv;                          /* 40010 欠压预警 mV */
    s_hold[10] = p->cell_dv_warn_mv;                          /* 40011 压差预警 mV */
    s_hold[11] = p->soc_low_warn_pct;                         /* 40012 低SOC预警 % */
    s_hold[12] = p->balance_threshold_mv;                     /* 40013 均衡触发阈值 mV */
    s_hold[13] = p->balance_stop_mv;                          /* 40014 均衡停止阈值 mV */
    s_hold[14] = p->cell_series_num;                          /* 40015 串联数 */
    s_hold[15] = p->cell_capacity_mah;                        /* 40016 容量 mAh */
    /* 其余保留 0, 供扩展 */
}

/* ================================================================
 *                       响应发送(自动拼 CRC)
 * ================================================================ */
static void mb_send_response(const uint8_t *data, size_t len)
{
    uint8_t tx[MB_TX_BUF];
    if (len + 2 > sizeof(tx)) return;
    memcpy(tx, data, len);
    uint16_t crc = mb_crc16(tx, len);
    tx[len]     = (uint8_t)(crc & 0xFF);       /* 低字节在前 */
    tx[len + 1] = (uint8_t)(crc >> 8);
    bsp_rs485_send(tx, len + 2);
}

/* 异常响应: [addr][func|0x80][ex_code] */
static void mb_send_exception(uint8_t addr, uint8_t func, uint8_t ex)
{
    uint8_t rsp[3] = { addr, (uint8_t)(func | 0x80), ex };
    mb_send_response(rsp, sizeof(rsp));
}

/* ================================================================
 *                    功能码处理(数据部分)
 * ================================================================ */
/* 03/04 读寄存器: [addr][func][start_hi][start_lo][cnt_hi][cnt_lo] */
static void mb_handle_read(uint8_t addr, uint8_t func,
                           const uint8_t *req, size_t req_len)
{
    if (req_len < 4) {
        mb_send_exception(addr, func, MB_EX_ILLEGAL_DATA);
        return;
    }
    uint16_t start = (uint16_t)((req[0] << 8) | req[1]);
    uint16_t count = (uint16_t)((req[2] << 8) | req[3]);
    if (count == 0 || count > 125) {                 /* 协议限 125 寄存器/次 */
        mb_send_exception(addr, func, MB_EX_ILLEGAL_DATA);
        return;
    }

    uint8_t rsp[256];
    rsp[0] = addr;
    rsp[1] = func;
    rsp[2] = (uint8_t)(count * 2);                   /* 字节数 */
    for (uint16_t i = 0; i < count; i++) {
        uint16_t reg_addr = start + i;
        uint16_t v = 0;
        bool ok = (func == MB_FC_READ_HOLDING)
                      ? hold_read(reg_addr, &v)
                      : input_read(reg_addr, &v);
        if (!ok) {
            mb_send_exception(addr, func, MB_EX_ILLEGAL_ADDR);
            return;
        }
        rsp[3 + i * 2]     = (uint8_t)(v >> 8);
        rsp[3 + i * 2 + 1] = (uint8_t)(v & 0xFF);
    }
    mb_send_response(rsp, 3 + count * 2);
}

/* 06 写单保持寄存器: [addr][func][reg_hi][reg_lo][val_hi][val_lo] */
static void mb_handle_write_single(uint8_t addr, const uint8_t *req, size_t req_len)
{
    if (req_len < 4) {
        mb_send_exception(addr, MB_FC_WRITE_SINGLE, MB_EX_ILLEGAL_DATA);
        return;
    }
    uint16_t reg = (uint16_t)((req[0] << 8) | req[1]);
    uint16_t val = (uint16_t)((req[2] << 8) | req[3]);
    if (!hold_write(reg, val)) {
        mb_send_exception(addr, MB_FC_WRITE_SINGLE, MB_EX_ILLEGAL_ADDR);
        return;
    }
    /* 写单寄存器回显: [addr][06][reg_hi][reg_lo][val_hi][val_lo] */
    uint8_t rsp[6] = { addr, MB_FC_WRITE_SINGLE, req[0], req[1], req[2], req[3] };
    mb_send_response(rsp, sizeof(rsp));
}

/* 10 写多保持寄存器: [addr][10][start_hi][start_lo][cnt_hi][cnt_lo][nbytes][data...] */
static void mb_handle_write_multi(uint8_t addr, const uint8_t *req, size_t req_len)
{
    if (req_len < 5) {
        mb_send_exception(addr, MB_FC_WRITE_MULTI, MB_EX_ILLEGAL_DATA);
        return;
    }
    uint16_t start = (uint16_t)((req[0] << 8) | req[1]);
    uint16_t count = (uint16_t)((req[2] << 8) | req[3]);
    uint8_t  nbytes = req[4];
    if (count == 0 || count > 123 || nbytes != count * 2 || req_len < 5 + nbytes) {
        mb_send_exception(addr, MB_FC_WRITE_MULTI, MB_EX_ILLEGAL_DATA);
        return;
    }
    for (uint16_t i = 0; i < count; i++) {
        uint16_t val = (uint16_t)((req[5 + i * 2] << 8) | req[5 + i * 2 + 1]);
        if (!hold_write(start + i, val)) {
            mb_send_exception(addr, MB_FC_WRITE_MULTI, MB_EX_ILLEGAL_ADDR);
            return;
        }
    }
    /* 响应: [addr][10][start_hi][start_lo][cnt_hi][cnt_lo] */
    uint8_t rsp[6] = { addr, MB_FC_WRITE_MULTI, req[0], req[1], req[2], req[3] };
    mb_send_response(rsp, sizeof(rsp));
}

/* ================================================================ */
bms_err_t sys_modbus_init(void)
{
    bms_err_t err = bsp_rs485_init();
    if (err != BMS_OK) {
        ESP_LOGE(TAG, "bsp_rs485_init fail");
        return err;
    }
    hold_cache_load();
    s_slave_addr = BMS_MODBUS_SLAVE_ADDR;
    s_baud       = BMS_RS485_BAUD_DEFAULT;
    ESP_LOGI(TAG, "Modbus RTU Slave 就绪: 地址=%u 波特率=%lu, 保持寄存器 %u~%u, 输入寄存器 %u~%u",
             (unsigned)s_slave_addr, (unsigned long)s_baud,
             (unsigned)MB_HOLD_START, (unsigned)(MB_HOLD_START + MB_HOLD_COUNT - 1),
             (unsigned)MB_INPUT_START, (unsigned)(MB_INPUT_START + MB_INPUT_COUNT - 1));
    return BMS_OK;
}

/* ================================================================
 *              RS485/Modbus 通信统计(供属性上报/状态卡片)
 * ================================================================ */
#define RS485_ONLINE_TIMEOUT_MS  60000   /* 60s 无轮询判离线(典型 EMS 1~10s 轮询, 留余量) */
void sys_modbus_get_report(uint8_t *online, uint16_t *last_poll_age_s, uint16_t *err_total)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t age = (s_last_poll_ms == 0) ? 0xFFFF : (now - s_last_poll_ms) / 1000;
    if (age > 0xFFFF) age = 0xFFFF;
    if (last_poll_age_s) *last_poll_age_s = (uint16_t)age;
    if (online) *online = (s_last_poll_ms != 0 && (now - s_last_poll_ms) < RS485_ONLINE_TIMEOUT_MS) ? 1 : 0;
    if (err_total) *err_total = (uint16_t)(s_err_total > 0xFFFF ? 0xFFFF : s_err_total);
}

/* ================================================================
 *                    从站任务主循环
 * ================================================================ */
void sys_modbus_task(void *arg)
{
    (void)arg;
    if (sys_modbus_init() != BMS_OK) {
        ESP_LOGE(TAG, "Modbus 初始化失败, 任务退出");
        vTaskDelete(NULL);
        return;
    }

    uint8_t rx[MB_RX_BUF];
    size_t  rx_len = 0;
    uint32_t last_rx_ms = 0;

    while (1) {
        /* 3.5 字符间隔判帧: 有数据则读入; 无数据时若距上次接收超时且已有字节 → 处理帧 */
        uint8_t tmp[MB_RX_BUF];
        size_t n = 0;
        if (bsp_rs485_recv(tmp, sizeof(tmp), &n, 5) == BMS_OK && n > 0) {
            if (rx_len + n > sizeof(rx)) rx_len = 0;        /* 溢出丢弃整帧 */
            memcpy(rx + rx_len, tmp, n);
            rx_len += n;
            last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
        } else {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (rx_len > 0 && (now - last_rx_ms) >= BMS_MODBUS_RTU_TIMEOUT_MS) {
                /* ===== 一帧完整: [addr][func][data...][crc_lo][crc_hi] ===== */
                if (rx_len >= 4) {
                    uint16_t crc_rx = (uint16_t)(rx[rx_len - 2] | (rx[rx_len - 1] << 8));
                    uint16_t crc_calc = mb_crc16(rx, rx_len - 2);
                    if (crc_rx == crc_calc) {
                        /* 统计: 收到一帧 CRC 正确的请求 = 主站在轮询本从站 */
                        s_last_poll_ms = now;
                        s_rx_total++;
                        uint8_t addr = rx[0];
                        uint8_t func = rx[1];
                        if (addr == s_slave_addr || addr == 0) {   /* 0=广播 */
                            const uint8_t *data = rx + 2;
                            size_t data_len = rx_len - 4;          /* 去 addr/func/CRC */
                            switch (func) {
                            case MB_FC_READ_HOLDING:
                            case MB_FC_READ_INPUT:
                                mb_handle_read(addr, func, data, data_len);
                                break;
                            case MB_FC_WRITE_SINGLE:
                                if (addr != 0) mb_handle_write_single(addr, data, data_len);
                                break;
                            case MB_FC_WRITE_MULTI:
                                if (addr != 0) mb_handle_write_multi(addr, data, data_len);
                                break;
                            default:
                                if (addr != 0) mb_send_exception(addr, func, MB_EX_ILLEGAL_FUNC);
                                break;
                            }
                        }
                        /* 地址不匹配: 静默丢弃 */
                    } else {
                        s_err_total++;   /* 统计: CRC 错误帧 */
                        ESP_LOGW(TAG, "CRC 校验失败(0x%04X != 0x%04X), 丢弃帧",
                                 (unsigned)crc_rx, (unsigned)crc_calc);
                    }
                }
                rx_len = 0;
            }
        }
    }
}
