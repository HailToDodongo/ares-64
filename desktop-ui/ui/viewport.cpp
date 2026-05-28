#include "ui.hpp"

#include "../desktop-ui.hpp"
#include "../application/application.hpp"
#include <cstdio>

namespace ares::ui {

auto DrawViewport() -> void {
  ruby::video.renderFrame();
  auto tex = ruby::video.outputTexture();

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);

  if(tex) {
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
    auto flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
    ImGui::Begin("ares", nullptr, flags);

    // Reserve bottom space for status bar if enabled
    float statusHeight = 0;
    if(program.message.text) {
      statusHeight = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y + 4;
    }

    auto avail = ImGui::GetContentRegionAvail();
    if(avail.x > 0 && avail.y > statusHeight) {
      ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(avail.x, avail.y - statusHeight));
    }

    // Status bar
    if(program.message.text) {
      ImGui::Separator();
      ImGui::TextUnformatted(program.message.text.data());
    }

    ImGui::End();
  } else {
    ImGui::Begin("ares", nullptr,
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoTitleBar);
    auto avail = ImGui::GetContentRegionAvail();
    auto text = "Load a game from File menu";
    auto textSize = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
    ImGui::TextUnformatted(text);
    ImGui::End();
  }

  ImGui::PopStyleVar(2);
}

}  // namespace ares::ui
