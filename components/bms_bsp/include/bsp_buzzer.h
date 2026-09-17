/**
 * @file    bsp_buzzer.h
 * @brief   有源蜂鸣器报警驱动
 * @author  BMS Team
 * @date    2026-08
 * @note    GPIO32, 高电平有效
 */
#ifndef BSP_BUZZER_H
#define BSP_BUZZER_H

#include <stdbool.h>
#include "bms_errno.h"

/**
 * @brief   初始化蜂鸣器 GPIO
 * @retval  BMS_OK 成功
 */
bms_err_t bsp_buzzer_init(void);

/**
 * @brief   设置蜂鸣器开关
 * @param   on  true=响, false=停
 */
void bsp_buzzer_set(bool on);

#endif // BSP_BUZZER_H
