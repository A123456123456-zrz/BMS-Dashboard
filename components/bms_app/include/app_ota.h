/**
 * @file    app_ota.h
 * @brief   OTA 固件升级管理 (HTTPS + A/B 双分区)
 * @author  BMS Team
 * @date    2026-08
 * @note    支持自动版本检查 + 强制 URL 升级
 */
#ifndef APP_OTA_H
#define APP_OTA_H

#include "bms_errno.h"
#include <stdbool.h>

/**
 * @brief   初始化 OTA 子系统
 * @note    新固件首次启动时自动标记 valid 防回滚
 * @retval  BMS_OK 成功
 */
bms_err_t app_ota_init(void);

/**
 * @brief   检查并执行 OTA 升级(阻塞, 由 OTA 任务调用)
 * @note    从 NVS 读取 version_url, 拉取版本 JSON 比较后下载升级
 * @retval  BMS_OK 升级成功(已重启) 或 无需升级
 */
bms_err_t app_ota_check_and_upgrade(void);

/**
 * @brief   强制使用指定 URL 升级(由 MQTT cmd 调用)
 * @param   firmware_url  固件下载 URL (HTTPS)
 * @retval  BMS_OK 升级成功(已重启)
 */
bms_err_t app_ota_upgrade_with_url(const char *firmware_url);

/**
 * @brief   查询 OTA 是否正在进行
 */
bool app_ota_is_in_progress(void);

/* 待升级版本/URL 缓存见 middleware 层 sys_ota_status.h (2026-08-13)
 * (app_ota 写入, sys_mqtt 上报读取, 避免 app↔middleware 循环依赖) */

#endif // APP_OTA_H
