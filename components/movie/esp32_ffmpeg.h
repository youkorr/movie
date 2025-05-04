#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/display/display_buffer.h"

#ifdef USE_ESP32

#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_http_client.h>

namespace esphome {
namespace esp32_ffmpeg {

// Types repris de l'original
typedef struct {
    uint8_t *data;
    size_t size;
    int width;
    int height;
    int64_t pts;
} esp_ffmpeg_frame_t;

typedef enum {
    ESP_FFMPEG_SOURCE_TYPE_FILE,
    ESP_FFMPEG_SOURCE_TYPE_HTTP
} esp_ffmpeg_source_type_t;

typedef void (*esp_ffmpeg_frame_callback_t)(esp_ffmpeg_frame_t *frame, void *user_data);

struct esp_ffmpeg_context_s;
typedef struct esp_ffmpeg_context_s esp_ffmpeg_context_t;

// Classe principale du composant ESPHome
class ESP32FFmpegComponent : public esphome::Component {
public:
    ESP32FFmpegComponent();
    ~ESP32FFmpegComponent();

    // Setters pour la configuration
    void set_source_url(const std::string &url) { source_url_ = url; }
    void set_source_type(const std::string &type) {
        if (type == "http") {
            source_type_ = ESP_FFMPEG_SOURCE_TYPE_HTTP;
        } else {
            source_type_ = ESP_FFMPEG_SOURCE_TYPE_FILE;
        }
    }
    void set_width(int width) { width_ = width; }
    void set_height(int height) { height_ = height; }
    
    // Méthodes ESPHome
    void setup() override;
    void loop() override;
    void dump_config() override;
    
    float get_setup_priority() const override { return setup_priority::LATE; }
    
    // Accès au dernier frame
    uint16_t *get_current_frame() { return current_frame_; }
    int get_width() const { return width_; }
    int get_height() const { return height_; }
    bool has_new_frame() const { return has_new_frame_; }
    void frame_consumed() { has_new_frame_ = false; }
    
    // Pour les triggers/actions
    void set_on_frame_callback(std::function<void()> callback) { frame_callback_ = std::move(callback); }
    
protected:
    static void frame_callback_static(esp_ffmpeg_frame_t *frame, void *user_data);
    void on_frame(esp_ffmpeg_frame_t *frame);
    
    std::string source_url_;
    esp_ffmpeg_source_type_t source_type_{ESP_FFMPEG_SOURCE_TYPE_HTTP};
    int width_{128};
    int height_{64};
    
    esp_ffmpeg_context_t *ctx_{nullptr};
    uint16_t *current_frame_{nullptr};
    bool has_new_frame_{false};
    std::function<void()> frame_callback_{};
};

// Trigger quand un nouveau frame est disponible
class NewFrameTrigger : public Trigger<> {
public:
    explicit NewFrameTrigger(ESP32FFmpegComponent *parent) {
        parent->set_on_frame_callback([this]() { this->trigger(); });
    }
};

} // namespace esp32_ffmpeg
} // namespace esphome

#endif // USE_ESP32
