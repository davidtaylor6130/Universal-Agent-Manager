#include "app/model_discovery_service.h"
#include "common/provider/provider_account_refusal.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "test_harness.h"

#include <chrono>
#include <thread>

using namespace uam_test;

namespace
{
	// Shaped from the refusal Gemini CLI returned for every workspace on an individual Code Assist account.
	constexpr const char* kGeminiIndividualsRefusal =
	    "This client is no longer supported for Gemini Code Assist for individuals. To continue using Gemini, please migrate to the Antigravity suite...";
	constexpr const char* kGeminiNotice = "Google no longer supports this Gemini client for personal accounts.";

	std::string ErrorLine(int id, int code, const std::string& message)
	{
		return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}}.dump();
	}

	struct AccountRefusalFixture
	{
		TempDir temp{"uam-account-refusal"};
		// Keep every provider's home inside the fixture so no developer catalogue or setting is read.
		ScopedEnvVar codex{"CODEX_HOME", (temp.root / "codex").string()};
		ScopedEnvVar claude{"CLAUDE_CONFIG_DIR", (temp.root / "claude").string()};
		ScopedEnvVar opencode{"XDG_CONFIG_HOME", (temp.root / "config").string()};
		ScopedEnvVar opencode_config{"OPENCODE_CONFIG", (temp.root / "config" / "opencode.json").string()};
		ScopedEnvVar gemini{"GEMINI_CLI_HOME", (temp.root / "gemini").string()};
		ScopedEnvVar copilot{"COPILOT_HOME", (temp.root / "copilot").string()};
		uam::AppState app;
		std::vector<uam::ModelDiscoveryScope> launched;

		AccountRefusalFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile(), ProviderProfileStore::DefaultCodexProfile()};
			app.settings.default_new_chat_provider_id = uam::provider_ids::kGeminiCli;
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			for (int index = 0; index < 3; ++index)
			{
				const fs::path workspace = temp.root / ("workspace-" + std::to_string(index));
				fs::create_directories(workspace);
				app.folders.push_back(ChatFolder{"folder-" + std::to_string(index), "Workspace", workspace.string(), false});
			}
			for (const char* provider : {uam::provider_ids::kGeminiCli, uam::provider_ids::kCodexCli})
			{
				app.runtime_cli_versions_by_provider_id[provider].checked = true;
				app.runtime_cli_versions_by_provider_id[provider].supported = true;
			}
			app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
			app.provider_model_catalog->Initialize(app.data_root, app.provider_profiles);
		}

		uam::StartupModelDiscoveryService::LaunchFunction Launcher()
		{
			return [this](uam::AppState&, const std::string& provider, const std::string& workspace, const std::string& host, std::string*)
			{
				launched.push_back({provider, workspace, host});
				return true;
			};
		}

		std::size_t Launches(std::string_view provider) const
		{
			return static_cast<std::size_t>(std::ranges::count(launched, std::string(provider), &uam::ModelDiscoveryScope::provider_id));
		}
	};
}

UAM_TEST(AccountRefusalClassifierNeedsTheServerCodeAndADocumentedPhrase)
{
	using uam::account_refusal::Kind;
	using uam::account_refusal::ClassifyAcpError;
	UAM_ASSERT(ClassifyAcpError("session/new", true, -32000, kGeminiIndividualsRefusal) == Kind::UnsupportedClient);
	UAM_ASSERT(ClassifyAcpError("initialize", true, -32000, kGeminiIndividualsRefusal) == Kind::UnsupportedClient);
	// Surrounding wording can change without losing the classification.
	UAM_ASSERT(ClassifyAcpError("session/new", true, -32000, "Sorry: this CLIENT IS NO LONGER SUPPORTED for Gemini Code Assist for teams.") == Kind::UnsupportedClient);
	UAM_ASSERT(ClassifyAcpError("session/new", true, -32000, "Your account is not eligible for this service.") == Kind::Entitlement);
	UAM_ASSERT(ClassifyAcpError("session/new", true, -32000, "Authentication required") == Kind::Authentication);
	// Resuming a saved session meets the same account refusal.
	UAM_ASSERT(ClassifyAcpError("session/load", true, -32000, kGeminiIndividualsRefusal) == Kind::UnsupportedClient);
	UAM_ASSERT(ClassifyAcpError("session/resume", true, -32000, kGeminiIndividualsRefusal) == Kind::UnsupportedClient);
	// Only setup methods, only the server error code, and only the phrase list qualify.
	UAM_ASSERT(ClassifyAcpError("session/prompt", true, -32000, kGeminiIndividualsRefusal) == Kind::None);
	// Model, plan and workspace messages that sound similar stay scoped to their workspace.
	UAM_ASSERT(ClassifyAcpError("session/new", true, -32000, "Model gemini-x is no longer supported for this project.") == Kind::None);
	UAM_ASSERT(ClassifyAcpError("session/new", true, -32000, "This workspace is not eligible for indexing.") == Kind::None);
	UAM_ASSERT(ClassifyAcpError("session/new", true, -32603, kGeminiIndividualsRefusal) == Kind::None);
	UAM_ASSERT(ClassifyAcpError("session/new", false, 0, kGeminiIndividualsRefusal) == Kind::None);
	UAM_ASSERT(ClassifyAcpError("session/new", true, -32000, "Workspace directory is not trusted.") == Kind::None);
	UAM_ASSERT(ClassifyAcpError("session/new", true, -32000, "") == Kind::None);
	UAM_ASSERT_EQ(uam::account_refusal::TtlSeconds(Kind::UnsupportedClient), uam::account_refusal::kAccountRefusalTtlSeconds);
	UAM_ASSERT(uam::account_refusal::TtlSeconds(Kind::Authentication) < uam::account_refusal::TtlSeconds(Kind::Entitlement));
	UAM_ASSERT(uam::account_refusal::KindFromName(uam::account_refusal::KindName(Kind::Entitlement)) == Kind::Entitlement);
	UAM_ASSERT(uam::account_refusal::KindFromName("unknown") == Kind::None);
	const std::string logged = uam::account_refusal::DiagnosticMessage(std::string("line one\nline two") + std::string(1000, 'x'));
	UAM_ASSERT(logged.find('\n') == std::string::npos);
	UAM_ASSERT(logged.size() <= 403);
	const IProviderRuntime& gemini = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kGeminiCli);
	UAM_ASSERT_EQ(gemini.AccountRefusalNotice(Kind::UnsupportedClient, "Gemini CLI", kGeminiIndividualsRefusal), std::string(kGeminiNotice));
	// Only an individuals refusal is described as a personal-account one.
	UAM_ASSERT_EQ(gemini.AccountRefusalNotice(Kind::UnsupportedClient, "Gemini CLI", "This client is no longer supported for Gemini Code Assist for teams."),
	    std::string("Google no longer supports this Gemini client for the signed-in account."));
	UAM_ASSERT_EQ(ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCodexCli).AccountRefusalNotice(Kind::Authentication, "Codex CLI", "Authentication required"),
	    std::string("Codex CLI needs you to sign in again on this computer."));
	UAM_ASSERT(!uam::account_refusal::BlocksChatLaunch(Kind::Authentication));
	UAM_ASSERT(uam::account_refusal::BlocksChatLaunch(Kind::UnsupportedClient));
	const std::string accented = uam::account_refusal::DiagnosticMessage(std::string(399, 'a') + "\xC3\xA9" + std::string(10, 'b'));
	UAM_ASSERT(accented.ends_with("a..."));
}

UAM_TEST(GeminiSessionNewAccountRefusalIsRecordedForTheProviderAndHost)
{
	AccountRefusalFixture fixture;
	uam::AppState& app = fixture.app;
	const std::string refused_workspace = app.folders[0].directory;
	const std::string other_workspace = app.folders[1].directory;
	ChatSession chat;
	chat.id = "model-discovery-account-refusal";
	chat.provider_id = uam::provider_ids::kGeminiCli;
	chat.workspace_directory = refused_workspace;
	chat.execution_host_id = "local";
	app.model_discovery_chats.push_back(chat);
	std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->provider_id = chat.provider_id;
	session->process_execution_host_id = "local";
	session->model_catalog_scope_key = app.provider_model_catalog->CaptureScopeKey(chat.provider_id, chat.workspace_directory, chat.execution_host_id);
	session->running = true;
	session->initialized = true;
	session->model_discovery_only = true;
	session->ephemeral_model_discovery = true;
	session->pending_request_methods[2] = "session/new";
	session->session_setup_request_id = 2;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));
	UAM_ASSERT(app.provider_model_catalog->BeginDiscovery(chat.provider_id, chat.workspace_directory, chat.execution_host_id));
	app.provider_model_catalog->MarkDiscoveryLaunchStarted(chat.provider_id, chat.workspace_directory, chat.execution_host_id);

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.model_discovery_chats.front(), ErrorLine(2, -32000, kGeminiIndividualsRefusal)));
	UAM_ASSERT(!app.provider_model_catalog->IsDiscoveryPending(chat.provider_id, refused_workspace));
	UAM_ASSERT(app.provider_model_catalog->IsAccountRefusalActive(chat.provider_id, "local"));
	UAM_ASSERT_EQ(app.provider_model_catalog->GetAccountRefusalNotice(chat.provider_id, "local"), std::string(kGeminiNotice));
	// Every workspace of the provider on that computer shows the same one line instead of the raw protocol error.
	for (const std::string& workspace : {refused_workspace, other_workspace})
	{
		UAM_ASSERT_EQ(app.provider_model_catalog->GetProviderRefreshError(chat.provider_id, workspace, "local"), std::string(kGeminiNotice));
		UAM_ASSERT_EQ(app.provider_model_catalog->GetCatalogSnapshot(chat.provider_id, workspace, "local")["modelRefreshError"], nlohmann::json(kGeminiNotice));
	}
	// Other providers and other computers are untouched.
	UAM_ASSERT(!app.provider_model_catalog->IsAccountRefusalActive(uam::provider_ids::kCodexCli, "local"));
	UAM_ASSERT(app.provider_model_catalog->GetProviderRefreshError(uam::provider_ids::kCodexCli, refused_workspace, "local").empty());
	// An automatic model-menu request does not ask the provider again while the refusal is active.
	UAM_ASSERT(!app.provider_model_catalog->BeginDiscoveryIfStale(chat.provider_id, other_workspace, "local"));

	const std::chrono::steady_clock::time_point stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (uam::AcpStopInProgress(app, chat.id) && std::chrono::steady_clock::now() < stop_deadline)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	(void)uam::PollAllAcpSessions(app);
	UAM_ASSERT(!uam::AcpStopInProgress(app, chat.id));

	// The refusal survives a restart, so startup does not relaunch the provider for every workspace.
	uam::ProviderModelCatalogService restarted;
	restarted.Initialize(app.data_root, app.provider_profiles);
	UAM_ASSERT(restarted.IsAccountRefusalActive(chat.provider_id, "local"));
	UAM_ASSERT_EQ(restarted.GetAccountRefusalNotice(chat.provider_id, "local"), std::string(kGeminiNotice));
}

UAM_TEST(OrdinaryGeminiSessionNewFailureStaysScopedToItsWorkspace)
{
	AccountRefusalFixture fixture;
	uam::AppState& app = fixture.app;
	ChatSession chat;
	chat.id = "model-discovery-scoped-failure";
	chat.provider_id = uam::provider_ids::kGeminiCli;
	chat.workspace_directory = app.folders[0].directory;
	chat.execution_host_id = "local";
	app.model_discovery_chats.push_back(chat);
	std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->provider_id = chat.provider_id;
	session->process_execution_host_id = "local";
	session->model_catalog_scope_key = app.provider_model_catalog->CaptureScopeKey(chat.provider_id, chat.workspace_directory, chat.execution_host_id);
	session->running = true;
	session->initialized = true;
	session->model_discovery_only = true;
	session->ephemeral_model_discovery = true;
	session->pending_request_methods[2] = "session/new";
	session->session_setup_request_id = 2;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));
	UAM_ASSERT(app.provider_model_catalog->BeginDiscovery(chat.provider_id, chat.workspace_directory, chat.execution_host_id));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.model_discovery_chats.front(), ErrorLine(2, -32000, "Workspace directory is not trusted.")));
	UAM_ASSERT(!app.provider_model_catalog->IsAccountRefusalActive(chat.provider_id, "local"));
	UAM_ASSERT(app.provider_model_catalog->GetAccountRefusalNotice(chat.provider_id, "local").empty());
	UAM_ASSERT(uam::strings::Contains(app.provider_model_catalog->GetProviderRefreshError(chat.provider_id, chat.workspace_directory, "local"), "Workspace directory is not trusted."));
	UAM_ASSERT(app.provider_model_catalog->GetProviderRefreshError(chat.provider_id, app.folders[1].directory, "local").empty());
	const std::chrono::steady_clock::time_point stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (uam::AcpStopInProgress(app, chat.id) && std::chrono::steady_clock::now() < stop_deadline)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	(void)uam::PollAllAcpSessions(app);
}

UAM_TEST(StartupDiscoverySkipsAnAccountRefusedProviderUntilItExpires)
{
	AccountRefusalFixture fixture;
	uam::AppState& app = fixture.app;
	app.provider_model_catalog->RememberAccountRefusal(uam::provider_ids::kGeminiCli, "local", uam::account_refusal::Kind::UnsupportedClient, kGeminiNotice);
	uam::StartupModelDiscoveryService discovery(fixture.Launcher());
	discovery.Initialize(app);
	// Gemini, the new-chat provider, registers all three workspaces and Codex the initial one; only Codex launches.
	for (int poll = 0; poll < 4; ++poll) (void)discovery.Poll(app);
	UAM_ASSERT_EQ(fixture.Launches(uam::provider_ids::kGeminiCli), std::size_t{0});
	UAM_ASSERT_EQ(fixture.Launches(uam::provider_ids::kCodexCli), std::size_t{1});
	UAM_ASSERT(!app.provider_model_catalog->IsDiscoveryPending(uam::provider_ids::kGeminiCli, app.folders.front().directory));

	// Once the TTL has passed, the held startup scopes ask the provider again.
	const std::int64_t expired = uam::time::TimestampNowSec() - uam::account_refusal::kAccountRefusalTtlSeconds - 1;
	app.provider_model_catalog->RememberAccountRefusal(uam::provider_ids::kGeminiCli, "local", uam::account_refusal::Kind::UnsupportedClient, kGeminiNotice, expired);
	UAM_ASSERT(!app.provider_model_catalog->IsAccountRefusalActive(uam::provider_ids::kGeminiCli, "local"));
	// The notice remains until a retry answers, rather than disappearing when the clock passes.
	UAM_ASSERT_EQ(app.provider_model_catalog->GetAccountRefusalNotice(uam::provider_ids::kGeminiCli, "local"), std::string(kGeminiNotice));
	UAM_ASSERT(discovery.Poll(app));
	for (int poll = 0; poll < 3; ++poll) (void)discovery.Poll(app);
	UAM_ASSERT_EQ(fixture.Launches(uam::provider_ids::kGeminiCli), std::size_t{3});
}

UAM_TEST(AccountRefusalIsLiftedByAManualRetryOrAnAnswer)
{
	AccountRefusalFixture fixture;
	uam::AppState& app = fixture.app;
	const std::string workspace = app.folders.front().directory;
	app.provider_model_catalog->RememberAccountRefusal(uam::provider_ids::kGeminiCli, "local", uam::account_refusal::Kind::UnsupportedClient, kGeminiNotice);
	const std::uint64_t generation = app.provider_model_catalog->Generation();
	// Retrying a provider with nothing recorded does not change catalogue state.
	UAM_ASSERT(!app.provider_model_catalog->ClearAccountRefusal(uam::provider_ids::kCodexCli, "local"));
	UAM_ASSERT_EQ(app.provider_model_catalog->Generation(), generation);
	UAM_ASSERT(app.provider_model_catalog->ClearAccountRefusal(uam::provider_ids::kGeminiCli, "local"));
	UAM_ASSERT(!app.provider_model_catalog->IsAccountRefusalActive(uam::provider_ids::kGeminiCli, "local"));
	UAM_ASSERT(app.provider_model_catalog->GetProviderRefreshError(uam::provider_ids::kGeminiCli, workspace, "local").empty());
	uam::ProviderModelCatalogService restarted;
	restarted.Initialize(app.data_root, app.provider_profiles);
	UAM_ASSERT(restarted.GetAccountRefusalNotice(uam::provider_ids::kGeminiCli, "local").empty());

	// A later successful answer on the same computer also lifts a recorded refusal.
	app.provider_model_catalog->RememberAccountRefusal(uam::provider_ids::kGeminiCli, "local", uam::account_refusal::Kind::Authentication, "Gemini CLI needs you to sign in again on this computer.");
	UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels(uam::provider_ids::kGeminiCli, nlohmann::json::array({{{"id", "gemini-fixture"}}}), workspace));
	UAM_ASSERT(app.provider_model_catalog->GetAccountRefusalNotice(uam::provider_ids::kGeminiCli, "local").empty());
}

UAM_TEST(ChatLaunchIsRefusedWithTheNoticeWhileTheAccountRefusalIsActive)
{
	AccountRefusalFixture fixture;
	uam::AppState& app = fixture.app;
	ChatSession chat;
	chat.id = "gemini-chat-account-refusal";
	chat.provider_id = uam::provider_ids::kGeminiCli;
	chat.execution_host_id = "local";
	chat.workspace_directory = app.folders.front().directory;
	app.chats.push_back(chat);
	app.provider_model_catalog->RememberAccountRefusal(uam::provider_ids::kGeminiCli, "local", uam::account_refusal::Kind::UnsupportedClient, kGeminiNotice);
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	std::string error;
	UAM_ASSERT(!uam::acp_detail::StartAcpProcessForChat(app, session, app.chats.front(), &error));
	UAM_ASSERT(error.starts_with(kGeminiNotice));
	UAM_ASSERT(uam::strings::Contains(error, "Retry"));
	UAM_ASSERT(!session.running);
}

UAM_TEST(AuthenticationRefusalHoldsDiscoveryButNotChatLaunches)
{
	AccountRefusalFixture fixture;
	uam::AppState& app = fixture.app;
	// Copilot and other ACP providers answer a signed-out account with -32000 "Authentication required".
	UAM_ASSERT(uam::account_refusal::ClassifyAcpError("session/new", true, -32000, "Authentication required") == uam::account_refusal::Kind::Authentication);
	app.provider_model_catalog->RememberAccountRefusal(uam::provider_ids::kGeminiCli, "local", uam::account_refusal::Kind::Authentication, "Gemini CLI needs you to sign in again on this computer.");
	UAM_ASSERT(app.provider_model_catalog->IsAccountRefusalActive(uam::provider_ids::kGeminiCli, "local"));
	UAM_ASSERT(!app.provider_model_catalog->IsAccountRefusalActive(uam::provider_ids::kGeminiCli, "local", true));
	UAM_ASSERT(!app.provider_model_catalog->BeginDiscoveryIfStale(uam::provider_ids::kGeminiCli, app.folders.front().directory, "local"));
	// A chat launch it let through that fails for its own reason shows that reason, not the old sign-in line.
	app.provider_model_catalog->RememberRefreshFailure(uam::provider_ids::kGeminiCli, "Workspace directory is not trusted.", app.folders[1].directory, "local");
	UAM_ASSERT(app.provider_model_catalog->GetAccountRefusalNotice(uam::provider_ids::kGeminiCli, "local").empty());
	UAM_ASSERT_EQ(app.provider_model_catalog->GetProviderRefreshError(uam::provider_ids::kGeminiCli, app.folders[1].directory, "local"), std::string("Workspace directory is not trusted."));
	UAM_ASSERT(app.provider_model_catalog->RememberAccountRefusal(uam::provider_ids::kGeminiCli, "local", uam::account_refusal::Kind::UnsupportedClient, kGeminiNotice));
	UAM_ASSERT(app.provider_model_catalog->IsAccountRefusalActive(uam::provider_ids::kGeminiCli, "local", true));
	UAM_ASSERT_EQ(app.provider_model_catalog->GetChatLaunchRefusalNotice(uam::provider_ids::kGeminiCli, "local"), std::string(kGeminiNotice));
	// A client refusal is not replaced by an ordinary workspace failure while it lasts.
	app.provider_model_catalog->RememberRefreshFailure(uam::provider_ids::kGeminiCli, "Workspace directory is not trusted.", app.folders[1].directory, "local");
	UAM_ASSERT_EQ(app.provider_model_catalog->GetAccountRefusalNotice(uam::provider_ids::kGeminiCli, "local"), std::string(kGeminiNotice));
}

UAM_TEST(ExpiredAccountRefusalGivesWayToTheNextAttemptsOwnError)
{
	AccountRefusalFixture fixture;
	uam::AppState& app = fixture.app;
	const std::string workspace = app.folders.front().directory;
	const std::int64_t now = uam::time::TimestampNowSec();
	app.provider_model_catalog->RememberAccountRefusal(uam::provider_ids::kGeminiCli, "local", uam::account_refusal::Kind::UnsupportedClient, kGeminiNotice,
	    now - uam::account_refusal::kAccountRefusalTtlSeconds - 1);
	UAM_ASSERT(app.provider_model_catalog->BeginDiscovery(uam::provider_ids::kGeminiCli, workspace, "local"));
	app.provider_model_catalog->RememberRefreshFailure(uam::provider_ids::kGeminiCli, "Workspace directory is not trusted.", workspace, "local");
	UAM_ASSERT(app.provider_model_catalog->GetAccountRefusalNotice(uam::provider_ids::kGeminiCli, "local").empty());
	UAM_ASSERT_EQ(app.provider_model_catalog->GetProviderRefreshError(uam::provider_ids::kGeminiCli, workspace, "local"), std::string("Workspace directory is not trusted."));
	// A refusal dated in the future (a clock moved back) is never held longer than its TTL from now.
	app.provider_model_catalog->RememberAccountRefusal(uam::provider_ids::kGeminiCli, "local", uam::account_refusal::Kind::UnsupportedClient, kGeminiNotice,
	    now + 10 * uam::account_refusal::kAccountRefusalTtlSeconds);
	UAM_ASSERT(!app.provider_model_catalog->IsAccountRefusalActive(uam::provider_ids::kGeminiCli, "local"));
}
