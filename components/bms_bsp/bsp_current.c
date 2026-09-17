/**
 * @file    bsp_current.c
 * @brief   电流采样驱动实现(BQ76952 内置库仑计 CC2)
 * @author  BMS Team
 * @date    2026-08
 * @note    2026-08-25: BMS-C1 仿制板无 INA181 外置 ADC,
 *          电流改由 BQ76952 内置库仑计(分流电阻采样)提供,
 *          经 bsp_bq76952_read_current_ma() 读取, 单位 mA, 正=放电 负=充电.
 *          接口(bsp_current.h)保持不变, 上层 app_tasks/app_protection 无需改动.
 */
#include "bsp_current.h"
#include "bsp_bq76952.h"
#include "bms_config.h"
#include "esp_log.h"

static const char *TAG = "BSP_CURRENT";

static bool s_last_valid  = false;   /* 最近一次读取是否有效(未初始化/通信失败=false) */

bms_err_t bsp_current_init(void)
{
    /* 硬件屏蔽: 缺电流采样硬件时直接返回, 不阻塞系统 */
    if (!HW_ENABLE_CURRENT_SENSE) {
        ESP_LOGW(TAG, "CURRENT_SENSE 屏蔽 (HW_ENABLE_CURRENT_SENSE=0)");
        return BMS_ERR_NOT_INIT;
    }

    s_last_valid = false;
    ESP_LOGI(TAG, "current init ok (source=BQ76952 CC2 库仑计)");
    return BMS_OK;
}

uint16_t bsp_current_read_raw(void)
{
    /* 库仑计无"原始 ADC 值", 返回当前电流绝对值(mA)供显示/调试 */
    int16_t ma = 0;
    if (bsp_bq76952_read_current_ma(&ma) != BMS_OK) {
        return 0;
    }
    return (uint16_t)(ma < 0 ? -ma : ma);
}

int16_t bsp_current_read_ma(void)
{
    int16_t ma = 0;
    if (bsp_bq76952_read_current_ma(&ma) != BMS_OK) {
        static uint16_t fail_cnt = 0;            /* 限频: AFE 离线时 30 次打 1 次 */
        if ((++fail_cnt % 30) == 1) {
            ESP_LOGW(TAG, "bq76952 current read fail (AFE 未就绪/通信失败, 累计 %u 次)", fail_cnt);
        }
        s_last_valid = false;
        return 0;
    }
    s_last_valid = true;
    return ma;
}

bool bsp_current_is_valid(void)
{
    return s_last_valid;
}
