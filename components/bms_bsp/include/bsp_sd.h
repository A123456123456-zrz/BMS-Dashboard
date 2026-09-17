/**
 * @file    bsp_sd.h
 * @brief   SD 卡驱动(SPI + FATFS 挂载)
 * @author  BMS Team
 * @date    2026-08
 * @note    挂载点 /sdcard, 黑匣子数据存储
 */
#ifndef BSP_SD_H
#define BSP_SD_H

#include <stdint.h>
#include <stdbool.h>
#include "bms_errno.h"

/**
 * @brief   初始化 SD 卡(SPI + FATFS 挂载)
 * @retval  BMS_OK 成功, BMS_ERR_STORAGE 挂载失败
 */
bms_err_t bsp_sd_init(void);

/**
 * @brief   SD 卡是否就绪
 * @return  true 就绪, false 未就绪
 */
bool bsp_sd_is_ready(void);

/**
 * @brief   追加写一行到指定文件
 * @param   path  文件路径(含挂载点, 如 "/sdcard/2026-08-01_BMS_LOG.csv")
 * @param   line  待写入行(含换行符), const 不修改
 * @retval  BMS_OK 成功
 */
bms_err_t bsp_sd_append_line(const char *path, const char *line);

#endif // BSP_SD_H
