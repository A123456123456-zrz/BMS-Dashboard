/**
 * @file    sys_params.c
 * @brief   系统参数持久化实现(NVS Flash)
 * @author  BMS Team
 * @date    2026-08
 * @note    实现要点:
 *          1. 内部 static bms_params_t s_params[2] 保存当前参数(双缓冲)
 *          2. NVS 命名空间 "bms_cfg", 键 "params" 存整个结构体 blob
 *          3. 首次上电 magic 不匹配 → 写默认值
 *          4. NVS 失败时降级: 用 bms_config.h 宏默认值填充, 不阻塞系统
 *          5. ESP32-S3 NVS 自动使用 nvs 分区(partitions.csv 中 0x9000 起)
 *
 * @note    2026-08-10 M1 修复(跨核并发撕裂读):
 *          原实现用 portMUX_TYPE 自旋锁仅屏蔽"当前核"中断, ESP32-S3 双核下,
 *          写方在核A做整结构赋值 `s_params = *params`(大结构体逐字节拷贝)时,
 *          核B的读方通过 sys_params_get() 返回裸指针 `&s_params` 正在读,
 *          会读到"半新半旧"的撕裂字段(如 float soc_offset 被截断).
 *          现改为"双缓冲 + FreeRTOS mutex":
 *            - 写方(sys_params_set/reset)在 mutex 保护下把新值写入"非活动缓冲",
 *              再原子切换 s_params_active 指针; 活动缓冲永不被原地改写.
 *            - 读方(sys_params_get)只取 &s_params[s_params_active], 该缓冲在两次
 *              写之间保持稳定, 不会被撕裂. mutex 真正跨双核互斥(另一核任务会阻塞).
 */
#include "sys_params.h"
#include "bms_config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
/* FreeRTOS 任务/互斥量(替代自旋锁, 真正跨双核互斥) */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "SYS_PARAMS";

/* ====== 内部状态 ====== */
/* 双缓冲: 索引 0/1. s_params_active 指向当前可读(活动)缓冲.
 * 写方永远写"非活动"缓冲并切换指针, 因此读方取到的活动缓冲不会被原地撕裂. */
static bms_params_t s_params[2];
static uint8_t      s_params_active = 0;
static SemaphoreHandle_t s_params_mutex = NULL;      // 保护 set/reset 的写+切换
static bool s_inited = false;                        // 初始化完成标志(预留, 供自检)

/* #4 NVS 写去抖: 网页逐字段下发 set_param 时, 每次都整 blob 写 Flash 会集中磨损.
 * 策略: params_commit 永远先同步切换活动缓冲(RAM 立即生效), NVS 落盘合并到时间窗内,
 * 窗口内多次写只产生一次整 blob 写; 脏标记由 sys_params_flush() 周期兜底刷盘. */
#define PARAMS_NVS_COALESCE_MS  2000
static bool    s_nvs_dirty = false;
static uint32_t s_last_nvs_ms = 0;

/* ====== 魔术字与版本 ======
 * magic  = 'B''M''S''5'(ASCII) + 版本号低字节
 *          0x424D5335 = "BMS5" 表示 BMS 项目 v5.x 配置
 * version = 1 (字段含义变更时递增) */
#define PARAMS_MAGIC      0x424D5338                 // "BMS8" (v8: mqtt_uri 扩展到128字节, 修复IoTDA地址截断)
#define PARAMS_VERSION    11                          // v11: 追加自建 Mosquitto 双通道参数 mqtt2_* (2026-08-16)
#define PARAMS_NVS_NS     "bms_cfg"                  // NVS 命名空间
#define PARAMS_NVS_KEY    "params"                   // NVS 键名

/* ====== 填充默认值(从 bms_config.h 宏) ====== */
static void params_load_defaults(bms_params_t *p)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->magic    = PARAMS_MAGIC;
    p->version  = PARAMS_VERSION;

    /* WiFi 配置 */
    strncpy(p->wifi_ssid, BMS_WIFI_SSID, sizeof(p->wifi_ssid) - 1);
    strncpy(p->wifi_pass, BMS_WIFI_PASS, sizeof(p->wifi_pass) - 1);
    p->wifi_configured  = 0;                            // 默认未配网, 首次开机进入 AP 配网

    /* 电池规格 */
    p->cell_capacity_mah = BMS_CELL_CAPACITY_MAH;
    p->cell_series_num   = BMS_CELL_SERIES_NUM;

    /* SOC 校准(默认无校准) */
    p->soc_offset = 0.0f;
    p->soc_gain   = 1.0f;

    /* 保护阈值(预警 + 保护 全部从 bms_config.h 填充) */
    p->cell_ov_prot_mv  = CELL_OV_PROT_MV;
    p->cell_uv_prot_mv  = CELL_UV_PROT_MV;
    p->temp_ot_prot_dc  = TEMP_OT_PROT_DC;
    p->temp_ut_prot_dc  = TEMP_UT_PROT_DC;
    p->chg_oc_prot_ma   = (int16_t)CHG_OC_PROT_MA;
    p->dsg_oc_prot_ma   = (int16_t)DSG_OC_PROT_MA;
    p->cell_ov_warn_mv  = CELL_OV_WARN_MV;
    p->cell_uv_warn_mv  = CELL_UV_WARN_MV;
    p->chg_oc_warn_ma   = (int16_t)CHG_OC_WARN_MA;
    p->dsg_oc_warn_ma   = (int16_t)DSG_OC_WARN_MA;
    p->temp_ot_warn_dc  = TEMP_OT_WARN_DC;
    p->temp_ut_warn_dc  = TEMP_UT_WARN_DC;
    p->cell_dv_warn_mv  = CELL_DV_WARN_MV;
    p->soc_low_warn_pct = SOC_LOW_WARN_PCT;

    /* 均衡参数 */
    p->balance_threshold_mv = BALANCE_THRESHOLD_MV;
    p->balance_stop_mv      = BALANCE_STOP_MV;

    /* v7: 网页可调参数默认值(与 bms_config.h 宏 + app.py PARAM_META 对齐) */
    p->cell_ov_recover_mv   = CELL_OV_RECOVER_MV;
    p->cell_uv_recover_mv   = CELL_UV_RECOVER_MV;
    p->cell_dv_recover_mv   = CELL_DV_RECOVER_MV;
    p->rated_current_ma     = (int16_t)RATED_CURRENT_MA;
    p->overload_warn_ratio  = OVERLOAD_WARN_RATIO;
    p->overload_prot_ratio  = OVERLOAD_PROT_RATIO;
    p->temp_ot_recover_dc   = TEMP_OT_RECOVER_DC;
    p->temp_ut_recover_dc   = TEMP_UT_RECOVER_DC;
    p->temp_dtdt_warn       = TEMP_DTDT_WARN;
    p->dv_dt_warn_mvps      = DV_DT_WARN_MVPS;
    p->soc_low_recover_pct  = SOC_LOW_RECOVER_PCT;
    p->soh_low_warn_pct     = SOH_LOW_WARN_PCT;
    p->balance_timeout_min  = BALANCE_TIMEOUT_MIN;
    p->charge_cc_current_ma = (uint16_t)BMS_CELL_CAPACITY_MAH;  /* 1C 恒流充电 */
    p->charge_cc_soc_thr    = 80;                                /* CC→CV 切换 SOC */
    p->charge_cv_soc_thr    = 100;                               /* 恒压截止 SOC */
    p->nominal_voltage_mv   = BMS_NOMINAL_VOLTAGE_MV;
    p->full_voltage_mv      = BMS_FULL_VOLTAGE_MV;
    p->cutoff_voltage_mv    = BMS_CUTOFF_VOLTAGE_MV;

    /* v8: 电池类型/SOC 算法(默认 NCM + AEKF, 与 app_soc.c 现有实现一致) */
    p->battery_type = 1;    /* 1=NCM 三元(默认, 对应现有多项式 OCV 曲线) */
    p->soc_algo     = 3;    /* 2026-08-09: 默认 MCC-EKF(3, 2025 精度0.78%抗野值, 最优);
                               0=AEKF 1=纯安时积分 2=OCV查表 4=UKF, 网页可实时切换 */

    /* v9: 均衡策略/起始SOC(默认电压差触发, 起始SOC不限制) */
    p->balance_start_soc_pct = 0;   /* 0=不限制 */
    p->balance_strategy      = 0;   /* 0=电压差触发(默认) */

    /* v10: 上报间隔(默认 10s, 8640/天, 低于15000上限) */
    p->report_interval_sec = BMS_REPORT_INTERVAL_SEC;

    /* 老化数据(全新电池) */
    p->cycle_count = 0;
    p->soh         = 1.0f;

    /* MQTT 配置 */
    strncpy(p->mqtt_uri, BMS_MQTT_URI, sizeof(p->mqtt_uri) - 1);
    strncpy(p->mqtt_client_id, BMS_MQTT_CLIENT_ID, sizeof(p->mqtt_client_id) - 1);
    strncpy(p->mqtt_user, BMS_MQTT_USER, sizeof(p->mqtt_user) - 1);
    strncpy(p->mqtt_pass, BMS_MQTT_PASS, sizeof(p->mqtt_pass) - 1);
    strncpy(p->mqtt_device_secret, BMS_MQTT_DEVICE_SECRET, sizeof(p->mqtt_device_secret) - 1);
    /* M2 修复: 主题前缀默认值与 bms_config.h 宏统一, 避免 NVS 残留 "bms" 导致主题错误 */
    strncpy(p->mqtt_topic_prefix, BMS_MQTT_TOPIC_PREFIX, sizeof(p->mqtt_topic_prefix) - 1);

    /* 自建 Mosquitto 双通道(v11, 2026-08-16): 与华为云双发并存, 默认值同 bms_config.h */
    strncpy(p->mqtt2_uri, BMS_MQTT2_URI, sizeof(p->mqtt2_uri) - 1);
    strncpy(p->mqtt2_client_id, BMS_MQTT2_CLIENT_ID, sizeof(p->mqtt2_client_id) - 1);
    strncpy(p->mqtt2_user, BMS_MQTT2_USER, sizeof(p->mqtt2_user) - 1);
    strncpy(p->mqtt2_pass, BMS_MQTT2_PASS, sizeof(p->mqtt2_pass) - 1);
    strncpy(p->mqtt2_topic_prefix, BMS_MQTT2_TOPIC_PREFIX, sizeof(p->mqtt2_topic_prefix) - 1);

    /* OTA 配置 */
    strncpy(p->ota_version_url, BMS_OTA_VERSION_URL, sizeof(p->ota_version_url) - 1);
    strncpy(p->ota_firmware_url, BMS_OTA_FIRMWARE_URL, sizeof(p->ota_firmware_url) - 1);
    strncpy(p->firmware_version, BMS_FIRMWARE_VERSION, sizeof(p->firmware_version) - 1);
}

/* ====== 从 NVS 读取 ====== */
static bms_err_t params_load_from_nvs(bms_params_t *p)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open fail: %s, 用默认值", esp_err_to_name(err));
        return BMS_ERR_STORAGE;
    }

    size_t len = sizeof(*p);
    err = nvs_get_blob(h, PARAMS_NVS_KEY, p, &len);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_blob fail: %s, 用默认值", esp_err_to_name(err));
        return BMS_ERR_STORAGE;
    }
    if (len != sizeof(*p)) {
        ESP_LOGW(TAG, "nvs blob 长度不匹配(%u != %u), 用默认值",
                 (unsigned)len, (unsigned)sizeof(*p));
        return BMS_ERR_STORAGE;
    }
    if (p->magic != PARAMS_MAGIC) {
        ESP_LOGW(TAG, "magic 不匹配(0x%08lX), 首次上电用默认值",
                 (unsigned long)p->magic);
        return BMS_ERR_STORAGE;
    }
    /* Bug1 修复: 增加 version 校验, 版本升级时旧 NVS 数据失效, 使用新默认值 */
    if (p->version != PARAMS_VERSION) {
        ESP_LOGW(TAG, "params version 不匹配(NVS=%u, 当前=%u), 用默认值覆盖",
                 (unsigned)p->version, (unsigned)PARAMS_VERSION);
        return BMS_ERR_STORAGE;
    }
    return BMS_OK;
}

/* ====== 保存到 NVS ====== */
static bms_err_t params_save_to_nvs(const bms_params_t *p)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(rw) fail: %s", esp_err_to_name(err));
        return BMS_ERR_STORAGE;
    }
    err = nvs_set_blob(h, PARAMS_NVS_KEY, p, sizeof(*p));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob/commit fail: %s", esp_err_to_name(err));
        return BMS_ERR_STORAGE;
    }
    return BMS_OK;
}

/* ====== 提交新参数(双缓冲核心) ======
 * 在 mutex 保护下把 src 拷入"非活动"缓冲, 强制 magic/version, 再原子切换活动指针.
 * 写方永不原地修改活动缓冲 → 读方(sys_params_get 取活动缓冲)永不被撕裂.
 * NVS 落盘去抖: 活动缓冲切换是同步的(RAM 立即生效, 运行时一致), 但整 blob 写 Flash
 * 合并到 PARAMS_NVS_COALESCE_MS 窗口, 减低磨损; 窗口内的写只置脏, 由 flush 兜底. */
static bms_err_t params_commit(const bms_params_t *src)
{
    if (src == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    /* mutex 未创建(极端初始化顺序异常)时退化为无锁直接操作, 不阻塞系统 */
    bool locked = (s_params_mutex != NULL)
                  && (xSemaphoreTake(s_params_mutex, portMAX_DELAY) == pdTRUE);
    uint8_t inact = (uint8_t)(1 - s_params_active);
    s_params[inact] = *src;
    s_params[inact].magic   = PARAMS_MAGIC;
    s_params[inact].version = PARAMS_VERSION;
    s_params_active = inact;                          // 原子切换活动指针(运行时立即生效)

    /* #fix: NVS 去抖记账(s_nvs_dirty/s_last_nvs_ms)移入互斥锁保护范围.
     *   原实现此处释放锁后再更新脏标记/时间戳, 与 sys_params_flush() 跨核竞争,
     *   可能导致窗口内某次写被跳过或 last_ms 被覆盖. 现 commit 与 flush 全程互斥. */
    uint32_t now_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
    if ((now_ms - s_last_nvs_ms) >= PARAMS_NVS_COALESCE_MS) {
        s_last_nvs_ms = now_ms;
        s_nvs_dirty = false;
        bms_err_t r = params_save_to_nvs(&s_params[s_params_active]);
        if (locked) {
            xSemaphoreGive(s_params_mutex);
        }
        return r;
    }
    s_nvs_dirty = true;
    if (locked) {
        xSemaphoreGive(s_params_mutex);
    }
    return BMS_OK;
}

/* ====== NVS 脏数据兜底刷盘 ======
 * 由周期任务(采集任务 100ms 循环)调用: 若处于脏状态且距上次写已满窗口, 落盘一次.
 * 保证窗口内多次 set_param 最终只写一次 Flash, 且不会因无后续写而永远丢失. */
void sys_params_flush(void)
{
    /* #fix: 判脏 + 保存全程持锁, 与 params_commit 互斥, 消除标量竞态 */
    bool locked = (s_params_mutex != NULL)
                  && (xSemaphoreTake(s_params_mutex, portMAX_DELAY) == pdTRUE);
    if (!s_nvs_dirty) {
        if (locked) {
            xSemaphoreGive(s_params_mutex);
        }
        return;
    }
    uint32_t now_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
    if ((now_ms - s_last_nvs_ms) < PARAMS_NVS_COALESCE_MS) {
        if (locked) {
            xSemaphoreGive(s_params_mutex);
        }
        return;
    }
    bms_err_t err = params_save_to_nvs(&s_params[s_params_active]);
    if (err == BMS_OK) {
        s_nvs_dirty = false;
        s_last_nvs_ms = now_ms;
    }
    if (locked) {
        xSemaphoreGive(s_params_mutex);
    }
}

/* ====== 立即强制落盘(无视去抖窗口) ======
 * Bug 修复(2026-08-13): 配网门户 handler_save / MQTT set_wifi 保存配置后立即重启,
 * 若距上次 NVS 写不足 PARAMS_NVS_COALESCE_MS(2s), params_commit 只置脏不落盘,
 * 重启早于窗口结束 → 新 WiFi 配置丢失, 设备继续用旧配置连旧路由器.
 * 本函数无条件把活动缓冲写 Flash, 由"保存后即将重启"的路径显式调用. */
bms_err_t sys_params_flush_now(void)
{
    bool locked = (s_params_mutex != NULL)
                  && (xSemaphoreTake(s_params_mutex, portMAX_DELAY) == pdTRUE);
    bms_err_t err = params_save_to_nvs(&s_params[s_params_active]);
    if (err == BMS_OK) {
        s_nvs_dirty = false;
        s_last_nvs_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
    }
    if (locked) {
        xSemaphoreGive(s_params_mutex);
    }
    return err;
}

/* ================================================================ */
bms_err_t sys_params_init(void)
{
    /* 0. 创建互斥锁(任务上下文, 真正跨双核互斥) */
    if (s_params_mutex == NULL) {
        s_params_mutex = xSemaphoreCreateMutex();
        if (s_params_mutex == NULL) {
            ESP_LOGE(TAG, "xSemaphoreCreateMutex 失败");
        }
    }

    /* 1. 双缓冲都先用默认值填充(兜底) */
    params_load_defaults(&s_params[0]);
    params_load_defaults(&s_params[1]);
    s_params_active = 0;

    /* 2. 尝试从 NVS 读取覆盖 */
    bms_err_t err = params_load_from_nvs(&s_params[s_params_active]);
    if (err != BMS_OK) {
        /* Bug1 修复: 读取失败(首次上电/损坏/version升级)时的处理
         *
         * 原bug: params_load_from_nvs 内部 nvs_get_blob 已把旧数据覆盖到 s_params,
         *        若 version 不匹配直接返回, 但 wifi_configured=1 等 WiFi 配置仍残留,
         *        导致系统用旧 WiFi 配置连接而不进入配网模式; 同时旧的电流阈值(2A)也残留
         *
         * 修复策略(区分两种情况):
         *   - magic/version 不匹配但 NVS 有数据: 保留 WiFi 配置(免重新配网),
         *     重置保护阈值等参数为新的默认值, 更新 version
         *   - NVS 完全无数据/损坏: 全部用默认值(wifi_configured=0, 进入配网模式)
         */
        if (s_params[s_params_active].magic == PARAMS_MAGIC
            && s_params[s_params_active].wifi_configured == 1) {
            /* version 升级场景: 保留 WiFi + MQTT 连接配置, 重置保护参数和密钥 */
            ESP_LOGI(TAG, "参数版本升级 (%u -> %u), 保留 WiFi+MQTT 配置, 重置密钥",
                     (unsigned)s_params[s_params_active].version, (unsigned)PARAMS_VERSION);

            /* 备份 WiFi 配置 */
            char    backup_ssid[sizeof(s_params[0].wifi_ssid)];
            char    backup_pass[sizeof(s_params[0].wifi_pass)];
            uint8_t backup_configured = s_params[s_params_active].wifi_configured;
            memcpy(backup_ssid, s_params[s_params_active].wifi_ssid, sizeof(backup_ssid));
            memcpy(backup_pass, s_params[s_params_active].wifi_pass, sizeof(backup_pass));

            /* 备份 MQTT 连接配置(client_id/uri/user), 但不备份 secret(强制重置) */
            char backup_mqtt_uri[sizeof(s_params[0].mqtt_uri)];
            char backup_mqtt_client_id[sizeof(s_params[0].mqtt_client_id)];
            char backup_mqtt_user[sizeof(s_params[0].mqtt_user)];
            memcpy(backup_mqtt_uri, s_params[s_params_active].mqtt_uri, sizeof(backup_mqtt_uri));
            memcpy(backup_mqtt_client_id, s_params[s_params_active].mqtt_client_id,
                   sizeof(backup_mqtt_client_id));
            memcpy(backup_mqtt_user, s_params[s_params_active].mqtt_user,
                   sizeof(backup_mqtt_user));

            /* 重新加载全部默认值(含正确的 device_secret + 电流保护阈值)到临时缓冲 */
            bms_params_t tmp;
            params_load_defaults(&tmp);

            /* 恢复 WiFi 配置 */
            memcpy(tmp.wifi_ssid, backup_ssid, sizeof(tmp.wifi_ssid));
            memcpy(tmp.wifi_pass, backup_pass, sizeof(tmp.wifi_pass));
            tmp.wifi_configured = backup_configured;

            /* 恢复 MQTT 连接配置(保留用户在配网时输入的 client_id/uri, 但 secret 用默认值) */
            memcpy(tmp.mqtt_uri, backup_mqtt_uri, sizeof(tmp.mqtt_uri));
            memcpy(tmp.mqtt_client_id, backup_mqtt_client_id, sizeof(tmp.mqtt_client_id));
            memcpy(tmp.mqtt_user, backup_mqtt_user, sizeof(tmp.mqtt_user));

            ESP_LOGI(TAG, "WiFi+MQTT 配置已保留 (SSID=%s, client_id=%s), 密钥已重置为默认值",
                     tmp.wifi_ssid, tmp.mqtt_client_id);
            /* 提交临时缓冲为新的活动参数(写非活动缓冲 + 切换 + 落盘) */
            params_commit(&tmp);
        } else {
            /* 首次上电或 NVS 损坏: 全部用默认值 */
            ESP_LOGI(TAG, "首次上电或 NVS 损坏, 重新填充默认值");
            bms_params_t tmp;
            params_load_defaults(&tmp);
            params_commit(&tmp);
        }
    } else {
        /* 3. URL 有效性校验: NVS 可能存了被截断的 URL, 校验并修复
         * IoTDA 地址较长(68字符), 之前 mqtt_uri[64] 不够会导致截断
         * 检查是否以正确前缀开头 + 是否包含完整域名标识 */
        bool need_fix = false;
        if (strncmp(s_params[s_params_active].ota_version_url, "http", 4) != 0) {
            ESP_LOGW(TAG, "ota_version_url 无效(%s), 恢复默认值",
                     s_params[s_params_active].ota_version_url);
            strncpy(s_params[s_params_active].ota_version_url, BMS_OTA_VERSION_URL,
                    sizeof(s_params[s_params_active].ota_version_url) - 1);
            need_fix = true;
        }
        if (strncmp(s_params[s_params_active].ota_firmware_url, "http", 4) != 0) {
            ESP_LOGW(TAG, "ota_firmware_url 无效(%s), 恢复默认值",
                     s_params[s_params_active].ota_firmware_url);
            strncpy(s_params[s_params_active].ota_firmware_url, BMS_OTA_FIRMWARE_URL,
                    sizeof(s_params[s_params_active].ota_firmware_url) - 1);
            need_fix = true;
        }
        /* mqtt_uri 校验: 必须以 mqtt:// 或 mqtts:// 开头, 且包含 ".com" (防止截断) */
        if ((strncmp(s_params[s_params_active].mqtt_uri, "mqtt://", 7) != 0 &&
             strncmp(s_params[s_params_active].mqtt_uri, "mqtts://", 8) != 0) ||
            strstr(s_params[s_params_active].mqtt_uri, ".com") == NULL) {
            ESP_LOGW(TAG, "mqtt_uri 无效或被截断(%s), 恢复默认值",
                     s_params[s_params_active].mqtt_uri);
            strncpy(s_params[s_params_active].mqtt_uri, BMS_MQTT_URI,
                    sizeof(s_params[s_params_active].mqtt_uri) - 1);
            need_fix = true;
        }
        /* client_id 校验: 不能为空, 不能等于 WiFi SSID (防止字段错位)
         * 2026-08-12: 也不能是 EMQX 时代短ID "BMS001"(IoTDA 需完整设备ID, 否则 CONNACK 4) */
        if (s_params[s_params_active].mqtt_client_id[0] == '\0' ||
            strcmp(s_params[s_params_active].mqtt_client_id,
                   s_params[s_params_active].wifi_ssid) == 0 ||
            strcmp(s_params[s_params_active].mqtt_client_id, "BMS001") == 0) {
            ESP_LOGW(TAG, "mqtt_client_id 无效(%s), 恢复默认值",
                     s_params[s_params_active].mqtt_client_id);
            strncpy(s_params[s_params_active].mqtt_client_id, BMS_MQTT_CLIENT_ID,
                    sizeof(s_params[s_params_active].mqtt_client_id) - 1);
            strncpy(s_params[s_params_active].mqtt_user, BMS_MQTT_USER,
                    sizeof(s_params[s_params_active].mqtt_user) - 1);
            strncpy(s_params[s_params_active].mqtt_device_secret, BMS_MQTT_DEVICE_SECRET,
                    sizeof(s_params[s_params_active].mqtt_device_secret) - 1);
            need_fix = true;
        }
        /* device_secret 校验: 长度应在 4~32 之间, 过长(如哈希值32+)或过短均为无效
         * 之前出现配网时误将 HMAC 哈希值(长度20)填入 secret 字段, 导致 MQTT 鉴权失败
         * 2026-08-12: 也不能是已知失效的占位/旧默认密钥(长度合法但非真实密钥,
         * 会导致 HMAC 密码错误 → IoTDA CONNACK 4 "bad username or password"):
         *   - "emqx-no-secret": EMQX 迁移时代的占位值
         *   - "20040605zrZ":   旧出厂默认密钥(已更换为控制台实际密钥) */
        size_t sec_len = strlen(s_params[s_params_active].mqtt_device_secret);
        if (sec_len < 4 || sec_len > 32 ||
            strcmp(s_params[s_params_active].mqtt_device_secret, "emqx-no-secret") == 0 ||
            strcmp(s_params[s_params_active].mqtt_device_secret, "20040605zrZ") == 0) {
            ESP_LOGW(TAG, "mqtt_device_secret 无效(%s, len=%d), 恢复默认值",
                     s_params[s_params_active].mqtt_device_secret, (int)sec_len);
            strncpy(s_params[s_params_active].mqtt_device_secret, BMS_MQTT_DEVICE_SECRET,
                    sizeof(s_params[s_params_active].mqtt_device_secret) - 1);
            need_fix = true;
        }
        if (need_fix) {
            s_params[s_params_active].magic   = PARAMS_MAGIC;
            s_params[s_params_active].version = PARAMS_VERSION;
            params_save_to_nvs(&s_params[s_params_active]);
            ESP_LOGI(TAG, "已修复损坏的 URL/字段并保存到 NVS");
        }

        /* OTA 升级场景修复: NVS 里保存的 firmware_version 是旧固件写入的,
         * 新固件启动后若与编译宏 BMS_FIRMWARE_VERSION 不一致, 必须刷新并落盘,
         * 否则设备一直上报旧版本号, 且 compare_version 会误判"有新版本",
         * 每 12 小时重复下载同一份固件再次升级(浪费流量且永不收敛) */
        if (strncmp(s_params[s_params_active].firmware_version, BMS_FIRMWARE_VERSION,
                    sizeof(s_params[s_params_active].firmware_version)) != 0) {
            ESP_LOGI(TAG, "固件版本更新 (%s -> %s), 刷新 NVS",
                     s_params[s_params_active].firmware_version, BMS_FIRMWARE_VERSION);
            strncpy(s_params[s_params_active].firmware_version, BMS_FIRMWARE_VERSION,
                    sizeof(s_params[s_params_active].firmware_version) - 1);
            s_params[s_params_active].firmware_version[sizeof(s_params[s_params_active].firmware_version) - 1] = '\0';
            s_params[s_params_active].magic   = PARAMS_MAGIC;
            s_params[s_params_active].version = PARAMS_VERSION;
            params_save_to_nvs(&s_params[s_params_active]);
        }

        /* 2026-09-09: 自建 broker 专属账号迁移 — NVS 里存的 mqtt2_user/mqtt2_pass
         * 是旧固件写入的(共享 student 账号), 编译宏更新后必须刷新并落盘,
         * 否则设备永远用旧凭据连接(被 EMQX ACL 限制/审计无法区分设备) */
        if (strcmp(s_params[s_params_active].mqtt2_user, BMS_MQTT2_USER) != 0 ||
            strcmp(s_params[s_params_active].mqtt2_pass, BMS_MQTT2_PASS) != 0) {
            ESP_LOGI(TAG, "mqtt2 凭据更新 (%s -> %s), 刷新 NVS",
                     s_params[s_params_active].mqtt2_user, BMS_MQTT2_USER);
            strncpy(s_params[s_params_active].mqtt2_user, BMS_MQTT2_USER,
                    sizeof(s_params[s_params_active].mqtt2_user) - 1);
            s_params[s_params_active].mqtt2_user[sizeof(s_params[s_params_active].mqtt2_user) - 1] = '\0';
            strncpy(s_params[s_params_active].mqtt2_pass, BMS_MQTT2_PASS,
                    sizeof(s_params[s_params_active].mqtt2_pass) - 1);
            s_params[s_params_active].mqtt2_pass[sizeof(s_params[s_params_active].mqtt2_pass) - 1] = '\0';
            s_params[s_params_active].magic   = PARAMS_MAGIC;
            s_params[s_params_active].version = PARAMS_VERSION;
            params_save_to_nvs(&s_params[s_params_active]);
        }

        ESP_LOGI(TAG, "NVS 参数加载成功 (version=%lu, cycle=%lu, soh=%.1f%%)",
                 (unsigned long)s_params[s_params_active].version,
                 (unsigned long)s_params[s_params_active].cycle_count,
                 s_params[s_params_active].soh * 100.0f);
    }

    s_inited = true;
    return BMS_OK;
}

/* ================================================================ */
const bms_params_t *sys_params_get(void)
{
    /* 返回活动缓冲指针. 活动缓冲在两次写之间保持稳定(写方只写非活动缓冲后切换),
     * 因此调用方读取其字段期间不会被撕裂, 无需加锁.
     * 注: 若调用方需要"跨多次读取保持一致"的快照, 应在调用处缓存本指针的拷贝. */
    return &s_params[s_params_active];
}

/* ================================================================ */
bms_err_t sys_params_set(const bms_params_t *params)
{
    if (params == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    /* 双缓冲提交: 写非活动缓冲 + 切换 + 落盘, 读方永不被撕裂 */
    bms_err_t err = params_commit(params);
    if (err == BMS_OK) {
        ESP_LOGI(TAG, "参数已保存到 NVS");
    }
    return err;
}

/* ================================================================ */
bms_err_t sys_params_reset(void)
{
    bms_params_t tmp;
    params_load_defaults(&tmp);
    bms_err_t err = params_commit(&tmp);
    ESP_LOGI(TAG, "参数已重置为默认值");
    return err;
}

/* ====== 重新烧录检测: 编译指纹存取(独立 NVS key, 不进 bms_params_t blob) ======
 * 指纹 = BMS_FIRMWARE_VERSION + 编译日期 + 编译时间, 每次编译都不同,
 * 用于区分"串口重新烧录"与"OTA 升级"及"正常重启". */
#define REFLASH_FP_NVS_KEY   "reflash_fp"

/* ====== 故障黑匣子: 复位原因存取(独立 NVS key, 2026-09-14 高可靠加固) ======
 * 启动时记录 esp_reset_reason(), MQTT get_info 上报 → 云端可追溯"上次为什么死".
 * 独立 key 模式与 reflash_fp 一致: 不动 bms_params_t blob, 零 NVS 兼容性风险. */
#define BLACKBOX_NVS_KEY     "blackbox"

/* ====== 命令鉴权开关(独立 NVS key, 2026-09-14 高可靠加固 A1) ======
 * 0=关闭(默认, 与历史行为完全一致), 1=高危命令需 HMAC token.
 * 独立 key: OTA 后开关状态保留, 且不影响老参数 blob 解析. */
#define CMD_AUTH_NVS_KEY     "cmd_auth"

/* ====== 黑匣子扩展: 死前现场快照(独立 NVS key, 2026-09-14 高可靠加固 B1) ======
 * 仅在检测到异常复位(看门狗/panic)时写一次 — 正常上电不写, Flash 磨损可忽略.
 * 记录内容: fault 掩码(死前故障态) + SOC + 包电压/电流(死前运行点).
 * 注意: 死前最后时刻的 RAM 状态在崩溃瞬间无法保存(esp 无 panic 钩子持久化),
 * 这里记录的是"上一次运行周期结束时的运行点", 与复位原因组合仍有很强定位价值:
 *   例: TASK_WDT + fault=0x100000 → 看门狗饿死时保护已触发; POWERON+fault=0 → 正常重启 */
#define BLACKBOX_CTX_KEY     "blackbox_ctx"

bms_err_t sys_params_set_blackbox_ctx(uint32_t fault, uint8_t soc_pct,
                                      uint16_t pack_mv, int16_t ma)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return BMS_ERR_STORAGE;
    }
    uint8_t blob[9];
    blob[0] = (uint8_t)(fault & 0xFF);
    blob[1] = (uint8_t)((fault >> 8) & 0xFF);
    blob[2] = (uint8_t)((fault >> 16) & 0xFF);
    blob[3] = (uint8_t)((fault >> 24) & 0xFF);
    blob[4] = soc_pct;
    blob[5] = (uint8_t)(pack_mv & 0xFF);
    blob[6] = (uint8_t)((pack_mv >> 8) & 0xFF);
    blob[7] = (uint8_t)((uint16_t)ma & 0xFF);
    blob[8] = (uint8_t)(((uint16_t)ma >> 8) & 0xFF);
    err = nvs_set_blob(h, BLACKBOX_CTX_KEY, blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return (err == ESP_OK) ? BMS_OK : BMS_ERR_STORAGE;
}

bms_err_t sys_params_get_blackbox_ctx(uint32_t *fault, uint8_t *soc_pct,
                                      uint16_t *pack_mv, int16_t *ma)
{
    if (fault == NULL || soc_pct == NULL || pack_mv == NULL || ma == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    *fault = 0; *soc_pct = 0; *pack_mv = 0; *ma = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return BMS_ERR_STORAGE;
    }
    uint8_t blob[9] = {0};
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, BLACKBOX_CTX_KEY, blob, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(blob)) {
        *fault   = (uint32_t)blob[0] | ((uint32_t)blob[1] << 8) |
                   ((uint32_t)blob[2] << 16) | ((uint32_t)blob[3] << 24);
        *soc_pct  = blob[4];
        *pack_mv  = (uint16_t)(blob[5] | ((uint16_t)blob[6] << 8));
        *ma       = (int16_t)((uint16_t)blob[7] | ((uint16_t)blob[8] << 8));
        return BMS_OK;
    }
    return BMS_ERR_STORAGE;
}


bms_err_t sys_params_set_cmd_auth(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return BMS_ERR_STORAGE;
    }
    err = nvs_set_u8(h, CMD_AUTH_NVS_KEY, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return (err == ESP_OK) ? BMS_OK : BMS_ERR_STORAGE;
}

bool sys_params_get_cmd_auth(void)
{
    uint8_t v = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        nvs_get_u8(h, CMD_AUTH_NVS_KEY, &v);
        nvs_close(h);
    }
    return (v == 1);
}


bms_err_t sys_params_set_blackbox(uint8_t reset_reason)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return BMS_ERR_STORAGE;
    }
    err = nvs_set_u8(h, BLACKBOX_NVS_KEY, reset_reason);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return (err == ESP_OK) ? BMS_OK : BMS_ERR_STORAGE;
}

bms_err_t sys_params_get_blackbox(uint8_t *reset_reason)
{
    if (reset_reason == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    *reset_reason = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return BMS_ERR_STORAGE;
    }
    err = nvs_get_u8(h, BLACKBOX_NVS_KEY, reset_reason);
    nvs_close(h);
    return (err == ESP_OK) ? BMS_OK : BMS_ERR_STORAGE;
}


bms_err_t sys_params_get_reflash_fp(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return BMS_ERR_PARAM_INVALID;
    }
    buf[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return BMS_ERR_STORAGE;
    }
    size_t len = buf_len;
    err = nvs_get_str(h, REFLASH_FP_NVS_KEY, buf, &len);
    nvs_close(h);
    if (err != ESP_OK || len == 0) {
        buf[0] = '\0';
        return BMS_ERR_STORAGE;
    }
    return BMS_OK;
}

bms_err_t sys_params_set_reflash_fp(const char *fp)
{
    if (fp == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return BMS_ERR_STORAGE;
    }
    err = nvs_set_str(h, REFLASH_FP_NVS_KEY, fp);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return (err == ESP_OK) ? BMS_OK : BMS_ERR_STORAGE;
}

/* ====== OTA 升级标记(独立 NVS key) ======
 * 2026-08-13 B7 修复: 串口烧录后 bootloader 也会把运行分区置为
 * PENDING_VERIFY, 分区状态无法区分"OTA 升级"与"串口烧录",
 * 改为 OTA 成功路径显式留标记, 启动时据此决定是否保留 WiFi 配置. */
#define OTA_UPGRADED_NVS_KEY  "ota_upgraded"

bms_err_t sys_params_set_ota_upgraded(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return BMS_ERR_STORAGE;
    }
    err = nvs_set_u8(h, OTA_UPGRADED_NVS_KEY, 1);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return (err == ESP_OK) ? BMS_OK : BMS_ERR_STORAGE;
}

bool sys_params_is_ota_upgraded(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    err = nvs_get_u8(h, OTA_UPGRADED_NVS_KEY, &v);
    nvs_close(h);
    return (err == ESP_OK && v == 1);
}

void sys_params_clear_ota_upgraded(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return;
    }
    err = nvs_set_u8(h, OTA_UPGRADED_NVS_KEY, 0);
    if (err == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ====== OTA 升级模式开关(独立 NVS key) ======
 * 2026-08-13 新增: 行业惯例默认"人工确认"——检查到新版本只上报,
 * 等用户在前端确认后才升级; 无人值守设备可设为自动. */
#define OTA_AUTO_CONFIRM_NVS_KEY  "ota_auto_confirm"

bms_err_t sys_params_set_ota_auto_confirm(bool auto_confirm)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return BMS_ERR_STORAGE;
    }
    err = nvs_set_u8(h, OTA_AUTO_CONFIRM_NVS_KEY, auto_confirm ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return (err == ESP_OK) ? BMS_OK : BMS_ERR_STORAGE;
}

bool sys_params_is_ota_auto_confirm(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;          /* 默认人工确认 */
    }
    uint8_t v = 0;
    err = nvs_get_u8(h, OTA_AUTO_CONFIRM_NVS_KEY, &v);
    nvs_close(h);
    return (err == ESP_OK && v == 1);
}
