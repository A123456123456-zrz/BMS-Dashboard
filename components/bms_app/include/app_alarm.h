/**
 * @file    app_alarm.h
 * @brief   报警管理器(集中管理 LED/蜂鸣器/OLED 分级报警)
 * @author  BMS Team
 * @date    2026-08
 * @note    根据故障掩码分级驱动报警输出设备:
 *          - 正常:   LED 1Hz 心跳, 蜂鸣器静音
 *          - 预警级: LED 2Hz 慢闪,  蜂鸣器短鸣 100ms + 静音 1900ms
 *          - 保护级: LED 4Hz 快闪,  蜂鸣器鸣 200ms  + 静音 800ms
 *          - 严重级: LED 常亮,      蜂鸣器急促鸣 100ms + 静音 100ms
 *          严重级 = 热失控(FAULT_THERMAL_RUN) + 短路(FAULT_SHORT)
 *          保护级 = OV/UV/CHG_OC/DSG_OC/OT/UT 的 _PROT 位
 *          预警级 = OV/UV/CHG_OC/DSG_OC/OT 的 _WARN 位
 *          调用方: task_alarm 每 100ms 调用 app_alarm_process()
 */
#ifndef APP_ALARM_H
#define APP_ALARM_H

#include "bms_types.h"

/**
 * @brief   报警等级枚举(由故障掩码映射而来)
 */
typedef enum {
    ALARM_LEVEL_NORMAL = 0,                             // 正常(无故障)
    ALARM_LEVEL_WARN,                                   // 预警级
    ALARM_LEVEL_PROT,                                   // 保护级
    ALARM_LEVEL_FATAL,                                  // 严重级(热失控/短路)
} alarm_level_e;

/**
 * @brief   初始化报警管理器
 * @note    内部读取当前故障掩码作为初始状态
 */
void app_alarm_init(void);

/**
 * @brief   报警周期处理(100ms 调用一次)
 * @note    读取 sys_data 的故障掩码, 驱动 LED 和蜂鸣器
 *          LED: 按等级输出不同闪烁频率
 *          蜂鸣器: 按等级输出不同间歇鸣叫模式
 */
void app_alarm_process(void);

/**
 * @brief   获取当前报警等级(供 OLED 显示用)
 * @return  当前报警等级
 */
alarm_level_e app_alarm_get_level(void);

/**
 * @brief   获取当前故障的简短文字描述(供 OLED 显示用)
 * @param   buf   输出缓冲区
 * @param   size  缓冲区大小
 * @return  故障文字(如 "OV-PROT" / "THERMAL!" / "OK")
 */
const char *app_alarm_get_text(char *buf, uint8_t size);

#endif // APP_ALARM_H
