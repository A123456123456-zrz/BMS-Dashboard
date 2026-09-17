/**
 * @file    bsp_oled.h
 * @brief   SH1106 OLED 显示驱动(I2C, 128x64)
 * @author  BMS Team
 * @date    2026-08
 * @note    地址 0x3C, 提供基础绘图与字符串显示接口
 */
#ifndef BSP_OLED_H
#define BSP_OLED_H

#include <stdint.h>
#include <stdbool.h>
#include "bms_errno.h"

/**
 * @brief   初始化 OLED(I2C 总线 + 屏幕配置 + 清屏)
 * @retval  BMS_OK 成功
 */
bms_err_t bsp_oled_init(void);

/**
 * @brief   清屏
 */
void bsp_oled_clear(void);

/**
 * @brief   显示字符串
 * @param   x     列坐标 0~127
 * @param   y     行坐标 0~7(每页8像素)
 * @param   str   字符串(ASCII)
 */
void bsp_oled_show_string(uint8_t x, uint8_t y, const char *str);

/**
 * @brief   显示浮点数
 * @param   x      列坐标
 * @param   y      行坐标
 * @param   value  数值
 * @param   digits 小数位数
 */
void bsp_oled_show_float(uint8_t x, uint8_t y, float value, uint8_t digits);

/**
 * @brief   刷新显存到屏幕
 */
void bsp_oled_refresh(void);

#endif // BSP_OLED_H
