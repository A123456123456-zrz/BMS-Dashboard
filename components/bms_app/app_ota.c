/**
 * @file    app_ota.c
 * @brief   OTA 固件升级实现 (HTTPS 下载 + A/B 双分区 + 版本检查)
 * @author  BMS Team
 * @date    2026-08
 * @note    工作流程:
 *          1. app_ota_init(): 校验当前分区, 标记 valid 防 rollback
 *          2. app_ota_check_and_upgrade():
 *             a) 从 version_url 拉 JSON 版本信息
 *             b) 比较版本号, 若有新版则下载 firmware_url
 *             c) 流式写入 OTA 备用分区, 校验大小
 *             d) 标记 pending, 重启切换到新分区
 *          3. 新固件启动 app_ota_init() 标记 valid, 否则回滚
 *          部署方法:
 *             把 bms.bin 和 version.json 上传到 GitHub Release 或 HTTPS 服务器
 *             version.json 格式: {"version":"1.0.1","url":"https://.../bms.bin"}
 */
#include "app_ota.h"
#include "sys_params.h"
#include "sys_ota_status.h"
#include "sys_mqtt.h"                                   /* OTA 阶段即时回报(单向依赖: app→middleware) */
#include "bms_config.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_app_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "APP_OTA";

/* 内部状态 (H15: volatile — OTA 任务写 / 其他任务经 app_ota_is_in_progress 读) */
static volatile bool s_ota_in_progress = false;

/* ================================================================ */
bms_err_t app_ota_init(void)
{
    /* 校验当前分区状态, 标记上次升级成功(确认启动) */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            /* 新固件首次启动成功, 确认升级(防止回滚) */
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "OTA 新固件验证通过, 已标记 valid (防回滚)");
        }
    }

    const bms_params_t *p = sys_params_get();
    ESP_LOGI(TAG, "OTA 初始化, 当前固件版本=%s, 运行分区=%s",
             p->firmware_version, running->label);
    return BMS_OK;
}

/* ====== 内部: 从远程拉取版本 JSON ====== */
static bms_err_t fetch_remote_version(const char *url,
                                      char *version_buf, size_t version_buf_len,
                                      char *firmware_url_buf, size_t firmware_url_buf_len)
{
    if (url == NULL || version_buf == NULL || firmware_url_buf == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP client init fail");
        return BMS_ERR_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        /* 开发阶段常见: OTA 服务器未部署(占位URL) -> 降级为 INFO, 避免刷屏 ERROR */
        ESP_LOGW(TAG, "HTTP open fail (OTA 服务器未就绪?): %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return BMS_ERR_FAIL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    /* ---- Bug修复: 当返回 404/非200 时降级为 WARN 而非 ERROR ----
     * 开发阶段 version.json 尚未上传到服务器是常态, 不应以 ERROR 级别刷屏
     * 仅当 server 返回 200 但 body 不对时才算 ERROR */
    if (status_code != 200) {
        ESP_LOGW(TAG, "version.json HTTP %d (如 404 属正常, 表示暂无新版固件)", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return BMS_ERR_FAIL;
    }
    if (content_length <= 0 || content_length > 4096) {
        ESP_LOGE(TAG, "version.json 长度异常: %d", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return BMS_ERR_FAIL;
    }

    char *json_buf = malloc(content_length + 1);
    if (json_buf == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return BMS_ERR_NO_MEM;
    }

    /* M8 修复: 循环读取直到读满 content_length, 避免分块/粘包导致 JSON 截断 */
    int total_read = 0;
    while (total_read < content_length) {
        int n = esp_http_client_read(client, json_buf + total_read, content_length - total_read);
        if (n <= 0) {
            break;
        }
        total_read += n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_read <= 0) {
        ESP_LOGE(TAG, "HTTP read fail");
        free(json_buf);
        return BMS_ERR_FAIL;
    }
    json_buf[total_read] = '\0';

    ESP_LOGI(TAG, "version.json: %s", json_buf);

    /* 解析 JSON: {"version":"1.0.1","url":"https://.../bms.bin"} */
    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "JSON 解析失败");
        return BMS_ERR_FAIL;
    }

    const cJSON *j_ver = cJSON_GetObjectItem(root, "version");
    const cJSON *j_url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(j_ver) || !cJSON_IsString(j_url)) {
        ESP_LOGW(TAG, "JSON 缺少 version/url 字段 -> 跳过升级 (非 ERROR)");
        cJSON_Delete(root);
        return BMS_ERR_FAIL;
    }

    /* H14 修复: strncpy 截断是静默的, 若版本号/URL 被截断直接报错跳过,
     * 避免用残缺 URL 下载导致升级失败却无任何提示 */
    if (strlen(j_ver->valuestring) >= version_buf_len) {
        ESP_LOGE(TAG, "远程版本号过长(%uB >= 缓冲%uB), 跳过升级",
                 (unsigned)strlen(j_ver->valuestring), (unsigned)version_buf_len);
        cJSON_Delete(root);
        return BMS_ERR_PARAM_INVALID;
    }
    strncpy(version_buf, j_ver->valuestring, version_buf_len - 1);
    version_buf[version_buf_len - 1] = '\0';

    if (strlen(j_url->valuestring) >= firmware_url_buf_len) {
        ESP_LOGE(TAG, "固件URL过长(%uB >= 缓冲%uB), 跳过升级",
                 (unsigned)strlen(j_url->valuestring), (unsigned)firmware_url_buf_len);
        cJSON_Delete(root);
        return BMS_ERR_PARAM_INVALID;
    }
    strncpy(firmware_url_buf, j_url->valuestring, firmware_url_buf_len - 1);
    firmware_url_buf[firmware_url_buf_len - 1] = '\0';

    cJSON_Delete(root);
    return BMS_OK;
}

/* ====== 内部: 比较版本号 "1.0.0" vs "1.0.1" ======
 * 返回: 1 = remote > local, 0 = 相同, -1 = remote < local */
static int compare_version(const char *local, const char *remote)
{
    int l1=0, l2=0, l3=0, r1=0, r2=0, r3=0;
    sscanf(local,  "%d.%d.%d", &l1, &l2, &l3);
    sscanf(remote, "%d.%d.%d", &r1, &r2, &r3);
    if (r1 != l1) return r1 > l1 ? 1 : -1;
    if (r2 != l2) return r2 > l2 ? 1 : -1;
    if (r3 != l3) return r3 > l3 ? 1 : -1;
    return 0;
}

/* ====== 内部: HTTPS 流式下载并写入 OTA 分区 ====== */
static bms_err_t download_and_flash(const char *firmware_url)
{
    ESP_LOGI(TAG, "开始下载: %s", firmware_url);

    /* 2026-08-14 修复: 清除上一轮残留阶段(如 FAILED), 防止旧状态随周期上报回传,
     * 导致前端在收到本轮 DOWNLOADING 之前误判失败(前端已用\"是否已见 downloading\"做门控) */
    sys_ota_status_set_stage(OTA_STAGE_IDLE, 0);
    sys_mqtt_report_ota_stage();

    esp_http_client_config_t config = {
        .url = firmware_url,
        .timeout_ms = BMS_OTA_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = BMS_OTA_BUFFER_SIZE,
        .buffer_size_tx = BMS_OTA_BUFFER_SIZE,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .partial_http_download = false,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin fail: %s", esp_err_to_name(err));
        return BMS_ERR_FAIL;
    }

    /* 流式下载并写入 */
    int last_percent = -1;
    /* 2026-08-14: 进入下载阶段, 立即回报云端(前端 ② 设备下载 标记为"设备已确认") */
    sys_ota_status_set_stage(OTA_STAGE_DOWNLOADING, 0);
    sys_mqtt_report_ota_stage();
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        /* 进度打印 */
        int data_read = esp_https_ota_get_image_len_read(ota_handle);
        int total = esp_https_ota_get_image_size(ota_handle);
        if (total > 0) {
            int percent = data_read * 100 / total;
            if (percent != last_percent && percent % 10 == 0) {
                ESP_LOGI(TAG, "OTA 进度: %d%% (%d/%d)", percent, data_read, total);
                /* 2026-08-14: 每 10% 回报下载进度(前端展示实时百分比) */
                sys_ota_status_set_stage(OTA_STAGE_DOWNLOADING, percent);
                sys_mqtt_report_ota_stage();
                last_percent = percent;
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform fail: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        sys_ota_status_set_failed(OTA_ERR_DOWNLOAD);     // HTTP/网络下载失败
        sys_mqtt_report_ota_stage();
        return BMS_ERR_FAIL;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "OTA 数据未完整接收");
        esp_https_ota_abort(ota_handle);
        sys_ota_status_set_failed(OTA_ERR_INCOMPLETE);   // 数据未完整接收
        sys_mqtt_report_ota_stage();
        return BMS_ERR_FAIL;
    }

    /* H14 修复: 镜像大小合理性校验 — 有效 OTA 固件不可能为 0 或极小,
     * 防止恶意/损坏 URL 返回空 body 时被 esp_https_ota_finish 放过 */
    {
        int total = esp_https_ota_get_image_size(ota_handle);
        if (total <= 0) {
            ESP_LOGE(TAG, "OTA 镜像大小异常 (%d), 拒绝升级", total);
            esp_https_ota_abort(ota_handle);
            sys_ota_status_set_failed(OTA_ERR_IMAGE_SIZE); // 镜像大小异常(可能损坏/恶意)
            sys_mqtt_report_ota_stage();
            return BMS_ERR_FAIL;
        }
        ESP_LOGI(TAG, "OTA 镜像大小 %d 字节, 通过合理性校验", total);
    }

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "OTA 镜像校验失败, 可能固件损坏");
            sys_ota_status_set_failed(OTA_ERR_VALIDATE);  // SHA256 校验失败
        } else {
            ESP_LOGE(TAG, "OTA finish fail: %s", esp_err_to_name(err));
            sys_ota_status_set_failed(OTA_ERR_WRITE);     // 写入/Finish 失败(非校验)
        }
        sys_mqtt_report_ota_stage();
        return BMS_ERR_FAIL;
    }

    /* 2026-08-14: 镜像写入 OTA 分区 + SHA256 校验通过 → 进入"校验写入"阶段
     * (前端 ③ 标记为"设备已确认"), 随后由调用方标记 rebooting 并重启 */
    sys_ota_status_set_stage(OTA_STAGE_VERIFYING, 100);
    sys_mqtt_report_ota_stage();
    ESP_LOGI(TAG, "OTA 下载完成, 即将重启");
    return BMS_OK;
}

/* ================================================================ */
bms_err_t app_ota_check_and_upgrade(void)
{
    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "OTA 已在进行中, 跳过");
        return BMS_OK;
    }

    const bms_params_t *p = sys_params_get();

    ESP_LOGI(TAG, "OTA 检查, version_url=%s", p->ota_version_url);

    /* 1. 拉取远程版本 JSON
     * H14 修复: 原 firmware_url[128] 会把长 URL 截断导致静默下载失败,
     * 现扩大缓冲区并在 fetch_remote_version 内检测截断(见该函数) */
    char remote_ver[32] = {0};
    char firmware_url[512] = {0};
    bms_err_t err = fetch_remote_version(p->ota_version_url,
                                         remote_ver, sizeof(remote_ver),
                                         firmware_url, sizeof(firmware_url));
    if (err != BMS_OK) {
        ESP_LOGW(TAG, "获取版本信息失败, 跳过本次升级");
        return err;
    }

    ESP_LOGI(TAG, "本地版本=%s 远程版本=%s", p->firmware_version, remote_ver);

    /* 2. 比较版本号: 远程 <= 本地 → 无需升级 */
    if (compare_version(p->firmware_version, remote_ver) <= 0) {
        ESP_LOGI(TAG, "已是最新版本, 无需升级");
        sys_ota_status_set_pending(NULL, NULL);    /* 清空待升级缓存 */
        return BMS_OK;
    }

    ESP_LOGI(TAG, "发现新版本 %s", remote_ver);

    /* 2026-08-13: 行业惯例"人工确认"模式(默认) — 检查到新版本只缓存上报,
     * 等用户在前端确认后才升级; 无人值守设备可经 ota_auto_confirm=1 自动升级. */
    sys_ota_status_set_pending(remote_ver, firmware_url);

    if (!sys_params_is_ota_auto_confirm()) {
        ESP_LOGI(TAG, "新版本 %s 待用户确认(人工确认模式), 已上报前端由用户决定升级",
                 remote_ver);
        return BMS_OK;                          /* 只检查, 不升级 */
    }

    ESP_LOGI(TAG, "自动确认模式, 开始升级 %s", remote_ver);

    /* 3. 下载并写入 OTA 分区 */
    s_ota_in_progress = true;
    err = download_and_flash(firmware_url);
    s_ota_in_progress = false;

    if (err != BMS_OK) {
        ESP_LOGE(TAG, "OTA 升级失败");
        return err;
    }

    /* 4. 重启切换到新分区
     * 2026-08-13 B7: 先写 OTA 升级标记, 新固件启动时据此保留 WiFi 配置
     * (串口烧录不会写此标记 → 走重新配网逻辑) */
    sys_params_set_ota_upgraded();
    ESP_LOGI(TAG, "OTA 升级成功, 3 秒后重启");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();

    return BMS_OK;                                      // 不会执行到
}

/* ================================================================ */
bms_err_t app_ota_upgrade_with_url(const char *firmware_url)
{
    if (firmware_url == NULL || strlen(firmware_url) == 0) {
        return BMS_ERR_PARAM_INVALID;
    }

    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "OTA 已在进行中");
        return BMS_ERR_FAIL;
    }

    s_ota_in_progress = true;
    bms_err_t err = download_and_flash(firmware_url);
    s_ota_in_progress = false;

    if (err == BMS_OK) {
        /* 2026-08-14: 标记"重启验证"阶段(前端 ④ 进入进行中), 立即回报云端;
         * 随后 3 秒延迟期间 MQTT 把该阶段发出, 设备重启后由新固件版本匹配判定成功 */
        sys_ota_status_set_stage(OTA_STAGE_REBOOTING, 100);
        sys_mqtt_report_ota_stage();
        /* 2026-08-13 B7: OTA 成功重启前写升级标记, 新固件启动保留 WiFi 配置 */
        sys_params_set_ota_upgraded();
        ESP_LOGI(TAG, "OTA 强制升级成功, 3 秒后重启");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
    return err;
}

/* ================================================================ */
bool app_ota_is_in_progress(void)
{
    return s_ota_in_progress;
}

/* 待升级版本/URL 缓存已下沉到 middleware 层 sys_ota_status (2026-08-13),
 * 供 sys_mqtt 上报读取, 避免 app_ota ↔ sys_mqtt 循环依赖. */
