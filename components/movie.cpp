#include "movie.h"
#include "esphome/core/log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "movie";

namespace esphome {
namespace movie {

// Initialiser la variable statique
MoviePlayer *MoviePlayer::active_instance_ = nullptr;

MoviePlayer::MoviePlayer() {
  this->mutex_ = xSemaphoreCreateMutex();
}

MoviePlayer::~MoviePlayer() {
  this->stop();
  
  if (this->mutex_ != nullptr) {
    vSemaphoreDelete(this->mutex_);
    this->mutex_ = nullptr;
  }
  
  // Libérer les buffers alloués
  if (this->frame_buffer_ != nullptr) {
    heap_caps_free(this->frame_buffer_);
    this->frame_buffer_ = nullptr;
  }
  
  if (this->rgb565_buffer_ != nullptr) {
    heap_caps_free(this->rgb565_buffer_);
    this->rgb565_buffer_ = nullptr;
  }
}

void MoviePlayer::setup() {
  // Trouver l'écran actif dans ESPHome
  for (auto *display : display::DisplayBuffer::displays) {
    if (display != nullptr) {
      this->display_ = display;
      ESP_LOGI(TAG, "Found display: %dx%d", display->get_width(), display->get_height());
      break;
    }
  }

  if (this->display_ == nullptr) {
    ESP_LOGE(TAG, "No display found!");
    this->mark_failed();
    return;
  }

  // Allouer le buffer pour les frames JPEG
  this->frame_buffer_ = (uint8_t *)heap_caps_malloc(this->buffer_size_, MALLOC_CAP_SPIRAM);
  if (this->frame_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate frame buffer memory");
    this->mark_failed();
    return;
  }

  // Allouer le buffer RGB565 pour l'affichage
  size_t rgb_buffer_size = this->width_ * this->height_ * sizeof(uint16_t);
  this->rgb565_buffer_ = (uint16_t *)heap_caps_malloc(rgb_buffer_size, MALLOC_CAP_SPIRAM);
  if (this->rgb565_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate RGB565 buffer memory");
    this->mark_failed();
    return;
  }

  // Initialiser le système de fichiers SPIFFS
  esp_vfs_spiffs_conf_t conf = {
    .base_path = "/spiffs",
    .partition_label = NULL,
    .max_files = 5,
    .format_if_mount_failed = false
  };
  
  esp_err_t ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount or format SPIFFS");
    } else if (ret == ESP_ERR_NOT_FOUND) {
      ESP_LOGE(TAG, "Failed to find SPIFFS partition");
    } else {
      ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "Trying to continue without SPIFFS...");
  } else {
    ESP_LOGI(TAG, "SPIFFS mounted successfully");
  }

  // Initialiser les structures de données
  this->frame_data_.width = 0;
  this->frame_data_.height = 0;
  this->frame_data_.buffer = this->rgb565_buffer_;
  
  // Définir cette instance comme active pour les callbacks
  MoviePlayer::active_instance_ = this;
  
  // Calculer la durée d'une frame en ms
  this->frame_duration_ms_ = 1000 / this->fps_;

  ESP_LOGI(TAG, "Movie player initialized with display size %dx%d", this->width_, this->height_);
}

void MoviePlayer::loop() {
  // Vérifier l'état de la lecture
  if (this->playing_) {
    if (this->decoder_task_handle_ == nullptr) {
      this->playing_ = false;
      ESP_LOGI(TAG, "Playback completed");
    }
  }
}

void MoviePlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "Movie Player:");
  ESP_LOGCONFIG(TAG, "  Display Size: %dx%d", this->width_, this->height_);
  ESP_LOGCONFIG(TAG, "  Buffer Size: %d bytes", this->buffer_size_);
  ESP_LOGCONFIG(TAG, "  Target FPS: %d", this->fps_);
}

bool MoviePlayer::play_file(const std::string &file_path, VideoFormat format) {
  if (this->playing_) {
    ESP_LOGW(TAG, "Already playing a video, stopping current playback");
    this->stop();
  }

  ESP_LOGI(TAG, "Starting playback of '%s'", file_path.c_str());
  this->current_file_ = file_path;
  this->current_format_ = format;
  
  // Ouvrir le fichier vidéo
  if (!this->open_file(file_path)) {
    ESP_LOGE(TAG, "Failed to open file '%s'", file_path.c_str());
    return false;
  }
  
  // Réinitialiser l'état de lecture
  this->stop_requested_ = false;
  this->playing_ = true;
  this->current_frame_ = 0;
  
  // Créer la tâche de décodage
  ESP_LOGI(TAG, "Creating decoder task");
  BaseType_t res = xTaskCreatePinnedToCore(
    MoviePlayer::decoder_task,
    "movie_decoder",
    8192,        // Stack size
    this,        // Task parameter
    tskIDLE_PRIORITY + 1,  // Priority
    &this->decoder_task_handle_,
    1           // Core ID (APP_CPU)
  );
  
  if (res != pdPASS) {
    ESP_LOGE(TAG, "Failed to create decoder task: %d", res);
    this->close_file();
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
  this->stop_requested_ = true;
  
  if (this->decoder_task_handle_ != nullptr) {
    // Attendre que la tâche se termine
    vTaskDelay(pdMS_TO_TICKS(100));
    if (eTaskGetState(this->decoder_task_handle_) != eDeleted) {
      ESP_LOGW(TAG, "Decoder task did not terminate, deleting forcefully");
      vTaskDelete(this->decoder_task_handle_);
    }
    this->decoder_task_handle_ = nullptr;
  }
  
  this->close_file();
  this->cleanup_decoder();
  this->playing_ = false;
}

bool MoviePlayer::open_file(const std::string &path) {
  this->video_file_ = fopen(path.c_str(), "rb");
  if (this->video_file_ == nullptr) {
    ESP_LOGE(TAG, "Failed to open file: %s", path.c_str());
    return false;
  }
  
  // Obtenir la taille du fichier
  fseek(this->video_file_, 0, SEEK_END);
  this->file_size_ = ftell(this->video_file_);
  fseek(this->video_file_, 0, SEEK_SET);
  
  ESP_LOGI(TAG, "File opened: %s, size: %d bytes", path.c_str(), this->file_size_);
  return true;
}

void MoviePlayer::close_file() {
  if (this->video_file_ != nullptr) {
    fclose(this->video_file_);
    this->video_file_ = nullptr;
  }
}

void MoviePlayer::decoder_task(void *arg) {
  MoviePlayer *player = static_cast<MoviePlayer *>(arg);
  
  ESP_LOGI(TAG, "Decoder task started");
  
  if (!player->init_decoder()) {
    ESP_LOGE(TAG, "Failed to initialize decoder");
    player->playing_ = false;
    player->close_file();
    vTaskDelete(nullptr);
    return;
  }
  
  int64_t last_frame_time = esp_timer_get_time();
  int64_t frame_period_us = player->frame_duration_ms_ * 1000;
  
  while (!player->stop_requested_) {
    int64_t decode_start = esp_timer_get_time();
    
    // Lire et décoder la frame
    if (!player->read_next_frame()) {
      ESP_LOGI(TAG, "End of video or read error");
      break;
    }
    
    // Calculer le temps de décodage
    player->decode_time_ms_ = (esp_timer_get_time() - decode_start) / 1000;
    
    // Afficher la frame
    int64_t render_start = esp_timer_get_time();
    if (!player->render_current_frame()) {
      ESP_LOGE(TAG, "Failed to render frame");
      break;
    }
    
    // Calculer le temps de rendu
    player->render_time_ms_ = (esp_timer_get_time() - render_start) / 1000;
    
    player->current_frame_++;
    
    if (player->current_frame_ % 30 == 0) {
      ESP_LOGI(TAG, "Frame %d, decode: %ld ms, render: %ld ms", 
              player->current_frame_, player->decode_time_ms_, player->render_time_ms_);
    }
    
    // Calculer le temps d'attente pour maintenir le FPS
    int64_t now = esp_timer_get_time();
    int64_t next_frame_time = last_frame_time + frame_period_us;
    
    if (now < next_frame_time) {
      int64_t delay_us = next_frame_time - now;
      if (delay_us > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_us / 1000));
      }
    }
    
    last_frame_time = esp_timer_get_time();
  }
  
  player->cleanup_decoder();
  player->playing_ = false;
  
  ESP_LOGI(TAG, "Decoder task finished after %d frames", player->current_frame_);
  player->decoder_task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

bool MoviePlayer::init_decoder() {
  ESP_LOGI(TAG, "Initializing decoder for format: %d", this->current_format_);
  
  switch (this->current_format_) {
    case VIDEO_FORMAT_MJPEG:
      return this->init_mjpeg();
    case VIDEO_FORMAT_MP4:
      ESP_LOGE(TAG, "MP4 format not yet implemented in ESP-IDF version");
      return false;
    default:
      ESP_LOGE(TAG, "Unsupported video format");
      return false;
  }
}

void MoviePlayer::cleanup_decoder() {
  // Rien de spécifique à nettoyer pour MJPEG pour l'instant
}

bool MoviePlayer::init_mjpeg() {
  if (this->video_file_ == nullptr) {
    ESP_LOGE(TAG, "No file open");
    return false;
  }
  
  // Vérifier que le fichier commence par un marqueur JPEG
  uint8_t header[2];
  size_t read = fread(header, 1, 2, this->video_file_);
  fseek(this->video_file_, 0, SEEK_SET);
  
  if (read != 2 || header[0] != 0xFF || header[1] != 0xD8) {
    ESP_LOGE(TAG, "File doesn't start with JPEG marker (0xFF 0xD8)");
    return false;
  }
  
  ESP_LOGI(TAG, "MJPEG decoder initialized");
  return true;
}

bool MoviePlayer::read_next_frame() {
  switch (this->current_format_) {
    case VIDEO_FORMAT_MJPEG:
      return this->decode_mjpeg_frame();
    case VIDEO_FORMAT_MP4:
      // Non implémenté
      return false;
    default:
      return false;
  }
}

size_t MoviePlayer::jpg_read_callback(void *arg, size_t index, uint8_t *buf, size_t len) {
  // Callback qui fournit les données JPEG au décodeur ESP-IDF
  jpg_data_t *jpg = (jpg_data_t *)arg;
  
  if (index + len > jpg->len) {
    // Ajuster la longueur si nous dépassons la fin des données
    len = jpg->len - index;
  }
  
  // Copier les données du buffer vers le décodeur
  memcpy(buf, &jpg->buf[index], len);
  return len;
}

bool MoviePlayer::jpg_decode_callback(void *arg, uint16_t *px_data, int pos_x, int pos_y, int width, int height) {
  // Callback qui reçoit les données RGB565 décodées
  frame_data_t *frame = (frame_data_t *)arg;
  
  // Vérifier que les dimensions sont correctes
  if (frame->width == 0) {
    frame->width = width;
    frame->height = height;
  }
  
  // Calculer la position dans le buffer
  uint16_t *dst = &frame->buffer[pos_y * frame->width + pos_x];
  
  // Copier les données RGB565 vers le buffer d'affichage
  for (int y = 0; y < height; y++) {
    memcpy(&dst[y * frame->width], &px_data[y * width], width * sizeof(uint16_t));
  }
  
  return true;
}

bool MoviePlayer::decode_mjpeg_frame() {
  if (this->video_file_ == nullptr || this->frame_buffer_ == nullptr) {
    return false;
  }
  
  // Recherche du marqueur de début JPEG
  uint8_t marker[2];
  bool found_start = false;
  while (!found_start && !feof(this->video_file_)) {
    size_t read = fread(marker, 1, 2, this->video_file_);
    if (read != 2) break;
    
    if (marker[0] == 0xFF && marker[1] == 0xD8) {
      // Trouvé un début de JPEG
      found_start = true;
      fseek(this->video_file_, -2, SEEK_CUR);  // Revenir au début du marqueur
    } else {
      fseek(this->video_file_, -1, SEEK_CUR);  // Reculer d'un octet et réessayer
    }
  }
  
  if (!found_start) {
    ESP_LOGI(TAG, "No more JPEG frames found");
    return false;
  }
  
  // Lire la frame JPEG dans le buffer
  size_t bytes_read = fread(this->frame_buffer_, 1, this->buffer_size_, this->video_file_);
  if (bytes_read == 0) {
    ESP_LOGE(TAG, "Failed to read JPEG data");
    return false;
  }
  
  // Configuration des données pour le décodage JPEG
  this->jpg_data_.buf = this->frame_buffer_;
  this->jpg_data_.len = bytes_read;
  this->jpg_data_.index = 0;
  
  // Réinitialiser les données de frame
  this->frame_data_.width = 0;
  this->frame_data_.height = 0;
  this->frame_data_.buffer = this->rgb565_buffer_;
  
  // Décoder le JPEG
  esp_err_t ret = esp_jpg_decode(
    bytes_read,
    JPG_SCALE_NONE,
    this->jpg_read_callback,
    this->jpg_decode_callback,
    &this->jpg_data_,
    &this->frame_data_
  );
  
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "JPEG decode failed: %d", ret);
    return false;
  }
  
  return true;
}

bool MoviePlayer::render_current_frame() {
  if (this->display_ == nullptr || this->rgb565_buffer_ == nullptr) {
    return false;
  }
  
  // Prendre le mutex pour protéger l'accès à l'affichage
  if (xSemaphoreTake(this->mutex_, portMAX_DELAY) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to take mutex");
    return false;
  }
  
  // Convertir RGB565 au format de l'écran et afficher
  this->display_rgb565_frame(this->rgb565_buffer_, this->frame_data_.width, this->frame_data_.height);
  
  // Libérer le mutex
  xSemaphoreGive(this->mutex_);
  
  return true;
}

void MoviePlayer::display_rgb565_frame(uint16_t *buffer, int width, int height) {
  // Cette méthode doit être adaptée au type d'affichage utilisé
  // Exemple pour un DisplayBuffer générique
  
  if (width <= 0 || height <= 0 || buffer == nullptr) {
    ESP_LOGE(TAG, "Invalid frame data");
    return;
  }
  
  // Dans la plupart des cas, nous devons convertir RGB565 au format de l'écran
  // et envoyer les données à l'écran
  
  // Pour cet exemple, nous allons convertir les pixels RGB565 en pixels binaires pour un affichage monochrome
  // Si vous utilisez un écran couleur, vous devrez adapter cette méthode
  
  this->display_->fill(display::COLOR_OFF);
  
  // Calculer les facteurs de mise à l'échelle si nécessaire
  float scale_x = (float)this->display_->get_width() / width;
  float scale_y = (float)this->display_->get_height() / height;
  
  for (int y = 0; y < height && y < this->display_->get_height(); y++) {
    for (int x = 0; x < width && x < this->display_->get_width(); x++) {
      // Position dans le buffer source
      uint16_t pixel = buffer[y * width + x];
      
      // Extraire les composants RGB
      uint8_t r = (pixel >> 11) & 0x1F;
      uint8_t g = (pixel >> 5) & 0x3F;
      uint8_t b = pixel & 0x1F;
      
      // Calculer la luminosité (simplifiée)
      uint8_t luminance = (r * 3 + g * 6 + b * 1) / 10;
      
      // Convertir en binaire avec seuil
      display::Color color = (luminance > 16) ? display::COLOR_ON : display::COLOR_OFF;
      
      // Position sur l'écran (avec mise à l'échelle si nécessaire)
      int display_x = x * scale_x;
      int display_y = y * scale_y;
      
      // Dessiner le pixel
      this->display_->draw_pixel_at(display_x, display_y, color);
    }
  }
  
  // Mettre à jour l'affichage
  this->display_->update();
}

}  // namespace movie
}  // namespace esphome
