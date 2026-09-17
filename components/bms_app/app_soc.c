/**
 * @file    app_soc.c
 * @brief   SOC/SOH 估算实现(5 种可选算法: AEKF / 安时积分 / OCV查表 / MCC-EKF / UKF + SOH 内阻法)
 * @author  BMS Team
 * @date    2026-08
 * @note    二阶 RC 等效电路模型:
 *          状态 x=[SOC, V1, V2]
 *          SOC(k) = SOC(k-1) - I*dt/(3600*Q)
 *          V1(k)  = V1(k-1)*exp(-dt/tau1) + R1*(1-exp(-dt/tau1))*I
 *          V2(k)  = V2(k-1)*exp(-dt/tau2) + R2*(1-exp(-dt/tau2))*I
 *          观测 V = OCV(SOC) - I*R0 - V1 - V2
 *          Sage-Husa 自适应: 滑动窗口新息方差在线更新 R/Q
 *          Dual EKF: 在线辨识 R0/R1/R2
 *          SOH: (R_EOL - R0) / (R_EOL - R_new)
 *          算法可选(soc_algo=0..4): 0=AEKF(默认) 1=纯安时积分 2=OCV查表 3=MCC-EKF(抗野值) 4=UKF(无迹卡尔曼)
 *          无有效电压(V_meas=0)时自动退化为安时积分, 避免观测值拉低 SOC
 */
#include "app_soc.h"
#include "bms_config.h"
#include "sys_data.h"
#include "sys_params.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <math.h>
#include <string.h>

static const char *TAG = "APP_SOC";

/* ====== 电池模型参数(二阶 RC) ====== */
/* TAU1/TAU2/R_NEW_OHM/R_EOL_OHM 等参数已统一收纳至 bms_config.h:
 *   BATT_TAU1_SEC / BATT_TAU2_SEC / BATT_R_NEW_OHM / BATT_R_EOL_OHM
 *   BATT_R0_MIN_OHM / BATT_R0_MAX_OHM / AEKF_Q_SOC / AEKF_Q_VRC
 *   AEKF_R_MEAS / OCV_POLY_A..D, 业务代码禁止再硬编码 */
#define CELL_CAPACITY_AH       ((float)BMS_CELL_CAPACITY_MAH / 1000.0f)

/* ====== AEKF 状态量 ====== */
typedef struct {
    float x[3];                       // 状态 [SOC, V1, V2]
    float P[3][3];                    // 协方差矩阵
    float Q[3];                       // 过程噪声对角
    float R;                          // 观测噪声方差
    float innov_hist[AEKF_INNOV_WINDOW];   // 新息历史(滑动窗口)
    uint8_t innov_idx;
    uint8_t innov_cnt;
    float dt;                         // 采样周期, 单位 s
    bool  inited;
} aekf_state_t;

static aekf_state_t s_aekf;
static float        s_r0, s_r1, s_r2;  // Dual EKF 辨识参数
static float        s_soh;
static uint32_t     s_cycle_count = 0;               /* Bug6: 等效循环次数 */
static float        s_measured_discharge_ah = 0.0f;  /* 总累计放电安时(保留, 供参考) */
static float        s_discharge_ah = 0.0f;           /* #fix: 等效循环计数累计放电安时, 跨重启持久化(原函数内 static 重启归零, 导致 cycle_count 恢复后被清零) */
static float        s_soh_cap = -1.0f;               /* P1: 容量法SOH, -1=暂无有效值 */
/* M2 修复: 独立跟踪"每满循环实测放电容量", 而非复用总累计量(否则分子=分母恒为1) */
static float        s_cycle_discharge_ah = 0.0f;     /* 本满循环累计放电(Ah), 满充时结算 */
static bool         s_was_full = false;              /* 上一拍是否满电(检测满充完成) */
static float        s_measured_full_cap_ah = -1.0f;  /* 最近一次满循环实测容量(Ah), -1=暂无 */

/* ====== #5 SOH/循环态跨重启持久化 ======
 * 原 s_cycle_count / s_measured_full_cap_ah / s_soh 均为文件静态量, 每次重启归零,
 * 容量法 SOH 需多次满循环才可信, 断电即丢, 前端"老化曲线"无法跨重启连续.
 * 现以独立 NVS blob 低频保存(仅在满循环结算时落盘, 磨损可忽略), 重启作 EKF 初值. */
#define SOC_STATE_NVS_NS   "bms_soc"
#define SOC_STATE_NVS_KEY  "state"
#define SOC_STATE_MAGIC    0x534F4332u   // "SOC2"
#define SOC_STATE_VERSION  2             // v2: 增加 discharge_ah 字段(跨重启持久化等效循环计数累计量)

typedef struct {
    uint32_t magic;
    uint8_t  version;
    float    soh_cap;        // s_measured_full_cap_ah / CELL_CAPACITY_AH (0..1)
    uint32_t cycle_count;
    float    soh;            // 最近融合 SOH (0..1)
    float    soc;            // 最近 SOC (0..1), 重启续接
    float    discharge_ah;   // #fix: 等效循环计数累计放电安时(重启续接, 防止 cycle_count 恢复后被清零)
} soc_state_t;

static void app_soc_save_state(void)
{
    soc_state_t st;
    memset(&st, 0, sizeof(st));
    st.magic   = SOC_STATE_MAGIC;
    st.version = SOC_STATE_VERSION;
    st.soh_cap = s_measured_full_cap_ah / CELL_CAPACITY_AH;
    if (st.soh_cap > 1.0f) st.soh_cap = 1.0f;
    if (st.soh_cap < 0.0f) st.soh_cap = 0.0f;
    st.cycle_count = s_cycle_count;
    st.discharge_ah = s_discharge_ah;               // #fix: 随 cycle_count 一起持久化
    st.soh = (s_soh > 1.0f) ? 1.0f : ((s_soh < 0.0f) ? 0.0f : s_soh);
    st.soc = s_aekf.x[0];
    nvs_handle_t h;
    if (nvs_open(SOC_STATE_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        /* 2026-09-07: 检查 set_blob/commit 返回值 — 掉电瞬间写失败只告警,
         * 状态随下次结算重算, 不阻塞 */
        if (nvs_set_blob(h, SOC_STATE_NVS_KEY, &st, sizeof(st)) != ESP_OK ||
            nvs_commit(h) != ESP_OK) {
            ESP_LOGW(TAG, "NVS 落盘 SOC 状态失败(忽略, 下次结算重算)");
        }
        nvs_close(h);
    }
}

/* ====== 2026-08-09: UKF(无迹卡尔曼)独立状态量 ======
 * 对标 2026 FOMIUKF / NDO-UKF / 2026 Sci.Rep OCV-UKF 的 UKF 基础核
 * 状态 x=[SOC, V_RC1, V_RC2], 3 状态 -> 2n+1=7 个 sigma 点
 * 无需雅可比矩阵, 通过 sigma 点直接传播非线性, 对强非线性更稳 */
typedef struct {
    float x[3];                       // 状态 [SOC, V1, V2]
    float P[3][3];                    // 协方差矩阵
    float Q[3];                       // 过程噪声对角
    float R;                          // 观测噪声方差
    float dt;                         // 采样周期, 单位 s
    bool  inited;
} ukf_state_t;

static ukf_state_t s_ukf;

static void app_soc_load_state(void)
{
    soc_state_t st;
    memset(&st, 0, sizeof(st));
    nvs_handle_t h;
    if (nvs_open(SOC_STATE_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(st);
    esp_err_t err = nvs_get_blob(h, SOC_STATE_NVS_KEY, &st, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(st) ||
        st.magic != SOC_STATE_MAGIC || st.version != SOC_STATE_VERSION) {
        return;
    }
    if (st.soh_cap > 0.0f) {
        s_measured_full_cap_ah = st.soh_cap * CELL_CAPACITY_AH;
    }
    s_cycle_count = st.cycle_count;
    if (st.discharge_ah >= 0.0f) {                  // #fix: 恢复累计放电安时, 使 cycle_count 重算与重启前连续
        s_discharge_ah = st.discharge_ah;
    }
    if (st.soh > 0.0f) {
        s_soh = st.soh;
    }
    if (st.soc > 0.0f && st.soc <= 1.0f) {     // 续接上次 SOC, 老化曲线连续
        s_aekf.x[0] = st.soc;
        s_ukf.x[0]  = st.soc;
    }
}

/* ====== OCV-SOC 多项式拟合(NCM 三元, 单位 V) ====== */
/**
 * @brief OCV-SOC 多项式查表
 * @param soc SOC值(0~1)
 * @return 开路电压(V)
 * @note 三次多项式拟合 NCM 三元锂电池
 */
static float ocv_lookup(float soc)
{
    /* 三次多项式: OCV = a + b*SOC + c*SOC^2 + d*SOC^3
     * 系数 a/b/c/d 取自 bms_config.h (OCV_POLY_A/B/C/D), 不再硬编码
     * SOC=0 -> 3.0V, SOC=1 -> 4.2V(近似) */
    return OCV_POLY_A + OCV_POLY_B * soc + OCV_POLY_C * soc * soc + OCV_POLY_D * soc * soc * soc;
}

/**
 * @brief OCV 对 SOC 的导数(雅可比矩阵 H 用)
 * @param soc SOC值(0~1)
 * @return dOCV/dSOC (V)
 */
static float ocv_deriv(float soc)
{
    /* dOCV/dSOC = b + 2c*SOC + 3d*SOC^2, 系数来源 bms_config.h (OCV_POLY_B/C/D) */
    return OCV_POLY_B + 2.0f * OCV_POLY_C * soc + 3.0f * OCV_POLY_D * soc * soc;
}

/* ====== v8: OCV 反查(电压 -> SOC), 供 soc_algo=2 "OCV查表法" 使用 ======
 * OCV 多项式单调递增, 用二分法在 [0,1] 内求根, 精度 ~0.1% */
static float ocv_inverse(float v_ocv)
{
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 20; i++) {
        float mid = 0.5f * (lo + hi);
        float v = ocv_lookup(mid);
        if (v < v_ocv) lo = mid; else hi = mid;
    }
    return 0.5f * (lo + hi);
}

/**
 * @brief SOC 估算模块初始化
 * @note 初始化 AEKF 状态/协方差/噪声/Dual EKF 参数
 */
void app_soc_init(void)
{
    memset(&s_aekf, 0, sizeof(s_aekf));
    s_aekf.x[0] = SOC_INIT_VALUE;                   // SOC 初值
    s_aekf.x[1] = 0.0f;
    s_aekf.x[2] = 0.0f;
    /* 协方差初值(较大, 表示初始不确定) */
    s_aekf.P[0][0] = 1.0f;
    s_aekf.P[1][1] = 0.01f;
    s_aekf.P[2][2] = 0.01f;
    /* 噪声初值 - 取自 bms_config.h (AEKF_Q_SOC / AEKF_Q_VRC / AEKF_R_MEAS) */
    s_aekf.Q[0] = AEKF_Q_SOC;       // SOC 过程噪声
    s_aekf.Q[1] = AEKF_Q_VRC;       // V1(RC1) 过程噪声
    s_aekf.Q[2] = AEKF_Q_VRC;       // V2(RC2) 过程噪声
    s_aekf.R    = AEKF_R_MEAS;      // 观测噪声初值
    s_aekf.dt   = (float)TASK_SOC_PERIOD_MS / 1000.0f;
    s_aekf.inited = true;

    /* 内阻初值 - 取自 bms_config.h (BATT_R_NEW_OHM) */
    s_r0 = BATT_R_NEW_OHM;
    s_r1 = 0.010f;
    s_r2 = 0.020f;
    s_soh = 1.0f;

    /* ====== 2026-08-09: UKF 状态初始化(soc_algo=4 时使用) ====== */
    memset(&s_ukf, 0, sizeof(s_ukf));
    s_ukf.x[0] = SOC_INIT_VALUE;
    s_ukf.x[1] = 0.0f;
    s_ukf.x[2] = 0.0f;
    s_ukf.P[0][0] = 1.0f;
    s_ukf.P[1][1] = 0.01f;
    s_ukf.P[2][2] = 0.01f;
    s_ukf.Q[0] = AEKF_Q_SOC;
    s_ukf.Q[1] = AEKF_Q_VRC;
    s_ukf.Q[2] = AEKF_Q_VRC;
    s_ukf.R    = AEKF_R_MEAS;
    s_ukf.dt   = (float)TASK_SOC_PERIOD_MS / 1000.0f;
    s_ukf.inited = true;

    /* #5: 回载上次保存的 SOH/循环态/SOC, 使老化曲线跨重启连续 */
    app_soc_load_state();

    ESP_LOGI(TAG, "soc init (Q=%.2fAh dt=%.1fs)", CELL_CAPACITY_AH, s_aekf.dt);
}

/* ====== 新息滑动窗口统计 ====== */
/**
 * @brief 计算新息滑动窗口方差(Sage-Husa 自适应用)
 * @param st AEKF 状态
 * @return 新息方差
 */
static float innov_variance(const aekf_state_t *st)
{
    if (st->innov_cnt == 0) {
        return st->R;
    }
    float mean = 0.0f;
    for (uint8_t i = 0; i < st->innov_cnt; i++) {
        mean += st->innov_hist[i];
    }
    mean /= st->innov_cnt;

    float var = 0.0f;
    for (uint8_t i = 0; i < st->innov_cnt; i++) {
        float d = st->innov_hist[i] - mean;
        var += d * d;
    }
    return (st->innov_cnt > 1) ? var / (st->innov_cnt - 1) : st->R;
}

/**
 * @brief 推入新息到滑动窗口
 * @param st AEKF 状态
 * @param e 新息值
 */
static void innov_push(aekf_state_t *st, float e)
{
    st->innov_hist[st->innov_idx] = e;
    st->innov_idx = (st->innov_idx + 1) % AEKF_INNOV_WINDOW;
    if (st->innov_cnt < AEKF_INNOV_WINDOW) {
        st->innov_cnt++;
    }
}

/**
 * @brief Dual EKF 参数辨识(在线辨识 R0/R1/R2)
 * @param soc 当前 SOC
 * @param i_a 电流(A, 正放电)
 * @param v_meas 实测电压(V)
 * @note RLS 风格递推, 含物理约束限幅
 */
static void dual_ekf_update(float soc, float i_a, float v_meas)
{
    /* 观测残差: V_meas - (OCV - I*R0 - V1 - V2) */
    float v_pred = ocv_lookup(soc) - i_a * s_r0 - s_aekf.x[1] - s_aekf.x[2];
    float e = v_meas - v_pred;

    /* R0 在线辨识增益(指数衰减自适应) */
    float k_r0 = 0.01f / (0.01f + i_a * i_a + 1e-6f);
    s_r0 += k_r0 * i_a * e;
    /* 内阻物理约束: 限幅范围取自 bms_config.h (BATT_R0_MIN_OHM / BATT_R0_MAX_OHM) */
    if (s_r0 < BATT_R0_MIN_OHM) s_r0 = BATT_R0_MIN_OHM;
    if (s_r0 > BATT_R0_MAX_OHM) s_r0 = BATT_R0_MAX_OHM;

    /* R1/R2 近似辨识(M10 修复注释): 随 SOC 衰减缓慢增大(充放电末端内阻升高),
     * 非严格在线辨识, 仅作为二价 RC 模型的时变参数近似 */
    s_r1 = 0.010f + 0.002f * (1.0f - soc);
    s_r2 = 0.020f + 0.004f * (1.0f - soc);
}

/**
 * @brief SOH 内阻法估算
 * @return SOH(0~1)
 * @note SOH = (R_EOL - R0) / (R_EOL - R_NEW)
 */
static float calc_soh(void)
{
    /* SOH = (R_EOL - R0) / (R_EOL - R_NEW), 阈值取自 bms_config.h */
    if (s_r0 >= BATT_R_EOL_OHM) {
        return 0.0f;
    }
    float soh = (BATT_R_EOL_OHM - s_r0) / (BATT_R_EOL_OHM - BATT_R_NEW_OHM);
    if (soh > 1.0f) soh = 1.0f;
    if (soh < 0.0f) soh = 0.0f;
    return soh;
}

/**
 * @brief P1: 低温容量补偿系数(容量法, 单位 0.1℃)
 * @param temp_dc 电池最低温度, 0.1℃ 单位
 * @return 容量可用系数(0.7~1.0)
 * @note 企业级 SOC 需温度-容量修正: 25℃=1.0, 0℃=0.9, -20℃=0.7(线性插值)
 *       低于 -20℃ 钳位 0.7(保守), 防止低温下可用容量被高估
 */
static float capacity_temp_factor(int16_t temp_dc)
{
    float t = (float)temp_dc / 10.0f;                /* 0.1℃ -> ℃ */
    if (t >= 25.0f) {
        return 1.0f;
    }
    if (t >= 0.0f) {
        return 1.0f - (25.0f - t) * (0.1f / 25.0f);  /* 25℃→1.0, 0℃→0.9 */
    }
    if (t >= -20.0f) {
        return 0.9f - (0.0f - t) * (0.2f / 20.0f);   /* 0℃→0.9, -20℃→0.7 */
    }
    return 0.7f;
}

/* ====== 2026-08-09: MCC-EKF(最大相关熵准则)更新 ======
 * 对标: 2025 Energy 自适应核宽 MCCEKF / 2025 西安交大 GMMCC-EKF
 * 原理: 标准 EKF 用最小均方误差(MSE)准则, 对非高斯噪声/野值敏感;
 *       MCC 用高斯核相关熵准则, 新息 e 的核权重 λ = exp(-e²/2σ²):
 *       - 正常新息(|e|小): λ≈1, 等价于标准 EKF
 *       - 野值新息(|e|大): λ→0, 等效观测噪声 R/λ→∞, 增益 K→0, 自动忽略野值
 * 实现: 复用 AEKF 状态(协方差预测/更新同), 仅把观测噪声 R 替换为 R_eff = R/λ
 * 使用: 网页 soc_algo=3 下发切换 */
static void mcc_ekf_update(float soc_pred, float v1_pred, float v2_pred,
                           float i_a, float v_meas)
{
    /* 阶段2: 协方差预测(与 AEKF 相同) */
    float a1 = expf(-s_aekf.dt / BATT_TAU1_SEC);
    float a2 = expf(-s_aekf.dt / BATT_TAU2_SEC);
    float P00 = s_aekf.P[0][0] + s_aekf.Q[0];
    float P11 = s_aekf.P[1][1] * a1 * a1 + s_aekf.Q[1];
    float P22 = s_aekf.P[2][2] * a2 * a2 + s_aekf.Q[2];
    float P01 = s_aekf.P[0][1] * a1;
    float P02 = s_aekf.P[0][2] * a2;
    float P12 = s_aekf.P[1][2] * a1 * a2;

    s_aekf.P[0][0] = P00; s_aekf.P[0][1] = P01; s_aekf.P[0][2] = P02;
    s_aekf.P[1][0] = P01; s_aekf.P[1][1] = P11; s_aekf.P[1][2] = P12;
    s_aekf.P[2][0] = P02; s_aekf.P[2][1] = P12; s_aekf.P[2][2] = P22;

    /* 阶段3: 观测预测与新息 */
    float v_pred = ocv_lookup(soc_pred) - i_a * s_r0 - v1_pred - v2_pred;
    float innov  = v_meas - v_pred;

    /* 高斯核权重(最大相关熵): λ = exp(-e²/2σ²), σ = MCC_KERNEL_WIDTH_V */
    float sigma2 = MCC_KERNEL_WIDTH_V * MCC_KERNEL_WIDTH_V;
    float lam = expf(-(innov * innov) / (2.0f * sigma2));
    if (lam < MCC_WEIGHT_MIN) lam = MCC_WEIGHT_MIN;   /* 下限保护: 不完全拒绝观测 */

    /* 等效观测噪声: 野值时 R/λ 放大 -> 增益减小 -> 抑制污染 */
    float R_eff = s_aekf.R / lam;

    /* 雅可比 H = [dOCV/dSOC, -1, -1] */
    float h0 = ocv_deriv(soc_pred);
    float h1 = -1.0f;
    float h2 = -1.0f;

    /* 阶段4: 卡尔曼增益 K = P*H^T / (H*P*H^T + R_eff) */
    float HPH = h0 * h0 * s_aekf.P[0][0] + h1 * h1 * s_aekf.P[1][1] +
                h2 * h2 * s_aekf.P[2][2] +
                2.0f * (h0 * h1 * s_aekf.P[0][1] + h0 * h2 * s_aekf.P[0][2] +
                        h1 * h2 * s_aekf.P[1][2]);
    float s = HPH + R_eff;
    if (s < 1e-9f) s = 1e-9f;
    float k0 = (h0 * s_aekf.P[0][0] + h1 * s_aekf.P[0][1] + h2 * s_aekf.P[0][2]) / s;
    float k1 = (h0 * s_aekf.P[1][0] + h1 * s_aekf.P[1][1] + h2 * s_aekf.P[1][2]) / s;
    float k2 = (h0 * s_aekf.P[2][0] + h1 * s_aekf.P[2][1] + h2 * s_aekf.P[2][2]) / s;

    /* 阶段5: 状态更新 */
    s_aekf.x[0] = soc_pred + k0 * innov;
    s_aekf.x[1] = v1_pred  + k1 * innov;
    s_aekf.x[2] = v2_pred  + k2 * innov;
    if (s_aekf.x[0] < 0.0f) s_aekf.x[0] = 0.0f;
    if (s_aekf.x[0] > 1.0f) s_aekf.x[0] = 1.0f;

    /* 协方差更新 P = (I - K*H)*P */
    float m00 = 1.0f - k0 * h0, m01 = -k0 * h1, m02 = -k0 * h2;
    float m10 = -k1 * h0,       m11 = 1.0f - k1 * h1, m12 = -k1 * h2;
    float m20 = -k2 * h0,       m21 = -k2 * h1,       m22 = 1.0f - k2 * h2;

    float newP[3][3];
    for (uint8_t r = 0; r < 3; r++) {
        float mr = (r == 0) ? m00 : (r == 1) ? m10 : m20;
        float ms = (r == 0) ? m01 : (r == 1) ? m11 : m21;
        float mt = (r == 0) ? m02 : (r == 1) ? m12 : m22;
        newP[r][0] = mr * s_aekf.P[0][0] + ms * s_aekf.P[1][0] + mt * s_aekf.P[2][0];
        newP[r][1] = mr * s_aekf.P[0][1] + ms * s_aekf.P[1][1] + mt * s_aekf.P[2][1];
        newP[r][2] = mr * s_aekf.P[0][2] + ms * s_aekf.P[1][2] + mt * s_aekf.P[2][2];
    }
    memcpy(s_aekf.P, newP, sizeof(newP));

    /* 参数辨识 */
    dual_ekf_update(s_aekf.x[0], i_a, v_meas);
}

/* ====== 2026-08-09: UKF(无迹卡尔曼滤波)完整预测+更新 ======
 * 对标: 2026 FOMIUKF(误差0.78%)/NDO-UKF/2026 Sci.Rep OCV-UKF 的 UKF 基础核
 * 状态 n=3 -> 7 个 sigma 点, 无需求雅可比, 直接传播非线性(OCV 曲线)
 * 仅在 soc_algo==4 时启用, 结果同步回 s_aekf 供上层统一输出 */
static void ukf_run(float i_a, float v_meas, float k_temp)
{
    if (!s_ukf.inited) {
        return;
    }

    const float n     = 3.0f;
    const float lam   = UKF_ALPHA * UKF_ALPHA * (n + UKF_KAPPA) - n;
    const float nlam  = n + lam;
    const float wm0   = lam / nlam;
    const float wc0   = lam / nlam + (1.0f - UKF_ALPHA * UKF_ALPHA + UKF_BETA);
    const float wmi   = 1.0f / (2.0f * nlam);
    const float c     = sqrtf(nlam);

    /* Cholesky 分解 P = L*L^T (3x3, 下三角) */
    float L[3][3] = {{0}};
    L[0][0] = sqrtf(fmaxf(s_ukf.P[0][0], 1e-9f));
    L[1][0] = s_ukf.P[1][0] / L[0][0];
    L[1][1] = sqrtf(fmaxf(s_ukf.P[1][1] - L[1][0] * L[1][0], 1e-9f));
    L[2][0] = s_ukf.P[2][0] / L[0][0];
    L[2][1] = (s_ukf.P[2][1] - L[2][0] * L[1][0]) / L[1][1];
    L[2][2] = sqrtf(fmaxf(s_ukf.P[2][2] - L[2][0] * L[2][0] - L[2][1] * L[2][1], 1e-9f));

    /* sigma 点: X[0]=x, X[1+j]=x+c*L[:,j], X[4+j]=x-c*L[:,j] */
    float X[7][3];
    for (uint8_t i = 0; i < 3; i++) X[0][i] = s_ukf.x[i];
    for (uint8_t j = 0; j < 3; j++) {
        for (uint8_t i = 0; i < 3; i++) {
            X[1 + j][i] = s_ukf.x[i] + c * L[i][j];
            X[4 + j][i] = s_ukf.x[i] - c * L[i][j];
        }
    }

    /* 预测: 传播 sigma 点(与 AEKF 同一过程模型) */
    float a1 = expf(-s_ukf.dt / BATT_TAU1_SEC);
    float a2 = expf(-s_ukf.dt / BATT_TAU2_SEC);
    float b1 = s_r1 * (1.0f - a1);
    float b2 = s_r2 * (1.0f - a2);
    float Xp[7][3];
    for (uint8_t i = 0; i < 7; i++) {
        float soc = X[i][0] - i_a * s_ukf.dt / (3600.0f * CELL_CAPACITY_AH * k_temp);
        if (soc < 0.0f) soc = 0.0f;
        if (soc > 1.0f) soc = 1.0f;
        Xp[i][0] = soc;
        Xp[i][1] = X[i][1] * a1 + b1 * i_a;
        Xp[i][2] = X[i][2] * a2 + b2 * i_a;
    }

    /* 加权均值 */
    float xm[3] = {0, 0, 0};
    for (uint8_t i = 0; i < 7; i++) {
        float w = (i == 0) ? wm0 : wmi;
        xm[0] += w * Xp[i][0];
        xm[1] += w * Xp[i][1];
        xm[2] += w * Xp[i][2];
    }

    /* 协方差预测 */
    float Pp[3][3] = {{0}};
    for (uint8_t i = 0; i < 7; i++) {
        float w  = (i == 0) ? wc0 : wmi;
        float d0 = Xp[i][0] - xm[0], d1 = Xp[i][1] - xm[1], d2 = Xp[i][2] - xm[2];
        Pp[0][0] += w * d0 * d0; Pp[0][1] += w * d0 * d1; Pp[0][2] += w * d0 * d2;
        Pp[1][0] += w * d1 * d0; Pp[1][1] += w * d1 * d1; Pp[1][2] += w * d1 * d2;
        Pp[2][0] += w * d2 * d0; Pp[2][1] += w * d2 * d1; Pp[2][2] += w * d2 * d2;
    }
    Pp[0][0] += s_ukf.Q[0]; Pp[1][1] += s_ukf.Q[1]; Pp[2][2] += s_ukf.Q[2];

    /* 测量预测: 每个 sigma 点电压 */
    float Z[7];
    for (uint8_t i = 0; i < 7; i++) {
        Z[i] = ocv_lookup(Xp[i][0]) - i_a * s_r0 - Xp[i][1] - Xp[i][2];
    }
    float zm = 0.0f;
    for (uint8_t i = 0; i < 7; i++) {
        zm += ((i == 0) ? wm0 : wmi) * Z[i];
    }

    /* 新息协方差 Pzz 与互协方差 Pxz */
    float Pzz  = s_ukf.R;
    float Pxz[3] = {0, 0, 0};
    for (uint8_t i = 0; i < 7; i++) {
        float w  = (i == 0) ? wc0 : wmi;
        float dz = Z[i] - zm;
        Pzz += w * dz * dz;
        Pxz[0] += w * (Xp[i][0] - xm[0]) * dz;
        Pxz[1] += w * (Xp[i][1] - xm[1]) * dz;
        Pxz[2] += w * (Xp[i][2] - xm[2]) * dz;
    }
    if (Pzz < 1e-9f) Pzz = 1e-9f;

    /* 增益与状态更新 */
    float e = v_meas - zm;
    float K[3] = {Pxz[0] / Pzz, Pxz[1] / Pzz, Pxz[2] / Pzz};
    for (uint8_t i = 0; i < 3; i++) {
        s_ukf.x[i] = xm[i] + K[i] * e;
    }
    if (s_ukf.x[0] < 0.0f) s_ukf.x[0] = 0.0f;
    if (s_ukf.x[0] > 1.0f) s_ukf.x[0] = 1.0f;

    /* 协方差更新 P = Pp - K*Pzz*K^T */
    for (uint8_t r = 0; r < 3; r++) {
        for (uint8_t cc = 0; cc < 3; cc++) {
            s_ukf.P[r][cc] = Pp[r][cc] - K[r] * Pzz * K[cc];
        }
    }

    /* 同步回 s_aekf 供上层输出/SOH/循环统计 */
    s_aekf.x[0] = s_ukf.x[0];
    s_aekf.x[1] = s_ukf.x[1];
    s_aekf.x[2] = s_ukf.x[2];
    memcpy(s_aekf.P, s_ukf.P, sizeof(s_aekf.P));

    dual_ekf_update(s_aekf.x[0], i_a, v_meas);
}

/**
 * @brief SOC/SOH 估算周期处理(AEKF 主循环)
 * @param pack 电池采集数据(电压/电流/温度)
 * @param soc 输出 SOC/SOH 结果
 * @note 5 阶段: 状态预测 -> 协方差预测 -> 新息 -> 增益 -> 更新
 */
void app_soc_process(const bms_pack_data_t *pack, bms_soc_data_t *soc)
{
    if (pack == NULL || soc == NULL || !s_aekf.inited) {
        return;
    }

    /* 电流单位转换: mA -> A, 正放电/负充电
     * 模型中 I 为放电电流(正值消耗 SOC) */
    float i_a = (float)pack->current_ma / 1000.0f;

    /* 取最弱/最强单体电压作为观测(P1: 不用平均)
     * 原用平均电压, 过放末端最弱单体(min)下降最快、过充末端最强单体(max)上升最快,
     * 用 min/max 观测对边界更敏感; 无电压时由 volt_valid 分支退化 */
    uint32_t sum_v = 0;
    uint8_t soc_series = (uint8_t)(sys_params_get()->cell_series_num);
    if (soc_series < 1 || soc_series > BMS_MAX_CELL_SERIES_NUM) {
        soc_series = BMS_CELL_SERIES_NUM;
    }
    for (uint8_t c = 0; c < soc_series; c++) {
        sum_v += pack->cell_mv[c];
    }
    float v_meas = 0.0f;
    if (pack->cell_mv_min > 0) {
        v_meas = (float)pack->cell_mv_min / 1000.0f;      /* 最弱单体, mV->V */
    } else if (pack->cell_mv_max > 0) {
        v_meas = (float)pack->cell_mv_max / 1000.0f;      /* 兜底: 最强单体 */
    }

    /* ====== 阶段1: 状态预测 ====== */
    /* 时间常数取自 bms_config.h (BATT_TAU1_SEC / BATT_TAU2_SEC) */
    float a1 = expf(-s_aekf.dt / BATT_TAU1_SEC);
    float a2 = expf(-s_aekf.dt / BATT_TAU2_SEC);
    float b1 = s_r1 * (1.0f - a1);
    float b2 = s_r2 * (1.0f - a2);

    /* SOC 预测: SOC - I*dt/(3600*Q*K_T)
     * P1: 容量按温度修正(低温容量衰减, 防止低温 SOC 高估) */
    float k_temp = capacity_temp_factor(pack->temp_min_dc);
    float soc_pred = s_aekf.x[0] - i_a * s_aekf.dt / (3600.0f * CELL_CAPACITY_AH * k_temp);
    if (soc_pred < 0.0f) soc_pred = 0.0f;
    if (soc_pred > 1.0f) soc_pred = 1.0f;
    float v1_pred = s_aekf.x[1] * a1 + b1 * i_a;
    float v2_pred = s_aekf.x[2] * a2 + b2 * i_a;

    /* ====== H1 修复: 电压数据有效性检查 ======
     * LTC6804 屏蔽(HW_ENABLE_LTC6804=0)或读取失败时 cell_mv 全 0,
     * v_meas=0 与 OCV 预测(≈3.5V)产生巨大新息, 卡尔曼增益会把 SOC 一步拉低到 0 锁死.
     * 无有效电压时仅做安时积分预测(SOC 随电流正常增减), 跳过 EKF 观测更新,
     * 同时避免 R0 辨识被 0 电压观测污染 */
    const bool volt_valid = (sum_v > 0);
    /* ====== v8: 算法选择(网页 battery_type/soc_algo 下发) ======
     *   soc_algo=0: AEKF 双卡尔曼(默认, 完整观测校正)
     *   soc_algo=1: 纯安时积分(仅预测, 不做观测校正)
     *   soc_algo=2: OCV 查表法(电压直接反查 SOC)
     * 2026-08-09 新增:
     *   soc_algo=3: MCC-EKF 最大相关熵(对标 2025 Energy/西安交大, 抗野值)
     *   soc_algo=4: UKF 无迹卡尔曼(对标 2026 FOMIUKF/NDO-UKF, 无雅可比) */
    const uint8_t soc_algo = (uint8_t)(sys_params_get()->soc_algo);
    if (volt_valid && soc_algo == 2) {
        /* OCV 查表法: 用实测电压反查 SOC, 无电压时回退纯积分 */
        s_aekf.x[0] = ocv_inverse(v_meas);
        s_aekf.x[1] = v1_pred;
        s_aekf.x[2] = v2_pred;
        dual_ekf_update(s_aekf.x[0], i_a, v_meas);
    } else if (volt_valid && soc_algo == 3) {
        /* MCC-EKF: 最大相关熵准则, 高斯核加权等效观测噪声 R_eff=R/λ,
         * 新息野值自动降权, 非高斯噪声下比 AEKF 精度提升 47~90%(2025 文献) */
        mcc_ekf_update(soc_pred, v1_pred, v2_pred, i_a, v_meas);
    } else if (volt_valid && soc_algo == 4) {
        /* UKF: 无迹变换 7 个 sigma 点直接传播非线性 OCV, 无需雅可比;
         * 2026 FOMIUKF 误差 0.78% 优于 EKF/UKF */
        ukf_run(i_a, v_meas, k_temp);
    } else if (volt_valid && soc_algo == 0) {
    /* ====== 阶段2: 协方差预测 P = A*P*A^T + Q ====== */
    /* A = diag(1, a1, a2) */
    float P00 = s_aekf.P[0][0] + s_aekf.Q[0];
    float P11 = s_aekf.P[1][1] * a1 * a1 + s_aekf.Q[1];
    float P22 = s_aekf.P[2][2] * a2 * a2 + s_aekf.Q[2];
    float P01 = s_aekf.P[0][1] * a1;
    float P02 = s_aekf.P[0][2] * a2;
    float P12 = s_aekf.P[1][2] * a1 * a2;

    s_aekf.P[0][0] = P00; s_aekf.P[0][1] = P01; s_aekf.P[0][2] = P02;
    s_aekf.P[1][0] = P01; s_aekf.P[1][1] = P11; s_aekf.P[1][2] = P12;
    s_aekf.P[2][0] = P02; s_aekf.P[2][1] = P12; s_aekf.P[2][2] = P22;

    /* ====== 阶段3: 观测预测与新息 ====== */
    float v_pred = ocv_lookup(soc_pred) - i_a * s_r0 - v1_pred - v2_pred;
    float innov  = v_meas - v_pred;                 // 新息 e(k)

    /* 雅可比 H = [dOCV/dSOC, -1, -1] */
    float h0 = ocv_deriv(soc_pred);
    float h1 = -1.0f;
    float h2 = -1.0f;

    /* Sage-Husa 自适应: 在线更新 R */
    innov_push(&s_aekf, innov);
    float innov_var = innov_variance(&s_aekf);
    float HPH = h0 * h0 * s_aekf.P[0][0] + h1 * h1 * s_aekf.P[1][1] +
                h2 * h2 * s_aekf.P[2][2] +
                2.0f * (h0 * h1 * s_aekf.P[0][1] + h0 * h2 * s_aekf.P[0][2] +
                        h1 * h2 * s_aekf.P[1][2]);
    s_aekf.R = AEKF_FORGETTING_FACTOR * s_aekf.R +
               (1.0f - AEKF_FORGETTING_FACTOR) * (innov_var - HPH);
    if (s_aekf.R < 1e-6f) s_aekf.R = 1e-6f;          // 下限保护

    /* ====== 阶段4: 卡尔曼增益 K = P*H^T / (H*P*H^T + R) ====== */
    float s = HPH + s_aekf.R;
    if (s < 1e-9f) s = 1e-9f;                        // 防除零
    float k0 = (h0 * s_aekf.P[0][0] + h1 * s_aekf.P[0][1] + h2 * s_aekf.P[0][2]) / s;
    float k1 = (h0 * s_aekf.P[1][0] + h1 * s_aekf.P[1][1] + h2 * s_aekf.P[1][2]) / s;
    float k2 = (h0 * s_aekf.P[2][0] + h1 * s_aekf.P[2][1] + h2 * s_aekf.P[2][2]) / s;

    /* ====== 阶段5: 状态更新 x = x_pred + K*innov ====== */
    s_aekf.x[0] = soc_pred + k0 * innov;
    s_aekf.x[1] = v1_pred  + k1 * innov;
    s_aekf.x[2] = v2_pred  + k2 * innov;

    /* SOC 物理约束 */
    if (s_aekf.x[0] < 0.0f) s_aekf.x[0] = 0.0f;
    if (s_aekf.x[0] > 1.0f) s_aekf.x[0] = 1.0f;

    /* ====== 协方差更新 P = (I - K*H)*P ====== */
    /* I-KH 的各行 */
    /* row0: [1-k0*h0, -k0*h1, -k0*h2] */
    float m00 = 1.0f - k0 * h0, m01 = -k0 * h1, m02 = -k0 * h2;
    float m10 = -k1 * h0,        m11 = 1.0f - k1 * h1, m12 = -k1 * h2;
    float m20 = -k2 * h0,        m21 = -k2 * h1,        m22 = 1.0f - k2 * h2;

    float newP[3][3];
    for (uint8_t r = 0; r < 3; r++) {
        float mr = (r == 0) ? m00 : (r == 1) ? m10 : m20;
        float ms = (r == 0) ? m01 : (r == 1) ? m11 : m21;
        float mt = (r == 0) ? m02 : (r == 1) ? m12 : m22;
        newP[r][0] = mr * s_aekf.P[0][0] + ms * s_aekf.P[1][0] + mt * s_aekf.P[2][0];
        newP[r][1] = mr * s_aekf.P[0][1] + ms * s_aekf.P[1][1] + mt * s_aekf.P[2][1];
        newP[r][2] = mr * s_aekf.P[0][2] + ms * s_aekf.P[1][2] + mt * s_aekf.P[2][2];
    }
    memcpy(s_aekf.P, newP, sizeof(newP));

    /* ====== Dual EKF 参数辨识 ====== */
    dual_ekf_update(s_aekf.x[0], i_a, v_meas);
    } else {
    /* ====== 无有效电压: 仅应用安时积分预测, 不做观测校正 ======
     * SOC 按电流方向正常增减, 协方差保持(未观测不缩小, 避免虚假收敛) */
    s_aekf.x[0] = soc_pred;
    s_aekf.x[1] = v1_pred;
    s_aekf.x[2] = v2_pred;
    /* P 保持原值(不执行观测更新), R0 辨识跳过(无有效电压观测) */
    }

    /* ====== SOH 估算 ====== */
    s_soh = calc_soh();

    /* ====== Bug6 修复: 循环次数计算 ======
     * 算法: 安时积分法累计放电电量, 每满一个完整容量(CELL_CAPACITY_AH)算一次循环
     * 当 SOC 从高(>90%)降到低(<10%)再回到高, 算一次完整循环
     * 简化实现: 累计放电安时, 除以电池容量得到等效循环次数 */
    static float s_last_soc = -1.0f;                  // 上次 SOC(函数内, 重启重置基线用)
    if (s_last_soc < 0.0f) {
        s_last_soc = s_aekf.x[0];                     // 首次初始化
    } else {
        /* SOC 下降=放电, 累计安时; SOC 上升=充电, 不累计 */
        float d_soc = s_last_soc - s_aekf.x[0];
        if (d_soc > 0.001f) {
            s_discharge_ah += d_soc * CELL_CAPACITY_AH;
        }
        s_last_soc = s_aekf.x[0];
    }
    /* 等效循环次数 = 累计放电安时 / 电池容量 */
    s_cycle_count = (uint32_t)(s_discharge_ah / CELL_CAPACITY_AH);

    /* ====== M2: 容量法 SOH(满循环实测容量对比标称容量) ======
     * 原实现 s_measured_discharge_ah / (标称Ah * s_cycle_count) 中,
     * 分子(总累计放电)与分母(标称Ah * 等效循环数)实为同一库仑积分量, 恒≈1.0,
     * 永远检测不出容量衰减.
     * 修复: 独立跟踪"每个满循环"的实测放电量 —— 在 SOC 回到满电(>=99%)的瞬间,
     *       本次循环从满到满(或空到满)放掉的电量即为该循环可用容量;
     *       与标称容量比较得到真实容量法 SOH. 仅当观察到完整循环才有意义. */
    if (i_a > 0.0f) {
        s_measured_discharge_ah += i_a * s_aekf.dt / 3600.0f;   /* 总累计(保留) */
        s_cycle_discharge_ah    += i_a * s_aekf.dt / 3600.0f;   /* 本循环累计 */
    }
    /* 满充完成检测: SOC 由非满跨越到满(>=99%) */
    bool is_full = (s_aekf.x[0] >= 0.99f);
    if (is_full && !s_was_full) {
        /* 刚完成一次满充: 结算本循环实测容量(至少放过一些电才有效, 否则跳过) */
        if (s_cycle_discharge_ah > 0.05f * CELL_CAPACITY_AH) {
            s_measured_full_cap_ah = s_cycle_discharge_ah;
            app_soc_save_state();                     /* #5: 满循环结算时低频落盘 */
        }
        s_cycle_discharge_ah = 0.0f;                  /* 重置, 开始下一循环 */
    }
    s_was_full = is_full;
    /* 容量法 SOH = 实测满循环容量 / 标称容量 */
    if (s_measured_full_cap_ah > 0.0f) {
        float soh_cap = s_measured_full_cap_ah / CELL_CAPACITY_AH;
        if (soh_cap > 1.0f) soh_cap = 1.0f;
        if (soh_cap < 0.0f) soh_cap = 0.0f;
        s_soh_cap = soh_cap;
    }
    /* ====== v9: 企业级加权融合 SOH(参考 GB/T 47136-2026 / CN103558556A) ======
     * 内阻法(R0)与容量法(实测放电量)分别估算, 再按可信度加权融合:
     *   容量法需完整循环数据才可信 -> 循环>=3 时权重 0.6, 否则 0.3;
     *   融合结果与纯内阻法取保守下限, 防止容量法早期波动虚高. */
    if (s_soh_cap > 0.0f) {
        float w_cap = (s_cycle_count >= 3) ? 0.6f : 0.3f;   /* 循环越多容量法越可信 */
        float fused = w_cap * s_soh_cap + (1.0f - w_cap) * s_soh;
        if (fused > 1.0f) fused = 1.0f;
        if (fused < 0.0f) fused = 0.0f;
        /* 保守下限: 融合值与内阻法取较小者, 避免单次异常拉高 SOH */
        if (fused < s_soh) s_soh = fused;
    }

    /* ====== 输出结果 ====== */
    soc->soc   = s_aekf.x[0];
    soc->soh   = s_soh;
    soc->soc_ekf = s_aekf.x[0];                      // 此处 AEKF 与 EKF 同源
    soc->soc_nn  = SOC_INVALID;                       // NN 推理由独立任务填充
    soc->r0_ohm = s_r0;
    soc->r1_ohm = s_r1;
    soc->r2_ohm = s_r2;
    soc->cycle_count = s_cycle_count;                 /* Bug6: 循环次数输出 */
}
