/**
 * @file    bsp_bq76952.h
 * @brief   BQ76952 AFE 驱动接口(3~16串电压采集 + 内置被动均衡 + TS温度 + 库仑计电流 + FET控制)
 * @author  BMS Team
 * @date    2026-08
 * @note    I2C 总线(I2C_NUM_0, 2026-08-25 起总线初始化迁至此驱动, 原 bsp_oled_init 屏蔽),
 *          设备地址 0x08 (BMS-C1 仿制板 ADDR 接法)
 *          替代 LTC6804 方案: LTC6804 驱动保留, 由 HW_ENABLE_* 宏二选一激活
 *          参考: TI BQ76952 TRM (SLUUBY2B)
 *          接口与 bsp_ltc6804 保持一致, 应用层仅需切换编译宏, 调用逻辑无需改动
 */
#ifndef BSP_BQ76952_H
#define BSP_BQ76952_H

#include <stdint.h>
#include <stdbool.h>
#include "bms_errno.h"
#include "bms_config.h"

/**
 * @brief   初始化 BQ76952(I2C 探测 + 唤醒)
 * @note    I2C 总线由 bsp_oled_init 初始化, 此处仅探测设备在线(与 bsp_bq34z100 同策略)
 * @retval  BMS_OK 成功, 其他见 bms_err_t
 */
bms_err_t bsp_bq76952_init(void);

/**
 * @brief   读取所有单体电压(VC1~VCn, n=BMS_CELL_SERIES_NUM)
 * @param   cell_mv  输出缓冲区, 单位 mV, 长度 >= BMS_CELL_SERIES_NUM
 * @note    BQ76952 原生 3~16S, 实际串数由 BMS_CELL_SERIES_NUM 控制(UI 可下发 1~16 切换)
 *          只填充 [0, n) 区间, 超出部分保持 0; n 被钳制到 <=16 防止读越界
 * @retval  BMS_OK 成功, BMS_ERR_I2C 通信失败, BMS_ERR_NOT_INIT 未初始化
 */
bms_err_t bsp_bq76952_read_voltages(uint16_t *cell_mv);

/**
 * @brief   读取 TS 温度通道(TS1~TS3, 命令 0x70/0x72/0x74)
 * @param   temp_dc  输出缓冲区, 单位 0.1℃, 长度 >= 3
 * @note    BMS-C1 仿制板: TS 引脚配置为 thermistor 模式(18k 上拉 NTC,
 *          ts1/ts3-pin-config=0x07), 温度命令返回 0.1K, 此处换算为 0.1℃
 *          (与 LTC6804 方案 bsp_temp_calc_dc 输出的 temp_dc 单位一致)
 * @retval  BMS_OK 成功, BMS_ERR_NOT_INIT 未初始化
 */
bms_err_t bsp_bq76952_read_temp(int16_t *temp_dc);

/**
 * @brief   读取 TS 温度通道分压电压(TS1~TS3, 命令 0x70/0x72/0x74)
 * @param   gpio_mv  输出缓冲区, 单位 mV, 长度 >= 3
 * @note    需将 TS 引脚配置为 ADCIN 模式(Settings:Configuration:TSx Config = no pull-up),
 *          命令才返回引脚电压(mV); 默认 thermistor 模式返回 0.1K 温度, 不可直接当电压
 *          分压电压经 bsp_temp_calc_dc() 换算 NTC 温度(与 LTC6804 GPIO 温压同源处理)
 * @retval  BMS_OK 成功, BMS_ERR_NOT_INIT 未初始化
 */
bms_err_t bsp_bq76952_read_gpio(uint16_t *gpio_mv);

/**
 * @brief   设置被动均衡开关掩码(BQ76952 内置均衡 MOS 直接驱动)
 * @param   mask  位掩码, bit0~bit15 对应 cell1~cell16, 1=均衡导通
 * @retval  BMS_OK 成功
 */
bms_err_t bsp_bq76952_set_balance(uint32_t mask);

/**
 * @brief   唤醒 BQ76952(退出 SHUTDOWN/SLEEP)
 * @note    通过 WAKE(LD) 引脚拉高唤醒; 若引脚未接则依赖 I2C 访问自唤醒
 */
void bsp_bq76952_wakeup(void);

/**
 * @brief   读取库仑计电流(CC2, 内置分流采样)
 * @param   ma  输出电流, 单位 mA; 正=放电, 负=充电
 * @note    2026-08-25 新增: BMS-C1 仿制板无 INA181, 电流改由 BQ76952 内置库仑计提供,
 *          读取 0x3A (CC2 Current, 16-bit signed, 单位 µA) 后换算为 mA
 * @retval  BMS_OK 成功, BMS_ERR_I2C 通信失败, BMS_ERR_NOT_INIT 未初始化
 */
bms_err_t bsp_bq76952_read_current_ma(int16_t *ma);

/**
 * @brief   控制 CHG/DSG FET(功率开关, 2026-08-25 替代继电器 GPIO)
 * @param   chg_on  true=允许充电 FET 导通, false=强制关闭充电 FET
 * @param   dsg_on  true=允许放电 FET 导通, false=强制关闭放电 FET
 * @note    通过 FET 子命令控制 (ALL_FETS_ON 0x0096 / ALL_FETS_OFF 0x0095 /
 *          CHG_PCHG_OFF 0x0094 / DSG_PDSG_OFF 0x0093);
 *          BQ76952 保护功能(OV/UV/OC)仍可独立关断 FET
 * @retval  BMS_OK 成功, BMS_ERR_I2C 通信失败, BMS_ERR_NOT_INIT 未初始化
 */
bms_err_t bsp_bq76952_set_fet(bool chg_on, bool dsg_on);

/**
 * @brief   写回 BMS-C1 仿制板硬件配置(2026-08-25 新增)
 * @note    与 LibreSolar bms-c1.dts + bms_c1_0_4_0.overlay 一致:
 *          TS1/TS3=NTC(18k 上拉, 0x07), DCHG=FET 温度(0x0F),
 *          DFETOFF=分流温度(0x07, v0.4), VCELL_MODE=16S 差分.
 *          需先 bsp_bq76952_init 成功; 失败仅告警不阻塞(出厂可能已配置).
 * @retval  BMS_OK 成功
 */
bms_err_t bsp_bq76952_apply_config(void);

/**
 * @brief   查询 BQ76952 ALERT 事件(2026-08-25 新增, 仿 C1)
 * @note    ALERT 引脚(开漏+外部10k上拉, 高电平=有事件)上升沿中断置位,
 *          本函数查询后自动清零; 采集任务可周期调用作为轮询双保险.
 * @retval  true=有待处理事件
 */
bool bsp_bq76952_alert_pending(void);

#endif // BSP_BQ76952_H
