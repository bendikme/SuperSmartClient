/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SUPERSMART_APP_UPDATE_H
#define SUPERSMART_APP_UPDATE_H

#include <chrono>
#include <filesystem>
#include <string>

namespace dashboard {
bool newerVersion(const std::string& candidate, const std::string& installed);

class AppUpdate {
public:
  enum class State { Idle, Checking, Current, Available, Downloading, Ready, Installing, Error, Unsupported };
  explicit AppUpdate(const std::filesystem::path& database, bool allowAutomatic,
                     const std::filesystem::path& applicationDirectory = {});
  ~AppUpdate();
  AppUpdate(const AppUpdate&) = delete;
  AppUpdate& operator=(const AppUpdate&) = delete;
  State state() const { return state_; }
  const std::string& version() const { return version_; }
  const std::string& message() const { return message_; }
  bool supported() const { return supported_; }
  bool automatic() const { return automatic_ && allowAutomatic_; }
  bool busy() const;
  bool available() const { return state_ == State::Available || state_ == State::Ready; }
  void automatic(bool enabled);
  void poll();
  void check();
  void download();
  bool install();
private:
  bool launch(const wchar_t* action);
  void readStatus();
  void fail(const std::string& message);
  std::filesystem::path database_, directory_, work_;
  void* process_ = nullptr;
  bool automatic_ = true, allowAutomatic_, supported_ = false;
  State state_ = State::Idle;
  std::string version_, message_ = "Check for the latest release.";
  std::chrono::steady_clock::time_point nextCheck_;
};
}
#endif
