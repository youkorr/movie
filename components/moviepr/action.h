#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "moviepr.h"

namespace esphome {
namespace movie {

// Déclarations des classes d'actions
class PlayAction : public Action<> {
 public:
  explicit PlayAction(MoviePlayer *player) : player_(player) {}
  
  void set_url(const std::string &url) { url_ = url; }
  
  void play(AsyncFunctionCall *call) override {
    if (!url_.empty()) {
      this->player_->set_url(url_);
    }
    this->player_->start_playback();
    call->over();
  }
  
 protected:
  MoviePlayer *player_;
  std::string url_{};
};

class StopAction : public Action<> {
 public:
  explicit StopAction(MoviePlayer *player) : player_(player) {}
  
  void play(AsyncFunctionCall *call) override {
    this->player_->stop_playback();
    call->over();
  }
  
 protected:
  MoviePlayer *player_;
};

class PauseAction : public Action<> {
 public:
  explicit PauseAction(MoviePlayer *player) : player_(player) {}
  
  void play(AsyncFunctionCall *call) override {
    this->player_->pause_playback();
    call->over();
  }
  
 protected:
  MoviePlayer *player_;
};

class ResumeAction : public Action<> {
 public:
  explicit ResumeAction(MoviePlayer *player) : player_(player) {}
  
  void play(AsyncFunctionCall *call) override {
    this->player_->resume_playback();
    call->over();
  }
  
 protected:
  MoviePlayer *player_;
};

class SetUrlAction : public Action<> {
 public:
  explicit SetUrlAction(MoviePlayer *player) : player_(player) {}
  
  void set_url(const std::string &url) { url_ = url; }
  
  void play(AsyncFunctionCall *call) override {
    this->player_->set_url(url_);
    call->over();
  }
  
 protected:
  MoviePlayer *player_;
  std::string url_{};
};

}  // namespace movie
}  // namespace esphome
