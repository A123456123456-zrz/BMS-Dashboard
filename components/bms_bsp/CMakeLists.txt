# ============================================================
# BSP 层组件: 板级硬件抽象封装
# 依赖 Config 层配置项, 封装 LTC6804/ADC/I2C/SPI/CAN/SD/GPIO 等
# ============================================================
idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "include"
    REQUIRES bms_config driver esp_adc
    PRIV_REQUIRES fatfs                 # bsp_sd.c 使用 esp_vfs_fat.h
)
