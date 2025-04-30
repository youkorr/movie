#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/network/ip_address.h"
#include "esp_http_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <string>
#include <vector>

namespace esphome {
namespace movie {

class MoviePlayer : public Component {
 public:
  MoviePlayer();
  ~MoviePlayer();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void set_url(const std::string &url);
  void set_dimensions(int width, int height);
  
  void start_playback();
  void stop_playback();
  void pause_playback();
  void resume_playback();
  
  bool is_playing() const { return playing_; }
  float get_current_time() const { return current_time_; }
  float get_total_time() const { return total_time_; }
  int get_width() const { return width_; }
  int get_height() const { return height_; }

 protected:
  void video_task();
  bool init_mp4_parser();
  bool fetch_video_data();
  void decode_and_render_frame();
  void cleanup_resources();
  
  static void video_task_static(void *args);

  std::string url_;
  int width_{320};
  int height_{240};
  
  bool playing_{false};
  bool paused_{false};
  float current_time_{0.0f};
  float total_time_{0.0f};
  
  // HTTP client for fetching video
  esp_http_client_handle_t http_client_{nullptr};
  
  // Buffer for video data
  uint8_t *video_buffer_{nullptr};
  size_t buffer_size_{32768}; // 32KB buffer by default
  size_t data_length_{0};
  
  // Task handle for video decoding
  TaskHandle_t video_task_handle_{nullptr};
  
  // Synchronization objects
  SemaphoreHandle_t buffer_mutex_{nullptr};
  QueueHandle_t frame_queue_{nullptr};
  
  // Frame data structure
  struct FrameData {
    uint8_t *data{nullptr};
    size_t size{0};
    int64_t pts{0};
  };
};

// Actions pour contrôler le lecteur vidéo
class PlayAction : public Action<> {
 public:
  explicit PlayAction(MoviePlayer *player) : player_(player) {}
  
  void set_url(const std::string &url) { url_ = url; }
  
  void play(AsyncFunctionCall *call) override {
    if (!url_.empty()) {
      this->player_->set_url(url_);
    }
    this->player_->start_playback();
    call->over();
  }
  
 protected:
  MoviePlayer *player_;
  std::string url_{};
};

class StopAction : public Action<> {
 public:
  explicit StopAction(MoviePlayer *player) : player_(player) {}
  
  void play(AsyncFunctionCall *call) override {
    this->player_->stop_playback();
    call->over();
  }
  
 protected:
  MoviePlayer *player_;
};

class PauseAction : public Action<> {
 public:
  explicit PauseAction(MoviePlayer *player) : player_(player) {}
  
  void play(AsyncFunctionCall *call) override {
    this->player_->pause_playback();
    call->over();
  }
  
 protected:
  MoviePlayer *player_;
};

class ResumeAction : public Action<> {
 public:
  explicit ResumeAction(MoviePlayer *player) : player_(player) {}
  
  void play(AsyncFunctionCall *call) override {
    this->player_->resume_playback();
    call->over();
  }
  
 protected:
  MoviePlayer *player_;
};

class SetUrlAction : public Action<> {
 public:
  explicit SetUrlAction(MoviePlayer *player) : player_(player) {}
  
  void set_url(const std::string &url) { url_ = url; }
  
  void play(AsyncFunctionCall *call) override {
    this->player_->set_url(url_);
    call->over();
  }
  
 protected:
  MoviePlayer *player_;
  std::string url_{};
};

}  // namespace movie
}  // namespace esphome
