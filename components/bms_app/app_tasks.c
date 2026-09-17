/**
 * @file    app_tasks.c
 * @brief   FreeRTOS 任务调度中心实现
 * @author  BMS Team
 * @date    2026-08
 * @note    系统初始化(BSP+Middleware+算法) + 创建全部业务任务
 *          采集/保护/SOC/均衡/通信/OLED/SD/OTA/看门狗 共 9 个任务
 */
#include "app_tasks.h"

/* 项目内头: 按层次从下到上 */
#include "bms_config.h"
#include "bms_types.h"
#include "bsp_ltc6804.h"
#if HW_ENABLE_BQ76952
#include "bsp_bq76952.h"
#endif
#include "bsp_current.h"
#include "bsp_insulation.h"
#include "bsp_temp.h"
#include "bsp_relay.h"
#include "bsp_oled.h"
#include "bsp_can.h"
#include "bsp_sd.h"
#include "bsp_led.h"
#include "bsp_buzzer.h"
#include "bsp_button.h"                                  /* GPIO0 BOOT 键: 独立配网长按检测 */
#include "bsp_sw_pwr.h"                                  /* GPIO13 电源键: 长按3s关功率(仿 C1) */
#include "bsp_bq34z100.h"
#include "sys_data.h"
#include "sys_filter.h"
#include "sys_fsm.h"
#include "sys_storage.h"
#include "sys_params.h"
#include "sys_wifi.h"
#include "sys_mqtt.h"
#include "sys_config_portal.h"                          /* HTTP 配网门户 */
#include "sys_can.h"                                    /* CAN 协议层(0x180~0x187) */
#include "sys_modbus.h"                                 /* Modbus RTU Slave(RS485) */
#include "app_protection.h"
#include "app_soc.h"
#include "app_balance.h"
#include "app_charge.h"
#include "app_ota.h"
#include "app_key_ui.h"                                 /* 按键 UI 管理器 (App 层) */
#include "app_alarm.h"                                  /* 报警管理器 (App 层) */
#include "app_reboot_guard.h"                        /* 重启幂等防护(独立模块) */

/* 标准库与 ESP-IDF */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs_flash.h"                                /* NVS Flash 初始化(sys_params/WiFi 配网依赖) */
#include "esp_wifi.h"                                 /* WiFi 停止/启动(STA 失败回退 AP 时用) */
#if __has_include("esp_attr.h")
#  include "esp_attr.h"                               /* RTC_DATA_ATTR 宏 */
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "cJSON.h"

static const char *TAG = "APP";


/* 任务存活标志(看门狗监控用, 9 个任务)
 * 每个任务在自己的循环中把对应标志置 true, 看门狗每秒检查后清零
 * 连续 3 秒未刷新则判定任务死亡, 触发看门狗复位 */
static volatile bool s_task_alive_acq     = false;   // 采集任务
static volatile bool s_task_alive_prot    = false;   // 保护任务
static volatile bool s_task_alive_soc     = false;   // SOC 估算任务
static volatile bool s_task_alive_bal     = false;   // 均衡任务
static volatile bool s_task_alive_comm    = false;   // 通信任务
static volatile bool s_task_alive_fast    = false;   // EMQX 高频上报任务(100ms, 2026-08-18)
static volatile bool s_task_alive_oled    = false;   // OLED 显示任务
static volatile bool s_task_alive_sd      = false;   // SD 日志任务
static volatile bool s_task_alive_ota     = false;   // OTA 任务
static volatile bool s_task_alive_alarm   = false;   // 报警任务
/* 看门狗任务自身不需监控 */

/* 电流低通滤波器 */
static sys_lpf_t s_current_lpf;

/* 实时电流值(mA)/ADC 原始值: task_acquisition/task_fast 写入,
 * OLED 页面与串口调试打印读取(留在门控外, 屏蔽 OLED 时仍正常编译) */
static volatile int16_t s_live_current_ma = 0;
static volatile uint16_t s_live_adc_raw = 0;

#if HW_ENABLE_OLED   /* OLED UI 整段: 屏蔽时全部编译剔除, 省 flash */
/* ================================================================
 * OLED 界面布局说明 (6x8 字库, 128x64 = 21列 x 8行)
 * 共 4 页, 每 2 秒自动切换一页
 *   Page0 [SYS]  系统概览: 总压/总电流/SOC/状态/单体范围
 *   Page1 [CELL] 6 串单体电压 + 均衡状态
 *   Page2 [TEMP] 温度 + 保护状态位
 *   Page3 [INFO] SOH/容量/充电参数/网络状态
 * 所有数据已切换为真实传感器: LTC6804 电压 + NTC 温度 + ADC 电流 + SOC 估算
 * ================================================================ */


/* ================================================================
 * 实时显示数据结构 (真实传感器数据源)
 * 由 ui_fetch_real_data() 填充, 各 OLED 页面共用一份快照
 * ================================================================ */
typedef struct {
    /* 电气量 (来自 LTC6804 + 电流 ADC) */
    uint32_t pack_mv;
    int16_t  pack_current_ma;
    uint8_t  soc_pct;
    float    soc;
    uint16_t cell_mv[BMS_MAX_CELL_SERIES_NUM];
    uint16_t cell_mv_max;
    uint16_t cell_mv_min;
    uint8_t  cell_vmax_idx;
    uint8_t  cell_vmin_idx;
    bms_balance_mask_t balance_mask;

    /* 温度 (来自 LTC6804 GPIO NTC) */
    int16_t  temp_c_dc[BMS_MAX_CELL_SERIES_NUM];
    int16_t  temp_max_dc;
    int16_t  temp_min_dc;
    float    dt_dt_dc_per_min;    /* 当前 dT/dt, ℃/min (正=升温) */

    /* SOH / 容量 (来自 SOC 估算 + sys_params) */
    float    soh;
    uint32_t cycle_count;
    float    capacity_ah;
    float    rated_ah;
    float    rint_ohm;

    /* 充电状态 (来自 app_charge + sys_data) */
    bms_charge_mode_e chg_mode;
    uint16_t chg_current_ma;
    uint16_t chg_voltage_mv;
    uint16_t remain_min;

    /* 保护状态 (来自 sys_data_get_fault) */
    bms_fault_mask_t fault;
    uint8_t flg_ovp;
    uint8_t flg_uvp;
    uint8_t flg_ocp;
    uint8_t flg_otp;
    uint8_t flg_scp;
    uint8_t flg_utp;

    /* 网络状态 */
    uint8_t wifi_ok;
    uint8_t mqtt_ok;

    /* 系统状态文字 */
    const char *status_str;
} ui_view_data_t;

/* 温度变化率跟踪 (ui_fetch_real_data 内部使用, 只读一次 / OLED 周期) */
static int16_t  s_ui_prev_temp_max_dc = 0;
static uint32_t s_ui_prev_temp_ts_ms  = 0;

/**
 * @brief 从全局数据总线抓取一份实时快照供 OLED 显示使用
 * @param vd  输出的实时数据(由调用方分配)
 * @note  必须在持锁的前提下读取 sys_data, 此处内部已通过 sys_data_get_* 封装
 *        温升速率基于上次调用的 temp_max_dc 和时间戳差分计算
 */
static void ui_fetch_real_data(ui_view_data_t *vd)
{
    if (vd == NULL) return;
    memset(vd, 0, sizeof(*vd));

    /* 1) 采集数据 */
    bms_pack_data_t pack;
    bms_soc_data_t  soc;
    sys_data_get_pack(&pack);
    sys_data_get_soc(&soc);

    vd->pack_mv         = pack.pack_mv;
    vd->pack_current_ma = pack.current_ma;
    vd->soc             = soc.soc;
    vd->soc_pct         = (uint8_t)(soc.soc * 100.0f + 0.5f);
    if (vd->soc_pct > 100) vd->soc_pct = 100;

    /* 运行时串数(网页可下发, 算法实时适配; 缺省回退编译期值) */
    uint8_t series_n = (uint8_t)(sys_params_get()->cell_series_num);
    if (series_n < 1 || series_n > BMS_MAX_CELL_SERIES_NUM) {
        series_n = BMS_CELL_SERIES_NUM;
    }

    for (uint8_t i = 0; i < series_n; i++) {
        vd->cell_mv[i]     = pack.cell_mv[i];
        vd->temp_c_dc[i]   = pack.temp_dc[i];
    }
    vd->cell_mv_max = pack.cell_mv_max;
    vd->cell_mv_min = pack.cell_mv_min;
    vd->temp_max_dc = pack.temp_max_dc;
    vd->temp_min_dc = pack.temp_min_dc;

    /* 找单体最高/最低电压索引 (Page1 Min/Max 显示用) */
    uint16_t vmin = 0xFFFF, vmax = 0;
    for (uint8_t i = 0; i < series_n; i++) {
        if (pack.cell_mv[i] < vmin) { vmin = pack.cell_mv[i]; vd->cell_vmin_idx = i; }
        if (pack.cell_mv[i] > vmax) { vmax = pack.cell_mv[i]; vd->cell_vmax_idx = i; }
    }

    /* 温度变化率 (基于上次 page0 调用的差分, 只读不更新基线) */
    {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (s_ui_prev_temp_ts_ms > 0 && pack.temp_max_dc > 0) {
            int32_t dt  = (int32_t)pack.temp_max_dc - (int32_t)s_ui_prev_temp_max_dc; /* 0.1℃ */
            int32_t dms = (int32_t)(now_ms - s_ui_prev_temp_ts_ms);
            if (dms > 0) {
                /* dT/dt (℃/min) = dt(0.1℃) * 10 / dms * 60000 */
                vd->dt_dt_dc_per_min = (float)dt * 10.0f / (float)dms * 60000.0f;
            }
        }
        /* 注意: 此处不更新 s_ui_prev_* 基线, 基线仅在 ui_draw_page0 末尾更新
         *       否则 page2/page4 等其他页面调用 ui_fetch_real_data 会污染基线 */
    }

    /* 2) 均衡 / 充电模式 */
    vd->balance_mask = sys_data_get_balance_mask();
    vd->chg_mode     = sys_data_get_charge_mode();

    /* 充电参数 (由 app_charge 策略给出) */
    bms_charge_params_t cp = app_charge_get_params();
    vd->chg_current_ma = cp.target_current_ma;
    vd->chg_voltage_mv = cp.target_voltage_mv;

    /* 预计剩余时间 (仅充电时计算, 效率按 80% 估算) */
    if (vd->chg_mode != CHARGE_MODE_STOP && vd->chg_current_ma > 0) {
        float remain_soc = 1.0f - soc.soc;
        float mah_remain = remain_soc * (float)BMS_CELL_CAPACITY_MAH;
        float ma_flow    = (float)vd->chg_current_ma * 0.8f;
        if (ma_flow > 0.0f) {
            vd->remain_min = (uint16_t)(mah_remain / ma_flow * 60.0f + 0.5f);
        }
    }

    /* 3) SOH / 容量 / 循环 / 内阻 (来自 SOC 估算 + NVS 参数) */
    vd->soh          = soc.soh;
    vd->cycle_count  = soc.cycle_count;
    const bms_params_t *params = sys_params_get();
    vd->rated_ah     = (float)params->cell_capacity_mah / 1000.0f;
    vd->capacity_ah  = vd->rated_ah * soc.soc;
    vd->rint_ohm     = soc.r0_ohm;

    /* 4) 保护标志位 (按位展开) */
    bms_fault_mask_t fault = sys_data_get_fault();
    vd->fault = fault;
    vd->flg_ovp = (fault & (FAULT_CELL_OV_PROT | FAULT_CELL_OV_WARN)) ? 1 : 0;
    vd->flg_uvp = (fault & (FAULT_CELL_UV_PROT | FAULT_CELL_UV_WARN)) ? 1 : 0;
    vd->flg_ocp = (fault & (FAULT_CHG_OC_PROT | FAULT_CHG_OC_WARN
                             | FAULT_DSG_OC_PROT | FAULT_DSG_OC_WARN)) ? 1 : 0;
    vd->flg_otp = (fault & (FAULT_OT_PROT | FAULT_OT_WARN)) ? 1 : 0;
    vd->flg_scp = (fault & FAULT_SHORT) ? 1 : 0;
    vd->flg_utp = (fault & (FAULT_UT_PROT | FAULT_UT_WARN)) ? 1 : 0;

    /* 5) 网络状态 */
    vd->wifi_ok = sys_wifi_is_connected() ? 1 : 0;
    vd->mqtt_ok = sys_mqtt_is_connected() ? 1 : 0;

    /* 6) 系统状态字符串 (根据故障 / 电流 / 充电模式推导) */
    if (vd->flg_ovp || vd->flg_uvp || vd->flg_ocp || vd->flg_otp
        || vd->flg_scp || vd->flg_utp) {
        vd->status_str = "FAULT";
    } else if (vd->chg_mode != CHARGE_MODE_STOP && vd->chg_mode != CHARGE_MODE_FULL) {
        vd->status_str = "CHARGING";
    } else if (vd->pack_current_ma > 50) {
        vd->status_str = "DISCHARGING";
    } else if (vd->pack_current_ma < -50) {
        vd->status_str = "CHARGING";
    } else {
        vd->status_str = "IDLE";
    }
}

/* 临时行缓冲(21+1 字符以内, 避免 sprintf 越界) */
static char s_line[24];

/* ---- 工具: 把 6 位 "状态位" 格式化为 "XXX OK" / "XXX TRIP" ---- */
static void ui_flag_line(char *buf, const char *name, uint8_t ok)
{
    /* 格式: 左 3 字符名称 + 空格 + 4 字符状态 = 每两个一组 8 字符, 一排放两组 */
    if (ok) {
        snprintf(buf, 9, "%-3s OK ", name);
    } else {
        snprintf(buf, 9, "%-3s TRP", name);      /* TRP = TRIP 触发 */
    }
}

/* ================================================================
 * 页 0 [SYS] 系统概览 (全部真实传感器 + SOC 算法数据)
 * 行0  [SYS] BMS 6S v1.0
 * 行1  Vbat= 22.35V  I= +3.2A    ← I 为实时电流
 * 行2  SOC=  85%   P=  71.5W     ← P 用实时电流计算
 * 行3  [=====>-----] 85%    (ASCII 进度条 13 格)
 * 行4  Status: DISCHARGING
 * 行5  Cell 3.68~3.75V
 * 行6  dV= 70mV  dT= 2.9C
 * 行7  ADC=2048  I=+3200mA      ← 调试行: ADC 原始值 + 电流 mA
 * ================================================================ */
static void ui_draw_page0(void)
{
    /* 抓取一份实时数据快照 (真实传感器 + 算法输出) */
    ui_view_data_t vd;
    ui_fetch_real_data(&vd);

    /* 行0: 顶部状态栏 - 根据报警等级显示不同内容 */
    {
        char fault_buf[12];
        app_alarm_get_text(fault_buf, sizeof(fault_buf));
        alarm_level_e level = app_alarm_get_level();

        switch (level) {
        case ALARM_LEVEL_FATAL:
            snprintf(s_line, sizeof(s_line), "[!!]%s", fault_buf);
            break;
        case ALARM_LEVEL_PROT:
            snprintf(s_line, sizeof(s_line), "[!]%s", fault_buf);
            break;
        case ALARM_LEVEL_WARN: {
            /* 预警级: 显示降额因子 D=xx% */
            uint8_t derate_pct = (uint8_t)(sys_data_get_derating() * 100.0f);
            snprintf(s_line, sizeof(s_line), "[W]%s D=%u%%", fault_buf, derate_pct);
            break;
        }
        case ALARM_LEVEL_NORMAL:
        default:
            snprintf(s_line, sizeof(s_line), "[SYS] BMS 6S v1.0");
            break;
        }
        bsp_oled_show_string(0, 0, s_line);
    }

    /* 行1: 总压(真实) + 实时电流(正放电加+号 负充电加-号) */
    {
        float v = vd.pack_mv / 1000.0f;
        char sign = (vd.pack_current_ma >= 0) ? '+' : '-';
        float i_abs = (vd.pack_current_ma < 0)
                      ? -vd.pack_current_ma : vd.pack_current_ma;
        i_abs = i_abs / 1000.0f;                        /* mA → A */
        snprintf(s_line, sizeof(s_line), "Vbat=%5.2fV I=%c%3.1fA",
                 v, sign, i_abs);
        bsp_oled_show_string(0, 1, s_line);
    }

    /* 行2: SOC 百分比(真实估算) + 功率(用实时电压*电流计算) */
    {
        float p = (vd.pack_mv / 1000.0f)
                  * (vd.pack_current_ma / 1000.0f);
        if (p < 0) p = -p;                              /* 取绝对值 */
        snprintf(s_line, sizeof(s_line), "SOC= %3d%%  P=%5.1fW",
                 vd.soc_pct, p);
        bsp_oled_show_string(0, 2, s_line);
    }

    /* 行3: ASCII 进度条 13 格 + 百分比后缀 */
    {
        char bar[16];
        uint8_t filled = (vd.soc_pct * 13 + 50) / 100;  /* 四舍五入 */
        if (filled > 13) filled = 13;
        bar[0] = '[';
        for (uint8_t i = 0; i < 13; i++) {
            bar[1 + i] = (i < filled) ? '=' : '-';
        }
        /* 加一个箭头指示当前位置 */
        if (filled > 0 && filled <= 13) bar[filled] = '>';
        bar[14] = ']';
        bar[15] = '\0';
        snprintf(s_line, sizeof(s_line), "%s %3d%%", bar, vd.soc_pct);
        bsp_oled_show_string(0, 3, s_line);
    }

    /* 行4: 状态文字 (根据故障/电流/充电模式推导) */
    snprintf(s_line, sizeof(s_line), "Status: %s", vd.status_str);
    bsp_oled_show_string(0, 4, s_line);

    /* 行5: 最低单体 ~ 最高单体 (mV -> V) */
    {
        snprintf(s_line, sizeof(s_line), "Cell %.2f~%.2fV",
                 vd.cell_mv_min / 1000.0f, vd.cell_mv_max / 1000.0f);
        bsp_oled_show_string(0, 5, s_line);
    }

    /* 行6: 单体压差 dV + 温差 dT */
    {
        int16_t tmin_dc = vd.temp_min_dc;
        int16_t tmax_dc = vd.temp_max_dc;
        int16_t dt_dc = tmax_dc - tmin_dc;
        snprintf(s_line, sizeof(s_line), "dV=%4umV dT=%3.1fC",
                 (unsigned)(vd.cell_mv_max - vd.cell_mv_min), dt_dc / 10.0f);
        bsp_oled_show_string(0, 6, s_line);
    }

    /* 行7: 调试行 - 显示 ADC 原始值 + 实时电流 mA, 方便验证电流采集是否正常 */
    {
        char sign = (s_live_current_ma >= 0) ? '+' : '-';
        int16_t i_abs = (s_live_current_ma < 0)
                        ? -s_live_current_ma : s_live_current_ma;
        snprintf(s_line, sizeof(s_line), "ADC=%4u I=%c%dmA",
                 (unsigned)s_live_adc_raw, sign, i_abs);
        bsp_oled_show_string(0, 7, s_line);
    }

    /* 更新 dT/dt 基线 (仅 page0 更新, 避免其他页面污染) */
    s_ui_prev_temp_max_dc = vd.temp_max_dc;
    s_ui_prev_temp_ts_ms  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ================================================================
 * 页 1 [CELL] 6 串单体电压 + 均衡
 * 行0  [CELL] 6S Monitors
 * 行1  C1=3.72V  C2=3.71V
 * 行2  C3=3.68V  C4=3.75V
 * 行3  C5=3.73V  C6=3.70V
 * 行4  Min=3.68V(C3)
 * 行5  Max=3.75V(C4)
 * 行6  dV=  70mV
 * 行7  Bal : 001000  (C4 均衡开)
 * ================================================================ */
static void ui_draw_page1(void)
{
    /* 抓取一份实时数据快照 (真实传感器 + 均衡状态) */
    ui_view_data_t vd;
    ui_fetch_real_data(&vd);

    bsp_oled_show_string(0, 0, "[CELL] 6S Monitors");

    /* 行1~3: 两两一组显示 C1~C6 (真实单体电压) */
    for (uint8_t row = 0; row < 3; row++) {
        uint8_t i1 = row * 2;       /* 0 2 4 → C1 C3 C5 */
        uint8_t i2 = row * 2 + 1;   /* 1 3 5 → C2 C4 C6 */
        snprintf(s_line, sizeof(s_line), "C%u=%.2fV  C%u=%.2fV",
                 i1 + 1, vd.cell_mv[i1] / 1000.0f,
                 i2 + 1, vd.cell_mv[i2] / 1000.0f);
        bsp_oled_show_string(0, 1 + row, s_line);
    }

    /* 行4: Min (真实最低单体 + 索引) */
    snprintf(s_line, sizeof(s_line), "Min=%.2fV(C%u)",
             vd.cell_mv_min / 1000.0f, vd.cell_vmin_idx + 1);
    bsp_oled_show_string(0, 4, s_line);

    /* 行5: Max (真实最高单体 + 索引) */
    snprintf(s_line, sizeof(s_line), "Max=%.2fV(C%u)",
             vd.cell_mv_max / 1000.0f, vd.cell_vmax_idx + 1);
    bsp_oled_show_string(0, 5, s_line);

    /* 行6: 压差 (真实) */
    snprintf(s_line, sizeof(s_line), "dV= %4umV",
             (unsigned)(vd.cell_mv_max - vd.cell_mv_min));
    bsp_oled_show_string(0, 6, s_line);

    /* 行7: 均衡掩码 (真实均衡状态 bit5~0 = C6~C1) */
    {
        char mask_str[7];
        for (uint8_t i = 0; i < 6; i++) {
            /* 高位在前: C6..C1 */
            mask_str[i] = (vd.balance_mask & (1 << (5 - i))) ? '1' : '0';
        }
        mask_str[6] = '\0';
        snprintf(s_line, sizeof(s_line), "Bal : %s", mask_str);
        bsp_oled_show_string(0, 7, s_line);
    }
}

/* ================================================================
 * 页 2 [TEMP] 温度与保护状态
 * 行0  [TEMP] Protections
 * 行1  T1= 28.5C  T2= 27.2C
 * 行2  T3= 30.1C  T4= 26.8C
 * 行3  Tmax=30.1C  dT/dt=0.2C
 * 行4  OVP OK   UVP OK
 * 行5  OCP OK   OTP OK
 * 行6  SCP OK   UT  OK
 * 行7  WARN: -   ERR: -
 * ================================================================ */
static void ui_draw_page2(void)
{
    /* 抓取一份实时数据快照 (真实温度 + 保护标志) */
    ui_view_data_t vd;
    ui_fetch_real_data(&vd);

    bsp_oled_show_string(0, 0, "[TEMP] Protections");

    /* 行1~2: 6 路单体温度 (真实 NTC, 0.1℃ → ℃) */
    snprintf(s_line, sizeof(s_line), "T1=%5.1fC  T2=%5.1fC",
             vd.temp_c_dc[0] / 10.0f, vd.temp_c_dc[1] / 10.0f);
    bsp_oled_show_string(0, 1, s_line);
    snprintf(s_line, sizeof(s_line), "T3=%5.1fC  T4=%5.1fC",
             vd.temp_c_dc[2] / 10.0f, vd.temp_c_dc[3] / 10.0f);
    bsp_oled_show_string(0, 2, s_line);

    /* 行3: 最高温 + 温升速率 (真实) */
    snprintf(s_line, sizeof(s_line), "Tmax=%4.1fC  dT/dt=%3.1fC",
             vd.temp_max_dc / 10.0f, vd.dt_dt_dc_per_min);
    bsp_oled_show_string(0, 3, s_line);

    /* 行4~6: 6 个保护标志 每行两个, 布局 "XXX YYY  XXX YYY" (真实) */
    char col1[9], col2[9];
    /* 行4: OVP / UVP */
    ui_flag_line(col1, "OVP", vd.flg_ovp == 0);
    ui_flag_line(col2, "UVP", vd.flg_uvp == 0);
    snprintf(s_line, sizeof(s_line), "%s %s", col1, col2);
    bsp_oled_show_string(0, 4, s_line);

    /* 行5: OCP / OTP */
    ui_flag_line(col1, "OCP", vd.flg_ocp == 0);
    ui_flag_line(col2, "OTP", vd.flg_otp == 0);
    snprintf(s_line, sizeof(s_line), "%s %s", col1, col2);
    bsp_oled_show_string(0, 5, s_line);

    /* 行6: SCP / UT (低温简写 UT) */
    ui_flag_line(col1, "SCP", vd.flg_scp == 0);
    ui_flag_line(col2, "UT",  vd.flg_utp == 0);
    snprintf(s_line, sizeof(s_line), "%s %s", col1, col2);
    bsp_oled_show_string(0, 6, s_line);

    /* 行7: 预警 / 故障 (基于真实 dT/dt 和故障位判定) */
    const char *warn = (vd.dt_dt_dc_per_min >= TEMP_THERMAL_RUNAWAY_WARN)
                       ? "TR" : "-";
    const char *err  = (vd.flg_ovp || vd.flg_uvp || vd.flg_ocp
                        || vd.flg_otp || vd.flg_scp || vd.flg_utp)
                       ? "TR" : "-";
    snprintf(s_line, sizeof(s_line), "WARN:%-3s  ERR:%-3s", warn, err);
    bsp_oled_show_string(0, 7, s_line);
}

/* ================================================================
 * 页 3 [INFO] SOH / 容量 / 充电 / 网络
 * 行0  [INFO] SOH & Charge
 * 行1  SOH= 96.8%  Cyc= 128
 * 行2  Cap=24.8Ah Rtd=25.6Ah
 * 行3  Rint=  13 mOhm
 * 行4  ChgMode: IDLE
 * 行5  ChgI= 5.0A V=25.20V
 * 行6  Remain: 2h 15m
 * 行7  WiFi:OK  MQTT:OK
 * ================================================================ */
static void ui_draw_page3(void)
{
    /* 抓取一份实时数据快照 (真实 SOH/容量/充电参数/网络状态) */
    ui_view_data_t vd;
    ui_fetch_real_data(&vd);

    bsp_oled_show_string(0, 0, "[INFO] SOH & Charge");

    /* 行1: SOH (真实估算, 0.0~1.0 → %) + 循环次数 */
    snprintf(s_line, sizeof(s_line), "SOH= %4.1f%%  Cyc=%4lu",
             vd.soh * 100.0f, (unsigned long)vd.cycle_count);
    bsp_oled_show_string(0, 1, s_line);

    /* 行2: 当前容量 / 额定容量 (真实 Ah) */
    snprintf(s_line, sizeof(s_line), "Cap=%4.1fAh Rtd=%4.1fAh",
             vd.capacity_ah, vd.rated_ah);
    bsp_oled_show_string(0, 2, s_line);

    /* 行3: 内阻 (真实估算, Ω → mΩ) */
    snprintf(s_line, sizeof(s_line), "Rint= %3u mOhm",
             (unsigned)(vd.rint_ohm * 1000.0f + 0.5f));
    bsp_oled_show_string(0, 3, s_line);

    /* 行4: 充电模式文字 (真实) */
    {
        const char *modes[] = {"STOP", "CC", "CV", "FULL"};
        uint8_t m = (vd.chg_mode < 4) ? vd.chg_mode : 0;
        snprintf(s_line, sizeof(s_line), "ChgMode: %s", modes[m]);
        bsp_oled_show_string(0, 4, s_line);
    }

    /* 行5: 充电设置 I / V (真实) */
    snprintf(s_line, sizeof(s_line), "ChgI=%4.1fA V=%5.2fV",
             vd.chg_current_ma / 1000.0f, vd.chg_voltage_mv / 1000.0f);
    bsp_oled_show_string(0, 5, s_line);

    /* 行6: 剩余时间 (真实估算) */
    {
        uint16_t h = vd.remain_min / 60;
        uint16_t m = vd.remain_min % 60;
        snprintf(s_line, sizeof(s_line), "Remain: %uh %02um", h, m);
        bsp_oled_show_string(0, 6, s_line);
    }

    /* 行7: WiFi / MQTT 连接状态 (真实) */
    snprintf(s_line, sizeof(s_line), "WiFi:%-3s MQTT:%-3s",
             vd.wifi_ok ? "OK" : "OFF",
             vd.mqtt_ok ? "OK" : "OFF");
    bsp_oled_show_string(0, 7, s_line);
}

/* ================================================================
 * 页 4 [ALARM] 报警详情页(报警时自动跳转, 禁止轮播)
 * 行0  顶部状态栏(等级标识 + 故障简码)
 * 行1  故障等级全称 + 降额因子
 * 行2  故障码(十六进制)
 * 行3  当前故障类型描述
 * 行4  电流相关: 实时电流 + 额定电流
 * 行5  温度相关: 最高温 + dT/dt
 * 行6  电压相关: 最高/最低单体 + 压差
 * 行7  操作提示: 长按解除报警
 * ================================================================ */
static void ui_draw_page4(void)
{
    char fault_buf[16];
    app_alarm_get_text(fault_buf, sizeof(fault_buf));
    alarm_level_e level   = app_alarm_get_level();
    bms_fault_mask_t fault = sys_data_get_fault();

    /* 行0: 顶部状态栏(同 Page0, 用醒目符号区分等级) */
    switch (level) {
    case ALARM_LEVEL_FATAL:
        snprintf(s_line, sizeof(s_line), "!! %s !!", fault_buf);
        break;
    case ALARM_LEVEL_PROT:
        snprintf(s_line, sizeof(s_line), "!  %s  !", fault_buf);
        break;
    case ALARM_LEVEL_WARN:
        snprintf(s_line, sizeof(s_line), "*  %s  *", fault_buf);
        break;
    case ALARM_LEVEL_NORMAL:
    default:
        snprintf(s_line, sizeof(s_line), "[ALARM] No Fault");
        break;
    }
    bsp_oled_show_string(0, 0, s_line);

    /* 行1: 等级全称 + 降额因子 */
    {
        const char *lvl_str;
        switch (level) {
        case ALARM_LEVEL_FATAL: lvl_str = "FATAL";   break;
        case ALARM_LEVEL_PROT:  lvl_str = "PROTECT"; break;
        case ALARM_LEVEL_WARN:  lvl_str = "WARNING"; break;
        default:                lvl_str = "NORMAL";  break;
        }
        uint8_t derate_pct = (uint8_t)(sys_data_get_derating() * 100.0f);
        snprintf(s_line, sizeof(s_line), "LVL:%-7s D=%u%%", lvl_str, derate_pct);
        bsp_oled_show_string(0, 1, s_line);
    }

    /* 行2: 故障码(32 位十六进制) */
    snprintf(s_line, sizeof(s_line), "Fault:0x%08lX", (unsigned long)fault);
    bsp_oled_show_string(0, 2, s_line);

    /* 行3: 当前故障类型描述(根据掩码逐项列出关键故障) */
    {
        const char *desc = "None";
        if (fault & FAULT_THERMAL_RUN)       desc = "Thermal Runaway!";
        else if (fault & FAULT_SHORT)        desc = "Short Circuit!";
        else if (fault & FAULT_SAMPLE_FAIL)  desc = "Sample Fail!";
        else if (fault & FAULT_CELL_OV_PROT) desc = "Cell OV Protect";
        else if (fault & FAULT_CELL_UV_PROT) desc = "Cell UV Protect";
        else if (fault & FAULT_CHG_OC_PROT)  desc = "Chg OC Protect";
        else if (fault & FAULT_DSG_OC_PROT)  desc = "Dsg OC Protect";
        else if (fault & FAULT_OT_PROT)      desc = "Over Temp Protect";
        else if (fault & FAULT_UT_PROT)      desc = "Low Temp Protect";
        else if (fault & FAULT_DTDT_WARN)    desc = "dT/dt Too Fast";
        else if (fault & FAULT_OVERLOAD_WARN)desc = "Overload Warning";
        else if (fault & FAULT_CELL_DV_WARN) desc = "Cell Diff Large";
        else if (fault & FAULT_UT_WARN)      desc = "Low Temp Warn";
        else if (fault & FAULT_LOW_SOC_WARN) desc = "Low SOC Warn";
        else if (fault & FAULT_LOW_SOH_WARN) desc = "Low SOH Warn";
        else if (fault & FAULT_COMM_WARN)    desc = "Comm Error";
        else if (fault & FAULT_CELL_OV_WARN) desc = "Cell OV Warn";
        else if (fault & FAULT_CELL_UV_WARN) desc = "Cell UV Warn";
        else if (fault & FAULT_CHG_OC_WARN)  desc = "Chg OC Warn";
        else if (fault & FAULT_DSG_OC_WARN)  desc = "Dsg OC Warn";
        else if (fault & FAULT_OT_WARN)      desc = "Over Temp Warn";
        snprintf(s_line, sizeof(s_line), "Type: %s", desc);
        bsp_oled_show_string(0, 3, s_line);
    }

    /* 行4: 电流相关(实时电流 + 额定电流) */
    {
        char sign = (s_live_current_ma >= 0) ? '+' : '-';
        uint16_t i_abs = (s_live_current_ma < 0)
                         ? (uint16_t)(-s_live_current_ma)
                         : (uint16_t)s_live_current_ma;
        snprintf(s_line, sizeof(s_line), "I=%c%umA Rtd=%umA",
                 sign, i_abs, (unsigned)RATED_CURRENT_MA);
        bsp_oled_show_string(0, 4, s_line);
    }

    /* 行5: 温度相关(最高温 + dT/dt, 实时数据)
     * dT/dt 基线仅在 page0 末尾更新, 此处只读不写, 不会污染基线 */
    {
        bms_pack_data_t pack;
        sys_data_get_pack(&pack);
        ui_view_data_t vd;
        ui_fetch_real_data(&vd);
        snprintf(s_line, sizeof(s_line), "Tmax=%4.1fC dT/dt=%4.1f",
                 pack.temp_max_dc / 10.0f, vd.dt_dt_dc_per_min);
        bsp_oled_show_string(0, 5, s_line);
    }

    /* 行6: 电压相关(最高/最低单体 + 压差, 实时数据) */
    {
        bms_pack_data_t pack;
        sys_data_get_pack(&pack);
        uint16_t vmin = pack.cell_mv_min;
        uint16_t vmax = pack.cell_mv_max;
        snprintf(s_line, sizeof(s_line), "V:%.2f~%.2fV dV=%umV",
                 vmin / 1000.0f, vmax / 1000.0f, (unsigned)(vmax - vmin));
        bsp_oled_show_string(0, 6, s_line);
    }

    /* 行7: 操作提示 */
    if (level == ALARM_LEVEL_NORMAL) {
        bsp_oled_show_string(0, 7, "System OK");
    } else {
        bsp_oled_show_string(0, 7, "Long Press: Clear");
    }
}

/* ---- 统一入口: 按页号绘制并刷新 ---- */
static void ui_draw_page(uint8_t page)
{
    /* 先清屏再画对应页 (每 2~3 秒才切换一次, 不会闪屏) */
    bsp_oled_clear();
    switch (page % OLED_PAGE_COUNT) {
    case 0: ui_draw_page0(); break;
    case 1: ui_draw_page1(); break;
    case 2: ui_draw_page2(); break;
    case 3: ui_draw_page3(); break;
    case 4: ui_draw_page4(); break;
    default: ui_draw_page0(); break;
    }
    /* 全部写入显存后统一刷新到屏幕硬件 */
    bsp_oled_refresh();
}

/* ================================================================
 * 菜单绘制(主菜单 / 子菜单 / 参数编辑)
 * 行0  标题
 * 行1  菜单项0 (选中则前面加 '>')
 * 行2  菜单项1
 * 行3  菜单项2
 * 行4  菜单项3
 * 行5  菜单项4 / 值
 * 行6  菜单项5 / 单位
 * 行7  底部提示: 短按=下  双击=选中  长按=返回
 * ================================================================ */
static void ui_draw_menu(void)
{
    const char *title = NULL;
    const char * const *items = NULL;
    uint8_t count = 0;
    uint8_t cur_idx = 0;
    int32_t edit_val = 0;
    const char *edit_unit = "";

    bool need_draw_menu = app_key_ui_get_menu_info(&title, &items, &count,
                                                   &cur_idx, &edit_val, &edit_unit);
    if (!need_draw_menu || items == NULL) {
        return;
    }

    bsp_oled_clear();

    /* 行0: 标题(带模式标识) */
    {
        ui_mode_e mode = app_key_ui_get_mode();
        const char *tag = (mode == UI_MODE_MENU)    ? "M"
                        : (mode == UI_MODE_SUBMENU) ? "S"
                        : (mode == UI_MODE_EDIT)    ? "E" : "?";
        snprintf(s_line, sizeof(s_line), "[%s] %s", tag, title ? title : "Menu");
        bsp_oled_show_string(0, 0, s_line);
    }

    /* 行1~6: 菜单项(一页最多 6 项, 不足则留空; 超过则滚动显示 cur_idx 附近) */
    if (count <= 6) {
        /* 6 项以内直接全显示 */
        for (uint8_t i = 0; i < 6; i++) {
            if (i < count) {
                const char *prefix = (i == cur_idx) ? ">" : " ";
                if (app_key_ui_get_mode() == UI_MODE_EDIT && i == 0) {
                    /* EDIT 模式第 1 项显示值, 第 2 项显示说明 */
                    snprintf(s_line, sizeof(s_line), "%s Value=%ld %s",
                             prefix, (long)edit_val, edit_unit);
                } else if (app_key_ui_get_mode() == UI_MODE_EDIT && i == 1) {
                    snprintf(s_line, sizeof(s_line), "%s %s", prefix, items[i]);
                } else {
                    snprintf(s_line, sizeof(s_line), "%s %s", prefix, items[i]);
                }
            } else {
                s_line[0] = '\0';
            }
            bsp_oled_show_string(0, 1 + i, s_line);
        }
    } else {
        /* 超过 6 项: 以 cur_idx 为中心滚动显示(当前不超过 6, 保留扩展) */
        uint8_t start = (cur_idx >= 3) ? (cur_idx - 3) : 0;
        for (uint8_t i = 0; i < 6 && (start + i) < count; i++) {
            const char *prefix = ((start + i) == cur_idx) ? ">" : " ";
            snprintf(s_line, sizeof(s_line), "%s %s", prefix, items[start + i]);
            bsp_oled_show_string(0, 1 + i, s_line);
        }
    }

    /* 行7: 底部操作提示(按模式区分) */
    {
        ui_mode_e mode = app_key_ui_get_mode();
        if (mode == UI_MODE_MENU) {
            bsp_oled_show_string(0, 7, "short:next  dbl:go  lng:back");
        } else if (mode == UI_MODE_SUBMENU) {
            bsp_oled_show_string(0, 7, "short:next  dbl:edit  lng:back");
        } else if (mode == UI_MODE_EDIT) {
            bsp_oled_show_string(0, 7, "short:+step  dbl:save  lng:can");
        } else {
            bsp_oled_show_string(0, 7, "");
        }
    }

    bsp_oled_refresh();
}

/* ---- OLED 主任务(页面/菜单由按键管理器 app_key_ui 控制) ----
 * 说明: 周期 100ms 调用 app_key_ui_process() 处理按键事件
 *       显示模式: 自动轮播 3s / 或按键手动切页
 *       菜单模式: 按键导航, 30s 无操作自动回显示
 *       缺按键硬件(HW_ENABLE_BUTTON=0): 自动降级为纯自动轮播 */
static void task_oled_ui(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t rotate_cnt = 0;                              /* 自动轮播计数器 */
    ui_mode_e last_mode  = UI_MODE_DISPLAY;               /* 上次 UI 模式(变化检测) */
    uint8_t   last_page  = 0xFF;                          /* 上次页号(变化检测, 0xFF 强制首绘) */
    uint32_t  redraw_cnt = 0;                             /* 重绘计数(定时强制刷新) */
    ESP_LOGI(TAG, "OLED UI task start, page=%d auto=%dms",
             OLED_PAGE_COUNT, OLED_AUTO_ROTATE_MS);

    while (1) {
        /* ---- 1. 读按键 + 驱动状态机(100ms 一次, 足够响应) ---- */
        app_key_ui_process();

        /* ---- 2. 从全局数据读取实时电流(供 Page0/Page4 显示) ----
         *      注意: 不要在 OLED 任务中直接读 ADC!
         *      ADC 由采集任务独占访问, 并发读取会导致 adc_oneshot_read
         *      状态混乱卡死, 表现为 OLED 运行一会后死机不刷新 */
#if HW_ENABLE_CURRENT_SENSE
        bms_pack_data_t pack;
        sys_data_get_pack(&pack);
        s_live_current_ma = pack.current_ma;
#endif

        /* ---- 3. 报警状态检测: 非正常等级自动锁定到 Page4 ----
         *    策略: 报警等级 > NORMAL 时, 强制 Page4 并禁止轮播
         *          用户手动切页可临时查看其他页, 但下次循环仍回 Page4
         *          报警解除后自动切回 Page0(实时电流页) */
        alarm_level_e cur_level = app_alarm_get_level();
        bool alarm_active = (cur_level != ALARM_LEVEL_NORMAL);

        if (alarm_active) {
            /* 报警中: 强制 Page4(报警详情页) + 禁止自动轮播 */
            if (app_key_ui_get_mode() == UI_MODE_DISPLAY &&
                app_key_ui_get_page() != 4) {
                app_key_ui_set_page(4);                    /* 强制跳转报警页 */
            }
        } else {
            /* 报警解除: 若仍在 Page4, 自动切回 Page0(实时电流页) */
            if (app_key_ui_get_mode() == UI_MODE_DISPLAY &&
                app_key_ui_get_page() == 4) {
                app_key_ui_set_page(0);
            }
        }

        /* ---- 4. 显示模式: 自动轮播触发(仅当 OLED_AUTO_ROTATE_MS > 0 且无报警) ---- */
#if OLED_AUTO_ROTATE_MS > 0
        if (!alarm_active) {
            rotate_cnt++;
            if (rotate_cnt >= (OLED_AUTO_ROTATE_MS / TASK_OLED_PERIOD_MS)) {
                rotate_cnt = 0;
                app_key_ui_rotate_page_if_idle();
            }
        } else {
            rotate_cnt = 0;                                /* 报警时重置计数器, 解除后从 Page0 开始 */
        }
#endif

        /* ---- 5. 串口电流调试: 每 20 次循环(2s)打印一次 ---- */
        if ((rotate_cnt & 0x0F) == 0) {
            ESP_LOGI(TAG, "[电流] ADC=%4u  I=%+dmA (%+.3fA)",
                     (unsigned)s_live_adc_raw, s_live_current_ma,
                     s_live_current_ma / 1000.0f);
        }

        /* ---- 6. 智能重绘: 仅在模式/页面变化或定时刷新时绘制 ----
         *    策略: 模式或页号变化 → 立即重绘
         *          报警页(Page4) → 每 5 次(500ms)刷新(实时电流/状态变化)
         *          显示模式 Page0(实时电流) → 每 5 次(500ms)重绘一次
         *          菜单模式 → 每 3 次(300ms)重绘一次(按键响应延迟)
         *          其他 → 每 10 次(1s)重绘一次
         *    注: bsp_oled_refresh 内部有显存快照比对, 数据不变时跳过 I2C */
        ui_mode_e cur_mode = app_key_ui_get_mode();
        uint8_t   cur_page = app_key_ui_get_page();
        bool need_redraw = false;

        if (cur_mode != last_mode || cur_page != last_page) {
            need_redraw = true;                            /* 模式或页面变化, 立即重绘 */
            redraw_cnt = 0;                                /* 重置计数器, 新页按其频率刷新 */
        } else if (cur_mode == UI_MODE_DISPLAY && (cur_page == 0 || cur_page == 4)) {
            need_redraw = ((redraw_cnt % 5) == 0);         /* Page0/Page4 实时数据, 500ms 刷新 */
        } else if (cur_mode != UI_MODE_DISPLAY) {
            need_redraw = ((redraw_cnt % 3) == 0);         /* 菜单模式, 300ms 刷新(按键响应) */
        } else {
            need_redraw = ((redraw_cnt % 10) == 0);        /* 其他固定页, 1s 刷新 */
        }

        if (need_redraw) {
            if (cur_mode == UI_MODE_DISPLAY) {
                ui_draw_page(cur_page);
            } else {
                ui_draw_menu();
            }
            last_mode = cur_mode;
            last_page = cur_page;
            redraw_cnt++;
        }

        s_task_alive_oled = true;                          /* 喂看门狗存活标志 */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TASK_OLED_PERIOD_MS));
    }
}
#endif /* HW_ENABLE_OLED */




/* ================================================================
 * MQTT 自定义命令回调(在 MQTT 事件线程中执行, 严禁阻塞!)
 * 处理: clear_alarm / restart / ota_check / ota_upgrade / reset_params
 *       set_charge / set_discharge / set_balance / set_relay (远程控制)
 * ================================================================ */
#if HW_ENABLE_WIFI

/* ---- 前向声明(解决隐式声明编译错误: 这些函数定义在文件更靠后) ---- */
static void ota_scheduler_request_check(void);
static void ota_scheduler_request_upgrade(const char *url);

/* 内部: 兼容 IoTDA 两种 paras 取值方式
 * 华为云命令格式: {command_name, paras:{...}} 或顶层平铺
 * 优先从 paras 子对象取, fallback 到 root 顶层 */
static const cJSON *cmd_get_field(const cJSON *root, const char *field)
{
    const cJSON *paras = cJSON_GetObjectItem(root, "paras");
    if (paras != NULL) {
        const cJSON *v = cJSON_GetObjectItem(paras, field);
        if (v != NULL) return v;
    }
    return cJSON_GetObjectItem(root, field);
}

static bool cmd_get_bool(const cJSON *root, const char *field, bool default_val)
{
    const cJSON *v = cmd_get_field(root, field);
    if (v == NULL) return default_val;
    if (cJSON_IsBool(v))  return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valueint != 0;
    if (cJSON_IsString(v)) {
        const char *s = v->valuestring;
        return (strcmp(s, "true") == 0 || strcmp(s, "1") == 0 ||
                strcmp(s, "on")  == 0 || strcmp(s, "yes") == 0);
    }
    return default_val;
}

static int cmd_get_int(const cJSON *root, const char *field, int default_val)
{
    const cJSON *v = cmd_get_field(root, field);
    if (v == NULL) return default_val;
    if (cJSON_IsNumber(v)) return v->valueint;
    return default_val;
}

static void mqtt_cmd_callback(const char *cmd, const cJSON *root)
{
    /* 命令名统一转大写(产品模型命令 SET_* 为大写, Dashboard /messages 为小写) */
    char cmd_upper[32] = {0};
    if (cmd != NULL) {
        strncpy(cmd_upper, cmd, sizeof(cmd_upper) - 1);
        for (char *p = cmd_upper; *p != '\0'; p++) {
            if (*p >= 'a' && *p <= 'z') {
                *p = (char)(*p - ('a' - 'A'));
            }
        }
    }
    cmd = cmd_upper;

    if (strcmp(cmd, "CLEAR_ALARM") == 0) {
        /* 解除报警: 清故障掩码 + 清热失控锁定 */
        sys_data_set_fault(FAULT_NONE);
        sys_fsm_clear_lock();
        ESP_LOGI(TAG, "[MQTT] 命令: 解除报警");
        /* 通过 MQTT 回复(直接发布, 因为已经在 mqtt 线程) */
        sys_mqtt_report_info();

    } else if (strcmp(cmd, "RESTART") == 0) {
        /* -------- 重启命令: 交给新的 4 层防线幂等处理器 -------- */
        handle_cmd_restart(root);
        /* handle_cmd_restart 是非阻塞的, 立即返回. PUBACK 将由 MQTT 线程发出. */

    } else if (strcmp(cmd, "OTA_CHECK") == 0) {
        /* -------- OTA 检查升级: 非阻塞 set eventbit, 让 task_ota 线程去执行 -------- */
        ESP_LOGI(TAG, "[MQTT] 命令: OTA 检查升级 (已转交给 OTA 任务, 非阻塞)");
        ota_scheduler_request_check();

    } else if (strcmp(cmd, "OTA_UPGRADE") == 0) {
        const cJSON *j_url = cJSON_GetObjectItem(root, "url");
        if (j_url == NULL) j_url = cmd_get_field(root, "url");
        if (cJSON_IsString(j_url)) {
            ESP_LOGI(TAG, "[MQTT] 命令: OTA 强制升级 (已转交给 OTA 任务, 非阻塞) URL=%.80s",
                     j_url->valuestring);
            ota_scheduler_request_upgrade(j_url->valuestring);
        } else {
            ESP_LOGW(TAG, "[MQTT] OTA 强制升级缺少 url 字段, 忽略");
        }

    } else if (strcmp(cmd, "RESET_PARAMS") == 0) {
        ESP_LOGI(TAG, "[MQTT] 命令: 重置参数");
        sys_params_reset();

    } else if (strcmp(cmd, "GET_INFO") == 0) {
        sys_mqtt_report_info();

    /* ====== 远程设备控制命令 ====== */
    } else if (strcmp(cmd, "SET_CHARGE") == 0) {
        bool enable = cmd_get_bool(root, "enable", false);
        bool chg_on = false, dsg_on = false;
        sys_data_set_remote_override(true);             /* 先置位, 30s 内保护任务不覆盖(防覆盖窗口) */
        sys_data_get_relay(&chg_on, &dsg_on);          /* 保留放电状态 */
        sys_data_set_relay(enable, dsg_on);
        sys_data_set_charge_mode(enable ? CHARGE_MODE_CC : CHARGE_MODE_STOP);
        if (enable) {
            sys_data_set_master_power_off(false);       /* 云端开启 = 解除 SW_PWR 关断 */
        }
        ESP_LOGI(TAG, "[MQTT] 命令: 充电回路 %s", enable ? "ON" : "OFF");

    } else if (strcmp(cmd, "SET_DISCHARGE") == 0) {
        bool enable = cmd_get_bool(root, "enable", false);
        bool chg_on = false, dsg_on = false;
        sys_data_set_remote_override(true);             /* 先置位, 30s 内保护任务不覆盖(防覆盖窗口) */
        sys_data_get_relay(&chg_on, &dsg_on);          /* 保留充电状态 */
        sys_data_set_relay(chg_on, enable);
        if (enable) {
            sys_data_set_master_power_off(false);       /* 云端开启 = 解除 SW_PWR 关断 */
        }
        ESP_LOGI(TAG, "[MQTT] 命令: 放电回路 %s", enable ? "ON" : "OFF");

    } else if (strcmp(cmd, "SET_BALANCE") == 0) {
        bool enable = cmd_get_bool(root, "enable", false);
        /* 2026-08-07: 均衡模式可选(OFF/PASSIVE/ACTIVE)
         * mode 字段: "off"/"passive"/"active" 或 0/1/2
         * 未传 mode 时保持当前模式(兼容旧前端) */
        bms_balance_mode_e bmode = sys_data_get_balance_mode();
        const cJSON *j_mode = cmd_get_field(root, "mode");
        if (cJSON_IsString(j_mode) && j_mode->valuestring) {
            if (strcmp(j_mode->valuestring, "passive") == 0 || strcmp(j_mode->valuestring, "1") == 0) {
                bmode = BALANCE_MODE_PASSIVE;
            } else if (strcmp(j_mode->valuestring, "active") == 0 || strcmp(j_mode->valuestring, "2") == 0) {
                bmode = BALANCE_MODE_ACTIVE;
            } else {
                bmode = BALANCE_MODE_OFF;
            }
        } else if (cJSON_IsNumber(j_mode)) {
            bmode = (bms_balance_mode_e)j_mode->valueint;
            if (bmode < BALANCE_MODE_OFF || bmode > BALANCE_MODE_ACTIVE) {
                bmode = BALANCE_MODE_OFF;
            }
        }
        if (enable) {
            /* 掩码位宽随 bms_balance_mask_t 扩到 32 位(默认全开自动均衡) */
            int64_t mask = cmd_get_int(root, "mask", -1);   /* 默认 -1 = 全开自动均衡 */
            sys_data_set_balance_mask((bms_balance_mask_t)(mask < 0 ? 0xFFFFFFFFu : (uint32_t)mask));
            sys_data_set_balance_mode(bmode);
            ESP_LOGI(TAG, "[MQTT] 命令: 开启均衡 mode=%d mask=0x%08X",
                     (int)bmode, (unsigned)(sys_data_get_balance_mask()));
        } else {
            sys_data_set_balance_mask(0x0000);
            ESP_LOGI(TAG, "[MQTT] 命令: 关闭均衡");
        }

    } else if (strcmp(cmd, "SET_RELAY") == 0) {
        bool chg   = cmd_get_bool(root, "charge",    false);
        bool dsg   = cmd_get_bool(root, "discharge", false);
        sys_data_set_remote_override(true);             /* 先置位, 30s 内保护任务不覆盖(防覆盖窗口) */
        sys_data_set_relay(chg, dsg);
        sys_data_set_charge_mode(chg ? CHARGE_MODE_CC : CHARGE_MODE_STOP);
        if (chg || dsg) {
            sys_data_set_master_power_off(false);       /* 云端开启 = 解除 SW_PWR 关断 */
        }
        ESP_LOGI(TAG, "[MQTT] 命令: 主继电器 充=%d 放=%d", chg, dsg);

    } else {
        ESP_LOGW(TAG, "[MQTT] 未知命令: %s", cmd);
    }
}
#endif


bms_err_t app_system_init(void)
{
    ESP_LOGI(TAG, "===== BMS 系统初始化 (硬件屏蔽机制启用) =====");
    fflush(stdout);

    /* ====== NVS Flash 初始化(必须最先, sys_params/WiFi 配网都依赖 NVS) ======
     * 若 NVS 分区损坏或版本不匹配, 擦除后重新初始化 */
    {
        esp_err_t nvs_ret = nvs_flash_init();
        if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
            nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS 分区需擦除重建: %s", esp_err_to_name(nvs_ret));
            ESP_ERROR_CHECK(nvs_flash_erase());
            nvs_ret = nvs_flash_init();
        }
        if (nvs_ret != ESP_OK) {
            ESP_LOGE(TAG, "NVS 初始化失败: %s (参数将用默认值, 配网无法保存)",
                     esp_err_to_name(nvs_ret));
        } else {
            ESP_LOGI(TAG, "NVS 初始化成功");
        }
    }

    /* ====== 中间件层(无条件初始化, 无硬件依赖) ====== */
    sys_params_init();                                  /* NVS 参数加载(必须最先, WiFi/MQTT/阈值依赖) */
    sys_data_init();
    sys_fsm_init();

    /* ====== 2026-08-08 功能安全 S1c: 启动自检 POST(上电安全态确认) ======
     * 校验 NVS 加载后的关键参数合法性, 非法则回退默认值并告警,
     * 防止损坏的参数(串数/阈值越界)在启动后立即触发误保护或漏保护. */
    {
        const bms_params_t *_p = sys_params_get();
        bool _post_bad = false;
        if (_p->cell_series_num < 1 || _p->cell_series_num > BMS_HW_MAX_SERIES_NUM) {
            ESP_LOGE(TAG, "[POST] 串数非法 %u -> 回退默认", (unsigned)_p->cell_series_num);
            _post_bad = true;
        }
        if (_p->cell_ov_prot_mv < 3000 || _p->cell_ov_prot_mv > 5000) {
            ESP_LOGE(TAG, "[POST] 过压保护阈值非法 %u -> 回退默认", (unsigned)_p->cell_ov_prot_mv);
            _post_bad = true;
        }
        if (_p->cell_uv_prot_mv < 2000 || _p->cell_uv_prot_mv > 3500) {
            ESP_LOGE(TAG, "[POST] 欠压保护阈值非法 %u -> 回退默认", (unsigned)_p->cell_uv_prot_mv);
            _post_bad = true;
        }
        /* cell_capacity_mah 为 uint16_t(最大65535), 原上界 100000 恒为假触发
         * -Wtype-limits 警告; 改为类型范围内合理上界 60000(=60Ah) */
        if (_p->cell_capacity_mah < 500 || _p->cell_capacity_mah > 60000) {
            ESP_LOGE(TAG, "[POST] 容量参数非法 %u -> 回退默认", (unsigned)_p->cell_capacity_mah);
            _post_bad = true;
        }
        if (_post_bad) {
            sys_params_reset();                         /* 回退默认并重载 */
            ESP_LOGW(TAG, "[POST] 参数非法已复位为默认值(安全态)");
            sys_data_set_fault(FAULT_COMM_WARN);        /* 提示参数异常 */
        } else {
            ESP_LOGI(TAG, "[POST] 启动自检通过(参数合法)");
        }
    }

    /* ====== 故障黑匣子: 记录本次复位原因(2026-09-14 高可靠加固) ======
     * 在 NVS/参数初始化之后尽早执行; MQTT get_info 自动上报给云端,
     * 运维可追溯"上次为什么死"(看门狗复位/任务饿死/崩溃/正常上电). */
    {
        uint8_t rst = (uint8_t)esp_reset_reason();
        if (sys_params_set_blackbox(rst) == BMS_OK) {
            static const char *const rst_names[] = {
                "UNKNOWN", "POWERON", "SW", "PANIC", "INT_WDT", "TASK_WDT",
                "WDT", "BROWNOUT", "SDIO", "USB", "JTAG", "EFUSE", "CLK_CAL",
            };
            const char *name = (rst < sizeof(rst_names) / sizeof(rst_names[0]))
                                   ? rst_names[rst] : "OTHER";
            /* 看门狗/ panic 类复位属异常事件, 用 W 级别醒目提示 */
            if (rst == 3 || rst == 4 || rst == 5 || rst == 6) {
                ESP_LOGW(TAG, "[黑匣子] 异常复位原因=%u(%s)", rst, name);
            } else {
                ESP_LOGI(TAG, "[黑匣子] 复位原因=%u(%s)", rst, name);
            }
            /* ====== B1 黑匣子扩展: 异常复位时把死前现场带上云 ======
             * 读上次运行点(fault/SOC/包压/包流)打日志; 本周期运行中由
             * task_watchdog 的 30s 周期刷新快照(见 app_tasks.c 看门狗任务),
             * 下次异常复位即可读到本次死前状态. */
            {
                uint32_t bb_fault = 0;
                uint8_t  bb_soc = 0;
                uint16_t bb_mv = 0;
                int16_t  bb_ma = 0;
                if (sys_params_get_blackbox_ctx(&bb_fault, &bb_soc,
                                                &bb_mv, &bb_ma) == BMS_OK) {
                    ESP_LOGW(TAG, "[黑匣子] 上次死前现场: fault=0x%06lX SOC=%u%% "
                                  "pack=%u(0.1V) %dmA",
                             (unsigned long)bb_fault, bb_soc, bb_mv, bb_ma);
                }
                /* 异常复位时立刻把"当前(=复位后初始)状态"占位写入? 否:
                 * 当前 fault 尚未初始化完, 无意义; 等看门狗任务首个 30s 快照. */
            }
        } else {
            ESP_LOGI(TAG, "[黑匣子] 复位原因=%u(POWERON/SW)", rst);
        }
    }

    /* ====== BSP 层(按硬件屏蔽宏决定是否初始化, 失败不阻塞) ======
     * 2026-08-25 初始化顺序调整: BQ76952(含 I2C 总线) → 继电器(FET) → 电流(库仑计)
     *            → LED/蜂鸣器 → SD卡 → CAN(TWAI)
     * 注: I2C 总线现由 bsp_bq76952_init 建立(原 bsp_oled_init 负责); OLED 需在其后初始化 */
    bms_err_t err;

    /* BQ34Z100 独立电量计 (I2C, 复用总线, 可选) - 需在 BQ76952 建立 I2C 之后 */
#if HW_ENABLE_BQ34Z100
    err = bsp_bq34z100_init();
    if (err != BMS_OK) {
        ESP_LOGW(TAG, "BQ34Z100 初始化失败(可选, 降级): %d", err);
    }
#endif

    /* LED 状态指示 */
#if HW_ENABLE_LED
    err = bsp_led_init();
    if (err != BMS_OK) { ESP_LOGW(TAG, "LED 初始化失败(降级): %d", err); }
#endif

    /* 蜂鸣器 */
#if HW_ENABLE_BUZZER
    err = bsp_buzzer_init();
    if (err != BMS_OK) { ESP_LOGW(TAG, "蜂鸣器 初始化失败(降级): %d", err); }
#endif

    /* 电源键 SW_PWR(GPIO13, 高有效, 仿 C1): 长按 3s 关功率(独立于 HW_ENABLE_BUZZER) */
#if PIN_SW_PWR >= 0
    err = bsp_sw_pwr_init();
    if (err != BMS_OK) { ESP_LOGW(TAG, "SW_PWR 电源键初始化失败(降级): %d", err); }
#endif

    /* 继电器 */
#if HW_ENABLE_RELAY
    err = bsp_relay_init();
    if (err != BMS_OK) { ESP_LOGW(TAG, "继电器 初始化失败(降级): %d", err); }
#endif

    /* 电流 ADC (GPIO1 = ADC1_CH0) */
#if HW_ENABLE_CURRENT_SENSE
    err = bsp_current_init();
    if (err != BMS_OK) {
        ESP_LOGW(TAG, "电流 ADC 初始化失败(降级): %d", err);
    } else {
        ESP_LOGI(TAG, "电流采集初始化成功 (BQ76952 CC2 库仑计)");
    }
    /* 首次读取电流, 供首屏显示 */
    s_live_adc_raw    = bsp_current_read_raw();
    s_live_current_ma = bsp_current_read_ma();
    ESP_LOGI(TAG, "[电流] 首次读取 ADC=%u  I=%dmA",
             (unsigned)s_live_adc_raw, s_live_current_ma);
#endif

    /* 电压采集 AFE (I2C BQ76952 / SPI2 LTC6804, 二选一由 HW_ENABLE_* 决定)
     * 2026-08-19 修复(F9): init 失败时置采样失效告警并允许采集任务周期性重试
     *   —— 原实现仅打日志降级, AFE 永久失效时电压保护静默缺失且无告警 */
#if HW_ENABLE_BQ76952
    err = bsp_bq76952_init();
    if (err != BMS_OK) {
        ESP_LOGW(TAG, "BQ76952 初始化失败(降级): %d, 采集任务将周期性重试", err);
        sys_data_set_fault(FAULT_SAMPLE_FAIL);      /* F9: 置采样失效告警(保护层会全断) */
    } else {
        /* 2026-08-25: 写回 BMS-C1 仿制板硬件配置(TS NTC/DCHG FET温度/DFETOFF 分流温度/16S)
         * 失败仅告警不阻塞(出厂配置可能已生效) */
        bms_err_t cfg_err = bsp_bq76952_apply_config();
        if (cfg_err != BMS_OK) {
            ESP_LOGW(TAG, "BQ76952 C1 配置写回失败(降级): %d", cfg_err);
        }

        /* OLED 显示 (I2C 0x3C, 与 BQ76952 0x08 并联同一条总线, 地址不冲突)
         * 必须在 bsp_bq76952_init 之后(总线由它建立); 失败不阻塞 */
#if HW_ENABLE_OLED
        bms_err_t oled_err = bsp_oled_init();
        if (oled_err != BMS_OK) {
            ESP_LOGE(TAG, "OLED 初始化失败: %d", oled_err);
        } else {
            ESP_LOGI(TAG, "OLED 初始化成功 (128x64 SH1106, 6x8 字库)");
        }
#endif
    }
#elif HW_ENABLE_LTC6804
    err = bsp_ltc6804_init();
    if (err != BMS_OK) {
        ESP_LOGW(TAG, "LTC6804 初始化失败(降级): %d, 采集任务将周期性重试", err);
        sys_data_set_fault(FAULT_SAMPLE_FAIL);      /* F9: 置采样失效告警(保护层会全断) */
    }
#endif

    /* 绝缘检测(不平衡电桥法, 2026-08-09 新增, 硬件预留) */
#if HW_ENABLE_INSULATION_DETECT
    err = bsp_insulation_init();
    if (err != BMS_OK) { ESP_LOGW(TAG, "绝缘检测初始化失败(降级): %d", err); }
#endif

    /* SD 卡黑匣子 (SPI3 总线) - 必须在 CAN 之前初始化 */
#if HW_ENABLE_SD_CARD
    err = bsp_sd_init();
    if (err != BMS_OK) { ESP_LOGW(TAG, "SD 卡 初始化失败(降级): %d", err); }
#endif

    /* CAN 通信 (SPI3 复用) - 必须在 SD 之后 */
#if HW_ENABLE_TWAI
    err = bsp_can_init();
    if (err != BMS_OK) { ESP_LOGW(TAG, "CAN 初始化失败(降级): %d", err); }
#endif

    /* ====== WiFi + MQTT (可选) ======
     * 2026-08-19 修复(F1/F2): WiFi 配网/连接/MQTT 初始化已移至独立任务 task_net_init
     * (在 app_tasks_start 中创建)。原实现在此同步阻塞——未配网时 AP 门户最长
     * BMS_CONFIG_PORTAL_TIMEOUT_MS(5 分钟)、已配网时 STA+SNTP+DNS 最长 ~23s，
     * 而 app_tasks_start() 在此函数返回后才被调用，导致配网/联网期间保护任务等
     * 全部未启动 → 电池零保护空窗。移入独立低优先级任务后，核心任务立即启动。 */


    /* ====== 算法与业务模块(无硬件依赖, 无条件初始化) ====== */
    app_protection_init();
    app_soc_init();
    app_balance_init();
    app_charge_init();
    app_ota_init();
    app_alarm_init();                                    /* 报警管理器(LED/蜂鸣器分级报警) */

    /* 按键 UI 管理器(内部会调用 bsp_button_init, HW_ENABLE_BUTTON=0 则降级为自动轮播) */
    {
        bms_err_t r = app_key_ui_init();
        if (r != BMS_OK) {
            ESP_LOGW(TAG, "按键 UI 降级(无按键硬件): 自动轮播模式");
        } else {
            ESP_LOGI(TAG, "按键 UI 初始化成功(短按切页/双击入菜单/长按解报警)");
        }
    }

    /* 电流低通滤波器初始化 */
    sys_lpf_init(&s_current_lpf, 0.1f);              // α=0.1 一阶低通

#if HW_ENABLE_WIFI
    /* 注册 MQTT 自定义命令回调(处理 clear_alarm/restart/ota 等) */
    sys_mqtt_register_cmd_callback(mqtt_cmd_callback);
#endif

    /* SD 卡黑匣子日志服务(依赖 bsp_sd 已就绪) */
#if HW_ENABLE_SD_CARD
    sys_storage_init();
#endif

    /* OLED 首屏显示 */
#if HW_ENABLE_OLED
    ui_draw_page(0);
    ESP_LOGI(TAG, "OLED 首屏已显示, 即将启动任务调度");
#endif

    ESP_LOGI(TAG, "===== 系统初始化完成 =====");
    return BMS_OK;
}

/* ====== 采集任务(100ms) ====== */
static void task_acquisition(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        bms_pack_data_t pack;
        memset(&pack, 0, sizeof(pack));
        pack.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        /* 读取 AFE 单体电压(BQ76952 / LTC6804, 二选一) */
#if HW_ENABLE_BQ76952
        if (bsp_bq76952_read_voltages(pack.cell_mv) == BMS_OK) {
#elif HW_ENABLE_LTC6804
        if (bsp_ltc6804_read_voltages(pack.cell_mv) == BMS_OK) {
#else
        if (false) {
#endif
            uint16_t v_max = 0, v_min = 0xFFFF;
            uint32_t sum = 0;
            uint8_t acq_series = (uint8_t)(sys_params_get()->cell_series_num);
            if (acq_series < 1 || acq_series > BMS_MAX_CELL_SERIES_NUM) {
                acq_series = BMS_CELL_SERIES_NUM;
            }
            for (uint8_t i = 0; i < acq_series; i++) {
                sum += pack.cell_mv[i];
                if (pack.cell_mv[i] > v_max) v_max = pack.cell_mv[i];
                if (pack.cell_mv[i] < v_min) v_min = pack.cell_mv[i];
            }
            pack.cell_mv_max = v_max;
            pack.cell_mv_min = v_min;
            pack.pack_mv = sum;                /* H2: 32S 总压可达 134V, 不能截断为 uint16 */
        } else {
            /* AFE 屏蔽/失败: 不再用模拟数据填充, 保持 0 并打警告
             * 注: 2026-08-06 起彻底禁用 s_mock 回退, 传感器失败时上游算法/OLED 需容忍 0 值
             * 保留原模拟回退代码作为参考:
             * 2026-08-19 修复(F9): 每 100 次(~10s)重试 AFE init, 恢复后清除采样失效告警 */
            static uint32_t fail_log_cnt = 0;
            if ((++fail_log_cnt % 100) == 0) {
                ESP_LOGW(TAG, "AFE 读取失败, 已 %lu 次, 尝试重新初始化",
                         (unsigned long)fail_log_cnt);
#if HW_ENABLE_BQ76952
                /* 2026-09-15 飞线唤醒: AFE 可能处于 SHUTDOWN, 此时 I2C 完全无响应,
                 * 直接 reinit 的探测必然失败 → 必须先拉 TS2 唤醒脉冲再 init.
                 * (唤醒后芯片需要 ~几ms 上电稳定, init 内部的首笔 I2C 已含重试余量) */
                bsp_bq76952_wakeup();
                vTaskDelay(pdMS_TO_TICKS(10));
                if (bsp_bq76952_init() == BMS_OK) {
                    /* 2026-09-14 高可靠加固: SHUTDOWN 唤醒/掉电后芯片寄存器回出厂默认,
                     * 必须跟随重配 C1 硬件配置(TS NTC/DCHG/DFETOFF/16S),
                     * 否则温度通道/16S 模式静默失效 — 与首启 init 后的配置写回同一份 */
                    bms_err_t cfg_err2 = bsp_bq76952_apply_config();
                    if (cfg_err2 != BMS_OK) {
                        ESP_LOGW(TAG, "BQ76952 重配写回失败(%d), 下轮重试", cfg_err2);
                    }
                    sys_data_clear_fault(FAULT_SAMPLE_FAIL);
                    ESP_LOGI(TAG, "BQ76952 重新初始化成功, 采样恢复");
                }
#elif HW_ENABLE_LTC6804
                if (bsp_ltc6804_init() == BMS_OK) {
                    sys_data_clear_fault(FAULT_SAMPLE_FAIL);
                    ESP_LOGI(TAG, "LTC6804 重新初始化成功, 采样恢复");
                }
#endif
            }
        }

        /* 读取温度
         * BQ76952(C1 仿制板): TS1~TS3 为 thermistor 模式(18k 上拉 NTC),
         *   命令 0x70/0x72/0x74 直读 0.1K → bsp_bq76952_read_temp 输出 0.1℃
         * LTC6804(旧方案): GPIO1~GPIO5 分压电压 → bsp_temp_calc_dc 换算 NTC 温度 */
        int16_t t_max = -9999, t_min = 9999;   /* H2 修复: 用 0.1℃ 单位极值, 允许负温参与统计 */
#if HW_ENABLE_BQ76952
        int16_t ts_temp[3] = {0};
        if (bsp_bq76952_read_temp(ts_temp) == BMS_OK) {
            for (uint8_t i = 0; i < 3 && i < BMS_CELL_SERIES_NUM; i++) {
                /* F7 修复: 用哨兵判断有效性, 真实 0℃(temp_dc=0)必须参与统计 */
                bool t_ok = ((int32_t)ts_temp[i] > (int32_t)NTC_TEMP_INVALID);
                pack.temp_dc[i] = t_ok ? ts_temp[i] : 0;
                if (t_ok) {
                    if (pack.temp_dc[i] > t_max) t_max = pack.temp_dc[i];
                    if (pack.temp_dc[i] < t_min) t_min = pack.temp_dc[i];
                }
            }
        }
#elif HW_ENABLE_LTC6804
        uint16_t gpio_mv[LTC6804_CHIP_NUM * 5] = {0};
        if (bsp_ltc6804_read_gpio(gpio_mv) == BMS_OK) {
            for (uint8_t i = 0; i < (uint8_t)(LTC6804_CHIP_NUM * 5) && i < BMS_CELL_SERIES_NUM; i++) {
                float t = bsp_temp_calc_dc(gpio_mv[i]);
                /* F7 修复: 用哨兵判断有效性, 真实 0℃(temp_dc=0)必须参与统计 */
                bool t_ok = (t > NTC_TEMP_INVALID);
                pack.temp_dc[i] = t_ok ? (int16_t)t : 0;
                if (t_ok) {
                    if (pack.temp_dc[i] > t_max) t_max = pack.temp_dc[i];
                    if (pack.temp_dc[i] < t_min) t_min = pack.temp_dc[i];
                }
            }
        }
#endif
        /* 若有有效温度, 则更新最大/最小值(含负温, 低温保护依赖); 否则保持 0 */
        pack.temp_max_dc = (t_max > -9999) ? t_max : 0;
        pack.temp_min_dc = (t_min < 9999)   ? t_min : 0;

        /* 读取电流(一阶低通滤波) */
        int16_t raw = bsp_current_read_ma();
        float filt = sys_lpf_update(&s_current_lpf, (float)raw);
        pack.current_ma = (int16_t)filt;

        /* 读取绝缘电阻(不平衡电桥法, 2026-08-09 新增, 硬件预留)
         * 未启用/未接线时保持 0, 由保护层跳过判定 */
#if HW_ENABLE_INSULATION_DETECT
        {
            uint32_t rp = 0, rn = 0;
            if (bsp_insulation_read(pack.pack_mv, &rp, &rn) == BMS_OK) {
                pack.insulation_rp_ohm = rp;
                pack.insulation_rn_ohm = rn;
                /* 综合绝缘电阻率 Ω/V = min(Rp,Rn)/总压(GB/T 18384.1 判据) */
                uint32_t rmin = (rp < rn) ? rp : rn;
                /* 2026-08-09 修复: pack_mv<1000mV 时 pack_mv/1000u=0 整数除零崩溃,
                 * 加下限保护: 总压至少按 1V 计, 且总压过低时绝缘判定无意义 */
                uint32_t v_series = pack.pack_mv / 1000u;
                if (v_series < 1) v_series = 1;
                if (pack.pack_mv > 0 && rmin > 0) {
                    pack.insulation_ohm_per_v = rmin / v_series;
                }
            }
        }
#endif

        /* 更新实时数据(供 OLED 显示 + 串口调试) */
        s_live_current_ma = pack.current_ma;
        s_live_adc_raw    = bsp_current_read_raw();

        /* 更新全局数据 */
        sys_data_set_pack(&pack);
        s_task_alive_acq = true;

        /* #4: 周期兜底刷盘 NVS 脏参数(去抖合并写, 防 Flash 磨损) */
        sys_params_flush();

        vTaskDelayUntil(&last, pdMS_TO_TICKS(TASK_ACQUISITION_PERIOD_MS));
    }
}

/* ====== 保护任务(100ms) ====== */
static void task_protection(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t dbg_cnt = 0;                                  /* 调试计数器 */

    while (1) {
        bms_pack_data_t pack;
        sys_data_get_pack(&pack);

        /* 保护处理(内含继电器控制) */
        bms_fault_mask_t fault = app_protection_process(&pack);

        /* 状态机驱动 */
        sys_state_e state = sys_fsm_process(fault);
        (void)state;

        /* 更新全局故障 */
        sys_data_set_fault(fault);

        /* 调试: 每秒打印一次电流和故障码, 便于排查 */
        if ((dbg_cnt % 10) == 0) {
            ESP_LOGI(TAG, "[prot] I=%+dmA fault=0x%08lX state=%d",
                     pack.current_ma, (unsigned long)fault, state);
        }
        dbg_cnt++;

        s_task_alive_prot = true;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TASK_PROTECTION_PERIOD_MS));
    }
}

/* ====== SOC 估算任务(1s) ====== */
static void task_soc_estimate(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        bms_pack_data_t pack;
        bms_soc_data_t  soc;
        sys_data_get_pack(&pack);
        sys_data_get_soc(&soc);

        app_soc_process(&pack, &soc);
        sys_data_set_soc(&soc);

        /* 充电策略决策
         * M3 修复: 远程控制(30s 覆盖窗口)期间保留远程命令设置的充电模式,
         * 不在此处覆盖, 避免 set_charge/set_relay 下发的模式被 1s 周期覆盖 */
        if (!sys_data_get_remote_override()) {
            bms_charge_mode_e mode = app_charge_process(soc.soc);
            sys_data_set_charge_mode(mode);
        }

        s_task_alive_soc = true;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TASK_SOC_PERIOD_MS));
    }
}

/* ====== 均衡任务(10s) ======
 * 依赖 AFE(被动均衡由 BQ76952 内置 / LTC6804 内置驱动), 屏蔽时不编译此函数 */
#if HW_ENABLE_LTC6804 || HW_ENABLE_BQ76952
static void task_balance(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        bms_pack_data_t pack;
        sys_data_get_pack(&pack);

        bms_balance_mask_t mask = app_balance_process(&pack);
        sys_data_set_balance_mask(mask);

        s_task_alive_bal = true;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TASK_BALANCE_PERIOD_MS));
    }
}
#endif  /* HW_ENABLE_LTC6804 */

/* ====== EMQX 高频上报任务(100ms) ======
 * 2026-08-18 双速率架构: EMQX 为主通道(100ms 精简帧), 华为云为备用通道(7s 全量帧).
 * 依赖 WiFi(需 MQTT2 第二通道在线), 屏蔽时不编译此函数 */
#if HW_ENABLE_WIFI
static void task_fast_report(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        s_task_alive_fast = true;

        /* 100ms 高频精简帧 → EMQX bms/<id>/fast (QoS0 尽力而为)
         * 内部自行判断 MQTT2 连接状态, 未连接时静默丢弃(华为云 7s 帧兜底) */
        bms_pack_data_t pack;
        bms_soc_data_t  soc;
        bms_fault_mask_t fault;
        sys_data_get_pack(&pack);
        sys_data_get_soc(&soc);
        fault = sys_data_get_fault();
        sys_mqtt_report_fast(&pack, &soc, fault);

        vTaskDelayUntil(&last, pdMS_TO_TICKS(TASK_FAST_PERIOD_MS));
    }
}
#endif  /* HW_ENABLE_WIFI */

/* ====== 通信任务(1s) ======
 * 依赖 WiFi 或 MCP2515, 屏蔽时不编译此函数 */
#if HW_ENABLE_WIFI || HW_ENABLE_TWAI
static void task_communication(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        /* 2026-08-08 修复: 循环开头先喂狗, 再执行网络操作.
         *   原因: sys_mqtt_check_reauth()/sys_mqtt_init() 内部可能阻塞
         *         (TLS select 超时/SNTP/DNS 等待), 原代码喂狗在循环末尾,
         *         网络卡死时 task_comm 长时间不刷新, 被看门狗判定死亡并复位. */
        s_task_alive_comm = true;

        /* ====== 通信健康状态聚合(设备自报, 取代前端推断) ======
         * 用 WiFi/MQTT/CAN 各模块真实连接态置位 comm_status 位域,
         * 随后 sys_mqtt_report 把它随属性上报一起上云;
         * 前端四张状态卡片改为读 comm_status, 监控才真正与设备同步. */
        {
            bms_comm_status_t cs = 0;
            if (sys_wifi_is_connected())   cs |= COMM_WIFI_CONNECTED;
            if (sys_mqtt_is_connected()) {
                cs |= COMM_MQTT_CONNECTED;
                cs |= COMM_TLS_OK;          // broker 为 mqtts://8883, 连接成功即 TLS 握手成功
            }
            /* 云端可达: 设备侧最近一次成功 publish 距现在 < (3×上报间隔 + 5s) */
            uint64_t lp = sys_mqtt_last_publish_ms();
            uint32_t ri = (uint32_t)(sys_params_get()->report_interval_sec);
            if (ri < 1) ri = BMS_REPORT_INTERVAL_SEC;
            if (lp > 0 && sys_mqtt_is_connected()) {
                uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
                uint64_t age_ms = (now_ms > lp) ? (now_ms - lp) : 0;
                if (age_ms < ((uint64_t)ri * 3000ULL + 5000ULL)) {
                    cs |= COMM_CLOUD_REACHABLE;
                }
            }
            if (sys_can_is_enabled()) {
                if (sys_can_is_ok()) cs |= COMM_CAN_OK;
            } else {
                cs |= COMM_CAN_DISABLED;    // 硬件未接线, 前端显示灰"未启用"而非故障
            }
            sys_data_set_comm_status(cs);
        }

        bms_pack_data_t pack;
        bms_soc_data_t  soc;
        bms_fault_mask_t fault;
        bms_charge_mode_e  chg_mode;
        bms_balance_mask_t bal_mask;
        sys_data_get_pack(&pack);
        sys_data_get_soc(&soc);
        fault    = sys_data_get_fault();
        chg_mode = sys_data_get_charge_mode();
        bal_mask = sys_data_get_balance_mask();

        /* 串口 CSV 上报(VOFA+ 兼容) */
        printf("%.1f,%.3f,%.3f,%d,%d,0x%08X\r\n",
               soc.soc * 100.0f,
               (float)pack.cell_mv_max / 1000.0f,
               (float)pack.cell_mv_min / 1000.0f,
               pack.current_ma,
               pack.temp_max_dc / 10,
               (unsigned)fault);

        /* MQTT JSON 上报(每 15 秒一次, WiFi 启用时有效, 内部判断连接状态)
         * 2026-08-07: 频率调整 60s -> 15s, 提升网页实时性.
         *   配额: 86400/10 = 8640 条/天(稳态, 默认10s上报), 加上故障切换 <10 条,
         *         低于华为云 15000 条/天消息上限, 且留足补传余量(最小7s=12343/天仍<15000). */
        /* 2026-08-06: 检查 MQTT 认证失败重连(旧 timestamp 过期后自动重连会认证失败,
         *             需要销毁客户端并重新 init 生成新的 HMAC 凭证) */
        sys_mqtt_check_reauth();
        /* H22: 离线缓存补传移到主循环分批执行(每批20条, 不阻塞 MQTT 事件线程,
         *      避免 keepalive 超时被服务端踢下线导致"认证成功后又断开") */
        sys_mqtt_process_pending_replay();
        static uint32_t mqtt_report_cnt = 0;
        /* 故障上报节流(2026-08-06 修复消息额度暴涨):
         *   旧逻辑: fault != FAULT_NONE 时每秒发一条 → 持续故障 86400 条/天, 远超 10000 限额
         *   新逻辑: 仅在故障"状态变化"(无<->有 / 故障掩码变化)时立即上报一次,
         *           持续故障不单独发事件(属性上报已含 fault 字段, 影子/网页照常更新).
         *   效果: 每天消息数稳定在 ~8640 条(正常上报, 默认10s) + 故障切换次数(通常 <10) */
        static bms_fault_mask_t s_last_fault = FAULT_NONE;
        const bool fault_changed = (fault != s_last_fault);
        if (fault_changed) {
            if (fault != FAULT_NONE) {
                /* 新故障发生 / 故障类型变化: 立即上报一次 */
                sys_mqtt_report_fault(fault);
                /* 2026-08-07: 补发属性上报(properties/report), 让设备影子里的 fault 秒级更新.
                 *   原因: Dashboard 只轮询华为云影子, 而事件上报(events/up)不更新影子,
                 *         若只发事件, 网页要等下一个 60s 周期才能看到告警(最坏~120s 延迟).
                 *   代价: 仅故障切换时多发一条, 每天 <10 条, 不影响消息额度. */
                sys_mqtt_report(&pack, &soc, fault);
            } else {
                /* 故障刚被清除(有->无): 发 fault_clear 事件(alert) + 立即发一次正常数据 */
                sys_mqtt_report_fault_clear(s_last_fault);
                sys_mqtt_report(&pack, &soc, fault);
            }
            s_last_fault = fault;
        }
        /* 2026-08-11: 上报间隔改为运行时参数(默认10s, 最小7s), 由网页 set_param 调节,
         *   保证设备消息数严格 ≤15000/天(7s=12343/天). TASK_COMM_PERIOD_MS=1000 => 计数==秒. */
        uint32_t _ri = (uint32_t)(sys_params_get()->report_interval_sec);
        if (_ri < 1) _ri = BMS_REPORT_INTERVAL_SEC;
        if (++mqtt_report_cnt >= _ri) {
            mqtt_report_cnt = 0;
            sys_mqtt_report(&pack, &soc, fault);
        }
        /* 2026-09-10 提速: EMQX-only 全量帧每 2s 一发(TASK_COMM_PERIOD_MS=1s => 计数==秒),
         *   不耗华为云 15000 条/天配额(该配额仍由上面 _ri>=7s 的全量上报保证),
         *   网页 KPI 全量数据刷新 7s→2s。EMQX 未连接时函数内部静默返回。 */
        static uint32_t emqx_only_cnt = 0;
        if (++emqx_only_cnt >= 2) {
            emqx_only_cnt = 0;
            sys_mqtt_report_emqx_only(&pack, &soc, fault);
        }

        /* CAN 全量报文: 0x180~0x187 (MCP2515 启用时有效, 内部容忍屏蔽) */
        sys_can_send_all(&pack, &soc, fault, chg_mode, bal_mask);

        vTaskDelayUntil(&last, pdMS_TO_TICKS(TASK_COMM_PERIOD_MS));
    }
}
#endif  /* HW_ENABLE_WIFI || HW_ENABLE_TWAI */

/* ====== OLED 显示任务说明 ======
 * 旧 task_oled 已删除, 统一使用上方的 task_oled_ui(4 页轮播 + 实时电流)
 * task_oled_ui 在 app_tasks_start 中通过 HW_ENABLE_OLED 宏条件创建
 * 周期: 2 秒/页 (4 页共 8 秒一轮), 与原 TASK_OLED_PERIOD_MS 不同
 * 存活标志: s_task_alive_oled 由 task_oled_ui 每次翻页后置位 */

/* ====== SD 卡日志任务(1s) ======
 * 依赖 SD 卡, 屏蔽时不编译此函数 */
#if HW_ENABLE_SD_CARD
static void task_sd_log(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        bms_pack_data_t pack;
        bms_soc_data_t  soc;
        bms_fault_mask_t fault;
        sys_data_get_pack(&pack);
        sys_data_get_soc(&soc);
        fault = sys_data_get_fault();

        /* 故障时缩短周期到 100ms */
        TickType_t period = (fault != FAULT_NONE) ? pdMS_TO_TICKS(SD_LOG_PERIOD_FAULT_MS)
                                                  : pdMS_TO_TICKS(SD_LOG_PERIOD_NORMAL_MS);
        sys_storage_log_frame(&pack, &soc, fault);

        s_task_alive_sd = true;
        vTaskDelayUntil(&last, period);
    }
}
#endif  /* HW_ENABLE_SD_CARD */

/* ================================================================
 * OTA 命令异步调度器: MQTT 回调线程里不阻塞下载, 改为 set EventBits,
 *                     task_ota 专用线程里阻塞执行.
 *   为什么? 因为 app_ota_check_and_upgrade() / app_ota_upgrade_with_url()
 *   可能阻塞长达 3~5 分钟 (固件下载). 如果在 MQTT 事件线程里阻塞:
 *   - MQTT PINGREQ 心跳超时 → broker 踢下线
 *   - QoS1 PUBACK 无法回 → broker 下次重投命令 → 命令重复执行
 *   - MQTT 下行消息堆积 → 各种不可预期副作用
 * ================================================================ */
static EventGroupHandle_t s_ota_evt_grp = NULL;
static StaticEventGroup_t s_ota_evt_grp_mem;
#define OTA_EVT_NEED_CHECK      (1U << 0)       /* 普通检查升级: ota_check */
#define OTA_EVT_NEED_UPGRADE    (1U << 1)       /* 强制升级指定URL: ota_upgrade */
static char s_ota_upgrade_url[384] = {0};       /* ota_upgrade 传入的 URL (最长383B) */

static void ota_scheduler_init(void)
{
    if (s_ota_evt_grp == NULL) {
        s_ota_evt_grp = xEventGroupCreateStatic(&s_ota_evt_grp_mem);
    }
}

/* 非阻塞 (由 MQTT 回调调用): 设置标志, 返回. URL 会被复制 */
static void ota_scheduler_request_check(void)
{
    ota_scheduler_init();
    xEventGroupSetBits(s_ota_evt_grp, OTA_EVT_NEED_CHECK);
}
static void ota_scheduler_request_upgrade(const char *url)
{
    ota_scheduler_init();
    /* 2026-08-10 P2 说明: s_ota_upgrade_url 由 MQTT 回调线程写入、task_ota 读取,
     * xEventGroupSetBits 内部临界区(portENTER_CRITICAL)提供写-读屏障,
     * 且升级命令为低频事件, 竞争窗口极小; 保留 strncpy 显式 NUL 终止 */
    if (url && url[0]) {
        strncpy(s_ota_upgrade_url, url, sizeof(s_ota_upgrade_url) - 1);
        s_ota_upgrade_url[sizeof(s_ota_upgrade_url) - 1] = '\0';
    } else {
        s_ota_upgrade_url[0] = '\0';
    }
    xEventGroupSetBits(s_ota_evt_grp, OTA_EVT_NEED_UPGRADE);
}

/* ====== OTA 任务(事件触发 + 周期保底检查) ======
 * 依赖 WiFi, 屏蔽时不编译此函数 */
#if HW_ENABLE_WIFI
static void task_ota(void *arg)
{
    (void)arg;
    ota_scheduler_init();

    /* 把循环中 600s 的保底检查周期改成 BMS_OTA_CHECK_PERIOD_MS / 1000 (默认 12h) */
    int period_sec = (BMS_OTA_CHECK_PERIOD_MS >= 1000) ? (BMS_OTA_CHECK_PERIOD_MS / 1000) : (12 * 3600);
    int sec_cnt = period_sec - 10;   /* 启动后 ~10s 先做一次检查, 便于调试(若已12h则跳过) */
    if (sec_cnt < 0) sec_cnt = 0;

    while (1) {
        /* 检查是否有外部命令触发: ota_check 或 ota_upgrade
         * 等待 1000ms, 返回值里若有对应 bit 立即执行 */
        EventBits_t bits = xEventGroupWaitBits(s_ota_evt_grp,
                                OTA_EVT_NEED_CHECK | OTA_EVT_NEED_UPGRADE,
                                pdTRUE,       /* 收到后自动清位 */
                                pdFALSE,      /* 任一 bit 满足即可 */
                                pdMS_TO_TICKS(1000));

        s_task_alive_ota = true;       /* 每秒都喂狗 */

        /* 优先级: 强制升级 > 普通检查 */
        if ((bits & OTA_EVT_NEED_UPGRADE) != 0) {
            ESP_LOGI(TAG, "[OTA] 触发强制升级: %s",
                     s_ota_upgrade_url[0] ? s_ota_upgrade_url : "(空)");
            if (s_ota_upgrade_url[0]) {
                s_task_alive_ota = true;
                app_ota_upgrade_with_url(s_ota_upgrade_url);
                s_task_alive_ota = true;
            }
            s_ota_upgrade_url[0] = '\0';
        } else if ((bits & OTA_EVT_NEED_CHECK) != 0) {
            ESP_LOGI(TAG, "[OTA] 触发手动检查升级");
            s_task_alive_ota = true;
            app_ota_check_and_upgrade();
            s_task_alive_ota = true;
        } else {
            /* 没有事件: 每秒 +1 计数, 到达 period_sec 时做一次保底周期检查 */
            sec_cnt++;
            if (sec_cnt >= period_sec) {
                sec_cnt = 0;
                ESP_LOGI(TAG, "[OTA] %dh 保底周期检查升级...", period_sec / 3600);
                s_task_alive_ota = true;
                app_ota_check_and_upgrade();
                s_task_alive_ota = true;
            }
        }
    }
}
#endif  /* HW_ENABLE_WIFI */

/* ====== 看门狗任务(1s) ======
 * 职责: 1) 喂硬件看门狗  2) 心跳 LED  3) 监控 8 个业务任务存活
 * 机制: 每个任务在自己的循环中把存活标志置 true, 看门狗每秒检查
 *       若某标志为 false 则对应计数器 +1, 连续 3 秒未刷新判定任务死亡
 *       死亡后打印告警, 连续多次死亡则停止喂狗触发系统复位 */
static uint8_t s_dead_cnt_acq  = 0;               // 采集任务死亡计数
static uint8_t s_dead_cnt_prot = 0;               // 保护任务死亡计数
static uint8_t s_dead_cnt_soc  = 0;               // SOC 任务死亡计数
#if HW_ENABLE_LTC6804 || HW_ENABLE_BQ76952
static uint8_t s_dead_cnt_bal  = 0;               // 均衡任务死亡计数
#endif
#if HW_ENABLE_WIFI || HW_ENABLE_TWAI
static uint8_t s_dead_cnt_comm = 0;               // 通信任务死亡计数
static uint8_t s_dead_cnt_fast = 0;               // EMQX 高频上报任务死亡计数(2026-08-18)
#endif
#if HW_ENABLE_OLED
static uint8_t s_dead_cnt_oled = 0;               // OLED 任务死亡计数
#endif
#if HW_ENABLE_SD_CARD
static uint8_t s_dead_cnt_sd   = 0;               // SD 日志任务死亡计数
#endif
#if HW_ENABLE_WIFI
/* OTA 任务死亡计数: 原 WD_CHECK 被注释, 加 __attribute__((unused)) 防 -Werror=unused-variable */
static uint8_t s_dead_cnt_ota  __attribute__((unused)) = 0;
#endif
static uint8_t s_dead_cnt_alarm = 0;              // 报警任务死亡计数

/* 宏: 检查单个任务存活状态(按任务周期差异化阈值)
 * alive_flag : 存活标志指针
 * dead_cnt   : 死亡计数器指针
 * name       : 任务名(日志用)
 * max_miss   : 允许的最大未刷新次数(根据任务周期设定, 一般 = 周期秒数 + 5)
 *
 * Bug修复: 原固定阈值 3 秒对长周期任务(均衡10s/OTA 600s)过于严格, 导致误报
 *          现按任务周期差异化设置阈值, 避免误报 */
#define WD_CHECK(alive_flag, dead_cnt, name, max_miss) do {           \
    if (alive_flag) { alive_flag = false; dead_cnt = 0; }             \
    else {                                                            \
        dead_cnt++;                                                   \
        if (dead_cnt == (max_miss)) {                                 \
            ESP_LOGE(TAG, "[看门狗] %s 任务死亡(%u次未刷新)!", name, (unsigned)dead_cnt); \
        }                                                             \
    }                                                                 \
} while (0)

/* ================================================================
 * 报警任务(100ms 周期)
 * 驱动 LED/蜂鸣器分级报警, 根据故障掩码输出不同闪烁/鸣叫模式
 * - 正常:   LED 1Hz 心跳, 蜂鸣器静音
 * - 预警级: LED 2.5Hz 慢闪, 蜂鸣器短鸣 100ms + 静音 1900ms
 * - 保护级: LED 5Hz 快闪, 蜂鸣器鸣 200ms + 静音 800ms
 * - 严重级: LED 常亮, 蜂鸣器急促鸣 100ms + 静音 100ms
 * 存活标志: s_task_alive_alarm 每次循环后置位
 * ================================================================ */
static void task_alarm(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint8_t beep_loops = 0;                    /* 短按蜂鸣剩余周期(100ms/个) */
    ESP_LOGI(TAG, "alarm task start (period=%dms)", TASK_ALARM_PERIOD_MS);

    while (1) {
        app_alarm_process();

        /* ---- SW_PWR 电源键处理(2026-09-09 迁移自 task_oled_ui:
         *      原轮询在 task_oled_ui 内, OLED 关闭后该任务不创建,
         *      电源键彻底失效; task_alarm 常驻(100ms)不受 OLED 开关影响 ---- */
        if (bsp_sw_pwr_process()) {
            /* 长按 3s 关功率: 同步全局状态 + 置覆盖标志,
             * 否则保护任务 1s 内把 FET 重新闭合(实测 bug) */
            sys_data_set_master_power_off(true);
            sys_data_set_relay(false, false);
            sys_data_set_remote_override(true);
            ESP_LOGW(TAG, "[SW_PWR] 主电源关闭(CHG/DSG 全断), 云端下发开启可恢复");
        }
        if (bsp_sw_pwr_take_short_press()) {
            /* 短按: 蜂鸣短促一声(在位/心跳确认) + 立即上报一帧信息 */
            bsp_buzzer_set(true);
            beep_loops = 1;                    /* 下个循环(100ms)自动关 */
            sys_mqtt_report_info();
            ESP_LOGI(TAG, "[SW_PWR] 短按: 蜂鸣确认 + 立即上报");
        }
        if (beep_loops > 0 && --beep_loops == 0) {
            bsp_buzzer_set(false);
        }

        /* ---- 独立配网通道: GPIO0 BOOT 键长按 5s → 清配置重启进 AP 配网
         *      (常驻此处, 不随 OLED 屏蔽而失效) ---- */
        if (bsp_button_provision_check()) {
            ESP_LOGW(TAG, "检测到 BOOT 键长按 5s, 清除 WiFi 配置并重启进入 AP 配网...");
            sys_wifi_clear_config();
            sys_params_flush_now();          /* 立即落盘, 防止重启前丢失 */
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        s_task_alive_alarm = true;                       /* 喂看门狗存活标志 */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TASK_ALARM_PERIOD_MS));
    }
}

static void task_watchdog(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    /* 注册看门狗 */
    esp_err_t wdt_ret = esp_task_wdt_add(NULL);
    if (wdt_ret != ESP_OK) {
        ESP_LOGE(TAG, "wdt add fail: %s", esp_err_to_name(wdt_ret));
    }

    while (1) {
        /* 喂硬件看门狗 */
        esp_task_wdt_reset();

        /* 注: LED 心跳/报警由 task_alarm 统一驱动, 此处不再 toggle */

        /* 逐个检查业务任务存活状态(仅检查实际创建的任务)
         * 阈值按任务周期差异化设置, 避免长周期任务误报:
         *   采集100ms/保护100ms/报警100ms -> 15次(1.5s)
         *   SOC 1s/通信1s/SD 1s           -> 15次(15s)
         *   均衡 10s                       -> 15次(15s, 周期+余量)
         *   OLED 100ms                    -> 15次(1.5s)
         *   OTA 600s                      -> 3次(1800s, 仅日志不复位) */
#if HW_ENABLE_LTC6804 || HW_ENABLE_CURRENT_SENSE
        WD_CHECK(s_task_alive_acq,  s_dead_cnt_acq,  "task_acq",  15);
#endif
        WD_CHECK(s_task_alive_prot, s_dead_cnt_prot, "task_prot", 15);
        WD_CHECK(s_task_alive_soc,  s_dead_cnt_soc,  "task_soc",  15);
#if HW_ENABLE_LTC6804 || HW_ENABLE_BQ76952
        WD_CHECK(s_task_alive_bal,  s_dead_cnt_bal,  "task_bal",  15);
#endif
#if HW_ENABLE_WIFI || HW_ENABLE_TWAI
        WD_CHECK(s_task_alive_comm, s_dead_cnt_comm, "task_comm", 15);
#endif
#if HW_ENABLE_WIFI
        /* EMQX 高频上报任务(100ms, 2026-08-18) */
        WD_CHECK(s_task_alive_fast, s_dead_cnt_fast, "task_fast", 15);
#endif
#if HW_ENABLE_OLED
        WD_CHECK(s_task_alive_oled, s_dead_cnt_oled, "task_oled", 15);
#endif
#if HW_ENABLE_SD_CARD
        WD_CHECK(s_task_alive_sd,   s_dead_cnt_sd,   "task_sd",   15);
#endif
#if HW_ENABLE_WIFI
        /* OTA 任务暂未使用, 屏蔽看门狗监控避免误报
         * 启用 OTA 时恢复此 WD_CHECK 调用即可 */
        /* WD_CHECK(s_task_alive_ota,  s_dead_cnt_ota,  "task_ota",   3); */
#endif
        WD_CHECK(s_task_alive_alarm, s_dead_cnt_alarm, "task_alarm", 15);

        /* 任一已创建任务死亡次数累计 >= 阈值则主动复位系统
         * 注: OTA 任务不参与复位触发(其 HTTP 下载可能长时间阻塞, 属正常行为)
         *
         * Bug修复: 原代码只 vTaskDelay(5000) 不喂狗, 期望硬件看门狗30s超时复位
         *          但 5s 后回到循环顶部又喂狗了, 导致永不复位, 任务死后进入僵尸状态
         *          现改为主动 esp_restart() 立即复位 */
        if (
#if HW_ENABLE_LTC6804 || HW_ENABLE_CURRENT_SENSE
            s_dead_cnt_acq >= 15  ||
#endif
            s_dead_cnt_prot >= 15 ||
            s_dead_cnt_soc >= 15  ||
#if HW_ENABLE_LTC6804 || HW_ENABLE_BQ76952
            s_dead_cnt_bal >= 15  ||
#endif
#if HW_ENABLE_WIFI || HW_ENABLE_TWAI
            s_dead_cnt_comm >= 15 ||
#endif
#if HW_ENABLE_WIFI
            /* 2026-08-19 修复(F8): EMQX 高频上报任务死亡也触发主动复位——
             *   原实现仅 WD_CHECK 统计不参与复位, task_fast 死后主通道静默失效无恢复 */
            s_dead_cnt_fast >= 15 ||
#endif
#if HW_ENABLE_OLED
            s_dead_cnt_oled >= 15 ||
#endif
#if HW_ENABLE_SD_CARD
            s_dead_cnt_sd >= 15   ||
#endif
            s_dead_cnt_alarm >= 15 ||
            false) {
            ESP_LOGE(TAG, "[看门狗] 任务持续死亡, 5秒后主动复位系统!");
            vTaskDelay(pdMS_TO_TICKS(5000));     /* 留时间打印日志 */
            esp_restart();                        /* 主动复位, 不依赖硬件看门狗 */
        }

        vTaskDelayUntil(&last, pdMS_TO_TICKS(TASK_WATCHDOG_PERIOD_MS));

        /* ====== 每 30s 采集栈高水位摘要, 存入 sys_data 供 MQTT 上报 ======
         * 前端 /api/diag 或 bms_data 事件可展示, <512B 标红告警 */
        {
            static uint8_t _wm_cnt = 0;
            if (++_wm_cnt >= 30) {
                _wm_cnt = 0;
                static TaskStatus_t _wm_stats[16];
                UBaseType_t _wm_n = uxTaskGetNumberOfTasks();
                if (_wm_n > 16) _wm_n = 16;
                _wm_n = uxTaskGetSystemState(_wm_stats, _wm_n, NULL);
                char _buf[128] = "";
                int _off = 0;
                for (UBaseType_t i = 0; i < _wm_n && _off < (int)sizeof(_buf) - 1; i++) {
                    uint32_t fb = (uint32_t)_wm_stats[i].usStackHighWaterMark * sizeof(StackType_t);
                    int n = snprintf(_buf + _off, sizeof(_buf) - _off,
                                     "%s%s=%lu", (i > 0 ? "," : ""),
                                     _wm_stats[i].pcTaskName, (unsigned long)fb);
                    if (n > 0 && _off + n < (int)sizeof(_buf)) _off += n;
                }
                sys_data_set_stack_summary(_buf);
                /* 低于 512B 的任务单独告警 */
                for (UBaseType_t i = 0; i < _wm_n; i++) {
                    uint32_t fb = (uint32_t)_wm_stats[i].usStackHighWaterMark * sizeof(StackType_t);
                    if (fb < 512) {
                        ESP_LOGW(TAG, "[栈水位] %s 仅剩 %luB, 有溢出风险!", _wm_stats[i].pcTaskName, (unsigned long)fb);
                    }
                }

                /* ====== B1 黑匣子: 周期刷新死前现场快照(30s 一次, 独立 NVS key) ======
                 * 仅在存在异常风险时写? 否: 统一周期刷新, Flash 写入 = 2880 次/天,
                 * 远低于 NVS 磨损预算(单 key 轮询页, 10 万次擦写/页 → 数十年寿命).
                 * 记录当前运行点, 下次异常复位时启动代码读出打印+上云 → 死前现场可追溯. */
                /* ====== B4 Flash 磨损审计修正(2026-09-15) ======
                 * 原实现随看门狗循环 1s 写一次 = 86400 次/天, 超 NVS 磨损预算
                 * (单 key 页 10 万次擦写 → 约 3 年磨穿). 节流到 30s:
                 * 2880 次/天 → 同页擦写均摊后寿命 > 90 年, 且死前现场最大 30s 盲区
                 * 对定位足够(死前最后一分钟的状态由 1~2 个快照覆盖). */
                {
                    static uint32_t s_bb_last_ms = 0;
                    uint32_t now_bb = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
                    if (s_bb_last_ms == 0 ||
                        (now_bb - s_bb_last_ms) >= 30000) {
                        s_bb_last_ms = now_bb;
                        bms_pack_data_t _bb_pack;
                        sys_data_get_pack(&_bb_pack);
                        bms_soc_data_t _bb_soc;
                        sys_data_get_soc(&_bb_soc);
                        uint8_t _soc_pct = (uint8_t)(_bb_soc.soc * 100.0f + 0.5f);
                        if (_soc_pct > 100) _soc_pct = 100;
                        /* pack_mv 为 uint32(32S 满充可达 134V), 按 0.1V 精度压缩到 uint16 */
                        uint16_t _mv = (uint16_t)(_bb_pack.pack_mv / 100);
                        int16_t _ma = _bb_pack.current_ma;
                        (void)sys_params_set_blackbox_ctx(
                            sys_data_get_fault(), _soc_pct, _mv, _ma);
                    }
                }
            }
        }
    }
}

/* ====== 栈高水位诊断(2026-08-14 质量修复 P1) ======
 * 枚举所有 FreeRTOS 任务并打印剩余栈空间(高水位 = 历史最小空闲字节)。
 * 用于实证确认 2KB 小栈任务(task_bal/task_alarm)余量, 防止静默栈溢出。
 * 使用静态缓冲, 不在运行时做堆分配(符合固件铁律)。 */
#define TASK_STATS_MAX  32
void app_dump_task_stack_watermarks(void)
{
    static TaskStatus_t s_stats[TASK_STATS_MAX];
    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n > TASK_STATS_MAX) {
        n = TASK_STATS_MAX;
    }
    n = uxTaskGetSystemState(s_stats, n, NULL);
    ESP_LOGI(TAG, "===== 任务栈高水位(剩余空闲; 单位 B; 越小越危险) =====");
    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t free_bytes = (uint32_t)s_stats[i].usStackHighWaterMark
                              * (uint32_t)sizeof(StackType_t);
        ESP_LOGI(TAG, "  %-14s free=%5u B (%u words)",
                 s_stats[i].pcTaskName, (unsigned)free_bytes,
                 (unsigned)s_stats[i].usStackHighWaterMark);
    }
}

/* ====== 网络初始化任务(2026-08-19 F1/F2 修复) ======
 * 把 WiFi 配网/连接/MQTT 初始化从 app_system_init 拆到独立低优先级任务:
 *   - 保护/采集/看门狗等核心任务立即启动, 消除"配网/联网期间电池零保护"空窗
 *   - 本任务可能阻塞较久(AP 门户最长 BMS_CONFIG_PORTAL_TIMEOUT_MS /
 *     已配网时 STA+SNTP+DNS 最长 ~23s), 放独立任务不影响核心任务调度 */
#if HW_ENABLE_WIFI
static void task_net_init(void *arg)
{
    (void)arg;
    /* 2026-08-13: 重新烧录检测 — 串口烧录新固件后自动清 WiFi 配置进 AP 配网.
     * 必须在读取 wifi_configured / 选择 STA/AP 之前调用(OTA 升级不触发). */
    sys_wifi_check_reflash();

    const bms_params_t *p = sys_params_get();
    if (p->wifi_configured) {
        /* 已配网, 直接 STA 模式连接 */
        ESP_LOGI(TAG, "已配网, STA 模式连接 SSID=%s", p->wifi_ssid);
        if (sys_wifi_init_sta() == BMS_OK) {
            sys_mqtt_init();
        } else {
            /* 已配网但连接失败: 不回退 AP 模式, 继续后台持久重连
             * (sys_wifi 事件回调已注册 esp_timer 指数退避重连)
             * 直接初始化 MQTT, WiFi 恢复后 MQTT 会自动连上 */
            ESP_LOGW(TAG, "WiFi 初始连接失败, 进入后台持久重连模式(不回退 AP)");
            sys_mqtt_init();
        }
    } else {
        /* 首次开机或配置丢失, 进入 AP 配网模式 */
        ESP_LOGI(TAG, "首次开机或未配网, 启动 AP 配网门户");
        if (sys_wifi_start_ap() == BMS_OK) {
            /* 阻塞直到用户配网成功(成功后内部 esp_restart)或超时 */
            bms_err_t portal_err = sys_config_portal_start(BMS_CONFIG_PORTAL_TIMEOUT_MS);
            if (portal_err == BMS_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "配网超时, 重启设备");
                esp_restart();
            }
        } else {
            ESP_LOGE(TAG, "AP 启动失败, 系统降级运行(无 WiFi)");
        }
    }
    /* 任务完成, 自我删除(WiFi/MQTT 事件回调/重连定时器在驱动内部独立运行) */
    vTaskDelete(NULL);
}
#endif  /* HW_ENABLE_WIFI */

/* ====== 任务创建(按硬件屏蔽宏决定启动哪些任务) ====== */
bms_err_t app_tasks_start(void)
{
    BaseType_t ret;

    /* 宏: 创建任务失败则报错返回 */
    #define CREATE_TASK(func, name, prio, stack)                       \
        do {                                                           \
            ret = xTaskCreate(func, name, stack, NULL, prio, NULL);    \
            if (ret != pdPASS) {                                       \
                ESP_LOGE(TAG, "create %s fail", name);                 \
                return BMS_ERR_FAIL;                                   \
            }                                                          \
            ESP_LOGI(TAG, "  [OK] %s (prio=%d, stack=%d)", name, prio, stack); \
        } while (0)

    ESP_LOGI(TAG, "===== 启动 FreeRTOS 任务调度 =====");

    /* 采集任务(100ms) - 依赖 LTC6804/电流ADC, 任一启用即可 */
#if HW_ENABLE_LTC6804 || HW_ENABLE_CURRENT_SENSE
    CREATE_TASK(task_acquisition,   "task_acq",   TASK_ACQUISITION_PRIO, TASK_ACQUISITION_STACK);
#else
    ESP_LOGW(TAG, "  [--] task_acq 跳过 (无 LTC6804/电流ADC)");
#endif

    /* 保护任务(100ms) - 无硬件依赖, 必启动 */
    CREATE_TASK(task_protection,    "task_prot",  TASK_PROTECTION_PRIO,  TASK_PROTECTION_STACK);

    /* SOC 估算任务(1s) - 无硬件依赖, 必启动 */
    CREATE_TASK(task_soc_estimate,  "task_soc",   TASK_SOC_PRIO,         TASK_SOC_STACK);

    /* 均衡任务(10s) - 依赖 AFE(BQ76952 内置 / LTC6804 内置被动均衡) */
#if HW_ENABLE_LTC6804 || HW_ENABLE_BQ76952
    CREATE_TASK(task_balance,       "task_bal",   TASK_BALANCE_PRIO,     TASK_BALANCE_STACK);
#else
    ESP_LOGW(TAG, "  [--] task_bal 跳过 (无 AFE)");
#endif

    /* 通信任务(1s) - WiFi/MQTT/CAN 至少一个启用 */
#if HW_ENABLE_WIFI || HW_ENABLE_TWAI
    CREATE_TASK(task_communication, "task_comm",  TASK_COMM_PRIO,        TASK_COMM_STACK);
#else
    ESP_LOGW(TAG, "  [--] task_comm 跳过 (无 WiFi/CAN)");
#endif

    /* EMQX 高频上报任务(100ms) - 2026-08-18 双速率架构, 仅 WiFi 启用 */
#if HW_ENABLE_WIFI
    CREATE_TASK(task_fast_report,   "task_fast",  TASK_FAST_PRIO,        TASK_FAST_STACK);
#else
    ESP_LOGW(TAG, "  [--] task_fast 跳过 (无 WiFi)");
#endif

    /* OLED 显示任务(2s/页, 4页轮播) - 依赖 OLED */
#if HW_ENABLE_OLED
    CREATE_TASK(task_oled_ui,       "task_oled",  TASK_OLED_PRIO,        TASK_OLED_STACK);
#else
    ESP_LOGW(TAG, "  [--] task_oled 跳过 (无 OLED)");
#endif

    /* SD 卡日志任务(1s) - 依赖 SD 卡 */
#if HW_ENABLE_SD_CARD
    CREATE_TASK(task_sd_log,        "task_sd",    TASK_SD_LOG_PRIO,      TASK_SD_LOG_STACK);
#else
    ESP_LOGW(TAG, "  [--] task_sd 跳过 (无 SD 卡)");
#endif

    /* OTA 任务 - 依赖 WiFi */
#if HW_ENABLE_WIFI
    CREATE_TASK(task_ota,           "task_ota",   TASK_OTA_PRIO,         TASK_OTA_STACK);
#else
    ESP_LOGW(TAG, "  [--] task_ota 跳过 (无 WiFi)");
#endif

    /* Modbus RTU 从站任务(RS485) - 依赖 RS485 硬件 */
#if HW_ENABLE_RS485
    CREATE_TASK(sys_modbus_task,    "task_mbus",  BMS_MODBUS_TASK_PRIO,  BMS_MODBUS_TASK_STACK);
#else
    ESP_LOGW(TAG, "  [--] task_mbus 跳过 (无 RS485)");
#endif

    /* 网络初始化任务(2026-08-19 F1/F2) - 低优先级, WiFi 配网/连接可能长时间阻塞,
     * 放最后创建让保护/采集等核心任务先行启动, 消除配网期间零保护空窗 */
#if HW_ENABLE_WIFI
    CREATE_TASK(task_net_init,      "task_net",   TASK_COMM_PRIO,      TASK_COMM_STACK);
#endif

    /* 看门狗任务(1s) - 必启动, 监控其他任务存活 */
    CREATE_TASK(task_watchdog,      "task_wdt",   TASK_WATCHDOG_PRIO,    TASK_WATCHDOG_STACK);

    /* 报警任务(100ms) - 必启动, 驱动 LED/蜂鸣器分级报警 */
    CREATE_TASK(task_alarm,         "task_alarm", TASK_ALARM_PRIO,       TASK_ALARM_STACK);

    #undef CREATE_TASK

    ESP_LOGI(TAG, "===== 全部任务启动完成 =====");
    return BMS_OK;
}
