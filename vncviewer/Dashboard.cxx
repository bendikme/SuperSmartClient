/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include "DashboardModel.h"
#include "DashboardStore.h"
#include "DashboardSession.h"
#include "keysym2ucs.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#include <core/string.h>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

namespace dashboard {
namespace {
struct Palette {
  Fl_Color background, card, border, text, muted, accent, soft, green, amber;
};
Palette palette(bool dark)
{
  if (dark) return {fl_rgb_color(17, 24, 39), fl_rgb_color(30, 41, 59),
    fl_rgb_color(55, 65, 81), fl_rgb_color(241, 245, 249), fl_rgb_color(148, 163, 184),
    fl_rgb_color(96, 165, 250), fl_rgb_color(39, 56, 78), fl_rgb_color(52, 211, 153),
    fl_rgb_color(251, 191, 36)};
  return {fl_rgb_color(242, 245, 249), FL_WHITE, fl_rgb_color(220, 227, 236),
    fl_rgb_color(24, 39, 60), fl_rgb_color(101, 116, 137), fl_rgb_color(37, 99, 235),
    fl_rgb_color(235, 241, 253), fl_rgb_color(20, 143, 102), fl_rgb_color(181, 110, 10)};
}
void rounded(int x, int y, int w, int h, Fl_Color color, int radius = 8)
{
  color = Fl::get_color(color);
  radius = std::min(radius, std::min(w, h) / 2);
  if (radius < 1) { fl_color(color); fl_rectf(x, y, w, h); return; }
  fl_color(color);
  fl_rectf(x + radius, y, w - 2 * radius, h);
  fl_rectf(x, y + radius, w, h - 2 * radius);
  // Windows GDI's partial pies have inconsistent corner geometry. Cached
  // alpha corners give the same smooth outline on Windows and X11.
  static std::map<std::pair<int, Fl_Color>, std::unique_ptr<Fl_RGB_Image>> corners;
  auto& corner = corners[{radius, color}];
  if (!corner) {
    int diameter = radius * 2;
    auto* pixels = new unsigned char[diameter * diameter * 4];
    unsigned char red, green, blue; Fl::get_color(color, red, green, blue);
    for (int row = 0; row < diameter; ++row) for (int col = 0; col < diameter; ++col) {
      int coverage = 0;
      for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
        double dx = col + (sx + 0.5) / 4 - radius, dy = row + (sy + 0.5) / 4 - radius;
        coverage += dx * dx + dy * dy <= radius * radius;
      }
      auto* pixel = pixels + (row * diameter + col) * 4;
      pixel[0] = red; pixel[1] = green; pixel[2] = blue; pixel[3] = coverage * 255 / 16;
    }
    corner.reset(new Fl_RGB_Image(pixels, diameter, diameter, 4)); corner->alloc_array = 1;
  }
  corner->draw(x, y, radius, radius, 0, 0);
  corner->draw(x + w - radius, y, radius, radius, radius, 0);
  corner->draw(x, y + h - radius, radius, radius, 0, radius);
  corner->draw(x + w - radius, y + h - radius, radius, radius, radius, radius);
}
void caption(const std::string& text, int x, int y, int w, int h,
             Fl_Color color, int size = 13, bool bold = false,
             Fl_Align align = FL_ALIGN_LEFT)
{
  fl_font(bold ? FL_HELVETICA_BOLD : FL_HELVETICA, size);
  fl_color(color);
  fl_draw(text.c_str(), x, y, w, h, align | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
}
bool hitRect(int x, int y, int w, int h)
{
  return Fl::event_x() >= x && Fl::event_x() < x + w &&
         Fl::event_y() >= y && Fl::event_y() < y + h;
}

void dialogButtonBox(int x, int y, int w, int h, Fl_Color color) {
  fl_color(FL_BACKGROUND_COLOR); fl_rectf(x, y, w, h);
  rounded(x, y, w, h, fl_color_average(FL_FOREGROUND_COLOR, color, 0.2f), 8);
  rounded(x + 1, y + 1, w - 2, h - 2, color, 7);
}

class Button : public Fl_Button {
public:
  Button(int x, int y, int w, int h, const char* text, bool primary = false)
    : Fl_Button(x, y, w, h, text), primary_(primary) { box(FL_NO_BOX); }
  void theme(const Palette& colors, Fl_Color background) { colors_ = colors; background_ = background; redraw(); }
  void draw() override {
    fl_color(background_); fl_rectf(x(), y(), w(), h());
    Fl_Color fill = primary_ ? fl_rgb_color(37, 99, 235) : colors_.card;
    if (value()) fill = fl_color_average(fill, colors_.text, 0.85f);
    else if (hover_) fill = fl_color_average(fill, primary_ ? FL_WHITE : colors_.accent, 0.92f);
    rounded(x(), y(), w(), h(), Fl::focus() == this ? colors_.accent : primary_ ? fill : colors_.border, 9);
    rounded(x() + 1, y() + 1, w() - 2, h() - 2, fill, 8);
    caption(label(), x() + 6, y(), w() - 12, h(), primary_ ? FL_WHITE : colors_.text,
      13, primary_, FL_ALIGN_CENTER);
  }
  int handle(int event) override {
    if (event == FL_ENTER || event == FL_LEAVE) { hover_ = event == FL_ENTER; redraw(); }
    return Fl_Button::handle(event);
  }
private:
  bool primary_;
  bool hover_ = false;
  Palette colors_ = palette(false);
  Fl_Color background_ = palette(false).background;
};

class Choice : public Fl_Choice {
public:
  using Fl_Choice::Fl_Choice;
  void theme(const Palette& colors, Fl_Color background) {
    colors_ = colors; background_ = background;
    color(colors.card); textcolor(colors.text); labelcolor(colors.text);
    // FLTK changes FL_FLAT_BOX to its light, beveled FL_UP_BOX for popups.
    selection_color(fl_rgb_color(37, 99, 235)); textsize(12); box(FL_BORDER_BOX); down_box(FL_FLAT_BOX);
    redraw();
  }
  void draw() override {
    fl_color(background_); fl_rectf(x(), y(), w(), h());
    rounded(x(), y(), w(), h(), Fl::focus() == this ? colors_.accent : colors_.border, 7);
    rounded(x() + 1, y() + 1, w() - 2, h() - 2, color(), 6);
    Fl_Color ink = active_r() ? textcolor() : colors_.muted;
    if (mvalue()) caption(mvalue()->label(), x() + 9, y(), w() - 33, h(), ink, textsize());
    fl_color(ink); int right = x() + w() - 13, middle = y() + h() / 2;
    fl_line(right - 4, middle - 2, right, middle + 2, right + 4, middle - 2);
    draw_label();
  }
private:
  Palette colors_ = palette(false);
  Fl_Color background_ = palette(false).background;
};

void styleDialog(Fl_Group& dialog, const Palette& colors) {
  dialog.color(colors.background);
  for (int i = 0; i < dialog.children(); ++i) {
    auto* widget = dialog.child(i); widget->labelcolor(colors.text);
    if (auto* button = dynamic_cast<Button*>(widget)) button->theme(colors, colors.background);
    else if (auto* choice = dynamic_cast<Choice*>(widget)) choice->theme(colors, colors.background);
    else if (auto* input = dynamic_cast<Fl_Input_*>(widget)) {
      input->color(colors.card); input->textcolor(colors.text); input->cursor_color(colors.accent);
      input->selection_color(fl_rgb_color(37, 99, 235)); input->box(FL_BORDER_BOX);
    } else { widget->color(colors.background); widget->selection_color(colors.accent); }
  }
}

class Editor : public Fl_Double_Window {
public:
  Editor(const Panel& panel, bool fresh, const Palette& colors)
    : Fl_Double_Window(540, 552, fresh ? "Add panel - SuperSmartClient" : "Edit panel - SuperSmartClient"),
      result(panel)
  {
    color(palette(false).background);
    begin();
    auto* heading = new Fl_Box(28, 18, 484, 32, fresh ? "Add a panel" : "Connection settings");
    heading->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    heading->labelfont(FL_HELVETICA_BOLD); heading->labelsize(22);
    // The form uses explicit labels, keeping keyboard navigation in reading order.
    name_ = new Fl_Input(28, 86, 484, 34, "Panel name");
    name_->align(FL_ALIGN_TOP_LEFT); name_->value(panel.name.c_str());
    address_ = new Fl_Input(28, 151, 484, 34, "IP address or hostname");
    address_->align(FL_ALIGN_TOP_LEFT); address_->value(panel.address.c_str());
    auto* addressHint = new Fl_Box(28, 189, 484, 21, "Optional port: 192.168.1.20::5900");
    addressHint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE); addressHint->labelsize(11);
    security_ = new Choice(28, 241, 484, 34, "Connection security");
    security_->align(FL_ALIGN_TOP_LEFT);
    security_->add("Unified - certificate TLS|Unified - anonymous TLS|Standard VNC (unencrypted)");
    security_->value(panel.security == SecurityMode::Certificate ? 0 :
                     panel.security == SecurityMode::AnonymousTLS ? 1 : 2);
    password_ = new Fl_Secret_Input(28, 308, 484, 34, "Password");
    password_->align(FL_ALIGN_TOP_LEFT);
    if (result.password.empty() && !result.credential.empty()) {
      try { result.password = unprotectPassword(result.id, result.credential); }
      catch (const std::exception&) { password_->tooltip("Enter a replacement if the saved password is unavailable"); }
    }
    password_->value(result.password.c_str());
    remember_ = new Fl_Check_Button(28, 352, 484, 25, "Remember password in my account");
    remember_->value(panel.rememberPassword && passwordStorageAvailable());
    if (!passwordStorageAvailable()) {
      remember_->deactivate(); remember_->label("Password storage unavailable in this build");
    }
    reconnect_ = new Fl_Check_Button(28, 383, 250, 25, "Reconnect automatically");
    reconnect_->value(panel.reconnect);
    startup_ = new Fl_Check_Button(282, 383, 230, 25, "Connect on startup");
    startup_->value(panel.autoConnect);
    monitor_ = new Fl_Check_Button(28, 414, 484, 25, "Monitor only (disable mouse and keyboard)");
    monitor_->value(panel.viewOnly);
    Fl_Widget* fields[] = {name_, address_, security_, password_, remember_, reconnect_, startup_, monitor_};
    for (auto* widget : fields) widget->labelsize(12);
    auto* cancel = new Button(28, 482, 94, 38, "Cancel");
    auto* save = new Button(234, 482, 100, 38, "Save");
    auto* connect = new Button(346, 482, 166, 38, "Save & connect", true);
    cancel->callback([](Fl_Widget*, void* data) { static_cast<Editor*>(data)->hide(); }, this);
    save->callback([](Fl_Widget*, void* data) { static_cast<Editor*>(data)->save(false); }, this);
    connect->callback([](Fl_Widget*, void* data) { static_cast<Editor*>(data)->save(true); }, this);
    callback([](Fl_Widget* widget, void*) { widget->hide(); });
    end(); set_modal(); styleDialog(*this, colors);
  }
  bool run() { show(); name_->take_focus(); while (shown()) Fl::wait(); return accepted; }
  Panel result;
  bool accepted = false, connectAfter = false;
private:
  void save(bool connect) {
    try {
      Panel candidate = result;
      candidate.name = name_->value(); candidate.address = address_->value();
      candidate.security = security_->value() == 0 ? SecurityMode::Certificate :
        security_->value() == 1 ? SecurityMode::AnonymousTLS : SecurityMode::Standard;
      candidate.rememberPassword = remember_->value();
      candidate.reconnect = reconnect_->value(); candidate.autoConnect = startup_->value();
      candidate.viewOnly = monitor_->value();
      validatePanel(candidate);
      bool changed = candidate.password != password_->value();
      candidate.password = password_->value();
      if (!candidate.rememberPassword) candidate.credential.clear();
      else if (changed || candidate.credential.empty())
        candidate.credential = protectPassword(candidate.id, candidate.password);
      result = std::move(candidate);
      accepted = true; connectAfter = connect; hide();
    } catch (const std::exception& error) { fl_alert("%s", error.what()); }
  }
  Fl_Input *name_, *address_;
  Fl_Secret_Input* password_;
  Fl_Choice* security_;
  Fl_Check_Button *remember_, *reconnect_, *startup_, *monitor_;
};

class ExportDialog : public Fl_Double_Window {
public:
  ExportDialog(const Palette& colors) : Fl_Double_Window(500, 274, "Export database - SuperSmartClient") {
    color(palette(false).background); begin();
    include_ = new Fl_Check_Button(24, 23, 452, 30, "Include saved passwords");
#ifdef HAVE_GNUTLS
    include_->value(1);
#else
    include_->deactivate();
#endif
    password_ = new Fl_Secret_Input(24, 94, 452, 34, "Protect export with a password");
    password_->align(FL_ALIGN_TOP_LEFT);
    auto* hint = new Fl_Box(24, 142, 452, 49,
      "Use at least 8 characters. You will need this password on import.\nUncheck above to export layouts without passwords.");
    hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP); hint->labelsize(12);
    auto* cancel = new Button(248, 214, 100, 36, "Cancel");
    auto* exportButton = new Button(360, 214, 116, 36, "Export", true);
    cancel->callback([](Fl_Widget*, void* data) { static_cast<ExportDialog*>(data)->hide(); }, this);
    exportButton->callback([](Fl_Widget*, void* data) {
      auto& dialog = *static_cast<ExportDialog*>(data);
      dialog.password = dialog.include_->value() ? dialog.password_->value() : "";
      if (dialog.include_->value() && dialog.password.size() < 8) {
        fl_alert("Use at least 8 characters for the export password."); return;
      }
      dialog.accepted = true; dialog.hide();
    }, this);
    include_->callback([](Fl_Widget*, void* data) {
      auto& dialog = *static_cast<ExportDialog*>(data);
      if (dialog.include_->value()) dialog.password_->activate(); else dialog.password_->deactivate();
    }, this);
    callback([](Fl_Widget* widget, void*) { widget->hide(); });
    end(); set_modal(); styleDialog(*this, colors);
  }
  bool run() { show(); password_->take_focus(); while (shown()) Fl::wait(); return accepted; }
  std::string password;
private:
  bool accepted = false;
  Fl_Check_Button* include_;
  Fl_Secret_Input* password_;
};

class ViewDialog : public Fl_Double_Window {
public:
  ViewDialog(const Panel& panel, int serverWidth, int serverHeight, const Palette& colors)
    : Fl_Double_Window(500, 326, "Panel size and scale - SuperSmartClient"), result(panel),
      serverWidth_(serverWidth), serverHeight_(serverHeight) {
    color(palette(false).background); begin();
    screen_ = new Choice(24, 55, 452, 32, "Siemens screen size");
    screen_->align(FL_ALIGN_TOP_LEFT); screen_->add("Automatic - use the panel's resolution");
    int selected = 0;
    const auto& sizes = unifiedDisplaySizes();
    for (size_t number = 0; number < sizes.size(); ++number) {
      screen_->add(sizes[number].label);
      if (sizes[number].width == panel.displayWidth && sizes[number].height == panel.displayHeight && !selected)
        selected = static_cast<int>(number) + 1;
    }
    if (panel.displayPreset > 0 && panel.displayPreset <= static_cast<int>(sizes.size()))
      selected = panel.displayPreset;
    screen_->value(selected);
    scale_ = new Fl_Int_Input(24, 123, 110, 32, "Scale (%)");
    scale_->align(FL_ALIGN_TOP_LEFT); scale_->value(std::to_string(panel.scale).c_str());
    fit_ = new Fl_Check_Button(153, 123, 323, 32, "Fit picture to the window"); fit_->value(panel.fit);
    preview_ = new Fl_Box(24, 171, 452, 32); preview_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    auto* note = new Fl_Box(24, 206, 452, 44, "Fixed sizes use free placement. Choose an arrangement to return to a grid.\nThe panel's own resolution is unchanged.");
    note->labelsize(11); note->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    auto* cancel = new Button(252, 270, 100, 34, "Cancel");
    auto* apply = new Button(364, 270, 112, 34, "Apply", true);
    auto update = [](Fl_Widget*, void* data) { static_cast<ViewDialog*>(data)->preview(); };
    screen_->callback(update, this); fit_->callback(update, this); scale_->callback(update, this); scale_->when(FL_WHEN_CHANGED);
    cancel->callback([](Fl_Widget*, void* data) { static_cast<ViewDialog*>(data)->hide(); }, this);
    apply->callback([](Fl_Widget*, void* data) {
      auto& dialog = *static_cast<ViewDialog*>(data);
      try {
        dialog.read();
        if (dialog.result.scale < 10 || dialog.result.scale > 200) throw std::runtime_error("Choose a scale from 10% to 200%");
        dialog.accepted = true; dialog.hide();
      } catch (const std::exception& error) { fl_alert("%s", error.what()); }
    }, this);
    callback([](Fl_Widget* widget, void*) { widget->hide(); });
    end(); set_modal(); styleDialog(*this, colors); preview();
  }
  bool run() { show(); while (shown()) Fl::wait(); return accepted; }
  Panel result;
private:
  void read() {
    result.displayWidth = result.displayHeight = 0;
    result.displayPreset = screen_->value();
    if (screen_->value() > 0) {
      const auto& size = unifiedDisplaySizes()[screen_->value() - 1];
      result.displayWidth = size.width; result.displayHeight = size.height;
    }
    result.fit = fit_->value();
    size_t used; std::string text(scale_->value()); result.scale = std::stoi(text, &used);
    if (used != text.size()) throw std::runtime_error("Enter a percentage from 10 to 200");
  }
  void preview() {
    try {
      read();
      if (result.fit) { scale_->deactivate(); preview_->copy_label("Picture fits the current window."); }
      else {
        scale_->activate(); auto size = scaledDisplaySize(result, serverWidth_, serverHeight_);
        std::string text = std::to_string(size.first) + " x " + std::to_string(size.second) + " pixel picture area";
        if (!result.displayWidth && !serverWidth_) text += " (updates when connected)";
        preview_->copy_label(text.c_str());
      }
    } catch (const std::exception&) { preview_->copy_label("Enter a percentage from 10 to 200"); }
    redraw();
  }
  Fl_Choice* screen_;
  Fl_Int_Input* scale_;
  Fl_Check_Button* fit_;
  Fl_Box* preview_;
  int serverWidth_, serverHeight_;
  bool accepted = false;
};

class Dashboard;
class Tile : public Fl_Widget {
public:
  Tile(Dashboard& owner, Panel panel);
  ~Tile() override = default;
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void changed();
  Panel panel;
  Session session;
private:
  void imageRect(int& x, int& y, int& w, int& h) const;
  void sendPointer(unsigned mask);
  unsigned mouseMask() const;
  Dashboard& owner_;
  std::unique_ptr<Fl_Image> image_;
  unsigned generation_ = ~0u;
  int imageWidth_ = 0, imageHeight_ = 0;
  int serverWidth_ = 0, serverHeight_ = 0;
  Session::State previousState_ = Session::State::Offline;
  int pressX_ = 0, pressY_ = 0, originalW_ = 0, originalH_ = 0;
  int originalX_ = 0, originalY_ = 0;
  enum class Gesture { Idle, Header, Resize, Remote, Menu, Focus, Mode, Connect };
  Gesture gesture_ = Gesture::Idle;
  bool dragged_ = false;
  bool remoteFocus_ = false;
};

class Dashboard : public Fl_Double_Window {
public:
  Dashboard(Library library, std::filesystem::path path)
    : Fl_Double_Window(library.layouts[activeLayout(library)].workspace.width,
                       library.layouts[activeLayout(library)].workspace.height, "SuperSmartClient - Panel workspace"),
      library_(std::move(library)), workspace_(library_.layouts[activeLayout(library_)].workspace), path_(std::move(path))
  {
    size_range(860, 600);
    begin();
    add_ = new Button(0, 0, 132, 38, "+ Add panel", true);
    connect_ = new Button(0, 0, 108, 34, "Connect all");
    disconnect_ = new Button(0, 0, 118, 34, "Disconnect all");
    back_ = new Button(0, 0, 112, 34, "Back to grid"); back_->hide();
    theme_ = new Button(0, 0, 100, 34, "Dark theme");
    layouts_ = new Choice(0, 0, 254, 32);
    libraryMenu_ = new Button(0, 0, 100, 34, "Layouts...");
    preset_ = new Choice(0, 0, 205, 27);
    preset_->add("Custom grid|1 - Single|2 - Side by side|2 - Stacked|3 - Top + two below|3 - Left + two right|4 - Grid 2 x 2|6 - Grid 3 x 2|9 - Grid 3 x 3|Free placement");
    preset_->value(static_cast<int>(workspace_.preset));
    preset_->tooltip("Choose a preset or place windows freely. Extra panels continue below the arrangement.");
    columns_ = new Choice(0, 0, 60, 32);
    columns_->add("1|2|3|4"); columns_->value(workspace_.columns - 1);
    height_ = new Choice(0, 0, 118, 32);
    height_->add("Fit window|Compact|Comfortable|Large");
    height_->value(heightChoice());
    scroll_ = new Fl_Scroll(24, 144, w() - 48, h() - 194);
    scroll_->type(Fl_Scroll::BOTH); scroll_->box(FL_NO_BOX);
    // Keep a stable canvas origin even when every freely placed tile has a
    // positive offset. Otherwise Fl_Scroll reports negative scroll positions.
    anchor_ = new Fl_Box(scroll_->x(), scroll_->y(), 1, 1); anchor_->box(FL_NO_BOX);
    scroll_->end();
    end();
    add_->callback([](Fl_Widget*, void* data) { static_cast<Dashboard*>(data)->edit(nullptr); }, this);
    connect_->callback([](Fl_Widget*, void* data) {
      auto& app = *static_cast<Dashboard*>(data);
      for (auto* tile : app.tiles_) if (!tile->session.wanted()) tile->session.start();
    }, this);
    disconnect_->callback([](Fl_Widget*, void* data) {
      for (auto* tile : static_cast<Dashboard*>(data)->tiles_) tile->session.stop();
    }, this);
    back_->callback([](Fl_Widget*, void* data) { static_cast<Dashboard*>(data)->focusTile(nullptr); }, this);
    theme_->callback([](Fl_Widget*, void* data) {
      auto& app = *static_cast<Dashboard*>(data);
      app.workspace_.dark = !app.workspace_.dark; app.applyTheme(); app.save();
    }, this);
    columns_->callback([](Fl_Widget*, void* data) {
      auto& app = *static_cast<Dashboard*>(data);
      app.workspace_.preset = Preset::CustomGrid; app.preset_->value(0);
      app.workspace_.columns = app.columns_->value() + 1;
      for (auto* tile : app.tiles_) tile->panel.fit = true;
      app.arrange(); app.save();
    }, this);
    height_->callback([](Fl_Widget*, void* data) {
      auto& app = *static_cast<Dashboard*>(data);
      const int sizes[] = {0, 200, 260, 380}; app.workspace_.rowHeight = sizes[app.height_->value()];
      app.arrange(); app.save();
    }, this);
    layouts_->callback([](Fl_Widget*, void* data) {
      auto& app = *static_cast<Dashboard*>(data);
      app.switchLayout(static_cast<size_t>(app.layouts_->value()));
    }, this);
    libraryMenu_->callback([](Fl_Widget*, void* data) { static_cast<Dashboard*>(data)->layoutMenu(); }, this);
    preset_->callback([](Fl_Widget*, void* data) {
      auto& app = *static_cast<Dashboard*>(data);
      app.choosePreset(static_cast<Preset>(app.preset_->value()));
    }, this);
    callback([](Fl_Widget*, void* data) {
      auto& app = *static_cast<Dashboard*>(data);
      if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
      if (!app.save()) return;
      for (auto* tile : app.tiles_) tile->session.stop();
      app.hide();
    }, this);
    for (const auto& panel : workspace_.panels) addTile(panel);
    updateLayouts(); applyTheme(); arrange();
    Fl::add_timeout(0.05, tick, this);
  }
  ~Dashboard() override { Fl::remove_timeout(tick, this); }
  void start() { for (auto* tile : tiles_) if (tile->panel.autoConnect) tile->session.start(); }
  Palette colors() const { return palette(workspace_.dark); }
  bool focused(const Tile* tile) const { return focused_ == tile; }
  bool selected(const Tile* tile) const { return selected_ == tile; }
  void select(Tile* tile) {
    selected_ = tile;
    if (freePlacement() && !focused_ && tiles_.back() != tile) {
      tiles_.erase(tiles_.begin() + index(tile)); tiles_.push_back(tile);
      scroll_->remove(tile); scroll_->add(tile); save();
    }
    scroll_->redraw();
  }
  void repaintTile() { if (freePlacement()) scroll_->redraw(); }
  void statusChanged() { redraw(); }
  void resolutionChanged() { if (!workspace_.rowHeight || freePlacement()) arrange(); }
  bool freePlacement() const { return workspace_.preset == Preset::Free; }
  void edit(Tile* tile) {
    if (!tile && tiles_.size() >= 32) { fl_alert("A workspace supports up to 32 panels."); return; }
    Panel profile = tile ? tile->panel : Panel{};
    if (!tile) { profile.id = newId(); profile.rememberPassword = passwordStorageAvailable(); }
    if (!tile && freePlacement() && !tiles_.empty()) {
      profile.pixelX = std::min(65535, tiles_.back()->panel.pixelX + 24);
      profile.pixelY = std::min(65535, tiles_.back()->panel.pixelY + 24);
    }
    Editor dialog(profile, !tile, colors());
    if (!dialog.run()) return;
    Workspace candidate = snapshot();
    if (tile) candidate.panels[index(tile)] = dialog.result;
    else candidate.panels.push_back(dialog.result);
    try { store(candidate); }
    catch (const std::exception& error) { fl_alert("Could not save connection: %s", error.what()); return; }
    bool restart = dialog.connectAfter || (tile && tile->session.wanted());
    if (tile) { tile->session.stop(); tile->panel = dialog.result; }
    else tile = addTile(dialog.result);
    if (!dialog.result.rememberPassword && !profile.credential.empty()) {
      try { forgetPassword(profile.id, profile.credential); }
      catch (const std::exception& error) { fl_alert("%s", error.what()); }
    }
    selected_ = tile; arrange();
    if (workspace_.preset != Preset::CustomGrid && workspace_.preset != Preset::Free)
      choosePreset(workspace_.preset);
    if (restart) tile->session.start();
  }
  void menu(Tile* tile) {
    // A popup is local to the header; none of its gestures reach the panel.
    Fl_Menu_Item items[] = {
      {"Edit connection...", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Reconnect", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Refresh picture", 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
      {"Move earlier", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Move later", 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
      {"Remove from workspace", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Size and scale...", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}
    };
    const Fl_Menu_Item* choice = items->popup(Fl::event_x(), Fl::event_y(), nullptr, nullptr, layouts_);
    if (!choice) return;
    switch (choice - items) {
      case 0: edit(tile); break;
      case 1: tile->session.start(); break;
      case 2: tile->session.refresh(); break;
      case 3: move(tile, index(tile) ? index(tile) - 1 : 0); break;
      case 4: move(tile, std::min(index(tile) + 1, tiles_.size() - 1)); break;
      case 5: removeTile(tile); break;
      case 6: viewSettings(tile); break;
    }
  }
  void focusTile(Tile* tile) {
    for (auto* item : tiles_) item->session.releaseInput();
    focused_ = focused_ == tile ? nullptr : tile;
    if (focused_) { back_->show(); columns_->deactivate(); height_->deactivate(); preset_->deactivate(); }
    else { back_->hide(); columns_->activate(); height_->activate(); preset_->activate(); }
    arrange();
  }
  void drop(Tile* source, int x, int y) {
    if (freePlacement()) { save(); return; }
    for (auto* target : tiles_) if (target != source && target->visible() &&
      x >= target->x() && x < target->x() + target->w() && y >= target->y() && y < target->y() + target->h()) {
      move(source, index(target)); return;
    }
  }
  void resizeTile(Tile* tile, int width, int height) {
    if (focused_) return;
    if (freePlacement()) {
      tile->panel.fit = true;
      tile->panel.pixelWidth = std::clamp(width, 144, 8192);
      tile->panel.pixelHeight = std::clamp(height, 100, 4320);
      arrange(); return;
    }
    workspace_.preset = Preset::CustomGrid; preset_->value(0);
    tile->panel.columns = std::clamp(static_cast<int>(std::round((width + gap) / static_cast<double>(cellWidth_ + gap))), 1, workspace_.columns);
    tile->panel.rows = std::clamp(static_cast<int>(std::round((height + gap) / static_cast<double>(cellHeight_ + gap))), 1, 4);
    arrange();
  }
  void moveFree(Tile* tile, int x, int y) {
    if (!freePlacement() || focused_) return;
    tile->panel.pixelX = std::clamp(x, 0, 65535); tile->panel.pixelY = std::clamp(y, 0, 65535);
    arrange();
  }
  bool save() {
    try { store(snapshot()); return true; }
    catch (const std::exception& error) { fl_alert("Could not save workspace: %s", error.what()); return false; }
  }
  void resize(int x, int y, int w, int h) override {
    Fl_Double_Window::resize(x, y, w, h);
    if (scroll_) arrange();
  }
  void draw() override {
    bool full = (damage() & ~FL_DAMAGE_CHILD) != 0;
    const auto p = colors();
    if (full) {
    fl_color(p.background); fl_rectf(0, 0, w(), h());
    fl_color(fl_rgb_color(19, 33, 53)); fl_rectf(0, 0, w(), 58);
    rounded(12, 14, 30, 30, fl_rgb_color(43, 119, 243), 7);
    fl_color(FL_WHITE);
    fl_rect(18, 20, 7, 7); fl_rect(29, 20, 7, 7);
    fl_rect(18, 31, 7, 7); fl_rect(29, 31, 7, 7);
    caption("SuperSmartClient", 52, 7, 245, 27, FL_WHITE, 20, true);
    caption("Unified panel workspace", 53, 33, 245, 17, fl_rgb_color(165, 187, 213), 10);
    caption("Layout", 299, 16, 45, 27, fl_rgb_color(165, 187, 213), 11);
    caption("Arrange", 10, 67, 48, 25, p.muted, 11);
    caption("Columns", 279, 67, 48, 25, p.muted, 11);
    caption("View", 385, 67, 32, 25, p.muted, 11);
    int live = 0;
    for (auto* tile : tiles_) live += tile->session.live();
    caption(std::to_string(live) + " / " + std::to_string(tiles_.size()) + " panels connected",
      9, h() - 19, 230, 17, p.muted, 10);
    caption(freePlacement() ? "Drag headers to move  /  Drag corners to resize  /  ... for size and scale" :
      "Drag headers to reorder  /  Drag corners to resize  /  ... for size and scale",
      245, h() - 19, w() - 254, 17, p.muted, 10, false, FL_ALIGN_RIGHT);
    }
    draw_children();
    if (full && tiles_.empty()) {
      rounded(w() / 2 - 34, h() / 2 - 88, 68, 68, p.soft, 16);
      caption("+", w() / 2 - 34, h() / 2 - 88, 68, 68, p.accent, 36, false, FL_ALIGN_CENTER);
      caption("Your panels, together", 30, h() / 2 - 4, w() - 60, 36, p.text, 25, true, FL_ALIGN_CENTER);
      caption("Add your first panel to build a workspace.", 30, h() / 2 + 39, w() - 60, 25, p.muted, 14, false, FL_ALIGN_CENTER);
      caption("Connections, passwords and layout are remembered on this PC.", 30, h() / 2 + 72, w() - 60, 24, p.muted, 12, false, FL_ALIGN_CENTER);
    }
  }
private:
  static constexpr int gap = 6;
  int heightChoice() const { return workspace_.rowHeight == 0 ? 0 : workspace_.rowHeight <= 220 ? 1 : workspace_.rowHeight <= 300 ? 2 : 3; }
  static void tick(void* data) {
    auto& app = *static_cast<Dashboard*>(data);
    // Schedule before processing so other sessions keep updating in certificate dialogs.
    Fl::repeat_timeout(0.04, tick, data);
    for (auto* tile : app.tiles_) tile->session.tick();
  }
  Tile* addTile(const Panel& panel) {
    Fl_Group* previous = Fl_Group::current();
    scroll_->begin();
    auto* tile = new Tile(*this, panel);
    scroll_->end(); Fl_Group::current(previous);
    tiles_.push_back(tile); return tile;
  }
  size_t index(Tile* tile) const { return std::find(tiles_.begin(), tiles_.end(), tile) - tiles_.begin(); }
  Workspace snapshot() const {
    Workspace result = workspace_; result.width = w(); result.height = h(); result.panels.clear();
    for (auto* tile : tiles_) result.panels.push_back(tile->panel);
    return result;
  }
  void store(const Workspace& workspace) {
    Library candidate = library_;
    candidate.layouts[activeLayout(candidate)].workspace = workspace;
    saveLibrary(path_, candidate);
    library_ = std::move(candidate);
  }
  void updateLayouts() {
    layouts_->clear();
    for (size_t number = 0; number < library_.layouts.size(); ++number) {
      layouts_->add("Layout"); layouts_->replace(static_cast<int>(number), library_.layouts[number].name.c_str());
    }
    layouts_->value(static_cast<int>(activeLayout(library_)));
  }
  void switchLayout(size_t number, bool connect = true) {
    if (number >= library_.layouts.size()) return;
    if (!save()) { updateLayouts(); return; }
    Library candidate = library_; candidate.active = candidate.layouts[number].id;
    try { saveLibrary(path_, candidate); }
    catch (const std::exception& error) { fl_alert("%s", error.what()); updateLayouts(); return; }
    loadLayout(std::move(candidate), connect);
  }
  void loadLayout(Library candidate, bool connect) {
    for (auto* tile : tiles_) {
      tile->session.stop(); scroll_->remove(tile); Fl::delete_widget(tile);
    }
    tiles_.clear(); selected_ = nullptr; focused_ = nullptr;
    back_->hide(); columns_->activate(); height_->activate(); preset_->activate();
    library_ = std::move(candidate);
    workspace_ = library_.layouts[activeLayout(library_)].workspace;
    columns_->value(workspace_.columns - 1);
    height_->value(heightChoice());
    preset_->value(static_cast<int>(workspace_.preset));
    for (const auto& panel : workspace_.panels) addTile(panel);
    updateLayouts(); applyTheme(); arrange();
    if (connect) for (auto* tile : tiles_) tile->session.start();
  }
  void choosePreset(Preset preset) {
    if (focused_) focusTile(nullptr);
    if (preset == Preset::Free && !freePlacement()) {
      for (auto* tile : tiles_) {
        tile->panel.pixelX = tile->x() - scroll_->x() + scroll_->xposition();
        tile->panel.pixelY = tile->y() - scroll_->y() + scroll_->yposition();
        tile->panel.pixelWidth = tile->w(); tile->panel.pixelHeight = tile->h();
      }
    }
    Workspace candidate = snapshot(); applyPreset(candidate, preset);
    workspace_.preset = preset; workspace_.columns = candidate.columns; workspace_.rowHeight = candidate.rowHeight;
    for (size_t number = 0; number < tiles_.size(); ++number) tiles_[number]->panel = candidate.panels[number];
    if (preset == Preset::CustomGrid) for (auto* tile : tiles_) tile->panel.fit = true;
    preset_->value(static_cast<int>(preset)); columns_->value(workspace_.columns - 1); height_->value(heightChoice());
    arrange(); save();
  }
  void viewSettings(Tile* tile) {
    ViewDialog dialog(tile->panel, tile->session.width(), tile->session.height(), colors()); if (!dialog.run()) return;
    if (!dialog.result.fit && !freePlacement()) choosePreset(Preset::Free);
    tile->panel.displayWidth = dialog.result.displayWidth; tile->panel.displayHeight = dialog.result.displayHeight;
    tile->panel.displayPreset = dialog.result.displayPreset;
    tile->panel.scale = dialog.result.scale; tile->panel.fit = dialog.result.fit;
    arrange(); save();
  }
  void layoutMenu() {
    Fl_Menu_Item items[] = {
      {"New empty layout...", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Save layout as...", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Rename layout...", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Delete layout...", 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
      {"Export database...", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Import database...", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Connect all", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Disconnect all", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}
    };
    const auto* choice = items->pulldown(libraryMenu_->x(), libraryMenu_->y(), libraryMenu_->w(), libraryMenu_->h(), nullptr, layouts_);
    if (!choice || !save()) return;
    try {
      size_t number = activeLayout(library_);
      Library candidate = library_;
      int action = static_cast<int>(choice - items);
      if (action <= 2) {
        const char* value = fl_input("Layout name:", action == 2 ? candidate.layouts[number].name.c_str() : "");
        if (!value) return;
        std::string name(value); validateLayoutName(name);
        if (action == 0) { candidate.layouts.push_back({newId(), name, {}}); candidate.active = candidate.layouts.back().id; }
        else if (action == 1) {
          candidate.layouts.push_back(duplicateLayout(candidate.layouts[number], name));
          candidate.active = candidate.layouts.back().id;
        } else candidate.layouts[number].name = name;
        saveLibrary(path_, candidate);
        if (action == 2) { library_ = std::move(candidate); updateLayouts(); }
        else loadLayout(std::move(candidate), action == 1);
      } else if (action == 3) {
        if (candidate.layouts.size() == 1) { fl_alert("Keep at least one layout. Create another layout first."); return; }
        if (!fl_choice("Delete layout '%s' and its saved connections?", "Cancel", "Delete", nullptr,
                       candidate.layouts[number].name.c_str())) return;
        auto removed = candidate.layouts[number];
        candidate.layouts.erase(candidate.layouts.begin() + number); candidate.active = candidate.layouts.front().id;
        saveLibrary(path_, candidate); loadLayout(std::move(candidate), false);
        for (const auto& panel : removed.workspace.panels) forgetPassword(panel.id, panel.credential);
      } else if (action == 4) {
        ExportDialog dialog(colors()); if (!dialog.run()) return;
        Fl_Native_File_Chooser file(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
        file.title("Export SuperSmartClient database"); file.filter("SuperSmartClient database\t*.sscdb");
        file.preset_file("SuperSmartClient.sscdb"); file.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
        if (file.show()) return;
        auto path = std::filesystem::u8path(file.filename());
        if (std::filesystem::exists(path) && std::filesystem::equivalent(path, path_))
          throw std::runtime_error("Choose an export file separate from the active application database");
        exportLibrary(path, library_, dialog.password);
        fl_message("Database exported: %d layouts.\n%s", static_cast<int>(library_.layouts.size()),
          dialog.password.empty() ? "Saved passwords were omitted." : "Saved passwords are protected by your export password.");
      } else if (action == 5) {
        Fl_Native_File_Chooser file(Fl_Native_File_Chooser::BROWSE_FILE);
        file.title("Import SuperSmartClient database"); file.filter("SuperSmartClient database\t*.{sscdb,db}");
        if (file.show()) return;
        auto path = std::filesystem::u8path(file.filename());
        std::string password;
        if (encryptedExport(path)) {
          const char* value = fl_password("Export password:", ""); if (!value) return; password = value;
        }
        auto imported = importLibrary(path, password); size_t count = imported.layouts.size();
        mergeLibrary(candidate, imported); saveLibrary(path_, candidate);
        library_ = std::move(candidate); updateLayouts();
        fl_message("Imported %d layouts. Choose one from the Layout dropdown to connect.", static_cast<int>(count));
      } else {
        for (auto* tile : tiles_) {
          if (action == 6) tile->session.start(); else tile->session.stop();
        }
      }
    } catch (const std::exception& error) { fl_alert("%s", error.what()); }
  }
  void applyTheme() {
    const auto p = colors(); color(p.background); scroll_->color(p.background);
    Fl::set_color(FL_BACKGROUND_COLOR, p.background); Fl::set_color(FL_BACKGROUND2_COLOR, p.card);
    Fl::set_color(FL_FOREGROUND_COLOR, p.text); Fl::set_color(FL_INACTIVE_COLOR, p.muted);
    Fl::set_color(FL_SELECTION_COLOR, fl_rgb_color(37, 99, 235));
    Fl::set_color(FL_LIGHT3, p.border); Fl::set_color(FL_DARK3, p.border);
    // FLTK's shared name/password/certificate dialogs use the standard boxes.
    Fl::set_boxtype(FL_UP_BOX, dialogButtonBox, 2, 2, 4, 4);
    Fl::set_boxtype(FL_DOWN_BOX, dialogButtonBox, 2, 2, 4, 4);
    for (auto* button : {add_, connect_, disconnect_, libraryMenu_}) button->theme(p, fl_rgb_color(19, 33, 53));
    for (auto* button : {back_, theme_}) button->theme(p, p.background);
    theme_->label(workspace_.dark ? "Light theme" : "Dark theme");
    for (auto* choice : {columns_, height_, preset_}) choice->theme(p, p.background);
    layouts_->theme(p, fl_rgb_color(19, 33, 53));
    redraw();
  }
  void arrange() {
    if (!focused_) {
      if (freePlacement()) { columns_->deactivate(); height_->deactivate(); }
      else { columns_->activate(); height_->activate(); }
    }
    add_->resize(w() - 122, 13, 110, 32);
    layouts_->resize(348, 16, 214, 27); libraryMenu_->resize(572, 13, 96, 32);
    preset_->resize(62, 66, 205, 27); columns_->resize(329, 66, 44, 27); height_->resize(425, 66, 164, 27);
    connect_->resize(w() - 342, 13, 96, 32); disconnect_->resize(w() - 238, 13, 108, 32);
    if (w() >= 1024) { connect_->show(); disconnect_->show(); }
    else { connect_->hide(); disconnect_->hide(); }
    back_->resize(603, 65, 108, 29); theme_->resize(w() - 108, 65, 100, 29);
    int oldX = std::max(0, scroll_->xposition()), oldY = std::max(0, scroll_->yposition());
    scroll_->scroll_to(0, 0); scroll_->resize(8, 104, w() - 16, h() - 127);
    anchor_->resize(scroll_->x(), scroll_->y(), 1, 1);
    auto positions = layout(snapshot().panels, workspace_.columns);
    int rows = 1;
    for (const auto& position : positions) rows = std::max(rows, position.row + position.rows);
    int available = scroll_->w();
    // Reserve scrollbar width only when a fixed-height grid needs it.
    if (workspace_.rowHeight && rows * (workspace_.rowHeight + gap) - gap > scroll_->h())
      available -= Fl::scrollbar_size();
    cellWidth_ = std::max(230, (available - (workspace_.columns - 1) * gap) / workspace_.columns);
    cellHeight_ = workspace_.rowHeight;
    if (!cellHeight_) {
      int natural = static_cast<int>(cellWidth_ * 9.0 / 16) + 51;
      for (const auto* tile : tiles_) if (tile->session.width())
        natural = std::max(natural, (cellWidth_ * std::min(tile->panel.columns, workspace_.columns) *
          tile->session.height() / tile->session.width() + 51) / tile->panel.rows);
      cellHeight_ = std::max(140, std::min(natural, (scroll_->h() - (rows - 1) * gap) / rows));
      if (rows * (cellHeight_ + gap) - gap > scroll_->h()) {
        available -= Fl::scrollbar_size();
        cellWidth_ = std::max(230, (available - (workspace_.columns - 1) * gap) / workspace_.columns);
      }
    }
    for (size_t number = 0; number < tiles_.size(); ++number) {
      auto* tile = tiles_[number];
      if (focused_ && focused_ != tile) { tile->hide(); continue; }
      tile->show();
      if (focused_) tile->resize(scroll_->x(), scroll_->y(), available,
        scroll_->h());
      else if (freePlacement()) {
        if (!tile->panel.fit) {
          auto size = scaledDisplaySize(tile->panel, tile->session.width(), tile->session.height());
          tile->panel.pixelWidth = std::max(144, size.first + 4);
          tile->panel.pixelHeight = std::max(100, size.second + 51);
        }
        tile->resize(scroll_->x() + tile->panel.pixelX, scroll_->y() + tile->panel.pixelY,
          tile->panel.pixelWidth, tile->panel.pixelHeight);
      }
      else {
        const auto& p = positions[number];
        tile->resize(scroll_->x() + p.column * (cellWidth_ + gap),
          scroll_->y() + p.row * (cellHeight_ + gap),
          p.columns * (cellWidth_ + gap) - gap,
          p.rows * (cellHeight_ + gap) - gap);
      }
    }
    if (freePlacement()) scroll_->scroll_to(oldX, oldY);
    redraw();
  }
  void move(Tile* tile, size_t to) {
    if (focused_) return;
    tiles_.erase(tiles_.begin() + index(tile));
    tiles_.insert(tiles_.begin() + to, tile);
    if (freePlacement()) for (auto* item : tiles_) { scroll_->remove(item); scroll_->add(item); }
    if (workspace_.preset != Preset::CustomGrid && !freePlacement()) choosePreset(workspace_.preset);
    else { arrange(); save(); }
  }
  void removeTile(Tile* tile) {
    // This affects the saved connection only. The panel itself is unchanged.
    Workspace candidate = snapshot();
    candidate.panels.erase(candidate.panels.begin() + index(tile));
    try { store(candidate); }
    catch (const std::exception& error) { fl_alert("%s", error.what()); return; }
    tile->session.stop();
    try { forgetPassword(tile->panel.id, tile->panel.credential); }
    catch (const std::exception& error) { fl_alert("%s", error.what()); }
    if (focused_ == tile) focusTile(nullptr);
    if (selected_ == tile) selected_ = nullptr;
    tiles_.erase(tiles_.begin() + index(tile));
    scroll_->remove(tile); Fl::delete_widget(tile);
    if (workspace_.preset != Preset::CustomGrid && !freePlacement()) choosePreset(workspace_.preset);
    else arrange();
  }
  Library library_;
  Workspace workspace_;
  std::filesystem::path path_;
  std::vector<Tile*> tiles_;
  Tile *focused_ = nullptr, *selected_ = nullptr;
  Fl_Scroll* scroll_ = nullptr;
  Fl_Box* anchor_;
  Button *add_, *connect_, *disconnect_, *back_, *theme_, *libraryMenu_;
  Choice *columns_, *height_, *layouts_, *preset_;
  int cellWidth_ = 400, cellHeight_ = 260;
};

Tile::Tile(Dashboard& owner, Panel profile)
  : Fl_Widget(0, 0, 300, 260), panel(std::move(profile)),
    session(panel, [this] { changed(); }), owner_(owner)
{
  tooltip("Drag the header to arrange. Drag the bottom-right corner to resize. Use ... for size and scale.");
}
void Tile::changed() {
  if (serverWidth_ != session.width() || serverHeight_ != session.height()) {
    serverWidth_ = session.width(); serverHeight_ = session.height();
    owner_.resolutionChanged();
  }
  redraw();
  owner_.repaintTile();
  if (previousState_ != session.status()) {
    previousState_ = session.status(); owner_.statusChanged();
  }
}
void Tile::resize(int x, int y, int w, int h) {
  Fl_Widget::resize(x, y, w, h); image_.reset();
}
void Tile::imageRect(int& left, int& top, int& width, int& height) const {
  left = x() + 2; top = y() + 31; width = w() - 4; height = h() - 51;
  if (!panel.fit && !owner_.focused(this)) {
    auto size = scaledDisplaySize(panel, session.width(), session.height());
    int fixedW = std::min(width, size.first), fixedH = std::min(height, size.second);
    left += (width - fixedW) / 2; top += (height - fixedH) / 2;
    width = fixedW; height = fixedH;
  }
  if (!session.width() || !session.height()) return;
  double scale = std::min(width / static_cast<double>(session.width()),
                          height / static_cast<double>(session.height()));
  int scaledW = std::max(1, static_cast<int>(session.width() * scale));
  int scaledH = std::max(1, static_cast<int>(session.height() * scale));
  left += (width - scaledW) / 2; top += (height - scaledH) / 2;
  width = scaledW; height = scaledH;
}
void Tile::draw() {
  const auto p = owner_.colors();
  rounded(x(), y(), w(), h(), owner_.selected(this) ? p.accent : p.border, 4);
  rounded(x() + 1, y() + 1, w() - 2, h() - 2, p.card, 3);
  for (int dy = 0; dy < 3; ++dy) for (int dx = 0; dx < 2; ++dx) {
    fl_color(p.muted); fl_rectf(x() + 5 + dx * 3, y() + 10 + dy * 4, 1, 1);
  }
  int nameWidth = w() - 143;
  if (w() >= 540) nameWidth -= 180;
  caption(panel.name, x() + 16, y() + 3, nameWidth, 24, p.text, 12, true);
  if (w() >= 540) caption(panel.address, x() + 20 + nameWidth, y() + 3, 175, 24, p.muted, 10);
  rounded(x() + w() - 120, y() + 5, 60, 21, p.soft, 4);
  caption(panel.viewOnly ? "Monitor" : "Control", x() + w() - 120, y() + 5,
    60, 21, p.accent, 10, true, FL_ALIGN_CENTER);
  caption(owner_.focused(this) ? "<>" : "[ ]", x() + w() - 55, y() + 4, 22, 23, p.muted, 12, false, FL_ALIGN_CENTER);
  caption("...", x() + w() - 29, y() + 1, 23, 23, p.muted, 17, true, FL_ALIGN_CENTER);
  fl_color(p.border); fl_line(x() + 2, y() + 30, x() + w() - 3, y() + 30);
  fl_color(fl_rgb_color(13, 22, 35)); fl_rectf(x() + 2, y() + 31, w() - 4, h() - 51);
  if (session.live() && !session.pixels().empty()) {
    int left, top, width, height; imageRect(left, top, width, height);
    if (!image_ || generation_ != session.generation() || imageWidth_ != width || imageHeight_ != height) {
      Fl_RGB_Image original(session.pixels().data(), session.width(), session.height(), 3);
      image_.reset(original.copy(width, height));
      generation_ = session.generation(); imageWidth_ = width; imageHeight_ = height;
    }
    image_->draw(left, top);
  } else {
    caption(session.statusText(), x() + 16, y() + 39, w() - 32, (h() - 61) / 2,
      fl_rgb_color(219, 229, 244), 16, true, FL_ALIGN_CENTER);
    caption(session.error().empty() ? (session.wanted() ? "Waiting for the panel" : "Use Connect to open this panel") : session.error(),
      x() + 16, y() + 39 + (h() - 61) / 2, w() - 32, (h() - 61) / 2 - 6,
      fl_rgb_color(148, 163, 184), 12, false, FL_ALIGN_CENTER | FL_ALIGN_WRAP);
  }
  Fl_Color status = session.live() ? p.green : session.status() == Session::State::Offline ? p.muted : p.amber;
  fl_color(status); fl_pie(x() + 7, y() + h() - 13, 5, 5, 0, 360);
  std::string statusText = session.statusText();
  if (!panel.fit) statusText += "  /  " + std::to_string(panel.scale) + "%";
  caption(statusText, x() + 17, y() + h() - 20, w() - 117, 19, status, 10);
  caption(session.wanted() ? "Disconnect" : "Connect", x() + w() - 90, y() + h() - 20, 74, 19,
    p.accent, 10, true, FL_ALIGN_CENTER);
  fl_color(p.muted);
  for (int i = 0; i < 3; ++i) fl_line(x() + w() - 7 - i * 4, y() + h() - 7,
    x() + w() - 7, y() + h() - 7 - i * 4);
}
unsigned Tile::mouseMask() const {
  int state = Fl::event_state();
  return (state & FL_BUTTON1 ? 1 : 0) | (state & FL_BUTTON2 ? 2 : 0) | (state & FL_BUTTON3 ? 4 : 0);
}
void Tile::sendPointer(unsigned mask) {
  int left, top, width, height; imageRect(left, top, width, height);
  session.pointer(static_cast<int>((Fl::event_x() - left) * static_cast<double>(session.width()) / width),
    static_cast<int>((Fl::event_y() - top) * static_cast<double>(session.height()) / height), mask);
}
int Tile::handle(int event) {
  switch (event) {
    case FL_FOCUS: redraw(); return 1;
    case FL_UNFOCUS: remoteFocus_ = false; session.releaseInput(); redraw(); return 1;
    case FL_PUSH: {
      take_focus(); owner_.select(this);
      remoteFocus_ = false;
      pressX_ = Fl::event_x(); pressY_ = Fl::event_y(); originalW_ = w(); originalH_ = h(); dragged_ = false;
      originalX_ = panel.pixelX; originalY_ = panel.pixelY;
      if (hitRect(x() + w() - 15, y() + h() - 15, 15, 15)) gesture_ = Gesture::Resize;
      else if (hitRect(x() + w() - 90, y() + h() - 20, 74, 20)) gesture_ = Gesture::Connect;
      else if (hitRect(x(), y(), w(), 31)) {
        session.releaseInput();
        if (hitRect(x() + w() - 29, y(), 25, 31)) gesture_ = Gesture::Menu;
        else if (hitRect(x() + w() - 55, y(), 22, 31)) gesture_ = Gesture::Focus;
        else if (hitRect(x() + w() - 120, y(), 60, 31)) gesture_ = Gesture::Mode;
        else gesture_ = Gesture::Header;
      } else {
        int left, top, width, height; imageRect(left, top, width, height);
        if (hitRect(left, top, width, height) && session.live()) {
          remoteFocus_ = true; gesture_ = Gesture::Remote; sendPointer(mouseMask());
        }
        else gesture_ = Gesture::Idle;
      }
      if (gesture_ != Gesture::Remote) session.releaseInput();
      return 1;
    }
    case FL_DRAG:
      dragged_ = dragged_ || std::abs(Fl::event_x() - pressX_) + std::abs(Fl::event_y() - pressY_) > 6;
      if (gesture_ == Gesture::Resize) owner_.resizeTile(this, originalW_ + Fl::event_x() - pressX_, originalH_ + Fl::event_y() - pressY_);
      else if (gesture_ == Gesture::Header && owner_.freePlacement())
        owner_.moveFree(this, originalX_ + Fl::event_x() - pressX_, originalY_ + Fl::event_y() - pressY_);
      else if (gesture_ == Gesture::Remote) sendPointer(mouseMask());
      return 1;
    case FL_RELEASE: {
      Gesture action = gesture_; gesture_ = Gesture::Idle;
      if (action == Gesture::Remote) sendPointer(mouseMask());
      else if (action == Gesture::Resize) owner_.save();
      else if (action == Gesture::Header) {
        if (dragged_) owner_.drop(this, Fl::event_x(), Fl::event_y());
        else if (Fl::event_clicks()) owner_.focusTile(this);
      } else if (!dragged_) {
        if (action == Gesture::Menu) owner_.menu(this);
        else if (action == Gesture::Focus) owner_.focusTile(this);
        else if (action == Gesture::Mode) { session.setViewOnly(!panel.viewOnly); owner_.save(); }
        else if (action == Gesture::Connect) { if (session.wanted()) session.stop(); else session.start(); }
      }
      return 1;
    }
    case FL_MOVE: {
      int left, top, width, height; imageRect(left, top, width, height);
      if (hitRect(left, top, width, height) && Fl::focus() == this && remoteFocus_) sendPointer(0);
      return 1;
    }
    case FL_MOUSEWHEEL: {
      int left, top, width, height; imageRect(left, top, width, height);
      if (!hitRect(left, top, width, height) || Fl::focus() != this || !remoteFocus_ || panel.viewOnly) return 0;
      int dy = std::clamp(Fl::event_dy(), -10, 10), dx = std::clamp(Fl::event_dx(), -10, 10);
      for (int i = 0; i < std::abs(dy); ++i) { sendPointer(mouseMask() | (dy < 0 ? 8 : 16)); sendPointer(mouseMask()); }
      for (int i = 0; i < std::abs(dx); ++i) { sendPointer(mouseMask() | (dx < 0 ? 32 : 64)); sendPointer(mouseMask()); }
      return 1;
    }
    case FL_KEYDOWN:
    case FL_KEYUP: {
      if (!remoteFocus_) return 0;
      unsigned key = Fl::event_key();
      if (event == FL_KEYDOWN && Fl::event_length() && key < FL_BackSpace &&
          !(Fl::event_state() & (FL_CTRL | FL_ALT | FL_META))) {
        unsigned codepoint = 0;
        core::utf8ToUCS4(Fl::event_text(), Fl::event_length(), &codepoint);
        if (codepoint) key = ucs2keysym(codepoint);
      }
      if (key == FL_Enter) key = 0xff0d;
      session.key(event == FL_KEYDOWN, Fl::event_key(), key); return 1;
    }
  }
  return Fl_Widget::handle(event);
}
}
}

int runDashboard(const char* configPath)
{
  try {
    auto path = configPath && configPath[0] ? std::filesystem::u8path(configPath) : dashboard::defaultWorkspacePath();
    auto library = dashboard::loadLibrary(path);
    dashboard::Dashboard window(std::move(library), path);
    window.show(); window.start();
    return Fl::run();
  } catch (const std::exception& error) {
    fl_alert("Cannot open the panel workspace: %s", error.what());
    return 1;
  }
}
