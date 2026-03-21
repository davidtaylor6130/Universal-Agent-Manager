#pragma once

/// <summary>
/// Global animated backdrop renderer.
/// </summary>
static void DrawAmbientBackdrop(const ImVec2& pos, const ImVec2& size, const float time_s) {
  const bool light = IsLightPaletteActive();
  ImDrawList* draw = ImGui::GetBackgroundDrawList();
  draw->AddRectFilledMultiColor(
      pos,
      ImVec2(pos.x + size.x, pos.y + size.y),
      ImGui::GetColorU32(light ? Rgb(243, 247, 253, 1.0f) : Rgb(13, 17, 24, 1.0f)),
      ImGui::GetColorU32(light ? Rgb(235, 242, 252, 1.0f) : Rgb(15, 20, 28, 1.0f)),
      ImGui::GetColorU32(light ? Rgb(238, 245, 255, 1.0f) : Rgb(11, 15, 22, 1.0f)),
      ImGui::GetColorU32(light ? Rgb(230, 239, 250, 1.0f) : Rgb(9, 13, 19, 1.0f)));

  const float pulse = 0.72f + 0.28f * std::sin(time_s * 0.30f);
  draw->AddCircleFilled(
      ImVec2(pos.x + size.x * 0.14f, pos.y + size.y * 0.16f),
      size.x * 0.20f,
      ImGui::GetColorU32(light ? Rgb(66, 126, 228, 0.07f * pulse) : Rgb(94, 160, 255, 0.06f * pulse)),
      72);
  draw->AddCircleFilled(
      ImVec2(pos.x + size.x * 0.87f, pos.y + size.y * 0.07f),
      size.x * 0.14f,
      ImGui::GetColorU32(light ? Rgb(66, 126, 228, 0.05f * pulse) : Rgb(94, 160, 255, 0.04f * pulse)),
      64);
  draw->AddRectFilledMultiColor(
      ImVec2(pos.x, pos.y + size.y * 0.68f),
      ImVec2(pos.x + size.x, pos.y + size.y),
      ImGui::GetColorU32(ui::kTransparent),
      ImGui::GetColorU32(ui::kTransparent),
      ImGui::GetColorU32(light ? Rgb(12, 30, 58, 0.12f) : Rgb(0, 0, 0, 0.42f)),
      ImGui::GetColorU32(light ? Rgb(12, 30, 58, 0.12f) : Rgb(0, 0, 0, 0.42f)));
}
