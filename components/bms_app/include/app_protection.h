/**
 * @file    app_protection.h
 * @brief   五级保护逻辑(过压/欠压/过流/过温/热失控 + 继电器控制)
 * @author  BMS Team
 * @date    2026-08
 * @note    预警级仅报警, 保护级断继电器, 锁定级永久断开需人工复位
 */
#ifndef APP_PROTECTION_H
#define APP_PROTECTION_H

#include "bms_types.h"

/**
 * @brief   初始化保护模块(加载阈值)
 */
void app_protection_init(void);

/**
 * @brief   保护周期处理(根据采集数据计算故障并控制继电器)
 * @param   pack  最新采集数据
 * @return  故障掩码
 */
bms_fault_mask_t app_protection_process(const bms_pack_data_t *pack);

#endif // APP_PROTECTION_H
