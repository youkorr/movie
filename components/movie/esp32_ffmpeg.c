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

// Structures pour le format AVI
typedef struct {
    char id[4];
    uint32_t size;
} riff_chunk_t;

typedef struct {
    uint32_t chunk_size;
    uint16_t width;
    uint16_t height;
    uint16_t planes;
    uint16_t bit_count;
    uint32_t compression;
    uint32_t image_size;
    uint32_t x_pels_per_meter;
    uint32_t y_pels_per_meter;
    uint32_t clr_used;
    uint32_t clr_important;
} avi_bitmap_info_header_t;

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
    
    // Pour AVI
    bool is_avi;
    long avi_data_offset;     // Offset où commencent les données vidéo
    long avi_current_offset;  // Offset courant de lecture
    uint32_t avi_frame_size;  // Taille moyenne des frames
    uint32_t avi_total_frames; // Nombre total de frames
    
    // Pour MP4
    bool is_mp4;
    int retry_count;
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

// Parser l'en-tête AVI
static bool parse_avi_header(esp_ffmpeg_context_t *ctx) {
    if (!ctx || !ctx->input_file) return false;
    
    // Revenir au début du fichier
    fseek(ctx->input_file, 0, SEEK_SET);
    
    // Lire le header RIFF
    riff_chunk_t riff_header;
    if (fread(&riff_header, sizeof(riff_chunk_t), 1, ctx->input_file) != 1) {
        ESP_LOGE(TAG, "Failed to read RIFF header");
        return false;
    }
    
    // Vérifier que c'est bien un RIFF
    if (strncmp(riff_header.id, "RIFF", 4) != 0) {
        ESP_LOGE(TAG, "Not a RIFF file");
        return false;
    }
    
    // Lire l'identifiant AVI
    char avi_id[4];
    if (fread(avi_id, 4, 1, ctx->input_file) != 1 || strncmp(avi_id, "AVI ", 4) != 0) {
        ESP_LOGE(TAG, "Not an AVI file");
        return false;
    }
    
    ESP_LOGI(TAG, "Valid AVI file detected");
    ctx->is_avi = true;
    
    // Parcourir les chunks pour trouver les informations nécessaires
    bool found_header = false;
    bool found_movi = false;
    char chunk_id[4];
    uint32_t chunk_size;
    uint32_t frames_count = 0;
    uint32_t stream_format = 0;
    
    while (!feof(ctx->input_file) && !(found_header && found_movi)) {
        // Lire l'identifiant du chunk
        if (fread(chunk_id, 4, 1, ctx->input_file) != 1) break;
        
        // Lire la taille du chunk
        if (fread(&chunk_size, 4, 1, ctx->input_file) != 1) break;
        
        ESP_LOGD(TAG, "Chunk: %.4s, Size: %d", chunk_id, chunk_size);
        
        // Recherche des chunks importants
        if (strncmp(chunk_id, "LIST", 4) == 0) {
            char list_type[4];
            if (fread(list_type, 4, 1, ctx->input_file) != 1) break;
            
            ESP_LOGD(TAG, "List type: %.4s", list_type);
            
            if (strncmp(list_type, "hdrl", 4) == 0) {
                // Traiter l'en-tête pour obtenir la résolution
                char subchunk_id[4];
                uint32_t subchunk_size;
                
                while (!feof(ctx->input_file) && !found_header) {
                    if (fread(subchunk_id, 4, 1, ctx->input_file) != 1) break;
                    if (fread(&subchunk_size, 4, 1, ctx->input_file) != 1) break;
                    
                    ESP_LOGD(TAG, "Subchunk: %.4s, Size: %d", subchunk_id, subchunk_size);
                    
                    if (strncmp(subchunk_id, "avih", 4) == 0) {
                        // MainAVIHeader
                        uint32_t micro_sec_per_frame;
                        if (fread(&micro_sec_per_frame, 4, 1, ctx->input_file) != 1) break;
                        
                        // Ignorer les 8 octets suivants
                        fseek(ctx->input_file, 8, SEEK_CUR);
                        
                        // Lire le nombre de frames
                        if (fread(&frames_count, 4, 1, ctx->input_file) != 1) break;
                        ctx->avi_total_frames = frames_count;
                        
                        // Ignorer le reste du chunk avih
                        fseek(ctx->input_file, subchunk_size - 16, SEEK_CUR);
                    }
                    else if (strncmp(subchunk_id, "strf", 4) == 0) {
                        avi_bitmap_info_header_t bih;
                        if (fread(&bih, sizeof(avi_bitmap_info_header_t), 1, ctx->input_file) != 1) break;
                        
                        ctx->width = bih.width;
                        ctx->height = bih.height;
                        stream_format = bih.compression;
                        
                        // Vérifier si c'est du MJPEG
                        char format_str[5] = {0};
                        memcpy(format_str, &stream_format, 4);
                        ESP_LOGI(TAG, "Video format: %.4s, Width: %d, Height: %d", 
                                format_str, ctx->width, ctx->height);
                        
                        // 'MJPG' en little endian = GPJM
                        ctx->is_mjpeg = (stream_format == 0x47504A4D);
                        
                        found_header = true;
                        
                        // Ignorer le reste du chunk strf
                        fseek(ctx->input_file, subchunk_size - sizeof(avi_bitmap_info_header_t), SEEK_CUR);
                    }
                    else {
                        // Ignorer les chunks inconnus
                        fseek(ctx->input_file, subchunk_size, SEEK_CUR);
                    }
                    
                    // Ajustement pour l'alignement
                    if (subchunk_size % 2 == 1) {
                        fseek(ctx->input_file, 1, SEEK_CUR);
                    }
                }
            }
            else if (strncmp(list_type, "movi", 4) == 0) {
                // On a trouvé les données vidéo
                ctx->avi_data_offset = ftell(ctx->input_file);
                ctx->avi_current_offset = ctx->avi_data_offset;
                found_movi = true;
                
                ESP_LOGI(TAG, "Found movi LIST at offset %ld", ctx->avi_data_offset);
                
                // Estimer la taille des frames si non trouvée précédemment
                if (frames_count > 0 && chunk_size > 4) {
                    ctx->avi_frame_size = (chunk_size - 4) / frames_count;
                    ESP_LOGI(TAG, "Estimated frame size: %d bytes", ctx->avi_frame_size);
                }
                
                // Sortir de la boucle car on a trouvé ce qu'on cherchait
                break;
            }
            else {
                // Ignorer les autres types de list
                fseek(ctx->input_file, chunk_size - 4, SEEK_CUR);  // -4 car on a déjà lu le type de liste
            }
        }
        else {
            // Ignorer les chunks inconnus
            fseek(ctx->input_file, chunk_size, SEEK_CUR);
        }
        
        // Ajustement pour l'alignement
        if (chunk_size % 2 == 1) {
            fseek(ctx->input_file, 1, SEEK_CUR);
        }
    }
    
    if (!found_header || !found_movi) {
        ESP_LOGE(TAG, "AVI file is missing required chunks (hdrl or movi)");
        return false;
    }
    
    // Revenir au début des données vidéo
    fseek(ctx->input_file, ctx->avi_data_offset, SEEK_SET);
    ctx->avi_current_offset = ctx->avi_data_offset;
    
    ESP_LOGI(TAG, "AVI header parsed successfully: %dx%d, %d frames", 
            ctx->width, ctx->height, ctx->avi_total_frames);
    
    return true;
}

// Lire une frame depuis un fichier AVI
static bool read_file_avi_frame(esp_ffmpeg_context_t *ctx, uint8_t *buffer,
                             size_t buffer_size, size_t *bytes_read) {
    if (ctx->input_file == NULL) return false;
    
    // Si on commence la lecture, positionner au début des données
    if (ctx->avi_current_offset == 0) {
        if (!parse_avi_header(ctx)) {
            ESP_LOGE(TAG, "Failed to parse AVI header");
            return false;
        }
    }
    
    // Se positionner au bon endroit si on a modifié la position
    if (ftell(ctx->input_file) != ctx->avi_current_offset) {
        fseek(ctx->input_file, ctx->avi_current_offset, SEEK_SET);
    }
    
    // Lire l'en-tête du chunk
    char chunk_id[4];
    uint32_t chunk_size;
    
    if (fread(chunk_id, 4, 1, ctx->input_file) != 1) {
        ESP_LOGE(TAG, "Failed to read chunk ID or end of file");
        // Essai de rebouclage sur le fichier
        fseek(ctx->input_file, ctx->avi_data_offset, SEEK_SET);
        ctx->avi_current_offset = ctx->avi_data_offset;
        return false;
    }
    
    if (fread(&chunk_size, 4, 1, ctx->input_file) != 1) {
        ESP_LOGE(TAG, "Failed to read chunk size");
        return false;
    }
    
    // Vérifier si c'est un chunk de données vidéo (00dc, 00db, etc.)
    if (chunk_id[0] == '0' && chunk_id[1] == '0' && 
        (chunk_id[2] == 'd' || chunk_id[2] == 'w') && 
        (chunk_id[3] == 'c' || chunk_id[3] == 'b')) {
        
        ESP_LOGD(TAG, "Video chunk: %.4s, Size: %d", chunk_id, chunk_size);
        
        // Limiter à la taille du buffer
        size_t to_read = chunk_size > buffer_size ? buffer_size : chunk_size;
        
        *bytes_read = fread(buffer, 1, to_read, ctx->input_file);
        
        if (*bytes_read <= 0) {
            ESP_LOGE(TAG, "Failed to read frame data");
            return false;
        }
        
        // Si MJPEG et pas de marqueur JPEG au début, chercher le marqueur
        if (ctx->is_mjpeg && (buffer[0] != 0xFF || buffer[1] != 0xD8)) {
            int marker_pos = find_jpeg_marker(buffer, *bytes_read);
            if (marker_pos >= 0) {
                ESP_LOGD(TAG, "Found JPEG marker at position %d", marker_pos);
                memmove(buffer, buffer + marker_pos, *bytes_read - marker_pos);
                *bytes_read -= marker_pos;
            } else {
                ESP_LOGE(TAG, "No JPEG marker found in frame data");
                return false;
            }
        }
        
        // Mettre à jour l'offset pour la prochaine lecture
        ctx->avi_current_offset = ftell(ctx->input_file);
        
        // Ajustement pour l'alignement
        if (chunk_size % 2 == 1) {
            fseek(ctx->input_file, 1, SEEK_CUR);
            ctx->avi_current_offset = ftell(ctx->input_file);
        }
        
        return true;
    }
    else {
        // Ce n'est pas un chunk vidéo, l'ignorer
        ESP_LOGD(TAG, "Skipping non-video chunk: %.4s, Size: %d", chunk_id, chunk_size);
        fseek(ctx->input_file, chunk_size, SEEK_CUR);
        
        // Ajustement pour l'alignement
        if (chunk_size % 2 == 1) {
            fseek(ctx->input_file, 1, SEEK_CUR);
        }
        
        ctx->avi_current_offset = ftell(ctx->input_file);
        
        // Si on atteint la fin du fichier, revenir au début des données
        if (feof(ctx->input_file) || ctx->avi_current_offset >= ctx->avi_data_offset + chunk_size) {
            ESP_LOGI(TAG, "Reached end of AVI file, restarting");
            fseek(ctx->input_file, ctx->avi_data_offset, SEEK_SET);
            ctx->avi_current_offset = ctx->avi_data_offset;
        }
        
        // Continuer à chercher un chunk vidéo
        return read_file_avi_frame(ctx, buffer, buffer_size, bytes_read);
    }
}

// Lecture d'une frame MJPEG depuis un fichier
static bool read_file_mjpeg_frame(esp_ffmpeg_context_t *ctx, uint8_t *buffer,
                             size_t buffer_size, size_t *bytes_read) {
    if (ctx->input_file == NULL) return false;
    
    // Si c'est un fichier AVI, utiliser le lecteur AVI
    if (ctx->is_avi) {
        return read_file_avi_frame(ctx, buffer, buffer_size, bytes_read);
    }
    
    // Sinon, utiliser le lecteur MJPEG original
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
        // Réduire la taille du buffer HTTP pour éviter les problèmes d'allocation
        size_t http_buffer_size = buffer_size > 8192 ? 8192 : buffer_size;
        
        esp_http_client_config_t config = {
            .url = ctx->source_url,
            .timeout_ms = 5000,
            .buffer_size = http_buffer_size,
            .disable_auto_redirect = false,
        };
        
        // Essayer d'allouer avec une taille plus petite en cas d'échec
        ctx->http_client = esp_http_client_init(&config);
        if (ctx->http_client == NULL) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client with buffer size %d", http_buffer_size);
            
            // Réessayer avec un buffer plus petit
            config.buffer_size = 4096;  // Taille minimale
            ctx->http_client = esp_http_client_init(&config);
            
            if (ctx->http_client == NULL) {
                ESP_LOGE(TAG, "Failed to initialize HTTP client even with reduced buffer");
                return false;
            }
            ESP_LOGI(TAG, "HTTP client initialized with reduced buffer size");
        }
    }
    
    // Ouvrir la connexion avec méthode GET explicite
    err = esp_http_client_set_method(ctx->http_client, HTTP_METHOD_GET);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set HTTP method: %s", esp_err_to_name(err));
    }
    
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
    
    // Extraire le type de contenu pour détecter le format
    char content_type[64] = {0};
    char *content_type_ptr = NULL;
    if (esp_http_client_get_header(ctx->http_client, "Content-Type", &content_type_ptr) == ESP_OK && content_type_ptr != NULL) {
        // Copier la valeur dans notre buffer pour éviter des problèmes de mémoire
        strncpy(content_type, content_type_ptr, sizeof(content_type) - 1);
        content_type[sizeof(content_type) - 1] = '\0'; // S'assurer que c'est terminé par un 0
        
        ESP_LOGI(TAG, "Content type: %s, length: %d", content_type, content_length);
        
        // Détection des types de fichiers vidéo
        bool is_mp4 = (strstr(content_type, "mp4") != NULL || strstr(content_type, "video/mp4") != NULL);
        bool is_avi = (strstr(content_type, "avi") != NULL || strstr(content_type, "video/avi") != NULL);
        
        if (is_mp4) {
            ESP_LOGW(TAG, "MP4 format detected - not directly supported, attempting to extract JPEG frames");
            ctx->is_mp4 = true;
        } else if (is_avi) {
            ESP_LOGI(TAG, "AVI format detected via HTTP");
            ctx->is_avi = true;
        }
    }
    
    // Limiter la lecture au buffer_size
    int to_read = content_length > 0 && content_length < buffer_size ? 
                  content_length : buffer_size;
    
    // Lire le corps avec gestion des échecs
    int total_read = 0;
    int remaining = to_read;
    int retry_count = 0;
    const int max_retries = 3;
    
    while (remaining > 0 && retry_count < max_retries) {
        int read_len = esp_http_client_read(ctx->http_client, 
                                          (char*)buffer + total_read, 
                                          remaining);
        if (read_len <= 0) {
            ESP_LOGW(TAG, "HTTP read returned %d, retry %d/%d", 
                   read_len, retry_count + 1, max_retries);
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(100));  // Petit délai avant nouvelle tentative
            continue;
        }
        
        total_read += read_len;
        remaining -= read_len;
    }
    
    esp_http_client_close(ctx->http_client);
    
    if (total_read <= 0) {
        ESP_LOGE(TAG, "HTTP client read failed after %d retries", retry_count);
        return false;
    }
    
    *bytes_read = total_read;
    
    // Pour MP4: traitement spécial pour extraire les keyframes JPEG si possible
    if (ctx->is_mp4) {
        int marker_pos = find_jpeg_marker(buffer, total_read);
        if (marker_pos >= 0) {
            ESP_LOGI(TAG, "Found JPEG marker in MP4 data at position %d", marker_pos);
            memmove(buffer, buffer + marker_pos, total_read - marker_pos);
            *bytes_read = total_read - marker_pos;
        } else {
            ESP_LOGW(TAG, "No JPEG marker found in MP4 data, continuing anyway");
            // On continue quand même, peut-être qu'on aura plus de chance avec les frames suivantes
        }
    }
    // Pour d'autres formats (MJPEG ou inconnus): vérifier pour JPEG
    else if (!ctx->is_avi && (buffer[0] != 0xFF || buffer[1] != 0xD8)) {
        ESP_LOGW(TAG, "Data doesn't start with JPEG marker, looking for marker");
        int marker_pos = find_jpeg_marker(buffer, total_read);
        
        if (marker_pos >= 0) {
            ESP_LOGI(TAG, "Found JPEG marker at position %d", marker_pos);
            memmove(buffer, buffer + marker_pos, total_read - marker_pos);
            *bytes_read = total_read - marker_pos;
        } else {
            ESP_LOGW(TAG, "No JPEG marker found in data");
            // On continue malgré tout - peut-être pas une frame JPEG
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
                // Si HTTP, essayer de réinitialiser la connexion
                if (ctx->source_type == ESP_FFMPEG_SOURCE_TYPE_HTTP && ctx->http_client) {
                    ESP_LOGW(TAG, "Trying to reset HTTP connection");
                    esp_http_client_cleanup(ctx->http_client);
                    ctx->http_client = NULL;
                    consecutive_errors = max_consecutive_errors - 1; // Donner une chance de plus
                } else {
                    break;
                }
            }
            
            vTaskDelay(delay_time); // Attendre avant de réessayer
            continue;
        }
        
        consecutive_errors = 0; // Réinitialiser le compteur d'erreurs
        
        ESP_LOGD(TAG, "Read %d bytes of video data", bytes_read);
        
        // Pour MP4 et autres formats, essayer plusieurs fois de trouver un JPEG
        bool decoded = false;
        int decode_attempts = 0;
        const int max_decode_attempts = 3;
        
        while (!decoded && decode_attempts < max_decode_attempts) {
            if (decode_jpeg(ctx->buffer, bytes_read, rgb565_buffer, ctx->width, ctx->height)) {
                decoded = true;
                break;
            }
            
            // Si échec du décodage, essayer de trouver un autre marqueur JPEG plus loin dans le buffer
            int marker_pos = find_jpeg_marker(ctx->buffer + 2, bytes_read - 2);
            if (marker_pos < 0) {
                ESP_LOGE(TAG, "No additional JPEG marker found in buffer");
                break;
            }
            
            marker_pos += 2; // Ajuster pour le début du buffer
            ESP_LOGW(TAG, "Trying alternate JPEG marker at position %d", marker_pos);
            memmove(ctx->buffer, ctx->buffer + marker_pos, bytes_read - marker_pos);
            bytes_read -= marker_pos;
            decode_attempts++;
        }
        
        if (!decoded) {
            ESP_LOGE(TAG, "Failed to decode JPEG frame after %d attempts", decode_attempts);
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

    // Détection du format basée sur l'extension du fichier
    bool is_avi = false;
    bool is_mp4 = false;
    
    const char *ext = strrchr(source_url, '.');
    if (ext) {
        is_avi = (strcasecmp(ext, ".avi") == 0);
        is_mp4 = (strcasecmp(ext, ".mp4") == 0);
    }
    
    // Si c'est un MP4, avertir que le support est limité
    if (is_mp4) {
        ESP_LOGW(TAG, "MP4 format detected. Note: limited MP4 support, only MJPEG frames will be extracted");
    }

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
    new_ctx->is_avi = is_avi;
    new_ctx->is_mp4 = is_mp4;
    new_ctx->avi_data_offset = 0;
    new_ctx->avi_current_offset = 0;
    new_ctx->avi_frame_size = 0;
    new_ctx->avi_total_frames = 0;
    new_ctx->retry_count = 0;
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
        
        // Vérifier si c'est un fichier AVI si pas déjà détecté
        if (!is_avi) {
            char file_header[12];
            size_t read_bytes = fread(file_header, 1, 12, new_ctx->input_file);
            fseek(new_ctx->input_file, 0, SEEK_SET); // Revenir au début
            
            if (read_bytes >= 12 && 
                strncmp(file_header, "RIFF", 4) == 0 && 
                strncmp(file_header + 8, "AVI ", 4) == 0) {
                ESP_LOGI(TAG, "File is in AVI format");
                new_ctx->is_avi = true;
            }
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


