/**
 * @file    app_alarm.c
 * @brief   报警管理器实现(LED/蜂鸣器分级报警)
 * @author  BMS Team
 * @date    2026-08
 * @note    设计要点:
 *          1. 单一职责: 本模块只管"报警输出", 不做故障检测(由 app_protection 负责)
 *          2. 分级驱动: 故障掩码 → 报警等级 → LED/蜂鸣器动作模式
 *          3. 间歇鸣叫: 用 100ms 计数器实现周期切换, 避免常响刺耳
 *          4. 硬件屏蔽: LED/蜂鸣器屏蔽时跳过 GPIO 操作, 不阻塞逻辑
 *          5. 线程安全: 只读 sys_data 的 fault, 无需加锁(单字节原子读)
 */
#include "app_alarm.h"
#include "bms_config.h"
#include "sys_data.h"
#include "bsp_led.h"
#include "bsp_buzzer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "APP_ALARM";

/* ====== 故障掩码分级定义 ====== */
#define FAULT_MASK_FATAL  (FAULT_THERMAL_RUN | FAULT_SHORT | FAULT_SAMPLE_FAIL)    // 严重级
#define FAULT_MASK_PROT   (FAULT_CELL_OV_PROT | FAULT_CELL_UV_PROT |             \
                           FAULT_CHG_OC_PROT  | FAULT_DSG_OC_PROT  |             \
                           FAULT_OT_PROT      | FAULT_UT_PROT      |             \
                           FAULT_INSULATION_PROT)                                // 保护级(H12: 补绝缘保护)
#define FAULT_MASK_WARN   (FAULT_CELL_OV_WARN  | FAULT_CELL_UV_WARN |            \
                           FAULT_CHG_OC_WARN   | FAULT_DSG_OC_WARN  |            \
                           FAULT_OT_WARN       | FAULT_CELL_DV_WARN |            \
                           FAULT_UT_WARN       | FAULT_DTDT_WARN   |             \
                           FAULT_OVERLOAD_WARN | FAULT_LOW_SOC_WARN |            \
                           FAULT_LOW_SOH_WARN  | FAULT_COMM_WARN   |             \
                           FAULT_INSULATION_WARN)                                // 预警级(H12: 补绝缘预警)

/* ====== 报警周期参数(单位: 100ms tick) ======
 * LED 闪烁模式(单白灯, 用亮度+频率双重区分等级, 1 tick = 100ms):
 *   正常  短脉冲:  亮 1 + 灭 9     (1Hz, 10%占空比, 暗淡心跳)
 *   预警  半亮闪:  亮 5 + 灭 5     (1Hz, 50%占空比, 明显慢闪)
 *   保护  快闪:    亮 1 + 灭 1     (5Hz, 50%占空比, 急促快闪)
 *   严重  常亮:    亮 不灭         (100%占空比, 紧急)
 * 区分度: 占空比 10% → 50% → 50%+高频 → 100%, 亮度和频率递增
 * 蜂鸣器鸣叫/静音周期(tick 数):
 *   预警级: 2 秒嘀一声 (鸣 100ms + 静音 1900ms)
 *   保护级: 1s 周期   (鸣 200ms + 静音 800ms)
 *   严重级: 急促      (鸣 100ms + 静音 100ms) */

/* LED 闪烁模式表: 每个 tick 的目标状态(亮/灭), 循环播放 */
static const bool LED_PATTERN_WARN[10]   = {1,1,1,1,1,0,0,0,0,0};        /* 半亮闪: 亮5 灭5 (明显) */
static const bool LED_PATTERN_PROT[2]    = {1,0};                        /* 快闪: 亮1 灭1 (5Hz急促) */
#define LED_PATTERN_WARN_LEN     10
#define LED_PATTERN_PROT_LEN     2

#define BUZZ_WARN_ON_TICKS        1       // 预警:   鸣 100ms (嘀)
#define BUZZ_WARN_OFF_TICKS      19       // 预警:   静音 1900ms (2 秒一声慢嘀)
#define BUZZ_PROT_ON_TICKS        2       // 保护:   鸣 200ms
#define BUZZ_PROT_OFF_TICKS       8       // 保护:   静音 800ms (周期 1s)
#define BUZZ_FATAL_ON_TICKS       1       // 严重:   鸣 100ms
#define BUZZ_FATAL_OFF_TICKS      1       // 严重:   静音 100ms (周期 200ms, 急促)

/* ====== 模块内部状态 ======
 * s_level        当前报警等级
 * s_tick         100ms 计数器(用于周期切换)
 * s_led_state    LED 当前电平(true=亮)
 * s_buzz_state   蜂鸣器当前状态(true=鸣) */
static alarm_level_e s_level      = ALARM_LEVEL_NORMAL;
static uint8_t        s_tick       = 0;
static bool           s_led_state  = false;
static bool           s_buzz_state = false;

/* ====== 内部函数声明 ====== */
static alarm_level_e fault_to_level(bms_fault_mask_t fault);
static void update_led(void);
static void update_buzzer(void);

/**
 * @brief   故障掩码 → 报警等级映射
 * @param   fault  故障掩码
 * @return  最高优先级的报警等级
 * @note    优先级: FATAL > PROT > WARN > NORMAL
 */
static alarm_level_e fault_to_level(bms_fault_mask_t fault)
{
    if (fault & FAULT_MASK_FATAL) {
        return ALARM_LEVEL_FATAL;
    }
    if (fault & FAULT_MASK_PROT) {
        return ALARM_LEVEL_PROT;
    }
    if (fault & FAULT_MASK_WARN) {
        return ALARM_LEVEL_WARN;
    }
    return ALARM_LEVEL_NORMAL;
}

/**
 * @brief   驱动 LED 按当前等级闪烁(三灯: 红=GPIO1 绿=GPIO2 白=GPIO38共阳)
 * @note    NORMAL: 绿常亮, 白灭
 *          WARN:   白灯慢闪(1Hz)
 *          PROT:   红灯快闪(5Hz) + 白灯快闪(5Hz, 同步)
 *          FATAL:  红灯常亮 + 白灯常亮
 */
static void update_led(void)
{
    const bool *pattern = NULL;
    uint8_t     pattern_len = 0;
    bool        new_state;

    switch (s_level) {
    case ALARM_LEVEL_FATAL:
        bsp_led_set_color(BSP_LED_RED);
        bsp_led_white(true);                              /* 白灯常亮 */
        s_led_state = true;
        return;

    case ALARM_LEVEL_PROT:
        pattern     = LED_PATTERN_PROT;
        pattern_len = LED_PATTERN_PROT_LEN;
        break;

    case ALARM_LEVEL_WARN:
        pattern     = LED_PATTERN_WARN;
        pattern_len = LED_PATTERN_WARN_LEN;
        break;

    case ALARM_LEVEL_NORMAL:
    default:
        bsp_led_set_color(BSP_LED_GREEN);                /* 绿灯常亮 */
        bsp_led_white(false);                              /* 白灯灭 */
        return;
    }

    new_state = pattern[s_tick % pattern_len];
    if (new_state != s_led_state) {
        s_led_state = new_state;
        /* PROT/WARN 时白灯随模式同步闪烁; NORMAL 时绿灯走模式 */
        bsp_led_set_color(s_level == ALARM_LEVEL_PROT ? BSP_LED_RED : BSP_LED_OFF);
        bsp_led_white(new_state);                          /* 白灯跟随 pattern */
    }
}

/**
 * @brief   驱动蜂鸣器按当前等级间歇鸣叫
 * @note    NORMAL: 静音
 *          WARN:   鸣 100ms + 静音 1900ms (周期 2s)
 *          PROT:   鸣 200ms + 静音 800ms  (周期 1s)
 *          FATAL:  鸣 100ms + 静音 100ms  (周期 200ms, 急促)
 */
static void update_buzzer(void)
{
    uint8_t on_ticks;
    uint8_t off_ticks;
    uint8_t period;

    if (s_level == ALARM_LEVEL_NORMAL) {
        if (s_buzz_state) {
            bsp_buzzer_set(false);                     // 正常: 静音
            s_buzz_state = false;
        }
        return;
    }

    /* 按等级选择鸣叫/静音时长 */
    switch (s_level) {
    case ALARM_LEVEL_WARN:
        on_ticks  = BUZZ_WARN_ON_TICKS;
        off_ticks = BUZZ_WARN_OFF_TICKS;
        break;

    case ALARM_LEVEL_PROT:
        on_ticks  = BUZZ_PROT_ON_TICKS;
        off_ticks = BUZZ_PROT_OFF_TICKS;
        break;

    case ALARM_LEVEL_FATAL:
        on_ticks  = BUZZ_FATAL_ON_TICKS;
        off_ticks = BUZZ_FATAL_OFF_TICKS;
        break;

    default:
        on_ticks  = 0;
        off_ticks = 1;
        break;
    }

    period = (uint8_t)(on_ticks + off_ticks);
    uint8_t phase = (uint8_t)(s_tick % period);

    bool new_state = (phase < on_ticks);               // 在 on 区间内则鸣叫
    if (new_state != s_buzz_state) {
        s_buzz_state = new_state;
        bsp_buzzer_set(new_state);
    }
}

void app_alarm_init(void)
{
    s_level      = ALARM_LEVEL_NORMAL;
    s_tick       = 0;
    s_led_state  = false;
    s_buzz_state = false;

    /* 初始状态: LED 灭, 蜂鸣器静音 */
    bsp_led_set(false);
    bsp_led_white(false);
    bsp_buzzer_set(false);

    ESP_LOGI(TAG, "alarm init (LED=%d BUZZER=%d)",
             HW_ENABLE_LED, HW_ENABLE_BUZZER);
}

void app_alarm_process(void)
{
    /* 读取当前故障掩码, 映射到报警等级 */
    bms_fault_mask_t fault = sys_data_get_fault();
    alarm_level_e new_level = fault_to_level(fault);

    /* 等级变化时打印日志(便于调试) */
    if (new_level != s_level) {
        ESP_LOGW(TAG, "level %d->%d fault=0x%08lX",
                 s_level, new_level, (unsigned long)fault);
        s_level = new_level;
    }

    /* 驱动 LED 和蜂鸣器 */
    update_led();
    update_buzzer();

    /* 计数器自增(8 位回绕, 0~255, 够用) */
    s_tick++;
}

alarm_level_e app_alarm_get_level(void)
{
    return s_level;
}

const char *app_alarm_get_text(char *buf, uint8_t size)
{
    if (buf == NULL || size == 0) {
        return "";
    }

    bms_fault_mask_t fault = sys_data_get_fault();

    /* 严重级优先(取第一个命中的) */
    if (fault & FAULT_THERMAL_RUN) {
        strncpy(buf, "THERMAL!", size);
    } else if (fault & FAULT_SHORT) {
        strncpy(buf, "SHORT!", size);
    } else if (fault & FAULT_SAMPLE_FAIL) {
        strncpy(buf, "SAMPLE!", size);                    // 电流采样失效(严重)
    } else if (fault & FAULT_CELL_OV_PROT) {
        strncpy(buf, "OV-PROT", size);
    } else if (fault & FAULT_CELL_UV_PROT) {
        strncpy(buf, "UV-PROT", size);
    } else if (fault & FAULT_CHG_OC_PROT) {
        strncpy(buf, "CHG-OC", size);
    } else if (fault & FAULT_DSG_OC_PROT) {
        strncpy(buf, "DSG-OC", size);
    } else if (fault & FAULT_OT_PROT) {
        strncpy(buf, "OT-PROT", size);
    } else if (fault & FAULT_UT_PROT) {
        strncpy(buf, "UT-PROT", size);
    } else if (fault & FAULT_INSULATION_PROT) {
        strncpy(buf, "ISO-PROT", size);                    // 绝缘电阻保护(<50Ω/V)
    } else if (fault & FAULT_DTDT_WARN) {
        strncpy(buf, "dT/dt!", size);                    // 温升速率预警(热失控前兆)
    } else if (fault & FAULT_OVERLOAD_WARN) {
        strncpy(buf, "OVERLD!", size);                   // 持续过载预警
    } else if (fault & FAULT_CELL_DV_WARN) {
        strncpy(buf, "DV-WARN", size);                   // 压差过大预警
    } else if (fault & FAULT_UT_WARN) {
        strncpy(buf, "UT-WARN", size);                   // 低温预警
    } else if (fault & FAULT_LOW_SOC_WARN) {
        strncpy(buf, "LOW-SOC", size);                   // 低SOC预警
    } else if (fault & FAULT_LOW_SOH_WARN) {
        strncpy(buf, "LOW-SOH", size);                   // SOH衰减预警
    } else if (fault & FAULT_COMM_WARN) {
        strncpy(buf, "COMM!", size);                     // 通信异常预警
    } else if (fault & FAULT_INSULATION_WARN) {
        strncpy(buf, "ISO-WARN", size);                  // 绝缘电阻预警(<100Ω/V)
    } else if (fault & FAULT_CELL_OV_WARN) {
        strncpy(buf, "OV-WARN", size);
    } else if (fault & FAULT_CELL_UV_WARN) {
        strncpy(buf, "UV-WARN", size);
    } else if (fault & FAULT_CHG_OC_WARN) {
        strncpy(buf, "CHG-WARN", size);
    } else if (fault & FAULT_DSG_OC_WARN) {
        strncpy(buf, "DSG-WARN", size);
    } else if (fault & FAULT_OT_WARN) {
        strncpy(buf, "OT-WARN", size);
    } else {
        strncpy(buf, "OK", size);
    }

    /* 确保字符串终止 */
    buf[size - 1] = '\0';
    return buf;
}
