#pragma once

/// <summary>
/// Global animated backdrop renderer.
/// </summary>
inline void DrawAmbientBackdrop(const ImVec2& pos, const ImVec2& size, const float time_s)
{
	const bool light = IsLightPaletteActive();
	ImDrawList* draw = ImGui::GetBackgroundDrawList();
	draw->AddRectFilledMultiColor(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(light ? Rgb(243, 247, 253, 1.0f) : Rgb(13, 17, 24, 1.0f)), ImGui::GetColorU32(light ? Rgb(235, 242, 252, 1.0f) : Rgb(15, 20, 28, 1.0f)), ImGui::GetColorU32(light ? Rgb(238, 245, 255, 1.0f) : Rgb(11, 15, 22, 1.0f)), ImGui::GetColorU32(light ? Rgb(230, 239, 250, 1.0f) : Rgb(9, 13, 19, 1.0f)));
	(void)time_s;
}
