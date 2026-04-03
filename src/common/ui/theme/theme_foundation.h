#pragma once

/// <summary>
/// Core UI color helpers, spacing constants, and shared UI enums.
/// </summary>
inline ImVec4 Rgb(const int r, const int g, const int b, const float a = 1.0f)
{
	return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a);
}

namespace ui
{
	constexpr float kSpace4 = 4.0f;
	constexpr float kSpace6 = 6.0f;
	constexpr float kSpace8 = 8.0f;
	constexpr float kSpace10 = 10.0f;
	constexpr float kSpace12 = 12.0f;
	constexpr float kSpace16 = 16.0f;
	constexpr float kSpace20 = 20.0f;
	constexpr float kSpace24 = 24.0f;
	constexpr float kSpace32 = 32.0f;

	constexpr float kSidebarWidth = 316.0f;
	constexpr float kRightPanelWidth = 348.0f;
	constexpr float kTopBarHeight = 78.0f;

	constexpr float kRadiusSmall = 8.0f;
	constexpr float kRadiusPanel = 12.0f;
	constexpr float kRadiusInput = 10.0f;

	ImVec4 kMainBackground = Rgb(13, 17, 24, 1.0f);
	ImVec4 kPrimarySurface = Rgb(20, 25, 34, 0.97f);
	ImVec4 kSecondarySurface = Rgb(24, 29, 39, 0.97f);
	ImVec4 kElevatedSurface = Rgb(30, 36, 47, 0.99f);
	ImVec4 kInputSurface = Rgb(17, 22, 31, 0.98f);
	ImVec4 kBorder = Rgb(255, 255, 255, 0.08f);
	ImVec4 kBorderStrong = Rgb(115, 171, 255, 0.38f);
	ImVec4 kShadow = Rgb(0, 0, 0, 0.40f);
	ImVec4 kShadowSoft = Rgb(0, 0, 0, 0.26f);

	ImVec4 kTextPrimary = Rgb(232, 237, 245, 1.0f);
	ImVec4 kTextSecondary = Rgb(176, 187, 202, 1.0f);
	ImVec4 kTextMuted = Rgb(130, 144, 162, 1.0f);

	ImVec4 kAccent = Rgb(94, 160, 255, 1.0f);
	ImVec4 kAccentSoft = Rgb(94, 160, 255, 0.24f);
	ImVec4 kSuccess = Rgb(34, 197, 94, 1.0f);
	ImVec4 kError = Rgb(255, 107, 107, 1.0f);
	ImVec4 kWarning = Rgb(245, 158, 11, 1.0f);
	ImVec4 kTransparent = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
} // namespace ui

enum class UiThemeResolved
{
	Dark,
	Light
};

enum class PanelTone
{
	Primary,
	Secondary,
	Elevated
};

enum class ButtonKind
{
	Primary,
	Ghost,
	Accent
};
