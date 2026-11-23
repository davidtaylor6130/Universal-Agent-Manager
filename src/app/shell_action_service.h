#pragma once

#include "common/state/app_state.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class ShellActionService
{
  public:
	/// The one-line warning Load gives when the saved file exists but cannot be read or parsed.
	static constexpr std::string_view kUnreadableWarning = "Saved shell actions could not be read. The file is kept, and set aside when shell actions are next saved.";
	/// Saved entries that fail validation are kept in place as disabled, read-only actions carrying a one-line `invalid_reason`;
	/// `warning_out` receives a one-line summary when any were kept or the saved file could not be read; `unreadable_out`
	/// reports the latter, which the next save must pass on as `set_current_aside`.
	static std::vector<ShellAction> Load(const std::filesystem::path& data_root, const std::filesystem::path& default_skills_root = {}, std::string* warning_out = nullptr, bool* unreadable_out = nullptr);
	/// Writes kept invalid entries back from their saved form (disabled). A saved file that does not parse, or any existing file when
	/// `set_current_aside` is true (the last load could not read it), is copied aside before it is replaced.
	static bool Save(const std::filesystem::path& data_root, const std::vector<ShellAction>& actions, std::string* error_out = nullptr, bool set_current_aside = false);
	/// The list to save from the interface's full list: an excluded provider's actions, which the interface never sees, keep their
	/// places, and each kept invalid entry the interface returns is saved exactly as loaded rather than as the interface sent it.
	static std::vector<ShellAction> MergeInterfaceList(const std::vector<ShellAction>& current, std::vector<ShellAction> submitted);
	static bool Apply(const uam::AppState& app, const std::filesystem::path& executable, std::string* error_out = nullptr);
	static bool QueueLaunchRequest(const std::filesystem::path& data_root, const std::vector<std::string>& args, bool* recognized_out, std::string* error_out = nullptr);
	static bool ProcessPendingRequests(uam::AppState& app);

	/// Finder lists Quick Actions in one flat menu, so the installed name carries the group path ("Group › Sub › Label").
	static std::string FinderMenuTitle(const ShellAction& action);
	/// True when a selected path contains a line break or other control character, which a skill prompt never receives.
	static bool PathHasControlCharacters(std::string_view utf8_path);
	/// The skill prompt: each selected path is a JSON string on its own line, marked as data rather than instructions.
	static std::string SkillPrompt(const std::vector<std::string>& utf8_paths);
};
