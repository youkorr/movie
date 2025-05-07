#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/image/image.h"

// Ajout de l'inclusion nécessaire pour TaskHandle_t
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace video_camera {

struct CameraFrame {
  uint8_t *buffer{nullptr};
  size_t size{0};
  bool is_jpeg{false};
  uint32_t width{0};
  uint32_t height{0};
};

using camera_frame_callback_t = std::function<void(const CameraFrame &)>;

class VideoCamera : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  
  void set_url(const std::string &url) { url_ = url; }
  const std::string &get_url() const { return url_; }
  
  void set_fps(int fps) { fps_ = fps; }
  int get_fps() const { return fps_; }
  
  void add_frame_callback(camera_frame_callback_t callback) {
    frame_callbacks_.push_back(std::move(callback));
  }
  
 protected:
  static void rtsp_task(void *parameter);
  void call_frame_callbacks_();
  
  std::string url_;
  int fps_{5};  // Fréquence d'images par défaut
  
  std::vector<camera_frame_callback_t> frame_callbacks_;
  CameraFrame last_frame_;
  
  bool rtsp_running_{false};
  TaskHandle_t rtsp_task_handle_{nullptr};  // Maintenant correctement déclaré avec l'inclusion de task.h
};

}  // namespace video_camera
}  // namespace esphome
