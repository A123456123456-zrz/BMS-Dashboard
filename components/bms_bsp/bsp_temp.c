/**
 * @file    bsp_temp.c
 * @brief   NTC 温度计算实现
 * @author  BMS Team
 * @date    2026-08
 * @note    NTC 10kΩ B3950, 分压电阻 10kΩ 接 3.3V
 *          R_ntc = R_ref * (Vcc - Vgpio) / Vgpio
 *          T = 1 / (1/T0 + (1/B) * ln(R_ntc/R25)) - 273.15
 */
#include "bsp_temp.h"
#include "bms_config.h"
#include <math.h>

float bsp_temp_calc_dc(uint16_t vgpio_mv)
{
    /* 防御: 电压为 0 或接近 VCC 时 logf 域非法, 返回哨兵值 */
    if (vgpio_mv < 50 || vgpio_mv > (ADC_VREF_MV - 50)) {
        return NTC_TEMP_INVALID;
    }

    /* 计算 NTC 当前阻值, 单位 Ω */
    float vgpio = (float)vgpio_mv / 1000.0f;       // mV -> V
    float vcc   = (float)ADC_VREF_MV / 1000.0f;
    float r_ntc = (float)NTC_R_REF_OHM * (vcc - vgpio) / vgpio;

    if (r_ntc <= 0.0f) {
        return NTC_TEMP_INVALID;
    }

    /* Beta 模型计算开尔文温度 */
    const float t0_kelvin = 298.15f;               // 25℃
    float ratio = r_ntc / (float)NTC_R25_OHM;
    if (ratio <= 0.0f) {
        return NTC_TEMP_INVALID;
    }

    float kelvin = 1.0f / (1.0f / t0_kelvin + (1.0f / (float)NTC_B_VALUE) * logf(ratio));
    float temp_c = kelvin - 273.15f;

    /* 返回 0.1℃ 为单位 */
    return temp_c * 10.0f;
}
