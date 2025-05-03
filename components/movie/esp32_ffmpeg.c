#include "esp32_ffmpeg.h"
#include <string.h>
#include <stdlib.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

static const char *TAG = "esp32_ffmpeg";

// Structure du contexte
struct esp_ffmpeg_context_s {
    char *source_url;
    esp_ffmpeg_source_type_t source_type;
    esp_ffmpeg_frame_callback_t frame_callback;
    void *user_data;
    bool running;
    TaskHandle_t task_handle;
    SemaphoreHandle_t mutex;

    // Pour les fichiers
    FILE *input_file;

    // Pour HTTP
    esp_http_client_handle_t http_client;

    // Buffer de décodage
    uint8_t *buffer;
    size_t buffer_size;

    // Info vidéo
    int width;
    int height;
    int frame_count;
    bool is_mjpeg;
};

// Décodage JPEG fictif (remplacer par tjpgd si besoin)
static bool decode_jpeg(uint8_t *jpeg_data, size_t data_len,
                        uint16_t *rgb565_buffer, int width, int height) {
    // Vérification du header JPEG (SOI marker)
    if (data_len < 2 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        ESP_LOGE(TAG, "Not a valid JPEG marker. First bytes: 0x%02x 0x%02x", 
                 data_len > 0 ? jpeg_data[0] : 0xFF, 
                 data_len > 1 ? jpeg_data[1] : 0xFF);
        
        // Dump first 16 bytes for debugging
        if (data_len > 0) {
            ESP_LOGI(TAG, "First bytes of data:");
            for (int i = 0; i < (data_len < 16 ? data_len : 16); i++) {
                ESP_LOGI(TAG, "Byte %d: 0x%02x (%c)", i, jpeg_data[i], 
                         (jpeg_data[i] >= 32 && jpeg_data[i] <= 126) ? jpeg_data[i] : '.');
            }
        }
        return false;
    }
    
    // Implémentation simplifiée (à remplacer par un décodeur JPEG réel)
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int offset = ((y * width + x) * 3) % (data_len - 10);
            if (offset < 0) offset = 0;
            uint8_t r = jpeg_data[offset % data_len];
            uint8_t g = jpeg_data[(offset + 1) % data_len];
            uint8_t b = jpeg_data[(offset + 2) % data_len];
            r = r >> 3;
            g = g >> 2;
            b = b >> 3;
            rgb565_buffer[y * width + x] = (r << 11) | (g << 5) | b;
        }
    }
    return true;
}

// Recherche du marqueur JPEG dans un buffer
static int find_jpeg_marker(uint8_t *buffer, size_t buffer_size) {
    for (size_t i = 0; i < buffer_size - 1; i++) {
        if (buffer[i] == 0xFF && buffer[i + 1] == 0xD8) {
            return i;
        }
    }
    return -1;
}

// Lecture d'une frame MJPEG depuis un fichier
static bool read_file_mjpeg_frame(esp_ffmpeg_context_t *ctx, uint8_t *buffer,
                             size_t buffer_size, size_t *bytes_read) {
    if (ctx->input_file == NULL) return false;
    
    // Recherche du début d'un JPEG (SOI marker: FF D8)
    uint8_t marker[2];
    bool found_start = false;
    
    while (!found_start && !feof(ctx->input_file)) {
        size_t read = fread(marker, 1, 2, ctx->input_file);
        if (read != 2) break;
        
        if (marker[0] == 0xFF && marker[1] == 0xD8) {
            found_start = true;
            fseek(ctx->input_file, -2, SEEK_CUR);  // Revenir au début du marqueur
        } else {
            fseek(ctx->input_file, -1, SEEK_CUR);  // Avancer d'un octet seulement
        }
    }
    
    if (!found_start) return false;
    
    // Lire les données du JPEG
    *bytes_read = fread(buffer, 1, buffer_size, ctx->input_file);
    return (*bytes_read > 0);
}

// Lecture d'une frame MJPEG depuis HTTP
static bool read_http_mjpeg_frame(esp_ffmpeg_context_t *ctx, uint8_t *buffer,
                             size_t buffer_size, size_t *bytes_read) {
    esp_err_t err;
    
    // Si c'est la première requête ou si on a besoin de réinitialiser
    if (ctx->http_client == NULL) {
        esp_http_client_config_t config = {
            .url = ctx->source_url,
            .timeout_ms = 5000,
            .buffer_size = buffer_size,
        };
        ctx->http_client = esp_http_client_init(&config);
        if (ctx->http_client == NULL) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            return false;
        }
    }
    
    // Ouvrir la connexion
    err = esp_http_client_open(ctx->http_client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(ctx->http_client);
        ctx->http_client = NULL;
        return false;
    }
    
    // Récupérer les headers
    int content_length = esp_http_client_fetch_headers(ctx->http_client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "HTTP client fetch headers failed");
        esp_http_client_close(ctx->http_client);
        return false;
    }
    
    // Limiter la lecture au buffer_size
    int to_read = content_length > 0 && content_length < buffer_size ? 
                  content_length : buffer_size;
    
    // Lire le corps
    int total_read = 0;
    int remaining = to_read;
    
    while (remaining > 0) {
        int read_len = esp_http_client_read(ctx->http_client, 
                                           (char*)buffer + total_read, 
                                           remaining);
        if (read_len <= 0) {
            break;
        }
        total_read += read_len;
        remaining -= read_len;
    }
    
    esp_http_client_close(ctx->http_client);
    
    if (total_read <= 0) {
        ESP_LOGE(TAG, "HTTP client read failed");
        return false;
    }
    
    *bytes_read = total_read;
    
    // Vérifier si les données commencent par un marqueur JPEG
    // Si non, rechercher le marqueur dans les données
    if (buffer[0] != 0xFF || buffer[1] != 0xD8) {
        ESP_LOGW(TAG, "HTTP data doesn't start with JPEG marker, looking for marker");
        int marker_pos = find_jpeg_marker(buffer, total_read);
        
        if (marker_pos >= 0) {
            ESP_LOGI(TAG, "Found JPEG marker at position %d", marker_pos);
            // Déplacer les données pour commencer au marqueur
            memmove(buffer, buffer + marker_pos, total_read - marker_pos);
            *bytes_read = total_read - marker_pos;
        } else {
            ESP_LOGE(TAG, "No JPEG marker found in HTTP data");
            return false;
        }
    }
    
    return true;
}

// Lecture d'une frame MJPEG (wrapper)
static bool read_mjpeg_frame(esp_ffmpeg_context_t *ctx, uint8_t *buffer,
                             size_t buffer_size, size_t *bytes_read) {
    if (ctx->source_type == ESP_FFMPEG_SOURCE_TYPE_FILE) {
        return read_file_mjpeg_frame(ctx, buffer, buffer_size, bytes_read);
    }
    else if (ctx->source_type == ESP_FFMPEG_SOURCE_TYPE_HTTP) {
        return read_http_mjpeg_frame(ctx, buffer, buffer_size, bytes_read);
    }
    return false;
}

// Tâche de décodage
static void ffmpeg_decode_task(void *arg) {
    esp_ffmpeg_context_t *ctx = (esp_ffmpeg_context_t *)arg;
    
    if (!ctx || !ctx->buffer) {
        ESP_LOGE(TAG, "Invalid context in decoder task");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Decoder task started with stack size: %d", uxTaskGetStackHighWaterMark(NULL));

    // Allouer la frame RGB565 (PSRAM si dispo)
    size_t rgb565_size = ctx->width * ctx->height * sizeof(uint16_t);
    uint16_t *rgb565_buffer = NULL;
    
#if CONFIG_SPIRAM
    rgb565_buffer = heap_caps_malloc(rgb565_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Allocated RGB565 buffer in SPIRAM");
#else
    rgb565_buffer = malloc(rgb565_size);
    ESP_LOGI(TAG, "Allocated RGB565 buffer in internal memory");
#endif

    if (!rgb565_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RGB565 buffer (size: %d bytes)", rgb565_size);
        ctx->running = false;
        vTaskDelete(NULL);
        return;
    }

    esp_ffmpeg_frame_t frame = {
        .data = (uint8_t *)rgb565_buffer,
        .size = rgb565_size,
        .width = ctx->width,
        .height = ctx->height,
        .pts = 0
    };

    const TickType_t delay_time = pdMS_TO_TICKS(100); // 10 FPS
    int consecutive_errors = 0;
    const int max_consecutive_errors = 5;

    while (ctx->running) {
        // Vérifier la stack disponible
        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGD(TAG, "Stack remaining: %d bytes", stack_remaining);
        
        if (stack_remaining < 256) {
            ESP_LOGW(TAG, "Stack space getting low: %d bytes", stack_remaining);
        }
        
        size_t bytes_read = 0;
        bool read_success = read_mjpeg_frame(ctx, ctx->buffer, ctx->buffer_size, &bytes_read);
        
        if (!read_success) {
            ESP_LOGW(TAG, "Failed to read frame, consecutive errors: %d", ++consecutive_errors);
            
            if (consecutive_errors >= max_consecutive_errors) {
                ESP_LOGE(TAG, "Too many consecutive read errors, stopping decoder");
                break;
            }
            
            vTaskDelay(delay_time); // Attendre avant de réessayer
            continue;
        }
        
        consecutive_errors = 0; // Réinitialiser le compteur d'erreurs
        
        ESP_LOGD(TAG, "Read %d bytes of MJPEG data", bytes_read);
        
        if (!decode_jpeg(ctx->buffer, bytes_read, rgb565_buffer, ctx->width, ctx->height)) {
            ESP_LOGE(TAG, "Failed to decode JPEG frame");
            vTaskDelay(delay_time);
            continue;
        }
        
        frame.pts++;
        
        // Appeler le callback si défini
        if (ctx->frame_callback) {
            ctx->frame_callback(&frame, ctx->user_data);
        }
        
        ctx->frame_count++;
        vTaskDelay(delay_time);
    }

    // Nettoyage
    free(rgb565_buffer);
    ESP_LOGI(TAG, "Decoder task finished, processed %d frames", ctx->frame_count);

    ctx->running = false;
    vTaskDelete(NULL);
}

// Initialisation
esp_err_t esp_ffmpeg_init(const char *source_url,
                         esp_ffmpeg_source_type_t source_type,
                         esp_ffmpeg_frame_callback_t frame_callback,
                         void *user_data,
                         esp_ffmpeg_context_t **ctx) {
    if (!source_url || !ctx) return ESP_ERR_INVALID_ARG;

    esp_ffmpeg_context_t *new_ctx = calloc(1, sizeof(esp_ffmpeg_context_t));
    if (!new_ctx) return ESP_ERR_NO_MEM;

    new_ctx->source_url = strdup(source_url);
    new_ctx->source_type = source_type;
    new_ctx->frame_callback = frame_callback;
    new_ctx->user_data = user_data;
    new_ctx->running = false;
    new_ctx->mutex = xSemaphoreCreateMutex();
    new_ctx->width = 128;   // Valeurs par défaut adaptées à ton écran
    new_ctx->height = 64;
    new_ctx->frame_count = 0;
    new_ctx->is_mjpeg = true;
    new_ctx->http_client = NULL;
    new_ctx->input_file = NULL;

    // Buffer de décodage (PSRAM si dispo)
    new_ctx->buffer_size = 32768; // 32 Ko (doublé par rapport à l'original)
#if CONFIG_SPIRAM
    new_ctx->buffer = heap_caps_malloc(new_ctx->buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Allocated decoder buffer in SPIRAM");
#else
    new_ctx->buffer = malloc(new_ctx->buffer_size);
    ESP_LOGI(TAG, "Allocated decoder buffer in internal memory");
#endif

    if (!new_ctx->buffer) {
        ESP_LOGE(TAG, "Failed to allocate decoder buffer (size: %d bytes)", new_ctx->buffer_size);
        free(new_ctx->source_url);
        vSemaphoreDelete(new_ctx->mutex);
        free(new_ctx);
        return ESP_ERR_NO_MEM;
    }

    if (source_type == ESP_FFMPEG_SOURCE_TYPE_FILE) {
        new_ctx->input_file = fopen(source_url, "rb");
        if (!new_ctx->input_file) {
            ESP_LOGE(TAG, "Failed to open file: %s", source_url);
            free(new_ctx->buffer);
            free(new_ctx->source_url);
            vSemaphoreDelete(new_ctx->mutex);
            free(new_ctx);
            return ESP_ERR_NOT_FOUND;
        }
    }
    // HTTP client sera initialisé dans la tâche pour éviter les problèmes de timeout

    *ctx = new_ctx;
    ESP_LOGI(TAG, "FFmpeg context initialized for %s", source_url);
    return ESP_OK;
}

esp_err_t esp_ffmpeg_start(esp_ffmpeg_context_t *ctx) {
    if (!ctx) return ESP_ERR_INVALID_ARG;
    if (ctx->running) return ESP_OK;
    
    ctx->running = true;
    BaseType_t ret = xTaskCreate(
        ffmpeg_decode_task,
        "ffmpeg_decode",
        4096, // Stack doublée
        ctx,
        tskIDLE_PRIORITY + 1,
        &ctx->task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create decoder task");
        ctx->running = false;
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t esp_ffmpeg_stop(esp_ffmpeg_context_t *ctx) {
    if (!ctx) return ESP_ERR_INVALID_ARG;
    if (!ctx->running) return ESP_OK;
    
    // Signaler l'arrêt
    ctx->running = false;
    
    // Attendre que la tâche se termine proprement
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Nettoyage des ressources
    if (ctx->source_type == ESP_FFMPEG_SOURCE_TYPE_FILE && ctx->input_file) {
        fclose(ctx->input_file);
        ctx->input_file = NULL;
    }
    else if (ctx->source_type == ESP_FFMPEG_SOURCE_TYPE_HTTP && ctx->http_client) {
        esp_http_client_cleanup(ctx->http_client);
        ctx->http_client = NULL;
    }
    
    // Libérer la mémoire
    free(ctx->buffer);
    free(ctx->source_url);
    vSemaphoreDelete(ctx->mutex);
    free(ctx);
    
    return ESP_OK;
}

esp_err_t esp_ffmpeg_convert_frame(uint16_t *src, void *dst, int width, int height, int dst_format) {
    if (!src || !dst) return ESP_ERR_INVALID_ARG;
    
    switch (dst_format) {
        case 0:  // RGB565 -> RGB565 (copie)
            memcpy(dst, src, width * height * sizeof(uint16_t));
            break;
        case 1:  // RGB565 -> RGB888
            for (int i = 0; i < width * height; i++) {
                uint16_t pixel = src[i];
                uint8_t *rgb = (uint8_t *)dst + (i * 3);
                rgb[0] = ((pixel >> 11) & 0x1F) << 3;  // R
                rgb[1] = ((pixel >> 5) & 0x3F) << 2;   // G
                rgb[2] = ((pixel) & 0x1F) << 3;        // B
            }
            break;
        case 2:  // RGB565 -> Grayscale
            for (int i = 0; i < width * height; i++) {
                uint16_t pixel = src[i];
                uint8_t r = ((pixel >> 11) & 0x1F) << 3;
                uint8_t g = ((pixel >> 5) & 0x3F) << 2;
                uint8_t b = ((pixel) & 0x1F) << 3;
                uint8_t gray = (r * 30 + g * 59 + b * 11) / 100;
                ((uint8_t *)dst)[i] = gray;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}
