/**
 * @file    test_bsp_insulation.c
 * @brief   2026-08-09 单元测试: 绝缘检测不平衡电桥解算算法
 * @note    bsp_insulation_solve() 为纯算法(无硬件依赖), 直接单测:
 *          - 已知绝缘电阻 -> 反推桥臂电压 -> 再解算应还原原值
 *          - 无效输入(总压0/桥臂电压0/越界/桥臂电阻0)返回 false
 *          - 对称接地(上下绝缘相等)与单侧接地两种典型工况
 * 说明: 不依赖 HW_ENABLE_INSULATION_DETECT(该宏只控制 init/read 硬件路径)
 */
#include <string.h>
#include "unity.h"
#include "bsp_insulation.h"

/* ---------- 构造: 已知 Rp/Rn/R0/Vbat -> 计算两次桥臂电压 ----------
 * 第一次(上 R0 下 R0): Vc1 = Vbat * (R0//Rn) / ((R0//Rp) + (R0//Rn))
 * 第二次(上 2R0 下 R0): Vc2 = Vbat * (R0//Rn) / ((2R0//Rp) + (R0//Rn))
 * 单位: mV / Ω */
static void calc_bridge_v(uint32_t vbat_mv, uint32_t r0_ohm,
                          uint32_t rp_ohm, uint32_t rn_ohm,
                          uint32_t *vc1_mv, uint32_t *vc2_mv)
{
    float R0 = (float)r0_ohm;
    float Rp = (float)rp_ohm;
    float Rn = (float)rn_ohm;
    float Vb = (float)vbat_mv;

    float Ra1 = R0 * Rp / (R0 + Rp);          /* 第一次上臂: R0//Rp */
    float Rb  = R0 * Rn / (R0 + Rn);          /* 下臂(两次相同): R0//Rn */
    float Ra2 = (2.0f * R0) * Rp / (2.0f * R0 + Rp);  /* 第二次上臂: 2R0//Rp */

    *vc1_mv = (uint32_t)(Vb * Rb / (Ra1 + Rb) + 0.5f);
    *vc2_mv = (uint32_t)(Vb * Rb / (Ra2 + Rb) + 0.5f);
}

/* ---------- 通用: 解算并断言还原(相对误差 <= 10%) ---------- */
static void assert_solve_restores(uint32_t vbat_mv, uint32_t r0_ohm,
                                  uint32_t rp_expect, uint32_t rn_expect)
{
    uint32_t vc1, vc2;
    calc_bridge_v(vbat_mv, r0_ohm, rp_expect, rn_expect, &vc1, &vc2);

    uint32_t rp = 0, rn = 0;
    bool ok = bsp_insulation_solve(vbat_mv, vc1, vc2, r0_ohm, &rp, &rn);
    TEST_ASSERT_TRUE_MESSAGE(ok, "解算应成功");

    /* 相对误差 10% 容差(桥臂电压取整引入误差) */
    TEST_ASSERT_FLOAT_WITHIN(0.10f * rp_expect, (float)rp_expect, (float)rp);
    TEST_ASSERT_FLOAT_WITHIN(0.10f * rn_expect, (float)rn_expect, (float)rn);
}

/* ---------- 对称绝缘(上下均 10kΩ, 36V 系统) ---------- */
TEST_CASE("insulation solve symmetric ground", "[insulation]")
{
    assert_solve_restores(36000, 1000000, 10000, 10000);
}

/* ---------- 正极接地(单侧绝缘劣化) ---------- */
TEST_CASE("insulation solve positive grounded", "[insulation]")
{
    assert_solve_restores(36000, 1000000, 2000, 1000000);
}

/* ---------- 负极接地(另一侧绝缘劣化) ---------- */
TEST_CASE("insulation solve negative grounded", "[insulation]")
{
    assert_solve_restores(36000, 1000000, 1000000, 3000);
}

/* ---------- 高阻开路(绝缘良好, 500kΩ) ---------- */
TEST_CASE("insulation solve healthy insulation", "[insulation]")
{
    assert_solve_restores(36000, 1000000, 500000, 500000);
}

/* ---------- 不同系统电压(48V) ---------- */
TEST_CASE("insulation solve 48V system", "[insulation]")
{
    assert_solve_restores(48000, 1000000, 20000, 20000);
}

/* ---------- 无效输入应返回 false ---------- */
TEST_CASE("insulation solve rejects invalid input", "[insulation]")
{
    uint32_t rp = 0, rn = 0;

    /* 总压为 0 */
    TEST_ASSERT_FALSE(bsp_insulation_solve(0, 18000, 17955, 1000000, &rp, &rn));
    /* 桥臂电压为 0 */
    TEST_ASSERT_FALSE(bsp_insulation_solve(36000, 0, 17955, 1000000, &rp, &rn));
    /* 桥臂电压越界(>= 总压) */
    TEST_ASSERT_FALSE(bsp_insulation_solve(36000, 36000, 17955, 1000000, &rp, &rn));
    /* 桥臂电阻为 0 */
    TEST_ASSERT_FALSE(bsp_insulation_solve(36000, 18000, 17955, 0, &rp, &rn));
    /* 空输出指针 */
    TEST_ASSERT_FALSE(bsp_insulation_solve(36000, 18000, 17955, 1000000, NULL, &rn));
    /* 两次测量相同(对称无法解, 理论上 k1==k2 -> B 无解) */
    TEST_ASSERT_FALSE(bsp_insulation_solve(36000, 18000, 18000, 1000000, &rp, &rn));
}
