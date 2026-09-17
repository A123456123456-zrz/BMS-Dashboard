/**
 * @file    sys_can.c
 * @brief   CAN 通信协议层实现(BMS 标准报文 0x180~0x187)
 * @author  BMS Team
 * @date    2026-08
 * @note    实现要点:
 *          1. 内部维护分频计数器, 1s 调用一次, 自动派生 100ms/1s/10s 三种周期
 *          2. 0x180/0x182 高频(100ms): 总压电流 + 单体电压(给外部设备实时监控)
 *          3. 0x181/0x183/0x184/0x185/0x187 中频(1s): 极值/温度/SOC/故障/充电参数
 *          4. 0x186 低频(10s): 均衡状态(变化慢, 减少总线占用)
 *          5. 故障掩码变化时立即发送 0x185(事件触发)
 *          6. 缺 MCP2515 时, bsp_can_send 内部已处理, 此处无需判断
 */
#include "sys_can.h"
#include "bms_config.h"
#include "sys_params.h"                             /* 运行时总压(3~16S 串数可切换) */
#include "sys_data.h"                              /* 降额因子读取 */
#include "esp_log.h"
#include <string.h>

static const char *TAG = "SYS_CAN";

/* ====== CAN 报文 ID 定义(11-bit 标准帧) ====== */
#define CAN_ID_PACK_VOLTS       0x180              // 总电压/总电流, 100ms
#define CAN_ID_CELL_EXTREMES    0x181              // 单体极值, 1s
#define CAN_ID_CELLS_1_4        0x182              // 单体 1-4 电压, 100ms (6 串分两帧)
#define CAN_ID_TEMPS            0x183              // 温度 1-3, 1s
#define CAN_ID_SOC_SOH          0x184              // SOC/SOH/循环, 1s
#define CAN_ID_FAULT            0x185              // 故障状态, 1s + 事件
#define CAN_ID_BALANCE          0x186              // 均衡状态, 10s
#define CAN_ID_CHARGE_PARAMS    0x187              // 充电参数/继电器, 1s
#define CAN_ID_CELLS_5_6        0x188              // 单体 5-6 电压, 100ms (6 串第二帧)

/* ====== 内部分频计数器 ======
 * 通信任务以 1s 周期调用 sys_can_send_all
 * 10s 报文通过 tick 计数 1/10 分频发出
 * 注: 100ms 报文在 1s 周期内仅发一次(实际需 100ms 高频需任务改为 100ms 调用) */
static uint32_t s_tick_10s = 0;                      // 1s 计数(0~9 循环, 用于 10s 分频)

/* 上次故障掩码(事件触发判断) */
static bms_fault_mask_t s_last_fault = FAULT_NONE;

/* ====== 内部辅助: 发送单帧 ====== */
/* s_can_ok: CAN 总线健康标志(设备自报 comm_status 用).
 *   收到一次成功发送置 true; 出现发送超时(TX 仲裁/总线错误)置 false.
 *   注: HW_ENABLE_TWAI=0 时 bsp_can_send 直接返回非 OK, 不会置 true,
 *       由 sys_can_is_ok() 结合 enabled 标志返回, 前端据此显示"未启用"而非"故障". */
static bool s_can_ok = false;
static void can_send(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    bms_can_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.id  = id;
    msg.dlc = (dlc > 8) ? 8 : dlc;
    if (data != NULL) {
        memcpy(msg.data, data, msg.dlc);
    }
    bms_err_t r = bsp_can_send(&msg);               // 失败静默(CAN 模块内部已处理屏蔽)
    if (r == BMS_OK) {
        s_can_ok = true;
    } else if (r == BMS_ERR_TIMEOUT) {
        s_can_ok = false;                           // 总线超时: 标记异常
    }
}

/* ====== 0x180 总电压/总电流(100ms 周期) ======
 * Byte 0-1: 总电压, 单位 0.1V, 大端 (例 2235 = 223.5V; 此处 6S 实际 22.35V → 224)
 *           实际单位调整: mV / 100 = 0.1V, 即 pack_mv / 100
 * Byte 2-3: 总电流, 单位 0.1A, 有符号, 大端 (例 +32 = +3.2A)
 *           current_ma / 100 = 0.1A
 * Byte 4:   SOC, 0-100%
 * Byte 5:   充电模式 0=STOP 1=CC 2=CV 3=FULL
 * Byte 6-7: 保留 0x00 */
static void send_pack_volts(const bms_pack_data_t *pack, bms_charge_mode_e chg_mode)
{
    uint8_t buf[8] = {0};
    uint16_t v_01v = (uint16_t)(pack->pack_mv / 100);          // mV -> 0.1V
    int16_t  i_01a = (int16_t)(pack->current_ma / 100);        // mA -> 0.1A
    buf[0] = (uint8_t)(v_01v >> 8);
    buf[1] = (uint8_t)(v_01v & 0xFF);
    buf[2] = (uint8_t)(i_01a >> 8);
    buf[3] = (uint8_t)(i_01a & 0xFF);
    /* SOC 暂用 0(本帧不带 SOC, 由 0x184 提供), 也可填估算值 */
    buf[4] = 0;
    buf[5] = (uint8_t)chg_mode;
    can_send(CAN_ID_PACK_VOLTS, buf, 8);
}

/* ====== 0x181 单体极值(1s 周期) ======
 * Byte 0-1: 最高单体电压, 0.001V, 大端 (mV 直传)
 * Byte 2-3: 最低单体电压, 0.001V
 * Byte 4:   最高单体编号(1~12)
 * Byte 5:   最低单体编号
 * Byte 6:   压差, 0.001V (低 8 位)
 * Byte 7:   压差, 0.001V (高 8 位) */
static void send_cell_extremes(const bms_pack_data_t *pack)
{
    uint8_t buf[8] = {0};
    buf[0] = (uint8_t)(pack->cell_mv_max >> 8);
    buf[1] = (uint8_t)(pack->cell_mv_max & 0xFF);
    buf[2] = (uint8_t)(pack->cell_mv_min >> 8);
    buf[3] = (uint8_t)(pack->cell_mv_min & 0xFF);
    /* 极值编号暂用 0(需要上层传入, 此处简化) */
    buf[4] = 0;
    buf[5] = 0;
    uint16_t diff = pack->cell_mv_max - pack->cell_mv_min;
    buf[6] = (uint8_t)(diff & 0xFF);
    buf[7] = (uint8_t)(diff >> 8);
    can_send(CAN_ID_CELL_EXTREMES, buf, 8);
}

/* ====== 0x182 单体电压 1-6(100ms 周期) ======
 * 6 串单体电压, 每串 2 字节(0.001V), 共 12 字节 → 分两帧
 * 帧一(0x182): C1-C4 共 8 字节
 * 帧二(0x188): C5-C6 共 4 字节 (H17 修复: 原注释声称有 0x188 第二帧,
 *              但实现缺失, 5/6 串电压永远无法经 CAN 上报; 现补齐) */
static void send_cells_1_4(const bms_pack_data_t *pack)
{
    uint8_t buf[8] = {0};
    for (uint8_t i = 0; i < 4 && i < BMS_CELL_SERIES_NUM; i++) {
        buf[i * 2]     = (uint8_t)(pack->cell_mv[i] >> 8);
        buf[i * 2 + 1] = (uint8_t)(pack->cell_mv[i] & 0xFF);
    }
    can_send(CAN_ID_CELLS_1_4, buf, 8);
}

/* ====== 0x188 单体电压 5-6(100ms 周期, 6 串第二帧) ====== */
static void send_cells_5_6(const bms_pack_data_t *pack)
{
    uint8_t buf[8] = {0};
    for (uint8_t i = 4; i < 6 && i < BMS_CELL_SERIES_NUM; i++) {
        uint8_t k = (uint8_t)(i - 4);
        buf[k * 2]     = (uint8_t)(pack->cell_mv[i] >> 8);
        buf[k * 2 + 1] = (uint8_t)(pack->cell_mv[i] & 0xFF);
    }
    can_send(CAN_ID_CELLS_5_6, buf, 4);
}

/* ====== 0x183 温度 1-3(1s 周期) ======
 * 3 路温度, 每路 1 字节, 单位 ℃(有符号, 偏移 0)
 * Byte 0: T1, Byte 1: T2, Byte 2: T3
 * Byte 3: 最高温
 * Byte 4-7: 保留 */
static void send_temps(const bms_pack_data_t *pack)
{
    uint8_t buf[8] = {0};
    for (uint8_t i = 0; i < 3 && i < BMS_CELL_SERIES_NUM; i++) {
        /* 0.1℃ → 1℃
         * H17 修复: 原强转 uint8_t 使负温(如 -20℃ → 236℃)变成大正数,
         * 现用 int8_t 携带符号位, 接收方按有符号解析 */
        buf[i] = (uint8_t)(int8_t)(pack->temp_dc[i] / 10);
    }
    buf[3] = (uint8_t)(int8_t)(pack->temp_max_dc / 10);
    can_send(CAN_ID_TEMPS, buf, 4);
}

/* ====== 0x184 SOC/SOH/循环(1s 周期) ======
 * Byte 0: SOC, 0-100%
 * Byte 1: SOH, 0-100%
 * Byte 2-3: 循环次数, 大端
 * Byte 4-5: 内阻, 0.1mΩ, 大端
 * Byte 6-7: 保留 */
static void send_soc_soh(const bms_soc_data_t *soc)
{
    uint8_t buf[8] = {0};
    buf[0] = (uint8_t)(soc->soc * 100.0f);
    buf[1] = (uint8_t)(soc->soh * 100.0f);
    buf[2] = (uint8_t)(soc->cycle_count >> 8);
    buf[3] = (uint8_t)(soc->cycle_count & 0xFF);
    uint16_t r_01mohm = (uint16_t)(soc->r0_ohm * 10000.0f);     // Ω -> 0.1mΩ
    buf[4] = (uint8_t)(r_01mohm >> 8);
    buf[5] = (uint8_t)(r_01mohm & 0xFF);
    can_send(CAN_ID_SOC_SOH, buf, 6);
}

/* ====== 0x185 故障状态(1s 周期 + 事件触发) ======
 * Byte 0-3: 故障掩码, 大端 32 位(见 bms_types.h FAULT_xxx)
 * Byte 4:   故障等级 0=正常 1=预警 2=保护 3=紧急
 * Byte 5:   降额因子(0~100, 单位 %, 100=满功率)
 * Byte 6-7: 保留 */
static void send_fault(bms_fault_mask_t fault)
{
    uint8_t buf[8] = {0};
    /* 32 位故障掩码, 大端 */
    buf[0] = (uint8_t)(fault >> 24);
    buf[1] = (uint8_t)(fault >> 16);
    buf[2] = (uint8_t)(fault >> 8);
    buf[3] = (uint8_t)(fault & 0xFF);

    /* 故障等级判定(严重 > 保护 > 预警) */
    uint8_t level = 0;
    if (fault & (FAULT_THERMAL_RUN | FAULT_SHORT | FAULT_SAMPLE_FAIL)) {
        level = 3;                                   // 紧急(严重级)
    } else if (fault & (FAULT_CELL_OV_PROT | FAULT_CELL_UV_PROT |
                        FAULT_CHG_OC_PROT | FAULT_DSG_OC_PROT |
                        FAULT_OT_PROT | FAULT_UT_PROT)) {
        level = 2;                                   // 保护级
    } else if (fault & (FAULT_CELL_OV_WARN  | FAULT_CELL_UV_WARN |
                        FAULT_CHG_OC_WARN   | FAULT_DSG_OC_WARN  |
                        FAULT_OT_WARN       | FAULT_CELL_DV_WARN |
                        FAULT_UT_WARN       | FAULT_DTDT_WARN   |
                        FAULT_OVERLOAD_WARN | FAULT_LOW_SOC_WARN |
                        FAULT_LOW_SOH_WARN  | FAULT_COMM_WARN)) {
        level = 1;                                   // 预警级(10 种)
    }
    buf[4] = level;

    /* 降额因子(预警级不切断主回路, 通过 CAN 请求上游降额) */
    buf[5] = (uint8_t)(sys_data_get_derating() * 100.0f);

    can_send(CAN_ID_FAULT, buf, 6);
}

/* ====== 0x186 均衡状态(10s 周期) ======
 * Byte 0-3: 均衡掩码, 大端 32 位(每位对应一串, 支持 32S)
 * Byte 4:   均衡串数
 * Byte 5-7: 保留 */
static void send_balance(bms_balance_mask_t mask)
{
    uint8_t buf[8] = {0};
    /* H17 修复: 原 uint16_t 截断掩码, 高 16 位(17~32 串)丢失; 现按 32 位大端发送 */
    buf[0] = (uint8_t)(mask >> 24);
    buf[1] = (uint8_t)(mask >> 16);
    buf[2] = (uint8_t)(mask >> 8);
    buf[3] = (uint8_t)(mask & 0xFF);
    /* 统计均衡串数 */
    uint8_t cnt = 0;
    uint32_t m = mask;
    while (m) {
        if (m & 1) cnt++;
        m >>= 1;
    }
    buf[4] = cnt;
    can_send(CAN_ID_BALANCE, buf, 5);
}

/* ====== 0x187 充电参数/继电器(1s 周期) ======
 * Byte 0-1: 充电电流设定, 0.01A, 大端 (mA / 10)
 * Byte 2-3: 充电电压设定, 0.01V, 大端 (mV / 10)
 * Byte 4:   充电模式 0=STOP 1=CC 2=CV 3=FULL
 * Byte 5:   继电器状态 bit0=充电 bit1=放电
 * Byte 6-7: 保留 */
static void send_charge_params(bms_charge_mode_e mode)
{
    uint8_t buf[8] = {0};
    /* 充电参数: 2026-09-07 满充总压取运行时值(3~16S 串数可切换) */
    uint16_t i_001a = (mode == CHARGE_MODE_CC) ? (CHARGE_CC_CURRENT_MA / 10) : 0;
    uint16_t v_001v = (uint16_t)(sys_params_get()->full_voltage_mv / 10);
    buf[0] = (uint8_t)(i_001a >> 8);
    buf[1] = (uint8_t)(i_001a & 0xFF);
    buf[2] = (uint8_t)(v_001v >> 8);
    buf[3] = (uint8_t)(v_001v & 0xFF);
    buf[4] = (uint8_t)mode;

    /* 继电器真实状态(M7 修复: 以 sys_data 为准, 不再用充电模式推断,
     * 避免故障/远程控制断继电器时 CAN 仍报闭合) */
    bool chg_on = false, dsg_on = false;
    sys_data_get_relay(&chg_on, &dsg_on);
    buf[5] = (uint8_t)((chg_on ? 0x01 : 0) | (dsg_on ? 0x02 : 0));
    can_send(CAN_ID_CHARGE_PARAMS, buf, 6);
}

/* ================================================================ */
bms_err_t sys_can_send_all(const bms_pack_data_t    *pack,
                           const bms_soc_data_t     *soc,
                           bms_fault_mask_t          fault,
                           bms_charge_mode_e         charge_mode,
                           bms_balance_mask_t        balance_mask)
{
    if (pack == NULL || soc == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }

    /* ====== 100ms 周期报文(每 1s 调用, 内部不补发 100ms, 仅发一次)
     * 注: 真正 100ms 周期需通信任务以 100ms 调用本函数
     *     当前通信任务 1s 调用, 此处每秒发一次, 满足监控需求
     *     若需 100ms 高频, 通信任务周期改为 100ms 即可 */
    send_pack_volts(pack, charge_mode);
    send_cells_1_4(pack);
    send_cells_5_6(pack);                          /* H17: 6 串第二帧(0x188) */

    /* ====== 1s 周期报文 ====== */
    send_cell_extremes(pack);
    send_temps(pack);
    send_soc_soh(soc);
    send_fault(fault);
    send_charge_params(charge_mode);

    /* ====== 事件触发: 故障掩码变化时立即重发 0x185 ====== */
    if (fault != s_last_fault) {
        send_fault(fault);
        s_last_fault = fault;
    }

    /* ====== 10s 周期报文(均衡状态) ====== */
    s_tick_10s++;
    if (s_tick_10s >= 10) {
        s_tick_10s = 0;
        send_balance(balance_mask);
    }

    return BMS_OK;
}

/* ================================================================ */
void sys_can_handle_rx(const bms_can_msg_t *msg)
{
    if (msg == NULL) {
        return;
    }

    /* 预留: 外部充电器协议解析
     * 常见充电器 CAN 协议:
     *   0x180xxxx 充电器状态反馈
     *   0x180xxxx 充电器故障码
     * 当前仅打印调试, 后续按实际充电器协议扩展 */
    ESP_LOGD(TAG, "CAN RX id=0x%03lX dlc=%u", (unsigned long)msg->id, msg->dlc);
}

/* ====== CAN 状态查询(设备自报 comm_status 用) ====== */
bool sys_can_is_enabled(void)
{
    return (bool)HW_ENABLE_TWAI;
}

bool sys_can_is_ok(void)
{
    /* 仅当硬件启用时, s_can_ok 才有意义; 未启用返回 false(前端据 DISABLED 位显示灰) */
    return (bool)HW_ENABLE_TWAI && s_can_ok;
}
