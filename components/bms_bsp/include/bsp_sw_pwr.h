/**
 * @file    bsp_sw_pwr.h
 * @brief   电源键驱动(GPIO13, 高有效, 仿 BMS-C1 SW_PWR)
 * @author  BMS Team
 * @date    2026-08
 * @note    C1 中 SW_PWR=GPIO3(高有效, 按下=高), 长按 3s 关闭功率输出;
 *          S3 上 GPIO3 是 strapping 不能做按键, 改用空闲 GPIO13(PIN_SW_PWR).
 *          按键一端接 GPIO13、一端接 3.3V: 按下=高, 松开=低.
 *          长按 >=3s 触发一次关功率(关 CHG/DSG FET), 释放后复位可再次触发.
 */
#ifndef BSP_SW_PWR_H
#define BSP_SW_PWR_H

#include <stdbool.h>
#include "bms_errno.h"

/**
 * @brief   初始化电源键 GPIO(输入+下拉, 高有效)
 * @retval  BMS_OK 成功, BMS_ERR_NOT_INIT 硬件屏蔽(PIN_SW_PWR<0)
 */
bms_err_t bsp_sw_pwr_init(void);

/**
 * @brief   周期检测电源键长按(建议 100ms 周期调用)
 * @note    高电平持续 >=3s → 调用 bsp_bq76952_set_fet(false,false) 关闭功率,
 *          并打日志; 按住期间仅触发一次, 释放后复位(可再次长按恢复功率前需先松手)
 * @retval  true = 本次周期触发了关机动作
 */
bool bsp_sw_pwr_process(void);

/**
 * @brief   取走短按事件(200ms~3s 内释放, 长按不产生)
 * @return  true = 有一次未消费的短按(取走即清除)
 * @note    2026-09-09 新增: 电源键分级动作 — 短按由 app 层做蜂鸣反馈,
 *          长按仍为关功率; 本驱动只管按键时序, 不含业务
 */
bool bsp_sw_pwr_take_short_press(void);

/**
 * @brief   查询电源键当前是否按下(实时电平)
 * @return  true 按下(高电平), false 释放
 */
bool bsp_sw_pwr_is_pressed(void);

#endif // BSP_SW_PWR_H
