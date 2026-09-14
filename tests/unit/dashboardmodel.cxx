/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include <gtest/gtest.h>
#include "DashboardModel.h"
#include "DashboardStore.h"
#include <cstdlib>
#include <fstream>
#include <set>
#include <sqlite3.h>

using namespace dashboard;
namespace {
Panel panel(const std::string& name = "Line 1") {
  Panel result; result.id = newId(); result.name = name;
  result.address = "127.0.0.1::5900"; return result;
}
std::string contents(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}
class DashboardModel : public ::testing::Test {
protected:
  std::filesystem::path directory = std::filesystem::temp_directory_path() / ("supersmart-test-" + newId());
  void SetUp() override { std::filesystem::create_directory(directory); }
  void TearDown() override { std::filesystem::remove_all(directory); }
};
bool canTestSecrets() {
#ifdef WIN32
  return true;
#else
  return passwordStorageAvailable() && std::getenv("SUPERSMART_TEST_KEYRING");
#endif
}
}

TEST_F(DashboardModel, LayoutPacksMixedSizesWithoutOverlap) {
  std::vector<Panel> panels(6, panel());
  panels[0].columns = 2; panels[0].rows = 2;
  panels[2].columns = 3; panels[3].rows = 4;
  for (int columns = 1; columns <= 4; ++columns) {
    std::set<std::pair<int,int>> occupied;
    auto positions = layout(panels, columns);
    ASSERT_EQ(positions.size(), panels.size());
    for (const auto& position : positions) {
      ASSERT_GE(position.column, 0); ASSERT_LE(position.column + position.columns, columns);
      for (int y = position.row; y < position.row + position.rows; ++y)
        for (int x = position.column; x < position.column + position.columns; ++x)
          EXPECT_TRUE(occupied.insert({x, y}).second);
    }
  }
}
TEST_F(DashboardModel, ReorderKeepsConnectionAndSizeTogether) {
  std::vector<Panel> panels{panel("First"), panel("Second"), panel("Third")};
  panels[0].rows = 3; panels[0].columns = 2; std::string id = panels[0].id;
  reorder(panels, 0, 2);
  EXPECT_EQ(panels[2].id, id); EXPECT_EQ(panels[2].rows, 3); EXPECT_EQ(panels[2].columns, 2);
  EXPECT_EQ(panels[0].name, "Second"); EXPECT_THROW(reorder(panels, 3, 0), std::out_of_range);
}
TEST_F(DashboardModel, PresetsPlacePanelsInTheirSelectedArrangement) {
  Workspace workspace; workspace.panels = {panel("One"), panel("Two"), panel("Three")};
  auto firstId = workspace.panels[0].id;
  applyPreset(workspace, Preset::TopAndTwo);
  auto top = layout(workspace.panels, workspace.columns);
  EXPECT_EQ(top[0].column, 0); EXPECT_EQ(top[0].row, 0); EXPECT_EQ(top[0].columns, 2);
  EXPECT_EQ(top[1].column, 0); EXPECT_EQ(top[1].row, 1);
  EXPECT_EQ(top[2].column, 1); EXPECT_EQ(top[2].row, 1);
  applyPreset(workspace, Preset::LeftAndTwo);
  auto left = layout(workspace.panels, workspace.columns);
  EXPECT_EQ(left[0].column, 0); EXPECT_EQ(left[0].rows, 2);
  EXPECT_EQ(left[1].column, 1); EXPECT_EQ(left[1].row, 0);
  EXPECT_EQ(left[2].column, 1); EXPECT_EQ(left[2].row, 1);
  workspace.panels.pop_back();
  applyPreset(workspace, Preset::Stacked);
  auto stacked = layout(workspace.panels, workspace.columns);
  EXPECT_EQ(stacked[1].column, 0); EXPECT_EQ(stacked[1].row, 1);
  applyPreset(workspace, Preset::SideBySide);
  auto side = layout(workspace.panels, workspace.columns);
  EXPECT_EQ(side[1].column, 1); EXPECT_EQ(side[1].row, 0);
  EXPECT_EQ(workspace.panels[0].id, firstId);
  EXPECT_EQ(workspace.rowHeight, 0);
}
TEST_F(DashboardModel, IndividualScaleUsesNativeOrSelectedSiemensResolution) {
  auto first = panel(), second = panel();
  first.displayWidth = 1280; first.displayHeight = 800; first.scale = 50;
  EXPECT_EQ(scaledDisplaySize(first, 320, 180), std::make_pair(640, 400));
  second.displayWidth = 1366; second.displayHeight = 768; second.scale = 75;
  EXPECT_EQ(scaledDisplaySize(second), std::make_pair(1025, 576));
  auto automatic = panel(); automatic.scale = 50;
  EXPECT_EQ(scaledDisplaySize(automatic, 1920, 1080), std::make_pair(960, 540));
  EXPECT_EQ(scaledDisplaySize(automatic), std::make_pair(640, 400));
  for (const auto& size : unifiedDisplaySizes()) {
    automatic.displayWidth = size.width; automatic.displayHeight = size.height; automatic.scale = 100;
    EXPECT_EQ(scaledDisplaySize(automatic), std::make_pair(size.width, size.height));
  }
}
TEST_F(DashboardModel, DatabaseAndExportPreserveFreePlacementAndIndependentScaling) {
  auto library = newLibrary(); auto& workspace = library.layouts[0].workspace;
  workspace.preset = Preset::Free; workspace.panels = {panel("12 inch"), panel("15 inch")};
  auto& first = workspace.panels[0]; first.fit = false; first.scale = 50; first.displayPreset = 3;
  first.displayWidth = 1280; first.displayHeight = 800; first.pixelX = 31; first.pixelY = 72;
  first.pixelWidth = 644; first.pixelHeight = 451;
  workspace.panels[1].scale = 75; workspace.panels[1].pixelX = 700;
  auto check = [&](const Library& copy) {
    const auto& saved = copy.layouts[0].workspace; ASSERT_EQ(saved.preset, Preset::Free);
    ASSERT_EQ(saved.panels.size(), 2u); const auto& restored = saved.panels[0];
    EXPECT_FALSE(restored.fit); EXPECT_EQ(restored.scale, 50); EXPECT_EQ(restored.displayPreset, 3);
    EXPECT_EQ(restored.pixelX, 31); EXPECT_EQ(restored.pixelY, 72);
    EXPECT_EQ(restored.pixelWidth, 644); EXPECT_EQ(restored.pixelHeight, 451);
    EXPECT_EQ(scaledDisplaySize(restored), std::make_pair(640, 400));
    EXPECT_EQ(saved.panels[1].scale, 75); EXPECT_EQ(saved.panels[1].pixelX, 700);
  };
  auto path = directory / "free.db"; saveLibrary(path, library); check(loadLibrary(path));
  auto exported = directory / "free.sscdb"; exportLibrary(exported, library, "");
  check(importLibrary(exported, ""));
}
TEST_F(DashboardModel, BackoffIsBounded) {
  EXPECT_EQ(reconnectDelay(1), 1u); EXPECT_EQ(reconnectDelay(2), 2u);
  EXPECT_EQ(reconnectDelay(5), 16u); EXPECT_EQ(reconnectDelay(6), 30u);
  EXPECT_EQ(reconnectDelay(999999), 30u);
}
TEST_F(DashboardModel, WorkspaceRoundtripNeverSerializesPlaintextPassword) {
  Workspace source; source.columns = 3; source.dark = true;
  auto item = panel("Pakkelinje \xc3\x98st"); item.password = "secret-marker-123!";
  item.credential = "opaque-test-ciphertext"; item.rows = 2; item.viewOnly = true;
  source.panels.push_back(item);
  auto encoded = encodeWorkspace(source);
  EXPECT_EQ(encoded.find(item.password), std::string::npos);
  EXPECT_EQ(encoded.find(hexEncode(item.password)), std::string::npos);
  auto decoded = decodeWorkspace(encoded);
  ASSERT_EQ(decoded.panels.size(), 1u); EXPECT_EQ(decoded.panels[0].name, item.name);
  EXPECT_EQ(decoded.panels[0].credential, item.credential); EXPECT_TRUE(decoded.panels[0].password.empty());
  EXPECT_TRUE(decoded.panels[0].viewOnly); EXPECT_EQ(decoded.panels[0].rows, 2);
  source.panels[0].rememberPassword = false;
  EXPECT_EQ(encodeWorkspace(source).find(item.credential), std::string::npos);
}
TEST_F(DashboardModel, RejectsInvalidProfilesAndDamagedWorkspaces) {
  auto item = panel(); item.address = "127.0.0.1::99999";
  EXPECT_THROW(validatePanel(item), std::exception);
  item = panel(); item.name = "Line\n[panel bad]"; EXPECT_THROW(validatePanel(item), std::exception);
  EXPECT_THROW(decodeWorkspace("garbage"), std::exception);
  EXPECT_THROW(hexDecode("not hex"), std::exception);
  EXPECT_THROW(hexDecode("abc"), std::exception);
}
TEST_F(DashboardModel, SQLiteRoundtripRetainsNamedLayoutsAndActiveSelection) {
  auto library = newLibrary(); library.layouts[0].workspace.panels.push_back(panel());
  library.layouts.push_back({newId(), "Factory / 'North' | assembly", {}});
  library.active = library.layouts[1].id; library.layouts[1].workspace.columns = 4;
  library.layouts[1].workspace.panels = {panel("One"), panel("Two")};
  library.layouts[1].workspace.panels[0].columns = 2;
  auto path = directory / "layouts.db"; saveLibrary(path, library);
  EXPECT_EQ(contents(path).substr(0, 15), "SQLite format 3");
  auto loaded = loadLibrary(path);
  ASSERT_EQ(loaded.layouts.size(), 2u); EXPECT_EQ(activeLayout(loaded), 1u);
  EXPECT_EQ(loaded.layouts[1].name, library.layouts[1].name);
  EXPECT_EQ(loaded.layouts[1].workspace.columns, 4);
  EXPECT_EQ(loaded.layouts[1].workspace.panels[0].columns, 2);
}
TEST_F(DashboardModel, FailedSavePreservesPreviousDatabase) {
  auto original = newLibrary(); auto path = directory / "layouts.db"; saveLibrary(path, original);
  auto invalid = original; invalid.layouts.push_back({newId(), original.layouts[0].name, {}});
  EXPECT_THROW(saveLibrary(path, invalid), std::exception);
  EXPECT_EQ(loadLibrary(path).layouts.size(), 1u);
  EXPECT_EQ(loadLibrary(path).active, original.active);
}
TEST_F(DashboardModel, DoesNotOverwriteUnrelatedDatabase) {
  auto path = directory / "unrelated.db";
  sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open(path.u8string().c_str(), &db), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE valuable(data TEXT); INSERT INTO valuable VALUES ('keep me');", nullptr, nullptr, nullptr), SQLITE_OK);
  sqlite3_close(db); std::string before = contents(path);
  EXPECT_THROW(saveLibrary(path, newLibrary()), std::exception);
  EXPECT_EQ(contents(path), before);
}
TEST_F(DashboardModel, MergePreservesExistingLayoutsAndResolvesNameCollisions) {
  auto destination = newLibrary(); auto source = newLibrary();
  source.layouts[0].workspace.panels.push_back(panel());
  auto original = destination.active; auto oldPanel = source.layouts[0].workspace.panels[0].id;
  mergeLibrary(destination, source);
  ASSERT_EQ(destination.layouts.size(), 2u); EXPECT_EQ(destination.active, original);
  EXPECT_EQ(destination.layouts[1].name, "Main workspace (2)");
  EXPECT_NE(destination.layouts[1].workspace.panels[0].id, oldPanel);
}
TEST_F(DashboardModel, PlainExportOmitsCredentialsAndPasswords) {
  auto library = newLibrary(); auto item = panel(); item.password = "never-export-plain!";
  item.credential = "opaque-secret-ciphertext"; library.layouts[0].workspace.panels.push_back(item);
  auto path = directory / "portable.sscdb"; exportLibrary(path, library, "");
  EXPECT_FALSE(encryptedExport(path)); auto bytes = contents(path);
  EXPECT_EQ(bytes.find(item.password), std::string::npos);
  EXPECT_EQ(bytes.find(hexEncode(item.password)), std::string::npos);
  EXPECT_EQ(bytes.find(item.credential), std::string::npos);
  auto imported = importLibrary(path, "");
  ASSERT_EQ(imported.layouts[0].workspace.panels.size(), 1u);
  EXPECT_TRUE(imported.layouts[0].workspace.panels[0].credential.empty());
}
#ifdef HAVE_GNUTLS
TEST_F(DashboardModel, EncryptedExportIsPortableAndRejectsWrongPasswordOrTampering) {
  auto library = newLibrary(); auto item = panel(); item.password = "portable-panel-password!";
  library.layouts[0].workspace.panels.push_back(item);
  auto path = directory / "encrypted.sscdb";
  exportLibrary(path, library, "export-password-123!");
  EXPECT_TRUE(encryptedExport(path)); auto bytes = contents(path);
  EXPECT_EQ(bytes.find(item.password), std::string::npos);
  EXPECT_EQ(bytes.find(hexEncode(item.password)), std::string::npos);
  EXPECT_EQ(bytes.find("SQLite format"), std::string::npos);
  auto imported = importLibrary(path, "export-password-123!");
  EXPECT_EQ(imported.layouts[0].workspace.panels[0].password, item.password);
  EXPECT_THROW(importLibrary(path, "wrong-password"), std::exception);
  bytes.back() ^= 1; { std::ofstream out(path, std::ios::binary); out << bytes; }
  EXPECT_THROW(importLibrary(path, "export-password-123!"), std::exception);
}
#endif
TEST_F(DashboardModel, PasswordStoreRoundtripAndIndependentConnections) {
  if (!canTestSecrets()) GTEST_SKIP() << "Requires an isolated desktop keyring or Windows DPAPI";
  auto first = panel(), second = panel();
  first.credential = protectPassword(first.id, "first-secret!");
  second.credential = protectPassword(second.id, "second-secret!");
  EXPECT_EQ(unprotectPassword(first.id, first.credential), "first-secret!");
  EXPECT_EQ(unprotectPassword(second.id, second.credential), "second-secret!");
  EXPECT_THROW(unprotectPassword(second.id, first.credential), std::exception);
  EXPECT_THROW(unprotectPassword(first.id, "dpapi:"), std::exception);
  forgetPassword(first.id, first.credential); forgetPassword(second.id, second.credential);
}
TEST_F(DashboardModel, DuplicateResealsSavedPasswordsForNewIdentifiers) {
  if (!canTestSecrets()) GTEST_SKIP() << "Requires an isolated desktop keyring or Windows DPAPI";
  auto item = panel(); item.credential = protectPassword(item.id, "saved-password!");
  SavedLayout original{newId(), "Original", {}}; original.workspace.panels.push_back(item);
  auto duplicate = duplicateLayout(original, "Copy");
  const auto& copy = duplicate.workspace.panels[0];
  EXPECT_NE(copy.id, item.id); EXPECT_NE(copy.credential, item.credential);
  EXPECT_EQ(unprotectPassword(copy.id, copy.credential), "saved-password!");
  forgetPassword(item.id, item.credential); forgetPassword(copy.id, copy.credential);
}
