#pragma once

#include "common/ui/modals/modal_app_settings_appearance_section.h"
#include "common/ui/modals/modal_app_settings_behavior_section.h"
#include "common/ui/modals/modal_app_settings_commit_section.h"
#include "common/ui/modals/modal_app_settings_compatibility_section.h"
#include "common/ui/modals/modal_app_settings_diagnostics_section.h"
#include "common/ui/modals/modal_app_settings_shortcuts_section.h"
#include "common/ui/modals/modal_app_settings_startup_section.h"
#include "common/ui/modals/modal_app_settings_templates_section.h"

// ---------------------------------------------------------------------------
// Settings page identifiers
// These are stable integer IDs — order in the nav is controlled separately.
// ---------------------------------------------------------------------------
namespace settings_page
{
	constexpr int kAppearance     = 0;
	constexpr int kGeneral        = 1;
	constexpr int kProvider       = 2;
	constexpr int kPromptProfiles  = 3;
	constexpr int kStartup        = 4;
	constexpr int kShortcuts      = 5;
	constexpr int kDiagnostics    = 6;
	constexpr int kVectorDB       = 7;
	constexpr int kCompatibility  = 8;
} // namespace settings_page

// ---------------------------------------------------------------------------
// Nav item helper
// Draws a single sidebar entry with full-width hit area, accent indicator,
// and hover/selected visual states — matching macOS Settings style.
// ---------------------------------------------------------------------------
inline void DrawSettingsNavItem(const char* label, const int page_id, int& selected_page)
{
	const bool selected   = (selected_page == page_id);
	const bool light      = IsLightPaletteActive();
	const float item_h    = ScaleUiLength(27.0f);
	const float full_w    = ImGui::GetContentRegionAvail().x;
	const float txt_x     = ScaleUiLength(14.0f);
	const float rounding  = ScaleUiLength(5.0f);
	const float bar_w     = ScaleUiLength(3.0f);
	const float bar_inset = ScaleUiLength(6.0f);

	const ImU32 col_sel  = ImGui::GetColorU32(light ? Rgb(66, 126, 228, 0.15f) : Rgb(94, 160, 255, 0.15f));
	const ImU32 col_hov  = ImGui::GetColorU32(light ? Rgb(7, 24, 56, 0.07f)   : Rgb(255, 255, 255, 0.08f));
	const ImU32 col_txt  = ImGui::GetColorU32(selected ? ui::kAccent : ui::kTextSecondary);
	const ImU32 col_bar  = ImGui::GetColorU32(ui::kAccent);

	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImDrawList*  dl  = ImGui::GetWindowDrawList();

	// Unique ID for InvisibleButton uses the label — page labels are unique.
	ImGui::InvisibleButton(label, ImVec2(full_w, item_h));
	const bool hov     = ImGui::IsItemHovered();
	const bool clicked = ImGui::IsItemClicked();

	if (clicked)
		selected_page = page_id;

	// Background fill
	if (selected)
		dl->AddRectFilled(pos, ImVec2(pos.x + full_w, pos.y + item_h), col_sel, rounding);
	else if (hov)
		dl->AddRectFilled(pos, ImVec2(pos.x + full_w, pos.y + item_h), col_hov, rounding);

	// Accent bar on the left edge when selected
	if (selected)
	{
		dl->AddRectFilled(
			ImVec2(pos.x + ScaleUiLength(2.0f), pos.y + bar_inset),
			ImVec2(pos.x + ScaleUiLength(2.0f) + bar_w, pos.y + item_h - bar_inset),
			col_bar,
			ScaleUiLength(2.0f)
		);
	}

	// Label, vertically centred
	dl->AddText(
		ImVec2(pos.x + txt_x, pos.y + (item_h - ImGui::GetFontSize()) * 0.5f),
		col_txt,
		label
	);

	// No extra gap — item height provides enough visual breathing room
}

// ---------------------------------------------------------------------------
// Nav section header (non-interactive group label inside the sidebar)
// ---------------------------------------------------------------------------
inline void DrawSettingsNavSectionLabel(const char* label)
{
	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(4.0f)));
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float  tx  = ScaleUiLength(8.0f);
	ImGui::GetWindowDrawList()->AddText(
		ImVec2(pos.x + tx, pos.y),
		ImGui::GetColorU32(ui::kTextMuted),
		label
	);
	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(6.0f)));
}

// ---------------------------------------------------------------------------
// Page title
// ---------------------------------------------------------------------------
inline const char* GetSettingsPageTitle(const int page_id)
{
	switch (page_id)
	{
	case settings_page::kAppearance:    return "Appearance";
	case settings_page::kGeneral:       return "General";
	case settings_page::kProvider:      return "Provider & Models";
	case settings_page::kPromptProfiles: return "Prompt Profiles";
	case settings_page::kStartup:       return "Startup & Window";
	case settings_page::kShortcuts:     return "Keyboard Shortcuts";
	case settings_page::kDiagnostics:   return "Diagnostics";
	case settings_page::kVectorDB:      return "Vector Retrieval";
	case settings_page::kCompatibility: return "CLI Compatibility";
	default:                            return "Settings";
	}
}

// ---------------------------------------------------------------------------
// Main settings modal
// macOS-style: fixed-size window, sidebar nav on the left, scrollable content
// on the right, Save / Cancel bar at the bottom.
// ---------------------------------------------------------------------------
inline void DrawAppSettingsModal(AppState& app, const float platform_scale)
{
	static AppSettings draft_settings{};
	static bool        initialized   = false;
	static int         selected_page = settings_page::kAppearance;

	// ---- Open trigger ----
	if (app.open_app_settings_popup)
	{
		draft_settings = app.settings;

		if (Trim(draft_settings.prompt_profile_root_path).empty())
			draft_settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();

		draft_settings.ui_theme = NormalizeThemeChoice(draft_settings.ui_theme);
		initialized   = true;
		selected_page = settings_page::kAppearance;

		ImGui::OpenPopup("Settings##app_settings");
		app.open_app_settings_popup = false;
		ProviderCliCompatibilityService().StartVersionCheck(app, true);
	}

	// ---- Size and position (centred) ----
	const ImVec2 modal_size = ScaleUiSize(ImVec2(820.0f, 560.0f));
	ImGui::SetNextWindowSize(modal_size, ImGuiCond_Always);
	ImGui::SetNextWindowPos(
		ImGui::GetMainViewport()->GetCenter(),
		ImGuiCond_Always,
		ImVec2(0.5f, 0.5f)
	);

	constexpr ImGuiWindowFlags kModalFlags =
		ImGuiWindowFlags_NoResize    |
		ImGuiWindowFlags_NoMove      |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoSavedSettings;

	if (!ImGui::BeginPopupModal("Settings##app_settings", nullptr, kModalFlags))
		return;

	// ---- Lazy init (opened via shortcut before trigger branch ran) ----
	if (!initialized)
	{
		draft_settings = app.settings;

		if (Trim(draft_settings.prompt_profile_root_path).empty())
			draft_settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();

		draft_settings.ui_theme = NormalizeThemeChoice(draft_settings.ui_theme);
		initialized = true;
		ProviderCliCompatibilityService().StartVersionCheck(app, false);
	}

	TemplateRuntimeService().RefreshTemplateCatalog(app);

	const bool  light        = IsLightPaletteActive();
	const float nav_w        = ScaleUiLength(170.0f);
	const float gap          = ScaleUiLength(ui::kSpace8);
	const float btn_area_h   = ScaleUiLength(46.0f);
	const float panel_h      = ImGui::GetContentRegionAvail().y - btn_area_h;
	const float content_w    = ImGui::GetContentRegionAvail().x - nav_w - gap;

	// =========================================================================
	//  Left sidebar nav
	// =========================================================================
	{
		const ImVec4 nav_bg = light
			? Rgb(238, 242, 250, 1.0f)
			: Rgb(16, 20, 28, 0.98f);

		ImGui::PushStyleColor(ImGuiCol_ChildBg, nav_bg);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ScaleUiLength(ui::kRadiusSmall));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ScaleUiSize(ImVec2(ui::kSpace4, ui::kSpace4)));
		ImGui::BeginChild("##settings_nav", ImVec2(nav_w, panel_h), false,
			ImGuiWindowFlags_NoScrollbar);

		ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace4)));
		DrawSettingsNavSectionLabel("DISPLAY");
		DrawSettingsNavItem("Appearance",        settings_page::kAppearance,     selected_page);
		DrawSettingsNavItem("Startup & Window",  settings_page::kStartup,        selected_page);

		DrawSettingsNavSectionLabel("BEHAVIOUR");
		DrawSettingsNavItem("General",           settings_page::kGeneral,        selected_page);
		DrawSettingsNavItem("Provider & Models", settings_page::kProvider,       selected_page);

#if UAM_ENABLE_ENGINE_RAG
		DrawSettingsNavItem("Vector Retrieval",  settings_page::kVectorDB,       selected_page);
#endif

		DrawSettingsNavSectionLabel("CONTENT");
		DrawSettingsNavItem("Prompt Profiles",   settings_page::kPromptProfiles, selected_page);

#if UAM_ENABLE_ANY_GEMINI_PROVIDER
		DrawSettingsNavItem("CLI Compatibility", settings_page::kCompatibility,  selected_page);
#endif

		DrawSettingsNavSectionLabel("INFO");
		DrawSettingsNavItem("Shortcuts",         settings_page::kShortcuts,      selected_page);
		DrawSettingsNavItem("Diagnostics",       settings_page::kDiagnostics,    selected_page);

		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();
	}

	ImGui::SameLine(0.0f, gap);

	// =========================================================================
	//  Right content panel
	// =========================================================================
	{
		const ImVec4 content_bg = light
			? Rgb(248, 250, 255, 1.0f)
			: ui::kPrimarySurface;

		ImGui::PushStyleColor(ImGuiCol_ChildBg, content_bg);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ScaleUiLength(ui::kRadiusSmall));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ScaleUiSize(ImVec2(ui::kSpace20, ui::kSpace16)));
		ImGui::BeginChild("##settings_content", ImVec2(content_w, panel_h), false);

		// Page title
		PushFontIfAvailable(g_font_title);
		ImGui::TextColored(ui::kTextPrimary, "%s", GetSettingsPageTitle(selected_page));
		PopFontIfAvailable(g_font_title);
		ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace6)));
		DrawSoftDivider();

		// Page content
		switch (selected_page)
		{
		case settings_page::kAppearance:
			DrawAppSettingsAppearanceSection(app, draft_settings, platform_scale);
			break;

		case settings_page::kGeneral:
			DrawAppSettingsBehaviorControls(draft_settings);
			break;

		case settings_page::kProvider:
			DrawAppSettingsProviderControls(app, draft_settings);
			break;

#if UAM_ENABLE_ENGINE_RAG
		case settings_page::kVectorDB:
			DrawAppSettingsVectorRetrievalControls(app, draft_settings);
			break;
#endif

		case settings_page::kPromptProfiles:
			DrawAppSettingsTemplatesSection(app, draft_settings);
			break;

#if UAM_ENABLE_ANY_GEMINI_PROVIDER
		case settings_page::kCompatibility:
		{
			const ProviderProfile* ap = ProviderProfileStore::FindById(
				app.provider_profiles, draft_settings.active_provider_id);
			if (ap != nullptr && ProviderProfileMigrationService().IsNativeHistoryProviderId(ap->id))
				DrawAppSettingsCompatibilitySection(app);
			else
				ImGui::TextColored(ui::kTextMuted,
					"Gemini compatibility checks are shown when\n"
					"a Gemini provider is the active provider.");
			break;
		}
#endif

		case settings_page::kStartup:
			DrawAppSettingsStartupSection(draft_settings);
			break;

		case settings_page::kShortcuts:
			DrawAppSettingsShortcutsSection();
			break;

		case settings_page::kDiagnostics:
			DrawAppSettingsDiagnosticsSection(app, draft_settings);
			break;

		default:
			break;
		}

		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();
	}

	// =========================================================================
	//  Bottom action bar
	// =========================================================================
	DrawSoftDivider();
	DrawAppSettingsCommitSection(app, draft_settings, platform_scale, initialized);

	ImGui::EndPopup();
}
