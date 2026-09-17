/**
 * @file    app_key_ui.h
 * @brief   单键 UI 管理器(页面切换 + 菜单 + 参数设置 + 解除报警)
 * @author  BMS Team
 * @date    2026-08
 * @note    归属层: App (UI 业务逻辑)
 *          依赖:  bsp_button(BSP层) / sys_params(Middleware层) / sys_data(Middleware层)
 *          原理: 单键(BOOT键)复用三种事件 + 状态机, 实现完整人机交互
 *
 *          状态机:
 *            DISPLAY  显示模式: 4页轮播, 短按=切页  双击=入菜单  长按=解报警
 *            MENU     菜单模式: 选项列表,     短按=下移  双击=选中   长按=返显示
 *            SUBMENU  子菜单:   参数列表,     短按=下移  双击=编辑   长按=返主菜单
 *            EDIT     参数编辑: 改值,         短按=加值  双击=保存   长按=取消
 *
 *          无按键硬件(HW_ENABLE_BUTTON=0)时自动降级: 纯自动轮播, 菜单不可用
 */
#ifndef APP_KEY_UI_H
#define APP_KEY_UI_H

#include <stdint.h>
#include <stdbool.h>
#include "bms_errno.h"
#include "bms_types.h"

/* ====== UI 工作模式枚举 ====== */
typedef enum {
    UI_MODE_DISPLAY  = 0,                           // 显示模式(默认): 4页轮播
    UI_MODE_MENU,                                    // 主菜单模式
    UI_MODE_SUBMENU,                                 // 子菜单(参数分组列表)
    UI_MODE_EDIT,                                    // 参数编辑模式
} ui_mode_e;

/* ====== 主菜单 ID 枚举(替代魔法数) ====== */
typedef enum {
    MENU_ID_CLEAR_FAULT = 0,                        // 01 解除报警
    MENU_ID_PROT_THRESH,                             // 02 保护阈值
    MENU_ID_BALANCE_CFG,                             // 03 均衡设置
    MENU_ID_BATT_SPEC,                               // 04 电池规格
    MENU_ID_SOC_CAL,                                 // 05 SOC 校准
    MENU_ID_SYS_OPS,                                 // 06 系统操作
    MENU_ID_MAX,                                     // 项数(哨兵, 不显示)
} menu_id_e;

/* ====== 子菜单: 保护阈值分组项 ID ====== */
typedef enum {
    SUB_PROT_OV = 0,                                 // 单体过压(mV)
    SUB_PROT_UV,                                     // 单体欠压(mV)
    SUB_PROT_OT,                                     // 过温保护(0.1℃)
    SUB_PROT_UT,                                     // 低温保护(0.1℃)
    SUB_PROT_CHG_OC,                                 // 充电过流(mA)
    SUB_PROT_DSG_OC,                                 // 放电过流(mA)
    SUB_PROT_MAX,
} submenu_prot_e;

/* ====== 子菜单: 均衡设置项 ID ====== */
typedef enum {
    SUB_BAL_THR = 0,                                 // 均衡触发阈值(mV)
    SUB_BAL_STOP,                                    // 均衡停止阈值(mV)
    SUB_BAL_MAX,
} submenu_balance_e;

/* ====== 子菜单: 电池规格项 ID ====== */
typedef enum {
    SUB_BATT_CAP = 0,                                // 单体容量(mAh)
    SUB_BATT_SERIES,                                 // 串联数
    SUB_BATT_MAX,
} submenu_battery_e;

/* ====== 子菜单: SOC 校准项 ID ====== */
typedef enum {
    SUB_SOC_OFFSET = 0,                              // SOC 偏移(-0.1~+0.1)
    SUB_SOC_GAIN,                                    // SOC 增益(0.9~1.1)
    SUB_SOC_MAX,
} submenu_soc_e;

/* ====== 子菜单: 系统操作项 ID ====== */
typedef enum {
    SUB_SYS_RESET = 0,                               // 复位参数为默认值
    SUB_SYS_REBOOT,                                  // 重启系统
    SUB_SYS_WIFI_RESET,                              // 清除 WiFi 配置, 重启进 AP 配网(重新配网)
    SUB_SYS_MAX,
} submenu_sys_e;

/* ================================================================
 *                      对外接口
 * ================================================================ */

/**
 * @brief   初始化按键 UI 管理器
 * @retval  BMS_OK 成功
 *          BMS_ERR_NOT_INIT 硬件屏蔽, 降级为纯自动轮播
 * @note    必须在 bsp_button_init() 之后调用, 内部检测按键是否可用
 */
bms_err_t app_key_ui_init(void);

/**
 * @brief   获取当前应显示的 OLED 页面(显示模式下)
 * @return  页号 0~OLED_PAGE_COUNT-1
 * @note    在 OLED 渲染循环中调用, 决定画哪页
 */
uint8_t app_key_ui_get_page(void);

/**
 * @brief   设置当前显示页(供报警自动跳转使用)
 * @param   page  目标页号(自动取模 OLED_PAGE_COUNT)
 * @note    仅在 DISPLAY 模式生效, 不会切换 UI 模式
 */
void app_key_ui_set_page(uint8_t page);

/**
 * @brief   获取当前 UI 工作模式(显示/菜单/编辑)
 * @retval  ui_mode_e
 * @note    OLED 任务根据模式决定是画页面还是画菜单
 */
ui_mode_e app_key_ui_get_mode(void);

/**
 * @brief   OLED 任务请求自动轮播前进一页(缺按键时的定时触发)
 * @note    仅当 UI_MODE_DISPLAY 且无用户输入时生效, 按键手动切页优先
 */
void app_key_ui_rotate_page_if_idle(void);

/**
 * @brief   周期性处理: 读取按键事件 + 驱动状态机 + 联动业务
 * @note    必须在 OLED 任务或独立低优先级任务中周期调用(建议 20~100ms)
 *          内部非阻塞, 无事件时立即返回
 */
void app_key_ui_process(void);

/**
 * @brief   获取菜单渲染信息(菜单模式下 OLED 画菜单使用)
 * @param   [out] title   菜单标题字符串(指向内部 static, 不 free)
 * @param   [out] items   菜单项字符串数组(指向内部 static, 不 free)
 * @param   [out] count   菜单项总数
 * @param   [out] cur_idx 当前选中项索引
 * @param   [out] edit_val 编辑模式下当前值(EDIT 模式有效, 其余为 NULL)
 * @param   [out] edit_unit 值单位字符串
 * @retval  true  处于菜单/子菜单/编辑模式, 需要画菜单
 *          false 处于显示模式, 不需要画菜单
 */
bool app_key_ui_get_menu_info(const char **title,
                              const char * const **items,
                              uint8_t *count,
                              uint8_t *cur_idx,
                              int32_t *edit_val,
                              const char **edit_unit);

#endif /* APP_KEY_UI_H */
