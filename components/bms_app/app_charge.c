/**
 * @file    app_charge.c
 * @brief   CC-CV 充电策略实现
 * @author  BMS Team
 * @date    2026-08
 * @note    SOC<80% CC 恒流(2A), 80~95% CV 恒压(25.2V), >95% 停止
 *          充电模式通过 sys_data 共享, app_protection 据此控制充电继电器
 *          充电参数(电流/电压)供外部充电器或显示参考
 * @note    硬件屏蔽: HW_ENABLE_RELAY=0 时充电策略仍运行(仅决策, 不控继电器)
 *          继电器由 app_protection 统一管理, 本模块只输出模式与参数
 */
#include "app_charge.h"
#include "bms_config.h"
#include "sys_params.h"                             /* 运行时串数/总压(3~16S 可切换) */
#include "esp_log.h"

static const char *TAG = "APP_CHARGE";

/* ====== 内部状态(对外不可见) ====== */
static bms_charge_mode_e s_last_mode = CHARGE_MODE_STOP;   // 上次模式(变化检测)
static bms_charge_mode_e s_cur_mode  = CHARGE_MODE_STOP;   // 当前模式

void app_charge_init(void)
{
    s_last_mode = CHARGE_MODE_STOP;
    s_cur_mode  = CHARGE_MODE_STOP;
    ESP_LOGI(TAG, "charge init (CC=%dmA CV=%umV stop@%d%%)",
             CHARGE_CC_CURRENT_MA, BMS_FULL_VOLTAGE_MV,
             (int)(CHARGE_CV_SOC_THRESHOLD * 100));
}

/**
 * @brief 充电策略周期处理: 根据 SOC 决定充电模式
 * @param soc  当前 SOC(0.0~1.0)
 * @return 充电模式
 * @note  CC-CV 策略:
 *        SOC < 80% → CC 恒流快充 (充电电流 = CHARGE_CC_CURRENT_MA)
 *        80% <= SOC < 95% → CV 恒压涓流 (充电电压 = BMS_FULL_VOLTAGE_MV)
 *        SOC >= 95% → FULL 已充满, 断开充电继电器
 *        SOC 无效 → STOP 停止充电
 */
bms_charge_mode_e app_charge_process(float soc)
{
    /* SOC 有效性检查 */
    if (soc < 0.0f || soc > 1.0f) {
        s_cur_mode = CHARGE_MODE_STOP;
        return CHARGE_MODE_STOP;
    }

    /* CC-CV 模式决策 */
    if (soc < CHARGE_CC_SOC_THRESHOLD) {
        s_cur_mode = CHARGE_MODE_CC;                       // 恒流快充阶段
    } else if (soc < CHARGE_CV_SOC_THRESHOLD) {
        s_cur_mode = CHARGE_MODE_CV;                       // 恒压涓流阶段
    } else {
        s_cur_mode = CHARGE_MODE_FULL;                     // 已充满, 停止充电
    }

    /* 模式变化时打印日志(避免每秒重复打印) */
    if (s_cur_mode != s_last_mode) {
        const char *names[] = {"STOP", "CC", "CV", "FULL"};
        uint8_t idx_old = (uint8_t)s_last_mode;
        uint8_t idx_new = (uint8_t)s_cur_mode;
        if (idx_old > 3) idx_old = 0;
        if (idx_new > 3) idx_new = 0;
        ESP_LOGI(TAG, "充电模式: %s -> %s (soc=%.1f%%)",
                 names[idx_old], names[idx_new], soc * 100.0f);
        s_last_mode = s_cur_mode;
    }

    return s_cur_mode;
}

/**
 * @brief 获取当前充电参数(目标电流/电压/模式)
 * @return 充电参数结构体
 * @note  供 OLED 显示 / CAN 报文 / 外部充电器通信使用
 *        各模式参数:
 *          CC  : 电流 = CHARGE_CC_CURRENT_MA (2A), 电压 = 运行时满充总压(随串数)
 *          CV  : 电流 = 递减(实际由充电器决定), 电压 = 运行时满充总压(随串数)
 *          FULL: 电流 = 0, 电压 = 运行时满充总压(随串数)
 *          STOP: 电流 = 0, 电压 = 0
 */
bms_charge_params_t app_charge_get_params(void)
{
    bms_charge_params_t params;
    params.mode = s_cur_mode;

    /* 2026-09-07: 3~16S 运行时串数——满充总压取 NVS 运行时值(网页/MQTT 可切换),
     * 原 BMS_FULL_VOLTAGE_MV 编译期宏仅在串数=16 默认值时正确 */
    const uint32_t full_mv = (uint32_t)(sys_params_get()->full_voltage_mv);

    switch (s_cur_mode) {
    case CHARGE_MODE_CC:
        /* 恒流阶段: 固定电流, 电压上限为满充电压 */
        params.target_current_ma = CHARGE_CC_CURRENT_MA;
        params.target_voltage_mv = full_mv;
        break;

    case CHARGE_MODE_CV:
        /* 恒压阶段: 电压固定, 电流递减(由外部充电器实现) */
        params.target_current_ma = CHARGE_CC_CURRENT_MA / 2;   // 涓流参考值
        params.target_voltage_mv = full_mv;
        break;

    case CHARGE_MODE_FULL:
        /* 已充满: 电流为 0 */
        params.target_current_ma = 0;
        params.target_voltage_mv = full_mv;
        break;

    case CHARGE_MODE_STOP:
    default:
        /* 停止充电: 全部归零 */
        params.target_current_ma = 0;
        params.target_voltage_mv = 0;
        break;
    }

    return params;
}
