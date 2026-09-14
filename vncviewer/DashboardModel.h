/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SUPERSMART_DASHBOARD_MODEL_H
#define SUPERSMART_DASHBOARD_MODEL_H

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace dashboard {

enum class SecurityMode { Certificate, AnonymousTLS, Standard };
enum class Preset { CustomGrid, Single, SideBySide, Stacked, TopAndTwo,
                    LeftAndTwo, Grid4, Grid6, Grid9, Free };

struct Panel {
  std::string id, name, address;
  // Only credential (an OS-protected blob/reference) is serialized.
  std::string password, credential;
  SecurityMode security = SecurityMode::Certificate;
  bool rememberPassword = true;
  bool reconnect = true;
  bool autoConnect = true;
  bool viewOnly = false;
  int columns = 1, rows = 1;
  int pixelX = 0, pixelY = 0, pixelWidth = 640, pixelHeight = 411;
  int displayWidth = 0, displayHeight = 0, displayPreset = 0, scale = 100;
  bool fit = true;
};

struct Workspace {
  int columns = 2, rowHeight = 0; // Zero fits the grid to the window.
  int width = 1320, height = 860;
  bool dark = false;
  Preset preset = Preset::CustomGrid;
  std::vector<Panel> panels;
};

struct Placement { int column, row, columns, rows; };
std::vector<Placement> layout(const std::vector<Panel>& panels, int columns);
void reorder(std::vector<Panel>& panels, size_t from, size_t to);
void applyPreset(Workspace& workspace, Preset preset);
struct DisplaySize { const char* label; int width, height; };
const std::vector<DisplaySize>& unifiedDisplaySizes();
std::pair<int, int> scaledDisplaySize(const Panel& panel, int serverWidth = 0, int serverHeight = 0);
unsigned reconnectDelay(unsigned failures);
std::string newId();
std::string hexEncode(const std::string& value);
std::string hexDecode(const std::string& value);
std::filesystem::path defaultWorkspacePath();
Workspace decodeWorkspace(const std::string& encoded);
std::string encodeWorkspace(const Workspace& workspace);
void validatePanel(const Panel& panel);

bool passwordStorageAvailable();
std::string protectPassword(const std::string& id, const std::string& password);
std::string unprotectPassword(const std::string& id, const std::string& credential);
void forgetPassword(const std::string& id, const std::string& credential);

}
#endif
