/**
 * @file    test_app_soc_algo.c
 * @brief   2026-08-09 单元测试: 新增 SOC 算法(MCC-EKF / UKF)
 * @note    通过公开 API(app_soc_init/app_soc_process) + 运行时参数
 *          sys_params_set 切换 soc_algo=3(MCC-EKF)/4(UKF) 验证:
 *          - SOC 始终有界 [0,1], 不出现 NaN/Inf
 *          - 有效电压下算法稳定迭代不爆炸(电压爬升/放电)
 *          - 无有效电压时退化为安时积分, SOC 不被 0 电压观测拉低
 *          - 内阻辨识 R0 始终为正
 */
#include <string.h>
#include <math.h>
#include "unity.h"
#include "app_soc.h"
#include "sys_params.h"
#include "bms_config.h"

#define TEST_CELL_CNT BMS_CELL_SERIES_NUM

static bms_pack_data_t s_pack;
static bms_soc_data_t  s_soc;

static void set_algo(uint8_t algo)
{
    bms_params_t p;
    memcpy(&p, sys_params_get(), sizeof(p));
    p.soc_algo = algo;
    sys_params_set(&p);
}

void setUp(void)
{
    memset(&s_pack, 0, sizeof(s_pack));
    memset(&s_soc, 0, sizeof(s_soc));
    for (int i = 0; i < TEST_CELL_CNT; i++) {
        s_pack.cell_mv[i] = 3800;
    }
    s_pack.cell_mv_max = 3800;
    s_pack.cell_mv_min = 3800;
    s_pack.temp_max_dc = 250;
    s_pack.temp_min_dc = 250;
    s_pack.current_ma  = 0;
    s_pack.timestamp_ms = 0;
    app_soc_init();
    set_algo(0);   /* 默认 AEKF */
}

void tearDown(void)
{
}

/* ---------- 通用: 迭代 N 步并断言 SOC 有界/无 NaN ---------- */
static void run_n_steps(int n, int voltage_ramp)
{
    for (int i = 0; i < n; i++) {
        s_pack.timestamp_ms += (uint32_t)((float)TASK_SOC_PERIOD_MS);
        if (voltage_ramp) {
            for (int c = 0; c < TEST_CELL_CNT; c++) {
                s_pack.cell_mv[c] = 3700 + (i % 200);
            }
            s_pack.cell_mv_max = 3700 + (i % 200);
            s_pack.cell_mv_min = 3700 + (i % 200);
        }
        app_soc_process(&s_pack, &s_soc);
        TEST_ASSERT_FALSE(isnan(s_soc.soc));
        TEST_ASSERT_FALSE(isinf(s_soc.soc));
        TEST_ASSERT_TRUE(s_soc.soc >= 0.0f && s_soc.soc <= 1.0f);
        TEST_ASSERT_TRUE(s_soc.r0_ohm > 0.0f);
    }
}

/* ====== MCC-EKF(soc_algo=3) ====== */
TEST_CASE("MCC-EKF stable with valid voltage", "[soc][mcc]")
{
    set_algo(3);
    run_n_steps(100, 1);
}

TEST_CASE("MCC-EKF discharge SOC decreases", "[soc][mcc]")
{
    set_algo(3);
    s_pack.current_ma = 1000;          /* 1A 放电 */
    float prev = SOC_INIT_VALUE;
    for (int i = 0; i < 30; i++) {
        s_pack.timestamp_ms += (uint32_t)((float)TASK_SOC_PERIOD_MS);
        app_soc_process(&s_pack, &s_soc);
        TEST_ASSERT_TRUE(s_soc.soc <= prev + 1e-4f);   /* 单调不增 */
        prev = s_soc.soc;
        TEST_ASSERT_TRUE(s_soc.soc >= 0.0f);
    }
    TEST_ASSERT_TRUE(s_soc.soc < SOC_INIT_VALUE);
}

TEST_CASE("MCC-EKF no voltage keeps SOC (H1 regression)", "[soc][mcc]")
{
    set_algo(3);
    memset(&s_pack, 0, sizeof(s_pack));   /* 全 0 = 无有效电压 */
    s_pack.current_ma = 0;
    app_soc_process(&s_pack, &s_soc);
    TEST_ASSERT_TRUE(s_soc.soc > 0.30f);  /* 不被 0V 观测拉低 */
    TEST_ASSERT_FALSE(isnan(s_soc.soc));
}

/* ====== UKF(soc_algo=4) ====== */
TEST_CASE("UKF stable with valid voltage", "[soc][ukf]")
{
    set_algo(4);
    run_n_steps(100, 1);
}

TEST_CASE("UKF discharge SOC decreases", "[soc][ukf]")
{
    set_algo(4);
    s_pack.current_ma = 1000;
    float prev = SOC_INIT_VALUE;
    for (int i = 0; i < 30; i++) {
        s_pack.timestamp_ms += (uint32_t)((float)TASK_SOC_PERIOD_MS);
        app_soc_process(&s_pack, &s_soc);
        TEST_ASSERT_TRUE(s_soc.soc <= prev + 1e-4f);
        prev = s_soc.soc;
        TEST_ASSERT_TRUE(s_soc.soc >= 0.0f);
    }
    TEST_ASSERT_TRUE(s_soc.soc < SOC_INIT_VALUE);
}

TEST_CASE("UKF no voltage keeps SOC (H1 regression)", "[soc][ukf]")
{
    set_algo(4);
    memset(&s_pack, 0, sizeof(s_pack));
    s_pack.current_ma = 0;
    app_soc_process(&s_pack, &s_soc);
    TEST_ASSERT_TRUE(s_soc.soc > 0.30f);
    TEST_ASSERT_FALSE(isnan(s_soc.soc));
}

/* ---------- 算法切换可逆: 切回 AEKF 后仍稳定 ---------- */
TEST_CASE("algo switch back to AEKF stable", "[soc]")
{
    set_algo(4);
    run_n_steps(30, 1);
    set_algo(0);
    run_n_steps(30, 1);   /* 切回默认算法, 状态沿用 s_aekf, 不应崩溃 */
}
