#pragma once

#include "esphome/core/component.h"
#include "esphome/components/display/display_buffer.h"
#include "esp_http_client.h"

namespace esphome {
namespace video_camera {

class VideoCamera : public Component {
 public:
  void set_display(display::DisplayBuffer *disp) { display_ = disp; }
  void set_url(const std::string &url) { url_ = url; }
  void set_update_interval(uint32_t interval) { update_interval_ = interval; }

  void setup() override;
  void loop() override;

 protected:
  display::DisplayBuffer *display_{nullptr};
  std::string url_;
  uint32_t update_interval_{1000};
  uint32_t last_update_{0};
  esp_http_client_handle_t http_client_{nullptr};

  bool fetch_jpeg(std::vector<uint8_t> &jpeg_data);
  void render_jpeg(const std::vector<uint8_t> &jpeg_data);
};

}  // namespace video_camera
}  // namespace esphome
