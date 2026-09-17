/**
 * @file    sys_data.c
 * @brief   系统全局数据管理实现(互斥锁保护)
 * @author  BMS Team
 * @date    2026-08
 * @note    系统状态 static 化, 跨任务访问通过 get/set 加锁
 *          
 */
#include "sys_data.h"
#include "bms_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static SemaphoreHandle_t s_mutex = NULL;
static bms_system_state_t s_state;                  // 全局状态(static 化, 不 extern)
static bool s_remote_override = false;              // 远程控制覆盖标志
static uint32_t s_remote_override_tick = 0;         // 远程控制设置时刻(tick)
#define REMOTE_OVERRIDE_TIMEOUT_MS  30000           // 远程控制覆盖超时 30s

/* ====== 内部加锁辅助 ====== */
static void lock(void)   { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

void sys_data_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_state, 0, sizeof(s_state));
    /* 默认值 */
    s_state.soc.soc = SOC_INIT_VALUE;
    s_state.soc.soh = 1.0f;
    s_state.charge_mode = CHARGE_MODE_STOP;
    s_state.relay_charge_on = false;
    s_state.relay_discharge_on = false;
    s_state.derating_factor = 1.0f;                 // 默认满功率, 预警时降低
    s_state.balance_mode = BALANCE_MODE_PASSIVE;    // 默认被动均衡(LTC6804 内部 MOS)
}

void sys_data_set_pack(const bms_pack_data_t *pack)
{
    if (pack == NULL) {
        return;
    }
    lock();
    s_state.pack = *pack;
    unlock();
}

void sys_data_get_pack(bms_pack_data_t *pack)
{
    if (pack == NULL) {
        return;
    }
    lock();
    *pack = s_state.pack;
    unlock();
}

void sys_data_set_soc(const bms_soc_data_t *soc)
{
    if (soc == NULL) {
        return;
    }
    lock();
    s_state.soc = *soc;
    unlock();
}

void sys_data_get_soc(bms_soc_data_t *soc)
{
    if (soc == NULL) {
        return;
    }
    lock();
    *soc = s_state.soc;
    unlock();
}

void sys_data_set_fault(bms_fault_mask_t fault)
{
    lock();
    s_state.fault = fault;
    unlock();
}

bms_fault_mask_t sys_data_get_fault(void)
{
    bms_fault_mask_t f;
    lock();
    f = s_state.fault;
    unlock();
    return f;
}

void sys_data_clear_fault(bms_fault_mask_t mask)
{
    lock();
    s_state.fault &= ~mask;
    unlock();
}

void sys_data_set_charge_mode(bms_charge_mode_e mode)
{
    lock();
    s_state.charge_mode = mode;
    unlock();
}

bms_charge_mode_e sys_data_get_charge_mode(void)
{
    bms_charge_mode_e m;
    lock();
    m = s_state.charge_mode;
    unlock();
    return m;
}

void sys_data_set_balance_mask(bms_balance_mask_t mask)
{
    lock();
    s_state.balance_mask = mask;
    unlock();
}

bms_balance_mask_t sys_data_get_balance_mask(void)
{
    bms_balance_mask_t m;
    lock();
    m = s_state.balance_mask;
    unlock();
    return m;
}

/* ====== 均衡模式(OFF/PASSIVE/ACTIVE, 2026-08-07 双模式) ====== */
void sys_data_set_balance_mode(bms_balance_mode_e mode)
{
    lock();
    s_state.balance_mode = mode;
    unlock();
}

bms_balance_mode_e sys_data_get_balance_mode(void)
{
    bms_balance_mode_e m;
    lock();
    m = s_state.balance_mode;
    unlock();
    return m;
}

void sys_data_set_relay(bool charge_on, bool discharge_on)
{
    lock();
    s_state.relay_charge_on = charge_on;
    s_state.relay_discharge_on = discharge_on;
    unlock();
}

void sys_data_get_relay(bool *charge_on, bool *discharge_on)
{
    lock();
    if (charge_on)    { *charge_on    = s_state.relay_charge_on; }
    if (discharge_on) { *discharge_on = s_state.relay_discharge_on; }
    unlock();
}

void sys_data_set_thermal_lock(bool lock_flag)
{
    lock();
    s_state.thermal_lock = lock_flag;
    unlock();
}

bool sys_data_get_thermal_lock(void)
{
    bool v;
    lock();
    v = s_state.thermal_lock;
    unlock();
    return v;
}

void sys_data_set_derating(float factor)
{
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    lock();
    s_state.derating_factor = factor;
    unlock();
}

float sys_data_get_derating(void)
{
    float v;
    lock();
    v = s_state.derating_factor;
    unlock();
    return v;
}

/* ====== 通信健康状态位域(设备自报, 真·同步监控) ====== */
void sys_data_set_comm_status(bms_comm_status_t status)
{
    lock();
    s_state.comm_status = status;
    unlock();
}

bms_comm_status_t sys_data_get_comm_status(void)
{
    bms_comm_status_t v;
    lock();
    v = s_state.comm_status;
    unlock();
    return v;
}

void sys_data_set_comm_bit(uint32_t bit)
{
    lock();
    s_state.comm_status |= (bms_comm_status_t)bit;
    unlock();
}

void sys_data_clr_comm_bit(uint32_t bit)
{
    lock();
    s_state.comm_status &= ~(bms_comm_status_t)bit;
    unlock();
}

/* ====== 远程控制覆盖标志(带 30s 超时自动清除) ====== */
void sys_data_set_remote_override(bool enable)
{
    lock();
    s_remote_override = enable;
    if (enable) {
        s_remote_override_tick = xTaskGetTickCount();
    }
    unlock();
}

bool sys_data_get_remote_override(void)
{
    bool v;
    lock();
    v = s_remote_override;
    /* 超时自动清除 */
    if (v && (xTaskGetTickCount() - s_remote_override_tick) * portTICK_PERIOD_MS > REMOTE_OVERRIDE_TIMEOUT_MS) {
        s_remote_override = false;
        v = false;
    }
    unlock();
    return v;
}

/* ====== 任务栈高水位摘要(watchdog 每 30s 写入, MQTT 上报带出) ====== */
static char s_stack_summary[128] = "";

void sys_data_set_stack_summary(const char *summary)
{
    lock();
    if (summary) {
        strncpy(s_stack_summary, summary, sizeof(s_stack_summary) - 1);
        s_stack_summary[sizeof(s_stack_summary) - 1] = '\0';
    }
    unlock();
}

const char *sys_data_get_stack_summary(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return NULL;
    }
    lock();
    strncpy(out, s_stack_summary, out_len - 1);
    out[out_len - 1] = '\0';
    unlock();
    return out;
}

/* ====== 主电源开关(SW_PWR 长按置位, 云端开启命令清除) ======
 * 仅 RAM: 重启后默认恢复输出, 关断需现场再长按或云端下发 */
static bool s_master_power_off = false;

void sys_data_set_master_power_off(bool off)
{
    lock();
    s_master_power_off = off;
    unlock();
}

bool sys_data_get_master_power_off(void)
{
    lock();
    bool v = s_master_power_off;
    unlock();
    return v;
}
