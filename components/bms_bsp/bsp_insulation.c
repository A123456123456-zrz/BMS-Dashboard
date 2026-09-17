/**
 * @file    bsp_insulation.c
 * @brief   绝缘检测驱动实现(不平衡电桥法)
 * @author  BMS Team
 * @date    2026-08
 * @note    对标 GB/T 38661 / GB/T 18384.1(直流绝缘电阻 >100Ω/V)
 * 2026-08-09: 软件框架 + 算法就绪, 硬件预留(HW_ENABLE_INSULATION_DETECT=0)
 * 2026-08-13: 补充真实 ADC 采样骨架(切 GPIO24→稳定→ADC1_CH6 两次采样); 仍 HW_ENABLE=0 不编译
 *
 * 不平衡电桥法原理:
 *   正极母线 --R0--+--(ADC1 测对地电压)  +--R0-- 负极母线
 *                  |                     |
 *                  +== Rp(对地)          +== Rn(对地)
 *   第一次: 上下桥臂均 R0, 测 Vc1;
 *   第二次: 正极桥臂改 2*R0(并入第二颗), 测 Vc2;
 *   由两次测量联立解出 Rp(正极对地) / Rn(负极对地).
 *   推导(设 Ra=R0//Rp, Rb=R0//Rn):
 *     Vc1 = Vbat*Rb/(Ra+Rb)        ... (1)
 *     Vc2 = Vbat*Rn//R0 / (Rp//2R0 + Rn//R0) ... (2)
 *   代入并化简得解析解(见 bsp_insulation_solve 注释), 输出 Rp/Rn.
 */
#include "bsp_insulation.h"
#include "bms_config.h"
#include "bms_pinmap.h"
#include "esp_log.h"
#include <math.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"        // v5.2 曲线拟合校准方案
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP_INSULATION";

static bool s_inited = false;

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;
static bool                      s_cali_enable = false;

/* ESP32-S3 ADC1 通道映射: GPIO7 = ADC1_CH6
 * 注意: ADC_UNIT_1 与 bsp_current(GPIO1=ADC1_CH0) 共用同一 oneshot 单元,
 *       ESP-IDF 同一 unit_id 仅允许 adc_oneshot_new_unit 一次;
 *       若 HW_ENABLE_CURRENT_SENSE 与 HW_ENABLE_INSULATION_DETECT 同时置 1,
 *       需把两通道统一到一个共享单元(抽 bsp_adc_common 或复用 bsp_current 的 handle)。
 *       当前二者默认均为 0, 不会实际冲突。 */
#define INS_ADC_UNIT    ADC_UNIT_1
#define INS_ADC_CHAN    ADC_CHANNEL_6

bms_err_t bsp_insulation_init(void)
{
    /* 硬件屏蔽: 缺绝缘检测电路时直接返回, 不阻塞系统 */
    if (!HW_ENABLE_INSULATION_DETECT) {
        ESP_LOGW(TAG, "绝缘检测屏蔽 (HW_ENABLE_INSULATION_DETECT=0)");
        return BMS_ERR_NOT_INIT;
    }

    /* 桥臂切换控制 GPIO(高=Q1闭合 上臂R0, 低=Q1断开 上臂2R0, 默认状态2) */
#if HW_ENABLE_INSULATION_DETECT
    gpio_reset_pin(PIN_INSULATION_SW);
    gpio_set_direction(PIN_INSULATION_SW, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_INSULATION_SW, 0);
#endif

    /* ADC 单元(与 bsp_current 共用 ADC_UNIT_1, 见上方注释) */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = INS_ADC_UNIT,
    };
    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc unit init fail (ADC_UNIT_1 可能已被 bsp_current 占用): %s",
                 esp_err_to_name(ret));
        return BMS_ERR_ADC;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, INS_ADC_CHAN, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc chan config fail: %s", esp_err_to_name(ret));
        return BMS_ERR_ADC;
    }
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = INS_ADC_UNIT,
        .chan     = INS_ADC_CHAN,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    s_cali_enable = (ret == ESP_OK);
    if (!s_cali_enable) {
        ESP_LOGW(TAG, "adc cali unavailable, use raw reference");
    }

    s_inited = true;
    ESP_LOGI(TAG, "insulation init ok (R0=%luΩ, 阈值=%luΩ/V, ADC_CH=%d)",
             (unsigned long)INSULATION_BRIDGE_OHM,
             (unsigned long)INSULATION_WARN_OHM_PER_V, (int)INS_ADC_CHAN);
    return BMS_OK;
}

/* 采样电桥中点 Vmid(调理后的电压), 还原为真实 Vmid(mV, 与 Vbat 同量纲)供解算
 * q1_closed=true  -> 状态1(上臂R0)   -> 对应 vc1
 * q1_closed=false -> 状态2(上臂2R0)  -> 对应 vc2
 * 返回 false 表示采样失败(ADC 未初始化/全部异常), 上层据此返回 BMS_ERR_CRC */
static bool ins_sample_vmid(bool q1_closed, uint32_t *vmid_mv_out)
{
    if (s_adc_handle == NULL || vmid_mv_out == NULL) {
        return false;
    }

    /* 切换桥臂比例并等待调理电路(RC/运放)稳定 */
#if HW_ENABLE_INSULATION_DETECT
    gpio_set_level(PIN_INSULATION_SW, q1_closed ? 1 : 0);
#endif
    vTaskDelay(pdMS_TO_TICKS(INSULATION_SW_SETTLE_MS));

    /* 多次采样均值, 抑制 ADC 噪声 */
    uint32_t sum = 0;
    uint8_t  valid = 0;
    for (uint8_t i = 0; i < INSULATION_AVG_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, INS_ADC_CHAN, &raw) != ESP_OK) {
            continue;
        }
        if (raw < 10 || raw > 4085) {            /* 剔除饱和/异常点 */
            continue;
        }
        sum += (uint32_t)raw;
        valid++;
    }
    if (valid == 0) {
        return false;
    }
    uint16_t avg_raw = (uint16_t)(sum / valid);

    int voltage_mv = 0;
    if (s_cali_enable) {
        adc_cali_raw_to_voltage(s_cali_handle, avg_raw, &voltage_mv);
    } else {
        /* 无校准: 按参考电压线性换算 */
        voltage_mv = (int)avg_raw * INSULATION_ADC_VREF_MV / (1 << INSULATION_ADC_BITS);
    }

    /* 还原真实 Vmid: 调理分压比(实际 Vmid = 调理后电压 × DIV_RATIO)
     * 必须与电路 Rd1/Rd2 实现的分压比一致, 否则解算偏差;
     * 注意: 调理前端须把 Vmid 搬移到 0~3.3V 单端量程(如中心偏置),
     *       此处按"调理后电压已是 Vmid 的线性缩放"还原, 偏置/电平搬移由硬件保证 */
    float ratio = (float)INSULATION_DIV_RATIO_X10 / 10.0f;
    *vmid_mv_out = (uint32_t)((float)voltage_mv * ratio);
    return true;
}

bms_err_t bsp_insulation_read(uint32_t pack_v_mv, uint32_t *rp_ohm, uint32_t *rn_ohm)
{
    if (rp_ohm == NULL || rn_ohm == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    *rp_ohm = 0;
    *rn_ohm = 0;
    if (!s_inited) {
        return BMS_ERR_NOT_INIT;
    }

    /* 两次测量解算(替换原写死的 0):
     *   状态1: Q1闭合(上臂R0)   -> vc1_mv
     *   状态2: Q1断开(上臂2R0)  -> vc2_mv */
    uint32_t vc1_mv = 0, vc2_mv = 0;
    if (!ins_sample_vmid(true, &vc1_mv)) {
        return BMS_ERR_CRC;      /* 采样失败(未接线/ADC异常) */
    }
    if (!ins_sample_vmid(false, &vc2_mv)) {
        return BMS_ERR_CRC;
    }

    bool ok = bsp_insulation_solve(pack_v_mv, vc1_mv, vc2_mv,
                                   INSULATION_BRIDGE_OHM, rp_ohm, rn_ohm);
    if (!ok) {
        return BMS_ERR_CRC;   /* 测量无效(未接线/电压为0) */
    }
    return BMS_OK;
}

/* ====== 不平衡电桥解析解 ======
 * 设 R0 = 桥臂基础电阻
 *  第一次(上 R0 下 R0): Vc1 = Vbat * (R0//Rn) / ((R0//Rp) + (R0//Rn))
 *  第二次(上 2R0 下 R0): Vc2 = Vbat * (R0//Rn) / ((2R0//Rp) + (R0//Rn))
 * 定义  A = R0//Rp = R0*Rp/(R0+Rp)        (第一次上臂等效)
 *        B = R0//Rn = R0*Rn/(R0+Rn)        (两次下臂相同)
 *        C = 2R0//Rp = 2R0*Rp/(2R0+Rp)     (第二次上臂等效)
 * 由 Vc1: Vc1*(A+B) = Vbat*B  -> A = B*(Vbat-Vc1)/Vc1   ... (a)
 * 由 Vc2: Vc2*(C+B) = Vbat*B  -> C = B*(Vbat-Vc2)/Vc2   ... (b)
 * 又 A = R0*Rp/(R0+Rp)  => Rp = A*R0/(R0-A)             ... (c)
 *    C = 2R0*Rp/(2R0+Rp) => Rp = C*2R0/(2R0-C)          ... (d)
 * 联立 (c)(d): A*R0/(R0-A) = C*2R0/(2R0-C)
 *  => A*(2R0-C) = 2C*(R0-A)
 *  => 2AR0 - AC = 2CR0 - 2AC
 *  => A(2R0 + C) = 2CR0
 *  => A = 2CR0/(2R0+C)                                    ... (e)
 * 由 (a)(b): A = B*k1, C = B*k2, 其中 k1=(Vbat-Vc1)/Vc1, k2=(Vbat-Vc2)/Vc2
 * 代入 (e): B*k1 = 2*(B*k2)*R0/(2R0+B*k2)
 *  => k1*(2R0+B*k2) = 2k2*R0
 *  => B = 2R0*(k2-k1)/(k1*k2)                             ... (f)
 * 回代: A = B*k1, C = B*k2
 * 再由 (c): Rp = A*R0/(R0-A); 由 B = R0*Rn/(R0+Rn) => Rn = B*R0/(R0-B)
 * 输出前做限幅: 0 < R < 100MΩ, 非法(分母<=0/电压无效)返回 false */
bool bsp_insulation_solve(uint32_t pack_v_mv, uint32_t vc1_mv, uint32_t vc2_mv,
                          uint32_t bridge_ohm, uint32_t *rp_ohm, uint32_t *rn_ohm)
{
    if (rp_ohm == NULL || rn_ohm == NULL || bridge_ohm == 0) {
        return false;
    }
    *rp_ohm = 0;
    *rn_ohm = 0;

    /* 测量有效性: 总压与两次对地电压必须 >0 且 <总压(桥臂分压不可能超过总压) */
    if (pack_v_mv == 0 || vc1_mv == 0 || vc2_mv == 0 ||
        vc1_mv >= pack_v_mv || vc2_mv >= pack_v_mv) {
        return false;
    }

    float Vbat = (float)pack_v_mv;
    float Vc1  = (float)vc1_mv;
    float Vc2  = (float)vc2_mv;
    float R0   = (float)bridge_ohm;

    float k1 = (Vbat - Vc1) / Vc1;
    float k2 = (Vbat - Vc2) / Vc2;
    if (k1 <= 0.0f || k2 <= 0.0f) {
        return false;
    }

    /* (f): B = 2R0*(k2-k1)/(k1*k2) —— 两次测量差必须非零(否则上下对称无法解) */
    float denom = k1 * k2;
    if (denom <= 1e-9f) {
        return false;
    }
    float B = 2.0f * R0 * (k2 - k1) / denom;
    if (B <= 0.0f || B >= R0) {
        return false;                    /* B 必须 (0, R0) 才物理有效 */
    }

    float A = B * k1;
    if (A <= 0.0f || A >= R0) {
        return false;
    }

    /* Rp = A*R0/(R0-A), Rn = B*R0/(R0-B) */
    float rp = A * R0 / (R0 - A);
    float rn = B * R0 / (R0 - B);

    /* 限幅: 上限 100MΩ(开路近似), 下限 100Ω */
    if (rp < 100.0f || rp > 100e6f || rn < 100.0f || rn > 100e6f) {
        return false;
    }

    *rp_ohm = (uint32_t)rp;
    *rn_ohm = (uint32_t)rn;
    return true;
}
