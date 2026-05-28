#pragma once

#include <nall/string.hpp>
#include <vector>
#include <memory>
#include <functional>

// Global stub types (were hiro types used without prefix)
struct Menu;  // forward decl
struct MenuBar {};
struct MenuItem {
  MenuItem() = default;
  MenuItem(Menu*) {}
  MenuItem(MenuItem*) {}
  auto setText(const nall::string&) -> MenuItem& { return *this; }
  auto setIcon(auto) -> MenuItem& { return *this; }
  auto onActivate(auto&&) -> MenuItem& { return *this; }
  auto setEnabled(bool) -> MenuItem& { return *this; }
};
struct MenuSeparator { MenuSeparator() = default; MenuSeparator(Menu*) {} };
struct MenuCheckItem {
  auto setText(const nall::string&) -> MenuCheckItem& { return *this; }
  auto setChecked(bool) -> MenuCheckItem& { return *this; }
  auto onToggle(auto&&) -> MenuCheckItem& { return *this; }
};
struct MenuRadioItem {
  MenuRadioItem() = default;
  MenuRadioItem(Menu*) {}
  MenuRadioItem(MenuItem*) {}
  auto setText(const nall::string&) -> MenuRadioItem& { return *this; }
  auto setChecked() -> MenuRadioItem& { return *this; }
  auto onActivate(auto&&) -> MenuRadioItem& { return *this; }
  template<typename T> auto setAttribute(const nall::string&, T) -> MenuRadioItem& { return *this; }
  template<typename T> auto attribute(const nall::string&) const -> T { return T{}; }
};
struct Menu {
  Menu() = default;
  Menu(MenuBar*) {}
  Menu(MenuItem*) {}
  Menu(Menu*) {}
  auto actionCount() const -> int { return 0; }
  auto append(MenuItem) -> Menu& { return *this; }
  auto append(MenuSeparator) -> Menu& { return *this; }
  auto append(MenuCheckItem) -> Menu& { return *this; }
  auto append(MenuRadioItem) -> Menu& { return *this; }
  auto setText(const nall::string&) -> Menu& { return *this; }
  auto reset() -> void {}
  auto setVisible(bool=true) -> Menu& { return *this; }
};
struct Group {
  Group() = default;
  template<typename... T> Group(T*...) {}
  auto append(MenuRadioItem&) -> void {}
};
struct Timer {
  auto onActivate(auto&& f) -> Timer& { return *this; }
  auto setInterval(int) -> Timer& { return *this; }
  auto setEnabled(bool = true) -> Timer& { return *this; }
  auto reset() -> void {}
};
struct sTimer {
  auto onActivate(auto&& f) -> sTimer& { return *this; }
  auto setInterval(int) -> sTimer& { return *this; }
  auto setEnabled(bool = true) -> sTimer& { return *this; }
  auto reset() -> void {}
  sTimer& operator=(Timer) { return *this; }
  sTimer* operator->() { return this; }
};

namespace hiro {
  struct multiFactorImage {};

  struct BrowserDialog {
    auto title() const -> nall::string { return ""; }
    auto path() const -> nall::string { return ""; }
    auto filters() const -> nall::string { return ""; }
    auto alignmentWindow() const -> uintptr { return 0; }
    auto setTitle(const nall::string&) -> BrowserDialog& { return *this; }
    auto setPath(const nall::string&) -> BrowserDialog& { return *this; }
    auto setAlignment(auto&) -> BrowserDialog& { return *this; }
    auto setFilters(const nall::string&) -> BrowserDialog& { return *this; }
    auto setParent(auto&) -> BrowserDialog& { return *this; }
    auto openFile() -> nall::string { return ""; }
  };

  struct MessageDialog {
    auto setTitle(const nall::string&) -> MessageDialog& { return *this; }
    auto setText(const nall::string&) -> MessageDialog& { return *this; }
    auto setAlignment(auto&) -> MessageDialog& { return *this; }
    auto error() -> void {}
    auto warning() -> void {}
    auto question(const std::vector<nall::string>&) -> nall::string { return ""; }
    auto question() -> nall::string { return ""; }
  };

  struct BrowserWindow {
    auto setTitle(const nall::string&) -> BrowserWindow& { return *this; }
    auto setPath(const nall::string&) -> BrowserWindow& { return *this; }
    auto setFilters(const nall::string&) -> BrowserWindow& { return *this; }
    auto setParent(uintptr) -> BrowserWindow& { return *this; }
    auto setAllowsFolders(bool) -> BrowserWindow& { return *this; }
    auto open() -> nall::string { return ""; }
    auto save() -> nall::string { return ""; }
    auto directory() -> nall::string { return ""; }
  };

  struct Application {
    static auto setName(const nall::string&) -> void {}
    static auto setScreenSaver(bool) -> void {}
    static auto modal() -> bool { return false; }
    static auto processEvents() -> void {}
    static auto quit() -> void {}
    static auto run() -> void {}
    static auto onMain(auto&&) -> void {}
    struct State { bool initialized = false; };
    static auto state() -> State& { static State s; return s; }
  };

  struct Alignment { double horizontal = 0, vertical = 0; static constexpr double Center = 0.5; };
  struct Font { auto setBold(bool=true)->Font&{return*this;} auto setSize(double)->Font&{return*this;} auto setFamily(const nall::string&)->Font&{return*this;} static constexpr const char* Mono = "monospace"; };
  struct Image {};

  struct Icon {
    struct Device { static inline multiFactorImage Joypad, Keyboard, Mouse, Speaker, Optical; };
    struct Go { static inline multiFactorImage Right; };
    struct Action { static inline multiFactorImage Settings, Save; };
    struct Emblem { static inline multiFactorImage Binary, Folder; };
    struct Place { static inline multiFactorImage Server, Settings; };
  };
}

using hiro::BrowserDialog;
using hiro::MessageDialog;
using hiro::BrowserWindow;
using hiro::Application;
using hiro::multiFactorImage;
using hiro::Icon;
