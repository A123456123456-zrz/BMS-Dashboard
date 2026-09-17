/**
 * @file    sys_ota_status.c
 * @brief   OTA 待升级状态缓存实现
 * @author  BMS Team
 * @date    2026-08
 * @note    简单静态缓存, 由 OTA 任务(task_ota)写、MQTT 上报线程读;
 *          低频事件 + 短字符串, 单字节写无撕裂风险, 不加锁(与 s_last_command_id 同级别).
 */
#include "sys_ota_status.h"
#include <string.h>

#define PEND_VER_MAX  32
#define PEND_URL_MAX  512

static char s_pend_ver[PEND_VER_MAX] = {0};
static char s_pend_url[PEND_URL_MAX] = {0};

/* 2026-08-14: OTA 阶段状态(供 sys_mqtt 上报读取)
 * 低频更新 + 单写单读(app_ota 写 / sys_mqtt 读), 单字节枚举与 int 进度无撕裂风险, 不加锁 */
static sys_ota_stage_e s_stage = OTA_STAGE_IDLE;
static int              s_progress = 0;
static const char     *s_error = OTA_ERR_NONE;   /* 失败错误码(仅 FAILED 时有意义) */

/* ================================================================ */
void sys_ota_status_set_pending(const char *ver, const char *url)
{
    if (ver != NULL && ver[0] != '\0') {
        strncpy(s_pend_ver, ver, PEND_VER_MAX - 1);
        s_pend_ver[PEND_VER_MAX - 1] = '\0';
    } else {
        s_pend_ver[0] = '\0';
    }
    if (url != NULL && url[0] != '\0') {
        strncpy(s_pend_url, url, PEND_URL_MAX - 1);
        s_pend_url[PEND_URL_MAX - 1] = '\0';
    } else {
        s_pend_url[0] = '\0';
    }
}

/* ================================================================ */
const char *sys_ota_status_get_pending_version(void)
{
    return (s_pend_ver[0] != '\0') ? s_pend_ver : NULL;
}

const char *sys_ota_status_get_pending_url(void)
{
    return (s_pend_url[0] != '\0') ? s_pend_url : NULL;
}

/* ================================================================ */
/* 2026-08-14: OTA 阶段回报 */
void sys_ota_status_set_stage(sys_ota_stage_e stage, int progress)
{
    if (stage >= OTA_STAGE_IDLE && stage <= OTA_STAGE_FAILED) {
        s_stage = stage;
    }
    if (progress < 0) {
        s_progress = 0;
    } else if (progress > 100) {
        s_progress = 100;
    } else {
        s_progress = progress;
    }
}

sys_ota_stage_e sys_ota_status_get_stage(void)
{
    return s_stage;
}

int sys_ota_status_get_progress(void)
{
    return s_progress;
}

const char *sys_ota_status_stage_str(void)
{
    switch (s_stage) {
    case OTA_STAGE_DOWNLOADING: return "downloading";
    case OTA_STAGE_VERIFYING:   return "verifying";
    case OTA_STAGE_REBOOTING:   return "rebooting";
    case OTA_STAGE_FAILED:      return "failed";
    default:                    return "idle";
    }
}

void sys_ota_status_set_failed(const char *code)
{
    s_stage = OTA_STAGE_FAILED;
    s_progress = 0;
    s_error = (code && code[0]) ? code : OTA_ERR_NONE;
}

const char *sys_ota_status_get_error(void)
{
    return s_error;
}
