/**
 * @file    bsp_sd.c
 * @brief   SD 卡驱动实现(SPI + FATFS 挂载)
 * @author  BMS Team
 * @date    2026-08
 * @note    挂载点 /sdcard, 黑匣子数据存储, 与 MCP2515 共用 SPI3
 *          挂载失败时降级运行(不阻塞系统启动)
 */
#include "bsp_sd.h"
#include "bms_pinmap.h"
#include "bms_config.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"                        // FATFS 挂载 + sdspi 类型(内部含 driver/sdspi_host.h)
#include "driver/spi_common.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>                             /* fsync/fileno — H16 强制落盘 */

static const char *TAG = "BSP_SD";

static sdmmc_card_t *s_card = NULL;
static bool          s_ready = false;

bms_err_t bsp_sd_init(void)
{
    /* 硬件屏蔽: 缺 SD 卡时直接返回, 不阻塞系统 */
    if (!HW_ENABLE_SD_CARD) {
        ESP_LOGW(TAG, "SD_CARD 屏蔽 (HW_ENABLE_SD_CARD=0)");
        return BMS_ERR_NOT_INIT;
    }

#if HW_ENABLE_SD_CARD
    /* 配置 SPI3 为 SD 卡专用(SD_MOSI/MISO/SCK 与 MCP2515 共用, CS 独立) */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;                           // 指定 SPI 外设
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_cfg.gpio_cs = PIN_SD_CS;
    dev_cfg.host_id = SPI3_HOST;

    /* SPI 总线配置(SPI3) */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = PIN_SPI3_MOSI,
        .miso_io_num   = PIN_SPI3_MISO,
        .sclk_io_num   = PIN_SPI3_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4092,
    };
    esp_err_t ret = spi_bus_initialize(host.slot, &buscfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi bus init fail: %s", esp_err_to_name(ret));
        return BMS_ERR_SPI;
    }

    /* 挂载 FATFS: esp_vfs_fat_sdspi_mount 内部完成设备初始化 + 探测 + 挂载
     * H16 修复: format_if_mount_failed 由 true 改为 false —
     * 自动格式化会把"黑匣子"里事故前最关键的数据抹掉, 与用途相悖.
     * 挂载失败仅降级运行并保留原始数据, 由人工处理. */
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,            // 挂载失败不格式化(保护事故数据)
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &dev_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount fail: %s, 黑匣子降级运行", esp_err_to_name(ret));
        return BMS_ERR_STORAGE;                     // 降级: 不阻塞系统启动
    }

    s_ready = true;
    ESP_LOGI(TAG, "sd init ok, mount=%s", SD_MOUNT_POINT);
#endif
    return BMS_OK;
}

bool bsp_sd_is_ready(void)
{
    return s_ready;
}

bms_err_t bsp_sd_append_line(const char *path, const char *line)
{
    if (!s_ready || path == NULL || line == NULL) {
        return BMS_ERR_PARAM_INVALID;
    }

    FILE *f = fopen(path, "a");                     // 追加模式
    if (f == NULL) {
        ESP_LOGW(TAG, "open %s fail", path);
        return BMS_ERR_STORAGE;
    }

    /* 检查写入结果, 失败回滚(关闭文件后返回错误) */
    size_t need = strlen(line);
    size_t wrote = fwrite(line, 1, need, f);
    /* H16 修复: fflush 只刷到 FATFS 层缓冲, 掉电仍可能丢数据;
     * 改 fsync 强制落盘(f_sync), 保证"黑匣子"记录真实持久化 */
    fflush(f);
    if (fsync(fileno(f)) != 0) {
        ESP_LOGW(TAG, "fsync %s fail", path);
    }
    fclose(f);

    if (wrote != need) {
        ESP_LOGW(TAG, "write %s incomplete: %u/%u", path, (unsigned)wrote, (unsigned)need);
        return BMS_ERR_STORAGE;
    }
    return BMS_OK;
}
