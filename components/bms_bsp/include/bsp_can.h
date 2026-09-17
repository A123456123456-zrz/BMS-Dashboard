/**
 * @file    bsp_can.h
 * @brief   TWAI CAN 通信驱动(ESP32-S3 内置控制器 + SN65HVD230)
 * @author  BMS Team
 * @date    2026-08
 * @note    2026-08-25: 由 MCP2515(SPI) 方案改为 ESP32-S3 内置 TWAI 控制器,
 *          外接 SN65HVD230 收发器(STB 低=正常).
 *          标准帧 11-bit ID, 500kbps
 */
#ifndef BSP_CAN_H
#define BSP_CAN_H

#include <stdint.h>
#include <stdbool.h>
#include "bms_errno.h"

/* CAN 报文结构 */
typedef struct {
    uint32_t id;                                // 标准 11-bit ID
    uint8_t  data[8];                           // 数据域
    uint8_t  dlc;                               // 数据长度 0~8
} bms_can_msg_t;

/**
 * @brief   初始化 MCP2515(SPI + 配置波特率 + 中断)
 * @retval  BMS_OK 成功
 */
bms_err_t bsp_can_init(void);

/**
 * @brief   发送一帧标准报文
 * @param   msg  报文指针(const, 不修改)
 * @retval  BMS_OK 成功, BMS_ERR_TIMEOUT 发送超时
 */
bms_err_t bsp_can_send(const bms_can_msg_t *msg);

/**
 * @brief   查询是否有报文可读
 * @return  true 有报文, false 无
 */
bool bsp_can_has_msg(void);

/**
 * @brief   读取一帧报文(非阻塞)
 * @param   msg  输出报文
 * @retval  BMS_OK 成功, BMS_ERR_FAIL 无报文
 */
bms_err_t bsp_can_recv(bms_can_msg_t *msg);

#endif // BSP_CAN_H
