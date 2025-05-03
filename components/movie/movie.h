#pragma once
#include "esphome/core/component.h"
#include "esphome/components/display/display_buffer.h"
#include "esphome/core/color.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Types pour FFmpeg
typedef enum {
    ESP_FFMPEG_SOURCE_TYPE_FILE,
    ESP_FFMPEG_SOURCE_TYPE_HTTP
} esp_ffmpeg_source_type_t;

// Structures FFmpeg
typedef struct {
    uint8_t *data;
    int width;
    int height;
    // Autres champs si nécessaire
} esp_ffmpeg_frame_t;

typedef void* esp_ffmpeg_context_t;

// Prototypes des fonctions FFmpeg
extern "C" {
    esp_err_t esp_ffmpeg_init(const char *source, esp_ffmpeg_source_type_t source_type, 
                             void (*frame_callback)(esp_ffmpeg_frame_t*, void*), 
                             void *user_data, esp_ffmpeg_context_t *ctx);
    esp_err_t esp_ffmpeg_start(esp_ffmpeg_context_t ctx);
    esp_err_t esp_ffmpeg_stop(esp_ffmpeg_context_t ctx);
}

namespace esphome {
namespace movie {

// Définition des formats vidéo - assurez-vous d'utiliser FORMAT_MJPEG et non VIDEO_FORMAT_MJPEG
enum VideoFormat {
    FORMAT_MJPEG,
    FORMAT_BINARY,
    FORMAT_RGB565
};

class MoviePlayer : public Component {
public:
    MoviePlayer();
    ~MoviePlayer();
    void setup() override;
    void loop() override;
    void dump_config() override;
    
    // Méthodes pour configurer le lecteur
    void set_display(display::DisplayBuffer *display) { this->display_ = display; }
    
    // Ajout des méthodes qui manquent selon les erreurs dans le YAML
    void set_width(int width) { this->width_ = width; }
    void set_height(int height) { this->height_ = height; }
    void set_format(VideoFormat format) { this->current_format_ = format; }
    
    // Les méthodes existantes
    void set_dimensions(int width, int height) {
        this->width_ = width;
        this->height_ = height;
    }
    void set_buffer_size(size_t size) { this->buffer_size_ = size; }
    void set_fps(int fps) { this->fps_ = fps; }
    void set_http_timeout(int timeout_ms) { this->http_timeout_ms_ = timeout_ms; }
    
    // Méthodes pour la lecture
    bool play_file(const std::string &file_path, VideoFormat format);
    bool play_http_stream(const std::string &url, VideoFormat format);
    void stop();
    
    // Callback statique pour FFmpeg
    static void ffmpeg_frame_callback(esp_ffmpeg_frame_t *frame, void *user_data);

protected:
    // Méthode d'affichage d'une frame
    bool display_frame(const uint8_t *data, int width, int height);
    
    // Affichage
    display::DisplayBuffer *display_{nullptr};
    int width_{320};
    int height_{240};
    
    // Configuration
    size_t buffer_size_{32 * 1024};
    int fps_{30};
    int http_timeout_ms_{5000};
    
    // État de lecture
    bool playing_{false};
    std::string current_path_{};
    VideoFormat current_format_{FORMAT_MJPEG};
    esp_ffmpeg_source_type_t current_source_type_{ESP_FFMPEG_SOURCE_TYPE_FILE};
    esp_ffmpeg_context_t ffmpeg_ctx_{nullptr};
    
    // Statistiques
    uint32_t frames_displayed_{0};
    uint32_t last_frame_time_{0};
    uint32_t avg_fps_{0};
    
    // Synchronisation
    SemaphoreHandle_t mutex_{nullptr};
};

}  // namespace movie
}  // namespace esphome




