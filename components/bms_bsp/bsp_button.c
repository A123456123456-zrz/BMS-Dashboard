/**
 * @file    bsp_button.c
 * @brief   按钮输入驱动实现(GPIO0 BOOT 键复用)
 * @author  BMS Team
 * @date    2026-08
 * @note    实现机制:
 *          1. GPIO 下降沿中断置位 s_pressed_flag
 *          2. bsp_button_get_event() 在任务中周期调用, 做软件消抖 + 长短按判定
 *          3. 双击检测: 短按事件后启动 400ms 窗口, 窗口内再次按下即双击
 *          按钮电路: GPIO0 通过 BOOT 键接地, 上拉使能, 按下为低电平
 *          缺硬件时 HW_ENABLE_BUTTON=0, 初始化直接返回, get_event 永远返回 NONE
 */
#include "bsp_button.h"
#include "bms_pinmap.h"
#include "bms_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP_BUTTON";

/* ====== 内部状态(static 化, 不暴露) ====== */
static volatile bool s_inited = false;
static volatile bool s_irq_flag = false;              // 中断置位, 任务读取后清零
static volatile uint32_t s_press_down_tick = 0;       // 按下时刻 tick
static volatile bool     s_is_pressed      = false;   // 当前是否按下(消抖后)

/* 双击检测状态 */
static volatile bool     s_wait_double = false;        // 是否在等待第二次按下
static volatile uint32_t s_last_short_release_tick = 0;

/* 长按已触发标志: 按住期间只触发一次, 释放后复位
 * (修复: 原实现按住不放会每轮重复触发 LONG_PRESS) */
static volatile bool     s_long_press_sent = false;

/* 超长按(5s, 进配网)已触发标志: 与 3s 长按标志独立,
 * 3s 先触发解除报警, 继续按住到 5s 仍可触发配网事件 */
static volatile bool     s_very_long_sent  = false;

/* 待取走的事件(任务消费) */
static volatile bms_button_event_e s_pending_event = BUTTON_EVENT_NONE;

/* ====== GPIO 中断回调 ======
 * 中断原则: 只置标志位, 不做任何业务逻辑
 * 中断里禁止调用 printf/LOG/延时等阻塞 API */
static void IRAM_ATTR button_isr_handler(void *arg)
{
    (void)arg;
    s_irq_flag = true;
}

bms_err_t bsp_button_init(void)
{
    /* 硬件屏蔽 */
    if (!HW_ENABLE_BUTTON) {
        ESP_LOGW(TAG, "BUTTON 屏蔽 (HW_ENABLE_BUTTON=0)");
        return BMS_ERR_NOT_INIT;
    }

    /* GPIO 配置: 输入 + 上拉 + 下降沿中断(按下为低) */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_BUTTON_USER),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config fail: %s", esp_err_to_name(ret));
        return BMS_ERR_FAIL;
    }

    /* 安装 GPIO 中断服务(若已安装则返回 ESP_ERR_INVALID_STATE, 容忍) */
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service fail: %s", esp_err_to_name(isr_ret));
        return BMS_ERR_FAIL;
    }
    esp_err_t add_ret = gpio_isr_handler_add(PIN_BUTTON_USER, button_isr_handler, NULL);
    if (add_ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add fail: %s", esp_err_to_name(add_ret));
        return BMS_ERR_FAIL;
    }

    s_inited = true;
    ESP_LOGI(TAG, "button init ok (pin=%d, BOOT 键复用)", PIN_BUTTON_USER);
    return BMS_OK;
}

bool bsp_button_is_pressed(void)
{
    if (!s_inited) {
        return false;
    }
    /* 实时读电平: 低 = 按下 */
    return (gpio_get_level(PIN_BUTTON_USER) == 0);
}

/* ====== 独立配网按键检测(GPIO0 BOOT 键, 不依赖 HW_ENABLE_BUTTON) ======
 * 场景: 产品化后 HW_ENABLE_BUTTON=0(无 OLED 按键 UI), 现场靠物理 BOOT 键
 *       长按 5s 强制重新配网——任何 ESP32 板 BOOT 键都接在 GPIO0, 必然可读.
 * 与完整按键驱动的关系: 完整驱动已启用(s_inited=true)时本通道让位,
 *       由 bsp_button_get_event 的 LONG_LONG_PRESS 事件负责, 避免双触发. */
bool bsp_button_provision_check(void)
{
    /* 完整按键驱动可用时让位(事件通道已处理 5s 长按) */
    if (s_inited) {
        return false;
    }

    static bool      s_prov_gpio_ready = false;   /* GPIO0 已配置为输入 */
    static bool      s_prov_pressed    = false;   /* 当前按下(状态机) */
    static uint32_t  s_prov_down_tick  = 0;       /* 按下起始 tick */
    static bool      s_prov_triggered  = false;   /* 5s 已触发(按住期间仅一次) */

    /* 1. 首次调用: 配置 GPIO0 为输入+上拉(仅轮询, 不装中断) */
    if (!s_prov_gpio_ready) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << PIN_BUTTON_USER),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        s_prov_gpio_ready = true;
        ESP_LOGI(TAG, "配网按键通道已就绪 (GPIO%d, 长按 %dms 进配网)",
                 PIN_BUTTON_USER, BMS_CONFIG_BUTTON_LONG_MS);
    }

    /* 2. 轮询状态机: 按下计时, >=5s 触发一次, 释放复位 */
    uint32_t now = xTaskGetTickCount();
    bool pressed = (gpio_get_level(PIN_BUTTON_USER) == 0);
    if (pressed) {
        if (!s_prov_pressed) {
            s_prov_pressed   = true;
            s_prov_down_tick = now;
            s_prov_triggered = false;
        }
        uint32_t held_ms = (now - s_prov_down_tick) * portTICK_PERIOD_MS;
        if (held_ms >= BMS_CONFIG_BUTTON_LONG_MS && !s_prov_triggered) {
            s_prov_triggered = true;
            ESP_LOGW(TAG, "[配网] GPIO%d BOOT 键长按 %lums, 进入 AP 配网模式",
                     PIN_BUTTON_USER, (unsigned long)held_ms);
            return true;
        }
    } else if (s_prov_pressed) {
        s_prov_pressed   = false;
        s_prov_triggered = false;
    }
    return false;
}

bms_button_event_e bsp_button_get_event(void)
{
    if (!s_inited) {
        return BUTTON_EVENT_NONE;
    }

    /* ====== 中断标志处理: 按下时刻记录 ====== */
    if (s_irq_flag) {
        s_irq_flag = false;
        /* 中断可能由抖动触发, 此处仅记录起点, 后续消抖 */
        if (!s_is_pressed) {
            s_press_down_tick = xTaskGetTickCountFromISR();
            s_is_pressed      = true;
        }
    }

    /* ====== 状态机: 消抖 + 长短按判定 ====== */
    uint32_t now = xTaskGetTickCount();

    if (s_is_pressed) {
        /* 实时电平检测: 若已释放(高电平), 进入释放处理 */
        if (gpio_get_level(PIN_BUTTON_USER) == 1) {
            /* 释放, 计算按下时长 */
            uint32_t held_ms = (now - s_press_down_tick) * portTICK_PERIOD_MS;

            /* 消抖: 按下时长 < 20ms 视为抖动, 丢弃 */
            if (held_ms < BUTTON_DEBOUNCE_MS) {
                s_is_pressed = false;
                return BUTTON_EVENT_NONE;
            }

            s_is_pressed = false;

            if (held_ms >= BMS_CONFIG_BUTTON_LONG_MS) {
                /* ====== 超长按事件(>= 5s, 优先级最高): 强制进入 AP 配网模式 ======
                 * 按住 3s 已触发解除报警(见按住分支), 继续按住到 5s 才触发配网,
                 * 释放时若未实时触发过则在此补发 */
                s_wait_double = false;
                if (!s_very_long_sent) {
                    s_pending_event = BUTTON_EVENT_LONG_LONG_PRESS;
                    ESP_LOGW(TAG, "超长按 %lums -> 进入 AP 配网模式", (unsigned long)held_ms);
                }
                s_very_long_sent = false;            // 释放, 允许下次再次超长按
                s_long_press_sent = false;
            } else if (held_ms >= BUTTON_LONG_PRESS_MS) {
                /* ====== 短按事件 ====== */
                if (s_wait_double) {
                    /* 双击窗口内再次短按 → 双击 */
                    s_wait_double = false;
                    s_pending_event = BUTTON_EVENT_DOUBLE_CLICK;
                    ESP_LOGI(TAG, "双击");
                } else {
                    /* 首次短按, 启动双击窗口 */
                    s_wait_double = true;
                    s_last_short_release_tick = now;
                }
            }
        } else {
            /* 仍然按下, 检查是否已达长按阈值(实时触发, 不等释放) */
            uint32_t held_ms = (now - s_press_down_tick) * portTICK_PERIOD_MS;
            /* 超长按(>=5s): 优先于 3s 长按, 实时触发进入配网(按住期间仅一次) */
            if (held_ms >= BMS_CONFIG_BUTTON_LONG_MS && !s_very_long_sent && s_pending_event == BUTTON_EVENT_NONE) {
                s_wait_double = false;
                s_very_long_sent = true;
                s_pending_event = BUTTON_EVENT_LONG_LONG_PRESS;
                ESP_LOGW(TAG, "超长按触发(实时) -> 进入 AP 配网模式");
            } else if (held_ms >= BUTTON_LONG_PRESS_MS && !s_long_press_sent && s_pending_event == BUTTON_EVENT_NONE) {
                /* 长按立即触发(不必等释放), 便于 UI 立即响应
                 * 修复: 按住期间仅触发一次, 避免每轮重复 */
                s_wait_double = false;
                s_long_press_sent = true;
                s_pending_event = BUTTON_EVENT_LONG_PRESS;
                ESP_LOGI(TAG, "长按触发(实时)");
            }
        }
    }

    /* ====== 双击窗口超时处理 ====== */
    if (s_wait_double) {
        uint32_t since_short = (now - s_last_short_release_tick) * portTICK_PERIOD_MS;
        if (since_short >= BUTTON_DOUBLE_CLICK_MS) {
            /* 窗口超时, 第一次短按升级为短按事件 */
            s_wait_double = false;
            s_pending_event = BUTTON_EVENT_SHORT_PRESS;
            ESP_LOGI(TAG, "短按");
        }
    }

    /* ====== 返回并清除待处理事件 ====== */
    bms_button_event_e ev = s_pending_event;
    s_pending_event = BUTTON_EVENT_NONE;
    return ev;
}
