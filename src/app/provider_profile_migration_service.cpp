#include "provider_profile_migration_service.h"

#include "app/application_core_helpers.h"

#include "common/provider/provider_runtime.h"

#include <algorithm>
#include <unordered_set>

namespace
{
	constexpr const char* kRuntimeIdLegacy = "gemini";
	constexpr const char* kRuntimeIdStructured = "gemini-structured";
	constexpr const char* kRuntimeIdCli = "gemini-cli";
	constexpr const char* kRuntimeIdLegacyCliA = "codex";
	constexpr const char* kRuntimeIdLegacyCliB = "claude";
	constexpr const char* kRuntimeIdLegacyCliC = "opencode";
	constexpr const char* kRuntimeIdCliA = "codex-cli";
	constexpr const char* kRuntimeIdCliB = "claude-cli";
	constexpr const char* kRuntimeIdCliC = "opencode-cli";
	constexpr const char* kRuntimeIdCliLocalBridge = "opencode-local";
	constexpr const char* kRuntimeIdLocalEngine = "ollama-engine";

	constexpr const char* kHistoryAdapterNativeJson = "gemini-cli-json";
	constexpr const char* kPromptBootstrapAtPath = "gemini-at-path";
	constexpr const char* kPromptBootstrapPath = "@.gemini/gemini.md";
	constexpr const char* kLegacyRuntimeCommandTemplate = "gemini {resume} {flags} {prompt}";
	constexpr const char* kRuntimeCommandTemplate = "gemini {resume} {flags} -p {prompt}";

	bool IsLegacyBuiltInProviderId(const std::string& provider_id)
	{
		const std::string lowered = ToLowerAscii(Trim(provider_id));
		return lowered == kRuntimeIdLegacy || lowered == kRuntimeIdLegacyCliA || lowered == kRuntimeIdLegacyCliB || lowered == kRuntimeIdLegacyCliC;
	}

	bool SaveChatByMappedProvider(const uam::AppState& app, const ChatSession& chat)
	{
		const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, chat.provider_id);

		if (profile == nullptr)
		{
			return false;
		}

		return ProviderRuntime::SaveHistory(*profile, app.data_root, chat);
	}

	bool ChatHasCliRuntimeHints(const uam::AppState& app, const ChatSession& chat)
	{
		for (const auto& terminal : app.cli_terminals)
		{
			if (terminal == nullptr || terminal->attached_chat_id != chat.id)
			{
				continue;
			}

			if (terminal->running || terminal->should_launch || terminal->input_ready || terminal->generation_in_progress)
			{
				return true;
			}
		}

		return false;
	}

	std::string CompactIdPreview(const std::string& value, const std::size_t max_len)
	{
		if (value.size() <= max_len)
		{
			return value;
		}

		if (max_len <= 3)
		{
			return value.substr(0, max_len);
		}

		return value.substr(0, max_len - 3) + "...";
	}

} // namespace

bool ProviderProfileMigrationService::IsNativeHistoryProviderId(const std::string& provider_id) const
{
	const std::string lowered = ToLowerAscii(Trim(provider_id));
	return lowered == kRuntimeIdLegacy || lowered == kRuntimeIdCli || lowered == kRuntimeIdStructured;
}

std::string ProviderProfileMigrationService::MapLegacyRuntimeId(const std::string& provider_id, const bool prefer_cli_for_native_history) const
{
	const std::string trimmed = Trim(provider_id);
	const std::string lowered = ToLowerAscii(trimmed);

	if (lowered == kRuntimeIdLegacy)
	{
		return prefer_cli_for_native_history ? kRuntimeIdCli : kRuntimeIdStructured;
	}

	if (lowered == kRuntimeIdLegacyCliA)
	{
		return kRuntimeIdCliA;
	}

	if (lowered == kRuntimeIdLegacyCliB)
	{
		return kRuntimeIdCliB;
	}

	if (lowered == kRuntimeIdLegacyCliC)
	{
		return kRuntimeIdCliC;
	}

	return trimmed;
}

std::string ProviderProfileMigrationService::DefaultRuntimeIdForLegacyViewHint(const uam::AppState& app) const
{
	return (app.center_view_mode == CenterViewMode::CliConsole) ? kRuntimeIdCli : kRuntimeIdStructured;
}

bool ProviderProfileMigrationService::ShouldShowProviderProfileInUi(const ProviderProfile& profile) const
{
	return !IsLegacyBuiltInProviderId(profile.id);
}

bool ProviderProfileMigrationService::MigrateProviderProfilesToFixedModeIds(uam::AppState& app) const
{
	bool changed = false;
	std::vector<ProviderProfile> migrated;
	migrated.reserve(app.provider_profiles.size());
	std::unordered_set<std::string> seen_ids;

	for (ProviderProfile profile : app.provider_profiles)
	{
		const std::string original_id = Trim(profile.id);
		const std::string mapped_id = MapLegacyRuntimeId(original_id, false);

		if (mapped_id != original_id)
		{
			changed = true;
		}

		if (mapped_id.empty())
		{
			changed = true;
			continue;
		}

		profile.id = mapped_id;
		const auto assign_if_changed = [&](auto& field, const auto& value)
		{
			if (field != value)
			{
				field = value;
				changed = true;
			}
		};

		if (mapped_id == kRuntimeIdStructured)
		{
			assign_if_changed(profile.output_mode, std::string("structured"));
			assign_if_changed(profile.supports_interactive, false);

			if (Trim(profile.command_template).empty() || profile.command_template == kLegacyRuntimeCommandTemplate)
			{
				assign_if_changed(profile.command_template, std::string(kRuntimeCommandTemplate));
			}

			if (Trim(profile.history_adapter).empty())
			{
				assign_if_changed(profile.history_adapter, std::string(kHistoryAdapterNativeJson));
			}

			if (Trim(profile.prompt_bootstrap).empty())
			{
				assign_if_changed(profile.prompt_bootstrap, std::string(kPromptBootstrapAtPath));
			}

			if (Trim(profile.prompt_bootstrap_path).empty())
			{
				assign_if_changed(profile.prompt_bootstrap_path, std::string(kPromptBootstrapPath));
			}
		}
		else if (mapped_id == kRuntimeIdCli)
		{
			assign_if_changed(profile.output_mode, std::string("cli"));
			assign_if_changed(profile.supports_interactive, true);

			if (Trim(profile.command_template).empty())
			{
				assign_if_changed(profile.command_template, std::string(kLegacyRuntimeCommandTemplate));
			}

			if (Trim(profile.history_adapter).empty())
			{
				assign_if_changed(profile.history_adapter, std::string(kHistoryAdapterNativeJson));
			}

			if (Trim(profile.prompt_bootstrap).empty())
			{
				assign_if_changed(profile.prompt_bootstrap, std::string(kPromptBootstrapAtPath));
			}

			if (Trim(profile.prompt_bootstrap_path).empty())
			{
				assign_if_changed(profile.prompt_bootstrap_path, std::string(kPromptBootstrapPath));
			}
		}
		else if (mapped_id == kRuntimeIdCliA || mapped_id == kRuntimeIdCliB || mapped_id == kRuntimeIdCliC || mapped_id == kRuntimeIdCliLocalBridge)
		{
			assign_if_changed(profile.output_mode, std::string("cli"));
			assign_if_changed(profile.supports_interactive, true);
		}
		else if (mapped_id == kRuntimeIdLocalEngine)
		{
			assign_if_changed(profile.output_mode, std::string("structured"));
			assign_if_changed(profile.supports_interactive, false);
		}

		const std::string dedupe_key = ToLowerAscii(profile.id);

		if (!seen_ids.insert(dedupe_key).second)
		{
			changed = true;
			continue;
		}

		migrated.push_back(std::move(profile));
	}

	if (migrated.size() != app.provider_profiles.size())
	{
		changed = true;
	}

	app.provider_profiles = std::move(migrated);
	ProviderProfileStore::EnsureDefaultProfile(app.provider_profiles);

	std::vector<ProviderProfile> filtered;
	filtered.reserve(app.provider_profiles.size());

	for (const ProviderProfile& profile : app.provider_profiles)
	{
		if (IsLegacyBuiltInProviderId(profile.id))
		{
			changed = true;
			continue;
		}

		filtered.push_back(profile);
	}

	app.provider_profiles = std::move(filtered);
	ProviderProfileStore::EnsureDefaultProfile(app.provider_profiles);
	return changed;
}

bool ProviderProfileMigrationService::MigrateActiveProviderIdToFixedModes(uam::AppState& app) const
{
	bool changed = false;
	const std::string mapped_id = MapLegacyRuntimeId(app.settings.active_provider_id, app.center_view_mode == CenterViewMode::CliConsole);

	if (mapped_id != app.settings.active_provider_id)
	{
		app.settings.active_provider_id = mapped_id;
		changed = true;
	}

	if (Trim(app.settings.active_provider_id).empty())
	{
		app.settings.active_provider_id = DefaultRuntimeIdForLegacyViewHint(app);
		changed = true;
	}

	if (ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id) == nullptr)
	{
		app.settings.active_provider_id = DefaultRuntimeIdForLegacyViewHint(app);

		if (ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id) == nullptr && !app.provider_profiles.empty())
		{
			app.settings.active_provider_id = app.provider_profiles.front().id;
		}

		changed = true;
	}

	return changed;
}

bool ProviderProfileMigrationService::MigrateChatProviderBindingsToFixedModes(uam::AppState& app) const
{
	bool changed = false;
	const std::string fallback_provider_id = Trim(app.settings.active_provider_id).empty() ? DefaultRuntimeIdForLegacyViewHint(app) : app.settings.active_provider_id;

	for (ChatSession& chat : app.chats)
	{
		const std::string original_provider_id = chat.provider_id;
		const bool prefer_cli_for_legacy_runtime = ChatHasCliRuntimeHints(app, chat) || app.center_view_mode == CenterViewMode::CliConsole;
		std::string mapped_provider_id = MapLegacyRuntimeId(original_provider_id, prefer_cli_for_legacy_runtime);

		if (mapped_provider_id.empty())
		{
			mapped_provider_id = fallback_provider_id;
		}

		if (ProviderProfileStore::FindById(app.provider_profiles, mapped_provider_id) == nullptr)
		{
			mapped_provider_id = fallback_provider_id;
		}

		if (mapped_provider_id.empty())
		{
			continue;
		}

		if (mapped_provider_id != original_provider_id)
		{
			chat.provider_id = mapped_provider_id;

			if (chat.updated_at.empty())
			{
				chat.updated_at = TimestampNow();
			}

			if (!SaveChatByMappedProvider(app, chat))
			{
				app.status_line = "Failed to persist migrated provider id for chat " + CompactIdPreview(chat.id, 24) + ".";
			}

			changed = true;
		}
	}

	return changed;
}

