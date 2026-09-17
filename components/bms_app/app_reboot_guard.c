/**
 * @file    app_reboot_guard.c
 * @brief   重启命令幂等防护(4 层防线, 防止 MQTT QoS1 重投导致重启死循环)
 * @author  BMS Team
 * @date    2026-08
 * @note    原位于 app_tasks.c, 2026-08-09 抽出为独立模块以降低耦合
 *          4 层防线: 独立延迟任务 / 优雅断开 MQTT / RTC 指纹幂等 / 启动保护期
 */
#include "app_reboot_guard.h"

#include <stdio.h>
#include <string.h>
#include <time.h>        /* time() — H10: epoch 秒作为跨重启比较基准 */
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "sys_mqtt.h"
#if __has_include("esp_attr.h")
#  include "esp_attr.h"    /* RTC_DATA_ATTR 宏 */
#endif

static const char *TAG = "REBOOT_GUARD";

/* ================================================================
 * ======== Bug 修复: 重启命令幂等保护(防止反复重启死循环) ========
 * ================================================================
 * 根因:  MQTT QoS=1 时, broker 要求设备回 PUBACK 才视为"消息送达".
 *        原代码在 MQTT 回调线程里直接 vTaskDelay(3s) 然后 esp_restart(), 导致:
 *         1) PUBACK 没发出去 → broker 超时后重投这条 restart 消息
 *         2) 设备重启连上 MQTT 后, 又收到刚才那条 restart → 再次重启 → 死循环!
 *
 * 方案(4 层防线):
 *   [防线1]  不在 MQTT 回调线程里 delay/esp_restart, 改为创建低优先级 delayed_reboot_task.
 *           让 MQTT 线程能继续发 PUBACK + DISCONNECT.
 *   [防线2]  重启前先显式调用 sys_mqtt_report_info() + sys_mqtt_stop()
 *           - publish 一条 "我要重启了" 状态, Dashboard 侧及时更新
 *           - 发送 MQTT DISCONNECT 正常下线, broker 取消 QoS 重投计时器
 *   [防线3]  用 RTC_DATA_ATTR 内存(软重启/深睡后仍然保留) 保存
 *           最近一次执行重启命令的时刻 + 命令指纹.
 *   [防线4]  新启动后 60s 内, 如果收到与上次指纹相同的 restart 命令, 直接
 *           DROP 丢弃, 不执行重启 → 即使 broker 仍然重投了, 也不会循环.
 * ================================================================ */

/* RTC 内存里的重启保护结构体 (RTC_DATA_ATTR = 软重启后仍保留值) */
#ifndef RTC_DATA_ATTR
#  define RTC_DATA_ATTR      /* 编译器不支持时退化为普通 static, 仅防编译失败 */
#endif
typedef struct {
    uint32_t magic;                          /* 0xBEEF5A5A 表示有效 */
    uint32_t boot_millis_at_exec;            /* 执行重启时的 esp_log_timestamp() */
    uint32_t epoch_sec_at_exec;              /* 执行重启时的 unix 秒, 0 = 未知 */
    uint16_t grace_period_sec;               /* 启动后多少秒内属于保护期 */
    char     last_cmd_fingerprint[64];       /* 上次重启命令的识别串: id前32B + timestamp */
} rtc_reboot_guard_t;

RTC_DATA_ATTR static rtc_reboot_guard_t s_rtc_rb_guard = {0};
#define RB_MAGIC           0xBEEF5A5AU
#define RB_DEFAULT_GRACE   60U               /* 默认保护期: 启动后 60s */
#define RB_MARK_EXECUTING  0xDEADBEEFU       /* 临时标记: 准备进入重启流程 */


/* ---- 内部: 从命令 JSON 计算 63 字节指纹 ----
 *   指纹 = "#" + id 前 32 字节 + "#" + timestamp 数字 + "\0"
 *   用于: 判断 "这次收到的 restart 是否与上次执行的是同一条消息(被 broker 重投)" */
static void cmd_make_fingerprint(const cJSON *root, char out[64])
{
    out[0] = '\0';
    const cJSON *jid = NULL;
    const cJSON *jts = NULL;
    /* 尝试 顶层 id, 以及顶层 root.request_id / id 字段 */
    jid = cJSON_GetObjectItem(root, "id");
    if (jid == NULL) jid = cJSON_GetObjectItem(root, "request_id");
    if (jid == NULL) {
        /* 再找 content.content 里面嵌套的(unwrap 前的消息格式) */
        const cJSON *c1 = cJSON_GetObjectItem(root, "content");
        if (cJSON_IsObject(c1)) {
            jid = cJSON_GetObjectItem(c1, "id");
        }
    }
    jts = cJSON_GetObjectItem(root, "timestamp");
    if (jts == NULL) {
        const cJSON *p = cJSON_GetObjectItem(root, "paras");
        if (p) jts = cJSON_GetObjectItem(p, "timestamp");
    }
    /* 拼接 */
    int pos = 0;
    out[pos++] = '#';
    if (cJSON_IsString(jid) && jid->valuestring) {
        strncat(out + pos, jid->valuestring, 32);
        pos = (int)strlen(out);
    } else if (cJSON_IsNumber(jid)) {
        snprintf(out + pos, 64 - pos, "n%d", jid->valueint);
        pos = (int)strlen(out);
    } else {
        strncat(out + pos, "no_id", 32);
        pos = (int)strlen(out);
    }
    out[pos++] = '#';
    if (cJSON_IsNumber(jts)) {
        snprintf(out + pos, 64 - pos, "%ld", (long)jts->valuedouble);
    } else if (cJSON_IsString(jts) && jts->valuestring) {
        strncat(out + pos, jts->valuestring, 63 - pos);
    } else {
        snprintf(out + pos, 64 - pos, "ts%u", (unsigned)esp_log_timestamp());
    }
    out[63] = '\0';
}


/* ---- 内部: 检查当前重启命令是否"已执行过"? 返回 true = 丢弃 ---- */
static bool reboot_guard_is_duplicate(const cJSON *root)
{
    /* RTC 内存 magic 不对 → 首次上电或硬复位 → 不是重复 */
    if (s_rtc_rb_guard.magic != RB_MAGIC) return false;

    char fp[64];
    cmd_make_fingerprint(root, fp);

    /* H10 修复: 原实现用 now_ms - boot_millis_at_exec 计算距上次重启的间隔,
     * 但 esp_log_timestamp() 是"本次 boot 内"的毫秒计数, 软重启后从 0 重新计时,
     * 与上一次 boot 记录的值相减必然 uint32 下溢成 ~42.9 亿 ms, 恒大于保护期,
     * 导致防线4(启动保护期)永久失效, 防重启死循环形同虚设.
     * 现在:
     *   1) SNTP 已同步时优先用 epoch 秒(跨重启单调, 精确比较);
     *   2) 无 epoch 时回退毫秒比较, 并对"跨 boot"(now < boot_millis)做下溢保护:
     *      视为刚重启(0s), 使新 boot 后的保护期仍然生效. */
    uint32_t since_exec_s = 0;
    time_t now_epoch = time(NULL);
    if (s_rtc_rb_guard.epoch_sec_at_exec > 0 && now_epoch > 1609459200) {
        /* SNTP 已同步: epoch 差即真实间隔 */
        uint32_t cur = (uint32_t)now_epoch;
        since_exec_s = (cur > s_rtc_rb_guard.epoch_sec_at_exec)
                       ? (cur - s_rtc_rb_guard.epoch_sec_at_exec) : 0;
    } else {
        /* 无 epoch: 毫秒比较, 带跨 boot 下溢保护 */
        uint32_t now_ms = (uint32_t)esp_log_timestamp();
        if (now_ms >= s_rtc_rb_guard.boot_millis_at_exec) {
            since_exec_s = (now_ms - s_rtc_rb_guard.boot_millis_at_exec) / 1000U;
        } else {
            since_exec_s = 0;   /* 新 boot: 计数已重置, 仍在保护期内 */
        }
    }

    /* 保护期已过? (正常情况: 启动 >60s 后, 新的命令是真正想重启的) */
    if (s_rtc_rb_guard.grace_period_sec > 0 && since_exec_s > s_rtc_rb_guard.grace_period_sec) {
        return false;
    }
    /* 在保护期内, 且指纹匹配上次执行的命令? → 就是 broker 重投的那条, DROP */
    if (strcmp(s_rtc_rb_guard.last_cmd_fingerprint, fp) == 0) {
        ESP_LOGW(TAG, "[重启·幂等] 检测到重复重启命令 (启动后 %us < 保护期 %us, 指纹相同) → 丢弃!",
                 (unsigned)since_exec_s,
                 (unsigned)s_rtc_rb_guard.grace_period_sec);
        return true;
    }
    return false;
}


/* ---- 内部: 标记"这条 restart 命令将要执行" → 写入 RTC 内存 ---- */
static void reboot_guard_mark_executing(const cJSON *root)
{
    char fp[64];
    cmd_make_fingerprint(root, fp);

    s_rtc_rb_guard.magic               = RB_MAGIC;
    s_rtc_rb_guard.boot_millis_at_exec = (uint32_t)esp_log_timestamp();
    /* H10: 同步记录 epoch 秒(SNTP 已同步时有效), 供跨重启精确比较; 未同步则为 0, 走毫秒回退 */
    time_t now = time(NULL);
    s_rtc_rb_guard.epoch_sec_at_exec   = (now > 1609459200) ? (uint32_t)now : 0;
    s_rtc_rb_guard.grace_period_sec    = RB_DEFAULT_GRACE;
    memcpy(s_rtc_rb_guard.last_cmd_fingerprint, fp, sizeof(s_rtc_rb_guard.last_cmd_fingerprint));
    s_rtc_rb_guard.last_cmd_fingerprint[63] = '\0';
}


/* ---- 独立任务: 延迟 + 优雅断开 MQTT + 重启 ----
 * (此任务创建后自销毁, 无需外部同步) */
static void delayed_reboot_task(void *arg)
{
    (void)arg;
    /* 第 1 段 500ms: 让 MQTT 事件线程继续运行发 PUBACK / send_cmd_resp */
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "[重启·任务] 阶段1/3: 上报最新状态 (便于Dashboard看到重启前状态)");
    /* 即使发布失败也没关系, 继续下一步 */
    (void)sys_mqtt_report_info();
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "[重启·任务] 阶段2/3: 优雅断开MQTT (DISCONNECT + 停止客户端)...");
    /* 关键: 正常断开. 让华为云 broker 知道设备正常下线, 取消 QoS1 消息重投 */
    sys_mqtt_stop();
    /* 给 DISCONNECT 包飞出去 + TCP FIN 关闭预留时间 */
    vTaskDelay(pdMS_TO_TICKS(1200));

    ESP_LOGI(TAG, "[重启·任务] 阶段3/3: esp_restart()");
    /* 刷新串口缓冲, 防止最后几条日志丢 */
    fflush(stdout);
    esp_restart();
    /* 不应到达; 防御性自杀 */
    vTaskDelete(NULL);
}


/* ---- 重启命令处理入口(在 MQTT 事件线程中调用, 必须非阻塞) ---- */
void handle_cmd_restart(const cJSON *root)
{
    /* 防线4: RTC 幂等保护 → 重复命令直接丢 */
    if (reboot_guard_is_duplicate(root)) {
        ESP_LOGW(TAG, "[重启] 本条命令已在 %us 内执行过, 忽略 (防重启循环)",
                 (unsigned)RB_DEFAULT_GRACE);
        return;
    }

    /* 防线3: 把命令指纹写入 RTC → 下次重启后即使收到重投也不会再执行 */
    reboot_guard_mark_executing(root);

    ESP_LOGI(TAG, "[重启] 已登记重启指纹, 创建延迟任务 (将在 ~2s 后正式重启, MQTT 会优雅下线)");

    /* 防线1: 创建独立任务执行延迟 + 重启, 当前线程立即返回, 不阻塞 MQTT */
    TaskHandle_t tsk = NULL;
    BaseType_t ok = xTaskCreate(delayed_reboot_task, "rb_delayed", 4096, NULL,
                                tskIDLE_PRIORITY + 1, &tsk);
    if (ok != pdPASS || tsk == NULL) {
        /* 任务创建失败? → 退化方案: 直接等 2s 再重启, 但先尽量发 DISCONNECT */
        ESP_LOGE(TAG, "[重启] 创建延迟任务失败 (xTaskCreate 返回 %d) → 退化直接重启", (int)ok);
        vTaskDelay(pdMS_TO_TICKS(1000));
        sys_mqtt_stop();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    /* 任务创建成功: 立即返回 MQTT 事件循环, 让 PUBACK 发出去! */
}
