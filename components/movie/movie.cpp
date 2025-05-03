#include "movie.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"  // Ajouté pour millis()
#include "esp_system.h"
#include "esphome/components/display/display.h"  // Ajouté pour COLOR_ON / COLOR_OFF

using namespace esphome::display;  // Ajouté pour éviter les longues notations

static const char *TAG = "movie";

namespace esphome {
namespace movie {

MoviePlayer::MoviePlayer() {
  this->mutex_ = xSemaphoreCreateMutex();
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
    ESP_LOGI(TAG, "Using provided display: %dx%d", this->display_->get_width(), this->display_->get_height());
    
    // Mettre à jour les dimensions en fonction de l'écran réel
    this->width_ = this->display_->get_width();
    this->height_ = this->display_->get_height();
  } else {
    ESP_LOGE(TAG, "No display found!");
    this->mark_failed();
    return;
  }

  this->frames_displayed_ = 0;
  this->last_frame_time_ = 0;
  this->avg_fps_ = 0;

  ESP_LOGI(TAG, "Movie player initialized with display size %dx%d", this->width_, this->height_);
}

void MoviePlayer::loop() {
  if (this->playing_ && this->frames_displayed_ > 0) {
    uint32_t current_time = esphome::millis();
    uint32_t elapsed = current_time - this->last_frame_time_;
    
    if (elapsed > 1000) {
      this->avg_fps_ = (this->frames_displayed_ * 1000) / elapsed;
      ESP_LOGI(TAG, "Playback stats - Frames: %lu, FPS: %lu", 
              this->frames_displayed_, this->avg_fps_);
      
      this->frames_displayed_ = 0;
      this->last_frame_time_ = current_time;
    }
  }
}

void MoviePlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "Movie Player:");
  ESP_LOGCONFIG(TAG, "  Display Size: %dx%d", this->width_, this->height_);
  ESP_LOGCONFIG(TAG, "  Buffer Size: %d bytes", this->buffer_size_);
  ESP_LOGCONFIG(TAG, "  Target FPS: %d", this->fps_);
  ESP_LOGCONFIG(TAG, "  HTTP Timeout: %d ms", this->http_timeout_ms_);
}

bool MoviePlayer::play_file(const std::string &file_path, VideoFormat format) {
  if (this->playing_) {
    ESP_LOGW(TAG, "Already playing a video, stopping current playback");
    this->stop();
  }

  // Détection du format basée sur l'extension
  if (format == VIDEO_FORMAT_AUTO) {
    if (file_path.find(".avi") != std::string::npos) {
      format = VIDEO_FORMAT_AVI;
      ESP_LOGI(TAG, "Auto-detected AVI format");
    } else if (file_path.find(".mp4") != std::string::npos) {
      format = VIDEO_FORMAT_MP4;
      ESP_LOGI(TAG, "Auto-detected MP4 format (limited support)");
    } else {
      format = VIDEO_FORMAT_MJPEG;
      ESP_LOGI(TAG, "Default to MJPEG format");
    }
  }

  ESP_LOGI(TAG, "Starting playback of local file: '%s' (format: %d)", file_path.c_str(), format);
  this->current_path_ = file_path;
  this->current_format_ = format;
  this->current_source_type_ = ESP_FFMPEG_SOURCE_TYPE_FILE;
  
  esp_err_t ret = esp_ffmpeg_init(
    file_path.c_str(),
    ESP_FFMPEG_SOURCE_TYPE_FILE,
    MoviePlayer::ffmpeg_frame_callback,
    this,
    &this->ffmpeg_ctx_
  );
  
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize FFmpeg: %s", esp_err_to_name(ret));
    return false;
  }
  
  this->frames_displayed_ = 0;
  this->last_frame_time_ = esphome::millis();
  this->playing_ = true;
  
  ret = esp_ffmpeg_start(this->ffmpeg_ctx_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start FFmpeg: %s", esp_err_to_name(ret));
    this->playing_ = false;
    return false;
  }
  
  return true;
}

bool MoviePlayer::play_http_stream(const std::string &url, VideoFormat format) {
  if (this->playing_) {
    ESP_LOGW(TAG, "Already playing a video, stopping current playback");
    this->stop();
  }

  // Détection du format basée sur l'URL
  if (format == VIDEO_FORMAT_AUTO) {
    if (url.find(".avi") != std::string::npos) {
      format = VIDEO_FORMAT_AVI;
      ESP_LOGI(TAG, "Auto-detected AVI format from URL");
    } else if (url.find(".mp4") != std::string::npos) {
      format = VIDEO_FORMAT_MP4;
      ESP_LOGI(TAG, "Auto-detected MP4 format from URL (limited support)");
    } else {
      format = VIDEO_FORMAT_MJPEG;
      ESP_LOGI(TAG, "Default to MJPEG format for HTTP stream");
    }
  }

  ESP_LOGI(TAG, "Starting playback from HTTP stream: '%s' (format: %d)", url.c_str(), format);
  this->current_path_ = url;
  this->current_format_ = format;
  this->current_source_type_ = ESP_FFMPEG_SOURCE_TYPE_HTTP;
  
  // Ajouter des tentatives de connexion pour les URLs HTTP
  int retry_count = 0;
  const int max_retries = 3;
  esp_err_t ret = ESP_FAIL;
  
  while (retry_count < max_retries) {
    ret = esp_ffmpeg_init(
      url.c_str(),
      ESP_FFMPEG_SOURCE_TYPE_HTTP,
      MoviePlayer::ffmpeg_frame_callback,
      this,
      &this->ffmpeg_ctx_
    );
    
    if (ret == ESP_OK) break;
    
    ESP_LOGW(TAG, "Failed to initialize FFmpeg, retrying (%d/%d): %s", 
             retry_count + 1, max_retries, esp_err_to_name(ret));
    retry_count++;
    delay(500);  // Petit délai entre les tentatives
  }
  
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize FFmpeg after %d attempts: %s", max_retries, esp_err_to_name(ret));
    return false;
  }
  
  this->frames_displayed_ = 0;
  this->last_frame_time_ = esphome::millis();
  this->playing_ = true;
  
  ret = esp_ffmpeg_start(this->ffmpeg_ctx_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start FFmpeg: %s", esp_err_to_name(ret));
    this->playing_ = false;
    return false;
  }
  
  return true;
}

void MoviePlayer::stop() {
  if (!this->playing_) return;

  ESP_LOGI(TAG, "Stopping video playback");
  
  if (this->ffmpeg_ctx_ != nullptr) {
    esp_ffmpeg_stop(this->ffmpeg_ctx_);
    this->ffmpeg_ctx_ = nullptr;
  }
  
  this->playing_ = false;
  ESP_LOGI(TAG, "Video playback stopped");
}

void MoviePlayer::ffmpeg_frame_callback(esp_ffmpeg_frame_t *frame, void *user_data) {
  MoviePlayer *player = static_cast<MoviePlayer *>(user_data);
  
  if (player && player->playing_ && frame && frame->data) {
    player->display_frame(frame->data, frame->width, frame->height);
    player->frames_displayed_++;
  }
}

bool MoviePlayer::display_frame(const uint8_t *data, int width, int height) {
  if (!this->display_ || !data) return false;
  
  if (xSemaphoreTake(this->mutex_, portMAX_DELAY) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to take mutex");
    return false;
  }
  
  const uint16_t *rgb_data = reinterpret_cast<const uint16_t *>(data);
  
  this->display_->fill(COLOR_OFF);  // corrigé
  
  // Calculer les facteurs d'échelle et la position pour centrer l'image
  float scale_x = 1.0, scale_y = 1.0;
  int pos_x = 0, pos_y = 0;
  
  // Option 1: Remplir l'écran (déformation possible)
  if (this->scaling_mode_ == SCALE_FILL) {
    scale_x = (float)this->display_->get_width() / width;
    scale_y = (float)this->display_->get_height() / height;
  } 
  // Option 2: Ajuster proportionnellement (préserver le ratio)
  else if (this->scaling_mode_ == SCALE_FIT) {
    float scale = std::min((float)this->display_->get_width() / width, 
                          (float)this->display_->get_height() / height);
    scale_x = scale_y = scale;
    
    // Centrer l'image
    pos_x = (this->display_->get_width() - width * scale_x) / 2;
    pos_y = (this->display_->get_height() - height * scale_y) / 2;
  }
  // Option 3: Pas de mise à l'échelle (1:1)
  else {
    // Centrer l'image
    pos_x = (this->display_->get_width() - width) / 2;
    pos_y = (this->display_->get_height() - height) / 2;
    
    // Limiter aux valeurs positives
    pos_x = std::max(0, pos_x);
    pos_y = std::max(0, pos_y);
  }
  
  // Optimisation pour certains types d'écrans monochromes
  int threshold = this->threshold_;  // Valeur de seuil (0-255)
  
  for (int y = 0; y < height; y++) {
    int target_y = pos_y + y * scale_y;
    if (target_y < 0 || target_y >= this->display_->get_height()) 
      continue;
      
    for (int x = 0; x < width; x++) {
      int target_x = pos_x + x * scale_x;
      if (target_x < 0 || target_x >= this->display_->get_width()) 
        continue;
      
      uint16_t pixel = rgb_data[y * width + x];
      
      // Extraction des composantes RGB
      uint8_t r = ((pixel >> 11) & 0x1F) << 3;  // Convertir 5 bits en 8 bits
      uint8_t g = ((pixel >> 5) & 0x3F) << 2;   // Convertir 6 bits en 8 bits
      uint8_t b = (pixel & 0x1F) << 3;          // Convertir 5 bits en 8 bits
      
      // Calcul de la luminance (formule standard)
      uint8_t luminance = (r * 3 + g * 6 + b) / 10;
      
      // Appliquer un seuil pour avoir des pixels noir/blanc
      esphome::Color color = (luminance > threshold) ? COLOR_ON : COLOR_OFF;
      
      this->display_->draw_pixel_at(target_x, target_y, color);
    }
  }
  
  this->display_->update();
  xSemaphoreGive(this->mutex_);
  
  return true;
}

// Défini un seuil personnalisé pour la conversion monochrome (0-255)
void MoviePlayer::set_threshold(uint8_t threshold) {
  this->threshold_ = threshold;
}

// Défini le mode de mise à l'échelle de la vidéo
void MoviePlayer::set_scaling_mode(ScalingMode mode) {
  this->scaling_mode_ = mode;
}

}  // namespace movie
}  // namespace esphome







