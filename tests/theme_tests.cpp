#include "test_harness.h"

#include "app/theme_service.h"

using namespace uam_test;

namespace
{
	nlohmann::json TestTheme(std::string id = "custom:midnight", std::string name = "Midnight")
	{
		return {
		    {"version", 1},
		    {"id", std::move(id)},
		    {"name", std::move(name)},
		    {"base", "dark"},
		    {"colors", {
		                   {"background", "#101218"},
		                   {"surface", "#171a22"},
		                   {"surfaceUp", "#202532"},
		                   {"text", "#f3f4f6"},
		                   {"textMuted", "#a5adbd"},
		                   {"accent", "#ff6a00"},
		                   {"sidebar", "#12151c"},
		                   {"userMessage", "#2b211b"},
		                   {"assistantMessage", "#171a22"},
		                   {"success", "#4ade80"},
		                   {"warning", "#facc15"},
		                   {"error", "#f87171"},
		               }},
		};
	}
} // namespace

UAM_TEST(ThemeServiceSavesListsAndDeletesValidatedThemes)
{
	TempDir temp("uam-themes");
	nlohmann::json normalized;
	std::string error;
	UAM_ASSERT(ThemeService::Save(temp.root, TestTheme(), &normalized, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(normalized["id"].get<std::string>(), std::string("custom:midnight"));
	UAM_ASSERT(fs::exists(ThemeService::ThemesRootPath(temp.root) / "midnight.json"));
	UAM_ASSERT(ThemeService::Exists(temp.root, "custom:midnight"));

	const nlohmann::json themes = ThemeService::List(temp.root);
	UAM_ASSERT_EQ(themes.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(themes[0]["name"].get<std::string>(), std::string("Midnight"));

	UAM_ASSERT(ThemeService::Delete(temp.root, "custom:midnight", &error));
	UAM_ASSERT(!ThemeService::Exists(temp.root, "custom:midnight"));
}

UAM_TEST(ThemeServiceRejectsTraversalAndInvalidColors)
{
	TempDir temp("uam-theme-invalid");
	std::string error;
	UAM_ASSERT(!ThemeService::Save(temp.root, TestTheme("custom:../outside"), nullptr, &error));
	UAM_ASSERT(!error.empty());

	nlohmann::json invalid_color = TestTheme();
	invalid_color["colors"]["accent"] = "orange";
	error.clear();
	UAM_ASSERT(!ThemeService::Save(temp.root, invalid_color, nullptr, &error));
	UAM_ASSERT(error.find("#RRGGBB") != std::string::npos);

	nlohmann::json invalid_type = TestTheme();
	invalid_type["name"] = 42;
	error.clear();
	UAM_ASSERT(!ThemeService::Save(temp.root, invalid_type, nullptr, &error));
	UAM_ASSERT(!error.empty());
}

UAM_TEST(CustomThemeIdsSurviveSettingsNormalization)
{
	UAM_ASSERT_EQ(uam::settings::NormalizeThemeId(" CUSTOM:Midnight "), std::string("custom:midnight"));
	UAM_ASSERT_EQ(uam::settings::NormalizeThemeId("custom:../bad"), std::string(uam::settings::kFocusThemeId));
	UAM_ASSERT_EQ(uam::settings::NormalizeThemeId("mono"), std::string(uam::settings::kFocusThemeId));
}
