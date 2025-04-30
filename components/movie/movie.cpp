#include "movie.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"  // Pour accéder à la liste des écrans
#include "esp_system.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "movie";

namespace esphome {
namespace movie {

// Implémentation simplifiée du décodeur JPEG
bool decode_jpeg(uint8_t *jpeg_data, size_t jpeg_len, uint16_t *rgb565_buffer, int width, int height) {
    // Vérifier que nous avons bien un JPEG (signature FF D8)
    if (jpeg_len < 2 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        ESP_LOGE(TAG, "Not a valid JPEG file (missing SOI marker)");
        return false;
    }
    
    // Implémentation simplifiée pour exemple
    // Remplir avec des motifs basés sur les données JPEG
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int offset = ((y * width + x) * 3) % (jpeg_len - 10);
            if (offset < 0) offset = 0;
            
            uint8_t r = jpeg_data[offset % jpeg_len];
            uint8_t g = jpeg_data[(offset + 1) % jpeg_len];
            uint8_t b = jpeg_data[(offset + 2) % jpeg_len];
            
            // Réduire à 5/6/5 bits
            r = r >> 3;
            g = g >> 2;
            b = b >> 3;
            
            // Assembler en RGB565
            rgb565_buffer[y * width + x] = (r << 11) | (g << 5) | b;
        }
    }
    
    return true;
}

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
  
  if (this->http_buffer_ != nullptr) {
    heap_caps_free(this->http_buffer_);
    this->http_buffer_ = nullptr;
  }
  
  if (this->rgb565_buffer_ != nullptr) {
    heap_caps_free(this->rgb565_buffer_);
    this->rgb565_buffer_ = nullptr;
  }
}

void MoviePlayer::setup() {
  // Trouver l'écran actif dans ESPHome
  // Correction: utilisation de App->get_displays()
  for (auto *display : App.get_displays()) {
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

  // Allouer le buffer HTTP
  this->http_buffer_ = (uint8_t *)heap_caps_malloc(this->http_buffer_size_, MALLOC_CAP_SPIRAM);
  if (this->http_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate HTTP buffer memory");
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
  this->frame_data_.width = this->width_;
  this->frame_data_.height = this->height_;
  this->frame_data_.buffer = this->rgb565_buffer_;
  
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
  ESP_LOGCONFIG(TAG, "  HTTP Timeout: %d ms", this->http_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  HTTP Buffer Size: %d bytes", this->http_buffer_size_);
}

bool MoviePlayer::play_file(const std::string &file_path, VideoFormat format) {
  if (this->playing_) {
    ESP_LOGW(TAG, "Already playing a video, stopping current playback");
    this->stop();
  }

  ESP_LOGI(TAG, "Starting playback of local file: '%s'", file_path.c_str());
  this->current_path_ = file_path;
  this->current_format_ = format;
  this->current_source_ = SOURCE_LOCAL_FILE;
  
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

bool MoviePlayer::play_http_stream(const std::string &url, VideoFormat format) {
  if (this->playing_) {
    ESP_LOGW(TAG, "Already playing a video, stopping current playback");
    this->stop();
  }

  ESP_LOGI(TAG, "Starting playback from HTTP stream: '%s'", url.c_str());
  this->current_path_ = url;
  this->current_format_ = format;
  this->current_source_ = SOURCE_HTTP_STREAM;
  
  // Réinitialiser l'état de lecture
  this->stop_requested_ = false;
  this->playing_ = true;
  this->current_frame_ = 0;
  this->http_data_ready_ = false;
  this->http_content_length_ = 0;
  this->http_data_received_ = 0;
  
  // Créer la tâche de décodage
  ESP_LOGI(TAG, "Creating decoder task for HTTP stream");
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
  
  if (this->current_source_ == SOURCE_LOCAL_FILE) {
    this->close_file();
  } else if (this->current_source_ == SOURCE_HTTP_STREAM) {
    this->cleanup_http_client();
  }
  
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

bool MoviePlayer::init_http_client(const std::string &url) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.event_handler = MoviePlayer::http_event_handler;
  config.user_data = this;
  config.timeout_ms = this->http_timeout_ms_;
  
  this->http_client_ = esp_http_client_init(&config);
  if (this->http_client_ == nullptr) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    return false;
  }
  
  ESP_LOGI(TAG, "HTTP client initialized for URL: %s", url.c_str());
  return true;
}

void MoviePlayer::cleanup_http_client() {
  if (this->http_client_ != nullptr) {
    esp_http_client_cleanup(this->http_client_);
    this->http_client_ = nullptr;
  }
}

esp_err_t MoviePlayer::http_event_handler(esp_http_client_event_t *evt) {
  MoviePlayer *player = static_cast<MoviePlayer *>(evt->user_data);
  
  switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
      ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
      break;
    case HTTP_EVENT_ON_CONNECTED:
      ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
      break;
    case HTTP_EVENT_HEADER_SENT:
      ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
      break;
    case HTTP_EVENT_ON_HEADER:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
      if (strcasecmp(evt->header_key, "Content-Length") == 0) {
        player->http_content_length_ = atoi(evt->header_value);
        ESP_LOGI(TAG, "Content length: %d", player->http_content_length_);
      }
      break;
    case HTTP_EVENT_ON_DATA:
      // Si nous recevons des données, les copier dans notre buffer
      if (evt->data_len > 0) {
        // Vérifier que nous avons assez d'espace
        if (player->http_data_received_ + evt->data_len <= player->http_buffer_size_) {
          memcpy(player->http_buffer_ + player->http_data_received_, evt->data, evt->data_len);
          player->http_data_received_ += evt->data_len;
          player->http_data_ready_ = true;
        } else {
          ESP_LOGE(TAG, "HTTP buffer overflow, received: %d, new: %d, max: %d", 
                   player->http_data_received_, evt->data_len, player->http_buffer_size_);
        }
      }
      break;
    case HTTP_EVENT_ON_FINISH:
      ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH, data received: %d", player->http_data_received_);
      player->http_data_ready_ = true;
      break;
    case HTTP_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
      break;
  }
  
  return ESP_OK;
}

bool MoviePlayer::fetch_http_data() {
  if (this->http_client_ == nullptr) {
    ESP_LOGE(TAG, "HTTP client not initialized");
    return false;
  }
  
  // Réinitialiser les indicateurs de données
  this->http_data_ready_ = false;
  this->http_data_received_ = 0;
  
  // Démarrer la requête HTTP
  esp_err_t err = esp_http_client_perform(this->http_client_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    return false;
  }
  
  // Vérifier le code de statut
  int status_code = esp_http_client_get_status_code(this->http_client_);
  if (status_code != 200) {
    ESP_LOGE(TAG, "HTTP GET request returned status code %d", status_code);
    return false;
  }
  
  ESP_LOGI(TAG, "HTTP GET request successful, received %d bytes", this->http_data_received_);
  return (this->http_data_received_ > 0);
}

void MoviePlayer::decoder_task(void *arg) {
  MoviePlayer *player = static_cast<MoviePlayer *>(arg);
  
  ESP_LOGI(TAG, "Decoder task started");
  
  if (!player->init_decoder()) {
    ESP_LOGE(TAG, "Failed to initialize decoder");
    player->playing_ = false;
    if (player->current_source_ == SOURCE_LOCAL_FILE) {
      player->close_file();
    } else if (player->current_source_ == SOURCE_HTTP_STREAM) {
      player->cleanup_http_client();
    }
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
      ESP_LOGI(TAG, "Frame %d, decode: %ld ms, render: %ld ms, network: %ld ms", 
              player->current_frame_, player->decode_time_ms_, player->render_time_ms_, 
              player->network_time_ms_);
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
  ESP_LOGI(TAG, "Initializing decoder for format: %d, source: %d", 
           this->current_format_, this->current_source_);
  
  if (this->current_source_ == SOURCE_HTTP_STREAM) {
    // Initialiser le client HTTP
    if (!this->init_http_client(this->current_path_)) {
      return false;
    }
  }
  
  switch (this->current_format_) {
    case VIDEO_FORMAT_MJPEG:
      return this->init_mjpeg();
    case VIDEO_FORMAT_MP4:
      ESP_LOGE(TAG, "MP4 format not yet implemented");
      return false;
    default:
      ESP_LOGE(TAG, "Unsupported video format");
      return false;
  }
}

void MoviePlayer::cleanup_decoder() {
  if (this->current_source_ == SOURCE_HTTP_STREAM) {
    this->cleanup_http_client();
  }
}

bool MoviePlayer::init_mjpeg() {
  if (this->current_source_ == SOURCE_LOCAL_FILE) {
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
  } else if (this->current_source_ == SOURCE_HTTP_STREAM) {
    // Pour un flux HTTP, nous vérifierons lors de la réception des données
  }
  
  ESP_LOGI(TAG, "MJPEG decoder initialized");
  return true;
}

bool MoviePlayer::read_next_frame() {
  if (this->current_source_ == SOURCE_LOCAL_FILE) {
    switch (this->current_format_) {
      case VIDEO_FORMAT_MJPEG:
        return this->decode_mjpeg_frame(this->frame_buffer_, this->buffer_size_);
      default:
        return false;
    }
  } else if (this->current_source_ == SOURCE_HTTP_STREAM) {
    // Pour le mode HTTP, nous récupérons d'abord les données
    int64_t network_start = esp_timer_get_time();
    
    if (!this->fetch_http_data()) {
      ESP_LOGE(TAG, "Failed to fetch HTTP data");
      return false;
    }
    
    this->network_time_ms_ = (esp_timer_get_time() - network_start) / 1000;
    
    // Vérifier l'en-tête JPEG
    if (this->http_data_received_ < 2 || this->http_buffer_[0] != 0xFF || this->http_buffer_[1] != 0xD8) {
      ESP_LOGE(TAG, "HTTP data doesn't start with JPEG marker (0xFF 0xD8)");
      return false;
    }
    
    // Décoder les données
    return this->decode_jpeg_frame(this->http_buffer_, this->http_data_received_);
  }
  
  return false;
}

bool MoviePlayer::decode_jpeg_frame(uint8_t *jpeg_data, size_t jpeg_len) {
  if (!jpeg_data || jpeg_len == 0 || !this->rgb565_buffer_) {
    return false;
  }

  // Utiliser notre fonction de décodage JPEG
  return decode_jpeg(jpeg_data, jpeg_len, this->rgb565_buffer_, this->width_, this->height_);
}

// Suppression de la méthode decode_jpeg_frame() sans paramètres qui causait l'erreur

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
  
  this->display_->fill(COLOR_OFF);  // Correction: enlever le préfixe display::
  
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
      Color color = (luminance > 16) ? COLOR_ON : COLOR_OFF;  // Correction: enlever le préfixe display::
      
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


