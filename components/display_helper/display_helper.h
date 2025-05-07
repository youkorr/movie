#pragma once
#include "esphome/core/component.h"
#include "esphome/components/display/display_buffer.h"
#include "esphome/components/video_camera/video_camera.h"  // Chemin corrigé

namespace esphome {
namespace video_camera {

class DisplayHelper : public Component {
 public:
  void set_camera(VideoCamera *camera) { camera_ = camera; }
  void set_display(display::DisplayBuffer *display) { 
    display_ = display; 
    // Récupérer automatiquement les dimensions de l'écran
    display_width_ = display->get_width();
    display_height_ = display->get_height();
  }
  
  // Définir manuellement les dimensions d'écran si nécessaire
  void set_display_dimensions(uint16_t width, uint16_t height) {
    display_width_ = width;
    display_height_ = height;
  }
  
  void setup() override {
    if (camera_ == nullptr || display_ == nullptr) {
      ESP_LOGE("display_helper", "Camera or display not set");
      this->mark_failed();
      return;
    }
    
    // S'inscrire aux mises à jour de frame
    camera_->add_frame_callback([this](const CameraFrame &frame) {  // Corrigé: RtspFrame → CameraFrame
      this->on_frame(frame);
    });
    
    ESP_LOGI("display_helper", "Display helper initialized");
  }
  
 protected:
  VideoCamera *camera_{nullptr};
  display::DisplayBuffer *display_{nullptr};
  uint16_t display_width_{320};  // Valeur par défaut pour ESP32-S3-BOX3
  uint16_t display_height_{240}; // Valeur par défaut pour ESP32-S3-BOX3
  
  void on_frame(const CameraFrame &frame) {  // Corrigé: RtspFrame → CameraFrame
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
    
    ESP_LOGD("display_helper", "Received frame: %d bytes, screen size: %dx%d", 
             frame.size, display_width_, display_height_);
    
    // Calculer le ratio d'aspect pour ne pas déformer l'image
    float scale_x = static_cast<float>(display_width_) / frame.width;
    float scale_y = static_cast<float>(display_height_) / frame.height;
    float scale = std::min(scale_x, scale_y);
    
    int scaled_width = static_cast<int>(frame.width * scale);
    int scaled_height = static_cast<int>(frame.height * scale);
    
    // Calculer la position centrée sur l'écran
    int pos_x = (display_width_ - scaled_width) / 2;
    int pos_y = (display_height_ - scaled_height) / 2;
    
    ESP_LOGD("display_helper", "Displaying at position (%d,%d) with size %dx%d",
             pos_x, pos_y, scaled_width, scaled_height);
    
    // Demander à l'écran de se rafraîchir
    display_->update();
  }
};

}  // namespace video_camera
}  // namespace esphome

