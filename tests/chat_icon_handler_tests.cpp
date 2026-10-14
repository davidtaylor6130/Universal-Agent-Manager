#include "test_harness.h"
#include "cef/uam_query_handler.h"
#include "app/agent_definition_service.h"
#include "app/computer_use_service.h"
#include "computer_use_terminal_runtime_fixture.h"
#include "include/cef_app.h"
#include "include/cef_task.h"
#include "common/runtime/acp/acp_session_internal.h"
#if defined(__APPLE__)
#include "common/platform/platform_application_macos.h"
#endif

using namespace uam_test;

/// Executes the real icon handler and the scalar identity seam shared by CEF routing.
struct ChatIconHandlerTestAccess
{
	static void Set(UamQueryHandler& handler, CefRefPtr<UamQueryHandler::Callback> callback,
	    const std::string& chat_id, const std::string& value, int browser = 1, std::string frame = "frame", int64_t query = 1)
	{
		handler.BeginChatIconQuery(callback.get(), browser, frame, query);
		handler.HandleSetCustomIcon(nullptr, {{"targetType", "chat"}, {"targetId", chat_id},
		    {"icon", {{"type", "text"}, {"value", value}}}}, callback);
		handler.m_chatIconQueryIdentities.erase(callback.get());
	}
	static void Create(UamQueryHandler& handler, const nlohmann::json& payload, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		handler.HandleCreateSession(nullptr, payload, callback);
	}
	static void Retry(UamQueryHandler& handler, const std::string& chat_id, int index, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		handler.HandleRetryFailedMessage(nullptr, {{"chatId", chat_id}, {"messageIndex", index}}, callback);
	}
	static void ComputerUseEnabled(UamQueryHandler& handler, const std::string& chat_id, bool enabled, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		handler.HandleSetChatComputerUseEnabled(nullptr, {{"chatId", chat_id}, {"enabled", enabled}}, callback);
	}
	static void ComputerUseSettings(UamQueryHandler& handler, bool enabled, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		handler.HandleSetComputerUseSettings(nullptr, {{"computerUseGloballyEnabled", enabled}}, callback);
	}
	static void ImportAgent(UamQueryHandler& handler, const nlohmann::json& payload, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		handler.HandleImportProviderAgent(nullptr, payload, callback);
	}

	static void Cancel(UamQueryHandler& handler, int browser, const std::string& frame, int64_t query)
	{
		handler.CancelChatIconQuery(browser, frame, query);
	}
};

namespace
{
	class RecordingIconCallback final : public UamQueryHandler::Callback
	{
	  public:
		int successes = 0, failures = 0, last_failure = 0;
		std::string failure_message;
		void Success(const CefString&) override { ++successes; }
		void Success(const void*, size_t) override { ++successes; }
		void Failure(int code, const CefString& message) override { ++failures; last_failure = code; failure_message = message.ToString(); }
	  private:
		IMPLEMENT_REFCOUNTING(RecordingIconCallback);
	};

	ChatSession HandlerChat(const std::string& id)
	{
		ChatSession chat;
		chat.id = id; chat.provider_id = "codex-cli"; chat.title = "Private handler fixture";
		chat.created_at = chat.updated_at = "2026-10-13T00:00:00Z";
		chat.custom_icon = {"text", "O"}; chat.messages.push_back({.role = MessageRole::User, .content = "Retained"});
		return chat;
	}

	void DrainIconHandlerSaves(uam::AppState& app)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!app.pending_chat_save_at_by_chat_id.empty() && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
	}
}

UAM_TEST(ChatIconHandlerLatestFailureRollsBackExactlyOnce)
{
	TempDir temp("uam-icon-handler-failure");
	uam::AppState app; app.data_root = temp.root; app.chats.push_back(HandlerChat("chat"));
	UamQueryHandler handler(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Set(handler, callback, "chat", "A");
	UAM_ASSERT_EQ(app.chats.front().custom_icon.value, std::string("A"));
	uam::ResolvePendingChatSaveResults(app, "chat", false);
	UAM_ASSERT_EQ(app.chats.front().custom_icon.value, std::string("O"));
	UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->last_failure, 500);
	uam::ResolvePendingChatSaveResults(app, "chat", false);
	DrainIconHandlerSaves(app); UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->successes, 0);
}

UAM_TEST(ChatIconHandlerSupersededCancellationCannotRollbackNewerIntent)
{
	TempDir temp("uam-icon-handler-superseded");
	uam::AppState app; app.data_root = temp.root; app.chats.push_back(HandlerChat("chat"));
	UamQueryHandler handler(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> first = new RecordingIconCallback, second = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Set(handler, first, "chat", "A", 1, "frame", 1);
	ChatIconHandlerTestAccess::Set(handler, second, "chat", "B", 1, "frame", 2);
	handler.OnQueryCanceled(nullptr, nullptr, 1); // A different CEF scope cannot cancel either request.
	ChatIconHandlerTestAccess::Cancel(handler, 1, "frame", 1);
	UAM_ASSERT_EQ(app.chats.front().custom_icon.value, std::string("B"));
	DrainIconHandlerSaves(app);
	UAM_ASSERT_EQ(first->successes + first->failures, 0); UAM_ASSERT_EQ(second->successes, 1);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, "chat")->custom_icon.value, std::string("B"));
}

UAM_TEST(ChatIconHandlerMatchingCefCancellationRollsBackWithoutCallback)
{
	TempDir temp("uam-icon-handler-cancel");
	uam::AppState app; app.data_root = temp.root; app.chats.push_back(HandlerChat("chat"));
	UamQueryHandler handler(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Set(handler, callback, "chat", "A", -1, "", 7);
	handler.OnQueryCanceled(nullptr, nullptr, 7);
	UAM_ASSERT_EQ(app.chats.front().custom_icon.value, std::string("O"));
	handler.OnQueryCanceled(nullptr, nullptr, 7); DrainIconHandlerSaves(app);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, "chat")->custom_icon.value, std::string("O"));
}

UAM_TEST(ChatIconHandlerReusedQueryIdIsScopedByBrowserAndFrame)
{
	TempDir temp("uam-icon-handler-query-scope");
	uam::AppState app; app.data_root = temp.root;
	for (const std::string id : {"one", "two", "three"}) app.chats.push_back(HandlerChat(id));
	UamQueryHandler handler(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> one = new RecordingIconCallback, two = new RecordingIconCallback, three = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Set(handler, one, "one", "A", 1, "f1", 7);
	ChatIconHandlerTestAccess::Set(handler, two, "two", "B", 2, "f1", 7);
	ChatIconHandlerTestAccess::Set(handler, three, "three", "C", 1, "f2", 7);
	ChatIconHandlerTestAccess::Cancel(handler, 1, "f1", 7);
	UAM_ASSERT_EQ(app.chats[0].custom_icon.value, std::string("O"));
	UAM_ASSERT_EQ(app.chats[1].custom_icon.value, std::string("B")); UAM_ASSERT_EQ(app.chats[2].custom_icon.value, std::string("C"));
	DrainIconHandlerSaves(app);
	UAM_ASSERT_EQ(one->successes + one->failures, 0); UAM_ASSERT_EQ(two->successes, 1); UAM_ASSERT_EQ(three->successes, 1);
}

UAM_TEST(ChatIconHandlerRootAndIncarnationChangesProtectReplacement)
{
	for (const bool root_switch : {false, true})
	{
		TempDir temp("uam-icon-handler-owner");
		uam::AppState app; app.data_root = temp.root / "first"; app.chats.push_back(HandlerChat("chat"));
		UamQueryHandler handler(app, "file:///private/index.html");
		CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
		ChatIconHandlerTestAccess::Set(handler, callback, "chat", "A");
		if (root_switch) app.data_root = temp.root / "second";
		else { app.chats.front() = HandlerChat("chat"); app.chats.front().custom_icon = {"text", "A"}; }
		uam::ResolvePendingChatSaveResults(app, "chat", false);
		UAM_ASSERT_EQ(app.chats.front().custom_icon.value, std::string("A"));
		UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->last_failure, 409);
		DrainIconHandlerSaves(app); UAM_ASSERT_EQ(callback->failures, 1);
	}
}

UAM_TEST(ChatIconHandlerDestructionSettlesReceiptWithoutDanglingAccess)
{
	TempDir temp("uam-icon-handler-lifetime");
	uam::AppState app; app.data_root = temp.root; app.chats.push_back(HandlerChat("chat"));
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	{
		UamQueryHandler handler(app, "file:///private/index.html");
		ChatIconHandlerTestAccess::Set(handler, callback, "chat", "A");
	}
	uam::ResolvePendingChatSaveResults(app, "chat", false);
	UAM_ASSERT_EQ(app.chats.front().custom_icon.value, std::string("A"));
	UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->last_failure, 409);
	DrainIconHandlerSaves(app); UAM_ASSERT_EQ(callback->failures, 1);
}

namespace
{
	class CreationCefApp final : public CefApp, public CefBrowserProcessHandler
	{
	public:
		bool context_ready = false;
		CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
		void OnContextInitialized() override { context_ready = true; }
		void OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> command) override
		{
			command->AppendSwitch("disable-background-networking");
			command->AppendSwitch("use-mock-keychain");
			command->AppendSwitchWithValue("password-store", "basic");
			command->AppendSwitch("disable-gpu");
		}
	private:
		IMPLEMENT_REFCOUNTING(CreationCefApp);
	};

	void RunCreationLoopUntil(const std::function<bool()>& complete, std::chrono::seconds timeout)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
		while (!complete() && std::chrono::steady_clock::now() < deadline)
		{
			CefDoMessageLoopWork();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	class CreationQueueWitness final : public CefTask
	{
		std::shared_ptr<std::atomic<int>> m_observed;
		int m_bit;
	public:
		CreationQueueWitness(std::shared_ptr<std::atomic<int>> observed, int bit) : m_observed(std::move(observed)), m_bit(bit) {}
		void Execute() override { m_observed->fetch_or(m_bit); }
	private:
		IMPLEMENT_REFCOUNTING(CreationQueueWitness);
	};

	class CreationCefRuntime
	{
		TempDir m_private{"uam-create-cef-runtime"};
	public:
		CreationCefRuntime(const CefMainArgs& arguments)
		{
#if defined(__APPLE__)
			UAM_ASSERT(uam::platform::InitializeMacApplication());
#endif
			CefSettings settings;
			settings.no_sandbox = true;
			settings.external_message_pump = true;
			CefString(&settings.root_cache_path) = (m_private.root / "cef").string();
			CefString(&settings.cache_path) = (m_private.root / "cef").string();
			CefString(&settings.log_file) = (m_private.root / "cef.log").string();
#if defined(__APPLE__)
			const std::optional<std::string> bundle = uam::env::GetNonEmptyString("UAM_TEST_CEF_APP_BUNDLE");
			UAM_ASSERT(bundle.has_value());
			const fs::path app = uam::paths::PathFromUtf8(*bundle);
			const fs::path frameworks = app / "Contents" / "Frameworks";
			const fs::path helper = frameworks / "universal_agent_manager Helper.app" / "Contents" / "MacOS" / "universal_agent_manager Helper";
			UAM_ASSERT(fs::is_regular_file(helper));
			CefString(&settings.main_bundle_path) = app.string();
			CefString(&settings.framework_dir_path) = (frameworks / "Chromium Embedded Framework.framework").string();
			CefString(&settings.browser_subprocess_path) = helper.string();
#endif
			CefRefPtr<CreationCefApp> cef_app = new CreationCefApp;
			UAM_ASSERT(CefInitialize(arguments, settings, cef_app, nullptr));
			RunCreationLoopUntil([cef_app]() { return cef_app->context_ready; }, std::chrono::seconds(10));
			if (!cef_app->context_ready)
			{
				CefShutdown();
				throw std::runtime_error("Private CEF context did not initialize before the deadline.");
			}
			const std::shared_ptr<std::atomic<int>> observed = std::make_shared<std::atomic<int>>(0);
			UAM_ASSERT(CefPostTask(TID_UI, new CreationQueueWitness(observed, 1)));
			UAM_ASSERT(CefPostTask(TID_FILE_USER_BLOCKING, new CreationQueueWitness(observed, 2)));
			RunCreationLoopUntil([observed]() { return observed->load() == 3; }, std::chrono::seconds(5));
			std::cout << "CEF_QUEUE_WITNESS ui=" << ((observed->load() & 1) != 0) << " background=" << ((observed->load() & 2) != 0) << std::endl;
			if (observed->load() != 3)
			{
				CefShutdown();
				throw std::runtime_error("Private CEF UI/background queues did not execute before the deadline.");
			}
		}
		~CreationCefRuntime() { CefShutdown(); }
	};

	std::string GitState(const fs::path& repo, const std::string& arguments)
	{
		const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
			"git -C " + ShellQuoteForTest(repo.string()) + " " + arguments, 20000);
		UAM_ASSERT(result.ok && !result.timed_out && !result.canceled && result.exit_code == 0 && !result.output_truncated);
		return result.output;
	}

	struct WorktreeCreationFixture
	{
		TempDir temp{"uam-connected-worktree-create"};
		fs::path repository = temp.root / "repository";
		uam::AppState app;
		WorktreeCreationFixture()
		{
			UAM_ASSERT(GitAvailableForTests());
			fs::create_directories(repository);
			UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repository.string())));
			UAM_ASSERT(RunGitForTest(repository, "config user.email uam@example.test"));
			UAM_ASSERT(RunGitForTest(repository, "config user.name UAM"));
			UAM_ASSERT(uam::io::WriteTextFile(repository / "source.txt", "source survivor\n"));
			UAM_ASSERT(RunGitForTest(repository, "add source.txt"));
			UAM_ASSERT(RunGitForTest(repository, "commit -m base"));
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			app.settings.git_enabled = app.settings.git_auto_worktree = true;
			app.folders.push_back({.id = "folder", .title = "Private repo", .directory = repository.string(), .execution_host_id = "local"});
			ChatSession source = HandlerChat("source");
			source.folder_id = "folder"; source.execution_host_id = "local";
			source.workspace_directory = repository.string(); source.native_session_id = "source-native";
			Goal source_goal; source_goal.id = "source-goal"; source_goal.objective = "Retained source objective";
			source.goals.push_back(std::move(source_goal));
			app.chats.push_back(std::move(source));
			UAM_ASSERT(uam::GitWorktreeService().CreateForChat(app, app.chats.front()).ok);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
		}
		nlohmann::json Payload() const
		{
			return {{"folderId", "folder"}, {"providerId", "codex-cli"}, {"routingMode", "explicit"},
				{"executionHostId", "local"}, {"sourceChatId", "source"},
				{"expectedWorkspaceDirectory", app.chats.front().workspace_worktree_directory}, {"expectedExecutionHostId", "local"}};
		}
		void Await(const CefRefPtr<RecordingIconCallback>& callback)
		{
			RunCreationLoopUntil([callback]() { return callback->successes + callback->failures != 0; }, std::chrono::seconds(20));
			UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
		}
	};
}

UAM_TEST(ChatCreateSelectedWorktreeIsEmptyAndPreservesLegacyCreation)
{
	WorktreeCreationFixture fixture;
	const ChatSession source = fixture.app.chats.front();
	const std::string source_metadata = ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, source);
	const std::string source_bytes = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, source.id));
	const std::string refs = GitState(fixture.repository, "show-ref");
	const std::string worktrees = GitState(fixture.repository, "worktree list --porcelain");
	UamQueryHandler handler(fixture.app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Create(handler, fixture.Payload(), callback);
	fixture.Await(callback);
	UAM_ASSERT_EQ(callback->successes, 1); UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
	const ChatSession& created = fixture.app.chats.back();
	UAM_ASSERT(created.id != source.id && created.messages.empty() && created.goals.empty());
	UAM_ASSERT(created.native_session_id.empty() && created.custom_icon.type.empty());
	UAM_ASSERT_EQ(created.workspace_directory, source.workspace_worktree_directory);
	UAM_ASSERT_EQ(created.workspace_isolation_kind, source.workspace_isolation_kind);
	UAM_ASSERT_EQ(created.workspace_source_directory, source.workspace_source_directory);
	UAM_ASSERT_EQ(created.workspace_branch_name, source.workspace_branch_name);
	UAM_ASSERT_EQ(created.workspace_worktree_directory, source.workspace_worktree_directory);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.front().content, source.messages.front().content);
	UAM_ASSERT_EQ(fixture.app.chats.front().native_session_id, source.native_session_id);
	UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, fixture.app.chats.front()), source_metadata);
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, source.id)), source_bytes);
	UAM_ASSERT_EQ(GitState(fixture.repository, "show-ref"), refs);
	UAM_ASSERT_EQ(GitState(fixture.repository, "worktree list --porcelain"), worktrees);
	fixture.app.settings.git_auto_worktree = false;
	CefRefPtr<RecordingIconCallback> legacy = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Create(handler, {{"folderId", "folder"}, {"providerId", "codex-cli"}}, legacy);
	fixture.Await(legacy);
	UAM_ASSERT_EQ(legacy->successes, 1); UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{3});
	UAM_ASSERT_EQ(fixture.app.chats.back().workspace_directory, fixture.repository.string());
	UAM_ASSERT(fixture.app.chats.back().messages.empty());
	UAM_ASSERT_EQ(GitState(fixture.repository, "worktree list --porcelain"), worktrees);
}

UAM_TEST(ChatCreateSelectedWorktreeRejectsStaleSourceScope)
{
	for (int mutation = 0; mutation < 4; ++mutation)
	{
		WorktreeCreationFixture fixture;
		nlohmann::json payload = fixture.Payload();
		UamQueryHandler handler(fixture.app, "file:///private/index.html");
		CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
		if (mutation == 0) payload["sourceChatId"] = "missing";
		else if (mutation == 1) fixture.app.chats.front().folder_id = "other-folder";
		else if (mutation == 2) payload["expectedExecutionHostId"] = "other-host";
		ChatIconHandlerTestAccess::Create(handler, payload, callback);
		if (mutation == 3) fixture.app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		fixture.Await(callback);
		UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	}
}

UAM_TEST(ChatCreateSelectedWorktreeRejectsForeignCommonDirectory)
{
	WorktreeCreationFixture fixture;
	const fs::path foreign = fixture.temp.root / "foreign";
	fs::create_directories(foreign);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(foreign.string())));
	UAM_ASSERT(RunGitForTest(foreign, "-c user.name=UAM -c user.email=uam@example.test commit --allow-empty -m foreign"));
	const std::string refs = GitState(fixture.repository, "show-ref");
	const std::string foreign_refs = GitState(foreign, "show-ref");
	fixture.app.chats.front().workspace_directory = foreign.string();
	fixture.app.chats.front().workspace_worktree_directory = foreign.string();
	UamQueryHandler handler(fixture.app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Create(handler, fixture.Payload(), callback);
	fixture.Await(callback);
	UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(GitState(fixture.repository, "show-ref"), refs);
	UAM_ASSERT_EQ(GitState(foreign, "show-ref"), foreign_refs);
}

UAM_TEST(ChatCreateSelectedWorktreeRejectsBranchMismatchWithoutMutation)
{
	WorktreeCreationFixture fixture;
	const std::string refs = GitState(fixture.repository, "show-ref");
	const std::string worktrees = GitState(fixture.repository, "worktree list --porcelain");
	fixture.app.chats.front().workspace_branch_name = "foreign-unowned-branch";
	UamQueryHandler handler(fixture.app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Create(handler, fixture.Payload(), callback);
	fixture.Await(callback);
	UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(GitState(fixture.repository, "show-ref"), refs);
	UAM_ASSERT_EQ(GitState(fixture.repository, "worktree list --porcelain"), worktrees);
}

namespace
{
	struct ColdRetryFixture
	{
		TempDir temp{"uam-connected-cold-retry"};
		uam::AppState app;
		int user_index = 200;
		explicit ColdRetryFixture(bool assistant_output = false)
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatSession chat = HandlerChat("cold-retry");
			chat.branch_root_chat_id = "original";
			chat.execution_host_id = "missing-host";
			chat.workspace_directory = temp.root.string();
			chat.memory_enabled = false;
			chat.messages.clear();
			for (int index = 0; index < user_index; ++index)
				chat.messages.push_back({.role = index % 2 ? MessageRole::Assistant : MessageRole::User,
					.content = "Retained history " + std::to_string(index)});
			chat.messages.push_back({.role = MessageRole::User, .content = "Retry original unsent prompt", .acp_prompt_not_sent = true});
			if (assistant_output) chat.messages.push_back({.role = MessageRole::Assistant, .content = "Already answered"});
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			std::optional<ChatSession> summary = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, chat.id);
			UAM_ASSERT(summary && !summary->messages_loaded && summary->messages.empty());
			app.chats.push_back(std::move(*summary));
		}
		void Await(const CefRefPtr<RecordingIconCallback>& callback)
		{
			RunCreationLoopUntil([callback]() { return callback->successes + callback->failures != 0; }, std::chrono::seconds(10));
			UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
		}
	};
}

UAM_TEST(ChatRetryColdLatestUserLoadsHistoryWithoutDuplicatingPrompt)
{
	ColdRetryFixture fixture;
	UamQueryHandler handler(fixture.app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	ChatIconHandlerTestAccess::Retry(handler, "cold-retry", fixture.user_index, callback);
	UAM_ASSERT(std::chrono::steady_clock::now() - started <= std::chrono::milliseconds(100));
	fixture.Await(callback);
	UAM_ASSERT_EQ(callback->failure_message, std::string("The selected execution host no longer exists."));
	UAM_ASSERT(fixture.app.chats.front().messages_loaded);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{201});
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.back().content, std::string("Retry original unsent prompt"));
	UAM_ASSERT(fixture.app.chats.front().messages.back().acp_prompt_not_sent);
}

UAM_TEST(ChatRetryColdPreparationCancellationLeavesHistoryUnloaded)
{
	ColdRetryFixture fixture;
	const std::string original = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, "cold-retry"));
	UamQueryHandler handler(fixture.app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Retry(handler, "cold-retry", fixture.user_index, callback);
	UAM_ASSERT(callback->failures == 0 && callback->successes == 0);
	ChatDomainService().CancelBranchPreparations(fixture.app, "cold-retry");
	fixture.Await(callback);
	UAM_ASSERT_EQ(callback->failures, 1);
	UAM_ASSERT(!fixture.app.chats.front().messages_loaded && fixture.app.chats.front().messages.empty());
	UAM_ASSERT(fixture.app.acp_sessions.empty());
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, "cold-retry")), original);
}

UAM_TEST(ChatRetryColdHistoryStillRejectsLaterAssistantOutput)
{
	ColdRetryFixture fixture(true);
	UamQueryHandler handler(fixture.app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Retry(handler, "cold-retry", fixture.user_index, callback);
	fixture.Await(callback);
	UAM_ASSERT(fixture.app.chats.front().messages_loaded);
	UAM_ASSERT_EQ(callback->failure_message, std::string("The failed message is no longer the latest message."));
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{202});
	UAM_ASSERT(fixture.app.acp_sessions.empty());
}


UAM_TEST(ComputerUseHandlerOffWriteFailureCannotRestoreConsent)
{
	TempDir temp("uam-cu-handler-revoke-write");
	uam::AppState app; app.data_root = temp.root;
	ChatSession chat = HandlerChat("cu-write-failure");
	chat.computer_use_backend = "uam"; chat.computer_use_enabled = true;
	app.chats.push_back(chat);
	const fs::path directory = temp.root / "computer-use" / chat.id;
	fs::create_directories(directory);
	const fs::path backing = directory / "stale-control.json";
	UAM_ASSERT(uam::io::WriteTextFile(backing, "{\"state\":\"armed\"}\n"));
	// Readable stale grant, but atomic writes reject a symlink destination.
	fs::create_symlink(backing, directory / "control.json");
	UamQueryHandler handler(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ComputerUseEnabled(handler, chat.id, false, callback);
	UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->successes, 0);
	UAM_ASSERT(!app.chats.front().computer_use_enabled);
	(void)uam::ComputerUseService::Poll(app);
	UAM_ASSERT(!app.chats.front().computer_use_enabled);
	UAM_ASSERT_EQ(ReadFile(backing), std::string("{\"state\":\"armed\"}\n"));
	UAM_ASSERT(app.computer_use_revocation_pending_chat_ids.contains(chat.id));
	CefRefPtr<RecordingIconCallback> blocked = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ComputerUseEnabled(handler, chat.id, true, blocked);
	UAM_ASSERT_EQ(blocked->failures, 1); UAM_ASSERT_EQ(blocked->successes, 0);
	UAM_ASSERT(fs::remove(directory / "control.json"));
	CefRefPtr<RecordingIconCallback> retried = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ComputerUseEnabled(handler, chat.id, false, retried);
	UAM_ASSERT_EQ(retried->successes, 1); UAM_ASSERT_EQ(retried->failures, 0);
	UAM_ASSERT(!app.computer_use_revocation_pending_chat_ids.contains(chat.id));
	CefRefPtr<RecordingIconCallback> enabled = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ComputerUseEnabled(handler, chat.id, true, enabled);
	UAM_ASSERT_EQ(enabled->successes, 1); UAM_ASSERT_EQ(enabled->failures, 0);
	for (const std::string state : {"armed", "running", "paused"})
	{
		TempDir map_temp("uam-cu-handler-map-owner");
		uam::AppState map_app; map_app.data_root = map_temp.root;
		map_app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		map_app.chats.push_back(HandlerChat("map-owned"));
		map_app.chats.push_back(HandlerChat("map-ordinary"));
		for (const ChatSession& persisted : map_app.chats) UAM_ASSERT(ChatRepository::SaveChat(map_app.data_root, persisted));
		map_app.computer_use_by_chat_id["map-owned"].state = state;
		ComputerUseTerminalRuntimeFixture runtime;
		runtime.AddTerminal(map_app, "map-owned"); runtime.AddTerminal(map_app, "map-ordinary");
		UamQueryHandler map_handler(map_app, "file:///private/index.html");
		CefRefPtr<RecordingIconCallback> map_off = new RecordingIconCallback;
		ChatIconHandlerTestAccess::ComputerUseSettings(map_handler, false, map_off);
		UAM_ASSERT_EQ(runtime.StopCount("map-owned"), 1);
		UAM_ASSERT_EQ(runtime.StopCount("map-ordinary"), 0);
		UAM_ASSERT_EQ(map_off->failures, 1); UAM_ASSERT_EQ(map_off->successes, 0);
		UAM_ASSERT(map_app.computer_use_revocation_pending_chat_ids.contains("map-owned"));
		runtime.ConfirmExit("map-owned");
		CefRefPtr<RecordingIconCallback> map_retry = new RecordingIconCallback;
		ChatIconHandlerTestAccess::ComputerUseSettings(map_handler, false, map_retry);
		UAM_ASSERT_EQ(runtime.StopCount("map-owned"), 2);
		UAM_ASSERT_EQ(runtime.StopCount("map-ordinary"), 0);
		UAM_ASSERT_EQ(map_retry->successes, 1); UAM_ASSERT_EQ(map_retry->failures, 0);
		UAM_ASSERT(map_app.computer_use_revocation_pending_chat_ids.empty());
	}
}

UAM_TEST(ComputerUseHandlerGlobalOffRetriesOwnedTerminal)
{
	TempDir temp("uam-cu-handler-terminal-retry");
	uam::AppState app; app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession controlled = HandlerChat("cu-owned"); controlled.computer_use_enabled = true;
	app.chats.push_back(controlled); app.chats.push_back(HandlerChat("ordinary"));
	for (const ChatSession& chat : app.chats) UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	// The linked test runtime retains real history synchronization without OS terminals.
	ComputerUseTerminalRuntimeFixture runtime;
	runtime.AddTerminal(app, "cu-owned"); runtime.AddTerminal(app, "ordinary");
	UamQueryHandler handler(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> ordinary_off = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ComputerUseEnabled(handler, "ordinary", false, ordinary_off);
	UAM_ASSERT_EQ(runtime.StopCount("ordinary"), 0);
	UAM_ASSERT_EQ(ordinary_off->successes, 1); UAM_ASSERT_EQ(ordinary_off->failures, 0);
	UAM_ASSERT(!app.computer_use_revocation_pending_chat_ids.contains("ordinary"));
	for (int attempt = 1; attempt <= 2; ++attempt)
	{
		CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
		ChatIconHandlerTestAccess::ComputerUseSettings(handler, false, callback);
		UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->successes, 0);
		UAM_ASSERT_EQ(runtime.StopCount("cu-owned"), attempt);
		UAM_ASSERT_EQ(runtime.StopCount("ordinary"), 0);
		UAM_ASSERT(!app.settings.computer_use_globally_enabled);
		UAM_ASSERT(!app.chats.empty());
		UAM_ASSERT(!app.chats.front().computer_use_enabled);
	}
	CefRefPtr<RecordingIconCallback> blocked = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ComputerUseSettings(handler, true, blocked);
	UAM_ASSERT_EQ(blocked->failures, 1); UAM_ASSERT_EQ(blocked->successes, 0);
	UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(app.data_root, "cu-owned").Failed());
	std::erase_if(app.chats, [](const ChatSession& chat) { return chat.id == "cu-owned"; });
	runtime.ConfirmExit("cu-owned");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ComputerUseSettings(handler, false, callback);
	UAM_ASSERT_EQ(callback->successes, 1); UAM_ASSERT_EQ(callback->failures, 0);
	UAM_ASSERT_EQ(runtime.StopCount("cu-owned"), 3);
	UAM_ASSERT_EQ(runtime.StopCount("ordinary"), 0);
	UAM_ASSERT_EQ(app.cli_terminals.size(), std::size_t{1});
	UAM_ASSERT(app.computer_use_revocation_pending_chat_ids.empty());
	CefRefPtr<RecordingIconCallback> enabled = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ComputerUseSettings(handler, true, enabled);
	UAM_ASSERT_EQ(enabled->successes, 1); UAM_ASSERT_EQ(enabled->failures, 0);
}

int main(int argc, char** argv)
{
#if defined(__APPLE__)
	if (const std::optional<int> watchdog_result = uam::platform::RunMacParentDeathWatchdogIfRequested(argc, argv); watchdog_result.has_value())
	{
		return *watchdog_result;
	}
#endif
#if defined(_WIN32)
	CefMainArgs main_args(GetModuleHandle(nullptr));
#else
	CefMainArgs main_args(argc, argv);
#endif
	const int subprocess = CefExecuteProcess(main_args, new CreationCefApp, nullptr);
	if (subprocess >= 0) return subprocess;
	std::unique_ptr<CreationCefRuntime> cef;
	bool initialization_attempted = false;
	std::size_t selected = 0, passed = 0, icon_passed = 0, create_passed = 0, retry_passed = 0;
	std::size_t icon_selected = 0, create_selected = 0, retry_selected = 0;
	for (const TestCase& test : Registry())
	{
		bool requested = argc <= 1;
		for (int index = 1; !requested && index < argc; ++index) requested = test.name == argv[index];
		if (!requested) continue;
		++selected;
		const bool icon = test.name.rfind("ChatIcon", 0) == 0;
		const bool creation = test.name.rfind("ChatCreate", 0) == 0;
		const bool retry = test.name.rfind("ChatRetry", 0) == 0;
		if (icon) ++icon_selected;
		if (creation) ++create_selected;
		if (retry) ++retry_selected;
		try
		{
			if ((creation || retry) && !initialization_attempted)
			{
				initialization_attempted = true;
				cef = std::make_unique<CreationCefRuntime>(main_args);
			}
			if (creation || retry) UAM_ASSERT(cef != nullptr);
			test.fn(); ++passed;
			if (creation) ++create_passed; else if (retry) ++retry_passed; else if (icon) ++icon_passed;
			std::cout << "PASS " << test.name << '\n';
		}
		catch (const std::exception& error) { std::cerr << "FAIL " << test.name << ": " << error.what() << '\n'; }
	}
	std::cout << icon_passed << '/' << icon_selected << " connected icon cases passed\n" << create_passed << '/' << create_selected << " connected create cases passed\n" << retry_passed << '/' << retry_selected << " connected retry cases passed\n";
	std::cout << passed << '/' << selected << " selected connected cases passed\n";
	cef.reset();
	return selected != 0 && passed == selected && (argc <= 1 || selected == static_cast<std::size_t>(argc - 1)) ? 0 : 1;
}

UAM_TEST(ChatAgentImportGlobalWithoutChatRetainsSelectedSkills)
{
	TempDir temp("uam-handler-agent-global");
	uam::AppState app;
	app.data_root = temp.root / "data";
	const fs::path source = temp.root / "agent.md";
	UAM_ASSERT(uam::io::WriteTextFile(source, "---\ndescription: Reviews code\n---\nReview.\n"));
	UamQueryHandler handler(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ImportAgent(handler,
	    {{"providerId", "opencode"}, {"sourcePath", source.string()}, {"workspaceAccess", "read"},
	     {"workspaceScope", false}, {"skills", {"review", "docs"}}}, callback);
	UAM_ASSERT_EQ(callback->failures, 0);
	UAM_ASSERT_EQ(callback->successes, 1);
	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(app.data_root, {});
	const std::vector<uam::AgentDefinition>::const_iterator imported = std::ranges::find(catalog.definitions, std::string("agent"), &uam::AgentDefinition::id);
	UAM_ASSERT(imported != catalog.definitions.end());
	UAM_ASSERT_EQ(imported->skills, std::vector<std::string>({"review", "docs"}));
	UAM_ASSERT_EQ(imported->workspace_access, std::string("read"));
	const fs::path target = app.data_root / "agents" / "agent.md";
	const std::string previous = ReadFile(target);
	CefRefPtr<RecordingIconCallback> malformed = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ImportAgent(handler,
	    {{"providerId", "opencode"}, {"sourcePath", source.string()}, {"workspaceAccess", "read"},
	     {"workspaceScope", false}, {"skills", {"review", 7}}}, malformed);
	UAM_ASSERT_EQ(malformed->failures, 1); UAM_ASSERT_EQ(malformed->successes, 0);
	UAM_ASSERT_EQ(malformed->last_failure, 400);
	UAM_ASSERT_EQ(ReadFile(target), previous);
}

UAM_TEST(ChatAgentImportWorkspaceWithoutChatDoesNotPublish)
{
	TempDir temp("uam-handler-agent-no-workspace");
	uam::AppState app;
	app.data_root = temp.root / "data";
	const fs::path source = temp.root / "agent.md";
	UAM_ASSERT(uam::io::WriteTextFile(source, "---\ndescription: Reviews code\n---\nReview.\n"));
	UamQueryHandler handler(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ImportAgent(handler,
	    {{"providerId", "opencode"}, {"sourcePath", source.string()}, {"workspaceAccess", "read"}, {"workspaceScope", true}}, callback);
	UAM_ASSERT_EQ(callback->failures, 1);
	UAM_ASSERT_EQ(callback->successes, 0);
	UAM_ASSERT(!fs::exists(app.data_root / "agents" / "agent.md"));
}
