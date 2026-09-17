/**
 * @file    app_charge.h
 * @brief   CC-CV 充电策略管理
 * @author  BMS Team
 * @date    2026-08
 * @note    SOC<80% CC 恒流, 80~95% CV 恒压, >95% 停止
 *          充电模式通过 sys_data 共享, app_protection 据此控制充电继电器
 */
#ifndef APP_CHARGE_H
#define APP_CHARGE_H

#include "bms_types.h"

/* ====== 充电参数结构体(供显示/通信/外部充电器参考) ====== */
typedef struct {
    uint16_t target_current_ma;                         // 目标充电电流, 单位 mA
    uint16_t target_voltage_mv;                         // 目标充电电压, 单位 mV
    bms_charge_mode_e mode;                             // 当前充电模式
} bms_charge_params_t;

/**
 * @brief   初始化充电管理
 */
void app_charge_init(void);

/**
 * @brief   充电策略周期处理(根据 SOC 决定充电模式)
 * @param   soc  当前 SOC(0.0~1.0)
 * @return  充电模式
 */
bms_charge_mode_e app_charge_process(float soc);

/**
 * @brief   获取当前充电参数(目标电流/电压/模式)
 * @return  充电参数结构体
 * @note    供 OLED 显示 / CAN 报文 / 外部充电器通信使用
 */
bms_charge_params_t app_charge_get_params(void);

#endif // APP_CHARGE_H
