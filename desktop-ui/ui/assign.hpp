#pragma once

struct InputNode;
struct InputMapping;

namespace ares::ui {

struct InputAssignmentState {
  InputNode* activeNode = nullptr;
  InputMapping* activeMapping = nullptr;
  int activeBinding = -1;
  bool waiting = false;
};

extern InputAssignmentState inputAssign;

}  // namespace ares::ui
