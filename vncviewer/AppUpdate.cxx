/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include "AppUpdate.h"
#include <array>
#include <fstream>
#include <regex>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <wincrypt.h>
#endif

namespace dashboard {
namespace {
std::array<int, 3> versionParts(const std::string& version) {
  static const std::regex pattern("(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})");
  std::smatch match;
  if (!std::regex_match(version, match, pattern)) throw std::invalid_argument("Invalid application version");
  return {std::stoi(match[1]), std::stoi(match[2]), std::stoi(match[3])};
}
#ifdef _WIN32
std::wstring quote(const std::wstring& value) {
  std::wstring result = L"\""; size_t slashes = 0;
  for (wchar_t character : value) {
    if (character == L'\\') { ++slashes; continue; }
    result.append(slashes * (character == L'"' ? 2 : 1), L'\\'); slashes = 0;
    if (character == L'"') result += L'\\';
    result += character;
  }
  result.append(slashes * 2, L'\\'); return result + L'"';
}
std::wstring restartArguments() {
  int count = 0; auto* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments) throw std::runtime_error("Could not preserve the application startup options.");
  std::wstring joined;
  for (int i = 1; i < count; ++i) { if (i > 1) joined += L' '; joined += quote(arguments[i]); }
  LocalFree(arguments);
  if (joined.empty()) return {};
  DWORD size = 0, flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
  auto* bytes = reinterpret_cast<const BYTE*>(joined.data());
  DWORD length = static_cast<DWORD>(joined.size() * sizeof(wchar_t));
  if (!CryptBinaryToStringW(bytes, length, flags, nullptr, &size)) throw std::runtime_error("Could not encode startup options.");
  std::wstring encoded(size, L'\0');
  if (!CryptBinaryToStringW(bytes, length, flags, encoded.data(), &size)) throw std::runtime_error("Could not encode startup options.");
  encoded.resize(size); return encoded;
}
#endif
}
bool newerVersion(const std::string& candidate, const std::string& installed) {
  return versionParts(candidate) > versionParts(installed);
}
AppUpdate::AppUpdate(const std::filesystem::path& database, bool allowAutomatic,
                     const std::filesystem::path& applicationDirectory)
  : database_(std::filesystem::absolute(database)), work_(database_.parent_path() / ".supersmart-updates"),
    allowAutomatic_(allowAutomatic), nextCheck_(std::chrono::steady_clock::now() + std::chrono::seconds(3))
{
  std::ifstream settings(work_ / "automatic.txt"); char value;
  if (settings.get(value)) automatic_ = value != '0';
#ifdef _WIN32
  std::wstring path(32768, L'\0'); DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (size && size < path.size()) {
    path.resize(size); directory_ = applicationDirectory.empty() ? std::filesystem::path(path).parent_path() : applicationDirectory;
    supported_ = std::filesystem::is_regular_file(directory_ / "update.ps1") &&
                 std::filesystem::is_regular_file(directory_ / "manifest.json");
  }
#else
  (void)applicationDirectory;
#endif
  if (!supported_) {
    state_ = State::Unsupported;
    message_ = "Automatic updates are available in the Windows portable release. Open Releases to download the latest version.";
  } else {
    readStatus();
    std::error_code error;
    auto checked = std::filesystem::last_write_time(work_ / "last-check", error);
    if (!error) {
      auto age = decltype(checked)::clock::now() - checked;
      if (age >= decltype(age)::zero() && age < std::chrono::hours(24))
        nextCheck_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::hours(24) - age);
    }
  }
}
AppUpdate::~AppUpdate() {
#ifdef _WIN32
  if (process_) CloseHandle(process_);
#endif
}
bool AppUpdate::busy() const {
  return state_ == State::Checking || state_ == State::Downloading || state_ == State::Installing;
}
void AppUpdate::fail(const std::string& message) { state_ = State::Error; message_ = message; }
void AppUpdate::automatic(bool enabled) {
  std::filesystem::create_directories(work_);
  std::ofstream output(work_ / "automatic.txt", std::ios::trunc);
  output << (enabled ? '1' : '0'); output.close();
  if (!output) throw std::runtime_error("Could not save the update preference.");
  automatic_ = enabled;
  if (enabled) nextCheck_ = std::chrono::steady_clock::now();
}
void AppUpdate::readStatus() {
  std::ifstream input(work_ / "status.txt", std::ios::binary);
  if (!input) return;
  std::string status, version, message;
  std::getline(input, status); std::getline(input, version); std::getline(input, message);
  if (status.size() > 30 || version.size() > 20 || message.size() > 1000) return;
  if (status == "available" || status == "ready") {
    try { if (!newerVersion(version, SUPERSMART_VERSION)) status = "current"; }
    catch (...) { fail("The update response contains an invalid version."); return; }
  }
  if (status == "available") state_ = State::Available;
  else if (status == "ready") state_ = State::Ready;
  else if (status == "current" || status == "installed") state_ = State::Current;
  else if (status == "error") state_ = State::Error;
  else return;
  version_ = version; message_ = message;
  if (state_ == State::Current) message_ = "You are using the latest release.";
}
bool AppUpdate::launch(const wchar_t* action) {
#ifdef _WIN32
  if (!supported_ || process_) return false;
  try {
    std::filesystem::create_directories(work_);
    auto script = directory_ / "update.ps1";
    bool installing = std::wstring(action) == L"Install";
    if (installing) {
      script = work_ / "install.ps1";
      std::filesystem::copy_file(directory_ / "update.ps1", script, std::filesystem::copy_options::overwrite_existing);
    }
    wchar_t system[32768]; UINT length = GetSystemDirectoryW(system, 32768);
    if (!length || length >= 32768) throw std::runtime_error("Cannot find Windows PowerShell.");
    auto shell = std::filesystem::path(system) / "WindowsPowerShell/v1.0/powershell.exe";
    std::wstring command = quote(shell.wstring()) + L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + quote(script.wstring());
    command += L" -Action " + std::wstring(action) + L" -AppDirectory " + quote(directory_.wstring()) +
      L" -WorkDirectory " + quote(work_.wstring()) + L" -InstalledVersion " + quote(std::filesystem::path(SUPERSMART_VERSION).wstring());
    if (installing) {
      command += L" -ParentId " + std::to_wstring(GetCurrentProcessId()) + L" -ConfigPath " + quote(database_.wstring());
      auto arguments = restartArguments();
      if (!arguments.empty()) command += L" -RestartArguments " + quote(arguments);
    }
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, work_.c_str(), &startup, &info))
      throw std::runtime_error("Could not start the update helper. Open Releases to update manually.");
    CloseHandle(info.hThread); process_ = info.hProcess; return true;
  } catch (const std::exception& error) { fail(error.what()); }
#else
  (void)action;
#endif
  return false;
}
void AppUpdate::check() {
  if (busy()) return;
  nextCheck_ = std::chrono::steady_clock::now() + std::chrono::hours(24);
  if (launch(L"Check")) { state_ = State::Checking; message_ = "Checking GitHub for a new release..."; }
}
void AppUpdate::download() {
  if (state_ != State::Available) return;
  if (launch(L"Download")) { state_ = State::Downloading; message_ = "Downloading and verifying the update. Your panels stay connected."; }
}
bool AppUpdate::install() {
  if (state_ != State::Ready || !launch(L"Install")) return false;
  state_ = State::Installing; return true;
}
void AppUpdate::poll() {
#ifdef _WIN32
  if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
    DWORD code = 1; GetExitCodeProcess(process_, &code); CloseHandle(process_); process_ = nullptr;
    readStatus();
    if (busy() || (code && state_ != State::Error)) fail("The update helper could not finish. Please try again or open Releases.");
  }
#endif
  if (supported_ && automatic() && !busy() && std::chrono::steady_clock::now() >= nextCheck_) check();
}
}
