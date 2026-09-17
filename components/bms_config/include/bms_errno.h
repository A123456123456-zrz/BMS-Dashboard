/**
 * @file    bms_errno.h
 * @brief   BMS 统一错误码定义
 * @author  BMS Team
 * @date    2026-08
 * @note    BSP 层封装 ESP-IDF esp_err_t 后映射为本错误码
 *          App/Middleware 层统一使用 bms_err_t 返回
 */
#ifndef BMS_ERRNO_H
#define BMS_ERRNO_H

#include <stdint.h>

/**
 * @brief BMS 错误码枚举
 * @note  BMS_OK=0 表示成功, 其余为正值错误码(2026-08-10 修正注释:
 *        枚举实际为 0/正值递增, 判断成败请用 `== BMS_OK`, 勿用 <0)
 */
typedef enum {
    BMS_OK = 0,                 // 成功
    BMS_ERR_FAIL,               // 通用失败
    BMS_ERR_PARAM_INVALID,      // 参数非法
    BMS_ERR_TIMEOUT,            // 超时
    BMS_ERR_CRC,                // CRC 校验失败
    BMS_ERR_SPI,                // SPI 通信失败
    BMS_ERR_I2C,                // I2C 通信失败
    BMS_ERR_ADC,                // ADC 采集失败
    BMS_ERR_NOT_INIT,           // 未初始化
    BMS_ERR_NO_MEM,             // 内存不足
    BMS_ERR_OVERFLOW,           // 缓冲区溢出
    BMS_ERR_SENSOR_FAIL,        // 传感器故障
    BMS_ERR_STORAGE,            // 存储读写失败
    BMS_ERR_COMM,               // 通信失败
} bms_err_t;

#endif // BMS_ERRNO_H
