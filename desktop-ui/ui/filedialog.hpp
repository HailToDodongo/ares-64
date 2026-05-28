#pragma once

namespace ares::ui {

auto openFileDialog(const char* title, const char* path = nullptr) -> const char*;
auto saveFileDialog(const char* title, const char* path = nullptr) -> const char*;
auto selectFolderDialog(const char* title, const char* path = nullptr) -> const char*;
auto messageBox(const char* title, const char* text, bool error = false) -> void;

}  // namespace ares::ui
