#pragma once

#include <nlohmann/json.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace uam
{
	struct AppState;

	/// <summary>One automation HTTP request after transport; the raw bearer token has already been reduced to its hash.</summary>
	struct AutomationApiRequest
	{
		std::string method;
		std::string path;
		std::string token_hash;
		std::string body;
	};

	struct AutomationApiResponse
	{
		int status = 200;
		nlohmann::json body = nlohmann::json::object();
		/// Set when a trusted token's chat was created and the caller must start it.
		std::string launch_chat_id;
	};

	/// <summary>The prepared first turn of an automation chat, returned so a refused send can be undone.</summary>
	struct AutomationChatLaunch
	{
		std::string text;
		std::string goal_id;
	};

	/// <summary>The credential an automation schedule runs under. It is the schedule itself: chosen in desktop Settings,
	/// kept only inside UAM and never expiring, unlike external tokens.</summary>
	struct AutomationScheduleCredential
	{
		std::string id;
		std::string name;
		bool trusted = false;
	};

	/// <summary>Token-scoped automation API. Tokens live only as hashes in one file, read per request, so revocation needs no cache invalidation.</summary>
	class AutomationApiService
	{
	  public:
		static constexpr std::size_t kMaxBodyBytes = 256 * 1024;

		static bool IsApiPath(std::string_view path);
		/// <summary>Hashes a well-formed "Bearer uamat_..." header; malformed headers return nothing.</summary>
		static std::optional<std::string> TokenHashFromAuthorization(std::string_view header);
		static bool ConstantTimeEquals(std::string_view left, std::string_view right);
		/// <summary>Authorizes and serves one request. Every refusal returns before any chat is created.</summary>
		static AutomationApiResponse Handle(AppState& app, const AutomationApiRequest& request);

		/// <summary>Checks a schedule's stored chat request without creating anything; empty when it is valid. One line for Settings.</summary>
		static std::string ScheduledRequestRefusal(const AppState& app, const nlohmann::json& body);
		/// <summary>Creates one scheduled run's chat through the same checks as POST /automation/chats. A trusted
		/// schedule's response carries launch_chat_id for the caller to start; every refusal creates nothing.</summary>
		static AutomationApiResponse CreateScheduledChat(AppState& app, const AutomationScheduleCredential& credential, const nlohmann::json& body);

		/// <summary>Token metadata for Settings; never contains a secret or its hash.</summary>
		static std::optional<nlohmann::json> ListTokens(const AppState& app, std::string* error_out);
		/// <summary>Creates a token. The result's "secret" is the only time the plaintext exists outside the caller.</summary>
		static std::optional<nlohmann::json> CreateToken(const AppState& app, const nlohmann::json& spec, std::string* error_out);
		static bool RevokeToken(const AppState& app, std::string_view token_id, std::string* error_out);
		/// <summary>Replaces one token's secret, keeping its settings and restarting its expiry. The old secret stops working at once.</summary>
		static std::optional<nlohmann::json> RegenerateToken(const AppState& app, std::string_view token_id, std::string* error_out);

		/// <summary>Creates the capped goal and releases the approval hold. Only an awaiting chat can launch.</summary>
		static std::optional<AutomationChatLaunch> BeginLaunch(AppState& app, const std::string& chat_id, std::string* error_out);
		/// <summary>Restores the approval hold after a refused send, so the chat waits instead of being lost. Returns false when the prompt may already have been delivered.</summary>
		static bool AbandonLaunch(AppState& app, const std::string& chat_id, const AutomationChatLaunch& launch);
	};
} // namespace uam
