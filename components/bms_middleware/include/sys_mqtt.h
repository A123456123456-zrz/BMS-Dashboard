/**
 * @file    sys_mqtt.h
 * @brief   MQTT 通信服务 (双向: 数据上报 + 远程命令)
 * @author  BMS Team
 * @date    2026-08
 * @note    适配华为云 IoTDA 原生 MQTT Topic 与鉴权 (2026-08-10 修正注释与实现对齐):
 *          上行(ESP32 → 云端):
 *            $oc/devices/{id}/sys/properties/report   属性上报(电池数据/设备信息)
 *            $oc/devices/{id}/sys/events/up           事件/故障上报
 *          下行(云端 → ESP32):
 *            $oc/devices/{id}/sys/commands/#          产品模型命令下发
 *            $oc/devices/{id}/sys/messages/down       设备消息下发(Dashboard /messages API, 不校验物模型)
 *            $oc/devices/{id}/sys/properties/set/#    RW 属性设置(chargeEnable/阈值等)
 *          响应(ESP32 → 云端):
 *            $oc/devices/{id}/sys/commands/response/{request_id}
 *          鉴权: ClientID={device_id}_0_0_{timestamp}, Username={device_id},
 *                Password=HMAC-SHA256(设备密钥, 时间戳)
 */
#ifndef SYS_MQTT_H
#define SYS_MQTT_H

#include "bms_types.h"
#include "bms_errno.h"
#include <stdbool.h>

/* forward declaration: cJSON 结构体 */
struct cJSON;

/**
 * @brief   命令回调函数类型(处理 clear_alarm/restart/ota_check 等自定义命令)
 * @param   cmd   命令字符串, 如 "clear_alarm"
 * @param   root  cJSON root 对象, 含完整 JSON 字段
 * @note    回调在 MQTT 事件线程中执行, 严禁阻塞或长耗时操作
 *          需要长操作的命令请通过队列转交业务任务
 */
typedef void (*sys_mqtt_cmd_callback_t)(const char *cmd, const struct cJSON *root);

/**
 * @brief   初始化并连接 MQTT Broker
 * @note    自动订阅 commands/# 、messages/down 、properties/set/# 三个下行主题
 * @retval  BMS_OK 成功
 */
bms_err_t sys_mqtt_init(void);

/**
 * @brief   检查并处理 MQTT 认证失败重连
 * @note    认证失败(timestamp过期/HMAC错误)时, 销毁客户端并重新生成凭证重连.
 *          由主循环定期调用.
 * @retval  BMS_OK 无需重连或重连成功
 */
bms_err_t sys_mqtt_check_reauth(void);

/**
 * @brief   注册自定义命令回调
 * @param   cb  回调函数指针, NULL 取消注册
 * @note    set_param/get_params/get_info/set_wifi 及 set_charge/set_discharge/
 *          set_balance/set_relay 已内置处理, 不需回调;
 *          clear_alarm/restart/ota_check/ota_upgrade/reset_params 由回调处理
 */
void sys_mqtt_register_cmd_callback(sys_mqtt_cmd_callback_t cb);

/**
 * @brief   上报一帧电池数据(JSON)
 * @param   pack   采集数据
 * @param   soc    SOC 数据
 * @param   fault  故障掩码
 */
void sys_mqtt_report(const bms_pack_data_t *pack,
                     const bms_soc_data_t  *soc,
                     bms_fault_mask_t       fault);

/**
 * @brief   上报一帧 EMQX-only 全量数据(2s, 不耗华为云配额)
 * @note    2026-09-10 提速: 与 sys_mqtt_report 组包相同, 但只发 EMQX 通道,
 *          跳过华为云发布(华为云 15000 条/天配额仍按 7s 节奏由 task_comm 控制);
 *          EMQX 未连接时静默返回(不误入华为云离线缓存)。
 * @param   pack   采集数据
 * @param   soc    SOC 数据
 * @param   fault  故障掩码
 */
void sys_mqtt_report_emqx_only(const bms_pack_data_t *pack,
                               const bms_soc_data_t  *soc,
                               bms_fault_mask_t       fault);

/**
 * @brief   上报一帧 EMQX 高频精简数据(100ms, 仅第二通道)
 * @note    2026-08-18 双速率架构: 本函数只发 EMQX 主通道 bms/<id>/fast 精简帧(QoS0),
 *          不含 cell 数组/CRC, 供前端高频实时曲线; 华为云备用通道仍走 sys_mqtt_report(7s 全量).
 * @param   pack   采集数据
 * @param   soc    SOC 数据
 * @param   fault  故障掩码
 */
void sys_mqtt_report_fast(const bms_pack_data_t *pack,
                          const bms_soc_data_t  *soc,
                          bms_fault_mask_t       fault);

/**
 * @brief   推送故障告警
 */
void sys_mqtt_report_fault(bms_fault_mask_t fault);

/**
 * @brief   推送故障恢复通知(fault_clear, eventType=alert)
 */
void sys_mqtt_report_fault_clear(bms_fault_mask_t fault);

/**
 * @brief   推送一般通知(info_report, eventType=info)
 * @param   msg  通知文本, 如"充满"/"均衡完成"/"OTA进度"
 */
void sys_mqtt_report_info_event(const char *msg);

/**
 * @brief   主动上报设备信息(IP/MAC/版本/运行时间)
 */
void sys_mqtt_report_info(void);

/**
 * @brief   OTA 阶段即时回报(设备 → 云端)
 * @note    2026-08-14: OTA 下载/校验期间由 task_ota 调用, 立刻发一条
 *          properties/report(含 otaStage/otaProgress), 让前端无需等待周期上报
 *          即可看到阶段变化(对齐企业 OTA 任务进度回报)。函数内部已判 MQTT 连接态。
 */
void sys_mqtt_report_ota_stage(void);

/**
 * @brief   查询 MQTT 是否已连接
 */
bool sys_mqtt_is_connected(void);

/**
 * @brief   最近一次成功 publish 的单调时间戳(ms, esp_timer)
 * @retval  0 从未成功发布; 否则为 esp_timer 毫秒计数
 * @note    供 task_communication 计算 comm_status 的 COMM_CLOUD_REACHABLE 位
 */
uint64_t sys_mqtt_last_publish_ms(void);

/**
 * @brief   处理待补传的离线缓存(分批, 供 task_comm 主循环周期调用)
 * @note    H22: 原补传在 MQTT 事件回调内同步全量执行, 阻塞 keepalive
 *          被服务端踢下线; 现由主循环分批补传, 每次最多 20 条.
 */
void sys_mqtt_process_pending_replay(void);

/**
 * @brief   优雅断开并停止 MQTT 客户端 (重启/关机前调用)
 * @note    会发送 DISCONNECT 包, 让 broker 取消 QoS1/2 消息重投计时器,
 *          防止"restart 命令被重投导致反复重启"
 * @retval  BMS_OK 成功, 其他错误码
 */
bms_err_t sys_mqtt_stop(void);

#endif // SYS_MQTT_H
