# ============================================================
# bms_middleware 组件单元测试(Unity)
# 运行: idf.py test -T bms_middleware  (需在 ESP-IDF 环境)
# ============================================================
idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "include"
    REQUIRES bms_config bms_bsp esp_timer esp_event esp_netif mqtt fatfs nvs_flash
             json mbedtls
)
