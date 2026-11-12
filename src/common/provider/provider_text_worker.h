#pragma once
#include "common/provider/provider_profile.h"
#include "common/provider/provider_ids.h"
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#include <algorithm>
#include <iterator>
#include <vector>
#include <optional>
#include <sstream>
#include <nlohmann/json.hpp>
#include "common/utils/string_utils.h"
namespace uam::provider_workers
{
	inline bool IsProvider(const ProviderProfile& profile, std::string_view provider_id)
	{
		return uam::provider_ids::IsCliProviderAliasOf(profile.id, provider_id);
	}

	/// <summary>True for an OpenCode `--print-logs` line, e.g. `timestamp=... level=ERROR run=... message="..."`.</summary>
	inline bool IsOpenCodeLogLine(std::string_view line)
	{
		return line.starts_with("timestamp=") && line.find(" level=") != std::string_view::npos;
	}

	/// <summary>Extracts the sole completed Codex assistant response; rejects incomplete or tool-bearing streams before domain parsing.</summary>
	inline std::optional<std::string> CompletedTextWorkerOutput(std::string_view provider_id, std::string_view output, std::size_t max_output_bytes)
	{
		if (output.size() > max_output_bytes) return std::nullopt;
		if (uam::provider_ids::IsCliProviderAliasOf(provider_id, uam::provider_ids::kOpenCodeCli))
		{
			// Worker runs request error-level logs (merged into output) to detect retry loops; they are not the reply.
			std::string text;
			for (std::size_t at = 0; at < output.size();)
			{
				const std::size_t end = std::min(output.find('\n', at), output.size());
				std::string_view line = output.substr(at, end - at);
				if (line.ends_with('\r')) line.remove_suffix(1);
				if (!IsOpenCodeLogLine(line)) text.append(output.substr(at, std::min(end + 1, output.size()) - at));
				at = end + 1;
			}
			return text;
		}
		if (!uam::provider_ids::IsCliProviderAliasOf(provider_id, uam::provider_ids::kCodexCli)) return std::string(output);
		const nlohmann::json direct = nlohmann::json::parse(output, nullptr, false);
		// Legacy plain domain JSON is supported, but a single protocol event is not a completed response.
		if (direct.is_object() && !direct.contains("type") && !direct.contains("item") && !direct.contains("error")) return std::string(output);

		enum class Phase { Thread, Turn, Items, Complete };
		Phase phase = Phase::Thread;
		bool preamble_seen = false;
		bool final_seen = false;
		std::string final_text;
		std::istringstream stream{std::string(output)};
		std::string line;
		while (std::getline(stream, line))
		{
			line = uam::strings::Trim(line);
			if (line.empty()) continue;
			if (line == "Reading prompt from stdin..." && phase == Phase::Thread && !preamble_seen)
			{
				preamble_seen = true;
				continue;
			}
			const nlohmann::json event = nlohmann::json::parse(line, nullptr, false);
			if (!event.is_object() || !event.contains("type") || !event["type"].is_string() || event.contains("error")) return std::nullopt;
			const std::string type = event["type"].get<std::string>();
			if (type == "thread.started" && phase == Phase::Thread)
			{
				if (!event.contains("thread_id") || !event["thread_id"].is_string() || event["thread_id"].get_ref<const std::string&>().empty()) return std::nullopt;
				phase = Phase::Turn;
			}
			else if (type == "turn.started" && phase == Phase::Turn)
			{
				phase = Phase::Items;
			}
			else if (type == "item.completed" && phase == Phase::Items)
			{
				if (!event.contains("item") || !event["item"].is_object()) return std::nullopt;
				const nlohmann::json& item = event["item"];
				if (!item.contains("type") || !item["type"].is_string() || item.contains("error")) return std::nullopt;
				const std::string item_type = item["type"].get<std::string>();
				if (item_type == "reasoning") continue;
				if (item_type != "agent_message" || final_seen || !item.contains("text") || !item["text"].is_string()) return std::nullopt;
				final_seen = true;
				final_text = item["text"].get<std::string>();
			}
			else if (type == "turn.completed" && phase == Phase::Items && final_seen)
			{
				phase = Phase::Complete;
			}
			else return std::nullopt;
		}
		return phase == Phase::Complete ? std::optional<std::string>(std::move(final_text)) : std::nullopt;
	}

	/// <summary>Counts OpenCode failed-model-request log lines in the complete lines of `output` into
	/// `reports`; returns a stop reason from the second report on, else empty.</summary>
	/// <remarks>OpenCode 1.x `run` retries retryable provider errors (usage or rate limits) without
	/// exiting or printing anything, so a worker would otherwise wait out its whole timeout. Its
	/// `--print-logs --log-level ERROR` output carries one `message="stream error"` line per attempt.
	/// One report is tolerated because OpenCode's own retry gets past a one-off 429 or overload.</remarks>
	inline std::string OpenCodeWorkerProviderError(std::string_view output, int& reports)
	{
		std::string reason;
		for (std::size_t at = 0, end; (end = output.find('\n', at)) != std::string_view::npos; at = end + 1)
		{
			std::string_view line = output.substr(at, end - at);
			if (line.ends_with('\r')) line.remove_suffix(1);
			if (!IsOpenCodeLogLine(line) || line.find(" level=ERROR ") == std::string_view::npos ||
			    line.find(" message=\"stream error\"") == std::string_view::npos || ++reports < 2) continue;
			reason = "OpenCode stopped after repeated provider errors instead of waiting for its retries";
			constexpr std::string_view kDetail = " error.error=\"";
			const std::size_t detail = line.find(kDetail);
			if (detail == std::string_view::npos) { reason += "."; continue; }
			const std::string_view value = line.substr(detail + kDetail.size());
			const std::string text = uam::strings::SafeLine(value.substr(0, value.find('"')), 200);
			reason += text.empty() ? "." : ": " + text + ".";
		}
		return reason;
	}

	/// <summary>Maps owned worker paths to the selected host without changing adjacent directory names.</summary>
	inline void RemapWorkerPaths(std::vector<std::string>& argv, const std::filesystem::path& original_directory,
	    const std::filesystem::path& target_directory, std::string_view platform)
	{
		const std::string original = uam::paths::Utf8PathString(original_directory);
		if (original.empty()) return;
		const std::string target = uam::paths::Utf8PathString(target_directory);
		for (std::string& argument : argv)
		{
			if (argument == original) argument = target;
			else if (argument.size() > original.size() && argument.starts_with(original) &&
			    (argument[original.size()] == '/' || argument[original.size()] == '\\'))
			{
				std::string relative = argument.substr(original.size() + 1);
				const char separator = platform == "windows" ? '\\' : '/';
				std::replace(relative.begin(), relative.end(), separator == '/' ? '\\' : '/', separator);
				argument = target + separator + relative;
			}
		}
	}

	inline bool ApplyWorkerIsolationPolicy(const ProviderProfile& profile, const std::filesystem::path& workspace, std::vector<std::string>& argv, bool private_isolation = false)
	{
		// The checked V2 bridge owns a proof-verified deny-all lease. Never leave
		// a V1 opencode.json or V1 argv flags in that worker's empty workspace.
		if (private_isolation) return IsProvider(profile, uam::provider_ids::kOpenCodeCli) && argv.size() == 5 && argv[1] == "--uam-opencode-v2-text-worker";
		if (IsProvider(profile, uam::provider_ids::kOpenCodeCli))
		{
			if (!uam::io::WriteTextFile(workspace / "opencode.json", R"({"permission":{"*":"deny","external_directory":"deny"},"agent":{"uam-text-worker":{"mode":"primary","prompt":"Complete the supplied text-only task directly. Do not delegate or compact. Tools are denied.","permission":{"*":"deny"}}},"share":"disabled","autoupdate":false})"))
			{
				return false;
			}
			if (argv.size() < 2) return false;
			argv.insert(argv.begin() + 2, {"--pure", "--dir", uam::paths::Utf8PathString(workspace)});
		}
		else if (IsProvider(profile, uam::provider_ids::kGeminiCli))
		{
			const std::filesystem::path policy_file = workspace / "deny-all-tools.toml";
			if (!uam::io::WriteTextFile(policy_file, R"([[rule]]
toolName = "*"
decision = "deny"
priority = 999

[[rule]]
toolName = "*"
mcpName = "*"
decision = "deny"
priority = 999
)"))
			{
				return false;
			}
			if (argv.empty()) return false;
			argv.insert(argv.begin() + 1, {
			                                  "--approval-mode",
			                                  "plan",
			                                  "--admin-policy",
			                                  uam::paths::Utf8PathString(policy_file),
			                              });
		}
		return true;
	}

	inline bool RemoveWorkerPromptArgument(const ProviderProfile& profile, std::vector<std::string>& argv)
	{
		if (IsProvider(profile, uam::provider_ids::kGeminiCli) || IsProvider(profile, uam::provider_ids::kCopilotCli))
		{
			const auto prompt_flag = std::ranges::find(argv, "-p");
			if (prompt_flag == argv.end() || std::next(prompt_flag) == argv.end()) return false;
			argv.erase(prompt_flag, std::next(prompt_flag, 2));
			return true;
		}
		if (IsProvider(profile, uam::provider_ids::kClaudeCli))
		{
			if (argv.size() < 2 || argv[argv.size() - 2] != "--") return false;
			argv.erase(argv.end() - 2, argv.end());
			return true;
		}
		if (IsProvider(profile, uam::provider_ids::kCodexCli) || IsProvider(profile, uam::provider_ids::kOpenCodeCli))
		{
			if (argv.size() < 2) return false;
			argv.pop_back();
			return true;
		}
		return false;
	}

}
