#include "computer_use/computer_use_mcp_config.h"

#include "common/platform/platform_services.h"
#include "common/provider/provider_ids.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>

namespace uam::computer_use
{
	namespace
	{
		constexpr const char* kGeminiPolicyFilename = "gemini-policy.toml";

		std::string ExecutablePath()
		{
			const std::filesystem::path executable = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
#if defined(__APPLE__)
			const std::filesystem::path companion = executable.parent_path().parent_path() / "Frameworks" / "UAM Computer Use.app" / "Contents" / "MacOS" / "UAM Computer Use";
			if (std::filesystem::exists(companion))
				return companion.string();
#endif
			return executable.string();
		}

		std::filesystem::path GeminiPolicyPath()
		{
			const std::filesystem::path executable = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
#if defined(__APPLE__)
			if (executable.parent_path().filename() == "MacOS")
				return executable.parent_path().parent_path() / "Resources" / kGeminiPolicyFilename;
#endif
			return executable.parent_path() / kGeminiPolicyFilename;
		}

		nlohmann::json ServerDefinition(const ChatSession& chat)
		{
			return {
			    {"command", ExecutablePath()},
			    {"args", McpServerArguments(chat)},
			};
		}
	} // namespace

	std::string BackendPreference(std::string_view value)
	{
		if (value == kBackendProvider)
			return kBackendProvider;
		if (value == kBackendUam)
			return kBackendUam;
		return kBackendAuto;
	}

	bool ProviderBackendAvailable(std::string_view provider_id)
	{
		// Claude's built-in computer-use server requires an interactive session. UAM's
		// structured Claude runtime uses `claude -p`, so it must use the UAM backend.
		return uam::provider_ids::IsCliProviderAliasOf(provider_id, uam::provider_ids::kCodexCli);
	}

	bool AvailableForChat(const ChatSession& chat)
	{
		return chat.execution_host_id.empty() || chat.execution_host_id == "local";
	}

	std::string EffectiveBackend(const ChatSession& chat)
	{
		const std::string preference = BackendPreference(chat.computer_use_backend);
		// Auto stays on UAM until a provider exposes a portable structured-runtime
		// capability contract. Codex can still be selected explicitly as a preview.
		return preference == kBackendProvider && ProviderBackendAvailable(chat.provider_id) ? kBackendProvider : kBackendUam;
	}

	bool UsesUamBackend(const ChatSession& chat)
	{
		return EffectiveBackend(chat) == kBackendUam;
	}

	bool IsPortableMcpChatId(std::string_view value)
	{
		if (value.empty() || value.size() > 128 ||
		    !std::ranges::all_of(value, [](const unsigned char ch)
		        { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'; }))
			return false;
		const std::string lower = uam::strings::ToLowerAscii(value);
		return lower != "con" && lower != "prn" && lower != "aux" &&
		    lower != "nul" &&
		    !(lower.size() == 4 && (lower.starts_with("com") ||
		                              lower.starts_with("lpt")) &&
		        lower[3] >= '1' && lower[3] <= '9');
	}

	std::vector<std::string> McpServerArguments(const ChatSession& chat)
	{
		return {kMcpServerFlag, "--chat-id", chat.id, "--target-id", chat.computer_use_target_id, "--target-pid", chat.computer_use_target_process_id, "--target-kind", chat.computer_use_target_kind == "screen" ? "screen" : "window"};
	}

	nlohmann::json AcpMcpServers(const ChatSession& chat)
	{
		if (!AvailableForChat(chat) || !UsesUamBackend(chat) || !IsPortableMcpChatId(chat.id))
		{
			return nlohmann::json::array();
		}

		nlohmann::json server = ServerDefinition(chat);
		server["name"] = kMcpServerName;
		server["env"] = nlohmann::json::array();
		return nlohmann::json::array({std::move(server)});
	}

	std::string ClaudeMcpConfig(const ChatSession& chat)
	{
		return nlohmann::json({{"mcpServers", {{kMcpServerName, ServerDefinition(chat)}}}}).dump();
	}

	void AppendCodexMcpLaunchArguments(std::vector<std::string>& argv, const ChatSession& chat)
	{
		if (!AvailableForChat(chat))
		{
			argv.push_back("--disable");
			argv.push_back("computer_use");
			return;
		}
		if (!UsesUamBackend(chat))
		{
			argv.push_back(chat.computer_use_enabled ? "--enable" : "--disable");
			argv.push_back("computer_use");
			return;
		}

		// Never expose two desktop controllers in the same provider process.
		argv.push_back("--disable");
		argv.push_back("computer_use");
		if (!IsPortableMcpChatId(chat.id))
		{
			return;
		}

		const nlohmann::json server = ServerDefinition(chat);
		for (const std::string& setting : {
		         std::string("mcp_servers.") + kMcpServerName + ".command=" + server["command"].dump(),
		         std::string("mcp_servers.") + kMcpServerName + ".args=" + server["args"].dump(),
		         std::string("mcp_servers.") + kMcpServerName + ".required=true",
		         std::string("mcp_servers.") + kMcpServerName + ".tool_timeout_sec=300",
		         std::string("mcp_servers.") + kMcpServerName + ".default_tools_approval_mode=\"approve\"",
		     })
		{
			argv.push_back("-c");
			argv.push_back(setting);
		}
	}

	void AppendClaudeMcpLaunchArguments(std::vector<std::string>& argv, const ChatSession& chat)
	{
		if (AvailableForChat(chat) && UsesUamBackend(chat) && IsPortableMcpChatId(chat.id))
		{
			argv.push_back("--mcp-config");
			argv.push_back(ClaudeMcpConfig(chat));
			argv.push_back("--allowedTools");
			argv.push_back("mcp__uam-computer__computer_observe,mcp__uam-computer__computer_action");
		}
	}

	void AppendGeminiMcpLaunchArguments(std::vector<std::string>& argv, const ChatSession& chat)
	{
		if (AvailableForChat(chat) && UsesUamBackend(chat) && IsPortableMcpChatId(chat.id))
		{
			argv.push_back("--policy");
			argv.push_back(GeminiPolicyPath().string());
		}
	}
} // namespace uam::computer_use
