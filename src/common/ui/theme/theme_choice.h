#ifndef UAM_COMMON_UI_THEME_THEME_CHOICE_H
#define UAM_COMMON_UI_THEME_THEME_CHOICE_H

/// <summary>
/// Theme choice normalization, system detection, and palette application.
/// </summary>
#include "common/platform/platform_services.h"

inline std::string ToLowerCopy(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return value;
}

inline std::string NormalizeThemeChoice(std::string value)
{
	value = ToLowerCopy(std::move(value));

	if (value == "light")
	{
		return "light";
	}

	if (value == "system")
	{
		return "system";
	}

	return "dark";
}

inline UiThemeResolved ResolveUiTheme(const AppSettings& settings)
{
	const std::string mode = NormalizeThemeChoice(settings.ui_theme);

	if (mode == "light")
	{
		return UiThemeResolved::Light;
	}

	if (mode == "dark")
	{
		return UiThemeResolved::Dark;
	}

	if (const std::optional<bool> system_prefers_light = PlatformServicesFactory::Instance().ui_traits.DetectSystemPrefersLightTheme(); system_prefers_light.has_value())
	{
		return system_prefers_light.value() ? UiThemeResolved::Light : UiThemeResolved::Dark;
	}

	return UiThemeResolved::Dark;
}

inline void ApplyResolvedPalette(const UiThemeResolved theme)
{
	if (theme == UiThemeResolved::Light)
	{
		ui::kMainBackground = Rgb(240, 244, 250, 1.0f);
		ui::kPrimarySurface = Rgb(254, 255, 255, 0.98f);
		ui::kSecondarySurface = Rgb(246, 249, 255, 0.98f);
		ui::kElevatedSurface = Rgb(236, 242, 251, 0.99f);
		ui::kInputSurface = Rgb(249, 252, 255, 1.0f);
		ui::kBorder = Rgb(26, 44, 71, 0.14f);
		ui::kBorderStrong = Rgb(72, 133, 233, 0.48f);
		ui::kShadow = Rgb(9, 20, 39, 0.16f);
		ui::kShadowSoft = Rgb(9, 20, 39, 0.10f);
		ui::kTextPrimary = Rgb(18, 31, 51, 1.0f);
		ui::kTextSecondary = Rgb(70, 88, 114, 1.0f);
		ui::kTextMuted = Rgb(99, 118, 144, 1.0f);
		ui::kAccent = Rgb(66, 126, 228, 1.0f);
		ui::kAccentSoft = Rgb(66, 126, 228, 0.18f);
		ui::kSuccess = Rgb(22, 163, 74, 1.0f);
		ui::kError = Rgb(220, 38, 38, 1.0f);
		ui::kWarning = Rgb(217, 119, 6, 1.0f);
		return;
	}

	ui::kMainBackground = Rgb(13, 17, 24, 1.0f);
	ui::kPrimarySurface = Rgb(20, 25, 34, 0.97f);
	ui::kSecondarySurface = Rgb(24, 29, 39, 0.97f);
	ui::kElevatedSurface = Rgb(30, 36, 47, 0.99f);
	ui::kInputSurface = Rgb(17, 22, 31, 0.98f);
	ui::kBorder = Rgb(255, 255, 255, 0.08f);
	ui::kBorderStrong = Rgb(115, 171, 255, 0.38f);
	ui::kShadow = Rgb(0, 0, 0, 0.40f);
	ui::kShadowSoft = Rgb(0, 0, 0, 0.26f);
	ui::kTextPrimary = Rgb(232, 237, 245, 1.0f);
	ui::kTextSecondary = Rgb(176, 187, 202, 1.0f);
	ui::kTextMuted = Rgb(130, 144, 162, 1.0f);
	ui::kAccent = Rgb(94, 160, 255, 1.0f);
	ui::kAccentSoft = Rgb(94, 160, 255, 0.24f);
	ui::kSuccess = Rgb(34, 197, 94, 1.0f);
	ui::kError = Rgb(255, 107, 107, 1.0f);
	ui::kWarning = Rgb(245, 158, 11, 1.0f);
}

#endif // UAM_COMMON_UI_THEME_THEME_CHOICE_H
