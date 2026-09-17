/**
 * @file    app_key_ui.c
 * @brief   单键 UI 管理器实现(页面切换 + 菜单 + 参数设置 + 解除报警)
 * @author  BMS Team
 * @date    2026-08
 * @note    归属层: App (UI 业务逻辑)
 *          原理: 状态机 + 单键三事件(短按/双击/长按)
 *
 *          ===== 按键事件映射 =====
 *          | 状态         | 短按(<3s)          | 双击(400ms)        | 长按(>=3s)        |
 *          |--------------|---------------------|---------------------|--------------------|
 *          | DISPLAY(显示)| 下一页(停止自动轮播)| 进入主菜单          | 解除报警/返回 Page0 |
 *          | MENU(主菜单)| 下一项(循环)        | 进入选中的子菜单    | 返回 DISPLAY        |
 *          | SUBMENU(子) | 下一项(循环)        | 进入 EDIT 编辑模式  | 返回 MENU 主菜单    |
 *          | EDIT(编辑)  | 当前值 + 一步       | 保存并退回 SUBMENU  | 取消退回 SUBMENU    |
 */
#include "app_key_ui.h"
#include "bsp_button.h"
#include "bms_config.h"
#include "bms_pinmap.h"
#include "sys_params.h"
#include "sys_wifi.h"
#include "sys_data.h"
#include "esp_log.h"
#include "esp_system.h"                               // esp_restart() 重启

/* FreeRTOS 头(任务延时/滴答计数) */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_KEY_UI";

/* ================================================================
 *                  内部状态(static 化, 不 extern)
 * ================================================================ */
static bool      s_button_ok = false;                   // 按键硬件是否可用
static ui_mode_e s_mode      = UI_MODE_DISPLAY;         // 当前 UI 模式
static uint8_t   s_page      = 0;                       // DISPLAY 模式当前页 0~3
static uint32_t  s_last_user_tick = 0;                  // 最后一次用户操作时间(菜单超时用)

/* 菜单索引 */
static menu_id_e s_menu_idx     = MENU_ID_CLEAR_FAULT;  // 主菜单当前选中项
static uint8_t   s_submenu_idx  = 0;                    // 子菜单当前选中项(子菜单内 0~N-1)

/* 参数编辑: 备份原值 + 当前修改值 + 当前子菜单归属 */
static int32_t   s_edit_orig    = 0;                    // 编辑前原值(取消时恢复)
static int32_t   s_edit_cur     = 0;                    // 当前编辑值
static menu_id_e s_edit_parent  = MENU_ID_MAX;          // 参数所属哪个子菜单(保护/均衡/规格/SOC)

/* ================================================================
 *              菜单/子菜单 静态字符串表(只读, 放 Flash)
 * ================================================================ */
/* 主菜单 */
static const char * const s_menu_items[MENU_ID_MAX] = {
    "01 解除报警",
    "02 保护阈值",
    "03 均衡设置",
    "04 电池规格",
    "05 SOC 校准",
    "06 系统操作",
};

/* 子菜单: 保护阈值 */
static const char * const s_sub_prot_items[SUB_PROT_MAX] = {
    "过压保护(mV)",
    "欠压保护(mV)",
    "过温保护(0.1C)",
    "低温保护(0.1C)",
    "充电过流(mA)",
    "放电过流(mA)",
};

/* 子菜单: 均衡设置 */
static const char * const s_sub_bal_items[SUB_BAL_MAX] = {
    "均衡触发(mV)",
    "均衡停止(mV)",
};

/* 子菜单: 电池规格 */
static const char * const s_sub_batt_items[SUB_BATT_MAX] = {
    "单体容量(mAh)",
    "串联数(6/12)",
};

/* 子菜单: SOC 校准(x10 存储, 显示时 /10) */
static const char * const s_sub_soc_items[SUB_SOC_MAX] = {
    "SOC 偏移(x0.01)",
    "SOC 增益(x0.01)",
};

/* 子菜单: 系统操作 */
static const char * const s_sub_sys_items[SUB_SYS_MAX] = {
    "复位参数默认",
    "重启系统",
    "重新配网(WiFi)",
};

/* ================================================================
 *                    参数读/写 辅助函数
 * ================================================================ */

/* 根据子菜单类型 + 子索引, 读取当前参数原始值 */
static int32_t param_read_current(menu_id_e parent, uint8_t sub_idx)
{
    const bms_params_t *p = sys_params_get();
    switch (parent) {
    case MENU_ID_PROT_THRESH:
        switch ((submenu_prot_e)sub_idx) {
        case SUB_PROT_OV:     return (int32_t)p->cell_ov_prot_mv;
        case SUB_PROT_UV:     return (int32_t)p->cell_uv_prot_mv;
        case SUB_PROT_OT:     return (int32_t)p->temp_ot_prot_dc;
        case SUB_PROT_UT:     return (int32_t)p->temp_ut_prot_dc;
        case SUB_PROT_CHG_OC: return (int32_t)p->chg_oc_prot_ma;
        case SUB_PROT_DSG_OC: return (int32_t)p->dsg_oc_prot_ma;
        default: return 0;
        }
    case MENU_ID_BALANCE_CFG:
        switch ((submenu_balance_e)sub_idx) {
        case SUB_BAL_THR:  return (int32_t)p->balance_threshold_mv;
        case SUB_BAL_STOP: return (int32_t)p->balance_stop_mv;
        default: return 0;
        }
    case MENU_ID_BATT_SPEC:
        switch ((submenu_battery_e)sub_idx) {
        case SUB_BATT_CAP:    return (int32_t)p->cell_capacity_mah;
        case SUB_BATT_SERIES: return (int32_t)p->cell_series_num;
        default: return 0;
        }
    case MENU_ID_SOC_CAL:
        /* 浮点转整数: 偏移 x100, 增益 x100 */
        switch ((submenu_soc_e)sub_idx) {
        case SUB_SOC_OFFSET: return (int32_t)(p->soc_offset * 100.0f);
        case SUB_SOC_GAIN:   return (int32_t)(p->soc_gain   * 100.0f);
        default: return 0;
        }
    default:
        return 0;
    }
}

/* 根据子菜单类型 + 子索引, 写入参数并保存到 NVS */
static bms_err_t param_write_back(menu_id_e parent, uint8_t sub_idx, int32_t val)
{
    /* 先读取当前值, 在副本上修改(避免破坏其他参数) */
    bms_params_t copy = *sys_params_get();
    switch (parent) {
    case MENU_ID_PROT_THRESH:
        switch ((submenu_prot_e)sub_idx) {
        case SUB_PROT_OV:     copy.cell_ov_prot_mv  = (uint16_t)val; break;
        case SUB_PROT_UV:     copy.cell_uv_prot_mv  = (uint16_t)val; break;
        case SUB_PROT_OT:     copy.temp_ot_prot_dc  = (int16_t)val;  break;
        case SUB_PROT_UT:     copy.temp_ut_prot_dc  = (int16_t)val;  break;
        case SUB_PROT_CHG_OC: copy.chg_oc_prot_ma   = (int16_t)val;  break;
        case SUB_PROT_DSG_OC: copy.dsg_oc_prot_ma   = (int16_t)val;  break;
        default: break;
        }
        break;
    case MENU_ID_BALANCE_CFG:
        switch ((submenu_balance_e)sub_idx) {
        case SUB_BAL_THR:  copy.balance_threshold_mv = (uint16_t)val; break;
        case SUB_BAL_STOP: copy.balance_stop_mv      = (uint16_t)val; break;
        default: break;
        }
        break;
    case MENU_ID_BATT_SPEC:
        switch ((submenu_battery_e)sub_idx) {
        case SUB_BATT_CAP:    copy.cell_capacity_mah = (uint16_t)val; break;
        case SUB_BATT_SERIES: copy.cell_series_num   = (uint8_t)val;  break;
        default: break;
        }
        break;
    case MENU_ID_SOC_CAL:
        switch ((submenu_soc_e)sub_idx) {
        case SUB_SOC_OFFSET: copy.soc_offset = (float)val / 100.0f; break;
        case SUB_SOC_GAIN:   copy.soc_gain   = (float)val / 100.0f; break;
        default: break;
        }
        break;
    default:
        return BMS_ERR_PARAM_INVALID;
    }
    return sys_params_set(&copy);
}

/* 获取参数步进(短按一次加多少) */
static int32_t param_get_step(menu_id_e parent, uint8_t sub_idx)
{
    (void)sub_idx;
    switch (parent) {
    case MENU_ID_PROT_THRESH: return 50;                  // 阈值 50mV/0.5℃/50mA 一步
    case MENU_ID_BALANCE_CFG: return 10;                  // 均衡 10mV 一步
    case MENU_ID_BATT_SPEC:
        /* H13 修复: 原固定 100 导致串联数 6→106 回绕只能设 3/6;
         * 容量按 100mAh 一步, 串联数按 1S 一步 */
        return (sub_idx == SUB_BATT_CAP) ? 100 : 1;
    case MENU_ID_SOC_CAL:     return 1;                   // SOC x100, 1 = 0.01
    default: return 1;
    }
}

/* 获取参数上下限(钳位) */
static void param_get_range(menu_id_e parent, uint8_t sub_idx,
                            int32_t *lo, int32_t *hi)
{
    switch (parent) {
    case MENU_ID_PROT_THRESH:
        switch ((submenu_prot_e)sub_idx) {
        case SUB_PROT_OV:     *lo = 3500; *hi = 4500; break;  // 单体 mV
        case SUB_PROT_UV:     *lo = 2500; *hi = 3500; break;
        case SUB_PROT_OT:     *lo = 400;  *hi = 800;  break;  // 0.1℃  40~80℃
        case SUB_PROT_UT:     *lo = -300; *hi = 200;  break;  // -30~+20℃
        case SUB_PROT_CHG_OC: *lo = 500;  *hi = 30000;break;  // mA
        case SUB_PROT_DSG_OC: *lo = 500;  *hi = 50000;break;
        default: *lo = 0; *hi = 0x7FFFFFFF; break;
        }
        break;
    case MENU_ID_BALANCE_CFG:
        *lo = 10;  *hi = 100;                                // mV
        break;
    case MENU_ID_BATT_SPEC:
        switch ((submenu_battery_e)sub_idx) {
        case SUB_BATT_CAP:    *lo = 1000;  *hi = 20000; break; // mAh
        case SUB_BATT_SERIES: *lo = 1;     *hi = BMS_HW_MAX_SERIES_NUM; break; // 串数 1~硬件上限 (F6: 防超硬件能力设串数)
        default: *lo = 0; *hi = 0x7FFFFFFF; break;
        }
        break;
    case MENU_ID_SOC_CAL:
        switch ((submenu_soc_e)sub_idx) {
        case SUB_SOC_OFFSET: *lo = -10; *hi = 10;   break;    // ±0.10 → x100
        case SUB_SOC_GAIN:   *lo = 90;  *hi = 110;  break;    // 0.9~1.1 → x100
        default: *lo = 0; *hi = 0x7FFFFFFF; break;
        }
        break;
    default:
        *lo = 0; *hi = 0x7FFFFFFF; break;
    }
}

/* ================================================================
 *                  子菜单信息获取(标题/项数/项表)
 * ================================================================ */
static void submenu_get_meta(menu_id_e parent,
                             const char **title,
                             const char * const **items,
                             uint8_t *count)
{
    switch (parent) {
    case MENU_ID_PROT_THRESH:
        *title = "子菜单: 保护阈值";
        *items = s_sub_prot_items;
        *count = SUB_PROT_MAX;
        break;
    case MENU_ID_BALANCE_CFG:
        *title = "子菜单: 均衡设置";
        *items = s_sub_bal_items;
        *count = SUB_BAL_MAX;
        break;
    case MENU_ID_BATT_SPEC:
        *title = "子菜单: 电池规格";
        *items = s_sub_batt_items;
        *count = SUB_BATT_MAX;
        break;
    case MENU_ID_SOC_CAL:
        *title = "子菜单: SOC 校准";
        *items = s_sub_soc_items;
        *count = SUB_SOC_MAX;
        break;
    case MENU_ID_SYS_OPS:
        *title = "子菜单: 系统操作";
        *items = s_sub_sys_items;
        *count = SUB_SYS_MAX;
        break;
    default:
        *title = "子菜单";
        *items = NULL;
        *count = 0;
        break;
    }
}

/* ================================================================
 *                  动作: 解除报警 / 参数复位 / 重启
 * ================================================================ */
static void action_clear_fault(void)
{
    bms_fault_mask_t cur = sys_data_get_fault();
    if (cur == FAULT_NONE) {
        ESP_LOGI(TAG, "当前无故障, 无需解除");
        return;
    }
    /* 清除所有预警类和可恢复类故障
     * 说明:
     *   - 预警类(WARN): 直接清除, 下次保护检测如条件仍满足会重新置位
     *   - 保护类(PROT) + 热失控: 原则上硬件触发应保持, 但用户可强制清除,
     *     如条件仍满足, 下一个保护周期(100ms)会重新置位, 所以安全地直接全清
     *   - 短路: 极危险, 但同样通过保护循环自恢复检查, 直接清除符合用户意图 */
    sys_data_clear_fault(cur);
    ESP_LOGW(TAG, "[解除报警] 已清除故障码 0x%08X, 如条件仍存在将在 100ms 内重新触发",
             (unsigned)cur);
}

static void action_reset_params(void)
{
    sys_params_reset();
    ESP_LOGW(TAG, "[系统] 已复位参数为默认值");
}

static void action_wifi_reset(void)
{
    /* 清除 WiFi 配置并重启, 重启后 wifi_configured=0 → 自动进入 AP 配网模式
     * (2026-08-13 新增: 手动重置配网入口, 不依赖重新烧录检测) */
    sys_wifi_clear_config();
    sys_params_flush_now();          /* 立即落盘, 防止重启前丢失 */
    ESP_LOGW(TAG, "[系统] WiFi 配置已清除, 3s 后重启进入 AP 配网...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void action_reboot(void)
{
    ESP_LOGW(TAG, "[系统] 3s 后重启...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ================================================================
 *                    状态机: 各模式事件处理
 * ================================================================ */

static void handle_display_mode(bms_button_event_e ev)
{
    switch (ev) {
    case BUTTON_EVENT_SHORT_PRESS:
        /* 短按: 下一页, 同时暂停自动轮播(用户手动控制优先) */
        s_page = (s_page + 1) % OLED_PAGE_COUNT;
        s_last_user_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "[DISPLAY] 手动切页 Page%d", s_page);
        break;

    case BUTTON_EVENT_DOUBLE_CLICK:
        /* 双击: 进入主菜单 */
        s_mode = UI_MODE_MENU;
        s_menu_idx = MENU_ID_CLEAR_FAULT;
        s_last_user_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "[DISPLAY] 双击进入主菜单");
        break;

    case BUTTON_EVENT_LONG_PRESS:
        /* 长按: 解除报警 + 回 Page0(最常用的一页) */
        s_page = 0;
        action_clear_fault();
        s_last_user_tick = xTaskGetTickCount();
        break;

    case BUTTON_EVENT_LONG_LONG_PRESS:
        /* 超长按(5s): 清除 WiFi 配置并重启 → 进入 AP 配网模式(产品现场重新配网入口) */
        ESP_LOGW(TAG, "[DISPLAY] 超长按 5s -> 重新配网");
        action_wifi_reset();
        break;

    default:
        break;
    }
}

static void handle_menu_mode(bms_button_event_e ev)
{
    switch (ev) {
    case BUTTON_EVENT_SHORT_PRESS:
        s_menu_idx = (menu_id_e)((s_menu_idx + 1) % MENU_ID_MAX);
        s_last_user_tick = xTaskGetTickCount();
        break;

    case BUTTON_EVENT_DOUBLE_CLICK: {
        /* 选中菜单项, 进入子菜单或直接执行 */
        if (s_menu_idx == MENU_ID_CLEAR_FAULT) {
            /* 解除报警: 立即执行, 不进子菜单 */
            action_clear_fault();
            s_mode = UI_MODE_DISPLAY;                    // 执行完退回显示
        } else {
            /* 进入子菜单 */
            s_mode = UI_MODE_SUBMENU;
            s_submenu_idx = 0;
        }
        s_last_user_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "[MENU] 选中 %d -> %s", s_menu_idx, s_menu_items[s_menu_idx]);
        break;
    }

    case BUTTON_EVENT_LONG_PRESS:
        /* 长按返回显示模式 */
        s_mode = UI_MODE_DISPLAY;
        s_last_user_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "[MENU] 长按返回显示");
        break;

    default:
        break;
    }
}

static void handle_submenu_mode(bms_button_event_e ev)
{
    const char *title = NULL;
    const char * const *items = NULL;
    uint8_t count = 0;
    submenu_get_meta(s_menu_idx, &title, &items, &count);
    if (count == 0) {
        s_mode = UI_MODE_MENU;                           // 容错, 直接退回
        return;
    }

    switch (ev) {
    case BUTTON_EVENT_SHORT_PRESS:
        s_submenu_idx = (s_submenu_idx + 1) % count;
        s_last_user_tick = xTaskGetTickCount();
        break;

    case BUTTON_EVENT_DOUBLE_CLICK:
        if (s_menu_idx == MENU_ID_SYS_OPS) {
            /* 系统操作: 立即执行 */
            switch ((submenu_sys_e)s_submenu_idx) {
            case SUB_SYS_RESET:     action_reset_params(); break;
            case SUB_SYS_REBOOT:    action_reboot();      break;
            case SUB_SYS_WIFI_RESET: action_wifi_reset(); break;
            default: break;
            }
            s_mode = UI_MODE_MENU;
        } else {
            /* 参数类: 进入编辑模式, 备份原值 */
            s_mode = UI_MODE_EDIT;
            s_edit_parent = s_menu_idx;
            s_edit_orig   = param_read_current(s_menu_idx, s_submenu_idx);
            s_edit_cur    = s_edit_orig;
            ESP_LOGI(TAG, "[SUB] 进入编辑, 原值=%ld", (long)s_edit_orig);
        }
        s_last_user_tick = xTaskGetTickCount();
        break;

    case BUTTON_EVENT_LONG_PRESS:
        /* 长按返回主菜单 */
        s_mode = UI_MODE_MENU;
        s_last_user_tick = xTaskGetTickCount();
        break;

    default:
        break;
    }
}

static void handle_edit_mode(bms_button_event_e ev)
{
    int32_t step = param_get_step(s_edit_parent, s_submenu_idx);
    int32_t lo, hi;
    param_get_range(s_edit_parent, s_submenu_idx, &lo, &hi);

    switch (ev) {
    case BUTTON_EVENT_SHORT_PRESS:
        /* 短按: 值 + 一步, 超上限回到下限(循环, 便于快速调整) */
        s_edit_cur += step;
        if (s_edit_cur > hi) {
            s_edit_cur = lo;
        }
        s_last_user_tick = xTaskGetTickCount();
        break;

    case BUTTON_EVENT_DOUBLE_CLICK: {
        /* 双击: 保存并退回子菜单 */
        bms_err_t r = param_write_back(s_edit_parent, s_submenu_idx, s_edit_cur);
        ESP_LOGI(TAG, "[EDIT] 保存 %ld -> NVS: %s",
                 (long)s_edit_cur, (r == BMS_OK) ? "OK" : "FAIL");
        s_mode = UI_MODE_SUBMENU;
        break;
    }

    case BUTTON_EVENT_LONG_PRESS:
        /* 长按: 取消, 恢复原值, 退回子菜单 */
        s_edit_cur = s_edit_orig;
        s_mode = UI_MODE_SUBMENU;
        ESP_LOGI(TAG, "[EDIT] 取消, 恢复原值 %ld", (long)s_edit_orig);
        break;

    default:
        break;
    }
    s_last_user_tick = xTaskGetTickCount();
}

/* ================================================================
 *                         对外接口实现
 * ================================================================ */

bms_err_t app_key_ui_init(void)
{
#if HW_ENABLE_BUTTON
    /* 按键驱动若未初始化, 先尝试初始化 */
    bms_err_t ret = bsp_button_init();
    if (ret != BMS_OK) {
        s_button_ok = false;
        ESP_LOGW(TAG, "按键不可用, 降级为纯自动轮播");
        return BMS_ERR_NOT_INIT;
    }
    s_button_ok = true;
#else
    s_button_ok = false;
    ESP_LOGW(TAG, "HW_ENABLE_BUTTON=0, 降级为纯自动轮播");
    return BMS_ERR_NOT_INIT;
#endif

    s_mode           = UI_MODE_DISPLAY;
    s_page           = 0;
    s_menu_idx       = MENU_ID_CLEAR_FAULT;
    s_submenu_idx    = 0;
    s_last_user_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "按键 UI 初始化完成(BOOT键 GPIO%d, 支持短按/双击/长按)",
             PIN_BUTTON_USER);
    return BMS_OK;
}

uint8_t app_key_ui_get_page(void)
{
    return s_page;
}

void app_key_ui_set_page(uint8_t page)
{
    /* 钳位到有效页号, 供外部(如报警自动跳转)调用 */
    s_page = page % OLED_PAGE_COUNT;
}

ui_mode_e app_key_ui_get_mode(void)
{
    return s_mode;
}

void app_key_ui_rotate_page_if_idle(void)
{
    /* 仅在 DISPLAY 模式自动轮播; 有按键硬件时若用户最近操作过则跳过 */
    if (s_mode != UI_MODE_DISPLAY) {
        return;
    }
#if OLED_AUTO_ROTATE_MS == 0
    return;                                                    /* 宏=0: 禁用自动轮播 */
#else
    if (s_button_ok) {
        uint32_t idle_ms = (xTaskGetTickCount() - s_last_user_tick) * portTICK_PERIOD_MS;
        /* 用户最近 1 个自动轮播周期内操作过, 手动控制优先, 暂不自动切换 */
        if (idle_ms < OLED_AUTO_ROTATE_MS) {
            return;
        }
    }
    s_page = (s_page + 1) % OLED_PAGE_COUNT;
#endif
}

void app_key_ui_process(void)
{
    /* 菜单空闲超时退回显示(避免用户在菜单里卡死) */
#if MENU_IDLE_TIMEOUT_MS > 0
    if (s_mode != UI_MODE_DISPLAY && s_button_ok) {
        uint32_t idle_ms = (xTaskGetTickCount() - s_last_user_tick) * portTICK_PERIOD_MS;
        if (idle_ms >= MENU_IDLE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "[UI] 菜单空闲超时 %ums, 退回显示", MENU_IDLE_TIMEOUT_MS);
            s_mode = UI_MODE_DISPLAY;
        }
    }
#endif

    /* 无按键硬件: 仅处理上面的超时逻辑, 不读事件 */
    if (!s_button_ok) {
        return;
    }

    bms_button_event_e ev = bsp_button_get_event();
    if (ev == BUTTON_EVENT_NONE) {
        return;
    }

    /* 根据模式分发事件 */
    switch (s_mode) {
    case UI_MODE_DISPLAY:  handle_display_mode(ev);  break;
    case UI_MODE_MENU:     handle_menu_mode(ev);     break;
    case UI_MODE_SUBMENU:  handle_submenu_mode(ev);  break;
    case UI_MODE_EDIT:     handle_edit_mode(ev);     break;
    default:                                     break;
    }
}

bool app_key_ui_get_menu_info(const char **title,
                              const char * const **items,
                              uint8_t *count,
                              uint8_t *cur_idx,
                              int32_t *edit_val,
                              const char **edit_unit)
{
    if (title)    *title    = NULL;
    if (items)    *items    = NULL;
    if (count)    *count    = 0;
    if (cur_idx)  *cur_idx  = 0;
    if (edit_val) *edit_val = 0;
    if (edit_unit)*edit_unit = "";

    switch (s_mode) {
    case UI_MODE_DISPLAY:
        return false;                                        /* 显示模式, 不用画菜单 */

    case UI_MODE_MENU:
        if (title)    *title    = "==== 主菜单 ====";
        if (items)    *items    = s_menu_items;
        if (count)    *count    = MENU_ID_MAX;
        if (cur_idx)  *cur_idx  = (uint8_t)s_menu_idx;
        return true;

    case UI_MODE_SUBMENU: {
        const char *t = NULL;
        const char * const *arr = NULL;
        uint8_t n = 0;
        submenu_get_meta(s_menu_idx, &t, &arr, &n);
        if (title)    *title   = t;
        if (items)    *items   = arr;
        if (count)    *count   = n;
        if (cur_idx)  *cur_idx = s_submenu_idx;
        return true;
    }

    case UI_MODE_EDIT: {
        /* 编辑模式: 构造一个特殊菜单, 标题=参数名, items[0]="保存/取消提示", edit_val=当前值 */
        static const char *edit_items[2] = {
            "短按:+步进",
            "双击:保存  长按:取消",
        };
        const char *param_title = NULL;
        const char * const *arr = NULL;
        uint8_t n = 0;
        submenu_get_meta(s_edit_parent, &param_title, &arr, &n);
        if (title && arr && s_submenu_idx < n) {
            static char s_edit_title[32];
            snprintf(s_edit_title, sizeof(s_edit_title), "EDIT: %s", arr[s_submenu_idx]);
            *title = s_edit_title;
        }
        if (items)    *items    = edit_items;
        if (count)    *count    = 2;
        if (cur_idx)  *cur_idx  = 0;
        if (edit_val) *edit_val = s_edit_cur;
        /* 单位 */
        if (edit_unit) {
            switch (s_edit_parent) {
            case MENU_ID_PROT_THRESH:
                if (s_submenu_idx == SUB_PROT_OT || s_submenu_idx == SUB_PROT_UT)
                    *edit_unit = "(0.1C)";
                else
                    *edit_unit = " ";
                break;
            case MENU_ID_BALANCE_CFG: *edit_unit = "mV";  break;
            case MENU_ID_BATT_SPEC:
                *edit_unit = (s_submenu_idx == SUB_BATT_CAP) ? "mAh" : "S";
                break;
            case MENU_ID_SOC_CAL:     *edit_unit = "(x0.01)"; break;
            default:                  *edit_unit = " ";     break;
            }
        }
        return true;
    }
    default:
        return false;
    }
}
