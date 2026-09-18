/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SUPERSMART_DASHBOARD_SESSION_H
#define SUPERSMART_DASHBOARD_SESSION_H

#include "DashboardModel.h"
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace dashboard {

class Session {
public:
  enum class State { Offline, Connecting, Authenticating, Live, Retrying, Attention };
  Session(Panel& panel, std::function<void()> changed);
  ~Session();
  void start();
  void stop();
  void tick();
  void setViewOnly(bool value);
  void pointer(int x, int y, unsigned buttons);
  void key(bool down, int code, unsigned keysym);
  void releaseInput();
  void refresh();
  // Application-wide: answer certificate trust prompts with "yes" unasked.
  static void acceptUnknownCertificates(bool value);

  State status() const { return status_; }
  bool live() const { return status_ == State::Live; }
  bool wanted() const { return wanted_; }
  const std::string& error() const { return error_; }
  std::string statusText() const;
  const std::vector<unsigned char>& pixels() const { return pixels_; }
  int width() const { return width_; }
  int height() const { return height_; }
  unsigned generation() const { return generation_; }

private:
  class Connection;
  struct Connector;
  void beginAttempt();
  void failed(const std::string& message, bool attention);
  void clearTransport();

  Panel& panel_;
  std::function<void()> changed_;
  std::shared_ptr<Connector> connector_;
  std::unique_ptr<Connection> connection_;
  State status_ = State::Offline;
  bool wanted_ = false, pumping_ = false;
  std::string error_;
  unsigned failures_ = 0, generation_ = 0, buttons_ = 0;
  int width_ = 0, height_ = 0, lastCountdown_ = -1;
  int pointerX_ = 0, pointerY_ = 0;
  std::vector<unsigned char> pixels_;
  std::chrono::steady_clock::time_point retryAt_, handshakeDeadline_;
};
}
#endif
