/**
 * @file    bsp_active_balance.h
 * @brief   主动均衡驱动抽象(能量转移式)
 * @author  BMS Team
 * @date    2026-08
 * @note    主动均衡通过外部电路(反激变压器/电感/电容 + 开关矩阵)在
 *          电芯间转移能量, 均衡电流可达 0.5~5A(远大于被动均衡 <100mA).
 *          本驱动抽象提供两路控制:
 *            - 使能开关 GPIO (PIN_ACTIVE_BAL_EN, 高有效)
 *            - PWM 调流     (PIN_ACTIVE_BAL_PWM, LEDC, 占空比控制转移电流)
 *          硬件未接线时 HW_ENABLE_ACTIVE_BALANCE=0, 所有接口空操作降级.
 *          实际转移电路(开关矩阵/变压器)由上层按 mask 选通, 见 app_balance.c.
 */
#ifndef BSP_ACTIVE_BALANCE_H
#define BSP_ACTIVE_BALANCE_H

#include "bms_types.h"
#include "bms_errno.h"    /* 2026-08-08: 修复缺失包含, BMS_OK/BMS_ERR_NOT_INIT 定义于此(与其他 BSP 头一致) */

/**
 * @brief   初始化主动均衡硬件(PWM + 使能 GPIO)
 * @return  BMS_OK / BMS_ERR_NOT_INIT(未接线屏蔽)
 */
bms_err_t bsp_active_balance_init(void);

/**
 * @brief   设置主动均衡使能与转移电流
 * @param   mask  均衡掩码(非0=开启, 0=关闭)
 * @param   current_ma  目标转移电流 mA (0~5000, 用于 PWM 占空比)
 * @return  BMS_OK / BMS_ERR_NOT_INIT
 */
bms_err_t bsp_active_balance_set(uint32_t mask, uint16_t current_ma);

#endif // BSP_ACTIVE_BALANCE_H
