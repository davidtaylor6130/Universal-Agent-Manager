#include "test_harness.h"
#include "app/automation_api_service.h"
#include "app/chat_domain_service.h"
#include "common/chat/chat_repository.h"
#include "common/config/execution_host_config.h"
#include "common/paths/workspace_root.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/terminal/terminal_launch.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/io_utils.h"

using namespace uam_test;
using uam::AutomationApiService;

namespace
{
	struct Issued
	{
		std::string secret;
		std::string id;
	};

	struct AutomationFixture
	{
		TempDir temp{"uam-automation-api"};
		uam::AppState app;
		fs::path workspace;
		AutomationFixture()
		{
			workspace = temp.root / "workspace";
			fs::create_directories(workspace);
			app.data_root = temp.root / "data";
			fs::create_directories(app.data_root);
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.settings.git_enabled = true;
			app.settings.git_auto_worktree = true;
			app.settings.git_snapshot_plain_folders = true;
			app.settings.git_snapshot_svn = true;
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
			for (const char* id : {"folder-a", "folder-b"})
			{
				ChatFolder folder;
				folder.id = id;
				folder.title = id;
				folder.directory = workspace.string();
				app.folders.push_back(folder);
			}
			uam::CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("codex-cli", "local")];
			version.checked = true;
			version.supported = true;
			version.installed_version = "1.2.3";
			version.execution_host = app.settings.execution_hosts.front();
		}
		Issued Token(bool trusted = false, const std::string& max_git = "commit")
		{
			std::string error;
			const auto created = AutomationApiService::CreateToken(app, {{"name", "n8n"}, {"trusted", trusted}, {"workspaces", {"folder-a"}}, {"maxGitPermission", max_git}}, &error);
			UAM_ASSERT(created.has_value());
			return {created->at("secret").get<std::string>(), created->at("token").at("id").get<std::string>()};
		}
		static nlohmann::json Body()
		{
			return {{"prompt", "Fix the parser."}, {"workspaceId", "folder-a"}, {"providerId", "codex-cli"}, {"maxIterations", 3}, {"gitPermission", "commit"}};
		}
		uam::AutomationApiResponse Post(const std::string& secret, const nlohmann::json& body)
		{
			return Raw(secret, body.dump());
		}
		uam::AutomationApiResponse Raw(const std::string& secret, std::string body)
		{
			const auto hash = AutomationApiService::TokenHashFromAuthorization("Bearer " + secret);
			return AutomationApiService::Handle(app, {.method = "POST", .path = "/automation/chats", .token_hash = hash.value_or(""), .body = std::move(body)});
		}
		uam::AutomationApiResponse Status(const std::string& secret, const std::string& chat_id)
		{
			const auto hash = AutomationApiService::TokenHashFromAuthorization("Bearer " + secret);
			return AutomationApiService::Handle(app, {.method = "GET", .path = "/automation/chats/" + chat_id, .token_hash = hash.value_or("")});
		}
		ChatSession* Chat(const std::string& id) { return ChatDomainService().FindChatById(app, id); }
		/// A refusal must leave no chat in memory and no chat file on disk.
		void RequireNoChat()
		{
			UAM_ASSERT(app.chats.empty());
			UAM_ASSERT(ChatRepository::LoadLocalChats(app.data_root).empty());
		}
	};
}

UAM_TEST(AutomationApiRefusesUnknownTokensWithNoSideEffects)
{
	AutomationFixture fixture;
	(void)fixture.Token();
	const std::string unknown = "uamat_" + std::string(64, 'a');
	UAM_ASSERT_EQ(fixture.Post(unknown, AutomationFixture::Body()).status, 401);
	// An absent hash is what a malformed header produces.
	UAM_ASSERT_EQ(AutomationApiService::Handle(fixture.app, {.method = "POST", .path = "/automation/chats", .body = AutomationFixture::Body().dump()}).status, 401);
	fixture.RequireNoChat();
}

UAM_TEST(AutomationApiParsesOnlyWellFormedBearerTokens)
{
	const std::string secret = "uamat_" + std::string(64, 'a');
	UAM_ASSERT(AutomationApiService::TokenHashFromAuthorization("Bearer " + secret).has_value());
	for (const std::string& header : {std::string{}, secret, "bearer " + secret, "Bearer " + std::string(64, 'a'), "Bearer uamat_" + std::string(63, 'a'),
	                                  "Bearer uamat_" + std::string(65, 'a'), "Bearer uamat_" + std::string(64, 'A'), "Bearer uamat_" + std::string(64, 'g')})
		UAM_ASSERT(!AutomationApiService::TokenHashFromAuthorization(header).has_value());
	UAM_ASSERT(AutomationApiService::ConstantTimeEquals("abc", "abc"));
	UAM_ASSERT(!AutomationApiService::ConstantTimeEquals("abc", "abd"));
	UAM_ASSERT(!AutomationApiService::ConstantTimeEquals("abc", "ab"));
}

UAM_TEST(AutomationApiRefusesWorkspaceOutsideAllowList)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	nlohmann::json body = AutomationFixture::Body();
	body["workspaceId"] = "folder-b"; // Exists, but is not on this token's list.
	UAM_ASSERT_EQ(fixture.Post(token.secret, body).status, 403);
	body["workspaceId"] = "no-such-folder";
	UAM_ASSERT_EQ(fixture.Post(token.secret, body).status, 403);
	fixture.RequireNoChat();
}

UAM_TEST(AutomationApiRequiresABoundedIntegerIterationCap)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	nlohmann::json body = AutomationFixture::Body();
	body.erase("maxIterations");
	UAM_ASSERT_EQ(fixture.Post(token.secret, body).status, 400);
	for (const nlohmann::json& invalid : {nlohmann::json(0), nlohmann::json(-1), nlohmann::json(201), nlohmann::json("3"), nlohmann::json(1.5), nlohmann::json(true), nlohmann::json(nullptr)})
	{
		body["maxIterations"] = invalid;
		UAM_ASSERT_EQ(fixture.Post(token.secret, body).status, 400);
	}
	fixture.RequireNoChat();
}

UAM_TEST(AutomationApiRefusesGitPermissionAboveTheTokenCeiling)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token(false, "commit");
	nlohmann::json body = AutomationFixture::Body();
	body["gitPermission"] = "pr";
	UAM_ASSERT_EQ(fixture.Post(token.secret, body).status, 403);
	body["gitPermission"] = "force-push";
	UAM_ASSERT_EQ(fixture.Post(token.secret, body).status, 400);
	body.erase("gitPermission");
	UAM_ASSERT_EQ(fixture.Post(token.secret, body).status, 400);
	fixture.RequireNoChat();
}

UAM_TEST(AutomationApiRefusesOversizedAndMalformedBodies)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	UAM_ASSERT_EQ(fixture.Raw(token.secret, std::string(AutomationApiService::kMaxBodyBytes + 1, ' ')).status, 413);
	UAM_ASSERT_EQ(fixture.Raw(token.secret, "{").status, 400);
	UAM_ASSERT_EQ(fixture.Raw(token.secret, "[]").status, 400);
	nlohmann::json body = AutomationFixture::Body();
	body["prompt"] = 42;
	UAM_ASSERT_EQ(fixture.Post(token.secret, body).status, 400);
	body["prompt"] = "   ";
	UAM_ASSERT_EQ(fixture.Post(token.secret, body).status, 400);
	body = AutomationFixture::Body();
	for (const char* model : {"--dangerously-skip-permissions", "gpt 5", "a;b"})
	{
		body["modelId"] = model;
		UAM_ASSERT_EQ(fixture.Post(token.secret, body).status, 400);
	}
	fixture.RequireNoChat();
}

UAM_TEST(AutomationApiRefusesWithoutAnIsolatedWorktree)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	fixture.app.settings.git_auto_worktree = false;
	UAM_ASSERT_EQ(fixture.Post(token.secret, AutomationFixture::Body()).status, 409);
	fixture.RequireNoChat();
}

UAM_TEST(AutomationApiRevocationTakesEffectImmediately)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	const auto created = fixture.Post(token.secret, AutomationFixture::Body());
	UAM_ASSERT_EQ(created.status, 200);
	const std::string chat_id = created.body.at("chatId").get<std::string>();
	UAM_ASSERT_EQ(fixture.Status(token.secret, chat_id).status, 200);

	std::string error;
	UAM_ASSERT(AutomationApiService::RevokeToken(fixture.app, token.id, &error));
	UAM_ASSERT_EQ(fixture.Post(token.secret, AutomationFixture::Body()).status, 401);
	UAM_ASSERT_EQ(fixture.Status(token.secret, chat_id).status, 401);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	// The waiting chat stays visible with its label, for the user to delete or start.
	UAM_ASSERT_EQ(fixture.Chat(chat_id)->automation_token_name, std::string("n8n"));
}

UAM_TEST(AutomationApiStoresOnlyTheTokenHash)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	std::string file;
	UAM_ASSERT(uam::io::TryReadTextFile(fixture.app.data_root / "automation_tokens.json", file));
	UAM_ASSERT(file.find(token.secret) == std::string::npos);
	const std::string hash = *AutomationApiService::TokenHashFromAuthorization("Bearer " + token.secret);
	UAM_ASSERT(file.find(hash) != std::string::npos);
	std::string error;
	const auto listed = AutomationApiService::ListTokens(fixture.app, &error);
	UAM_ASSERT(listed.has_value() && listed->size() == 1);
	UAM_ASSERT(listed->dump().find(hash) == std::string::npos && listed->dump().find(token.secret) == std::string::npos);
#if !defined(_WIN32)
	UAM_ASSERT((fs::status(fixture.app.data_root / "automation_tokens.json").permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
}

UAM_TEST(AutomationApiRefusesWhenTheTokenStoreIsCorrupt)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	UAM_ASSERT(uam::io::WriteTextFile(fixture.app.data_root / "automation_tokens.json", "{not json"));
	UAM_ASSERT_EQ(fixture.Post(token.secret, AutomationFixture::Body()).status, 503);
	std::string error;
	UAM_ASSERT(!AutomationApiService::CreateToken(fixture.app, {{"name", "x"}, {"workspaces", {"folder-a"}}}, &error).has_value());
	std::string file;
	UAM_ASSERT(uam::io::TryReadTextFile(fixture.app.data_root / "automation_tokens.json", file));
	UAM_ASSERT_EQ(file, std::string("{not json"));
	fixture.RequireNoChat();
}

UAM_TEST(AutomationApiUntrustedChatWaitsAndCannotReachAProvider)
{
	AutomationFixture fixture;
	fixture.app.settings.provider_chat_defaults["codex-cli"].approval_mode = "yolo";
	const Issued token = fixture.Token();
	const auto created = fixture.Post(token.secret, AutomationFixture::Body());
	UAM_ASSERT_EQ(created.status, 200);
	UAM_ASSERT_EQ(created.body.at("status").get<std::string>(), std::string("awaitingApproval"));
	UAM_ASSERT(created.launch_chat_id.empty());
	const std::string chat_id = created.body.at("chatId").get<std::string>();
	ChatSession* chat = fixture.Chat(chat_id);
	UAM_ASSERT(chat->AwaitsAutomationApproval());
	UAM_ASSERT_EQ(chat->workspace_isolation_kind, std::string(uam::paths::kPendingGitWorktreeIsolationKind));
	UAM_ASSERT_EQ(chat->approval_mode, std::string("default"));
	UAM_ASSERT_EQ(chat->command_safety_tier, std::string("off"));
	UAM_ASSERT(chat->goals.empty());
	UAM_ASSERT(!chat->attention_revision.empty());
	UAM_ASSERT(fixture.app.chats_with_unseen_updates.contains(chat_id));
	UAM_ASSERT_EQ(fixture.Status(token.secret, chat_id).body.at("status").get<std::string>(), std::string("awaitingApproval"));

	// Every route to a provider refuses until approval.
	std::string error;
	UAM_ASSERT(!uam::SendAcpPrompt(fixture.app, chat_id, "go anyway", &error));
	UAM_ASSERT_EQ(error, std::string(kAwaitingAutomationApprovalError));
	uam::AcpSessionState session;
	error.clear();
	UAM_ASSERT(!uam::acp_detail::StartAcpProcessForChat(fixture.app, session, *chat, &error));
	UAM_ASSERT_EQ(error, std::string(kAwaitingAutomationApprovalError));
	uam::CliTerminalState terminal;
	UAM_ASSERT(!uam::StartCliTerminalForChat(fixture.app, terminal, *chat, 24, 80));
	UAM_ASSERT(fixture.app.acp_sessions.empty());
}

UAM_TEST(AutomationApiLaunchRefusesAChatThatLostItsWorktree)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	const std::string chat_id = fixture.Post(token.secret, AutomationFixture::Body()).body.at("chatId").get<std::string>();
	std::string error;
	UAM_ASSERT(AutomationApiService::BeginLaunch(fixture.app, chat_id, &error).has_value());
	// What SSH preparation does for a directory with no Git metadata.
	ChatSession* chat = fixture.Chat(chat_id);
	chat->workspace_isolation_kind.clear();
	uam::AcpSessionState session;
	UAM_ASSERT(!uam::acp_detail::StartAcpProcessForChat(fixture.app, session, *chat, &error));
	UAM_ASSERT_EQ(error, std::string(kAutomationWorktreeRequiredError));
	uam::CliTerminalState terminal;
	UAM_ASSERT(!uam::StartCliTerminalForChat(fixture.app, terminal, *chat, 24, 80));
}

UAM_TEST(AutomationApiRefusesAProviderThatAnswersItsOwnPrompts)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	nlohmann::json body = AutomationFixture::Body();
	body["providerId"] = "antigravity-cli";
	fixture.app.provider_profiles.push_back(ProviderProfileStore::DefaultAntigravityProfile());
	uam::CliProviderVersionState& version = fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("antigravity-cli", "local")];
	version.checked = true;
	version.supported = true;
	version.installed_version = "1.2.3";
	version.execution_host = fixture.app.settings.execution_hosts.front();
	UAM_ASSERT_EQ(fixture.Post(token.secret, body).status, 409);
	fixture.RequireNoChat();
}

UAM_TEST(AutomationApiKeepsARecordedPromptOutOfTheHoldOnFailedLaunch)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	const std::string chat_id = fixture.Post(token.secret, AutomationFixture::Body()).body.at("chatId").get<std::string>();
	std::string error;
	const auto launch = AutomationApiService::BeginLaunch(fixture.app, chat_id, &error);
	UAM_ASSERT(launch.has_value());
	Message sent;
	sent.content = launch->text;
	fixture.Chat(chat_id)->messages.push_back(sent);
	AutomationApiService::AbandonLaunch(fixture.app, chat_id, *launch);
	UAM_ASSERT(!fixture.Chat(chat_id)->AwaitsAutomationApproval());
	UAM_ASSERT(!fixture.Chat(chat_id)->goals.empty());
}

UAM_TEST(AutomationApiTrustedChatLaunchesOnceWithItsCap)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token(true, "pr");
	nlohmann::json body = AutomationFixture::Body();
	body["gitPermission"] = "pr";
	body["maxIterations"] = 5;
	const auto created = fixture.Post(token.secret, body);
	UAM_ASSERT_EQ(created.status, 200);
	UAM_ASSERT(!created.launch_chat_id.empty());
	const std::string chat_id = created.launch_chat_id;

	std::string error;
	const auto launch = AutomationApiService::BeginLaunch(fixture.app, chat_id, &error);
	UAM_ASSERT(launch.has_value());
	UAM_ASSERT(!fixture.Chat(chat_id)->AwaitsAutomationApproval());
	UAM_ASSERT(launch->text.find("open a pull request") != std::string::npos);
	const Goal* goal = uam::GoalService::FindGoalById(fixture.app, chat_id, launch->goal_id);
	UAM_ASSERT(goal != nullptr);
	UAM_ASSERT_EQ(goal->max_iterations, 5);
	UAM_ASSERT_EQ(fixture.Chat(chat_id)->active_goal_id, launch->goal_id);
	UAM_ASSERT_EQ(fixture.Status(token.secret, chat_id).body.at("status").get<std::string>(), std::string("running"));
	// The hold is gone, so the launch cannot be repeated.
	UAM_ASSERT(!AutomationApiService::BeginLaunch(fixture.app, chat_id, &error).has_value());

	// A refused send returns the chat to waiting instead of losing the prompt.
	AutomationApiService::AbandonLaunch(fixture.app, chat_id, *launch);
	UAM_ASSERT(fixture.Chat(chat_id)->AwaitsAutomationApproval());
	UAM_ASSERT_EQ(fixture.Chat(chat_id)->automation_pending_prompt, std::string("Fix the parser."));
	UAM_ASSERT(fixture.Chat(chat_id)->goals.empty());
	UAM_ASSERT(fixture.Chat(chat_id)->active_goal_id.empty());
}

UAM_TEST(AutomationApiLaunchRefusedWhenIsolationIsLaterDisabled)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	const std::string chat_id = fixture.Post(token.secret, AutomationFixture::Body()).body.at("chatId").get<std::string>();
	fixture.app.settings.git_auto_worktree = false;
	std::string error;
	UAM_ASSERT(!AutomationApiService::BeginLaunch(fixture.app, chat_id, &error).has_value());
	UAM_ASSERT(fixture.Chat(chat_id)->AwaitsAutomationApproval());
}

UAM_TEST(AutomationApiStatusIsScopedToTheCreatingToken)
{
	AutomationFixture fixture;
	const Issued owner = fixture.Token();
	const Issued other = fixture.Token();
	const std::string chat_id = fixture.Post(owner.secret, AutomationFixture::Body()).body.at("chatId").get<std::string>();
	UAM_ASSERT_EQ(fixture.Status(other.secret, chat_id).status, 404);
	UAM_ASSERT_EQ(fixture.Status(owner.secret, "no-such-chat").status, 404);
	// A hand-made chat has no automation owner.
	ChatSession own = ChatDomainService().CreateNewChat("folder-a", "codex-cli");
	own.id = "desktop-chat";
	fixture.app.chats.push_back(own);
	UAM_ASSERT_EQ(fixture.Status(owner.secret, "desktop-chat").status, 404);
}

UAM_TEST(AutomationApiBoundsOpenChatsPerToken)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	for (int index = 0; index < 16; ++index) UAM_ASSERT_EQ(fixture.Post(token.secret, AutomationFixture::Body()).status, 200);
	UAM_ASSERT_EQ(fixture.Post(token.secret, AutomationFixture::Body()).status, 429);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{16});
}

UAM_TEST(AutomationApiFieldsSurviveAReload)
{
	AutomationFixture fixture;
	const Issued token = fixture.Token();
	const std::string chat_id = fixture.Post(token.secret, AutomationFixture::Body()).body.at("chatId").get<std::string>();
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, chat_id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(loaded->AwaitsAutomationApproval());
	UAM_ASSERT_EQ(loaded->automation_token_id, token.id);
	UAM_ASSERT_EQ(loaded->automation_max_iterations, 3);
	std::string error;
	const auto launch = AutomationApiService::BeginLaunch(fixture.app, chat_id, &error);
	UAM_ASSERT(launch.has_value());
	const std::optional<ChatSession> started = ChatRepository::LoadLocalChat(fixture.app.data_root, chat_id);
	UAM_ASSERT(started.has_value() && !started->AwaitsAutomationApproval());
	UAM_ASSERT_EQ(started->goals.at(0).max_iterations, 3);
}

UAM_TEST(GoalIterationLimitUsesTheStricterCap)
{
	AppSettings settings;
	settings.goal_max_loop_iterations = 50;
	Goal goal;
	UAM_ASSERT_EQ(uam::GoalService::IterationLimit(goal, settings), 50);
	goal.max_iterations = 5;
	UAM_ASSERT_EQ(uam::GoalService::IterationLimit(goal, settings), 5);
	goal.max_iterations = 80;
	UAM_ASSERT_EQ(uam::GoalService::IterationLimit(goal, settings), 50);
}

UAM_TEST(StalledGoalResumeStopsAtTheIterationCap)
{
	AutomationFixture fixture;
	ChatSession chat = ChatDomainService().CreateNewChat("folder-a", "codex-cli");
	chat.workspace_directory = fixture.workspace.string();
	fixture.app.chats.push_back(chat);
	const std::string chat_id = chat.id;
	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(fixture.app, chat_id, "Keep going", 0, &goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(fixture.app, chat_id, goal_id, nullptr, nullptr, true));
	Goal* goal = uam::GoalService::FindGoalById(fixture.app, chat_id, goal_id);
	goal->max_iterations = 2;
	goal->loop_count = 2;
	uam::AcpSessionState session;
	session.chat_id = chat_id;
	session.provider_id = "codex-cli";
	session.running = true;
	session.session_ready = true;
	session.last_runtime_activity_time_s = 1.0;
	UAM_ASSERT(uam::ResumeStalledGoalLoopForTests(fixture.app, session, *fixture.Chat(chat_id), 1000.0));
	UAM_ASSERT(uam::GoalService::FindGoalById(fixture.app, chat_id, goal_id)->status == GoalStatus::Blocked);
	UAM_ASSERT(session.queued_user_prompts.empty());
}

UAM_TEST(AutomationApiNeverLogsTheTokenOrPrompt)
{
	AutomationFixture fixture;
	const fs::path log = fixture.temp.root / "diagnostic.log";
	std::string error;
	UAM_ASSERT(uam::diagnostics::StartFileLog(log, error));
	struct StopLog { ~StopLog() { uam::diagnostics::StopFileLog(); } } stop;
	const Issued token = fixture.Token();
	nlohmann::json body = AutomationFixture::Body();
	body["prompt"] = "SECRET-PROMPT-MARKER";
	UAM_ASSERT_EQ(fixture.Post(token.secret, body).status, 200);
	UAM_ASSERT_EQ(fixture.Post("uamat_" + std::string(64, 'b'), body).status, 401);
	uam::diagnostics::FlushFileLog();
	const std::string text = uam::io::ReadTextFile(log);
	UAM_ASSERT(text.find("Automation API") != std::string::npos);
	UAM_ASSERT(text.find(token.secret) == std::string::npos);
	UAM_ASSERT(text.find(*AutomationApiService::TokenHashFromAuthorization("Bearer " + token.secret)) == std::string::npos);
	UAM_ASSERT(text.find("SECRET-PROMPT-MARKER") == std::string::npos);
}
