#include "provider_model_catalog_service.h"
#include "app/provider_worker_command.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/command_line_words.h"

#include "common/config/execution_host_config.h"
#include "common/config/provider_chat_defaults.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/acp/acp_model_json.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/state/app_state.h"
#include "common/utils/env_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "remote/runner_client.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
			return nlohmann::json::parse(text, nullptr, true, path.extension() == ".jsonc");
		}
		catch (const nlohmann::json::exception&)
		{
			return nullptr;
		}
	}

	void PushModelIfNew(nlohmann::json& models_json, std::unordered_set<std::string>& seen_model_ids, const std::string& id, const std::string& name, const std::string& description)
	{
		if (id.empty() || !seen_model_ids.insert(id).second)
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

	std::string NormalizedWorkspace(std::string_view workspace_directory)
	{
		const std::string trimmed = uam::strings::Trim(std::string(workspace_directory));
		return trimmed.empty() ? std::string{} : uam::paths::NormalizeExistingPath(uam::paths::AbsolutePathNoThrow(fs::path(trimmed))).generic_string();
	}

	std::string NormalizedCatalogWorkspace(std::string_view workspace_directory,
	                                      std::string_view execution_host_id)
	{
		if (execution_host_id.empty() || execution_host_id == "local")
			return NormalizedWorkspace(workspace_directory);
		std::string workspace = uam::strings::Trim(std::string(workspace_directory));
		if (uam::execution_hosts::IsAbsoluteRemotePath("windows", workspace))
			std::ranges::replace(workspace, '\\', '/');
		while (workspace.size() > 1 && workspace.back() == '/') workspace.pop_back();
		return workspace;
	}

	nlohmann::json CatalogScope(const std::string& provider_id,
	                           std::string_view workspace_directory,
	                           std::string_view execution_host_id)
	{
		return {
		    {"providerId", uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id)},
		    {"workspaceDirectory", NormalizedCatalogWorkspace(workspace_directory, execution_host_id)},
		    {"executionHostId", uam::strings::NonEmptyOrFallback(
		                            uam::strings::Trim(std::string(execution_host_id)), "local")},
		};
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

	/// <summary>Keeps progress diagnostics separate from the TSV catalogue on SSH as on local execution.</summary>
	ProcessExecutionResult ReadRemoteAntigravityModels(const ExecutionHost& host, const std::vector<std::string>& argv,
	    const std::string& workspace, std::stop_token stop, int timeout_ms = 30000)
	{
		ProcessExecutionResult result;
		auto& process = PlatformServicesFactory::Instance().process_service;
		uam::remote::RunnerClient client(process, host);
		if (!client.Connect(&result.error, stop)) return result;
		const std::string uuid = process.GenerateUuid();
		if (uuid.empty()) { result.error = "Could not allocate a model discovery process identity."; return result; }
		const std::string id = "model-list-" + uuid;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
		bool exited = false;
		if (!stop.stop_requested() && client.StartProcess(id, uam::paths::PathFromUtf8(workspace), argv, {}, &result.error) &&
		    client.CloseProcessInput(id, &result.error))
		{
			std::size_t diagnostic_bytes = 0;
			while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
			{
				uam::remote::ProcessPollResult poll;
				if (!client.PollProcess(id, poll, &result.error, [&]() { return stop.stop_requested() || std::chrono::steady_clock::now() >= deadline; })) break;
				if (result.output.size() + poll.standard_output.size() > 256 * 1024 || diagnostic_bytes + poll.standard_error.size() > 64 * 1024)
				{
					result.output_truncated = true;
					break;
				}
				result.output += poll.standard_output;
				diagnostic_bytes += poll.standard_error.size();
				if (!client.AcknowledgeProcessOutput(id, poll, &result.error)) break;
				if (!poll.running) { result.exit_code = poll.exit_code; exited = true; break; }
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
		result.canceled = stop.stop_requested();
		result.timed_out = !exited && !result.canceled && std::chrono::steady_clock::now() >= deadline;
		std::string cleanup;
		if (!exited && !client.StopProcess(id, &cleanup)) result.error += " Model discovery stop failed.";
		if (!client.RemoveProcess(id, &cleanup)) result.error += " Model discovery cleanup failed.";
		result.ok = exited && result.exit_code == 0 && result.error.empty() && !result.canceled && !result.timed_out && !result.output_truncated;
		return result;
	}

	ProcessExecutionResult ReadAntigravityCatalogueCommand(const ExecutionHost& host, const std::vector<std::string>& argv,
	    const std::string& workspace, std::stop_token stop, const std::chrono::steady_clock::time_point& deadline)
	{
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (stop.stop_requested() || remaining <= 0) { ProcessExecutionResult result; result.canceled = stop.stop_requested(); result.timed_out = !result.canceled; return result; }
		if (host.transport == "ssh") return ReadRemoteAntigravityModels(host, argv, workspace, stop, static_cast<int>(std::min<std::int64_t>(remaining, 30000)));
		uam::ProviderWorkerInvocation invocation;
		invocation.direct_process = true; invocation.structured_stdout = true; invocation.argv = argv;
		return uam::ExecuteProviderWorkerInvocation(invocation, uam::paths::PathFromUtf8(workspace), static_cast<int>(std::min<std::int64_t>(remaining, 30000)), stop);
	}

	bool AntigravityReadOnlyBuiltinsAvailable(const ExecutionHost& host, std::vector<std::string> command,
	    const std::string& workspace, std::stop_token stop, const std::chrono::steady_clock::time_point& deadline)
	{
		// Never submit a slash string to an older CLI that could treat an unknown builtin as a model prompt.
		// The current native1.4.0 contract was verified with empty conversation identity and zero model turns/tokens.
		command.push_back("--version");
		const auto result = ReadAntigravityCatalogueCommand(host, command, workspace, stop, deadline);
		const std::string version = uam::strings::Trim(result.output);
		return result.ok && result.exit_code == 0 && !result.output_truncated && !stop.stop_requested() &&
		    version.starts_with("1.") && std::ranges::count(version, '.') == 2 &&
		    std::ranges::all_of(version, [](unsigned char ch) { return (ch >= '0' && ch <= '9') || ch == '.'; }) &&
		    CliProviderVersionAtLeast(version, "1.4.0");
	}

	std::optional<nlohmann::json> AntigravityBuiltinData(std::string_view output, const std::string& command_name)
	{
		if (output.empty() || output.size() > 256 * 1024) return std::nullopt;
		const auto result = nlohmann::json::parse(output, nullptr, false);
		if (!result.is_object() || uam::nlohmann_json::TrimmedStringValue(result, {"status"}) != "SUCCESS" ||
		    !result.contains("conversation_id") || !result["conversation_id"].is_string() || !result["conversation_id"].get_ref<const std::string&>().empty() ||
		    !result.contains("num_turns") || !result["num_turns"].is_number_integer() || result["num_turns"] != 0) return std::nullopt;
		const auto command = result.find("command");
		if (command == result.end() || !command->is_object() || uam::nlohmann_json::TrimmedStringValue(*command, {"name"}) != command_name) return std::nullopt;
		const auto data = command->find("data");
		return data != command->end() && data->is_object() ? std::optional<nlohmann::json>(*data) : std::nullopt;
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
	m_native_model_discoveries.clear();
	m_configuration_worker.reset();
	m_configuration_refresh.reset();
	m_data_root = data_root;
	m_catalog_key_by_provider_id.clear();
	m_pending_discovery_provider_ids.clear();
	m_refresh_attempted_provider_ids.clear();
	m_scope_by_catalog_key.clear();
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
	m_cached_codex_models = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCodexCli).ReadLocalModelCatalog();

	// Load cached zen free models.
	m_open_code_zen_free_models = ReadOpenCodeZenFreeModelsCache();

	// Read opencode config once at startup.
	m_configured_open_code_models = nlohmann::json::array();
	m_configured_open_code_default_model.clear();
	RefreshConfiguredOpenCodeModels();

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
	std::vector<std::unique_ptr<NativeModelDiscovery>> completed;

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (auto task = m_native_model_discoveries.begin(); task != m_native_model_discoveries.end();)
		{
			if (!task->second->state->completed.load(std::memory_order_acquire)) { ++task; continue; }
			completed.push_back(std::move(task->second));
			task = m_native_model_discoveries.erase(task);
		}
		if (m_refresh_task->running && TryConsumeRefreshOutput())
		{
			updated = true;
		}

		if (m_configuration_refresh != nullptr && m_configuration_refresh->completed.load(std::memory_order_acquire))
		{
			updated = true;
			if (m_configuration_refresh->snapshot)
			{
				m_configured_open_code_models = std::move(m_configuration_refresh->snapshot->models);
				m_configured_open_code_default_model = std::move(m_configuration_refresh->snapshot->default_model);
			}
			else diagnostics::Write("OpenCode configuration refresh failed; cached models were retained.");
			m_configuration_worker.reset();
			m_configuration_refresh.reset();
		}
	}
	for (const auto& task : completed)
	{
		if (task->state->models)
		{
			if (task->metadata_only && !task->state->models->empty())
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_refresh_attempted_provider_ids.erase(task->scope_key + "\neffort:" + uam::nlohmann_json::TrimmedStringValue(task->state->models->front(), {"id"}));
			}
			auto previous = GetCachedProviderModels(uam::provider_ids::kAntigravityCli, task->workspace, task->host_id);
			auto models = task->metadata_only ? previous : *task->state->models;
			for (auto& model : models)
			{
				const std::string id = uam::nlohmann_json::TrimmedStringValue(model, {"id"});
				const auto& source = task->metadata_only ? *task->state->models : previous;
				for (const auto& old : source)
					if (uam::nlohmann_json::TrimmedStringValue(old, {"id"}) == id && (task->metadata_only || !model.contains("effortMetadataAtSec")))
						for (const char* field : {"defaultReasoningEffort", "supportedReasoningEfforts", "effortMetadataAtSec"})
							if (old.contains(field)) model[field] = old[field];
			}
			(void)RememberSuccessfulModels(uam::provider_ids::kAntigravityCli, models,
			    task->workspace, nlohmann::json::array(), task->host_id, task->scope_key);
		}
		if (!task->state->error.empty() || !task->state->models) RememberRefreshFailure(uam::provider_ids::kAntigravityCli, task->state->error,
		    task->workspace, task->host_id, task->scope_key);
		updated = true;
	}

	return updated;
}

std::optional<nlohmann::json> ProviderModelCatalogService::ParseAntigravityModels(std::string_view output)
{
	if (output.empty() || output.size() > 256 * 1024) return std::nullopt;
	nlohmann::json models = nlohmann::json::array();
	std::unordered_set<std::string> seen;
	std::istringstream lines{std::string(output)};
	std::string line;
	while (std::getline(lines, line))
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) continue;
		const std::size_t tab = line.find('\t');
		if (tab == std::string::npos || line.find('\t', tab + 1) != std::string::npos || models.size() >= 512) return std::nullopt;
		const std::string id = line.substr(0, tab);
		const std::string name = uam::strings::Trim(line.substr(tab + 1));
		if (id.empty() || !uam::provider_chat_defaults::IsAllowedModelId(id) || !seen.insert(id).second || name.empty() || name.size() > 512 ||
		    std::ranges::any_of(name, [](unsigned char ch) { return ch < 32 || ch == 127; })) return std::nullopt;
		models.push_back({{"id", id}, {"name", name}, {"description", "Advertised by Antigravity CLI."}});
	}
	try { (void)models.dump(); }
	catch (const nlohmann::json::exception&) { return std::nullopt; }
	return models.empty() ? std::nullopt : std::optional<nlohmann::json>(std::move(models));
}


std::optional<nlohmann::json> ProviderModelCatalogService::ParseAntigravityEffort(std::string_view output, const std::string& model_id)
{
	if (model_id.empty() || !uam::provider_chat_defaults::IsAllowedModelId(model_id)) return std::nullopt;
	const auto data = AntigravityBuiltinData(output, "effort");
	if (!data || !data->contains("adjustable") || !(*data)["adjustable"].is_boolean() ||
	    !data->contains("available") || !(*data)["available"].is_array() || (*data)["available"].size() > 5 ||
	    !data->contains("current") || !(*data)["current"].is_string()) return std::nullopt;
	std::vector<std::string> levels;
	for (const auto& value : (*data)["available"])
	{
		if (!value.is_string()) return std::nullopt;
		const std::string level = value.get<std::string>();
		if ((level != "low" && level != "medium" && level != "high" && level != "xhigh" && level != "max") ||
		    std::ranges::find(levels, level) != levels.end()) return std::nullopt;
		levels.push_back(level);
	}
	const std::string current = (*data)["current"].get<std::string>();
	if ((*data)["adjustable"].get<bool>() && (levels.empty() || std::ranges::find(levels, current) == levels.end())) return std::nullopt;
	if (!(*data)["adjustable"].get<bool>()) levels.clear();
	return nlohmann::json{{"id", model_id}, {"supportedReasoningEfforts", levels},
	    {"defaultReasoningEffort", levels.empty() ? "" : current}, {"effortMetadataAtSec", uam::time::TimestampNowSec()}};
}

bool ProviderModelCatalogService::StartAntigravityEffortDiscovery(const ProviderProfile& profile,
	const ExecutionHost& host, const std::string& workspace, const std::string& model_id, std::string* error_out)
{
	const auto fail = [&](const std::string& error) { if (error_out) *error_out = error; return false; };
	if (!uam::provider_ids::IsCliProviderAliasOf(profile.id, uam::provider_ids::kAntigravityCli) ||
	    model_id.empty() || !uam::provider_chat_defaults::IsAllowedModelId(model_id) || uam::strings::IsBlank(workspace)) return fail("An advertised Antigravity model and workspace are required.");
	if ((host.transport != "local" && host.transport != "ssh") ||
	    (host.transport == "ssh" && (!uam::remote::CanAttemptRunnerConnection(host) || !uam::execution_hosts::IsAbsoluteRemotePath(host.platform, workspace))) ||
	    (host.transport == "local" && !uam::paths::IsDirectoryNoThrow(uam::paths::PathFromUtf8(workspace)))) return fail("The reasoning discovery host or workspace is unavailable.");
	const auto models = GetCachedProviderModels(profile.id, workspace, host.id);
	const auto selected = std::ranges::find_if(models, [&](const auto& row) { return uam::nlohmann_json::TrimmedStringValue(row, {"id"}) == model_id; });
	if (selected == models.end()) return fail("The selected Antigravity model has not been advertised by this host.");
	const auto updated = selected->find("effortMetadataAtSec");
	if (updated != selected->end() && updated->is_number_integer() && *updated > uam::time::TimestampNowSec() - 600) return true;
	std::vector<std::string> argv = uam::command_line::SplitWords(profile.interactive_command);
	if (argv.empty()) argv.push_back("agy");
	argv.insert(argv.end(), {"-p", "/effort", "--output-format", "json", "--model", model_id});
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string scope = CatalogKey(profile.id, workspace, host.id), key = scope + "\neffort:" + model_id;
	if (scope.empty()) return fail("The model catalogue host is no longer configured.");
	if (m_native_model_discoveries.contains(key) || m_refresh_attempted_provider_ids.contains(key)) return true;
	if (m_native_model_discoveries.size() >= 2) return fail("Wait for another model discovery to finish, then retry reasoning discovery.");
	m_refresh_attempted_provider_ids.insert(key);
	auto task = std::make_unique<NativeModelDiscovery>();
	task->scope_key = scope; task->workspace = workspace; task->host_id = host.id; task->metadata_only = true;
	task->state = std::make_shared<NativeModelDiscoveryState>();
	const auto state = task->state;
	task->worker = std::make_unique<std::jthread>([state, host, argv, workspace, model_id](std::stop_token stop)
	{
		try
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			std::vector<std::string> base_command = argv;
			base_command.resize(base_command.size() - 6);
			if (!AntigravityReadOnlyBuiltinsAvailable(host, base_command, workspace, stop, deadline))
			{
				state->error = "Update Antigravity CLI to a compatible 1.4.x-or-newer 1.x release for safe read-only reasoning discovery.";
				state->completed.store(true, std::memory_order_release);
				return;
			}
			const auto result = ReadAntigravityCatalogueCommand(host, argv, workspace, stop, deadline);
			const auto metadata = result.ok && result.exit_code == 0 && !result.output_truncated && !stop.stop_requested() ? ParseAntigravityEffort(result.output, model_id) : std::nullopt;
			if (metadata) state->models = nlohmann::json::array({*metadata});
			else state->error = "The CLI could not verify this model's reasoning options. Update Antigravity CLI or retry discovery.";
		}
		catch (...) { state->error = "Antigravity reasoning discovery failed; previous model metadata was retained."; }
		state->completed.store(true, std::memory_order_release);
	});
	m_native_model_discoveries.emplace(key, std::move(task));
	return true;
}

std::size_t ProviderModelCatalogService::ActiveNativeModelDiscoveries() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_native_model_discoveries.size();
}

bool ProviderModelCatalogService::StartAntigravityModelDiscovery(const ProviderProfile& profile,
	const ExecutionHost& host, const std::string& workspace, std::string* error_out)
{
	const auto fail = [&](const std::string& error) { if (error_out) *error_out = error; return false; };
	if (!uam::provider_ids::IsCliProviderAliasOf(profile.id, uam::provider_ids::kAntigravityCli) || uam::strings::IsBlank(workspace))
		return fail("Antigravity model discovery requires a provider and workspace.");
	if (host.transport != "local" && host.transport != "ssh") return fail("Unsupported model discovery execution host.");
	if (host.transport == "ssh" && (!uam::remote::CanAttemptRunnerConnection(host) || !uam::execution_hosts::IsAbsoluteRemotePath(host.platform, workspace)))
		return fail("The remote workspace or helper is not ready for model discovery.");
	if (host.transport == "local" && !uam::paths::IsDirectoryNoThrow(uam::paths::PathFromUtf8(workspace)))
		return fail("The model discovery workspace is not a directory.");
	// Use the configured CLI command without adding conversation, prompt or selected-model arguments.
	const std::vector<std::string> command = uam::command_line::SplitWords(profile.interactive_command);
	std::vector<std::string> argv = command.empty() ? std::vector<std::string>{"agy"} : command;
	argv.push_back("models");
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string key = CatalogKey(profile.id, workspace, host.id);
	if (key.empty()) return fail("The model catalogue execution host is no longer configured.");
	if (m_native_model_discoveries.contains(key)) return true;
	std::erase_if(m_refresh_attempted_provider_ids, [&](const std::string& attempted) { return attempted.starts_with(key + "\neffort:"); });
	if (m_native_model_discoveries.size() >= 2) return fail("Wait for another model discovery to finish.");
	auto task = std::make_unique<NativeModelDiscovery>();
	task->scope_key = key;
	task->workspace = workspace;
	task->host_id = host.id;
	task->state = std::make_shared<NativeModelDiscoveryState>();
	const auto state = task->state;
	task->worker = std::make_unique<std::jthread>([state, host, argv, workspace](std::stop_token stop)
	{
		try
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			const ProcessExecutionResult result = ReadAntigravityCatalogueCommand(host, argv, workspace, stop, deadline);
			if (!stop.stop_requested() && result.ok && result.exit_code == 0 && !result.timed_out && !result.canceled && !result.output_truncated)
				state->models = ParseAntigravityModels(result.output);
			std::vector<std::string> base_command = argv;
			base_command.pop_back();
			const bool verified_builtins = state->models && AntigravityReadOnlyBuiltinsAvailable(host, base_command, workspace, stop, deadline);
			if (state->models && !verified_builtins) state->error = "Antigravity 1.4.x or newer compatible 1.x is required for safe read-only reasoning discovery; the native model list was retained.";
			if (verified_builtins)
			{
				std::vector<std::string> query = argv;
				query.pop_back(); query.insert(query.end(), {"-p", "/model", "--output-format", "json"});
				const auto current = ReadAntigravityCatalogueCommand(host, query, workspace, stop, deadline);
				const auto model = current.ok && current.exit_code == 0 && !current.output_truncated ? AntigravityBuiltinData(current.output, "model") : std::nullopt;
				const std::string id = model ? uam::nlohmann_json::TrimmedStringValue(*model, {"id"}) : std::string{};
				auto selected = std::ranges::find_if(*state->models, [&](const auto& row) { return uam::nlohmann_json::TrimmedStringValue(row, {"id"}) == id; });
				if (selected != state->models->end())
				{
					query = argv; query.pop_back(); query.insert(query.end(), {"-p", "/effort", "--output-format", "json", "--model", id});
					const auto effort = ReadAntigravityCatalogueCommand(host, query, workspace, stop, deadline);
					const auto metadata = effort.ok && effort.exit_code == 0 && !effort.output_truncated ? ParseAntigravityEffort(effort.output, id) : std::nullopt;
					if (metadata) for (auto field = metadata->begin(); field != metadata->end(); ++field) (*selected)[field.key()] = field.value();
					else state->error = "The CLI could not verify this model's reasoning options. Update Antigravity CLI or retry discovery.";
					(*selected)["isDefault"] = true;
					std::rotate(state->models->begin(), selected, std::next(selected));
				}
				else state->error = "The CLI did not expose its current model identity. Update Antigravity CLI to discover reasoning options.";
			}

			if (!state->models) state->error = stop.stop_requested() || result.canceled ? "Antigravity model discovery was cancelled." :
			    result.timed_out ? "Antigravity model discovery timed out." : "Antigravity model discovery failed or returned an invalid catalogue.";
		}
		catch (...) { state->error = "Antigravity model discovery failed."; }
		state->completed.store(true, std::memory_order_release);
	});
	m_native_model_discoveries.emplace(key, std::move(task));
	return true;
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


void ProviderModelCatalogService::SetExecutionHostContext(const std::vector<ExecutionHost>& hosts)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_connection_key_by_host_id.clear();
	for (const ExecutionHost& host : hosts)
	{
		if (host.id.empty() || host.id == "local" || host.transport != "ssh") continue;
		const nlohmann::json connection = nlohmann::json::array({host.id, host.transport,
		    host.ssh_alias, host.platform, host.architecture, host.runner_version,
		    host.runner_protocol_version, host.runner_directory, host.instruction_file, host.instruction_text ? nlohmann::json(*host.instruction_text) : nlohmann::json(nullptr)});
		m_connection_key_by_host_id[host.id] = StableCatalogFingerprint(connection.dump());
	}
	for (auto scope = m_scope_by_catalog_key.begin(); scope != m_scope_by_catalog_key.end();)
	{
		const nlohmann::json& metadata = scope->second;
		const std::string current_key = CatalogKey(metadata.value("providerId", ""),
		    metadata.value("workspaceDirectory", ""), metadata.value("executionHostId", "local"));
		if (current_key == scope->first) { ++scope; continue; }
		m_pending_discovery_provider_ids.erase(scope->first);
		m_refresh_attempted_provider_ids.erase(scope->first);
		m_refresh_error_by_provider_id.erase(scope->first);
		scope = m_scope_by_catalog_key.erase(scope);
	}
	for (const auto& [key, task] : m_native_model_discoveries)
		if (task->scope_key != CatalogKey(uam::provider_ids::kAntigravityCli, task->workspace, task->host_id)) task->worker->request_stop();
}

std::string ProviderModelCatalogService::CaptureScopeKey(const std::string& provider_id,
	std::string_view workspace_directory, std::string_view execution_host_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return CatalogKey(provider_id, workspace_directory, execution_host_id);
}

std::string ProviderModelCatalogService::CatalogKey(const std::string& provider_id,
	std::string_view workspace_directory, std::string_view execution_host_id) const
{
	const std::string normalized = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
	const auto configured = m_catalog_key_by_provider_id.find(normalized);
	const std::string provider_key = configured == m_catalog_key_by_provider_id.end() ? normalized : configured->second;
	const std::string host = uam::strings::NonEmptyOrFallback(
	    uam::strings::Trim(std::string(execution_host_id)), "local");
	const std::string workspace = NormalizedCatalogWorkspace(workspace_directory, host);
	std::string key = workspace.empty() ? provider_key : provider_key + "-workspace-" + StableCatalogFingerprint(workspace);
	if (host != "local")
	{
		const auto connection = m_connection_key_by_host_id.find(host);
		if (connection == m_connection_key_by_host_id.end()) return {};
		key += "-host-" + StableCatalogFingerprint(host) + "-connection-" + connection->second;
	}
	return key;
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
			for (const auto& [key, entry] : m_persistent_catalogs.items())
			{
				if (!entry.is_object()) continue;
				m_scope_by_catalog_key[key] = CatalogScope(
				    entry.value("providerId", ""), entry.value("workspaceDirectory", ""),
				    entry.value("executionHostId", "local"));
			}
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

bool ProviderModelCatalogService::RememberSuccessfulModels(const std::string& provider_id,
	const nlohmann::json& models, std::string_view workspace_directory,
	const nlohmann::json& config_options, std::string_view execution_host_id,
	std::optional<std::string_view> expected_scope_key)
{
	const bool has_models = models.is_array() && !models.empty();
	const bool has_config_options = config_options.is_array() && !config_options.empty();
	if (!has_models && !has_config_options)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string normalized_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
	const std::string host = uam::strings::NonEmptyOrFallback(
	    uam::strings::Trim(std::string(execution_host_id)), "local");
	const std::string workspace = NormalizedCatalogWorkspace(workspace_directory, host);
	const std::string key = CatalogKey(normalized_provider_id, workspace, host);
	if (key.empty() || (expected_scope_key.has_value() && (expected_scope_key->empty() || *expected_scope_key != key))) return false;
	m_scope_by_catalog_key[key] = CatalogScope(normalized_provider_id, workspace, host);
	m_pending_discovery_provider_ids.erase(key);
	m_refresh_attempted_provider_ids.insert(key);
	const nlohmann::json previous = m_persistent_catalogs.contains(key) ? m_persistent_catalogs[key] : nlohmann::json{};
	const nlohmann::json* cached_models = uam::nlohmann_json::FindArrayField(previous, "models");
	m_persistent_catalogs[key] = {
	    {"providerId", normalized_provider_id},
	    {"workspaceDirectory", workspace},
	    {"executionHostId", host},
	    {"models", has_models ? models : cached_models == nullptr ? nlohmann::json::array() : *cached_models},
	    {"updatedAt", uam::time::TimestampNow()},
	    {"updatedAtSec", uam::time::TimestampNowSec()},
	};
	if (has_config_options)
	{
		m_persistent_catalogs[key]["configOptions"] = config_options;
	}
	else if (const nlohmann::json* cached_options = uam::nlohmann_json::FindArrayField(previous, "configOptions"))
	{
		m_persistent_catalogs[key]["configOptions"] = *cached_options;
	}
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
		m_refresh_error_by_provider_id[key] = "Failed to persist provider model cache.";
		return false;
	}
	m_refresh_error_by_provider_id.erase(key);
	return true;
}

void ProviderModelCatalogService::RememberRefreshFailure(const std::string& provider_id,
	std::string error, std::string_view workspace_directory, std::string_view execution_host_id,
	std::optional<std::string_view> expected_scope_key)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string key = CatalogKey(provider_id, workspace_directory, execution_host_id);
	if (key.empty() || (expected_scope_key.has_value() && (expected_scope_key->empty() || *expected_scope_key != key))) return;
	m_scope_by_catalog_key[key] = CatalogScope(provider_id, workspace_directory, execution_host_id);
	m_pending_discovery_provider_ids.erase(key);
	m_refresh_error_by_provider_id[key] = uam::strings::Trim(std::move(error));
}

void ProviderModelCatalogService::ApplyLaunchOptionMetadata(nlohmann::json& models, std::string_view provider_id, std::string_view checked_version)
{
	const auto& runtime = ProviderRuntimeRegistry::ResolveById(provider_id);
	if (!runtime.ModelOptionsRequireRestart() || !models.is_array()) return;
	for (auto& model : models)
	{
		if (!model.is_object()) continue;
		model["supportedReasoningEfforts"] = runtime.ModelReasoningEfforts(uam::nlohmann_json::TrimmedStringValue(model, {"id"}), checked_version);
		model["defaultReasoningEffort"] = "";
		model["additionalSpeedTiers"] = nlohmann::json::array();
	}
}

nlohmann::json ProviderModelCatalogService::GetCatalogSnapshot(const std::string& provider_id,
	std::string_view workspace_directory, std::string_view execution_host_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string host = uam::strings::NonEmptyOrFallback(uam::strings::Trim(std::string(execution_host_id)), "local");
	const std::string key = CatalogKey(provider_id, workspace_directory, host);
	if (key.empty()) return {{"availableModels", nlohmann::json::array()}, {"configOptions", nlohmann::json::array()}, {"modelsLoading", false}, {"modelRefreshError", ""}};

	nlohmann::json models = nlohmann::json::array();
	nlohmann::json options = nlohmann::json::array();
	if (const nlohmann::json* catalog = uam::nlohmann_json::FindObjectField(m_persistent_catalogs, key))
	{
		if (const nlohmann::json* value = uam::nlohmann_json::FindArrayField(*catalog, "models")) models = *value;
		if (const nlohmann::json* value = uam::nlohmann_json::FindArrayField(*catalog, "configOptions")) options = *value;
	}
	if (host == "local") models = MergeLocalModelFallbacksLocked(provider_id, std::move(models));
	// Persistent metadata is not evidence for the current installation; the app decorates checked host snapshots.
	ApplyLaunchOptionMetadata(models, provider_id, {});
	const std::map<std::string, std::string>::const_iterator error = m_refresh_error_by_provider_id.find(key);
	return {
	    {"availableModels", std::move(models)},
	    {"configOptions", std::move(options)},
	    {"modelsLoading", m_pending_discovery_provider_ids.contains(key)},
	    {"modelRefreshError", error == m_refresh_error_by_provider_id.end() ? std::string{} : error->second},
	};
}

nlohmann::json ProviderModelCatalogService::GetCachedProviderModels(const std::string& provider_id,
	std::string_view workspace_directory, std::string_view execution_host_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string key = CatalogKey(provider_id, workspace_directory, execution_host_id);
	if (key.empty()) return nlohmann::json::array();
	const auto entry = m_persistent_catalogs.find(key);
	if (entry == m_persistent_catalogs.end() || !entry->is_object())
	{
		return nlohmann::json::array();
	}
	const nlohmann::json* models = uam::nlohmann_json::FindArrayField(*entry, "models");
	nlohmann::json result = models == nullptr ? nlohmann::json::array() : *models;
	ApplyLaunchOptionMetadata(result, provider_id, {});
	return result;
}

nlohmann::json ProviderModelCatalogService::GetCachedProviderModelChoices(const std::string& provider_id,
	std::string_view workspace_directory, std::string_view execution_host_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string key = CatalogKey(provider_id, workspace_directory, execution_host_id);
	if (key.empty()) return nlohmann::json::array();
	const auto entry = m_persistent_catalogs.find(key);
	if (entry == m_persistent_catalogs.end() || !entry->is_object()) return nlohmann::json::array();
	std::vector<std::string> ids;
	bool model_config_present = false;
	if (const auto* options = uam::nlohmann_json::FindArrayField(*entry, "configOptions"))
	{
		// Current native publication is bounded. Ambiguous oversized legacy cache
		// must not hide a later authoritative model option and fall back to legacy IDs.
		if (options->size() > 64) return nlohmann::json::array();
		for (const auto& option : *options)
			if (const auto* values = uam::nlohmann_json::FindArrayField(option, "options"); values && values->size() > 128)
				return nlohmann::json::array();
		AcpSessionState parsed;
		(void)uam::acp_detail::UpdateAcpConfigOptions(parsed, *options);
		const auto option = std::ranges::find_if(parsed.available_config_options, [](const AcpConfigOptionState& value)
		{
			return value.category == "model" || value.id == "model";
		});
		if (option != parsed.available_config_options.end())
		{
			model_config_present = true;
			ids = uam::acp_models::ConfigModelIds(*option);
		}
	}
	if (!model_config_present)
		if (const auto* models = uam::nlohmann_json::FindArrayField(*entry, "models"))
			for (const auto& model : *models)
			{
				if (!model.is_object() || !model.contains("id") || !model["id"].is_string()) continue;
				const std::string id = model["id"].get<std::string>();
				if (id.empty() || id.size() > 256 || id.find('\0') != std::string::npos) continue;
				uam::ranges::PushUniqueNonEmptyString(ids, id);
				if (ids.size() == 32) break;
			}
	nlohmann::json result = nlohmann::json::array();
	std::vector<std::string> seen;
	for (const std::string& id : ids)
	{
		if (id.empty() || id.size() > 256 || id.find('\0') != std::string::npos || std::ranges::find(seen, id) != seen.end()) continue;
		seen.push_back(id);
		result.push_back({{"id", id}});
		if (result.size() == 32) break;
	}
	return result;
}

nlohmann::json ProviderModelCatalogService::GetCachedProviderConfigOptions(const std::string& provider_id,
	std::string_view workspace_directory, std::string_view execution_host_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string key = CatalogKey(provider_id, workspace_directory, execution_host_id);
	if (key.empty()) return nlohmann::json::array();
	const auto entry = m_persistent_catalogs.find(key);
	if (entry == m_persistent_catalogs.end() || !entry->is_object()) return nlohmann::json::array();
	const nlohmann::json* options = uam::nlohmann_json::FindArrayField(*entry, "configOptions");
	return options == nullptr ? nlohmann::json::array() : *options;
}

std::string ProviderModelCatalogService::GetProviderRefreshError(const std::string& provider_id,
	std::string_view workspace_directory, std::string_view execution_host_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string key = CatalogKey(provider_id, workspace_directory, execution_host_id);
	if (key.empty()) return {};
	const auto error = m_refresh_error_by_provider_id.find(key);
	return error == m_refresh_error_by_provider_id.end() ? std::string{} : error->second;
}

nlohmann::json ProviderModelCatalogService::GetCatalogScopes() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	nlohmann::json scopes = nlohmann::json::array();
	for (const auto& [key, scope] : m_scope_by_catalog_key)
		if (!key.empty() && key == CatalogKey(scope.value("providerId", ""), scope.value("workspaceDirectory", ""), scope.value("executionHostId", "local"))) scopes.push_back(scope);
	return scopes;
}

bool ProviderModelCatalogService::BeginDiscoveryIfStale(const std::string& provider_id,
	std::string_view workspace_directory, std::string_view execution_host_id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string normalized = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
	const std::string key = CatalogKey(normalized, workspace_directory, execution_host_id);
	if (key.empty()) return false;
	m_scope_by_catalog_key[key] = CatalogScope(normalized, workspace_directory, execution_host_id);
	const auto entry = m_persistent_catalogs.find(key);
	const nlohmann::json* models = entry == m_persistent_catalogs.end() ? nullptr : uam::nlohmann_json::FindArrayField(entry.value(), "models");
	const std::int64_t updated_at = entry == m_persistent_catalogs.end() ? 0 : entry->value("updatedAtSec", static_cast<std::int64_t>(0));
	if (models != nullptr && !models->empty() && updated_at > 0 && uam::time::TimestampNowSec() - updated_at < kProviderModelCacheFreshnessSeconds)
	{
		return false;
	}
	if (m_pending_discovery_provider_ids.contains(key) || m_refresh_attempted_provider_ids.contains(key)) return false;
	m_pending_discovery_provider_ids.insert(key);
	m_refresh_error_by_provider_id.erase(key);
	return true;
}

bool ProviderModelCatalogService::BeginDiscovery(const std::string& provider_id,
	std::string_view workspace_directory, std::string_view execution_host_id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string key = CatalogKey(provider_id, workspace_directory, execution_host_id);
	if (key.empty()) return false;
	m_scope_by_catalog_key[key] = CatalogScope(provider_id, workspace_directory, execution_host_id);
	if (m_pending_discovery_provider_ids.contains(key)) return false;
	m_pending_discovery_provider_ids.insert(key);
	m_refresh_error_by_provider_id.erase(key);
	return true;
}

bool ProviderModelCatalogService::BeginDiscoveryIfMissing(const std::string& provider_id,
	std::string_view workspace_directory, std::string_view execution_host_id)
{
	return BeginDiscoveryIfStale(provider_id, workspace_directory, execution_host_id);
}

bool ProviderModelCatalogService::IsDiscoveryPending(const std::string& provider_id,
	std::string_view workspace_directory, std::string_view execution_host_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_pending_discovery_provider_ids.contains(CatalogKey(provider_id, workspace_directory, execution_host_id));
}

void ProviderModelCatalogService::MarkDiscoveryLaunchStarted(const std::string& provider_id,
	std::string_view workspace_directory, std::string_view execution_host_id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string key = CatalogKey(provider_id, workspace_directory, execution_host_id);
	if (key.empty()) return;
	m_scope_by_catalog_key[key] = CatalogScope(provider_id, workspace_directory, execution_host_id);
	m_refresh_attempted_provider_ids.insert(key);
	m_refresh_error_by_provider_id.erase(key);
}

void ProviderModelCatalogService::RememberDiscoveryCompatibilityBlocked(const std::string& provider_id,
	std::string error, std::string_view workspace_directory, std::string_view execution_host_id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string key = CatalogKey(provider_id, workspace_directory, execution_host_id);
	if (key.empty()) return;
	m_scope_by_catalog_key[key] = CatalogScope(provider_id, workspace_directory, execution_host_id);
	m_pending_discovery_provider_ids.insert(key);
	m_refresh_error_by_provider_id[key] = uam::strings::Trim(std::move(error));
}

nlohmann::json ProviderModelCatalogService::MergeAcpModelArrays(nlohmann::json fallback_models, nlohmann::json runtime_models, bool prefer_runtime)
{
	if (!fallback_models.is_array())
	{
		fallback_models = nlohmann::json::array();
	}
	if (!runtime_models.is_array())
	{
		return fallback_models;
	}

	std::unordered_set<std::string> seen_model_ids;
	std::unordered_map<std::string, std::size_t> fallback_indices;
	for (std::size_t index = 0; index < fallback_models.size(); ++index)
	{
		const nlohmann::json& model = fallback_models[index];
		if (model.is_object())
		{
			const std::string id = uam::nlohmann_json::TrimmedStringValue(model, {"id"});
			if (uam::ranges::InsertTrimmedNonEmptyString(seen_model_ids, id) && prefer_runtime)
				fallback_indices.emplace(id, index);
		}
	}
	for (const nlohmann::json& model : runtime_models)
	{
		if (!model.is_object())
		{
			continue;
		}
		const std::string id = uam::nlohmann_json::TrimmedStringValue(model, {"id"});
		if (id.empty()) continue;
		std::size_t target_index = fallback_models.size();
		if (!uam::ranges::InsertTrimmedNonEmptyString(seen_model_ids, id))
		{
			const std::unordered_map<std::string, std::size_t>::iterator target = fallback_indices.find(id);
			if (target == fallback_indices.end()) continue;
			target_index = target->second;
			// Only the first runtime entry replaces a fallback, including cleared capabilities.
			fallback_indices.erase(target);
		}
		nlohmann::json normalized_model = model;
		normalized_model["id"] = id;
		if (target_index == fallback_models.size()) fallback_models.push_back(std::move(normalized_model));
		else fallback_models[target_index] = std::move(normalized_model);
	}
	return fallback_models;
}

nlohmann::json ProviderModelCatalogService::FallbackAcpModelsForChat(const std::string& provider_id, std::string_view workspace_directory) const
{
	nlohmann::json cached = GetCachedProviderModels(provider_id, workspace_directory);
	std::lock_guard<std::mutex> lock(m_mutex);
	return MergeLocalModelFallbacksLocked(provider_id, std::move(cached));
}

nlohmann::json ProviderModelCatalogService::MergeLocalModelFallbacksLocked(const std::string& provider_id, nlohmann::json cached) const
{
	if (uam::provider_ids::IsCliProviderAliasOf(provider_id, uam::provider_ids::kCodexCli))
	{
		return MergeAcpModelArrays(std::move(cached), m_cached_codex_models);
	}
	if (uam::provider_ids::IsCliProviderAliasOf(provider_id, uam::provider_ids::kOpenCodeCli))
	{
		// Zen's public list is advisory only.  Runtime discovery and configured
		// provider models are the selectable sources; the endpoint can advertise
		// IDs that the installed OpenCode binary does not accept.
		return MergeAcpModelArrays(std::move(cached), m_configured_open_code_models);
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

std::vector<fs::path> ProviderModelCatalogService::OpenCodeConfigPaths() const
{
	fs::path config_directory;
	if (const std::optional<fs::path> config_home = uam::env::GetTrimmedPath("XDG_CONFIG_HOME"))
	{
		config_directory = *config_home / "opencode";
	}
#if defined(_WIN32)
	else if (const std::optional<fs::path> app_data = uam::env::GetTrimmedPath("APPDATA"))
	{
		config_directory = *app_data / "opencode";
	}
#endif
	else if (const std::optional<fs::path> home = uam::env::GetTrimmedPath("HOME"))
	{
		config_directory = *home / ".config" / "opencode";
	}
	else
	{
		config_directory = uam::paths::CurrentPathOrDot() / ".config" / "opencode";
	}

	std::vector<fs::path> paths{config_directory / "config.json", config_directory / "opencode.json", config_directory / "opencode.jsonc"};
	if (const std::optional<fs::path> custom_config = uam::env::GetTrimmedPath("OPENCODE_CONFIG")) paths.push_back(*custom_config);
	return paths;
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

	std::unordered_set<std::string> seen_model_ids;
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

std::optional<nlohmann::json> ProviderModelCatalogService::FetchOpenCodeZenModels(std::stop_token stop_token)
{
	if (stop_token.stop_requested()) return std::nullopt;
	if (const std::optional<nlohmann::json> fixture_path = uam::env::GetTrimmedPath(kOpenCodeZenModelsFixtureEnv))
	{
		const nlohmann::json root = ReadJsonFile(*fixture_path);
		return root.is_object() ? std::optional<nlohmann::json>(root) : std::nullopt;
	}

	const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(std::string("curl -s --max-time 4 ") + kOpenCodeZenModelsUrl, 6000, stop_token);
	if (!result.ok || result.timed_out || result.canceled || result.exit_code != 0 || result.output.empty())
	{
		return std::nullopt;
	}

	const nlohmann::json root = nlohmann::json::parse(result.output, nullptr, false);
	return root.is_object() ? std::optional<nlohmann::json>(root) : std::nullopt;
}

std::optional<ProviderModelCatalogService::ConfiguredOpenCodeSnapshot> ProviderModelCatalogService::ReadConfiguredOpenCodeModels(const std::vector<fs::path>& paths, std::stop_token stop_token)
{
	nlohmann::json models_json = nlohmann::json::array();
	std::string default_model_id;
	std::unordered_set<std::string> seen_model_ids;
	for (const fs::path& path : paths)
	{
		if (stop_token.stop_requested()) return std::nullopt;
		std::error_code error;
		const bool exists = fs::exists(path, error);
		if (error) return std::nullopt;
		if (!exists) continue;
		if (!fs::is_regular_file(path, error) || error) return std::nullopt;
		const nlohmann::json config = ReadJsonFile(path);
		if (!config.is_object()) return std::nullopt;
		const std::string configured = uam::nlohmann_json::TrimmedStringValue(config, {"model"});
		if (!configured.empty()) default_model_id = configured;
		const nlohmann::json* providers = uam::nlohmann_json::FindObjectField(config, "provider");
		if (providers == nullptr) continue;
		for (const auto& provider_entry : providers->items())
		{
			const std::string provider_id = uam::strings::Trim(provider_entry.key());
			const nlohmann::json* models = provider_entry.value().is_object() ? uam::nlohmann_json::FindObjectField(provider_entry.value(), "models") : nullptr;
			if (provider_id.empty() || models == nullptr) continue;
			for (const auto& model_entry : models->items())
			{
				const std::string model_id = uam::strings::Trim(model_entry.key());
				if (model_id.empty()) continue;
				const std::string name = model_entry.value().is_object() ? uam::nlohmann_json::TrimmedStringValue(model_entry.value(), {"name", "displayName", "display_name"}) : std::string{};
				const std::string description = model_entry.value().is_object() ? uam::nlohmann_json::TrimmedStringValue(model_entry.value(), {"description"}) : std::string{};
				const std::size_t previous_count = models_json.size();
				PushModelIfNew(models_json, seen_model_ids, provider_id + "/" + model_id, uam::strings::NonEmptyOrFallback(name, model_id), description);
				if (models_json.size() == previous_count) continue;
				const nlohmann::json* limit = uam::nlohmann_json::FindObjectField(model_entry.value(), "limit");
				if (limit == nullptr) continue;
				const std::optional<std::int64_t> context = uam::acp_models::ModelTokenLimit(*limit, {"context"});
				const std::optional<std::int64_t> output = uam::acp_models::ModelTokenLimit(*limit, {"output"});
				if (context) models_json.back()["contextWindowTokens"] = *context;
				if (output) models_json.back()["maxOutputTokens"] = *output;
			}
		}
	}

	if (stop_token.stop_requested()) return std::nullopt;
	return ConfiguredOpenCodeSnapshot{std::move(models_json), std::move(default_model_id)};
}


void ProviderModelCatalogService::RefreshConfiguredOpenCodeModels()
{
	const std::optional<ConfiguredOpenCodeSnapshot> snapshot = ReadConfiguredOpenCodeModels(OpenCodeConfigPaths());
	if (snapshot)
	{
		m_configured_open_code_models = snapshot->models;
		m_configured_open_code_default_model = snapshot->default_model;
	}
}

bool ProviderModelCatalogService::StartConfiguredOpenCodeRefresh()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_configuration_refresh != nullptr) return false;
	const std::vector<fs::path> paths = OpenCodeConfigPaths();
	const std::shared_ptr<ConfiguredOpenCodeRefresh> state = std::make_shared<ConfiguredOpenCodeRefresh>();
	m_configuration_worker = std::make_unique<std::jthread>([paths, state](std::stop_token stop_token)
	{
		try { state->snapshot = ReadConfiguredOpenCodeModels(paths, stop_token); }
		catch (...) { state->snapshot.reset(); }
		state->completed.store(true, std::memory_order_release);
	});
	m_configuration_refresh = state;
	return true;
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
		    const std::optional<nlohmann::json> result = FetchOpenCodeZenModels(stop_token);

		    if (!result.has_value())
		    {
			    state->completed.store(true, std::memory_order_release);
			    return;
		    }

		    const nlohmann::json models = ParseOpenCodeZenFreeModels(*result);
		    if (!stop_token.stop_requested() && models.is_array() && !models.empty())
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
