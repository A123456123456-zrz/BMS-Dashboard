/**
 * @file    bsp_led.h
 * @brief   LED 三灯状态指示驱动
 * @note    GPIO1=红, GPIO2=绿, GPIO38=板载白色RGB(共阳, 低=亮高=灭)
 */
#ifndef BSP_LED_H
#define BSP_LED_H

#include <stdbool.h>
#include <stdint.h>
#include "bms_errno.h"

typedef enum {
    BSP_LED_OFF     = 0,
    BSP_LED_GREEN   = 1,
    BSP_LED_YELLOW  = 2,
    BSP_LED_RED     = 3,
} bsp_led_color_e;

bms_err_t bsp_led_init(void);
void      bsp_led_set(bool on);
void      bsp_led_set_color(bsp_led_color_e color);
void      bsp_led_toggle(void);
void      bsp_led_white(bool on);
void      bsp_led_white_toggle(void);

#endif // BSP_LED_H
