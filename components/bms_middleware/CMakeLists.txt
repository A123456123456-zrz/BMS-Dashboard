# ============================================================
# Middleware 层组件: 中间件服务
# 数据管理/滤波/状态机/SD黑匣子/WiFi/MQTT/CAN协议/NVS参数持久化/HTTP配网
# 依赖 BSP 与 Config
# ============================================================
idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "include"
    REQUIRES bms_config bms_bsp fatfs esp_wifi esp_event esp_netif mqtt nvs_flash
             json esp_http_server spiffs mbedtls app_update esp_app_format
)
