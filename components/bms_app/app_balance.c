/**
 * @file    app_balance.c
 * @brief   均衡策略实现(被动/主动双模式)
 * @author  BMS Team
 * @date    2026-08
 * @note    触发: 单体 > 均值 + 30mV; 停止: 压差 < 10mV / 超时 30min / 过温保护
 *          均衡掩码写入电压采集 AFE 配置寄存器(被动: BQ76952/LTC6804) 或 主动均衡驱动(PWM调流)
 *          模式选择: sys_data_get_balance_mode() = PASSIVE / ACTIVE / OFF
 *          安全保护: 过温(>55℃)停止均衡, 超时(>30min)强制停止
 * @note    硬件屏蔽: HW_ENABLE_LTC6804=0 且 HW_ENABLE_BQ76952=0 时被动均衡返回 0;
 *          HW_ENABLE_ACTIVE_BALANCE=0 时主动均衡返回 0, 不阻塞系统
 */
#include "app_balance.h"
#include "bms_config.h"
#include "sys_params.h"
#include "bsp_ltc6804.h"
#if HW_ENABLE_BQ76952
#include "bsp_bq76952.h"
#endif
#include "bsp_active_balance.h"
#include "sys_data.h"
#include "esp_log.h"
#include <stddef.h>                                   // NULL

static const char *TAG = "APP_BAL";

/* ====== 均衡状态记录(内部 static, 对外不可见) ====== */
static uint32_t s_balance_start_ms;                   // 本次均衡开始时间戳, 单位 ms
static bool     s_balance_active;                     // 是否有串在均衡中
static uint32_t s_last_check_ms;                      // 上次检查时间戳, 单位 ms

/* 主动均衡目标电流(2026-08-07, 企业级典型 1A, 可在 bms_config.h 调整) */
#ifndef ACTIVE_BAL_CURRENT_MA
#define ACTIVE_BAL_CURRENT_MA       1000
#endif

void app_balance_init(void)
{
    s_balance_start_ms = 0;
    s_balance_active   = false;
    s_last_check_ms    = 0;
    ESP_LOGI(TAG, "balance init (超时=%dmin, 过温阈值=%d.%d℃)",
             BALANCE_TIMEOUT_MIN,
             TEMP_OT_PROT_DC / 10, TEMP_OT_PROT_DC % 10);
}

/**
 * @brief 均衡周期处理: 计算均衡掩码并写入 LTC6804
 * @param pack  最新采集数据
 * @return 均衡掩码(每位对应一串), 0=无均衡
 * @note  保护逻辑:
 *        1. 电压采集 AFE(LTC6804/BQ76952) 全部屏蔽 → 返回 0
 *        2. 过温保护(温度 >= 55℃) → 关闭均衡
 *        3. 超时保护(均衡持续 > 30min) → 关闭均衡
 *        4. 压差 < 10mV → 关闭均衡
 *        5. 单体 > 均值 + 30mV → 开启对应串均衡
 */
bms_balance_mask_t app_balance_process(const bms_pack_data_t *pack)
{
    if (pack == NULL) {
        return 0;
    }

    /* ====== 保护1: 电压采集 AFE 全部屏蔽时直接返回(无电压数据源) ====== */
#if !HW_ENABLE_LTC6804 && !HW_ENABLE_BQ76952
    return 0;
#endif

    /* 当前均衡模式(OFF/PASSIVE/ACTIVE, 由 set_balance 命令/前端选择) */
    bms_balance_mode_e mode = sys_data_get_balance_mode();

    /* ====== 内部: 按模式关闭均衡 ====== */
    /* 被动均衡关闭: 写入 0 掩码到当前启用的 AFE(宏体内不能有 #if, 先选好函数再定义宏) */
#if HW_ENABLE_BQ76952
    #define BALANCE_AFE_OFF() bsp_bq76952_set_balance(0)
#elif HW_ENABLE_LTC6804
    #define BALANCE_AFE_OFF() bsp_ltc6804_set_balance(0)
#else
    #define BALANCE_AFE_OFF() ((void)0)
#endif
    #define BALANCE_STOP() do {                                                 \
        if (mode == BALANCE_MODE_ACTIVE) {                                      \
            bsp_active_balance_set(0, 0);                                       \
        } else {                                                                \
            BALANCE_AFE_OFF();                                                  \
        }                                                                       \
    } while (0)

    /* ====== 保护2: 过温保护(均衡会发热, 高温下必须停止) ======
     * 阈值取 TEMP_OT_PROT_DC (60.0℃), 实际取 TEMP_OT_WARN_DC (50.0℃) 留安全余量 */
    if (pack->temp_max_dc >= TEMP_OT_WARN_DC) {
        if (s_balance_active) {
            ESP_LOGW(TAG, "过温停止均衡 (Tmax=%d.%d℃)",
                     pack->temp_max_dc / 10, pack->temp_max_dc % 10);
            BALANCE_STOP();
            s_balance_active = false;
        }
        return 0;
    }

    /* ====== 保护3: 超时保护(单次均衡最长 30 分钟, 防止电阻/变压器过热) ====== */
    uint32_t now_ms = pack->timestamp_ms;
    if (s_balance_active && s_balance_start_ms > 0) {
        uint32_t elapsed_ms = now_ms - s_balance_start_ms;
        if (elapsed_ms > (uint32_t)BALANCE_TIMEOUT_MIN * 60 * 1000) {
            ESP_LOGW(TAG, "均衡超时停止 (>%dmin)", BALANCE_TIMEOUT_MIN);
            BALANCE_STOP();
            s_balance_active = false;
            return 0;
        }
    }

    /* ====== 计算单体均值(运行时串数, 网页可下发) ====== */
    uint8_t bal_series = (uint8_t)(sys_params_get()->cell_series_num);
    if (bal_series < 1 || bal_series > BMS_MAX_CELL_SERIES_NUM) {
        bal_series = BMS_CELL_SERIES_NUM;
    }
    uint32_t sum = 0;
    for (uint8_t i = 0; i < bal_series; i++) {
        sum += pack->cell_mv[i];
    }
    uint16_t avg_mv = (uint16_t)(sum / bal_series);

    /* ====== 计算压差 ====== */
    uint16_t diff_mv = pack->cell_mv_max - pack->cell_mv_min;

    /* ====== 保护4: 压差小于停止阈值, 关闭所有均衡 ====== */
    if (diff_mv < BALANCE_STOP_MV) {
        if (s_balance_active) {
            BALANCE_STOP();
            s_balance_active = false;
        }
        return 0;
    }

    /* ====== 保护5: 充电中不均衡(避免干扰充电曲线) ======
     * 充电电流为负, 若正在充电则跳过均衡
     * (可选策略, 此处保留注释供用户按需启用)
     * if (pack->current_ma < -100) {
     *     return 0;
     * } */

    /* ====== v9: 均衡起始 SOC 门槛(网页下发 balance_start_soc_pct) ======
     * SOC 低于门槛时不启动均衡(高 SOC 区间均衡效率更高, 企业级常见策略) */
    const bms_params_t *bp = sys_params_get();
    if (bp->balance_start_soc_pct > 0) {
        bms_soc_data_t soc_now;
        sys_data_get_soc(&soc_now);
        uint8_t soc_pct = (uint8_t)(soc_now.soc * 100.0f);
        if (soc_pct < bp->balance_start_soc_pct) {
            if (s_balance_active) {
                BALANCE_STOP();
                s_balance_active = false;
            }
            return 0;
        }
    }

    /* ====== v9: 按策略计算均衡掩码 ======
     *   balance_strategy=0: 电压差触发(默认) - 单体 > 均值 + 阈值 开启
     *   balance_strategy=1: 容量差触发      - 高于均值最多的 N 串开启(按压差比例)
     *   balance_strategy=2: 定时均衡        - 达到启动压差后, 高低两端各开一半 */
    const uint8_t bal_strategy = bp->balance_strategy;
    bms_balance_mask_t mask = 0;
    if (bal_strategy == 0) {
        for (uint8_t i = 0; i < bal_series; i++) {
            if (pack->cell_mv[i] > (uint16_t)(avg_mv + BALANCE_THRESHOLD_MV)) {
                mask |= (1u << i);                       /* uint32 掩码, 防符号溢出 */
            }
        }
    } else if (bal_strategy == 1) {
        /* 容量差触发: 高出均值阈值以上的串按电压降序, 开最高的一半(均衡高位放能) */
        uint8_t high_cnt = 0;
        for (uint8_t i = 0; i < bal_series; i++) {
            if (pack->cell_mv[i] > (uint16_t)(avg_mv + BALANCE_THRESHOLD_MV)) high_cnt++;
        }
        uint8_t half = (high_cnt + 1) / 2;   /* 开高的一半天数 */
        for (uint8_t r = 0; r < half; r++) {
            uint16_t best_v = 0; uint8_t best_i = 0xFF;
            for (uint8_t i = 0; i < bal_series; i++) {
                if (pack->cell_mv[i] > best_v && !(mask & (1u << i))) {
                    best_v = pack->cell_mv[i]; best_i = i;
                }
            }
            if (best_i != 0xFF) mask |= (1u << best_i);
        }
    } else {
        /* 定时均衡: 压差触发后, 高于均值+阈值的串全部开启, 由超时保护统一停止 */
        for (uint8_t i = 0; i < bal_series; i++) {
            if (pack->cell_mv[i] > (uint16_t)(avg_mv + BALANCE_THRESHOLD_MV)) {
                mask |= (1u << i);
            }
        }
    }

    /* ====== 按模式写入均衡控制 ======
     * PASSIVE: LTC6804 内部放电 MOS(电阻耗散, <100mA)
     * ACTIVE : 主动均衡驱动(PWM 调流, 能量转移, 需外接电路)
     * OFF    : 关闭均衡 */
    if (mode == BALANCE_MODE_ACTIVE) {
#if HW_ENABLE_ACTIVE_BALANCE
        bsp_active_balance_set(mask, ACTIVE_BAL_CURRENT_MA);
#else
        ESP_LOGW(TAG, "主动均衡硬件未接线(HW_ENABLE_ACTIVE_BALANCE=0), 忽略");
        return 0;
#endif
    } else if (mode == BALANCE_MODE_PASSIVE) {
        /* 被动均衡写入: 写入掩码到当前启用的 AFE(忽略返回值, 失败不阻塞) */
#if HW_ENABLE_BQ76952
        bsp_bq76952_set_balance(mask);
#elif HW_ENABLE_LTC6804
        bsp_ltc6804_set_balance(mask);
#endif
    } else {
        /* BALANCE_MODE_OFF: 显式关闭 */
        if (s_balance_active) {
            BALANCE_STOP();
            s_balance_active = false;
        }
        return 0;
    }

    /* ====== 更新均衡状态记录 ====== */
    if (mask != 0 && !s_balance_active) {
        /* 首次开启均衡, 记录开始时间 */
        s_balance_start_ms = now_ms;
        s_balance_active   = true;
        ESP_LOGI(TAG, "均衡启动 mode=%d mask=0x%08X avg=%umV diff=%umV",
                 (int)mode, (unsigned)mask, avg_mv, diff_mv);
    } else if (mask == 0 && s_balance_active) {
        /* 均衡全部停止 */
        s_balance_active = false;
    } else if (mask != 0) {
        /* 均衡持续中, 每 60 秒打印一次状态(避免日志刷屏) */
        if (now_ms - s_last_check_ms > 60000) {
            uint32_t elapsed_min = (now_ms - s_balance_start_ms) / 60000;
            ESP_LOGI(TAG, "均衡中 mode=%d mask=0x%08X 已运行%lumin",
                     (int)mode, (unsigned)mask, (unsigned long)elapsed_min);
            s_last_check_ms = now_ms;
        }
    }

    return mask;
}
