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

		struct TokenRecord
		{
			std::string id;
			std::string name;
			std::string hash;
			bool trusted = false;
			std::vector<std::string> workspaces;
			std::string max_git_permission = "commit";
			std::string created_at;
		};

		std::filesystem::path TokenFile(const AppState& app)
		{
			return app.data_root / "automation_tokens.json";
		}

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
					tokens.push_back(std::move(record));
				}
			}
			catch (const nlohmann::json::exception&)
			{
				return std::nullopt;
			}
			return tokens;
		}

		bool WriteTokens(const AppState& app, const std::vector<TokenRecord>& tokens)
		{
			nlohmann::json list = nlohmann::json::array();
			for (const TokenRecord& token : tokens)
				list.push_back({{"id", token.id}, {"name", token.name}, {"hash", token.hash}, {"trusted", token.trusted},
				                {"workspaces", token.workspaces}, {"maxGitPermission", token.max_git_permission}, {"createdAt", token.created_at}});
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

		bool ChatIsOpen(const AppState& app, const ChatSession& chat)
		{
			return chat.AwaitsAutomationApproval() || (GoalService::FindActiveGoal(app, chat.id) != nullptr && !LaunchFailedAfterAcceptance(app, chat));
		}

		AutomationApiResponse CreateChat(AppState& app, const AutomationApiRequest& request, const TokenRecord& token)
		{
			const nlohmann::json body = nlohmann::json::parse(request.body, nullptr, false);
			if (!body.is_object()) return Refuse(400, "The request body must be a JSON object.");
			std::string prompt, workspace_id, provider, model, title, git_permission;
			if (!ReadField(body, "prompt", kMaxPromptBytes, prompt) || prompt.empty()) return Refuse(400, "prompt is required and must be between 1 byte and 128 KiB.");
			if (!ReadField(body, "workspaceId", kMaxFieldBytes, workspace_id) || workspace_id.empty()) return Refuse(400, "workspaceId is required.");
			if (!ReadField(body, "providerId", kMaxFieldBytes, provider) || provider.empty()) return Refuse(400, "providerId is required.");
			// A model ID reaches a provider command line, so it may not look like a flag.
			if (!ReadField(body, "modelId", kMaxFieldBytes, model) || model.starts_with('-') ||
			    !std::ranges::all_of(model, [](unsigned char ch) { return std::isalnum(ch) != 0 || std::string_view("-_./:[]").find(static_cast<char>(ch)) != std::string_view::npos; }))
				return Refuse(400, "modelId must be a plain model name.");
			if (!ReadField(body, "title", kMaxFieldBytes, title)) return Refuse(400, "title must be a short string.");
			const auto cap = body.find("maxIterations");
			if (cap == body.end() || !cap->is_number_integer() || cap->get<std::int64_t>() < 1 || cap->get<std::int64_t>() > kMaxIterations)
				return Refuse(400, "maxIterations is required and must be an integer from 1 to 200.");
			if (!ReadField(body, "gitPermission", kMaxFieldBytes, git_permission) || GitRank(git_permission) == 0)
				return Refuse(400, "gitPermission is required and must be \"commit\" or \"pr\".");

			if (std::ranges::find(token.workspaces, workspace_id) == token.workspaces.end()) return Refuse(403, "This token may not use that workspace.");
			if (GitRank(git_permission) > GitRank(token.max_git_permission)) return Refuse(403, "This token may not request that Git permission.");
			const ChatFolder* folder = ChatDomainService().FindFolderById(app, workspace_id);
			if (folder == nullptr || uam::strings::IsBlank(folder->directory)) return Refuse(404, "That workspace no longer exists.");
			if (static_cast<std::size_t>(std::ranges::count_if(app.chats, [&](const ChatSession& chat) { return chat.automation_token_id == token.id && ChatIsOpen(app, chat); })) >= kMaxOpenChatsPerToken)
				return Refuse(429, "This token already has 16 open chats. Finish or delete one first.");

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
			chat.automation_max_iterations = cap->get<int>();
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
			if (chat == app.chats.end() || chat->automation_token_id != token.id) return Refuse(404, "Chat not found.");
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
		else if (request.body.size() > kMaxBodyBytes) response = Refuse(413, "Request exceeds 256 KiB.");
		else if (request.method == "POST" && request.path == kChatsPath) response = CreateChat(app, request, *token);
		else if (request.method == "GET" && IsApiPath(request.path) && request.path != kChatsPath) response = ChatStatus(app, std::string_view(request.path).substr(kChatsPath.size() + 1), *token);
		else response = Refuse(404, "Unknown automation endpoint.");
		LogRequest(request, response.status == 401 ? nullptr : token, response.status, response.body.value("chatId", std::string{}));
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
		for (const TokenRecord& token : *tokens)
			list.push_back({{"id", token.id}, {"name", token.name}, {"trusted", token.trusted}, {"workspaces", token.workspaces}, {"maxGitPermission", token.max_git_permission}, {"createdAt", token.created_at}});
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
		const std::string secret = std::string(kTokenPrefix) + RandomHex(8);
		const std::optional<std::string> hash = Sha256(secret);
		if (!hash) return fail("The token could not be generated.");
		record.hash = *hash;
		record.id = "at_" + RandomHex(2);
		record.created_at = uam::time::TimestampNow();
		tokens->push_back(record);
		if (!WriteTokens(app, *tokens)) return fail("The automation token could not be saved.");
		uam::diagnostics::Write("Automation API: created token " + record.id + ".");
		return nlohmann::json{{"secret", secret}, {"token", {{"id", record.id}, {"name", record.name}, {"trusted", record.trusted}, {"workspaces", record.workspaces}, {"maxGitPermission", record.max_git_permission}, {"createdAt", record.created_at}}}};
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

	std::optional<AutomationChatLaunch> AutomationApiService::BeginLaunch(AppState& app, const std::string& chat_id, std::string* error_out)
	{
		const auto fail = [&](std::string message) { if (error_out != nullptr) *error_out = std::move(message); return std::nullopt; };
		ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		if (chat == nullptr || !chat->AwaitsAutomationApproval()) return fail("This chat is not waiting for approval.");
		if (std::string refusal = UamControlService::ChatWorktreeIsolationRefusal(app, *chat); !refusal.empty()) return fail(std::move(refusal));
		const ChatSession before = *chat;
		AutomationChatLaunch launch;
		launch.original_prompt = chat->automation_pending_prompt;
		// ponytail: an instruction, not a block; Default approval is what stops a push, and a Git-aware command gate would make this enforceable.
		launch.text = (chat->automation_git_permission == "pr"
		                   ? "Automation policy: you may commit to this chat's branch and open a pull request. Do not push anywhere else.\n\n"
		                   : "Automation policy: commit to this chat's branch only. Do not push or open a pull request.\n\n") + launch.original_prompt;
		if (!GoalService::CreateGoal(app, chat_id, launch.text, app.settings.goal_token_budget, &launch.goal_id) || !GoalService::SetActiveGoal(app, chat_id, launch.goal_id, nullptr, nullptr, true))
		{
			*chat = before;
			return fail("The chat's goal could not be created.");
		}
		GoalService::FindGoalById(app, chat_id, launch.goal_id)->max_iterations = chat->automation_max_iterations;
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
		chat->automation_pending_prompt = launch.original_prompt;
		if (!ChatRepository::SaveChat(app.data_root, *chat)) uam::diagnostics::Write("Automation API: the approval hold for chat " + chat_id + " could not be saved.");
		ChatDomainService().MarkChatNeedsAttention(app, chat_id);
		return true;
	}
} // namespace uam
