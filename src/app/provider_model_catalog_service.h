#pragma once

#include "common/models/app_models.h"

#include <string>
#include <string_view>
#include <vector>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <memory>
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
	void Initialize(const std::filesystem::path& data_root);

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

	/// Merge fallback models with runtime models (same logic as before).
	static nlohmann::json MergeAcpModelArrays(nlohmann::json fallback_models, nlohmann::json runtime_models);

	/// Get fallback models for a chat (OpenCode Zen + configured, or Codex cache).
	nlohmann::json FallbackAcpModelsForChat(const std::string& provider_id) const;

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

	// OpenCode config file mtime for cache invalidation.
	std::filesystem::file_time_type m_open_code_config_mtime;

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

	// Internal helpers.
	std::filesystem::path OpenCodeZenFreeModelsCachePath() const;
	std::filesystem::path OpenCodeConfigPath() const;
	nlohmann::json BuiltInOpenCodeZenFreeModels() const;
	nlohmann::json ReadOpenCodeZenFreeModelsCache() const;
	void WriteOpenCodeZenFreeModelsCache(const nlohmann::json& models) const;
	std::optional<nlohmann::json> FetchOpenCodeZenModels();
	nlohmann::json ParseOpenCodeZenFreeModels(const nlohmann::json& root) const;
	nlohmann::json ReadConfiguredOpenCodeModels();
	std::string ReadConfiguredOpenCodeDefaultModel();
	nlohmann::json ReadCachedCodexModels();

	void StartRefreshTask();
	bool TryConsumeRefreshOutput();
};

} // namespace uam