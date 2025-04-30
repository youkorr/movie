idf_component_register(SRCS "esp32_ffmpeg.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client esp_wifi freertos nvs_flash)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)
