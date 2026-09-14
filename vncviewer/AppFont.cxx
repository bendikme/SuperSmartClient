/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include <filesystem>
#include <vector>
#include <core/LogWriter.h>
#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_ask.H>
#ifdef WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <CoreText/CoreText.h>
#else
#include <unistd.h>
#include <fontconfig/fontconfig.h>
#endif

void initAppFont()
{
  static core::LogWriter log("AppFont");
  std::filesystem::path executable;
#ifdef WIN32
  std::vector<wchar_t> path(32768);
  DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (count && count < path.size()) executable = std::filesystem::path(path.data());
#elif defined(__APPLE__)
  uint32_t size = 0; _NSGetExecutablePath(nullptr, &size);
  std::vector<char> path(size);
  if (_NSGetExecutablePath(path.data(), &size) == 0) executable = path.data();
#else
  std::vector<char> path(4096);
  ssize_t count = readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (count > 0) { path[count] = '\0'; executable = path.data(); }
#endif
  auto directory = executable.parent_path() / "fonts";
  if (!std::filesystem::exists(directory / "Roboto-Regular.ttf"))
    directory = std::filesystem::path(CMAKE_INSTALL_FULL_DATADIR) / "supersmartclient/fonts";
  int loaded = 0;
  for (const char* name : {"Roboto-Regular.ttf", "Roboto-Bold.ttf", "Roboto-Italic.ttf", "Roboto-BoldItalic.ttf"}) {
    auto font = directory / name;
#ifdef WIN32
    // FR_PRIVATE affects this process only; Windows releases these at exit.
    if (AddFontResourceExW(font.c_str(), FR_PRIVATE, nullptr)) ++loaded;
#elif defined(__APPLE__)
    auto string = font.u8string();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr,
      reinterpret_cast<const UInt8*>(string.data()), string.size(), false);
    if (url) {
      if (CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, nullptr)) ++loaded;
      CFRelease(url);
    }
#else
    if (FcConfigAppFontAddFile(FcConfigGetCurrent(), reinterpret_cast<const FcChar8*>(font.c_str()))) ++loaded;
#endif
  }
  if (loaded != 4) log.error("Some bundled Roboto fonts could not be loaded from %s", directory.u8string().c_str());
  else log.info("Loaded bundled Roboto regular, bold, italic and bold italic");
#ifdef __APPLE__
  const char* names[] = {"Roboto-Regular", "Roboto-Bold", "Roboto-Italic", "Roboto-BoldItalic"};
#else
  const char* names[] = {" Roboto", "BRoboto", "IRoboto", "PRoboto"};
#endif
  // Set the standard text slots, including legacy dialogs. Keep symbol fonts
  // available for toolkit glyphs. The remote framebuffer is unaffected.
  for (int font = FL_HELVETICA; font <= FL_TIMES_BOLD_ITALIC; ++font)
    Fl::set_font(static_cast<Fl_Font>(font), names[font % 4]);
  Fl::set_font(FL_SCREEN, names[0]); Fl::set_font(FL_SCREEN_BOLD, names[1]);
  Fl_Tooltip::font(FL_HELVETICA);
  fl_message_font(FL_HELVETICA, FL_NORMAL_SIZE);
}
