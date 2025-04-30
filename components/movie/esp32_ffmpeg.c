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

static const char *TAG = "esp32_ffmpeg";

// Structure légère pour imiter un contexte FFmpeg minimal
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
    
    // Info sur la vidéo
    int width;
    int height;
    int frame_count;
    bool is_mjpeg;    // Format MJPEG (le seul vraiment supporté pour l'instant)
};

// Fonction de décodage JPEG simplifiée
static bool decode_jpeg(uint8_t *jpeg_data, size_t data_len, 
                         uint16_t *rgb565_buffer, int width, int height) {
    // Vérifier l'en-tête JPEG
    if (data_len < 2 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        ESP_LOGE(TAG, "Not a valid JPEG marker");
        return false;
    }
    
    // Implémentation simplifiée pour exemple
    // Dans une utilisation réelle, utilisez un décodeur JPEG comme tjpgd
    
    // Remplir avec des motifs basés sur les données JPEG
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int offset = ((y * width + x) * 3) % (data_len - 10);
            if (offset < 0) offset = 0;
            
            uint8_t r = jpeg_data[offset % data_len];
            uint8_t g = jpeg_data[(offset + 1) % data_len];
            uint8_t b = jpeg_data[(offset + 2) % data_len];
            
            // Réduire à 5/6/5 bits
            r = r >> 3;
            g = g >> 2;
            b = b >> 3;
            
            // Assembler en RGB565
            rgb565_buffer[y * width + x] = (r << 11) | (g << 5) | b;
        }
    }
    
    return true;
}

// Fonction pour lire les données MJPEG
static bool read_mjpeg_frame(esp_ffmpeg_context_t *ctx, uint8_t *buffer, 
                             size_t buffer_size, size_t *bytes_read) {
    if (ctx->source_type == ESP_FFMPEG_SOURCE_TYPE_FILE) {
        if (ctx->input_file == NULL) {
            return false;
        }
        
        // Chercher le marqueur de début JPEG (FF D8)
        uint8_t marker[2];
        bool found_start = false;
        while (!found_start && !feof(ctx->input_file)) {
            size_t read = fread(marker, 1, 2, ctx->input_file);
            if (read != 2) break;
            
            if (marker[0] == 0xFF && marker[1] == 0xD8) {
                found_start = true;
                fseek(ctx->input_file, -2, SEEK_CUR);
            } else {
                fseek(ctx->input_file, -1, SEEK_CUR);
            }
        }
        
        if (!found_start) {
            return false;
        }
        
        // Lire les données JPEG
        *bytes_read = fread(buffer, 1, buffer_size, ctx->input_file);
        return (*bytes_read > 0);
    }
    else if (ctx->source_type == ESP_FFMPEG_SOURCE_TYPE_HTTP) {
        // Utilisation d'ESP HTTP Client
        esp_err_t err = esp_http_client_open(ctx->http_client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
            return false;
        }
        
        int content_length = esp_http_client_fetch_headers(ctx->http_client);
        
        if (content_length <= 0) {
            ESP_LOGE(TAG, "HTTP client fetch headers failed");
            esp_http_client_close(ctx->http_client);
            return false;
        }
        
        int data_read = esp_http_client_read_response(ctx->http_client, (char*)buffer, buffer_size);
        if (data_read <= 0) {
            ESP_LOGE(TAG, "HTTP client read failed");
            esp_http_client_close(ctx->http_client);
            return false;
        }
        
        esp_http_client_close(ctx->http_client);
        *bytes_read = data_read;
        
        // Vérifier si les données commencent par un marqueur JPEG
        if (buffer[0] != 0xFF || buffer[1] != 0xD8) {
            ESP_LOGW(TAG, "HTTP data doesn't start with JPEG marker");
        }
        
        return true;
    }
    
    return false;
}

// Tâche de décodage
static void ffmpeg_decode_task(void *arg) {
    esp_ffmpeg_context_t *ctx = (esp_ffmpeg_context_t *)arg;
    if (!ctx || !ctx->buffer) {
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Decoder task started");
    
    // Allouer mémoire pour la frame RGB565
    uint16_t *rgb565_buffer = malloc(ctx->width * ctx->height * sizeof(uint16_t));
    if (!rgb565_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RGB565 buffer");
        ctx->running = false;
        vTaskDelete(NULL);
        return;
    }
    
    // Créer la structure de frame pour le callback
    esp_ffmpeg_frame_t frame = {
        .data = (uint8_t *)rgb565_buffer,
        .size = ctx->width * ctx->height * sizeof(uint16_t),
        .width = ctx->width,
        .height = ctx->height,
        .pts = 0
    };
    
    const TickType_t delay_time = pdMS_TO_TICKS(33); // ~30 FPS
    
    while (ctx->running) {
        size_t bytes_read = 0;
        
        // Lire une frame
        if (!read_mjpeg_frame(ctx, ctx->buffer, ctx->buffer_size, &bytes_read)) {
            ESP_LOGI(TAG, "End of stream or read error");
            break;
        }
        
        // Décoder la frame JPEG en RGB565
        if (!decode_jpeg(ctx->buffer, bytes_read, rgb565_buffer, ctx->width, ctx->height)) {
            ESP_LOGE(TAG, "Failed to decode JPEG frame");
            continue;
        }
        
        // Mise à jour du PTS (timestamp)
        frame.pts++;
        
        // Appeler le callback avec la frame décodée
        if (ctx->frame_callback) {
            ctx->frame_callback(&frame, ctx->user_data);
        }
        
        ctx->frame_count++;
        
        // Délai pour maintenir le FPS
        vTaskDelay(delay_time);
    }
    
    free(rgb565_buffer);
    ESP_LOGI(TAG, "Decoder task finished, processed %d frames", ctx->frame_count);
    
    ctx->running = false;
    vTaskDelete(NULL);
}

esp_err_t esp_ffmpeg_init(const char *source_url, 
                         esp_ffmpeg_source_type_t source_type,
                         esp_ffmpeg_frame_callback_t frame_callback,
                         void *user_data,
                         esp_ffmpeg_context_t **ctx) {
    if (!source_url || !ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_ffmpeg_context_t *new_ctx = calloc(1, sizeof(esp_ffmpeg_context_t));
    if (!new_ctx) {
        return ESP_ERR_NO_MEM;
    }
    
    // Initialiser les valeurs de base
    new_ctx->source_url = strdup(source_url);
    new_ctx->source_type = source_type;
    new_ctx->frame_callback = frame_callback;
    new_ctx->user_data = user_data;
    new_ctx->running = false;
    new_ctx->mutex = xSemaphoreCreateMutex();
    new_ctx->width = 320;  // Valeurs par défaut
    new_ctx->height = 240;
    new_ctx->frame_count = 0;
    new_ctx->is_mjpeg = true;
    
    // Allouer le buffer de décodage
    new_ctx->buffer_size = 65536;  // 64KB buffer par défaut
    new_ctx->buffer = malloc(new_ctx->buffer_size);
    if (!new_ctx->buffer) {
        free(new_ctx->source_url);
        vSemaphoreDelete(new_ctx->mutex);
        free(new_ctx);
        return ESP_ERR_NO_MEM;
    }
    
    // Initialiser la source
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
    else if (source_type == ESP_FFMPEG_SOURCE_TYPE_HTTP) {
        esp_http_client_config_t config = {
            .url = source_url,
            .timeout_ms = 5000,
        };
        
        new_ctx->http_client = esp_http_client_init(&config);
        if (!new_ctx->http_client) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            free(new_ctx->buffer);
            free(new_ctx->source_url);
            vSemaphoreDelete(new_ctx->mutex);
            free(new_ctx);
            return ESP_ERR_NO_MEM;
        }
    }
    
    *ctx = new_ctx;
    ESP_LOGI(TAG, "FFmpeg context initialized for %s", source_url);
    return ESP_OK;
}

esp_err_t esp_ffmpeg_start(esp_ffmpeg_context_t *ctx) {
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (ctx->running) {
        return ESP_OK;  // Déjà en cours d'exécution
    }
    
    ctx->running = true;
    
    // Créer la tâche de décodage
    BaseType_t ret = xTaskCreate(
        ffmpeg_decode_task,
        "ffmpeg_decode",
        4096,
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
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!ctx->running) {
        return ESP_OK;  // Déjà arrêté
    }
    
    // Demander l'arrêt de la tâche
    ctx->running = false;
    
    // Attendre que la tâche se termine
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Nettoyer les ressources
    if (ctx->source_type == ESP_FFMPEG_SOURCE_TYPE_FILE && ctx->input_file) {
        fclose(ctx->input_file);
        ctx->input_file = NULL;
    }
    else if (ctx->source_type == ESP_FFMPEG_SOURCE_TYPE_HTTP && ctx->http_client) {
        esp_http_client_cleanup(ctx->http_client);
        ctx->http_client = NULL;
    }
    
    free(ctx->buffer);
    free(ctx->source_url);
    vSemaphoreDelete(ctx->mutex);
    free(ctx);
    
    return ESP_OK;
}

esp_err_t esp_ffmpeg_convert_frame(uint16_t *src, void *dst, int width, int height, int dst_format) {
    if (!src || !dst) {
        return ESP_ERR_INVALID_ARG;
    }
    
    switch (dst_format) {
        case 0:  // RGB565 -> RGB565 (copie)
            memcpy(dst, src, width * height * sizeof(uint16_t));
            break;
        
        case 1:  // RGB565 -> RGB888
            for (int i = 0; i < width * height; i++) {
                uint16_t pixel = src[i];
                uint8_t *rgb = (uint8_t *)dst + (i * 3);
                
                // Extraire les composantes et les étendre
                rgb[0] = ((pixel >> 11) & 0x1F) << 3;  // R
                rgb[1] = ((pixel >> 5) & 0x3F) << 2;   // G
                rgb[2] = ((pixel) & 0x1F) << 3;        // B
            }
            break;
            
        case 2:  // RGB565 -> Grayscale
            for (int i = 0; i < width * height; i++) {
                uint16_t pixel = src[i];
                
                // Extraire les composantes
                uint8_t r = ((pixel >> 11) & 0x1F) << 3;
                uint8_t g = ((pixel >> 5) & 0x3F) << 2;
                uint8_t b = ((pixel) & 0x1F) << 3;
                
                // Calculer la luminosité (pondération standard pour la luminance perçue)
                uint8_t gray = (r * 30 + g * 59 + b * 11) / 100;
                ((uint8_t *)dst)[i] = gray;
            }
            break;
            
        default:
            return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}
