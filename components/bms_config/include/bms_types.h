/**
 * @file    bms_types.h
 * @brief   BMS 公共数据类型定义
 * @author  BMS Team
 * @date    2026-08
 * @note    跨层共享的数据结构集中定义, 避免循环依赖
 *          字段必须注明单位与量纲
 */
#ifndef BMS_TYPES_H
#define BMS_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "bms_config.h"

/* ====== 电池采集数据(单位见字段注释) ======
 * 2026-08-07: 数组尺寸用 BMS_MAX_CELL_SERIES_NUM(编译期上限)而非实际串数,
 *             避免改 BMS_CELL_SERIES_NUM 时结构体 ABI 变动(跨任务/持久化兼容).
 *             实际串数由 BMS_CELL_SERIES_NUM 控制, 循环遍历用它即可. */
typedef struct {
    uint16_t cell_mv[BMS_MAX_CELL_SERIES_NUM]; // 各单体电压, 单位 mV
    uint16_t cell_mv_max;                      // 最高单体电压, 单位 mV
    uint16_t cell_mv_min;                      // 最低单体电压, 单位 mV
    uint32_t pack_mv;                          // 电池组总压, 单位 mV (32S 满充可达 134V, 用 uint32 防溢出)
    int16_t  current_ma;                       // 总电流, 单位 mA(正放电/负充电)
    int16_t  temp_dc[BMS_MAX_CELL_SERIES_NUM]; // 各点温度, 单位 0.1℃
    int16_t  temp_max_dc;                      // 最高温度, 单位 0.1℃
    int16_t  temp_min_dc;                      // 最低温度, 单位 0.1℃
    uint32_t timestamp_ms;                     // 采集时间戳, 单位 ms
    /* 2026-08-09: 绝缘检测结果(不平衡电桥法, 硬件预留, HW_ENABLE_INSULATION_DETECT) */
    uint32_t insulation_rp_ohm;                // 正极母线对地绝缘电阻, 单位 Ω (0=未启用/无效)
    uint32_t insulation_rn_ohm;                // 负极母线对地绝缘电阻, 单位 Ω
    uint32_t insulation_ohm_per_v;             // 综合绝缘电阻率 = min(Rp,Rn)/pack_v, 单位 Ω/V
} bms_pack_data_t;

/* ====== SOC/SOH 估算结果 ====== */
typedef struct {
    float soc;                                 // 荷电状态, 0.0~1.0
    float soh;                                 // 健康状态, 0.0~1.0
    float soc_ekf;                             // EKF 估算结果(对比用)
    float soc_nn;                              // 神经网络估算结果(对比用)
    float r0_ohm;                              // 欧姆内阻, 单位 Ω
    float r1_ohm;                              // 极化内阻 RC1, 单位 Ω
    float r2_ohm;                              // 扩散内阻 RC2, 单位 Ω
    uint32_t cycle_count;                      // 循环次数
} bms_soc_data_t;

/* ====== 故障状态(位域掩码) ======
 * 分级说明:
 *   WARN  = 预警级(不切断主回路, 降额限流 + 声光提示, 自动恢复)
 *   PROT  = 保护级(切断相应继电器, 蜂鸣器报警, 滞回恢复)
 *   FATAL = 严重级(全断继电器, 急促报警, 需人工复位)
 * 位分配:
 *   0x0001~0x0080  电压/电流 WARN/PROT (8 位)
 *   0x0100~0x0200  温度 WARN/PROT (2 位)
 *   0x0400~0x0800  热失控/短路 FATAL (2 位)
 *   0x1000~0x2000  低温 PROT/压差 WARN (2 位)
 *   0x4000~0x8000  低温 WARN/温升速率 WARN (2 位)
 *   0x10000~0x80000 持续过载/低SOC/SOH/通信 WARN (4 位) */
typedef enum {
    FAULT_NONE          = 0x00000,
    /* ---- 电压类 ---- */
    FAULT_CELL_OV_WARN  = 0x00001,             // 单体过压预警
    FAULT_CELL_OV_PROT  = 0x00002,             // 单体过压保护
    FAULT_CELL_UV_WARN  = 0x00004,             // 单体欠压预警
    FAULT_CELL_UV_PROT  = 0x00008,             // 单体欠压保护
    FAULT_CELL_DV_WARN  = 0x02000,             // 单体压差过大预警
    /* ---- 电流类 ---- */
    FAULT_CHG_OC_WARN   = 0x00010,             // 充电过流预警
    FAULT_CHG_OC_PROT   = 0x00020,             // 充电过流保护
    FAULT_DSG_OC_WARN   = 0x00040,             // 放电过流预警
    FAULT_DSG_OC_PROT   = 0x00080,             // 放电过流保护
    FAULT_OVERLOAD_WARN = 0x10000,             // 持续过载预警(1.05~1.3倍额定, 持续>2s)
    /* ---- 温度类 ---- */
    FAULT_OT_WARN       = 0x00100,             // 过温预警
    FAULT_OT_PROT       = 0x00200,             // 过温保护
    FAULT_UT_WARN       = 0x04000,             // 低温预警(充电限流)
    FAULT_UT_PROT       = 0x01000,             // 低温保护(充电禁止)
    FAULT_DTDT_WARN     = 0x08000,             // 温升速率过快预警(热失控前兆)
    FAULT_THERMAL_RUN   = 0x00400,             // 热失控报警(严重级)
    FAULT_SHORT         = 0x00800,             // 短路(严重级)
    /* ---- 状态/健康度 ---- */
    FAULT_LOW_SOC_WARN  = 0x20000,             // 电量过低预警(SOC<=15%)
    FAULT_LOW_SOH_WARN  = 0x40000,             // 健康度衰减预警(SOH<=80%)
    /* ---- 系统与通信 ---- */
    FAULT_COMM_WARN     = 0x80000,             // 通信/采样异常预警
    FAULT_SAMPLE_FAIL   = 0x100000,            // 电流采样失效(严重级, ADC读数异常)
    /* ---- 绝缘检测(2026-08-09, 对标 GB/T 38661/GB/T 18384.1) ---- */
    FAULT_INSULATION_WARN = 0x200000,          // 绝缘电阻预警(<100Ω/V)
    FAULT_INSULATION_PROT = 0x400000,          // 绝缘电阻保护(<50Ω/V, 切断主回路)
} bms_fault_e;

typedef uint32_t bms_fault_mask_t;             // 故障掩码(32 位, 支持更多预警类型)

/* ====== 通信健康状态位域(设备自报, 随属性上报, 前端状态卡片真·同步) ======
 * 由 WiFi/MQTT/CAN 各模块用真实连接态置位, 取代前端推断.
 * 位定义(1=正常/已建立, 0=异常/未建立):
 *   bit0  WiFi 已关联 AP
 *   bit1  MQTT broker 会话已建立
 *   bit2  TLS 握手成功(mqtts://8883 连接即代表握手成功)
 *   bit3  云端可达: 设备侧最近一次成功 publish 距现在在阈值内
 *   bit4  CAN 总线正常(仅 HW_ENABLE_TWAI=1 时有效)
 *   bit5  CAN 硬件未启用(配置关闭, 前端显示"未启用"灰, 非故障)
 * 命名前缀 COMM_ 避免与 FAULT_ 混淆. */
typedef enum {
    COMM_WIFI_CONNECTED   = (1u << 0),   // WiFi STA 已关联
    COMM_MQTT_CONNECTED   = (1u << 1),   // MQTT broker 会话已建立
    COMM_TLS_OK           = (1u << 2),   // TLS 握手成功
    COMM_CLOUD_REACHABLE  = (1u << 3),   // 设备侧最近一次成功 publish 收到云端确认
    COMM_CAN_OK           = (1u << 4),   // CAN 总线正常(仅启用时有效)
    COMM_CAN_DISABLED     = (1u << 5),   // CAN 硬件未启用(前端显示灰"未启用")
} comm_status_bit_t;

typedef uint32_t bms_comm_status_t;             // 通信状态位域(32 位)

/* ====== 充电模式 ====== */
typedef enum {
    CHARGE_MODE_STOP    = 0,                   // 停止充电
    CHARGE_MODE_CC,                             // 恒流充电
    CHARGE_MODE_CV,                             // 恒压充电
    CHARGE_MODE_FULL,                           // 已充满
} bms_charge_mode_e;

/* ====== 均衡掩码(每位对应一串) ======
 * 2026-08-07: uint16 → uint32, 支持最多 32 串均衡(16S/24S/32S 均覆盖) */
typedef uint32_t bms_balance_mask_t;

/* ====== 均衡模式(2026-08-07 新增: 主动/被动双模式可选) ======
 * PASSIVE: 被动均衡(LTC6804 内部放电 MOS, 电阻耗散, <100mA)
 * ACTIVE : 主动均衡(能量转移, 0.5~5A, 需外接变压器/电感/电容电路)
 * OFF    : 关闭均衡 */
typedef enum {
    BALANCE_MODE_OFF     = 0,      // 关闭均衡
    BALANCE_MODE_PASSIVE = 1,      // 被动均衡(默认, LTC6804 内部 MOS)
    BALANCE_MODE_ACTIVE  = 2,      // 主动均衡(能量转移, 需外接电路)
} bms_balance_mode_e;

/* ====== 系统全局状态(App 层维护) ====== */
typedef struct {
    bms_pack_data_t     pack;                  // 采集数据
    bms_soc_data_t      soc;                   // 估算结果
    bms_fault_mask_t    fault;                 // 故障掩码
    bms_charge_mode_e   charge_mode;           // 充电模式
    bms_balance_mask_t  balance_mask;          // 均衡掩码
    bms_balance_mode_e  balance_mode;          // 均衡模式(OFF/PASSIVE/ACTIVE)
    bool                relay_charge_on;       // 充电继电器状态
    bool                relay_discharge_on;    // 放电继电器状态
    bool                thermal_lock;          // 热失控锁定标志(需人工复位)
    float               derating_factor;       // 降额限流因子(0.0~1.0, 1.0=满功率, 预警时降低)
    bms_comm_status_t   comm_status;           // 通信健康状态位域(设备自报, 真·同步监控)
} bms_system_state_t;

#endif // BMS_TYPES_H
