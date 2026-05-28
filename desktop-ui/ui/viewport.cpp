#include "ui.hpp"

#include "../desktop-ui.hpp"
#include "../application/application.hpp"
#include <cstdio>

namespace ares::ui {

auto DrawViewport() -> void {
  ruby::video.renderFrame();
  auto tex = ruby::video.outputTexture();

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

  auto flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  ImGui::Begin("ares", nullptr, flags);

  if(tex) {
    auto avail = ImGui::GetContentRegionAvail();
    float statusH = program.message.text ? ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y + 4 : 0;
    if(avail.x > 0 && avail.y > statusH) {
      ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(avail.x, avail.y - statusH));
    }
    if(program.message.text) {
      ImGui::Separator();
      ImGui::TextUnformatted(program.message.text.data());
    }
  } else {
    auto avail = ImGui::GetContentRegionAvail();
    auto text = "Load a game from File menu";
    auto textSize = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
    ImGui::TextUnformatted(text);
  }

  ImGui::End();
  ImGui::PopStyleVar();
}

}  // namespace ares::ui
