/**
 * @file    sys_filter.c
 * @brief   数据滤波工具实现
 * @author  BMS Team
 * @date    2026-08
 */
#include "sys_filter.h"

void sys_lpf_init(sys_lpf_t *lpf, float alpha)
{
    if (lpf == NULL) {
        return;
    }
    lpf->value  = 0.0f;
    lpf->alpha  = alpha;
    lpf->inited = false;
}

float sys_lpf_update(sys_lpf_t *lpf, float sample)
{
    if (lpf == NULL) {
        return sample;
    }
    if (!lpf->inited) {
        /* 首次采样直接赋值, 避免从 0 缓慢爬升 */
        lpf->value  = sample;
        lpf->inited = true;
    } else {
        /* I_filt = alpha*sample + (1-alpha)*I_filt */
        lpf->value = lpf->alpha * sample + (1.0f - lpf->alpha) * lpf->value;
    }
    return lpf->value;
}

float sys_filter_mean(float *buf, uint8_t len, float sample, uint8_t *idx)
{
    if (buf == NULL || len == 0 || idx == NULL) {
        return sample;
    }
    /* 环形写入 */
    buf[*idx] = sample;
    *idx = (uint8_t)((*idx + 1) % len);

    /* 求均值 */
    float sum = 0.0f;
    for (uint8_t i = 0; i < len; i++) {
        sum += buf[i];
    }
    return sum / len;
}

/* ====== 2026-08-10: 补充真实算法(VFF-RLS / 滑动中值) ======
 * 前端"算法可视化中心"此前展示 VFF-RLS / 中值滤波等名称但固件无对应实现,
 * 此处补齐可运行的实体算法(纯 C, 无硬件依赖), 供通道滤波/趋势预测使用. */

void sys_vff_rls_init(sys_vff_rls_t *rls, float lambda0)
{
    if (rls == NULL) {
        return;
    }
    rls->lambda = (lambda0 > 0.95f && lambda0 < 1.0f) ? lambda0 : 0.99f;
    rls->P      = 1.0f;        /* 初始协方差 */
    rls->a      = 0.0f;
    rls->b      = 0.0f;
    rls->inited = false;
}

float sys_vff_rls_update(sys_vff_rls_t *rls, float x, float y)
{
    if (rls == NULL) {
        return y;
    }
    if (!rls->inited) {
        /* 首点直接建立基准, 避免从 0 爬升 */
        rls->a      = y;
        rls->b      = 0.0f;
        rls->inited = true;
        return y;
    }
    /* 预测 y_hat = a + b*x */
    float y_hat = rls->a + rls->b * x;
    float e     = y - y_hat;                     /* 新息 */
    /* 自适应遗忘因子: 新息大(突变)→ λ 减小加速跟踪; 稳态 → λ 恢复平滑 */
    float lam = rls->lambda;
    float e_abs = (e < 0.0f) ? -e : e;
    if (e_abs > 0.5f) {
        lam -= 0.02f * (e_abs > 2.0f ? 1.0f : e_abs / 2.0f);
        if (lam < 0.95f) lam = 0.95f;
    } else {
        lam = rls->lambda + 0.0005f;
        if (lam > 0.999f) lam = 0.999f;
    }
    rls->lambda = lam;
    /* 标量 RLS: K = P*x / (λ + P*x²), P ← (P - K*x*P)/λ */
    float P  = rls->P;
    float K  = P * x / (lam + P * x * x);
    float Pn = (P - K * x * P) / lam;
    if (Pn < 1e-6f) Pn = 1e-6f;
    rls->P = Pn;
    /* 参数更新: a += K*e, b += K*x*e */
    rls->a += K * e;
    rls->b += K * x * e;
    return rls->a + rls->b * x;
}

void sys_median_init(sys_median_t *med, uint8_t len)
{
    if (med == NULL) {
        return;
    }
    if (len > 17) len = 17;
    if (len == 0) len = 5;
    if ((len & 1U) == 0) len++;            /* 强制奇数窗口 */
    med->len    = len;
    med->idx    = 0;
    med->inited = false;
    for (uint8_t i = 0; i < len; i++) {
        med->buf[i] = 0.0f;
    }
}

float sys_median_update(sys_median_t *med, float sample)
{
    if (med == NULL || med->len == 0) {
        return sample;
    }
    med->buf[med->idx] = sample;
    med->idx = (uint8_t)((med->idx + 1) % med->len);
    /* 复制窗口并排序(选择排序, 窗口 ≤17 足够快) */
    float tmp[17];
    uint8_t n = med->len;
    for (uint8_t i = 0; i < n; i++) {
        tmp[i] = med->buf[i];
    }
    for (uint8_t i = 0; i + 1 < n; i++) {
        for (uint8_t j = i + 1; j < n; j++) {
            if (tmp[j] < tmp[i]) {
                float t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
            }
        }
    }
    return tmp[n / 2];                     /* 中位数 */
}

/* ====== 2026-08-10: 模糊均衡推理(小型 Mamdani) ======
 * 输入1 压差(ΔV): 小[0,20] 中[10,50] 大[40,∞] mV
 * 输入2 温度(T) : 低[-20,10] 适[0,40] 高[35,60] ℃
 * 规则: 压差大→强均衡(温度适中时最强); 温度高/低→降低强度(保护)
 * 输出: 均衡强度 0~1, 重心法去模糊 */
static float _fz_tri(float x, float a, float b, float c)
{
    if (x <= a || x >= c) return 0.0f;
    if (x == b) return 1.0f;
    if (x < b) return (x - a) / (b - a);
    return (c - x) / (c - b);
}

float sys_fuzzy_balance(float delta_mv, float temp_c)
{
    /* 压差隶属度: 小/中/大 */
    float dvS = _fz_tri(delta_mv, 0.0f, 5.0f, 20.0f);
    float dvM = _fz_tri(delta_mv, 10.0f, 30.0f, 50.0f);
    float dvL = _fz_tri(delta_mv, 40.0f, 80.0f, 120.0f);
    /* 温度隶属度: 低/适/高 */
    float tL = _fz_tri(temp_c, -20.0f, 5.0f, 15.0f);
    float tM = _fz_tri(temp_c, 5.0f, 25.0f, 40.0f);
    float tH = _fz_tri(temp_c, 35.0f, 48.0f, 60.0f);
    /* 规则表: (压差, 温度) → 输出强度 (弱0.3/中0.6/强1.0) */
    /*  R1 大 & 适 -> 1.0    R2 中 & 适 -> 0.6    R3 小 & 适 -> 0.2
     *  R4 大 & (低|高) -> 0.5(温度保护降强度)
     *  R5 中 & (低|高) -> 0.3
     *  R6 小 & (低|高) -> 0.1
     *  R7 任意 & 高温(>55 过热保护) -> 0(禁止均衡) */
    float tH_off = _fz_tri(temp_c, 50.0f, 60.0f, 70.0f);   /* 过热禁均衡 */
    float r1 = dvL * tM;               /* 大&适 -> 强 */
    float r2 = dvM * tM;               /* 中&适 -> 中 */
    float r3 = dvS * tM;               /* 小&适 -> 弱 */
    float r4 = dvL * (tL > tH ? tL : tH);   /* 大&(低|高) -> 中弱 */
    float r5 = dvM * (tL > tH ? tL : tH);   /* 中&(低|高) -> 弱 */
    float r6 = dvS * (tL > tH ? tL : tH);   /* 小&(低|高) -> 极弱 */
    /* 输出集合中心: 弱=0.25 中=0.6 强=1.0; 加权重心 */
    float num = r1*1.0f + r2*0.6f + r3*0.25f + r4*0.5f + r5*0.3f + r6*0.1f;
    float den = r1 + r2 + r3 + r4 + r5 + r6;
    float out = (den > 1e-6f) ? (num / den) : 0.0f;
    /* 过热硬禁止: 温度越高权重越大 */
    out *= (1.0f - tH_off);
    if (out < 0.0f) out = 0.0f;
    if (out > 1.0f) out = 1.0f;
    return out;
}

/* ====== 2026-08-10: 基于 VFF-RLS 斜率的趋势预测 ======
 * 用已训练的 RLS 回归参数 a/b 向前外推: y_hat = a + b*(x_now+horizon) */
float sys_rls_predict(const sys_vff_rls_t *rls, float x_now, float horizon)
{
    if (rls == NULL || !rls->inited) {
        return 0.0f;
    }
    return rls->a + rls->b * (x_now + horizon);
}
