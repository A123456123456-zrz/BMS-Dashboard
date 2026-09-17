/**
 * @file    sys_config_portal.h
 * @brief   HTTP 配网门户 (Captive Portal + Web 配置页)
 * @author  BMS Team
 * @date    2026-08
 * @note    工作流程:
 *          1. sys_wifi_start_ap() 启动 AP 热点 (192.168.4.1)
 *          2. sys_config_portal_start() 启动 HTTP Server
 *          3. 用户手机连接 AP, 自动弹出(或浏览器访问 192.168.4.1)
 *          4. 填写 WiFi SSID/密码/MQTT URI, 提交保存到 NVS
 *          5. 保存成功后调用 esp_restart() 重启进入 STA 模式
 *          额外页面:
 *            /          配网页(SSID/密码/MQTT URI)
 *            /status    查询当前状态(JSON)
 *            /scan      扫描周边 WiFi(JSON 列表)
 *            /reset     清除配置(强制重新配网)
 */
#ifndef SYS_CONFIG_PORTAL_H
#define SYS_CONFIG_PORTAL_H

#include "bms_errno.h"

/**
 * @brief   启动 HTTP 配网门户(阻塞, 直到用户配网成功或超时)
 * @note    调用前必须先调用 sys_wifi_start_ap()
 * @param   timeout_ms  超时时间(ms), 0=永不超时, 配网成功后自动重启
 * @retval  BMS_OK 配网成功并已重启(不会返回)
 *          BMS_ERR_FAIL 启动失败
 *          BMS_ERR_TIMEOUT 超时退出
 */
bms_err_t sys_config_portal_start(uint32_t timeout_ms);

/**
 * @brief   停止配网门户(用于超时后清理)
 */
void sys_config_portal_stop(void);

#endif // SYS_CONFIG_PORTAL_H
