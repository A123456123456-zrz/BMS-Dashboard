/**
 * @file    sys_wifi.h
 * @brief   WiFi 连接管理 (STA + AP 双模式 + NVS 配网)
 * @author  BMS Team
 * @date    2026-08
 * @note    封装 ESP-IDF WiFi, 支持:
 *          1. STA 模式连接路由器(SSID/密码从 NVS 读取)
 *          2. AP 模式启动热点供配网(192.168.4.1)
 *          3. 配网成功后保存到 NVS 并重启
 *          4. 自动重连 + IP/MAC 查询
 */
#ifndef SYS_WIFI_H
#define SYS_WIFI_H

#include "bms_errno.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief   初始化 WiFi 并以 STA 模式连接路由器(阻塞)
 * @note    SSID/密码从 sys_params_get() 读取(NVS 持久化)
 * @retval  BMS_OK 成功, BMS_ERR_FAIL 连接失败
 */
bms_err_t sys_wifi_init_sta(void);

/**
 * @brief   启动 AP 热点(配网模式, 192.168.4.1)
 * @note    调用此函数后应启动 HTTP Server 提供配网页面
 * @retval  BMS_OK 成功
 */
bms_err_t sys_wifi_start_ap(void);

/**
 * @brief   保存 WiFi 配置到 NVS(配网成功后调用)
 * @param   ssid  路由器 SSID
 * @param   pass  路由器密码
 * @retval  BMS_OK 成功, 保存后需调用 esp_restart() 重启进入 STA 模式
 */
bms_err_t sys_wifi_save_config(const char *ssid, const char *pass);

/**
 * @brief   清除 NVS 中的 WiFi 配置(强制重新配网)
 */
bms_err_t sys_wifi_clear_config(void);

/**
 * @brief   查询 STA 模式是否已连接路由器
 */
bool sys_wifi_is_connected(void);

/**
 * @brief   查询是否处于 AP 配网模式
 */
bool sys_wifi_is_ap_mode(void);

/**
 * @brief   获取当前 IP 地址字符串
 * @param   buf     输出缓冲区
 * @param   buf_len 缓冲区长度(>=16)
 * @retval  BMS_OK 成功
 */
bms_err_t sys_wifi_get_ip_str(char *buf, size_t buf_len);

/**
 * @brief   获取 MAC 地址字符串
 * @param   buf     输出缓冲区
 * @param   buf_len 缓冲区长度(>=18)
 * @retval  BMS_OK 成功
 */
bms_err_t sys_wifi_get_mac_str(char *buf, size_t buf_len);

/* 旧接口兼容(已弃用, 内部转调用 sys_wifi_init_sta) */
bms_err_t sys_wifi_init(void);

/**
 * @brief   重新烧录检测: 串口烧录新固件后自动清 WiFi 配置, 下次启动进入 AP 配网
 * @note    调用时机: 启动流程选择 STA/AP 之前(app_tasks 已调用).
 *          判定: 运行分区 OTA 状态 + 编译指纹(版本|日期|时间):
 *            - OTA 升级后首启(PENDING_VERIFY) → 保留配置, 仅更新指纹
 *            - 指纹与 NVS 记录不同(串口烧录新固件) → 清配置, 进入配网模式
 *            - 指纹相同(正常重启) → 不动
 *          由 BMS_REPROVISION_ON_REFLASH 宏开关(默认 1).
 * @retval  BMS_OK 成功
 */
bms_err_t sys_wifi_check_reflash(void);

#endif // SYS_WIFI_H
