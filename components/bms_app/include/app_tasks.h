/**
 * @file    app_tasks.h
 * @brief   FreeRTOS 任务调度中心
 * @author  BMS Team
 * @date    2026-08
 * @note    创建所有业务任务并启动调度, 由 main 调用 app_tasks_start
 */
#ifndef APP_TASKS_H
#define APP_TASKS_H

#include "bms_errno.h"

/**
 * @brief   系统初始化(BSP + Middleware + 算法 + 通信)
 * @note    在 app_main 中调用, 完成所有硬件与中间件初始化
 * @retval  BMS_OK 成功
 */
bms_err_t app_system_init(void);

/**
 * @brief   创建并启动所有 FreeRTOS 任务
 * @retval  BMS_OK 成功
 */
bms_err_t app_tasks_start(void);

/**
 * @brief   打印各 FreeRTOS 任务栈高水位(剩余空闲字节)
 * @note    启动后延迟调用一次, 用于实证确认 2KB 小栈任务(task_bal/task_alarm)
 *           余量, 防止静默栈溢出(质量评估 09-P1)。也可由调试命令周期触发。
 *           高水位 = 历史最小剩余栈空间, 值越小越危险(接近 0 即溢出)。
 */
void app_dump_task_stack_watermarks(void);

#endif // APP_TASKS_H
