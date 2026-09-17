/**
 * @file    sys_fsm.h
 * @brief   系统状态机(运行态/告警态/保护态/锁定态)
 * @author  BMS Team
 * @date    2026-08
 * @note    根据故障掩码驱动状态转移, App 层据此决定继电器/蜂鸣器动作
 */
#ifndef SYS_FSM_H
#define SYS_FSM_H

#include "bms_types.h"

/* 系统运行状态 */
typedef enum {
    SYS_STATE_INIT      = 0,                        // 初始化中
    SYS_STATE_NORMAL,                               // 正常运行
    SYS_STATE_WARN,                                 // 告警(预警级故障)
    SYS_STATE_PROTECT,                              // 保护(断继电器)
    SYS_STATE_LOCKED,                               // 锁定(热失控, 需人工复位)
} sys_state_e;

/**
 * @brief   初始化状态机
 */
void sys_fsm_init(void);

/**
 * @brief   状态机周期处理(根据故障掩码转移状态)
 * @param   fault  当前故障掩码
 * @return  当前系统状态
 */
sys_state_e sys_fsm_process(bms_fault_mask_t fault);

/**
 * @brief   获取当前系统状态
 */
sys_state_e sys_fsm_get_state(void);

/**
 * @brief   解除锁定态(热失控/短路锁定后, 远程命令或人工复位时调用)
 * @note    仅清除状态机锁定, 不清故障掩码; 仍需配合 sys_data_set_fault(FAULT_NONE)
 */
void sys_fsm_clear_lock(void);

#endif // SYS_FSM_H
