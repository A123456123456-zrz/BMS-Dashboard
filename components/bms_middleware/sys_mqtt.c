/**
 * @file    sys_mqtt.c
 * @brief   MQTT 通信服务实现 (双向: 数据上报 + 远程命令)
 * @author  BMS Team
 * @date    2026-08
 * @note    适配华为云 IoTDA 标准鉴权 (2026-08-12 恢复, 覆盖 08-12 短暂迁移的 EMQX 静态账号):
 *          Topic 沿用华为云 IoTDA 格式:
 *          上行(ESP32 → 云端):
 *            $oc/devices/{id}/sys/properties/report   属性上报(电池数据/设备信息)
 *            $oc/devices/{id}/sys/events/up           事件/故障上报
 *          下行(云端 → ESP32):
 *            $oc/devices/{id}/sys/commands/#          命令下发
 *          响应(ESP32 → 云端):
 *            $oc/devices/{id}/sys/commands/response/{request_id}
 *          鉴权(华为云 IoTDA 平台标准鉴权, HMAC-SHA256 动态密码):
 *            ClientID = {device_id}_0_0_{YYYYMMDDHH}  (身份类型0, 签名类型0=不校验时间戳, 时间戳到小时UTC)
 *            Username = {device_id}                   (即 mqtt_client_id)
 *            Password = HMAC-SHA256(key=timestamp, msg=device_secret) → 64字符hex小写
 *            (官方规范: 以时间戳为密钥、设备密钥为内容, 与 ClientID 中时间戳一致)
 *          接入点(mqtt_uri)由配网页填写, 形如 mqtts://{实例ID}.iotda-app.cn-south-4.myhuaweicloud.com:8883
 *          签名类型0下平台仅校验密码, 不校验时间精确性 → 对 SNTP 抖动不敏感, 最稳。
 */
#include "sys_mqtt.h"
#include "sys_params.h"
#include "sys_data.h"
#include "sys_modbus.h"                                 /* Modbus RTU Slave(RS485 通信统计) */
#include "sys_wifi.h"
#include "sys_ota_status.h"
#include "bms_config.h"
#include "bms_types.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"                               /* 2026-09-14: 构建指纹上报(版本可追溯) */
#include "esp_timer.h"
#include "esp_sntp.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"                             /* ESP-IDF 内置 CA 证书 bundle (TLS) */
#include "cJSON.h"
#include "sys_offline_cache.h"                          /* 断网数据缓存与补传 */
#include "mbedtls/md.h"                                   /* HMAC-SHA256 (华为云 IoTDA 标准鉴权) */
#include "mqtt2_client_cert.h"                            /* 2026-08-16: 自建 Mosquitto mTLS 客户端证书 */
#include "mqtt2_client_key.h"                             /* 2026-08-16: 自建 Mosquitto mTLS 客户端私钥 */
#include "mqtt2_ca_bundle.h"                            /* 2026-08-17: wss 信任锚 = GTS Root R4(交叉) + GlobalSign Root CA */
#include <time.h>                                          /* time()/gmtime_r (IoTDA 时间戳 YYYYMMDDHH) */

/* ====== SNTP 时间同步(保留用于日志时间戳; EMQX 静态鉴权不依赖) ====== */
static bool s_time_synced  = false;
static bool s_sntp_started = false;    /* SNTP 只 init 一次, 重复 init 会破坏正在进行的同步 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <lwip/netdb.h>     // getaddrinfo, freeaddrinfo
#include <lwip/sockets.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "SYS_MQTT";

/* ====== SNTP 时间同步函数(保留用于日志时间戳; EMQX 静态鉴权不依赖) ======
 * 2026-08-12 迁移 EMQX 后, 静态账号不再需要时间戳签名,
 * 但设备时间对日志/上报时间戳仍有价值, 保留 SNTP 同步.
 * 2026-08-06 修复: esp_sntp_init() 只调用一次, check_reauth 重连时不再重复 init,
 *                 否则会重置 SNTP 模块导致时间永远无法同步, 认证持续失败 */
static void sntp_callback(struct timeval *tv)
{
    s_time_synced = true;
    ESP_LOGI(TAG, "SNTP 时间同步成功, UTC=%ld", (long)tv->tv_sec);
}

static void sync_time_via_sntp(void)
{
    if (s_time_synced) return;          /* 已同步 */

    /* SNTP 只初始化一次: 重复调用 esp_sntp_init() 不做 deinit 会破坏模块状态 */
    bool first_init = !s_sntp_started;
    if (first_init) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "启动 SNTP 时间同步...");
        setenv("TZ", "UTC0", 1);
        tzset();

        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");        /* 阿里云 NTP (国内优先) */
        esp_sntp_setservername(1, "ntp1.aliyun.com");       /* 阿里云备用 */
        esp_sntp_setservername(2, "pool.ntp.org");          /* 国际备用 */
        esp_sntp_setservername(3, "time.windows.com");      /* Windows 备用 */
        esp_sntp_set_time_sync_notification_cb(sntp_callback);
        esp_sntp_init();
    } else {
        /* SNTP 已启动但尚未同步(check_reauth 重连场景), 短暂等待 */
        ESP_LOGI(TAG, "SNTP 已启动, 等待同步...");
    }

    /* 首次启动等 10 秒, 重连场景只等 3 秒(SNTP 后台仍在跑, 不阻塞太久) */
    int wait_max = first_init ? 100 : 30;
    for (int i = 0; i < wait_max && !s_time_synced; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_time_synced) {
        ESP_LOGI(TAG, "时间已同步, timestamp=%lu", (unsigned long)time(NULL));
    } else {
        ESP_LOGW(TAG, "SNTP 同步超时(%ds), MQTT 鉴权可能失败! 请检查网络/DNS",
                 wait_max / 10);
    }
}

/* B1/B2 修复: MQTT 共享状态跨任务互斥保护
 * s_client / s_connected / s_last_command_id 由 MQTT 事件任务(写)与
 * task_comm / 上报任务(读并 publish)并发访问. 重连重建客户端时若 publish 线程
 * 读到已 destroy 的 s_client → use-after-free 崩溃; s_last_command_id 并发
 * 读写可能产生撕裂读. 统一用互斥锁保护, publish 前在锁内校验 s_client 有效性,
 * 重建时锁内 NULL 旧句柄后于锁外用本地副本销毁. */
static SemaphoreHandle_t s_state_mutex = NULL;
#define MQTT_STATE_LOCK()   do { if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY); } while (0)
#define MQTT_STATE_UNLOCK() do { if (s_state_mutex) xSemaphoreGive(s_state_mutex); } while (0)

static esp_mqtt_client_handle_t s_client = NULL;
static bool                     s_connected = false;
static esp_mqtt_client_handle_t s_client2 = NULL;   /* 2026-08-16: 自建 Mosquitto 第二通道 */
static bool                     s_connected2 = false;
/* 2026-09-10 提速: EMQX-only 全量帧期间临时抑制华为云发布(15000 条/天配额保 7s 节奏) */
static bool                     s_skip_iotda_publish = false;
static uint32_t                 s_boot_time_ms = 0;     // 启动时刻(用于计算运行时间)
static char                     s_last_command_id[64] = {0}; // 最近一次命令的 request_id

/* ====== 命令鉴权(2026-09-14 高可靠加固 A1) ======
 * 1) 防重放: 控制类命令按 command_id 环形去重(QoS1 重投/重放包只执行一次)
 * 2) 高危命令 HMAC token: token = HMAC-SHA256(mqtt_pass, command_id) 前 16 hex
 *    - 开关默认关闭(sys_params_get_cmd_auth), 打开后才强制校验 — 老链路零破坏
 *    - RESTART/RESET_PARAMS/OTA_UPGRADE/SET_* 属高危; 查询类(GET_*)不校验 */
#define CMD_AUTH_REPLAY_N     8                             /* 防重放环形表容量 */
static char      s_cmd_seen_id[CMD_AUTH_REPLAY_N][40];      /* 已执行 command_id 环形表 */
static uint8_t   s_cmd_seen_idx = 0;                        /* 环形写指针 */
static SemaphoreHandle_t s_cmd_auth_lock = NULL;            /* 环形表互斥 */

/* 高危命令白名单(大写命令名) */
static bool cmd_is_privileged(const char *cmd)
{
    static const char *const k_priv[] = {
        "RESTART", "RESET_PARAMS", "OTA_UPGRADE", "OTA_CHECK",
        "SET_CHARGE", "SET_DISCHARGE", "SET_BALANCE", "SET_RELAY",
        "SET_PARAM", "SET_WIFI", "SWITCH_BATTERY",
    };
    for (size_t i = 0; i < sizeof(k_priv) / sizeof(k_priv[0]); i++) {
        if (strcmp(cmd, k_priv[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* 防重放: 返回 true=重复命令(应丢弃), false=新命令(已登记) */
static bool cmd_replay_check(const char *cmd_id)
{
    if (cmd_id == NULL || cmd_id[0] == '\0') {
        return false;                    /* 无 ID 的命令不做防重放(兼容旧格式) */
    }
    if (s_cmd_auth_lock == NULL) {
        s_cmd_auth_lock = xSemaphoreCreateMutex();   /* 惰性创建, ISR 外安全 */
    }
    bool dup = false;
    if (xSemaphoreTake(s_cmd_auth_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < CMD_AUTH_REPLAY_N; i++) {
            if (s_cmd_seen_id[i][0] != '\0' &&
                strcmp(s_cmd_seen_id[i], cmd_id) == 0) {
                dup = true;              /* 命中: 同一 command_id 已执行过 */
                break;
            }
        }
        if (!dup) {
            /* 登记到环形表(覆盖最旧槽位) */
            strncpy(s_cmd_seen_id[s_cmd_seen_idx], cmd_id,
                    sizeof(s_cmd_seen_id[0]) - 1);
            s_cmd_seen_id[s_cmd_seen_idx][sizeof(s_cmd_seen_id[0]) - 1] = '\0';
            s_cmd_seen_idx = (uint8_t)((s_cmd_seen_idx + 1) % CMD_AUTH_REPLAY_N);
        }
        xSemaphoreGive(s_cmd_auth_lock);
    }
    return dup;
}

/* 高危命令 token 校验: token = HMAC-SHA256(mqtt_pass, command_id) 前 16 hex
 * 返回 true=通过(或鉴权未启用); false=拒绝 */
static bool cmd_auth_verify(const char *cmd, const char *cmd_id,
                            const cJSON *root)
{
    if (!sys_params_get_cmd_auth()) {
        return true;                     /* 鉴权未启用: 与历史行为一致, 全放行 */
    }
    if (!cmd_is_privileged(cmd)) {
        return true;                     /* 查询类命令不校验 */
    }
    if (cmd_id == NULL || cmd_id[0] == '\0') {
        ESP_LOGW(TAG, "[鉴权] %s 缺少 command_id, 无法校验 token, 拒绝", cmd);
        return false;
    }
    const cJSON *j_tok = cJSON_GetObjectItem(root, "token");
    if (!cJSON_IsString(j_tok) || j_tok->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "[鉴权] %s 缺少 token, 拒绝", cmd);
        return false;
    }
    /* HMAC 密钥源: 当前生效 broker 的密码(历史 broker= mqtt_pass, 自建= mqtt2_pass) */
    const bms_params_t *p = sys_params_get();
    const char *key = p->mqtt2_pass[0] != '\0' ? p->mqtt2_pass : p->mqtt_pass;
    size_t key_len = strlen(key);
    if (key_len == 0) {
        ESP_LOGE(TAG, "[鉴权] broker 密码为空, 无法计算 HMAC, 拒绝 %s", cmd);
        return false;
    }
    /* 与华为云 IoTDA 鉴权同一套 mbedtls HMAC 基础设施 */
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t mac[32] = {0};
    if (md == NULL ||
        mbedtls_md_hmac(md, (const unsigned char *)key, key_len,
                        (const unsigned char *)cmd_id, strlen(cmd_id),
                        mac) != 0) {
        ESP_LOGE(TAG, "[鉴权] HMAC 计算失败, 拒绝 %s", cmd);
        return false;
    }
    /* 前 16 字节转 hex(32 字符) */
    char expect[33] = {0};
    for (int i = 0; i < 16; i++) {
        snprintf(expect + i * 2, 3, "%02x", mac[i]);
    }
    /* 常量时间比较(防时序侧信道, 安全编码规范要求) */
    volatile uint8_t diff = 0;
    size_t tl = strlen(j_tok->valuestring);
    if (tl != 32) {
        diff = 1;
    } else {
        for (int i = 0; i < 32; i++) {
            diff |= (uint8_t)(expect[i] ^ j_tok->valuestring[i]);
        }
    }
    if (diff != 0) {
        ESP_LOGW(TAG, "[鉴权] %s token 校验失败, 拒绝", cmd);
        return false;
    }
    ESP_LOGI(TAG, "[鉴权] %s token 校验通过", cmd);
    return true;
}
/* ====== 命令鉴权结束 ====== */

/* H22 修复: 离线缓存补传"待执行"标志 — 由 MQTT 事件回调置位,
 * task_comm 主循环 sys_mqtt_process_pending_replay() 消费(分批补传).
 * 原因: 原在 MQTT_EVENT_CONNECTED 回调内同步全量补传, 阻塞事件线程
 * keepalive 数十秒, 被服务端判定超时踢下线("认证成功后又断开"死循环). */
static volatile bool s_replay_pending = false;

/* 命令回调函数指针(由 app_tasks 注册, 处理 clear_alarm/restart/ota 等) */
static sys_mqtt_cmd_callback_t s_cmd_cb = NULL;

/* 最近一次成功 publish 的单调时间戳(ms, esp_timer, 与墙钟无关)
 * 用于 comm_status 的 COMM_CLOUD_REACHABLE 位: 设备侧判断"数据真的发到云了" */
static uint64_t s_last_publish_ms = 0;

/* ====== 内部: 构造完整 IoTDA 格式 Topic ====== */
static void build_iotda_topic(char *buf, size_t buf_len, const char *suffix)
{
    const bms_params_t *p = sys_params_get();
    snprintf(buf, buf_len, "$oc/devices/%s/sys/%s", p->mqtt_client_id, suffix);
}

/* ====== 内部: 发送 JSON 到指定主题 ====== */
/* B1 修复: 在锁内校验 s_client 有效性后再发布, 杜绝重连销毁窗口内的 use-after-free */
static bool publish_json(const char *topic, const char *json)
{
    if (topic == NULL || json == NULL) {
        return false;
    }
    MQTT_STATE_LOCK();
    bool published = false;
    if (s_connected && s_client != NULL) {
        esp_mqtt_client_publish(s_client, topic, json, 0, BMS_MQTT_QOS, 0);
        published = true;
        s_last_publish_ms = (uint64_t)esp_timer_get_time() / 1000ULL;  // 记录成功 publish 时刻(驱动 cloud_reachable 位)
    }
    MQTT_STATE_UNLOCK();
    return published;
}

/* ====== 内部: 离线缓存补传发送函数 ======
 * 供 sys_offline_cache_replay() 回调, topic 为 NULL 时内部构造属性上报 topic */
static int cache_replay_send_func(const char *topic, const char *json)
{
    if (json == NULL) {
        return -1;
    }
    char real_topic[128];
    if (topic == NULL) {
        build_iotda_topic(real_topic, sizeof(real_topic), "properties/report");
        topic = real_topic;
    }
    MQTT_STATE_LOCK();
    int msg_id = -1;
    if (s_client != NULL) {
        msg_id = esp_mqtt_client_publish(s_client, topic, json, 0, BMS_MQTT_QOS, 0);
    }
    MQTT_STATE_UNLOCK();
    return (msg_id >= 0) ? 0 : -1;
}

/* ====== 内部: 命令回复(华为云格式) ====== */
/* B2 修复: s_last_command_id 在锁内读取并清空, 防止与事件任务并发写入产生撕裂读;
 * 必须在调用 publish_json 前释放锁, 避免递归死锁(非递归互斥量) */
static void send_cmd_resp(const char *command_name, bool ok, const cJSON *paras)
{
    char cmd_id[64];
    bool has_id;
    MQTT_STATE_LOCK();
    has_id = (s_last_command_id[0] != '\0');
    if (has_id) {
        strncpy(cmd_id, s_last_command_id, sizeof(cmd_id) - 1);
        cmd_id[sizeof(cmd_id) - 1] = '\0';
        s_last_command_id[0] = '\0'; // 清空,避免重复回复
    }
    MQTT_STATE_UNLOCK();

    if (!has_id) {
        return;     // 没有 command_id 时不回复
    }

    char topic[192];
    build_iotda_topic(topic, sizeof(topic), "commands/response/");
    strncat(topic, cmd_id, sizeof(topic) - strlen(topic) - 1);

    char paras_json[512];
    if (paras != NULL) {
        char *s = cJSON_PrintUnformatted(paras);
        if (s) {
            strncpy(paras_json, s, sizeof(paras_json) - 1);
            paras_json[sizeof(paras_json) - 1] = '\0';
            free(s);
        } else {
            snprintf(paras_json, sizeof(paras_json), "{\"result\":\"%s\"}", ok ? "ok" : "fail");
        }
    } else {
        snprintf(paras_json, sizeof(paras_json), "{\"result\":\"%s\"}", ok ? "ok" : "fail");
    }

    char json[640];
    snprintf(json, sizeof(json),
             "{\"result_code\":%d,\"response_name\":\"%s\",\"paras\":%s}",
             ok ? 0 : 1, command_name ? command_name : "", paras_json);
    publish_json(topic, json);   // 内部加锁, 此处必须已释放(防递归死锁)

    /* 2026-08-21 命令回执真确认(#2): EMQX 主通道回发 bms/<id>/cmd/ack.
     * 网页命令经 EMQX 下发后, 设备执行完在此回执; 后端消费腿订阅
     * bms/+/cmd/ack → 推 cmd_resp 事件, 前端据此真确认"设备已执行",
     * 不再依赖 15s 轮询回比(charge_mos 是否变化). 华为云回执仍保留(兜底). */
    {
        const bms_params_t *p = sys_params_get();
        if (p != NULL && p->mqtt2_client_id[0] != '\0' && s_connected2 && s_client2 != NULL) {
            char ack_topic[160];
            snprintf(ack_topic, sizeof(ack_topic), "%s%s/cmd/ack",
                     p->mqtt2_topic_prefix, p->mqtt2_client_id);
            char ack_json[384];
            snprintf(ack_json, sizeof(ack_json),
                     "{\"command_name\":\"%s\",\"ok\":%s,\"status\":\"%s\",\"ts\":%llu}",
                     command_name ? command_name : "", ok ? "true" : "false",
                     ok ? "ok" : "fail",
                     (unsigned long long)(esp_timer_get_time() / 1000ULL));
            MQTT_STATE_LOCK();
            esp_mqtt_client_publish(s_client2, ack_topic, ack_json, 0, BMS_MQTT_QOS, 0);
            MQTT_STATE_UNLOCK();
            ESP_LOGI(TAG, "[ACK] EMQX 回执 %s -> %s (%s)", ack_topic, command_name ? command_name : "", ok ? "ok" : "fail");
        }
    }
}

/* ====== 内部: 兼容 IoTDA 两种 paras 取值方式 ======
 * 华为云命令/消息格式: {cmd, paras:{...}} 或顶层平铺
 * 优先从 paras 子对象取, fallback 到 root 顶层(与 app_tasks.c cmd_get_field 一致)
 * 2026-08-10 修复: publish_cmd(/messages API)下发参数在 paras 内层,
 *   原 handle_set_param/handle_switch_battery 仅读顶层导致串数/容量下发不生效 */
static const cJSON *cmd_get_field(const cJSON *root, const char *field)
{
    const cJSON *paras = cJSON_GetObjectItem(root, "paras");
    if (paras != NULL) {
        const cJSON *v = cJSON_GetObjectItem(paras, field);
        if (v != NULL) return v;
    }
    return cJSON_GetObjectItem(root, field);
}

/* ====== 内部: 处理 set_param 命令(修改阈值) ====== */
static void handle_set_param(const cJSON *root)
{
    const cJSON *j_key   = cmd_get_field(root, "key");
    const cJSON *j_value = cmd_get_field(root, "value");
    if (!cJSON_IsString(j_key) || !cJSON_IsNumber(j_value)) {
        send_cmd_resp("set_param", false, NULL);
        return;
    }

    const char *key = j_key->valuestring;
    int value = j_value->valueint;

    bms_params_t params = *sys_params_get();
    bool found = true;

    if      (strcmp(key, "cell_ov_prot_mv") == 0)  params.cell_ov_prot_mv  = (uint16_t)value;
    else if (strcmp(key, "cell_uv_prot_mv") == 0)  params.cell_uv_prot_mv  = (uint16_t)value;
    else if (strcmp(key, "temp_ot_prot_dc") == 0)  params.temp_ot_prot_dc  = (int16_t)value;
    else if (strcmp(key, "temp_ut_prot_dc") == 0)  params.temp_ut_prot_dc  = (int16_t)value;
    else if (strcmp(key, "chg_oc_prot_ma")  == 0)  params.chg_oc_prot_ma   = (int16_t)value;
    else if (strcmp(key, "dsg_oc_prot_ma")  == 0)  params.dsg_oc_prot_ma   = (int16_t)value;
    else if (strcmp(key, "cell_ov_warn_mv") == 0)  params.cell_ov_warn_mv  = (uint16_t)value;
    else if (strcmp(key, "cell_uv_warn_mv") == 0)  params.cell_uv_warn_mv  = (uint16_t)value;
    else if (strcmp(key, "chg_oc_warn_ma")  == 0)  params.chg_oc_warn_ma   = (int16_t)value;
    else if (strcmp(key, "dsg_oc_warn_ma")  == 0)  params.dsg_oc_warn_ma   = (int16_t)value;
    else if (strcmp(key, "temp_ot_warn_dc") == 0)  params.temp_ot_warn_dc  = (int16_t)value;
    else if (strcmp(key, "temp_ut_warn_dc") == 0)  params.temp_ut_warn_dc  = (int16_t)value;
    else if (strcmp(key, "cell_dv_warn_mv") == 0)  params.cell_dv_warn_mv  = (uint16_t)value;
    else if (strcmp(key, "soc_low_warn_pct")== 0)  params.soc_low_warn_pct = (uint8_t)value;
    else if (strcmp(key, "balance_threshold_mv") == 0) params.balance_threshold_mv = (uint16_t)value;
    else if (strcmp(key, "balance_stop_mv") == 0)  params.balance_stop_mv  = (uint16_t)value;
    else if (strcmp(key, "soc_offset") == 0)       params.soc_offset       = (float)j_value->valuedouble;
    else if (strcmp(key, "soc_gain")   == 0)       params.soc_gain         = (float)j_value->valuedouble;
    /* v7: 网页可调参数补齐(与 tools/dashboard app.py PARAM_META 对齐) */
    else if (strcmp(key, "cell_ov_recover_mv") == 0)  params.cell_ov_recover_mv = (uint16_t)value;
    else if (strcmp(key, "cell_uv_recover_mv") == 0)  params.cell_uv_recover_mv = (uint16_t)value;
    else if (strcmp(key, "cell_dv_recover_mv") == 0)  params.cell_dv_recover_mv = (uint16_t)value;
    else if (strcmp(key, "rated_current_ma")   == 0)  params.rated_current_ma   = (int16_t)value;
    else if (strcmp(key, "overload_warn_ratio")== 0)  params.overload_warn_ratio= (float)j_value->valuedouble;
    else if (strcmp(key, "overload_prot_ratio")== 0)  params.overload_prot_ratio= (float)j_value->valuedouble;
    else if (strcmp(key, "temp_ot_recover_dc") == 0)  params.temp_ot_recover_dc = (int16_t)value;
    else if (strcmp(key, "temp_ut_recover_dc") == 0)  params.temp_ut_recover_dc = (int16_t)value;
    else if (strcmp(key, "temp_dtdt_warn")     == 0)  params.temp_dtdt_warn     = (float)j_value->valuedouble;
    else if (strcmp(key, "dv_dt_warn_mvps")    == 0)  params.dv_dt_warn_mvps    = (float)j_value->valuedouble;
    else if (strcmp(key, "soc_low_recover_pct")== 0)  params.soc_low_recover_pct= (uint8_t)value;
    else if (strcmp(key, "soh_low_warn_pct")   == 0)  params.soh_low_warn_pct   = (uint8_t)value;
    else if (strcmp(key, "balance_timeout_min") == 0)  params.balance_timeout_min = (uint16_t)value;
    else if (strcmp(key, "charge_cc_current_ma")== 0)  params.charge_cc_current_ma= (uint16_t)value;
    else if (strcmp(key, "charge_cc_soc_thr")  == 0)  params.charge_cc_soc_thr  = (uint8_t)value;
    else if (strcmp(key, "charge_cv_soc_thr")  == 0)  params.charge_cv_soc_thr  = (uint8_t)value;
    else if (strcmp(key, "cell_series_num")    == 0) {
        /* 串数运行时切换: 网页下发串数, 算法循环上限/上报数组实时适配
         * F6 修复: 上限用硬件能力 BMS_HW_MAX_SERIES_NUM(非编译期数组上限 32),
         *          防止 6S 硬件设 32 → 读取不到的串位出现虚假 0V 单体误报警 */
        uint8_t n = (uint8_t)value;
        if (n < 1 || n > BMS_HW_MAX_SERIES_NUM) {
            send_cmd_resp("set_param", false, NULL);
            return;
        }
        params.cell_series_num = n;
        /* 总压基准随串数联动(标称/满充/截止 = 单体 × 串数) */
        params.nominal_voltage_mv = (uint32_t)(BMS_NOMINAL_VOLTAGE_MV / BMS_CELL_SERIES_NUM) * n;
        params.full_voltage_mv    = (uint32_t)(BMS_FULL_VOLTAGE_MV    / BMS_CELL_SERIES_NUM) * n;
        params.cutoff_voltage_mv  = (uint32_t)(BMS_CUTOFF_VOLTAGE_MV  / BMS_CELL_SERIES_NUM) * n;
    }
    else if (strcmp(key, "cell_capacity_mah") == 0)  params.cell_capacity_mah = (uint16_t)value;
    else if (strcmp(key, "nominal_voltage_mv")== 0)  params.nominal_voltage_mv= (uint32_t)value;
    else if (strcmp(key, "full_voltage_mv")   == 0)  params.full_voltage_mv   = (uint32_t)value;
    else if (strcmp(key, "cutoff_voltage_mv") == 0)  params.cutoff_voltage_mv = (uint32_t)value;
    /* v8: 电池类型/SOC 算法选择(网页电池参数配置下发, 实时切换估算算法) */
    else if (strcmp(key, "battery_type") == 0) {
        uint8_t bt = (uint8_t)value;
        if (bt > 3) {   /* 0=LFP 1=NCM 2=LTO 3=铅酸 */
            send_cmd_resp("set_param", false, NULL);
            return;
        }
        params.battery_type = bt;
    }
    else if (strcmp(key, "soc_algo") == 0) {
        uint8_t sa = (uint8_t)value;
        if (sa > 4) {   /* 0=AEKF 1=纯安时积分 2=OCV查表 3=MCC-EKF 4=UKF */
            send_cmd_resp("set_param", false, NULL);
            return;
        }
        params.soc_algo = sa;
    }
    /* v9: 均衡策略/起始SOC(网页均衡控制下发) */
    else if (strcmp(key, "balance_start_soc_pct") == 0) {
        uint8_t bsp = (uint8_t)value;
        if (bsp > 100) {
            send_cmd_resp("set_param", false, NULL);
            return;
        }
        params.balance_start_soc_pct = bsp;
    }
    else if (strcmp(key, "balance_strategy") == 0) {
        uint8_t bs = (uint8_t)value;
        if (bs > 2) {   /* 0=电压差触发 1=容量差触发 2=定时均衡 */
            send_cmd_resp("set_param", false, NULL);
            return;
        }
        params.balance_strategy = bs;
    }
    /* v10: 上报间隔(网页实时性调节, 受15000/天消息上限约束, 最小7s=12343/天) */
    else if (strcmp(key, "report_interval") == 0) {
        uint16_t ri = (uint16_t)value;
        if (ri < 7 || ri > 3600) {   /* 7s=12343/天, 严格低于15000上限; 上限1h防止误设 */
            send_cmd_resp("set_param", false, NULL);
            return;
        }
        params.report_interval_sec = ri;
    }
    else {
        found = false;
    }

    if (!found) {
        send_cmd_resp("set_param", false, NULL);
        return;
    }

    /* 2026-08-19 安全加固(F3): 保护阈值下发统一范围校验, 与启动 POST 自检一致.
     *   原实现 cell_ov/uv/temp/chg_oc/dsg_oc 等阈值直接强转写入 NVS 并立即生效,
     *   云端一条越界 set_param(如 uv=0 / ov=99999)即可让电池保护全部失效.
     *   非法值拒绝下发并回复失败, 不回滚已合法字段(保持其他字段可用).
     * 2026-08-22 改进(B1): 校验范围由"整个 params 结构"改为"仅本次下发的字段".
     *   原实现若设备 NVS 中任一旧阈值越界(历史非法值/未接线残留), 会拒绝**所有**
     *   后续 set_param(实测下发合法 soc_algo=3 也回执 fail). 现只校验本次 key 对应
     *   字段, 其他字段保持原值不受影响, 单字段问题不再卡死整个参数下发链路. */
    {
        int _v = value;
        bool _bad = false;
        if      (strcmp(key, "cell_ov_prot_mv") == 0)   _bad = (_v < 3000 || _v > 5000);
        else if (strcmp(key, "cell_uv_prot_mv") == 0)   _bad = (_v < 2000 || _v > 3500);
        else if (strcmp(key, "cell_ov_warn_mv") == 0)   _bad = (_v < 3000 || _v > 5000);
        else if (strcmp(key, "cell_uv_warn_mv") == 0)   _bad = (_v < 2000 || _v > 3500);
        else if (strcmp(key, "cell_ov_recover_mv") == 0) _bad = (_v < 3000 || _v > 5000);
        else if (strcmp(key, "cell_uv_recover_mv") == 0) _bad = (_v < 2000 || _v > 3500);
        else if (strcmp(key, "temp_ot_prot_dc") == 0)   _bad = (_v < -50  || _v > 150);
        else if (strcmp(key, "temp_ut_prot_dc") == 0)   _bad = (_v < -50  || _v > 100);
        else if (strcmp(key, "temp_ot_warn_dc") == 0)   _bad = (_v < -50  || _v > 150);
        else if (strcmp(key, "temp_ut_warn_dc") == 0)   _bad = (_v < -50  || _v > 100);
        else if (strcmp(key, "chg_oc_prot_ma") == 0)    _bad = (_v < 0    || _v > 50000);
        else if (strcmp(key, "dsg_oc_prot_ma") == 0)    _bad = (_v < 0    || _v > 50000);
        else if (strcmp(key, "cell_capacity_mah") == 0) _bad = (_v < 500  || _v > 60000);
        if (_bad) {
            ESP_LOGW(TAG, "set_param: 阈值越界被拒绝 (key=%s value=%d)", key, value);
            send_cmd_resp("set_param", false, NULL);
            return;
        }
    }

    bms_err_t err = sys_params_set(&params);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", err == BMS_OK ? "saved" : "nvs error");
    send_cmd_resp("set_param", err == BMS_OK, resp);
    cJSON_Delete(resp);

    ESP_LOGI(TAG, "set_param: %s=%d %s", key, value, err == BMS_OK ? "OK" : "FAIL");
}

/* ====== 内部: 处理 switch_battery 命令(多电池组切换, 网页 deviceSelect) ======
 * paras: {"group": 1, "series": 8, "capacity_mah": 3000}
 *   group        电池组编号(BMS-001/002/003 → 1/2/3), 仅记录/回显
 *   series       该组串数(1~BMS_MAX_CELL_SERIES_NUM), 可选(缺省保持当前)
 *   capacity_mah 该组单体容量 mAh, 可选(缺省保持当前)
 * 生效方式: 写入 NVS 参数并实时适配算法(串数/容量/总压基准联动) */
static void handle_switch_battery(const cJSON *root)
{
    const cJSON *j_group = cmd_get_field(root, "group");
    const cJSON *j_series = cmd_get_field(root, "series");
    const cJSON *j_cap = cmd_get_field(root, "capacity_mah");

    bms_params_t params = *sys_params_get();
    int group = cJSON_IsNumber(j_group) ? j_group->valueint : 0;

    /* 串数: 校验 1~BMS_HW_MAX_SERIES_NUM (F6: 硬件上限, 防虚假 0V 单体) */
    if (cJSON_IsNumber(j_series)) {
        int n = j_series->valueint;
        if (n < 1 || n > BMS_HW_MAX_SERIES_NUM) {
            send_cmd_resp("switch_battery", false, NULL);
            return;
        }
        params.cell_series_num = (uint8_t)n;
        /* 总压基准随串数联动(标称/满充/截止 = 单体 × 串数) */
        params.nominal_voltage_mv = (uint32_t)(BMS_NOMINAL_VOLTAGE_MV / BMS_CELL_SERIES_NUM) * n;
        params.full_voltage_mv    = (uint32_t)(BMS_FULL_VOLTAGE_MV    / BMS_CELL_SERIES_NUM) * n;
        params.cutoff_voltage_mv  = (uint32_t)(BMS_CUTOFF_VOLTAGE_MV  / BMS_CELL_SERIES_NUM) * n;
    }
    /* 容量: 校验合理范围 */
    if (cJSON_IsNumber(j_cap)) {
        int cap = j_cap->valueint;
        if (cap < 100 || cap > 100000) {
            send_cmd_resp("switch_battery", false, NULL);
            return;
        }
        params.cell_capacity_mah = (uint16_t)cap;
        params.charge_cc_current_ma = (uint16_t)cap;   /* 恒流充电默认 1C 跟随容量 */
    }

    bms_err_t err = sys_params_set(&params);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", err == BMS_OK ? "switched" : "nvs error");
    if (group > 0) cJSON_AddNumberToObject(resp, "group", group);
    cJSON_AddNumberToObject(resp, "series", params.cell_series_num);
    cJSON_AddNumberToObject(resp, "capacity_mah", params.cell_capacity_mah);
    send_cmd_resp("switch_battery", err == BMS_OK, resp);
    cJSON_Delete(resp);

    ESP_LOGI(TAG, "switch_battery: group=%d series=%u capacity=%umAh %s",
             group, params.cell_series_num, params.cell_capacity_mah,
             err == BMS_OK ? "OK" : "FAIL");
}

/* ====== 内部: 处理 get_params 命令(返回当前所有参数) ====== */
static void handle_get_params(void)
{
    const bms_params_t *p = sys_params_get();
    cJSON *resp = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();

    cJSON_AddNumberToObject(params, "cell_ov_prot_mv", p->cell_ov_prot_mv);
    cJSON_AddNumberToObject(params, "cell_uv_prot_mv", p->cell_uv_prot_mv);
    cJSON_AddNumberToObject(params, "temp_ot_prot_dc", p->temp_ot_prot_dc);
    cJSON_AddNumberToObject(params, "temp_ut_prot_dc", p->temp_ut_prot_dc);
    cJSON_AddNumberToObject(params, "chg_oc_prot_ma",  p->chg_oc_prot_ma);
    cJSON_AddNumberToObject(params, "dsg_oc_prot_ma",  p->dsg_oc_prot_ma);
    cJSON_AddNumberToObject(params, "cell_ov_warn_mv", p->cell_ov_warn_mv);
    cJSON_AddNumberToObject(params, "cell_uv_warn_mv", p->cell_uv_warn_mv);
    cJSON_AddNumberToObject(params, "chg_oc_warn_ma",  p->chg_oc_warn_ma);
    cJSON_AddNumberToObject(params, "dsg_oc_warn_ma",  p->dsg_oc_warn_ma);
    cJSON_AddNumberToObject(params, "temp_ot_warn_dc", p->temp_ot_warn_dc);
    cJSON_AddNumberToObject(params, "temp_ut_warn_dc", p->temp_ut_warn_dc);
    cJSON_AddNumberToObject(params, "cell_dv_warn_mv", p->cell_dv_warn_mv);
    cJSON_AddNumberToObject(params, "soc_low_warn_pct",p->soc_low_warn_pct);
    cJSON_AddNumberToObject(params, "balance_threshold_mv", p->balance_threshold_mv);
    cJSON_AddNumberToObject(params, "balance_stop_mv", p->balance_stop_mv);
    cJSON_AddNumberToObject(params, "soc_offset",      p->soc_offset);
    cJSON_AddNumberToObject(params, "soc_gain",        p->soc_gain);
    cJSON_AddNumberToObject(params, "cell_capacity_mah", p->cell_capacity_mah);
    cJSON_AddNumberToObject(params, "cycle_count",     p->cycle_count);
    cJSON_AddNumberToObject(params, "soh",             p->soh);
    cJSON_AddNumberToObject(params, "report_interval", p->report_interval_sec);

    cJSON_AddItemToObject(resp, "params", params);
    send_cmd_resp("get_params", true, resp);
    cJSON_Delete(resp);
}

/* ====== 内部: 处理 get_info 命令(返回设备信息) ====== */
static void handle_get_info(void)
{
    char ip[16] = "0.0.0.0";
    char mac[18] = "00:00:00:00:00:00";
    (void)sys_wifi_get_ip_str(ip, sizeof(ip));
    (void)sys_wifi_get_mac_str(mac, sizeof(mac));

    const bms_params_t *p = sys_params_get();
    uint32_t uptime_s = (uint32_t)((esp_timer_get_time() - s_boot_time_ms * 1000) / 1000000);

    /* 2026-08-13: 上报待确认升级信息(人工确认模式) — 检查到新版本后,
     * 前端据此展示"发现新版本 vX.X.X, 是否升级?"弹窗 */
    const char *ota_new_ver = sys_ota_status_get_pending_version();
    const char *ota_new_url = sys_ota_status_get_pending_url();

    cJSON *resp = cJSON_CreateObject();
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "firmware",    p->firmware_version);
    if (ota_new_ver != NULL) {
        cJSON_AddStringToObject(info, "ota_new_version", ota_new_ver);
    }
    if (ota_new_url != NULL) {
        cJSON_AddStringToObject(info, "ota_new_url", ota_new_url);
    }
    cJSON_AddStringToObject(info, "ip",          ip);
    cJSON_AddStringToObject(info, "mac",         mac);
    cJSON_AddNumberToObject(info, "uptime_s",    uptime_s);
    cJSON_AddBoolToObject(  info, "wifi_connected", sys_wifi_is_connected());
    bool mqtt_conn;
    MQTT_STATE_LOCK();
    mqtt_conn = s_connected;
    MQTT_STATE_UNLOCK();
    cJSON_AddBoolToObject(  info, "mqtt_connected", mqtt_conn);
    cJSON_AddStringToObject(info, "client_id",   p->mqtt_client_id);
    cJSON_AddStringToObject(info, "broker",      p->mqtt_uri);
    /* 2026-09-14 高可靠加固: 上报上次复位原因(故障黑匣子) — 云端可追溯"上次为什么死" */
    {
        uint8_t bb_rst = 0;
        if (sys_params_get_blackbox(&bb_rst) == BMS_OK && bb_rst != 0) {
            cJSON_AddNumberToObject(info, "last_reset_reason", bb_rst);
        }
    }
    /* 2026-09-14 高可靠加固: 上报完整构建指纹(版本|编译日期时间) — 版本可追溯 */
    {
        const esp_app_desc_t *app_desc = esp_app_get_description();
        if (app_desc != NULL) {
            char fp[64];
            snprintf(fp, sizeof(fp), BMS_BUILD_FINGERPRINT_FMT,
                     (app_desc->version[0] != '\0') ? app_desc->version : p->firmware_version,
                     app_desc->date, app_desc->time);
            cJSON_AddStringToObject(info, "build", fp);
        }
    }
    cJSON_AddItemToObject(resp, "info", info);

    /* H4 修复: 原代码无条件走 send_cmd_resp, 而无 command_id 时(连接后主动上报)
     * send_cmd_resp 直接 return, 导致设备上线信息永远发布不出去.
     * 现在: 有 command_id(命令响应场景)走命令响应 topic;
     *       无 command_id(连接后主动上报/外部调用)直接发布到属性上报 topic. */
    bool has_cmd_id;
    MQTT_STATE_LOCK();
    has_cmd_id = (s_last_command_id[0] != '\0');
    MQTT_STATE_UNLOCK();
    if (has_cmd_id) {
        send_cmd_resp("get_info", true, resp);
    } else {
        char topic[128];
        build_iotda_topic(topic, sizeof(topic), BMS_MQTT_TOPIC_INFO);
        char *s = cJSON_PrintUnformatted(resp);
        if (s != NULL) {
            publish_json(topic, s);
            free(s);
        }
    }
    cJSON_Delete(resp);
}

/* ====== 内部: 从 JSON 对象中解包出 cmd_root(含 cmd 字段的对象) ======
 * 支持 4 种嵌套格式, 按优先级自动探测:
 *  Format A: {"cmd":"xxx","paras":{...}}                        顶层就是命令
 *  Format B: {"content":"{\"cmd\":\"xxx\"}"}                    content是字符串(1层字符串化)
 *  Format C: {"content":{"content":"{\"cmd\":\"xxx\"}"}}        content.content是字符串(2层对象+字符串化)
 *              ↑ 这就是用户日志里的格式: 华为云 /messages 标准包装
 *  Format D: {"content":{"cmd":"xxx","paras":{...}}}            content是对象,内含cmd
 *  Format E: {"request_id":"...","paras":{"cmd":"xxx",...}}     IoTDA commands/# 旧格式
 *
 * 输出: *out_cmd_root = 指向含有 cmd 字段的 cJSON 对象 (调用者需要 cJSON_Delete 它,
 *                                                  除非它就是输入root, 需要用 *out_needs_delete 判断)
 *        *out_needs_delete = true 时 *out_cmd_root 是新申请的, 需要 delete;
 *                            = false 时 *out_cmd_root == input root, 不能单独 delete
 * 返回: true = 找到 cmd_root 并保证里面有 cmd 字符串字段
 *       false = 所有格式都不匹配 (调用者直接打错误日志返回)  */
static bool unwrap_cmd_root(cJSON *root, cJSON **out_cmd_root, bool *out_needs_delete)
{
    *out_cmd_root = root;
    *out_needs_delete = false;

    /* ===== 格式 A: 顶层就是命令 (直接检测) ===== */
    if (cJSON_IsString(cJSON_GetObjectItem(root, "cmd"))) {
        return true;
    }

    /* ===== 格式 B/C/D: 沿 content/paras 链路逐级解包, 最多 4 层防止死循环 =====
     * H9 修复: 原实现 Case 2 下钻子对象时把 cursor_needs_del 置 false,
     * 若其后未找到 cmd 而 break, 动态 cJSON_Parse 出的整棵树无人释放 → 堆泄漏.
     * 现在把"动态树根"始终记录在 *out_cmd_root(needs_delete=true),
     * 下钻子对象只是移动 cursor, 树的归属不变; 失败路径统一释放该树. */
    cJSON *cursor = root;
    bool found = false;

    for (int depth = 0; depth < 4; depth++) {
        /* 当前 cursor 已含 cmd → 找到了 */
        if (cJSON_IsString(cJSON_GetObjectItem(cursor, "cmd"))) {
            found = true;
            break;
        }

        /* 找下一层候选: content 或 paras 字段 */
        cJSON *next = cJSON_GetObjectItem(cursor, "content");
        if (next == NULL) {
            next = cJSON_GetObjectItem(cursor, "paras");
        }
        if (next == NULL) {
            break;   /* 没有下一层了, 结束探测 */
        }

        /* ---- Case 1: next 是字符串 → 它是 JSON 字符串化的, 二次 parse ----
         *        (Format B: content=字符串  或  Format C 内层 content=字符串) */
        if (cJSON_IsString(next) && next->valuestring != NULL && next->valuestring[0] != '\0') {
            cJSON *parsed = cJSON_Parse(next->valuestring);
            if (parsed == NULL) {
                ESP_LOGW(TAG, "[unwrap] 第%d层 content/paras 字符串解析失败: %.100s",
                         depth, next->valuestring);
                break;   /* parse 失败就停止探测, 不要硬试更深 */
            }
            /* 上一棵动态树(若存在)已不再需要, 释放后再挂新树 */
            if (*out_needs_delete && *out_cmd_root != NULL && *out_cmd_root != root) {
                cJSON_Delete(*out_cmd_root);
            }
            *out_cmd_root = parsed;
            *out_needs_delete = true;
            cursor = parsed;
            continue;   /* 进入下一轮, 检查新解析出的对象里是否直接有 cmd */
        }

        /* ---- Case 2: next 是对象 → 直接进入下一层 (Format D / Format C 外层对象) ----
         * 只移动 cursor, 树归属不变: next 是当前树(或其子对象)的成员,
         * 最终由 *out_cmd_root 所指的树根统一释放 */
        if (cJSON_IsObject(next)) {
            cursor = next;
            continue;
        }

        /* next 是数组/数字等其他类型, 没有意义 → 停止探测 */
        break;
    }

    if (!found) {
        /* 兜底: 动态解析出的树没有 cmd, 释放掉, 不能泄露 */
        if (*out_needs_delete && *out_cmd_root != NULL && *out_cmd_root != root) {
            cJSON_Delete(*out_cmd_root);
        }
        *out_cmd_root = root;
        *out_needs_delete = false;
        return false;
    }

    return true;
}

/* ====== 内部: 属性设置响应(华为云格式) ======
 * topic:  $oc/devices/{id}/sys/properties/set/response/{request_id}
 * 格式:   {"result_code":0,"result_desc":"success"} */
static void send_prop_set_resp(const char *request_id, int result_code, const char *desc)
{
    if (request_id == NULL || request_id[0] == '\0') {
        return;
    }

    char topic[192];
    build_iotda_topic(topic, sizeof(topic), "properties/set/response/");
    strncat(topic, request_id, sizeof(topic) - strlen(topic) - 1);

    char json[192];
    snprintf(json, sizeof(json), "{\"result_code\":%d,\"result_desc\":\"%s\"}",
             result_code, (desc && desc[0]) ? desc : "success");
    publish_json(topic, json);
}

/* ====== 内部: 处理属性设置(RW 属性下发, BMS-V2 物模型) ======
 * topic:  $oc/devices/{id}/sys/properties/set/{request_id}
 * 数据:   {"object_device_id":"...","services":[{"service_id":"BMS","properties":{...}}]}
 * 遍历 properties 中的 RW 属性并应用:
 *   - 继电器/模式类 → sys_data_set_* (实时生效)
 *   - 阈值/限流类   → 收集到 bms_params_t, 最后统一 sys_params_set 持久化
 * 注意: request_id 是下行 topic 中 properties/set/ 之后的部分
 *       (可能带 "request_id=" 前缀, 原样带回响应 topic) */
static void handle_property_set(const char *data, int data_len, const char *request_id)
{
    /* ====== B3 幂等查漏(2026-09-15 高可靠加固) ======
     * properties/set 通道下发 chargeEnable(=关/开 FET)与阈值参数,
     * 与主命令通道同等高危; QoS1 重投/重放会重复执行 — 按 request_id 去重 */
    if (cmd_replay_check(request_id)) {
        ESP_LOGW(TAG, "[鉴权] property set 重放(rid=%s 已执行), 丢弃", request_id);
        send_prop_set_resp(request_id, 1, "replayed");
        return;
    }
    char buf[1024];
    int copy_len = data_len < (int)sizeof(buf) - 1 ? data_len : (int)sizeof(buf) - 1;
    memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    ESP_LOGI(TAG, "recv property set: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        send_prop_set_resp(request_id, 1, "parse error");
        return;
    }

    /* 定位 services[0].properties */
    const cJSON *j_services = cJSON_GetObjectItem(root, "services");
    const cJSON *j_props = NULL;
    if (cJSON_IsArray(j_services) && cJSON_GetArraySize(j_services) > 0) {
        const cJSON *j_svc = cJSON_GetArrayItem(j_services, 0);
        j_props = cJSON_GetObjectItem(j_svc, "properties");
    }
    if (!cJSON_IsObject(j_props)) {
        ESP_LOGW(TAG, "属性设置: 未找到 properties 对象");
        send_prop_set_resp(request_id, 1, "no properties");
        cJSON_Delete(root);
        return;
    }

    /* 阈值/限流参数(收集后统一保存, 避免多次写 NVS) */
    bms_params_t params = *sys_params_get();
    bool params_changed = false;

    const cJSON *j = NULL;

    /* ---- 充电/放电使能 ---- */
    j = cJSON_GetObjectItem(j_props, "chargeEnable");
    if (cJSON_IsNumber(j)) {
        bool chg_on = false, dsg_on = false;
        sys_data_get_relay(&chg_on, &dsg_on);
        sys_data_set_remote_override(true);   /* 先置位, 30s 内保护任务不覆盖(防覆盖窗口) */
        sys_data_set_relay(j->valueint != 0, dsg_on);
        sys_data_set_charge_mode((j->valueint != 0) ? CHARGE_MODE_CC : CHARGE_MODE_STOP);
        if (j->valueint != 0) {
            sys_data_set_master_power_off(false);   /* 云端开启 = 解除 SW_PWR 关断 */
        }
        ESP_LOGI(TAG, "prop chargeEnable=%d", j->valueint);
    }
    j = cJSON_GetObjectItem(j_props, "dischargeEnable");
    if (cJSON_IsNumber(j)) {
        bool chg_on = false, dsg_on = false;
        sys_data_get_relay(&chg_on, &dsg_on);
        sys_data_set_remote_override(true);   /* 先置位, 30s 内保护任务不覆盖(防覆盖窗口) */
        sys_data_set_relay(chg_on, j->valueint != 0);
        if (j->valueint != 0) {
            sys_data_set_master_power_off(false);   /* 云端开启 = 解除 SW_PWR 关断 */
        }
        ESP_LOGI(TAG, "prop dischargeEnable=%d", j->valueint);
    }

    /* ---- 充电模式 ---- */
    j = cJSON_GetObjectItem(j_props, "chargeMode");
    if (cJSON_IsNumber(j)) {
        int m = j->valueint;
        if (m >= CHARGE_MODE_STOP && m <= CHARGE_MODE_FULL) {
            sys_data_set_charge_mode((bms_charge_mode_e)m);
            ESP_LOGI(TAG, "prop chargeMode=%d", m);
        }
    }

    /* ---- 充/放电限流(映射到过流预警阈值, 供保护任务降额) ---- */
    j = cJSON_GetObjectItem(j_props, "chargeCurLimit");
    if (cJSON_IsNumber(j)) { params.chg_oc_warn_ma = (int16_t)j->valueint; params_changed = true; }
    j = cJSON_GetObjectItem(j_props, "dischargeCurLimit");
    if (cJSON_IsNumber(j)) { params.dsg_oc_warn_ma = (int16_t)j->valueint; params_changed = true; }

    /* ---- 均衡控制 ---- */
    j = cJSON_GetObjectItem(j_props, "balanceEnable");
    if (cJSON_IsNumber(j)) {
        if (j->valueint != 0) {
            sys_data_set_balance_mask(0xFFFFFFFFu);   /* 默认全开自动均衡 */
            sys_data_set_balance_mode(BALANCE_MODE_PASSIVE);
        } else {
            sys_data_set_balance_mask(0x0000);
        }
        ESP_LOGI(TAG, "prop balanceEnable=%d", j->valueint);
    }
    j = cJSON_GetObjectItem(j_props, "balanceMode");
    if (cJSON_IsString(j) && j->valuestring) {
        /* 物模型: auto(自动)/manual(手动); LTC6804 内部均衡均走 PASSIVE,
         * 主动均衡需外接电路(HW_ENABLE_ACTIVE_BALANCE), 此处统一 PASSIVE */
        if (strcmp(j->valuestring, "auto") == 0) {
            sys_data_set_balance_mode(BALANCE_MODE_PASSIVE);
        }
        ESP_LOGI(TAG, "prop balanceMode=%s", j->valuestring);
    }
    j = cJSON_GetObjectItem(j_props, "balanceMask");
    if (cJSON_IsNumber(j)) {
        sys_data_set_balance_mask((bms_balance_mask_t)j->valueint);
        ESP_LOGI(TAG, "prop balanceMask=0x%08X", (unsigned)j->valueint);
    }

    /* ---- 继电器控制 ---- */
    j = cJSON_GetObjectItem(j_props, "relayCharge");
    if (cJSON_IsNumber(j)) {
        bool chg_on = false, dsg_on = false;
        sys_data_get_relay(&chg_on, &dsg_on);
        sys_data_set_remote_override(true);   /* 先置位, 30s 内保护任务不覆盖(防覆盖窗口) */
        sys_data_set_relay(j->valueint != 0, dsg_on);
        ESP_LOGI(TAG, "prop relayCharge=%d", j->valueint);
    }
    j = cJSON_GetObjectItem(j_props, "relayDischarge");
    if (cJSON_IsNumber(j)) {
        bool chg_on = false, dsg_on = false;
        sys_data_get_relay(&chg_on, &dsg_on);
        sys_data_set_remote_override(true);   /* 先置位, 30s 内保护任务不覆盖(防覆盖窗口) */
        sys_data_set_relay(chg_on, j->valueint != 0);
        ESP_LOGI(TAG, "prop relayDischarge=%d", j->valueint);
    }

    /* ---- 保护阈值(持久化到 NVS) ---- */
    j = cJSON_GetObjectItem(j_props, "cellOvV");
    if (cJSON_IsNumber(j)) { params.cell_ov_prot_mv = (uint16_t)j->valueint; params_changed = true; }
    j = cJSON_GetObjectItem(j_props, "cellUvV");
    if (cJSON_IsNumber(j)) { params.cell_uv_prot_mv = (uint16_t)j->valueint; params_changed = true; }
    j = cJSON_GetObjectItem(j_props, "packOtT");
    if (cJSON_IsNumber(j)) { params.temp_ot_prot_dc = (int16_t)j->valueint; params_changed = true; }
    j = cJSON_GetObjectItem(j_props, "packUtT");
    if (cJSON_IsNumber(j)) { params.temp_ut_prot_dc = (int16_t)j->valueint; params_changed = true; }
    j = cJSON_GetObjectItem(j_props, "ocProtectI");
    if (cJSON_IsNumber(j)) {
        params.chg_oc_prot_ma = (int16_t)j->valueint;
        params.dsg_oc_prot_ma = (int16_t)j->valueint;
        params_changed = true;
    }
    j = cJSON_GetObjectItem(j_props, "balanceDeltaV");
    if (cJSON_IsNumber(j)) { params.balance_threshold_mv = (uint16_t)j->valueint; params_changed = true; }

    if (params_changed) {
        bms_err_t err = sys_params_set(&params);
        ESP_LOGI(TAG, "prop 阈值参数已保存 err=%d", (int)err);
    }

    send_prop_set_resp(request_id, 0, "success");
    cJSON_Delete(root);
}

/* ====== Bug7 修复: 处理设备消息(Dashboard 通过 /messages API 下发) ======
 * messages/down topic 收到的标准华为云包装(用户实测) 为 Format C:
 *   {"id":"38cfa3b8-...","name":null,
 *    "content": {"content":"{\"cmd\":\"set_charge\",\"paras\":{},\"timestamp\":1785917722039}"}}
 * 兼容: Format A / B / C / D (详见 unwrap_cmd_root 注释) */
static void handle_device_message(const char *data, int data_len)
{
    /* 复制到本地缓冲(确保 0 结尾) */
    char buf[640];
    int copy_len = data_len < (int)sizeof(buf) - 1 ? data_len : (int)sizeof(buf) - 1;
    memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    ESP_LOGI(TAG, "recv msg: %s", buf);

    /* ====== B3 幂等查漏(2026-09-15 高可靠加固) ======
     * Dashboard /messages 通道可下发 SET_xxx / RESTART 等高危命令,
     * 与主命令通道同等对待 — 提取 command_id/request_id 做防重放.
     * 无 ID 的旧格式消息跳过去重(兼容历史行为, 不阻塞既有链路). */
    {
        cJSON *pre = cJSON_Parse(buf);
        if (pre != NULL) {
            cJSON *j_rid = cJSON_GetObjectItem(pre, "command_id");
            if (!cJSON_IsString(j_rid)) {
                j_rid = cJSON_GetObjectItem(pre, "request_id");
            }
            if (cJSON_IsString(j_rid) && j_rid->valuestring[0] != '\0' &&
                cmd_replay_check(j_rid->valuestring)) {
                ESP_LOGW(TAG, "[鉴权] device message 重放(rid=%s 已执行), 丢弃",
                         j_rid->valuestring);
                cJSON_Delete(pre);
                return;
            }
            cJSON_Delete(pre);
        }
    }

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "设备消息 JSON 解析失败");
        return;
    }

    /* 自动解包 4 种嵌套格式, 找到真正的 {cmd, paras} 对象 */
    cJSON *cmd_root = NULL;
    bool need_delete_cmd_root = false;
    if (!unwrap_cmd_root(root, &cmd_root, &need_delete_cmd_root)) {
        ESP_LOGE(TAG, "设备消息缺少 cmd 字段(所有 4 种格式探测失败, 可 unwrap 深度不足?)");
        cJSON_Delete(root);
        return;
    }

    /* unwrap_cmd_root 已经保证 cmd_root 中 cmd 字段是字符串, 直接取即可 */
    const cJSON *j_cmd = cJSON_GetObjectItem(cmd_root, "cmd");
    const char *cmd = j_cmd->valuestring;
    const cJSON *paras = cJSON_GetObjectItem(cmd_root, "paras");

    ESP_LOGI(TAG, "[MSG] 命令: %s", cmd);

    /* 构造与 handle_command 兼容的 JSON: {command_name, paras, command_id} */
    cJSON *compat = cJSON_CreateObject();
    cJSON_AddStringToObject(compat, "command_name", cmd);
    if (paras != NULL) {
        /* cJSON_AddItemReferenceToObject 只添加引用不修改 item, 显式去掉 const 满足 API 签名 */
        cJSON_AddItemReferenceToObject(compat, "paras", (cJSON *)paras);
    }
    /* 生成一个伪 command_id 用于响应 */
    char fake_cmd_id[32];
    snprintf(fake_cmd_id, sizeof(fake_cmd_id), "msg_%lu", (unsigned long)(esp_timer_get_time() / 1000));
    cJSON_AddStringToObject(compat, "command_id", fake_cmd_id);

    /* 保存 command_id 用于响应 (B2: 写入端同样加锁, 与读取端 send_cmd_resp 互斥) */
    MQTT_STATE_LOCK();
    strncpy(s_last_command_id, fake_cmd_id, sizeof(s_last_command_id) - 1);
    s_last_command_id[sizeof(s_last_command_id) - 1] = '\0';
    MQTT_STATE_UNLOCK();

    /* 调用现有的命令处理逻辑 */
    /* 直接复用 handle_command 的命令分发 */
    if (strcmp(cmd, "set_param") == 0) {
        /* set_param 需要从 paras 提取 key/value(2026-08-10: 兼容 paras 内层/顶层平铺) */
        const cJSON *j_key = cmd_get_field(cmd_root, "key");
        const cJSON *j_value = cmd_get_field(cmd_root, "value");
        if (cJSON_IsString(j_key) && cJSON_IsNumber(j_value)) {
            /* 重新构造 set_param 兼容格式 */
            cJSON *set_param_root = cJSON_CreateObject();
            cJSON_AddStringToObject(set_param_root, "command_name", "set_param");
            cJSON_AddStringToObject(set_param_root, "key", j_key->valuestring);
            cJSON_AddNumberToObject(set_param_root, "value", j_value->valueint);
            handle_set_param(set_param_root);
            cJSON_Delete(set_param_root);
        } else {
            send_cmd_resp("set_param", false, NULL);
        }
    }
    else if (strcmp(cmd, "get_params") == 0) {
        handle_get_params();
    }
    else if (strcmp(cmd, "switch_battery") == 0) {
        handle_switch_battery(cmd_root);
    }
    else if (strcmp(cmd, "get_info") == 0) {
        handle_get_info();
    }
    else if (strcmp(cmd, "set_wifi") == 0) {
        const cJSON *j_ssid = cmd_get_field(cmd_root, "ssid");
        const cJSON *j_pass = cmd_get_field(cmd_root, "pass");
        if (cJSON_IsString(j_ssid) && cJSON_IsString(j_pass)) {
            bms_err_t err = sys_wifi_save_config(j_ssid->valuestring, j_pass->valuestring);
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "result", err == BMS_OK ? "saved, restart needed" : "save error");
            send_cmd_resp("set_wifi", err == BMS_OK, resp);
            cJSON_Delete(resp);
        } else {
            send_cmd_resp("set_wifi", false, NULL);
        }
    }
    /* 远程控制命令和其他命令交给上层回调处理 */
    else if (s_cmd_cb != NULL) {
        /* 把 paras 合并到 compat 的顶层(与 handle_command 兼容) */
        if (paras != NULL && cJSON_IsObject(paras)) {
            cJSON *child = paras->child;
            while (child != NULL) {
                cJSON_AddItemReferenceToObject(compat, child->string, child);
                child = child->next;
            }
        }
        s_cmd_cb(cmd, compat);
    } else {
        send_cmd_resp(cmd, false, NULL);
    }

    cJSON_Delete(compat);
    if (need_delete_cmd_root) cJSON_Delete(cmd_root);
    cJSON_Delete(root);
}

/* ====== 内部: 处理下行命令(华为云 IoTDA 格式) ====== */
/* ====== 内部: 处理产品模型命令 ======
 * 华为云命令下发:
 *   topic: $oc/devices/{id}/sys/commands/{request_id}
 *   data:  {"command_name":"SET_CHARGE","paras":{...}}
 * 兼容旧格式 payload 里的 command_id; 新格式 request_id 在 topic 末段
 * 命令名统一转大写比较(新物模型命令为大写, Dashboard /messages 为小写) */
static void handle_command(const char *data, int data_len, const char *topic_request_id)
{
    /* 复制到本地缓冲(确保 0 结尾) */
    char buf[512];
    int copy_len = data_len < (int)sizeof(buf) - 1 ? data_len : (int)sizeof(buf) - 1;
    memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    ESP_LOGI(TAG, "recv cmd: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        send_cmd_resp("parse", false, NULL);
        return;
    }

    /* 保存 command_id / request_id, 用于构造响应 topic:
     * 优先 payload.command_id(旧格式), 其次 topic 末段 request_id(新格式)
     * (B2: 写入端同样加锁, 与读取端 send_cmd_resp/handle_get_info 互斥) */
    const cJSON *j_cmd_id = cJSON_GetObjectItem(root, "command_id");
    MQTT_STATE_LOCK();
    if (cJSON_IsString(j_cmd_id) && j_cmd_id->valuestring[0] != '\0') {
        strncpy(s_last_command_id, j_cmd_id->valuestring, sizeof(s_last_command_id) - 1);
        s_last_command_id[sizeof(s_last_command_id) - 1] = '\0';
    } else if (topic_request_id != NULL && topic_request_id[0] != '\0') {
        strncpy(s_last_command_id, topic_request_id, sizeof(s_last_command_id) - 1);
        s_last_command_id[sizeof(s_last_command_id) - 1] = '\0';
    } else {
        s_last_command_id[0] = '\0';
    }
    MQTT_STATE_UNLOCK();

    const cJSON *j_cmd = cJSON_GetObjectItem(root, "command_name");
    if (!cJSON_IsString(j_cmd)) {
        send_cmd_resp("parse", false, NULL);
        cJSON_Delete(root);
        return;
    }

    /* 命令名统一转大写(兼容产品模型大写名与 Dashboard 小写名) */
    char cmd_upper[32] = {0};
    strncpy(cmd_upper, j_cmd->valuestring, sizeof(cmd_upper) - 1);
    for (char *p = cmd_upper; *p != '\0'; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p = (char)(*p - ('a' - 'A'));
        }
    }
    const char *cmd = cmd_upper;

    /* ====== 命令鉴权(2026-09-14 高可靠加固 A1) ======
     * 1) 防重放: 控制/高危类命令按 command_id 去重(QoS1 重投只执行一次);
     *    查询类(GET_INFO/GET_PARAMS)不去重 — 幂等只读, 重复响应无害
     * 2) token 校验: 鉴权开关打开时高危命令需 HMAC token(见 cmd_auth_verify)
     * 失败时仍回错误响应(复用 s_last_command_id), 便于发送方定位 */
    bool is_query = (strcmp(cmd, "GET_INFO") == 0 ||
                     strcmp(cmd, "GET_PARAMS") == 0);
    if (!is_query) {
        if (cmd_replay_check(s_last_command_id)) {
            ESP_LOGW(TAG, "[鉴权] 命令 %s 重放(command_id=%s 已执行), 丢弃",
                     cmd, s_last_command_id);
            send_cmd_resp(cmd, false, "replayed");
            cJSON_Delete(root);
            return;
        }
    }
    if (!cmd_auth_verify(cmd, s_last_command_id, root)) {
        send_cmd_resp(cmd, false, "auth_fail");
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd, "SET_PARAM") == 0) {
        handle_set_param(root);
    }
    else if (strcmp(cmd, "GET_PARAMS") == 0) {
        handle_get_params();
    }
    else if (strcmp(cmd, "SWITCH_BATTERY") == 0) {
        handle_switch_battery(root);
    }
    else if (strcmp(cmd, "GET_INFO") == 0) {
        handle_get_info();
    }
    else if (strcmp(cmd, "SET_WIFI") == 0) {
        const cJSON *j_ssid = cmd_get_field(root, "ssid");
        const cJSON *j_pass = cmd_get_field(root, "pass");
        if (cJSON_IsString(j_ssid) && cJSON_IsString(j_pass)) {
            bms_err_t err = sys_wifi_save_config(j_ssid->valuestring, j_pass->valuestring);
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "result", err == BMS_OK ? "saved, restart needed" : "save error");
            send_cmd_resp("set_wifi", err == BMS_OK, resp);
            cJSON_Delete(resp);
        } else {
            send_cmd_resp("set_wifi", false, NULL);
        }
    }
    /* ====== 远程控制命令(交给上层 app_tasks 回调统一处理) ======
     * SET_CHARGE:     {enable: bool}           控制充电回路
     * SET_DISCHARGE:  {enable: bool}           控制放电回路
     * SET_BALANCE:    {enable: bool, mask?: int} 控制均衡(mask=0xFFFF 表示自动)
     * SET_RELAY:      {charge: bool, discharge: bool} 同时控制两个回路
     * 这些命令的 paras 在 IoTDA 格式中位于 root 的 paras 子对象
     * (cmd_get_field 已兼容 paras 子对象与顶层平铺两种) */
    else if (strcmp(cmd, "SET_CHARGE")    == 0 ||
             strcmp(cmd, "SET_DISCHARGE") == 0 ||
             strcmp(cmd, "SET_BALANCE")   == 0 ||
             strcmp(cmd, "SET_RELAY")     == 0) {
        if (s_cmd_cb != NULL) {
            s_cmd_cb(cmd, root);
        } else {
            send_cmd_resp(cmd, false, NULL);
        }
    }
    else {
        /* 其他命令(CLEAR_ALARM/RESTART/OTA_CHECK/OTA_UPGRADE/RESET_PARAMS)交给上层回调处理 */
        if (s_cmd_cb != NULL) {
            s_cmd_cb(cmd, root);
        } else {
            send_cmd_resp(cmd, false, NULL);
        }
    }

    cJSON_Delete(root);
}

/* ====== MQTT 事件回调 ======
 * 增加重连计数与 DNS 就绪检查, 解决 WiFi 刚连上 DNS 未就绪的问题 */
static int s_retry_count = 0;     // 已重连次数
static const int MAX_RETRY = 5;   // 最大重连次数(超出则等下一周期)
/* 2026-08-06 修复: MQTT 认证失败时销毁并重建客户端
 * 原因: 自动重连失败时 ESP-IDF 用旧的连接参数重试, 若 EMQX 侧瞬时鉴权异常
 *       会反复失败. 检测到 mqtt_ret=4(认证失败)时设置标志, 由主循环 destroy+init
 *       使用 NVS 中的静态账号重新连接 (2026-08-12 迁移 EMQX 后保留此机制) */
static volatile bool s_need_reauth = false;

/* ====== 内部: 等待 DNS 就绪 ======
 * WiFi 连接后 DNS 服务器需要数百毫秒才能响应, 直接 MQTT 会 getaddrinfo 失败 */
static bool wait_dns_ready(const char *hostname, int timeout_ms)
{
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    for (int i = 0; i < timeout_ms / 100; i++) {
        int ret = getaddrinfo(hostname, NULL, &hints, &res);
        if (ret == 0 && res != NULL) {
            freeaddrinfo(res);
            return true;
        }
        if (res) freeaddrinfo(res);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        MQTT_STATE_LOCK(); s_connected = true; MQTT_STATE_UNLOCK();
        s_retry_count = 0;                              // 连接成功, 重置重连计数
        ESP_LOGI(TAG, "MQTT 已连接");

        /* 订阅下行命令主题(产品模型命令) */
        char cmd_topic[128];
        build_iotda_topic(cmd_topic, sizeof(cmd_topic), "commands/#");
        esp_mqtt_client_subscribe(s_client, cmd_topic, BMS_MQTT_QOS);
        ESP_LOGI(TAG, "已订阅 %s", cmd_topic);

        /* Bug7 修复: 订阅设备消息主题(不依赖产品模型, Dashboard 通过 /messages API 下发) */
        char msg_topic[128];
        build_iotda_topic(msg_topic, sizeof(msg_topic), "messages/down");
        esp_mqtt_client_subscribe(s_client, msg_topic, BMS_MQTT_QOS);
        ESP_LOGI(TAG, "已订阅 %s", msg_topic);

        /* BMS-V2 物模型: 订阅属性设置主题(RW 属性下发, 如 chargeEnable/阈值调整) */
        char set_topic[128];
        build_iotda_topic(set_topic, sizeof(set_topic), "properties/set/#");
        esp_mqtt_client_subscribe(s_client, set_topic, BMS_MQTT_QOS);
        ESP_LOGI(TAG, "已订阅 %s", set_topic);

        /* 补传离线缓存数据(MQTT 断开期间缓存的数据)
         * H22 修复: 原此处直接 sys_offline_cache_replay() 同步全量补传,
         * 阻塞 MQTT 事件线程数十秒 → keepalive 无法发送 → 服务端超时踢下线.
         * 现仅置标志, 由 task_comm 主循环 sys_mqtt_process_pending_replay() 分批补传. */
        int cached = sys_offline_cache_count();
        if (cached > 0) {
            ESP_LOGI(TAG, "检测到 %d 条离线缓存, 置补传标志(由主循环分批补传)", cached);
            s_replay_pending = true;
        }

        /* 上报设备上线信息 */
        handle_get_info();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        MQTT_STATE_LOCK(); s_connected = false; MQTT_STATE_UNLOCK();
        ESP_LOGW(TAG, "MQTT 断开 (重连 %d/%d)", s_retry_count, MAX_RETRY);
        break;
    case MQTT_EVENT_DATA:
        /* 收到订阅主题的消息 */
        if (event->topic && event->data) {
            /* 确保 topic 0 结尾 */
            char topic[160];
            int tlen = event->topic_len < (int)sizeof(topic) - 1 ? event->topic_len : (int)sizeof(topic) - 1;
            memcpy(topic, event->topic, tlen);
            topic[tlen] = '\0';

            ESP_LOGI(TAG, "MQTT msg: topic=%s len=%d", topic, event->data_len);

            /* Bug7 修复: 判断是 commands 还是 messages/down
             * commands topic: $oc/devices/{id}/sys/commands/{request_id}
             *   data 格式: {command_name, paras, command_id}
             * messages/down topic: $oc/devices/{id}/sys/messages/down
             *   data 格式: {content: "{cmd, paras, timestamp}"} 或直接 {cmd, paras}
             * properties/set topic: $oc/devices/{id}/sys/properties/set/{request_id}
             *   data 格式: {services:[{service_id, properties:{...}}]} */
            if (strstr(topic, "/properties/set/") != NULL) {
                /* BMS-V2: RW 属性下发(chargeEnable/阈值等), 需回复响应 */
                const char *rid = strrchr(topic, '/');
                handle_property_set(event->data, event->data_len,
                                    (rid != NULL) ? rid + 1 : "");
            } else if (strstr(topic, "/messages/down") != NULL) {
                /* 设备消息: Dashboard 通过 /messages API 下发的 JSON */
                handle_device_message(event->data, event->data_len);
            } else {
                /* 产品模型命令: topic 末段是 request_id(新格式), 传入用于构造响应 */
                const char *rid = strrchr(topic, '/');
                handle_command(event->data, event->data_len,
                               (rid != NULL) ? rid + 1 : NULL);
            }
        }
        break;
    case MQTT_EVENT_ERROR: {
        /* ====== 增强错误日志: 打印具体错误类型和TLS错误码 ======
         * 解决之前 "连不上" 但不知道失败原因的问题
         * 常见错误码:
         *   - MQTT_CONNECTION_REFUSED (0x01): 协议版本错误
         *   - MQTT_CONNECTION_REFUSE_BAD_USERNAME (0x04): 用户名/密码错误
         *   - MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED (0x05): 未授权/HMAC失败
         *   - error_type = MQTT_ERROR_TYPE_TCP_TRANSPORT: TCP/TLS连接失败(网络/证书)
         *   - error_type = MQTT_ERROR_TYPE_CONNECTION_REFUSED: MQTT层被拒绝 */
        esp_mqtt_error_codes_t *err = event->error_handle;
        if (err != NULL) {
            ESP_LOGE(TAG, "MQTT 错误(重连 %d/%d): err_type=%d, mqtt_ret=%d, tls_err=%d, sock_errno=%d, tls_flags=0x%X",
                     s_retry_count, MAX_RETRY,
                     (int)err->error_type,
                     (int)err->connect_return_code,      /* ESP-IDF v5.2: connect_return_code */
                     (int)err->esp_tls_last_esp_err,
                     (int)err->esp_transport_sock_errno,
                     (unsigned)err->esp_tls_stack_err);
            /* TIPS:
             * mqtt_ret = 4/5 → 用户名/密码/HMAC鉴权失败 或 时间戳超差>5min
             * tls_err != 0 / tls_flags ≠ 0 → CA证书验证失败(证书链不包含目标根证)
             * sock_errno = 113/118 → 主机不可达/连接超时(网络问题)
             * error_type = 0x100 (MQTT_ERROR_TYPE_NONE) */
            /* 认证失败(mqtt_ret=4/5)时, 标记需要重建客户端重连
             * (2026-08-12 EMQX 静态账号场景: 无时间戳过期问题, 保留机制应对瞬时抖动) */
            if (err->connect_return_code == 4 || err->connect_return_code == 5) {
                s_need_reauth = true;
                ESP_LOGW(TAG, "MQTT 认证失败(mqtt_ret=%d), 将在主循环中重新生成凭证并重连",
                         (int)err->connect_return_code);
            }
        } else {
            ESP_LOGE(TAG, "MQTT 错误(重连 %d/%d): 无错误句柄", s_retry_count, MAX_RETRY);
        }
        s_retry_count++;
        break;
    }
    default:
        break;
    }
}

/* ================================================================
 * 2026-08-16 双通道架构: 自建 Mosquitto 第二通道 (仅上报, 不收命令)
 *   - 与华为云主通道并行: 同一份属性 JSON 双发 → bms/<device_id>/telemetry
 *   - 命令/回执仍走华为云主通道(第二通道不订阅, 避免双端命令竞争)
 *   - 静态用户名/密码鉴权(非 HMAC), 断线由 ESP-IDF 自动重连
 * ================================================================ */
static void mqtt2_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        MQTT_STATE_LOCK(); s_connected2 = true; MQTT_STATE_UNLOCK();
        ESP_LOGI(TAG, "MQTT2(自建) 已连接");
        /* 2026-08-21 EMQX 主通道命令下行: 订阅 bms/<device_id>/cmd,
         * 网页命令经 EMQX 直达设备(实时, 不依赖华为云 /messages).
         * 与后端 local_mqtt.publish_cmd 的 cmd_topic(bms/cmd) 对应. */
        {
            const bms_params_t *p = sys_params_get();
            if (p != NULL && p->mqtt2_client_id[0] != '\0') {
                char cmd_topic[160];
                snprintf(cmd_topic, sizeof(cmd_topic), "%s%s/cmd",
                         p->mqtt2_topic_prefix, p->mqtt2_client_id);
                esp_mqtt_client_subscribe(s_client2, cmd_topic, BMS_MQTT_QOS);
                ESP_LOGI(TAG, "MQTT2 已订阅命令主题 %s", cmd_topic);
            }
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        MQTT_STATE_LOCK(); s_connected2 = false; MQTT_STATE_UNLOCK();
        ESP_LOGW(TAG, "MQTT2(自建) 断开");
        break;
    case MQTT_EVENT_DATA:
        /* 2026-08-21 EMQX 主通道命令处理: 复用 handle_device_message
         * (支持裸 {cmd,paras} 及华为云 messages/down 多层包装, 统一分发) */
        if (event != NULL && event->data != NULL && event->data_len > 0) {
            char topic[128];
            int tl = (event->topic_len > 0 && event->topic_len < (int)sizeof(topic) - 1)
                     ? event->topic_len : 0;
            if (tl > 0) {
                memcpy(topic, event->topic, tl);
                topic[tl] = '\0';
                if (strstr(topic, "/cmd") != NULL || strstr(topic, "/cmd/") != NULL) {
                    handle_device_message(event->data, event->data_len);
                }
            }
        }
        break;
    default:
        break;
    }
}

/* 第二通道发布: 把同一份属性 JSON 发到 bms/<device_id>/telemetry */
static bool publish_json2(const char *json)
{
    const bms_params_t *p = sys_params_get();
    if (p == NULL || p->mqtt2_client_id[0] == '\0' || json == NULL) {
        return false;
    }
    char topic[160];
    snprintf(topic, sizeof(topic), "%s%s/telemetry",
             p->mqtt2_topic_prefix, p->mqtt2_client_id);
    MQTT_STATE_LOCK();
    bool published = false;
    if (s_connected2 && s_client2 != NULL) {
        esp_mqtt_client_publish(s_client2, topic, json, 0, BMS_MQTT_QOS, 0);
        published = true;
    }
    MQTT_STATE_UNLOCK();
    return published;
}

/* ================================================================
 * 2026-08-18 双速率架构: EMQX 主通道高频精简帧(100ms)
 *   - 主题 bms/<device_id>/fast, QoS 0 尽力而为(100ms 一帧, 丢一帧无感)
 *   - 字段精简: 只含前端高频曲线所需核心量(soc/pack_v/current/temp/fault),
 *     不含 cell 数组/CRC/RS485 等低频全量帧字段(仍由 7s 华为云帧补全)
 *   - 华为云备用通道(7s)与 EMQX 全量帧(7s)保持原 sys_mqtt_report 路径
 * ================================================================ */
static bool publish_json2_fast(const char *json)
{
    const bms_params_t *p = sys_params_get();
    if (p == NULL || p->mqtt2_client_id[0] == '\0' || json == NULL) {
        return false;
    }
    char topic[160];
    snprintf(topic, sizeof(topic), "%s%s/fast",
             p->mqtt2_topic_prefix, p->mqtt2_client_id);
    MQTT_STATE_LOCK();
    bool published = false;
    if (s_connected2 && s_client2 != NULL) {
        /* QoS 0: 100ms 高频尽力而为, 避免 QoS1 ACK 堆积 */
        esp_mqtt_client_publish(s_client2, topic, json, 0, 0, 0);
        published = true;
    }
    MQTT_STATE_UNLOCK();
    return published;
}

/* 高频精简帧(100ms): 仅发 EMQX 主通道, 与 7s 全量帧独立 */
void sys_mqtt_report_fast(const bms_pack_data_t *pack,
                          const bms_soc_data_t  *soc,
                          bms_fault_mask_t       fault)
{
    if (pack == NULL || soc == NULL) {
        return;
    }
    /* 第二通道未就绪: 静默丢弃(高频帧不缓存, 华为云 7s 帧已兜底) */
    MQTT_STATE_LOCK();
    bool ready = (s_connected2 && s_client2 != NULL);
    MQTT_STATE_UNLOCK();
    if (!ready) {
        return;
    }

    /* 真实 UTC 毫秒时间戳(与全量帧同一来源, 未同步时回退 boot tick) */
    unsigned long long ts_ms;
    if (s_time_synced) {
        ts_ms = (unsigned long long)time(NULL) * 1000ULL;
    } else {
        ts_ms = (unsigned long long)pack->timestamp_ms;
    }

    bool chg_on = false, dsg_on = false;
    sys_data_get_relay(&chg_on, &dsg_on);
    bms_balance_mask_t bmask = sys_data_get_balance_mask();

    char json[384];
    int n = snprintf(json, sizeof(json),
        "{\"soc\":%.1f,\"soh\":%.1f,\"pack_v\":%lu,\"current\":%d,"
        "\"temp_max\":%d,\"temp_min\":%d,\"v_max\":%u,\"v_min\":%u,"
        "\"fault\":%u,\"charge_mos\":%d,\"discharge_mos\":%d,"
        "\"balance_on\":%d,\"timestamp\":%llu}",
        soc->soc * 100.0f, soc->soh * 100.0f,
        (unsigned long)pack->pack_mv, pack->current_ma,
        pack->temp_max_dc, pack->temp_min_dc,
        pack->cell_mv_max, pack->cell_mv_min,
        (unsigned)fault, chg_on ? 1 : 0, dsg_on ? 1 : 0,
        (bmask != 0) ? 1 : 0, ts_ms);
    if (n > 0 && n < (int)sizeof(json)) {
        publish_json2_fast(json);
    }
}

/* ================================================================ */
bms_err_t sys_mqtt_init(void)
{
    /* B1 修复: 创建共享状态互斥锁(仅一次) */
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
    if (s_client != NULL) {
        return BMS_OK;                                  // 已初始化
    }

    s_boot_time_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* 初始化离线缓存(SPIFFS 挂载) */
    sys_offline_cache_init();

    const bms_params_t *p = sys_params_get();

    /* ====== SNTP 时间同步(保留: 日志时间戳 + IoTDA ClientID 时间戳生成) ====== */
    sync_time_via_sntp();

    /* 生成华为云 IoTDA 标准鉴权参数(2026-08-12 恢复):
     *   ClientID = {device_id}_0_0_{YYYYMMDDHH}   (身份类型0, 签名类型0=不校验时间戳, 时间戳到小时UTC)
     *   Username = {device_id}                    (即 mqtt_client_id)
     *   Password = HMAC-SHA256(key=timestamp, msg=device_secret) → 64字符hex小写
     *   其中 key=时间戳(YYYYMMDDHH), msg=设备密钥, 与 ClientID 中时间戳一致
     *   (官方示例验证: secret=12345678, ts=2025041401 → c75150e6...)
     * 签名类型0下平台仅校验密码, 不校验时间精确性 → 对 SNTP 时间抖动不敏感, 最稳。
     * 接入点(mqtt_uri)由配网页填写, 形如 mqtts://{实例ID}.iotda-app.cn-south-4.myhuaweicloud.com:8883 */
    char client_id[128];
    char password[65];
    password[0] = '\0';

    /* UTC 小时级时间戳 YYYYMMDDHH */
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    /* clamp 时间字段到实际合理范围, 让编译器可证明 snprintf 不截断
     * (否则 -Werror=format-truncation 会因 tm 字段理论全范围而报错) */
    int ts_year = tm_utc.tm_year + 1900;
    int ts_mon  = tm_utc.tm_mon + 1;
    int ts_day  = tm_utc.tm_mday;
    int ts_hour = tm_utc.tm_hour;
    if (ts_year < 2000) ts_year = 2000; else if (ts_year > 9999) ts_year = 9999;
    if (ts_mon  < 1)    ts_mon  = 1;    else if (ts_mon  > 12)   ts_mon  = 12;
    if (ts_day  < 1)    ts_day  = 1;    else if (ts_day  > 31)   ts_day  = 31;
    if (ts_hour < 0)    ts_hour = 0;    else if (ts_hour > 23)   ts_hour = 23;
    char ts[16];
    snprintf(ts, sizeof(ts), "%04d%02d%02d%02d",
             ts_year, ts_mon, ts_day, ts_hour);

    snprintf(client_id, sizeof(client_id), "%s_0_0_%s", p->mqtt_client_id, ts);

    /* HMAC-SHA256(key=ts, msg=device_secret) → 64字符hex小写
     * 官方规范: 以时间戳为密钥、设备密钥为内容, Password = hmacsha256("时间戳", "secret")
     * 已用华为官方示例(secret=12345678, ts=2025041401 → c75150e6...)验证参数顺序 */
    if (p->mqtt_device_secret[0] != '\0'
            && strcmp(p->mqtt_device_secret, "emqx-no-secret") != 0) {
        unsigned char digest[32];
        const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (md && mbedtls_md_hmac(md,
                (const unsigned char *)ts, strlen(ts),              /* key = 时间戳 */
                (const unsigned char *)p->mqtt_device_secret,
                strlen(p->mqtt_device_secret), digest) == 0) {      /* msg = 设备密钥 */
            for (int i = 0; i < 32; i++) {
                snprintf(password + 2 * i, 3, "%02x", digest[i]);
            }
        } else {
            ESP_LOGE(TAG, "HMAC-SHA256 计算失败, device_secret 可能无效");
        }
    } else {
        ESP_LOGE(TAG, "mqtt_device_secret 未配置(仍是默认 emqx-no-secret), IoTDA 鉴权必失败! 请在配网页填写真实密钥");
    }

    ESP_LOGI(TAG, "IoTDA ClientID=%s User=%s PassLen=%d",
             client_id, p->mqtt_client_id, (int)strlen(password));

    /* ---- DNS 就绪检查 + Broker 地址解析(合并, 避免重复定义) ----
     * 1. 从 URI 中分离 scheme/host/port/transport (解决 URI+port 冲突问题)
     * 2. 等待 DNS 解析就绪(解决 WiFi 刚连上 DNS 未就绪问题) */
    const char *uri_raw = p->mqtt_uri;
    char hostname[128] = {0};
    int  port = 1883;
    bool is_tls = false;
    const char *uri = uri_raw;      /* 去掉 scheme 后的指针 */

    if      (strncmp(uri, "mqtts://", 8) == 0) { is_tls = true;  uri += 8; port = 8883; }
    else if (strncmp(uri, "mqtt://",  7) == 0) { is_tls = false; uri += 7; port = 1883; }
    else if (strncmp(uri, "wss://",   6) == 0) { is_tls = true;  uri += 6; port = 443;  }
    else if (strncmp(uri, "ws://",    5) == 0) { is_tls = false; uri += 5; port = 80;   }

    /* 提取 host 和可选的 :port */
    const char *colon = strchr(uri, ':');
    const char *slash = strchr(uri, '/');
    size_t host_len;
    if (colon != NULL && (slash == NULL || colon < slash)) {
        host_len = (size_t)(colon - uri);
        port = atoi(colon + 1);       /* URI 中显式的端口号覆盖默认值 */
    } else if (slash != NULL) {
        host_len = (size_t)(slash - uri);
    } else {
        host_len = strlen(uri);
    }
    if (host_len >= sizeof(hostname)) host_len = sizeof(hostname) - 1;
    memcpy(hostname, uri, host_len);
    hostname[host_len] = '\0';

    ESP_LOGI(TAG, "解析 Broker: host=%s port=%d tls=%d (原始URI: %s)",
             hostname, port, is_tls, uri_raw);

    /* DNS 就绪检查 */
    if (hostname[0] != '\0') {
        ESP_LOGI(TAG, "等待 DNS 解析: %s", hostname);
        bool dns_ok = wait_dns_ready(hostname, 3000);   /* 最多等 3 秒 */
        if (!dns_ok) {
            ESP_LOGW(TAG, "DNS 解析超时, MQTT 可能连接失败(将自动重连)");
        } else {
            ESP_LOGI(TAG, "DNS 解析成功: %s", hostname);
        }
    }

    /* ====== ALPN 协议标识 (MQTTS over TLS 推荐配置) ======
     * 华为云 IoTDA 等云平台通过 ALPN 识别 MQTT 流量,
     * 缺少此标识可能导致 TLS 握手后被云平台断开 */
    static const char *alpn_mqtt[] = { "x-amzn-mqtt-ca", "mqtt", NULL };
    (void)alpn_mqtt;   /* 防止未使用警告 */

    /* ====== ESP-IDF v5.2 要求显式初始化所有嵌套子结构体 ======
     * 否则 -Werror=missing-braces 会报错. 因此:
     *   .last_will / .task / .outbox 等未使用的子结构都显式 {0} 初始化 */
    esp_mqtt_client_config_t cfg = {
        /* ---- Broker 地址: 三字段分离指定, 避免 URI+port 重复解析 ---- */
        .broker = {
            .address = {
                .hostname   = hostname,          /* 纯主机名, 不含 scheme 和 port */
                .port       = (uint32_t)port,    /* 单独指定端口: 1883/8883 */
                .transport  = is_tls ? MQTT_TRANSPORT_OVER_SSL
                                     : MQTT_TRANSPORT_OVER_TCP,
            },
            /* ---- TLS 验证配置 ---- */
            .verification = {
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
                .crt_bundle_attach            = esp_crt_bundle_attach,  /* ESP-IDF 内置 200+ CA */
#endif
                .skip_cert_common_name_check  = true,                   /* 跳过 CN 校验, 防 SNI 问题 */
#ifdef CONFIG_MBEDTLS_SSL_ALPN
                .alpn_protos                  = (const char **)alpn_mqtt,  /* ALPN: broker.verification 下 */
#endif
            },
        },
        /* ---- 客户端身份认证 (华为云 IoTDA 平台标准鉴权) ---- */
        .credentials = {
            .username           = p->mqtt_client_id,         /* IoTDA: 设备ID */
            .client_id          = client_id,                  /* IoTDA: {device_id}_0_0_{ts} */
            .set_null_client_id = false,
            .authentication = {
                .password = password,                         /* IoTDA: HMAC-SHA256(device_secret, ts) */
            },
        },
        /* ---- 会话 ---- */
        .session = {
            .last_will              = {0},                    /* 未使用 LWT, 显式 0 初始化 */
            .disable_clean_session  = false,
            .keepalive              = BMS_MQTT_KEEPALIVE_SEC,
        },
        /* ---- 网络 ----
         * 保留自动重连(处理网络断开/WiFi闪断),
         * 认证失败(mqtt_ret=4)时由 sys_mqtt_check_reauth() 销毁并重建客户端 */
        .network = {
            .disable_auto_reconnect = false,
            .reconnect_timeout_ms   = 5000,
        },
        /* ---- 任务 (2026-08-17 修复: 默认 6KB 栈在 wss+TLS+缓存补传时栈溢出,
         * 导致 mqtt_task stack overflow 周期重启(上线下线), 增大到 10KB) ---- */
        .task = {
            .stack_size = 10 * 1024,
        },
        /* ---- 缓冲区 ---- */
        .buffer = {
            .size       = 2048,
            .out_size   = 2048,
        },
        /* ---- 发件箱 (必须显式初始化) ---- */
        .outbox = {0},
    };

    ESP_LOGI(TAG, "MQTT 配置: transport=%s host=%s port=%d client_id=%s",
             is_tls ? "SSL/TLS" : "TCP", hostname, port, client_id);

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "MQTT client init fail");
        return BMS_ERR_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    ESP_LOGI(TAG, "MQTT 初始化, broker=%s", p->mqtt_uri);

    /* ====== 2026-08-16 双通道架构: 第二通道 = 自建 Mosquitto (仅上报) ======
     * 同一份属性 JSON 双发到 bms/<device_id>/telemetry, 供 App/后端实时消费;
     * 静态用户名/密码鉴权(非 HMAC), 命令仍走华为云主通道 */
    if (p->mqtt2_uri[0] != '\0' && p->mqtt2_client_id[0] != '\0') {
        const char *uri2 = p->mqtt2_uri;
        char host2[128] = {0};
        int  port2 = 1883;
        bool tls2 = false;
        bool ws2  = false;
        /* 2026-08-18 通道加密强制(双速率架构安全要求):
         *   拒绝明文 ws:// 与 mqtt:// 配置(NVS 残留/配网页误填), 一律回退默认 wss 隧道域名,
         *   杜绝设备以明文直连 ECS:8083(公网明文可被嗅探). 加密判定仅认 wss:// 或 mqtts:// */
        if (strncmp(uri2, "ws://", 5) == 0 || strncmp(uri2, "mqtt://", 7) == 0) {
            ESP_LOGE(TAG, "MQTT2 配置为明文通道(%s), 违反加密要求, 强制回退默认 wss 隧道 %s",
                     uri2, BMS_MQTT2_URI);
            uri2 = BMS_MQTT2_URI;
        }
        /* 2026-08-16 支持 wss:// (经 cloudflared HTTPS 隧道, 免费版隧道不支持 TCP 直连):
         *   wss://host[:port]/mqtt  →  WebSocket over TLS, 端口默认 443 */
        if      (strncmp(uri2, "wss://",  6) == 0) { tls2 = true;  ws2 = true;  uri2 += 6; port2 = 443; }
        else if (strncmp(uri2, "mqtts://", 8) == 0) { tls2 = true;  ws2 = false; uri2 += 8; port2 = 8883; }
        else if (strncmp(uri2, "mqtt://",  7) == 0) { tls2 = false; ws2 = false; uri2 += 7; port2 = 1883; }
        const char *colon2 = strchr(uri2, ':');
        const char *slash2 = strchr(uri2, '/');
        size_t host_len2;
        if (colon2 != NULL && (slash2 == NULL || colon2 < slash2)) {
            host_len2 = (size_t)(colon2 - uri2);
            port2 = atoi(colon2 + 1);
        } else if (slash2 != NULL) {
            host_len2 = (size_t)(slash2 - uri2);
        } else {
            host_len2 = strlen(uri2);
        }
        if (host_len2 >= sizeof(host2)) host_len2 = sizeof(host2) - 1;
        memcpy(host2, uri2, host_len2);
        host2[host_len2] = '\0';

        /* 2026-08-16 wss: 提取 WebSocket 路径(如 /mqtt), EMQX ws 监听器挂载在 /mqtt,
         * 缺路径连接会被拒绝; 仅 wss/ws 使用 */
        char path2[64] = {0};
        if (ws2 && slash2 != NULL) {
            strncpy(path2, slash2, sizeof(path2) - 1);
        }

        if (host2[0] != '\0') {
            /* 2026-08-20 修复(#2 断电第一时间离线): EMQX 通道配置 LWT 遗嘱——
             * 设备断电/断网时 WiFi 链路层断开, TCP 无 FIN/RST, EMQX 只能靠 keepalive
             * 超时发现断线(原 60s 太久); 配置遗嘱后 broker 一检测到断开立即发布
             * bms/<device_id>/status 离线消息, 消费腿收到即把前端转离线并推企微.
             * 同时 EMQX 腿 keepalive 60s→15s, 把发现时间压到 ~15-30s;
             * 华为云腿保持 60s(IoTDA 平台兼容, 且由桥接进程轮询兜底). */
            static char s_will_topic[128] = {0};
            static char s_will_msg[64]    = {0};
            snprintf(s_will_topic, sizeof(s_will_topic), "%s%s/status",
                     p->mqtt2_topic_prefix, p->mqtt2_client_id);
            snprintf(s_will_msg, sizeof(s_will_msg),
                     "{\"device_online\":false,\"no_data\":true,\"lwt\":true}");
            esp_mqtt_client_config_t cfg2 = {
                .broker = {
                    .address = {
                        .hostname   = host2,
                        .port       = (uint32_t)port2,
                        /* wss 走 WebSocket over TLS; 否则 mqtts/mqtt 用 SSL/TCP */
                        .transport  = ws2 ? MQTT_TRANSPORT_OVER_WSS
                                          : (tls2 ? MQTT_TRANSPORT_OVER_SSL
                                                  : MQTT_TRANSPORT_OVER_TCP),
                        /* wss/ws 必须带 WebSocket 路径(EMQX 挂载 /mqtt) */
                        .path       = path2[0] ? path2 : NULL,
                    },
                    .verification = {
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
                        /* 2026-08-16: cloudflared 证书链信任锚为 GlobalSign Root CA(交叉签发 GTS Root R4)。
                         * bundle 验证在设备上不可靠(仍报 -0x7780), 改用自定义 CA 精确校验 */
                        .crt_bundle_attach           = NULL,
#endif
                        /* 自定义 CA: GTS Root R4(交叉) + GlobalSign Root CA(仅 wss 使用)
                         * 2026-08-17: cloudflared 边缘可能只发 2 级链(leaf+WE1), 仅信任 GlobalSign 根
                         * 会验证失败(-0x7780); 加入 GTS R4 交叉签名证书后 2/3 级链均可验证。
                         * 注意: certificate_len 必须留 0 —— v5.2.7 定义了 MQTT_SUPPORTED_FEATURE_DER_CERTIFICATES,
                         * len>0 时 esp-mqtt 把证书当 DER 解析, PEM 会直接 INVALID_FORMAT(-0x2180);
                         * len=0 时 esp-mqtt 走 strlen + PEM 分支 */
                        .certificate                 = ws2 ? mqtt2_ca_bundle_pem : NULL,
                        .certificate_len             = 0,
                        /* 2026-08-17 关键修复: 必须 false 让 esp-tls 发 SNI。
                         * skip=true 时 set_client_config 不调 mbedtls_ssl_set_hostname,
                         * ClientHello 无 SNI → Cloudflare 边缘回 fatal alert 40 (handshake_failure)
                         * → -0x7780. 证书 SAN 覆盖 *.bms0605.dpdns.org, CN 校验可通过 */
                        .skip_cert_common_name_check = false,
                    },
                },
                .credentials = {
                    .username           = p->mqtt2_user,
                    .client_id          = p->mqtt2_client_id,
                    .set_null_client_id = false,
                    .authentication = {
                        .password        = p->mqtt2_pass,
                        /* 2026-08-16 mTLS: 双向证书(broker require_certificate,
                         * use_identity_as_username 以证书 CN=bms01 作为身份)
                         * ESP-IDF v5.2 字段: certificate/certificate_len + key/key_len */
                        .certificate     = mqtt2_client_cert_pem,
                        .certificate_len = sizeof(mqtt2_client_cert_pem) - 1,
                        .key             = mqtt2_client_key_pem,
                        .key_len         = sizeof(mqtt2_client_key_pem) - 1,
                    },
                },
                .session = {
                    /* 2026-08-20 修复(#2): LWT 遗嘱——断电时 EMQX 立即发离线状态,
                     * 前端秒级转离线; retain 让新订阅者/刷新页面也能读到离线状态 */
                    .last_will = {
                        .topic    = s_will_topic,
                        .msg      = s_will_msg,
                        .msg_len  = (int)strlen(s_will_msg),
                        .qos      = 1,
                        .retain   = true,
                    },
                    .disable_clean_session  = false,
                    .keepalive              = 15,   /* 2026-08-20: 60s→15s, 断电快速发现 */
                },
                .network = {
                    .disable_auto_reconnect = false,
                    .reconnect_timeout_ms   = 5000,
                },
                /* 2026-08-17 修复: 同 MQTT1, wss+TLS 加密路径栈消耗大, 默认 6KB 会栈溢出 */
                .task = {
                    .stack_size = 10 * 1024,
                },
                .buffer = { .size = 2048, .out_size = 2048 },
                .outbox = {0},
            };
            /* wss(经 cloudflared 隧道)不需要 mTLS 客户端证书——隧道终止 TLS, 后端 EMQX 为
             * ws 明文 + 用户名/密码鉴权; 保留 mqtts 直连的 mTLS 行为不变 */
            if (ws2) {
                cfg2.credentials.authentication.certificate     = NULL;
                cfg2.credentials.authentication.certificate_len = 0;
                cfg2.credentials.authentication.key             = NULL;
                cfg2.credentials.authentication.key_len         = 0;
            }
            s_client2 = esp_mqtt_client_init(&cfg2);
            if (s_client2 != NULL) {
                esp_mqtt_client_register_event(s_client2, ESP_EVENT_ANY_ID, mqtt2_event_handler, NULL);
                esp_mqtt_client_start(s_client2);
                ESP_LOGI(TAG, "MQTT2(自建) 初始化, broker=%s:%d tls=%d (双通道)", host2, port2, tls2);
            } else {
                ESP_LOGW(TAG, "MQTT2(自建) client init fail, 仅华为云单通道");
            }
        }
    } else {
        ESP_LOGI(TAG, "MQTT2(自建) 未配置, 保持华为云单通道");
    }

    return BMS_OK;
}

/* ================================================================ */
/* 2026-08-06: 认证失败后重新连接 (EMQX 静态账号场景保留, 应对瞬时鉴权抖动)
 * 由主循环定期调用, 检测 s_need_reauth 标志.
 * 步骤: stop → destroy → s_client=NULL → init(使用 NVS 中的静态账号) */
bms_err_t sys_mqtt_check_reauth(void)
{
    if (!s_need_reauth) {
        return BMS_OK;                   /* 无需重新认证 */
    }
    s_need_reauth = false;               /* 清除标志 */

    ESP_LOGW(TAG, "MQTT 认证失败, 重新连接...");

    if (s_client != NULL) {
        /* 2026-08-08 修复: 用异步 disconnect 替代同步 stop.
         *   原因: esp_mqtt_client_stop() 会等待内部任务/网络清理结束,
         *         网络不可达(TLS select 超时)时可能阻塞数秒~十几秒,
         *         而本函数由 task_comm 主循环调用, 会导致该任务饿死看门狗复位.
         *         disconnect 为异步发起, 立即返回, 不阻塞调用方. */
        esp_mqtt_client_disconnect(s_client);
        vTaskDelay(pdMS_TO_TICKS(200));  /* 短暂等待断开动作发出 */
        /* B1 修复: 锁内置空 s_client 后于锁外用本地副本销毁,
         * 杜绝并发 publish 线程(上报/命令响应)读取已销毁句柄的 use-after-free */
        MQTT_STATE_LOCK();
        esp_mqtt_client_handle_t old_client = s_client;
        s_client = NULL;
        s_connected = false;
        MQTT_STATE_UNLOCK();
        if (old_client != NULL) {
            esp_mqtt_client_destroy(old_client);
            vTaskDelay(pdMS_TO_TICKS(500));  /* 等资源释放 */
        }
    }

    /* 重新初始化(使用 NVS 中的静态账号重连) */
    bms_err_t ret = sys_mqtt_init();
    if (ret == BMS_OK) {
        ESP_LOGI(TAG, "MQTT 重新认证成功");
    } else {
        ESP_LOGE(TAG, "MQTT 重新认证失败 ret=%d", (int)ret);
    }
    return ret;
}

/* ================================================================ */
void sys_mqtt_register_cmd_callback(sys_mqtt_cmd_callback_t cb)
{
    s_cmd_cb = cb;
}

/* ====== 内部: 故障掩码 → 新物模型(BMS-V2)结构化标志位 ======
 * 物模型 fault 位定义: bit0单体过压 bit1单体欠压 bit2过温 bit3欠温
 *                     bit4过流 bit5短路 bit6MOS故障 bit7不均衡 bit8通信故障
 * 内部掩码(bms_fault_e)按 WARN/PROT 分级, 此处归并到模型标志位 */
static void fault_to_flags(bms_fault_mask_t fault,
                           int *cell_ov, int *cell_uv, int *pack_ot, int *pack_ut,
                           int *over_cur, int *short_cir, int *mos_fet,
                           int *imbalance, int *comm)
{
    *cell_ov   = (fault & (FAULT_CELL_OV_WARN | FAULT_CELL_OV_PROT)) ? 1 : 0;
    *cell_uv   = (fault & (FAULT_CELL_UV_WARN | FAULT_CELL_UV_PROT)) ? 1 : 0;
    *pack_ot   = (fault & (FAULT_OT_WARN | FAULT_OT_PROT | FAULT_DTDT_WARN | FAULT_THERMAL_RUN)) ? 1 : 0;
    *pack_ut   = (fault & (FAULT_UT_WARN | FAULT_UT_PROT)) ? 1 : 0;
    *over_cur  = (fault & (FAULT_CHG_OC_WARN | FAULT_CHG_OC_PROT |
                           FAULT_DSG_OC_WARN | FAULT_DSG_OC_PROT | FAULT_OVERLOAD_WARN)) ? 1 : 0;
    *short_cir = (fault & FAULT_SHORT) ? 1 : 0;
    *mos_fet   = (fault & FAULT_SAMPLE_FAIL) ? 1 : 0;
    *imbalance = (fault & FAULT_CELL_DV_WARN) ? 1 : 0;
    *comm      = (fault & FAULT_COMM_WARN) ? 1 : 0;
}

/* ================================================================ */
/* 2026-08-08 功能安全 S1b: CRC8 前置声明(定义在文件尾部, 此处供上报使用) */
static uint8_t crc8_calc(const uint8_t *data, size_t len);
/* 2026-08-09 高可靠加固: CRC32 前置声明(与 CRC8 形成双重校验) */
static uint32_t crc32_calc(const uint8_t *data, size_t len);

void sys_mqtt_report(const bms_pack_data_t *pack,
                     const bms_soc_data_t  *soc,
                     bms_fault_mask_t       fault)
{
    if (pack == NULL || soc == NULL) {
        return;
    }

    char topic[128];
    build_iotda_topic(topic, sizeof(topic), "properties/report");

    /* 华为云 IoTDA 属性上报格式:
     * {"services":[{"service_id":"BMS","properties":{...}}]} */
    /* BMS-V2 物模型: cellVoltages / temps 为 string list 类型(字符串数组)
     * 动态拼接, 支持 6S~16S 任意串数, 上限 BMS_MAX_CELL_SERIES_NUM
     * v7: 串数用运行时参数(网页可下发 cell_series_num), 实时适配 */
    uint8_t report_series = (uint8_t)(sys_params_get()->cell_series_num);
    if (report_series < 1 || report_series > BMS_MAX_CELL_SERIES_NUM) {
        report_series = BMS_CELL_SERIES_NUM;
    }
    /* 缓冲尺寸按最坏情况容纳 BMS_MAX_CELL_SERIES_NUM 串:
     *   cell: 每串 ",\"35000\"" 最多 8 字符(N 串共 8N) + '[' + ']'
     *   temp: int16_t 最多 "-32768" 6 位数字, 每串最多 9 字符(9N) + 括号
     * 原 *7+4 在 32 串时不足, 且 off 累加 snprintf 返回值(本应写入长度而非实际)
     * 导致临近末尾越界写栈 + 缺 ']' 畸形 JSON. 现用实际写入长度推进, 空间不足即停. */
    char cell_str[BMS_MAX_CELL_SERIES_NUM * 8 + 2];
    char temp_str[BMS_MAX_CELL_SERIES_NUM * 9 + 2];
    {
        int off = 0;
        int w = snprintf(cell_str, sizeof(cell_str), "[");
        off += (w > 0) ? w : 0;
        for (uint8_t i = 0; i < report_series; i++) {
            w = snprintf(cell_str + off, sizeof(cell_str) - off, "%s\"%u\"",
                         (i > 0) ? "," : "", pack->cell_mv[i]);
            if (w < 0 || w >= (int)(sizeof(cell_str) - off)) {
                break;   /* 剩余空间不足, 停止追加(产出部分数组, 不越界) */
            }
            off += w;
        }
        if (off < (int)sizeof(cell_str) - 1) {
            cell_str[off++] = ']';
            cell_str[off] = '\0';
        } else {   /* 极端满缓冲: 强制收尾, 不越界 */
            cell_str[sizeof(cell_str) - 2] = ']';
            cell_str[sizeof(cell_str) - 1] = '\0';
        }
    }
    {
        int off = 0;
        int w = snprintf(temp_str, sizeof(temp_str), "[");
        off += (w > 0) ? w : 0;
        for (uint8_t i = 0; i < report_series; i++) {
            w = snprintf(temp_str + off, sizeof(temp_str) - off, "%s\"%d\"",
                         (i > 0) ? "," : "", pack->temp_dc[i]);
            if (w < 0 || w >= (int)(sizeof(temp_str) - off)) {
                break;
            }
            off += w;
        }
        if (off < (int)sizeof(temp_str) - 1) {
            temp_str[off++] = ']';
            temp_str[off] = '\0';
        } else {
            temp_str[sizeof(temp_str) - 2] = ']';
            temp_str[sizeof(temp_str) - 1] = '\0';
        }
    }

    /* 硬件状态回读: 真实继电器状态 + 均衡 + 充电模式
       用于前端 KPI 卡片状态文字判定(不再单靠电流猜), 以及下发命令后状态同步 */
    bool chg_on = false, dsg_on = false;
    sys_data_get_relay(&chg_on, &dsg_on);
    bms_balance_mask_t bmask = sys_data_get_balance_mask();
    bms_charge_mode_e cmode = sys_data_get_charge_mode();

    /* workState: 0=待机 1=充电中 2=放电中 3=故障保护 */
    int work_state = (fault != FAULT_NONE) ? 3 : (chg_on ? 1 : (dsg_on ? 2 : 0));

    /* 结构化故障标志位(物模型独立布尔属性, 便于规则引擎/告警) */
    int f_cov, f_cuv, f_pot, f_put, f_oc, f_sc, f_mos, f_imb, f_comm;
    fault_to_flags(fault, &f_cov, &f_cuv, &f_pot, &f_put, &f_oc, &f_sc, &f_mos, &f_imb, &f_comm);

    const bms_params_t *p = sys_params_get();

    /* 2026-08-08: 真实 UTC 时间戳(毫秒)替代 boot tick.
     *   pack->timestamp_ms 是 FreeRTOS tick(上电毫秒), 非真实时间,
     *   断网缓存补发时后端无法据此把数据放回断网时段, 历史曲线缺段.
     *   SNTP 同步后(正常联网)用 time(NULL)*1000, 未同步回退 tick. */
    unsigned long long report_ts_ms;
    if (s_time_synced) {
        report_ts_ms = (unsigned long long)time(NULL) * 1000ULL;
    } else {
        report_ts_ms = (unsigned long long)pack->timestamp_ms;
    }

    char json[1536];
    char stack_summary[128];                 /* 拷贝出内部缓冲, 防并发写入撕裂 */
    sys_data_get_stack_summary(stack_summary, sizeof(stack_summary));
    /* 通信健康状态位域(设备自报): 取代前端对 MQTT/TLS/CAN 的推断, 监控真·同步 */
    bms_comm_status_t cs = sys_data_get_comm_status();
    /* RS485/Modbus 从站通信状态(供状态卡片): 在线/最近轮询/错误计数 */
    uint8_t  rs485_online = 0;
    uint16_t rs485_last_poll = 0;
    uint16_t rs485_err = 0;
    sys_modbus_get_report(&rs485_online, &rs485_last_poll, &rs485_err);
    int n = snprintf(json, sizeof(json),
        "{\"services\":[{\"service_id\":\"%s\",\"properties\":{"
        "\"soc\":%.1f,\"soh\":%.1f,\"capacityAh\":%.1f,\"cellCount\":%u,"
        "\"vMax\":%u,\"vMin\":%u,\"packV\":%lu,\"current\":%d,"
        "\"tempMax\":%d,\"tempMin\":%d,\"temps\":%s,\"cellVoltages\":%s,"
        "\"fault\":%u,"
        "\"insulationRp\":%lu,\"insulationRn\":%lu,\"insulationOhmPerV\":%lu,"
        "\"cellOverVoltage\":%d,\"cellUnderVoltage\":%d,\"packOverTemp\":%d,"
        "\"packUnderTemp\":%d,\"overCurrent\":%d,\"shortCircuit\":%d,"
        "\"mosFetFault\":%d,\"cellImbalance\":%d,\"commFault\":%d,"
        "\"chargeMos\":%u,\"dischargeMos\":%u,\"balanceOn\":%u,\"workState\":%d,"
        "\"cycleCount\":%lu,\"timestamp\":%llu,\"chargeMode\":%u,\"fwVersion\":\"%s\","
        "\"commStatus\":%u,\"rs485Online\":%u,\"rs485LastPoll\":%u,\"rs485Err\":%u,"
        "\"otaStage\":\"%s\",\"otaProgress\":%d,\"otaError\":\"%s\","
        "\"stackSummary\":\"%s\"",
        BMS_MQTT_SERVICE_ID,
        soc->soc * 100.0f, soc->soh * 100.0f,
        (double)p->cell_capacity_mah / 1000.0, (unsigned)report_series,
        pack->cell_mv_max, pack->cell_mv_min, (unsigned long)pack->pack_mv, pack->current_ma,
        pack->temp_max_dc, pack->temp_min_dc, temp_str, cell_str,
        (unsigned)fault,
        (unsigned long)pack->insulation_rp_ohm,
        (unsigned long)pack->insulation_rn_ohm,
        (unsigned long)pack->insulation_ohm_per_v,
        f_cov, f_cuv, f_pot, f_put, f_oc, f_sc, f_mos, f_imb, f_comm,
        (unsigned)(chg_on ? 1 : 0),
        (unsigned)(dsg_on ? 1 : 0),
        (unsigned)(bmask != 0 ? 1 : 0),
        work_state,
        (unsigned long)soc->cycle_count,
        report_ts_ms,
        (unsigned)cmode,
        p->firmware_version,
        (unsigned)cs,
        (unsigned)rs485_online, (unsigned)rs485_last_poll, (unsigned)rs485_err,
        sys_ota_status_stage_str(), sys_ota_status_get_progress(), sys_ota_status_get_error(),
        stack_summary);

    if (n < 0 || n >= (int)sizeof(json)) {
        ESP_LOGW(TAG, "JSON 截断");
        return;
    }

    /* ====== 2026-08-08 功能安全 S1b: 通信 CRC 校验(SIL2 诊断覆盖) ======
     * 对关键安全字段(fault/mos/balance/工作状态/时间戳)计算 CRC8,
     * 追加到上报 JSON, 后端/云端可校验数据完整性(防位翻转/篡改).
     * 2026-08-10 D6: crc_key 与上报字段严格一一对应(balanceOn 布尔替代 bmask),
     *               Dashboard 后端已实现同算法复算校验(crc_ok), 链路闭环 */
    {
        uint8_t crc_key[24];
        int cn = 0;
        uint32_t f = (uint32_t)fault;
        crc_key[cn++] = (uint8_t)(f & 0xFF);
        crc_key[cn++] = (uint8_t)((f >> 8) & 0xFF);
        crc_key[cn++] = (uint8_t)((f >> 16) & 0xFF);
        crc_key[cn++] = (uint8_t)((f >> 24) & 0xFF);
        crc_key[cn++] = (uint8_t)(chg_on ? 1 : 0);
        crc_key[cn++] = (uint8_t)(dsg_on ? 1 : 0);
        /* 2026-08-10 D6: balanceMask 原始 2 字节 → 上报的 balanceOn 布尔,
         *   使 crc_key 与上报 JSON 字段一一对应(fault/chargeMos/dischargeMos/
         *   balanceOn/workState/chargeMode/timestamp), 后端可精确复算校验 */
        crc_key[cn++] = (uint8_t)(bmask != 0 ? 1 : 0);
        crc_key[cn++] = (uint8_t)work_state;
        crc_key[cn++] = (uint8_t)cmode;
        crc_key[cn++] = (uint8_t)(report_ts_ms & 0xFF);
        crc_key[cn++] = (uint8_t)((report_ts_ms >> 8) & 0xFF);
        uint8_t crc8 = crc8_calc(crc_key, cn);
        /* 2026-08-09 高可靠加固: 追加 CRC32 双重校验(与 CRC8 并存) */
        uint32_t crc32 = crc32_calc(crc_key, cn);
        /* 2026-08-10 F1 修复: crc/crc32 追加到 properties 内部(fwVersion 之后),
         * 由本行提供 }}]} 闭合; 原实现拼接在根对象闭合(}}]})之后, 整帧 JSON 非法,
         * 华为云 IoTDA 拒收(实测 Python json.loads → Extra data), 上报链路全断 */
        int tail = snprintf(json + n, sizeof(json) - n, ",\"crc\":%u,\"crc32\":%lu}}]}",
                            (unsigned)crc8, (unsigned long)crc32);
        if (tail > 0 && (n + tail) < (int)sizeof(json)) {
            n += tail;
        } else {
            ESP_LOGW(TAG, "JSON 追加 crc 截断");
        }
    }

    /* MQTT 已连接: 直接发布; 断开或非就绪: 缓存到 SPIFFS 等恢复后补传
     * (B1 修复: 由 publish_json 在锁内统一判定, 避免判定与发布之间的竞态丢帧) */
    if (!s_skip_iotda_publish) {          /* 2026-09-10: EMQX-only 帧跳过华为云 */
        if (!publish_json(topic, json)) {
            /* ====== B2 时间可信标志(2026-09-15 高可靠加固) ======
             * 断网期间 SNTP 可能未同步(SPIFFS 不掉时钟), 缓存数据的时间戳
             * 精度不可信. 内嵌 "_tcf" 字段: 1=SNTP 已同步(时间可信),
             * 0=未同步(时间为重启累计值, 云端应降权/标记).
             * 行仍是纯 JSON — 补传直接发原文, 云端解析多余字段无副作用. */
            char tcf_json[1536];
            size_t jl = strlen(json);
            if (jl > 1 && jl < sizeof(tcf_json) - 12 && json[jl - 1] == '}') {
                int n = snprintf(tcf_json, sizeof(tcf_json), "%.*s,\"_tcf\":%d}",
                                 (int)(jl - 1), json, s_time_synced ? 1 : 0);
                if (n > 0 && (size_t)n < sizeof(tcf_json)) {
                    sys_offline_cache_save(tcf_json);
                } else {
                    sys_offline_cache_save(json);   /* 注入超长, 保底原文 */
                }
            } else {
                sys_offline_cache_save(json);       /* 非常规行/超长, 保底原文 */
            }
        }
    }
    /* 2026-08-16 双通道: 同一份 JSON 双发到自建 Mosquitto(bms/<id>/telemetry)。
     * 第二通道不缓存(华为云主通道已兜底), 仅尽力而为; 命令仍走华为云主通道。 */
    publish_json2(json);
}

/* 2026-09-10 提速: EMQX-only 全量帧(无华为云发布, 不耗 15000 条/天配额)。
 * task_comm 每 2s 调用一次, 复用 sys_mqtt_report 的组包逻辑但只走 EMQX 通道——
 * 网页 KPI 实时性 7s→2s; 华为云仍按原 7s 节奏(由调用方 task_comm 控频)。 */
void sys_mqtt_report_emqx_only(const bms_pack_data_t *pack,
                               const bms_soc_data_t  *soc,
                               bms_fault_mask_t       fault)
{
    if (pack == NULL || soc == NULL) {
        return;
    }
    if (!s_connected2) {               /* EMQX 未连: 不发(避免误入华为云缓存) */
        return;
    }
    /* 复用主报告函数, 但临时抑制华为云发布 */
    s_skip_iotda_publish = true;
    sys_mqtt_report(pack, soc, fault);
    s_skip_iotda_publish = false;
}

/* ====== 2026-08-08 功能安全 S1b: CRC8 计算(多项式 0x07, 初始 0x00) ====== */
static uint8_t crc8_calc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* ====== 2026-08-09 高可靠加固: CRC32 完整校验(双重完整性度量) ======
 * 标准 IEEE CRC32(多项式 0xEDB88320, 初始 0xFFFFFFFF, 结果取反),
 * 与 CRC8 形成双重校验: CRC8 快速检测 + CRC32 抗碰撞完整性, 抵御位翻转/篡改. */
static uint32_t crc32_calc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

/* ====== 内部: 生成华为云事件时间(ISO8601 UTC 格式, 如 20260808T120000Z) ====== */
static void format_event_time(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    strftime(buf, len, "%Y%m%dT%H%M%SZ", &tm_now);
}

/* ================================================================ */
/* 2026-08-14: OTA 阶段即时回报(设备 → 云端)
 * OTA 下载/校验期间 task_ota 调用, 立刻发一条 property 更新,
 * 让前端无需等待周期上报即可看到阶段变化(对齐企业 OTA 任务进度回报) */
void sys_mqtt_report_ota_stage(void)
{
    if (!s_connected || s_client == NULL) {
        return;     /* 离线时丢弃(周期上报会从 task_comm 补带最新阶段) */
    }

    char topic[128];
    build_iotda_topic(topic, sizeof(topic), "properties/report");

    char json[200];
    int n = snprintf(json, sizeof(json),
        "{\"services\":[{\"service_id\":\"%s\",\"properties\":{"
        "\"otaStage\":\"%s\",\"otaProgress\":%d,\"otaError\":\"%s\"}}]}",
        BMS_MQTT_SERVICE_ID,
        sys_ota_status_stage_str(),
        sys_ota_status_get_progress(),
        sys_ota_status_get_error());
    if (n > 0 && n < (int)sizeof(json)) {
        publish_json(topic, json);
    }
}

/* ================================================================
 * BMS-V2 物模型事件(events/up topic):
 *   fault_report: event_type=fault  paras={fault:位掩码}
 *   fault_clear : event_type=alert paras={fault:已恢复的位掩码}
 *   info_report : event_type=info  paras={msg:通知文本}
 * ================================================================ */
void sys_mqtt_report_fault(bms_fault_mask_t fault)
{
    if (!s_connected || s_client == NULL) {
        return;
    }

    char topic[128];
    build_iotda_topic(topic, sizeof(topic), "events/up");

    char et[24];
    format_event_time(et, sizeof(et));

    char json[256];
    snprintf(json, sizeof(json),
             "{\"services\":[{\"service_id\":\"%s\",\"event_type\":\"fault\","
             "\"event_time\":\"%s\",\"paras\":{\"fault\":%u}}]}",
             BMS_MQTT_SERVICE_ID, et, (unsigned)fault);
    publish_json(topic, json);
}

/* ====== 故障恢复事件(fault_clear, eventType=alert) ====== */
void sys_mqtt_report_fault_clear(bms_fault_mask_t fault)
{
    if (!s_connected || s_client == NULL) {
        return;
    }

    char topic[128];
    build_iotda_topic(topic, sizeof(topic), "events/up");

    char et[24];
    format_event_time(et, sizeof(et));

    char json[256];
    snprintf(json, sizeof(json),
             "{\"services\":[{\"service_id\":\"%s\",\"event_type\":\"alert\","
             "\"event_time\":\"%s\",\"paras\":{\"fault\":%u}}]}",
             BMS_MQTT_SERVICE_ID, et, (unsigned)fault);
    publish_json(topic, json);
}

/* ====== 一般通知事件(info_report, eventType=info) ======
 * msg 用于充满/均衡完成/OTA 进度等短文本通知 */
void sys_mqtt_report_info_event(const char *msg)
{
    if (!s_connected || s_client == NULL || msg == NULL) {
        return;
    }

    char topic[128];
    build_iotda_topic(topic, sizeof(topic), "events/up");

    char et[24];
    format_event_time(et, sizeof(et));

    char json[320];
    snprintf(json, sizeof(json),
             "{\"services\":[{\"service_id\":\"%s\",\"event_type\":\"info\","
             "\"event_time\":\"%s\",\"paras\":{\"msg\":\"%s\"}}]}",
             BMS_MQTT_SERVICE_ID, et, msg);
    publish_json(topic, json);
}

/* ================================================================ */
void sys_mqtt_report_info(void)
{
    handle_get_info();
}

/* ================================================================ */
bool sys_mqtt_is_connected(void)
{
    bool c;
    MQTT_STATE_LOCK();
    c = s_connected;
    MQTT_STATE_UNLOCK();
    return c;
}

/* 最近一次成功 publish 的单调时间戳(ms), 供 comm_status 云可达位判定 */
uint64_t sys_mqtt_last_publish_ms(void)
{
    uint64_t t;
    MQTT_STATE_LOCK();
    t = s_last_publish_ms;
    MQTT_STATE_UNLOCK();
    return t;
}

/* ================================================================ */
/* H22 修复: 离线缓存补传处理(由 task_comm 主循环周期调用)
 * 事件回调只置 s_replay_pending 标志, 真正补传放主循环分批执行,
 * 避免阻塞 MQTT 事件线程导致 keepalive 超时被服务端踢下线. */
void sys_mqtt_process_pending_replay(void)
{
    if (!s_replay_pending) {
        return;
    }
    /* 连接已断开时保持标志, 等下次连接成功后再补传 */
    if (!s_connected || s_client == NULL) {
        return;
    }

    int ret = sys_offline_cache_replay(cache_replay_send_func);
    if (ret == 1) {
        /* 还有剩余缓存, 保持标志, 下个周期继续 */
        ESP_LOGI(TAG, "离线缓存尚未补传完, 下个周期继续");
    } else {
        /* 全部补传完成或失败, 清除标志(失败时数据保留在文件中, 下次连接再试) */
        s_replay_pending = false;
    }
}

/* ================================================================ */
bms_err_t sys_mqtt_stop(void)
{
    esp_err_t err1 = ESP_OK, err2 = ESP_OK;
    if (s_client == NULL) {
        return BMS_OK;   /* 没启动或已销毁, 直接OK */
    }

    MQTT_STATE_LOCK(); s_connected = false; MQTT_STATE_UNLOCK();

    /* 第1步: 主动断开, 发送 MQTT DISCONNECT 包
     *        ESP-IDF 内部会把 DISCONNECT 包放进发送队列, 返回 ESP_OK 不代表已经 TCP 发出,
     *        所以后面要 stop 阻塞等一会儿, 给 TCP 发包预留时间 */
    err1 = esp_mqtt_client_disconnect(s_client);
    if (err1 != ESP_OK) {
        ESP_LOGW(TAG, "mqtt disconnect 立即返回 err=%s (没关系, 后面还会 stop)", esp_err_to_name(err1));
    }

    /* 给 DISCONNECT 包飞出去的时间 (保守 150ms, 不要求100%送达)
     * 如果在 MQTT 事件线程里调用, vTaskDelay 不会阻塞后续事件:
     * 若此函数由其他任务调用, Delay 是安全的 */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 第2步: 停止 MQTT 客户端 (内部等待 socket 关闭, 释放 task, free 内部 buffer)
     *        ESP-IDF v5 中 esp_mqtt_client_stop 是阻塞的, 一般 300~1500ms 返回 */
    err2 = esp_mqtt_client_stop(s_client);
    if (err2 != ESP_OK) {
        ESP_LOGW(TAG, "mqtt stop err=%s (仍继续 destroy 防止泄漏)", esp_err_to_name(err2));
    }
    /* 第3步: destroy 释放所有资源 (可选, 若后续还要重连 init 则无需; 但 esp_restart 之前 destroy 更稳)
     * 为了保险, 这里不 destroy, 只 stop, 因为后面 app_tasks 里可能因为某种原因没有重启
     * (比如 delayed_reboot_task 创建失败后的退化重启也不会走到这里) → 保留 client 结构方便下次连 */
    ESP_LOGI(TAG, "MQTT 已停止 (disconnect ret=%d, stop ret=%d). 准备重启或后续动作",
             (int)err1, (int)err2);

    /* 2026-08-16 双通道: 同步停止第二通道(自建 Mosquitto) */
    if (s_client2 != NULL) {
        MQTT_STATE_LOCK(); s_connected2 = false; MQTT_STATE_UNLOCK();
        esp_mqtt_client_disconnect(s_client2);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_mqtt_client_stop(s_client2);
        ESP_LOGI(TAG, "MQTT2(自建) 已停止");
    }

    return (err2 == ESP_OK || err1 == ESP_OK) ? BMS_OK : BMS_ERR_FAIL;
}
