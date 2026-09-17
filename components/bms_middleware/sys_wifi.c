/**
 * @file    sys_wifi.c
 * @brief   WiFi 连接管理实现 (STA + AP 双模式)
 * @author  BMS Team
 * @date    2026-08
 * @note    工作流程:
 *          1. sys_wifi_init_sta(): 用 NVS 中的 SSID/密码 STA 模式连接路由器
 *          2. sys_wifi_start_ap(): 启动 AP 热点 + DHCP, 供配网门户使用
 *          3. 配网成功后调用 sys_wifi_save_config() 保存到 NVS 并重启
 *          4. 长按 BOOT 5s 强制进入 AP 配网模式 (由 app_tasks 检测)
 */
#include "sys_wifi.h"
#include "sys_params.h"
#include "bms_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "SYS_WIFI";

#define WIFI_CONNECTED_BIT   (1 << 0)
#define WIFI_FAIL_BIT        (1 << 1)
#define WIFI_AP_STARTED_BIT  (1 << 2)

static EventGroupHandle_t s_event_group = NULL;
static bool               s_connected   = false;
static bool               s_ap_mode     = false;
static esp_netif_t        *s_sta_netif  = NULL;
static esp_netif_t        *s_ap_netif   = NULL;
static int                s_retry_count = 0;
static esp_timer_handle_t s_reconnect_timer = NULL;   /* 非阻塞延迟重连定时器 */
static uint32_t           s_last_got_ip_ms = 0;        /* 上次获取IP时刻(ms), 用于稳定性判断 */

/* ====== 内部: 非阻塞延迟重连回调(由 esp_timer 触发) ====== */
static void reconnect_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "定时重连: 正在重新连接 WiFi...");
    esp_wifi_connect();
}

/* ====== 内部: 启动延迟重连定时器 ====== */
static void schedule_reconnect(int delay_ms)
{
    if (s_reconnect_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = reconnect_timer_cb,
            .arg = NULL,
            .name = "wifi_reconnect",
        };
        /* 2026-09-07: 检查 create 返回值 — 失败则本轮跳过,
         * 下次 schedule_reconnect 重试创建 */
        if (esp_timer_create(&timer_args, &s_reconnect_timer) != ESP_OK) {
            ESP_LOGW(TAG, "重连定时器创建失败, 跳过本轮重连");
            return;
        }
    }
    esp_timer_stop(s_reconnect_timer);   /* 避免重复定时 */
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
}

/* ====== WiFi 事件回调 ====== */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)data;
            s_connected = false;
            s_retry_count++;
            ESP_LOGW(TAG, "STA 断开, 原因码=%u (重试 %d/%d)", event->reason, s_retry_count, BMS_WIFI_MAX_RETRY);
            /* Bug2 修复: 所有重连都走非阻塞定时器, 避免立即重连导致反复断连
             * 初次重试: 500ms 短延迟(快速恢复)
             * 后续重试: 指数退避(1s/2s/4s/8s... 上限 30s) */
            int delay_ms;
            if (s_retry_count <= BMS_WIFI_MAX_RETRY) {
                delay_ms = 500;     /* 初次快速重试: 500ms */
            } else {
                int shift = s_retry_count - BMS_WIFI_MAX_RETRY;
                if (shift > 5) shift = 5;   /* 限制位移避免溢出 */
                delay_ms = 1000 * (1 << shift);
                if (delay_ms > 30000) delay_ms = 30000;  /* 上限 30s */
            }
            ESP_LOGW(TAG, "%dms 后重试 (第 %d 次)", delay_ms, s_retry_count);
            schedule_reconnect(delay_ms);
        } else if (id == WIFI_EVENT_AP_START) {
            ESP_LOGI(TAG, "AP 热点已启动: SSID=%s", BMS_AP_SSID);
            s_ap_mode = true;
            if (s_event_group) {
                xEventGroupSetBits(s_event_group, WIFI_AP_STARTED_BIT);
            }
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "设备连接到 AP, mac=" MACSTR ", aid=%d",
                     MAC2STR(event->mac), event->aid);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA 获取 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;

        /* 2026-08-12 修复: DHCP 下发的 DNS 可能无法解析华为云 IoTDA 域名
         * (症状: esp-tls getaddrinfo 返回 202, MQTT 一直连不上),
         * 获取 IP 后强制设置静态 DNS 兜底: 主=阿里 223.5.5.5, 备=谷歌 8.8.8.8.
         * 即使路由器 DHCP DNS 异常, 也能正常解析 IoTDA 接入点域名.
         * 注: v5.2 中 esp_ip4addr_aton(const char*) 直接返回 uint32_t 地址 */
        esp_netif_dns_info_t dns_main = {0}, dns_backup = {0};
        dns_main.ip.type = ESP_IPADDR_TYPE_V4;
        dns_main.ip.u_addr.ip4.addr = esp_ip4addr_aton("223.5.5.5");
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns_main);
        dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
        dns_backup.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns_backup);
        ESP_LOGI(TAG, "已设置静态 DNS 兜底: 223.5.5.5 / 8.8.8.8");
        /* Bug修复: 原代码每次 GOT_IP 都清零 s_retry_count, 导致弱信号场景下
         *         指数退避永远不生效, 设备陷入"断开->500ms重连->连上->断开"的快速震荡
         *         现在只有连接稳定超过 60 秒才清零计数器, 让指数退避能正常生效 */
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (s_last_got_ip_ms == 0 || (now_ms - s_last_got_ip_ms) > 60000) {
            s_retry_count = 0;            /* 连接稳定超过60s, 清零计数器 */
        } else {
            ESP_LOGW(TAG, "连接仅持续 %ums, 保留重连计数(%d)让退避生效",
                     (unsigned)(now_ms - s_last_got_ip_ms), s_retry_count);
        }
        s_last_got_ip_ms = now_ms;
        if (s_event_group) {
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

/* ====== 内部: 初始化 WiFi 子系统(只执行一次) ====== */
static void wifi_subsystem_init(void)
{
    static bool s_inited = false;
    if (s_inited) {
        return;
    }

    s_event_group = xEventGroupCreate();

    /* 网络接口与默认配置 */
    ESP_ERROR_CHECK(esp_netif_init());

    /* 创建默认事件循环(若未创建) */
    esp_err_t ev_ret = esp_event_loop_create_default();
    if (ev_ret != ESP_OK && ev_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ev_ret);
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        ESP_LOGE(TAG, "默认网络接口创建失败(STA=%p AP=%p), WiFi 不可用",
                 (void *)s_sta_netif, (void *)s_ap_netif);
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 禁用 WiFi 省电模式, 提升连接稳定性(避免默认 PS 导致掉线) */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* 注册事件回调 */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    s_inited = true;
}

/* ================================================================ */
bms_err_t sys_wifi_init_sta(void)
{
    wifi_subsystem_init();

    const bms_params_t *params = sys_params_get();
    if (strlen(params->wifi_ssid) == 0) {
        ESP_LOGE(TAG, "NVS 中无 SSID, 请先配网");
        return BMS_ERR_FAIL;
    }

    /* 配置 STA(使用 WPA/WPA2 混合模式提升兼容性, 全信道扫描) */
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = { .capable = true, .required = false },
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    strncpy((char *)wifi_cfg.sta.ssid, params->wifi_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, params->wifi_pass, sizeof(wifi_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA 连接中, SSID=%s ...", params->wifi_ssid);

    /* 清除旧标志 */
    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    /* 阻塞等待连接结果 */
    EventBits_t bits = xEventGroupWaitBits(
        s_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(BMS_WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA 连接成功");
        return BMS_OK;
    }

    ESP_LOGW(TAG, "STA 连接超时/失败, 继续降级运行");
    return BMS_ERR_FAIL;
}

/* ================================================================ */
bms_err_t sys_wifi_start_ap(void)
{
    wifi_subsystem_init();

    /* 配置 AP */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len = strlen(BMS_AP_SSID),
            .channel  = BMS_AP_CHANNEL,
            .max_connection = BMS_AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
            .ssid_hidden = 0,
            .beacon_interval = 100,
        },
    };
    memcpy(ap_cfg.ap.ssid, BMS_AP_SSID, strlen(BMS_AP_SSID));
    /* 密码为空表示开放热点 */
    if (strlen(BMS_AP_PASS) >= 8) {
        strncpy((char *)ap_cfg.ap.password, BMS_AP_PASS, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 等待 AP 启动事件 */
    xEventGroupWaitBits(s_event_group, WIFI_AP_STARTED_BIT,
                        pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));

    /* 打印 AP 信息 */
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "AP 网关 IP: " IPSTR, IP2STR(&ip_info.ip));
    }
    ESP_LOGI(TAG, "请连接 WiFi '%s' 后访问 http://192.168.4.1 配网", BMS_AP_SSID);

    s_ap_mode = true;
    return BMS_OK;
}

/* ================================================================ */
bms_err_t sys_wifi_save_config(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL || strlen(ssid) == 0) {
        return BMS_ERR_PARAM_INVALID;
    }

    bms_params_t params = *sys_params_get();
    strncpy(params.wifi_ssid, ssid, sizeof(params.wifi_ssid) - 1);
    params.wifi_ssid[sizeof(params.wifi_ssid) - 1] = '\0';
    strncpy(params.wifi_pass, pass, sizeof(params.wifi_pass) - 1);
    params.wifi_pass[sizeof(params.wifi_pass) - 1] = '\0';
    params.wifi_configured = 1;

    bms_err_t err = sys_params_set(&params);
    /* 2026-08-13 修复: set_wifi 保存后可能立即重启(MQTT 命令或门户),
     * 去抖窗口内 set 只置脏不落盘, 重启会丢配置; 此处强制立即写 Flash. */
    if (err == BMS_OK) {
        err = sys_params_flush_now();
    }
    if (err == BMS_OK) {
        ESP_LOGI(TAG, "WiFi 配置已保存, 即将重启进入 STA 模式");
    }
    return err;
}

/* ================================================================ */
bms_err_t sys_wifi_clear_config(void)
{
    bms_params_t params = *sys_params_get();
    params.wifi_configured = 0;
    params.wifi_ssid[0] = '\0';
    params.wifi_pass[0] = '\0';
    return sys_params_set(&params);
}

/* ====== 重新烧录检测: 串口烧录新固件后自动清配置进 AP 配网 ======
 * 2026-08-13 新增(需求: 重新烧录时自动重新配网, OTA 升级不受影响)
 * 2026-08-13 B7 修复: 原方案用 esp_ota_get_state_partition()==PENDING_VERIFY
 *   判断"OTA 升级后首启"——但串口烧录(idf.py flash)会把 otadata 分区清空
 *   (ota_data_initial.bin 全 0xFF), 且启用 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE,
 *   bootloader 首次启动同样把运行分区置为 PENDING_VERIFY, 导致烧录被误判为
 *   OTA → 保留旧配置不配网. 现改为 OTA 成功路径显式写 NVS 标记(ota_upgraded),
 *   启动时仅凭标记区分: 有标记=OTA(保留配置), 无标记=串口烧录/正常重启(比指纹).
 * 判定逻辑:
 *   - ota_upgraded 标记存在       → OTA 升级后首启 → 保留配置, 清标记, 刷指纹
 *   - 指纹与 NVS 记录不同         → 串口重新烧录 → 清 WiFi 配置(进 AP 配网)
 *   - 无指纹记录但已有 WiFi 配置  → 旧固件遗留配置 + 新固件首启 → 清配置重配网
 *   - 指纹相同 / 无配置           → 正常重启或全新 → 不动 */
bms_err_t sys_wifi_check_reflash(void)
{
#if BMS_REPROVISION_ON_REFLASH
    /* 当前编译指纹: 版本 + 编译日期 + 编译时间.
     * 用 esp_app_get_description() 而非 __DATE__/__TIME__: project_elf_src 由构建系统
     * 每次构建重新生成(含最新编译时间), 即使 ccache/增量编译命中也会更新,
     * 保证每次 idf.py build 后指纹必然变化 → 重新烧录必触发重新配网. */
    const esp_app_desc_t *desc = esp_app_get_description();
    char cur_fp[96];
    snprintf(cur_fp, sizeof(cur_fp), "%s|%s|%s",
             (desc != NULL && desc->version[0] != '\0') ? desc->version : BMS_FIRMWARE_VERSION,
             (desc != NULL) ? desc->date : __DATE__,
             (desc != NULL) ? desc->time : __TIME__);

    /* 1. OTA 升级后首启(OTA 成功路径显式留标记): 保留 WiFi 配置 */
    if (sys_params_is_ota_upgraded()) {
        ESP_LOGI(TAG, "OTA 升级后首次启动, 保留 WiFi 配置 (标记已消费, 指纹已刷新)");
        sys_params_clear_ota_upgraded();
        sys_params_set_reflash_fp(cur_fp);
        return BMS_OK;
    }

    /* 2. 非 OTA(串口烧录 / 正常重启): 比对指纹判断是否重新烧录 */
    char last_fp[96] = {0};
    bms_err_t err = sys_params_get_reflash_fp(last_fp, sizeof(last_fp));
    bool need_clear = false;
    if (err == BMS_OK) {
        need_clear = (strcmp(last_fp, cur_fp) != 0);   /* 指纹不同 = 烧录了新固件 */
    } else {
        /* 无指纹记录: 首次运行带检测功能的固件. 若 NVS 里已有 WiFi 配置
         * (旧固件遗留), 说明固件被更换过, 同样应重新配网 */
        const bms_params_t *p = sys_params_get();
        need_clear = (p != NULL && p->wifi_configured);
    }
    if (need_clear) {
        ESP_LOGW(TAG, "检测到固件重新烧录 (%s -> %s), 清除 WiFi 配置, 进入 AP 配网",
                 (err == BMS_OK) ? last_fp : "(无记录)", cur_fp);
        sys_wifi_clear_config();
        sys_params_flush_now();          /* 立即落盘, 防止重启/复位前丢失 */
    }
    /* 记录当前指纹(下次启动比对用) */
    sys_params_set_reflash_fp(cur_fp);
#endif
    return BMS_OK;
}

/* ================================================================ */
bool sys_wifi_is_connected(void)
{
    return s_connected;
}

/* ================================================================ */
bool sys_wifi_is_ap_mode(void)
{
    return s_ap_mode;
}

/* ================================================================ */
bms_err_t sys_wifi_get_ip_str(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < 16) {
        return BMS_ERR_PARAM_INVALID;
    }

    if (s_connected && s_sta_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
            snprintf(buf, buf_len, IPSTR, IP2STR(&ip_info.ip));
            return BMS_OK;
        }
    }
    if (s_ap_mode && s_ap_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
            snprintf(buf, buf_len, IPSTR, IP2STR(&ip_info.ip));
            return BMS_OK;
        }
    }
    snprintf(buf, buf_len, "0.0.0.0");
    return BMS_ERR_FAIL;
}

/* ================================================================ */
bms_err_t sys_wifi_get_mac_str(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < 18) {
        return BMS_ERR_PARAM_INVALID;
    }
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        snprintf(buf, buf_len, "00:00:00:00:00:00");
        return BMS_ERR_FAIL;
    }
    snprintf(buf, buf_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return BMS_OK;
}

/* ================================================================ */
/* 旧接口兼容: 直接转调用 sys_wifi_init_sta() */
bms_err_t sys_wifi_init(void)
{
    return sys_wifi_init_sta();
}
