#pragma once

/// <summary>
/// Shared panel, input, button, divider, and compact-text UI primitives.
/// </summary>
inline void PushFontIfAvailable(ImFont* font)
{
	if (font != nullptr)
	{
		ImGui::PushFont(font);
	}
}

inline void PopFontIfAvailable(ImFont* font)
{
	if (font != nullptr)
	{
		ImGui::PopFont();
	}
}

inline bool IsLightPaletteActive()
{
	return ui::kMainBackground.x > 0.55f;
}

inline ImVec4 PanelColor(const PanelTone tone)
{
	switch (tone)
	{
	case PanelTone::Primary:
		return ui::kPrimarySurface;
	case PanelTone::Secondary:
		return ui::kSecondarySurface;
	case PanelTone::Elevated:
		return ui::kElevatedSurface;
	}

	return ui::kPrimarySurface;
}

inline ImVec4 PanelStrokeColor(const PanelTone tone)
{
	const bool light = IsLightPaletteActive();

	switch (tone)
	{
	case PanelTone::Primary:
		return ui::kBorderStrong;
	case PanelTone::Secondary:
		return ui::kBorder;
	case PanelTone::Elevated:
		return light ? Rgb(13, 38, 69, 0.15f) : Rgb(255, 255, 255, 0.10f);
	}

	return ui::kBorder;
}

inline void PushInputChrome(const float rounding = ui::kRadiusSmall)
{
	const bool light = IsLightPaletteActive();
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::kInputSurface);
	ImGui::PushStyleColor(ImGuiCol_Border, ui::kBorder);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, light ? Rgb(237, 244, 255, 1.0f) : Rgb(21, 27, 37, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, light ? Rgb(232, 240, 254, 1.0f) : Rgb(25, 32, 43, 1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ScaleUiLength(rounding));
}

inline void PopInputChrome()
{
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);
}

struct VerticalSplitterResult
{
	bool hovered = false;
	bool active = false;
	bool released = false;
	float drag_delta_x = 0.0f;
};

inline VerticalSplitterResult DrawVerticalSplitter(const char* id,
                                                   const float height,
                                                   bool* was_active_last_frame = nullptr,
                                                   const float visible_thickness = 1.5f,
                                                   const float hit_thickness = 10.0f)
{
	VerticalSplitterResult result;
	const bool light = IsLightPaletteActive();
	const bool was_active = (was_active_last_frame != nullptr) ? *was_active_last_frame : false;
	const float scaled_height = std::max(0.0f, height);
	const float scaled_visible = std::max(1.0f, ScaleUiLength(visible_thickness));
	const float scaled_hit = std::max(scaled_visible, ScaleUiLength(hit_thickness));
	const float line_inset = ScaleUiLength(ui::kSpace8);
	const ImVec2 splitter_pos = ImGui::GetCursorScreenPos();

	ImGui::InvisibleButton(id, ImVec2(scaled_hit, scaled_height), ImGuiButtonFlags_MouseButtonLeft);
	result.hovered = ImGui::IsItemHovered();
	result.active = ImGui::IsItemActive();
	result.released = was_active && !result.active && ImGui::IsMouseReleased(ImGuiMouseButton_Left);

	if (result.hovered || result.active)
	{
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	}

	if (result.active)
	{
		result.drag_delta_x = ImGui::GetIO().MouseDelta.x;
	}

	if (was_active_last_frame != nullptr)
	{
		*was_active_last_frame = result.active;
	}

	const float line_x = splitter_pos.x + (scaled_hit - scaled_visible) * 0.5f;
	const float line_top = splitter_pos.y + line_inset;
	const float line_bottom = splitter_pos.y + std::max(line_inset, scaled_height - line_inset);
	const ImVec4 line_color = result.active
		? ui::kAccent
		: (result.hovered ? (light ? Rgb(66, 126, 228, 0.58f) : Rgb(94, 160, 255, 0.52f)) : ui::kBorder);

	ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(line_x, line_top),
	                                          ImVec2(line_x + scaled_visible, line_bottom),
	                                          ImGui::GetColorU32(line_color),
	                                          ScaleUiLength(ui::kRadiusSmall));

	return result;
}

inline bool BeginPanel(const char* id, const ImVec2& size, const PanelTone tone, const bool border = true, const ImGuiWindowFlags flags = 0, const ImVec2 padding = ImVec2(ui::kSpace16, ui::kSpace16), const float rounding = ui::kRadiusPanel)
{
	const bool light = IsLightPaletteActive();
	const ImVec2 scaled_size = ScaleUiSize(size);
	const ImVec2 scaled_padding = ScaleUiSize(padding);
	const float scaled_rounding = ScaleUiLength(rounding);
	const float shadow_dx = ScaleUiLength(1.0f);
	const float shadow_dy = ScaleUiLength(4.0f);
	const float accent_h = std::max(1.0f, ScaleUiLength(1.5f));
	const float border_thickness = std::max(1.0f, ScaleUiLength(1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, scaled_rounding);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, scaled_padding);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, PanelColor(tone));
	ImGui::PushStyleColor(ImGuiCol_Border, border ? PanelStrokeColor(tone) : ui::kTransparent);
	const bool is_open = ImGui::BeginChild(id, scaled_size, border, flags);

	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 min = ImGui::GetWindowPos();
	const ImVec2 max(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
	draw->AddRectFilled(ImVec2(min.x + shadow_dx, min.y + shadow_dy), ImVec2(max.x + shadow_dx, max.y + shadow_dy), ImGui::GetColorU32(ui::kShadowSoft), scaled_rounding);

	if (border)
	{
		draw->AddRect(min, max, ImGui::GetColorU32(PanelStrokeColor(tone)), scaled_rounding, 0, border_thickness);
	}

	(void)accent_h;
	(void)light;
	return is_open;
}

inline void EndPanel()
{
	ImGui::EndChild();
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(2);
}

inline bool DrawButton(const char* label, const ImVec2& size, const ButtonKind kind)
{
	const bool light = IsLightPaletteActive();
	const float spacing_scale = PlatformServicesFactory::Instance().ui_traits.PlatformUiSpacingScale();
	const ImVec2 scaled_size = ScaleUiSize(size);
	const float scaled_rounding = ScaleUiLength(ui::kRadiusSmall);
	const ImVec2 frame_padding(ScaleUiLength(13.0f * spacing_scale), ScaleUiLength(7.5f * spacing_scale));
	ImVec4 bg = ui::kElevatedSurface;
	ImVec4 bg_hover = light ? Rgb(226, 237, 251, 1.0f) : Rgb(35, 42, 53, 1.0f);
	ImVec4 bg_active = light ? Rgb(216, 230, 250, 1.0f) : Rgb(39, 47, 59, 1.0f);
	ImVec4 border = ui::kBorder;
	ImVec4 text = ui::kTextPrimary;

	if (kind == ButtonKind::Primary)
	{
		bg = light ? Rgb(66, 126, 228, 0.96f) : Rgb(78, 145, 248, 0.96f);
		bg_hover = light ? Rgb(80, 138, 235, 1.0f) : Rgb(94, 157, 255, 1.0f);
		bg_active = light ? Rgb(56, 109, 209, 1.0f) : Rgb(67, 129, 229, 1.0f);
		border = light ? Rgb(94, 149, 240, 0.72f) : Rgb(127, 176, 255, 0.75f);
		text = Rgb(255, 255, 255, 1.0f);
	}
	else if (kind == ButtonKind::Accent)
	{
		bg = light ? Rgb(57, 115, 214, 0.96f) : Rgb(60, 122, 218, 0.95f);
		bg_hover = light ? Rgb(70, 127, 223, 1.0f) : Rgb(73, 136, 234, 1.0f);
		bg_active = light ? Rgb(48, 99, 193, 1.0f) : Rgb(53, 111, 202, 1.0f);
		border = light ? Rgb(90, 141, 230, 0.68f) : Rgb(108, 162, 250, 0.70f);
		text = Rgb(255, 255, 255, 1.0f);
	}
	else if (kind == ButtonKind::Ghost)
	{
		bg = ui::kTransparent;
		bg_hover = light ? Rgb(7, 24, 56, 0.06f) : Rgb(255, 255, 255, 0.06f);
		bg_active = light ? Rgb(7, 24, 56, 0.10f) : Rgb(255, 255, 255, 0.10f);
		border = light ? Rgb(7, 24, 56, 0.14f) : Rgb(255, 255, 255, 0.12f);
	}

	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, scaled_rounding);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, frame_padding);
	ImGui::PushStyleColor(ImGuiCol_Button, bg);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg_hover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg_active);
	ImGui::PushStyleColor(ImGuiCol_Border, border);
	ImGui::PushStyleColor(ImGuiCol_Text, text);
	const bool clicked = ImGui::Button(label, scaled_size);
	ImGui::PopStyleColor(5);
	ImGui::PopStyleVar(2);
	return clicked;
}

inline void DrawSoftDivider()
{
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float width = ImGui::GetContentRegionAvail().x;
	const float line_h = std::max(1.0f, ScaleUiLength(1.0f));
	const bool light = IsLightPaletteActive();
	const ImU32 left = ImGui::GetColorU32(light ? Rgb(9, 20, 39, 0.0f) : Rgb(255, 255, 255, 0.0f));
	const ImU32 center = ImGui::GetColorU32(light ? Rgb(15, 35, 62, 0.15f) : Rgb(255, 255, 255, 0.11f));
	draw->AddRectFilledMultiColor(p, ImVec2(p.x + width, p.y + line_h), left, center, center, left);
	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace12)));
}

inline std::string CompactPreview(const std::string& text, const std::size_t max_len)
{
	std::string compact = text;
	std::replace(compact.begin(), compact.end(), '\n', ' ');
	compact = Trim(compact);

	if (compact.size() <= max_len)
	{
		return compact;
	}

	if (max_len < 4)
	{
		return compact.substr(0, max_len);
	}

	return compact.substr(0, max_len - 3) + "...";
}
