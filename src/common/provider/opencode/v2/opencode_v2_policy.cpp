#include "common/provider/opencode_v2_session.h"
#include "common/runtime/cli_version_parser.h"
#include <stdexcept>

namespace uam::opencode_v2
{
std::string WorkerVersionError(bool checked, bool same_host, std::string_view version, std::string_view check_error)
{
	if (!checked || !same_host || version.empty() || !check_error.empty())
		return "OpenCode text worker is held until the executing local host's version check succeeds. Check OpenCode in Settings, then retry; no V1 fallback was launched.";
	if (uam::cli_versions::ExtractSemverVersion(version) != std::optional<std::string>{version} ||
	    (!version.starts_with("1.") && version != "2.1.2"))
		return "This OpenCode text-worker protocol has not been verified.";
	return {};
}

std::vector<std::string> ServeArgv(std::vector<std::string> command)
{
	if (command.empty()) throw std::runtime_error("Missing OpenCode executable.");
	command.insert(command.end(), {"serve", "--stdio", "--hostname", "127.0.0.1", "--port", "0"});
	return command;
}
std::vector<std::string> HistoryArgv(std::string_view version, std::vector<std::string> command,
	const std::string& operation, const std::string& id)
{
	const bool v2 = version == "2.1.2";
	if (command.empty() || (!v2 && !version.starts_with("1."))) throw std::runtime_error("This OpenCode history protocol has not been verified.");
	if (operation == "list")
	{
		command.insert(command.end(), {"session", "list", "--format", "json", v2 ? "--standalone" : "--pure"});
		if (v2) command.insert(command.end(), {"--max-count", "100001"});
	}
	else if (operation == "export" && !id.empty() && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos)
	{
		if (v2) command.push_back("session");
		command.insert(command.end(), {"export", id, v2 ? "--standalone" : "--pure"});
	}
	else throw std::runtime_error("Invalid OpenCode history request.");
	return command;
}

std::string PermissionPluginSource()
{
	// Original UAM implementation of OpenCode's public Promise plugin API. See
	// docs/reference/opencode-v2-protocol-assessment.md for pinned API evidence.
	return R"UAMV2(import { readFileSync, writeFileSync } from 'node:fs';
export default {
  id: 'uam.v2.permission-boundary',
  async setup(ctx) {
    const { policyPath, proofPath, nonce } = ctx.options;
    if (typeof policyPath !== 'string' || typeof proofPath !== 'string' || typeof nonce !== 'string')
      throw new Error('UAM V2 permission lease is missing');
    await ctx.permission.hook('evaluate', async event => {
      let effect = 'deny';
      try {
        const raw = readFileSync(policyPath, 'utf8');
        if (raw.length > 16384) throw new Error('Oversized UAM policy');
        const policy = JSON.parse(raw);
        // Native children are denied until an independently verified child
        // admission contract exists. A saved native ID is never authority.
        if (policy.nonce === nonce && policy.active === true &&
            policy.sessionID === event.sessionID && policy.directory === ctx.location.directory &&
            typeof policy.readOnly === 'boolean') {
          effect = event.effect === 'deny' ? 'deny' : 'ask';
          if (['task', 'subagent'].includes(event.action) || (policy.readOnly && event.action === 'edit')) effect = 'deny';
        }
      } catch { effect = 'deny'; }
      // OpenCode invokes hooks after composing saved/project/session grants.
      // Later hooks may narrow this decision, but cannot widen it to allow.
      // Failure to install the latch fails the native evaluation, not open.
      Object.defineProperty(event, 'effect', {
        enumerable: true, configurable: false,
        get: () => effect,
        set: value => { if (value === 'deny') effect = 'deny'; }
      });
    });
    writeFileSync(proofPath, JSON.stringify({ nonce, directory: ctx.location.directory }), { mode: 0o600 });
  }
};
)UAMV2";
}

Json NormalizeExport(const Json& native)
{
	const Json root = native.contains("data") ? native.at("data") : native;
	if (!root.is_object() || !root.contains("info") || !root.contains("messages") || !root.at("messages").is_array()) throw std::runtime_error("Invalid OpenCode V2 export.");
	Json info = root.at("info");
	info["directory"] = info.at("location").at("directory");
	Json messages = Json::array();
	for (const auto& entry : root.at("messages"))
	{
		const auto type = entry.at("type").get<std::string>();
		Json item = entry;
		Json parts = Json::array();
		item["role"] = type;
		if (type == "user" || type == "system" || type == "synthetic")
		{
			if (type == "synthetic") item["role"] = "system";
			parts.push_back({{"type", "text"}, {"text", entry.at("text")}});
			for (const auto& file : entry.value("files", Json::array())) parts.push_back(file);
		}
		else if (type == "assistant")
		{
			item["providerID"] = entry.at("model").at("providerID");
			item["modelID"] = entry.at("model").at("id");
			for (const auto& content : entry.at("content"))
			{
				Json part = content;
				if (content.at("type") == "tool")
				{
					part["callID"] = ToolKey(entry.at("id").get<std::string>(), content.at("id").get<std::string>()); part["tool"] = content.at("name");
					Json state = content.at("state");
					if (state.value("status", "") == "streaming") state["status"] = "pending";
					if (state.value("status", "") == "error")
					{
						// V2 errors are structured; the shared V1 history reader consumes text.
						// Keep the native details in the normalized export as inert data.
						state["errorDetails"] = state.at("error");
						state["error"] = state.at("errorDetails").at("message").get<std::string>();
					}
					if (state.contains("content"))
					{
						std::string text;
						bool only_text = state.at("content").is_array();
						for (const auto& value : state.at("content"))
						{
							if (!value.is_object() || value.value("type", "") != "text" || !value.contains("text") || !value.at("text").is_string()) { only_text = false; break; }
							if (!text.empty()) text += '\n';
							text += value.at("text").get<std::string>();
						}
						state["output"] = only_text ? text : state.at("content").dump();
					}
					part["state"] = state;
				}
				parts.push_back(part);
			}
			if (entry.value("error", Json::object()).value("type", "") == "aborted") item["error"] = {{"name", "MessageAbortedError"}};
		}
		else if (type == "compaction")
		{
			// Retain visible summaries as DATA, never invent V1 coverage metadata.
			// UAM's V1 compaction metadata allowlist does not admit V2.
			if (entry.value("status", "") != "completed") continue;
			item["role"] = "system";
			item.erase("summary");
			parts.push_back({{"type", "text"}, {"text", entry.value("summary", "")}});
		}
		else if (type == "idle")
		{
			const auto outcome = entry.at("outcome").get<std::string>();
			if (outcome != "succeeded" && outcome != "failed" && outcome != "interrupted")
				throw std::runtime_error("OpenCode V2 export contains an unverified idle outcome; history was not replaced.");
			// A native terminal marker is history data, not a new message or proof
			// that any live UAM input was consumed, cancelled or safe to replay.
			continue;
		}
		else if (type == "agent-switched" || type == "model-switched" || type == "location-switched" || type == "skill") continue;
		else throw std::runtime_error("OpenCode V2 export contains an unsupported message type; history was not replaced.");
		messages.push_back({{"info", item}, {"parts", parts}});
	}
	return {{"info", info}, {"messages", messages}};
}
}
