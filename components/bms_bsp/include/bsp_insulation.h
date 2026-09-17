/**
 * @file    bsp_insulation.h
 * @brief   绝缘检测驱动接口(不平衡电桥法)
 * @author  BMS Team
 * @date    2026-08
 * @note    对标 GB/T 38661 / GB/T 18384.1(直流绝缘电阻 >100Ω/V)
 *          当前硬件未接线(HW_ENABLE_INSULATION_DETECT=0)时全部接口安全返回,
 *          软件算法框架已就绪, 接线后置 1 即启用
 * 2026-08-09: 新增不平衡电桥绝缘检测(方案详见 docs/企业级BMS标准对标报告.md)
 */
#ifndef BSP_INSULATION_H
#define BSP_INSULATION_H

#include <stdint.h>
#include <stdbool.h>
#include "bms_errno.h"

/**
 * @brief   初始化绝缘检测(硬件屏蔽时直接返回, 不阻塞系统)
 * @retval  BMS_OK 成功 / BMS_ERR_NOT_INIT 硬件未启用
 */
bms_err_t bsp_insulation_init(void);

/**
 * @brief   读取正/负极母线对地绝缘电阻(不平衡电桥法, 两次测量解算)
 * @param   pack_v_mv       电池组总压, 单位 mV
 * @param   rp_ohm          输出: 正极母线对地绝缘电阻, 单位 Ω
 * @param   rn_ohm          输出: 负极母线对地绝缘电阻, 单位 Ω
 * @retval  BMS_OK 成功 / BMS_ERR_NOT_INIT 未启用 / BMS_ERR_CRC 测量无效
 * @note    硬件未接时输出 0 并返回 BMS_ERR_NOT_INIT(上层显示"未启用")
 */
bms_err_t bsp_insulation_read(uint32_t pack_v_mv, uint32_t *rp_ohm, uint32_t *rn_ohm);

/**
 * @brief   纯算法: 由两次桥臂测量电压解算 Rp/Rn(无硬件依赖, 可单测)
 * @param   pack_v_mv   电池组总压 mV
 * @param   vc1_mv      第一次测量: 正负极桥臂 R0/R0 时的对地电压 mV
 * @param   vc2_mv      第二次测量: 正极桥臂 2*R0、负极 R0 时的对地电压 mV
 * @param   bridge_ohm  桥臂基础电阻 R0, 单位 Ω
 * @param   rp_ohm      输出正极绝缘电阻 Ω
 * @param   rn_ohm      输出负极绝缘电阻 Ω
 * @retval  true  解算有效 / false 测量无效(电压为 0/短路)
 */
bool bsp_insulation_solve(uint32_t pack_v_mv, uint32_t vc1_mv, uint32_t vc2_mv,
                          uint32_t bridge_ohm, uint32_t *rp_ohm, uint32_t *rn_ohm);

#endif // BSP_INSULATION_H
