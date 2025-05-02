#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "movie.h"

namespace esphome {
namespace movie {

class PlayFileAction : public Action<> {
 public:
  explicit PlayFileAction(MoviePlayer *player) : player_(player) {}

  void set_file_path(const std::string &file_path) { this->file_path_ = file_path; }
  void set_format(VideoFormat format) { this->format_ = format; }

  void play(bool wait_for_completion = false) {
    this->player_->play_file(this->file_path_, this->format_);
    // Option pour attendre la fin de la lecture si nécessaire
    if (wait_for_completion) {
      // Implémentez la logique d'attente si nécessaire
    }
  }

  void set_player(MoviePlayer *player) { this->player_ = player; }

  void play() override { this->play(false); }

 protected:
  MoviePlayer *player_;
  std::string file_path_;
  VideoFormat format_{FORMAT_MJPEG};
};

class PlayHttpStreamAction : public Action<> {
 public:
  explicit PlayHttpStreamAction(MoviePlayer *player) : player_(player) {}

  void set_url(const std::string &url) { this->url_ = url; }
  void set_format(VideoFormat format) { this->format_ = format; }

  void play(bool wait_for_completion = false) {
    this->player_->play_http_stream(this->url_, this->format_);
    // Option pour attendre la fin de la lecture si nécessaire
    if (wait_for_completion) {
      // Implémentez la logique d'attente si nécessaire
    }
  }

  void set_player(MoviePlayer *player) { this->player_ = player; }

  void play() override { this->play(false); }

 protected:
  MoviePlayer *player_;
  std::string url_;
  VideoFormat format_{FORMAT_MJPEG};
};

class StopAction : public Action<> {
 public:
  explicit StopAction(MoviePlayer *player) : player_(player) {}

  void set_player(MoviePlayer *player) { this->player_ = player; }

  void play() override { this->player_->stop(); }

 protected:
  MoviePlayer *player_;
};

}  // namespace movie
}  // namespace esphome



