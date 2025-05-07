#include "video_camera.h"
#include "esp_log.h"
#include "esp_jpg_decode.h"
#include "esp_http_client.h"

namespace esphome {
namespace video_camera {

static const char *TAG = "video_camera";

void VideoCamera::setup() {
  esp_http_client_config_t config = {
    .url = this->url_.c_str(),
    .method = HTTP_METHOD_GET,
    .timeout_ms = 5000,
  };
  this->http_client_ = esp_http_client_init(&config);
  if (esp_http_client_open(this->http_client_, 0) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open HTTP connection");
  } else {
    ESP_LOGI(TAG, "Connected to MJPEG stream");
  }
}

void VideoCamera::loop() {
  uint32_t now = millis();
  if (now - last_update_ < update_interval_) return;
  last_update_ = now;

  std::vector<uint8_t> jpeg_data;
  if (!fetch_jpeg(jpeg_data)) {
    ESP_LOGW(TAG, "Failed to fetch JPEG frame");
    return;
  }

  render_jpeg(jpeg_data);
}

bool VideoCamera::fetch_jpeg(std::vector<uint8_t> &jpeg_data) {
  static constexpr uint8_t SOI[] = {0xFF, 0xD8};
  static constexpr uint8_t EOI[] = {0xFF, 0xD9};
  uint8_t buffer[1024];
  jpeg_data.clear();
  bool found_soi = false;

  while (true) {
    int len = esp_http_client_read(this->http_client_, (char *)buffer, sizeof(buffer));
    if (len <= 0) break;

    for (int i = 0; i < len - 1; ++i) {
      if (!found_soi && buffer[i] == SOI[0] && buffer[i + 1] == SOI[1]) {
        found_soi = true;
        jpeg_data.insert(jpeg_data.end(), buffer + i, buffer + len);
        break;
      }
      if (found_soi) {
        jpeg_data.insert(jpeg_data.end(), buffer, buffer + len);
        for (int j = 0; j < len - 1; ++j) {
          if (buffer[j] == EOI[0] && buffer[j + 1] == EOI[1]) {
            return true;
          }
        }
        break;
      }
    }
  }
  return false;
}

void VideoCamera::render_jpeg(const std::vector<uint8_t> &jpeg_data) {
  if (!display_) return;
  int w = display_->get_width(), h = display_->get_height();
  size_t rgb_size = w * h * 2;
  auto *rgb_buf = static_cast<uint8_t *>(heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (rgb_buf && jpg2rgb565(jpeg_data.data(), jpeg_data.size(), rgb_buf, JPG_SCALE_AUTO)) {
    int idx = 0;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        uint16_t pixel = (rgb_buf[idx + 1] << 8) | rgb_buf[idx];
        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
        uint8_t b = (pixel & 0x1F) << 3;
        display_->draw_pixel_at(x, y, display::Color(r, g, b));
        idx += 2;
      }
    }
    display_->update();
  } else {
    ESP_LOGE(TAG, "Failed to decode JPEG");
  }
  free(rgb_buf);
}

}  // namespace video_camera
}  // namespace esphome
