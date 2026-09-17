/**
 * @file    app_soc.h
 * @brief   SOC/SOH 估算(AEKF + Dual EKF + SOH 内阻法)
 * @author  BMS Team
 * @date    2026-08
 * @note    状态量 x=[SOC, V_RC1, V_RC2], Sage-Husa 自适应 Q/R
 *          Dual EKF 在线辨识 R0/R1/R2, SOH 由内阻法估算
 */
#ifndef APP_SOC_H
#define APP_SOC_H

#include "bms_types.h"

/**
 * @brief   初始化估算器(电池模型参数 + 初始 SOC)
 */
void app_soc_init(void);

/**
 * @brief   SOC 估算周期处理(AEKF 预测 + 更新)
 * @param   pack  最新采集数据
 * @param   soc   输出估算结果
 */
void app_soc_process(const bms_pack_data_t *pack, bms_soc_data_t *soc);

#endif // APP_SOC_H
