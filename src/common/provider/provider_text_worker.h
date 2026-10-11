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

	/// <summary>Extracts the sole completed Codex assistant response; rejects incomplete or tool-bearing streams before domain parsing.</summary>
	inline std::optional<std::string> CompletedTextWorkerOutput(std::string_view provider_id, std::string_view output, std::size_t max_output_bytes)
	{
		if (output.size() > max_output_bytes) return std::nullopt;
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

	inline bool ApplyWorkerIsolationPolicy(const ProviderProfile& profile, const std::filesystem::path& workspace, std::vector<std::string>& argv)
	{
		if (IsProvider(profile, uam::provider_ids::kOpenCodeCli))
		{
			if (!uam::io::WriteTextFile(workspace / "opencode.json", R"({"permission":{"*":"deny","external_directory":"deny"},"share":"disabled","autoupdate":false})"))
			{
				return false;
			}
			if (argv.size() < 2) return false;
			argv.insert(argv.begin() + 2, "--pure");
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
