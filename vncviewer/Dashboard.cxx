/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include "DashboardModel.h"
#include "DashboardStore.h"
#include "DashboardSession.h"
#include "DashboardImage.h"
#include "AppUpdate.h"
#include "parameters.h"
#include "keysym2ucs.h"
#include "fltk/event_dispatch_handler.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <set>
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
#include <FL/filename.H>

namespace dashboard {
namespace {
constexpr int frameWidth = 2, frameHeight = 26, toolbarHeight = 40;
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
    if (!active_r()) fill = colors_.soft;
    else if (value()) fill = fl_color_average(fill, colors_.text, 0.85f);
    else if (hover_) fill = fl_color_average(fill, primary_ ? FL_WHITE : colors_.accent, 0.92f);
    rounded(x(), y(), w(), h(), active_r() && Fl::focus() == this ? colors_.accent : primary_ && active_r() ? fill : colors_.border, 9);
    rounded(x() + 1, y() + 1, w() - 2, h() - 2, fill, 8);
    caption(label(), x() + 6, y(), w() - 12, h(), !active_r() ? colors_.muted : primary_ ? FL_WHITE : colors_.text,
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

class Checkbox : public Fl_Check_Button {
public:
  using Fl_Check_Button::Fl_Check_Button;
  void theme(const Palette& colors) {
    colors_ = colors; color(colors.background); labelcolor(colors.text); redraw();
  }
  void draw() override {
    fl_color(color()); fl_rectf(x(), y(), w(), h());
    const int left = x() + 2, top = y() + (h() - 18) / 2;
    Fl_Color blue = fl_rgb_color(37, 99, 235);
    Fl_Color fill = value() ? (active_r() ? blue : colors_.muted) : colors_.card;
    if (Fl::focus() == this && active_r()) {
      rounded(left - 2, top - 2, 22, 22, colors_.accent, 6);
      rounded(left - 1, top - 1, 20, 20, color(), 5);
    }
    rounded(left, top, 18, 18, value() ? fill : hover_ ? colors_.accent : colors_.muted, 4);
    rounded(left + 1, top + 1, 16, 16, fill, 3);
    if (value()) {
      fl_color(FL_WHITE);
      fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, 2);
      fl_line(left + 4, top + 9, left + 7, top + 12, left + 14, top + 5);
      fl_line_style(0);
    }
    draw_label(x() + 27, y(), w() - 27, h());
  }
  int handle(int event) override {
    if (event == FL_ENTER || event == FL_LEAVE) { hover_ = event == FL_ENTER; redraw(); }
    return Fl_Check_Button::handle(event);
  }
private:
  Palette colors_ = palette(false);
  bool hover_ = false;
};

// The popup geometry FLTK 1.3 will use for these items, which it does not expose.
struct MenuSize { int width, height, itemHeight, selected, border; };
MenuSize measureMenu(const Fl_Menu_Item* items, const Fl_Menu_* owner, const Fl_Menu_Item* current = nullptr) {
  const int leading = 4;
  MenuSize size{0, 0, 1, 0, 0};
  int count = 0;
  for (const Fl_Menu_Item* item = items->first(); item && item->text; item = item->next(), ++count) {
    int height, width = item->measure(&height, owner);
    size.itemHeight = std::max(size.itemHeight, height + leading);
    size.width = std::max(size.width, width);
    if (item == current) size.selected = count;
  }
  Fl_Boxtype frame = owner->box() == FL_NO_BOX || owner->box() == FL_FLAT_BOX ? FL_UP_BOX : owner->box();
  size.border = Fl::box_dx(frame);
  size.width += 2 * size.border + 7;
  size.height = (count ? size.itemHeight * count - leading : 0) + 2 * size.border + 3;
  return size;
}

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
  int handle(int event) override {
    bool open = event == FL_PUSH ||
      (event == FL_KEYBOARD && Fl::event_key() == ' ' &&
       !(Fl::event_state() & (FL_SHIFT | FL_CTRL | FL_ALT | FL_META))) ||
      (event == FL_SHORTCUT && Fl_Widget::test_shortcut());
    if (!open || !menu() || !menu()->text) return Fl_Choice::handle(event);
    if (event == FL_PUSH && Fl::visible_focus()) Fl::focus(this);
    dropdown();
    return 1;
  }
private:
  // FLTK opens the list with the current item over the field and never keeps
  // that placement on screen, so a field near a screen edge gets a list that
  // starts outside it and scrolls back. Open below the field, or above it when
  // there is no room, and keep the current item highlighted for the keyboard.
  //
  // FLTK clears the highlight on any pointer movement outside the list, and the
  // pointer now starts on the field rather than on the current item. Ignore
  // that movement so Up and Down still continue from the current item.
  static int keepHighlight(int event, Fl_Window*, void*) {
    if (event != FL_MOVE && event != FL_ENTER && event != FL_DRAG) return 0;
    const Fl_Window* list = Fl::grab();
    if (!list) return 0;
    int mouseX = Fl::event_x_root(), mouseY = Fl::event_y_root();
    return mouseX < list->x() || mouseX >= list->x() + list->w() ||
           mouseY < list->y() || mouseY >= list->y() + list->h();
  }
  void dropdown() {
    const Fl_Menu_Item* current = mvalue();
    int top = y();
    if (current) {
      MenuSize size = measureMenu(menu(), this, current);
      int originX = 0, origin = 0;
      for (Fl_Window* parent = window(); parent; parent = parent->window()) {
        originX += parent->x(); origin += parent->y();
      }
      int screenX, screenY, screenW, screenH;
      Fl::screen_work_area(screenX, screenY, screenW, screenH, originX + x(), origin + y());
      int wanted = origin + y() + h();
      if (wanted + size.height > screenY + screenH) {
        int above = origin + y() - size.height;
        wanted = above >= screenY ? above : std::max(screenY, screenY + screenH - size.height);
      }
      // Undo the offset FLTK applies for the highlighted item.
      top = wanted - origin - (h() - size.itemHeight) / 2 + size.selected * size.itemHeight + size.border;
    }
    Fl_Widget_Tracker tracker(this);
    fl_add_event_dispatch(keepHighlight, this);
    const Fl_Menu_Item* choice = menu()->pulldown(x(), top, w(), h(), current, this);
    fl_remove_event_dispatch(keepHighlight, this);
    if (!choice || choice->submenu() || tracker.deleted()) return;
    if (choice != mvalue()) redraw();
    picked(choice);
  }
  Palette colors_ = palette(false);
  Fl_Color background_ = palette(false).background;
};

void styleDialog(Fl_Group& dialog, const Palette& colors) {
  dialog.color(colors.background);
  for (int i = 0; i < dialog.children(); ++i) {
    auto* widget = dialog.child(i); widget->labelcolor(colors.text);
    if (auto* button = dynamic_cast<Button*>(widget)) button->theme(colors, colors.background);
    else if (auto* check = dynamic_cast<Checkbox*>(widget)) check->theme(colors);
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
    remember_ = new Checkbox(28, 352, 484, 25, "Remember password in my account");
    remember_->value(panel.rememberPassword && passwordStorageAvailable());
    if (!passwordStorageAvailable()) {
      remember_->deactivate(); remember_->label("Password storage unavailable in this build");
    }
    reconnect_ = new Checkbox(28, 383, 250, 25, "Reconnect automatically");
    reconnect_->value(panel.reconnect);
    startup_ = new Checkbox(282, 383, 230, 25, "Connect on startup");
    startup_->value(panel.autoConnect);
    monitor_ = new Checkbox(28, 414, 484, 25, "Monitor only (disable mouse and keyboard)");
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
    include_ = new Checkbox(24, 23, 452, 30, "Include saved passwords");
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
    fit_ = new Checkbox(153, 123, 323, 32, "Fit picture to the window"); fit_->value(panel.fit);
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

class UpdateDialog : public Fl_Double_Window {
public:
  UpdateDialog(AppUpdate& updater, const Palette& colors)
    : Fl_Double_Window(560, 302, "SuperSmartClient updates"), updater_(updater)
  {
    begin();
    auto* heading = new Fl_Box(24, 18, 512, 34, "SuperSmartClient " SUPERSMART_VERSION);
    heading->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE); heading->labelfont(FL_HELVETICA_BOLD); heading->labelsize(22);
    status_ = new Fl_Box(24, 62, 512, 95);
    status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP); status_->labelsize(14);
    automatic_ = new Checkbox(24, 168, 512, 26, "Check for updates automatically (once a day)");
    automatic_->value(updater_.automatic());
    if (!updater_.supported()) automatic_->deactivate();
    automatic_->callback([](Fl_Widget*, void* data) {
      auto& dialog = *static_cast<UpdateDialog*>(data);
      try { dialog.updater_.automatic(dialog.automatic_->value()); }
      catch (const std::exception& error) { fl_alert("%s", error.what()); dialog.automatic_->value(dialog.updater_.automatic()); }
    }, this);
    auto* releases = new Button(24, 211, 100, 32, "Releases");
    releases->callback([](Fl_Widget*, void*) {
      char error[256];
      if (!fl_open_uri("https://github.com/bendikme/SuperSmartClient/releases", error, sizeof(error))) fl_alert("%s", error);
    });
    check_ = new Button(132, 211, 120, 32, "Check now");
    check_->callback([](Fl_Widget*, void* data) { static_cast<UpdateDialog*>(data)->updater_.check(); }, this);
    action_ = new Button(300, 211, 236, 32, "Download update", true);
    action_->callback([](Fl_Widget*, void* data) {
      auto& dialog = *static_cast<UpdateDialog*>(data);
      if (dialog.updater_.state() == AppUpdate::State::Ready) { dialog.install_ = true; dialog.hide(); }
      else dialog.updater_.download();
    }, this);
    auto* close = new Button(436, 254, 100, 30, "Close");
    close->callback([](Fl_Widget*, void* data) { static_cast<UpdateDialog*>(data)->hide(); }, this);
    callback([](Fl_Widget* widget, void*) { widget->hide(); });
    end(); styleDialog(*this, colors); set_modal(); refresh();
  }
  bool run() {
    show();
    while (shown()) { Fl::wait(0.1); updater_.poll(); refresh(); }
    return install_;
  }
private:
  void refresh() {
    if (previous_ == updater_.state()) return;
    previous_ = updater_.state(); status_->copy_label(updater_.message().c_str());
    if (updater_.supported() && !updater_.busy()) check_->activate(); else check_->deactivate();
    if (updater_.available()) action_->activate(); else action_->deactivate();
    action_->copy_label(updater_.state() == AppUpdate::State::Ready ? "Install and restart" : "Download update");
  }
  AppUpdate& updater_;
  AppUpdate::State previous_ = AppUpdate::State::Installing;
  Fl_Box* status_;
  Checkbox* automatic_;
  Button *check_, *action_;
  bool install_ = false;
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
  void releaseInput() { remoteFocus_ = false; session.releaseInput(); }
  Panel panel;
  Session session;
private:
  void imageRect(int& x, int& y, int& w, int& h) const;
  void sendPointer(unsigned mask);
  unsigned mouseMask() const;
  Dashboard& owner_;
  ImageScaler scaler_;
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

class Divider : public Fl_Widget {
public:
  Divider(Dashboard& owner) : Fl_Widget(0, 0, 1, 1), owner_(owner) {}
  void configure(bool vertical, int boundary) { vertical_ = vertical; boundary_ = boundary; }
  void draw() override;
  int handle(int event) override;
private:
  Dashboard& owner_;
  bool vertical_ = true, hovering_ = false, dragging_ = false;
  int boundary_ = 0, press_ = 0;
  std::vector<int> initial_;
};

class Dashboard : public Fl_Double_Window {
public:
  Dashboard(Library library, std::filesystem::path path)
    : Fl_Double_Window(library.layouts[activeLayout(library)].workspace.width,
                       library.layouts[activeLayout(library)].workspace.height, "SuperSmartClient - Panel workspace"),
      library_(std::move(library)), workspace_(library_.layouts[activeLayout(library_)].workspace), path_(std::move(path)),
      updater_(path_, checkUpdates)
  {
    Session::acceptUnknownCertificates(library_.acceptUnknownCertificates);
    size_range(860, 600);
    begin();
    add_ = new Button(0, 0, 132, 38, "+ Add panel", true);
    back_ = new Button(0, 0, 84, 30, "Overview"); back_->hide();
    actions_ = new Button(0, 0, 30, 30, "..."); actions_->tooltip("Workspace actions and appearance");
    full_ = new Button(0, 0, 108, 30, "Full screen"); full_->tooltip("Screens only (F11). Exit with F11 or Esc.");
    layouts_ = new Choice(0, 0, 254, 32);
    layouts_->tooltip("Open and connect a saved layout");
    libraryMenu_ = new Button(0, 0, 76, 30, "Layouts");
    preset_ = new Choice(0, 0, 205, 27);
    preset_->add("Custom grid|Single column|Side by side|Stacked|Top + two below|Left + two right|Grid 2 x 2|Grid 3 x 2|Grid 3 x 3|Free placement");
    preset_->value(static_cast<int>(workspace_.preset));
    preset_->tooltip("Choose an arrangement. Drag its dividers to resize panes. Free placement restores saved window positions.");
    scroll_ = new Fl_Scroll(24, 144, w() - 48, h() - 194);
    scroll_->type(Fl_Scroll::BOTH); scroll_->box(FL_NO_BOX);
    // Keep a stable canvas origin even when every freely placed tile has a
    // positive offset. Otherwise Fl_Scroll reports negative scroll positions.
    anchor_ = new Fl_Box(scroll_->x(), scroll_->y(), 1, 1); anchor_->box(FL_NO_BOX);
    scroll_->end();
    exitFull_ = new Button(0, 0, 30, 24, "\xc3\x97"); exitFull_->hide();
    exitFull_->tooltip("Exit full screen (F11 / Esc)");
    end();
    add_->callback([](Fl_Widget*, void* data) { static_cast<Dashboard*>(data)->edit(nullptr); }, this);
    back_->callback([](Fl_Widget*, void* data) { static_cast<Dashboard*>(data)->focusTile(nullptr); }, this);
    actions_->callback([](Fl_Widget*, void* data) { static_cast<Dashboard*>(data)->workspaceMenu(); }, this);
    auto toggle = [](Fl_Widget*, void* data) { static_cast<Dashboard*>(data)->toggleFullScreen(); };
    full_->callback(toggle, this); exitFull_->callback(toggle, this);
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
    windowed_ = {x(), y(), w(), h()};
    updateLayouts(); applyTheme(); arrange();
    Fl::add_timeout(0.05, tick, this);
    Fl::add_timeout(1.0, updateTick, this);
  }
  ~Dashboard() override { Fl::remove_timeout(tick, this); Fl::remove_timeout(updateTick, this); }
  void start() { for (auto* tile : tiles_) if (tile->panel.autoConnect) tile->session.start(); }
  Palette colors() const { return palette(workspace_.dark); }
  bool focused(const Tile* tile) const { return focused_ == tile; }
  bool selected(const Tile* tile) const { return selected_ == tile; }
  bool screensOnly() const { return presentation_; }
  void select(Tile* tile) {
    selected_ = tile;
    if (freePlacement() && !focused_ && !presentation_ && tiles_.back() != tile) {
      tiles_.erase(tiles_.begin() + index(tile)); tiles_.push_back(tile);
      scroll_->remove(tile); scroll_->add(tile); save();
    }
    scroll_->redraw();
  }
  void repaintTile() { if (freePlacement()) scroll_->redraw(); }
  void statusChanged() { redraw(); }
  void resolutionChanged() { if (freePlacement()) arrange(); }
  bool freePlacement() const { return workspace_.preset == Preset::Free; }
  std::vector<int> dividerSizes(bool vertical) const { return vertical ? grid_.columnSizes : grid_.rowSizes; }
  void resizeDivider(bool vertical, int boundary, const std::vector<int>& initial, int delta) {
    resizeGridDivider(workspace_, vertical, boundary, initial, delta, vertical ? 160 : 96);
    arrange();
  }
  void resetDividers() { workspace_.columnWeights.clear(); workspace_.rowWeights.clear(); arrange(); save(); }
  void releaseInput() { for (auto* tile : tiles_) tile->releaseInput(); }
  void toggleFullScreen() {
    releaseInput(); transitioning_ = true;
    if (!presentation_) {
      windowed_ = {x(), y(), w(), h()}; presentation_ = true;
      fullscreen();
    } else {
      presentation_ = false; fullscreen_off(windowed_.x, windowed_.y, windowed_.width, windowed_.height);
    }
    transitioning_ = false; arrange(); save();
  }
  bool localKey(int event) {
    if (event != FL_KEYDOWN && event != FL_KEYUP && event != FL_SHORTCUT) return false;
    int key = Fl::event_key();
    if (event == FL_KEYUP) return localKeys_.erase(key) != 0 || key == FL_F + 11;
    if (key != FL_F + 11 && !(key == FL_Escape && presentation_) && !localKeys_.count(key)) return false;
    if (localKeys_.insert(key).second) toggleFullScreen();
    return true;
  }
  int handle(int event) override {
    if (localKey(event)) return 1;
    return Fl_Double_Window::handle(event);
  }
  void edit(Tile* tile) {
    if (!tile && tiles_.size() >= 32) { fl_alert("A workspace supports up to 32 panels."); return; }
    Panel profile = tile ? tile->panel : Panel{};
    if (!tile) { profile.id = newId(); profile.rememberPassword = passwordStorageAvailable(); }
    if (!tile && freePlacement() && !tiles_.empty()) {
      profile.pixelX = std::min(65535, tiles_.back()->panel.pixelX + 24);
      profile.pixelY = std::min(65535, tiles_.back()->panel.pixelY + 24);
      profile.freePositioned = true;
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
      choosePreset(workspace_.preset, false);
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
      {"Disconnect", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
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
      case 7: tile->session.stop(); break;
    }
  }
  void focusTile(Tile* tile) {
    releaseInput();
    focused_ = focused_ == tile ? nullptr : tile;
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
    if (focused_ || presentation_) return;
    if (freePlacement()) {
      tile->panel.fit = true;
      tile->panel.freePositioned = true;
      tile->panel.pixelWidth = std::clamp(width, 160, 8192);
      tile->panel.pixelHeight = std::clamp(height, 100, 4320);
      arrange(); return;
    }
  }
  void moveFree(Tile* tile, int x, int y) {
    if (!freePlacement() || focused_ || presentation_) return;
    tile->panel.pixelX = std::clamp(x, 0, 65535); tile->panel.pixelY = std::clamp(y, 0, 65535);
    tile->panel.freePositioned = true;
    arrange();
  }
  bool save() {
    try { store(snapshot()); return true; }
    catch (const std::exception& error) { fl_alert("Could not save workspace: %s", error.what()); return false; }
  }
  void resize(int x, int y, int w, int h) override {
    Fl_Double_Window::resize(x, y, w, h);
    if (!presentation_ && !transitioning_) windowed_ = {x, y, w, h};
    if (scroll_) arrange();
  }
  void draw() override {
    bool full = (damage() & ~FL_DAMAGE_CHILD) != 0;
    const auto p = colors();
    if (full) {
      fl_color(presentation_ ? fl_rgb_color(13, 22, 35) : p.background); fl_rectf(0, 0, w(), h());
      if (!presentation_) {
        fl_color(p.card); fl_rectf(0, 0, w(), toolbarHeight);
        rounded(4, 8, 24, 24, fl_rgb_color(37, 99, 235), 6);
        fl_color(FL_WHITE);
        fl_rect(9, 13, 5, 5); fl_rect(18, 13, 5, 5);
        fl_rect(9, 22, 5, 5); fl_rect(18, 22, 5, 5);
        int live = 0; for (auto* tile : tiles_) live += tile->session.live();
        int left = focused_ ? 700 : 606;
        if (w() - 164 - left >= 100)
          caption(std::to_string(live) + " / " + std::to_string(tiles_.size()) + " connected",
                  left, 5, w() - 164 - left, 30, p.muted, 11, false, FL_ALIGN_RIGHT);
      }
    }
    draw_children();
    if (full && tiles_.empty() && !presentation_) {
      rounded(w() / 2 - 34, h() / 2 - 88, 68, 68, p.soft, 16);
      caption("+", w() / 2 - 34, h() / 2 - 88, 68, 68, p.accent, 36, false, FL_ALIGN_CENTER);
      caption("Your panels, together", 30, h() / 2 - 4, w() - 60, 36, p.text, 25, true, FL_ALIGN_CENTER);
      caption("Add your first panel to build a workspace.", 30, h() / 2 + 39, w() - 60, 25, p.muted, 14, false, FL_ALIGN_CENTER);
      caption("Connections, passwords and layout are remembered on this PC.", 30, h() / 2 + 72, w() - 60, 24, p.muted, 12, false, FL_ALIGN_CENTER);
    }
    // Streaming tiles repaint underneath the exit control, so composite it last.
    if (presentation_) exitFull_->draw();
  }
private:
  static void updateTick(void* data) {
    auto& app = *static_cast<Dashboard*>(data);
    Fl::repeat_timeout(1.0, updateTick, data);
    app.updater_.poll();
    bool available = app.updater_.available();
    app.actions_->copy_label(available ? "!" : "...");
    app.actions_->tooltip(available ? "An update is available. Open Updates to download and install." : "Workspace actions, appearance and updates");
  }
  static void tick(void* data) {
    auto& app = *static_cast<Dashboard*>(data);
    // Schedule before processing so other sessions keep updating in certificate dialogs.
    Fl::repeat_timeout(0.04, tick, data);
    for (auto* tile : app.tiles_) tile->session.tick();
    // X11 without a window manager recreates the window for fullscreen and the
    // shortcut's release goes to the old one. Ask the keyboard instead of
    // waiting for it, or the key stays "held" and later presses are ignored.
    for (auto key = app.localKeys_.begin(); key != app.localKeys_.end();)
      key = Fl::get_key(*key) ? std::next(key) : app.localKeys_.erase(key);
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
    Workspace result = workspace_; result.width = windowed_.width; result.height = windowed_.height; result.panels.clear();
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
    back_->hide();
    library_ = std::move(candidate);
    workspace_ = library_.layouts[activeLayout(library_)].workspace;
    preset_->value(static_cast<int>(workspace_.preset));
    for (const auto& panel : workspace_.panels) addTile(panel);
    updateLayouts(); applyTheme(); arrange();
    if (connect) for (auto* tile : tiles_) tile->session.start();
  }
  void choosePreset(Preset preset, bool resetDividers = true) {
    releaseInput();
    if (focused_) focusTile(nullptr);
    if (preset == Preset::Free && !freePlacement()) {
      for (auto* tile : tiles_) if (!tile->panel.freePositioned) {
        tile->panel.pixelX = tile->x() - scroll_->x() + scroll_->xposition();
        tile->panel.pixelY = tile->y() - scroll_->y() + scroll_->yposition();
        tile->panel.pixelWidth = tile->w(); tile->panel.pixelHeight = tile->h();
        tile->panel.freePositioned = true;
      }
    }
    Workspace candidate = snapshot(); applyPreset(candidate, preset, resetDividers);
    workspace_ = candidate;
    for (size_t number = 0; number < tiles_.size(); ++number) tiles_[number]->panel = candidate.panels[number];
    preset_->value(static_cast<int>(preset));
    arrange(); save();
  }
  void viewSettings(Tile* tile) {
    ViewDialog dialog(tile->panel, tile->session.width(), tile->session.height(), colors()); if (!dialog.run()) return;
    if (!dialog.result.fit && !freePlacement()) choosePreset(Preset::Free);
    tile->panel.displayWidth = dialog.result.displayWidth; tile->panel.displayHeight = dialog.result.displayHeight;
    tile->panel.displayPreset = dialog.result.displayPreset;
    tile->panel.scale = dialog.result.scale; tile->panel.fit = dialog.result.fit;
    if (freePlacement() && !tile->panel.fit) {
      auto size = scaledDisplaySize(tile->panel, tile->session.width(), tile->session.height());
      tile->panel.pixelWidth = size.first + frameWidth; tile->panel.pixelHeight = size.second + frameHeight;
      tile->panel.freePositioned = true;
    }
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
  void workspaceMenu() {
    releaseInput();
    Fl_Menu_Item items[] = {
      {"Connect all", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Disconnect all", 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
      {"Reset pane divisions", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {workspace_.dark ? "Light theme" : "Dark theme", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Accept unknown certificates", 0, nullptr, nullptr,
       FL_MENU_TOGGLE | FL_MENU_DIVIDER | (library_.acceptUnknownCertificates ? FL_MENU_VALUE : 0), 0, 0, 0, 0},
      {"Custom grid: 1 column", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Custom grid: 2 columns", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Custom grid: 3 columns", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Custom grid: 4 columns", 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
      {updater_.available() ? "Update available..." : "Updates...", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}
    };
    // The button sits at the window's right edge, so align the menu's right side with it.
    int left = actions_->x() + actions_->w() - measureMenu(items, layouts_).width;
    const auto* choice = items->pulldown(left, actions_->y(), actions_->w(), actions_->h(), nullptr, layouts_);
    if (!choice) return;
    int action = static_cast<int>(choice - items);
    if (action <= 1) for (auto* tile : tiles_) { if (action == 0) tile->session.start(); else tile->session.stop(); }
    else if (action == 2) resetDividers();
    else if (action == 3) { workspace_.dark = !workspace_.dark; applyTheme(); save(); }
    else if (action == 4) toggleCertificates();
    else if (action == 9) {
      UpdateDialog dialog(updater_, colors());
      if (dialog.run() && save()) {
        if (!updater_.install()) { fl_alert("%s", updater_.message().c_str()); return; }
        for (auto* tile : tiles_) tile->session.stop();
        hide();
      }
    } else {
      workspace_.columns = action - 4;
      for (auto* tile : tiles_) tile->panel.columns = tile->panel.rows = 1;
      choosePreset(Preset::CustomGrid);
    }
  }
  void toggleCertificates() {
    bool enable = !library_.acceptUnknownCertificates;
    if (enable && fl_choice("Accept new, changed, expired and mismatched panel certificates\n"
                            "without asking, on every layout?\n\n"
                            "Connections stay encrypted, but a panel's identity is no longer\n"
                            "checked. Use this only on a trusted network.",
                            "Cancel", "Accept all", nullptr) != 1) return;
    Library candidate = library_; candidate.acceptUnknownCertificates = enable;
    try { saveLibrary(path_, candidate); }
    catch (const std::exception& error) { fl_alert("Could not save setting: %s", error.what()); return; }
    library_ = std::move(candidate);
    Session::acceptUnknownCertificates(enable);
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
    for (auto* button : {add_, libraryMenu_, back_, actions_, full_}) button->theme(p, p.card);
    exitFull_->theme(palette(true), fl_rgb_color(13, 22, 35));
    for (auto* choice : {preset_, layouts_}) choice->theme(p, p.card);
    redraw();
  }
  void arrange() {
    for (Fl_Widget* widget : std::vector<Fl_Widget*>{add_, libraryMenu_, layouts_, preset_, actions_, full_}) {
      if (presentation_) widget->hide(); else widget->show();
    }
    if (focused_ && !presentation_) back_->show(); else back_->hide();
    if (presentation_) exitFull_->show(); else exitFull_->hide();
    layouts_->resize(36, 5, 186, 30); preset_->resize(228, 5, 184, 30);
    add_->resize(418, 5, 94, 30); libraryMenu_->resize(518, 5, 76, 30);
    back_->resize(600, 5, 84, 30); full_->resize(w() - 150, 5, 108, 30);
    actions_->resize(w() - 36, 5, 30, 30); exitFull_->resize(w() - 36, 6, 30, 24);
    int oldX = std::max(0, scroll_->xposition()), oldY = std::max(0, scroll_->yposition());
    scroll_->scroll_to(0, 0);
    if (presentation_) scroll_->resize(0, 0, w(), h());
    else scroll_->resize(4, toolbarHeight + 4, w() - 8, h() - toolbarHeight - 8);
    scroll_->color(presentation_ ? fl_rgb_color(13, 22, 35) : colors().background);
    anchor_->resize(scroll_->x(), scroll_->y(), 1, 1);
    std::vector<Rect> rectangles;
    if (!freePlacement()) {
      auto current = snapshot();
      grid_ = gridGeometry(current, scroll_->w(), scroll_->h(), presentation_ ? 2 : 4,
                           presentation_ ? 1 : 160, presentation_ ? 1 : 96);
      if (!presentation_ && !focused_ && grid_.height > scroll_->h())
        grid_ = gridGeometry(current, scroll_->w() - Fl::scrollbar_size(), scroll_->h());
      rectangles = grid_.panels;
    } else {
      for (auto* tile : tiles_) {
        auto& panel = tile->panel;
        int width = panel.pixelWidth, height = panel.pixelHeight;
        if (!panel.fit) {
          auto size = scaledDisplaySize(panel, tile->session.width(), tile->session.height());
          width = size.first + frameWidth; height = size.second + frameHeight;
        }
        rectangles.push_back({panel.pixelX, panel.pixelY, width, height});
      }
      if (presentation_ && !rectangles.empty()) {
        int left = rectangles[0].x, top = rectangles[0].y, right = 0, bottom = 0;
        for (const auto& rect : rectangles) {
          left = std::min(left, rect.x); top = std::min(top, rect.y);
          right = std::max(right, rect.x + rect.width); bottom = std::max(bottom, rect.y + rect.height);
        }
        double scale = std::min(scroll_->w() / static_cast<double>(right - left), scroll_->h() / static_cast<double>(bottom - top));
        int dx = (scroll_->w() - static_cast<int>((right - left) * scale)) / 2;
        int dy = (scroll_->h() - static_cast<int>((bottom - top) * scale)) / 2;
        for (auto& rect : rectangles) rect = {dx + static_cast<int>((rect.x - left) * scale),
          dy + static_cast<int>((rect.y - top) * scale), std::max(1, static_cast<int>(rect.width * scale)),
          std::max(1, static_cast<int>(rect.height * scale))};
      }
    }
    for (size_t number = 0; number < tiles_.size(); ++number) {
      auto* tile = tiles_[number];
      if (focused_ && focused_ != tile) { tile->hide(); continue; }
      tile->show();
      const auto& rect = rectangles[number];
      if (focused_) tile->resize(scroll_->x(), scroll_->y(), scroll_->w(), scroll_->h());
      else tile->resize(scroll_->x() + rect.x, scroll_->y() + rect.y, rect.width, rect.height);
    }
    size_t count = freePlacement() || focused_ || presentation_ ? 0 : grid_.dividers.size();
    while (dividers_.size() < count) {
      Fl_Group* previous = Fl_Group::current(); scroll_->begin();
      auto* divider = new Divider(*this); divider->tooltip("Drag to resize panes. Double-click to reset divisions.");
      dividers_.push_back(divider); scroll_->end(); Fl_Group::current(previous);
    }
    for (size_t number = 0; number < dividers_.size(); ++number) {
      auto* divider = dividers_[number];
      if (number >= count) { divider->hide(); continue; }
      const auto& placement = grid_.dividers[number]; const auto& rect = placement.rect;
      divider->configure(placement.vertical, placement.boundary);
      divider->resize(scroll_->x() + rect.x, scroll_->y() + rect.y, rect.width, rect.height); divider->show();
    }
    if (!presentation_ && !focused_ && freePlacement()) scroll_->scroll_to(oldX, oldY);
    redraw();
  }
  void move(Tile* tile, size_t to) {
    if (focused_) return;
    tiles_.erase(tiles_.begin() + index(tile));
    tiles_.insert(tiles_.begin() + to, tile);
    if (freePlacement()) for (auto* item : tiles_) { scroll_->remove(item); scroll_->add(item); }
    if (workspace_.preset != Preset::CustomGrid && !freePlacement()) choosePreset(workspace_.preset, false);
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
    if (workspace_.preset != Preset::CustomGrid && !freePlacement()) choosePreset(workspace_.preset, false);
    else arrange();
  }
  Library library_;
  Workspace workspace_;
  std::filesystem::path path_;
  AppUpdate updater_;
  std::vector<Tile*> tiles_;
  Tile *focused_ = nullptr, *selected_ = nullptr;
  Fl_Scroll* scroll_ = nullptr;
  Fl_Box* anchor_;
  Button *add_, *back_, *actions_, *full_, *exitFull_, *libraryMenu_;
  Choice *layouts_, *preset_;
  GridGeometry grid_;
  std::vector<Divider*> dividers_;
  bool presentation_ = false, transitioning_ = false;
  Rect windowed_{};
  std::set<int> localKeys_;
};

Tile::Tile(Dashboard& owner, Panel profile)
  : Fl_Widget(0, 0, 300, 260), panel(std::move(profile)),
    session(panel, [this] { changed(); }), owner_(owner)
{
  tooltip("Drag headers to arrange. Resize grid dividers or free-window corners. Use ... for connection, size and scale.");
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
  if (w != this->w() || h != this->h()) image_.reset();
  Fl_Widget::resize(x, y, w, h);
}
void Tile::imageRect(int& left, int& top, int& width, int& height) const {
  left = x(); top = y(); width = w(); height = h();
  if (!owner_.screensOnly()) { left += 1; top += 25; width -= frameWidth; height -= frameHeight; }
  if (!panel.fit && owner_.freePlacement() && !owner_.focused(this) && !owner_.screensOnly()) {
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
void Divider::draw() {
  fl_color(hovering_ || dragging_ ? owner_.colors().accent : owner_.colors().background);
  fl_rectf(x(), y(), w(), h());
}
int Divider::handle(int event) {
  if (owner_.localKey(event)) return 1;
  switch (event) {
    case FL_FOCUS: return 1;
    case FL_ENTER: hovering_ = true; window()->cursor(vertical_ ? FL_CURSOR_WE : FL_CURSOR_NS); redraw(); return 1;
    case FL_LEAVE: hovering_ = false; if (!dragging_) window()->cursor(FL_CURSOR_DEFAULT); redraw(); return 1;
    case FL_PUSH:
      owner_.releaseInput(); take_focus();
      if (Fl::event_clicks()) { owner_.resetDividers(); return 1; }
      dragging_ = true; press_ = vertical_ ? Fl::event_x() : Fl::event_y();
      initial_ = owner_.dividerSizes(vertical_); return 1;
    case FL_DRAG:
      if (dragging_) owner_.resizeDivider(vertical_, boundary_, initial_, (vertical_ ? Fl::event_x() : Fl::event_y()) - press_);
      return 1;
    case FL_RELEASE:
      if (dragging_) { dragging_ = false; owner_.save(); }
      window()->cursor(FL_CURSOR_DEFAULT); redraw(); return 1;
  }
  return Fl_Widget::handle(event);
}
void Tile::draw() {
  const auto p = owner_.colors(); bool chrome = !owner_.screensOnly();
  if (chrome) {
    fl_color(owner_.selected(this) ? p.accent : p.border); fl_rectf(x(), y(), w(), h());
    fl_color(p.card); fl_rectf(x() + 1, y() + 1, w() - 2, 23);
    Fl_Color status = session.live() ? p.green : session.status() == Session::State::Offline ? p.muted : p.amber;
    fl_color(status); fl_pie(x() + 7, y() + 10, 6, 6, 0, 360);
    int nameWidth = std::max(0, w() - 142);
    if (w() >= 540) nameWidth -= 180;
    caption(panel.name, x() + 18, y() + 1, nameWidth, 23, p.text, 12, true);
    if (w() >= 540) caption(panel.address, x() + 20 + nameWidth, y() + 1, 175, 23, p.muted, 10);
    rounded(x() + w() - 120, y() + 3, 60, 19, p.soft, 4);
    caption(panel.viewOnly ? "Monitor" : "Control", x() + w() - 120, y() + 2, 60, 20, p.accent, 10, true, FL_ALIGN_CENTER);
    caption(owner_.focused(this) ? "<>" : "[ ]", x() + w() - 55, y() + 1, 22, 23, p.muted, 12, false, FL_ALIGN_CENTER);
    caption("...", x() + w() - 29, y() - 1, 23, 23, p.muted, 17, true, FL_ALIGN_CENTER);
  }
  fl_color(fl_rgb_color(13, 22, 35));
  fl_rectf(x() + (chrome ? 1 : 0), y() + (chrome ? 25 : 0), w() - (chrome ? frameWidth : 0), h() - (chrome ? frameHeight : 0));
  if (session.live() && !session.pixels().empty()) {
    int left, top, width, height; imageRect(left, top, width, height);
    if (!image_ || generation_ != session.generation() || imageWidth_ != width || imageHeight_ != height) {
      image_.reset();
      const auto& pixels = scaler_.scale(session.pixels(), session.width(), session.height(), width, height);
      image_.reset(new Fl_RGB_Image(pixels.data(), width, height, 3));
      generation_ = session.generation(); imageWidth_ = width; imageHeight_ = height;
    }
    image_->draw(left, top);
  } else {
    caption(session.statusText(), x() + 12, y() + h() / 2 - 44, w() - 24, 28,
      fl_rgb_color(219, 229, 244), 16, true, FL_ALIGN_CENTER);
    caption(session.error().empty() ? (session.wanted() ? "Waiting for the panel" : panel.address) : session.error(),
      x() + 12, y() + h() / 2 - 12, w() - 24, 44,
      fl_rgb_color(148, 163, 184), 12, false, FL_ALIGN_CENTER | FL_ALIGN_WRAP);
    if (chrome && !session.wanted() && h() >= 160) {
      rounded(x() + w() / 2 - 44, y() + h() / 2 + 38, 88, 26, p.soft, 7);
      caption("Connect", x() + w() / 2 - 44, y() + h() / 2 + 38, 88, 26, p.accent, 11, true, FL_ALIGN_CENTER);
    }
  }
  if (chrome && owner_.freePlacement() && !owner_.focused(this)) {
    fl_color(p.muted);
    for (int i = 0; i < 3; ++i) fl_line(x() + w() - 5 - i * 3, y() + h() - 5,
      x() + w() - 5, y() + h() - 5 - i * 3);
  }
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
  if (owner_.localKey(event)) return 1;
  switch (event) {
    case FL_FOCUS: redraw(); return 1;
    case FL_UNFOCUS: remoteFocus_ = false; session.releaseInput(); redraw(); return 1;
    case FL_PUSH: {
      take_focus(); owner_.select(this);
      remoteFocus_ = false;
      pressX_ = Fl::event_x(); pressY_ = Fl::event_y(); originalW_ = w(); originalH_ = h(); dragged_ = false;
      originalX_ = panel.pixelX; originalY_ = panel.pixelY;
      if (!owner_.screensOnly() && owner_.freePlacement() && !owner_.focused(this) &&
          hitRect(x() + w() - 15, y() + h() - 15, 15, 15)) gesture_ = Gesture::Resize;
      else if (!owner_.screensOnly() && !session.wanted() && h() >= 160 &&
               hitRect(x() + w() / 2 - 44, y() + h() / 2 + 38, 88, 26)) gesture_ = Gesture::Connect;
      else if (!owner_.screensOnly() && hitRect(x(), y(), w(), 25)) {
        session.releaseInput();
        if (hitRect(x() + w() - 29, y(), 25, 25)) gesture_ = Gesture::Menu;
        else if (hitRect(x() + w() - 55, y(), 22, 25)) gesture_ = Gesture::Focus;
        else if (hitRect(x() + w() - 120, y(), 60, 25)) gesture_ = Gesture::Mode;
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
