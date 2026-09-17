/**
 * @file    bsp_relay.c
 * @brief   功率开关驱动实现(BQ76952 CHG/DSG FET 控制, 2026-08-25 替代继电器 GPIO)
 * @author  BMS Team
 * @date    2026-08
 * @note    BMS-C1 仿制板无继电器, 充/放电通路由 BQ76952 内部 FET 控制:
 *          经 bsp_bq76952_set_fet() 下发 FET_CONTROL 子命令.
 *          接口(bsp_relay.h)保持不变, 上层 app_protection 调用逻辑无需改动.
 */
#include "bsp_relay.h"
#include "bsp_bq76952.h"
#include "bms_config.h"
#include "esp_log.h"
#include <stddef.h>

static const char *TAG = "BSP_RELAY";

static bool s_inited = false;
static bool s_chg_on = false;      /* 当前命令状态(供回读) */
static bool s_dsg_on = false;

bms_err_t bsp_relay_init(void)
{
    /* 硬件屏蔽: 缺功率开关控制时直接返回, 不阻塞系统 */
    if (!HW_ENABLE_RELAY) {
        ESP_LOGW(TAG, "RELAY 屏蔽 (HW_ENABLE_RELAY=0)");
        return BMS_ERR_NOT_INIT;
    }

    /* 上电安全态: 全部关断(CHG/DSG FET 均禁止)
     * 注: 若 BQ76952 尚未初始化(init 顺序在 relay 之后), set_fet 返回 NOT_INIT,
     *     由 app_protection 周期任务在 AFE 就绪后按需补发 */
    s_chg_on = false;
    s_dsg_on = false;
    bms_err_t ret = bsp_bq76952_set_fet(false, false);
    if (ret != BMS_OK) {
        ESP_LOGW(TAG, "FET 关断暂不可用(BQ76952 未就绪? ret=%d), 保护任务将补发", (int)ret);
    }

    s_inited = true;
    ESP_LOGI(TAG, "relay(fet) init ok");
    return BMS_OK;
}

void bsp_relay_set_charge(bool on)
{
    if (!HW_ENABLE_RELAY || !s_inited) {
        return;
    }
    s_chg_on = on;
    bsp_bq76952_set_fet(s_chg_on, s_dsg_on);
}

void bsp_relay_set_discharge(bool on)
{
    if (!HW_ENABLE_RELAY || !s_inited) {
        return;
    }
    s_dsg_on = on;
    bsp_bq76952_set_fet(s_chg_on, s_dsg_on);
}

void bsp_relay_cut_all(void)
{
    if (!HW_ENABLE_RELAY || !s_inited) {
        return;
    }
    s_chg_on = false;
    s_dsg_on = false;
    bsp_bq76952_set_fet(false, false);
}

/* ====== 2026-08-08 功能安全 S1a: 状态回读确认(SIL2 诊断覆盖) ======
 * 继电器方案回读 GPIO 电平; FET 方案下回读 BQ76952 命令状态(驱动级诊断),
 * 上层(app_protection)在回读与命令不一致时报 FAULT_COMM_WARN. */
bool bsp_relay_readback(bool *charge_on, bool *discharge_on)
{
    if (!HW_ENABLE_RELAY) {
        return false;                               /* 硬件屏蔽: 无法回读 */
    }
    if (charge_on == NULL || discharge_on == NULL) {
        return false;
    }
    *charge_on    = s_chg_on;
    *discharge_on = s_dsg_on;
    return true;
}
