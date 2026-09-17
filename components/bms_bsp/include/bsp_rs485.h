/**
 * @file    bsp_rs485.h
 * @brief   RS485 半双工通信驱动(MAX3485 + ESP32-S3 UART1, Modbus RTU 物理层)
 * @author  BMS Team
 * @date    2026-08
 * @note    行业标准设计要点:
 *          1. 半双工收发方向控制: DE/RE 并联接 GPIO20, 高电平=发送, 低电平=接收
 *          2. 发送时序: 先拉高 DE → 写 UART → 等 FIFO/移位寄存器排空 → 拉低 DE
 *             (必须等 uart_wait_tx_done, 否则最后一字节被切断, 从站 CRC 校验失败)
 *          3. 默认 9600-8-N-1 (Modbus RTU 标准), 由 bms_config.h 宏可调
 *          4. 接收用轮询 + 3.5 字符间隔判帧(由上层 sys_modbus 处理帧边界)
 *          5. 引脚: TX=GPIO16(DI) / RX=GPIO17(RO) / DE-RE=GPIO20(方向), 见 bms_pinmap.h
 */
#ifndef BSP_RS485_H
#define BSP_RS485_H

#include <stdint.h>
#include <stddef.h>
#include "bms_errno.h"

/**
 * @brief   初始化 RS485(UART1 + 方向控制引脚)
 * @note    波特率/数据位/停止位来自 bms_config.h(BMS_RS485_*), 默认 9600-8-N-1
 * @retval  BMS_OK 成功
 */
bms_err_t bsp_rs485_init(void);

/**
 * @brief   重新配置波特率(运行时改, Modbus 保持寄存器 40002 支持)
 * @param   baud  波特率(1200/2400/4800/9600/19200/38400/57600/115200)
 * @retval  BMS_OK 成功
 */
bms_err_t bsp_rs485_set_baud(uint32_t baud);

/**
 * @brief   获取当前波特率
 */
uint32_t bsp_rs485_get_baud(void);

/**
 * @brief   发送一帧数据(半双工, 内部完成 DE 拉高→发送→排空→拉低)
 * @param   data  数据指针
 * @param   len   长度(字节)
 * @retval  BMS_OK 成功, BMS_ERR_PARAM_INVALID 参数错误
 */
bms_err_t bsp_rs485_send(const uint8_t *data, size_t len);

/**
 * @brief   接收一帧数据(非阻塞轮询, 由上层判断帧边界/超时)
 * @param   buf    接收缓冲
 * @param   buf_len 缓冲容量
 * @param   out_len 实际收到字节数(出参)
 * @param   timeout_ms 等待超时(ms), 0=不等待直接读已有数据
 * @retval  BMS_OK 收到数据, BMS_ERR_TIMEOUT 超时无数据
 */
bms_err_t bsp_rs485_recv(uint8_t *buf, size_t buf_len, size_t *out_len,
                         uint32_t timeout_ms);

/**
 * @brief   查询接收缓冲中已有字节数(用于 3.5 字符间隔判帧)
 * @retval  字节数
 */
size_t bsp_rs485_available(void);

#endif // BSP_RS485_H
