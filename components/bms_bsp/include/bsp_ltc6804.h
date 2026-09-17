/**
 * @file    bsp_ltc6804.h
 * @brief   LTC6804-2 AFE 驱动接口(12串电压采集 + 被动均衡 + GPIO温度)
 * @author  BMS Team
 * @date    2026-08
 * @note    SPI Mode3(CPOL=1,CPHA=1), 最大 1MHz, PEC15 校验
 * 2026-08-09: 多芯片驱动(方案A: 共享 SPI2 + 独立片选, LTC6804_CHIP_NUM 控制芯片数)
 *             接口不变, 应用层无需改动; 芯片内部自动拆分掩码/拼接串号
 */
#ifndef BSP_LTC6804_H
#define BSP_LTC6804_H

#include <stdint.h>
#include <stdbool.h>
#include "bms_errno.h"
#include "bms_config.h"

/**
 * @brief   初始化 LTC6804(SPI 总线 + 逐芯片片选 + 唤醒)
 * @note    多芯片时初始化全部 LTC6804_CHIP_NUM 片; 单芯片失败不影响其他芯片
 * @retval  BMS_OK 成功, 其他见 bms_err_t
 */
bms_err_t bsp_ltc6804_init(void);

/**
 * @brief   启动电压 ADC 转换并阻塞读取所有串电压
 * @param   cell_mv  输出缓冲区, 单位 mV, 长度 >= BMS_CELL_SERIES_NUM
 * @note    多芯片: ADC 并发启动, 串号按 chip*12 + 组内序号 全局拼接,
 *          超出 BMS_CELL_SERIES_NUM 的部分保持 0(由上层截断)
 * @retval  BMS_OK 成功, BMS_ERR_CRC 校验失败, BMS_ERR_TIMEOUT 转换超时
 */
bms_err_t bsp_ltc6804_read_voltages(uint16_t *cell_mv);

/**
 * @brief   读取 LTC6804 GPIO 辅助寄存器(用于 NTC 温度分压电压)
 * @param   gpio_mv  输出缓冲区, 单位 mV, 长度 >= LTC6804_CHIP_NUM*5
 *          (每芯片 GPIO1~GPIO5 共 5 路, 写入 gpio_mv[chip*5 + i])
 * @note    遍历所有已初始化芯片, 支持多芯片级联每片独立 NTC 测温;
 *          单芯片(LTC6804_CHIP_NUM=1)时仅读取芯片 0, 行为与原版一致
 * @retval  BMS_OK 成功, BMS_ERR_FAIL 无已初始化芯片
 */
bms_err_t bsp_ltc6804_read_gpio(uint16_t *gpio_mv);

/**
 * @brief   设置被动均衡开关掩码
 * @param   mask  位掩码, bit0~bit11 对应 DCC1~DCC12, 1=均衡导通
 *                (uint32 位宽, 最多 32 串; 多芯片时按每芯片 12 位自动拆分)
 * @retval  BMS_OK 成功
 */
bms_err_t bsp_ltc6804_set_balance(uint32_t mask);

/**
 * @brief   唤醒 LTC6804(退出空闲模式)
 * @note    多芯片时唤醒全部已初始化芯片
 */
void bsp_ltc6804_wakeup(void);

#endif // BSP_LTC6804_H
