/**
 * @file    sys_params.h
 * @brief   系统参数持久化(NVS Flash 存储)
 * @author  BMS Team
 * @date    2026-08
 * @note    设计要点:
 *          1. 关键参数掉电不丢失(SSID/密码/容量/校准值/循环次数)
 *          2. 首次上电检测魔术字, 写入默认值
 *          3. 提供 load/save/get/set 受控接口, 内部 static 化
 *          4. 与 bms_config.h 宏默认值协同: NVS 优先, 缺失回退到宏
 *          缺 NVS 时(HW_ENABLE_NVS=0) 静默返回默认值, 不阻塞系统
 */
#ifndef SYS_PARAMS_H
#define SYS_PARAMS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "bms_errno.h"

/* ====== 可持久化参数结构体(每个字段必须注明单位) ======
 * 注意: 修改此结构体会破坏已存数据的二进制兼容性
 *       增加字段只能追加到末尾, 删除字段需保留占位
 *       修改字段含义需 bump NVS_PARAMS_VERSION */
typedef struct {
    uint32_t magic;                                // 魔术字 0xBMS58042(版本+识别)
    uint32_t version;                              // 参数版本号

    /* WiFi 配置 */
    char     wifi_ssid[32];                        // SSID, 0 结尾字符串
    char     wifi_pass[64];                        // 密码, 0 结尾字符串
    uint8_t  wifi_configured;                      // 1=已配网(直接 STA 模式), 0=首次开机需 AP 配网

    /* 电池规格 */
    uint16_t cell_capacity_mah;                    // 单体容量, 单位 mAh
    uint8_t  cell_series_num;                      // 串联数(3~16, 运行时可经网页/MQTT/Modbus/OLED菜单下发切换)

    /* SOC 校准 */
    float    soc_offset;                           // SOC 校准偏移, -0.1~+0.1
    float    soc_gain;                             // SOC 校准增益, 0.9~1.1

    /* 保护阈值(可调, 默认 = bms_config.h 宏) */
    uint16_t cell_ov_prot_mv;                      // 单体过压保护, 单位 mV
    uint16_t cell_uv_prot_mv;                      // 单体欠压保护, 单位 mV
    int16_t  temp_ot_prot_dc;                      // 过温保护, 单位 0.1℃
    int16_t  temp_ut_prot_dc;                      // 低温保护, 单位 0.1℃
    int16_t  chg_oc_prot_ma;                       // 充电过流保护, 单位 mA
    int16_t  dsg_oc_prot_ma;                       // 放电过流保护, 单位 mA
    uint16_t cell_ov_warn_mv;                      // 单体过压预警, 单位 mV
    uint16_t cell_uv_warn_mv;                      // 单体欠压预警, 单位 mV
    int16_t  chg_oc_warn_ma;                       // 充电过流预警, 单位 mA
    int16_t  dsg_oc_warn_ma;                       // 放电过流预警, 单位 mA
    int16_t  temp_ot_warn_dc;                      // 过温预警, 单位 0.1℃
    int16_t  temp_ut_warn_dc;                      // 低温预警, 单位 0.1℃
    uint16_t cell_dv_warn_mv;                      // 单体压差预警, 单位 mV
    uint8_t  soc_low_warn_pct;                     // 低 SOC 预警, 单位 %

    /* 均衡参数 */
    uint16_t balance_threshold_mv;                 // 均衡触发阈值, 单位 mV
    uint16_t balance_stop_mv;                      // 均衡停止阈值, 单位 mV

    /* 电池老化数据 */
    uint32_t cycle_count;                          // 循环次数
    float    soh;                                  // 健康度 0.0~1.0

    /* MQTT 配置 */
    char     mqtt_uri[128];                        // MQTT Broker URI (IoTDA地址较长,需128字节)
    char     mqtt_client_id[64];                   // MQTT 客户端 ID(华为云IoTDA为设备ID)
    char     mqtt_user[64];                        // MQTT 用户名(华为云IoTDA为设备ID)
    char     mqtt_pass[64];                        // MQTT 密码(华为云IoTDA为HMAC动态生成)
    char     mqtt_device_secret[64];               // 华为云IoTDA设备密钥(用于HMAC-SHA256鉴权)
    char     mqtt_topic_prefix[16];                // MQTT 主题前缀(默认 "$oc/devices/", 多台设备用 bms_001 等)

    /* OTA 配置 */
    char     ota_version_url[128];                 // OTA 版本检查 URL
    char     ota_firmware_url[128];                // OTA 固件下载 URL
    char     firmware_version[16];                 // 当前固件版本号

    /* ====== v7 新增: 网页可调参数(与 tools/dashboard app.py PARAM_META 对齐) ======
     * 追加到结构体末尾保持 NVS 二进制兼容; 增加字段须 bump PARAMS_VERSION */
    uint16_t cell_ov_recover_mv;                   // 单体过压恢复, mV
    uint16_t cell_uv_recover_mv;                   // 单体欠压恢复, mV
    uint16_t cell_dv_recover_mv;                   // 单体压差恢复, mV
    int16_t  rated_current_ma;                     // 额定电流(1C), mA
    float    overload_warn_ratio;                  // 过载预警倍率(×额定)
    float    overload_prot_ratio;                  // 过载保护倍率(×额定)
    int16_t  temp_ot_recover_dc;                   // 过温恢复, 0.1℃
    int16_t  temp_ut_recover_dc;                   // 低温恢复, 0.1℃
    float    temp_dtdt_warn;                       // 温升速率预警, ℃/min
    float    dv_dt_warn_mvps;                      // 电压降速率预警, mV/s
    uint8_t  soc_low_recover_pct;                  // 低SOC恢复阈值, %
    uint8_t  soh_low_warn_pct;                     // SOH低预警阈值, %
    uint16_t balance_timeout_min;                  // 均衡超时, min
    uint16_t charge_cc_current_ma;                 // 恒流充电电流, mA
    uint8_t  charge_cc_soc_thr;                    // 恒流转恒压SOC阈值, %
    uint8_t  charge_cv_soc_thr;                    // 恒压截止SOC阈值, %
    uint32_t nominal_voltage_mv;                   // 标称总压, mV(3.6V×串数)
    uint32_t full_voltage_mv;                      // 满充总压, mV(4.2V×串数)
    uint32_t cutoff_voltage_mv;                    // 放电截止总压, mV(2.8V×串数)

    /* ====== v8 新增: 电池类型 / SOC 算法选择(网页电池参数配置下发) ======
     * 追加到结构体末尾保持 NVS 二进制兼容; 增加字段须 bump PARAMS_VERSION */
    uint8_t  battery_type;                         // 0=LFP 1=NCM 2=LTO 3=铅酸 (默认 NCM)
    uint8_t  soc_algo;                             // 0=AEKF(默认) 1=纯安时积分 2=OCV查表 3=MCC-EKF 4=UKF

    /* ====== v9 新增: 均衡策略/起始SOC(网页均衡控制下发) ====== */
    uint8_t  balance_start_soc_pct;                // 均衡起始SOC门槛(0~100, 0=不限制)
    uint8_t  balance_strategy;                     // 0=电压差触发(默认) 1=容量差触发 2=定时均衡

    /* ====== v10 新增: 属性上报间隔(网页实时性调节, 受15000/天消息上限约束) ======
     * 追加到结构体末尾保持 NVS 二进制兼容; 增加字段须 bump PARAMS_VERSION */
    uint16_t report_interval_sec;                  // 属性上报间隔, 秒(7~3600, 默认10)

    /* ====== v11 新增: 自建 Mosquitto 双通道连接参数(2026-08-16 双发架构) ======
     * 追加到结构体末尾保持 NVS 二进制兼容; 增加字段须 bump PARAMS_VERSION */
    char     mqtt2_uri[128];                       // 自建 Mosquitto URI (mqtt://host:port)
    char     mqtt2_client_id[64];                  // 自建 broker 客户端ID(如 bms01)
    char     mqtt2_user[64];                       // 自建 broker 用户名
    char     mqtt2_pass[64];                       // 自建 broker 密码
    char     mqtt2_topic_prefix[32];               // 自建 topic 前缀(如 "bms/")
} bms_params_t;

/**
 * @brief   初始化参数模块(挂载 NVS + 读取参数)
 * @retval  BMS_OK 成功, BMS_ERR_STORAGE NVS 失败(用默认值继续)
 * @note    首次上电 magic 不匹配则写入默认值
 */
bms_err_t sys_params_init(void);

/**
 * @brief   获取参数结构体指针(只读, 不修改)
 * @return  参数结构体指针, 永不为 NULL
 * @note    返回内部 static 变量地址, 不要 free
 */
const bms_params_t *sys_params_get(void);

/**
 * @brief   修改并立即保存参数到 NVS
 * @param   params  新参数(const, 内部拷贝)
 * @retval  BMS_OK 成功, BMS_ERR_STORAGE 写入失败
 * @note    频繁写 NVS 会损耗 Flash, 建议仅在用户修改时调用
 */
bms_err_t sys_params_set(const bms_params_t *params);

/**
 * @brief   重置参数为默认值并保存
 * @retval  BMS_OK 成功
 */
bms_err_t sys_params_reset(void);

/**
 * @brief   NVS 脏数据兜底刷盘(#4 去抖)
 *          若处于脏状态且距上次写已满合并窗口, 落盘一次. 由周期任务调用,
 *          保证窗口内多次 set_param 最终只写一次 Flash, 且不会永远丢失.
 */
void sys_params_flush(void);

/**
 * @brief   立即强制落盘当前活动参数(无视去抖窗口)
 * @note    供"保存配置后即将重启"的路径调用(配网门户 / set_wifi):
 *          去抖窗口内 params_commit 只置脏返回 OK, 若重启早于窗口结束
 *          (窗口 2s vs 门户重启延迟 1s) 脏数据会丢失, 设备继续用旧配置.
 *          本函数无条件写 Flash, 保证重启前配置真正持久化.
 * @retval  BMS_OK 成功
 */
bms_err_t sys_params_flush_now(void);

/**
 * @brief   读取上次启动的固件编译指纹(独立 NVS key, 不进 bms_params_t blob)
 * @param   buf     输出缓冲(建议 >=64)
 * @param   buf_len 缓冲大小
 * @retval  BMS_OK 有记录; BMS_ERR_STORAGE 无记录/读取失败(视为首次烧录)
 * @note    供"重新烧录检测"使用: 指纹 = 版本 + 编译日期 + 编译时间.
 */
bms_err_t sys_params_get_reflash_fp(char *buf, size_t buf_len);

/**
 * @brief   记录当前固件编译指纹到 NVS
 * @param   fp  指纹字符串
 * @retval  BMS_OK 成功
 */
bms_err_t sys_params_set_reflash_fp(const char *fp);

/**
 * @brief   故障黑匣子: 记录本次复位原因到 NVS(2026-09-14 高可靠加固)
 * @param   reset_reason  esp_reset_reason() 返回值
 * @note    独立 NVS key(不进 bms_params_t blob), 启动初期调用一次,
 *          MQTT get_info 上报 → 云端可追溯上次复位原因(看门狗/崩溃/上电等)
 */
bms_err_t sys_params_set_blackbox(uint8_t reset_reason);

/**
 * @brief   读取上次记录的复位原因
 * @param   reset_reason  输出: 上次 esp_reset_reason() 值(无记录时为 0)
 */
bms_err_t sys_params_get_blackbox(uint8_t *reset_reason);

/**
 * @brief   命令鉴权开关(2026-09-14 高可靠加固 A1)
 * @param   enabled  true=高危命令需 HMAC token; false=关闭(与历史行为一致)
 * @note    独立 NVS key 存储, OTA 后保留, 默认关闭
 */
bms_err_t sys_params_set_cmd_auth(bool enabled);

/**
 * @brief   读取命令鉴权开关当前状态(默认 false)
 */
bool sys_params_get_cmd_auth(void);

/**
 * @brief   黑匣子扩展(B1): 记录死前运行点现场到 NVS(2026-09-14 高可靠加固)
 * @param   fault    死前故障掩码(sys_data_get_fault)
 * @param   soc_pct  SOC 百分比 0~100
 * @param   pack_mv  包电压 mV
 * @param   ma       包电流 mA(带符号, 充+放-)
 * @note    仅异常复位(TASK_WDT/PANIC 类)时由启动代码调用一次, 正常上电不写 —
 *          与复位原因组合可定位"看门狗饿死时保护是否已触发"等场景
 */
bms_err_t sys_params_set_blackbox_ctx(uint32_t fault, uint8_t soc_pct,
                                      uint16_t pack_mv, int16_t ma);

/**
 * @brief   读取死前运行点现场(无记录时各输出为 0)
 */
bms_err_t sys_params_get_blackbox_ctx(uint32_t *fault, uint8_t *soc_pct,
                                      uint16_t *pack_mv, int16_t *ma);




/**
 * @brief   标记"本次启动来自 OTA 升级"(OTA 成功重启前调用)
 * @note    独立 NVS key, 用于区分 OTA 升级与串口烧录:
 *          串口烧录(idf.py flash)会把 otadata 清空, bootloader 首次启动
 *          同样把运行分区置为 PENDING_VERIFY —— 分区状态无法区分二者,
 *          必须由 OTA 成功路径显式留标记.
 */
bms_err_t sys_params_set_ota_upgraded(void);

/**
 * @brief   查询"本次启动来自 OTA 升级"标记
 * @retval  true=OTA 升级后首启(应保留 WiFi 配置); false=非 OTA(串口烧录/正常重启)
 */
bool sys_params_is_ota_upgraded(void);

/**
 * @brief   清除 OTA 升级标记(启动消费后调用, 防止下次正常重启误判)
 */
void sys_params_clear_ota_upgraded(void);

/**
 * @brief   设置 OTA 升级模式: 自动确认 or 人工确认
 * @param   auto_confirm  true=发现新版本自动升级(无人值守);
 *                        false=只检查上报, 等用户确认后才升级(推荐默认)
 * @retval  BMS_OK 成功
 */
bms_err_t sys_params_set_ota_auto_confirm(bool auto_confirm);

/**
 * @brief   查询 OTA 升级模式
 * @retval  true=自动升级; false=人工确认(默认)
 */
bool sys_params_is_ota_auto_confirm(void);

#endif // SYS_PARAMS_H
