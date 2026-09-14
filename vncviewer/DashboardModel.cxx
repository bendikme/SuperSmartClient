/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "DashboardModel.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

#include <network/TcpSocket.h>
#ifdef WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace dashboard {

std::string hexEncode(const std::string& value)
{
  const char* digits = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (unsigned char byte : value) {
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}

std::string hexDecode(const std::string& value)
{
  auto digit = [](char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    throw std::runtime_error("Invalid encoded value in dashboard file");
  };
  if (value.size() % 2)
    throw std::runtime_error("Truncated value in dashboard file");
  std::string result;
  for (size_t i = 0; i < value.size(); i += 2)
    result += static_cast<char>((digit(value[i]) << 4) | digit(value[i + 1]));
  return result;
}

std::string newId()
{
  std::random_device random;
  std::string bytes;
  for (int i = 0; i < 16; ++i)
    bytes += static_cast<char>(random());
  return hexEncode(bytes);
}

unsigned reconnectDelay(unsigned failures)
{
  return std::min(30u, 1u << std::min(failures ? failures - 1 : 0, 5u));
}

std::vector<Placement> layout(const std::vector<Panel>& panels, int columns)
{
  columns = std::clamp(columns, 1, 4);
  std::vector<std::vector<bool>> occupied;
  std::vector<Placement> result;
  for (const Panel& panel : panels) {
    int width = std::clamp(panel.columns, 1, columns);
    int height = std::clamp(panel.rows, 1, 4);
    bool placed = false;
    for (int row = 0; !placed; ++row) {
      while (occupied.size() < static_cast<size_t>(row + height))
        occupied.emplace_back(columns, false);
      for (int col = 0; col <= columns - width; ++col) {
        bool available = true;
        for (int y = row; y < row + height; ++y)
          for (int x = col; x < col + width; ++x)
            available = available && !occupied[y][x];
        if (!available) continue;
        for (int y = row; y < row + height; ++y)
          for (int x = col; x < col + width; ++x)
            occupied[y][x] = true;
        result.push_back({col, row, width, height});
        placed = true;
        break;
      }
    }
  }
  return result;
}

void reorder(std::vector<Panel>& panels, size_t from, size_t to)
{
  if (from >= panels.size() || to >= panels.size())
    throw std::out_of_range("Invalid panel order");
  Panel panel = std::move(panels[from]);
  panels.erase(panels.begin() + from);
  panels.insert(panels.begin() + to, std::move(panel));
}

void applyPreset(Workspace& workspace, Preset preset, bool resetDividers)
{
  workspace.preset = preset;
  if (resetDividers) { workspace.columnWeights.clear(); workspace.rowWeights.clear(); }
  if (preset == Preset::Free || preset == Preset::CustomGrid) return;
  workspace.columns = preset == Preset::Single || preset == Preset::Stacked ? 1 :
                      preset == Preset::Grid6 || preset == Preset::Grid9 ? 3 : 2;
  workspace.rowHeight = 0;
  for (size_t index = 0; index < workspace.panels.size(); ++index) {
    auto& panel = workspace.panels[index]; panel.columns = 1; panel.rows = 1;
    if (preset == Preset::TopAndTwo && index % 3 == 0) panel.columns = 2;
    if (preset == Preset::LeftAndTwo && index % 3 == 0) panel.rows = 2;
  }
}

namespace {
std::vector<int> distribute(int available, int count, const std::vector<int>& saved, int minimum) {
  std::vector<int> weights = saved.size() == static_cast<size_t>(count) ? saved : std::vector<int>(count, 1);
  double total = 0; for (auto& weight : weights) { weight = std::max(1, weight); total += weight; }
  std::vector<int> sizes(count); double cumulative = 0; int used = 0;
  for (int i = 0; i < count; ++i) {
    cumulative += weights[i]; int end = static_cast<int>(std::round(available * cumulative / total));
    sizes[i] = end - used; used = end;
  }
  for (auto& size : sizes) while (size < minimum) {
    auto donor = std::max_element(sizes.begin(), sizes.end());
    int amount = std::min(minimum - size, *donor - minimum);
    if (amount <= 0) break;
    *donor -= amount; size += amount;
  }
  return sizes;
}
std::vector<int> readWeights(const std::string& text) {
  std::vector<int> result; std::istringstream input(text); std::string part;
  while (std::getline(input, part, ',')) {
    if (result.size() >= 128) throw std::runtime_error("Too many saved divider sizes");
    size_t used; int value = std::stoi(part, &used);
    if (used != part.size() || value < 1 || value > 1000000) throw std::runtime_error("Invalid saved divider size");
    result.push_back(value);
  }
  return result;
}
std::string writeWeights(const std::vector<int>& weights) {
  std::string result;
  for (int weight : weights) { if (!result.empty()) result += ','; result += std::to_string(weight); }
  return result;
}
}

GridGeometry gridGeometry(const Workspace& workspace, int width, int height, int gap, int minimumWidth, int minimumHeight)
{
  GridGeometry result;
  int columns = std::clamp(workspace.columns, 1, 4), rows = 1;
  if (workspace.preset == Preset::Grid4 || workspace.preset == Preset::Grid6) rows = 2;
  else if (workspace.preset == Preset::Grid9) rows = 3;
  auto placements = layout(workspace.panels, columns);
  for (const auto& p : placements) rows = std::max(rows, p.row + p.rows);
  // A lone panel uses the whole workspace. More panels follow the selected
  // arrangement without depending on their framebuffer or connection state.
  if (placements.size() == 1) { columns = rows = 1; placements[0] = {0, 0, 1, 1}; }
  result.width = std::max(width, columns * minimumWidth + (columns - 1) * gap);
  result.height = std::max(height, rows * minimumHeight + (rows - 1) * gap);
  result.columnSizes = distribute(result.width - (columns - 1) * gap, columns, workspace.columnWeights, minimumWidth);
  result.rowSizes = distribute(result.height - (rows - 1) * gap, rows, workspace.rowWeights, minimumHeight);
  std::vector<int> xs(columns + 1, 0), ys(rows + 1, 0);
  for (int i = 0; i < columns; ++i) xs[i + 1] = xs[i] + result.columnSizes[i] + gap;
  for (int i = 0; i < rows; ++i) ys[i + 1] = ys[i] + result.rowSizes[i] + gap;
  std::vector<std::vector<int>> owners(rows, std::vector<int>(columns, -1));
  for (size_t i = 0; i < placements.size(); ++i) {
    const auto& p = placements[i];
    result.panels.push_back({xs[p.column], ys[p.row], xs[p.column + p.columns] - xs[p.column] - gap,
                            ys[p.row + p.rows] - ys[p.row] - gap});
    for (int y = p.row; y < p.row + p.rows; ++y)
      for (int x = p.column; x < p.column + p.columns; ++x) owners[y][x] = static_cast<int>(i);
  }
  for (int column = 0; column < columns - 1; ++column) {
    int start = -1;
    for (int row = 0; row <= rows; ++row) {
      bool separates = row < rows && owners[row][column] >= 0 && owners[row][column + 1] >= 0 &&
                       owners[row][column] != owners[row][column + 1];
      if (separates && start < 0) start = row;
      if (!separates && start >= 0) {
        result.dividers.push_back({{xs[column + 1] - gap, ys[start], gap, ys[row] - ys[start] - gap}, true, column}); start = -1;
      }
    }
  }
  for (int row = 0; row < rows - 1; ++row) {
    int start = -1;
    for (int column = 0; column <= columns; ++column) {
      bool separates = column < columns && owners[row][column] >= 0 && owners[row + 1][column] >= 0 &&
                       owners[row][column] != owners[row + 1][column];
      if (separates && start < 0) start = column;
      if (!separates && start >= 0) {
        result.dividers.push_back({{xs[start], ys[row + 1] - gap, xs[column] - xs[start] - gap, gap}, false, row}); start = -1;
      }
    }
  }
  return result;
}

void resizeGridDivider(Workspace& workspace, bool vertical, int boundary, const std::vector<int>& initialSizes, int delta, int minimumSize)
{
  if (boundary < 0 || boundary + 1 >= static_cast<int>(initialSizes.size())) return;
  auto sizes = initialSizes; int total = sizes[boundary] + sizes[boundary + 1];
  minimumSize = std::min(minimumSize, total / 2);
  sizes[boundary] = std::clamp(initialSizes[boundary] + delta, minimumSize, total - minimumSize);
  sizes[boundary + 1] = total - sizes[boundary];
  (vertical ? workspace.columnWeights : workspace.rowWeights) = std::move(sizes);
}

const std::vector<DisplaySize>& unifiedDisplaySizes()
{
  // Siemens Unified Comfort Panels V20, Technical specifications:
  // MTP700/MTP1000/MTP1200 and MTP1500/MTP1900/MTP2200.
  static const std::vector<DisplaySize> sizes = {
    {"7 inch - MTP700 - 800 x 480", 800, 480},
    {"10.1 inch - MTP1000 - 1280 x 800", 1280, 800},
    {"12.1 inch - MTP1200 - 1280 x 800", 1280, 800},
    {"15.6 inch - MTP1500 - 1366 x 768", 1366, 768},
    {"18.5 inch - MTP1900 - 1920 x 1080", 1920, 1080},
    {"21.5 inch - MTP2200 - 1920 x 1080", 1920, 1080}
  };
  return sizes;
}

std::pair<int, int> scaledDisplaySize(const Panel& panel, int serverWidth, int serverHeight)
{
  int width = panel.displayWidth > 0 ? panel.displayWidth : serverWidth > 0 ? serverWidth : 1280;
  int height = panel.displayHeight > 0 ? panel.displayHeight : serverHeight > 0 ? serverHeight : 800;
  int scale = std::clamp(panel.scale, 10, 200);
  return {(width * scale + 50) / 100, (height * scale + 50) / 100};
}

void validatePanel(const Panel& panel)
{
  if (panel.id.empty() || panel.id.size() > 64 ||
      panel.id.find_first_not_of("0123456789abcdefABCDEF-") != std::string::npos)
    throw std::runtime_error("Invalid saved connection identifier");
  if (panel.name.empty() || panel.name.size() > 160 ||
      panel.name.find_first_of("\r\n") != std::string::npos ||
      panel.name.find('\0') != std::string::npos)
    throw std::runtime_error("Enter a panel name (up to 160 characters)");
  if (panel.address.empty() || panel.address.size() > 255 ||
      panel.address.find_first_of("\r\n\t ") != std::string::npos ||
      panel.address.find('\0') != std::string::npos)
    throw std::runtime_error("Enter an IP address or hostname, optionally followed by ::port");
  std::string host;
  int port;
  network::getHostAndPort(panel.address.c_str(), &host, &port);
  if (port < 1 || port > 65535)
    throw std::runtime_error("Port must be between 1 and 65535");
}

std::filesystem::path defaultWorkspacePath()
{
#ifdef WIN32
  PWSTR path = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
    throw std::runtime_error("Cannot find the local application data directory");
  std::filesystem::path result(path);
  CoTaskMemFree(path);
  return result / L"SuperSmartClient" / L"dashboard.db";
#else
  const char* config = std::getenv("XDG_CONFIG_HOME");
  if (config && config[0] == '/')
    return std::filesystem::path(config) / "supersmartclient/dashboard.db";
  const char* homeDir = std::getenv("HOME");
  if (!homeDir) throw std::runtime_error("Cannot find the user configuration directory");
  return std::filesystem::path(homeDir) / ".config/supersmartclient/dashboard.db";
#endif
}

Workspace decodeWorkspace(const std::string& encoded)
{
  if (encoded.size() > 1024 * 1024) throw std::runtime_error("Layout exceeds the size limit");
  Workspace result;
  std::istringstream file(encoded);
  std::string line;
  std::getline(file, line);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (line != "SuperSmartClient Dashboard 1")
    throw std::runtime_error("Unrecognized dashboard file format");
  Panel* panel = nullptr;
  auto integer = [](const std::string& value) {
    size_t consumed;
    int number = std::stoi(value, &consumed);
    if (consumed != value.size()) throw std::runtime_error("Invalid dashboard setting");
    return number;
  };
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (line.compare(0, 7, "[panel ") == 0 && line.back() == ']') {
      if (result.panels.size() >= 32)
        throw std::runtime_error("A dashboard supports up to 32 panels");
      result.panels.emplace_back();
      panel = &result.panels.back();
      panel->id = line.substr(7, line.size() - 8);
      continue;
    }
    size_t equal = line.find('=');
    if (equal == std::string::npos) throw std::runtime_error("Invalid dashboard setting");
    std::string key = line.substr(0, equal), value = line.substr(equal + 1);
    if (!panel) {
      if (key == "columns") result.columns = std::clamp(integer(value), 1, 4);
      else if (key == "rowHeight") result.rowHeight = integer(value) == 0 ? 0 : std::clamp(integer(value), 180, 500);
      else if (key == "width") result.width = std::clamp(integer(value), 860, 7680);
      else if (key == "height") result.height = std::clamp(integer(value), 600, 4320);
      else if (key == "dark") result.dark = integer(value) != 0;
      else if (key == "preset") result.preset = static_cast<Preset>(std::clamp(integer(value), 0, 9));
      else if (key == "columnWeights") result.columnWeights = readWeights(value);
      else if (key == "rowWeights") result.rowWeights = readWeights(value);
    } else {
      if (key == "name") panel->name = hexDecode(value);
      else if (key == "address") panel->address = hexDecode(value);
      else if (key == "credential") panel->credential = value;
      else if (key == "security") {
        if (value == "Certificate") panel->security = SecurityMode::Certificate;
        else if (value == "AnonymousTLS") panel->security = SecurityMode::AnonymousTLS;
        else if (value == "Standard") panel->security = SecurityMode::Standard;
        else throw std::runtime_error("Unknown connection security mode");
      } else if (key == "remember") panel->rememberPassword = integer(value) != 0;
      else if (key == "reconnect") panel->reconnect = integer(value) != 0;
      else if (key == "autoConnect") panel->autoConnect = integer(value) != 0;
      else if (key == "viewOnly") panel->viewOnly = integer(value) != 0;
      else if (key == "columns") panel->columns = std::clamp(integer(value), 1, 4);
      else if (key == "rows") panel->rows = std::clamp(integer(value), 1, 4);
      else if (key == "pixelX") panel->pixelX = std::clamp(integer(value), 0, 65535);
      else if (key == "pixelY") panel->pixelY = std::clamp(integer(value), 0, 65535);
      else if (key == "pixelWidth") panel->pixelWidth = std::clamp(integer(value), 144, 8192);
      else if (key == "pixelHeight") panel->pixelHeight = std::clamp(integer(value), 100, 4320);
      else if (key == "displayWidth") panel->displayWidth = std::clamp(integer(value), 0, 4096);
      else if (key == "displayHeight") panel->displayHeight = std::clamp(integer(value), 0, 2160);
      else if (key == "displayPreset") panel->displayPreset = std::clamp(integer(value), 0, 6);
      else if (key == "scale") panel->scale = std::clamp(integer(value), 10, 200);
      else if (key == "fit") panel->fit = integer(value) != 0;
      else if (key == "freePositioned") panel->freePositioned = integer(value) != 0;
    }
  }
  std::set<std::string> ids;
  if (result.preset == Preset::Free) for (auto& item : result.panels) item.freePositioned = true;
  for (const Panel& item : result.panels) {
    validatePanel(item);
    if (!ids.insert(item.id).second) throw std::runtime_error("Duplicate connection identifier");
  }
  return result;
}

std::string encodeWorkspace(const Workspace& workspace)
{
  if (workspace.panels.size() > 32) throw std::runtime_error("Too many panels");
  std::set<std::string> ids;
  for (const Panel& panel : workspace.panels) {
    validatePanel(panel);
    if (!ids.insert(panel.id).second) throw std::runtime_error("Duplicate connection identifier");
    if (panel.credential.find_first_of("\r\n") != std::string::npos)
      throw std::runtime_error("Invalid credential reference");
  }
  std::ostringstream file;
  file << "SuperSmartClient Dashboard 1\n"
       << "columns=" << workspace.columns << "\nrowHeight=" << workspace.rowHeight
       << "\nwidth=" << workspace.width << "\nheight=" << workspace.height
       << "\ndark=" << workspace.dark << "\npreset=" << static_cast<int>(workspace.preset)
       << "\ncolumnWeights=" << writeWeights(workspace.columnWeights) << "\nrowWeights=" << writeWeights(workspace.rowWeights) << '\n';
  for (const Panel& panel : workspace.panels) {
    const char* mode = panel.security == SecurityMode::Certificate ? "Certificate" :
                       panel.security == SecurityMode::AnonymousTLS ? "AnonymousTLS" : "Standard";
    file << "\n[panel " << panel.id << "]\nname=" << hexEncode(panel.name)
         << "\naddress=" << hexEncode(panel.address) << "\nsecurity=" << mode
         << "\nremember=" << panel.rememberPassword << "\ncredential="
         << (panel.rememberPassword ? panel.credential : "")
         << "\nreconnect=" << panel.reconnect << "\nautoConnect=" << panel.autoConnect
         << "\nviewOnly=" << panel.viewOnly << "\ncolumns=" << panel.columns
         << "\nrows=" << panel.rows << "\npixelX=" << panel.pixelX << "\npixelY=" << panel.pixelY
         << "\npixelWidth=" << panel.pixelWidth << "\npixelHeight=" << panel.pixelHeight
         << "\ndisplayWidth=" << panel.displayWidth << "\ndisplayHeight=" << panel.displayHeight
         << "\ndisplayPreset=" << panel.displayPreset
         << "\nscale=" << panel.scale << "\nfit=" << panel.fit << "\nfreePositioned=" << panel.freePositioned << '\n';
  }
  return file.str();
}

}
