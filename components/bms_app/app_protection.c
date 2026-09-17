/**
 * @file    app_protection.c
 * @brief   五级保护逻辑实现
 * @author  BMS Team
 * @date    2026-08
 * @note    检查过压/欠压/过流/过温/热失控, 输出故障掩码
 *          含恢复滞回(避免临界抖动), 热失控锁定需人工复位
 */
#include "app_protection.h"
#include "bms_config.h"
#include "sys_data.h"
#include "sys_params.h"
#include "bsp_relay.h"
#include "bsp_current.h"
#include "bsp_insulation.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "APP_PROT";

/* 热失控监测: 低通滤波 + 时间窗端点微分 + 持续确认, 防单点噪声误锁
 * 原实现仅用相邻两点差分(100ms采样), 噪声被放大约600倍,
 * 单次0.2℃抖动即超 10℃/min 锁定阈值 -> 永久锁死(严重误报).
 * 现改为: 一阶低通滤波 + 约7s时间窗端点微分 + 持续超阈值2s才锁定. */
#define THERM_WIN_SAMPLES      8
#define THERM_PUSH_MS          1000                 // 每秒存一点, 8点≈7s窗口
#define THERM_WIN_MIN_MS       2000                 // 窗口不足2s不计算(避免初始两点放大)
#define DTDT_FILTER_ALPHA      0.20f
#define DV_PUSH_MS             500                  // dV/dt窗口短些(电压突变快), 8点≈3.5s
#define DV_WIN_MIN_MS          1500
#define DV_FILTER_ALPHA        0.30f
#define THERM_LOCK_SUSTAIN_MS  2000                 // 持续超阈值2s才锁定(防单点野值)

/* dT/dt 状态 */
static float   s_t_filt = 0.0f;
static int32_t s_t_ts[THERM_WIN_SAMPLES];
static float  s_t_vc[THERM_WIN_SAMPLES];
static uint8_t s_t_wi = 0;
static uint8_t s_t_cnt = 0;
static bool   s_t_inited = false;
static uint32_t s_t_last_push = 0;
static uint32_t s_dtdt_lock_since = 0;              // 热失控锁定持续计时

/* dV/dt 状态(设计文档 4.1.3) */
static float   s_v_filt = 0.0f;
static int32_t s_v_ts[THERM_WIN_SAMPLES];
static float  s_v_vc[THERM_WIN_SAMPLES];
static uint8_t s_v_wi = 0;
static uint8_t s_v_cnt = 0;
static bool   s_v_inited = false;
static uint32_t s_v_last_push = 0;
static uint32_t s_dvdt_lock_since = 0;
static bool     s_dvdt_warn_active;                 // dV/dt 预警状态(恢复滞回)

/* 故障状态记录(用于恢复滞回判断) */
static bool s_ov_prot_active;
static bool s_uv_prot_active;
static bool s_chg_oc_prot_active;
static bool s_dsg_oc_prot_active;
static bool s_ot_prot_active;
static bool s_ut_prot_active;                         // 低温保护状态
static bool s_ut_warn_active;                         // 低温预警状态
static bool s_dv_warn_active;                         // 压差过大预警状态
static bool s_low_soc_warn_active;                    // 低SOC预警状态
static bool s_dtdt_warn_active;                       // 温升速率预警状态

/* 持续过载计时器(单位 ms) */
static uint32_t s_overload_start_ms;                  // 过载开始时间戳(0=未过载)
static bool     s_overload_warn_active;               // 持续过载预警状态

/* ====== 内部函数声明 ====== */
static float calc_dtdt(const bms_pack_data_t *pack);
static float calc_dvdt(const bms_pack_data_t *pack);
static float calc_derating_factor(bms_fault_mask_t fault);

void app_protection_init(void)
{
    /* 热失控监测状态重置(滤波/窗口/持续计时) */
    s_t_filt = 0.0f;
    s_t_wi = 0;
    s_t_cnt = 0;
    s_t_inited = false;
    s_t_last_push = 0;
    s_dtdt_lock_since = 0;
    s_v_filt = 0.0f;
    s_v_wi = 0;
    s_v_cnt = 0;
    s_v_inited = false;
    s_v_last_push = 0;
    s_dvdt_lock_since = 0;
    s_dvdt_warn_active = false;
    s_ov_prot_active = false;
    s_uv_prot_active = false;
    s_chg_oc_prot_active = false;
    s_dsg_oc_prot_active = false;
    s_ot_prot_active = false;
    s_ut_prot_active = false;
    s_ut_warn_active = false;
    s_dv_warn_active = false;
    s_low_soc_warn_active = false;
    s_dtdt_warn_active = false;
    s_overload_start_ms = 0;
    s_overload_warn_active = false;
    ESP_LOGI(TAG, "protection init (10 种预警 + 6 种保护 + 2 种严重 + dV/dt 热失控检测)");
}

/* 计算热失控 dT/dt(单位 ℃/min)
 * 改进: 0.1℃量化 + 100ms采样下相邻两点差分会把噪声放大约600倍,
 *       现用一阶低通滤波抑制单点抖动, 并用约7s时间窗端点微分,
 *       窗口不足时不计算, 由调用方做"持续超阈值才锁定"二次确认. */
static float calc_dtdt(const bms_pack_data_t *pack)
{
    if (pack == NULL) {
        return 0.0f;
    }
    float t_c = (float)pack->temp_max_dc / 10.0f;   // 0.1℃ -> ℃
    int32_t now = (int32_t)pack->timestamp_ms;

    if (!s_t_inited) {
        s_t_filt = t_c;
        s_t_inited = true;
        s_t_last_push = (uint32_t)now;
    } else {
        s_t_filt += DTDT_FILTER_ALPHA * (t_c - s_t_filt);
    }

    /* 每秒落一点到环形缓冲(避免缓冲过大) */
    if ((uint32_t)now - s_t_last_push >= THERM_PUSH_MS) {
        s_t_ts[s_t_wi] = now;
        s_t_vc[s_t_wi] = s_t_filt;
        s_t_wi = (s_t_wi + 1) % THERM_WIN_SAMPLES;
        if (s_t_cnt < THERM_WIN_SAMPLES) s_t_cnt++;
        s_t_last_push = (uint32_t)now;
    }

    /* 取窗口内(跨度合理)最旧样本做端点微分 */
    int32_t oldest_ts = now;
    float oldest_v = s_t_filt;
    for (uint8_t i = 0; i < THERM_WIN_SAMPLES; i++) {
        int32_t dt = now - s_t_ts[i];
        if (dt >= 0
            && (uint32_t)dt <= (uint32_t)(THERM_PUSH_MS * THERM_WIN_SAMPLES)
            && s_t_ts[i] < oldest_ts) {
            oldest_ts = s_t_ts[i];
            oldest_v = s_t_vc[i];
        }
    }

    int32_t span = now - oldest_ts;
    if (span < THERM_WIN_MIN_MS) {
        return 0.0f;                                // 窗口不足, 不计算
    }
    return (s_t_filt - oldest_v) / ((float)span / 60000.0f);   // ℃/min
}

/* 计算热失控 dV/dt(单位 mV/s, 负值=电压下降)
 * 设计文档 4.1.3: dV/dt < -50mV/s 为异常(内部短路导致电压快速下降)
 * 改进: 同 dT/dt, 用低通滤波 + 约3.5s窗口端点微分 + 持续确认, 防误报锁死. */
static float calc_dvdt(const bms_pack_data_t *pack)
{
    if (pack == NULL) {
        return 0.0f;
    }
    float v_mv = (float)pack->cell_mv_max;          // 最高单体电压, mV
    int32_t now = (int32_t)pack->timestamp_ms;

    if (!s_v_inited) {
        s_v_filt = v_mv;
        s_v_inited = true;
        s_v_last_push = (uint32_t)now;
    } else {
        s_v_filt += DV_FILTER_ALPHA * (v_mv - s_v_filt);
    }

    if ((uint32_t)now - s_v_last_push >= DV_PUSH_MS) {
        s_v_ts[s_v_wi] = now;
        s_v_vc[s_v_wi] = s_v_filt;
        s_v_wi = (s_v_wi + 1) % THERM_WIN_SAMPLES;
        if (s_v_cnt < THERM_WIN_SAMPLES) s_v_cnt++;
        s_v_last_push = (uint32_t)now;
    }

    int32_t oldest_ts = now;
    float oldest_v = s_v_filt;
    for (uint8_t i = 0; i < THERM_WIN_SAMPLES; i++) {
        int32_t dt = now - s_v_ts[i];
        if (dt >= 0
            && (uint32_t)dt <= (uint32_t)(DV_PUSH_MS * THERM_WIN_SAMPLES)
            && s_v_ts[i] < oldest_ts) {
            oldest_ts = s_v_ts[i];
            oldest_v = s_v_vc[i];
        }
    }

    int32_t span = now - oldest_ts;
    if (span < DV_WIN_MIN_MS) {
        return 0.0f;
    }
    return (s_v_filt - oldest_v) / ((float)span / 1000.0f);   // mV/s
}

/**
 * @brief   根据故障掩码计算降额限流因子(0.0~1.0)
 * @param   fault  当前故障掩码
 * @return  降额因子, 1.0=满功率, 取最严格(最小)者
 * @note    工业级预警原则: 预警级不切断主回路, 仅降额限流
 *          多个预警同时存在时, 取最小因子(最保守)
 */
static float calc_derating_factor(bms_fault_mask_t fault)
{
    float factor = 1.0f;                                // 默认满功率

    /* 逐项检查预警, 取最小因子 */
    if (fault & FAULT_DTDT_WARN) {
        if (DERATING_DTDT < factor) factor = DERATING_DTDT;
    }
    if (fault & FAULT_OVERLOAD_WARN) {
        if (DERATING_OVERLOAD < factor) factor = DERATING_OVERLOAD;
    }
    if (fault & FAULT_UT_WARN) {
        if (DERATING_UT_WARN < factor) factor = DERATING_UT_WARN;
    }
    if (fault & FAULT_LOW_SOC_WARN) {
        if (DERATING_LOW_SOC < factor) factor = DERATING_LOW_SOC;
    }
    if (fault & FAULT_OT_WARN) {
        if (DERATING_OT_WARN < factor) factor = DERATING_OT_WARN;
    }
    if (fault & FAULT_CELL_DV_WARN) {
        if (DERATING_CELL_DV < factor) factor = DERATING_CELL_DV;
    }

    /* 保护级及以上直接限流到 0(已断继电器) */
    if (fault & (FAULT_CELL_OV_PROT | FAULT_CELL_UV_PROT |
                 FAULT_CHG_OC_PROT  | FAULT_DSG_OC_PROT  |
                 FAULT_OT_PROT      | FAULT_UT_PROT      |
                 FAULT_THERMAL_RUN  | FAULT_SHORT)) {
        factor = 0.0f;
    }

    return factor;
}

bms_fault_mask_t app_protection_process(const bms_pack_data_t *pack)
{
    if (pack == NULL) {
        return FAULT_NONE;
    }

    /* v7: 网页可下发的阈值/参数实时生效 —— 从 NVS 参数区读取, 而非编译期宏 */
    const bms_params_t *P = sys_params_get();

    bms_fault_mask_t fault = FAULT_NONE;

    /* ====== 热失控锁定检查(最高优先级, 不可自动恢复) ======
     * 注: 蜂鸣器/LED 报警由 app_alarm 模块统一管理, 此处只断继电器 */
    if (sys_data_get_thermal_lock()) {
        bsp_relay_cut_all();
        sys_data_set_relay(false, false);
        return FAULT_THERMAL_RUN;
    }

    /* ====== 电流采样失效检查(严重级, ADC读数全异常) ======
     * 采样失效时电流返回0, 无法监测充放电, 视为严重异常
     * 触发后 LED 常亮最大亮度, 蜂鸣器急促鸣叫 */
#if HW_ENABLE_CURRENT_SENSE
    if (!bsp_current_is_valid()) {
        fault |= FAULT_SAMPLE_FAIL;
    }
#endif

    /* ====== 单体过压/欠压(带恢复滞回) ======
     * 数据有效性判断: LTC6804 未启用时 cell_mv 全 0, 跳过电压类保护
     * 有效条件: cell_mv_max > 0 且 cell_mv_min > 0(均为 0 说明未采集)
     * 2026-08-19 修复(F5): AFE 已启用但采集时间戳有效且电压全 0(读取失败)时,
     *   置 FAULT_SAMPLE_FAIL 而非静默跳过 —— 原实现 AFE 读取失败时电压类保护
     *   被静默跳过, 过压/欠压/热失控全部失效, 且无任何告警. */
#if HW_ENABLE_BQ76952 || HW_ENABLE_LTC6804
    if (pack->timestamp_ms > 0 &&
        pack->cell_mv_max == 0 && pack->cell_mv_min == 0) {
        fault |= FAULT_SAMPLE_FAIL;                  /* AFE 读取失败 → 采样失效 */
    }
#endif
    if (pack->cell_mv_max > 0 && pack->cell_mv_min > 0) {

        /* ====== 2026-08-08 功能安全 S1c: 传感器合理性检查 + 双通道校验 ======
         * 合理性: 单体电压超出物理范围(0.5V~5.5V)标记采样失效;
         * 双通道: 过压判定同时用 cell_mv_max 与 pack_mv/串数 换算交叉验证,
         *         两通道不一致(偏差>500mV)视为单通道故障, 报 FAULT_COMM_WARN. */
        if (pack->cell_mv_max > 5500 || pack->cell_mv_max < 500 ||
            pack->cell_mv_min > 5500 || pack->cell_mv_min < 500) {
            fault |= FAULT_SAMPLE_FAIL;              /* 超出物理范围 -> 采样失效 */
        }
        {
            uint8_t _sn = (uint8_t)(P->cell_series_num);
            if (_sn < 1 || _sn > BMS_HW_MAX_SERIES_NUM) _sn = BMS_CELL_SERIES_NUM;
            uint32_t _sum_check = (uint32_t)pack->pack_mv;
            uint32_t _sum_max = (uint32_t)pack->cell_mv_max * _sn;
            /* 双通道交叉验证: 总压应 ≈ 单体最高×串数; 偏差>500mV 且总压有效时判单通道故障 */
            if (_sum_check > 0 && _sum_max > 0) {
                int32_t _dev = (int32_t)(_sum_max) - (int32_t)_sum_check;
                if (_dev > 500 || _dev < -500) {
                    fault |= FAULT_COMM_WARN;        /* 双通道不一致 */
                }
            }
        }

        /* 单体过压 */
        if (pack->cell_mv_max >= P->cell_ov_prot_mv) {
            fault |= FAULT_CELL_OV_PROT;
            s_ov_prot_active = true;
        } else if (pack->cell_mv_max >= P->cell_ov_warn_mv) {
            fault |= FAULT_CELL_OV_WARN;
        } else if (s_ov_prot_active && pack->cell_mv_max <= P->cell_ov_recover_mv) {
            s_ov_prot_active = false;                    // 恢复
        } else if (s_ov_prot_active) {
            fault |= FAULT_CELL_OV_PROT;                // 未恢复保持保护
        }

        /* 单体欠压 */
        if (pack->cell_mv_min <= P->cell_uv_prot_mv) {
            fault |= FAULT_CELL_UV_PROT;
            s_uv_prot_active = true;
        } else if (pack->cell_mv_min <= P->cell_uv_warn_mv) {
            fault |= FAULT_CELL_UV_WARN;
        } else if (s_uv_prot_active && pack->cell_mv_min >= P->cell_uv_recover_mv) {
            s_uv_prot_active = false;
        } else if (s_uv_prot_active) {
            fault |= FAULT_CELL_UV_PROT;
        }
    }

    /* ====== 充电过流(负电流, 单位 mA) ======
     * 注意: 原 CHG_OC_PROT_MA=500 代表 5A(0.01A单位), 但 current_ma 单位是 mA
     *       0.5A 即触发, 已修正 bms_config.h 为 5000 (直接 mA 单位)
     * 恢复条件: 充电电流绝对值低于预警阈值(含零点偏移容差) */
    if (-pack->current_ma >= P->chg_oc_prot_ma) {
        fault |= FAULT_CHG_OC_PROT;
        s_chg_oc_prot_active = true;
    } else if (-pack->current_ma >= P->chg_oc_warn_ma) {
        fault |= FAULT_CHG_OC_WARN;
    } else if (s_chg_oc_prot_active) {
        s_chg_oc_prot_active = false;                // 低于预警阈值即恢复(含零点偏移容差)
    }

    /* ====== 放电过流(正电流, 单位 mA) ======
     * 注意: 原 DSG_OC_PROT_MA=1000 代表 10A(0.01A单位), 1A 即触发, 已修正
     * 恢复条件: 放电电流低于预警阈值(含零点偏移容差) */
    if (pack->current_ma >= (int16_t)P->dsg_oc_prot_ma) {
        fault |= FAULT_DSG_OC_PROT;
        s_dsg_oc_prot_active = true;
    } else if (pack->current_ma >= (int16_t)P->dsg_oc_warn_ma) {
        fault |= FAULT_DSG_OC_WARN;
    } else if (s_dsg_oc_prot_active) {
        s_dsg_oc_prot_active = false;                // 低于预警阈值即恢复(含零点偏移容差)
    }

    /* ====== 短路保护(电流绝对值超阈值, 严重故障, 全断) ====== */
    int16_t abs_current = (pack->current_ma >= 0)
                          ? pack->current_ma : (int16_t)(-pack->current_ma);
    if (abs_current >= (int16_t)SHORT_PROT_MA) {
        fault |= FAULT_SHORT;
    }

    /* ====== 过温(带恢复滞回) ====== */
    if (pack->temp_max_dc >= P->temp_ot_prot_dc) {
        fault |= FAULT_OT_PROT;
        s_ot_prot_active = true;
    } else if (pack->temp_max_dc >= P->temp_ot_warn_dc) {
        fault |= FAULT_OT_WARN;
    } else if (s_ot_prot_active && pack->temp_max_dc <= P->temp_ot_recover_dc) {
        s_ot_prot_active = false;
    } else if (s_ot_prot_active) {
        fault |= FAULT_OT_PROT;
    }

    /* ====== 低温保护(带恢复滞回, 充电禁止) ====== */
    if (pack->temp_min_dc <= P->temp_ut_prot_dc) {
        fault |= FAULT_UT_PROT;
        s_ut_prot_active = true;
    } else if (s_ut_prot_active && pack->temp_min_dc >= P->temp_ut_recover_dc) {
        s_ut_prot_active = false;
    } else if (s_ut_prot_active) {
        fault |= FAULT_UT_PROT;
    }

    /* ====== 低温预警(带恢复滞回, 充电限流) ======
     * 触发: T <= -10℃ / 恢复: T > 0℃ (迟滞 10℃) */
    if (pack->temp_min_dc <= P->temp_ut_warn_dc) {
        fault |= FAULT_UT_WARN;
        s_ut_warn_active = true;
    } else if (s_ut_warn_active && pack->temp_min_dc >= P->temp_ut_recover_dc) {
        s_ut_warn_active = false;
    } else if (s_ut_warn_active) {
        fault |= FAULT_UT_WARN;
    }

    /* ====== 温升速率预警 + 热失控报警(dT/dt 单位 ℃/min) ======
     * 三级: dT/dt > 2℃/min 预警 / > 3℃/min 热失控预警 / > 10℃/min 热失控报警(锁定)
     * 改进: 经滤波+时间窗后斜率已稳定; 锁定需"持续超阈值 THERM_LOCK_SUSTAIN_MS"
     *       才生效, 进一步杜绝单点野值导致永久锁死(需人工复位). */
    float dtdt = calc_dtdt(pack);
    if (dtdt > TEMP_THERMAL_RUNAWAY_ALARM) {
        if (s_dtdt_lock_since == 0) {
            s_dtdt_lock_since = pack->timestamp_ms;
        } else if ((uint32_t)(pack->timestamp_ms - s_dtdt_lock_since) >= THERM_LOCK_SUSTAIN_MS) {
            fault |= FAULT_THERMAL_RUN;
            sys_data_set_thermal_lock(true);         // 锁定, 需人工复位
        }
    } else {
        s_dtdt_lock_since = 0;
    }
    /* 预警级(未达锁定)按稳定斜率判定, 不再噪声触发 */
    if (!(dtdt > TEMP_THERMAL_RUNAWAY_ALARM)) {
        if (dtdt > TEMP_THERMAL_RUNAWAY_WARN) {
            fault |= FAULT_OT_WARN;                  // 热失控预警合并到过温预警
        } else if (dtdt > P->temp_dtdt_warn) {
            fault |= FAULT_DTDT_WARN;                // 温升速率预警(热失控前兆)
            s_dtdt_warn_active = true;
        } else if (s_dtdt_warn_active && dtdt < P->temp_dtdt_warn) {
            s_dtdt_warn_active = false;              // 温升速率恢复
        }
    }

    /* ====== 电压降速率预警 + 热失控报警(dV/dt 单位 mV/s, 设计文档 4.1.3) ======
     * 热失控三要素之一: dV/dt < -50mV/s 预警 / < -100mV/s 报警(锁定)
     * 原理: 热失控内部短路导致单体电压快速下降
     * 改进: 同 dT/dt, 锁定需持续超阈值确认, 防单点噪声误锁死. */
    float dvdt = calc_dvdt(pack);
    if (dvdt < DV_DT_ALARM_MVPS) {
        if (s_dvdt_lock_since == 0) {
            s_dvdt_lock_since = pack->timestamp_ms;
        } else if ((uint32_t)(pack->timestamp_ms - s_dvdt_lock_since) >= THERM_LOCK_SUSTAIN_MS) {
            fault |= FAULT_THERMAL_RUN;              // 严重电压下降, 热失控报警
            sys_data_set_thermal_lock(true);         // 锁定, 需人工复位
        }
    } else {
        s_dvdt_lock_since = 0;
    }
    if (!(dvdt < DV_DT_ALARM_MVPS)) {
        if (dvdt < P->dv_dt_warn_mvps) {
            fault |= FAULT_DTDT_WARN;                // 电压降速率预警(合并到热失控前兆)
            s_dvdt_warn_active = true;
        } else if (s_dvdt_warn_active && dvdt >= 0.0f) {
            s_dvdt_warn_active = false;              // 电压恢复稳定
        }
    }

    /* ====== 单体压差过大预警(带恢复滞回) ======
     * 触发: ΔV >= 100mV / 恢复: ΔV < 60mV (迟滞 40mV) */
    /* B5 修复: 无符号减法下溢防护(若采集与保护任务非原子导致 min>max,
     * 避免回绕成大数误报压差过大) */
    uint16_t cell_dv = (pack->cell_mv_max > pack->cell_mv_min)
                       ? (uint16_t)(pack->cell_mv_max - pack->cell_mv_min)
                       : (uint16_t)(pack->cell_mv_min - pack->cell_mv_max);
    if (cell_dv >= P->cell_dv_warn_mv) {
        fault |= FAULT_CELL_DV_WARN;
        s_dv_warn_active = true;
    } else if (s_dv_warn_active && cell_dv <= P->cell_dv_recover_mv) {
        s_dv_warn_active = false;
    } else if (s_dv_warn_active) {
        fault |= FAULT_CELL_DV_WARN;
    }

    /* ====== 持续过载预警(>= 1.05 倍额定, 持续 > 2s) ======
     * H11 修复: 原区间限定 1.05~1.30 倍, 注释声称"超过 1.30 倍由 DSG_OC_PROT 保护",
     * 但 DSG_OC_PROT_MA=10A=4 倍额定(2.5A), 与 1.30 倍(3.25A)不衔接,
     * 导致 3.25A~10A 区间既不预警也不保护 → 安全空档.
     * 现改为: ratio >= 1.05 倍即纳入累计(含 1.30 倍以上区间),
     * 持续超 2s 触发降额预警; 达到 DSG_OC_PROT(10A) 时由放电过流硬保护兜底. */
    {
        uint16_t abs_i = (pack->current_ma >= 0)
                         ? (uint16_t)pack->current_ma
                         : (uint16_t)(-pack->current_ma);
        float ratio = (float)abs_i / (float)(P->rated_current_ma > 0 ? P->rated_current_ma : 1);

        if (ratio >= P->overload_warn_ratio) {
            /* 在过载区间内, 累计时间 */
            if (s_overload_start_ms == 0) {
                s_overload_start_ms = pack->timestamp_ms;
            }
            uint32_t duration = pack->timestamp_ms - s_overload_start_ms;
            if (duration >= OVERLOAD_DURATION_MS) {
                fault |= FAULT_OVERLOAD_WARN;        // 持续过载超 2s, 触发预警
                s_overload_warn_active = true;
            }
        } else {
            /* 不在过载区间, 重置计时器 */
            s_overload_start_ms = 0;
            s_overload_warn_active = false;
        }
    }

    /* ====== 低 SOC 预警(带恢复滞回) ======
     * 触发: SOC <= 15% / 恢复: SOC > 20% (迟滞 5%) */
    {
        bms_soc_data_t soc;
        sys_data_get_soc(&soc);
        uint8_t soc_pct = (uint8_t)(soc.soc * 100.0f);

        if (soc_pct <= P->soc_low_warn_pct) {
            fault |= FAULT_LOW_SOC_WARN;
            s_low_soc_warn_active = true;
        } else if (s_low_soc_warn_active && soc_pct >= P->soc_low_recover_pct) {
            s_low_soc_warn_active = false;
        } else if (s_low_soc_warn_active) {
            fault |= FAULT_LOW_SOC_WARN;
        }

        /* ====== SOH 衰减预警(SOH <= 80%) ======
         * 数据有效性: SOH 初始为 0(未估算), 跳过避免误报 */
        if (soc.soh > 0.0f) {
            uint8_t soh_pct = (uint8_t)(soc.soh * 100.0f);
            if (soh_pct <= P->soh_low_warn_pct) {
                fault |= FAULT_LOW_SOH_WARN;
            }
        }
    }

    /* ====== 绝缘电阻预警/保护(2026-08-09, 对标 GB/T 38661 / GB/T 18384.1) ======
     * 判据: 综合绝缘电阻率 Ω/V = min(Rp,Rn)/总压
     *   < 100Ω/V 预警(GB/T 18384.1 直流下限) / < 50Ω/V 保护(切断主回路)
     * 数据有效性: insulation_ohm_per_v>0(硬件未接/未启用时为 0, 跳过) */
    if (pack->insulation_ohm_per_v > 0) {
        if (pack->insulation_ohm_per_v < INSULATION_PROT_OHM_PER_V) {
            fault |= FAULT_INSULATION_PROT;            /* 严重: 断继电器由上层处理 */
        } else if (pack->insulation_ohm_per_v < INSULATION_WARN_OHM_PER_V) {
            fault |= FAULT_INSULATION_WARN;            /* 预警 */
        }
        ESP_LOGI(TAG, "[绝缘] Rp=%luΩ Rn=%luΩ %.1fΩ/V %s",
                 (unsigned long)pack->insulation_rp_ohm,
                 (unsigned long)pack->insulation_rn_ohm,
                 (float)pack->insulation_ohm_per_v,
                 (fault & FAULT_INSULATION_PROT) ? "PROT" :
                 (fault & FAULT_INSULATION_WARN) ? "WARN" : "OK");
    }

    /* ====== 继电器控制(根据故障类型 + 充电模式独立控制充放电) ======
     * 充电继电器控制优先级:
     *   1. 严重故障(过温/热失控/短路) → 全断
     *   2. 充电类故障(过压/充电过流/低温) → 断充电
     *   3. 充电模式 FULL(已充满) → 断充电
     *   4. 正常 → 闭合
     * 放电继电器控制优先级:
     *   1. 严重故障 → 全断
     *   2. 放电类故障(欠压/放电过流) → 断放电
     *   3. 正常 → 闭合 */
    bool chg_on = true;                              // 默认闭合
    bool dsg_on = true;

    /* 充电类故障(含低温)断充电 */
    if (fault & (FAULT_CELL_OV_PROT | FAULT_CHG_OC_PROT | FAULT_UT_PROT)) {
        chg_on = false;
    }
    /* 放电类故障断放电 */
    if (fault & (FAULT_CELL_UV_PROT | FAULT_DSG_OC_PROT)) {
        dsg_on = false;
    }
    /* 严重故障全断(含绝缘保护: <50Ω/V 切断主回路, 对标 GB/T 18384.1)
     * H12 修复: 原漏掉 FAULT_INSULATION_PROT, 绝缘保护"只置位不动作"
     * 2026-08-19 修复(F4): 电流采样失效(FAULT_SAMPLE_FAIL)纳入全断——
     *   原实现采样失效时电流恒为 0, 过流/短路保护静默失效但主回路仍闭合. */
    if (fault & (FAULT_OT_PROT | FAULT_THERMAL_RUN | FAULT_SHORT | FAULT_INSULATION_PROT | FAULT_SAMPLE_FAIL)) {
        chg_on = false;
        dsg_on = false;
    }

    /* ====== 充电模式联动(非故障态下的充电继电器控制) ======
     * 充电模式 FULL 时断开充电继电器(已充满, 不再充电)
     * 充电模式 STOP 时断开充电继电器(SOC 无效或异常)
     * 仅在无充电类故障时生效(故障优先级高于充电策略) */
    if (chg_on) {
        bms_charge_mode_e chg_mode = sys_data_get_charge_mode();
        if (chg_mode == CHARGE_MODE_FULL || chg_mode == CHARGE_MODE_STOP) {
            chg_on = false;                          // 已充满或停止, 断充电
        }
    }

    /* ====== 远程控制覆盖检测 ======
     * 云端下发 set_charge/set_discharge/set_relay 后 30s 内,
     * 保护任务不覆盖继电器状态(故障保护除外, 故障始终优先)
     * 无故障时保留远程命令设置的继电器状态 */
    bool has_protection_fault = (fault & (FAULT_CELL_OV_PROT | FAULT_CELL_UV_PROT |
                                          FAULT_CHG_OC_PROT | FAULT_DSG_OC_PROT |
                                          FAULT_OT_PROT | FAULT_UT_PROT |
                                          FAULT_THERMAL_RUN | FAULT_SHORT |
                                          FAULT_INSULATION_PROT));
    if (sys_data_get_remote_override() && !has_protection_fault) {
        /* 远程控制模式: 保留当前继电器状态, 不覆盖 */
        sys_data_get_relay(&chg_on, &dsg_on);
    }

    /* ====== 主电源开关守卫(2026-09-09) ======
     * SW_PWR 长按 3s 关功率后, 即使 override 30s 超时,
     * 保护任务也保持全断, 直到云端下发开启命令清除标志.
     * 严重故障优先级更高(故障态强制全断, 行为不变) */
    if (!has_protection_fault && sys_data_get_master_power_off()) {
        chg_on = false;
        dsg_on = false;
    }

    bsp_relay_set_charge(chg_on);
    bsp_relay_set_discharge(dsg_on);
    sys_data_set_relay(chg_on, dsg_on);

    /* ====== 2026-08-08 功能安全 S1a: 继电器状态回读确认(SIL2 诊断覆盖) ======
     * 驱动失效检测: 命令输出后回读实际电平, 与命令不一致 -> 报 FAULT_COMM_WARN.
     * 仅当继电器硬件启用且回读成功时执行(屏蔽时跳过). */
#if HW_ENABLE_RELAY
    {
        bool rb_chg = false, rb_dsg = false;
        if (bsp_relay_readback(&rb_chg, &rb_dsg)) {
            if (rb_chg != chg_on || rb_dsg != dsg_on) {
                ESP_LOGW(TAG, "[SIL2] 继电器回读不一致 cmd=(chg=%d dsg=%d) rb=(chg=%d dsg=%d)",
                         chg_on ? 1 : 0, dsg_on ? 1 : 0, rb_chg ? 1 : 0, rb_dsg ? 1 : 0);
                fault |= FAULT_COMM_WARN;           /* 驱动失效 -> 预警 */
            }
        }
    }
#endif

    /* ====== 降额限流因子计算(工业级预警核心: 不切断主回路, 降额限流) ======
     * 预警级故障不断继电器, 但通过降额因子限制最大允许电流
     * 上游设备(CAN 总线)和本地充电策略根据此因子调整功率 */
    float derating = calc_derating_factor(fault);
    sys_data_set_derating(derating);

    /* 注: 蜂鸣器/LED 报警由 app_alarm 模块统一管理
     * 此处只返回故障掩码, 由上层任务写入 sys_data 供 app_alarm 读取 */

    return fault;
}
