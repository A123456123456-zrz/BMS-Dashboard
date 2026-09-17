/**
 * @file    sys_data.h
 * @brief   系统全局数据管理(互斥锁保护 + 受控访问)
 * @author  BMS Team
 * @date    2026-08
 * @note    系统状态结构体 static 化, 通过 get/set 函数访问
 *          跨任务共享的多字节结构体必须加锁, 禁止直接 extern
 */
#ifndef SYS_DATA_H
#define SYS_DATA_H

#include <stddef.h>

#include "bms_types.h"
#include <stdbool.h>                                 // bool 类型(自包含原则)

/**
 * @brief   初始化数据管理(创建互斥锁, 填充默认值)
 */
void sys_data_init(void);

/* ====== 采集数据 ====== */
void      sys_data_set_pack(const bms_pack_data_t *pack);
void      sys_data_get_pack(bms_pack_data_t *pack);

/* ====== SOC 数据 ====== */
void      sys_data_set_soc(const bms_soc_data_t *soc);
void      sys_data_get_soc(bms_soc_data_t *soc);

/* ====== 故障掩码 ====== */
void      sys_data_set_fault(bms_fault_mask_t fault);
bms_fault_mask_t sys_data_get_fault(void);
void      sys_data_clear_fault(bms_fault_mask_t mask);     // 清除指定位

/* ====== 充电模式 ====== */
void      sys_data_set_charge_mode(bms_charge_mode_e mode);
bms_charge_mode_e sys_data_get_charge_mode(void);

/* ====== 均衡掩码 ====== */
void      sys_data_set_balance_mask(bms_balance_mask_t mask);
bms_balance_mask_t sys_data_get_balance_mask(void);
void      sys_data_set_balance_mode(bms_balance_mode_e mode);
bms_balance_mode_e sys_data_get_balance_mode(void);

/* ====== 继电器状态 ====== */
void      sys_data_set_relay(bool charge_on, bool discharge_on);
void      sys_data_get_relay(bool *charge_on, bool *discharge_on);

/* ====== 热失控锁定 ====== */
void sys_data_set_thermal_lock(bool lock);
bool sys_data_get_thermal_lock(void);

/* ====== 降额限流因子(0.0~1.0, 预警时降低, 1.0=满功率) ====== */
void  sys_data_set_derating(float factor);
float sys_data_get_derating(void);

/* ====== 通信健康状态位域(设备自报, 取代前端推断) ======
 * 由 task_communication 聚合 WiFi/MQTT/CAN 真实连接态后整体写入,
 * 随 sys_mqtt_report 一起上报, 前端状态卡片据此真·同步显示 */
void            sys_data_set_comm_status(bms_comm_status_t status);
bms_comm_status_t sys_data_get_comm_status(void);
void            sys_data_set_comm_bit(uint32_t bit);   // 置位(事件驱动用)
void            sys_data_clr_comm_bit(uint32_t bit);   // 清位(事件驱动用)

/* ====== 远程控制覆盖标志(云端命令下发后 30s 内保护任务不覆盖继电器) ======
 * set_charge/set_discharge/set_relay 命令设置此标志
 * app_protection 检测到此标志时跳过自动继电器控制(故障保护仍强制覆盖) */
void      sys_data_set_remote_override(bool enable);
bool      sys_data_get_remote_override(void);

/* ====== 任务栈高水位摘要(watchdog 每 30s 采集, MQTT 上报带出, 前端诊断面板展示) ======
 * 格式: "task_prot=2048,task_soc=1536,task_comm=1024,..." (剩余空闲字节, 越小越危险)
 * 前端收到后可渲染为表格或告警(<512B 标红) */
void  sys_data_set_stack_summary(const char *summary);
/* 拷贝出内部缓冲(避免返回内部指针被并发写入撕裂), out_len 建议 >=128 */
const char *sys_data_get_stack_summary(char *out, size_t out_len);

/* ====== 主电源开关状态(SW_PWR 长按 3s 置位, 云端开启命令清除) ======
 * 置位后保护任务在无严重故障时保持 CHG/DSG 全断,
 * 解决"SW_PWR 关断 1s 后被保护任务重开"的 bug (2026-09-09)
 * 仅 RAM 保存: 重启后恢复输出(云端可再次下发关闭) */
void sys_data_set_master_power_off(bool off);
bool sys_data_get_master_power_off(void);

#endif // SYS_DATA_H
