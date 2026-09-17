/**
 * @file    sys_offline_cache.c
 * @brief   断网数据缓存与补传实现(SPIFFS)
 * @author  BMS Team
 * @date    2026-08
 * @note    使用 SPIFFS 分区缓存 MQTT 断开期间的数据,
 *          恢复连接后按时间顺序补传, 补传完成后清空缓存
 */
#include "sys_offline_cache.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "OFFLINE_CACHE";
static const char *CACHE_FILE = "/spiffs/cache.dat";
static bool s_mounted = false;

/* 缓存文件单行最大长度(属性上报 JSON 缓冲为 1536 字节, 见 sys_mqtt.c)
 * H5 修复: 原 512 会截断 6S(~690B)/32S(>1KB) 的上报 JSON, 导致补传全部失败
 * 且缓存仍被删除, 断网数据静默丢失 */
#define CACHE_LINE_MAX  1536

/* H22 修复: 单批补传条数上限 — 原全量补传逐条 vTaskDelay(100ms),
 * 断网时间长时缓存数千条, 在 MQTT 事件线程内同步补传会阻塞 keepalive
 * 数十秒, 被服务端判定超时踢下线("认证成功后又断开"死循环).
 * 现每批最多 20 条(约 2s), 剩余保留在文件中, 由主循环下一周期继续. */
#define CACHE_REPLAY_BATCH_MAX  20

/* #3 上限保护: 缓存文件最大字节数(约 400 条 6S 上报).
 * 2026-08-19 断网优化: 300KB→600KB. 原 300KB 按 7s 上报间隔只覆盖约 23 分钟
 *   断网, 超过即丢最旧数据; SPIFFS 分区 1MB(partitions.csv 0xE20000/0x100000),
 *   600KB 覆盖约 46 分钟断网(400+ 条), 仍留 40% 分区余量防写满.
 * 长时间断网(1s/条)若不限制会无限增长撑爆 SPIFFS 分区. 超限时丢弃最旧一行. */
#define CACHE_MAX_BYTES         (600 * 1024)

int sys_offline_cache_init(void)
{
    if (s_mounted) {
        return 0;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 3,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS 挂载失败: %s", esp_err_to_name(ret));
        return -1;
    }

    s_mounted = true;

    /* 打印 SPIFFS 使用情况 */
    size_t total = 0, used = 0;
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS 已挂载: 总计 %uKB, 已用 %uKB", (unsigned)(total / 1024), (unsigned)(used / 1024));
    }

    /* 检查是否有遗留缓存(上次断电未补传) */
    struct stat st;
    if (stat(CACHE_FILE, &st) == 0 && st.st_size > 0) {
        ESP_LOGW(TAG, "发现遗留缓存 %d 字节, 将在 MQTT 连接后补传", (int)st.st_size);
    }

    return 0;
}

static void cache_trim_oldest(void);   /* 前置声明: 供 sys_offline_cache_save 超限裁剪调用 */

int sys_offline_cache_save(const char *json_str)
{
    if (!s_mounted || json_str == NULL) {
        return -1;
    }

    /* 追加模式打开文件 */
    FILE *f = fopen(CACHE_FILE, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "无法打开缓存文件");
        return -1;
    }

    /* 写入一行 JSON + 换行 */
    fputs(json_str, f);
    fputc('\n', f);
    fclose(f);

    /* #3 上限保护: 超限丢弃最旧一行, 防 SPIFFS 撑爆 */
    struct stat fs;
    if (stat(CACHE_FILE, &fs) == 0 && fs.st_size > CACHE_MAX_BYTES) {
        ESP_LOGW(TAG, "缓存超限 %d/%dKB, 丢弃最旧一行", (int)(fs.st_size / 1024), CACHE_MAX_BYTES / 1024);
        cache_trim_oldest();
    }

    return 0;
}

/* 流式丢弃最旧一行: 读原文件跳过首行, 其余写入临时文件后 rename 覆盖.
 * 不把整文件载入内存, 避免 ESP32 堆压力; 用于 #3 超限裁剪. */
static void cache_trim_oldest(void)
{
    FILE *f = fopen(CACHE_FILE, "r");
    if (f == NULL) {
        return;
    }
    char tmp[strlen(CACHE_FILE) + 8];   /* CACHE_FILE 为指针, sizeof 仅 4, 需按 strlen 定长 */
    snprintf(tmp, sizeof(tmp), "%s.tmp", CACHE_FILE);
    FILE *tf = fopen(tmp, "w");
    if (tf == NULL) {
        fclose(f);
        return;
    }
    char line[CACHE_LINE_MAX];
    bool skip_first = true;
    bool wrote = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (skip_first) {        /* 丢弃最旧一行 */
            skip_first = false;
            continue;
        }
        fwrite(line, 1, strlen(line), tf);
        wrote = true;
    }
    fclose(f);
    if (fclose(tf) == 0) {
        if (wrote) {
            rename(tmp, CACHE_FILE);
        } else {
            remove(CACHE_FILE);
            remove(tmp);
        }
    } else {
        remove(tmp);
    }
}

int sys_offline_cache_replay(int (*send_func)(const char *topic, const char *json))
{
    if (!s_mounted || send_func == NULL) {
        return -1;
    }

    /* 检查文件是否存在 */
    struct stat st;
    if (stat(CACHE_FILE, &st) != 0 || st.st_size == 0) {
        return 0;   /* 无缓存, 正常返回 */
    }

    ESP_LOGI(TAG, "开始补传缓存数据 (%d 字节)...", (int)st.st_size);

    /* 打开缓存文件读取 */
    FILE *f = fopen(CACHE_FILE, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "无法读取缓存文件");
        return -1;
    }

    /* #2 修复: 用临时文件承接"需保留的补传数据".
     *   原实现每周期从文件头重读前 20 行, 发完若文件 >20 行就保留原文件、
     *   下周期再从头读 → 前 20 条无限重发、第 21 条之后永远发不出去、缓存清不掉.
     *   现: 本批成功发送的行丢弃; 失败的该行及其之后、以及超出本批上限的后续行
     *   写入临时文件, 结束后用临时文件覆盖(或为空则删除), 保证顺序耗尽、不重发、不丢后续. */
    char tmp[strlen(CACHE_FILE) + 8];   /* CACHE_FILE 为指针, sizeof 仅 4, 需按 strlen 定长 */
    snprintf(tmp, sizeof(tmp), "%s.tmp", CACHE_FILE);
    FILE *tf = fopen(tmp, "w");
    if (tf == NULL) {
        fclose(f);
        ESP_LOGE(TAG, "无法创建临时缓存文件");
        return -1;
    }

    char line[CACHE_LINE_MAX];
    int sent = 0, fail = 0, line_num = 0;
    bool failed = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        line_num++;
        /* 去掉末尾换行 */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        /* 本批内且尚未失败: 尝试发送 */
        if (!failed && line_num <= CACHE_REPLAY_BATCH_MAX) {
            int ret = send_func(NULL, line);
            if (ret == 0) {
                sent++;                 /* 已成功发送 → 丢弃(不写入 temp) */
                /* 2026-08-12 修复: 原延迟写在 temp 写入后, 成功发送走 continue 跳过,
                 * 导致 20 条成功补传在毫秒级内突发出去, MQTT 发送队列/IoTDA 过载,
                 * 设备被断开("一直设备断网"). 现对成功发送也节流(本批最后一条除外). */
                if (line_num < CACHE_REPLAY_BATCH_MAX) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                continue;
            }
            fail++;
            failed = true;              /* 该行起保留(写入 temp), 后续同理 */
            ESP_LOGW(TAG, "第 %d 条补传失败, 停止本批(失败行及之后保留待续)", line_num);
        }
        /* 失败行 / 失败之后 / 超出本批上限 → 保留到临时文件 */
        fwrite(line, 1, len, tf);
        fputc('\n', tf);
    }
    fclose(f);
    if (fclose(tf) != 0) {
        remove(tmp);
        return -1;
    }

    if (fail == 0) {
        /* 本批全部成功: temp 为空(全部发完)则删文件, 否则用 temp 覆盖(剩余后续) */
        if (stat(tmp, &st) == 0 && st.st_size == 0) {
            remove(CACHE_FILE);
            remove(tmp);
            ESP_LOGI(TAG, "缓存已清空 (成功 %d 条)", sent);
            return 0;
        } else {
            rename(tmp, CACHE_FILE);
            ESP_LOGI(TAG, "本批补传 %d 条成功, 剩余缓存下次继续", sent);
            /* 2026-08-12 修复: 原返回 0 导致 sys_mqtt_process_pending_replay
             * 清除 s_replay_pending, 剩余缓存要等下次断网重连才继续补传,
             * 在连接稳定时永远积压在 SPIFFS. 返回 1 让主循环下个周期继续. */
            return 1;
        }
    }

    /* 有失败: 用 temp(失败行+之后)覆盖原文件, 下个周期重试失败行 */
    rename(tmp, CACHE_FILE);
    ESP_LOGW(TAG, "本批补传 成功 %d 失败 %d, 保留失败行及之后待续", sent, fail);
    return 1;
}

int sys_offline_cache_count(void)
{
    if (!s_mounted) {
        return 0;
    }

    FILE *f = fopen(CACHE_FILE, "r");
    if (f == NULL) {
        return 0;
    }

    int count = 0;
    char line[CACHE_LINE_MAX];
    while (fgets(line, sizeof(line), f) != NULL) {
        count++;
    }
    fclose(f);
    return count;
}
