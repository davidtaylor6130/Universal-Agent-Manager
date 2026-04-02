#pragma once

/// <summary>
/// Theme choice normalization, system detection, and palette application.
/// </summary>
static std::string ToLowerCopy(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return value;
}

static std::string NormalizeThemeChoice(std::string value)
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

static bool ContainsInsensitive(const std::string& haystack, const std::string& needle)
{
	const std::string lowered_haystack = ToLowerCopy(haystack);
	const std::string lowered_needle = ToLowerCopy(needle);
	return lowered_haystack.find(lowered_needle) != std::string::npos;
}

static std::optional<bool> DetectSystemPrefersLightTheme()
{
#if defined(_WIN32)
	DWORD value = 1;
	DWORD value_size = sizeof(value);
	const LONG rc = RegGetValueA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", "AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &value_size);

	if (rc == ERROR_SUCCESS)
	{
		return value != 0;
	}

	return std::nullopt;
#elif defined(__APPLE__)

	if (const char* env_style = std::getenv("AppleInterfaceStyle"))
	{
		return ContainsInsensitive(env_style, "dark") ? std::optional<bool>(false) : std::optional<bool>(true);
	}

	FILE* pipe = popen("defaults read -g AppleInterfaceStyle 2>/dev/null", "r");

	if (pipe == nullptr)
	{
		return std::nullopt;
	}

	std::array<char, 128> buffer{};
	std::string output;

	while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
	{
		output += buffer.data();
	}

	pclose(pipe);
	const std::string trimmed = Trim(output);

	if (trimmed.empty())
	{
		return std::nullopt;
	}

	return ContainsInsensitive(trimmed, "dark") ? std::optional<bool>(false) : std::optional<bool>(true);
#else
	const std::array<const char*, 4> env_candidates = {"GTK_THEME", "QT_STYLE_OVERRIDE", "KDE_COLOR_SCHEME", "COLORFGBG"};

	for (const char* env_key : env_candidates)
	{
		const char* value = std::getenv(env_key);

		if (value == nullptr || *value == '\0')
		{
			continue;
		}

		const std::string text = value;

		if (ContainsInsensitive(text, "dark"))
		{
			return false;
		}

		if (ContainsInsensitive(text, "light"))
		{
			return true;
		}
	}

	return std::nullopt;
#endif
}

static UiThemeResolved ResolveUiTheme(const AppSettings& settings)
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

	if (const std::optional<bool> system_prefers_light = DetectSystemPrefersLightTheme(); system_prefers_light.has_value())
	{
		return system_prefers_light.value() ? UiThemeResolved::Light : UiThemeResolved::Dark;
	}

	return UiThemeResolved::Dark;
}

static void ApplyResolvedPalette(const UiThemeResolved theme)
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
