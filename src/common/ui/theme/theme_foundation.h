#ifndef UAM_COMMON_UI_THEME_THEME_FOUNDATION_H
#define UAM_COMMON_UI_THEME_THEME_FOUNDATION_H

/// <summary>
/// Core UI color helpers, spacing constants, and shared UI enums.
/// ImGui removed — UI rendering is handled by React/CEF. Color values kept for
/// persistence and theme-string → enum resolution that C++ still owns.
/// </summary>

struct RgbaColor
{
	float r, g, b, a;
};

inline RgbaColor Rgb(const int r, const int g, const int b, const float a = 1.0f)
{
	return RgbaColor{
		static_cast<float>(r) / 255.0f,
		static_cast<float>(g) / 255.0f,
		static_cast<float>(b) / 255.0f,
		a
	};
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

	inline RgbaColor kMainBackground = Rgb(13, 17, 24, 1.0f);
	inline RgbaColor kPrimarySurface = Rgb(20, 25, 34, 0.97f);
	inline RgbaColor kSecondarySurface = Rgb(24, 29, 39, 0.97f);
	inline RgbaColor kElevatedSurface = Rgb(30, 36, 47, 0.99f);
	inline RgbaColor kInputSurface = Rgb(17, 22, 31, 0.98f);
	inline RgbaColor kBorder = Rgb(255, 255, 255, 0.08f);
	inline RgbaColor kBorderStrong = Rgb(115, 171, 255, 0.38f);
	inline RgbaColor kShadow = Rgb(0, 0, 0, 0.40f);
	inline RgbaColor kShadowSoft = Rgb(0, 0, 0, 0.26f);

	inline RgbaColor kTextPrimary = Rgb(232, 237, 245, 1.0f);
	inline RgbaColor kTextSecondary = Rgb(176, 187, 202, 1.0f);
	inline RgbaColor kTextMuted = Rgb(130, 144, 162, 1.0f);

	inline RgbaColor kAccent = Rgb(94, 160, 255, 1.0f);
	inline RgbaColor kAccentSoft = Rgb(94, 160, 255, 0.24f);
	inline RgbaColor kSuccess = Rgb(34, 197, 94, 1.0f);
	inline RgbaColor kError = Rgb(255, 107, 107, 1.0f);
	inline RgbaColor kWarning = Rgb(245, 158, 11, 1.0f);
	inline RgbaColor kTransparent = RgbaColor{0.0f, 0.0f, 0.0f, 0.0f};
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

#endif // UAM_COMMON_UI_THEME_THEME_FOUNDATION_H
