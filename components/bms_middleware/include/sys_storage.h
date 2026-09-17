/**
 * @file    sys_storage.h
 * @brief   SD 卡黑匣子日志服务(CSV 格式, 按日期分文件)
 * @author  BMS Team
 * @date    2026-08
 * @note    正常每秒一帧, 故障时切 100ms 高频记录
 *          文件名: /sdcard/YYYY-MM-DD_BMS_LOG.csv
 */
#ifndef SYS_STORAGE_H
#define SYS_STORAGE_H

#include "bms_types.h"
#include "bms_errno.h"

/**
 * @brief   初始化日志服务(写表头)
 * @retval  BMS_OK 成功, BMS_ERR_STORAGE SD 卡未就绪(降级)
 */
bms_err_t sys_storage_init(void);

/**
 * @brief   记录一帧数据到黑匣子
 * @param   pack  采集数据快照
 * @param   soc   SOC 数据快照
 * @param   fault 当前故障掩码
 * @note    故障时由调用方提高调用频率
 */
void sys_storage_log_frame(const bms_pack_data_t *pack,
                           const bms_soc_data_t  *soc,
                           bms_fault_mask_t       fault);

#endif // SYS_STORAGE_H
