# ============================================================
# App 层组件: 业务逻辑(SOC估算/保护/均衡/充电/OTA/任务调度)
# 依赖 Middleware 与 BSP, 由 main 启动
# ============================================================
idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "include"
    REQUIRES bms_config bms_bsp bms_middleware freertos esp_timer
    PRIV_REQUIRES app_update esp_https_ota esp_http_client mbedtls json
)
