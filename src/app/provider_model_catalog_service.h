#pragma once

#include "common/models/app_models.h"
#include "common/provider/provider_profile.h"

#include <string>
#include <string_view>
#include <vector>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

namespace uam
{

struct AsyncCommandTask;

class ProviderModelCatalogService
{
  public:
	ProviderModelCatalogService();

	/// Initialize with data root path. Must be called before any other methods.
	void Initialize(const std::filesystem::path& data_root, const std::vector<ProviderProfile>& profiles = {}, std::string_view configuration_context = {});

	/// Start an async refresh of OpenCode Zen free models if needed (respects 10-min throttle).
	/// Returns true if a refresh was started, false if skipped (throttled/disabled/fixture).
	bool MaybeStartRefresh();

	/// Poll for completed refresh task. Updates in-memory model list and disk cache.
	/// Returns true if models were updated.
	bool Poll();

	/// Get the current OpenCode Zen free models for the frontend (cached, no network).
	nlohmann::json GetOpenCodeZenFreeModels() const;

	/// Get the current configured OpenCode models from ~/.config/opencode/opencode.json (cached, no disk I/O on repeated calls).
	nlohmann::json GetConfiguredOpenCodeModels() const;

	/// Get the current default OpenCode model from ~/.config/opencode/opencode.json (cached, no disk I/O on repeated calls).
	std::string GetConfiguredOpenCodeDefaultModel() const;

	/// Get Codex models from cache file (cached).
	nlohmann::json GetCachedCodexModels() const;

	/// Persist a provider's latest successful runtime model discovery without replacing a valid cache on empty results.
	bool RememberSuccessfulModels(const std::string& provider_id, const nlohmann::json& models, std::string_view workspace_directory = {}, const nlohmann::json& config_options = nlohmann::json::array());

	/// Record a refresh failure while retaining the last successful provider cache.
	void RememberRefreshFailure(const std::string& provider_id, std::string error, std::string_view workspace_directory = {});

	/// Return the isolated persistent cache for the provider's current configuration.
	nlohmann::json GetCachedProviderModels(const std::string& provider_id, std::string_view workspace_directory = {}) const;
	nlohmann::json GetCachedProviderConfigOptions(const std::string& provider_id, std::string_view workspace_directory = {}) const;
	std::string GetProviderRefreshError(const std::string& provider_id, std::string_view workspace_directory = {}) const;
	/// Start discovery only when no usable cache exists or its last successful refresh is stale.
	bool BeginDiscoveryIfStale(const std::string& provider_id, std::string_view workspace_directory = {});
	/// Start a user-requested discovery even when the cache is fresh.
	bool BeginDiscovery(const std::string& provider_id, std::string_view workspace_directory = {});
	bool BeginDiscoveryIfMissing(const std::string& provider_id, std::string_view workspace_directory = {});
	bool IsDiscoveryPending(const std::string& provider_id, std::string_view workspace_directory = {}) const;
	void MarkDiscoveryLaunchStarted(const std::string& provider_id, std::string_view workspace_directory = {});
	void RememberDiscoveryCompatibilityBlocked(const std::string& provider_id, std::string error, std::string_view workspace_directory = {});

	/// Merge fallback models with runtime models (same logic as before).
	static nlohmann::json MergeAcpModelArrays(nlohmann::json fallback_models, nlohmann::json runtime_models);

	/// Parse OpenCode Zen's model-list response into free model options (pure; for tests and refresh).
	static nlohmann::json ParseOpenCodeZenFreeModels(const nlohmann::json& root);

	/// Get fallback models for a chat (OpenCode Zen + configured, or Codex cache).
	nlohmann::json FallbackAcpModelsForChat(const std::string& provider_id, std::string_view workspace_directory = {}) const;

	/// Get fallback current model for a chat.
	std::string FallbackAcpCurrentModelForChat(const std::string& provider_id, const std::string& chat_model_id) const;

	/// Check if OpenCode Zen refresh is disabled via env var.
	static bool OpenCodeZenRefreshDisabled();

	/// Check if OpenCode Zen fixture is set via env var (for tests).
	static bool HasOpenCodeZenModelsFixture();

  private:
	mutable std::mutex m_mutex;

	// Data root for cache files.
	std::filesystem::path m_data_root;

	// Cached model data (protected by mutex).
	nlohmann::json m_open_code_zen_free_models;
	nlohmann::json m_configured_open_code_models;
	std::string m_configured_open_code_default_model;
	nlohmann::json m_cached_codex_models;
	nlohmann::json m_persistent_catalogs;
	std::map<std::string, std::string> m_catalog_key_by_provider_id;
	std::map<std::string, std::string> m_refresh_error_by_provider_id;
	std::set<std::string> m_pending_discovery_provider_ids;
	std::set<std::string> m_refresh_attempted_provider_ids;

	// OpenCode config files fingerprint for cache invalidation.
	std::string m_open_code_config_fingerprint;

	// Async refresh state (heap-allocated to avoid complete type issues in header).
	std::unique_ptr<AsyncCommandTask> m_refresh_task;

	// Throttling.
	std::chrono::steady_clock::time_point m_last_refresh_attempt;
	static constexpr auto kOpenCodeZenRefreshInterval = std::chrono::minutes(10);

	// Fixture/env constants.
	static constexpr const char* kOpenCodeZenModelsUrl = "https://opencode.ai/zen/v1/models";
	static constexpr const char* kOpenCodeZenFreeModelsCacheFile = "opencode_zen_free_models_cache.json";
	static constexpr const char* kOpenCodeZenModelsFixtureEnv = "UAM_OPENCODE_ZEN_MODELS_PATH";
	static constexpr const char* kOpenCodeZenRefreshDisabledEnv = "UAM_DISABLE_OPENCODE_ZEN_REFRESH";
	static constexpr const char* kProviderModelsCacheFile = "provider_model_catalog_cache.json";
	static constexpr std::int64_t kProviderModelCacheFreshnessSeconds = 7 * 24 * 60 * 60;

	// Internal helpers.
	std::filesystem::path OpenCodeZenFreeModelsCachePath() const;
	std::vector<std::filesystem::path> OpenCodeConfigPaths() const;
	std::string OpenCodeConfigFingerprint() const;
	nlohmann::json BuiltInOpenCodeZenFreeModels() const;
	nlohmann::json ReadOpenCodeZenFreeModelsCache() const;
	void WriteOpenCodeZenFreeModelsCache(const nlohmann::json& models) const;
	std::optional<nlohmann::json> FetchOpenCodeZenModels();
	nlohmann::json ReadConfiguredOpenCodeModels();
	std::string ReadConfiguredOpenCodeDefaultModel();
	nlohmann::json ReadCachedCodexModels();
	std::string CatalogKey(const std::string& provider_id, std::string_view workspace_directory = {}) const;
	void LoadPersistentCatalogs();
	bool WritePersistentCatalogs() const;

	void StartRefreshTask();
	bool TryConsumeRefreshOutput();
};

} // namespace uam
