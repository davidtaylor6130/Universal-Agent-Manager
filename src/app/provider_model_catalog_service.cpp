#include "provider_model_catalog_service.h"

#include "common/platform/platform_services.h"
#include "common/provider/codex/cli/codex_session_index.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/acp/acp_model_json.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/state/app_state.h"
#include "common/utils/env_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
	nlohmann::json ReadJsonFile(const fs::path& path)
	{
		const std::string text = uam::io::ReadTextFile(path);
		if (text.empty())
		{
			return nullptr;
		}

		try
		{
			return nlohmann::json::parse(text);
		}
		catch (const nlohmann::json::exception&)
		{
			return nullptr;
		}
	}

	void PushModelIfNew(nlohmann::json& models_json, std::vector<std::string>& seen_model_ids, const std::string& id, const std::string& name, const std::string& description)
	{
		if (!uam::ranges::PushUniqueNonEmptyString(seen_model_ids, id))
		{
			return;
		}

		models_json.push_back({
		    {"id", id},
		    {"name", uam::strings::NonEmptyOrFallback(name, id)},
		    {"description", description},
		});
	}

	std::string TitleFromModelId(std::string model_id)
	{
		if (const std::size_t slash = model_id.rfind('/'); slash != std::string::npos)
		{
			model_id = model_id.substr(slash + 1);
		}

		std::string title;
		bool uppercase_next = true;
		for (const char ch : model_id)
		{
			if (ch == '-' || ch == '_' || ch == '.' || ch == ':')
			{
				if (!title.empty() && title.back() != ' ')
				{
					title.push_back(' ');
				}
				uppercase_next = true;
				continue;
			}

			title.push_back(uppercase_next ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch))) : ch);
			uppercase_next = false;
		}

		title = uam::strings::Trim(uam::strings::NonEmptyOrFallback(title, model_id));
		if (title.starts_with("Deepseek "))
		{
			title.replace(0, 8, "DeepSeek");
		}
		if (title.starts_with("Minimax "))
		{
			title.replace(0, 7, "MiniMax");
		}
		if (title.starts_with("Mimo "))
		{
			title.replace(0, 4, "MiMo");
		}
		return title;
	}

	bool IsOpenCodeZenFreeModelId(const std::string& id)
	{
		constexpr std::string_view free_suffix = "-free";
		return id == "big-pickle" || (id.size() >= free_suffix.size() && id.compare(id.size() - free_suffix.size(), free_suffix.size(), free_suffix) == 0);
	}

	std::string StableCatalogFingerprint(std::string_view value)
	{
		std::uint64_t hash = 1469598103934665603ULL;
		for (const unsigned char ch : value)
		{
			hash ^= ch;
			hash *= 1099511628211ULL;
		}
		std::ostringstream out;
		out << std::hex << hash;
		return out.str();
	}

	std::string NonSecretCatalogEnvironmentContext()
	{
		constexpr std::array<const char*, 17> names{
		    "CODEX_HOME", "CLAUDE_CONFIG_DIR", "GEMINI_CLI_HOME", "GEMINI_HOME", "XDG_CONFIG_HOME", "OPENCODE_CONFIG", "COPILOT_HOME",
		    "OPENAI_BASE_URL", "OPENAI_API_BASE", "ANTHROPIC_BASE_URL", "GOOGLE_GEMINI_BASE_URL", "GEMINI_API_BASE_URL",
		    "GH_HOST", "OPENAI_ORGANIZATION", "OPENAI_ORG_ID", "OPENAI_PROJECT", "GITHUB_USER",
		};
		std::string context;
		for (const char* name : names)
		{
			if (const auto value = uam::env::GetTrimmedString(name)) context += "\n" + std::string(name) + "=" + *value;
		}
		return context;
	}
}

namespace uam
{

ProviderModelCatalogService::ProviderModelCatalogService()
	: m_open_code_zen_free_models(nlohmann::json::array())
	, m_configured_open_code_models(nlohmann::json::array())
	, m_cached_codex_models(nlohmann::json::array())
	, m_refresh_task(std::make_unique<AsyncCommandTask>())
{
}

void ProviderModelCatalogService::Initialize(const fs::path& data_root, const std::vector<ProviderProfile>& profiles, std::string_view configuration_context)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_data_root = data_root;
	m_catalog_key_by_provider_id.clear();
	m_pending_discovery_provider_ids.clear();
	m_refresh_attempted_provider_ids.clear();
	for (const ProviderProfile& profile : profiles)
	{
		const std::string provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(profile.id);
		std::string fingerprint_source = provider_id + "\n" + profile.execution_mode + "\n" + profile.output_mode + "\n" + profile.interactive_command + "\n" + profile.structured_protocol + "\n" + profile.native_goal_command + "\n" + std::string(configuration_context) + NonSecretCatalogEnvironmentContext();
		for (const std::string& flag : profile.runtime_flags)
		{
			fingerprint_source += "\n" + flag;
		}
		m_catalog_key_by_provider_id[provider_id] = provider_id + "-" + StableCatalogFingerprint(fingerprint_source);
	}
	LoadPersistentCatalogs();

	// Pre-load cached models at startup (no network).
	m_cached_codex_models = ReadCachedCodexModels();

	// Load cached zen free models.
	m_open_code_zen_free_models = ReadOpenCodeZenFreeModelsCache();
	if (m_open_code_zen_free_models.empty())
	{
		m_open_code_zen_free_models = BuiltInOpenCodeZenFreeModels();
	}

	// Read opencode config once at startup.
	m_configured_open_code_models = ReadConfiguredOpenCodeModels();
	m_configured_open_code_default_model = ReadConfiguredOpenCodeDefaultModel();

	// Seed the cached config mtime. The file commonly does not exist (no OpenCode install),
	// so guard last_write_time, which throws for a missing path.
	const fs::path config_path = OpenCodeConfigPath();
	std::error_code mtime_error;
	m_open_code_config_mtime = fs::last_write_time(config_path, mtime_error);
	if (mtime_error)
	{
		m_open_code_config_mtime = fs::file_time_type{};
	}
}

bool ProviderModelCatalogService::MaybeStartRefresh()
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_refresh_task->running)
	{
		return false;
	}

	if (OpenCodeZenRefreshDisabled() && !HasOpenCodeZenModelsFixture())
	{
		return false;
	}

	const auto now = std::chrono::steady_clock::now();
	const bool has_recent_attempt = m_last_refresh_attempt.time_since_epoch().count() != 0 && now - m_last_refresh_attempt < kOpenCodeZenRefreshInterval;
	if (!HasOpenCodeZenModelsFixture() && has_recent_attempt)
	{
		return false;
	}

	m_last_refresh_attempt = now;
	StartRefreshTask();
	return true;
}

bool ProviderModelCatalogService::Poll()
{
	// Check for async refresh completion.
	bool updated = false;

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_refresh_task->running && TryConsumeRefreshOutput())
		{
			updated = true;
		}

		// Check opencode config mtime for cache invalidation.
		const fs::path config_path = OpenCodeConfigPath();
		if (fs::exists(config_path))
		{
			try
			{
				const auto current_mtime = fs::last_write_time(config_path);
				if (current_mtime != m_open_code_config_mtime)
				{
					m_open_code_config_mtime = current_mtime;
					m_configured_open_code_models = ReadConfiguredOpenCodeModels();
					m_configured_open_code_default_model = ReadConfiguredOpenCodeDefaultModel();
				}
			}
			catch (const fs::filesystem_error&)
			{
				// Ignore filesystem errors during mtime check.
			}
		}
	}

	return updated;
}

nlohmann::json ProviderModelCatalogService::GetOpenCodeZenFreeModels() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_open_code_zen_free_models;
}

nlohmann::json ProviderModelCatalogService::GetConfiguredOpenCodeModels() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_configured_open_code_models;
}

std::string ProviderModelCatalogService::GetConfiguredOpenCodeDefaultModel() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_configured_open_code_default_model;
}

nlohmann::json ProviderModelCatalogService::GetCachedCodexModels() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_cached_codex_models;
}

std::string ProviderModelCatalogService::CatalogKey(const std::string& provider_id) const
{
	const std::string normalized = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
	const auto configured = m_catalog_key_by_provider_id.find(normalized);
	return configured == m_catalog_key_by_provider_id.end() ? normalized : configured->second;
}

void ProviderModelCatalogService::LoadPersistentCatalogs()
{
	m_persistent_catalogs = nlohmann::json::object();
	const nlohmann::json root = ReadJsonFile(m_data_root / kProviderModelsCacheFile);
	if (root.is_object())
	{
		const nlohmann::json* catalogs = uam::nlohmann_json::FindObjectField(root, "catalogs");
		if (catalogs != nullptr)
		{
			m_persistent_catalogs = *catalogs;
		}
	}
}

bool ProviderModelCatalogService::WritePersistentCatalogs() const
{
	if (m_data_root.empty())
	{
		return false;
	}
	return uam::io::WriteTextFile(m_data_root / kProviderModelsCacheFile, nlohmann::json{{"version", 1}, {"catalogs", m_persistent_catalogs}}.dump(2) + "\n");
}

bool ProviderModelCatalogService::RememberSuccessfulModels(const std::string& provider_id, const nlohmann::json& models)
{
	if (!models.is_array() || models.empty())
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string normalized_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
	m_pending_discovery_provider_ids.erase(normalized_provider_id);
	const std::string key = CatalogKey(normalized_provider_id);
	const nlohmann::json previous = m_persistent_catalogs.contains(key) ? m_persistent_catalogs[key] : nlohmann::json{};
	m_persistent_catalogs[key] = {
	    {"providerId", normalized_provider_id},
	    {"models", models},
	    {"updatedAt", uam::time::TimestampNow()},
	    {"updatedAtSec", uam::time::TimestampNowSec()},
	};
	if (!WritePersistentCatalogs())
	{
		if (previous.is_null() || previous.empty())
		{
			m_persistent_catalogs.erase(key);
		}
		else
		{
			m_persistent_catalogs[key] = previous;
		}
		m_refresh_error_by_provider_id[normalized_provider_id] = "Failed to persist provider model cache.";
		return false;
	}
	m_refresh_error_by_provider_id.erase(normalized_provider_id);
	return true;
}

void ProviderModelCatalogService::RememberRefreshFailure(const std::string& provider_id, std::string error)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string normalized = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
	m_pending_discovery_provider_ids.erase(normalized);
	m_refresh_error_by_provider_id[normalized] = uam::strings::Trim(std::move(error));
}

nlohmann::json ProviderModelCatalogService::GetCachedProviderModels(const std::string& provider_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string key = CatalogKey(provider_id);
	const auto entry = m_persistent_catalogs.find(key);
	if (entry == m_persistent_catalogs.end() || !entry->is_object())
	{
		return nlohmann::json::array();
	}
	const nlohmann::json* models = uam::nlohmann_json::FindArrayField(*entry, "models");
	return models == nullptr ? nlohmann::json::array() : *models;
}

std::string ProviderModelCatalogService::GetProviderRefreshError(const std::string& provider_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto error = m_refresh_error_by_provider_id.find(uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id));
	return error == m_refresh_error_by_provider_id.end() ? std::string{} : error->second;
}

bool ProviderModelCatalogService::BeginDiscoveryIfStale(const std::string& provider_id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string normalized = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
	const auto entry = m_persistent_catalogs.find(CatalogKey(normalized));
	const nlohmann::json* models = entry == m_persistent_catalogs.end() ? nullptr : uam::nlohmann_json::FindArrayField(entry.value(), "models");
	const std::int64_t updated_at = entry == m_persistent_catalogs.end() ? 0 : entry->value("updatedAtSec", static_cast<std::int64_t>(0));
	if (models != nullptr && !models->empty() && updated_at > 0 && uam::time::TimestampNowSec() - updated_at < kProviderModelCacheFreshnessSeconds)
	{
		return false;
	}
	if (m_pending_discovery_provider_ids.contains(normalized) || m_refresh_attempted_provider_ids.contains(normalized)) return false;
	m_pending_discovery_provider_ids.insert(normalized);
	m_refresh_attempted_provider_ids.insert(normalized);
	m_refresh_error_by_provider_id.erase(normalized);
	return true;
}

bool ProviderModelCatalogService::BeginDiscovery(const std::string& provider_id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string normalized = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
	if (m_pending_discovery_provider_ids.contains(normalized)) return false;
	m_pending_discovery_provider_ids.insert(normalized);
	m_refresh_attempted_provider_ids.insert(normalized);
	m_refresh_error_by_provider_id.erase(normalized);
	return true;
}

bool ProviderModelCatalogService::BeginDiscoveryIfMissing(const std::string& provider_id)
{
	return BeginDiscoveryIfStale(provider_id);
}

bool ProviderModelCatalogService::IsDiscoveryPending(const std::string& provider_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_pending_discovery_provider_ids.contains(uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id));
}

nlohmann::json ProviderModelCatalogService::MergeAcpModelArrays(nlohmann::json fallback_models, nlohmann::json runtime_models)
{
	if (!fallback_models.is_array())
	{
		fallback_models = nlohmann::json::array();
	}
	if (!runtime_models.is_array())
	{
		return fallback_models;
	}

	std::vector<std::string> seen_model_ids;
	for (const nlohmann::json& model : fallback_models)
	{
		if (model.is_object())
		{
			uam::ranges::PushUniqueNonEmptyString(seen_model_ids, uam::nlohmann_json::TrimmedStringValue(model, {"id"}));
		}
	}
	for (const nlohmann::json& model : runtime_models)
	{
		if (!model.is_object())
		{
			continue;
		}
		const std::string id = uam::nlohmann_json::TrimmedStringValue(model, {"id"});
		if (!uam::ranges::PushUniqueNonEmptyString(seen_model_ids, id))
		{
			continue;
		}
		nlohmann::json normalized_model = model;
		normalized_model["id"] = id;
		fallback_models.push_back(std::move(normalized_model));
	}
	return fallback_models;
}

nlohmann::json ProviderModelCatalogService::FallbackAcpModelsForChat(const std::string& provider_id) const
{
	nlohmann::json cached = GetCachedProviderModels(provider_id);
	if (uam::provider_ids::IsCliProviderAliasOf(provider_id, uam::provider_ids::kCodexCli))
	{
		return MergeAcpModelArrays(std::move(cached), GetCachedCodexModels());
	}
	if (uam::provider_ids::IsCliProviderAliasOf(provider_id, uam::provider_ids::kOpenCodeCli))
	{
		return MergeAcpModelArrays(std::move(cached), MergeAcpModelArrays(GetConfiguredOpenCodeModels(), GetOpenCodeZenFreeModels()));
	}
	return cached;
}

std::string ProviderModelCatalogService::FallbackAcpCurrentModelForChat(const std::string& provider_id, const std::string& chat_model_id) const
{
	if (!uam::strings::IsBlank(chat_model_id))
	{
		return chat_model_id;
	}
	return uam::provider_ids::IsCliProviderAliasOf(provider_id, uam::provider_ids::kOpenCodeCli) ? GetConfiguredOpenCodeDefaultModel() : std::string{};
}

bool ProviderModelCatalogService::OpenCodeZenRefreshDisabled()
{
	const std::string value = uam::strings::TrimAndLowerAscii(uam::env::GetTrimmedString(kOpenCodeZenRefreshDisabledEnv).value_or(""));
	return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool ProviderModelCatalogService::HasOpenCodeZenModelsFixture()
{
	return uam::env::GetTrimmedPath(kOpenCodeZenModelsFixtureEnv).has_value();
}

fs::path ProviderModelCatalogService::OpenCodeZenFreeModelsCachePath() const
{
	return m_data_root / kOpenCodeZenFreeModelsCacheFile;
}

fs::path ProviderModelCatalogService::OpenCodeConfigPath() const
{
	if (const std::optional<fs::path> config_home = uam::env::GetTrimmedPath("XDG_CONFIG_HOME"))
	{
		return *config_home / "opencode" / "opencode.json";
	}

#if defined(_WIN32)
	if (const std::optional<fs::path> app_data = uam::env::GetTrimmedPath("APPDATA"))
	{
		return *app_data / "opencode" / "opencode.json";
	}
#endif

	if (const std::optional<fs::path> home = uam::env::GetTrimmedPath("HOME"))
	{
		return *home / ".config" / "opencode" / "opencode.json";
	}

	return uam::paths::CurrentPathOrDot() / ".config" / "opencode" / "opencode.json";
}

nlohmann::json ProviderModelCatalogService::BuiltInOpenCodeZenFreeModels() const
{
	auto models_json = nlohmann::json::array();
	std::vector<std::string> seen_model_ids;
	PushModelIfNew(models_json, seen_model_ids, "opencode/big-pickle", "Big Pickle", "OpenCode Zen limited-time stealth free model.");
	PushModelIfNew(models_json, seen_model_ids, "opencode/deepseek-v4-flash-free", "DeepSeek V4 Flash Free", "OpenCode Zen free model.");
	PushModelIfNew(models_json, seen_model_ids, "opencode/mimo-v2.5-free", "MiMo V2.5 Free", "OpenCode Zen free model.");
	PushModelIfNew(models_json, seen_model_ids, "opencode/qwen3.6-plus-free", "Qwen3.6 Plus Free", "OpenCode Zen free model.");
	PushModelIfNew(models_json, seen_model_ids, "opencode/minimax-m3-free", "MiniMax M3 Free", "OpenCode Zen free model.");
	PushModelIfNew(models_json, seen_model_ids, "opencode/nemotron-3-super-free", "Nemotron 3 Super Free", "OpenCode Zen free model.");
	return models_json;
}

nlohmann::json ProviderModelCatalogService::ReadOpenCodeZenFreeModelsCache() const
{
	const nlohmann::json cache = ReadJsonFile(OpenCodeZenFreeModelsCachePath());
	return cache.is_array() ? cache : nlohmann::json::array();
}

void ProviderModelCatalogService::WriteOpenCodeZenFreeModelsCache(const nlohmann::json& models) const
{
	if (m_data_root.empty())
	{
		return;
	}

	(void)uam::io::WriteTextFile(OpenCodeZenFreeModelsCachePath(), models.dump(2));
}

nlohmann::json ProviderModelCatalogService::ParseOpenCodeZenFreeModels(const nlohmann::json& root)
{
	auto models_json = nlohmann::json::array();
	const nlohmann::json* models = uam::nlohmann_json::FindArrayField(root, "data");
	if (models == nullptr)
	{
		return models_json;
	}

	std::vector<std::string> seen_model_ids;
	for (const nlohmann::json& model : *models)
	{
		if (!model.is_object())
		{
			continue;
		}

		const std::string owner = uam::nlohmann_json::TrimmedStringValue(model, {"owned_by", "ownedBy"});
		const std::string model_id = uam::nlohmann_json::TrimmedStringValue(model, {"id"});
		if (owner != "opencode" || model_id.empty() || !IsOpenCodeZenFreeModelId(model_id))
		{
			continue;
		}

		const std::string full_id = "opencode/" + model_id;
		const std::string description = model_id == "big-pickle" ? "OpenCode Zen limited-time stealth free model." : "OpenCode Zen free model.";
		PushModelIfNew(models_json, seen_model_ids, full_id, TitleFromModelId(model_id), description);
	}

	return models_json;
}

std::optional<nlohmann::json> ProviderModelCatalogService::FetchOpenCodeZenModels()
{
	if (const std::optional<nlohmann::json> fixture_path = uam::env::GetTrimmedPath(kOpenCodeZenModelsFixtureEnv))
	{
		const nlohmann::json root = ReadJsonFile(*fixture_path);
		return root.is_object() ? std::optional<nlohmann::json>(root) : std::nullopt;
	}

	const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(std::string("curl -s --max-time 4 ") + kOpenCodeZenModelsUrl, 6000);
	if (!result.ok || result.timed_out || result.canceled || result.exit_code != 0 || result.output.empty())
	{
		return std::nullopt;
	}

	const nlohmann::json root = nlohmann::json::parse(result.output, nullptr, false);
	return root.is_object() ? std::optional<nlohmann::json>(root) : std::nullopt;
}

nlohmann::json ProviderModelCatalogService::ReadConfiguredOpenCodeModels()
{
	auto models_json = nlohmann::json::array();
	const nlohmann::json config = ReadJsonFile(OpenCodeConfigPath());
	if (!config.is_object())
	{
		return models_json;
	}

	const nlohmann::json* providers = uam::nlohmann_json::FindObjectField(config, "provider");
	if (providers == nullptr)
	{
		return models_json;
	}

	std::vector<std::string> seen_model_ids;
	for (const auto& provider_entry : providers->items())
	{
		const std::string provider_id = uam::strings::Trim(provider_entry.key());
		if (provider_id.empty() || !provider_entry.value().is_object())
		{
			continue;
		}

		const nlohmann::json* models = uam::nlohmann_json::FindObjectField(provider_entry.value(), "models");
		if (models == nullptr)
		{
			continue;
		}

		for (const auto& model_entry : models->items())
		{
			const std::string model_id = uam::strings::Trim(model_entry.key());
			if (model_id.empty())
			{
				continue;
			}
			const std::string full_id = provider_id + "/" + model_id;
			std::string name;
			std::string description;
			if (model_entry.value().is_object())
			{
				name = uam::nlohmann_json::TrimmedStringValue(model_entry.value(), {"name", "displayName", "display_name"});
				description = uam::nlohmann_json::TrimmedStringValue(model_entry.value(), {"description"});
			}
			PushModelIfNew(models_json, seen_model_ids, full_id, uam::strings::NonEmptyOrFallback(name, model_id), description);
		}
	}

	return models_json;
}

std::string ProviderModelCatalogService::ReadConfiguredOpenCodeDefaultModel()
{
	const nlohmann::json config = ReadJsonFile(OpenCodeConfigPath());
	if (!config.is_object())
	{
		return "";
	}

	return uam::nlohmann_json::TrimmedStringValue(config, {"model"});
}

nlohmann::json ProviderModelCatalogService::ReadCachedCodexModels()
{
	auto models_json = nlohmann::json::array();
	const nlohmann::json cache = ReadJsonFile(uam::codex::CodexHomePath() / "models_cache.json");
	if (!cache.is_object())
	{
		return models_json;
	}

	const nlohmann::json* models = uam::nlohmann_json::FindArrayField(cache, "models");
	if (models == nullptr)
	{
		return models_json;
	}

	std::vector<std::string> seen_model_ids;
	uam::acp_models::CodexModelParseOptions parse_options;
	parse_options.skip_hidden_field = false;
	parse_options.allow_default_non_list_visibility = false;
	for (const nlohmann::json& model : *models)
	{
		const auto parsed = uam::acp_models::ParseCodexModelEntry(model, parse_options);
		if (!parsed || !uam::ranges::PushUniqueNonEmptyString(seen_model_ids, parsed->model.id))
		{
			continue;
		}

		models_json.push_back({
		    {"id", parsed->model.id},
		    {"name", parsed->model.name},
		    {"description", parsed->model.description},
		    {"defaultReasoningEffort", parsed->model.default_reasoning_effort},
		    {"supportedReasoningEfforts", parsed->model.supported_reasoning_efforts},
		    {"additionalSpeedTiers", parsed->model.additional_speed_tiers},
		});
	}

	return models_json;
}

void ProviderModelCatalogService::StartRefreshTask()
{
	uam::ResetAsyncCommandTask(*m_refresh_task);
	m_refresh_task->running = true;
	m_refresh_task->state = std::make_shared<AsyncProcessTaskState>();
	std::shared_ptr<AsyncProcessTaskState> state = m_refresh_task->state;
	m_refresh_task->worker = std::make_unique<std::jthread>(
	    [state, this](std::stop_token stop_token)
	    {
		    const std::optional<nlohmann::json> result = [this]() -> std::optional<nlohmann::json>
		    {
			    return FetchOpenCodeZenModels();
		    }();

		    if (!result.has_value())
		    {
			    state->completed.store(true, std::memory_order_release);
			    return;
		    }

		    const nlohmann::json models = ParseOpenCodeZenFreeModels(*result);
		    if (models.is_array() && !models.empty())
		    {
			    WriteOpenCodeZenFreeModelsCache(models);

			    std::lock_guard<std::mutex> lock(m_mutex);
			    m_open_code_zen_free_models = models;
		    }

		    state->completed.store(true, std::memory_order_release);
	    });
}

bool ProviderModelCatalogService::TryConsumeRefreshOutput()
{
	if (!m_refresh_task->running)
	{
		return false;
	}

	if (m_refresh_task->state == nullptr)
	{
		uam::ResetAsyncCommandTask(*m_refresh_task);
		return true;
	}

	if (!m_refresh_task->state->completed.load(std::memory_order_acquire))
	{
		return false;
	}

	uam::ResetAsyncCommandTask(*m_refresh_task);
	return true;
}

} // namespace uam
