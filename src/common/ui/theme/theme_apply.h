#pragma once

/// <summary>
/// Applies modern Dear ImGui style and selected palette mode.
/// </summary>
inline void ApplyModernTheme()
{
	const bool light = IsLightPaletteActive();
	const float spacing_scale = PlatformServicesFactory::Instance().ui_traits.PlatformUiSpacingScale();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowPadding = ImVec2(ui::kSpace16, ui::kSpace16);
	style.FramePadding = ImVec2(10.0f * spacing_scale, 7.0f * spacing_scale);
	style.CellPadding = ImVec2(ui::kSpace10 * spacing_scale, ui::kSpace8 * spacing_scale);
	style.ItemSpacing = ImVec2(ui::kSpace10 * spacing_scale, ui::kSpace10 * spacing_scale);
	style.ItemInnerSpacing = ImVec2(ui::kSpace8 * spacing_scale, ui::kSpace8 * spacing_scale);
	style.IndentSpacing = 16.0f * spacing_scale;
	style.ScrollbarSize = 11.0f;
	style.GrabMinSize = 10.0f;

	style.WindowBorderSize = 0.0f;
	style.ChildBorderSize = 1.0f;
	style.PopupBorderSize = 1.0f;
	style.FrameBorderSize = 1.0f;
	style.TabBorderSize = 0.0f;

	style.WindowRounding = 8.0f;
	style.ChildRounding = ui::kRadiusPanel;
	style.FrameRounding = ui::kRadiusSmall;
	style.PopupRounding = ui::kRadiusPanel;
	style.ScrollbarRounding = ui::kRadiusSmall;
	style.GrabRounding = ui::kRadiusSmall;
	style.TabRounding = ui::kRadiusSmall;

	ImVec4* colors = style.Colors;
	colors[ImGuiCol_Text] = ui::kTextPrimary;
	colors[ImGuiCol_TextDisabled] = ui::kTextMuted;
	colors[ImGuiCol_WindowBg] = ui::kMainBackground;
	colors[ImGuiCol_ChildBg] = ui::kPrimarySurface;
	colors[ImGuiCol_PopupBg] = light ? Rgb(252, 254, 255, 0.99f) : Rgb(25, 31, 41, 0.99f);
	colors[ImGuiCol_Border] = ui::kBorder;
	colors[ImGuiCol_BorderShadow] = ui::kTransparent;
	colors[ImGuiCol_FrameBg] = ui::kInputSurface;
	colors[ImGuiCol_FrameBgHovered] = light ? Rgb(237, 244, 255, 1.0f) : Rgb(21, 27, 37, 1.0f);
	colors[ImGuiCol_FrameBgActive] = light ? Rgb(232, 240, 254, 1.0f) : Rgb(25, 32, 43, 1.0f);
	colors[ImGuiCol_TitleBg] = ui::kPrimarySurface;
	colors[ImGuiCol_TitleBgActive] = ui::kPrimarySurface;
	colors[ImGuiCol_MenuBarBg] = ui::kSecondarySurface;
	colors[ImGuiCol_ScrollbarBg] = ui::kTransparent;
	colors[ImGuiCol_ScrollbarGrab] = light ? Rgb(17, 35, 61, 0.22f) : Rgb(255, 255, 255, 0.17f);
	colors[ImGuiCol_ScrollbarGrabHovered] = light ? Rgb(17, 35, 61, 0.34f) : Rgb(255, 255, 255, 0.24f);
	colors[ImGuiCol_ScrollbarGrabActive] = light ? Rgb(17, 35, 61, 0.46f) : Rgb(255, 255, 255, 0.30f);
	colors[ImGuiCol_CheckMark] = ui::kAccent;
	colors[ImGuiCol_SliderGrab] = ui::kAccent;
	colors[ImGuiCol_SliderGrabActive] = light ? Rgb(54, 114, 220, 1.0f) : Rgb(116, 174, 255, 1.0f);
	colors[ImGuiCol_Button] = ui::kElevatedSurface;
	colors[ImGuiCol_ButtonHovered] = light ? Rgb(226, 237, 251, 1.0f) : Rgb(35, 42, 53, 1.0f);
	colors[ImGuiCol_ButtonActive] = light ? Rgb(216, 230, 250, 1.0f) : Rgb(39, 47, 59, 1.0f);
	colors[ImGuiCol_Header] = light ? Rgb(9, 20, 39, 0.04f) : Rgb(255, 255, 255, 0.04f);
	colors[ImGuiCol_HeaderHovered] = light ? Rgb(9, 20, 39, 0.07f) : Rgb(255, 255, 255, 0.07f);
	colors[ImGuiCol_HeaderActive] = ui::kAccentSoft;
	colors[ImGuiCol_Separator] = ui::kBorder;
	colors[ImGuiCol_ResizeGrip] = light ? Rgb(17, 35, 61, 0.18f) : Rgb(255, 255, 255, 0.09f);
	colors[ImGuiCol_ResizeGripHovered] = light ? Rgb(66, 126, 228, 0.44f) : Rgb(94, 160, 255, 0.44f);
	colors[ImGuiCol_ResizeGripActive] = light ? Rgb(66, 126, 228, 0.76f) : Rgb(94, 160, 255, 0.76f);
	colors[ImGuiCol_Tab] = ui::kSecondarySurface;
	colors[ImGuiCol_TabHovered] = light ? Rgb(226, 237, 251, 1.0f) : Rgb(34, 41, 53, 1.0f);
	colors[ImGuiCol_TabActive] = ui::kElevatedSurface;
	colors[ImGuiCol_TabUnfocused] = ui::kSecondarySurface;
	colors[ImGuiCol_TabUnfocusedActive] = ui::kElevatedSurface;
	colors[ImGuiCol_TableHeaderBg] = ui::kSecondarySurface;
	colors[ImGuiCol_TableBorderStrong] = ui::kBorder;
	colors[ImGuiCol_TableBorderLight] = light ? Rgb(17, 35, 61, 0.08f) : Rgb(255, 255, 255, 0.04f);
	colors[ImGuiCol_TableRowBg] = ui::kTransparent;
	colors[ImGuiCol_TableRowBgAlt] = light ? Rgb(9, 20, 39, 0.03f) : Rgb(255, 255, 255, 0.02f);
	colors[ImGuiCol_TextSelectedBg] = light ? Rgb(66, 126, 228, 0.28f) : Rgb(94, 160, 255, 0.32f);
	colors[ImGuiCol_DragDropTarget] = ui::kAccent;
	colors[ImGuiCol_NavCursor] = ui::kAccent;
	colors[ImGuiCol_NavWindowingHighlight] = light ? Rgb(66, 126, 228, 0.64f) : Rgb(94, 160, 255, 0.64f);
	colors[ImGuiCol_ModalWindowDimBg] = light ? Rgb(20, 30, 45, 0.26f) : Rgb(0, 0, 0, 0.45f);
}

inline void ApplyThemeFromSettings(AppState& app)
{
	app.settings.ui_theme = NormalizeThemeChoice(app.settings.ui_theme);
	ApplyResolvedPalette(ResolveUiTheme(app.settings));
	ApplyModernTheme();
}
