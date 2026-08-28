if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.5")
    set(REQ esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_sdmmc)
    set(PRIV_REQ esp_driver_ledc)
else()
    set(REQ driver)
    set(PRIV_REQ "")
endif()

if(${IDF_VERSION_MAJOR} LESS 6)
    list(APPEND PRIV_REQ usb)
endif()

idf_component_register(
    SRCS "esp32_s3_touch_amoled_1_8.c" ${SRC_VER}
    INCLUDE_DIRS "include" "include/bsp"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES
        ${REQ}
        esp_codec_dev
        esp_lcd
        esp_lcd_touch
        esp_lvgl_port
        lvgl
    PRIV_REQUIRES
        ${PRIV_REQ}
        esp_lcd_co5300
        esp_lcd_sh8601
        esp_lcd_panel_io_additions
        esp_lcd_touch_cst816s
        esp_lcd_touch_ft5x06
        esp_psram
        esp_timer
        fatfs
        spiffs
)
