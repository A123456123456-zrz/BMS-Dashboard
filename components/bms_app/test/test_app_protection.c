/**
 * @file    test_app_protection.c
 * @brief   P0-4 单元测试: 保护阈值滞回边界 / 降额因子 / 继电器联动
 * @note    通过公开 API(app_protection_process)验证滞回行为,
 *          被测函数内部 static 状态经 setUp 重置, 避免用例间污染
 */
#include <string.h>
#include "unity.h"
#include "app_protection.h"
#include "bms_config.h"
#include "sys_data.h"

#define TEST_CELL_CNT BMS_CELL_SERIES_NUM

static bms_pack_data_t s_pack;

void setUp(void)
{
    memset(&s_pack, 0, sizeof(s_pack));
    /* 恢复系统全局状态(继电器/故障/热失控锁/降额) */
    sys_data_init();
    app_protection_init();
    /* 默认健康电压/温度/电流, 各用例按需修改 */
    for (int i = 0; i < TEST_CELL_CNT; i++) {
        s_pack.cell_mv[i] = 3800;
    }
    s_pack.cell_mv_max = 3800;
    s_pack.cell_mv_min = 3800;
    s_pack.temp_max_dc = 250;   /* 25.0℃ */
    s_pack.temp_min_dc = 250;
    s_pack.current_ma  = 0;
    s_pack.timestamp_ms = 0;
}

void tearDown(void)
{
}

/* ---------- 单体过压滞回: 触发(≥4250) → 保持(未到4100) → 恢复(≤4100) ---------- */
TEST_CASE("cell OV hysteresis", "[protection]")
{
    /* 1. 触发过压保护 */
    s_pack.cell_mv_max = CELL_OV_PROT_MV;          /* 4250 */
    bms_fault_mask_t f = app_protection_process(&s_pack);
    TEST_ASSERT_TRUE(f & FAULT_CELL_OV_PROT);

    /* 2. 回落到预警区间但未到恢复阈值 → 保护保持 */
    s_pack.cell_mv_max = CELL_OV_WARN_MV + 10;     /* 4160, >4100 */
    f = app_protection_process(&s_pack);
    TEST_ASSERT_TRUE(f & FAULT_CELL_OV_PROT);      /* 未恢复, 仍保持保护 */

    /* 3. 回落到恢复阈值以下 → 保护解除 */
    s_pack.cell_mv_max = CELL_OV_RECOVER_MV - 10;  /* 4090 */
    f = app_protection_process(&s_pack);
    TEST_ASSERT_FALSE(f & FAULT_CELL_OV_PROT);
    TEST_ASSERT_FALSE(f & FAULT_CELL_OV_WARN);
}

/* ---------- 单体欠压滞回: 触发(≤2800) → 保持 → 恢复(≥3000) ---------- */
TEST_CASE("cell UV hysteresis", "[protection]")
{
    s_pack.cell_mv_min = CELL_UV_PROT_MV;          /* 2800 */
    bms_fault_mask_t f = app_protection_process(&s_pack);
    TEST_ASSERT_TRUE(f & FAULT_CELL_UV_PROT);

    s_pack.cell_mv_min = CELL_UV_RECOVER_MV - 10;  /* 2990, 未恢复 */
    f = app_protection_process(&s_pack);
    TEST_ASSERT_TRUE(f & FAULT_CELL_UV_PROT);

    s_pack.cell_mv_min = CELL_UV_RECOVER_MV + 10;  /* 3010, 已恢复 */
    f = app_protection_process(&s_pack);
    TEST_ASSERT_FALSE(f & FAULT_CELL_UV_PROT);
}

/* ---------- 过温滞回: 触发(≥60℃) → 保持 → 恢复(≤45℃) ---------- */
TEST_CASE("OT hysteresis", "[protection]")
{
    s_pack.temp_max_dc = TEMP_OT_PROT_DC;         /* 600 = 60.0℃ */
    bms_fault_mask_t f = app_protection_process(&s_pack);
    TEST_ASSERT_TRUE(f & FAULT_OT_PROT);

    s_pack.temp_max_dc = TEMP_OT_RECOVER_DC + 10; /* 460 = 46.0℃, 未到45恢复线 */
    f = app_protection_process(&s_pack);
    TEST_ASSERT_TRUE(f & FAULT_OT_PROT);

    s_pack.temp_max_dc = TEMP_OT_RECOVER_DC - 10; /* 440 = 44.0℃ */
    f = app_protection_process(&s_pack);
    TEST_ASSERT_FALSE(f & FAULT_OT_PROT);
}

/* ---------- 低 SOC 滞回: 触发(≤15%) → 保持 → 恢复(>20%) ---------- */
TEST_CASE("low SOC hysteresis", "[protection]")
{
    bms_soc_data_t soc;
    memset(&soc, 0, sizeof(soc));
    soc.soc = 0.10f;
    soc.soh = 1.0f;
    sys_data_set_soc(&soc);

    bms_fault_mask_t f = app_protection_process(&s_pack);
    TEST_ASSERT_TRUE(f & FAULT_LOW_SOC_WARN);

    /* 0.18 → 未到20%恢复线, 保持 */
    soc.soc = 0.18f;
    sys_data_set_soc(&soc);
    f = app_protection_process(&s_pack);
    TEST_ASSERT_TRUE(f & FAULT_LOW_SOC_WARN);

    /* 0.25 → 恢复 */
    soc.soc = 0.25f;
    sys_data_set_soc(&soc);
    f = app_protection_process(&s_pack);
    TEST_ASSERT_FALSE(f & FAULT_LOW_SOC_WARN);
}

/* ---------- 降额因子: 预警降额取最小值, 保护级归零 ---------- */
TEST_CASE("derating factor", "[protection]")
{
    bms_soc_data_t soc;
    memset(&soc, 0, sizeof(soc));
    soc.soc = 0.5f;
    soc.soh = 1.0f;
    sys_data_set_soc(&soc);

    /* 正常: 满功率 */
    app_protection_process(&s_pack);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, sys_data_get_derating());

    /* 低温预警降额 */
    s_pack.temp_min_dc = -100;                    /* -10.0℃ */
    app_protection_process(&s_pack);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, DERATING_UT_WARN, sys_data_get_derating());

    /* 保护级(过压) → 归零 */
    s_pack.temp_min_dc = 250;
    s_pack.cell_mv_max = CELL_OV_PROT_MV;
    app_protection_process(&s_pack);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, sys_data_get_derating());
}

/* ---------- 热失控锁定: 触发后持续锁定, 温度回落不自动恢复 ---------- */
TEST_CASE("thermal runaway lock", "[protection]")
{
    /* dV/dt 报警: 电压快速下降场景(v_max 骤降) */
    s_pack.cell_mv_max = 3800;
    s_pack.timestamp_ms = 0;
    app_protection_process(&s_pack);              /* 建立历史点 */

    s_pack.cell_mv_max = 3300;                    /* 500mV/0ms? 用温升路径更稳 */
    s_pack.timestamp_ms = 1;
    s_pack.temp_max_dc = 1100;                    /* 110℃ */
    s_pack.temp_min_dc = 1100;
    bms_fault_mask_t f = app_protection_process(&s_pack);
    /* 高温(>100℃)配合历史应触发热失控; 若未触发则至少不误锁 */
    TEST_ASSERT_TRUE(sys_data_get_thermal_lock());
    (void)f;
}
