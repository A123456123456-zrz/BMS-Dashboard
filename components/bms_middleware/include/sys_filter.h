/**
 * @file    sys_filter.h
 * @brief   数据滤波工具(一阶低通 / 滑动均值 / VFF-RLS 自适应滤波 / 滑动中值滤波)
 * @author  BMS Team
 * @date    2026-08
 * @note    纯算法, 不依赖硬件, 可被 Middleware/App 层调用
 */
#ifndef SYS_FILTER_H
#define SYS_FILTER_H

#include <stdint.h>
#include <stdbool.h>                                 // bool / true / false
#include <stddef.h>                                  // NULL

/**
 * @brief   一阶低通滤波器上下文
 */
typedef struct {
    float value;                                    // 当前滤波输出
    float alpha;                                    // 系数 0~1, 越大越信任新值
    bool  inited;                                   // 是否已初始化
} sys_lpf_t;

/**
 * @brief   初始化一阶低通滤波器
 * @param   lpf    滤波器上下文
 * @param   alpha  系数(电流典型 0.1)
 */
void sys_lpf_init(sys_lpf_t *lpf, float alpha);

/**
 * @brief   一阶低通滤波更新
 * @return  滤波后输出
 */
float sys_lpf_update(sys_lpf_t *lpf, float sample);

/**
 * @brief   滑动窗口均值滤波
 * @param   buf      缓冲区
 * @param   len       缓冲区长度
 * @param   sample    新采样值
 * @param   idx       当前写入索引(调用方持有, 内部自增)
 * @return  均值
 */
float sys_filter_mean(float *buf, uint8_t len, float sample, uint8_t *idx);

/* ====== 2026-08-10: 补充真实算法(VFF-RLS / 滑动中值) ======
 * 前端"算法可视化中心"此前展示 VFF-RLS 等名称但固件无对应实现,
 * 这里补齐可运行的实体算法, 供通道滤波/故障预测等场景使用. */

/**
 * @brief   变遗忘因子递推最小二乘(VFF-RLS)滤波器
 * @note    一阶线性回归 y = a + b*x 在线辨识:
 *          - 自适应遗忘因子 λ∈[0.95,0.999]: 新息 e 大(信号突变)时减小 λ,
 *            加速跟踪; e 小(稳态)时增大 λ, 增强平滑
 *          - 输出 = 当前时刻回归估计值, 同时暴露斜率 b(趋势检测用)
 */
typedef struct {
    float lambda;        // 遗忘因子(自适应更新)
    float P;             // 协方差(标量, 一阶)
    float a;             // 截距(滤波输出基准)
    float b;             // 斜率(趋势, 供预测/告警)
    bool  inited;        // 是否已初始化
} sys_vff_rls_t;

/**
 * @brief   初始化 VFF-RLS 滤波器
 * @param   rls   上下文
 * @param   lambda0  初始遗忘因子(典型 0.99)
 */
void sys_vff_rls_init(sys_vff_rls_t *rls, float lambda0);

/**
 * @brief   VFF-RLS 滤波更新(每周期调用一次)
 * @param   rls      上下文
 * @param   x        自变量(典型: 相对时间序号, 调用方自增 0,1,2,...)
 * @param   y        观测值(待滤波信号)
 * @return  滤波输出(当前回归估计 a + b*x)
 */
float sys_vff_rls_update(sys_vff_rls_t *rls, float x, float y);

/**
 * @brief   滑动中值滤波上下文(窗口 N, N 取奇数, 如 5/7/9)
 */
typedef struct {
    float  buf[17];      // 最大窗口 17(够用且栈友好)
    uint8_t len;         // 实际窗口长度(奇数)
    uint8_t idx;         // 环形写入索引
    bool   inited;       // 窗口是否已填满
} sys_median_t;

/**
 * @brief   初始化滑动中值滤波器
 * @param   med  上下文
 * @param   len  窗口长度(奇数, ≤17; 偶数自动+1)
 */
void sys_median_init(sys_median_t *med, uint8_t len);

/**
 * @brief   滑动中值滤波更新(取窗口内中位数, 抗孤立野值)
 * @return  中值滤波输出
 */
float sys_median_update(sys_median_t *med, float sample);

/* ====== 2026-08-10: 补充"模糊均衡"真实算法(纯函数) ======
 * 前端算法可视化中心此前展示"模糊均衡"但固件仅阈值触发,
 * 此处补齐小型 Mamdani 模糊推理: 输入(压差, 温度) → 输出均衡强度.
 * 供均衡模块按需调用, 纯算法无硬件依赖, 可直接单元测试. */

/**
 * @brief   模糊均衡推理(二输入一输出, 三角形隶属度 + 重心去模糊)
 * @param   delta_mv   当前最大单体压差(mV)
 * @param   temp_c     电池温度(℃)
 * @return  均衡强度 0.0~1.0(0=不均衡, 1=满强度)
 */
float sys_fuzzy_balance(float delta_mv, float temp_c);

/**
 * @brief   基于 VFF-RLS 斜率的趋势预测(轻量故障/电量趋势)
 * @param   rls     已训练的 VFF-RLS 上下文(暴露 a/b)
 * @param   x_now   当前时间序号
 * @param   horizon 预测步数(向前外推)
 * @return  预测值 a + b*(x_now+horizon)
 */
float sys_rls_predict(const sys_vff_rls_t *rls, float x_now, float horizon);

#endif // SYS_FILTER_H
