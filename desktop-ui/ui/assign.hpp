#pragma once

#include <nall/string.hpp>

struct InputNode;

namespace ares::ui {

struct InputAssignmentState {
  InputNode* activeNode = nullptr;
  int activeBinding = -1;
  bool waiting = false;
};

extern InputAssignmentState inputAssign;

}  // namespace ares::ui
