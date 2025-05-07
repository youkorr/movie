#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace video_camera {

// Structure pour stocker les informations d'une frame
struct RtspFrame {
  uint8_t *buffer;
  size_t size;
  int width;
  int height;
  bool is_jpeg;
};

// Callback lorsqu'une nouvelle frame est disponible
using FrameCallback = std::function<void(const RtspFrame &frame)>;

class VideoCamera : public Component {
 public:
  void set_url(const std::string &url) { url_ = url; }
  void set_fps(int fps) { fps_ = fps; }
  
  void setup() override;
  void loop() override;
  void dump_config() override;
  
  // Méthode pour enregistrer un callback lorsqu'une nouvelle frame est disponible
  void add_frame_callback(FrameCallback &&callback) { frame_callbacks_.push_back(std::move(callback)); }
  
  // Obtenir l'URL du flux
  const std::string &get_url() const { return url_; }
  
  // Obtenir la dernière frame
  const RtspFrame *get_last_frame() const { return &last_frame_; }
  
 protected:
  std::string url_;
  int fps_{1};
  unsigned long last_frame_time_{0};
  RtspFrame last_frame_{nullptr, 0, 0, 0, false};
  
  std::vector<FrameCallback> frame_callbacks_{};
  
  bool initialize_rtsp_client_();
  bool fetch_rtsp_frame_();
  void call_frame_callbacks_();
  
  // Task handle pour le thread de traitement RTSP
  TaskHandle_t rtsp_task_handle_{nullptr};
  bool rtsp_running_{false};
  
  // Méthode statique pour le thread de traitement RTSP
  static void rtsp_task(void *parameter);
};

}  // namespace video_camera
}  // namespace esphome
