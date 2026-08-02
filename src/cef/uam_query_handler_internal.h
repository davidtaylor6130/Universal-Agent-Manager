#pragma once

#include "app/chat_domain_service.h"
#include "common/models/app_models.h"
#include "common/config/approval_modes.h"
#include "common/config/provider_chat_defaults.h"
#include "common/memory/memory_levels.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/chat/chat_repository.h"
#include "common/chat/native_chat_identity.h"
#include "app/provider_resolution_service.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/utils/nlohmann_json_utils.h"

#include "common/config/editor_file_associations.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

namespace uam::query_handler_internal
{

// ---- Pure utility functions (no domain service dependencies) ----------

inline std::string FailureDetailOrFallback(const std::string& detail, std::string fallback)
{
	return detail.empty() ? std::move(fallback) : detail;
}

inline nlohmann::json WithOptionalRequestId(nlohmann::json value, const std::string& request_id)
{
	if (!request_id.empty())
	{
		value["requestId"] = request_id;
	}
	return value;
}

inline std::string ProviderSwitchChatIdFromPayload(const nlohmann::json& payload)
{
	return uam::nlohmann_json::TrimmedStringValue(payload, {"chatId"});
}

inline std::string ProviderSwitchProviderIdFromPayload(const nlohmann::json& payload)
{
	return uam::nlohmann_json::TrimmedStringValue(payload, {"providerId"});
}

inline std::string AcpPromptGoalIdFromPayload(const nlohmann::json& payload)
{
	return uam::nlohmann_json::TrimmedStringValue(payload, {"goalId"});
}

inline std::string ParseDeleteChatIds(const nlohmann::json& payload, std::vector<std::string>& chat_ids)
{
	chat_ids.clear();
	const auto chat_ids_it = payload.find("chatIds");
	if (chat_ids_it == payload.end() || !chat_ids_it->is_array())
	{
		return "chatIds must be an array.";
	}
	for (const nlohmann::json& value : *chat_ids_it)
	{
		if (!value.is_string())
		{
			chat_ids.clear();
			return "Every chat id must be a string.";
		}
		chat_ids.push_back(value.get<std::string>());
	}
	return chat_ids.empty() ? "chatIds must contain at least one chat id." : "";
}

using uam::provider_chat_defaults::IsAllowedModelId;

inline bool CommandSafetyTierNeedsLiveUpdate(const ChatSession& previous, const ChatSession& requested)
{
	if (uam::approval_modes::EffectiveProviderMode(previous.approval_mode, previous.command_safety_tier) !=
	    uam::approval_modes::EffectiveProviderMode(requested.approval_mode, requested.command_safety_tier))
	{
		return true;
	}
	ProviderProfile provider;
	provider.id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(requested.provider_id);
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(provider.id);
	return runtime.BuildStructuredLaunchArgv(provider, previous) != runtime.BuildStructuredLaunchArgv(provider, requested);
}

// ---- Provider chat defaults helpers ----------

inline ProviderChatDefaults DefaultsFromPayload(const nlohmann::json& value, const ProviderChatDefaults& fallback, std::string_view provider_id)
{
	ProviderChatDefaults defaults = fallback;
	if (!value.is_object())
	{
		return uam::provider_chat_defaults::Normalize(defaults, provider_id);
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
	if (const std::optional<bool> small_model_mode = uam::nlohmann_json::BoolFieldStrict(value, "smallModelMode"))
	{
		defaults.small_model_mode = *small_model_mode;
	}
	defaults.memory_level = uam::memory_levels::Normalize(uam::nlohmann_json::TrimmedStringValueOr(value, "memoryLevel", defaults.memory_level), defaults.memory_enabled);
	defaults.reasoning_effort = uam::nlohmann_json::TrimmedStringValueOr(value, "reasoningEffort", defaults.reasoning_effort);
	defaults.service_tier = uam::nlohmann_json::TrimmedStringValueOr(value, "serviceTier", defaults.service_tier);
	return uam::provider_chat_defaults::Normalize(defaults, provider_id);
}

inline ProviderChatDefaults DefaultsForProvider(const AppSettings& settings, const std::string& provider_id)
{
	return uam::provider_chat_defaults::ForProvider(settings, provider_id);
}

inline ProviderChatDefaults ProviderDefaultsFromSettingsPayload(const nlohmann::json& value, const ProviderChatDefaults& fallback, std::string_view provider_id)
{
	ProviderChatDefaults defaults = DefaultsFromPayload(value, fallback, provider_id);
	if (!uam::provider_ids::IsCliProviderAliasOf(provider_id, uam::provider_ids::kCodexCli))
	{
		defaults.service_tier.clear();
	}
	return defaults;
}

inline void ApplyProviderDefaultsToChat(const AppSettings& settings, ChatSession& chat, const nlohmann::json* payload_defaults = nullptr)
{
	ProviderChatDefaults defaults = DefaultsForProvider(settings, chat.provider_id);
	if (payload_defaults != nullptr)
	{
		defaults = DefaultsFromPayload(*payload_defaults, defaults, chat.provider_id);
	}
	uam::provider_chat_defaults::ApplyToChat(chat, std::move(defaults));
}

// ---- Chat domain helpers (use ChatDomainService) ----------

inline ChatSession* FindChatOrFail(uam::AppState& app, const std::string& chat_id, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb, const std::string& not_found_message, int status_code = 404)
{
	ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);

	if (chat == nullptr)
	{
		cb->Failure(status_code, not_found_message);
		return nullptr;
	}

	return chat;
}

inline ChatSession* FindPayloadChatOrFail(uam::AppState& app, const nlohmann::json& payload, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	return FindChatOrFail(app, chat_id, cb, "Chat not found.");
}

inline bool ChatIdExists(const uam::AppState& app, const std::string& chat_id)
{
	return ChatDomainService().FindChatById(app, chat_id) != nullptr;
}

// ---- Provider helpers ----------

constexpr const char* kPreferredProviderId = provider_build_config::FirstEnabledProviderId();

inline const ProviderProfile* ResolvePreferredCliProvider(const uam::AppState& app)
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

inline std::string DefaultNewChatProviderId(const uam::AppState& app, const ProviderProfile* preferred_provider)
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

inline std::string ResolveNewChatProviderId(const uam::AppState& app, const std::string& requested_provider_id, const ProviderProfile* preferred_provider)
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

inline std::string ResolveRequestedNewChatFolderId(const uam::AppState& app, const std::string& requested_folder_id)
{
	if (!requested_folder_id.empty())
	{
		const ChatFolder* folder = ChatDomainService().FindFolderById(app, requested_folder_id);
		if (folder != nullptr)
		{
			return folder->id;
		}
	}

	const ChatSession* selected_chat = ChatDomainService().SelectedChat(app);
	if (selected_chat == nullptr || selected_chat->folder_id.empty())
	{
		return std::string{};
	}

	const ChatFolder* folder = ChatDomainService().FindFolderById(app, selected_chat->folder_id);
	return folder ? folder->id : std::string{};
}

// ---- Session lifecycle helpers ----------

inline void RollbackCreatedChat(uam::AppState& app, const std::string& chat_id, const std::string& previous_selected_chat_id, bool delete_storage)
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

inline bool AcpSessionBlocksModelChange(const uam::AcpSessionState& session)
{
	return AcpSessionHasBlockingRuntimeWork(session);
}

inline std::string MakeCollisionSafeImportedChatId(const ChatSession& chat, const uam::AppState& app)
{
	const std::string chat_id = uam::strings::Trim(chat.id);
	const std::string native_session_id = uam::strings::Trim(chat.native_session_id);
	const std::string base_id = uam::strings::NonEmptyOrFallback(chat_id, native_session_id);
	const std::string suffix = uam::chat_identity::NativeIdentityKeyHash(uam::chat_identity::NativeIdentityKeyForHistoryImport(chat));
	std::string candidate = base_id + "--" + suffix;

	while (ChatDomainService().FindChatById(app, candidate) != nullptr)
	{
		candidate += "_";
	}

	return candidate;
}

inline bool ChatProviderAvailableOrFail(const uam::AppState& app, const ChatSession& chat, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb)
{
	if (ProviderResolutionService().ChatProviderIsAvailable(app, chat))
	{
		return true;
	}

	cb->Failure(409, ProviderResolutionService().ChatProviderUnavailableReason(app, chat));
	return false;
}

inline bool AutoApprovePendingAcpPermissionOrFail(uam::AppState& app, const std::string& chat_id, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb)
{
	std::string acp_error;
	if (uam::TryAutoApprovePendingAcpPermission(app, chat_id, &acp_error) || acp_error.empty())
	{
		return true;
	}

	cb->Failure(409, acp_error);
	return false;
}

inline bool ShouldSkipEditorScanDirectory(const std::filesystem::path& path)
{
	static constexpr std::array<std::string_view, 8> kIgnoredDirectoryNames = {
	    ".git", "node_modules", "Builds", "build", "dist", "out", ".venv", "venv",
	};
	const std::string name = path.filename().string();
	return uam::ranges::Contains(kIgnoredDirectoryNames, std::string_view(name)) || uam::strings::StartsWith(name, "cmake-build-");
}

inline std::string SelectEditorPresetForWorkspace(const AppSettings& settings, const std::filesystem::path& workspace_root)
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

} // namespace uam::query_handler_internal
