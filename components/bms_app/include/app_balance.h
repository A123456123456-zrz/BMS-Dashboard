/**
 * @file    app_balance.h
 * @brief   被动均衡策略
 * @author  BMS Team
 * @date    2026-08
 * @note    触发: 单体 > 均值 + 30mV; 停止: 压差 < 10mV 或超时
 */
#ifndef APP_BALANCE_H
#define APP_BALANCE_H

#include "bms_types.h"

/**
 * @brief   初始化均衡模块
 */
void app_balance_init(void);

/**
 * @brief   均衡周期处理(计算均衡掩码并写入 LTC6804)
 * @param   pack  最新采集数据
 * @return  均衡掩码(每位对应一串)
 */
bms_balance_mask_t app_balance_process(const bms_pack_data_t *pack);

#endif // APP_BALANCE_H
