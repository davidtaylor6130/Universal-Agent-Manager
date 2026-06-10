#include "cef/uam_query_handler.h"
#include "cef/uam_bridge_request.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"
#include "cef/uam_cef_security.h"

#include "app/chat_domain_service.h"
#include "app/chat_lifecycle_service.h"
#include "app/goal_service.h"
#include "app/git_worktree_service.h"
#include "app/markdown_store_service.h"
#include "app/memory_library_service.h"
#include "app/memory_service.h"
#include "app/persistence_coordinator.h"
#include "app/runtime_orchestration_services.h"
#include "app/vcs_commit_service.h"
#include "common/chat/chat_branching.h"
#include "common/chat/native_chat_identity.h"
#include "common/config/approval_modes.h"
#include "common/config/editor_file_associations.h"
#include "common/config/settings_normalization.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"

#include "common/platform/platform_services.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_launch.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/runtime/terminal/terminal_provider_cli.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/message_attachment_json.h"
#include "common/chat/chat_repository.h"
#include "common/utils/base64.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include "include/wrapper/cef_helpers.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
	namespace attachment_fields = uam::message_attachment_json;
	namespace attachment_frontend_fields = uam::message_attachment_json::frontend;

	constexpr const char* kPreferredProviderId = provider_build_config::FirstEnabledProviderId();
	constexpr std::size_t kRecentOutputReplayLimitBytes = 256 * 1024;
	constexpr std::size_t kMaxClipboardTextBytes = 1024 * 1024;
	constexpr std::uintmax_t kMaxAttachmentBytes = 25ull * 1024ull * 1024ull;

	struct AsyncCefResult
	{
		bool ok = true;
		int status = 500;
		std::string body;
		std::string error;
	};

	AsyncCefResult AsyncSuccess(nlohmann::json body)
	{
		return {true, 200, body.dump(), ""};
	}

	AsyncCefResult AsyncFailure(int status, std::string error)
	{
		return {false, status, "", std::move(error)};
	}

	std::string FailureDetailOrFallback(const std::string& detail, std::string fallback)
	{
		return detail.empty() ? std::move(fallback) : detail;
	}

	nlohmann::json WithOptionalRequestId(nlohmann::json value, const std::string& request_id)
	{
		if (!request_id.empty())
		{
			value["requestId"] = request_id;
		}
		return value;
	}

		uam::AppState BuildReadOnlyAppSnapshot(std::filesystem::path data_root, AppSettings settings, std::vector<ChatFolder> folders, std::vector<ProviderProfile> provider_profiles)
		{
			uam::AppState snapshot;
		snapshot.data_root = std::move(data_root);
		snapshot.settings = std::move(settings);
		snapshot.folders = std::move(folders);
		snapshot.provider_profiles = std::move(provider_profiles);
		return snapshot;
	}

	struct ReadOnlyAppSnapshotInputs
	{
		std::filesystem::path data_root;
		AppSettings settings;
		std::vector<ChatFolder> folders;
		std::vector<ProviderProfile> provider_profiles;
	};

	ReadOnlyAppSnapshotInputs CaptureReadOnlyAppSnapshotInputs(const uam::AppState& app)
	{
		return {app.data_root, app.settings, app.folders, app.provider_profiles};
	}

	uam::AppState BuildReadOnlyAppSnapshot(ReadOnlyAppSnapshotInputs inputs)
	{
		return BuildReadOnlyAppSnapshot(std::move(inputs.data_root), std::move(inputs.settings), std::move(inputs.folders), std::move(inputs.provider_profiles));
	}

	class CefQueryCallbackTask : public CefTask
	{
	  public:
		CefQueryCallbackTask(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, AsyncCefResult result) : m_callback(std::move(callback)), m_result(std::move(result))
		{
		}

		void Execute() override
		{
			CEF_REQUIRE_UI_THREAD();
			if (!m_callback)
			{
				return;
			}
			if (m_result.ok)
			{
				m_callback->Success(m_result.body);
			}
			else
			{
				m_callback->Failure(m_result.status, m_result.error);
			}
		}

	  private:
		CefRefPtr<CefMessageRouterBrowserSide::Callback> m_callback;
		AsyncCefResult m_result;
		IMPLEMENT_REFCOUNTING(CefQueryCallbackTask);
	};

	template <typename Worker> void RunAsyncCefQuery(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, Worker worker)
	{
		std::thread(
		    [callback = std::move(callback), worker = std::move(worker)]() mutable
		    {
			    AsyncCefResult result;
			    try
			    {
				    result = worker();
			    }
			    catch (const std::exception& ex)
			    {
				    result = AsyncFailure(500, ex.what());
			    }
			    catch (...)
			    {
				    // Detached CEF workers must translate non-standard exceptions into callback failures.
				    result = AsyncFailure(500, "Async bridge request failed.");
			    }
			    CefPostTask(TID_UI, new CefQueryCallbackTask(callback, std::move(result)));
		    })
		    .detach();
	}

	ChatSession* FindChatOrFail(uam::AppState& app, const std::string& chat_id, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb, const std::string& not_found_message, int status_code = 404)
	{
		ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);

		if (chat == nullptr)
		{
			cb->Failure(status_code, not_found_message);
			return nullptr;
		}

		return chat;
	}

	ChatSession* FindPayloadChatOrFail(uam::AppState& app, const nlohmann::json& payload, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb)
	{
		const std::string chat_id = payload.value("chatId", "");
		return FindChatOrFail(app, chat_id, cb, "Chat not found.");
	}

	bool ChatIdExists(const uam::AppState& app, const std::string& chat_id)
	{
		return ChatDomainService().FindChatById(app, chat_id) != nullptr;
	}

	std::string MakeCollisionSafeImportedChatId(const ChatSession& chat, const uam::AppState& app)
	{
		const std::string chat_id = uam::strings::Trim(chat.id);
		const std::string native_session_id = uam::strings::Trim(chat.native_session_id);
		const std::string base_id = uam::strings::NonEmptyOrFallback(chat_id, native_session_id);
		const std::string suffix = uam::chat_identity::NativeIdentityKeyHash(uam::chat_identity::NativeIdentityKeyForHistoryImport(chat));
		std::string candidate = base_id + "--" + suffix;

		while (ChatIdExists(app, candidate))
		{
			candidate += "_";
		}

		return candidate;
	}

	bool ChatProviderAvailableOrFail(const uam::AppState& app, const ChatSession& chat, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb)
	{
		if (ProviderResolutionService().ChatProviderIsAvailable(app, chat))
		{
			return true;
		}

		cb->Failure(409, ProviderResolutionService().ChatProviderUnavailableReason(app, chat));
		return false;
	}

	bool AutoApprovePendingAcpPermissionOrFail(uam::AppState& app, const std::string& chat_id, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb)
	{
		std::string acp_error;
		if (uam::TryAutoApprovePendingAcpPermission(app, chat_id, &acp_error) || acp_error.empty())
		{
			return true;
		}

		cb->Failure(409, acp_error);
		return false;
	}

	void RollbackCreatedChat(uam::AppState& app, const std::string& chat_id, const std::string& previous_selected_chat_id, bool delete_storage)
	{
		const int chat_index = ChatDomainService().FindChatIndexById(app, chat_id);
		if (chat_index >= 0)
		{
			app.chats.erase(app.chats.begin() + chat_index);
		}

		if (delete_storage)
		{
			ChatRepository::DeleteChatStorageFiles(app.data_root, chat_id);
		}

		ChatDomainService().SelectChatById(app, previous_selected_chat_id);
	}

	int FolderFailureCode(const std::string& status_line)
	{
		if (uam::strings::ContainsAny(status_line, {"no longer exists", "not found"}))
		{
			return 404;
		}

		return 400;
	}

	const ProviderProfile* ResolvePreferredCliProvider(const uam::AppState& app)
	{
		if (const ProviderProfile* preferred = ProviderProfileStore::FindById(app.provider_profiles, kPreferredProviderId); preferred != nullptr)
		{
			if (ProviderRuntime::IsRuntimeEnabled(*preferred) && ProviderRuntime::UsesCliOutput(*preferred) && preferred->supports_interactive)
			{
				return preferred;
			}
		}

		for (const ProviderProfile& provider : app.provider_profiles)
		{
			if (ProviderRuntime::IsRuntimeEnabled(provider) && ProviderRuntime::UsesCliOutput(provider) && provider.supports_interactive)
			{
				return &provider;
			}
		}

		return nullptr;
	}

	std::string DefaultNewChatProviderId(const uam::AppState& app, const ProviderProfile* preferred_provider)
	{
		if (!app.settings.default_new_chat_provider_id.empty())
		{
			return app.settings.default_new_chat_provider_id;
		}
		if (preferred_provider != nullptr)
		{
			return preferred_provider->id;
		}

		return app.settings.active_provider_id;
	}

	std::string ResolveNewChatProviderId(const uam::AppState& app, const std::string& requested_provider_id, const ProviderProfile* preferred_provider)
	{
		if (const ProviderProfile* requested_provider = ProviderProfileStore::FindById(app.provider_profiles, requested_provider_id); requested_provider != nullptr)
		{
			return requested_provider->id;
		}
		if (const ProviderProfile* fallback_provider = ProviderProfileStore::FindById(app.provider_profiles, app.settings.default_new_chat_provider_id); fallback_provider != nullptr)
		{
			return fallback_provider->id;
		}
		if (preferred_provider != nullptr)
		{
			return preferred_provider->id;
		}

		return app.settings.active_provider_id;
	}

	constexpr std::size_t kMaxAcpModelIdLength = 160;

	bool IsSafeAcpModelIdToken(std::string_view value)
	{
		if (value.empty() || value.size() > kMaxAcpModelIdLength || value.front() == '-')
		{
			return false;
		}
		for (const char ch : value)
		{
			const bool safe = uam::strings::IsAsciiAlnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-' || ch == ':' || ch == '/';
			if (!safe)
			{
				return false;
			}
		}
		return true;
	}

	bool IsAllowedAcpModelId(std::string_view model_id)
	{
		return model_id.empty() || IsSafeAcpModelIdToken(model_id);
	}

	ProviderChatDefaults NormalizeProviderChatDefaultsForRuntime(ProviderChatDefaults defaults)
	{
		defaults.model_id = uam::strings::Trim(defaults.model_id);
		if (!IsAllowedAcpModelId(defaults.model_id))
		{
			defaults.model_id.clear();
		}

		defaults.approval_mode = uam::approval_modes::NormalizeIncomingApprovalModeId(defaults.approval_mode);
		if (!uam::approval_modes::IsAppApprovalMode(defaults.approval_mode))
		{
			defaults.approval_mode = uam::approval_modes::kDefaultApprovalMode;
		}

		defaults.reasoning_effort = uam::codex::NormalizeReasoningEffort(defaults.reasoning_effort);
		defaults.service_tier = uam::codex::NormalizeServiceTier(defaults.service_tier);
		return defaults;
	}

	ProviderChatDefaults DefaultsFromPayload(const nlohmann::json& value, const ProviderChatDefaults& fallback)
	{
		ProviderChatDefaults defaults = fallback;
		if (!value.is_object())
		{
			return NormalizeProviderChatDefaultsForRuntime(defaults);
		}
		defaults.model_id = uam::nlohmann_json::TrimmedStringValueOr(value, "modelId", defaults.model_id);
		defaults.approval_mode = uam::nlohmann_json::TrimmedStringValueOr(value, "approvalMode", defaults.approval_mode);
		if (const std::optional<bool> auto_approve_commands = uam::nlohmann_json::BoolFieldStrict(value, "autoApproveCommands"))
		{
			defaults.auto_approve_commands = *auto_approve_commands;
		}
		if (const std::optional<bool> memory_enabled = uam::nlohmann_json::BoolFieldStrict(value, "memoryEnabled"))
		{
			defaults.memory_enabled = *memory_enabled;
		}
		defaults.reasoning_effort = uam::nlohmann_json::TrimmedStringValueOr(value, "reasoningEffort", defaults.reasoning_effort);
		defaults.service_tier = uam::nlohmann_json::TrimmedStringValueOr(value, "serviceTier", defaults.service_tier);
		return NormalizeProviderChatDefaultsForRuntime(defaults);
	}

	ProviderChatDefaults DefaultsForProvider(const AppSettings& settings, const std::string& provider_id)
	{
		const std::string normalized_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
		const auto found = settings.provider_chat_defaults.find(normalized_provider_id);
		if (found != settings.provider_chat_defaults.end())
		{
			return NormalizeProviderChatDefaultsForRuntime(found->second);
		}
		return ProviderChatDefaults{"", uam::approval_modes::kDefaultApprovalMode, false, settings.memory_enabled_default, "", ""};
	}

	void ApplyProviderDefaultsToChat(const AppSettings& settings, ChatSession& chat, const nlohmann::json* payload_defaults = nullptr)
	{
		ProviderChatDefaults defaults = DefaultsForProvider(settings, chat.provider_id);
		if (payload_defaults != nullptr)
		{
			defaults = DefaultsFromPayload(*payload_defaults, defaults);
		}
		if (!uam::provider_ids::IsCliProviderAliasOf(chat.provider_id, uam::provider_ids::kCodexCli))
		{
			defaults.reasoning_effort.clear();
			defaults.service_tier.clear();
		}
		chat.model_id = defaults.model_id;
		chat.approval_mode = defaults.approval_mode;
		chat.auto_approve_commands = defaults.auto_approve_commands;
		chat.memory_enabled = defaults.memory_enabled;
		chat.reasoning_effort = defaults.reasoning_effort;
		chat.service_tier = defaults.service_tier;
	}

	bool AcpSessionBlocksModelChange(const uam::AcpSessionState& session)
	{
		return uam::AcpSessionHasBlockingRuntimeWork(session);
	}

	std::vector<EditorFileAssociation> ParseEditorFileAssociationsPayload(const nlohmann::json& payload)
	{
		std::vector<EditorFileAssociation> associations;
		const nlohmann::json* file_associations = uam::nlohmann_json::FindArrayField(payload, "fileAssociations");
		if (file_associations == nullptr)
		{
			return associations;
		}

		for (const nlohmann::json& item : *file_associations)
		{
			if (!item.is_object())
			{
				continue;
			}

			EditorFileAssociation association;
			association.id = uam::nlohmann_json::TrimmedStringValue(item, {"id"});
			association.name = uam::nlohmann_json::TrimmedStringValue(item, {"name"});
			if (association.id.empty())
			{
				association.id = "editor-group-" + std::to_string(associations.size() + 1);
			}
			association.editor_preset_id = uam::nlohmann_json::TrimmedStringValue(item, {"editorPresetId"});
			association.extensions = uam::nlohmann_json::StringArrayField(item, "extensions");

			std::optional<EditorFileAssociation> normalized_association = uam::editor_file_associations::NormalizeEditorFileAssociation(std::move(association));
			if (!normalized_association)
			{
				continue;
			}
			associations.push_back(std::move(*normalized_association));
		}

		return associations;
	}

	bool ShouldSkipEditorScanDirectory(const std::filesystem::path& path)
	{
		static constexpr std::array<std::string_view, 8> kIgnoredDirectoryNames = {
		    ".git", "node_modules", "Builds", "build", "dist", "out", ".venv", "venv",
		};

		const std::string name = path.filename().string();
		return uam::ranges::Contains(kIgnoredDirectoryNames, std::string_view(name)) || uam::strings::StartsWith(name, "cmake-build-");
	}

	std::string SelectEditorPresetForWorkspace(const AppSettings& settings, const std::filesystem::path& workspace_root)
	{
		constexpr std::size_t kMaxEditorPresetScanFiles = 5000;
		std::set<std::string> found_extensions;
		std::error_code ec;
		std::size_t visited_files = 0;
		std::filesystem::recursive_directory_iterator it(workspace_root, std::filesystem::directory_options::skip_permission_denied, ec);
		const std::filesystem::recursive_directory_iterator end;
		while (!ec && it != end && visited_files < kMaxEditorPresetScanFiles)
		{
			const std::filesystem::directory_entry entry = *it;
			if (entry.is_directory(ec) && ShouldSkipEditorScanDirectory(entry.path()))
			{
				it.disable_recursion_pending();
			}
			else if (entry.is_regular_file(ec))
			{
				++visited_files;
				const std::string extension = uam::editor_file_associations::NormalizeFileExtension(entry.path().extension().string());
				if (!extension.empty())
				{
					found_extensions.insert(extension);
				}
			}
			it.increment(ec);
		}

		for (const EditorFileAssociation& association : settings.editor_file_associations)
		{
			for (const std::string& raw_extension : association.extensions)
			{
				if (found_extensions.contains(uam::editor_file_associations::NormalizeFileExtension(raw_extension)))
				{
					return uam::editor_file_associations::NormalizeEditorPresetId(association.editor_preset_id, settings.default_editor_preset_id);
				}
			}
		}

		return uam::editor_file_associations::NormalizeEditorPresetId(settings.default_editor_preset_id);
	}

	std::string NormalizeCliVersionProviderId(std::string_view provider_id)
	{
		const std::string normalized = uam::provider_ids::NormalizeCliProviderAlias(provider_id);
		return uam::provider_ids::IsVersionManagedCliProviderId(normalized) ? normalized : std::string{};
	}

	std::string FallbackCliVersionProviderId(const std::string& active_provider_id)
	{
		return uam::provider_ids::NormalizeVersionManagedCliProviderId(active_provider_id);
	}

	std::string CliVersionProviderFromPayloadOrSelection(const uam::AppState& app, const nlohmann::json& payload)
	{
		const std::string requested = NormalizeCliVersionProviderId(uam::nlohmann_json::TrimmedStringValue(payload, {"providerId"}));
		if (!requested.empty())
		{
			return requested;
		}

		const std::string selected_provider_id = NormalizeCliVersionProviderId(ChatDomainService().SelectedChatProviderId(app));
		if (!selected_provider_id.empty())
		{
			return selected_provider_id;
		}

		return FallbackCliVersionProviderId(app.settings.active_provider_id);
	}

#if defined(_WIN32)
	std::wstring WideFromUtf8(const std::string& value)
	{
		if (value.empty())
		{
			return std::wstring();
		}
		const int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
		if (wide_len <= 0)
		{
			return std::wstring();
		}
		std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), wide.data(), wide_len) <= 0)
		{
			return std::wstring();
		}
		return wide;
	}
#endif

#if defined(__APPLE__)
	bool WriteAllToFileDescriptor(int fd, const std::string& text, std::string* error_out)
	{
		std::size_t offset = 0;
		while (offset < text.size())
		{
			const ssize_t written = write(fd, text.data() + offset, text.size() - offset);
			if (written > 0)
			{
				offset += static_cast<std::size_t>(written);
				continue;
			}

			if (written < 0 && errno == EINTR)
			{
				continue;
			}

			if (error_out != nullptr)
			{
				*error_out = written == 0 ? "Failed to write clipboard text: write made no progress." : "Failed to write clipboard text: " + std::string(std::strerror(errno)) + ".";
			}
			return false;
		}

		return true;
	}

	bool WaitForClipboardProcess(const pid_t pid, std::string* error_out)
	{
		int status = 0;
		while (waitpid(pid, &status, 0) < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			if (error_out != nullptr)
			{
				*error_out = "Failed waiting for pbcopy: " + std::string(std::strerror(errno)) + ".";
			}
			return false;
		}

		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		{
			return true;
		}

		if (error_out != nullptr)
		{
			if (WIFEXITED(status))
			{
				*error_out = "pbcopy exited with status " + std::to_string(WEXITSTATUS(status)) + ".";
			}
			else if (WIFSIGNALED(status))
			{
				*error_out = "pbcopy terminated by signal " + std::to_string(WTERMSIG(status)) + ".";
			}
			else
			{
				*error_out = "pbcopy ended without a normal exit status.";
			}
		}

		return false;
	}
#endif

	bool WriteNativeClipboardText(const std::string& text, std::string* error_out)
	{
#if defined(__APPLE__)
		int stdin_pipe[2] = {-1, -1};
		if (pipe(stdin_pipe) != 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to create pbcopy pipe: " + std::string(std::strerror(errno)) + ".";
			}
			return false;
		}

		const pid_t pid = fork();
		if (pid < 0)
		{
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			if (error_out != nullptr)
			{
				*error_out = "Failed to launch pbcopy: " + std::string(std::strerror(errno)) + ".";
			}
			return false;
		}

		if (pid == 0)
		{
			close(stdin_pipe[1]);
			if (dup2(stdin_pipe[0], STDIN_FILENO) < 0)
			{
				_exit(126);
			}
			close(stdin_pipe[0]);
			execl("/usr/bin/pbcopy", "pbcopy", static_cast<char*>(nullptr));
			_exit(127);
		}

		close(stdin_pipe[0]);
		std::string write_error;
		const bool write_ok = WriteAllToFileDescriptor(stdin_pipe[1], text, &write_error);
		close(stdin_pipe[1]);

		std::string wait_error;
		const bool wait_ok = WaitForClipboardProcess(pid, &wait_error);

		if (!write_ok || !wait_ok)
		{
			if (error_out != nullptr)
			{
				*error_out = !write_ok ? write_error : wait_error;
			}
			return false;
		}

		return true;
#elif defined(_WIN32)
		const std::wstring wide = WideFromUtf8(text);
		if (wide.empty() && !text.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "Clipboard text is not valid UTF-8.";
			}
			return false;
		}
		if (!OpenClipboard(nullptr))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to open the Windows clipboard.";
			}
			return false;
		}

		if (!EmptyClipboard())
		{
			CloseClipboard();
			if (error_out != nullptr)
			{
				*error_out = "Failed to clear the Windows clipboard.";
			}
			return false;
		}

		const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
		HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
		if (memory == nullptr)
		{
			CloseClipboard();
			if (error_out != nullptr)
			{
				*error_out = "Failed to allocate Windows clipboard memory.";
			}
			return false;
		}

		void* locked = GlobalLock(memory);
		if (locked == nullptr)
		{
			GlobalFree(memory);
			CloseClipboard();
			if (error_out != nullptr)
			{
				*error_out = "Failed to lock Windows clipboard memory.";
			}
			return false;
		}
		std::memcpy(locked, wide.c_str(), bytes);
		GlobalUnlock(memory);

		if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
		{
			GlobalFree(memory);
			CloseClipboard();
			if (error_out != nullptr)
			{
				*error_out = "Failed to set Windows clipboard text.";
			}
			return false;
		}
		CloseClipboard();
		return true;
#else
		if (error_out != nullptr)
		{
			*error_out = "Native clipboard writes are not implemented for this platform.";
		}
		return false;
#endif
	}

	nlohmann::json SerializeMarkdownStoreEntry(const MarkdownStoreService::Entry& entry)
	{
		nlohmann::json entry_json;
		entry_json["id"] = entry.id;
		entry_json["title"] = entry.title;
		entry_json["maker"] = entry.maker;
		entry_json["review"] = entry.review;
		entry_json["dateCreated"] = entry.date_created;
		entry_json["dateUpdated"] = entry.date_updated;
		entry_json["preview"] = entry.preview;
		entry_json["filePath"] = entry.file_path.string();
		return entry_json;
	}

	std::string SafeAttachmentName(std::string value, const std::string& fallback)
	{
		std::string out;
		out.reserve(value.size());
		for (const unsigned char ch : value)
		{
			if (uam::strings::IsAsciiAlnum(ch) || ch == '.' || ch == '-' || ch == '_')
			{
				out.push_back(static_cast<char>(ch));
			}
			else if (ch == ' ')
			{
				out.push_back('-');
			}
		}
		if (out.empty() || out == "." || out == "..")
		{
			out = fallback;
		}
		return out.substr(0, 120);
	}

	std::string AttachmentId()
	{
		return "att-" + uam::time::SystemEpochMicrosecondsTokenNow();
	}

	std::string PathForPrompt(const std::filesystem::path& workspace_root, const std::filesystem::path& path)
	{
		const std::filesystem::path final_path = uam::paths::AbsolutePathNoThrow(path);
		if (const std::optional<std::filesystem::path> relative = uam::paths::RelativePathIfInsideRoot(workspace_root, final_path))
		{
			return uam::paths::PortablePathString(*relative);
		}
		return uam::paths::PortablePathString(final_path);
	}

	nlohmann::json AttachmentToJson(const MessageAttachment& attachment)
	{
		return {
		    {std::string(attachment_fields::kIdField), attachment.id},
		    {std::string(attachment_fields::kNameField), attachment.name},
		    {std::string(attachment_fields::kKindField), attachment.kind},
		    {std::string(attachment_frontend_fields::kMimeTypeField), attachment.mime_type},
		    {std::string(attachment_fields::kPathField), attachment.path},
		    {std::string(attachment_frontend_fields::kSizeBytesField), attachment.size_bytes},
		    {std::string(attachment_fields::kCopiedField), attachment.copied},
		};
	}

	std::string NormalizeStagedAttachmentKind(std::string_view requested_kind)
	{
		requested_kind = uam::strings::TrimAsciiView(requested_kind);
		if (requested_kind == attachment_frontend_fields::kDirectoryKind)
		{
			return std::string(attachment_frontend_fields::kDirectoryKind);
		}
		if (requested_kind == attachment_frontend_fields::kImageKind)
		{
			return std::string(attachment_frontend_fields::kImageKind);
		}

		return std::string(attachment_frontend_fields::kFileKind);
	}

	std::vector<MessageAttachment> ParseStagedAttachments(const nlohmann::json& payload)
	{
		std::vector<MessageAttachment> attachments;
		const nlohmann::json* attachment_items = uam::nlohmann_json::FindArrayField(payload, attachment_frontend_fields::kAttachmentsField);
		if (attachment_items == nullptr)
		{
			return attachments;
		}
		for (const nlohmann::json& item : *attachment_items)
		{
			if (!item.is_object())
			{
				continue;
			}
			MessageAttachment attachment;
			attachment.id = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kIdField});
			attachment.name = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kNameField});
			attachment.kind = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kKindField, attachment_frontend_fields::kMimeTypeField});
			attachment.mime_type = uam::nlohmann_json::TrimmedStringValue(item, {attachment_frontend_fields::kMimeTypeInputField, attachment_frontend_fields::kMimeTypeField});
			if (attachment.kind == attachment.mime_type || uam::strings::Contains(attachment.kind, '/'))
			{
				attachment.kind = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kKindField});
			}
			attachment.path = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kPathField});
			attachment.size_bytes = item.value(std::string(attachment_frontend_fields::kSizeBytesField), static_cast<std::uintmax_t>(0));
			attachment.copied = item.value(std::string(attachment_fields::kCopiedField), false);
			if (attachment.path.empty())
			{
				continue;
			}
			if (attachment.id.empty())
			{
				attachment.id = AttachmentId();
			}
			if (attachment.name.empty())
			{
				attachment.name = std::filesystem::path(attachment.path).filename().string();
			}
			if (attachment.kind.empty())
			{
				attachment.kind = "file";
			}
			attachments.push_back(std::move(attachment));
		}
		return attachments;
	}

	std::vector<std::string> SearchTokens(const std::string& query)
	{
		std::istringstream in(uam::strings::ToLowerAscii(uam::strings::Trim(query)));
		std::vector<std::string> tokens;
		std::string token;
		while (in >> token)
		{
			tokens.push_back(token);
		}
		return tokens;
	}

	uam::CliTerminalState* FindCliTerminalByRoutingKey(uam::AppState& app, const std::string& chat_id, const std::string& terminal_id)
	{
		return uam::FindCliTerminalForRoutingKey(app, chat_id, terminal_id);
	}

	bool CliInputLooksLikeTurnSubmit(const std::string& data)
	{
		return uam::strings::Contains(data, '\r') || uam::strings::Contains(data, '\n');
	}

	nlohmann::json BuildCliBindingResponse(const uam::CliTerminalState& terminal)
	{
		nlohmann::json data;
		data["terminalId"] = terminal.terminal_id;
		data["sessionId"] = terminal.frontend_chat_id;
		data["sourceChatId"] = uam::CliTerminalPrimaryChatId(terminal);
		data["running"] = terminal.running;
		data["lifecycleState"] = uam::CliTerminalLifecycleStateLabel(terminal);
		data["turnState"] = uam::CliTerminalLifecycleIsProcessing(terminal) ? "busy" : "idle";
		data["lastError"] = terminal.last_error;

		if (!terminal.recent_output_bytes.empty())
		{
			const std::size_t start_offset = terminal.recent_output_bytes.size() > kRecentOutputReplayLimitBytes ? terminal.recent_output_bytes.size() - kRecentOutputReplayLimitBytes : 0;
			data["replayData"] = uam::base64::Encode(terminal.recent_output_bytes.substr(start_offset));
		}

		return data;
	}

	nlohmann::json SerializeGitWorktreeStatus(const uam::GitWorktreeStatus& status)
	{
		nlohmann::json json;
		json["isGitRepository"] = status.is_git_repository;
		json["isSvnWorkspace"] = status.is_svn_workspace;
		json["isolated"] = status.isolated;
		json["sourceDirty"] = status.source_dirty;
		json["worktreeDirty"] = status.worktree_dirty;
		json["worktreeMissing"] = status.worktree_missing;
		json["sourceDirectory"] = status.source_directory;
		json["worktreeDirectory"] = status.worktree_directory;
		json["branchName"] = status.branch_name;
		json["baseRef"] = status.base_ref;
		json["warning"] = status.warning;
		json["error"] = status.error;
		return json;
	}

	nlohmann::json SerializeGitWorktreeResult(const uam::GitWorktreeOperationResult& result)
	{
		nlohmann::json json;
		json["status"] = SerializeGitWorktreeStatus(result.status);
		json["message"] = result.message;
		json["patchPath"] = result.patch_path.empty() ? "" : result.patch_path.string();
		return json;
	}

	nlohmann::json SerializeVcsCommitStatus(const uam::VcsCommitStatus& status)
	{
		nlohmann::json types = nlohmann::json::array();
		for (const uam::VcsType type : status.vcs_types)
		{
			types.push_back(uam::VcsTypeToString(type));
		}

		nlohmann::json files = nlohmann::json::array();
		for (const uam::VcsChangedFile& file : status.changed_files)
		{
			files.push_back({
			    {"path", file.path},
			    {"status", file.status},
			    {"staged", file.staged},
			    {"additions", file.additions},
			    {"deletions", file.deletions},
			    {"binary", file.binary},
			});
		}

		nlohmann::json json;
		json["available"] = status.available;
		json["vcsTypes"] = std::move(types);
		json["activeVcsType"] = uam::VcsTypeToString(status.active_vcs_type);
		json["workspaceDirectory"] = status.workspace_directory;
		json["branchOrRevision"] = status.branch_or_revision;
		json["changedFiles"] = std::move(files);
		json["lineStatsReady"] = status.line_stats_ready;
		json["warning"] = status.warning;
		json["error"] = status.error;
		return json;
	}

	nlohmann::json SerializeVcsCommitResult(const uam::VcsCommitResult& result)
	{
		return {
		    {"ok", result.ok},
		    {"status", SerializeVcsCommitStatus(result.status)},
		    {"message", result.message},
		    {"error", result.error},
		};
	}

	bool ChatRuntimeBusy(const uam::AppState& app, const std::string& chat_id)
	{
		if (uam::HasPendingCallForChat(app, chat_id))
		{
			return true;
		}
		for (const auto& session : app.acp_sessions)
		{
			if (session == nullptr || session->chat_id != chat_id || !session->running)
			{
				continue;
			}

			if (uam::AcpSessionHasBlockingRuntimeWork(*session))
			{
				return true;
			}
		}
		for (const auto& terminal : app.cli_terminals)
		{
			if (terminal != nullptr && terminal->running && uam::CliTerminalMatchesChatId(*terminal, chat_id))
			{
				return true;
			}
		}
		return false;
	}
} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UamQueryHandler::UamQueryHandler(uam::AppState& app, std::string trusted_ui_index_url) : m_app(app), m_trustedUiIndexUrl(std::move(trusted_ui_index_url))
{
}

// ---------------------------------------------------------------------------
// CefMessageRouterBrowserSide::Handler
// ---------------------------------------------------------------------------

bool UamQueryHandler::DispatchAction(std::string_view action, CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	struct Route
	{
		std::string_view action;
		ActionHandler handler;
	};

	static constexpr Route kRoutes[] = {
		{"getInitialState", &UamQueryHandler::HandleGetInitialState},
		{"selectSession", &UamQueryHandler::HandleSelectSession},
		{"getChatMessages", &UamQueryHandler::HandleGetChatMessages},
		{"createSession", &UamQueryHandler::HandleCreateSession},
		{"openNativeSessionChat", &UamQueryHandler::HandleOpenNativeSessionChat},
		{"renameSession", &UamQueryHandler::HandleRenameSession},
		{"setChatPinned", &UamQueryHandler::HandleSetChatPinned},
		{"setChatProvider", &UamQueryHandler::HandleSetChatProvider},
		{"setChatModel", &UamQueryHandler::HandleSetChatModel},
		{"setChatCodexOptions", &UamQueryHandler::HandleSetChatCodexOptions},
		{"setChatApprovalMode", &UamQueryHandler::HandleSetChatApprovalMode},
		{"setChatAutoApproveCommands", &UamQueryHandler::HandleSetChatAutoApproveCommands},
		{"setChatMemoryEnabled", &UamQueryHandler::HandleSetChatMemoryEnabled},
		{"setMemorySettings", &UamQueryHandler::HandleSetMemorySettings},
		{"setProviderChatDefaults", &UamQueryHandler::HandleSetProviderChatDefaults},
		{"setEditorSettings", &UamQueryHandler::HandleSetEditorSettings},
		{"refreshCliProviderVersion", &UamQueryHandler::HandleRefreshCliProviderVersion},
		{"applyCliProviderVersion", &UamQueryHandler::HandleApplyCliProviderVersion},
		{"browseMarkdownStoreDirectory", &UamQueryHandler::HandleBrowseMarkdownStoreDirectory},
		{"setMarkdownStoreDirectory", &UamQueryHandler::HandleSetMarkdownStoreDirectory},
		{"listMarkdownStoreEntries", &UamQueryHandler::HandleListMarkdownStoreEntries},
		{"createMarkdownStoreEntry", &UamQueryHandler::HandleCreateMarkdownStoreEntry},
		{"revealMarkdownStoreEntry", &UamQueryHandler::HandleRevealMarkdownStoreEntry},
		{"deleteSession", &UamQueryHandler::HandleDeleteSession},
		{"createFolder", &UamQueryHandler::HandleCreateFolder},
		{"renameFolder", &UamQueryHandler::HandleRenameFolder},
		{"deleteFolder", &UamQueryHandler::HandleDeleteFolder},
		{"toggleFolder", &UamQueryHandler::HandleToggleFolder},
		{"browseFolderDirectory", &UamQueryHandler::HandleBrowseFolderDirectory},
		{"searchChatMessages", &UamQueryHandler::HandleSearchChatMessages},
		{"listMemoryEntries", &UamQueryHandler::HandleListMemoryEntries},
		{"createMemoryEntry", &UamQueryHandler::HandleCreateMemoryEntry},
		{"deleteMemoryEntry", &UamQueryHandler::HandleDeleteMemoryEntry},
		{"openMemoryRoot", &UamQueryHandler::HandleOpenMemoryRoot},
		{"revealMemoryEntry", &UamQueryHandler::HandleRevealMemoryEntry},
		{"openWorkspaceDirectory", &UamQueryHandler::HandleOpenWorkspaceDirectory},
		{"openWorkspaceEditor", &UamQueryHandler::HandleOpenWorkspaceEditor},
		{"openWorkspaceTerminal", &UamQueryHandler::HandleOpenWorkspaceTerminal},
		{"getChatWorktreeStatus", &UamQueryHandler::HandleGetChatWorktreeStatus},
		{"createChatWorktree", &UamQueryHandler::HandleCreateChatWorktree},
		{"discardChatWorktreeChanges", &UamQueryHandler::HandleDiscardChatWorktreeChanges},
		{"portChatWorktreeChanges", &UamQueryHandler::HandlePortChatWorktreeChanges},
		{"getVcsCommitStatus", &UamQueryHandler::HandleGetVcsCommitStatus},
		{"getVcsFileDiff", &UamQueryHandler::HandleGetVcsFileDiff},
		{"commitVcsChanges", &UamQueryHandler::HandleCommitVcsChanges},
		{"generateVcsCommitMessage", &UamQueryHandler::HandleGenerateVcsCommitMessage},
		{"listMemoryScanCandidates", &UamQueryHandler::HandleListMemoryScanCandidates},
		{"scanCurrentChats", &UamQueryHandler::HandleScanCurrentChats},
		{"startCliTerminal", &UamQueryHandler::HandleStartCli},
		{"stopCliTerminal", &UamQueryHandler::HandleStopCli},
		{"resizeCliTerminal", &UamQueryHandler::HandleResizeCli},
		{"writeCliInput", &UamQueryHandler::HandleWriteCliInput},
		{"stageChatAttachments", &UamQueryHandler::HandleStageChatAttachments},
		{"sendAcpPrompt", &UamQueryHandler::HandleSendAcpPrompt},
		{"cancelAcpTurn", &UamQueryHandler::HandleCancelAcpTurn},
		{"resolveAcpPermission", &UamQueryHandler::HandleResolveAcpPermission},
		{"resolveAcpUserInput", &UamQueryHandler::HandleResolveAcpUserInput},
		{"stopAcpSession", &UamQueryHandler::HandleStopAcpSession},
		{"writeClipboardText", &UamQueryHandler::HandleWriteClipboardText},
		{"setTheme", &UamQueryHandler::HandleSetTheme},
		{"setGoal", &UamQueryHandler::HandleSetGoal},
		{"updateGoalStatus", &UamQueryHandler::HandleUpdateGoalStatus},
		{"setActiveGoal", &UamQueryHandler::HandleSetActiveGoal},
		{"removeGoal", &UamQueryHandler::HandleRemoveGoal},
	};

	for (const Route& route : kRoutes)
	{
		if (route.action == action)
		{
			(this->*route.handler)(browser, payload, cb);
			return true;
		}
	}

	return false;
}

bool UamQueryHandler::OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t /*query_id*/, const CefString& request, bool /*persistent*/, CefRefPtr<Callback> callback)
{
	CEF_REQUIRE_UI_THREAD();

	if (!uam::cef::IsTrustedMainFrame(frame, m_trustedUiIndexUrl))
	{
		callback->Failure(403, "Privileged bridge is restricted to the bundled UI.");
		return true;
	}

	const uam::cef::BridgeRequestParseResult parsed = uam::cef::ParseBridgeRequest(request.ToString());
	if (!parsed.ok)
	{
		callback->Failure(parsed.status, parsed.error);
		return true;
	}

	const std::string& action = parsed.request.action;
	const nlohmann::json& payload = parsed.request.payload;

	try
	{
		if (!DispatchAction(action, browser, payload, callback))
		{
			callback->Failure(404, "Unknown action: " + action);
		}
	}
	catch (const nlohmann::json::exception& ex)
	{
		callback->Failure(400, std::string("Invalid bridge payload: ") + ex.what());
	}
	catch (const std::exception& ex)
	{
		callback->Failure(500, std::string("Bridge request failed: ") + ex.what());
	}

	return true;
}

void UamQueryHandler::OnQueryCanceled(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/, int64_t /*query_id*/)
{
	// Persistent queries are not used; nothing to cancel.
}

// ---------------------------------------------------------------------------
// Action handlers
// ---------------------------------------------------------------------------

void UamQueryHandler::HandleGetInitialState(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& /*payload*/, CefRefPtr<Callback> cb)
{
	nlohmann::json state = uam::StateSerializer::Serialize(m_app);
	cb->Success(state.dump());
}

void UamQueryHandler::HandleSelectSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string previous_selected_chat_id = ChatDomainService().SelectedChatId(m_app);
	ChatSession* target_chat = FindChatOrFail(m_app, chat_id, cb, "Selected chat no longer exists.");
	if (target_chat == nullptr)
	{
		return;
	}

	const std::string previous_last_opened_at = target_chat->last_opened_at;
	ChatDomainService().SelectChatById(m_app, chat_id);

	ChatSession* selected_chat = ChatDomainService().SelectedChat(m_app);
	if (selected_chat == nullptr)
	{
		cb->Failure(404, "Selected chat no longer exists.");
		return;
	}

	selected_chat->last_opened_at = uam::time::TimestampNow();
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		selected_chat->last_opened_at = previous_last_opened_at;
		ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
		return;
	}

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *selected_chat, "", ""))
	{
		selected_chat->last_opened_at = previous_last_opened_at;
		ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		(void)PersistenceCoordinator().SaveSettings(m_app);
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
		return;
	}

	ChatDomainService().SortChatsByRecent(m_app.chats);
	ChatDomainService().SelectChatById(m_app, chat_id);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleGetChatMessages(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string known_digest = payload.value("messagesDigest", "");
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	std::string hydrate_warning;
	if (!ChatRepository::HydrateChatMessages(m_app.data_root, *chat, &hydrate_warning))
	{
		cb->Failure(500, FailureDetailOrFallback(hydrate_warning, "Failed to load chat messages."));
		return;
	}

	const nlohmann::json serialized = uam::StateSerializer::SerializeSession(*chat);
	const std::string messages_digest = serialized.value("messagesDigest", "");
	nlohmann::json result;
	result["chatId"] = chat_id;
	result["messagesDigest"] = messages_digest;
	if (!known_digest.empty() && known_digest == messages_digest)
	{
		result["unchanged"] = true;
		cb->Success(result.dump());
		return;
	}

	result["unchanged"] = false;
	const nlohmann::json* messages = uam::nlohmann_json::FindArrayField(serialized, "messages");
	result["messages"] = messages == nullptr ? nlohmann::json::array() : *messages;
	cb->Success(result.dump());
}

void UamQueryHandler::HandleCreateSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string title = payload.value("title", "New Chat");
	const std::string requested_folder_id = payload.value("folderId", "");
	const ProviderProfile* preferred_provider = ResolvePreferredCliProvider(m_app);
	const std::string default_provider_id = DefaultNewChatProviderId(m_app, preferred_provider);
	const std::string requested_provider_id = payload.value("providerId", default_provider_id);
	const std::string provider_id = ResolveNewChatProviderId(m_app, requested_provider_id, preferred_provider);
	const std::string previous_selected_chat_id = ChatDomainService().SelectedChatId(m_app);

	const std::string target_folder_id = ResolveRequestedNewChatFolderId(m_app, requested_folder_id);
	if (target_folder_id.empty())
	{
		cb->Failure(400, FailureDetailOrFallback(m_app.status_line, "A workspace folder is required to create a chat."));
		return;
	}

	ChatSession chat = ChatDomainService().CreateNewChat(target_folder_id, provider_id);
	if (!title.empty())
	{
		chat.title = title;
	}
	chat.workspace_directory = uam::paths::ResolveWorkspaceRootPath(m_app, chat).string();
	const nlohmann::json* payload_defaults = uam::nlohmann_json::FindObjectField(payload, "defaults");
	ApplyProviderDefaultsToChat(m_app.settings, chat, payload_defaults);

	m_app.chats.push_back(std::move(chat));

	ChatSession& created_chat = m_app.chats.back();
	const std::string created_chat_id = created_chat.id;
	ChatDomainService().SelectChatById(m_app, created_chat_id);

	ChatHistorySyncService sync;
	if (!sync.SaveChatWithStatus(m_app, created_chat, "", ""))
	{
		RollbackCreatedChat(m_app, created_chat_id, previous_selected_chat_id, false);
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist new chat."));
		return;
	}

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		RollbackCreatedChat(m_app, created_chat_id, previous_selected_chat_id, true);
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist new chat settings."));
		return;
	}

	if (const ChatSession* selected = ChatDomainService().SelectedChat(m_app); selected != nullptr && ProviderResolutionService().ChatUsesCliOutput(m_app, *selected))
	{
		uam::MarkSelectedCliTerminalForLaunch(m_app);
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleOpenNativeSessionChat(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string source_chat_id = payload.value("chatId", "");
	const std::string native_session_id = uam::strings::Trim(payload.value("nativeSessionId", ""));
	if (native_session_id.empty())
	{
		cb->Failure(400, "A native session id is required.");
		return;
	}

	ChatSession* source_chat = FindChatOrFail(m_app, source_chat_id, cb, "Source chat not found: " + source_chat_id);
	if (source_chat == nullptr)
	{
		return;
	}

	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(m_app, *source_chat);
	if (!ProviderRuntime::UsesNativeOverlayHistory(provider) && !ProviderRuntime::UsesLocalHistory(provider))
	{
		cb->Failure(409, "This provider does not expose a native or local session history path.");
		return;
	}

	const std::string source_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider.id);

	const std::string previous_selected_chat_id = ChatDomainService().SelectedChatId(m_app);
	const auto previous_resolved_native_session = m_app.resolved_native_sessions_by_chat_id.find(source_chat->id);
	const bool had_previous_resolved_native_session = previous_resolved_native_session != m_app.resolved_native_sessions_by_chat_id.end();
	const std::string previous_resolved_native_session_id = had_previous_resolved_native_session ? previous_resolved_native_session->second : std::string{};
	ChatSession* target_chat = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(m_app, *source_chat, provider, native_session_id, false);

	bool inserted_chat = false;
	std::string target_chat_id;
	bool had_previous_target_resolved_native_session = false;
	std::string previous_target_resolved_native_session_id;
	if (target_chat == nullptr)
	{
		target_chat = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(m_app, *source_chat, provider, native_session_id, false);
		if (target_chat == nullptr)
		{
			cb->Failure(404, "Sub-agent chat not found in native history.");
			return;
		}
		inserted_chat = true;
		target_chat_id = target_chat->id;
	}
	else
	{
		target_chat_id = target_chat->id;
		const auto previous_target_resolved_native_session = m_app.resolved_native_sessions_by_chat_id.find(target_chat_id);
		had_previous_target_resolved_native_session = previous_target_resolved_native_session != m_app.resolved_native_sessions_by_chat_id.end();
		previous_target_resolved_native_session_id = had_previous_target_resolved_native_session ? previous_target_resolved_native_session->second : std::string{};
		m_app.resolved_native_sessions_by_chat_id[target_chat->id] = native_session_id;
	}

	const std::string previous_provider_id = target_chat->provider_id;
	const std::string previous_native_session_id = target_chat->native_session_id;
	const std::string previous_updated_at = target_chat->updated_at;
	const std::string previous_last_opened_at = target_chat->last_opened_at;
	if (target_chat->provider_id.empty())
	{
		target_chat->provider_id = source_provider_id;
	}
	ChatDomainService().SelectChatById(m_app, target_chat_id);

	ChatSession* selected_chat = ChatDomainService().SelectedChat(m_app);
	if (selected_chat == nullptr)
	{
		if (inserted_chat)
		{
			ChatHistorySyncService().RollbackOpenNativeSessionChatImport(m_app, target_chat_id, previous_selected_chat_id, true);
		}
		if (!inserted_chat)
		{
			ChatHistorySyncService().RestoreOpenNativeSessionChatMetadata(*target_chat, previous_provider_id, previous_native_session_id, previous_updated_at);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, target_chat_id, had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
		}
		ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, source_chat->id, had_previous_resolved_native_session, previous_resolved_native_session_id);
		cb->Failure(404, "Selected chat no longer exists.");
		return;
	}

	selected_chat->last_opened_at = uam::time::TimestampNow();
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		selected_chat->last_opened_at = previous_last_opened_at;
		ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		if (!inserted_chat)
		{
			ChatHistorySyncService().RestoreOpenNativeSessionChatMetadata(*selected_chat, previous_provider_id, previous_native_session_id, previous_updated_at);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, target_chat_id, had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, source_chat->id, had_previous_resolved_native_session, previous_resolved_native_session_id);
		}
		if (inserted_chat)
		{
			ChatHistorySyncService().RollbackOpenNativeSessionChatImport(m_app, target_chat_id, previous_selected_chat_id, true);
		}
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
		return;
	}

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *selected_chat, "", ""))
	{
		selected_chat->last_opened_at = previous_last_opened_at;
		ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		if (!inserted_chat)
		{
			ChatHistorySyncService().RestoreOpenNativeSessionChatMetadata(*selected_chat, previous_provider_id, previous_native_session_id, previous_updated_at);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, target_chat_id, had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, source_chat->id, had_previous_resolved_native_session, previous_resolved_native_session_id);
		}
		if (inserted_chat)
		{
			ChatHistorySyncService().RollbackOpenNativeSessionChatImport(m_app, target_chat_id, previous_selected_chat_id, true);
		}
		(void)PersistenceCoordinator().SaveSettings(m_app);
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
		return;
	}

	ChatDomainService().SortChatsByRecent(m_app.chats);
	ChatDomainService().SelectChatById(m_app, target_chat_id);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleRenameSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string title = payload.value("title", "");

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatHistorySyncService().RenameChat(m_app, *chat, title))
	{
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to rename chat: " + chat_id));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatPinned(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool pinned = payload.value("pinned", false);

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (chat->pinned == pinned)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous_pinned = chat->pinned;
	chat->pinned = pinned;

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat pin updated.", "Chat pin changed in UI, but failed to save."))
	{
		chat->pinned = previous_pinned;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat pin."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatModel(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string model_id = uam::strings::Trim(payload.value("modelId", ""));

	if (!IsAllowedAcpModelId(model_id))
	{
		cb->Failure(400, "Unsupported ACP model: " + model_id);
		return;
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	if (session != nullptr && AcpSessionBlocksModelChange(*session))
	{
		cb->Failure(409, "Cannot change model while the structured runtime is busy.");
		return;
	}

	if (chat->model_id == model_id)
	{
		if (session != nullptr && session->running && !model_id.empty() && session->current_model_id != model_id)
		{
			std::string acp_error;
			if (!uam::SetAcpSessionModel(m_app, chat->id, model_id, &acp_error))
			{
				cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP model."));
				return;
			}
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const std::string previous_model_id = chat->model_id;
	const std::string previous_updated_at = chat->updated_at;
	chat->model_id = model_id;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model updated.", "Chat model changed in UI, but failed to save."))
	{
		chat->model_id = previous_model_id;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat model."));
		return;
	}

	if (session != nullptr && session->running)
	{
		std::string acp_error;
		const bool live_updated = model_id.empty() ? uam::StopAcpSession(m_app, chat->id) : uam::SetAcpSessionModel(m_app, chat->id, model_id, &acp_error);
		if (!live_updated)
		{
			chat->model_id = previous_model_id;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model reverted.", "Chat model changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP model."));
			return;
		}
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatCodexOptions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string reasoning_effort = uam::codex::NormalizeReasoningEffort(payload.value("reasoningEffort", ""));
	const std::string service_tier = uam::codex::NormalizeServiceTier(payload.value("serviceTier", ""));

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kCodexCli))
	{
		cb->Failure(409, "Codex reasoning and speed options are only available for Codex chats.");
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	if (session != nullptr && AcpSessionBlocksModelChange(*session))
	{
		cb->Failure(409, "Cannot change Codex reasoning or speed while the structured runtime is busy.");
		return;
	}

	if (chat->reasoning_effort == reasoning_effort && chat->service_tier == service_tier)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const std::string previous_reasoning_effort = chat->reasoning_effort;
	const std::string previous_service_tier = chat->service_tier;
	const std::string previous_updated_at = chat->updated_at;
	chat->reasoning_effort = reasoning_effort;
	chat->service_tier = service_tier;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Codex chat options updated.", "Codex chat options changed in UI, but failed to save."))
	{
		chat->reasoning_effort = previous_reasoning_effort;
		chat->service_tier = previous_service_tier;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist Codex chat options."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatProvider(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string provider_id = uam::strings::Trim(payload.value("providerId", ""));

	const ProviderProfile* provider = ProviderProfileStore::FindById(m_app.provider_profiles, provider_id);
	if (provider == nullptr || !ProviderRuntime::IsRuntimeEnabled(*provider))
	{
		cb->Failure(400, "Unsupported provider: " + provider_id);
		return;
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat->provider_id) == provider->id)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const std::size_t message_count = chat->messages_loaded ? chat->messages.size() : chat->persisted_message_count;
	if (message_count > 0)
	{
		cb->Failure(409, "Cannot change provider after messages have been added.");
		return;
	}

	if (uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id); session != nullptr && session->running)
	{
		cb->Failure(409, "Cannot change provider while the structured runtime is running.");
		return;
	}

	if (uam::CliTerminalState* terminal = FindCliTerminalByRoutingKey(m_app, chat->id, ""); terminal != nullptr && terminal->running)
	{
		cb->Failure(409, "Cannot change provider while the CLI terminal is running.");
		return;
	}

	const std::string previous_provider_id = chat->provider_id;
	const std::string previous_model_id = chat->model_id;
	const std::string previous_reasoning_effort = chat->reasoning_effort;
	const std::string previous_service_tier = chat->service_tier;
	const std::string previous_approval_mode = chat->approval_mode;
	const bool previous_auto_approve_commands = chat->auto_approve_commands;
	const bool previous_memory_enabled = chat->memory_enabled;
	const std::string previous_native_session_id = chat->native_session_id;
	const auto previous_resolved_native_session = m_app.resolved_native_sessions_by_chat_id.find(chat->id);
	const bool had_previous_resolved_native_session = previous_resolved_native_session != m_app.resolved_native_sessions_by_chat_id.end();
	const std::string previous_resolved_native_session_id = had_previous_resolved_native_session ? previous_resolved_native_session->second : std::string{};
	const std::string previous_updated_at = chat->updated_at;
	chat->provider_id = provider->id;
	ApplyProviderDefaultsToChat(m_app.settings, *chat);
	chat->native_session_id.clear();
	ChatHistorySyncService().ForgetResolvedNativeSessionForChat(m_app, chat->id);
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat provider updated.", "Chat provider changed in UI, but failed to save."))
	{
		chat->provider_id = previous_provider_id;
		chat->model_id = previous_model_id;
		chat->reasoning_effort = previous_reasoning_effort;
		chat->service_tier = previous_service_tier;
		chat->approval_mode = previous_approval_mode;
		chat->auto_approve_commands = previous_auto_approve_commands;
		chat->memory_enabled = previous_memory_enabled;
		chat->native_session_id = previous_native_session_id;
		if (had_previous_resolved_native_session)
		{
			m_app.resolved_native_sessions_by_chat_id[chat->id] = previous_resolved_native_session_id;
		}
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat provider."));
		return;
	}

	uam::ClearStoppedCliTerminalAttachmentForChat(m_app, chat->id);

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatApprovalMode(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string mode_id = uam::approval_modes::NormalizeIncomingApprovalModeId(payload.value("modeId", ""));

	if (!uam::approval_modes::IsAppApprovalMode(mode_id))
	{
		cb->Failure(400, "Unsupported ACP mode: " + mode_id);
		return;
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	if (session != nullptr && AcpSessionBlocksModelChange(*session))
	{
		cb->Failure(409, "Cannot change structured runtime mode while the structured runtime is busy.");
		return;
	}

	if (chat->approval_mode == mode_id)
	{
		if (session != nullptr && session->running && session->current_mode_id != mode_id)
		{
			std::string acp_error;
			if (!uam::SetAcpSessionMode(m_app, chat->id, mode_id, &acp_error))
			{
				cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP mode."));
				return;
			}
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const std::string previous_mode_id = chat->approval_mode;
	const std::string previous_updated_at = chat->updated_at;
	chat->approval_mode = mode_id;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat mode updated.", "Chat mode changed in UI, but failed to save."))
	{
		chat->approval_mode = previous_mode_id;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat mode."));
		return;
	}

	if (session != nullptr && session->running)
	{
		std::string acp_error;
		if (!uam::SetAcpSessionMode(m_app, chat->id, mode_id, &acp_error))
		{
			chat->approval_mode = previous_mode_id;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat mode reverted.", "Chat mode changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP mode."));
			return;
		}
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatAutoApproveCommands(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool enabled = payload.value("enabled", false);
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (chat->auto_approve_commands == enabled)
	{
		if (enabled && !AutoApprovePendingAcpPermissionOrFail(m_app, chat->id, cb))
		{
			return;
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous = chat->auto_approve_commands;
	const std::string previous_updated_at = chat->updated_at;
	chat->auto_approve_commands = enabled;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat auto-approval updated.", "Chat auto-approval changed in UI, but failed to save."))
	{
		chat->auto_approve_commands = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat auto-approval."));
		return;
	}

	if (enabled && !AutoApprovePendingAcpPermissionOrFail(m_app, chat->id, cb))
	{
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatMemoryEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool enabled = payload.value("enabled", true);
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (chat->memory_enabled == enabled)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous = chat->memory_enabled;
	const std::string previous_updated_at = chat->updated_at;
	chat->memory_enabled = enabled;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat memory setting updated.", "Chat memory setting changed in UI, but failed to save."))
	{
		chat->memory_enabled = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat memory setting."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetMemorySettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (const std::optional<bool> enabled_default = uam::nlohmann_json::BoolFieldStrict(payload, "enabledDefault"))
	{
		m_app.settings.memory_enabled_default = *enabled_default;
	}
	if (const std::optional<int> idle_delay_seconds = uam::nlohmann_json::IntFieldStrict(payload, "idleDelaySeconds"))
	{
		m_app.settings.memory_idle_delay_seconds = *idle_delay_seconds;
	}
	if (const std::optional<int> recall_budget_bytes = uam::nlohmann_json::IntFieldStrict(payload, "recallBudgetBytes"))
	{
		m_app.settings.memory_recall_budget_bytes = *recall_budget_bytes;
	}
	uam::settings::ClampMemorySettings(m_app.settings);
	if (const nlohmann::json* worker_bindings = uam::nlohmann_json::FindObjectField(payload, "workerBindings"); worker_bindings != nullptr)
	{
		for (auto it = worker_bindings->begin(); it != worker_bindings->end(); ++it)
		{
			if (!it.value().is_object())
			{
				continue;
			}
			const std::string chat_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(it.key());
			const std::string worker_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(it.value().value("workerProviderId", ""));
			const std::string worker_model_id = uam::strings::Trim(it.value().value("workerModelId", ""));
			if (chat_provider_id.empty() || worker_provider_id.empty() || ProviderProfileStore::FindById(m_app.provider_profiles, chat_provider_id) == nullptr || ProviderProfileStore::FindById(m_app.provider_profiles, worker_provider_id) == nullptr)
			{
				continue;
			}
			m_app.settings.memory_worker_bindings[chat_provider_id] = MemoryWorkerBinding{worker_provider_id, worker_model_id};
		}
	}

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist memory settings."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetProviderChatDefaults(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string requested_default_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(payload.value("defaultProviderId", m_app.settings.default_new_chat_provider_id));
	if (!requested_default_provider_id.empty())
	{
		const ProviderProfile* provider = ProviderProfileStore::FindById(m_app.provider_profiles, requested_default_provider_id);
		if (provider == nullptr || !ProviderRuntime::IsRuntimeEnabled(*provider))
		{
			cb->Failure(400, "Unsupported default provider: " + requested_default_provider_id);
			return;
		}
		m_app.settings.default_new_chat_provider_id = provider->id;
	}

	if (const nlohmann::json* defaults_by_provider = uam::nlohmann_json::FindObjectField(payload, "defaults"); defaults_by_provider != nullptr)
	{
		for (auto it = defaults_by_provider->begin(); it != defaults_by_provider->end(); ++it)
		{
			const std::string provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(it.key());
			const ProviderProfile* provider = ProviderProfileStore::FindById(m_app.provider_profiles, provider_id);
			if (provider == nullptr || !ProviderRuntime::IsRuntimeEnabled(*provider))
			{
				continue;
			}
			const ProviderChatDefaults fallback = DefaultsForProvider(m_app.settings, provider->id);
			ProviderChatDefaults defaults = DefaultsFromPayload(it.value(), fallback);
			if (!uam::provider_ids::IsCliProviderAliasOf(provider->id, uam::provider_ids::kCodexCli))
			{
				defaults.reasoning_effort.clear();
				defaults.service_tier.clear();
			}
			m_app.settings.provider_chat_defaults[provider->id] = defaults;
		}
	}

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist provider chat defaults."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetEditorSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string default_editor_preset_id = uam::strings::Trim(payload.value("defaultEditorPresetId", m_app.settings.default_editor_preset_id));
	m_app.settings.default_editor_preset_id = uam::editor_file_associations::NormalizeEditorPresetId(default_editor_preset_id);

	std::vector<EditorFileAssociation> associations = ParseEditorFileAssociationsPayload(payload);
	if (associations.empty())
	{
		associations = uam::editor_file_associations::DefaultEditorFileAssociations();
	}
	m_app.settings.editor_file_associations = std::move(associations);
	m_app.settings.editor_default_groups_version = uam::editor_file_associations::kDefaultGroupsVersion;

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist editor settings."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleRefreshCliProviderVersion(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string provider_id = CliVersionProviderFromPayloadOrSelection(m_app, payload);
	if (ProviderProfileStore::FindById(m_app.provider_profiles, provider_id) == nullptr)
	{
		cb->Failure(400, "Unsupported provider: " + provider_id);
		return;
	}

	ProviderCliCompatibilityService().StartProviderVersionCheck(m_app, provider_id, true);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleApplyCliProviderVersion(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string provider_id = CliVersionProviderFromPayloadOrSelection(m_app, payload);
	const std::string version = uam::strings::Trim(payload.value("version", ""));
	std::string error;
	if (!ProviderCliCompatibilityService().StartInstallProviderVersion(m_app, provider_id, version, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to start provider CLI install."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleBrowseMarkdownStoreDirectory(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string current_value = payload.value("currentValue", m_app.settings.markdown_store_directory);
	const std::filesystem::path initial_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(current_value);
	std::string selected_path;
	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(PlatformPathBrowseTarget::Directory, initial_path, &selected_path, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to browse Markdown Store directory."));
		return;
	}
	cb->Success(nlohmann::json{{"selectedPath", selected_path}}.dump());
}

void UamQueryHandler::HandleSetMarkdownStoreDirectory(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string directory = uam::strings::Trim(payload.value("directory", ""));
	if (!directory.empty())
	{
		const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(directory);
		std::string error;
		if (!MarkdownStoreService::IsConfiguredRoot(root, &error))
		{
			cb->Failure(400, FailureDetailOrFallback(error, "Invalid Markdown Store directory."));
			return;
		}
		m_app.settings.markdown_store_directory = root.string();
	}
	else
	{
		m_app.settings.markdown_store_directory.clear();
	}

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist Markdown Store directory."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"directory", m_app.settings.markdown_store_directory}}.dump());
}

void UamQueryHandler::HandleListMarkdownStoreEntries(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(m_app.settings.markdown_store_directory);
	const std::string request_id = payload.value("requestId", "");
	RunAsyncCefQuery(cb,
	                 [root, request_id]()
	                 {
		                 std::string error;
		                 std::vector<MarkdownStoreService::Entry> entries = MarkdownStoreService::ListEntries(root, &error);
		                 if (!error.empty())
		                 {
			                 return AsyncFailure(400, error);
		                 }

		                 nlohmann::json entry_json = nlohmann::json::array();
		                 for (const MarkdownStoreService::Entry& entry : entries)
		                 {
			                 entry_json.push_back(SerializeMarkdownStoreEntry(entry));
		                 }
		                 return AsyncSuccess(WithOptionalRequestId(nlohmann::json{{"directory", root.string()}, {"entries", entry_json}}, request_id));
	                 });
}

void UamQueryHandler::HandleCreateMarkdownStoreEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(m_app.settings.markdown_store_directory);
	MarkdownStoreService::Draft draft;
	draft.title = payload.value("title", "");
	draft.maker = payload.value("maker", "");
	draft.review = payload.value("review", "");
	draft.body = payload.value("body", "");

	MarkdownStoreService::Entry created;
	std::string error;
	if (!MarkdownStoreService::CreateEntry(root, draft, &created, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to create Markdown Store entry."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(SerializeMarkdownStoreEntry(created).dump());
}

void UamQueryHandler::HandleRevealMarkdownStoreEntry(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(m_app.settings.markdown_store_directory);
	const std::string file_path = payload.value("filePath", "");
	std::filesystem::path normalized_file;
	std::string error;
	if (!MarkdownStoreService::ValidateStoreFilePath(root, file_path, &normalized_file, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Invalid Markdown Store file."));
		return;
	}

	if (!PlatformServicesFactory::Instance().file_dialog_service.RevealPathInFileManager(normalized_file, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to reveal Markdown Store file."));
		return;
	}
	cb->Success("{}");
}

void UamQueryHandler::HandleDeleteSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	if (FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id) == nullptr)
	{
		return;
	}

	if (!RemoveChatById(m_app, chat_id))
	{
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "Failed to delete chat: " + chat_id));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleCreateFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string title = payload.value("title", "New Folder");
	const std::string directory = payload.value("directory", "");
	std::string created_folder_id;

	if (!CreateFolder(m_app, title, directory, &created_folder_id))
	{
		cb->Failure(400, m_app.status_line);
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	ChatFolder* folder = ChatDomainService().FindFolderById(m_app, created_folder_id);
	if (folder == nullptr)
	{
		cb->Success("{}");
		return;
	}

	cb->Success(uam::StateSerializer::SerializeFolder(*folder).dump());
}

void UamQueryHandler::HandleRenameFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string folder_id = payload.value("folderId", "");
	const std::string title = payload.value("title", "");
	const std::string directory = payload.value("directory", "");

	if (!RenameFolderById(m_app, folder_id, title, directory))
	{
		cb->Failure(FolderFailureCode(m_app.status_line), m_app.status_line);
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleDeleteFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string folder_id = payload.value("folderId", "");

	if (!DeleteFolderById(m_app, folder_id))
	{
		cb->Failure(FolderFailureCode(m_app.status_line), m_app.status_line);
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleToggleFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	(void)browser;
	const std::string folder_id = payload.value("folderId", "");
	ChatFolder* folder = ChatDomainService().FindFolderById(m_app, folder_id);
	if (!folder)
	{
		cb->Failure(404, "Folder not found: " + folder_id);
		return;
	}

	folder->collapsed = !folder->collapsed;

	if (!ChatFolderStore::Save(m_app.data_root, m_app.folders))
	{
		folder->collapsed = !folder->collapsed;
		cb->Failure(500, "Failed to persist folder state.");
		return;
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleBrowseFolderDirectory(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string current_value = payload.value("currentValue", "");
	const std::filesystem::path initial_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(current_value);

	std::string selected_path;
	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(PlatformPathBrowseTarget::Directory, initial_path, &selected_path, &error))
	{
		if (!error.empty())
		{
			cb->Failure(500, error);
		}
		else
		{
			nlohmann::json result;
			result["selectedPath"] = "";
			cb->Success(result.dump());
		}
		return;
	}

	nlohmann::json result;
	result["selectedPath"] = selected_path;
	cb->Success(result.dump());
}

void UamQueryHandler::HandleSearchChatMessages(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::vector<std::string> tokens = SearchTokens(payload.value("query", ""));
	const std::filesystem::path data_root = m_app.data_root;
	const std::string request_id = payload.value("requestId", "");
	nlohmann::json result;
	result["chatIds"] = nlohmann::json::array();
	if (!request_id.empty())
	{
		result["requestId"] = request_id;
	}
	if (tokens.empty())
	{
		cb->Success(result.dump());
		return;
	}

	RunAsyncCefQuery(cb,
	                 [data_root, tokens, request_id]()
	                 {
		                 nlohmann::json async_result;
		                 async_result["chatIds"] = nlohmann::json::array();
		                 if (!request_id.empty())
		                 {
			                 async_result["requestId"] = request_id;
		                 }

		                 std::string warning;
		                 const std::vector<ChatSession> chats = ChatRepository::LoadLocalChats(data_root, &warning);
		                 for (const ChatSession& chat : chats)
		                 {
			                 std::string haystack = uam::strings::ToLowerAscii(chat.title + " " + chat.provider_id + " " + chat.workspace_directory);
			                 for (const Message& message : chat.messages)
			                 {
				                 haystack += " " + uam::strings::ToLowerAscii(message.content);
				                 haystack += " " + uam::strings::ToLowerAscii(message.thoughts);
				                 haystack += " " + uam::strings::ToLowerAscii(message.plan_summary);
			                 }

			                 const bool matches = std::ranges::all_of(tokens, [&](const std::string& token) { return uam::strings::Contains(haystack, token); });
			                 if (matches)
			                 {
				                 async_result["chatIds"].push_back(chat.id);
			                 }
		                 }

		                 if (!warning.empty())
		                 {
			                 async_result["warning"] = warning;
		                 }
		                 return AsyncSuccess(std::move(async_result));
	                 });
}

void UamQueryHandler::HandleListMemoryEntries(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	MemoryLibraryService::Scope scope;
	std::string error;
	if (!MemoryLibraryService::ResolveScope(m_app, payload.value("scopeType", "global"), payload.value("folderId", ""), scope, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to resolve memory scope."));
		return;
	}
	const std::string request_id = payload.value("requestId", "");

	RunAsyncCefQuery(cb,
	                 [scope, request_id]()
	                 {
		                 std::string error;
		                 const std::vector<MemoryLibraryService::Entry> entries = MemoryLibraryService::ListEntries(scope, &error);
		                 if (!error.empty())
		                 {
			                 return AsyncFailure(500, error);
		                 }

		                 nlohmann::json scope_json;
		                 scope_json["scopeType"] = scope.scope_type;
		                 scope_json["folderId"] = scope.folder_id;
		                 scope_json["label"] = scope.label;
		                 scope_json["rootPath"] = uam::strings::NonEmptyOrFallback(scope.root_path.string(), "Global and project memory roots");
		                 scope_json["rootCount"] = scope.roots.size();

		                 nlohmann::json response;
		                 response["scope"] = std::move(scope_json);
		                 response["entries"] = nlohmann::json::array();
		                 for (const MemoryLibraryService::Entry& entry : entries)
		                 {
			                 response["entries"].push_back({
			                     {"id", entry.id},
			                     {"title", entry.title},
			                     {"category", entry.category},
			                     {"scope", entry.scope},
			                     {"confidence", entry.confidence},
			                     {"sourceChatId", entry.source_chat_id},
			                     {"lastObserved", entry.last_observed},
			                     {"occurrenceCount", entry.occurrence_count},
			                     {"preview", entry.preview},
			                     {"filePath", entry.file_path.string()},
			                     {"scopeType", entry.scope_type},
			                     {"folderId", entry.folder_id},
			                     {"scopeLabel", entry.scope_label},
			                     {"rootPath", entry.root_path.string()},
			                 });
		                 }
		                 return AsyncSuccess(WithOptionalRequestId(std::move(response), request_id));
	                 });
}

void UamQueryHandler::HandleCreateMemoryEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string requested_scope_type = payload.value("scopeType", "global");
	const std::string requested_folder_id = payload.value("folderId", "");
	std::string concrete_scope_type = requested_scope_type;
	std::string concrete_folder_id = requested_folder_id;
	if (uam::strings::Trim(requested_scope_type) == "all")
	{
		concrete_scope_type = payload.value("targetScopeType", "");
		concrete_folder_id = payload.value("targetFolderId", "");
		if (uam::strings::IsBlank(concrete_scope_type))
		{
			cb->Failure(400, "A target memory scope is required.");
			return;
		}
	}

	MemoryLibraryService::Scope scope;
	std::string error;
	if (!MemoryLibraryService::ResolveScope(m_app, concrete_scope_type, concrete_folder_id, scope, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to resolve memory scope."));
		return;
	}

	MemoryLibraryService::Draft draft;
	draft.category = payload.value("category", "");
	draft.title = payload.value("title", "");
	draft.memory = payload.value("memory", "");
	draft.evidence = payload.value("evidence", "");
	draft.confidence = payload.value("confidence", "medium");
	draft.source_chat_id = payload.value("sourceChatId", "");

	MemoryLibraryService::Entry created;
	if (!MemoryLibraryService::CreateEntry(scope, draft, &created, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to create memory entry."));
		return;
	}

	nlohmann::json response;
	response["id"] = created.id;
	response["title"] = created.title;
	response["category"] = created.category;
	response["scope"] = created.scope;
	response["confidence"] = created.confidence;
	response["sourceChatId"] = created.source_chat_id;
	response["lastObserved"] = created.last_observed;
	response["occurrenceCount"] = created.occurrence_count;
	response["preview"] = created.preview;
	response["filePath"] = created.file_path.string();
	response["scopeType"] = created.scope_type;
	response["folderId"] = created.folder_id;
	response["scopeLabel"] = created.scope_label;
	response["rootPath"] = created.root_path.string();
	MemoryService::RefreshMemoryActivity(m_app);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(response.dump());
}

void UamQueryHandler::HandleDeleteMemoryEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	MemoryLibraryService::Scope scope;
	std::string error;
	if (!MemoryLibraryService::ResolveScope(m_app, payload.value("scopeType", "global"), payload.value("folderId", ""), scope, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to resolve memory scope."));
		return;
	}

	if (!MemoryLibraryService::DeleteEntry(scope, payload.value("entryId", ""), &error))
	{
		cb->Failure(404, FailureDetailOrFallback(error, "Failed to delete memory entry."));
		return;
	}

	MemoryService::RefreshMemoryActivity(m_app);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleOpenMemoryRoot(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	MemoryLibraryService::Scope scope;
	std::string error;
	if (!MemoryLibraryService::ResolveScope(m_app, payload.value("scopeType", "global"), payload.value("folderId", ""), scope, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to resolve memory scope."));
		return;
	}
	if (scope.scope_type == "all")
	{
		cb->Failure(400, "The all memory view has multiple roots. Reveal a memory entry instead.");
		return;
	}

	if (!MemoryService::EnsureMemoryLayout(scope.root_path))
	{
		cb->Failure(500, "Failed to create memory root.");
		return;
	}

	if (!PlatformServicesFactory::Instance().file_dialog_service.OpenFolderInFileManager(scope.root_path, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to open memory root."));
		return;
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleRevealMemoryEntry(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	MemoryLibraryService::Scope scope;
	std::string error;
	if (!MemoryLibraryService::ResolveScope(m_app, payload.value("scopeType", "global"), payload.value("folderId", ""), scope, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to resolve memory scope."));
		return;
	}

	const std::string entry_id = payload.value("entryId", "");
	if (entry_id.empty())
	{
		cb->Failure(400, "Memory entry id is required.");
		return;
	}

	const std::vector<MemoryLibraryService::Entry> entries = MemoryLibraryService::ListEntries(scope, &error);
	if (!error.empty())
	{
		cb->Failure(500, error);
		return;
	}

	for (const MemoryLibraryService::Entry& entry : entries)
	{
		if (entry.id != entry_id)
		{
			continue;
		}

		if (!PlatformServicesFactory::Instance().file_dialog_service.RevealPathInFileManager(entry.file_path, &error))
		{
			cb->Failure(500, FailureDetailOrFallback(error, "Failed to reveal memory file."));
			return;
		}

		cb->Success("{}");
		return;
	}

	cb->Failure(404, "Memory entry not found: " + entry_id);
}

void UamQueryHandler::HandleOpenWorkspaceDirectory(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(m_app, *chat);
	if (workspace_root.empty())
	{
		cb->Failure(400, "Chat has no workspace directory.");
		return;
	}

	if (!uam::paths::PathExistsNoThrow(workspace_root))
	{
		cb->Failure(404, "Workspace directory does not exist.");
		return;
	}

	if (!uam::paths::IsDirectoryNoThrow(workspace_root))
	{
		cb->Failure(400, "Workspace path is not a directory.");
		return;
	}

	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.OpenFolderInFileManager(workspace_root, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to open workspace directory."));
		return;
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleOpenWorkspaceEditor(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(m_app, *chat);
	if (workspace_root.empty())
	{
		cb->Failure(400, "Chat has no workspace directory.");
		return;
	}

	if (!uam::paths::PathExistsNoThrow(workspace_root))
	{
		cb->Failure(404, "Workspace directory does not exist.");
		return;
	}

	if (!uam::paths::IsDirectoryNoThrow(workspace_root))
	{
		cb->Failure(400, "Workspace path is not a directory.");
		return;
	}

	const std::string editor_preset_id = SelectEditorPresetForWorkspace(m_app.settings, workspace_root);
	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.OpenFolderInEditorPreset(workspace_root, editor_preset_id, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to open workspace editor."));
		return;
	}

	cb->Success(nlohmann::json{{"editorPresetId", editor_preset_id}}.dump());
}

void UamQueryHandler::HandleOpenWorkspaceTerminal(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(m_app, *chat);
	if (workspace_root.empty())
	{
		cb->Failure(400, "Chat has no workspace directory.");
		return;
	}

	if (!uam::paths::PathExistsNoThrow(workspace_root))
	{
		cb->Failure(404, "Workspace directory does not exist.");
		return;
	}

	if (!uam::paths::IsDirectoryNoThrow(workspace_root))
	{
		cb->Failure(400, "Workspace path is not a directory.");
		return;
	}

	std::string error;
	if (!PlatformServicesFactory::Instance().process_service.LaunchShellAt(workspace_root, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to open terminal."));
		return;
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleGetChatWorktreeStatus(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const uam::GitWorktreeStatus status = uam::GitWorktreeService().Status(m_app, *chat);
	cb->Success(SerializeGitWorktreeStatus(status).dump());
}

void UamQueryHandler::HandleCreateChatWorktree(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before changing workspace isolation.");
		return;
	}

	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().CreateForChat(m_app, *chat);
	if (!result.ok)
	{
		cb->Failure(400, FailureDetailOrFallback(result.message, "Failed to create isolated Git worktree."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(SerializeGitWorktreeResult(result).dump());
}

void UamQueryHandler::HandleDiscardChatWorktreeChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before discarding worktree changes.");
		return;
	}

	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().DiscardChatChanges(m_app, *chat);
	if (!result.ok)
	{
		cb->Failure(400, FailureDetailOrFallback(result.message, "Failed to discard worktree changes."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(SerializeGitWorktreeResult(result).dump());
}

void UamQueryHandler::HandlePortChatWorktreeChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before porting worktree changes.");
		return;
	}

	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().PortChatChanges(m_app, *chat);
	if (!result.ok)
	{
		std::string message = FailureDetailOrFallback(result.message, "Failed to port worktree changes.");
		if (!result.patch_path.empty())
		{
			message += "\nPatch saved at: " + result.patch_path.string();
		}
		cb->Failure(400, message);
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(SerializeGitWorktreeResult(result).dump());
}

void UamQueryHandler::HandleGetVcsCommitStatus(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const uam::VcsType requested_type = uam::VcsTypeFromString(payload.value("vcsType", "git"));
	const bool include_line_stats = payload.value("includeLineStats", true);
	const std::string request_id = payload.value("requestId", "");
	const ChatSession chat_snapshot = *chat;
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);

	RunAsyncCefQuery(cb,
	                 [snapshot_inputs = std::move(snapshot_inputs), chat = std::move(chat_snapshot), requested_type, include_line_stats, request_id]() mutable
	                 {
		                 uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		                 const uam::VcsCommitStatus status = uam::VcsCommitService().Status(snapshot, chat, requested_type, include_line_stats);
		                 return AsyncSuccess(WithOptionalRequestId(SerializeVcsCommitStatus(status), request_id));
	                 });
}

void UamQueryHandler::HandleGetVcsFileDiff(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const std::string path = payload.value("path", "");
	const uam::VcsType type = uam::VcsTypeFromString(payload.value("vcsType", "git"));
	const std::string request_id = payload.value("requestId", "");
	const ChatSession chat_snapshot = *chat;
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);

	RunAsyncCefQuery(cb,
	                 [snapshot_inputs = std::move(snapshot_inputs), chat = std::move(chat_snapshot), path, type, request_id]() mutable
	                 {
		                 uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		                 std::string error;
		                 const std::string diff = uam::VcsCommitService().Diff(snapshot, chat, path, type, &error);
		                 if (!error.empty())
		                 {
			                 return AsyncFailure(400, error);
		                 }
		                 return AsyncSuccess(WithOptionalRequestId(nlohmann::json{{"diff", diff}}, request_id));
	                 });
}

void UamQueryHandler::HandleCommitVcsChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const std::vector<std::string> files = uam::nlohmann_json::TrimmedStringArrayField(payload, "files");
	const std::string message = payload.value("message", "");
	const uam::VcsType type = uam::VcsTypeFromString(payload.value("vcsType", "git"));
	const uam::VcsCommitResult result = uam::VcsCommitService().Commit(m_app, *chat, type, message, files);
	if (!result.ok)
	{
		cb->Failure(400, FailureDetailOrFallback(result.error, "Failed to commit changes."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(SerializeVcsCommitResult(result).dump());
}

void UamQueryHandler::HandleGenerateVcsCommitMessage(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	std::vector<std::string> files = uam::nlohmann_json::TrimmedStringArrayField(payload, "files");
	const uam::VcsType type = uam::VcsTypeFromString(payload.value("vcsType", "git"));
	const std::string request_id = payload.value("requestId", "");
	const ChatSession chat_snapshot = *chat;
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);

	RunAsyncCefQuery(cb,
	                 [snapshot_inputs = std::move(snapshot_inputs), chat = std::move(chat_snapshot), type, files = std::move(files), request_id]() mutable
	                 {
		                 uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		                 const uam::VcsCommitMessageSuggestion suggestion = uam::VcsCommitService().GenerateMessage(snapshot, chat, type, files);
		                 if (!suggestion.ok)
		                 {
			                 return AsyncFailure(400, FailureDetailOrFallback(suggestion.error, "Failed to generate commit message."));
		                 }

		                 return AsyncSuccess(WithOptionalRequestId(
		                     nlohmann::json{
		                         {"title", suggestion.title},
		                         {"description", suggestion.description},
		                     },
		                     request_id));
	                 });
}

void UamQueryHandler::HandleListMemoryScanCandidates(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& /*payload*/, CefRefPtr<Callback> cb)
{
	const std::vector<MemoryService::ManualScanCandidate> candidates = MemoryService::ListManualScanCandidates(m_app);
	nlohmann::json response;
	response["candidates"] = nlohmann::json::array();
	for (const MemoryService::ManualScanCandidate& candidate : candidates)
	{
		response["candidates"].push_back({
		    {"chatId", candidate.chat_id},
		    {"title", candidate.title},
		    {"folderId", candidate.folder_id},
		    {"folderTitle", candidate.folder_title},
		    {"providerId", candidate.provider_id},
		    {"messageCount", candidate.message_count},
		    {"memoryEnabled", candidate.memory_enabled},
		    {"memoryLastProcessedAt", candidate.memory_last_processed_at},
		    {"alreadyFullyProcessed", candidate.already_fully_processed},
		});
	}
	cb->Success(response.dump());
}

void UamQueryHandler::HandleScanCurrentChats(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::vector<std::string> chat_ids = uam::nlohmann_json::TrimmedStringArrayField(payload, "chatIds");
	if (chat_ids.empty() && uam::nlohmann_json::FindArrayField(payload, "chatIds") == nullptr)
	{
		cb->Failure(400, "chatIds is required.");
		return;
	}

	std::string error;
	int queued_count = 0;
	if (!MemoryService::QueueManualScan(m_app, chat_ids, &queued_count, &error))
	{
		cb->Failure(409, FailureDetailOrFallback(error, "No chats were queued for memory scanning."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	nlohmann::json response;
	response["queuedCount"] = queued_count;
	cb->Success(response.dump());
}

void UamQueryHandler::HandleStartCli(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const int rows = payload.value("rows", uam::kCliTerminalDefaultRows);
	const int cols = payload.value("cols", uam::kCliTerminalDefaultCols);
	const std::string terminal_id = payload.value("terminalId", "");
	uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "request_received", nullptr, "chat_id=" + chat_id + ", terminal_id=" + terminal_id);

	if (uam::CliTerminalState* existing = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id); existing != nullptr && existing->running)
	{
		if (existing->lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown)
		{
			uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "restart_shutting_down_terminal", existing);
			uam::StopCliTerminal(*existing, false, uam::CliTerminalStopMode::FastExit);
		}
		else
		{
			ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
			if (chat == nullptr)
			{
				return;
			}

			const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(m_app, *chat);
			uam::RepairCliTerminalIdentityForChat(m_app, *existing, *chat, provider);
			uam::CliTerminalState& terminal = *existing;
			terminal.ui_attached = true;
			terminal.rows = uam::ClampCliTerminalResizeRows(rows);
			terminal.cols = uam::ClampCliTerminalResizeCols(cols);
			PlatformServicesFactory::Instance().terminal_runtime.ResizeCliTerminal(terminal);
			uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "reused_running_terminal", &terminal);
			cb->Success(BuildCliBindingResponse(terminal).dump());
			return;
		}
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	std::string hydrate_warning;
	if (!ChatRepository::HydrateChatMessages(m_app.data_root, *chat, &hydrate_warning))
	{
		cb->Failure(500, FailureDetailOrFallback(hydrate_warning, "Failed to load chat messages."));
		return;
	}
	uam::CliTerminalState& terminal = uam::EnsureCliTerminalForChat(m_app, *chat);
	terminal.frontend_chat_id = chat->id;
	terminal.ui_attached = true;
	if (terminal.terminal_id.empty())
	{
		terminal.terminal_id = "term-" + chat->id;
	}
	uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "terminal_prepared", &terminal);

	if (!ProviderResolutionService().ChatProviderIsAvailable(m_app, *chat))
	{
		terminal.running = false;
		terminal.generation_in_progress = false;
		terminal.turn_state = uam::CliTerminalTurnState::Idle;
		terminal.should_launch = false;
		terminal.last_error = ProviderResolutionService().ChatProviderUnavailableReason(m_app, *chat);
		terminal.lifecycle_state = uam::CliTerminalLifecycleState::Stopped;
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(BuildCliBindingResponse(terminal).dump());
		return;
	}

	if (!terminal.running)
	{
		if (!uam::StartCliTerminalForChat(m_app, terminal, *chat, rows, cols))
		{
			uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "start_failed", &terminal, terminal.last_error);
			cb->Success(BuildCliBindingResponse(terminal).dump());
			return;
		}

		uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "started_terminal", &terminal);
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(BuildCliBindingResponse(terminal).dump());
}

void UamQueryHandler::HandleStopCli(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string terminal_id = payload.value("terminalId", "");
	uam::CliTerminalState* term = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id);

	if (term == nullptr)
	{
		cb->Success("{}");
		return;
	}

	term->ui_attached = false;
	uam::LogCliDiagnosticEvent(m_app, "handle_stop_cli", "ui_detached", term);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleResizeCli(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string terminal_id = payload.value("terminalId", "");
	const int rows = uam::ClampCliTerminalResizeRows(payload.value("rows", uam::kCliTerminalDefaultRows));
	const int cols = uam::ClampCliTerminalResizeCols(payload.value("cols", uam::kCliTerminalDefaultCols));

	if (uam::CliTerminalState* term = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id); term != nullptr && term->running)
	{
		term->rows = rows;
		term->cols = cols;
		PlatformServicesFactory::Instance().terminal_runtime.ResizeCliTerminal(*term);
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleWriteCliInput(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string terminal_id = payload.value("terminalId", "");
	const std::string data = payload.value("data", "");

	if (!data.empty())
	{
		if (uam::CliTerminalState* term = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id); term != nullptr && term->running && term->lifecycle_state != uam::CliTerminalLifecycleState::ShuttingDown)
		{
			// Write raw PTY bytes directly to the terminal master fd.
			// xterm.js sends individual keystrokes and escape sequences that
			// must reach the child process unmodified — do NOT queue these as
			// structured prompts (which wrap them in bracketed-paste sequences
			// and append \r, breaking all interactive CLI communication).
			const bool wrote = uam::WriteToCliTerminal(*term, data.c_str(), data.size());
			uam::LogCliDiagnosticEvent(m_app, "handle_write_cli_input", wrote ? "pty_write_ok" : "pty_write_failed", term, "", static_cast<long long>(data.size()));
			if (wrote && CliInputLooksLikeTurnSubmit(data))
			{
				uam::MarkCliTerminalTurnBusy(*term);
				uam::LogCliDiagnosticEvent(m_app, "handle_write_cli_input", "turn_marked_busy_from_submit", term);
				uam::PushStateUpdateIfChanged(browser, m_app);
			}
		}
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleSendAcpPrompt(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string text = payload.value("text", "");
	const int missing_chat_status = chat_id.empty() || text.empty() ? 400 : 404;
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id, missing_chat_status);
	if (chat == nullptr)
	{
		return;
	}

	std::string hydrate_warning;
	if (!ChatRepository::HydrateChatMessages(m_app.data_root, *chat, &hydrate_warning))
	{
		cb->Failure(500, FailureDetailOrFallback(hydrate_warning, "Failed to load chat messages."));
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	const std::vector<std::string> markdown_store_files = uam::nlohmann_json::StringArrayField(payload, "markdownStoreFiles");

	std::string error;
	const std::vector<MessageAttachment> attachments = ParseStagedAttachments(payload);
	const bool goal_mode = payload.value("goalMode", false);
	if (!uam::SendAcpPrompt(m_app, chat_id, text, markdown_store_files, attachments, goal_mode, &error))
	{
		cb->Failure(chat_id.empty() || text.empty() ? 400 : 500, FailureDetailOrFallback(error, "Failed to send ACP prompt."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleStageChatAttachments(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	(void)browser;
	const std::string chat_id = payload.value("chatId", "");
	if (chat_id.empty())
	{
		cb->Failure(400, "Missing chat id.");
		return;
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	const nlohmann::json* items = uam::nlohmann_json::FindArrayField(payload, attachment_frontend_fields::kItemsField);
	if (items == nullptr)
	{
		cb->Failure(400, "Attachment staging requires an items array.");
		return;
	}

	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(m_app, *chat);
	if (workspace_root.empty())
	{
		cb->Failure(400, "Chat has no workspace directory.");
		return;
	}

	std::error_code ec;
	if (!uam::paths::CreateDirectoriesNoThrow(workspace_root, &ec))
	{
		cb->Failure(500, "Failed to create workspace directory.");
		return;
	}

	const std::filesystem::path attachment_root = workspace_root / ".UAM" / "attachments" / chat_id;
	if (!uam::paths::CreateDirectoriesNoThrow(attachment_root, &ec))
	{
		cb->Failure(500, "Failed to create attachment directory.");
		return;
	}

	auto staged = nlohmann::json::array();
	std::size_t index = 0;
	for (const nlohmann::json& item : *items)
	{
		if (!item.is_object())
		{
			continue;
		}

		const std::string requested_kind = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kKindField});
		const std::string mime_type = uam::nlohmann_json::TrimmedStringValue(item, {attachment_frontend_fields::kMimeTypeInputField});
		const std::string source_path_text = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kPathField});
		const std::string attachment_kind = NormalizeStagedAttachmentKind(requested_kind);
		const bool is_directory = attachment_kind == attachment_frontend_fields::kDirectoryKind;
		MessageAttachment attachment;
		attachment.id = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kIdField});
		if (attachment.id.empty())
		{
			attachment.id = AttachmentId();
		}
		const std::string fallback_name = source_path_text.empty() ? "attachment" : std::filesystem::path(source_path_text).filename().string();
		attachment.name = SafeAttachmentName(uam::strings::TrimOrFallback(uam::nlohmann_json::StringViewOrEmpty(item, attachment_fields::kNameField), fallback_name), "attachment");
		attachment.kind = attachment_kind;
		attachment.mime_type = mime_type;

		if (is_directory)
		{
			if (source_path_text.empty())
			{
				cb->Failure(400, "Directory attachments require a filesystem path.");
				return;
			}
			const std::filesystem::path source_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(source_path_text);
			if (!uam::paths::IsDirectoryNoThrow(source_path))
			{
				cb->Failure(400, "Directory attachment does not exist: " + source_path_text);
				return;
			}
			attachment.path = PathForPrompt(workspace_root, source_path);
			attachment.copied = false;
			staged.push_back(AttachmentToJson(attachment));
			continue;
		}

		std::string bytes;
		if (const nlohmann::json* data_base64 = uam::nlohmann_json::FindStringField(item, attachment_frontend_fields::kDataBase64Field); data_base64 != nullptr)
		{
			if (!uam::base64::Decode(data_base64->get_ref<const std::string&>(), bytes))
			{
				cb->Failure(400, "Attachment data is not valid base64.");
				return;
			}
			if (bytes.size() > kMaxAttachmentBytes)
			{
				cb->Failure(413, "Attachment is larger than the 25 MB limit.");
				return;
			}
		}
		else if (!source_path_text.empty())
		{
			const std::filesystem::path source_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(source_path_text);
			if (!uam::paths::IsRegularFileNoThrow(source_path))
			{
				cb->Failure(400, "File attachment does not exist: " + source_path_text);
				return;
			}
			const std::optional<std::uintmax_t> source_size = uam::paths::FileSizeNoThrow(source_path);
			if (!source_size || *source_size > kMaxAttachmentBytes)
			{
				cb->Failure(413, "Attachment is larger than the 25 MB limit.");
				return;
			}
			if (!uam::io::TryReadBinaryFile(source_path, bytes))
			{
				cb->Failure(500, "Failed to read attachment: " + source_path_text);
				return;
			}
			if (bytes.size() > kMaxAttachmentBytes)
			{
				cb->Failure(413, "Attachment is larger than the 25 MB limit.");
				return;
			}
		}
		else
		{
			cb->Failure(400, "File attachments require data or a filesystem path.");
			return;
		}

		const std::string prefix = uam::time::SystemEpochMicrosecondsTokenNow() + "-" + std::to_string(index++);
		const std::filesystem::path target = attachment_root / (prefix + "-" + attachment.name);
		if (!uam::io::WriteBinaryFile(target, bytes))
		{
			cb->Failure(500, "Failed to write attachment.");
			return;
		}
		attachment.path = PathForPrompt(workspace_root, target);
		attachment.size_bytes = bytes.size();
		attachment.copied = true;
		staged.push_back(AttachmentToJson(attachment));
	}

	cb->Success(nlohmann::json{{"attachments", staged}}.dump());
}

void UamQueryHandler::HandleCancelAcpTurn(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");

	std::string error;
	if (!uam::CancelAcpTurn(m_app, chat_id, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to cancel ACP turn."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleResolveAcpPermission(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string request_id = payload.value("requestId", "");
	const std::string option_id = payload.value("optionId", "");
	const bool cancelled = payload.value("cancelled", false) || option_id == uam::acp_permissions::kCancelledOptionId;

	std::string error;
	if (!uam::ResolveAcpPermission(m_app, chat_id, request_id, option_id, cancelled, &error))
	{
		cb->Failure(409, FailureDetailOrFallback(error, "Failed to resolve ACP permission request."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleResolveAcpUserInput(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string request_id = payload.value("requestId", "");
	std::map<std::string, std::vector<std::string>> answers;

	const nlohmann::json* raw_answers = uam::nlohmann_json::FindObjectField(payload, "answers");
	if (raw_answers == nullptr)
	{
		cb->Failure(400, "ACP user input answers must be an object.");
		return;
	}

	for (auto it = raw_answers->begin(); it != raw_answers->end(); ++it)
	{
		if (it.key().empty())
		{
			continue;
		}

		answers[it.key()] = uam::nlohmann_json::StringListValue(it.value());
	}

	std::string error;
	if (!uam::ResolveAcpUserInput(m_app, chat_id, request_id, answers, &error))
	{
		cb->Failure(409, FailureDetailOrFallback(error, "Failed to resolve ACP user input request."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleStopAcpSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	uam::StopAcpSession(m_app, chat_id);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleWriteClipboardText(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string text = payload.value("text", "");
	if (text.empty())
	{
		cb->Failure(400, "Clipboard text is empty.");
		return;
	}
	if (text.size() > kMaxClipboardTextBytes)
	{
		cb->Failure(413, "Clipboard text is too large.");
		return;
	}

	std::string error;
	if (!WriteNativeClipboardText(text, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to write clipboard text."));
		return;
	}

	cb->Success(R"({"copied":true})");
}

void UamQueryHandler::HandleSetTheme(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string theme = payload.value("theme", "dark");
	const std::string previous_theme = m_app.settings.ui_theme;
	m_app.settings.ui_theme = theme;
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings.ui_theme = previous_theme;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist theme."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	if (chat_id.empty())
	{
		cb->Failure(400, "Missing chat id.");
		return;
	}

	const std::string objective = payload.value("objective", "");
	if (objective.empty())
	{
		cb->Failure(400, "Goal requires an objective.");
		return;
	}

	const int64_t token_budget = payload.value("tokenBudget", static_cast<int64_t>(0));

	std::string goal_id;
	if (!uam::GoalService::CreateGoal(m_app, chat_id, objective, token_budget, &goal_id))
	{
		cb->Failure(404, "Chat not found.");
		return;
	}

	// Auto-activate the new goal
	uam::GoalService::SetActiveGoal(m_app, chat_id, goal_id);
	if (ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id); chat != nullptr)
	{
		(void)ChatRepository::SaveChat(m_app.data_root, *chat);
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(R"({"goalId":")" + goal_id + R"("})");
}

void UamQueryHandler::HandleUpdateGoalStatus(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string goal_id = payload.value("goalId", "");
	if (goal_id.empty())
	{
		cb->Failure(400, "Missing goal id.");
		return;
	}

	const std::string status_str = payload.value("status", "");
	GoalStatus status = GoalStatus::Active;
	if (status_str == "complete")
	{
		status = GoalStatus::Complete;
	}
	else if (status_str == "blocked")
	{
		status = GoalStatus::Blocked;
	}
	else if (status_str == "paused")
	{
		status = GoalStatus::Paused;
	}
	else if (status_str == "active")
	{
		status = GoalStatus::Active;
	}
	else
	{
		cb->Failure(400, "Invalid goal status: " + status_str);
		return;
	}

	if (!uam::GoalService::UpdateGoalStatus(m_app, goal_id, status))
	{
		cb->Failure(404, "Goal not found.");
		return;
	}

	// Find parent chat for push update
	std::string parent_chat_id;
	for (const auto& chat : m_app.chats)
	{
		for (const auto& goal : chat.goals)
		{
			if (goal.id == goal_id)
			{
				parent_chat_id = chat.id;
				break;
			}
		}
		if (!parent_chat_id.empty())
			break;
	}

	if (!parent_chat_id.empty())
	{
		if (ChatSession* chat = ChatDomainService().FindChatById(m_app, parent_chat_id); chat != nullptr)
		{
			(void)ChatRepository::SaveChat(m_app.data_root, *chat);
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleSetActiveGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string goal_id = payload.value("goalId", "");

	if (chat_id.empty())
	{
		cb->Failure(400, "Missing chatId.");
		return;
	}

	const bool updated = goal_id.empty() ? uam::GoalService::ClearActiveGoal(m_app, chat_id) : uam::GoalService::SetActiveGoal(m_app, chat_id, goal_id);
	if (!updated)
	{
		cb->Failure(404, "Failed to set active goal. Goal may not exist or is not in this chat.");
		return;
	}

	if (ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id); chat != nullptr)
	{
		(void)ChatRepository::SaveChat(m_app.data_root, *chat);
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleRemoveGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string goal_id = payload.value("goalId", "");
	if (goal_id.empty())
	{
		cb->Failure(400, "Missing goal id.");
		return;
	}

	// Find parent chat before removing
	std::string parent_chat_id;
	for (const auto& chat : m_app.chats)
	{
		for (const auto& goal : chat.goals)
		{
			if (goal.id == goal_id)
			{
				parent_chat_id = chat.id;
				break;
			}
		}
		if (!parent_chat_id.empty())
			break;
	}

	if (!uam::GoalService::RemoveGoal(m_app, goal_id))
	{
		cb->Failure(404, "Goal not found.");
		return;
	}

	if (!parent_chat_id.empty())
	{
		if (ChatSession* chat = ChatDomainService().FindChatById(m_app, parent_chat_id); chat != nullptr)
		{
			(void)ChatRepository::SaveChat(m_app.data_root, *chat);
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
	}

	cb->Success("{}");
}
