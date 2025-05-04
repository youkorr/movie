#include "movie.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esp_system.h"
#include "esphome/components/display/display.h"

using namespace esphome::display;

static const char *TAG = "movie";

namespace esphome {
namespace movie {

MoviePlayer::MoviePlayer() {
  this->mutex_ = xSemaphoreCreateMutex();
  this->scaling_mode_ = SCALE_FIT;  // Changed from SCALE_CENTER to SCALE_FIT
}

MoviePlayer::~MoviePlayer() {
  this->stop();
  if (this->mutex_ != nullptr) {
    vSemaphoreDelete(this->mutex_);
    this->mutex_ = nullptr;
  }
}

void MoviePlayer::setup() {
  if (this->display_ != nullptr) {
    this->width_ = this->display_->get_width();
    this->height_ = this->display_->get_height();
    ESP_LOGI(TAG, "Display size: %dx%d", width_, height_);
  } else {
    ESP_LOGE(TAG, "No display configured!");
    this->mark_failed();
  }
}

void MoviePlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "Movie Player:");
  ESP_LOGCONFIG(TAG, "  Width: %d", this->width_);
  ESP_LOGCONFIG(TAG, "  Height: %d", this->height_);
  ESP_LOGCONFIG(TAG, "  Buffer Size: %u bytes", this->buffer_size_);
  ESP_LOGCONFIG(TAG, "  Target FPS: %d", this->fps_);
  ESP_LOGCONFIG(TAG, "  HTTP Timeout: %d ms", this->http_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Default Format: %d", this->default_format_);
  ESP_LOGCONFIG(TAG, "  Threshold: %d", this->threshold_);
  ESP_LOGCONFIG(TAG, "  Scaling Mode: %d", this->scaling_mode_);
}

void MoviePlayer::loop() {
  if (this->playing_ && this->frames_displayed_ > 0) {
    uint32_t now = millis();
    uint32_t elapsed = now - this->last_frame_time_;
    if (elapsed >= 1000) {
      this->avg_fps_ = (this->frames_displayed_ * 1000) / elapsed;
      ESP_LOGI(TAG, "FPS: %lu", this->avg_fps_);
      this->frames_displayed_ = 0;
      this->last_frame_time_ = now;
    }
  }
}

bool MoviePlayer::play_file(const std::string &file_path, VideoFormat format) {
  if (this->playing_) stop();
  this->current_path_ = file_path;
  this->current_format_ = resolve_format(file_path, format);
  this->current_source_type_ = ESP_FFMPEG_SOURCE_TYPE_FILE;
  return this->start_ffmpeg_async();
}

bool MoviePlayer::play_http_stream(const std::string &url, VideoFormat format) {
  if (this->playing_) stop();
  this->current_path_ = url;
  this->current_format_ = resolve_format(url, format);
  this->current_source_type_ = ESP_FFMPEG_SOURCE_TYPE_HTTP;
  return this->start_ffmpeg_async();
}

VideoFormat MoviePlayer::resolve_format(const std::string &path, VideoFormat format) {
  if (format != VIDEO_FORMAT_AUTO) return format;
  if (path.find(".avi") != std::string::npos) return VIDEO_FORMAT_AVI;
  if (path.find(".mp4") != std::string::npos) return VIDEO_FORMAT_MP4;
  return VIDEO_FORMAT_MJPEG;
}

bool MoviePlayer::start_ffmpeg_async() {
  this->frames_displayed_ = 0;
  this->last_frame_time_ = millis();
  this->playing_ = true;

  BaseType_t result = xTaskCreatePinnedToCore(
    [](void *param) {
      MoviePlayer *self = static_cast<MoviePlayer *>(param);
      esp_err_t ret = esp_ffmpeg_init(
        self->current_path_.c_str(),
        self->current_source_type_,
        MoviePlayer::ffmpeg_frame_callback,
        self,
        &self->ffmpeg_ctx_
      );
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FFmpeg init failed: %s", esp_err_to_name(ret));
        self->playing_ = false;
        vTaskDelete(nullptr);
        return;
      }

      ESP_LOGI(TAG, "Starting FFmpeg decoding...");
      ret = esp_ffmpeg_start(self->ffmpeg_ctx_);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FFmpeg start failed: %s", esp_err_to_name(ret));
      }

      self->playing_ = false;
      self->ffmpeg_ctx_ = nullptr;
      ESP_LOGI(TAG, "FFmpeg decoding finished.");
      vTaskDelete(nullptr);
    },
    "ffmpeg_task",
    8192,  // Stack size
    this,
    5,     // Priority
    &this->ffmpeg_task_,
    1      // Core
  );

  if (result != pdPASS) {
    ESP_LOGE(TAG, "Failed to create FFmpeg task");
    this->playing_ = false;
    return false;
  }

  return true;
}

void MoviePlayer::stop() {
  if (!this->playing_) return;

  ESP_LOGI(TAG, "Stopping playback...");
  this->playing_ = false;

  if (this->ffmpeg_ctx_) {
    esp_ffmpeg_stop(this->ffmpeg_ctx_);
    this->ffmpeg_ctx_ = nullptr;
  }

  if (this->ffmpeg_task_ != nullptr) {
    // FFmpeg lib stops internally and deletes task
    this->ffmpeg_task_ = nullptr;
  }

  ESP_LOGI(TAG, "Playback stopped.");
}

void MoviePlayer::ffmpeg_frame_callback(esp_ffmpeg_frame_t *frame, void *user_data) {
  MoviePlayer *self = static_cast<MoviePlayer *>(user_data);
  if (!self || !frame || !frame->data || !self->playing_) return;
  self->display_frame(frame->data, frame->width, frame->height);
  self->frames_displayed_++;
}

bool MoviePlayer::display_frame(const uint8_t *data, int width, int height) {
  if (!this->display_ || !data) return false;

  if (xSemaphoreTake(this->mutex_, portMAX_DELAY) != pdTRUE) return false;

  const uint16_t *rgb_data = reinterpret_cast<const uint16_t *>(data);
  this->display_->fill(COLOR_OFF);

  float scale_x = 1.0, scale_y = 1.0;
  int pos_x = 0, pos_y = 0;

  switch (this->scaling_mode_) {
    case SCALE_FILL:
      scale_x = (float)this->width_ / width;
      scale_y = (float)this->height_ / height;
      break;
      
    case SCALE_FIT:
      scale_x = scale_y = std::min((float)this->width_ / width, (float)this->height_ / height);
      pos_x = (this->width_ - width * scale_x) / 2;
      pos_y = (this->height_ - height * scale_y) / 2;
      break;
      
    case SCALE_NONE:
    default:
      // Center the image without scaling
      pos_x = (this->width_ - width) / 2;
      pos_y = (this->height_ - height) / 2;
      break;
  }

  int threshold = this->threshold_;

  for (int y = 0; y < height; y++) {
    int ty = pos_y + y * scale_y;
    if (ty >= this->height_ || ty < 0) continue;
    for (int x = 0; x < width; x++) {
      int tx = pos_x + x * scale_x;
      if (tx >= this->width_ || tx < 0) continue;

      uint16_t pixel = rgb_data[y * width + x];
      uint8_t r = ((pixel >> 11) & 0x1F) << 3;
      uint8_t g = ((pixel >> 5) & 0x3F) << 2;
      uint8_t b = (pixel & 0x1F) << 3;
      uint8_t lum = (r * 3 + g * 6 + b) / 10;

      this->display_->draw_pixel_at(tx, ty, (lum > threshold) ? COLOR_ON : COLOR_OFF);
    }
  }

  this->display_->update();
  xSemaphoreGive(this->mutex_);
  return true;
}

void MoviePlayer::set_threshold(uint8_t threshold) {
  this->threshold_ = threshold;
}

void MoviePlayer::set_scaling_mode(ScalingMode mode) {
  this->scaling_mode_ = mode;
}

}  // namespace movie
}  // namespace esphome









