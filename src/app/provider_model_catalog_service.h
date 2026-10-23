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
#include <optional>
#include <set>
#include <thread>
#include <atomic>

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

	/// Bind remote catalogs to configured connection ownership; labels and health do not invalidate caches.
	void SetExecutionHostContext(const std::vector<ExecutionHost>& hosts);
	/// Capture the immutable catalog scope before launching a provider process.
	std::string CaptureScopeKey(const std::string& provider_id, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local") const;

	/// Start an async refresh of OpenCode Zen free models if needed (respects 10-min throttle).
	/// Returns true if a refresh was started, false if skipped (throttled/disabled/fixture).
	bool MaybeStartRefresh();

	/// Start an explicit background read of local OpenCode configuration, retaining the current cache.
	bool StartConfiguredOpenCodeRefresh();
	/// <summary>Lists the native Antigravity catalogue without opening a conversation on the selected host.</summary>
	bool StartAntigravityModelDiscovery(const ProviderProfile& profile, const ExecutionHost& host, const std::string& workspace, std::string* error_out = nullptr);
	bool StartAntigravityEffortDiscovery(const ProviderProfile& profile, const ExecutionHost& host, const std::string& workspace, const std::string& model_id, std::string* error_out = nullptr);
	static std::optional<nlohmann::json> ParseAntigravityEffort(std::string_view output, const std::string& model_id);
	std::size_t ActiveNativeModelDiscoveries() const;
	/// <summary>Accepts only the bounded native ID-tab-name catalogue; no model-specific effort support is inferred.</summary>
	static std::optional<nlohmann::json> ParseAntigravityModels(std::string_view output);

	/// Poll completed refresh tasks without reading local configuration files.
	/// Returns true when a refresh result was consumed.
	bool Poll();

	/// Get the current OpenCode Zen free models for the frontend (cached, no network).
	nlohmann::json GetOpenCodeZenFreeModels() const;

	/// Get the current configured OpenCode models from ~/.config/opencode/opencode.json (cached, no disk I/O on repeated calls).
	nlohmann::json GetConfiguredOpenCodeModels() const;

	/// Get the current default OpenCode model from ~/.config/opencode/opencode.json (cached, no disk I/O on repeated calls).
	std::string GetConfiguredOpenCodeDefaultModel() const;

	/// Persist a provider's latest successful runtime model discovery without replacing a valid cache on empty results.
	bool RememberSuccessfulModels(const std::string& provider_id, const nlohmann::json& models, std::string_view workspace_directory = {}, const nlohmann::json& config_options = nlohmann::json::array(), std::string_view execution_host_id = "local", std::optional<std::string_view> expected_scope_key = std::nullopt);

	/// Record a refresh failure while retaining the last successful provider cache.
	void RememberRefreshFailure(const std::string& provider_id, std::string error, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local", std::optional<std::string_view> expected_scope_key = std::nullopt);

	/// Return the isolated persistent cache for the provider's current configuration.
	nlohmann::json GetCachedProviderModels(const std::string& provider_id, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local") const;
	/// Return at most 32 exact same-scope advertised IDs, native model-config choices take precedence, without local/global fallbacks.
	nlohmann::json GetCachedProviderModelChoices(const std::string& provider_id, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local") const;
	nlohmann::json GetCachedProviderConfigOptions(const std::string& provider_id, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local") const;
	std::string GetProviderRefreshError(const std::string& provider_id, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local") const;
	/// Read models, config and discovery state together, resolving the scope once.
	nlohmann::json GetCatalogSnapshot(const std::string& provider_id, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local") const;

	nlohmann::json GetCatalogScopes() const;
	/// Start discovery only when no usable cache exists or its last successful refresh is stale.
	bool BeginDiscoveryIfStale(const std::string& provider_id, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local");
	/// Start a user-requested discovery even when the cache is fresh.
	bool BeginDiscovery(const std::string& provider_id, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local");
	bool BeginDiscoveryIfMissing(const std::string& provider_id, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local");
	bool IsDiscoveryPending(const std::string& provider_id, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local") const;
	void MarkDiscoveryLaunchStarted(const std::string& provider_id, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local");
	void RememberDiscoveryCompatibilityBlocked(const std::string& provider_id, std::string error, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local");

	/// Append unseen runtime models to the fallback list.
	/// Live metadata can replace matching fallbacks without changing selector order.
	/// Decorate launch-configured models only from the same host's checked installation, clearing stale choices.
	static void ApplyLaunchOptionMetadata(nlohmann::json& models, std::string_view provider_id, std::string_view checked_version);

	static nlohmann::json MergeAcpModelArrays(nlohmann::json fallback_models, nlohmann::json runtime_models, bool prefer_runtime = false);

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
	// Caller holds m_mutex so cached models and local fallbacks share one snapshot.
	nlohmann::json MergeLocalModelFallbacksLocked(const std::string& provider_id, nlohmann::json cached) const;

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
	std::map<std::string, std::string> m_connection_key_by_host_id;
	std::map<std::string, std::string> m_refresh_error_by_provider_id;
	std::map<std::string, nlohmann::json> m_scope_by_catalog_key;
	std::set<std::string> m_pending_discovery_provider_ids;
	std::set<std::string> m_refresh_attempted_provider_ids;

	struct ConfiguredOpenCodeSnapshot
	{
		nlohmann::json models = nlohmann::json::array();
		std::string default_model;
	};
	struct ConfiguredOpenCodeRefresh
	{
		std::optional<ConfiguredOpenCodeSnapshot> snapshot;
		std::atomic<bool> completed{false};
	};
	std::shared_ptr<ConfiguredOpenCodeRefresh> m_configuration_refresh;
	std::unique_ptr<std::jthread> m_configuration_worker;
	struct NativeModelDiscoveryState
	{
		std::optional<nlohmann::json> models;
		std::string error;
		std::atomic<bool> completed{false};
	};
	struct NativeModelDiscovery
	{
		std::string scope_key;
		bool metadata_only = false;
		std::string workspace;
		std::string host_id;
		std::shared_ptr<NativeModelDiscoveryState> state;
		// Last member joins the worker before its output fields can be destroyed.
		std::unique_ptr<std::jthread> worker;
	};
	std::map<std::string, std::unique_ptr<NativeModelDiscovery>> m_native_model_discoveries;

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
	static std::optional<ConfiguredOpenCodeSnapshot> ReadConfiguredOpenCodeModels(const std::vector<std::filesystem::path>& paths, std::stop_token stop_token = {});
	nlohmann::json ReadOpenCodeZenFreeModelsCache() const;
	void WriteOpenCodeZenFreeModelsCache(const nlohmann::json& models) const;
	std::optional<nlohmann::json> FetchOpenCodeZenModels(std::stop_token stop_token);
	void RefreshConfiguredOpenCodeModels();
	std::string CatalogKey(const std::string& provider_id, std::string_view workspace_directory = {}, std::string_view execution_host_id = "local") const;
	void LoadPersistentCatalogs();
	bool WritePersistentCatalogs() const;

	void StartRefreshTask();
	bool TryConsumeRefreshOutput();
};

} // namespace uam
