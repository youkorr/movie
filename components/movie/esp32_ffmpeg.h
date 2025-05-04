#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;
    int size;
    int width;
    int height;
    int64_t pts;
} esp_ffmpeg_frame_t;

typedef struct esp_ffmpeg_context_s esp_ffmpeg_context_t;

typedef enum {
    ESP_FFMPEG_SOURCE_TYPE_FILE,
    ESP_FFMPEG_SOURCE_TYPE_HTTP
} esp_ffmpeg_source_type_t;

typedef void (*esp_ffmpeg_frame_callback_t)(esp_ffmpeg_frame_t *frame, void *user_data);

/**
 * @brief Initialize the FFmpeg context
 * 
 * @param source_url File path or HTTP URL
 * @param source_type Type of source (file or HTTP)
 * @param frame_callback Callback function to receive decoded frames
 * @param user_data User data to pass to callback
 * @param ctx Pointer to receive the created context
 * @return esp_err_t 
 */
esp_err_t esp_ffmpeg_init(const char *source_url, 
                         esp_ffmpeg_source_type_t source_type,
                         esp_ffmpeg_frame_callback_t frame_callback,
                         void *user_data,
                         esp_ffmpeg_context_t **ctx);

/**
 * @brief Start decoding frames in a separate task
 * 
 * @param ctx FFmpeg context
 * @return esp_err_t 
 */
esp_err_t esp_ffmpeg_start(esp_ffmpeg_context_t *ctx);

/**
 * @brief Stop decoding and free resources
 * 
 * @param ctx FFmpeg context
 * @return esp_err_t 
 */
esp_err_t esp_ffmpeg_stop(esp_ffmpeg_context_t *ctx);

/**
 * @brief Convert RGB565 frame to different format
 * 
 * @param src Source RGB565 data
 * @param dst Destination buffer
 * @param width Frame width
 * @param height Frame height 
 * @param dst_format Destination format (0=RGB565, 1=RGB888, 2=GRAYSCALE)
 * @return esp_err_t 
 */
esp_err_t esp_ffmpeg_convert_frame(uint16_t *src, void *dst, int width, int height, int dst_format);

#ifdef __cplusplus
}
#endif
