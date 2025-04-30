#pragma once

#include "esphome/core/component.h"
#include "esphome/components/display/display_buffer.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
// Remplacer l'inclusion de i2s déprécié
#include "driver/i2s_std.h"
#include "esp_spiffs.h"
// Résoudre l'erreur de esp_jpg_decode.h en utilisant les bibliothèques JPEG disponibles
#include "esp_jpeg/include/esp_jpg_decode.h" // Certains ESP-IDF
#include "libjpeg-turbo/include/jpeglib.h"   // Alternative si la première ne fonctionne pas
#include "esp_http_client.h"
#include <string>

// Inclure les en-têtes ESP-IDF spécifiques
#include "esp_vfs.h"
#include "esp_vfs_fat.h"

namespace esphome {
namespace movie {

// Structure pour stocker les informations sur une frame décodée
typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t *buffer;
} frame_data_t;

// Structure pour stocker les données de l'image JPEG en cours de décodage
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t index;
} jpg_data_t;

// Type de source vidéo
enum VideoSource {
  SOURCE_LOCAL_FILE,
  SOURCE_HTTP_STREAM,
};

// Formats vidéo pris en charge
enum VideoFormat {
  VIDEO_FORMAT_MJPEG,
  VIDEO_FORMAT_MP4,
};

// Fonction pour décoder un JPEG en RGB565
// Cette fonction remplace l'utilisation de esp_jpg_decode.h
bool decode_jpeg(uint8_t *jpeg_data, size_t jpeg_len, uint16_t *rgb565_buffer, int width, int height);

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
  void set_http_buffer_size(int buffer_size) { this->http_buffer_size_ = buffer_size; }
  
  // Méthodes publiques pour contrôler la lecture
  bool play_file(const std::string &file_path, VideoFormat format = VIDEO_FORMAT_MJPEG);
  bool play_http_stream(const std::string &url, VideoFormat format = VIDEO_FORMAT_MJPEG);
  void stop();
  bool is_playing() const { return this->playing_; }
  
 protected:
  // Méthodes pour la tâche de décodage
  static void decoder_task(void *arg);
  bool init_decoder();
  void cleanup_decoder();
  bool read_next_frame();
  bool render_current_frame();
  
  // Méthodes spécifiques au format MJPEG
  bool init_mjpeg();
  bool decode_mjpeg_frame();
  
  // Méthodes HTTP
  bool init_http_client(const std::string &url);
  void cleanup_http_client();
  bool fetch_http_data();
  
  // Notre propre implémentation de décodage JPEG
  bool decode_jpeg_frame(uint8_t *jpeg_data, size_t jpeg_len);
  
  // Callback pour HTTP
  static esp_err_t http_event_handler(esp_http_client_event_t *evt);
  
  // Méthodes utilitaires
  void display_rgb565_frame(uint16_t *buffer, int width, int height);
  bool open_file(const std::string &path);
  void close_file();
  
  // Affichage
  display::DisplayBuffer *display_{nullptr};
  int width_{320};
  int height_{240};
  int buffer_size_{8192};
  int fps_{15};
  
  // Configuration HTTP
  int http_timeout_ms_{5000};
  int http_buffer_size_{4096};
  
  // État de lecture
  bool playing_{false};
  bool stop_requested_{false};
  std::string current_path_;
  VideoFormat current_format_{VIDEO_FORMAT_MJPEG};
  VideoSource current_source_{SOURCE_LOCAL_FILE};
  
  // Gestion des tâches
  TaskHandle_t decoder_task_handle_{nullptr};
  SemaphoreHandle_t mutex_{nullptr};
  
  // Fichier et décodage
  FILE *video_file_{nullptr};
  size_t file_size_{0};
  
  // Client HTTP
  esp_http_client_handle_t http_client_{nullptr};
  bool http_data_ready_{false};
  size_t http_content_length_{0};
  size_t http_data_received_{0};
  
  // Buffers
  uint8_t *frame_buffer_{nullptr};
  uint8_t *http_buffer_{nullptr};
  uint16_t *rgb565_buffer_{nullptr};
  
  // Frame courante et timing
  int current_frame_{0};
  uint32_t frame_duration_ms_{0};
  
  // Données spécifiques au format MJPEG
  jpg_data_t jpg_data_;
  frame_data_t frame_data_;
  
  // Statistiques
  uint32_t decode_time_ms_{0};
  uint32_t render_time_ms_{0};
  uint32_t network_time_ms_{0};
};

}  // namespace movie
}  // namespace esphome

