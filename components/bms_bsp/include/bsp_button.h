/**
 * @file    bsp_button.h
 * @brief   按钮输入驱动(GPIO0 BOOT 键复用)
 * @author  BMS Team
 * @date    2026-08
 * @note    短按翻页 / 长按 3s 复位故障 / 双击进入/退出强制页
 *          中断 + 软件消抖, 不阻塞任务
 *          缺按钮时 HW_ENABLE_BUTTON=0, 驱动返回 BMS_ERR_NOT_INIT
 */
#ifndef BSP_BUTTON_H
#define BSP_BUTTON_H

#include <stdbool.h>
#include "bms_errno.h"

/* ====== 按钮事件枚举 ====== */
typedef enum {
    BUTTON_EVENT_NONE        = 0,                  // 无事件
    BUTTON_EVENT_SHORT_PRESS,                      // 短按(< 长按阈值)
    BUTTON_EVENT_LONG_PRESS,                       // 长按(>= 3s)
    BUTTON_EVENT_LONG_LONG_PRESS,                  // 超长按(>= 5s): 强制进入 AP 配网模式
    BUTTON_EVENT_DOUBLE_CLICK,                     // 双击(400ms 内两次短按)
} bms_button_event_e;

/**
 * @brief   初始化按钮 GPIO(中断 + 消抖)
 * @retval  BMS_OK 成功, BMS_ERR_NOT_INIT 硬件屏蔽
 */
bms_err_t bsp_button_init(void);

/**
 * @brief   获取并清除按钮事件(非阻塞)
 * @return  当前待处理的事件, BUTTON_EVENT_NONE 表示无事件
 * @note    在 OLED 任务或独立按钮任务中周期调用
 */
bms_button_event_e bsp_button_get_event(void);

/**
 * @brief   查询按钮当前是否按下(实时电平)
 * @return  true 按下, false 释放
 */
bool bsp_button_is_pressed(void);

/**
 * @brief   独立配网按键轮询检测(GPIO0 BOOT 键, 不依赖 HW_ENABLE_BUTTON)
 * @return  true = 检测到长按 >=5s, 调用方应清除 WiFi 配置并重启进入 AP 配网
 * @note    产品现场无 OLED 按键 UI(HW_ENABLE_BUTTON=0)时仍可靠:
 *          任何 ESP32 板物理 BOOT 键都在 GPIO0, 长按 5s 即强制进配网.
 *          内部自管理 GPIO 配置与状态机, 需在任务中周期(建议 100ms)调用,
 *          按住期间仅返回一次 true, 释放后复位.
 */
bool bsp_button_provision_check(void);

#endif // BSP_BUTTON_H
