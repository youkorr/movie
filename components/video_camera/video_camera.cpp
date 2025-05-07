#include "video_camera.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_http_client.h"
//#include "esp_camera.h"
#include <algorithm>

#ifdef USE_ESP32
#include "esp_heap_caps.h"
#endif

namespace esphome {
namespace video_camera {

static const char *TAG = "video_camera";

// Structure pour passer les données à la tâche RTSP
struct RtspTaskData {
  VideoCamera *camera;
};

void VideoCamera::setup() {
  ESP_LOGCONFIG(TAG, "Setting up RTSP Video Camera...");
  
  // Initialiser le client RTSP dans un thread séparé
  RtspTaskData *task_data = new RtspTaskData{this};
  
  xTaskCreatePinnedToCore(
    VideoCamera::rtsp_task,
    "rtsp_task",
    8192,  // Stack size
    task_data,
    1,  // Priority
    &rtsp_task_handle_,
    0  // Core où exécuter la tâche (0)
  );
  
  rtsp_running_ = true;
  ESP_LOGI(TAG, "RTSP Video Camera setup complete");
}

void VideoCamera::loop() {
  // La tâche principale est gérée par le thread RTSP
  // Cette boucle n'a pas grand-chose à faire car le traitement se fait dans rtsp_task
  
  // Vérifier si le thread RTSP est toujours en vie
  if (rtsp_running_ && rtsp_task_handle_ == nullptr) {
    ESP_LOGE(TAG, "RTSP task died unexpectedly, restarting...");
    setup();
  }
}

void VideoCamera::dump_config() {
  ESP_LOGCONFIG(TAG, "RTSP Video Camera:");
  ESP_LOGCONFIG(TAG, "  URL: %s", url_.c_str());
  ESP_LOGCONFIG(TAG, "  FPS: %d", fps_);
}

// Tâche RTSP qui s'exécute en parallèle
void VideoCamera::rtsp_task(void *parameter) {
  RtspTaskData *data = static_cast<RtspTaskData *>(parameter);
  VideoCamera *camera = data->camera;
  
  ESP_LOGI(TAG, "RTSP task started");
  
  // Configuration du client HTTP pour le flux RTSP (version simplifiée)
  esp_http_client_config_t config = {};
  config.url = camera->get_url().c_str();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = 10000;
  
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    delete data;
    camera->rtsp_running_ = false;
    vTaskDelete(NULL);
    return;
  }
  
  // Boucle principale de récupération des images
  while (camera->rtsp_running_) {
    // Limiter la fréquence de récupération des images
    vTaskDelay(1000 / camera->fps_ / portTICK_PERIOD_MS);
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to open connection to server: %s (%d)", esp_err_to_name(err), err);
      vTaskDelay(5000 / portTICK_PERIOD_MS);  // Attendre avant de réessayer
      continue;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
      ESP_LOGW(TAG, "HTTP client fetch headers failed");
      esp_http_client_close(client);
      vTaskDelay(5000 / portTICK_PERIOD_MS);  // Attendre avant de réessayer
      continue;
    }
    
    // Allouer de la mémoire pour l'image
    uint8_t *buffer = nullptr;
    #ifdef USE_ESP32
    buffer = static_cast<uint8_t *>(heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM));
    #else
    buffer = static_cast<uint8_t *>(malloc(content_length));
    #endif
    
    if (buffer == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate memory for image buffer");
      esp_http_client_close(client);
      vTaskDelay(5000 / portTICK_PERIOD_MS);  // Attendre avant de réessayer
      continue;
    }
    
    // Lire les données
    int read_len = 0;
    int total_read = 0;
    
    while (total_read < content_length) {
      read_len = esp_http_client_read(client, reinterpret_cast<char *>(buffer + total_read), content_length - total_read);
      if (read_len <= 0) {
        ESP_LOGW(TAG, "Error reading data: %d", read_len);
        break;
      }
      total_read += read_len;
    }
    
    esp_http_client_close(client);
    
    if (total_read != content_length) {
      ESP_LOGW(TAG, "Incomplete read: %d/%d", total_read, content_length);
      free(buffer);
      continue;
    }
    
    // Libérer l'ancienne mémoire si elle existe
    if (camera->last_frame_.buffer != nullptr) {
      free(camera->last_frame_.buffer);
    }
    
    // Mettre à jour la frame
    camera->last_frame_.buffer = buffer;
    camera->last_frame_.size = total_read;
    camera->last_frame_.is_jpeg = true;  // Supposer que c'est du JPEG pour l'instant
    
    // Déterminer la taille de l'image (pour un JPEG)
    // Note: Ceci est une simplification et pourrait ne pas fonctionner pour tous les formats
    camera->last_frame_.width = 640;  // Valeur par défaut
    camera->last_frame_.height = 480;  // Valeur par défaut
    
    // Appeler les callbacks
    camera->call_frame_callbacks_();
  }
  
  // Nettoyer
  esp_http_client_cleanup(client);
  
  delete data;
  camera->rtsp_task_handle_ = nullptr;
  camera->rtsp_running_ = false;
  
  ESP_LOGI(TAG, "RTSP task ended");
  vTaskDelete(NULL);
}

void VideoCamera::call_frame_callbacks_() {
  for (auto &callback : frame_callbacks_) {
    callback(last_frame_);
  }
}

}  // namespace video_camera
}  // namespace esphome
