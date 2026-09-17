/**
 * @file    app_reboot_guard.h
 * @brief   重启命令幂等防护模块接口
 * @author  BMS Team
 * @date    2026-08
 */
#ifndef APP_REBOOT_GUARD_H
#define APP_REBOOT_GUARD_H

#include "cJSON.h"

/**
 * @brief  处理 MQTT restart 命令(非阻塞, 必须在 MQTT 事件线程调用)
 * @note   内部实现 4 层防线: 独立延迟任务 / 优雅断开 MQTT / RTC 指纹幂等 / 启动保护期
 * @param  root  命令 JSON 根(用于计算命令指纹)
 */
void handle_cmd_restart(const cJSON *root);

#endif /* APP_REBOOT_GUARD_H */
