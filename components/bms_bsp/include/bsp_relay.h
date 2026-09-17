/**
 * @file    bsp_relay.h
 * @brief   充放电继电器驱动(低电平有效)
 * @author  BMS Team
 * @date    2026-08
 * @note    GPIO25=充电继电器, GPIO26=放电继电器
 */
#ifndef BSP_RELAY_H
#define BSP_RELAY_H

#include <stdbool.h>
#include "bms_errno.h"

/**
 * @brief   初始化继电器 GPIO, 默认断开(安全态)
 * @retval  BMS_OK 成功
 */
bms_err_t bsp_relay_init(void);

/**
 * @brief   设置充电继电器
 * @param   on  true=吸合(导通), false=断开(切断)
 */
void bsp_relay_set_charge(bool on);

/**
 * @brief   设置放电继电器
 * @param   on  true=吸合(导通), false=断开(切断)
 */
void bsp_relay_set_discharge(bool on);

/**
 * @brief   紧急切断所有继电器(过温/短路/热失控时调用)
 */
void bsp_relay_cut_all(void);

/**
 * @brief   继电器状态回读确认(2026-08-08 功能安全 S1a, SIL2 诊断覆盖)
 * @param   charge_on  输出: 实际回读充电继电器状态 true=吸合/导通
 * @param   discharge_on 输出: 实际回读放电继电器状态 true=吸合/导通
 * @note    控制引脚同时配置为输入回读; 命令与回读不一致由上层报 FAULT_COMM_WARN.
 *          硬件屏蔽(HW_ENABLE_RELAY=0)或回读失败时返回 false.
 * @retval  true=回读成功  false=无法回读(屏蔽/引脚未配置)
 */
bool bsp_relay_readback(bool *charge_on, bool *discharge_on);

#endif // BSP_RELAY_H
