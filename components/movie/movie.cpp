#include "movie.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"  // Ajouté pour millis()
#include "esp_system.h"

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
  // Problème: displays n'est pas un membre de DisplayBuffer
  // Solution: Nous utiliserons l'affichage qui a été défini via set_display()
  if (this->display_ != nullptr) {
    ESP_LOGI(TAG, "Using provided display: %dx%d", this->display_->get_width(), this->display_->get_height());
  } else {
    ESP_LOGE(TAG, "No display found!");
    this->mark_failed();
    return;
  }

  // Initialiser les valeurs par défaut
  this->frames_displayed_ = 0;
  this->last_frame_time_ = 0;
  this->avg_fps_ = 0;

  ESP_LOGI(TAG, "Movie player initialized with display size %dx%d", this->width_, this->height_);
}

void MoviePlayer::loop() {
  // Vérifier les FPS pour les logs
  if (this->playing_ && this->frames_displayed_ > 0) {
    // Correction: millis() -> esphome::millis()
    uint32_t current_time = esphome::millis();
    uint32_t elapsed = current_time - this->last_frame_time_;
    
    if (elapsed > 1000) {  // Toutes les secondes
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

  ESP_LOGI(TAG, "Starting playback of local file: '%s'", file_path.c_str());
  this->current_path_ = file_path;
  this->current_format_ = format;
  this->current_source_type_ = ESP_FFMPEG_SOURCE_TYPE_FILE;
  
  // Initialiser FFmpeg
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
  
  // Réinitialiser les compteurs
  this->frames_displayed_ = 0;
  // Correction: millis() -> esphome::millis()
  this->last_frame_time_ = esphome::millis();
  this->playing_ = true;
  
  // Démarrer la lecture
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

  ESP_LOGI(TAG, "Starting playback from HTTP stream: '%s'", url.c_str());
  this->current_path_ = url;
  this->current_format_ = format;
  this->current_source_type_ = ESP_FFMPEG_SOURCE_TYPE_HTTP;
  
  // Initialiser FFmpeg
  esp_err_t ret = esp_ffmpeg_init(
    url.c_str(),
    ESP_FFMPEG_SOURCE_TYPE_HTTP,
    MoviePlayer::ffmpeg_frame_callback,
    this,
    &this->ffmpeg_ctx_
  );
  
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize FFmpeg: %s", esp_err_to_name(ret));
    return false;
  }
  
  // Réinitialiser les compteurs
  this->frames_displayed_ = 0;
  // Correction: millis() -> esphome::millis()
  this->last_frame_time_ = esphome::millis();
  this->playing_ = true;
  
  // Démarrer la lecture
  ret = esp_ffmpeg_start(this->ffmpeg_ctx_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start FFmpeg: %s", esp_err_to_name(ret));
    this->playing_ = false;
    return false;
  }
  
  return true;
}

void MoviePlayer::stop() {
  if (!this->playing_) {
    return;
  }
  
  ESP_LOGI(TAG, "Stopping video playback");
  
  // Arrêter FFmpeg
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
    // Afficher la frame
    player->display_frame(frame->data, frame->width, frame->height);
    
    // Mettre à jour les statistiques
    player->frames_displayed_++;
  }
}

bool MoviePlayer::display_frame(const uint8_t *data, int width, int height) {
  if (!this->display_ || !data) {
    return false;
  }
  
  // Prendre le mutex pour protéger l'accès à l'affichage
  if (xSemaphoreTake(this->mutex_, portMAX_DELAY) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to take mutex");
    return false;
  }
  
  // Le buffer contient des données RGB565
  const uint16_t *rgb_data = reinterpret_cast<const uint16_t *>(data);
  
  // Effacer l'écran
  this->display_->fill(esphome::COLOR_OFF);  // Changé pour utiliser le namespace correct
  
  // Calculer les facteurs de mise à l'échelle
  float scale_x = (float)this->display_->get_width() / width;
  float scale_y = (float)this->display_->get_height() / height;
  
  // Convertir les données en pixels pour notre affichage
  for (int y = 0; y < height && y < this->display_->get_height(); y++) {
    for (int x = 0; x < width && x < this->display_->get_width(); x++) {
      uint16_t pixel = rgb_data[y * width + x];
      
      // Extraire les composantes RGB
      uint8_t r = (pixel >> 11) & 0x1F;
      uint8_t g = (pixel >> 5) & 0x3F;
      uint8_t b = pixel & 0x1F;
      
      // Calculer la luminosité (pour écrans monochromes)
      uint8_t luminance = (r * 3 + g * 6 + b) / 10;
      
      // Convertir en binaire avec seuil pour les écrans monochromes
      // Correction: display::Color -> esphome::Color
      // Correction: display::COLOR_ON -> esphome::COLOR_ON
      esphome::Color color = (luminance > 16) ? esphome::COLOR_ON : esphome::COLOR_OFF;
      
      // Position sur l'écran (avec mise à l'échelle)
      int display_x = x * scale_x;
      int display_y = y * scale_y;
      
      // Dessiner le pixel
      this->display_->draw_pixel_at(display_x, display_y, color);
    }
  }
  
  // Mettre à jour l'affichage
  this->display_->update();
  
  // Libérer le mutex
  xSemaphoreGive(this->mutex_);
  
  return true;
}

}  // namespace movie
}  // namespace esphome





