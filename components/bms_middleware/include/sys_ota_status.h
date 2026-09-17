/**
 * @file    sys_ota_status.h
 * @brief   OTA 待升级状态缓存(Middleware 层, 供 app_ota 写入 / sys_mqtt 上报读取)
 * @author  BMS Team
 * @date    2026-08
 * @note    2026-08-13 "人工确认"升级模式: app_ota 检查到新版本后只缓存于此,
 *          sys_mqtt 上报给前端展示"发现新版本", 用户确认后才执行升级.
 *          放 Middleware 层避免 app_ota(app) ↔ sys_mqtt(middleware) 循环依赖.
 */
#ifndef SYS_OTA_STATUS_H
#define SYS_OTA_STATUS_H

/**
 * @brief   OTA 阶段(设备侧真·状态机, 随属性上报云端, 供前端精确展示)
 * @note    对齐企业 OTA 任务(华为 IoTDA 软件升级 / AWS IoT Jobs): 状态分段 + 阶段可观测。
 *          前端 4 段进度条映射:
 *            ① 指令下发   → 云端 ack(前端固有, 不依赖此字段)
 *            ② 设备下载   → OTA_STAGE_DOWNLOADING
 *            ③ 校验写入   → OTA_STAGE_VERIFYING(镜像写入+SHA256 校验完成, 即将重启)
 *            ④ 重启验证   → OTA_STAGE_REBOOTING(标记 pending, 即将重启) + 新固件回联版本匹配
 */
typedef enum {
    OTA_STAGE_IDLE        = 0,  /**< 空闲/未升级/升级完成后复位 */
    OTA_STAGE_DOWNLOADING = 1,  /**< 正在下载固件(HTTPS 流式下载) */
    OTA_STAGE_VERIFYING   = 2,  /**< 镜像写入 OTA 分区 + SHA256 校验通过, 即将重启(=前端"校验写入") */
    OTA_STAGE_REBOOTING   = 3,  /**< 已标记 pending, 即将重启切换到新分区 */
    OTA_STAGE_FAILED      = 4   /**< 升级失败(下载/校验/写入错误) */
} sys_ota_stage_e;

/**
 * @brief   记录待确认升级的新版本(检查到新版本时调用)
 * @param   ver  新版本号, 如 "1.0.225"; NULL/空串 = 清除
 * @param   url  固件下载 URL; NULL/空串 = 清除
 */
void sys_ota_status_set_pending(const char *ver, const char *url);

/**
 * @brief   查询待确认升级的新版本号
 * @return  版本字符串, 无待升级时返回 NULL
 */
const char *sys_ota_status_get_pending_version(void);

/**
 * @brief   查询待确认升级的固件 URL
 * @return  URL 字符串, 无待升级时返回 NULL
 */
const char *sys_ota_status_get_pending_url(void);

/* ============ 2026-08-14: OTA 阶段回报(设备 → 云端) ============ */

/**
 * @brief   设置当前 OTA 阶段与下载进度
 * @param   stage     阶段枚举(见 sys_ota_stage_e), 越界值忽略
 * @param   progress  下载进度 0~100(仅 DOWNLOADING 有意义; 其余传 100/0 即可)
 */
void sys_ota_status_set_stage(sys_ota_stage_e stage, int progress);

/**
 * @brief   查询当前 OTA 阶段
 */
sys_ota_stage_e sys_ota_status_get_stage(void);

/**
 * @brief   查询当前下载进度(0~100)
 */
int sys_ota_status_get_progress(void);

/**
 * @brief   阶段枚举 → 字符串(供属性上报: idle/downloading/verifying/rebooting/failed)
 */
const char *sys_ota_status_stage_str(void);

/* OTA 失败错误码(随属性上报, 设备真·失败原因, 供前端精确展示)
 * 对齐企业 OTA 任务: 失败态带错误码而非笼统"失败" */
#define OTA_ERR_NONE            ""                      /**< 无错误 */
#define OTA_ERR_PARAM           "PARAM_INVALID"         /**< URL 为空/非法 */
#define OTA_ERR_DOWNLOAD        "DOWNLOAD_FAIL"         /**< HTTP/网络下载失败 */
#define OTA_ERR_INCOMPLETE      "INCOMPLETE_DATA"       /**< 数据未完整接收 */
#define OTA_ERR_IMAGE_SIZE      "INVALID_IMAGE_SIZE"    /**< 镜像大小异常(<=0, 可能损坏/恶意 URL) */
#define OTA_ERR_VALIDATE        "VALIDATE_FAILED"       /**< SHA256 校验失败(固件损坏) */
#define OTA_ERR_WRITE           "WRITE_FAILED"          /**< 写入/Finish 失败(非校验类) */

/**
 * @brief   标记 OTA 失败并携带错误码(等价于 set_stage(FAILED) + 记录错误码)
 * @param   code  错误码字符串(见 OTA_ERR_* 宏), NULL/空串 = 未知
 */
void sys_ota_status_set_failed(const char *code);

/**
 * @brief   查询当前 OTA 失败错误码(无失败返回 OTA_ERR_NONE)
 */
const char *sys_ota_status_get_error(void);

#endif // SYS_OTA_STATUS_H
