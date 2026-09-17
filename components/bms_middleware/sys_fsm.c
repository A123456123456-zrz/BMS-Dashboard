/**
 * @file    sys_fsm.c
 * @brief   系统状态机实现
 * @author  BMS Team
 * @date    2026-08
 * @note    状态转移规则:
 *          - 热失控/短路 -> LOCKED(永久, 需人工复位)
 *          - 保护级故障  -> PROTECT(断继电器, 可恢复)
 *          - 预警级故障  -> WARN(仅报警)
 *          - 无故障      -> NORMAL
 */
#include "sys_fsm.h"
#include "esp_log.h"

static const char *TAG = "SYS_FSM";

/* 保护级故障掩码(任一位置位即进入保护态) */
#define FAULT_MASK_PROTECT  (FAULT_CELL_OV_PROT | FAULT_CELL_UV_PROT | \
                             FAULT_CHG_OC_PROT  | FAULT_DSG_OC_PROT  | \
                             FAULT_OT_PROT      | FAULT_UT_PROT)

/* 锁定级故障掩码 */
#define FAULT_MASK_LOCKED   (FAULT_THERMAL_RUN | FAULT_SHORT | FAULT_SAMPLE_FAIL)

static sys_state_e s_state = SYS_STATE_INIT;

void sys_fsm_init(void)
{
    s_state = SYS_STATE_INIT;
    ESP_LOGI(TAG, "fsm init");
}

sys_state_e sys_fsm_process(bms_fault_mask_t fault)
{
    /* 优先级最高: 锁定态一旦进入不可自动恢复 */
    if (s_state == SYS_STATE_LOCKED) {
        return s_state;
    }

    /* 热失控/短路 -> 锁定 */
    if (fault & FAULT_MASK_LOCKED) {
        s_state = SYS_STATE_LOCKED;
        ESP_LOGE(TAG, "state -> LOCKED (fault=0x%08X)", (unsigned)fault);
        return s_state;
    }

    /* 保护级故障 -> 保护态 */
    if (fault & FAULT_MASK_PROTECT) {
        if (s_state != SYS_STATE_PROTECT) {
            ESP_LOGW(TAG, "state -> PROTECT (fault=0x%08X)", (unsigned)fault);
        }
        s_state = SYS_STATE_PROTECT;
        return s_state;
    }

    /* 预警级故障 -> 告警态 */
    if (fault != FAULT_NONE) {
        s_state = SYS_STATE_WARN;
        return s_state;
    }

    /* 无故障 -> 正常态 */
    if (s_state != SYS_STATE_NORMAL && s_state != SYS_STATE_INIT) {
        ESP_LOGI(TAG, "state -> NORMAL");
    }
    s_state = SYS_STATE_NORMAL;
    return s_state;
}

sys_state_e sys_fsm_get_state(void)
{
    return s_state;
}

/* 解除锁定态(远程命令或人工复位调用) */
void sys_fsm_clear_lock(void)
{
    if (s_state == SYS_STATE_LOCKED) {
        s_state = SYS_STATE_NORMAL;
        ESP_LOGI(TAG, "LOCKED 已人工解除, state -> NORMAL");
    }
}
