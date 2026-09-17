/**
 * @file    bsp_bq34z100.h
 * @brief   BQ34Z100-G1 电量计驱动(I2C, 可选交叉验证)
 * @author  BMS Team
 * @date    2026-08
 * @note    提供独立第三方 SOC/SOH 参考, 验证 AEKF 精度
 */
#ifndef BSP_BQ34Z100_H
#define BSP_BQ34Z100_H

#include <stdint.h>
#include "bms_errno.h"

/**
 * @brief   初始化 BQ34Z100(I2C 总线)
 * @retval  BMS_OK 成功, BMS_ERR_I2C 设备未响应
 */
bms_err_t bsp_bq34z100_init(void);

/**
 * @brief   读取 BQ34Z100 估算的 SOC
 * @return  SOC 百分比 0~100, 异常返回 -1
 */
int8_t bsp_bq34z100_read_soc(void);

/**
 * @brief   读取电池电压
 * @return  电压, 单位 mV, 异常返回 -1
 */
int16_t bsp_bq34z100_read_voltage_mv(void);

#endif // BSP_BQ34Z100_H
