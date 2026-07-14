#include "app/shell_action_service.h"

#include "app/chat_domain_service.h"
#include "app/chat_lifecycle_service.h"
#include "app/markdown_store_service.h"
#include "app/persistence_coordinator.h"
#include "common/chat/chat_repository.h"
#include "common/config/provider_chat_defaults.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/utils/env_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <random>
#include <set>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{
	namespace fs = std::filesystem;

	constexpr std::string_view kLaunchFlag = "--uam-shell-action";
	constexpr std::string_view kMacWorkflowPrefix = "Universal Agent Manager - ";
	constexpr std::string_view kWindowsKeyPrefix = "UAM_";

	fs::path ActionsPath(const fs::path& data_root)
	{
		return data_root / "shell_actions.json";
	}

	fs::path RequestsPath(const fs::path& data_root)
	{
		return data_root / "shell-action-requests";
	}

	std::string SafeId(std::string_view value)
	{
		std::string result;
		for (const unsigned char c : uam::strings::Trim(value))
		{
			if (std::isalnum(c) || c == '-' || c == '_')
			{
				result.push_back(static_cast<char>(c));
			}
		}
		return result;
	}

	std::string SafeFileName(std::string_view value)
	{
		std::string result;
		for (const unsigned char c : uam::strings::Trim(value))
		{
			if (c >= 0x20 && c != '/' && c != '\\' && c != ':')
			{
				result.push_back(static_cast<char>(c));
			}
		}
		return result.empty() ? "Action" : result;
	}

	std::string XmlEscape(std::string_view value)
	{
		std::string result;
		result.reserve(value.size());
		for (const char c : value)
		{
			switch (c)
			{
			case '&': result += "&amp;"; break;
			case '<': result += "&lt;"; break;
			case '>': result += "&gt;"; break;
			case '\"': result += "&quot;"; break;
			case '\'': result += "&apos;"; break;
			default: result.push_back(c); break;
			}
		}
		return result;
	}

	std::string ShellQuote(std::string_view value)
	{
		std::string result = "'";
		for (const char c : value)
		{
			if (c == '\'')
			{
				result += "'\\''";
			}
			else
			{
				result.push_back(c);
			}
		}
		return result + "'";
	}

	bool NormalizeAction(const ShellAction& source, ShellAction& result, std::string* error_out)
	{
		result = source;
		result.id = SafeId(source.id);
		result.label = uam::strings::Trim(source.label);
		result.skill_path = uam::strings::Trim(source.skill_path);
		result.provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(source.provider_id);
		result.model_id = uam::strings::Trim(source.model_id);
		if (result.id.empty() || result.label.empty())
		{
			if (error_out != nullptr) *error_out = "Every shell action requires an id and label.";
			return false;
		}
		if (!result.accepts_files && !result.accepts_folders)
		{
			if (error_out != nullptr) *error_out = "Shell action '" + result.label + "' must accept files, folders, or both.";
			return false;
		}
		if (result.enabled && !result.open_workspace && result.skill_path.empty())
		{
			if (error_out != nullptr) *error_out = "Shell action '" + result.label + "' requires a skill file.";
			return false;
		}
		if (!result.provider_id.empty() && !uam::provider_ids::IsKnownCliProviderId(result.provider_id))
		{
			if (error_out != nullptr) *error_out = "Shell action '" + result.label + "' has an unsupported provider.";
			return false;
		}
		if (!uam::provider_chat_defaults::IsAllowedModelId(result.model_id))
		{
			if (error_out != nullptr) *error_out = "Shell action '" + result.label + "' has an invalid model.";
			return false;
		}
		return true;
	}

	nlohmann::json ActionJson(const ShellAction& action)
	{
		return {
			{"id", action.id}, {"label", action.label}, {"skillPath", action.skill_path},
			{"providerId", action.provider_id}, {"modelId", action.model_id},
			{"acceptsFiles", action.accepts_files}, {"acceptsFolders", action.accepts_folders},
			{"enabled", action.enabled}, {"openWorkspace", action.open_workspace}
		};
	}

	std::string MacWorkflowDocument(const ShellAction& action, const fs::path& executable)
	{
		const std::string command = ShellQuote(executable.string()) + " " + std::string(kLaunchFlag) + " " + ShellQuote(action.id) + " \"$@\"";
		const std::string input_type = action.accepts_files && action.accepts_folders ? "com.apple.Automator.fileSystemObject" :
		                               action.accepts_folders ? "com.apple.Automator.folder" : "com.apple.Automator.file";
		return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		       "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
		       "<plist version=\"1.0\"><dict>"
		       "<key>AMApplicationBuild</key><string>512</string>"
		       "<key>AMApplicationVersion</key><string>2.10</string>"
		       "<key>AMDocumentVersion</key><string>2</string>"
		       "<key>actions</key><array><dict>"
		       "<key>action</key><dict><key>AMAccepts</key><dict><key>Container</key><string>List</string><key>Optional</key><true/><key>Types</key><array><string>com.apple.cocoa.string</string></array></dict>"
		       "<key>AMActionVersion</key><string>2.0.3</string><key>AMApplication</key><array><string>Automator</string></array>"
		       "<key>AMParameterProperties</key><dict><key>COMMAND_STRING</key><dict/><key>CheckedForUserDefaultShell</key><dict/><key>inputMethod</key><dict/><key>shell</key><dict/><key>source</key><dict/></dict>"
		       "<key>AMProvides</key><dict><key>Container</key><string>List</string><key>Types</key><array><string>com.apple.cocoa.string</string></array></dict>"
		       "<key>ActionBundlePath</key><string>/System/Library/Automator/Run Shell Script.action</string>"
		       "<key>ActionName</key><string>Run Shell Script</string><key>ActionParameters</key><dict>"
		       "<key>COMMAND_STRING</key><string>" + XmlEscape(command) + "</string><key>CheckedForUserDefaultShell</key><true/>"
		       "<key>inputMethod</key><integer>1</integer><key>shell</key><string>/bin/zsh</string><key>source</key><string></string></dict>"
		       "<key>BundleIdentifier</key><string>com.apple.RunShellScript</string><key>CFBundleVersion</key><string>2.0.3</string>"
		       "<key>CanShowSelectedItemsWhenRun</key><false/><key>CanShowWhenRun</key><true/><key>Category</key><array><string>AMCategoryUtilities</string></array>"
		       "<key>Class Name</key><string>RunShellScriptAction</string><key>InputUUID</key><string>" + XmlEscape(action.id) + "-input</string>"
		       "<key>Keywords</key><array><string>Shell</string><string>Script</string><string>Command</string><string>Run</string><string>Unix</string></array>"
		       "<key>OutputUUID</key><string>" + XmlEscape(action.id) + "-output</string><key>UUID</key><string>" + XmlEscape(action.id) + "</string></dict>"
		       "<key>isViewVisible</key><true/></dict></array>"
		       "<key>connectors</key><dict/><key>workflowMetaData</key><dict>"
		       "<key>serviceApplicationBundleID</key><string></string><key>serviceApplicationPath</key><string></string>"
		       "<key>serviceInputTypeIdentifier</key><string>" + input_type + "</string>"
		       "<key>serviceOutputTypeIdentifier</key><string>com.apple.Automator.nothing</string>"
		       "<key>serviceProcessesInput</key><integer>0</integer><key>workflowTypeIdentifier</key><string>com.apple.Automator.servicesMenu</string>"
		       "</dict></dict></plist>\n";
	}

	std::string MacWorkflowInfo(const ShellAction& action)
	{
		const std::string input_type = action.accepts_files && action.accepts_folders ? "public.item" :
		                               action.accepts_folders ? "public.folder" : "public.data";
		return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		       "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
		       "<plist version=\"1.0\"><dict><key>NSServices</key><array><dict>"
		       "<key>NSMenuItem</key><dict><key>default</key><string>" + XmlEscape(action.label) + "</string></dict>"
		       "<key>NSMessage</key><string>runWorkflowAsService</string>"
		       "<key>NSRequiredContext</key><dict><key>NSApplicationIdentifier</key><string>com.apple.finder</string></dict>"
		       "<key>NSSendFileTypes</key><array><string>" + input_type + "</string></array>"
		       "</dict></array></dict></plist>\n";
	}

#if defined(_WIN32)
	std::wstring Wide(std::string_view value)
	{
		if (value.empty()) return {};
		const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
		std::wstring result(static_cast<std::size_t>(std::max(size, 0)), L'\0');
		if (size > 0) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
		return result;
	}

	bool SetRegistryString(HKEY key, const wchar_t* name, const std::wstring& value)
	{
		return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
	}

	bool InstallWindowsVerb(const ShellAction& action, const fs::path& executable, std::wstring_view kind, std::string* error_out)
	{
		const std::wstring key_path = L"Software\\Classes\\" + std::wstring(kind) + L"\\shell\\" + Wide(std::string(kWindowsKeyPrefix) + action.id);
		HKEY key = nullptr;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
		{
			if (error_out != nullptr) *error_out = "Failed to create the Windows shell action registry key.";
			return false;
		}
		const bool label_ok = SetRegistryString(key, nullptr, Wide(action.label));
		HKEY command_key = nullptr;
		const bool command_key_ok = RegCreateKeyExW(key, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &command_key, nullptr) == ERROR_SUCCESS;
		const std::wstring command = L"\"" + executable.wstring() + L"\" " + Wide(kLaunchFlag) + L" \"" + Wide(action.id) + L"\" \"%1\"";
		const bool command_ok = command_key_ok && SetRegistryString(command_key, nullptr, command);
		if (command_key != nullptr) RegCloseKey(command_key);
		RegCloseKey(key);
		if (!label_ok || !command_ok)
		{
			if (error_out != nullptr) *error_out = "Failed to configure the Windows shell action command.";
			return false;
		}
		return true;
	}

	void RemoveWindowsVerbs(std::wstring_view kind)
	{
		const std::wstring shell_path = L"Software\\Classes\\" + std::wstring(kind) + L"\\shell";
		HKEY shell = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, shell_path.c_str(), 0, KEY_READ | KEY_WRITE, &shell) != ERROR_SUCCESS) return;
		std::vector<std::wstring> keys;
		wchar_t name[512];
		for (DWORD index = 0;; ++index)
		{
			DWORD length = static_cast<DWORD>(std::size(name));
			const LSTATUS status = RegEnumKeyExW(shell, index, name, &length, nullptr, nullptr, nullptr, nullptr);
			if (status == ERROR_NO_MORE_ITEMS) break;
			if (status == ERROR_SUCCESS && std::wstring_view(name, length).starts_with(Wide(kWindowsKeyPrefix))) keys.emplace_back(name, length);
		}
		for (const std::wstring& key : keys) RegDeleteTreeW(shell, key.c_str());
		RegCloseKey(shell);
	}
#endif

	std::string FolderTitle(const fs::path& workspace)
	{
		const std::string title = uam::paths::Utf8PathString(workspace.filename());
		return title.empty() ? uam::paths::Utf8PathString(workspace) : title;
	}

	std::string EnsureWorkspace(uam::AppState& app, const fs::path& workspace)
	{
		for (const ChatFolder& folder : app.folders)
		{
			if (FolderDirectoryMatches(uam::paths::PathFromUtf8(folder.directory), workspace)) return folder.id;
		}
		std::string id;
		return CreateFolder(app, FolderTitle(workspace), uam::paths::Utf8PathString(workspace), &id) ? id : std::string();
	}

	bool RunRequest(uam::AppState& app, const nlohmann::json& request, std::string& status)
	{
		const std::string action_id = SafeId(request.value("actionId", ""));
		const auto action_it = std::find_if(app.shell_actions.begin(), app.shell_actions.end(), [&](const ShellAction& action) { return action.id == action_id; });
		if (action_it == app.shell_actions.end() || !action_it->enabled)
		{
			status = "Shell action is no longer available.";
			return false;
		}

		std::vector<fs::path> paths;
		if (request.contains("paths") && request["paths"].is_array())
		{
			for (const auto& value : request["paths"])
			{
				if (!value.is_string()) continue;
				const fs::path path = uam::paths::PathFromUtf8(value.get<std::string>());
				const bool directory = uam::paths::IsDirectoryNoThrow(path);
				const bool file = uam::paths::IsRegularFileNoThrow(path);
				if ((!directory && !file) || (directory && !action_it->accepts_folders) || (file && !action_it->accepts_files))
				{
					status = "Shell action '" + action_it->label + "' cannot use: " + uam::paths::Utf8PathString(path);
					return false;
				}
				paths.push_back(uam::paths::AbsolutePathNoThrow(path));
			}
		}
		if (paths.empty())
		{
			status = "Shell action did not receive a valid file or folder.";
			return false;
		}

		const fs::path workspace = uam::paths::IsDirectoryNoThrow(paths.front()) ? paths.front() : paths.front().parent_path();
		fs::path skill_path;
		if (!action_it->open_workspace)
		{
			const fs::path store_root = MarkdownStoreService::NormalizeRoot(app.settings.markdown_store_directory);
			std::string error;
			if (!MarkdownStoreService::ValidateStoreFilePath(store_root, action_it->skill_path, &skill_path, &error))
			{
				status = "Shell action skill is unavailable: " + error;
				return false;
			}
		}
		const std::string folder_id = EnsureWorkspace(app, workspace);
		if (folder_id.empty())
		{
			status = "Failed to open the selected workspace.";
			return false;
		}

		ChatSession chat = ChatDomainService().CreateNewChat(
		    folder_id,
		    action_it->provider_id.empty() ? app.settings.default_new_chat_provider_id : action_it->provider_id);
		chat.title = action_it->open_workspace ? FolderTitle(workspace) : action_it->label;
		chat.workspace_directory = uam::paths::Utf8PathString(workspace);
		uam::provider_chat_defaults::ApplyToChat(app.settings, chat);
		if (!action_it->model_id.empty()) chat.model_id = action_it->model_id;
		app.chats.push_back(std::move(chat));
		ChatSession& created = app.chats.back();
		ChatDomainService().SelectChatById(app, created.id);
		if (!ChatRepository::SaveChat(app.data_root, created) || !PersistenceCoordinator().SaveSettings(app))
		{
			status = "Opened the workspace, but failed to persist its chat.";
			return false;
		}

		if (action_it->open_workspace)
		{
			status = "Opened workspace: " + uam::paths::Utf8PathString(workspace);
			return true;
		}

		std::string error;
		std::ostringstream prompt;
		prompt << "Run the referenced skill for the following selected path" << (paths.size() == 1 ? ":" : "s:") << '\n';
		for (const fs::path& path : paths) prompt << "- " << uam::paths::Utf8PathString(path) << '\n';
		if (!uam::SendAcpPrompt(app, created.id, prompt.str(), {uam::paths::Utf8PathString(skill_path)}, {}, false, &error))
		{
			status = "Failed to run shell action '" + action_it->label + "': " + error;
			return false;
		}
		status = "Started shell action: " + action_it->label;
		return true;
	}
}

std::vector<ShellAction> ShellActionService::Load(const std::filesystem::path& data_root)
{
	std::vector<ShellAction> result;
	const std::string text = uam::io::ReadTextFile(ActionsPath(data_root));
	if (text.empty()) return result;
	try
	{
		const nlohmann::json root = nlohmann::json::parse(text);
		if (!root.is_array()) return result;
		std::set<std::string> ids;
		for (const auto& value : root)
		{
			if (!value.is_object()) continue;
			ShellAction raw;
			raw.id = value.value("id", "");
			raw.label = value.value("label", "");
			raw.skill_path = value.value("skillPath", "");
			raw.provider_id = value.value("providerId", "");
			raw.model_id = value.value("modelId", "");
			raw.accepts_files = value.value("acceptsFiles", true);
			raw.accepts_folders = value.value("acceptsFolders", true);
			raw.enabled = value.value("enabled", true);
			raw.open_workspace = value.value("openWorkspace", false);
			ShellAction normalized;
			if (NormalizeAction(raw, normalized, nullptr) && ids.insert(normalized.id).second) result.push_back(std::move(normalized));
		}
	}
	catch (const nlohmann::json::exception&)
	{
	}
	return result;
}

bool ShellActionService::Save(const std::filesystem::path& data_root, const std::vector<ShellAction>& actions, std::string* error_out)
{
	nlohmann::json root = nlohmann::json::array();
	std::set<std::string> ids;
	std::set<std::string> labels;
	for (const ShellAction& action : actions)
	{
		ShellAction normalized;
		if (!NormalizeAction(action, normalized, error_out)) return false;
		if (!ids.insert(normalized.id).second)
		{
			if (error_out != nullptr) *error_out = "Shell action ids must be unique.";
			return false;
		}
		std::string os_label = SafeFileName(normalized.label);
		std::ranges::transform(os_label, os_label.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (!labels.insert(os_label).second)
		{
			if (error_out != nullptr) *error_out = "Shell action labels must be unique.";
			return false;
		}
		root.push_back(ActionJson(normalized));
	}
	if (!uam::io::WriteTextFile(ActionsPath(data_root), root.dump(2) + "\n"))
	{
		if (error_out != nullptr) *error_out = "Failed to persist shell actions.";
		return false;
	}
	return true;
}

bool ShellActionService::Apply(const uam::AppState& app, const std::filesystem::path& executable, std::string* error_out)
{
	if (executable.empty())
	{
		if (error_out != nullptr) *error_out = "The UAM executable path is unavailable.";
		return false;
	}
	for (const ShellAction& action : app.shell_actions)
	{
		ShellAction normalized;
		if (!NormalizeAction(action, normalized, error_out)) return false;
		if (!normalized.enabled || normalized.open_workspace) continue;
		std::filesystem::path skill;
		if (!MarkdownStoreService::ValidateStoreFilePath(MarkdownStoreService::NormalizeRoot(app.settings.markdown_store_directory), normalized.skill_path, &skill, error_out)) return false;
	}

#if defined(__APPLE__)
	const auto home = uam::env::GetUserHomePath();
	if (!home)
	{
		if (error_out != nullptr) *error_out = "The user home directory is unavailable.";
		return false;
	}
	const fs::path services = *home / "Library" / "Services";
	std::error_code ec;
	if (!uam::paths::CreateDirectoriesNoThrow(services, &ec))
	{
		if (error_out != nullptr) *error_out = "Failed to create the Finder Services directory.";
		return false;
	}
	for (const fs::directory_entry& entry : fs::directory_iterator(services, ec))
	{
		if (entry.path().extension() == ".workflow" && entry.path().stem().string().starts_with(kMacWorkflowPrefix)) uam::paths::RemoveAllNoThrow(entry.path());
	}
	for (const ShellAction& action : app.shell_actions)
	{
		if (!action.enabled) continue;
		const fs::path contents = services / (std::string(kMacWorkflowPrefix) + SafeFileName(action.label) + ".workflow") / "Contents";
		if (!uam::io::WriteTextFile(contents / "document.wflow", MacWorkflowDocument(action, executable)) ||
		    !uam::io::WriteTextFile(contents / "Info.plist", MacWorkflowInfo(action)))
		{
			if (error_out != nullptr) *error_out = "Failed to install Finder Quick Action '" + action.label + "'.";
			return false;
		}
	}
	return true;
#elif defined(_WIN32)
	RemoveWindowsVerbs(L"*");
	RemoveWindowsVerbs(L"Directory");
	for (const ShellAction& action : app.shell_actions)
	{
		if (!action.enabled) continue;
		if (action.accepts_files && !InstallWindowsVerb(action, executable, L"*", error_out)) return false;
		if (action.accepts_folders && !InstallWindowsVerb(action, executable, L"Directory", error_out)) return false;
	}
	return true;
#else
	if (error_out != nullptr) *error_out = "Shell actions are supported on macOS and Windows.";
	return false;
#endif
}

bool ShellActionService::QueueLaunchRequest(const std::filesystem::path& data_root, const std::vector<std::string>& args, bool* recognized_out, std::string* error_out)
{
	if (recognized_out != nullptr) *recognized_out = false;
	const auto flag = std::find(args.begin(), args.end(), kLaunchFlag);
	if (flag == args.end()) return true;
	if (recognized_out != nullptr) *recognized_out = true;
	if (std::distance(flag, args.end()) < 3)
	{
		if (error_out != nullptr) *error_out = "Shell action invocation requires an action id and selected path.";
		return false;
	}
	const std::string action_id = SafeId(*(flag + 1));
	if (action_id.empty())
	{
		if (error_out != nullptr) *error_out = "Shell action invocation has an invalid action id.";
		return false;
	}
	nlohmann::json paths = nlohmann::json::array();
	for (auto it = flag + 2; it != args.end(); ++it)
	{
		if (!it->empty()) paths.push_back(*it);
	}
	if (paths.empty())
	{
		if (error_out != nullptr) *error_out = "Shell action invocation did not include a selected path.";
		return false;
	}
	const nlohmann::json request{{"actionId", action_id}, {"paths", std::move(paths)}};
	const std::string token = std::to_string(uam::time::SteadyEpochNanosecondsNow()) + "-" + std::to_string(std::random_device{}());
	if (!uam::io::WriteTextFile(RequestsPath(data_root) / (token + ".json"), request.dump() + "\n"))
	{
		if (error_out != nullptr) *error_out = "Failed to queue the shell action request.";
		return false;
	}
	return true;
}

bool ShellActionService::ProcessPendingRequests(uam::AppState& app)
{
	const fs::path requests = RequestsPath(app.data_root);
	std::error_code ec;
	if (!fs::is_directory(requests, ec)) return false;
	std::vector<fs::path> files;
	for (const fs::directory_entry& entry : fs::directory_iterator(requests, ec))
	{
		if (entry.path().extension() == ".json") files.push_back(entry.path());
	}
	std::sort(files.begin(), files.end());
	bool changed = false;
	std::vector<std::string> statuses;
	for (const fs::path& file : files)
	{
		std::string status;
		try
		{
			const nlohmann::json request = nlohmann::json::parse(uam::io::ReadTextFile(file));
			RunRequest(app, request, status);
		}
		catch (const nlohmann::json::exception&)
		{
			status = "Ignored an invalid shell action request.";
		}
		uam::paths::RemoveFileNoThrow(file);
		if (!status.empty()) statuses.push_back(std::move(status));
		changed = true;
	}
	if (!statuses.empty()) app.shell_action_notification = uam::strings::JoinNonEmpty(statuses, "\n");
	return changed;
}
