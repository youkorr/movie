#pragma once

#include "esphome/core/component.h"
#include "esphome/components/display/display_buffer.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string>

// Inclure notre mini-FFmpeg
#include "esp32_ffmpeg.h"

namespace esphome {
namespace movie {

// Formats vidéo pris en charge
enum VideoFormat {
  VIDEO_FORMAT_MJPEG,
  VIDEO_FORMAT_MP4,
};

class MoviePlayer : public Component {
 public:
  MoviePlayer();
  virtual ~MoviePlayer();
  
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_display_width(int width) { this->width_ = width; }
  void set_display_height(int height) { this->height_ = height; }
  void set_buffer_size(int buffer_size) { this->buffer_size_ = buffer_size; }
  void set_fps(int fps) { this->fps_ = fps; }
  void set_http_timeout(int timeout_ms) { this->http_timeout_ms_ = timeout_ms; }
  
  // Méthodes publiques pour contrôler la lecture
  bool play_file(const std::string &file_path, VideoFormat format = VIDEO_FORMAT_MJPEG);
  bool play_http_stream(const std::string &url, VideoFormat format = VIDEO_FORMAT_MJPEG);
  void stop();
  bool is_playing() const { return this->playing_; }
  
 protected:
  // Callback pour recevoir les frames de FFmpeg
  static void ffmpeg_frame_callback(esp_ffmpeg_frame_t *frame, void *user_data);
  
  // Méthode pour afficher une frame
  bool display_frame(const uint8_t *data, int width, int height);
  
  // Affichage
  display::DisplayBuffer *display_{nullptr};
  int width_{320};
  int height_{240};
  int buffer_size_{8192};
  int fps_{15};
  int http_timeout_ms_{5000};
  
  // État de lecture
  bool playing_{false};
  std::string current_path_;
  VideoFormat current_format_{VIDEO_FORMAT_MJPEG};
  esp_ffmpeg_source_type_t current_source_type_{ESP_FFMPEG_SOURCE_TYPE_FILE};
  
  // Gestion des mutex
  SemaphoreHandle_t mutex_{nullptr};
  
  // FFmpeg context
  esp_ffmpeg_context_t *ffmpeg_ctx_{nullptr};
  
  // Statistiques
  uint32_t frames_displayed_{0};
  uint32_t last_frame_time_{0};
  uint32_t avg_fps_{0};
};

}  // namespace movie
}  // namespace esphome




