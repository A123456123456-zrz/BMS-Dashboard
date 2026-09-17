/**
 * @file    sys_modbus.h
 * @brief   Modbus RTU Slave 协议栈(Middleware 层, 行业标准实现)
 * @author  BMS Team
 * @date    2026-08
 * @note    符合 Modbus Application Protocol V1.1b3 (modbus.org):
 *          1. RTU 模式: 8N1, 帧间隔 ≥3.5 字符, CRC16(MODBUS 多项式 0xA001, 低字节在前)
 *          2. 从站地址 1~247 (0=广播, 广播不回复), 默认 BMS_MODBUS_SLAVE_ADDR
 *          3. 功能码: 03 读保持寄存器 / 04 读输入寄存器 / 06 写单寄存器 / 10 写多寄存器
 *          4. 异常响应: 功能码|0x80 + 异常码(01 非法功能 / 02 非法地址 / 03 非法数据)
 *          5. 寄存器区(1 基址):
 *             30001~30050 输入寄存器(04 只读): BMS 实时数据
 *             40001~40050 保持寄存器(03/06/10 读写): 站地址/波特率/保护阈值
 *          6. 读写保持寄存器实时落盘 sys_params(NVS), 重启不丢失
 */
#ifndef SYS_MODBUS_H
#define SYS_MODBUS_H

#include "bms_errno.h"

/**
 * @brief   初始化 Modbus RTU(内部调用 bsp_rs485_init)
 * @retval  BMS_OK 成功
 */
bms_err_t sys_modbus_init(void);

/**
 * @brief   Modbus 从站任务主循环(由 app_tasks 创建独立任务调用)
 * @note    阻塞接收 + 3.5 字符间隔判帧 + CRC 校验 + 功能码分发 + 回复
 */
void sys_modbus_task(void *arg);

/**
 * @brief   获取 RS485/Modbus 从站通信统计(供属性上报 / 前端状态卡片)
 * @param[out] online          1=近期被主站轮询(在线) 0=离线(超时未轮询或 RS485 未启用)
 * @param[out] last_poll_age_s 距最近一次有效(CRC 正确)请求的年龄(秒, 0xFFFF=从未被轮询)
 * @param[out] err_total       CRC/帧错误累计计数
 * @note    由 sys_mqtt_report 在属性上报时调用; RS485 未启用时(last_poll_ms=0)返回离线。
 */
void sys_modbus_get_report(uint8_t *online, uint16_t *last_poll_age_s, uint16_t *err_total);

#endif // SYS_MODBUS_H
