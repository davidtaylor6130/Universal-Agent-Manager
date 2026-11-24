#include "app/automation_api_service.h"

#include "app/chat_domain_service.h"
#include "app/goal_service.h"
#include "app/provider_resolution_service.h"
#include "app/uam_control_service.h"
#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
#include "common/config/execution_host_config.h"
#include "common/config/provider_chat_defaults.h"
#include "common/platform/platform_incremental_sha256.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/state/app_state.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <random>
#include <ranges>
#include <sstream>
#include <vector>

namespace uam
{
	namespace
	{
		constexpr std::string_view kTokenPrefix = "uamat_";
		constexpr std::string_view kBearerPrefix = "Bearer ";
		constexpr std::string_view kChatsPath = "/automation/chats";
		constexpr std::size_t kSecretHexChars = 64;
		constexpr std::size_t kMaxTokens = 32;
		constexpr std::size_t kMaxTokenFileBytes = 256 * 1024;
		constexpr std::size_t kMaxPromptBytes = 128 * 1024;
		constexpr std::size_t kMaxFieldBytes = 256;
		constexpr std::size_t kMaxTokenNameChars = 80;
		constexpr std::size_t kMaxOpenChatsPerToken = 16;
		constexpr int kMaxIterations = 200;
		constexpr int kDefaultLifetimeDays = 90;
		constexpr int kMaxLifetimeDays = 365;
		constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;

		struct TokenRecord
		{
			std::string id;
			std::string name;
			std::string hash;
			bool trusted = false;
			std::vector<std::string> workspaces;
			std::string max_git_permission = "commit";
			std::string created_at;
			int lifetime_days = kDefaultLifetimeDays;
			std::int64_t expires_at = 0; // Epoch seconds; checked on every request.
			/// What refusals call this credential: an external "token" or an internal "schedule".
			std::string kind = "token";
		};

		std::filesystem::path TokenFile(const AppState& app)
		{
			return app.data_root / "automation_tokens.json";
		}

		bool WriteTokens(const AppState& app, const std::vector<TokenRecord>& tokens);

		/// <summary>Null means the store exists but cannot be trusted; callers refuse rather than treat it as empty.</summary>
		std::optional<std::vector<TokenRecord>> ReadTokens(const AppState& app)
		{
			std::error_code error;
			const std::filesystem::path path = TokenFile(app);
			if (!std::filesystem::exists(path, error)) return error ? std::nullopt : std::make_optional(std::vector<TokenRecord>{});
			const auto size = std::filesystem::file_size(path, error);
			std::string text;
			if (error || size > kMaxTokenFileBytes || !uam::io::TryReadTextFile(path, text)) return std::nullopt;
			const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
			if (!root.is_object() || !root.contains("tokens") || !root["tokens"].is_array()) return std::nullopt;
			std::vector<TokenRecord> tokens;
			bool migrated = false;
			try
			{
				for (const nlohmann::json& value : root["tokens"])
				{
					TokenRecord record;
					record.id = value.at("id").get<std::string>();
					record.name = value.at("name").get<std::string>();
					record.hash = value.at("hash").get<std::string>();
					record.trusted = value.at("trusted").get<bool>();
					record.workspaces = value.at("workspaces").get<std::vector<std::string>>();
					record.max_git_permission = value.at("maxGitPermission").get<std::string>();
					record.created_at = value.value("createdAt", "");
					record.lifetime_days = value.value("lifetimeDays", kDefaultLifetimeDays);
					if (record.lifetime_days < 1 || record.lifetime_days > kMaxLifetimeDays) return std::nullopt;
					// Tokens from before expiry existed get their full lifetime from the first read after upgrade.
					if (value.contains("expiresAt")) record.expires_at = value.at("expiresAt").get<std::int64_t>();
					else
					{
						record.expires_at = uam::time::TimestampNowSec() + record.lifetime_days * kSecondsPerDay;
						migrated = true;
					}
					tokens.push_back(std::move(record));
				}
			}
			catch (const nlohmann::json::exception&)
			{
				return std::nullopt;
			}
			if (migrated && !WriteTokens(app, tokens)) uam::diagnostics::Write("Automation API: token expiry could not be saved; it is applied in memory and retried on the next read.");
			return tokens;
		}

		bool WriteTokens(const AppState& app, const std::vector<TokenRecord>& tokens)
		{
			nlohmann::json list = nlohmann::json::array();
			for (const TokenRecord& token : tokens)
				list.push_back({{"id", token.id}, {"name", token.name}, {"hash", token.hash}, {"trusted", token.trusted},
				                {"workspaces", token.workspaces}, {"maxGitPermission", token.max_git_permission}, {"createdAt", token.created_at},
				                {"lifetimeDays", token.lifetime_days}, {"expiresAt", token.expires_at}});
			const std::filesystem::path path = TokenFile(app);
			if (!uam::io::WriteTextFile(path, nlohmann::json{{"version", 1}, {"tokens", std::move(list)}}.dump(1, '\t'))) return false;
			std::error_code error;
			std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
			return true;
		}

		std::string RandomHex(int words)
		{
			// random_device is the OS CSPRNG on the supported toolchains (arc4random, rand_s, getrandom).
			std::random_device random;
			std::ostringstream out;
			for (int index = 0; index < words; ++index) out << std::hex << std::setw(8) << std::setfill('0') << static_cast<std::uint32_t>(random());
			return out.str();
		}

		std::optional<std::string> Sha256(std::string_view value)
		{
			platform::IncrementalSha256 hash;
			if (!hash.Add(value)) return std::nullopt;
			std::string digest = hash.Finish();
			return digest.empty() ? std::nullopt : std::make_optional(std::move(digest));
		}

		int GitRank(std::string_view permission)
		{
			return permission == "pr" ? 2 : permission == "commit" ? 1 : 0;
		}

		nlohmann::json TokenMetadata(const TokenRecord& token)
		{
			return {{"id", token.id}, {"name", token.name}, {"trusted", token.trusted}, {"workspaces", token.workspaces}, {"maxGitPermission", token.max_git_permission},
			    {"createdAt", token.created_at}, {"lifetimeDays", token.lifetime_days}, {"expiresAt", token.expires_at}};
		}

		/// <summary>A fresh secret replaces any earlier one; the returned plaintext is the only copy.</summary>
		std::optional<std::string> IssueSecret(TokenRecord& record)
		{
			const std::string secret = std::string(kTokenPrefix) + RandomHex(8);
			const std::optional<std::string> hash = Sha256(secret);
			if (!hash) return std::nullopt;
			record.hash = *hash;
			record.expires_at = uam::time::TimestampNowSec() + record.lifetime_days * kSecondsPerDay;
			return secret;
		}

		AutomationApiResponse Refuse(int status, std::string message)
		{
			return {.status = status, .body = {{"error", std::move(message)}}};
		}

		void LogRequest(const AutomationApiRequest& request, const TokenRecord* token, int status, std::string_view chat_id = {})
		{
			// Method, route and outcome only: never the token, its hash, the prompt or the body.
			uam::diagnostics::Write("Automation API: " + uam::strings::SafeLine(request.method, 8, true) + " " +
			                        (request.path == kChatsPath ? "chats" : "chat-status") + " token=" + (token != nullptr ? token->id : std::string("unknown")) +
			                        " status=" + std::to_string(status) + (chat_id.empty() ? "" : " chat=" + std::string(chat_id)) + ".");
		}

		/// <summary>Reads an optional bounded string field. An incorrect type is an error, not a default.</summary>
		bool ReadField(const nlohmann::json& body, const char* key, std::size_t limit, std::string& out)
		{
			if (!body.contains(key)) return true;
			if (!body[key].is_string()) return false;
			out = uam::strings::Trim(body[key].get<std::string>());
			return out.size() <= limit && out.find('\0') == std::string::npos;
		}

		/// <summary>A send can be accepted and then fail while the worktree is prepared, leaving the goal Active with nothing running.</summary>
		bool LaunchFailedAfterAcceptance(const AppState& app, const ChatSession& chat)
		{
			const AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
			return chat.acp_queue_held || (session != nullptr && session->lifecycle_state == "error");
		}

		/// <summary>Counts against the token's open-chat limit.</summary>
		bool ChatIsOpen(const AppState& app, const ChatSession& chat)
		{
			if (chat.AwaitsAutomationApproval()) return true;
			// A chat an automation chat started holds its place for as long as it exists, like the 8-per-caller
			// allowance, because later wakes such as a pull request watch do not check the token.
			if (!chat.created_by_chat_id.empty()) return true;
			const AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
			if (ChatHasBusyCliTerminal(app, chat) || (session != nullptr && (session->processing || AcpSessionIsWaitingForInput(*session)))) return true;
			// A launch that failed after acceptance keeps its goal and queued prompt but waits for the user, not the token.
			if (LaunchFailedAfterAcceptance(app, chat)) return false;
			// A branch has no goal of its own, so a turn waiting to be sent also counts.
			return GoalService::FindActiveGoal(app, chat.id) != nullptr || (session != nullptr && AcpSessionHasActiveTurn(*session));
		}

		std::size_t OpenChatCount(const AppState& app, std::string_view token_id)
		{
			return static_cast<std::size_t>(std::ranges::count_if(app.chats, [&](const ChatSession& chat) { return chat.automation_token_id == token_id && ChatIsOpen(app, chat); }));
		}

		/// <summary>A chat started directly through the API, as opposed to one an automation chat started or branched; only those report status.</summary>
		bool CreatedThroughApi(const ChatSession& chat)
		{
			return chat.created_by_chat_id.empty() && chat.parent_chat_id.empty();
		}

		/// <summary>The fields of a chat request after the same validation every caller gets.</summary>
		struct ChatRequest
		{
			std::string prompt, workspace_id, provider, model, title, git_permission;
			int max_iterations = 0;
		};

		/// <summary>Reads and bounds a chat request body. Returns the refusal when a field is missing or invalid.</summary>
		std::optional<AutomationApiResponse> ReadChatRequest(const nlohmann::json& body, ChatRequest& out)
		{
			if (!body.is_object()) return Refuse(400, "The request body must be a JSON object.");
			if (!ReadField(body, "prompt", kMaxPromptBytes, out.prompt) || out.prompt.empty()) return Refuse(400, "prompt is required and must be between 1 byte and 128 KiB.");
			if (!ReadField(body, "workspaceId", kMaxFieldBytes, out.workspace_id) || out.workspace_id.empty()) return Refuse(400, "workspaceId is required.");
			if (!ReadField(body, "providerId", kMaxFieldBytes, out.provider) || out.provider.empty()) return Refuse(400, "providerId is required.");
			// A model ID reaches a provider command line, so it may not look like a flag.
			if (!ReadField(body, "modelId", kMaxFieldBytes, out.model) || out.model.starts_with('-') ||
			    !std::ranges::all_of(out.model, [](unsigned char ch) { return std::isalnum(ch) != 0 || std::string_view("-_./:[]").find(static_cast<char>(ch)) != std::string_view::npos; }))
				return Refuse(400, "modelId must be a plain model name.");
			if (!ReadField(body, "title", kMaxFieldBytes, out.title)) return Refuse(400, "title must be a short string.");
			const auto cap = body.find("maxIterations");
			if (cap == body.end() || !cap->is_number_integer() || cap->get<std::int64_t>() < 1 || cap->get<std::int64_t>() > kMaxIterations)
				return Refuse(400, "maxIterations is required and must be an integer from 1 to 200.");
			out.max_iterations = cap->get<int>();
			if (!ReadField(body, "gitPermission", kMaxFieldBytes, out.git_permission) || GitRank(out.git_permission) == 0)
				return Refuse(400, "gitPermission is required and must be \"commit\" or \"pr\".");
			return std::nullopt;
		}

		AutomationApiResponse CreateChat(AppState& app, const nlohmann::json& body, const TokenRecord& token)
		{
			ChatRequest request;
			if (std::optional<AutomationApiResponse> refusal = ReadChatRequest(body, request)) return *refusal;
			const std::string& prompt = request.prompt;
			const std::string& workspace_id = request.workspace_id;
			const std::string& provider = request.provider;
			const std::string& model = request.model;
			const std::string& title = request.title;
			const std::string& git_permission = request.git_permission;

			if (std::ranges::find(token.workspaces, workspace_id) == token.workspaces.end()) return Refuse(403, "This token may not use that workspace.");
			if (GitRank(git_permission) > GitRank(token.max_git_permission)) return Refuse(403, "This token may not request that Git permission.");
			const ChatFolder* folder = ChatDomainService().FindFolderById(app, workspace_id);
			if (folder == nullptr || uam::strings::IsBlank(folder->directory)) return Refuse(404, "That workspace no longer exists.");
			if (OpenChatCount(app, token.id) >= kMaxOpenChatsPerToken)
				return Refuse(429, "This " + token.kind + " already has 16 open chats. Finish or delete one first; a chat an automation chat started counts until it is deleted.");

			ChatSession chat = ChatDomainService().CreateNewChat(folder->id, provider_ids::NormalizeCliProviderAliasOrSelf(provider), true);
			chat.execution_host_id = uam::strings::NonEmptyOrFallback(folder->execution_host_id, execution_hosts::kLocalHostId);
			chat.workspace_directory = folder->directory;
			if (std::string refusal = UamControlService::ChatWorktreeIsolationRefusal(app, chat); !refusal.empty()) return Refuse(409, std::move(refusal));
			if (!ProviderResolutionService().ChatProviderIsAvailable(app, chat)) return Refuse(409, ProviderResolutionService().ChatProviderUnavailableReason(app, chat));
			// Default permissions are the only thing that stops a push, and this provider answers its own prompts.
			if (uam::provider_chat_defaults::ProviderManagesOwnPermissions(chat.provider_id)) return Refuse(409, "That provider answers its own permission prompts, so UAM cannot hold it to a Git permission.");
			chat.svn_update_pending = chat.execution_host_id == execution_hosts::kLocalHostId && app.settings.svn_enabled && app.settings.svn_update_before_chat;
			if (!title.empty()) chat.title = uam::strings::SafeLine(title, 120, true);
			uam::provider_chat_defaults::ApplyToChat(app.settings, chat);
			if (!model.empty()) chat.model_id = model;
			// A remote caller never inherits a permissive Settings default; every command and edit asks.
			chat.approval_mode = uam::approval_modes::kDefaultApprovalMode;
			chat.command_safety_tier = "off";
			chat.automation_token_id = token.id;
			chat.automation_token_name = token.name;
			chat.automation_pending_prompt = prompt;
			chat.automation_max_iterations = request.max_iterations;
			chat.automation_git_permission = git_permission;
			if (!ChatRepository::SaveChat(app.data_root, chat)) return Refuse(500, "The chat could not be saved.");

			const std::string chat_id = chat.id;
			app.chats.push_back(std::move(chat));
			if (!token.trusted) ChatDomainService().MarkChatNeedsAttention(app, chat_id);
			AutomationApiResponse response{.body = {{"chatId", chat_id}, {"status", token.trusted ? "starting" : "awaitingApproval"}}};
			if (token.trusted) response.launch_chat_id = chat_id;
			return response;
		}

		AutomationApiResponse ChatStatus(const AppState& app, std::string_view chat_id, const TokenRecord& token)
		{
			const auto chat = std::ranges::find_if(app.chats, [&](const ChatSession& value) { return value.id == chat_id; });
			// A chat from another token is indistinguishable from a missing one.
			// So is a chat an automation chat started or branched: the token's read scope stays the chats it created.
			if (chat == app.chats.end() || chat->automation_token_id != token.id || !CreatedThroughApi(*chat)) return Refuse(404, "Chat not found.");
			std::string status = "awaitingApproval";
			int used = 0;
			if (!chat->AwaitsAutomationApproval())
			{
				const Goal* goal = chat->goals.empty() ? nullptr : &chat->goals.back();
				status = goal == nullptr ? "stopped" : goal->status == GoalStatus::Active ? (LaunchFailedAfterAcceptance(app, *chat) ? "blocked" : "running") : goal->status == GoalStatus::Complete ? "completed" : goal->status == GoalStatus::Blocked ? "blocked" : "paused";
				if (goal != nullptr) used = goal->loop_count;
			}
			return {.body = {{"chatId", chat->id}, {"status", status}, {"iterationsUsed", used}, {"maxIterations", chat->automation_max_iterations}, {"gitPermission", chat->automation_git_permission}}};
		}
	}

	bool AutomationApiService::IsApiPath(std::string_view path)
	{
		return path == kChatsPath || (path.starts_with(kChatsPath) && path.size() > kChatsPath.size() && path[kChatsPath.size()] == '/');
	}

	bool AutomationApiService::ConstantTimeEquals(std::string_view left, std::string_view right)
	{
		if (left.size() != right.size()) return false;
		unsigned char difference = 0;
		for (std::size_t index = 0; index < left.size(); ++index) difference |= static_cast<unsigned char>(left[index] ^ right[index]);
		return difference == 0;
	}

	std::optional<std::string> AutomationApiService::TokenHashFromAuthorization(std::string_view header)
	{
		if (!header.starts_with(kBearerPrefix)) return std::nullopt;
		const std::string_view token = header.substr(kBearerPrefix.size());
		if (token.size() != kTokenPrefix.size() + kSecretHexChars || !token.starts_with(kTokenPrefix)) return std::nullopt;
		if (!std::ranges::all_of(token.substr(kTokenPrefix.size()), [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); })) return std::nullopt;
		return Sha256(token);
	}

	AutomationApiResponse AutomationApiService::Handle(AppState& app, const AutomationApiRequest& request)
	{
		const std::optional<std::vector<TokenRecord>> tokens = ReadTokens(app);
		if (!tokens)
		{
			uam::diagnostics::Write("Automation API: the token store could not be read; request refused.");
			return Refuse(503, "Automation tokens are unavailable.");
		}
		const TokenRecord* token = nullptr;
		// Every record is compared, so timing does not reveal the matching position.
		for (const TokenRecord& candidate : *tokens)
			if (ConstantTimeEquals(candidate.hash, request.token_hash)) token = &candidate;
		AutomationApiResponse response;
		if (token == nullptr || request.token_hash.empty()) response = Refuse(401, "Invalid automation token.");
		else if (uam::time::TimestampNowSec() >= token->expires_at) response = Refuse(401, "This automation token has expired. Regenerate it in UAM Settings.");
		else if (request.body.size() > kMaxBodyBytes) response = Refuse(413, "Request exceeds 256 KiB.");
		else if (request.method == "POST" && request.path == kChatsPath)
		{
			const nlohmann::json body = nlohmann::json::parse(request.body, nullptr, false);
			response = CreateChat(app, body, *token);
		}
		else if (request.method == "GET" && IsApiPath(request.path) && request.path != kChatsPath) response = ChatStatus(app, std::string_view(request.path).substr(kChatsPath.size() + 1), *token);
		else response = Refuse(404, "Unknown automation endpoint.");
		LogRequest(request, response.status == 401 ? nullptr : token, response.status, response.body.value("chatId", std::string{}));
		return response;
	}

	std::string AutomationApiService::ScheduledRequestRefusal(const AppState& app, const nlohmann::json& body)
	{
		ChatRequest request;
		if (std::optional<AutomationApiResponse> refusal = ReadChatRequest(body, request)) return refusal->body.value("error", "The request is not valid.");
		const ChatFolder* folder = ChatDomainService().FindFolderById(app, request.workspace_id);
		if (folder == nullptr || uam::strings::IsBlank(folder->directory)) return "That workspace no longer exists.";
		if (uam::provider_chat_defaults::ProviderManagesOwnPermissions(provider_ids::NormalizeCliProviderAliasOrSelf(request.provider)))
			return "That provider answers its own permission prompts, so UAM cannot hold it to a Git permission.";
		// The run's own checks, made now too, so a schedule that could never start is not saved.
		ChatSession probe = ChatDomainService().CreateNewChat(folder->id, provider_ids::NormalizeCliProviderAliasOrSelf(request.provider), true);
		probe.execution_host_id = uam::strings::NonEmptyOrFallback(folder->execution_host_id, execution_hosts::kLocalHostId);
		probe.workspace_directory = folder->directory;
		probe.automation_token_id = "schedule-check";
		if (std::string refusal = UamControlService::ChatWorktreeIsolationRefusal(app, probe); !refusal.empty()) return refusal;
		if (!ProviderResolutionService().ChatProviderIsAvailable(app, probe)) return ProviderResolutionService().ChatProviderUnavailableReason(app, probe);
		return {};
	}

	AutomationApiResponse AutomationApiService::CreateScheduledChat(AppState& app, const AutomationScheduleCredential& credential, const nlohmann::json& body)
	{
		// The schedule is its own credential: the user chose its workspace, Git access and trust in desktop Settings, it
		// never leaves UAM and it does not expire. Every other check is the one POST /automation/chats gets.
		TokenRecord token;
		token.id = credential.id;
		token.name = credential.name;
		token.trusted = credential.trusted;
		token.kind = "schedule";
		if (body.is_object() && body.contains("workspaceId") && body["workspaceId"].is_string()) token.workspaces = {body["workspaceId"].get<std::string>()};
		if (body.is_object() && body.contains("gitPermission") && body["gitPermission"].is_string()) token.max_git_permission = body["gitPermission"].get<std::string>();
		AutomationApiResponse response = CreateChat(app, body, token);
		// Status, route and chat only: never the prompt.
		uam::diagnostics::Write("Automation schedule: " + credential.id + " run status=" + std::to_string(response.status) +
		                        (response.body.contains("chatId") ? " chat=" + response.body.value("chatId", std::string{}) : std::string{}) + ".");
		return response;
	}

	std::optional<nlohmann::json> AutomationApiService::ListTokens(const AppState& app, std::string* error_out)
	{
		const auto tokens = ReadTokens(app);
		if (!tokens)
		{
			if (error_out != nullptr) *error_out = "The automation token file could not be read.";
			return std::nullopt;
		}
		nlohmann::json list = nlohmann::json::array();
		for (const TokenRecord& token : *tokens) list.push_back(TokenMetadata(token));
		return list;
	}

	std::optional<nlohmann::json> AutomationApiService::CreateToken(const AppState& app, const nlohmann::json& spec, std::string* error_out)
	{
		const auto fail = [&](std::string message) { if (error_out != nullptr) *error_out = std::move(message); return std::nullopt; };
		std::optional<std::vector<TokenRecord>> tokens = ReadTokens(app);
		if (!tokens) return fail("The automation token file could not be read, so no token was created.");
		if (tokens->size() >= kMaxTokens) return fail("At most 32 automation tokens can exist. Revoke one first.");
		TokenRecord record;
		record.name = uam::strings::SafeLine(uam::strings::Trim(spec.value("name", "")), kMaxTokenNameChars, true);
		if (record.name.empty()) return fail("Enter a token name.");
		record.trusted = spec.value("trusted", false);
		record.max_git_permission = spec.value("maxGitPermission", "commit");
		if (GitRank(record.max_git_permission) == 0) return fail("Choose a Git permission.");
		for (const nlohmann::json& value : spec.value("workspaces", nlohmann::json::array()))
		{
			if (!value.is_string() || ChatDomainService().FindFolderById(app, value.get<std::string>()) == nullptr) return fail("A selected workspace no longer exists.");
			if (std::ranges::find(record.workspaces, value.get<std::string>()) == record.workspaces.end()) record.workspaces.push_back(value.get<std::string>());
		}
		if (record.workspaces.empty()) return fail("Choose at least one workspace.");
		const nlohmann::json lifetime = spec.value("lifetimeDays", nlohmann::json(kDefaultLifetimeDays));
		if (!lifetime.is_number_integer() || lifetime.get<std::int64_t>() < 1 || lifetime.get<std::int64_t>() > kMaxLifetimeDays) return fail("Choose an expiry from 1 to 365 days.");
		record.lifetime_days = lifetime.get<int>();
		const std::optional<std::string> secret = IssueSecret(record);
		if (!secret) return fail("The token could not be generated.");
		record.id = "at_" + RandomHex(2);
		record.created_at = uam::time::TimestampNow();
		tokens->push_back(record);
		if (!WriteTokens(app, *tokens)) return fail("The automation token could not be saved.");
		uam::diagnostics::Write("Automation API: created token " + record.id + ".");
		return nlohmann::json{{"secret", *secret}, {"token", TokenMetadata(record)}};
	}

	std::optional<nlohmann::json> AutomationApiService::RegenerateToken(const AppState& app, std::string_view token_id, std::string* error_out)
	{
		const auto fail = [&](std::string message) { if (error_out != nullptr) *error_out = std::move(message); return std::nullopt; };
		std::optional<std::vector<TokenRecord>> tokens = ReadTokens(app);
		if (!tokens) return fail("The automation token file could not be read, so the token was not regenerated.");
		const auto token = std::ranges::find(*tokens, token_id, &TokenRecord::id);
		if (token == tokens->end()) return fail("That token no longer exists.");
		const std::optional<std::string> secret = IssueSecret(*token);
		if (!secret) return fail("The token could not be generated.");
		// Every request reads this file, so a completed write retires the old secret, including for later status polls.
		if (!WriteTokens(app, *tokens)) return fail("The token could not be regenerated; the old token still works. Try again.");
		uam::diagnostics::Write("Automation API: regenerated token " + token->id + ".");
		return nlohmann::json{{"secret", *secret}, {"token", TokenMetadata(*token)}};
	}

	bool AutomationApiService::RevokeToken(const AppState& app, std::string_view token_id, std::string* error_out)
	{
		const auto fail = [&](std::string message) { if (error_out != nullptr) *error_out = std::move(message); return false; };
		std::optional<std::vector<TokenRecord>> tokens = ReadTokens(app);
		if (!tokens) return fail("The automation token file could not be read, so the token was not revoked.");
		if (std::erase_if(*tokens, [&](const TokenRecord& token) { return token.id == token_id; }) == 0) return fail("That token no longer exists.");
		// Every request reads this file, so a completed write is the revocation.
		if (!WriteTokens(app, *tokens)) return fail("The token could not be revoked and still works. Try again.");
		uam::diagnostics::Write("Automation API: revoked token " + std::string(token_id) + ".");
		return true;
	}

	std::string AutomationApiService::StartedChatRefusal(const AppState& app, const ChatSession& caller)
	{
		if (caller.automation_token_id.empty()) return {};
		const std::optional<std::vector<TokenRecord>> tokens = ReadTokens(app);
		if (!tokens) return "Automation tokens are unavailable, so this automation chat cannot start another chat.";
		const auto token = std::ranges::find(*tokens, caller.automation_token_id, &TokenRecord::id);
		// A revoked or expired token creates nothing, through the API or through its chats.
		if (token == tokens->end()) return "This automation chat's token was revoked, so it cannot start another chat.";
		if (uam::time::TimestampNowSec() >= token->expires_at) return "This automation chat's token has expired, so it cannot start another chat.";
		if (OpenChatCount(app, token->id) >= kMaxOpenChatsPerToken) return "This automation chat's token already has 16 open chats. Chats started from automation chats count until they are deleted, so ask the user to delete one, or wait for another chat to finish.";
		return {};
	}

	std::optional<AutomationChatLaunch> AutomationApiService::BeginLaunch(AppState& app, const std::string& chat_id, std::string* error_out)
	{
		const auto fail = [&](std::string message) { if (error_out != nullptr) *error_out = std::move(message); return std::nullopt; };
		ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		if (chat == nullptr || !chat->AwaitsAutomationApproval()) return fail("This chat is not waiting for approval.");
		if (std::string refusal = UamControlService::ChatWorktreeIsolationRefusal(app, *chat); !refusal.empty()) return fail(std::move(refusal));
		const ChatSession before = *chat;
		AutomationChatLaunch launch;
		launch.text = chat->automation_pending_prompt;
		// CreateGoal adds the Git policy and iteration cap to every goal of an automation chat. The policy goes in the goal
		// rather than the message, so the transcript shows what was asked and every continuation repeats it.
		if (!GoalService::CreateGoal(app, chat_id, launch.text, app.settings.goal_token_budget, &launch.goal_id) || !GoalService::SetActiveGoal(app, chat_id, launch.goal_id, nullptr, nullptr, true))
		{
			*chat = before;
			return fail("The chat's goal could not be created.");
		}
		chat->automation_pending_prompt.clear();
		if (!ChatRepository::SaveChat(app.data_root, *chat))
		{
			*chat = before;
			return fail("The chat could not be saved.");
		}
		return launch;
	}

	bool AutomationApiService::AbandonLaunch(AppState& app, const std::string& chat_id, const AutomationChatLaunch& launch)
	{
		ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		// A recorded prompt may have reached the provider, so holding the chat again could run it twice.
		if (chat == nullptr || !chat->messages.empty()) return false;
		std::erase_if(chat->goals, [&](const Goal& goal) { return goal.id == launch.goal_id; });
		if (chat->active_goal_id == launch.goal_id) chat->active_goal_id.clear();
		chat->automation_pending_prompt = launch.text;
		if (!ChatRepository::SaveChat(app.data_root, *chat)) uam::diagnostics::Write("Automation API: the approval hold for chat " + chat_id + " could not be saved.");
		ChatDomainService().MarkChatNeedsAttention(app, chat_id);
		return true;
	}
} // namespace uam
