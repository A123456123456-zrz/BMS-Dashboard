/**
 * @file    bsp_temp.h
 * @brief   NTC 温度计算驱动
 * @author  BMS Team
 * @date    2026-08
 * @note    NTC 10kΩ B3950, 分压电阻 10kΩ 接 3.3V
 *          温度由 LTC6804 GPIO 采集的分压电压换算
 */
#ifndef BSP_TEMP_H
#define BSP_TEMP_H

#include <stdint.h>
#include "bms_errno.h"

/**
 * @brief   由分压电压计算 NTC 温度
 * @param   vgpio_mv  分压电压, 单位 mV
 * @return  温度, 单位 0.1℃; 异常返回 NTC_TEMP_INVALID 哨兵值
 */
float bsp_temp_calc_dc(uint16_t vgpio_mv);

#endif // BSP_TEMP_H
