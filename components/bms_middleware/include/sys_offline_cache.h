/**
 * @file    sys_offline_cache.h
 * @brief   断网数据缓存与补传(SPIFFS 文件系统)
 * @author  BMS Team
 * @date    2026-08
 * @note    MQTT 断开时将上报数据缓存到 SPIFFS 文件,
 *          MQTT 恢复后自动补传并清空缓存
 *          缓存格式: 每行一条 JSON 记录
 *          容量: 1MB SPIFFS 可存约 5000 条(约 35 小时)
 */
#ifndef SYS_OFFLINE_CACHE_H
#define SYS_OFFLINE_CACHE_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief 初始化离线缓存(挂载 SPIFFS)
 * @return 0=成功, 非0=失败
 */
int sys_offline_cache_init(void);

/**
 * @brief 保存一条数据到缓存(MQTT 断开时调用)
 * @param json_str  JSON 字符串(属性上报格式)
 * @return 0=成功, 非0=失败
 */
int sys_offline_cache_save(const char *json_str);

/**
 * @brief 补传缓存数据并清空(MQTT 恢复后调用)
 * @param send_func  发送函数指针, 返回 0=成功
 * @return 0=全部补传成功, 非0=部分失败
 */
int sys_offline_cache_replay(int (*send_func)(const char *topic, const char *json));

/**
 * @brief 获取当前缓存条数
 */
int sys_offline_cache_count(void);

#endif // SYS_OFFLINE_CACHE_H
