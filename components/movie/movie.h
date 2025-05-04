#pragma once

#include "esphome/core/component.h"
#include "esphome/components/display/display_buffer.h"
#include "esp32_ffmpeg.h"  // Inclure votre code FFmpeg

namespace esphome {
namespace movie {

enum VideoFormat {
  VIDEO_FORMAT_AUTO = 0,
  VIDEO_FORMAT_MJPEG = 1,
  VIDEO_FORMAT_AVI = 2,
  VIDEO_FORMAT_MP4 = 3,
};

enum ScalingMode {
  SCALE_NONE = 0,  // Pas de mise à l'échelle (1:1)
  SCALE_FIT = 1,   // Maintenir les proportions
  SCALE_FILL = 2,  // Remplir l'écran (peut déformer l'image)
};

class MoviePlayer : public Component {
 public:
  MoviePlayer();
  ~MoviePlayer();
  
  void setup() override;
  void loop() override;
  void dump_config() override;
  
  bool play_file(const std::string &file_path, VideoFormat format = VIDEO_FORMAT_AUTO);
  bool play_http_stream(const std::string &url, VideoFormat format = VIDEO_FORMAT_AUTO);
  void stop();
  
  bool is_playing() const { return this->playing_; }
  
  void set_display(display::DisplayBuffer *display) { this->display_ = display; }
  void set_width(int width) { this->width_ = width; }
  void set_height(int height) { this->height_ = height; }
  void set_buffer_size(size_t buffer_size) { this->buffer_size_ = buffer_size; }
  void set_fps(int fps) { this->fps_ = fps; }
  void set_http_timeout(int timeout_ms) { this->http_timeout_ms_ = timeout_ms; }
  
  // Nouvelles méthodes
  void set_threshold(uint8_t threshold);
  void set_scaling_mode(ScalingMode mode);
  void set_format(VideoFormat format) { this->default_format_ = format; }

  
 protected:
  static void ffmpeg_frame_callback(esp_ffmpeg_frame_t *frame, void *user_data);
  bool display_frame(const uint8_t *data, int width, int height);

  bool start_ffmpeg_async();
  VideoFormat resolve_format(const std::string &path, VideoFormat format);
  
  display::DisplayBuffer *display_{nullptr};
  int width_{128};
  int height_{64};
  size_t buffer_size_{32768};  // 32KB
  int fps_{10};
  int http_timeout_ms_{5000};
  
  bool playing_{false};
  std::string current_path_;
  VideoFormat default_format_{VIDEO_FORMAT_AUTO};
  VideoFormat current_format_{VIDEO_FORMAT_MJPEG};
  esp_ffmpeg_source_type_t current_source_type_;
  esp_ffmpeg_context_t *ffmpeg_ctx_{nullptr};
  
  uint32_t frames_displayed_{0};
  uint32_t last_frame_time_{0};
  uint32_t avg_fps_{0};
  
  SemaphoreHandle_t mutex_{nullptr};

  TaskHandle_t ffmpeg_task_{nullptr};


  
  // Nouveaux membres
  uint8_t threshold_{128};  // Valeur par défaut pour la conversion monochrome
  ScalingMode scaling_mode_{SCALE_FIT};  // Mode de mise à l'échelle par défaut
};

}  // namespace movie
}  // namespace esphome






