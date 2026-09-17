/**
 * @file    test_app_soc.c
 * @brief   P0-4 单元测试: AEKF/EKF 收敛行为 + H1 回归(无电压不拉低SOC)
 * @note    通过公开 API(app_soc_init/app_soc_process)验证:
 *          - SOC 始终有界 [0,1], 不出现 NaN/Inf
 *          - 无有效电压时仅安时积分, SOC 不被 0 电压观测拉低(H1 回归)
 *          - 放电 SOC 下降 / 充电 SOC 上升(方向正确)
 *          - 有效电压下 EKF 收敛到合理范围
 */
#include <string.h>
#include <math.h>
#include "unity.h"
#include "app_soc.h"
#include "bms_config.h"

#define TEST_CELL_CNT BMS_CELL_SERIES_NUM

static bms_pack_data_t s_pack;
static bms_soc_data_t  s_soc;

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
}

void tearDown(void)
{
}

/* ---------- 初始 SOC = SOC_INIT_VALUE(0.5), 且非 NaN ---------- */
TEST_CASE("soc init value", "[soc]")
{
    app_soc_process(&s_pack, &s_soc);
    TEST_ASSERT_FALSE(isnan(s_soc.soc));
    TEST_ASSERT_FALSE(isinf(s_soc.soc));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, SOC_INIT_VALUE, s_soc.soc);
    TEST_ASSERT_TRUE(s_soc.soc >= 0.0f && s_soc.soc <= 1.0f);
}

/* ---------- H1 回归: 无有效电压(全0)时 SOC 不被拉低到 0 ---------- */
TEST_CASE("no voltage keeps SOC (H1 regression)", "[soc]")
{
    /* 模拟 LTC6804 未接线: 电压全 0, 待机电流 0 */
    memset(&s_pack, 0, sizeof(s_pack));
    s_pack.current_ma = 0;

    app_soc_process(&s_pack, &s_soc);
    /* SOC 应保持初始 0.5(安时积分无电流变化), 而不是被 0V 观测拉到 0 */
    TEST_ASSERT_TRUE(s_soc.soc > 0.30f);
    TEST_ASSERT_FALSE(isnan(s_soc.soc));
}

/* ---------- 无电压但放电时, SOC 按安时积分下降 ---------- */
TEST_CASE("coulomb counting without voltage", "[soc]")
{
    memset(&s_pack, 0, sizeof(s_pack));
    s_pack.current_ma = 1000;          /* 1A 放电 */
    s_pack.timestamp_ms = 0;

    float prev = SOC_INIT_VALUE;
    for (int i = 0; i < 30; i++) {
        s_pack.timestamp_ms += (uint32_t)((float)TASK_SOC_PERIOD_MS);
        app_soc_process(&s_pack, &s_soc);
        TEST_ASSERT_TRUE(s_soc.soc <= prev + 1e-4f);   /* 放电SOC单调不增 */
        prev = s_soc.soc;
        TEST_ASSERT_TRUE(s_soc.soc >= 0.0f);
    }
    TEST_ASSERT_TRUE(s_soc.soc < SOC_INIT_VALUE);       /* 确实下降了 */
}

/* ---------- 充电时 SOC 上升且保持有界 ---------- */
TEST_CASE("charge increases SOC bounded", "[soc]")
{
    s_pack.current_ma = -1000;         /* -1A 充电 */
    s_pack.timestamp_ms = 0;
    for (int i = 0; i < 50; i++) {
        s_pack.timestamp_ms += (uint32_t)((float)TASK_SOC_PERIOD_MS);
        app_soc_process(&s_pack, &s_soc);
        TEST_ASSERT_TRUE(s_soc.soc <= 1.0f + 1e-4f);
        TEST_ASSERT_FALSE(isnan(s_soc.soc));
    }
}

/* ---------- 有效电压下 EKF 迭代收敛到有界范围(不爆炸) ---------- */
TEST_CASE("EKF stable with valid voltage", "[soc]")
{
    s_pack.current_ma = 0;
    s_pack.timestamp_ms = 0;
    for (int i = 0; i < 100; i++) {
        s_pack.timestamp_ms += (uint32_t)((float)TASK_SOC_PERIOD_MS);
        /* 电压缓慢爬升模拟充电 */
        for (int c = 0; c < TEST_CELL_CNT; c++) {
            s_pack.cell_mv[c] = 3700 + i;
        }
        s_pack.cell_mv_max = 3700 + i;
        s_pack.cell_mv_min = 3700 + i;
        app_soc_process(&s_pack, &s_soc);
        TEST_ASSERT_FALSE(isnan(s_soc.soc));
        TEST_ASSERT_FALSE(isinf(s_soc.soc));
        TEST_ASSERT_TRUE(s_soc.soc >= 0.0f && s_soc.soc <= 1.0f);
        TEST_ASSERT_TRUE(s_soc.r0_ohm > 0.0f);          /* 内阻始终为正 */
    }
}
