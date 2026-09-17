/**
 * @file    sys_can.h
 * @brief   CAN 通信协议层(BMS 标准报文定义 + 收发封装)
 * @author  BMS Team
 * @date    2026-08
 * @note    报文协议(参考硬件配置说明.md 3.11 节):
 *          0x180 总电压/总电流      (100ms 周期)
 *          0x181 单体极值电压        (1s    周期)
 *          0x182 单体电压 1-6       (100ms 周期, 6 串分两帧)
 *          0x183 温度 1-3           (1s    周期)
 *          0x184 SOC/SOH/循环       (1s    周期)
 *          0x185 故障状态/报警      (事件触发 + 1s 周期)
 *          0x186 均衡状态           (10s   周期)
 *          0x187 充电参数/继电器     (1s    周期)
 *          缺 CAN 模块时 sys_can_send_all 静默返回, 不阻塞调用方
 */
#ifndef SYS_CAN_H
#define SYS_CAN_H

#include "bms_types.h"
#include "bms_errno.h"
#include "bsp_can.h"                              // bms_can_msg_t 类型

/**
 * @brief   发送 BMS 全量报文(0x180~0x187)
 * @note    在通信任务中周期调用(1s), 内部自动分频 100ms/1s/10s 三种周期
 *          非阻塞, 无 CAN 时静默返回 BMS_ERR_NOT_INIT
 */
bms_err_t sys_can_send_all(const bms_pack_data_t    *pack,
                           const bms_soc_data_t     *soc,
                           bms_fault_mask_t          fault,
                           bms_charge_mode_e         charge_mode,
                           bms_balance_mask_t        balance_mask);

/**
 * @brief   处理接收到的 CAN 报文(预留: 外部充电器协议)
 * @param   msg  CAN 报文(const, 不修改)
 * @note    当前仅预留接口, 实际协议解析待后续扩展
 */
void sys_can_handle_rx(const bms_can_msg_t *msg);

/**
 * @brief   CAN 硬件是否启用(对应 HW_ENABLE_TWAI)
 * @retval  true 已启用 MCP2515; false 未接线屏蔽
 */
bool sys_can_is_enabled(void);

/**
 * @brief   CAN 总线是否健康(设备自报 comm_status 用)
 * @retval  启用且最近一次发送成功返回 true; 未启用或总线异常返回 false
 * @note    前端据此区分"正常(绿)"与"未启用(灰)", 避免把未接线误报为故障
 */
bool sys_can_is_ok(void);

#endif // SYS_CAN_H
