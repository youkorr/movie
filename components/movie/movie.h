#pragma once
#include "esphome/core/component.h"
#include "esphome/components/display/display_buffer.h"
#include "esphome/core/color.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp32_ffmpeg.h"


namespace esphome {
namespace movie {

// Enumération des formats vidéo
enum VideoFormat {
    FORMAT_MJPEG = 1,
    FORMAT_AVI = 2,
    FORMAT_BINARY = 3,
    FORMAT_RGB565 = 4,
};

// Constantes exportables vers Python (__init__.py)
static const VideoFormat VIDEO_FORMAT_MJPEG = FORMAT_MJPEG;
static const VideoFormat VIDEO_FORMAT_AVI = FORMAT_AVI;

class MoviePlayer : public Component {
public:
    MoviePlayer();
    ~MoviePlayer();
    void setup() override;
    void loop() override;
    void dump_config() override;

    // Méthodes pour configurer le lecteur
    void set_display(display::DisplayBuffer *display) { this->display_ = display; }
    void set_width(int width) { this->width_ = width; }
    void set_height(int height) { this->height_ = height; }
    void set_format(VideoFormat format) { this->current_format_ = format; }
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





