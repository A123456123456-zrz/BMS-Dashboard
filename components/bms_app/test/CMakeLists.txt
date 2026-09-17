# ============================================================
# bms_app 组件单元测试(Unity)
# 运行: idf.py test -T bms_app  (需在 ESP-IDF 环境)
# ============================================================
idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    REQUIRES bms_app bms_config bms_middleware bms_bsp esp_log
)
