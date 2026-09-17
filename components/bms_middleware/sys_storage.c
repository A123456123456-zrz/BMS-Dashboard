/**
 * @file    sys_storage.c
 * @brief   SD 卡黑匣子日志服务实现
 * @author  BMS Team
 * @date    2026-08
 * @note    CSV 格式按日期分文件, SD 卡未就绪时静默降级(不阻塞系统)
 */
#include "sys_storage.h"
#include "bsp_sd.h"
#include "bms_config.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "SYS_STORAGE";

/* 2026-08-07: CSV 表头改为运行时按实际串数动态生成(支持 6S~16S) */
static void build_csv_header(char *buf, size_t len)
{
    int off = snprintf(buf, len, "timestamp_ms,SOC,SOH,");
    for (uint8_t i = 0; i < BMS_CELL_SERIES_NUM; i++) {
        off += snprintf(buf + off, len - off, "V%u,", i + 1);
    }
    snprintf(buf + off, len - off, "Current_mA,Temp1,Temp2,Temp3,Fault\r\n");
}

static char s_cur_filename[48] = {0};               // 当前日志文件名

/* 构造当天文件名, 时间未同步时使用 "BOOT" */
static void build_filename(char *buf, uint8_t len)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm tm_info;
    if (now > 1700000000) {                          // 时间已同步(2023 年之后)
        localtime_r(&now, &tm_info);
        snprintf(buf, len, "%s/%04d-%02d-%02d_BMS_LOG.csv",
                 SD_MOUNT_POINT,
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday);
    } else {
        snprintf(buf, len, "%s/BOOT_BMS_LOG.csv", SD_MOUNT_POINT);
    }
}

bms_err_t sys_storage_init(void)
{
    if (!bsp_sd_is_ready()) {
        ESP_LOGW(TAG, "sd card not ready, storage degraded");
        return BMS_ERR_STORAGE;
    }

    build_filename(s_cur_filename, sizeof(s_cur_filename));

    /* 写表头(仅新文件时写, 追加模式下首行检测简化处理) */
    FILE *f = fopen(s_cur_filename, "r");            // 探测文件是否存在
    bool need_header = (f == NULL);
    if (f) {
        fclose(f);
    }

    if (need_header) {
        char header[160];
        build_csv_header(header, sizeof(header));
        bsp_sd_append_line(s_cur_filename, header);
    }

    ESP_LOGI(TAG, "storage init ok, file=%s", s_cur_filename);
    return BMS_OK;
}

void sys_storage_log_frame(const bms_pack_data_t *pack,
                           const bms_soc_data_t  *soc,
                           bms_fault_mask_t       fault)
{
    if (!bsp_sd_is_ready() || pack == NULL || soc == NULL) {
        return;
    }

    /* 每天切换文件(跨日时重建文件名) */
    char filename[48];
    build_filename(filename, sizeof(filename));
    if (strcmp(filename, s_cur_filename) != 0) {
        strncpy(s_cur_filename, filename, sizeof(s_cur_filename) - 1);
        /* 跨日新文件同样写动态表头(原 CSV_HEADER 宏已废弃, 2026-08-07 串数动态化) */
        char header[160];
        build_csv_header(header, sizeof(header));
        bsp_sd_append_line(s_cur_filename, header);
    }

    /* 组装 CSV 行(使用 snprintf 防止溢出, 2026-08-07 串数动态拼接) */
    char line[256];
    int off = 0;
    int cap = (int)sizeof(line) - 1;
    off += snprintf(line + off, cap - off + 1, "%lu,%.1f,%.1f,",
                    (unsigned long)pack->timestamp_ms,
                    soc->soc * 100.0f, soc->soh * 100.0f);
    for (uint8_t i = 0; i < BMS_CELL_SERIES_NUM && off < cap; i++) {
        off += snprintf(line + off, cap - off + 1, "%u,", pack->cell_mv[i]);
    }
    off += snprintf(line + off, cap - off + 1, "%d,%d,%d,%d,0x%08X\r\n",
                    pack->current_ma,
                    pack->temp_dc[0], pack->temp_dc[1], pack->temp_dc[2],
                    (unsigned)fault);
    int n = off;

    /* 校验拼接结果是否完整 */
    if (n < 0 || n >= (int)sizeof(line)) {
        ESP_LOGW(TAG, "csv line truncated");
        return;
    }

    bms_err_t ret = bsp_sd_append_line(s_cur_filename, line);          // BSP 层 SD 卡追加写
    if (ret != BMS_OK) {
        ESP_LOGW(TAG, "sd log write fail: %d", ret);
    }
}
