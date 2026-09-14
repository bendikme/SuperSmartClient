/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include <gtest/gtest.h>
#include "DashboardModel.h"
#include "DashboardImage.h"
#include "AppUpdate.h"
#include "DashboardStore.h"
#include <cstdlib>
#include <fstream>
#include <set>
#include <thread>
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
TEST_F(DashboardModel, GridFillsViewportAndDividersRespectSpanningPanes) {
  Workspace source; source.panels = {panel(), panel(), panel()};
  applyPreset(source, Preset::TopAndTwo);
  auto grid = gridGeometry(source, 1312, 812);
  ASSERT_EQ(grid.panels.size(), 3u);
  EXPECT_EQ(grid.width, 1312); EXPECT_EQ(grid.height, 812);
  EXPECT_EQ(grid.panels[0].width, 1312); EXPECT_EQ(grid.panels[0].height, 404);
  EXPECT_EQ(grid.panels[1].x, 0); EXPECT_EQ(grid.panels[1].y, 408);
  EXPECT_EQ(grid.panels[2].x, 658); EXPECT_EQ(grid.panels[2].width, 654);
  EXPECT_EQ(grid.panels[2].y + grid.panels[2].height, 812);
  ASSERT_EQ(grid.dividers.size(), 2u);
  for (const auto& divider : grid.dividers) {
    if (divider.vertical) { EXPECT_EQ(divider.rect.y, 408); EXPECT_EQ(divider.rect.height, 404); }
    else { EXPECT_EQ(divider.rect.y, 404); EXPECT_EQ(divider.rect.width, 1312); }
  }
  // Resolution, scale, and old fixed-height settings do not influence grids.
  source.panels[0].displayWidth = 1920; source.panels[0].displayHeight = 1080;
  source.panels[1].scale = 35; source.panels[1].fit = false; source.rowHeight = 500;
  auto unchanged = gridGeometry(source, 1312, 812);
  for (size_t number = 0; number < grid.panels.size(); ++number) {
    EXPECT_EQ(unchanged.panels[number].x, grid.panels[number].x);
    EXPECT_EQ(unchanged.panels[number].y, grid.panels[number].y);
    EXPECT_EQ(unchanged.panels[number].width, grid.panels[number].width);
    EXPECT_EQ(unchanged.panels[number].height, grid.panels[number].height);
  }
  source.panels.resize(1);
  auto single = gridGeometry(source, 1312, 812);
  EXPECT_EQ(single.panels[0].width, 1312); EXPECT_EQ(single.panels[0].height, 812);
  EXPECT_TRUE(single.dividers.empty());
}
TEST_F(DashboardModel, DividerResizingPreservesAdjacentTotalsAndSurvivesSave) {
  Workspace source; source.panels = {panel(), panel(), panel(), panel(), panel(), panel()};
  applyPreset(source, Preset::Grid6);
  auto before = gridGeometry(source, 1312, 812);
  resizeGridDivider(source, true, 0, before.columnSizes, 80, 160);
  resizeGridDivider(source, false, 0, before.rowSizes, -90, 96);
  auto saved = decodeWorkspace(encodeWorkspace(source));
  auto after = gridGeometry(saved, 1312, 812);
  EXPECT_EQ(after.columnSizes[0], before.columnSizes[0] + 80);
  EXPECT_EQ(after.columnSizes[1], before.columnSizes[1] - 80);
  EXPECT_EQ(after.columnSizes[2], before.columnSizes[2]);
  EXPECT_EQ(after.rowSizes[0], before.rowSizes[0] - 90);
  EXPECT_EQ(after.rowSizes[1], before.rowSizes[1] + 90);
  reorder(saved.panels, 0, 1); applyPreset(saved, Preset::Grid6, false);
  EXPECT_EQ(gridGeometry(saved, 1312, 812).columnSizes, after.columnSizes);
  resizeGridDivider(saved, true, 0, before.columnSizes, 100000, 160);
  EXPECT_EQ(gridGeometry(saved, 1312, 812).columnSizes[1], 160);
  applyPreset(saved, Preset::Stacked);
  EXPECT_TRUE(saved.columnWeights.empty()); EXPECT_TRUE(saved.rowWeights.empty());
}
TEST_F(DashboardModel, FixedGridPresetsKeepTheirDeclaredRows) {
  Workspace source; source.panels = {panel(), panel(), panel(), panel()};
  for (auto preset : {Preset::Grid4, Preset::Grid6, Preset::Grid9}) {
    applyPreset(source, preset);
    auto grid = gridGeometry(source, 1312, 812);
    EXPECT_EQ(grid.columnSizes.size(), preset == Preset::Grid4 ? 2u : 3u);
    EXPECT_EQ(grid.rowSizes.size(), preset == Preset::Grid9 ? 3u : 2u);
  }
}
TEST_F(DashboardModel, GridRatiosScaleWithViewportWithoutRoundingGaps) {
  Workspace source; source.panels = {panel(), panel(), panel(), panel()};
  applyPreset(source, Preset::Grid4); source.columnWeights = {3, 2}; source.rowWeights = {2, 3};
  for (auto size : {std::make_pair(857, 547), std::make_pair(1920, 1080), std::make_pair(3840, 2160)}) {
    auto grid = gridGeometry(source, size.first, size.second);
    EXPECT_EQ(grid.panels[3].x + grid.panels[3].width, size.first);
    EXPECT_EQ(grid.panels[3].y + grid.panels[3].height, size.second);
    EXPECT_NEAR(grid.columnSizes[0] / static_cast<double>(size.first - 4), 0.6, 0.002);
    EXPECT_NEAR(grid.rowSizes[0] / static_cast<double>(size.second - 4), 0.4, 0.002);
  }
  source.panels.resize(32, panel()); applyPreset(source, Preset::Stacked);
  auto full = gridGeometry(source, 1600, 1000, 2, 1, 1);
  EXPECT_EQ(full.panels.back().y + full.panels.back().height, 1000);
  auto windowed = gridGeometry(source, 852, 552);
  EXPECT_GE(windowed.rowSizes.back(), 96); EXPECT_GT(windowed.height, 552);
}
TEST_F(DashboardModel, GridTransitionsPreserveFreeWindowGeometryAndScale) {
  Workspace source; auto item = panel(); item.pixelX = 85; item.pixelY = 126;
  item.pixelWidth = 642; item.pixelHeight = 426; item.scale = 50;
  item.displayWidth = 1280; item.displayHeight = 800; item.fit = false; item.freePositioned = true;
  source.panels.push_back(item);
  for (auto preset : {Preset::TopAndTwo, Preset::Grid4, Preset::SideBySide, Preset::Free}) {
    applyPreset(source, preset); source = decodeWorkspace(encodeWorkspace(source));
    EXPECT_EQ(source.panels[0].pixelX, 85); EXPECT_EQ(source.panels[0].pixelY, 126);
    EXPECT_EQ(source.panels[0].pixelWidth, 642); EXPECT_EQ(source.panels[0].pixelHeight, 426);
    EXPECT_EQ(source.panels[0].scale, 50); EXPECT_FALSE(source.panels[0].fit);
    EXPECT_TRUE(source.panels[0].freePositioned);
  }
  // Older free layouts do not carry the positioning flag, but must still restore.
  auto legacy = encodeWorkspace(source); auto flag = legacy.find("freePositioned=1");
  legacy.erase(flag, std::string("freePositioned=1\n").size());
  EXPECT_TRUE(decodeWorkspace(legacy).panels[0].freePositioned);
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
TEST_F(DashboardModel, NativeImageIsPixelExactAfterChangingScale) {
  ImageScaler scaler;
  std::vector<uint8_t> source(17 * 13 * 3);
  for (size_t index = 0; index < source.size(); ++index) source[index] = static_cast<uint8_t>(index * 97);
  auto original = source;
  scaler.scale(source, 17, 13, 11, 8);
  EXPECT_EQ(scaler.scale(source, 17, 13, 17, 13), original);
  EXPECT_EQ(source, original);
  EXPECT_EQ(scaler.scale(source, 17, 13, 23, 18), ImageScaler().scale(source, 17, 13, 23, 18));
  for (auto& byte : source) byte = 255 - byte;
  EXPECT_EQ(scaler.scale(source, 17, 13, 23, 18), ImageScaler().scale(source, 17, 13, 23, 18));
  EXPECT_EQ(scaler.scale(source, 13, 17, 23, 18), ImageScaler().scale(source, 13, 17, 23, 18));
}
TEST_F(DashboardModel, ImageScalingPreservesFlatColoursAndEdges) {
  ImageScaler scaler;
  std::vector<uint8_t> source(29 * 19 * 3);
  for (size_t index = 0; index < source.size(); index += 3) {
    source[index] = 17; source[index + 1] = 139; source[index + 2] = 241;
  }
  for (auto size : {std::make_pair(1, 1), std::make_pair(7, 5), std::make_pair(43, 31)}) {
    const auto& pixels = scaler.scale(source, 29, 19, size.first, size.second);
    ASSERT_EQ(pixels.size(), static_cast<size_t>(size.first) * size.second * 3);
    for (size_t index = 0; index < pixels.size(); index += 3) {
      ASSERT_EQ(pixels[index], 17); ASSERT_EQ(pixels[index + 1], 139); ASSERT_EQ(pixels[index + 2], 241);
    }
  }
  EXPECT_EQ(scaler.scale({17, 139, 241}, 1, 1, 29, 19), source);
  EXPECT_THROW(scaler.scale(source, 0, 19, 7, 5), std::invalid_argument);
  EXPECT_THROW(scaler.scale(source, 29, 19, 0, 5), std::invalid_argument);
  EXPECT_THROW(scaler.scale({1, 2}, 1, 1, 7, 5), std::invalid_argument);
}
TEST_F(DashboardModel, DownscalingSuppressesAliasingAtPanelSizes) {
  ImageScaler scaler;
  std::vector<uint8_t> source(1280 * 800 * 3);
  for (int y = 0; y < 800; ++y) for (int x = 0; x < 1280; ++x)
    for (int channel = 0; channel < 3; ++channel) source[(y * 1280 + x) * 3 + channel] = (x + y) % 2 ? 255 : 0;
  for (int percent : {10, 25, 50, 75}) {
    int width = 1280 * percent / 100, height = 800 * percent / 100;
    const auto& pixels = scaler.scale(source, 1280, 800, width, height);
    // Fine detail above the output's Nyquist limit must average to neutral grey,
    // not disappear into black/white or produce a new checkerboard pattern.
    for (int y = 4; y < height - 4; ++y) for (int x = 4; x < width - 4; ++x)
      ASSERT_NEAR(pixels[(y * width + x) * 3], 127.5, 2);
  }
}
TEST_F(DashboardModel, DownscalingRetainsThinStrokesBetweenSamplePositions) {
  ImageScaler scaler;
  for (int position = 20; position < 24; ++position) {
    std::vector<uint8_t> source(80 * 32 * 3, 255);
    for (int y = 0; y < 32; ++y) for (int channel = 0; channel < 3; ++channel)
      source[(y * 80 + position) * 3 + channel] = 0;
    const auto& pixels = scaler.scale(source, 80, 32, 20, 8);
    int darkest = 255;
    for (int x = 0; x < 20; ++x) darkest = std::min(darkest, static_cast<int>(pixels[(4 * 20 + x) * 3]));
    EXPECT_LT(darkest, 220) << "Lost a one-pixel stroke at source column " << position;
  }
}
TEST_F(DashboardModel, EnlargementInterpolatesColoursWithoutChangingChannels) {
  ImageScaler scaler;
  const auto& pixels = scaler.scale({255, 0, 0, 0, 0, 255}, 2, 1, 8, 4);
  int blended = 0;
  for (size_t index = 0; index < pixels.size(); index += 3) {
    EXPECT_EQ(pixels[index + 1], 0);
    EXPECT_NEAR(pixels[index] + pixels[index + 2], 255, 1);
    blended += pixels[index] > 0 && pixels[index + 2] > 0;
  }
  EXPECT_GE(blended, 8);
  EXPECT_EQ(pixels[0], 255); EXPECT_EQ(pixels[8 * 3 - 1], 255);
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
TEST_F(DashboardModel, UpdateVersionsCompareNumericallyAndRejectInvalidReleases) {
  EXPECT_TRUE(newerVersion("1.10.0", "1.9.0"));
  EXPECT_TRUE(newerVersion("2.0.0", "1.99.99"));
  EXPECT_TRUE(newerVersion("1.0.1", "1.0.0"));
  EXPECT_FALSE(newerVersion("1.0.0", "1.0.0"));
  EXPECT_FALSE(newerVersion("1.9.0", "1.10.0"));
  for (auto invalid : {"", "1.0", "1.0.0.1", "01.0.0", "ssc-v1.0.0", "1.0.0-beta", "1.0.0\n", "1.0.0;cmd", "99999999999.0.0"})
    EXPECT_THROW(newerVersion(invalid, "1.0.0"), std::invalid_argument);
}
#ifdef _WIN32
TEST_F(DashboardModel, UpdateHelperRunsInBackgroundWithUnicodePathsAndPersistsPreference) {
  auto app = directory / std::filesystem::u8path("portable app \xc3\xb8 & (test)");
  std::filesystem::create_directory(app);
  std::ofstream(app / "manifest.json") << "{}";
  std::ofstream(app / "update.ps1") <<
    "param($Action, $AppDirectory, $WorkDirectory, $InstalledVersion, $ParentId, $ConfigPath, $RestartArguments)\n"
    "$state = if ($Action -eq 'Install') { 'installed' } elseif ($Action -eq 'Download') { 'ready' } else { 'available' }\n"
    "if ($Action -eq 'Install') { [IO.File]::WriteAllText((Join-Path $WorkDirectory 'restart-fixture.txt'), [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($RestartArguments)), [Text.UTF8Encoding]::new($false)); [IO.File]::WriteAllText((Join-Path $WorkDirectory 'config-fixture.txt'), $ConfigPath, [Text.UTF8Encoding]::new($false)) }\n"
    "[IO.File]::WriteAllText((Join-Path $WorkDirectory 'status.txt'), ($state + \"`n1.2.0`nFixture update`n\"), [Text.UTF8Encoding]::new($false))\n";
  AppUpdate updater(directory / "test layouts.db", true, app);
  ASSERT_TRUE(updater.supported());
  updater.automatic(false);
  EXPECT_FALSE(updater.automatic());
  updater.check();
  ASSERT_EQ(updater.state(), AppUpdate::State::Checking);
  auto finish = [&] {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (updater.busy() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20)); updater.poll();
    }
  };
  finish();
  ASSERT_EQ(updater.state(), AppUpdate::State::Available) << updater.message();
  EXPECT_EQ(updater.version(), "1.2.0");
  updater.download(); finish();
  ASSERT_EQ(updater.state(), AppUpdate::State::Ready) << updater.message();
  AppUpdate restored(directory / "test layouts.db", true, app);
  EXPECT_FALSE(restored.automatic());
  EXPECT_EQ(restored.state(), AppUpdate::State::Ready);
  ASSERT_TRUE(updater.install()); finish();
  EXPECT_EQ(updater.state(), AppUpdate::State::Current) << updater.message();
  EXPECT_TRUE(std::filesystem::exists(directory / ".supersmart-updates/restart-fixture.txt"));
  EXPECT_EQ(contents(directory / ".supersmart-updates/config-fixture.txt"), (directory / "test layouts.db").u8string());
}
#endif
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
