#pragma once

#include "esphome/core/component.h"
#include "esphome/components/display/display_buffer.h"
#include "video_camera.h"

namespace esphome {
namespace video_camera {

class DisplayHelper : public Component {
 public:
  void set_camera(VideoCamera *camera) { camera_ = camera; }
  void set_display(display::DisplayBuffer *display) { display_ = display; }
  
  void setup() override {
    if (camera_ == nullptr || display_ == nullptr) {
      ESP_LOGE("display_helper", "Camera or display not set");
      this->mark_failed();
      return;
    }
    
    // S'inscrire aux mises à jour de frame
    camera_->add_frame_callback([this](const RtspFrame &frame) {
      this->on_frame(frame);
    });
    
    ESP_LOGI("display_helper", "Display helper initialized");
  }
  
 protected:
  VideoCamera *camera_{nullptr};
  display::DisplayBuffer *display_{nullptr};
  
  void on_frame(const RtspFrame &frame) {
    if (frame.buffer == nullptr || frame.size == 0) {
      return;
    }
    
    // Ici, vous implémenteriez le code pour décoder l'image JPEG
    // et l'afficher sur l'écran.
    // Cela pourrait utiliser une bibliothèque comme TJpgDec ou une 
    // implémentation personnalisée.
    
    // Exemple simplifié (pseudocode):
    // 1. Décoder l'image JPEG
    // 2. Redimensionner à la taille de l'écran si nécessaire
    // 3. Convertir au format de couleur approprié
    // 4. Afficher sur l'écran
    
    // Note: L'implémentation réelle nécessite une bibliothèque de décodage JPEG
    
    ESP_LOGD("display_helper", "Received frame: %d bytes", frame.size);
    
    // Demander à l'écran de se rafraîchir (ceci n'affichera pas directement l'image)
    display_->update();
  }
};

}  // namespace video_camera
}  // namespace esphome
