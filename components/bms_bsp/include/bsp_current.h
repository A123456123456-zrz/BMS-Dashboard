/**
 * @file    bsp_current.h
 * @brief   电流采样驱动(INA181 + 分流电阻, 双向检测)
 * @author  BMS Team
 * @date    2026-08
 * @note    正电流=放电, 负电流=充电; ADC1_CH6
 */
#ifndef BSP_CURRENT_H
#define BSP_CURRENT_H

#include <stdint.h>
#include <stdbool.h>
#include "bms_errno.h"

/**
 * @brief   初始化电流采样 ADC 通道
 * @retval  BMS_OK 成功
 */
bms_err_t bsp_current_init(void);

/**
 * @brief   读取单次 ADC 原始值
 * @return  ADC 原始值 0~4095
 */
uint16_t bsp_current_read_raw(void);

/**
 * @brief   读取滤波后电流值(内部多次采样均值)
 * @return  电流值, 单位 mA(正放电/负充电)
 */
int16_t bsp_current_read_ma(void);

/**
 * @brief   查询最近一次采样是否有效
 * @return  true=采样有效, false=采样失效(ADC读数异常)
 * @note    采样失效时电流返回0, 上层应据此触发采样异常故障
 */
bool bsp_current_is_valid(void);

#endif // BSP_CURRENT_H
