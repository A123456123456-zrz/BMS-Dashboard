/**
 * @file    sys_config_portal.c
 * @brief   HTTP 配网门户实现 (Captive Portal + 配置页)
 * @author  BMS Team
 * @date    2026-08
 * @note    核心 HTTP 路由:
 *            GET  /             主配网页(HTML 表单)
 *            POST /save         保存配置(WiFi/MQTT), 触发重启
 *            GET  /status       查询当前状态 JSON
 *            GET  /scan         扫描周边 WiFi
 *            GET  /reset        清除配置
 *            GET  /generate_204 Android Captive Portal 探测
 *            GET  /hotspot.html iOS Captive Portal 探测
 *          Captive Portal: 设备连上 AP 后, 系统会自动弹出配网页
 */
#include "sys_config_portal.h"
#include "sys_wifi.h"
#include "sys_params.h"
#include "bms_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CONFIG_PORTAL";

static httpd_handle_t s_server = NULL;
static volatile bool  s_config_done = false;             // 配网完成标志
static uint32_t       s_start_time_ms = 0;

/* ====== 内部: 成功提示页 HTML ====== */
static const char *SAVE_OK_HTML =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>配网成功</title>"
"<meta http-equiv='refresh' content='10;url=/'></head>"
"<body style='font-family:Arial;text-align:center;padding:40px'>"
"<h2 style='color:#4CAF50'>✅ 配置已保存!</h2>"
"<p>设备正在重启, 请等待约 15 秒...</p>"
"<p>重启后设备将连接您的 WiFi, 您可以重新连接到家庭 WiFi 后访问网页查看数据</p>"
"<p style='color:#888;font-size:12px'>本页面 10 秒后自动跳转</p>"
"</body></html>";

/* ====== 内部: URL 解码(简化版, 处理 + 和 %xx) ====== */
static void url_decode(char *str)
{
    if (str == NULL) return;
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* ====== 内部: 从 POST body 解析表单字段 ====== */
static void parse_form_field(const char *body, const char *field, char *out, size_t out_len)
{
    if (body == NULL || field == NULL || out == NULL || out_len == 0) return;
    out[0] = '\0';

    char pattern[32];
    snprintf(pattern, sizeof(pattern), "%s=", field);
    const char *p = strstr(body, pattern);
    if (p == NULL) return;
    p += strlen(pattern);

    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    url_decode(out);
}

/* ====== 内部: HTML 转义(防存储型 XSS, H18) ======
 * 转义 & < > " ' 五个危险字符; 输出缓冲需 ≥ 6×src+1(最坏 &quot;) */
static void html_escape(const char *src, char *dst, size_t dst_len)
{
    if (src == NULL || dst == NULL || dst_len == 0) return;
    dst[0] = '\0';
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 6 < dst_len; i++) {
        switch (src[i]) {
        case '&':  memcpy(dst + j, "&amp;",  5); j += 5; break;
        case '<':  memcpy(dst + j, "&lt;",   4); j += 4; break;
        case '>':  memcpy(dst + j, "&gt;",   4); j += 4; break;
        case '"':  memcpy(dst + j, "&quot;", 6); j += 6; break;
        case '\'': memcpy(dst + j, "&#39;",  5); j += 5; break;
        default:   dst[j++] = src[i]; break;
        }
    }
    dst[j] = '\0';
}

/* ====== 内部: JSON 字符串转义(防 JSON 注入/损坏, H18) ======
 * 转义 " \ 换行/回车/Tab 及控制字符; 输出缓冲需 ≥ 6×src+1 */
static void json_escape(const char *src, char *dst, size_t dst_len)
{
    if (src == NULL || dst == NULL || dst_len == 0) return;
    dst[0] = '\0';
    static const char hexd[] = "0123456789abcdef";
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 6 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
        case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
        case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
        case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
        case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
        case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
        default:
            if (c < 0x20) {
                dst[j++] = '\\'; dst[j++] = 'u'; dst[j++] = '0'; dst[j++] = '0';
                dst[j++] = hexd[(c >> 4) & 0xF];
                dst[j++] = hexd[c & 0xF];
            } else {
                dst[j++] = (char)c;                  /* UTF-8 多字节原样保留 */
            }
        }
    }
    dst[j] = '\0';
}

/* ====== HTTP handler: GET / 主配网页 ======
 * 动态填充当前 NVS 中的 client_id, 让用户能看到之前保存的值
 * 2026-08-13: SSID/密码输入框预填代码设定的默认 WiFi(BMS_WIFI_SSID/BMS_WIFI_PASS),
 *   而不是 NVS 历史值 —— 出厂/重新烧录后页面即显示默认 WiFi, 换其他 WiFi 直接在页面改 */
static esp_err_t handler_root(httpd_req_t *req)
{
    const bms_params_t *p = sys_params_get();
    /* H18: NVS 值可能被 /save 或 MQTT set_wifi 写入恶意内容,
     * 反射进 HTML 前必须转义, 否则构成存储型 XSS(默认 AP 密码 12345678 可被邻近攻击者利用) */
    char ssid_esc[200] = {0}, pass_esc[200] = {0}, cur_ssid_esc[200] = {0};
    char uri_esc[800] = {0}, cid_esc[400] = {0}, user_esc[400] = {0};
    html_escape(BMS_WIFI_SSID,   ssid_esc,    sizeof(ssid_esc));      /* 默认 WiFi(预填) */
    html_escape(BMS_WIFI_PASS,   pass_esc,    sizeof(pass_esc));      /* 默认密码(预填) */
    html_escape(p->wifi_ssid,    cur_ssid_esc, sizeof(cur_ssid_esc)); /* 当前已存值(提示) */
    html_escape(p->mqtt_uri,     uri_esc,     sizeof(uri_esc));
    html_escape(p->mqtt_client_id, cid_esc,   sizeof(cid_esc));
    html_escape(p->mqtt_user,    user_esc,    sizeof(user_esc));

    /* 2026-08-12: page 加大到 4608 — 表单新增 MQTT 用户名后,
     * snprintf 最大输出约 4237 字节, 原 4096 触发 -Werror=format-truncation
     * 2026-09-07: 改 static — 4.5KB 栈上分配占该任务 16KB 栈的 28%,
     *   httpd 配网任务栈仅 16KB, 挪到 static 释放栈空间
     *   (httpd 单 worker 串行处理, 无并发写入风险) */
    static char page[4608];
    snprintf(page, sizeof(page),
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>BMS 配网</title>"
        "<style>body{font-family:Arial;margin:0;padding:20px;background:#f5f5f5}"
        ".card{max-width:400px;margin:0 auto;background:#fff;padding:24px;border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,0.1)}"
        "h1{font-size:20px;color:#333}label{display:block;margin:12px 0 4px;font-size:14px;color:#555}"
        "input{width:100%%;padding:10px;border:1px solid #ddd;border-radius:4px;font-size:14px;box-sizing:border-box}"
        "button{width:100%%;padding:12px;margin-top:18px;background:#1976D2;color:#fff;border:none;border-radius:4px;font-size:16px;cursor:pointer}"
        "button:hover{background:#1565C0}.tip{font-size:12px;color:#888;margin-top:4px}"
        ".status{padding:8px;background:#E3F2FD;border-radius:4px;font-size:13px;margin-bottom:12px}"
        ".cur{font-size:11px;color:#999;margin-top:2px}"
        "</style></head><body><div class='card'>"
        "<h1>🔋 BMS 电池管理系统</h1>"
        "<div class='status'>默认已填入出厂 WiFi, 如需更换请修改后保存, 保存后自动重启</div>"
        "<form action='/save' method='POST'>"
        "<label>WiFi 名称 (SSID)</label>"
        "<input name='ssid' value='%s' placeholder='如: MyHomeWiFi' required>"
        "<div class='cur'>当前已存: %s</div>"
        "<label>WiFi 密码</label>"
        "<input name='pass' type='password' value='%s'>"
        "<label>MQTT 服务器地址 (华为云 IoTDA)</label>"
        "<input name='mqtt_uri' value='%s'>"
        "<div class='tip'>如: mqtts://{实例ID}.iotda-app.cn-south-4.myhuaweicloud.com:8883</div>"
        "<label>设备 ID (device_id)</label>"
        "<input name='client_id' value='%s' placeholder='如: <IOTDA_DEVICE_ID>_BMS001'>"
        "<div class='cur'>当前: %s</div>"
        "<label>设备密钥 (device_secret, IoTDA 鉴权用)</label>"
        "<input name='mqtt_secret' type='password' placeholder='华为云 IoTDA 控制台复制的设备密钥'>"
        "<div class='tip'>留空则保持当前密钥不变; 首次必须填写真实密钥(默认 emqx-no-secret 无效)</div>"
        "<button type='submit'>💾 保存并重启</button>"
        "</form>"
        "<p style='text-align:center;margin-top:16px;font-size:12px;color:#888'>"
        "ESP32-S3 BMS v1.0 &copy; 2026</p>"
        "</div></body></html>",
        ssid_esc,
        cur_ssid_esc,
        pass_esc,
        uri_esc,
        cid_esc,
        cid_esc);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ====== HTTP handler: POST /save 保存配置 ====== */
static esp_err_t handler_save(httpd_req_t *req)
{
    /* 读取 POST body(缓冲区 512, 超出部分丢弃避免 httpd 内部堆积) */
    char buf[512] = {0};
    int total = req->content_len < (int)sizeof(buf) - 1 ? req->content_len : (int)sizeof(buf) - 1;
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) break;
        received += ret;
    }
    buf[received] = '\0';
    /* 若 Content-Length 大于缓冲区, 丢弃剩余数据防止连接异常 */
    if (req->content_len > (int)sizeof(buf) - 1) {
        char discard[128];
        int remain = req->content_len - received;
        while (remain > 0) {
            int n = httpd_req_recv(req, discard, remain < (int)sizeof(discard) ? remain : (int)sizeof(discard));
            if (n <= 0) break;
            remain -= n;
        }
    }

    /* 解析字段 (mqtt_uri 缓冲区必须足够大, 否则会截断 IoTDA 地址) */
    char ssid[32] = {0}, pass[64] = {0}, mqtt_uri[128] = {0};
    char client_id[64] = {0}, mqtt_secret[64] = {0};
    parse_form_field(buf, "ssid", ssid, sizeof(ssid));
    parse_form_field(buf, "pass", pass, sizeof(pass));
    parse_form_field(buf, "mqtt_uri", mqtt_uri, sizeof(mqtt_uri));
    parse_form_field(buf, "client_id", client_id, sizeof(client_id));
    parse_form_field(buf, "mqtt_secret", mqtt_secret, sizeof(mqtt_secret));

    ESP_LOGI(TAG, "保存配置: ssid=%s mqtt_uri=%s client_id=%s", ssid, mqtt_uri, client_id);

    /* 保存到 NVS (H18: strncpy 后显式补 '\0', 边界长度输入时保证终止) */
    bms_params_t params = *sys_params_get();
    strncpy(params.wifi_ssid, ssid, sizeof(params.wifi_ssid) - 1);
    params.wifi_ssid[sizeof(params.wifi_ssid) - 1] = '\0';
    strncpy(params.wifi_pass, pass, sizeof(params.wifi_pass) - 1);
    params.wifi_pass[sizeof(params.wifi_pass) - 1] = '\0';
    if (strlen(mqtt_uri) > 0) {
        strncpy(params.mqtt_uri, mqtt_uri, sizeof(params.mqtt_uri) - 1);
        params.mqtt_uri[sizeof(params.mqtt_uri) - 1] = '\0';
    }
    if (strlen(client_id) > 0) {
        strncpy(params.mqtt_client_id, client_id, sizeof(params.mqtt_client_id) - 1);
        params.mqtt_client_id[sizeof(params.mqtt_client_id) - 1] = '\0';
    }
    /* 华为云 IoTDA 设备密钥: 留空=保持当前(首次须填写真实密钥, 默认 emqx-no-secret 无效) */
    if (strlen(mqtt_secret) > 0) {
        strncpy(params.mqtt_device_secret, mqtt_secret, sizeof(params.mqtt_device_secret) - 1);
        params.mqtt_device_secret[sizeof(params.mqtt_device_secret) - 1] = '\0';
    }
    /* 兼容清理: 清空旧 EMQX 遗留的 user/pass(IoTDA 标准鉴权不使用, 避免混淆) */
    params.mqtt_user[0] = '\0';
    params.mqtt_pass[0] = '\0';
    params.wifi_configured = 1;

    bms_err_t err = sys_params_set(&params);

    /* 2026-08-13 修复: 保存后立即重启前强制落盘. 否则若距上次 NVS 写不足
     * 2s(去抖窗口), set 只置脏不写 Flash, 1s 后 esp_restart() 重启时
     * 新 WiFi 配置丢失, 设备继续用旧配置连旧路由器. */
    if (err == BMS_OK) {
        err = sys_params_flush_now();
    }

    /* 回复页面 */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (err == BMS_OK) {
        s_config_done = true;
        httpd_resp_send(req, SAVE_OK_HTML, HTTPD_RESP_USE_STRLEN);
        /* 延迟 1s 重启, 让 HTTP 响应先发出去 */
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "配网成功, 重启中...");
        esp_restart();
    } else {
        httpd_resp_send(req, "<h2>❌ 保存失败, 请重试</h2><a href='/'>返回</a>",
                        HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/* ====== HTTP handler: GET /status 当前状态 JSON ====== */
static esp_err_t handler_status(httpd_req_t *req)
{
    const bms_params_t *p = sys_params_get();
    /* H18: JSON 字符串转义, 防 SSID 含引号/反斜杠时 JSON 损坏 */
    char ssid_esc[200] = {0}, uri_esc[800] = {0}, cid_esc[400] = {0};
    json_escape(p->wifi_ssid,      ssid_esc, sizeof(ssid_esc));
    json_escape(p->mqtt_uri,       uri_esc,  sizeof(uri_esc));
    json_escape(p->mqtt_client_id, cid_esc,  sizeof(cid_esc));

    char json[2048];   /* 转义后的值可能接近 1500 字节, 缓冲需足够大防 snprintf 溢出 */
    snprintf(json, sizeof(json),
             "{\"configured\":%s,\"ssid\":\"%s\",\"mqtt_uri\":\"%s\","
             "\"client_id\":\"%s\",\"firmware\":\"%s\"}",
             p->wifi_configured ? "true" : "false",
             ssid_esc, uri_esc, cid_esc,
             p->firmware_version);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ====== HTTP handler: GET /scan 扫描周边 WiFi ====== */
static esp_err_t handler_scan(httpd_req_t *req)
{
    /* 切换到 STA 模式扫描(若已 AP 模式, 临时切换可能失败, 这里简化处理) */
    wifi_scan_config_t scan_cfg = { 0 };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"scan failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num > 20) ap_num = 20;                       // 限制最多 20 个

    wifi_ap_record_t *aps = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (aps == NULL) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"no memory\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&ap_num, aps);

    /* 拼 JSON */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"count\":");
    char num[8];
    snprintf(num, sizeof(num), "%u", (unsigned)ap_num);
    httpd_resp_sendstr_chunk(req, num);
    httpd_resp_sendstr_chunk(req, ",\"aps\":[");
    for (uint16_t i = 0; i < ap_num; i++) {
        /* H18: 扫描到的 SSID 可能含引号/控制字符, JSON 转义防损坏 */
        char ssid_esc[200] = {0};
        json_escape((const char *)aps[i].ssid, ssid_esc, sizeof(ssid_esc));
        char item[256];
        snprintf(item, sizeof(item),
                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                 i ? "," : "", ssid_esc, aps[i].rssi, aps[i].authmode);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);                 // 结束 chunk
    free(aps);
    return ESP_OK;
}

/* ====== HTTP handler: GET /reset 清除配置 ====== */
static esp_err_t handler_reset(httpd_req_t *req)
{
    sys_wifi_clear_config();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<h2>配置已清除, 3 秒后重启...</h2>"
        "<p>请重新连接 BMS_Config 热点配网</p>", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return ESP_OK;
}

/* ====== HTTP handler: Captive Portal 探测响应 ====== */
static esp_err_t handler_redirect(httpd_req_t *req)
{
    /* Android/iOS/Windows 探测到 302 跳转后自动弹出浏览器 */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ====== HTTP handler: iOS Captive Portal 探测(返回 200) ====== */
static esp_err_t handler_hotspot(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<html><head><title>Success</title></head><body>Success</body></html>",
        HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ====== 启动 HTTP Server ====== */
static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;
    /* 2026-08-13 修复: 8192→16384. handler_root 局部缓冲 ~6.6KB(page[4608]+转义缓冲),
     * 加上 snprintf/框架调用链, 原 8192 栈极易溢出 → 配网页打不开(连接后页面无响应).
     * 16384 提供足够余量. */
    config.stack_size = 16384;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP Server 启动失败");
        return ESP_FAIL;
    }

    /* 注册 URI 处理器 */
    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = handler_root};
    static const httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = handler_save};
    static const httpd_uri_t uri_status = {
        .uri = "/status", .method = HTTP_GET, .handler = handler_status};
    static const httpd_uri_t uri_scan = {
        .uri = "/scan", .method = HTTP_GET, .handler = handler_scan};
    static const httpd_uri_t uri_reset = {
        .uri = "/reset", .method = HTTP_GET, .handler = handler_reset};
    static const httpd_uri_t uri_204 = {
        .uri = "/generate_204", .method = HTTP_GET, .handler = handler_redirect};
    static const httpd_uri_t uri_hotspot = {
        .uri = "/hotspot.html", .method = HTTP_GET, .handler = handler_hotspot};

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_save);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_scan);
    httpd_register_uri_handler(s_server, &uri_reset);
    httpd_register_uri_handler(s_server, &uri_204);
    httpd_register_uri_handler(s_server, &uri_hotspot);

    ESP_LOGI(TAG, "HTTP Server 已启动, 访问 http://192.168.4.1 配网");
    return ESP_OK;
}

/* ================================================================ */
bms_err_t sys_config_portal_start(uint32_t timeout_ms)
{
    s_start_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_config_done   = false;

    if (start_http_server() != ESP_OK) {
        return BMS_ERR_FAIL;
    }

    /* 阻塞等待: 配网成功(s_config_done=true) 或 超时 */
    while (!s_config_done) {
        vTaskDelay(pdMS_TO_TICKS(500));

        if (timeout_ms > 0) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (now - s_start_time_ms > timeout_ms) {
                ESP_LOGW(TAG, "配网超时 %u ms, 退出", (unsigned)timeout_ms);
                sys_config_portal_stop();
                return BMS_ERR_TIMEOUT;
            }
        }
    }

    /* 走到这里说明 s_config_done=true, 但 handler_save 已调用 esp_restart()
     * 理论上不会返回, 加防御性返回 */
    return BMS_OK;
}

/* ================================================================ */
void sys_config_portal_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
