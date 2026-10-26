#include "test_harness.h"
#include "app/native_agent_history_service.h"
#include "common/provider/gemini_history_loader.h"
#include "common/utils/base64.h"
#include "cef_handler_fixture.h"
#include "remote/native_history_snapshot.h"
#include "cef/uam_query_handler_async.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"
#include "app/agent_definition_service.h"
#include "app/storage_retention_service.h"
#include "app/computer_use_service.h"
#include "app/attachment_preview_service.h"
#include "common/config/execution_host_config.h"
#include "common/config/central_provider_configuration.h"
#include "common/provider/native_agent_history.h"
#include "common/chat/chat_branching.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "computer_use_terminal_runtime_fixture.h"
#include "include/cef_app.h"
#include "include/cef_task.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include <future>
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
	static void GetComputerUseSettings(UamQueryHandler& handler, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		handler.HandleGetComputerUseSettings(nullptr, {}, callback);
	}
	static void ImportAgent(UamQueryHandler& handler, const nlohmann::json& payload, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		handler.HandleImportProviderAgent(nullptr, payload, callback);
	}
	static void OpenNative(UamQueryHandler& handler, const nlohmann::json& payload, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		handler.HandleOpenNativeSessionChat(nullptr, payload, callback);
	}
	static uam::query_handler_async::AsyncCefResult FinishNative(UamQueryHandler& handler, const nlohmann::json& payload,
	    const ChatSession* snapshot = nullptr, const std::string& expected_revision = {})
	{
		return handler.FinishOpenNativeSessionChat(nullptr, payload, snapshot, snapshot != nullptr, expected_revision);
	}

	static bool InstructionInvoke(UamQueryHandler& handler, const char* action, const nlohmann::json& payload,
	    CefRefPtr<UamQueryHandler::Callback> callback)
	{
		return handler.DispatchAction(action, nullptr, payload, callback);
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
		std::string success_message;
		void Success(const CefString& value) override { ++successes; success_message = value.ToString(); }
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
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
		auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
		auto& handler = *handler_owner;
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
		auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
		auto& handler = *handler_owner;
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

	class StagingBrowserClient final : public CefClient, public CefRenderHandler, public CefLifeSpanHandler, public CefLoadHandler
	{
	public:
		bool loaded = false, painted = false, closed = false;
		CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
		CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
		CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
		void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, 64, 64); }
		void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override { painted = true; }
		void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override { if (frame->IsMain()) loaded = true; }
		void OnBeforeClose(CefRefPtr<CefBrowser>) override { closed = true; }
	private:
		IMPLEMENT_REFCOUNTING(StagingBrowserClient);
	};

	class StagingBrowser
	{
	public:
		CefRefPtr<StagingBrowserClient> client = new StagingBrowserClient;
		CefRefPtr<CefBrowser> browser;
		void Start()
		{
			CefWindowInfo window;
			window.SetAsWindowless(nullptr);
			CefBrowserSettings settings;
			settings.windowless_frame_rate = 30;
			browser = CefBrowserHost::CreateBrowserSync(window, client, "about:blank", settings, nullptr, nullptr);
		}
		bool Close()
		{
			if (!browser) return true;
			if (!client->closed) browser->GetHost()->CloseBrowser(true);
			RunCreationLoopUntil([this]() { return client->closed; }, std::chrono::seconds(3));
			if (!client->closed) return false;
			browser = nullptr;
			return true;
		}
		~StagingBrowser() { (void)Close(); }
	};

	class InstructionQueueHold final : public CefTask
	{
		std::shared_future<void> m_release;
		std::shared_ptr<std::atomic<int>> m_state;
	public:
		InstructionQueueHold(std::shared_future<void> release, std::shared_ptr<std::atomic<int>> state)
		    : m_release(std::move(release)), m_state(std::move(state)) {}
		void Execute() override
		{
			m_state->store(1);
			(void)m_release.wait_for(std::chrono::seconds(30));
			m_state->store(2);
		}
	private:
		IMPLEMENT_REFCOUNTING(InstructionQueueHold);
	};

	class RecordingInstructionCallback final : public UamQueryHandler::Callback
	{
	public:
		int successes = 0, failures = 0, status = 0;
		std::string body;
		void Success(const CefString& response) override { ++successes; body = response.ToString(); }
		void Success(const void* data, size_t size) override { ++successes; body.assign(static_cast<const char*>(data), size); }
		void Failure(int code, const CefString& message) override { ++failures; status = code; body = message.ToString(); }
	private:
		IMPLEMENT_REFCOUNTING(RecordingInstructionCallback);
	};

	// Chromium releases BEST_EFFORT work after browser startup. A context-only
	// fixture can execute USER_BLOCKING tasks while starving the real history queue.
	class CreationBrowserClient final : public CefClient, public CefRenderHandler,
	    public CefLifeSpanHandler, public CefLoadHandler
	{
	public:
		bool loaded = false;
		bool closed = false;
		CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
		CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
		CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
		void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, 32, 32); }
		void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override {}
		void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override
		{
			if (frame->IsMain()) loaded = true;
		}
		void OnBeforeClose(CefRefPtr<CefBrowser>) override { closed = true; }
	private:
		IMPLEMENT_REFCOUNTING(CreationBrowserClient);
	};

	class CreationCefRuntime
	{
		TempDir m_private{"uam-create-cef-runtime"};
		CefRefPtr<CreationBrowserClient> m_client;
		CefRefPtr<CefBrowser> m_browser;
	public:
		CreationCefRuntime(const CefMainArgs& arguments)
		{
#if defined(__APPLE__)
			UAM_ASSERT(uam::platform::InitializeMacApplication());
#endif
			CefSettings settings;
			settings.no_sandbox = true;
			settings.external_message_pump = true;
			settings.windowless_rendering_enabled = true;
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
			m_client = new CreationBrowserClient;
			CefWindowInfo window;
			window.SetAsWindowless(nullptr);
			CefBrowserSettings browser_settings;
			m_browser = CefBrowserHost::CreateBrowserSync(window, m_client, "about:blank", browser_settings, nullptr, nullptr);
			UAM_ASSERT(m_browser);
			RunCreationLoopUntil([this]() { return m_client->loaded; }, std::chrono::seconds(10));
			UAM_ASSERT(m_client->loaded);
			const std::shared_ptr<std::atomic<int>> observed = std::make_shared<std::atomic<int>>(0);
			UAM_ASSERT(CefPostTask(TID_UI, new CreationQueueWitness(observed, 1)));
			UAM_ASSERT(CefPostTask(TID_FILE_USER_BLOCKING, new CreationQueueWitness(observed, 2)));
			RunCreationLoopUntil([observed]() { return observed->load() == 3; }, std::chrono::seconds(5));
			std::cout << "CEF_QUEUE_WITNESS ui=" << ((observed->load() & 1) != 0) << " user_blocking=" << ((observed->load() & 2) != 0) << std::endl;
			if (observed->load() != 3)
			{
				CefShutdown();
				throw std::runtime_error("Private CEF UI/background queues did not execute before the deadline.");
			}
		}
		~CreationCefRuntime()
		{
			if (m_browser)
			{
				m_browser->GetHost()->CloseBrowser(true);
				RunCreationLoopUntil([this]() { return m_client->closed; }, std::chrono::seconds(10));
				m_browser = nullptr;
			}
			m_client = nullptr;
			CefShutdown();
		}
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
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
		auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		auto& handler = *handler_owner;
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Retry(handler, "cold-retry", fixture.user_index, callback);
	fixture.Await(callback);
	UAM_ASSERT(fixture.app.chats.front().messages_loaded);
	UAM_ASSERT_EQ(callback->failure_message, std::string("The failed message is no longer the latest message."));
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{202});
	UAM_ASSERT(fixture.app.acp_sessions.empty());
}


UAM_TEST(CentralInstructionHandlersIgnoreBlockedSharedFileQueue)
{
	TempDir temp("uam-instruction-dedicated-queue");
	const fs::path path = temp.root / "AGENTS.md";
	const std::string requested = uam::paths::Utf8PathString(path);
	UAM_ASSERT(uam::io::WriteTextFile(path, "original"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.central_provider_configuration.instruction_files = {requested};
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
	std::promise<void> release;
	struct ReleaseQueue
	{
		std::promise<void>& promise;
		~ReleaseQueue() { promise.set_value(); }
	} release_queue{release};
	const std::shared_ptr<std::atomic<int>> state = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_USER_BLOCKING, new InstructionQueueHold(release.get_future().share(), state)));
	RunCreationLoopUntil([state]() { return state->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(state->load(), 1);
	const auto invoke = [&](const char* action, nlohmann::json payload)
	{
		CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
		UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, action, payload, callback));
		return callback;
	};
	const auto await = [](CefRefPtr<RecordingInstructionCallback> callback)
	{
		RunCreationLoopUntil([callback]() { return callback->successes + callback->failures > 0; }, std::chrono::seconds(3));
		UAM_ASSERT_EQ(callback->successes, 1);
		UAM_ASSERT_EQ(callback->failures, 0);
	};
	const CefRefPtr<RecordingInstructionCallback> read = invoke("readCentralInstructionFile", {{"path", requested}});
	await(read);
	UAM_ASSERT_EQ(nlohmann::json::parse(read->body).at("content").get<std::string>(), std::string("original"));
	const CefRefPtr<RecordingInstructionCallback> save = invoke("saveCentralInstructionFile", {{"path", requested}, {"content", "changed"}, {"expectedContent", "original"}});
	await(save);
	UAM_ASSERT_EQ(ReadFile(path), std::string("changed"));
	UAM_ASSERT_EQ(state->load(), 1);
	std::vector<CefRefPtr<RecordingInstructionCallback>> pending;
	for (int index = 0; index < 4; ++index) pending.push_back(invoke("readCentralInstructionFile", {{"path", requested}}));
	const CefRefPtr<RecordingInstructionCallback> busy = invoke("readCentralInstructionFile", {{"path", requested}});
	UAM_ASSERT_EQ(busy->failures, 1);
	UAM_ASSERT_EQ(busy->status, 429);
	for (const CefRefPtr<RecordingInstructionCallback>& callback : pending) await(callback);
	await(invoke("readCentralInstructionFile", {{"path", requested}}));
	UAM_ASSERT_EQ(state->load(), 1);
}

UAM_TEST(ComputerUseSettingsExposePendingRevocationWithoutChangingConsent)
{
	TempDir temp("uam-cu-global-status");
	uam::AppState app;
	app.data_root = temp.root;
	auto handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	for (bool enabled : {false, true})
	{
		app.settings.computer_use_globally_enabled = enabled;
		for (bool pending : {false, true, false})
		{
			app.computer_use_revocation_pending_chat_ids.clear();
			if (pending) app.computer_use_revocation_pending_chat_ids.insert("deleted-owned-chat");
			const nlohmann::json state = uam::StateSerializer::Serialize(app, true);
			const nlohmann::json fingerprint = uam::StateSerializer::SerializeFingerprint(app);
			const nlohmann::json settings_patch = nlohmann::json::parse(uam::SettingsPatchForTests(app));
			uam::AppState before;
			before.data_root = temp.root;
			before.settings.computer_use_globally_enabled = enabled;
			if (!pending) before.computer_use_revocation_pending_chat_ids.insert("deleted-owned-chat");
			const nlohmann::json changed_patch = nlohmann::json::parse(uam::StatePatchForTests(before, app)).at("data").at("settings");
			UAM_ASSERT(!nlohmann::json::parse(uam::StatePatchForTests(app, app)).at("data").contains("settings"));
			CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
			ChatIconHandlerTestAccess::GetComputerUseSettings(*handler, callback);
			UAM_ASSERT_EQ(callback->successes, 1);
			const nlohmann::json readback = nlohmann::json::parse(callback->success_message);
			for (const nlohmann::json* settings : {&state.at("settings"), &fingerprint.at("settings"), &settings_patch, &changed_patch, &readback})
			{
				UAM_ASSERT_EQ(settings->at("computerUseGloballyEnabled").get<bool>(), enabled);
				UAM_ASSERT_EQ(settings->at("computerUseRevocationPending").get<bool>(), pending);
				UAM_ASSERT(settings->dump().find("deleted-owned-chat") == std::string::npos);
			}
			UAM_ASSERT_EQ(app.computer_use_revocation_pending_chat_ids.contains("deleted-owned-chat"), pending);
		}
	}
	UAM_ASSERT(!uam::settings_frontend_json::SerializeCommonSettingsFields(app.settings).contains("computerUseRevocationPending"));
}

UAM_TEST(ComputerUseGlobalSettingsSaveFailurePreservesCapturedAuthority)
{
	TempDir temp("uam-cu-global-persistence-failure");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession controlled = HandlerChat("cu-owned");
	controlled.computer_use_enabled = true;
	app.chats.push_back(controlled);
	ComputerUseTerminalRuntimeFixture runtime;
	runtime.AddTerminal(app, controlled.id);
	fs::create_directories(AppPaths::SettingsFilePath(temp.root));
	auto handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> failed = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ComputerUseSettings(*handler, false, failed);
	UAM_ASSERT_EQ(failed->failures, 1);
	UAM_ASSERT_EQ(failed->successes, 0);
	UAM_ASSERT(app.settings.computer_use_globally_enabled);
	UAM_ASSERT(app.chats.front().computer_use_enabled);
	UAM_ASSERT(app.computer_use_revocation_pending_chat_ids.empty());
	UAM_ASSERT_EQ(runtime.StopCount(controlled.id), 0);
	CefRefPtr<RecordingIconCallback> read = new RecordingIconCallback;
	ChatIconHandlerTestAccess::GetComputerUseSettings(*handler, read);
	const nlohmann::json receipt = nlohmann::json::parse(read->success_message);
	UAM_ASSERT(receipt.at("computerUseGloballyEnabled").get<bool>());
	UAM_ASSERT(!receipt.at("computerUseRevocationPending").get<bool>());
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
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
		auto map_handler_owner = UamQueryHandlerTestAccess::Create(map_app, "file:///private/index.html");
		auto& map_handler = *map_handler_owner;
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
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
		CefRefPtr<RecordingIconCallback> read = new RecordingIconCallback;
		ChatIconHandlerTestAccess::GetComputerUseSettings(handler, read);
		const nlohmann::json receipt = nlohmann::json::parse(read->success_message);
		UAM_ASSERT(!receipt.at("computerUseGloballyEnabled").get<bool>());
		UAM_ASSERT(receipt.at("computerUseRevocationPending").get<bool>());
		AppSettings saved;
		UAM_ASSERT(SettingsStore::Load(AppPaths::SettingsFilePath(temp.root), saved).loaded);
		UAM_ASSERT(!saved.computer_use_globally_enabled);
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
	const nlohmann::json stopped = nlohmann::json::parse(callback->success_message);
	UAM_ASSERT(!stopped.at("computerUseGloballyEnabled").get<bool>());
	UAM_ASSERT(!stopped.at("computerUseRevocationPending").get<bool>());
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
		const bool instruction = test.name.rfind("CentralInstruction", 0) == 0;
		const bool attachment = test.name.rfind("AttachmentStaging", 0) == 0;
		const bool window_chrome = test.name.rfind("WindowChrome", 0) == 0;
		const bool companion_live = test.name.rfind("CompanionLive", 0) == 0;
		const bool native_lineage = test.name.rfind("NativeChildLineageHandler", 0) == 0;
		const bool remote_history = test.name.rfind("RemoteNativeHistoryHandler", 0) == 0;
		if (icon) ++icon_selected;
		if (creation) ++create_selected;
		if (retry) ++retry_selected;
		try
		{
			if ((creation || retry || instruction || attachment || window_chrome || companion_live || native_lineage || remote_history) && !initialization_attempted)
			{
				initialization_attempted = true;
				cef = std::make_unique<CreationCefRuntime>(main_args);
			}
			if (creation || retry || instruction || attachment || window_chrome || companion_live || native_lineage || remote_history) UAM_ASSERT(cef != nullptr);
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
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
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::ImportAgent(handler,
	    {{"providerId", "opencode"}, {"sourcePath", source.string()}, {"workspaceAccess", "read"}, {"workspaceScope", true}}, callback);
	UAM_ASSERT_EQ(callback->failures, 1);
	UAM_ASSERT_EQ(callback->successes, 0);
	UAM_ASSERT(!fs::exists(app.data_root / "agents" / "agent.md"));
}

UAM_TEST(ProviderMcpOverrideHandlerUsesEffectiveHostScopedServerMembership)
{
	TempDir temp("uam-mcp-handler-scope");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ExecutionHost local;
	ExecutionHost remote;
	remote.id = "ssh-lab"; remote.transport = "ssh"; remote.ssh_alias = "private-lab";
	app.settings.execution_hosts = {local, remote};
	McpServerConfiguration global;
	global.id = "global"; global.name = "Global"; global.transport = "http"; global.url = "https://example.test/global";
	app.settings.mcp_servers = {global};
	McpServerConfiguration scoped = global;
	scoped.id = "remote"; scoped.name = "Remote"; scoped.execution_host_id = remote.id;
	McpServerConfiguration foreign = scoped;
	foreign.id = "foreign"; foreign.execution_host_id = "ssh-other";
	app.settings.host_scoped_settings[remote.id].mcp_servers = std::vector<McpServerConfiguration>{scoped, foreign};
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
	const auto invoke = [&](const std::string& host, const std::string& server)
	{
		CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
		UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, "setProviderMcpConfiguration",
		    {{"executionHostId", host}, {"providerId", "codex-cli"}, {"serverId", server}, {"enabled", false}}, callback));
		return callback;
	};
	const CefRefPtr<RecordingInstructionCallback> accepted = invoke(remote.id, scoped.id);
	UAM_ASSERT_EQ(accepted->successes, 1);
	UAM_ASSERT_EQ(accepted->failures, 0);
	const nlohmann::json status = nlohmann::json::parse(accepted->body);
	UAM_ASSERT_EQ(status.at("executionHostId").get<std::string>(), remote.id);
	const nlohmann::json::const_iterator row = std::ranges::find_if(status.at("providers"), [](const nlohmann::json& provider) { return provider.at("providerId") == "codex-cli"; });
	UAM_ASSERT(row != status.at("providers").end());
	UAM_ASSERT_EQ(row->at("mcpServers").size(), std::size_t(1));
	UAM_ASSERT_EQ(row->at("mcpServers").front().at("id").get<std::string>(), scoped.id);
	UAM_ASSERT(!row->at("mcpServers").front().at("enabled").get<bool>());
	AppSettings persisted;
	UAM_ASSERT(!SettingsStore::Load(AppPaths::SettingsFilePath(app.data_root), persisted).unrecovered_error);
	UAM_ASSERT_EQ(persisted.central_provider_configuration.mcp_overrides.size(), std::size_t(1));
	UAM_ASSERT_EQ(persisted.central_provider_configuration.mcp_overrides.front().execution_host_id, remote.id);
	UAM_ASSERT_EQ(persisted.central_provider_configuration.mcp_overrides.front().server_id, scoped.id);
	for (const std::pair<std::string, std::string>& invalid : std::vector<std::pair<std::string, std::string>>{{remote.id, global.id}, {remote.id, foreign.id}, {"local", scoped.id}})
	{
		const CefRefPtr<RecordingInstructionCallback> rejected = invoke(invalid.first, invalid.second);
		UAM_ASSERT_EQ(rejected->successes, 0);
		UAM_ASSERT_EQ(rejected->failures, 1);
		UAM_ASSERT_EQ(rejected->status, 404);
		UAM_ASSERT_EQ(app.settings.central_provider_configuration.mcp_overrides.size(), std::size_t(1));
	}
	const CefRefPtr<RecordingInstructionCallback> local_result = invoke("local", global.id);
	UAM_ASSERT_EQ(local_result->successes, 1);
	UAM_ASSERT_EQ(local_result->failures, 0);
}

UAM_TEST(OpenCodeProgressiveMcpHandlerPersistsScopeAndRollsBackFailedSave)
{
	TempDir temp("uam-opencode-progressive-handler");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ExecutionHost remote;
	remote.id = "ssh-lab"; remote.transport = "ssh"; remote.ssh_alias = "private-fixture";
	app.settings.execution_hosts = {ExecutionHost{}, remote};
	app.settings.central_provider_configuration.instructions = "Keep instructions";
	app.settings.central_provider_configuration.mcp_overrides.push_back({"codex-cli", "local", "docs", false});
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	const auto invoke = [&](const std::string& provider, const std::string& host, bool enabled)
	{
		CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
		UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(*handler_owner, "setProviderMcpConfiguration",
		    {{"providerId", provider}, {"executionHostId", host}, {"serverId", "opencode-progressive-mcp"}, {"enabled", enabled}, {"kind", "preference"}}, callback));
		return callback;
	};
	for (const bool enabled : {false, true})
	{
		const CefRefPtr<RecordingInstructionCallback> callback = invoke("opencode-cli", remote.id, enabled);
		UAM_ASSERT_EQ(callback->successes, 1);
		UAM_ASSERT_EQ(callback->failures, 0);
		const nlohmann::json status = nlohmann::json::parse(callback->body);
		UAM_ASSERT_EQ(status.at("executionHostId"), nlohmann::json(remote.id));
		const auto row = std::ranges::find_if(status.at("providers"), [](const nlohmann::json& item) { return item.at("providerId") == "opencode-cli"; });
		UAM_ASSERT(row != status.at("providers").end());
		UAM_ASSERT_EQ(row->at("progressiveMcpDiscoveryEnabled").get<bool>(), enabled);
		AppSettings persisted;
		UAM_ASSERT(SettingsStore::Load(AppPaths::SettingsFilePath(app.data_root), persisted).loaded);
		UAM_ASSERT_EQ(persisted.central_provider_configuration.mcp_overrides.size(), std::size_t{2});
		UAM_ASSERT_EQ(persisted.central_provider_configuration.instructions, std::string("Keep instructions"));
		UAM_ASSERT_EQ(uam::central_configuration::OpenCodeProgressiveMcpEnabled(persisted.central_provider_configuration, remote.id), enabled);
		UAM_ASSERT(uam::central_configuration::OpenCodeProgressiveMcpEnabled(persisted.central_provider_configuration, "local"));
	}
	const nlohmann::json previous = uam::central_configuration::Serialize(app.settings.central_provider_configuration);
	for (const std::string provider : {"gemini-cli", "codex-cli", "claude-cli", "copilot-cli", "antigravity-cli"})
	{
		const CefRefPtr<RecordingInstructionCallback> callback = invoke(provider, remote.id, false);
		UAM_ASSERT_EQ(callback->failures, 1);
		UAM_ASSERT_EQ(callback->status, 400);
	}
	const CefRefPtr<RecordingInstructionCallback> missing_host = invoke("opencode-cli", "unknown-host", false);
	UAM_ASSERT_EQ(missing_host->status, 404);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "blocked", "File prevents a settings directory"));
	app.data_root = temp.root / "blocked";
	const CefRefPtr<RecordingInstructionCallback> failed_save = invoke("opencode-cli", remote.id, false);
	UAM_ASSERT_EQ(failed_save->failures, 1);
	UAM_ASSERT_EQ(failed_save->successes, 0);
	UAM_ASSERT_EQ(failed_save->status, 500);
	UAM_ASSERT_EQ(uam::central_configuration::Serialize(app.settings.central_provider_configuration), previous);
}


UAM_TEST(CompanionStoragePartialUpdatePreservesOmittedPolicy)
{
	TempDir temp("uam-companion-storage-partial-handler");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	std::string error;
	const auto owner = std::make_shared<uam::StorageRetentionService::Owner>();
	UAM_ASSERT(uam::StorageRetentionService::SavePolicy(temp.root, {30, 7}, owner, error));
	app.storage_retention_service = std::make_shared<uam::StorageRetentionService>();
	const auto poll_until = [&](const std::function<bool()>& finished)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do
		{
			app.storage_retention_service->Poll(app);
			if (finished()) return;
			std::this_thread::yield();
		} while (std::chrono::steady_clock::now() < deadline);
		UAM_ASSERT(finished());
	};
	poll_until([&]() { return app.storage_retention_service->Ready(); });
	UAM_ASSERT_EQ(app.settings.backup_log_retention_days, 30);
	UAM_ASSERT_EQ(app.settings.http_cache_clear_interval_days, 7);
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
	for (const bool logs : {true, false})
	{
		CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
		const nlohmann::json payload = logs ? nlohmann::json{{"backupLogRetentionDays", 0}} : nlohmann::json{{"httpCacheClearIntervalDays", 14}};
		UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, "setStorageSettings", payload, callback));
		poll_until([&]() { return callback->successes + callback->failures > 0; });
		UAM_ASSERT_EQ(callback->successes, 1);
		UAM_ASSERT_EQ(callback->failures, 0);
		const auto saved = uam::StorageRetentionService::LoadPolicy(temp.root);
		UAM_ASSERT(saved.ok);
		UAM_ASSERT_EQ(saved.policy.backup_log_retention_days, 0);
		UAM_ASSERT_EQ(saved.policy.http_cache_clear_interval_days, logs ? 7 : 14);
	}
	for (const nlohmann::json& payload : {nlohmann::json::object(), nlohmann::json{{"backupLogRetentionDays", "0"}}, nlohmann::json{{"httpCacheClearIntervalDays", -1}}})
	{
		CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
		UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, "setStorageSettings", payload, callback));
		UAM_ASSERT_EQ(callback->failures, 1);
		UAM_ASSERT_EQ(callback->last_failure, 400);
		UAM_ASSERT_EQ(callback->successes, 0);
		const auto saved = uam::StorageRetentionService::LoadPolicy(temp.root);
		UAM_ASSERT(saved.ok && saved.policy == (uam::StorageRetentionPolicy{0, 14}));
	}
}


UAM_TEST(CompanionNativeOpenPreservesDesktopDraftAcrossSort)
{
	for (const bool select_chat : {false, true})
	{
		TempDir temp("uam-companion-native-open-selection");
		uam::AppState app;
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession desktop;
		desktop.id = "desktop"; desktop.provider_id = "opencode-cli"; desktop.native_session_id = "ses_parent";
		desktop.workspace_directory = temp.root.string(); desktop.execution_host_id = "local";
		desktop.created_at = desktop.updated_at = desktop.interaction_at = "2020-01-01T00:00:00Z";
		desktop.messages.push_back({MessageRole::User, "Original parent history"});
		ChatSession child = desktop;
		child.id = "child"; child.native_session_id = "ses_child";
		child.updated_at = child.interaction_at = "2026-01-01T00:00:00Z";
		child.messages.front().content = "Original child history";
		app.chats = {desktop, child};
		for (const auto& chat : app.chats) UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		ChatDomainService().SelectChatById(app, desktop.id);
		app.composer_text = "Unsaved desktop draft";
		auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
		auto& handler = *handler_owner;
		CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
		UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, "openNativeSessionChat",
		    {{"chatId", desktop.id}, {"nativeSessionId", child.native_session_id}, {"refreshHistory", false}, {"selectChat", select_chat}}, callback));
		UAM_ASSERT_EQ(callback->successes, 1);
		UAM_ASSERT_EQ(callback->failures, 0);
		UAM_ASSERT_EQ(app.chats.front().id, child.id);
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), select_chat ? child.id : desktop.id);
		UAM_ASSERT_EQ(app.composer_text, select_chat ? std::string{} : std::string("Unsaved desktop draft"));
		UAM_ASSERT_EQ(ChatDomainService().FindChatById(app, desktop.id)->messages.front().content, "Original parent history");
		UAM_ASSERT_EQ(ChatDomainService().FindChatById(app, child.id)->messages.front().content, "Original child history");
	}
}

UAM_TEST(AttachmentStagingIgnoresBlockedBackgroundQueue)
{
	TempDir temp("uam-staging-visible-queue");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.execution_hosts = {ExecutionHost{}};
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat = HandlerChat("attachment-staging");
	chat.workspace_directory = (temp.root / "workspace").string();
	app.chats.push_back(chat);
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
	// Chromium defers BEST_EFFORT work until initial browser content has painted. A
	// browser-less CEF context witnesses USER_BLOCKING but cannot start a background
	// hold, so create a private offscreen blank page before exercising both queues.
	StagingBrowser browser;
	browser.Start();
	UAM_ASSERT(browser.browser != nullptr);
	RunCreationLoopUntil([&browser]() { return browser.client->loaded && browser.client->painted; }, std::chrono::seconds(3));
	UAM_ASSERT(browser.client->loaded && browser.client->painted);

	std::promise<void> release;
	struct ReleaseQueue
	{
		std::promise<void>& promise;
		~ReleaseQueue() { promise.set_value(); }
	} release_queue{release};
	const auto state = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(release.get_future().share(), state)));
	RunCreationLoopUntil([state]() { return state->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(state->load(), 1);
	const std::string ordinary(112 * 1024, 'x');
	std::string image;
	UAM_ASSERT(uam::base64::Decode("iVBORw0KGgoAAAANSUhEUgAAAAIAAAADCAYAAAC56t6BAAAAEUlEQVR4nGP4z8DQAMIMGAwAfcEI+4thakwAAAAASUVORK5CYII=", image));
	CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
	UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, "stageChatAttachments",
	    {{"chatId", chat.id}, {"items", nlohmann::json::array({
	        {{"name", "ordinary.png"}, {"kind", "file"}, {"mimeType", "image/png"}, {"dataBase64", uam::base64::Encode(ordinary)}},
	        {{"name", "pixels.dat"}, {"kind", "image"}, {"mimeType", "image/png"}, {"dataBase64", uam::base64::Encode(image)}}})}}, callback));
	RunCreationLoopUntil([callback]() { return callback->successes + callback->failures > 0; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(callback->successes, 1);
	UAM_ASSERT_EQ(callback->failures, 0);
	UAM_ASSERT_EQ(state->load(), 1); // Completion must not depend on releasing the maintenance queue.
	const auto attachments = nlohmann::json::parse(callback->body).at("attachments");
	UAM_ASSERT_EQ(attachments.size(), std::size_t(2));
	for (std::size_t index = 0; index < attachments.size(); ++index)
	{
		const fs::path staged = fs::path(chat.workspace_directory) / attachments[index].at("path").get<std::string>();
		UAM_ASSERT_EQ(ReadFile(staged), index == 0 ? ordinary : image);
		UAM_ASSERT(attachments[index].at("copied").get<bool>());
	}
	// Classification comes from bytes, never a misleading filename or MIME label.
	UAM_ASSERT(attachments[0].at("previewKey").get<std::string>().empty());
	UAM_ASSERT(uam::attachment_preview::IsKey(attachments[1].at("previewKey").get<std::string>()));
	const fs::path preview = temp.root / "attachment-previews" / (attachments[1].at("previewKey").get<std::string>() + ".png");
	const uam::attachment_preview::Image decoded = uam::attachment_preview::Decode(ReadFile(preview));
	UAM_ASSERT_EQ(decoded.width, 2u);
	UAM_ASSERT_EQ(decoded.height, 3u);
	UAM_ASSERT(browser.Close());
}
UAM_TEST(WindowChromeRejectsUntrustedFramesAndCompanionDispatch)
{
	TempDir temp("uam-window-chrome-boundary");
	uam::AppState app;
	app.data_root = temp.root;
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "uam://app/index.html");
	auto& handler = *handler_owner;
	for (const std::string& action : {std::string("getWindowChrome"), std::string("windowCommand"), std::string("restartApplication")})
	{
		CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
		const nlohmann::json payload = {{"command", "close"}};
		UAM_ASSERT(!ChatIconHandlerTestAccess::InstructionInvoke(handler, action.c_str(), payload, callback));
		UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
		UAM_ASSERT(handler.OnQuery(nullptr, nullptr, 1, nlohmann::json({{"action", action}, {"payload", payload}}).dump(), false, callback));
		UAM_ASSERT_EQ(callback->successes, 0);
		UAM_ASSERT_EQ(callback->failures, 1);
		UAM_ASSERT_EQ(callback->last_failure, 403);
	}
}
namespace
{
	void ConfigureGeminiOptionHandler(uam::AppState& app, const fs::path& data_root, bool running = false)
	{
		app.data_root = data_root;
		app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
		ChatSession chat = HandlerChat("gemini-thinking"); chat.provider_id = "gemini-cli";
		chat.model_id = "gemini-3.1-pro-preview"; chat.reasoning_effort = "low"; chat.execution_host_id = "local";
		app.chats.push_back(chat);
		auto& version = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("gemini-cli", "local")];
		version.execution_host = uam::execution_hosts::LocalHost(); version.checked = version.supported = true; version.installed_version = "0.55.1";
		auto session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = chat.id; session->provider_id = chat.provider_id; session->protocol_kind = "gemini-acp";
		session->running = running; session->session_ready = true; session->lifecycle_state = "ready";
		session->current_model_id = chat.model_id;
		app.acp_sessions.push_back(std::move(session));
	}
}

UAM_TEST(GeminiThinkingHandlerRejectsUnsupportedAndUnverifiedChoices)
{
	TempDir temp("uam-gemini-thinking-handler-invalid");
	uam::AppState app; ConfigureGeminiOptionHandler(app, temp.root);
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
	for (const std::string effort : {"minimal", "max", "4096", "unknown"})
	{
		CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
		UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, "setChatCodexOptions", {{"chatId", app.chats.front().id}, {"reasoningEffort", effort}}, callback));
		UAM_ASSERT_EQ(callback->last_failure, 409);
		UAM_ASSERT_EQ(app.chats.front().reasoning_effort, std::string("low"));
	}
	app.runtime_cli_versions_by_provider_id.clear();
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, "setChatCodexOptions", {{"chatId", app.chats.front().id}, {"reasoningEffort", "high"}}, callback));
	UAM_ASSERT_EQ(callback->last_failure, 409);
	UAM_ASSERT_EQ(app.chats.front().reasoning_effort, std::string("low"));
	// Default remains an escape hatch even after a CLI version becomes unknown.
	callback = new RecordingIconCallback;
	UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, "setChatCodexOptions", {{"chatId", app.chats.front().id}, {"reasoningEffort", ""}}, callback));
	UAM_ASSERT_EQ(callback->successes, 1);
	UAM_ASSERT(app.chats.front().reasoning_effort.empty());
}

UAM_TEST(GeminiThinkingHandlerStopsIdleRuntimeAndBlocksBusyChanges)
{
	TempDir temp("uam-gemini-thinking-handler-restart");
	uam::AppState app; ConfigureGeminiOptionHandler(app, temp.root, true);
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
	app.acp_sessions.front()->processing = true;
	for (const std::string action : {"setChatCodexOptions", "setChatModel"})
	{
		CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
		UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, action.c_str(), {{"chatId", app.chats.front().id}, {"reasoningEffort", "high"}, {"modelId", "gemini-3-flash-preview"}}, callback));
		UAM_ASSERT_EQ(callback->last_failure, 409);
		UAM_ASSERT_EQ(app.chats.front().reasoning_effort, std::string("low"));
		UAM_ASSERT_EQ(app.chats.front().model_id, std::string("gemini-3.1-pro-preview"));
		UAM_ASSERT(app.acp_sessions.front()->running);
	}
	app.acp_sessions.front()->processing = false;
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, "setChatCodexOptions", {{"chatId", app.chats.front().id}, {"reasoningEffort", "medium"}}, callback));
	UAM_ASSERT_EQ(callback->successes, 1);
	UAM_ASSERT_EQ(app.chats.front().reasoning_effort, std::string("medium"));
	UAM_ASSERT(!app.acp_sessions.front()->running);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, app.chats.front().id)->reasoning_effort, std::string("medium"));
}

UAM_TEST(GeminiModelChangeClearsUnsupportedEffortToDefaultAndStopsIdleRuntime)
{
	TempDir temp("uam-gemini-thinking-model-transition");
	uam::AppState app; ConfigureGeminiOptionHandler(app, temp.root, true);
	app.chats.front().reasoning_effort = "medium";
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, "setChatModel", {{"chatId", app.chats.front().id}, {"modelId", "gemini-3-pro-preview"}}, callback));
	UAM_ASSERT_EQ(callback->successes, 1);
	UAM_ASSERT(app.chats.front().reasoning_effort.empty());
	UAM_ASSERT_EQ(app.chats.front().model_id, std::string("gemini-3-pro-preview"));
	UAM_ASSERT(!app.acp_sessions.front()->running);
}

// Native child lineage handler packet: existing tests above remain unchanged.
namespace
{
	struct NativeLineageHandlerFixture
	{
		TempDir temp{"uam-native-lineage-handler"};
		uam::AppState app;
		ChatSession parent;
		ChatSession child;
		std::string native_child_id;

		explicit NativeLineageHandlerFixture(const std::string& provider = "codex-cli", bool insert_child = true)
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			parent = HandlerChat("lineage-source-uam");
			parent.provider_id = provider;
			parent.native_session_id = "550e8400-e29b-41d4-a716-446655440000";
			parent.workspace_directory = temp.root.string();
			parent.branch_root_chat_id = parent.id;
			parent.messages.clear();
			child = HandlerChat("lineage-target-uam");
			child.provider_id = provider;
			child.native_session_id = provider == "codex-cli" ? "660e8400-e29b-41d4-a716-446655440001" : "ses_lineage_child";
			child.workspace_directory = parent.workspace_directory;
			child.created_at = child.updated_at = child.last_opened_at = "2026-10-16T10:01:00Z";
			child.branch_root_chat_id = child.id;
			child.model_id = "saved-child-model";
			child.approval_mode = "default";
			if (provider != "codex-cli" && provider != "opencode-cli")
			{
				child.native_session_id.clear();
				child.imported_read_only = true;
				child.parent_chat_id = parent.native_session_id;
				child.id = uam::native_agents::BuildHistoryId(provider, parent.native_session_id, "lineage-agent", "local");
				child.branch_root_chat_id = child.id;
			}
			native_child_id = child.imported_read_only ? child.id : child.native_session_id;
			child.messages = {{MessageRole::Assistant, "Actual saved child transcript"}};
			Message owner{MessageRole::Assistant, "Delegate one child"};
			ToolCall tool;
			tool.id = "native-lineage-tool";
			tool.is_sub_agent = true;
			tool.sub_agent_id = native_child_id;
			tool.sub_agent_children_authoritative = provider == "codex-cli";
			if (tool.sub_agent_children_authoritative) tool.sub_agent_children = {{native_child_id, "Child", "completed"}};
			owner.tool_calls.push_back(std::move(tool));
			parent.messages.push_back(std::move(owner));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, parent));
			app.chats.push_back(parent);
			if (insert_child)
			{
				UAM_ASSERT(ChatRepository::SaveChat(app.data_root, child));
				app.chats.push_back(child);
			}
			app.resolved_native_sessions_by_chat_id[parent.id] = parent.native_session_id;
			ChatDomainService().SelectChatById(app, parent.id);
		}

		nlohmann::json Payload(bool select = false) const
		{
			return {{"chatId", parent.id}, {"nativeSessionId", native_child_id}, {"selectChat", select}, {"refreshHistory", false}};
		}
		ChatSession& Parent()
		{
			ChatSession* chat = ChatDomainService().FindChatById(app, parent.id);
			UAM_ASSERT(chat != nullptr);
			return *chat;
		}
		ChatSession& Child()
		{
			ChatSession* chat = ChatDomainService().FindChatById(app, child.id);
			UAM_ASSERT(chat != nullptr);
			return *chat;
		}
		void SeedReceipt(const std::string& parent_id, const std::string& birth)
		{
			Child().subagent_parent_chat_id = parent_id;
			Child().subagent_parent_created_at = birth;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, Child()));
		}
		void MakeChildCold()
		{
			const auto summary = ChatRepository::LoadLocalChat(app.data_root, child.id, false);
			UAM_ASSERT(summary && !summary->messages_loaded);
			Child() = *summary;
		}
		void AssertReceipt(const ChatSession& chat) const
		{
			UAM_ASSERT_EQ(chat.subagent_parent_chat_id, parent.id);
			UAM_ASSERT_EQ(chat.subagent_parent_created_at, parent.created_at);
			UAM_ASSERT(chat.subagent_parent_chat_id != parent.native_session_id);
		}
		void Await(CefRefPtr<RecordingInstructionCallback> callback)
		{
			RunCreationLoopUntil([callback]() { return callback->successes + callback->failures != 0; }, std::chrono::seconds(10));
			UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
		}
		void BlockSettingsPublication()
		{
			const auto settings = AppPaths::SettingsFilePath(app.data_root);
			fs::create_directories(settings);
			UAM_ASSERT(uam::io::WriteTextFile(settings / "keep-directory-nonempty", "fixture obstruction"));
		}
	};
}

UAM_TEST(NativeChildLineageHandlerResumableEvidenceAndOrdinaryOpen)
{
	for (const std::string provider : {"codex-cli", "opencode-cli"})
	for (const bool evidence : {false, true})
	for (const bool select : {false, true})
	{
		NativeLineageHandlerFixture fixture(provider);
		if (!evidence) fixture.Parent().messages.clear();
		auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		auto& handler = *handler_owner;
		CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
		ChatIconHandlerTestAccess::OpenNative(handler, fixture.Payload(select), callback);
		UAM_ASSERT_EQ(callback->successes, 1);
		UAM_ASSERT_EQ(callback->failures, 0);
		UAM_ASSERT_EQ(nlohmann::json::parse(callback->body).at("chatId"), nlohmann::json(fixture.child.id));
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), select ? fixture.child.id : fixture.parent.id);
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.child.id);
		UAM_ASSERT(saved);
		if (evidence)
		{
			fixture.AssertReceipt(fixture.Child());
			fixture.AssertReceipt(*saved);
		}
		else
		{
			UAM_ASSERT(fixture.Child().subagent_parent_chat_id.empty() && fixture.Child().subagent_parent_created_at.empty());
			UAM_ASSERT(saved->subagent_parent_chat_id.empty() && saved->subagent_parent_created_at.empty());
		}
		UAM_ASSERT_EQ(saved->native_session_id, fixture.native_child_id);
		UAM_ASSERT_EQ(saved->model_id, fixture.child.model_id);
		UAM_ASSERT_EQ(saved->approval_mode, fixture.child.approval_mode);
		UAM_ASSERT_EQ(saved->parent_chat_id, fixture.child.parent_chat_id);
		UAM_ASSERT_EQ(saved->branch_root_chat_id, fixture.child.branch_root_chat_id);
		UAM_ASSERT_EQ(fixture.app.resolved_native_sessions_by_chat_id.at(fixture.child.id), fixture.native_child_id);
	}
}

#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
namespace
{
	struct OwnedAntigravityChildHandlerFixture
	{
		TempDir temp{"uam-owned-agy-child-handler"};
		uam::AppState app;
		ChatSession child;
		const std::string parent_id = "owned-agy-parent";
		const std::string child_native_id = "660e8400-e29b-41d4-a716-446655440001";

		OwnedAntigravityChildHandlerFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatSession parent = HandlerChat(parent_id);
			parent.provider_id = "antigravity-cli";
			parent.native_session_id = "550e8400-e29b-41d4-a716-446655440000";
			parent.execution_host_id = "local";
			parent.workspace_directory = temp.root.string();
			parent.branch_root_chat_id = parent.id;
			parent.messages.clear();
			app.chats.push_back(parent);
			uam::AcpSessionState captured;
			captured.chat_id = parent.id;
			captured.provider_id = parent.provider_id;
			captured.session_id = parent.native_session_id;
			captured.running = captured.processing = true;
			const nlohmann::json event{{"event", "step_update"}, {"step_update", {{"conversation_id", parent.native_session_id}, {"step_index", 4}, {"state", "ACTIVE"}, {"step_type", "subagent"}, {"subagent_info", {{"subagents", nlohmann::json::array({{{"type_name", "Owned reviewer"}, {"role", "Inspect source"}, {"conversation_id", child_native_id}}})}}}}}};
			// Decode the actual native event; never fabricate tool evidence or a journal reference.
			UAM_ASSERT(ProviderRuntimeRegistry::ResolveById(parent.provider_id).OnAcpHandleMessage(app, captured, Parent(), event, nullptr));
			UAM_ASSERT_EQ(Parent().messages.size(), std::size_t{1});
			UAM_ASSERT_EQ(Parent().messages.front().tool_calls.size(), std::size_t{1});
			UAM_ASSERT(Parent().messages.front().tool_calls.front().is_sub_agent);
			UAM_ASSERT_EQ(Parent().messages.front().tool_calls.front().sub_agent_id, child_native_id);
			UAM_ASSERT(uam::acp_detail::SaveChatQuietly(app, Parent()));
			UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
			child = HandlerChat("owned-agy-child");
			child.provider_id = parent.provider_id;
			child.native_session_id = child_native_id;
			child.execution_host_id = "local";
			child.workspace_directory = parent.workspace_directory;
			child.created_at = child.updated_at = "2026-10-16T10:01:00Z";
			child.model_id = "owned-child-model";
			child.approval_mode = "plan";
			child.reasoning_effort = "high";
			child.parent_chat_id = "retained-branch-parent";
			child.branch_root_chat_id = "retained-branch-root";
			child.branch_from_message_index = 0;
			child.messages = {{MessageRole::User, "Original child work"}, {MessageRole::Assistant, "Actual stored child answer"}};
			// Independent, valid saved branch ancestry. It is not the native-event
			// display parent and must survive a cold open's isolated preparation.
			ChatSession branch_root = HandlerChat(child.branch_root_chat_id);
			branch_root.provider_id = child.provider_id;
			branch_root.execution_host_id = child.execution_host_id;
			branch_root.workspace_directory = child.workspace_directory;
			branch_root.native_session_id = "770e8400-e29b-41d4-a716-446655440002";
			branch_root.branch_root_chat_id = branch_root.id;
			branch_root.messages = {child.messages.front()};
			ChatSession branch_parent = HandlerChat(child.parent_chat_id);
			branch_parent.provider_id = child.provider_id;
			branch_parent.execution_host_id = child.execution_host_id;
			branch_parent.workspace_directory = child.workspace_directory;
			branch_parent.native_session_id = "880e8400-e29b-41d4-a716-446655440003";
			branch_parent.parent_chat_id = branch_root.id;
			branch_parent.branch_root_chat_id = branch_root.id;
			branch_parent.branch_from_message_index = 0;
			branch_parent.messages = branch_root.messages;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, branch_root));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, branch_parent));
			app.chats.push_back(branch_root);
			app.chats.push_back(branch_parent);
			// Prove this is a valid complete catalogue before testing a partial one.
			auto complete_catalogue = app.chats;
			complete_catalogue.push_back(child);
			ChatBranching::Normalize(complete_catalogue);
			const auto normalized_child = std::ranges::find(complete_catalogue, child.id, &ChatSession::id);
			UAM_ASSERT(normalized_child != complete_catalogue.end());
			UAM_ASSERT_EQ(normalized_child->parent_chat_id, child.parent_chat_id);
			UAM_ASSERT_EQ(normalized_child->branch_root_chat_id, child.branch_root_chat_id);
			UAM_ASSERT_EQ(normalized_child->branch_from_message_index, child.branch_from_message_index);
			UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummariesReadOnly(app.data_root).size(), std::size_t{3});
			ChatDomainService().SelectChatById(app, parent_id);
			app.composer_text = "Unsent independent parent draft";
		}

		ChatSession& Parent()
		{
			ChatSession* parent = ChatDomainService().FindChatById(app, parent_id);
			UAM_ASSERT(parent != nullptr);
			return *parent;
		}
		nlohmann::json Payload() const
		{
			return {{"chatId", parent_id}, {"nativeSessionId", child_native_id}, {"selectChat", false}, {"refreshHistory", false}};
		}
		void Open(UamQueryHandler& handler, CefRefPtr<RecordingInstructionCallback> callback)
		{
			ChatIconHandlerTestAccess::OpenNative(handler, Payload(), callback);
			RunCreationLoopUntil([callback]() { return callback->successes + callback->failures != 0; }, std::chrono::seconds(10));
			UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
		}
	};
} // namespace

UAM_TEST(NativeChildLineageHandlerAntigravityActualEventOpensOnlyExistingSavedChild)
{
	for (const bool already_loaded : {true, false})
	{
		OwnedAntigravityChildHandlerFixture fixture;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.child));
		const auto restored = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.child.id);
		UAM_ASSERT(restored && restored->messages_loaded && !restored->imported_read_only);
		// Full and summary reloads must retain the valid branch receipt before Open.
		for (const bool messages : {false, true})
		{
			const auto baseline = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.child.id, messages);
			UAM_ASSERT(baseline);
			UAM_ASSERT_EQ(baseline->parent_chat_id, fixture.child.parent_chat_id);
			UAM_ASSERT_EQ(baseline->branch_root_chat_id, fixture.child.branch_root_chat_id);
			UAM_ASSERT_EQ(baseline->branch_from_message_index, fixture.child.branch_from_message_index);
		}
		UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummariesReadOnly(fixture.app.data_root).size(), std::size_t{4});
		std::cout << "Antigravity existing child arm: " << (already_loaded ? "warm-loaded" : "cold-saved") << '\n';
		if (already_loaded)
			fixture.app.chats.push_back(*restored);
		const auto parent_bytes = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.parent_id));
		auto handler = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
		fixture.Open(*handler, callback);
		UAM_ASSERT_EQ(callback->successes, 1);
		UAM_ASSERT_EQ(callback->failures, 0);
		UAM_ASSERT_EQ(nlohmann::json::parse(callback->body).at("chatId"), nlohmann::json(fixture.child.id));
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{4});
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), fixture.parent_id);
		UAM_ASSERT_EQ(fixture.app.composer_text, std::string("Unsent independent parent draft"));
		UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.parent_id)), parent_bytes);
		const ChatSession* live = ChatDomainService().FindChatById(fixture.app, fixture.child.id);
		UAM_ASSERT(live != nullptr);
		for (const bool messages : {false, true})
		{
			const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.child.id, messages);
			UAM_ASSERT(saved);
			UAM_ASSERT_EQ(saved->subagent_parent_chat_id, fixture.parent_id);
			UAM_ASSERT_EQ(saved->subagent_parent_created_at, fixture.Parent().created_at);
			UAM_ASSERT_EQ(saved->native_session_id, fixture.child_native_id);
			UAM_ASSERT_EQ(saved->model_id, fixture.child.model_id);
			UAM_ASSERT_EQ(saved->approval_mode, fixture.child.approval_mode);
			UAM_ASSERT_EQ(saved->reasoning_effort, fixture.child.reasoning_effort);
			UAM_ASSERT_EQ(saved->workspace_directory, fixture.child.workspace_directory);
			UAM_ASSERT_EQ(saved->execution_host_id, fixture.child.execution_host_id);
			UAM_ASSERT_EQ(saved->parent_chat_id, fixture.child.parent_chat_id);
			UAM_ASSERT_EQ(saved->branch_root_chat_id, fixture.child.branch_root_chat_id);
			UAM_ASSERT_EQ(saved->branch_from_message_index, fixture.child.branch_from_message_index);
			UAM_ASSERT(!saved->imported_read_only);
			if (messages)
				UAM_ASSERT_EQ(saved->messages, fixture.child.messages);
		}
		UAM_ASSERT_EQ(live->subagent_parent_chat_id, fixture.parent_id);
		UAM_ASSERT_EQ(live->subagent_parent_created_at, fixture.Parent().created_at);
		UAM_ASSERT_EQ(live->messages, fixture.child.messages);
		UAM_ASSERT_EQ(fixture.app.resolved_native_sessions_by_chat_id.at(fixture.child.id), fixture.child_native_id);
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
	}
}

UAM_TEST(NativeChildLineageHandlerAntigravityDoesNotMintMissingOrForeignHostChild)
{
	for (const bool foreign_host : {false, true})
	{
		OwnedAntigravityChildHandlerFixture fixture;
		if (foreign_host)
		{
			fixture.child.execution_host_id = "foreign-host";
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.child));
		}
		const auto parent_bytes = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.parent_id));
		const auto child_path = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.child.id);
		const auto child_bytes = foreign_host ? ReadFile(child_path) : std::string{};
		const auto mappings = fixture.app.resolved_native_sessions_by_chat_id;
		auto handler = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
		fixture.Open(*handler, callback);
		UAM_ASSERT_EQ(callback->successes, 0);
		UAM_ASSERT_EQ(callback->failures, 1);
		UAM_ASSERT_EQ(callback->status, 409);
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{3});
		UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummariesReadOnly(fixture.app.data_root).size(), foreign_host ? std::size_t{4} : std::size_t{3});
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), fixture.parent_id);
		UAM_ASSERT_EQ(fixture.app.composer_text, std::string("Unsent independent parent draft"));
		UAM_ASSERT_EQ(fixture.app.resolved_native_sessions_by_chat_id, mappings);
		UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.parent_id)), parent_bytes);
		if (foreign_host)
			UAM_ASSERT_EQ(ReadFile(child_path), child_bytes);
		else
			UAM_ASSERT(!fs::exists(child_path));
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
	}
}
#endif

UAM_TEST(NativeChildLineageHandlerPassiveRefreshAndColdReopenPreserveReceipt)
{
	for (const std::string provider : {"gemini-cli", "claude-cli", "copilot-cli"})
	{
		NativeLineageHandlerFixture fixture(provider);
		fixture.SeedReceipt(fixture.parent.id, fixture.parent.created_at);
		fixture.Parent().messages.clear(); // Preservation must not manufacture new source evidence.
		auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		auto& handler = *handler_owner;
		ChatSession snapshot = fixture.child;
		snapshot.messages.front().content = "Actual refreshed native journal transcript";
		UAM_ASSERT(snapshot.subagent_parent_chat_id.empty() && snapshot.subagent_parent_created_at.empty());
		const auto refresh = ChatIconHandlerTestAccess::FinishNative(handler, fixture.Payload(), &snapshot);
		UAM_ASSERT(refresh.ok);
		fixture.AssertReceipt(fixture.Child());
		UAM_ASSERT_EQ(fixture.Child().messages.front().content, snapshot.messages.front().content);
		fixture.MakeChildCold();
		CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
		ChatIconHandlerTestAccess::OpenNative(handler, fixture.Payload(), callback);
		UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
		fixture.Await(callback);
		UAM_ASSERT_EQ(callback->successes, 1);
		UAM_ASSERT_EQ(callback->failures, 0);
		fixture.AssertReceipt(fixture.Child());
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), fixture.parent.id);
		UAM_ASSERT(fixture.Child().imported_read_only && fixture.Child().native_session_id.empty());
		UAM_ASSERT(!fixture.app.resolved_native_sessions_by_chat_id.contains(fixture.child.id));
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.child.id);
		UAM_ASSERT(saved);
		fixture.AssertReceipt(*saved);
		UAM_ASSERT_EQ(saved->messages.front().content, snapshot.messages.front().content);
	}
}

UAM_TEST(NativeChildLineageHandlerExistingSaveFailureRestoresReceiptAndMappings)
{
	for (int failure = 0; failure < 3; ++failure)
		for (const bool prior_receipt : {false, true})
		{
			NativeLineageHandlerFixture fixture;
			if (prior_receipt)
				fixture.SeedReceipt("prior-uam-parent", "2026-10-01T09:00:00Z");
			fixture.app.resolved_native_sessions_by_chat_id[fixture.child.id] = "previous-resolved-target-link";
			const ChatSession before = fixture.Child();
			const auto mappings = fixture.app.resolved_native_sessions_by_chat_id;
			const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.child.id);
			const auto saved_bytes = ReadFile(primary);
			if (failure == 0)
				fixture.BlockSettingsPublication();
			const auto retained_primary = fixture.temp.root / "retained-child-primary.json";
			if (failure == 2)
			{
				fs::rename(primary, retained_primary);
				fs::create_directories(primary);
				UAM_ASSERT(uam::io::WriteTextFile(primary / "block-primary-replace", "fixture obstruction"));
			}
			auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
			auto& handler = *handler_owner;
			// A stale actual save receipt exercises the real Finish rollback path;
			// there is no injected save function or duplicate transaction logic.
			const auto result = ChatIconHandlerTestAccess::FinishNative(handler, fixture.Payload(true), nullptr, failure == 1 ? std::string("stale-native-preparation-revision") : std::string{});
			UAM_ASSERT(!result.ok);
			UAM_ASSERT_EQ(result.status, 500);
			UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), fixture.parent.id);
			UAM_ASSERT_EQ(fixture.app.resolved_native_sessions_by_chat_id, mappings);
			UAM_ASSERT_EQ(fixture.Child().subagent_parent_chat_id, before.subagent_parent_chat_id);
			UAM_ASSERT_EQ(fixture.Child().subagent_parent_created_at, before.subagent_parent_created_at);
			UAM_ASSERT_EQ(fixture.Child().last_opened_at, before.last_opened_at);
			UAM_ASSERT_EQ(fixture.Child().messages, before.messages);
			if (failure == 2)
			{
				UAM_ASSERT(fs::is_directory(primary));
				UAM_ASSERT_EQ(ReadFile(primary / "block-primary-replace"), std::string("fixture obstruction"));
				fs::remove_all(primary);
				fs::rename(retained_primary, primary);
			}
			UAM_ASSERT_EQ(ReadFile(primary), saved_bytes);
			const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.child.id);
			UAM_ASSERT(saved);
			UAM_ASSERT_EQ(saved->subagent_parent_chat_id, before.subagent_parent_chat_id);
			UAM_ASSERT_EQ(saved->subagent_parent_created_at, before.subagent_parent_created_at);
		}
}

UAM_TEST(NativeChildLineageHandlerInsertedSaveFailureRemovesTargetAndMapping)
{
	for (int failure = 0; failure < 3; ++failure)
	{
		NativeLineageHandlerFixture fixture("opencode-cli", false);
		const auto mappings = fixture.app.resolved_native_sessions_by_chat_id;
		const auto source_bytes = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.parent.id));
		if (failure == 0)
			fixture.BlockSettingsPublication();
		const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.child.id);
		if (failure == 2)
		{
			fs::create_directories(primary);
			UAM_ASSERT(uam::io::WriteTextFile(primary / "block-primary-replace", "fixture obstruction"));
		}
		auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		auto& handler = *handler_owner;
		const auto result = ChatIconHandlerTestAccess::FinishNative(handler, fixture.Payload(true), &fixture.child, failure == 1 ? std::string("stale-native-preparation-revision") : std::string{});
		UAM_ASSERT(!result.ok);
		UAM_ASSERT_EQ(result.status, 500);
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
		UAM_ASSERT(ChatDomainService().FindChatById(fixture.app, fixture.child.id) == nullptr);
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), fixture.parent.id);
		UAM_ASSERT_EQ(fixture.app.resolved_native_sessions_by_chat_id, mappings);
		if (failure == 2)
		{
			UAM_ASSERT(fs::is_directory(primary));
			UAM_ASSERT_EQ(ReadFile(primary / "block-primary-replace"), std::string("fixture obstruction"));
			fs::remove_all(primary);
		}
		UAM_ASSERT(!fs::exists(primary));
		UAM_ASSERT(!fs::exists(AppPaths::UamChatSummaryFilePath(fixture.app.data_root, fixture.child.id)));
		UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.parent.id)), source_bytes);
		UAM_ASSERT(fixture.Parent().subagent_parent_chat_id.empty());
	}
}

UAM_TEST(NativeChildLineageHandlerRejectsConflictsAndAsyncRecreatedRecords)
{
	for (const bool same_parent_id : {false, true})
	{
		NativeLineageHandlerFixture fixture;
		const std::string recorded_parent = same_parent_id ? fixture.parent.id : "prior-uam-parent";
		const std::string recorded_birth = "2026-10-01T09:00:00Z";
		fixture.SeedReceipt(recorded_parent, recorded_birth);
		auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		auto& handler = *handler_owner;
		const auto result = ChatIconHandlerTestAccess::FinishNative(handler, fixture.Payload());
		UAM_ASSERT(result.ok); // A valid transcript remains openable without transferring its display receipt.
		UAM_ASSERT_EQ(fixture.Child().subagent_parent_chat_id, recorded_parent);
		UAM_ASSERT_EQ(fixture.Child().subagent_parent_created_at, recorded_birth);
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.child.id);
		UAM_ASSERT(saved);
		UAM_ASSERT_EQ(saved->subagent_parent_chat_id, recorded_parent);
		UAM_ASSERT_EQ(saved->subagent_parent_created_at, recorded_birth);
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), fixture.parent.id);
	}
	for (int replacement = 0; replacement < 4; ++replacement)
	{
		NativeLineageHandlerFixture fixture("claude-cli");
		fixture.SeedReceipt(fixture.parent.id, fixture.parent.created_at);
		fixture.MakeChildCold();
		const auto before = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.child.id));
		const auto mappings = fixture.app.resolved_native_sessions_by_chat_id;
		auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		auto& handler = *handler_owner;
		CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
		ChatIconHandlerTestAccess::OpenNative(handler, fixture.Payload(), callback);
		UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
		// The real background read is already queued, and its real completion
		// cannot run on this owner until the existing CEF pump below executes.
		if (replacement == 0)
			fixture.Parent().created_at = "2026-10-16T12:00:00Z";
		if (replacement == 1)
			fixture.Parent().metadata_incarnation = ChatSession{}.metadata_incarnation;
		if (replacement == 2)
			fixture.Child().created_at = "2026-10-16T12:00:00Z";
		if (replacement == 3)
			fixture.Child().metadata_incarnation = ChatSession{}.metadata_incarnation;
		fixture.Await(callback);
		UAM_ASSERT_EQ(callback->successes, 0);
		UAM_ASSERT_EQ(callback->failures, 1);
		UAM_ASSERT_EQ(callback->status, 409);
		UAM_ASSERT_EQ(callback->body, replacement < 2 ? std::string("Source chat changed while its child history was loading.") : std::string("Child chat changed while its history was loading. Try again."));
		UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.child.id)), before);
		UAM_ASSERT_EQ(fixture.app.resolved_native_sessions_by_chat_id, mappings);
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), fixture.parent.id);
		UAM_ASSERT(!fixture.Child().messages_loaded);
		fixture.AssertReceipt(fixture.Child());
	}
}

// Verify the precise priority used by native history preparation, rather than
// treating the USER_BLOCKING initialization witness as proof of this queue.
UAM_TEST(NativeChildLineageHandlerBackgroundQueueExecutes)
{
	const auto observed = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new CreationQueueWitness(observed, 1)));
	RunCreationLoopUntil([observed]() { return observed->load() == 1; }, std::chrono::seconds(10));
	UAM_ASSERT_EQ(observed->load(), 1);
}

#include "app/persistence_coordinator.h"
#include "common/chat/native_attachment_assets.h"
#include "common/chat/native_media_chat_save.h"
#include <set>

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	const std::string kHandlerBranchOriginalPng = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
	const std::string kHandlerBranchOpaqueUri = "https://assets.invalid/handler-branch-reference.pdf";

	struct HandlerBranchOriginalFixture
	{
		TempDir temp{"uam-cef-branch-original"};
		uam::AppState app;
		bool owned = true;
		std::size_t attachment_row = 1;
		std::string source_id, source_key, expected_bytes, source_primary;
		std::optional<std::string> source_revision;
		std::vector<Message> source_messages;

		explicit HandlerBranchOriginalFixture(bool with_original = true, bool selected_user_original = false) : owned(with_original), attachment_row(selected_user_original ? 2 : 1)
		{
			app.data_root = temp.root / "data";
			fs::create_directories(app.data_root);
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			app.settings.git_enabled = app.settings.git_auto_worktree = false;
			const auto workspace = temp.root / "workspace";
			fs::create_directories(workspace);
			ChatSession source = ChatDomainService().CreateNewChat("", uam::provider_ids::kGeminiCli, false);
			source_id = source.id;
			source.title = "Private CEF branch original source";
			source.execution_host_id = "local";
			source.workspace_directory = workspace.string();
			source.native_session_id = "ffffffff-ffff-4fff-8fff-ffffffffffff";
			source.memory_enabled = false;
			source.messages = {{MessageRole::User, "Inspect the retained source."}, {MessageRole::Assistant, "Recorded answer with retained context."}, {MessageRole::User, "Continue from this selected message."}};
			for (std::size_t index = 0; index < source.messages.size(); ++index)
			{
				source.messages[index].provider = uam::provider_ids::kGeminiCli;
				source.messages[index].created_at = "2026-10-17T12:00:0" + std::to_string(index + 1) + "Z";
			}
			UAM_ASSERT(source.messages_loaded && !source.imported_read_only && source.workspace_isolation_kind.empty());
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, source));
			if (owned)
			{
				UAM_ASSERT(uam::base64::Decode(kHandlerBranchOriginalPng, expected_bytes));
				UAM_ASSERT(!expected_bytes.empty());
				const auto root = app.data_root;
				const auto row = attachment_row;
				source = std::async(std::launch::async,
				                    [root, row, candidate = std::move(source)]() mutable
				                    {
					                    MessageAttachment attachment;
					                    attachment.id = "cef-retained-original";
					                    attachment.name = "Recorded original";
					                    attachment.kind = "image";
					                    attachment.source = AttachmentSource::NativeInlinePending;
					                    attachment.native_part_key = "v1:cef-branch/recorded-message/part";
					                    attachment.native_source_field = "content/0/inlineData";
					                    attachment.origin_execution_host_id = "local";
					                    std::string error;
					                    attachment.native_inline_payload = uam::native_assets::MakeInlinePayload(std::string(kHandlerBranchOriginalPng), "image/png", std::make_shared<uam::native_assets::PayloadBudget>(), error);
					                    UAM_ASSERT(attachment.native_inline_payload && error.empty());
					                    attachment.mime_type = attachment.native_inline_payload->MimeType();
					                    attachment.content_digest = attachment.native_inline_payload->Digest();
					                    attachment.size_bytes = attachment.native_inline_payload->DecodedSize();
					                    candidate.messages[row].attachments.push_back(std::move(attachment));
					                    const auto revision = ChatRepository::ChatStorageRevision(root, candidate.id);
					                    UAM_ASSERT(revision);
					                    const auto before = ReadFile(AppPaths::UamChatFilePath(root, candidate.id));
					                    auto prepared = uam::PreparedNativeMediaChatSave::Prepare(root, std::move(candidate), *revision, error);
					                    UAM_ASSERT(prepared && error.empty() && !prepared->PrimaryAcknowledged());
					                    UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(root, prepared->Candidate().id)), before);
					                    UAM_ASSERT(prepared->Publish(error, [] { return true; }));
					                    UAM_ASSERT(prepared->PrimaryAcknowledged() && error.empty());
					                    UAM_ASSERT(prepared->AcknowledgeOriginals(error) && error.empty());
					                    auto committed = prepared->TakeCommittedChat();
					                    UAM_ASSERT(committed);
					                    return std::move(*committed);
				                    })
				             .get();
				source_key = source.messages[attachment_row].attachments.front().native_asset_key;
				UAM_ASSERT(uam::native_assets::IsKeyForOwner(source_id, source_key));
				UAM_ASSERT(!uam::native_assets::IsKeyForOwner(source.native_session_id, source_key));
			}
			else
			{
				MessageAttachment attachment;
				attachment.id = "cef-opaque-reference";
				attachment.name = "Opaque recorded reference";
				attachment.kind = "file";
				attachment.mime_type = "application/pdf";
				attachment.source = AttachmentSource::ProviderUri;
				attachment.source_uri = kHandlerBranchOpaqueUri;
				attachment.native_part_key = "v1:cef-branch/recorded-message/uri-part";
				attachment.native_source_field = "content/1/fileData";
				attachment.origin_execution_host_id = "local";
				source.messages[attachment_row].attachments.push_back(std::move(attachment));
			}
			// Existing public wrapper semantics preserve a branch with operationId
			// when this real, private invalid-workspace preflight blocks regeneration.
			const auto barrier = temp.root / "not-a-workspace-directory";
			UAM_ASSERT(uam::io::WriteTextFile(barrier, "private regular-file barrier"));
			source.workspace_directory = barrier.string();
			UAM_ASSERT(fs::is_regular_file(barrier) && !fs::is_directory(barrier));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, source));
			source_messages = source.messages;
			app.chats.push_back(std::move(source));
			ChatDomainService().SelectChatById(app, source_id);
			UAM_ASSERT(PersistenceCoordinator().SaveSettings(app));
			source_primary = ReadFile(AppPaths::UamChatFilePath(app.data_root, source_id));
			source_revision = ChatRepository::ChatStorageRevision(app.data_root, source_id);
			UAM_ASSERT(!source_primary.empty() && source_revision);
			AssertSource();
			AssertNoProviderProcess();
		}

		void AssertNoProviderProcess()
		{
			for (const auto& session : app.acp_sessions)
			{
				UAM_ASSERT(session && !session->running && !session->stdin_writer);
				UAM_ASSERT(session->last_process_id.empty() && session->prompt_request_id == 0);
#if defined(_WIN32)
				UAM_ASSERT(session->process_info.dwProcessId == 0);
#else
				UAM_ASSERT(session->child_pid == -1 && session->watchdog_pid == -1);
#endif
			}
			for (const auto& terminal : app.cli_terminals)
			{
				UAM_ASSERT(terminal && !terminal->running && !terminal->input_writer);
				UAM_ASSERT(!terminal->uam_control_session && !terminal->uam_control_relay);
				UAM_ASSERT(!terminal->context_preparation && !terminal->native_session_setup_cancel);
				UAM_ASSERT(!uam::platform::CliTerminalHasWritableInput(*terminal));
#if defined(_WIN32)
				UAM_ASSERT(terminal->process_info.dwProcessId == 0 && terminal->pseudo_console == nullptr);
#else
				UAM_ASSERT(terminal->child_pid == -1 && terminal->watchdog_pid == -1);
#endif
			}
		}

		void AssertSource()
		{
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(app.data_root, source_id)), source_primary);
			UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(app.data_root, source_id), source_revision);
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, source_id);
			UAM_ASSERT(saved && saved->id == source_id && !saved->imported_read_only);
			UAM_ASSERT_EQ(saved->messages, source_messages);
			const auto* current = ChatDomainService().FindChatById(app, source_id);
			UAM_ASSERT(current);
			if (current->messages_loaded)
				UAM_ASSERT_EQ(current->messages, source_messages);
			else
				UAM_ASSERT(current->messages.empty() && current->persisted_message_count == source_messages.size());
			const auto& attachment = saved->messages[attachment_row].attachments.front();
			if (owned)
			{
				UAM_ASSERT_EQ(attachment.source, AttachmentSource::OwnedNativeAsset);
				UAM_ASSERT_EQ(attachment.native_asset_key, source_key);
				const auto original = uam::native_assets::ReadOwned(app.data_root, source_id, source_key);
				UAM_ASSERT(original.ok && original.error.empty());
				UAM_ASSERT_EQ(original.bytes, expected_bytes);
			}
			else
				UAM_ASSERT_EQ(attachment.source_uri, kHandlerBranchOpaqueUri);
		}

		void MakeCold(const std::string& id)
		{
			const auto summary = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, id);
			UAM_ASSERT(summary && !summary->messages_loaded && summary->messages.empty());
			auto* current = ChatDomainService().FindChatById(app, id);
			UAM_ASSERT(current);
			*current = *summary;
		}

		nlohmann::json Payload(const std::string& operation) const
		{
			return {{"chatId", source_id}, {"messageIndex", 2}, {"operationId", operation}};
		}

		CefRefPtr<RecordingInstructionCallback> Start(UamQueryHandler& handler, const nlohmann::json& payload)
		{
			CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
			UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, "branchFromMessage", payload, callback));
			return callback;
		}

		std::string AwaitRetainedBranch(CefRefPtr<RecordingInstructionCallback> callback)
		{
			RunCreationLoopUntil([callback]() { return callback->successes + callback->failures != 0; }, std::chrono::seconds(10));
			UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
			AssertSource();
			AssertNoProviderProcess();
			UAM_ASSERT_EQ(callback->failures, 0);
			UAM_ASSERT_EQ(callback->successes, 1);
			const auto body = nlohmann::json::parse(callback->body);
			const auto branch_id = body.value("chatId", "");
			UAM_ASSERT(!branch_id.empty() && branch_id != source_id);
			UAM_ASSERT(!body.value("warning", "").empty());
			return branch_id;
		}

		std::string AssertBranch(const std::string& id)
		{
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, id);
			UAM_ASSERT(saved && saved->id == id && saved->parent_chat_id == source_id);
			UAM_ASSERT_EQ(saved->branch_from_message_index, 2);
			UAM_ASSERT(saved->native_session_id.empty() && !saved->branch_retry_error.empty());
			UAM_ASSERT(saved->messages.size() >= source_messages.size());
			for (std::size_t index = 0; index < source_messages.size(); ++index)
			{
				UAM_ASSERT_EQ(saved->messages[index].content, source_messages[index].content);
				UAM_ASSERT_EQ(saved->messages[index].created_at, source_messages[index].created_at);
			}
			for (std::size_t index = source_messages.size(); index < saved->messages.size(); ++index)
				UAM_ASSERT(saved->messages[index].role == MessageRole::Assistant && saved->messages[index].content.empty() && saved->messages[index].tool_calls.empty() && saved->messages[index].blocks.empty());
			UAM_ASSERT_EQ(saved->messages[attachment_row].attachments.size(), std::size_t{1});
			const auto& attachment = saved->messages[attachment_row].attachments.front();
			if (owned)
			{
				UAM_ASSERT_EQ(attachment.source, AttachmentSource::OwnedNativeAsset);
				UAM_ASSERT(uam::native_assets::IsKeyForOwner(id, attachment.native_asset_key));
				UAM_ASSERT(!uam::native_assets::IsKeyForOwner(source_id, attachment.native_asset_key));
				auto expected = source_messages[attachment_row].attachments.front();
				expected.native_asset_key = attachment.native_asset_key;
				UAM_ASSERT_EQ(attachment, expected);
				const auto original = uam::native_assets::ReadOwned(app.data_root, id, attachment.native_asset_key);
				UAM_ASSERT(original.ok && original.error.empty());
				UAM_ASSERT_EQ(original.bytes, expected_bytes);
				return attachment.native_asset_key;
			}
			UAM_ASSERT_EQ(attachment, source_messages[attachment_row].attachments.front());
			UAM_ASSERT_EQ(attachment.source_uri, kHandlerBranchOpaqueUri);
			return {};
		}

		std::set<std::string> Generations() const
		{
			std::set<std::string> result;
			const auto store = app.data_root / std::string(uam::native_assets::kStoreDirectory);
			if (!fs::exists(store))
				return result;
			for (const auto& owner : fs::directory_iterator(store))
				if (owner.is_directory())
					for (const auto& generation : fs::directory_iterator(owner.path()))
						if (generation.is_directory())
							result.insert(generation.path().lexically_relative(store).generic_string());
			return result;
		}
	};

	struct HandlerBranchQueueRelease
	{
		std::promise<void> promise;
		bool released = false;
		void Release()
		{
			if (!released)
			{
				released = true;
				promise.set_value();
			}
		}
		~HandlerBranchQueueRelease()
		{
			Release();
		}
	};
} // namespace

UAM_TEST(ChatCreateBranchOriginalLoadedWaitsForWorkerAndRetainsBytes)
{
	HandlerBranchOriginalFixture fixture;
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	HandlerBranchQueueRelease release;
	const auto held = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(release.promise.get_future().share(), held)));
	RunCreationLoopUntil([held]() { return held->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(held->load(), 1);
	const auto witnessed = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new CreationQueueWitness(witnessed, 2)));
	UAM_ASSERT(CefPostTask(TID_UI, new CreationQueueWitness(witnessed, 1)));
	const auto started = std::chrono::steady_clock::now();
	const auto callback = fixture.Start(handler, fixture.Payload("loaded-original-operation"));
	const auto elapsed = std::chrono::steady_clock::now() - started;
	UAM_ASSERT(elapsed < std::chrono::milliseconds(100));
	RunCreationLoopUntil([witnessed]() { return (witnessed->load() & 1) != 0; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(witnessed->load(), 1);
	UAM_ASSERT_EQ(held->load(), 1);
	fixture.AssertSource();
	fixture.AssertNoProviderProcess();
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	release.Release();
	const auto branch_id = fixture.AwaitRetainedBranch(callback);
	UAM_ASSERT(!fixture.AssertBranch(branch_id).empty());
	RunCreationLoopUntil([witnessed]() { return witnessed->load() == 3; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(witnessed->load(), 3);
	fixture.AssertSource();
}

UAM_TEST(ChatCreateBranchOriginalColdRetainsBytesWithoutWarmingSource)
{
	HandlerBranchOriginalFixture fixture;
	fixture.MakeCold(fixture.source_id);
	fixture.AssertSource();
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	const auto callback = fixture.Start(handler, fixture.Payload("cold-original-operation"));
	const auto branch_id = fixture.AwaitRetainedBranch(callback);
	UAM_ASSERT(!fixture.AssertBranch(branch_id).empty());
	const auto* source = ChatDomainService().FindChatById(fixture.app, fixture.source_id);
	UAM_ASSERT(source && !source->messages_loaded && source->messages.empty());
	fixture.AssertSource();
}

UAM_TEST(ChatCreateBranchOriginalOperationReplayKeepsOwnerAndGeneration)
{
	HandlerBranchOriginalFixture fixture(true, true);
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	const auto payload = fixture.Payload("selected-original-operation");
	const auto first = fixture.Start(handler, payload);
	const auto branch_id = fixture.AwaitRetainedBranch(first);
	const auto branch_key = fixture.AssertBranch(branch_id);
	UAM_ASSERT(!branch_key.empty());
	const auto generations = fixture.Generations();
	UAM_ASSERT(generations.size() >= std::size_t{2});
	auto* branch = ChatDomainService().FindChatById(fixture.app, branch_id);
	UAM_ASSERT(branch && !branch->branch_retry_error.empty());
	// Preserve the real first failed-regeneration receipt with an explicit
	// witness; an accidental second Retry would overwrite this stored error.
	branch->branch_retry_error += " [first regeneration receipt witness]";
	const auto first_error = branch->branch_retry_error;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *branch));
	const auto primary = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, branch_id));
	fixture.MakeCold(fixture.source_id);
	fixture.MakeCold(branch_id);
	const auto replay = fixture.Start(handler, payload);
	UAM_ASSERT_EQ(fixture.AwaitRetainedBranch(replay), branch_id);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.AssertBranch(branch_id), branch_key);
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, branch_id)), primary);
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, branch_id);
	UAM_ASSERT(saved && saved->branch_retry_error == first_error);
	fixture.AssertSource();
	fixture.AssertNoProviderProcess();
}

UAM_TEST(ChatCreateBranchTextAndOpaqueUriStayOriginalFree)
{
	HandlerBranchOriginalFixture fixture(false);
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	const auto callback = fixture.Start(handler, fixture.Payload("opaque-reference-operation"));
	const auto branch_id = fixture.AwaitRetainedBranch(callback);
	UAM_ASSERT(fixture.AssertBranch(branch_id).empty());
	UAM_ASSERT(fixture.Generations().empty());
	UAM_ASSERT(!fs::exists(fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory)));
	fixture.AssertSource();
	fixture.AssertNoProviderProcess();
}
#endif

#include "cef/state_serializer.h"

namespace
{
	struct HandlerUnsentEditFixture
	{
		TempDir temp{"uam-cef-unsent-local-edit"};
		uam::AppState app;
		std::string id, primary, digest;
		std::vector<Message> messages;
		HandlerUnsentEditFixture()
		{
			app.data_root = temp.root;
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
			ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli", false);
			chat.memory_enabled = chat.uam_control_enabled = false;
			Message row{MessageRole::User, "Original local input"};
			row.provider = "codex-cli";
			row.created_at = "2026-10-17T08:00:00Z";
			row.acp_prompt_not_sent = row.interrupted = true;
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "keep.txt", "keep"));
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "drop.txt", "drop"));
			row.attachments = {{.id = "keep", .name = "keep.txt", .kind = "file", .mime_type = "text/plain", .path = (temp.root / "keep.txt").string(), .size_bytes = 4}, {.id = "drop", .name = "drop.txt", .kind = "file", .mime_type = "text/plain", .path = (temp.root / "drop.txt").string(), .size_bytes = 4}};
			row.markdown_store_files = {"keep-skill.md", "drop-skill.md"};
			row.markdown_store_prompt_blocks = {"CAPTURED_KEEP_SKILL", "CAPTURED_DROP_SKILL"};
			chat.messages = {row};
			id = chat.id;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			const auto round_trip = ChatRepository::LoadLocalChat(app.data_root, id);
			UAM_ASSERT(round_trip && round_trip->messages == chat.messages);
			app.chats.push_back(std::move(chat));
			primary = ReadFile(AppPaths::UamChatFilePath(app.data_root, id));
			messages = app.chats.front().messages;
			digest = uam::StateSerializer::MessageDigest(app.chats.front());
			UAM_ASSERT_EQ(digest.size(), std::size_t{16});
			UAM_ASSERT_EQ(uam::KnownUnsentAcpMessageIndices(app, app.chats.front()), (std::vector<int>{0}));
		}
		nlohmann::json Payload() const
		{
			return {{"chatId", id}, {"operation", "editUnsent"}, {"messageIndex", 0}, {"expectedMessagesDigest", digest}, {"text", "  Edited local input  "}, {"retainedAttachmentIds", {"keep"}}, {"retainedMarkdownStoreFiles", {"keep-skill.md"}}};
		}
		CefRefPtr<RecordingInstructionCallback> Invoke(UamQueryHandler& handler, const nlohmann::json& payload)
		{
			CefRefPtr<RecordingInstructionCallback> cb = new RecordingInstructionCallback;
			UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(handler, "manageQueuedAcpPrompt", payload, cb));
			UAM_ASSERT_EQ(cb->successes + cb->failures, 1);
			UAM_ASSERT(app.acp_sessions.empty() && app.cli_terminals.empty());
			UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty());
			UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
			return cb;
		}
		void AssertUnchanged() const
		{
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(app.data_root, id)), primary);
			UAM_ASSERT_EQ(app.chats.front().messages, messages);
		}
	};
} // namespace

UAM_TEST(ChatCreateUnsentEditReturnsOnlyCommittedLocalRow)
{
	HandlerUnsentEditFixture fixture;
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	const auto cb = fixture.Invoke(handler, fixture.Payload());
	UAM_ASSERT_EQ(cb->failures, 0);
	const auto result = nlohmann::json::parse(cb->body);
	UAM_ASSERT_EQ(result.at("chatId").get<std::string>(), fixture.id);
	UAM_ASSERT_EQ(result.at("startIndex").get<int>(), 0);
	UAM_ASSERT_EQ(result.at("totalCount").get<int>(), 1);
	UAM_ASSERT(!result.at("unchanged").get<bool>());
	UAM_ASSERT(result.at("messages").is_array() && result.at("messages").size() == 1);
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.id);
	UAM_ASSERT(saved && saved->messages == fixture.app.chats.front().messages);
	const auto& row = saved->messages.front();
	UAM_ASSERT_EQ(row.content, std::string("Edited local input"));
	UAM_ASSERT_EQ(row.created_at, fixture.messages.front().created_at);
	UAM_ASSERT(row.acp_prompt_not_sent && row.interrupted && !row.acp_prompt_discarded);
	UAM_ASSERT_EQ(row.attachments, (std::vector<MessageAttachment>{fixture.messages.front().attachments.front()}));
	UAM_ASSERT_EQ(row.markdown_store_files, (std::vector<std::string>{"keep-skill.md"}));
	UAM_ASSERT_EQ(row.markdown_store_prompt_blocks, (std::vector<std::string>{"CAPTURED_KEEP_SKILL"}));
	UAM_ASSERT(saved->acp_unsent_prompt_recoveries.empty() && saved->acp_queued_prompts.empty());
	UAM_ASSERT_EQ(result.at("messagesDigest").get<std::string>(), uam::StateSerializer::MessageDigest(*saved));
	UAM_ASSERT(result.at("messagesDigest").get<std::string>() != fixture.digest);
}

UAM_TEST(ChatCreateUnsentEditRejectsMalformedSelectionBeforeMutation)
{
	HandlerUnsentEditFixture fixture;
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	for (const std::string& field : {"retainedAttachmentIds", "retainedMarkdownStoreFiles"})
	{
		for (const auto& invalid : {nlohmann::json("not-an-array"), nlohmann::json::array({false}), nlohmann::json::array({""})})
		{
			auto payload = fixture.Payload();
			payload[field] = invalid;
			const auto cb = fixture.Invoke(handler, payload);
			UAM_ASSERT_EQ(cb->successes, 0);
			UAM_ASSERT_EQ(cb->status, 400);
			fixture.AssertUnchanged();
		}
	}
}

UAM_TEST(ChatCreateUnsentEditRejectsStaleBodyWithoutBranchOrQueue)
{
	HandlerUnsentEditFixture fixture;
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	auto payload = fixture.Payload();
	payload["expectedMessagesDigest"] = "ffffffffffffffff";
	UAM_ASSERT(payload["expectedMessagesDigest"] != fixture.digest);
	const auto cb = fixture.Invoke(handler, payload);
	UAM_ASSERT_EQ(cb->successes, 0);
	UAM_ASSERT_EQ(cb->status, 409);
	fixture.AssertUnchanged();
}

#include "common/runtime/native_refresh_lifetime.h"

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	void AwaitBranchFaultOwnerCount(unsigned expected)
	{
		RunCreationLoopUntil([expected]() { return uam::native_history::detail::refresh_owners.load() == expected; }, std::chrono::seconds(10));
		UAM_ASSERT_EQ(uam::native_history::detail::refresh_owners.load(), expected);
	}

	fs::path AddedBranchFaultGeneration(const HandlerBranchOriginalFixture& fixture, const std::set<std::string>& before)
	{
		std::vector<std::string> added;
		for (const auto& generation : fixture.Generations())
			if (!before.contains(generation))
				added.push_back(generation);
		UAM_ASSERT_EQ(added.size(), std::size_t{1});
		return fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory) / added.front();
	}
} // namespace

UAM_TEST(ChatCreateBranchOriginalCancelledHeldWorkerPreservesSource)
{
	AwaitBranchFaultOwnerCount(0);
	const unsigned baseline = uam::native_history::detail::refresh_owners.load();
	HandlerBranchOriginalFixture fixture;
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	const auto generations = fixture.Generations();
	const auto selected = ChatDomainService().SelectedChatId(fixture.app);
	HandlerBranchQueueRelease release;
	const auto held = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(release.promise.get_future().share(), held)));
	RunCreationLoopUntil([held]() { return held->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(held->load(), 1);
	const auto callback = fixture.Start(handler, fixture.Payload("cancel-held-original-operation"));
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	UAM_ASSERT_EQ(uam::native_history::detail::refresh_owners.load(), baseline + 1);
	ChatDomainService().CancelBranchPreparations(fixture.app, fixture.source_id);
	UAM_ASSERT_EQ(held->load(), 1);
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	release.Release();
	RunCreationLoopUntil([callback]() { return callback->successes + callback->failures != 0; }, std::chrono::seconds(10));
	UAM_ASSERT_EQ(callback->successes, 0);
	UAM_ASSERT_EQ(callback->failures, 1);
	UAM_ASSERT_EQ(callback->status, 409);
	AwaitBranchFaultOwnerCount(baseline);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selected);
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	fixture.AssertSource();
	fixture.AssertNoProviderProcess();
}

UAM_TEST(ChatCreateBranchOriginalStaleIncarnationRetainsUnacknowledgedBytes)
{
	AwaitBranchFaultOwnerCount(0);
	const unsigned baseline = uam::native_history::detail::refresh_owners.load();
	HandlerBranchOriginalFixture fixture;
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	const auto generations = fixture.Generations();
	const auto selected = ChatDomainService().SelectedChatId(fixture.app);
	HandlerBranchQueueRelease release;
	const auto held = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(release.promise.get_future().share(), held)));
	RunCreationLoopUntil([held]() { return held->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(held->load(), 1);
	const auto callback = fixture.Start(handler, fixture.Payload("stale-incarnation-original-operation"));
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	UAM_ASSERT_EQ(uam::native_history::detail::refresh_owners.load(), baseline + 1);
	auto* source = ChatDomainService().FindChatById(fixture.app, fixture.source_id);
	UAM_ASSERT(source);
	const auto replacement = ChatSession{}.metadata_incarnation;
	UAM_ASSERT(source->metadata_incarnation != replacement);
	source->metadata_incarnation = replacement;
	// Incarnation is runtime-only: the worker can prepare the captured valid
	// source; final UI admission must reject it without publishing or ACK.
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id), fixture.source_revision);
	UAM_ASSERT_EQ(held->load(), 1);
	release.Release();
	RunCreationLoopUntil([callback]() { return callback->successes + callback->failures != 0; }, std::chrono::seconds(10));
	UAM_ASSERT_EQ(callback->successes, 0);
	UAM_ASSERT_EQ(callback->failures, 1);
	UAM_ASSERT_EQ(callback->status, 409);
	AwaitBranchFaultOwnerCount(baseline);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selected);
	UAM_ASSERT_EQ(ChatDomainService().FindChatById(fixture.app, fixture.source_id)->metadata_incarnation, replacement);
	const auto generation = AddedBranchFaultGeneration(fixture, generations);
	UAM_ASSERT(fs::is_regular_file(generation / "attempted.receipt"));
	UAM_ASSERT(!fs::exists(generation / "published.receipt"));
	const auto& original = fixture.source_messages[fixture.attachment_row].attachments.front();
	UAM_ASSERT_EQ(ReadFile(generation / (original.content_digest + ".bin")), fixture.expected_bytes);
	fixture.AssertSource();
	fixture.AssertNoProviderProcess();
}

UAM_TEST(ChatCreateBranchOriginalAckFailurePreservesRealBranchOutcome)
{
	AwaitBranchFaultOwnerCount(0);
	const unsigned baseline = uam::native_history::detail::refresh_owners.load();
	HandlerBranchOriginalFixture fixture;
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	const auto generations = fixture.Generations();
	HandlerBranchQueueRelease preparation_release, acknowledgement_release;
	const auto preparation_held = std::make_shared<std::atomic<int>>(0);
	const auto acknowledgement_held = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(preparation_release.promise.get_future().share(), preparation_held)));
	RunCreationLoopUntil([preparation_held]() { return preparation_held->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(preparation_held->load(), 1);
	const auto callback = fixture.Start(handler, fixture.Payload("ack-obstruction-original-operation"));
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	UAM_ASSERT_EQ(uam::native_history::detail::refresh_owners.load(), baseline + 1);
	// Queue order is hold -> real preparation -> second hold. UI commit may
	// run next, but its ACK worker must remain behind that already queued hold.
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(acknowledgement_release.promise.get_future().share(), acknowledgement_held)));
	preparation_release.Release();
	RunCreationLoopUntil([acknowledgement_held]() { return acknowledgement_held->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(acknowledgement_held->load(), 1);
	RunCreationLoopUntil([&fixture]() { return fixture.app.chats.size() == 2; }, std::chrono::seconds(10));
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	const auto* selected = ChatDomainService().SelectedChat(fixture.app);
	UAM_ASSERT(selected && selected->id != fixture.source_id);
	const auto branch_id = selected->id;
	const auto branch_key = fixture.AssertBranch(branch_id);
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, branch_id);
	const auto committed_primary = ReadFile(primary);
	UAM_ASSERT(!committed_primary.empty() && !branch_key.empty());
	const auto generation = AddedBranchFaultGeneration(fixture, generations);
	UAM_ASSERT(fs::is_regular_file(generation / "attempted.receipt"));
	const auto obstruction = generation / "published.receipt";
	UAM_ASSERT(!fs::exists(obstruction));
	fs::create_directories(obstruction);
	UAM_ASSERT(uam::io::WriteTextFile(obstruction / "keep", "private acknowledgement obstruction"));
	UAM_ASSERT_EQ(acknowledgement_held->load(), 1);
	fixture.AssertSource();
	fixture.AssertNoProviderProcess();
	acknowledgement_release.Release();
	UAM_ASSERT_EQ(fixture.AwaitRetainedBranch(callback), branch_id);
	const auto body = nlohmann::json::parse(callback->body);
	UAM_ASSERT(body.at("warning").get<std::string>().find("Attached originals were kept, but their save could not be fully confirmed.") != std::string::npos);
	UAM_ASSERT_EQ(callback->successes, 1);
	UAM_ASSERT_EQ(callback->failures, 0);
	UAM_ASSERT_EQ(ReadFile(primary), committed_primary);
	UAM_ASSERT_EQ(fixture.AssertBranch(branch_id), branch_key);
	UAM_ASSERT(fs::is_directory(obstruction));
	UAM_ASSERT_EQ(ReadFile(obstruction / "keep"), std::string("private acknowledgement obstruction"));
	UAM_ASSERT(fs::is_regular_file(generation / "attempted.receipt"));
	AwaitBranchFaultOwnerCount(baseline);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
	fixture.AssertSource();
	fixture.AssertNoProviderProcess();
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	enum class DeliveredSkillSelection
	{
		Subset,
		Empty,
		Omitted
	};

	void RunDeliveredEditSkillSelection(DeliveredSkillSelection selection)
	{
		AwaitBranchFaultOwnerCount(0);
		const unsigned baseline = uam::native_history::detail::refresh_owners.load();
		HandlerBranchOriginalFixture fixture(true, true);
		const std::vector<std::string> files = {(fixture.temp.root / "keep-skill.md").string(), (fixture.temp.root / "drop-skill.md").string()};
		const std::vector<std::string> blocks = {"  SAVED KEEP SKILL\nsecond line\n", "SAVED DROP SKILL\n  preserved spaces  "};
		const std::string prefix_file = (fixture.temp.root / "prefix-skill.md").string();
		const std::string prefix_block = "SAVED PREFIX SKILL\n";
		const std::vector<std::string> resource_files = {prefix_file, files[0], files[1]};
		const std::vector<std::string> disk_bytes = {"CURRENT PREFIX DISK CONTENT", "CURRENT KEEP DISK CONTENT", "CURRENT DROP DISK CONTENT"};
		for (std::size_t index = 0; index < resource_files.size(); ++index)
		{
			UAM_ASSERT(uam::io::WriteTextFile(fs::path(resource_files[index]), disk_bytes[index]));
			UAM_ASSERT_EQ(ReadFile(fs::path(resource_files[index])), disk_bytes[index]);
		}
		UAM_ASSERT(prefix_block != disk_bytes[0] && blocks[0] != disk_bytes[1] && blocks[1] != disk_bytes[2]);
		auto* source = ChatDomainService().FindChatById(fixture.app, fixture.source_id);
		UAM_ASSERT(source && source->messages_loaded && source->messages.size() == 3);
		UAM_ASSERT_EQ(fixture.attachment_row, std::size_t{2});
		UAM_ASSERT_EQ(source->messages[2].attachments.size(), std::size_t{1});
		UAM_ASSERT_EQ(source->messages[2].attachments.front().source, AttachmentSource::OwnedNativeAsset);
		source->messages[0].markdown_store_files = {prefix_file};
		source->messages[0].markdown_store_prompt_blocks = {prefix_block};
		source->messages[2].markdown_store_files = files;
		source->messages[2].markdown_store_prompt_blocks = blocks;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *source));
		fixture.source_messages = source->messages;
		fixture.source_primary = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.source_id));
		fixture.source_revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id);
		UAM_ASSERT(!fixture.source_primary.empty() && fixture.source_revision);
		fixture.AssertSource();
		fixture.AssertNoProviderProcess();
		UAM_ASSERT(fs::is_regular_file(fs::path(source->workspace_directory)));
		UAM_ASSERT(!fs::is_directory(fs::path(source->workspace_directory)));
		const std::string native_id = source->messages[2].attachments.front().id;
		UAM_ASSERT(!native_id.empty() && native_id != files[0] && native_id != files[1]);
		const std::string edited = "Edited delivered input retaining independently selected resources.";
		auto payload = fixture.Payload("delivered-skill-selection-operation");
		payload["content"] = edited;
		payload["retainedAttachmentIds"] = std::vector<std::string>{native_id};
		std::vector<std::string> expected_files = files;
		std::vector<std::string> expected_blocks = blocks;
		if (selection == DeliveredSkillSelection::Subset)
		{
			payload["retainedMarkdownStoreFiles"] = std::vector<std::string>{files[0]};
			expected_files = {files[0]};
			expected_blocks = {blocks[0]};
		}
		else if (selection == DeliveredSkillSelection::Empty)
		{
			payload["retainedMarkdownStoreFiles"] = nlohmann::json::array();
			expected_files.clear();
			expected_blocks.clear();
		}
		else
			UAM_ASSERT(!payload.contains("retainedMarkdownStoreFiles"));
		auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		auto& handler = *handler_owner;
		const auto callback = fixture.Start(handler, payload);
		const auto branch_id = fixture.AwaitRetainedBranch(callback);
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, branch_id);
		UAM_ASSERT(saved && saved->messages_loaded && saved->messages.size() >= fixture.source_messages.size());
		// Drain the existing worker owner before an intended assertion failure can destroy the fixture.
		AwaitBranchFaultOwnerCount(baseline);
		// First skill-result oracle: the public action must persist the requested subset, not all context.
		UAM_ASSERT_EQ(saved->messages[2].markdown_store_files, expected_files);
		UAM_ASSERT_EQ(saved->messages[2].markdown_store_prompt_blocks, expected_blocks);
		for (std::size_t index = 0; index < 2; ++index)
		{
			UAM_ASSERT_EQ(saved->messages[index].markdown_store_files, fixture.source_messages[index].markdown_store_files);
			UAM_ASSERT_EQ(saved->messages[index].markdown_store_prompt_blocks, fixture.source_messages[index].markdown_store_prompt_blocks);
			UAM_ASSERT_EQ(saved->messages[index].content, fixture.source_messages[index].content);
			UAM_ASSERT_EQ(saved->messages[index].created_at, fixture.source_messages[index].created_at);
		}
		UAM_ASSERT_EQ(saved->messages[2].content, edited);
		UAM_ASSERT_EQ(saved->messages[2].created_at, fixture.source_messages[2].created_at);
		UAM_ASSERT_EQ(saved->parent_chat_id, fixture.source_id);
		UAM_ASSERT_EQ(saved->branch_from_message_index, 2);
		UAM_ASSERT(saved->branch_message_edited && saved->native_session_id.empty() && !saved->branch_retry_error.empty());
		for (std::size_t index = fixture.source_messages.size(); index < saved->messages.size(); ++index)
			UAM_ASSERT(saved->messages[index].role == MessageRole::Assistant && saved->messages[index].content.empty() && saved->messages[index].tool_calls.empty() && saved->messages[index].blocks.empty());
		UAM_ASSERT_EQ(saved->messages[2].attachments.size(), std::size_t{1});
		const auto& attachment = saved->messages[2].attachments.front();
		UAM_ASSERT_EQ(attachment.source, AttachmentSource::OwnedNativeAsset);
		UAM_ASSERT(uam::native_assets::IsKeyForOwner(branch_id, attachment.native_asset_key));
		UAM_ASSERT(!uam::native_assets::IsKeyForOwner(fixture.source_id, attachment.native_asset_key));
		auto expected_attachment = fixture.source_messages[2].attachments.front();
		expected_attachment.native_asset_key = attachment.native_asset_key;
		UAM_ASSERT_EQ(attachment, expected_attachment);
		const auto original = uam::native_assets::ReadOwned(fixture.app.data_root, branch_id, attachment.native_asset_key);
		UAM_ASSERT(original.ok && original.error.empty());
		UAM_ASSERT_EQ(original.bytes, fixture.expected_bytes);
		for (std::size_t index = 0; index < resource_files.size(); ++index)
		{
			UAM_ASSERT(fs::is_regular_file(fs::path(resource_files[index])));
			UAM_ASSERT_EQ(ReadFile(fs::path(resource_files[index])), disk_bytes[index]);
		}
		UAM_ASSERT_EQ(callback->successes, 1);
		UAM_ASSERT_EQ(callback->failures, 0);
		fixture.AssertSource();
		fixture.AssertNoProviderProcess();
	}
} // namespace

UAM_TEST(ChatCreateDeliveredEditRetainsSelectedSkillSnapshotAndNativeOriginal)
{
	RunDeliveredEditSkillSelection(DeliveredSkillSelection::Subset);
}

UAM_TEST(ChatCreateDeliveredEditExplicitEmptySkillsRemovesSavedContextOnly)
{
	RunDeliveredEditSkillSelection(DeliveredSkillSelection::Empty);
}

UAM_TEST(ChatCreateDeliveredEditOmittedSkillsPreservesLegacySnapshotsAndNativeOriginal)
{
	RunDeliveredEditSkillSelection(DeliveredSkillSelection::Omitted);
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	struct DeliveredBranchSkillFixture : HandlerBranchOriginalFixture
	{
		std::vector<std::string> files, blocks, resource_files, disk_bytes;
		std::string prefix_file, prefix_block;
		const std::string edited = "Edited delivered input with separate native and skill selections.";

		DeliveredBranchSkillFixture() : HandlerBranchOriginalFixture(true, true)
		{
			files = {(temp.root / "keep-skill.md").string(), (temp.root / "drop-skill.md").string()};
			blocks = {"  SAVED KEEP SKILL\nsecond line\n", "SAVED DROP SKILL\n  preserved spaces  "};
			prefix_file = (temp.root / "prefix-skill.md").string();
			prefix_block = "SAVED PREFIX SKILL\n";
			resource_files = {prefix_file, files[0], files[1]};
			disk_bytes = {"CURRENT PREFIX DISK CONTENT", "CURRENT KEEP DISK CONTENT", "CURRENT DROP DISK CONTENT"};
			for (std::size_t index = 0; index < resource_files.size(); ++index)
				UAM_ASSERT(uam::io::WriteTextFile(fs::path(resource_files[index]), disk_bytes[index]));
			UAM_ASSERT(prefix_block != disk_bytes[0] && blocks[0] != disk_bytes[1] && blocks[1] != disk_bytes[2]);
			Source().messages[0].markdown_store_files = {prefix_file};
			Source().messages[0].markdown_store_prompt_blocks = {prefix_block};
			Source().messages[2].markdown_store_files = files;
			Source().messages[2].markdown_store_prompt_blocks = blocks;
			SaveSourceWitness();
			AssertResources();
			AssertSource();
			AssertNoProviderProcess();
		}

		ChatSession& Source()
		{
			auto* source = ChatDomainService().FindChatById(app, source_id);
			UAM_ASSERT(source);
			return *source;
		}

		void SaveSourceWitness()
		{
			UAM_ASSERT(Source().messages_loaded);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, Source()));
			source_messages = Source().messages;
			source_primary = ReadFile(AppPaths::UamChatFilePath(app.data_root, source_id));
			source_revision = ChatRepository::ChatStorageRevision(app.data_root, source_id);
			UAM_ASSERT(!source_primary.empty() && source_revision);
		}

		void AssertResources() const
		{
			for (std::size_t index = 0; index < resource_files.size(); ++index)
			{
				UAM_ASSERT(fs::is_regular_file(fs::path(resource_files[index])));
				UAM_ASSERT_EQ(ReadFile(fs::path(resource_files[index])), disk_bytes[index]);
			}
		}

		void AssertSavedSourceAndOriginal() const
		{
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(app.data_root, source_id)), source_primary);
			UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(app.data_root, source_id), source_revision);
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, source_id, true);
			UAM_ASSERT(saved);
			UAM_ASSERT_EQ(saved->messages, source_messages);
			const auto original = uam::native_assets::ReadOwned(app.data_root, source_id, source_key);
			UAM_ASSERT(original.ok && original.error.empty());
			UAM_ASSERT_EQ(original.bytes, expected_bytes);
		}

		nlohmann::json DeliveredPayload(const std::string& operation) const
		{
			auto payload = Payload(operation);
			payload["content"] = edited;
			payload["retainedAttachmentIds"] = std::vector<std::string>{source_messages[2].attachments.front().id};
			return payload;
		}

		std::string AwaitSuccess(CefRefPtr<RecordingInstructionCallback> callback)
		{
			const auto id = AwaitRetainedBranch(callback);
			AwaitBranchFaultOwnerCount(0);
			return id;
		}

		void AwaitFailure(CefRefPtr<RecordingInstructionCallback> callback, int status)
		{
			RunCreationLoopUntil([callback]() { return callback->successes + callback->failures != 0; }, std::chrono::seconds(10));
			AwaitBranchFaultOwnerCount(0);
			UAM_ASSERT_EQ(callback->successes, 0);
			UAM_ASSERT_EQ(callback->failures, 1);
			UAM_ASSERT_EQ(callback->status, status);
		}

		std::string AssertDeliveredBranch(const std::string& id, const std::vector<std::string>& expected_files, const std::vector<std::string>& expected_blocks)
		{
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, id, true);
			UAM_ASSERT(saved && saved->messages_loaded && saved->messages.size() >= source_messages.size());
			UAM_ASSERT_EQ(saved->parent_chat_id, source_id);
			UAM_ASSERT_EQ(saved->branch_from_message_index, 2);
			UAM_ASSERT(saved->branch_message_edited && saved->native_session_id.empty() && !saved->branch_retry_error.empty());
			for (std::size_t index = 0; index < 2; ++index)
			{
				UAM_ASSERT_EQ(saved->messages[index].markdown_store_files, source_messages[index].markdown_store_files);
				UAM_ASSERT_EQ(saved->messages[index].markdown_store_prompt_blocks, source_messages[index].markdown_store_prompt_blocks);
				UAM_ASSERT_EQ(saved->messages[index].content, source_messages[index].content);
				UAM_ASSERT_EQ(saved->messages[index].created_at, source_messages[index].created_at);
			}
			UAM_ASSERT_EQ(saved->messages[2].content, edited);
			UAM_ASSERT_EQ(saved->messages[2].created_at, source_messages[2].created_at);
			UAM_ASSERT_EQ(saved->messages[2].markdown_store_files, expected_files);
			UAM_ASSERT_EQ(saved->messages[2].markdown_store_prompt_blocks, expected_blocks);
			UAM_ASSERT_EQ(saved->messages[2].attachments.size(), std::size_t{1});
			const auto& attachment = saved->messages[2].attachments.front();
			auto expected = source_messages[2].attachments.front();
			expected.native_asset_key = attachment.native_asset_key;
			UAM_ASSERT_EQ(attachment, expected);
			UAM_ASSERT(uam::native_assets::IsKeyForOwner(id, attachment.native_asset_key));
			UAM_ASSERT(!uam::native_assets::IsKeyForOwner(source_id, attachment.native_asset_key));
			const auto original = uam::native_assets::ReadOwned(app.data_root, id, attachment.native_asset_key);
			UAM_ASSERT(original.ok && original.error.empty());
			UAM_ASSERT_EQ(original.bytes, expected_bytes);
			return attachment.native_asset_key;
		}
	};
} // namespace

UAM_TEST(ChatCreateDeliveredEditColdSourceRetainsSelectedSnapshotsAndOriginal)
{
	AwaitBranchFaultOwnerCount(0);
	DeliveredBranchSkillFixture fixture;
	fixture.MakeCold(fixture.source_id);
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	auto payload = fixture.DeliveredPayload("cold-delivered-skill-operation");
	payload["retainedMarkdownStoreFiles"] = std::vector<std::string>{fixture.files[0]};
	const auto callback = fixture.Start(handler, payload);
	const auto id = fixture.AwaitSuccess(callback);
	UAM_ASSERT(!fixture.AssertDeliveredBranch(id, {fixture.files[0]}, {fixture.blocks[0]}).empty());
	UAM_ASSERT(!fixture.Source().messages_loaded && fixture.Source().messages.empty());
	fixture.AssertSource();
	fixture.AssertResources();
	fixture.AssertNoProviderProcess();
}

UAM_TEST(ChatCreateDeliveredEditSkillReplayUsesEffectiveSelectionAndSavedPrefix)
{
	AwaitBranchFaultOwnerCount(0);
	DeliveredBranchSkillFixture fixture;
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	const std::string operation = "skill-effective-all-replay";
	auto all = fixture.DeliveredPayload(operation);
	all["retainedMarkdownStoreFiles"] = std::vector<std::string>{fixture.files[1], fixture.files[0]};
	const auto id = fixture.AwaitSuccess(fixture.Start(handler, all));
	const auto key = fixture.AssertDeliveredBranch(id, fixture.files, fixture.blocks);
	auto* branch = ChatDomainService().FindChatById(fixture.app, id);
	UAM_ASSERT(branch);
	branch->branch_retry_error += " [delivered skill first retry witness]";
	const auto retry_witness = branch->branch_retry_error;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *branch));
	const auto primary_path = AppPaths::UamChatFilePath(fixture.app.data_root, id);
	const auto primary = ReadFile(primary_path);
	const auto generations = fixture.Generations();
	fixture.MakeCold(fixture.source_id);
	fixture.MakeCold(id);
	auto omitted = fixture.DeliveredPayload(operation);
	UAM_ASSERT(!omitted.contains("retainedMarkdownStoreFiles"));
	UAM_ASSERT_EQ(fixture.AwaitSuccess(fixture.Start(handler, omitted)), id);
	UAM_ASSERT_EQ(fixture.AssertDeliveredBranch(id, fixture.files, fixture.blocks), key);
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	UAM_ASSERT_EQ(ReadFile(primary_path), primary);
	const auto replayed = ChatRepository::LoadLocalChat(fixture.app.data_root, id, true);
	UAM_ASSERT(replayed && replayed->branch_retry_error == retry_witness);
	UAM_ASSERT(!fixture.Source().messages_loaded);

	ChatDomainService().SelectChatById(fixture.app, fixture.source_id);
	const auto selection = ChatDomainService().SelectedChatId(fixture.app);
	auto subset = omitted;
	subset["retainedMarkdownStoreFiles"] = std::vector<std::string>{fixture.files[0]};
	fixture.AwaitFailure(fixture.Start(handler, subset), 409);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selection);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	UAM_ASSERT_EQ(ReadFile(primary_path), primary);
	fixture.AssertSource();

	const auto loaded_source = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.source_id, true);
	UAM_ASSERT(loaded_source);
	fixture.Source() = *loaded_source;
	fixture.Source().messages[0].markdown_store_prompt_blocks[0] = "NEW SAVED PREFIX SNAPSHOT\n";
	fixture.SaveSourceWitness();
	fixture.MakeCold(fixture.source_id);
	fixture.AwaitFailure(fixture.Start(handler, omitted), 409);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selection);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	UAM_ASSERT_EQ(ReadFile(primary_path), primary);
	const auto saved_branch = ChatRepository::LoadLocalChat(fixture.app.data_root, id, true);
	UAM_ASSERT(saved_branch && saved_branch->branch_retry_error == retry_witness);
	UAM_ASSERT_EQ(saved_branch->messages[0].markdown_store_prompt_blocks, (std::vector<std::string>{fixture.prefix_block}));
	UAM_ASSERT_EQ(fixture.source_messages[0].markdown_store_prompt_blocks, (std::vector<std::string>{"NEW SAVED PREFIX SNAPSHOT\n"}));
	fixture.AssertSource();
	fixture.AssertResources();
	fixture.AssertNoProviderProcess();
}

UAM_TEST(ChatCreateDeliveredEditSkillSelectionRejectsMalformedAndForeignFiles)
{
	AwaitBranchFaultOwnerCount(0);
	DeliveredBranchSkillFixture fixture;
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	const auto generations = fixture.Generations();
	const auto selection = ChatDomainService().SelectedChatId(fixture.app);
	const std::vector<nlohmann::json> malformed = {nlohmann::json("not-an-array"), nlohmann::json::array({false}), nlohmann::json::array({""}), nlohmann::json::array({std::string(16385, 'x')}), nlohmann::json(std::vector<std::string>(129, fixture.files[0]))};
	for (const auto& invalid : malformed)
	{
		auto payload = fixture.DeliveredPayload("malformed-skill-selection");
		payload["retainedMarkdownStoreFiles"] = invalid;
		fixture.AwaitFailure(fixture.Start(handler, payload), 400);
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selection);
		UAM_ASSERT_EQ(fixture.Generations(), generations);
		fixture.AssertSource();
		fixture.AssertResources();
		fixture.AssertNoProviderProcess();
	}
	const auto foreign_file = fixture.temp.root / "not-delivered.md";
	UAM_ASSERT(uam::io::WriteTextFile(foreign_file, "EXISTING BUT NOT DELIVERED SKILL"));
	for (const auto& invalid : {std::vector<std::string>{fixture.files[0], fixture.files[0]}, std::vector<std::string>{foreign_file.string()}})
	{
		auto payload = fixture.DeliveredPayload("foreign-skill-selection");
		payload["retainedMarkdownStoreFiles"] = invalid;
		fixture.AwaitFailure(fixture.Start(handler, payload), 409);
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selection);
		UAM_ASSERT_EQ(fixture.Generations(), generations);
		fixture.AssertSource();
		fixture.AssertResources();
		fixture.AssertNoProviderProcess();
		UAM_ASSERT_EQ(ReadFile(foreign_file), std::string("EXISTING BUT NOT DELIVERED SKILL"));
	}
	auto valid = fixture.DeliveredPayload("valid-after-skill-rejections");
	valid["retainedMarkdownStoreFiles"] = std::vector<std::string>{fixture.files[0]};
	const auto id = fixture.AwaitSuccess(fixture.Start(handler, valid));
	UAM_ASSERT(!fixture.AssertDeliveredBranch(id, {fixture.files[0]}, {fixture.blocks[0]}).empty());
	fixture.AssertSource();
	fixture.AssertResources();
	fixture.AssertNoProviderProcess();
}

UAM_TEST(ChatCreateDeliveredEditRejectsUnsavedPrefixChangeDuringOriginalPreparation)
{
	AwaitBranchFaultOwnerCount(0);
	DeliveredBranchSkillFixture fixture;
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	const auto generations = fixture.Generations();
	const auto selection = ChatDomainService().SelectedChatId(fixture.app);
	const auto identity = ChatDomainService().CaptureBranchHistoryIdentity(fixture.Source());
	const auto incarnation = fixture.Source().metadata_incarnation;
	HandlerBranchQueueRelease release;
	const auto held = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(release.promise.get_future().share(), held)));
	RunCreationLoopUntil([held]() { return held->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(held->load(), 1);
	auto payload = fixture.DeliveredPayload("stale-skill-prefix-original-operation");
	payload["retainedMarkdownStoreFiles"] = std::vector<std::string>{fixture.files[0]};
	const auto callback = fixture.Start(handler, payload);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	const std::string newer = "NEWER LIVE PREFIX SNAPSHOT THAT MUST SURVIVE\n";
	fixture.Source().messages[0].markdown_store_prompt_blocks[0] = newer;
	UAM_ASSERT(ChatDomainService().MatchesBranchHistoryIdentity(fixture.Source(), identity));
	UAM_ASSERT_EQ(fixture.Source().metadata_incarnation, incarnation);
	fixture.AssertSavedSourceAndOriginal();
	UAM_ASSERT_EQ(held->load(), 1);
	release.Release();
	fixture.AwaitFailure(callback, 409);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selection);
	auto newer_messages = fixture.source_messages;
	newer_messages[0].markdown_store_prompt_blocks[0] = newer;
	UAM_ASSERT_EQ(fixture.Source().messages, newer_messages);
	fixture.AssertSavedSourceAndOriginal();
	const auto generation = AddedBranchFaultGeneration(fixture, generations);
	UAM_ASSERT(fs::is_regular_file(generation / "attempted.receipt"));
	UAM_ASSERT(!fs::exists(generation / "published.receipt"));
	const auto& original = fixture.source_messages[2].attachments.front();
	UAM_ASSERT_EQ(ReadFile(generation / (original.content_digest + ".bin")), fixture.expected_bytes);
	fixture.AssertResources();
	fixture.AssertNoProviderProcess();
	// Restore only after proving final rejection preserved the newer live value.
	fixture.Source().messages[0].markdown_store_prompt_blocks = fixture.source_messages[0].markdown_store_prompt_blocks;
	fixture.AssertSource();
}

UAM_TEST(ChatCreateDeliveredEditKnownOperationRejectsChangedCapturedSkillPrefix)
{
	AwaitBranchFaultOwnerCount(0);
	DeliveredBranchSkillFixture fixture;
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	auto& handler = *handler_owner;
	auto payload = fixture.DeliveredPayload("known-operation-skill-prefix-witness");
	payload["retainedMarkdownStoreFiles"] = fixture.files;
	const auto id = fixture.AwaitSuccess(fixture.Start(handler, payload));
	const auto key = fixture.AssertDeliveredBranch(id, fixture.files, fixture.blocks);
	auto* branch = ChatDomainService().FindChatById(fixture.app, id);
	UAM_ASSERT(branch);
	branch->branch_retry_error += " [known operation retry witness]";
	const auto retry_witness = branch->branch_retry_error;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *branch));
	const auto primary_path = AppPaths::UamChatFilePath(fixture.app.data_root, id);
	const auto primary = ReadFile(primary_path);
	const auto generations = fixture.Generations();
	fixture.MakeCold(id);
	UAM_ASSERT(fixture.Source().messages_loaded);
	ChatDomainService().SelectChatById(fixture.app, fixture.source_id);
	const auto selection = ChatDomainService().SelectedChatId(fixture.app);
	const auto identity = ChatDomainService().CaptureBranchHistoryIdentity(fixture.Source());
	const auto incarnation = fixture.Source().metadata_incarnation;
	fixture.Source().messages[0].markdown_store_prompt_blocks[0] = "CAPTURED UNSAVED SNAPSHOT B\n";
	fixture.AssertSavedSourceAndOriginal();
	HandlerBranchQueueRelease release;
	const auto held = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(release.promise.get_future().share(), held)));
	RunCreationLoopUntil([held]() { return held->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(held->load(), 1);
	const auto callback = fixture.Start(handler, payload);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	UAM_ASSERT_EQ(uam::native_history::detail::refresh_owners.load(), 1U);
	// Work captured B, but fresh domain reuse would now see A and match the
	// existing branch. Only the captured prefix witness can reject this replay.
	fixture.Source().messages[0].markdown_store_prompt_blocks = fixture.source_messages[0].markdown_store_prompt_blocks;
	UAM_ASSERT(ChatDomainService().MatchesBranchHistoryIdentity(fixture.Source(), identity));
	UAM_ASSERT_EQ(fixture.Source().metadata_incarnation, incarnation);
	fixture.AssertSource();
	UAM_ASSERT_EQ(held->load(), 1);
	release.Release();
	fixture.AwaitFailure(callback, 409);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selection);
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	UAM_ASSERT_EQ(ReadFile(primary_path), primary);
	UAM_ASSERT_EQ(fixture.AssertDeliveredBranch(id, fixture.files, fixture.blocks), key);
	const auto saved_branch = ChatRepository::LoadLocalChat(fixture.app.data_root, id, true);
	UAM_ASSERT(saved_branch && saved_branch->branch_retry_error == retry_witness);
	fixture.AssertSource();
	fixture.AssertResources();
	fixture.AssertNoProviderProcess();
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(ChatCreateDeliveredEditColdEmptySkillsRemovesReloadedMissingSnapshots)
{
	AwaitBranchFaultOwnerCount(0);
	DeliveredBranchSkillFixture fixture;
	fixture.Source().messages[2].markdown_store_prompt_blocks.back().clear();
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Source()));
	const auto source_path = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.source_id);
	const auto source_primary = ReadFile(source_path);
	const auto source_revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id);
	const auto reloaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.source_id, true);
	UAM_ASSERT(reloaded && reloaded->messages_loaded && reloaded->messages.size() == 3);
	UAM_ASSERT_EQ(reloaded->messages[2].markdown_store_files, fixture.files);
	UAM_ASSERT_EQ(reloaded->messages[2].markdown_store_prompt_blocks, (std::vector<std::string>{fixture.blocks[0]}));
	UAM_ASSERT_EQ(reloaded->messages[2].attachments, fixture.source_messages[2].attachments);
	UAM_ASSERT_EQ(reloaded->messages[0], fixture.source_messages[0]);
	UAM_ASSERT_EQ(reloaded->messages[1], fixture.source_messages[1]);
	// Adopt the actual two-file/one-block reload without repairing or resaving it.
	fixture.Source() = *reloaded;
	fixture.source_messages = reloaded->messages;
	fixture.source_primary = source_primary;
	fixture.source_revision = source_revision;
	fixture.MakeCold(fixture.source_id);
	fixture.AssertSource();
	fixture.AssertResources();
	UAM_ASSERT(!fixture.Source().messages_loaded && fixture.Source().messages.empty());

	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");

	auto& handler = *handler_owner;
	const std::string operation = "cold-empty-skill-missing-snapshot-replay";
	auto empty = fixture.DeliveredPayload(operation);
	empty["retainedMarkdownStoreFiles"] = nlohmann::json::array();
	const auto id = fixture.AwaitSuccess(fixture.Start(handler, empty));
	const auto key = fixture.AssertDeliveredBranch(id, {}, {});
	UAM_ASSERT(!key.empty());
	UAM_ASSERT(!fixture.Source().messages_loaded && fixture.Source().messages.empty());
	auto* branch = ChatDomainService().FindChatById(fixture.app, id);
	UAM_ASSERT(branch);
	branch->branch_retry_error += " [empty skill first retry witness]";
	const auto retry_witness = branch->branch_retry_error;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *branch));
	const auto primary_path = AppPaths::UamChatFilePath(fixture.app.data_root, id);
	const auto primary = ReadFile(primary_path);
	const auto generations = fixture.Generations();
	fixture.MakeCold(id);
	UAM_ASSERT_EQ(fixture.AwaitSuccess(fixture.Start(handler, empty)), id);
	UAM_ASSERT_EQ(fixture.AssertDeliveredBranch(id, {}, {}), key);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	UAM_ASSERT_EQ(ReadFile(primary_path), primary);
	const auto replayed = ChatRepository::LoadLocalChat(fixture.app.data_root, id, true);
	UAM_ASSERT(replayed && replayed->branch_retry_error == retry_witness);
	UAM_ASSERT(!fixture.Source().messages_loaded && fixture.Source().messages.empty());

	ChatDomainService().SelectChatById(fixture.app, fixture.source_id);
	const auto selection = ChatDomainService().SelectedChatId(fixture.app);
	auto omitted = fixture.DeliveredPayload(operation);
	UAM_ASSERT(!omitted.contains("retainedMarkdownStoreFiles"));
	fixture.AwaitFailure(fixture.Start(handler, omitted), 409);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selection);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	UAM_ASSERT_EQ(ReadFile(primary_path), primary);
	UAM_ASSERT_EQ(fixture.AssertDeliveredBranch(id, {}, {}), key);
	const auto after_conflict = ChatRepository::LoadLocalChat(fixture.app.data_root, id, true);
	UAM_ASSERT(after_conflict && after_conflict->branch_retry_error == retry_witness);
	UAM_ASSERT(!fixture.Source().messages_loaded && fixture.Source().messages.empty());
	UAM_ASSERT_EQ(ReadFile(source_path), source_primary);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id), source_revision);
	fixture.AssertSource();
	fixture.AssertResources();
	fixture.AssertNoProviderProcess();
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	constexpr std::string_view kPassiveGeminiChildPng = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
	struct PassiveGeminiChildMediaFixture
	{
		using Json = nlohmann::json;
		TempDir temp{"uam-passive-gemini-child-media"};
		const fs::path profile = temp.root / "profile";
		ScopedEnvVar home{"GEMINI_CLI_HOME", uam::paths::Utf8PathString(profile)};
		const fs::path storage = profile / ".gemini";
		const fs::path project = storage / "tmp" / "private-project";
		const fs::path chats = project / "chats";
		const std::string workspace = uam::paths::Utf8PathString(temp.root / "workspace");
		const std::string parent_native = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
		const std::string child_native = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
		uam::AppState app;
		ChatSession parent;
		ChatSession unrelated;
		std::string reference, parent_bytes, child_bytes, parent_primary, unrelated_primary, folders_before, original;
		std::string expected_answer;
		bool has_media = false;
		std::vector<std::pair<std::size_t, MessageAttachment>> parts;
		static std::string Line(const Json& value) { return value.dump() + "\n"; }
		fs::path ParentFile() const { return chats / (parent_native + ".jsonl"); }
		fs::path ChildFile() const { return chats / parent_native / (child_native + ".jsonl"); }

		explicit PassiveGeminiChildMediaFixture(bool media)
		{
			AwaitBranchFaultOwnerCount(0);
			UAM_ASSERT(uam::base64::Decode(kPassiveGeminiChildPng, original) && !original.empty());
			fs::create_directories(uam::paths::PathFromUtf8(workspace));
			UAM_ASSERT(uam::io::WriteTextFile(storage / "projects.json", Json{{"projects", {{workspace, "private-project"}}}}.dump()));
			UAM_ASSERT(uam::io::WriteTextFile(project / ".project_root", workspace));
			parent_bytes = Line({{"sessionId", parent_native}, {"projectHash", "private-project"}, {"startTime", "2020-10-18T08:00:00Z"}, {"lastUpdated", "2020-10-18T08:00:02Z"}}) +
			    Line({{"id", "parent-user"}, {"type", "user"}, {"content", "Retain parent question"}}) +
			    Line({{"id", "parent-answer"}, {"type", "gemini"}, {"content", "Actual parent delegation"},
			        {"toolCalls", Json::array({{{"id", "native-delegation"}, {"name", "delegate_to_agent"}, {"status", "success"}, {"agentId", child_native},
			            {"resultDisplay", {{"agentName", "Private child"}, {"state", "completed"}}}, {"result", "Child completed"}}})}});
			UAM_ASSERT(uam::io::WriteTextFile(ParentFile(), parent_bytes));
			app.data_root = temp.root / "data";
			app.native_history_chats_dir = chats;
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			const auto loaded_parent = GeminiJsonHistoryStore::ParseFile(ParentFile(), ProviderProfileStore::DefaultGeminiProfile(), {0, 0, "local"});
			UAM_ASSERT(loaded_parent && loaded_parent->messages.size() == 2 && loaded_parent->messages.back().tool_calls.size() == 1);
			parent = *loaded_parent;
			parent.id = "passive-media-parent"; parent.execution_host_id = "local"; parent.workspace_directory = workspace;
			parent.branch_root_chat_id = parent.id; parent.title = "Retain native parent"; parent.pinned = true;
			parent.folder_id = "passive-media-folder";
			const auto& tool = parent.messages.back().tool_calls.front();
			UAM_ASSERT(tool.is_sub_agent && tool.sub_agent_children_authoritative && tool.sub_agent_children.size() == 1);
			reference = tool.sub_agent_id; // Actual parser output is the public Open request.
			const auto decoded = uam::native_agents::ParseHistoryId(reference);
			UAM_ASSERT(decoded && decoded->provider_id == "gemini-cli" && decoded->parent_session_id == parent_native && decoded->agent_id == child_native && decoded->execution_host_id == "local");
			UAM_ASSERT_EQ(tool.sub_agent_children.front().id, reference);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, parent));
			const auto saved_parent = ChatRepository::LoadLocalChat(app.data_root, parent.id, true);
			UAM_ASSERT(saved_parent && saved_parent->messages == parent.messages);
			parent = *saved_parent;
			unrelated = HandlerChat("passive-media-unrelated"); unrelated.provider_id = "gemini-cli";
			unrelated.native_session_id = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
			unrelated.execution_host_id = "local";
			const auto other_workspace = temp.root / "unrelated-workspace"; fs::create_directories(other_workspace);
			unrelated.workspace_directory = uam::paths::Utf8PathString(other_workspace);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, unrelated));
			app.chats = {unrelated, parent};
			app.resolved_native_sessions_by_chat_id[parent.id] = parent_native;
			ChatFolder folder; folder.id = parent.folder_id; folder.title = "Private native workspace"; folder.directory = workspace; folder.execution_host_id = "local";
			app.folders.push_back(folder); UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
			folders_before = ChatFolderStore::Serialize(app.folders);
			ChatDomainService().SelectChatById(app, parent.id); app.composer_text = "Retain unsent parent draft";
			parent_primary = ReadFile(AppPaths::UamChatFilePath(app.data_root, parent.id));
			unrelated_primary = ReadFile(AppPaths::UamChatFilePath(app.data_root, unrelated.id));
			WriteChild(media, media ? "Original child answer" : "Text-only child answer");
		}

		std::optional<ChatSession> ReadNativeChild() const
		{
			std::string error;
			auto child = uam::native_agents::LoadHistory(parent, reference, std::nullopt, &error);
			if (!child || !error.empty())
				throw std::runtime_error("Private Gemini child LoadHistory prerequisite failed (child=" + std::to_string(child.has_value()) + "): " + error);
			UAM_ASSERT(child && error.empty());
			UAM_ASSERT(child->imported_read_only && child->native_session_id.empty() && child->id == reference);
			UAM_ASSERT_EQ(child->parent_chat_id, parent_native); UAM_ASSERT_EQ(child->workspace_directory, workspace);
			UAM_ASSERT_EQ(child->messages.size(), std::size_t{2});
			return child;
		}

		void WriteChild(bool media, const std::string& answer)
		{
			has_media = media; expected_answer = answer;
			const Json image{{"inlineData", {{"mimeType", "image/png"}, {"data", std::string(kPassiveGeminiChildPng)}}}};
			const Json user = media ? Json::array({{{"text", "Native child question"}}, image}) : Json("Native child question");
			const Json result = media ? Json::array({{{"functionResponse", {{"id", "read-child"}, {"name", "read_file"}, {"response", {{"output", "Exact child tool result"}}}}}}, image}) : Json("Exact child tool result");
			// Parent linkage comes from the attested parent directory. The optional
			// parentSessionId field is absent, matching existing genuine child fixtures.
			child_bytes = Line({{"sessionId", child_native}, {"projectHash", "private-project"}, {"kind", "subagent"}, {"directories", Json::array({workspace})}, {"startTime", "2020-10-18T08:01:00Z"}, {"lastUpdated", "2020-10-18T08:01:02Z"}}) +
			    Line({{"id", "child-user"}, {"type", "user"}, {"timestamp", "2020-10-18T08:01:01Z"}, {"content", user}}) +
			    Line({{"id", "child-answer"}, {"type", "gemini"}, {"timestamp", "2020-10-18T08:01:02Z"}, {"content", answer}, {"thoughts", Json::array({"Exact child reasoning"})},
			        {"toolCalls", Json::array({{{"id", "read-child"}, {"name", "read_file"}, {"status", "success"}, {"result", result}}})}});
			UAM_ASSERT(uam::io::WriteTextFile(ChildFile(), child_bytes));
			const auto native = ReadNativeChild();
			UAM_ASSERT_EQ(native->messages.back().content, expected_answer);
			parts.clear();
			for (std::size_t row = 0; row < native->messages.size(); ++row)
				for (const auto& part : native->messages[row].attachments)
				{
					UAM_ASSERT(part.source == AttachmentSource::NativeInlinePending && part.native_inline_payload && part.path.empty() && part.native_asset_key.empty());
					auto expected = part; expected.native_inline_payload.reset(); parts.emplace_back(row, std::move(expected));
				}
			UAM_ASSERT_EQ(parts.size(), media ? std::size_t{2} : std::size_t{0});
			if (media) UAM_ASSERT(parts.front().second.native_tool_call_id.empty() && parts.back().second.native_tool_call_id == "read-child" && parts.front().second.native_part_key != parts.back().second.native_part_key);
		}

		CefRefPtr<RecordingInstructionCallback> Open(UamQueryHandler& handler, bool refresh = true, const std::string& requested = {}) const
		{
			CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
			ChatIconHandlerTestAccess::OpenNative(handler, {{"chatId", parent.id}, {"nativeSessionId", requested.empty() ? reference : requested}, {"selectChat", false}, {"refreshHistory", refresh}}, callback);
			RunCreationLoopUntil([callback] { return callback->successes + callback->failures != 0; }, std::chrono::seconds(15));
			UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
			if (callback->failures)
				std::cerr << "PASSIVE_GEMINI_OPEN_REFUSAL " << Json{{"refresh", refresh}, {"status", callback->status}, {"body", callback->body}}.dump() << std::endl;
			// A reported success must already own saved/live originals. Waiting for
			// retirement cannot turn a premature queued acknowledgement into success.
			if (callback->successes) (void)AssertSaved(has_media);
			AwaitBranchFaultOwnerCount(0); // Real worker retirement, never a fabricated ACK.
			return callback;
		}

		void AssertParentAndUnrelated() const
		{
			UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), parent.id);
			UAM_ASSERT_EQ(app.composer_text, std::string("Retain unsent parent draft"));
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(app.data_root, parent.id)), parent_primary);
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(app.data_root, unrelated.id)), unrelated_primary);
			UAM_ASSERT_EQ(ReadFile(ParentFile()), parent_bytes); UAM_ASSERT_EQ(ReadFile(ChildFile()), child_bytes);
			UAM_ASSERT_EQ(ReadFile(project / ".project_root"), workspace);
			UAM_ASSERT_EQ(ReadFile(storage / "projects.json"), (Json{{"projects", {{workspace, "private-project"}}}}.dump()));
			UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), folders_before);
			UAM_ASSERT(app.acp_sessions.empty() && app.cli_terminals.empty() && app.pending_chat_save_at_by_chat_id.empty());
			UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.size(), std::size_t{1});
			UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.at(parent.id), parent_native);
			UAM_ASSERT(!app.resolved_native_sessions_by_chat_id.contains(reference));
			const auto* live_parent = ChatDomainService().FindChatById(app, parent.id);
			UAM_ASSERT(live_parent && live_parent->messages == parent.messages && live_parent->pinned && live_parent->title == parent.title);
		}

		std::vector<std::string> AssertSaved(bool media) const
		{
			AssertParentAndUnrelated();
			UAM_ASSERT_EQ(app.chats.size(), std::size_t{3});
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, reference, true);
			const auto* live = ChatDomainService().FindChatById(app, reference);
			UAM_ASSERT(saved && live && saved->messages_loaded && live->messages_loaded && saved->messages == live->messages);
			UAM_ASSERT(saved->imported_read_only && live->imported_read_only && saved->native_session_id.empty() && live->native_session_id.empty());
			UAM_ASSERT_EQ(saved->id, reference); UAM_ASSERT_EQ(saved->provider_id, std::string("gemini-cli"));
			UAM_ASSERT_EQ(saved->parent_chat_id, parent_native); UAM_ASSERT_EQ(saved->workspace_directory, workspace); UAM_ASSERT_EQ(saved->execution_host_id, std::string("local"));
			UAM_ASSERT_EQ(saved->subagent_parent_chat_id, parent.id); UAM_ASSERT_EQ(saved->subagent_parent_created_at, parent.created_at);
			UAM_ASSERT_EQ(live->subagent_parent_chat_id, parent.id); UAM_ASSERT_EQ(live->subagent_parent_created_at, parent.created_at);
			UAM_ASSERT_EQ(saved->messages.size(), std::size_t{2}); UAM_ASSERT_EQ(saved->messages.front().content, std::string("Native child question"));
			UAM_ASSERT_EQ(saved->messages.back().content, expected_answer); UAM_ASSERT_EQ(saved->messages.back().thoughts, std::string("Exact child reasoning"));
			UAM_ASSERT_EQ(saved->messages.back().tool_calls.size(), std::size_t{1});
			UAM_ASSERT_EQ(saved->messages.back().tool_calls.front().id, std::string("read-child")); UAM_ASSERT_EQ(saved->messages.back().tool_calls.front().result_text, std::string("Exact child tool result"));
			std::size_t count = 0; for (const auto& message : saved->messages) count += message.attachments.size();
			UAM_ASSERT_EQ(count, media ? std::size_t{2} : std::size_t{0});
			std::vector<std::string> keys;
			if (!media) return keys;
			UAM_ASSERT(has_media && parts.size() == 2);
			for (const auto& [row, expected] : parts)
			{
				const auto& attachments = saved->messages[row].attachments;
				const auto found = std::ranges::find_if(attachments, [&](const auto& value) { return value.id == expected.id; });
				UAM_ASSERT(found != attachments.end() && found->source == AttachmentSource::OwnedNativeAsset && found->path.empty() && !found->native_inline_payload);
				UAM_ASSERT_EQ(found->name, expected.name); UAM_ASSERT_EQ(found->kind, expected.kind); UAM_ASSERT_EQ(found->native_part_key, expected.native_part_key); UAM_ASSERT_EQ(found->native_tool_call_id, expected.native_tool_call_id);
				UAM_ASSERT_EQ(found->native_source_field, expected.native_source_field); UAM_ASSERT_EQ(found->origin_execution_host_id, expected.origin_execution_host_id); UAM_ASSERT_EQ(found->mime_type, expected.mime_type);
				UAM_ASSERT_EQ(found->content_digest, expected.content_digest); UAM_ASSERT_EQ(found->size_bytes, expected.size_bytes);
				UAM_ASSERT(uam::native_assets::IsKeyForOwner(reference, found->native_asset_key));
				UAM_ASSERT(!uam::native_assets::IsKeyForOwner(parent.id, found->native_asset_key) && !uam::native_assets::IsKeyForOwner(child_native, found->native_asset_key));
				const auto read = uam::native_assets::ReadOwned(app.data_root, reference, found->native_asset_key);
				UAM_ASSERT(read.ok && read.error.empty()); UAM_ASSERT_EQ(read.bytes, original);
				keys.push_back(found->native_asset_key);
			}
			return keys;
		}
	};
}

UAM_TEST(NativeChildLineageHandlerGeminiInlineOriginalsOpenRefreshAndColdReopen)
{
	PassiveGeminiChildMediaFixture fixture(true);
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	const auto initial = fixture.Open(*handler_owner);
	UAM_ASSERT_EQ(initial->failures, 0); UAM_ASSERT_EQ(initial->successes, 1);
	UAM_ASSERT_EQ(nlohmann::json::parse(initial->body).at("chatId"), nlohmann::json(fixture.reference));
	const auto keys = fixture.AssertSaved(true);
	fixture.WriteChild(true, "Fresh native child answer");
	const auto refresh = fixture.Open(*handler_owner);
	UAM_ASSERT_EQ(refresh->failures, 0); UAM_ASSERT_EQ(refresh->successes, 1);
	UAM_ASSERT_EQ(fixture.AssertSaved(true), keys);
	const auto summary = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.reference, false);
	UAM_ASSERT(summary && !summary->messages_loaded);
	*ChatDomainService().FindChatById(fixture.app, fixture.reference) = *summary;
	const auto cold = fixture.Open(*handler_owner, false);
	UAM_ASSERT_EQ(cold->failures, 0); UAM_ASSERT_EQ(cold->successes, 1);
	UAM_ASSERT_EQ(fixture.AssertSaved(true), keys);
}

UAM_TEST(NativeChildLineageHandlerGeminiInlineFailurePreservesTextUntilFreshRetry)
{
	PassiveGeminiChildMediaFixture fixture(false);
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	const auto text = fixture.Open(*handler_owner);
	UAM_ASSERT_EQ(text->failures, 0); UAM_ASSERT_EQ(text->successes, 1);
	(void)fixture.AssertSaved(false); // Genuine actual public text-only control, before the media trigger.
	const auto old_primary = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.reference));
	const auto* old_child = ChatDomainService().FindChatById(fixture.app, fixture.reference);
	UAM_ASSERT(old_child != nullptr);
	const auto old_messages = old_child->messages;
	const auto old_metadata = ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, *old_child);
	fixture.WriteChild(true, "Image update after text control");
	const auto store = fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory);
	UAM_ASSERT(!fs::exists(store)); UAM_ASSERT(uam::io::WriteTextFile(store, "Private child original-store obstruction"));
	const auto failed = fixture.Open(*handler_owner);
	UAM_ASSERT_EQ(failed->successes, 0); UAM_ASSERT_EQ(failed->failures, 1);
	UAM_ASSERT(failed->status == 500 || failed->status == 502);
	UAM_ASSERT(fs::is_regular_file(store)); UAM_ASSERT_EQ(ReadFile(store), std::string("Private child original-store obstruction"));
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.reference)), old_primary);
	const auto* retained = ChatDomainService().FindChatById(fixture.app, fixture.reference);
	UAM_ASSERT(retained && retained->messages == old_messages && retained->imported_read_only && retained->native_session_id.empty());
	UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, *retained), old_metadata);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{3});
	fixture.AssertParentAndUnrelated();
	UAM_ASSERT(fs::remove(store));
	const auto retry = fixture.Open(*handler_owner);
	UAM_ASSERT_EQ(retry->failures, 0); UAM_ASSERT_EQ(retry->successes, 1);
	(void)fixture.AssertSaved(true);
}

UAM_TEST(NativeChildLineageHandlerGeminiForeignInlineReferenceCannotMintTranscript)
{
	for (int foreign = 0; foreign < 3; ++foreign)
	{
		PassiveGeminiChildMediaFixture fixture(false);
		auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		const auto text = fixture.Open(*handler_owner);
		UAM_ASSERT_EQ(text->failures, 0); UAM_ASSERT_EQ(text->successes, 1);
		(void)fixture.AssertSaved(false);
		const auto old_primary = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.reference));
		const auto old_messages = ChatDomainService().FindChatById(fixture.app, fixture.reference)->messages;
		fixture.WriteChild(true, "Correct child media stays unimported during foreign request");
		const auto request = uam::native_agents::BuildHistoryId("gemini-cli", foreign == 0 ? "dddddddd-dddd-4ddd-8ddd-dddddddddddd" : fixture.parent_native,
		    foreign == 2 ? "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee" : fixture.child_native, foreign == 1 ? "foreign-host" : "local");
		UAM_ASSERT(!request.empty() && request != fixture.reference);
		const auto refused = fixture.Open(*handler_owner, true, request);
		UAM_ASSERT_EQ(refused->successes, 0); UAM_ASSERT_EQ(refused->failures, 1);
		UAM_ASSERT_EQ(refused->status, foreign == 2 ? 502 : 400);
		UAM_ASSERT(ChatDomainService().FindChatById(fixture.app, request) == nullptr && !ChatRepository::LoadLocalChat(fixture.app.data_root, request, true));
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{3});
		UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.reference)), old_primary);
		const auto* retained = ChatDomainService().FindChatById(fixture.app, fixture.reference);
		UAM_ASSERT(retained && retained->messages == old_messages && retained->imported_read_only && retained->native_session_id.empty());
		fixture.AssertParentAndUnrelated();
	}
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	void AssertPassiveGeminiCachedColdOpenWithoutNativeFile(bool media)
	{
		PassiveGeminiChildMediaFixture fixture(media);
		auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		const auto initial = fixture.Open(*handler_owner);
		UAM_ASSERT_EQ(initial->failures, 0); UAM_ASSERT_EQ(initial->successes, 1);
		const auto keys = fixture.AssertSaved(media);
		const auto before = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.reference, true);
		const auto cold = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.reference, false);
		UAM_ASSERT(before && before->messages_loaded && cold && !cold->messages_loaded);
		*ChatDomainService().FindChatById(fixture.app, fixture.reference) = *cold;
		// Native history is genuinely unavailable throughout this public non-refresh
		// request. The existing saved primary and scoped originals remain intact.
		const auto parked = fixture.temp.root / "parked-child.jsonl";
		fs::rename(fixture.ChildFile(), parked);
		UAM_ASSERT(!fs::exists(fixture.ChildFile()) && fs::is_regular_file(parked));
		CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
		ChatIconHandlerTestAccess::OpenNative(*handler_owner,
		    {{"chatId", fixture.parent.id}, {"nativeSessionId", fixture.reference}, {"selectChat", false}, {"refreshHistory", false}}, callback);
		RunCreationLoopUntil([callback] { return callback->successes + callback->failures != 0; }, std::chrono::seconds(15));
		AwaitBranchFaultOwnerCount(0);
		UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
		UAM_ASSERT_EQ(callback->failures, 0); UAM_ASSERT_EQ(callback->successes, 1);
		UAM_ASSERT_EQ(nlohmann::json::parse(callback->body).at("chatId"), nlohmann::json(fixture.reference));
		UAM_ASSERT(!fs::exists(fixture.ChildFile()));
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.reference, true);
		const auto* live = ChatDomainService().FindChatById(fixture.app, fixture.reference);
		UAM_ASSERT(saved && live && saved->messages_loaded && live->messages_loaded);
		UAM_ASSERT(saved->messages == before->messages && live->messages == before->messages);
		UAM_ASSERT(saved->imported_read_only && live->imported_read_only && saved->native_session_id.empty() && live->native_session_id.empty());
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty() && !fixture.app.resolved_native_sessions_by_chat_id.contains(fixture.reference));
		for (const auto& key : keys)
		{
			const auto original = uam::native_assets::ReadOwned(fixture.app.data_root, fixture.reference, key);
			UAM_ASSERT(original.ok && original.error.empty()); UAM_ASSERT_EQ(original.bytes, fixture.original);
		}
		// Restore only after the callback, real worker retirement, saved/live check
		// and original-byte check, so shared fixture source-preservation assertions
		// remain meaningful without making a source file available to this request.
		fs::rename(parked, fixture.ChildFile());
		UAM_ASSERT_EQ(fixture.AssertSaved(media), keys);
	}
}

UAM_TEST(NativeChildLineageHandlerGeminiCachedTextColdOpenWithoutNativeFiles)
{
	AssertPassiveGeminiCachedColdOpenWithoutNativeFile(false);
}

UAM_TEST(NativeChildLineageHandlerGeminiCachedOriginalsColdOpenWithoutNativeFiles)
{
	AssertPassiveGeminiCachedColdOpenWithoutNativeFile(true);
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(NativeChildLineageHandlerGeminiRenamedChildRefreshImportsSameCountOriginals)
{
	PassiveGeminiChildMediaFixture fixture(false);
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	const auto initial = fixture.Open(*handler_owner);
	UAM_ASSERT_EQ(initial->failures, 0); UAM_ASSERT_EQ(initial->successes, 1);
	UAM_ASSERT(fixture.AssertSaved(false).empty()); // Actual public text-only child prerequisite.
	auto* child = ChatDomainService().FindChatById(fixture.app, fixture.reference);
	UAM_ASSERT(child && child->imported_read_only && child->native_session_id.empty() && child->messages_loaded);
	const auto before_messages = child->messages;
	const auto before_count = before_messages.size();
	const auto before_birth = child->metadata_incarnation;
	const std::string requested_title = "Renamed child before native media refresh";
	UAM_ASSERT(child->title != requested_title);
	UAM_ASSERT(ChatHistorySyncService().RenameChat(fixture.app, *child, requested_title)); // Genuine UAM rename and durable save.
	const auto renamed = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.reference, true);
	UAM_ASSERT(renamed && renamed->messages == before_messages && child->messages == before_messages);
	UAM_ASSERT_EQ(renamed->title, requested_title); UAM_ASSERT_EQ(child->title, requested_title);
	UAM_ASSERT_EQ(renamed->updated_at, child->updated_at);
	UAM_ASSERT(child->metadata_incarnation == before_birth);
	const auto before_native = fixture.ReadNativeChild();
	UAM_ASSERT(before_native && !before_native->updated_at.empty());
	UAM_ASSERT(renamed->updated_at > before_native->updated_at && "Actual Rename timestamp must follow the private native conversation timestamp.");
	fixture.AssertParentAndUnrelated();

	fixture.WriteChild(true, "Fresh same-count answer after saved child rename");
	const auto changed_native = fixture.ReadNativeChild();
	UAM_ASSERT(changed_native && changed_native->messages_loaded && changed_native->messages.size() == before_count);
	UAM_ASSERT_EQ(before_count, std::size_t{2});
	UAM_ASSERT(changed_native->messages.back().content != before_messages.back().content);
	UAM_ASSERT_EQ(changed_native->messages.back().content, fixture.expected_answer);
	UAM_ASSERT_EQ(fixture.parts.size(), std::size_t{2}); // Actual user/tool PNG parts, not manufactured metadata.
	UAM_ASSERT(renamed->updated_at > changed_native->updated_at); // Prove the equal-count recency boundary before refresh.
	const auto still_renamed = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.reference, true);
	UAM_ASSERT(still_renamed && still_renamed->messages == before_messages);
	UAM_ASSERT_EQ(still_renamed->title, requested_title);

	const auto refreshed = fixture.Open(*handler_owner, true); // One explicit actual native refresh, no supplied snapshot.
	UAM_ASSERT_EQ(refreshed->failures, 0); UAM_ASSERT_EQ(refreshed->successes, 1);
	const auto keys = fixture.AssertSaved(true); // New answer and exact decoded scoped PNG bytes/part associations, saved + live.
	UAM_ASSERT_EQ(keys.size(), std::size_t{2});
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.reference, true);
	const auto* live = ChatDomainService().FindChatById(fixture.app, fixture.reference);
	UAM_ASSERT(saved && live && saved->messages == live->messages);
	UAM_ASSERT_EQ(saved->title, requested_title); UAM_ASSERT_EQ(live->title, requested_title);
	UAM_ASSERT(saved->updated_at >= renamed->updated_at && live->updated_at >= renamed->updated_at);
	UAM_ASSERT(saved->imported_read_only && live->imported_read_only && saved->native_session_id.empty() && live->native_session_id.empty());
	fixture.AssertParentAndUnrelated();
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	CefRefPtr<RecordingInstructionCallback> StartHeldPassiveGeminiOpen(PassiveGeminiChildMediaFixture& fixture, UamQueryHandler& handler)
	{
		CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
		ChatIconHandlerTestAccess::OpenNative(handler,
		    {{"chatId", fixture.parent.id}, {"nativeSessionId", fixture.reference}, {"selectChat", false}, {"refreshHistory", true}}, callback);
		UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
		const auto owner = fixture.app.branch_preparations.find(fixture.parent.id);
		UAM_ASSERT(owner != fixture.app.branch_preparations.end() && !owner->second.expired());
		return callback;
	}

	void AwaitPassiveGeminiOpenRefusal(CefRefPtr<RecordingInstructionCallback> callback)
	{
		RunCreationLoopUntil([callback] { return callback->successes + callback->failures != 0; }, std::chrono::seconds(15));
		UAM_ASSERT_EQ(callback->successes, 0); UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->status, 409);
		AwaitBranchFaultOwnerCount(0);
		UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
	}

	struct PassiveGeminiPendingSnapshot
	{
		std::string parent_primary, parent_summary, child_primary, child_summary;
		ChatSession live_parent, live_child;
		std::vector<std::string> keys;
		explicit PassiveGeminiPendingSnapshot(PassiveGeminiChildMediaFixture& fixture)
		{
			keys = fixture.AssertSaved(true); // Actual public owned-original prerequisite.
			UAM_ASSERT_EQ(keys.size(), std::size_t{2});
			live_parent = *ChatDomainService().FindChatById(fixture.app, fixture.parent.id);
			live_child = *ChatDomainService().FindChatById(fixture.app, fixture.reference);
			CaptureStorage(fixture);
		}
		void CaptureStorage(const PassiveGeminiChildMediaFixture& fixture)
		{
			parent_primary = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.parent.id));
			parent_summary = ReadFile(AppPaths::UamChatSummaryFilePath(fixture.app.data_root, fixture.parent.id));
			child_primary = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.reference));
			child_summary = ReadFile(AppPaths::UamChatSummaryFilePath(fixture.app.data_root, fixture.reference));
			UAM_ASSERT(!parent_primary.empty() && !parent_summary.empty() && !child_primary.empty() && !child_summary.empty());
		}
		void AssertRetained(PassiveGeminiChildMediaFixture& fixture) const
		{
			fixture.AssertParentAndUnrelated();
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.parent.id)), parent_primary);
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatSummaryFilePath(fixture.app.data_root, fixture.parent.id)), parent_summary);
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.reference)), child_primary);
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatSummaryFilePath(fixture.app.data_root, fixture.reference)), child_summary);
			const auto* parent = ChatDomainService().FindChatById(fixture.app, fixture.parent.id);
			const auto* child = ChatDomainService().FindChatById(fixture.app, fixture.reference);
			UAM_ASSERT(parent && child && fixture.app.chats.size() == 3);
			UAM_ASSERT(parent->messages == live_parent.messages && parent->metadata_incarnation == live_parent.metadata_incarnation);
			UAM_ASSERT(child->messages == live_child.messages && child->metadata_incarnation == live_child.metadata_incarnation);
			UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, *child), ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, live_child));
			UAM_ASSERT(child->imported_read_only && child->native_session_id.empty());
			const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.reference, true);
			UAM_ASSERT(saved && saved->imported_read_only && saved->native_session_id.empty());
			std::vector<std::string> saved_keys;
			for (const auto& message : saved->messages) for (const auto& attachment : message.attachments)
			{
				UAM_ASSERT(attachment.source == AttachmentSource::OwnedNativeAsset && !attachment.native_inline_payload && attachment.path.empty());
				saved_keys.push_back(attachment.native_asset_key);
			}
			UAM_ASSERT_EQ(saved_keys, keys);
			for (const auto& key : keys)
			{
				const auto original = uam::native_assets::ReadOwned(fixture.app.data_root, fixture.reference, key);
				UAM_ASSERT(original.ok && original.error.empty()); UAM_ASSERT_EQ(original.bytes, fixture.original);
			}
			UAM_ASSERT(fixture.app.branch_preparations.empty());
		}
	};

	void AssertPassiveGeminiPendingStorageDrift(bool target, bool summary_only)
	{
		PassiveGeminiChildMediaFixture fixture(true);
		auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		const auto initial = fixture.Open(*handler_owner);
		UAM_ASSERT_EQ(initial->failures, 0); UAM_ASSERT_EQ(initial->successes, 1);
		PassiveGeminiPendingSnapshot retained(fixture);
		fixture.WriteChild(true, "Pending native media refresh must not replace newer saved intent");
		HandlerBranchQueueRelease release;
		const auto held = std::make_shared<std::atomic<int>>(0);
		UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(release.promise.get_future().share(), held)));
		RunCreationLoopUntil([held] { return held->load() == 1; }, std::chrono::seconds(3));
		UAM_ASSERT_EQ(held->load(), 1);
		const auto callback = StartHeldPassiveGeminiOpen(fixture, *handler_owner);
		const auto id = target ? fixture.reference : fixture.parent.id;
		const auto primary_path = AppPaths::UamChatFilePath(fixture.app.data_root, id);
		const auto summary_path = AppPaths::UamChatSummaryFilePath(fixture.app.data_root, id);
		const auto old_primary = ReadFile(primary_path), old_summary = ReadFile(summary_path);
		const auto old_primary_revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, id);
		const auto old_summary_revision = ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, id);
		auto newer = ChatRepository::LoadLocalChat(fixture.app.data_root, id, true);
		UAM_ASSERT(newer && newer->messages.size() == 2);
		if (summary_only)
		{
			newer->last_opened_at = "2026-12-20T12:34:56Z";
			UAM_ASSERT(ChatRepository::SaveLastOpenedAt(fixture.app.data_root, *newer));
			UAM_ASSERT_EQ(ReadFile(primary_path), old_primary);
			UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, id), old_primary_revision);
			UAM_ASSERT(ReadFile(summary_path) != old_summary);
			UAM_ASSERT(ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, id) != old_summary_revision);
		}
		else
		{
			newer->messages.front().content += " Newer authored saved intent while native Open waits.";
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *newer));
			UAM_ASSERT(ReadFile(primary_path) != old_primary);
			UAM_ASSERT(ChatRepository::ChatStorageRevision(fixture.app.data_root, id) != old_primary_revision);
		}
		// Repository-only edits are genuine authority drift; do not change the live
		// ownership witness to make a stale operation look current.
		retained.CaptureStorage(fixture);
		fixture.parent_primary = retained.parent_primary; // Deliberately newer source bytes must be preserved.
		UAM_ASSERT_EQ(held->load(), 1); UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
		release.Release();
		AwaitPassiveGeminiOpenRefusal(callback);
		retained.AssertRetained(fixture);
		const auto saved_newer = ChatRepository::LoadLocalChat(fixture.app.data_root, id, true);
		UAM_ASSERT(saved_newer && saved_newer->messages == newer->messages);
		if (summary_only) UAM_ASSERT_EQ(saved_newer->last_opened_at, newer->last_opened_at);
	}
}

UAM_TEST(NativeChildLineageHandlerGeminiPendingSourcePrimaryAndSummaryChangesRefuse)
{
	AssertPassiveGeminiPendingStorageDrift(false, false);
	AssertPassiveGeminiPendingStorageDrift(false, true);
}

UAM_TEST(NativeChildLineageHandlerGeminiPendingTargetPrimaryAndSummaryChangesRefuse)
{
	AssertPassiveGeminiPendingStorageDrift(true, false);
	AssertPassiveGeminiPendingStorageDrift(true, true);
}

UAM_TEST(NativeChildLineageHandlerGeminiPendingRefreshSupersededByExactPublicOpen)
{
	PassiveGeminiChildMediaFixture fixture(true);
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	const auto initial = fixture.Open(*handler_owner);
	UAM_ASSERT_EQ(initial->failures, 0); UAM_ASSERT_EQ(initial->successes, 1);
	const auto old_keys = fixture.AssertSaved(true);
	fixture.WriteChild(true, "Only the replacement public Open may publish this actual native answer");
	HandlerBranchQueueRelease release;
	const auto held = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(release.promise.get_future().share(), held)));
	RunCreationLoopUntil([held] { return held->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(held->load(), 1);
	const auto first = StartHeldPassiveGeminiOpen(fixture, *handler_owner);
	const auto replacement = StartHeldPassiveGeminiOpen(fixture, *handler_owner);
	UAM_ASSERT_EQ(held->load(), 1);
	release.Release();
	RunCreationLoopUntil([first, replacement] { return first->successes + first->failures != 0 && replacement->successes + replacement->failures != 0; }, std::chrono::seconds(15));
	UAM_ASSERT_EQ(first->successes, 0); UAM_ASSERT_EQ(first->failures, 1); UAM_ASSERT_EQ(first->status, 409);
	UAM_ASSERT_EQ(replacement->successes, 1); UAM_ASSERT_EQ(replacement->failures, 0);
	UAM_ASSERT_EQ(nlohmann::json::parse(replacement->body).at("chatId"), nlohmann::json(fixture.reference));
	(void)fixture.AssertSaved(true); // Callback success already means actual saved/live bytes, never queue admission.
	AwaitBranchFaultOwnerCount(0);
	UAM_ASSERT_EQ(first->successes + first->failures, 1); UAM_ASSERT_EQ(replacement->successes + replacement->failures, 1);
	UAM_ASSERT(fixture.app.branch_preparations.empty());
	for (const auto& key : old_keys)
	{
		const auto original = uam::native_assets::ReadOwned(fixture.app.data_root, fixture.reference, key);
		UAM_ASSERT(original.ok); UAM_ASSERT_EQ(original.bytes, fixture.original);
	}
}

UAM_TEST(NativeChildLineageHandlerGeminiPendingRefreshDomainCancellationPreservesOriginals)
{
	PassiveGeminiChildMediaFixture fixture(true);
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	const auto initial = fixture.Open(*handler_owner);
	UAM_ASSERT_EQ(initial->failures, 0); UAM_ASSERT_EQ(initial->successes, 1);
	PassiveGeminiPendingSnapshot retained(fixture);
	fixture.WriteChild(true, "Cancelled native answer requires one genuine later retry");
	HandlerBranchQueueRelease release;
	const auto held = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(release.promise.get_future().share(), held)));
	RunCreationLoopUntil([held] { return held->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(held->load(), 1);
	const auto callback = StartHeldPassiveGeminiOpen(fixture, *handler_owner);
	ChatDomainService().CancelBranchPreparations(fixture.app, fixture.reference);
	UAM_ASSERT(fixture.app.branch_preparations.empty());
	UAM_ASSERT_EQ(held->load(), 1); UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	release.Release();
	AwaitPassiveGeminiOpenRefusal(callback);
	retained.AssertRetained(fixture);
	const auto retry = fixture.Open(*handler_owner);
	UAM_ASSERT_EQ(retry->successes, 1); UAM_ASSERT_EQ(retry->failures, 0);
	(void)fixture.AssertSaved(true);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(NativeChildLineageHandlerGeminiAckUncertaintyNotifiesWithoutRollingBackCommit)
{
	PassiveGeminiChildMediaFixture fixture(false);
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	fixture.app.status_line = "Earlier unrelated application status";
	const auto text = fixture.Open(*handler_owner);
	UAM_ASSERT_EQ(text->successes, 1); UAM_ASSERT_EQ(text->failures, 0);
	UAM_ASSERT(!nlohmann::json::parse(text->body).contains("warning"));
	UAM_ASSERT_EQ(fixture.app.status_line, std::string("Earlier unrelated application status"));
	UAM_ASSERT(fixture.AssertSaved(false).empty());
	const auto target_path = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.reference);
	const auto target_summary = AppPaths::UamChatSummaryFilePath(fixture.app.data_root, fixture.reference);
	const auto old_primary = ReadFile(target_path);
	const auto store = fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory);
	UAM_ASSERT(!fs::exists(store)); // The next actual PNG refresh must prepare a NEW batch.
	fixture.WriteChild(true, "Committed child originals survive an unconfirmed final save");
	HandlerBranchQueueRelease load_release, preparation_release, acknowledgement_release;
	const auto load_held = std::make_shared<std::atomic<int>>(0);
	const auto preparation_held = std::make_shared<std::atomic<int>>(0);
	const auto acknowledgement_held = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(load_release.promise.get_future().share(), load_held)));
	RunCreationLoopUntil([load_held] { return load_held->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(load_held->load(), 1);
	const auto callback = StartHeldPassiveGeminiOpen(fixture, *handler_owner);
	// Queue A -> actual load -> B. A real UI fence after B enters is after the
	// actual load completion, which must queue preparation behind B.
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(preparation_release.promise.get_future().share(), preparation_held)));
	load_release.Release();
	RunCreationLoopUntil([preparation_held] { return preparation_held->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(preparation_held->load(), 1);
	const auto loaded_owner = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_UI, new CreationQueueWitness(loaded_owner, 1)));
	RunCreationLoopUntil([loaded_owner] { return loaded_owner->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(loaded_owner->load(), 1);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	UAM_ASSERT_EQ(ReadFile(target_path), old_primary);
	// Queue B -> actual prepare -> C; the real commit/ACK handoff must then
	// happen on UI while C prevents the ACK worker from running.
	UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(acknowledgement_release.promise.get_future().share(), acknowledgement_held)));
	preparation_release.Release();
	RunCreationLoopUntil([acknowledgement_held] { return acknowledgement_held->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(acknowledgement_held->load(), 1);
	const auto committed_owner = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(CefPostTask(TID_UI, new CreationQueueWitness(committed_owner, 1)));
	RunCreationLoopUntil([committed_owner] { return committed_owner->load() == 1; }, std::chrono::seconds(3));
	UAM_ASSERT_EQ(committed_owner->load(), 1);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	const auto keys = fixture.AssertSaved(true); // Exact saved/live PNG bytes and associations BEFORE callback/ACK.
	UAM_ASSERT_EQ(keys.size(), std::size_t{2});
	const auto committed_primary = ReadFile(target_path), committed_summary = ReadFile(target_summary);
	UAM_ASSERT(committed_primary != old_primary);
	std::vector<fs::path> generations;
	for (const auto& owner : fs::directory_iterator(store))
		if (owner.is_directory()) for (const auto& generation : fs::directory_iterator(owner.path()))
			if (generation.is_directory()) generations.push_back(generation.path());
	UAM_ASSERT_EQ(generations.size(), std::size_t{1});
	const auto generation = generations.front();
	UAM_ASSERT(fs::is_regular_file(generation / "attempted.receipt"));
	const auto obstruction = generation / "published.receipt";
	UAM_ASSERT(!fs::exists(obstruction));
	fs::create_directories(obstruction);
	UAM_ASSERT(uam::io::WriteTextFile(obstruction / "keep", "Private actual passive acknowledgement obstruction"));
	UAM_ASSERT_EQ(acknowledgement_held->load(), 1);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	UAM_ASSERT_EQ(fixture.app.status_line, std::string("Earlier unrelated application status"));
	acknowledgement_release.Release();
	RunCreationLoopUntil([callback] { return callback->successes + callback->failures != 0; }, std::chrono::seconds(15));
	UAM_ASSERT_EQ(callback->successes, 1); UAM_ASSERT_EQ(callback->failures, 0);
	const auto body = nlohmann::json::parse(callback->body);
	UAM_ASSERT_EQ(body.at("chatId"), nlohmann::json(fixture.reference));
	UAM_ASSERT(body.contains("warning") && !body.at("warning").get<std::string>().empty()); // Real uncertain branch, not fabricated success.
	const std::string expected_warning = "History and attached files were kept, but UAM could not confirm the final save. Reopen the child to check it.";
	UAM_ASSERT_EQ(fixture.app.status_line, expected_warning); // Intended old-source RED: callback warning was otherwise invisible.
	UAM_ASSERT_EQ(body.at("warning"), nlohmann::json(expected_warning));
	UAM_ASSERT_EQ(uam::StateSerializer::Serialize(fixture.app, true).at("statusLine"), nlohmann::json(expected_warning));
	UAM_ASSERT_EQ(ReadFile(target_path), committed_primary); UAM_ASSERT_EQ(ReadFile(target_summary), committed_summary);
	UAM_ASSERT_EQ(fixture.AssertSaved(true), keys);
	AwaitBranchFaultOwnerCount(0);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
	UAM_ASSERT_EQ(ReadFile(target_path), committed_primary); UAM_ASSERT_EQ(ReadFile(target_summary), committed_summary);
	UAM_ASSERT_EQ(fixture.AssertSaved(true), keys);
	UAM_ASSERT(fixture.app.branch_preparations.empty());
	UAM_ASSERT(fs::is_directory(obstruction));
	UAM_ASSERT_EQ(ReadFile(obstruction / "keep"), std::string("Private actual passive acknowledgement obstruction"));
	UAM_ASSERT(fs::is_regular_file(generation / "attempted.receipt"));
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(NativeChildLineageHandlerGeminiShortReadableRefreshPreservesSavedOriginals)
{
	PassiveGeminiChildMediaFixture fixture(true);
	auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
	const auto initial = fixture.Open(*handler_owner);
	UAM_ASSERT_EQ(initial->successes, 1); UAM_ASSERT_EQ(initial->failures, 0);
	const auto keys = fixture.AssertSaved(true); // Actual complete two-message import and acknowledged originals first.
	UAM_ASSERT_EQ(keys.size(), std::size_t{2});
	const auto primary_path = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.reference);
	const auto summary_path = AppPaths::UamChatSummaryFilePath(fixture.app.data_root, fixture.reference);
	const auto primary = ReadFile(primary_path), summary = ReadFile(summary_path);
	const auto primary_revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.reference);
	const auto summary_revision = ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, fixture.reference);
	const auto* before = ChatDomainService().FindChatById(fixture.app, fixture.reference);
	UAM_ASSERT(before && before->messages_loaded && before->messages.size() == 2);
	const auto metadata = ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, *before);
	const auto messages = before->messages;

	// Physically retain the genuine header and genuine first user record only.
	// Their exact native IDs/project/workspace/parent and image payload stay intact.
	const auto complete_native_bytes = fixture.child_bytes;
	const auto header_end = fixture.child_bytes.find('\n');
	UAM_ASSERT(header_end != std::string::npos);
	const auto user_end = fixture.child_bytes.find('\n', header_end + 1);
	UAM_ASSERT(user_end != std::string::npos && user_end + 1 < fixture.child_bytes.size());
	fixture.child_bytes.resize(user_end + 1);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.ChildFile(), fixture.child_bytes));
	std::string error;
	const auto shortened = uam::native_agents::LoadHistory(fixture.parent, fixture.reference, std::nullopt, &error);
	UAM_ASSERT(shortened && error.empty() && shortened->messages_loaded);
	UAM_ASSERT_EQ(shortened->messages.size(), std::size_t{1});
	UAM_ASSERT_EQ(shortened->messages.front().role, MessageRole::User);
	UAM_ASSERT_EQ(shortened->messages.front().content, std::string("Native child question"));
	UAM_ASSERT_EQ(shortened->messages.front().attachments.size(), std::size_t{1});
	UAM_ASSERT(shortened->messages.front().attachments.front().source == AttachmentSource::NativeInlinePending);
	UAM_ASSERT_EQ(shortened->id, fixture.reference); UAM_ASSERT_EQ(shortened->provider_id, std::string("gemini-cli"));
	UAM_ASSERT_EQ(shortened->parent_chat_id, fixture.parent_native);
	UAM_ASSERT_EQ(shortened->workspace_directory, fixture.workspace);
	UAM_ASSERT(shortened->execution_host_id == "local" && shortened->imported_read_only && shortened->native_session_id.empty());

	// Call the public handler directly: the shared success helper deliberately
	// checks full content and would mask this case's first refusal oracle.
	CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
	ChatIconHandlerTestAccess::OpenNative(*handler_owner,
	    {{"chatId", fixture.parent.id}, {"nativeSessionId", fixture.reference}, {"selectChat", false}, {"refreshHistory", true}}, callback);
	RunCreationLoopUntil([callback] { return callback->successes + callback->failures != 0; }, std::chrono::seconds(15));
	UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
	UAM_ASSERT_EQ(callback->successes, 0); // Intended actual R9 RED after all genuine reader/import prerequisites.
	UAM_ASSERT_EQ(callback->failures, 1);
	const std::string expected_refusal = "Native child history is shorter than the saved conversation. Saved history was retained for reading. Refresh can succeed when this child's complete native history is available; the same shorter source cannot replace it.";
	UAM_ASSERT_EQ(callback->body, expected_refusal);
	UAM_ASSERT_EQ(ReadFile(primary_path), primary); UAM_ASSERT_EQ(ReadFile(summary_path), summary);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.reference), primary_revision);
	UAM_ASSERT_EQ(ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, fixture.reference), summary_revision);
	const auto* retained = ChatDomainService().FindChatById(fixture.app, fixture.reference);
	UAM_ASSERT(retained && retained->messages_loaded);
	UAM_ASSERT_EQ(retained->messages, messages);
	UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, *retained), metadata);
	UAM_ASSERT_EQ(fixture.AssertSaved(true), keys); // Saved/live exact original bytes, associations, drafts and source isolation.
	AwaitBranchFaultOwnerCount(0);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
	UAM_ASSERT(fixture.app.branch_preparations.empty());
	UAM_ASSERT_EQ(ReadFile(primary_path), primary); UAM_ASSERT_EQ(ReadFile(summary_path), summary);
	UAM_ASSERT_EQ(fixture.AssertSaved(true), keys);

	// Repeating refresh cannot repair the source or discard saved originals.
	const auto repeated = fixture.Open(*handler_owner, true);
	UAM_ASSERT_EQ(repeated->successes, 0); UAM_ASSERT_EQ(repeated->failures, 1);
	UAM_ASSERT_EQ(repeated->body, expected_refusal);
	UAM_ASSERT_EQ(ReadFile(primary_path), primary); UAM_ASSERT_EQ(ReadFile(summary_path), summary);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.reference), primary_revision);
	UAM_ASSERT_EQ(ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, fixture.reference), summary_revision);
	UAM_ASSERT_EQ(fixture.AssertSaved(true), keys);

	// A normal reopen still reads saved history while the genuine source remains
	// short. The ordinary reopen may save metadata; refusal checks above compare
	// the original bytes and revisions before that separate successful operation.
	const auto reopened = fixture.Open(*handler_owner, false);
	UAM_ASSERT_EQ(reopened->successes, 1); UAM_ASSERT_EQ(reopened->failures, 0);
	UAM_ASSERT_EQ(fixture.AssertSaved(true), keys);

	// Restore the exact original journal, then issue a NEW public refresh. A
	// shorter-source refusal must not poison a valid equal-count later request.
	fixture.child_bytes = complete_native_bytes;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.ChildFile(), fixture.child_bytes));
	UAM_ASSERT_EQ(ReadFile(fixture.ChildFile()), complete_native_bytes);
	const auto restored = fixture.ReadNativeChild();
	UAM_ASSERT(restored && restored->messages_loaded && restored->messages.size() == 2);
	const auto retried = fixture.Open(*handler_owner, true);
	UAM_ASSERT_EQ(retried->successes, 1); UAM_ASSERT_EQ(retried->failures, 0);
	UAM_ASSERT_EQ(fixture.AssertSaved(true), keys);
	UAM_ASSERT_EQ(callback->successes, 0); UAM_ASSERT_EQ(callback->failures, 1);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
	UAM_ASSERT_EQ(retried->successes + retried->failures, 1);
	UAM_ASSERT(fixture.app.branch_preparations.empty());
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_SSH && defined(__APPLE__)
namespace
{
	// This future observes only a genuine bridge callback; no test code can
	// manufacture worker/transport success or treat a launch marker as completion.
	class RemoteHistoryQueryCallback final : public UamQueryHandler::Callback
	{
		std::promise<void> m_completed;
		void Finish()
		{
			if (successes + failures == 1) m_completed.set_value();
		}
	public:
		std::shared_future<void> completion = m_completed.get_future().share();
		int successes = 0, failures = 0, status = 0;
		std::string body;
		void Success(const CefString& response) override { ++successes; body = response.ToString(); Finish(); }
		void Success(const void* value, size_t size) override { ++successes; body.assign(static_cast<const char*>(value), size); Finish(); }
		void Failure(int code, const CefString& message) override { ++failures; status = code; body = message.ToString(); Finish(); }
		bool Completed() const { return completion.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
	private:
		IMPLEMENT_REFCOUNTING(RemoteHistoryQueryCallback);
	};

	struct RemoteGeminiHistoryHandlerFixture
	{
		using Json = nlohmann::json;
		TempDir temp{"uam-remote-gemini-cef-originals"};
		const fs::path private_home = temp.root / "home";
		const fs::path runner = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
		const fs::path marker = temp.root / "ssh-launches";
		const fs::path arguments = temp.root / "ssh-arguments";
		ScopedEnvVar home{"HOME", uam::paths::Utf8PathString(private_home)};
		ScopedEnvVar userprofile{"USERPROFILE", uam::paths::Utf8PathString(private_home)};
		ScopedEnvVar gemini{"GEMINI_CLI_HOME", uam::paths::Utf8PathString(private_home)};
		ScopedEnvVar data{"UAM_DATA_DIR", uam::paths::Utf8PathString(temp.root / "controller")};
		ScopedEnvVar path{"PATH", uam::paths::Utf8PathString(temp.root / "bin") + ":" + uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin")};
		ScopedEnvVar runner_env{"UAM_TEST_RUNNER", uam::paths::Utf8PathString(runner)};
		ScopedEnvVar marker_env{"UAM_PRIVATE_HISTORY_SSH_MARKER", uam::paths::Utf8PathString(marker)};
		ScopedEnvVar arguments_env{"UAM_PRIVATE_HISTORY_SSH_ARGUMENTS", uam::paths::Utf8PathString(arguments)};
		const std::string native_id = "77777777-7777-4777-8777-777777777777";
		const std::string owner_id = "remote-gemini-cef-saved-owner";
		const std::string unrelated_id = "remote-gemini-cef-desktop-owner";
		const std::string workspace = uam::paths::Utf8PathString(temp.root / "native-workspace");
		const std::string png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
		uam::AppState app;
		ExecutionHost host;
		std::unique_ptr<UamQueryHandler> handler;
		std::vector<CefRefPtr<RemoteHistoryQueryCallback>> pending_callbacks;
		std::string native_bytes, original, unrelated_primary, unrelated_summary, folders;
		std::vector<Message> unrelated_messages;
		std::string unrelated_metadata;

		fs::path NativeFile() const
		{
			return private_home / ".gemini" / "tmp" / "project" / "chats" /
			    ("session-2020-10-18T08-00-" + native_id.substr(0, 8) + ".json");
		}
		ChatSession& Current() { auto* chat = ChatDomainService().FindChatById(app, owner_id); UAM_ASSERT(chat); return *chat; }
		const ChatSession& Current() const { const auto* chat = ChatDomainService().FindChatById(app, owner_id); UAM_ASSERT(chat); return *chat; }
		fs::path Primary() const { return AppPaths::UamChatFilePath(app.data_root, owner_id); }
		fs::path Summary() const { return AppPaths::UamChatSummaryFilePath(app.data_root, owner_id); }
		std::size_t Launches() const { return fs::exists(marker) ? CountSubstring(ReadFile(marker), "launch\n") : 0; }

		RemoteGeminiHistoryHandlerFixture()
		{
			UAM_ASSERT(fs::is_regular_file(runner));
			UAM_ASSERT(uam::base64::Decode(png, original) && !original.empty());
			fs::create_directories(private_home); fs::create_directories(uam::paths::PathFromUtf8(workspace));
			const auto ssh = temp.root / "bin" / "ssh";
			// The generated remote command is recorded, never evaluated or executed.
			UAM_ASSERT(uam::io::WriteTextFile(ssh,
			    "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$UAM_PRIVATE_HISTORY_SSH_ARGUMENTS\" || exit 91\n"
			    "printf 'launch\\n' >> \"$UAM_PRIVATE_HISTORY_SSH_MARKER\" || exit 92\n"
			    "exec \"$UAM_TEST_RUNNER\" bridge-direct\n"));
			fs::permissions(ssh, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
			host.id = "private-history-host"; host.transport = "ssh"; host.ssh_alias = "private-history-fixture";
			host.platform = uam::remote::native_history::Platform(); host.runner_status = "ready";
			host.runner_version = std::string(uam::constants::kAppVersion).substr(1);
			host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
			UAM_ASSERT(uam::remote::CanAttemptRunnerConnection(host));
			app.data_root = temp.root / "controller"; app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			app.settings.execution_hosts.push_back(host);
			UAM_ASSERT(uam::io::WriteTextFile(NativeFile().parent_path().parent_path() / ".project_root", workspace));
			ChatSession owner;
			owner.id = owner_id; owner.provider_id = "gemini-cli"; owner.native_session_id = native_id;
			owner.execution_host_id = host.id; owner.workspace_directory = workspace;
			owner.branch_root_chat_id = owner.id; owner.folder_id = "remote-history-folder";
			owner.title = "Retain remote saved title"; owner.pinned = true;
			owner.created_at = "2020-10-18T07:00:00Z"; owner.updated_at = owner.created_at;
			owner.messages.push_back(Message{MessageRole::User, "Retained earlier question"});
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, owner));
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, owner.id, true);
			UAM_ASSERT(saved && saved->messages_loaded); app.chats.push_back(*saved);
			ChatSession unrelated = HandlerChat(unrelated_id); unrelated.title = "Retain selected desktop chat";
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, unrelated)); app.chats.push_back(unrelated);
			ChatFolder folder; folder.id = owner.folder_id; folder.title = "Private remote project";
			folder.directory = workspace; folder.execution_host_id = host.id;
			app.folders.push_back(folder); UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
			folders = ChatFolderStore::Serialize(app.folders);
			ChatDomainService().SelectChatById(app, unrelated_id); app.composer_text = "Retain selected desktop draft";
			unrelated_primary = ReadFile(AppPaths::UamChatFilePath(app.data_root, unrelated_id));
			unrelated_summary = ReadFile(AppPaths::UamChatSummaryFilePath(app.data_root, unrelated_id));
			unrelated_messages = app.chats.back().messages;
			unrelated_metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, app.chats.back());
			WriteNative(false);
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
		}

		bool AllQueriesCompleted() const
		{
			return std::ranges::all_of(pending_callbacks, [](const auto& callback) { return callback->Completed(); });
		}

		~RemoteGeminiHistoryHandlerFixture() noexcept
		{
			// Body runs BEFORE handler/app/env/temp member destruction. In particular,
			// keep the handler lifetime valid so real posted UI callbacks can settle.
			try
			{
				RunCreationLoopUntil([this] { return AllQueriesCompleted(); }, std::chrono::seconds(90));
				if (!AllQueriesCompleted())
				{
					std::cerr << "FATAL_PRIVATE_REMOTE_QUERY_DRAIN_TIMEOUT: actual callback did not settle; private environment and files will not be unwound." << std::endl;
					std::abort();
				}
				handler.reset();
			}
			catch (...)
			{
				std::cerr << "FATAL_PRIVATE_REMOTE_QUERY_DRAIN_ERROR: private environment and files will not be unwound." << std::endl;
				std::abort();
			}
		}

		void WriteNative(bool media)
		{
			const Json image{{"inlineData", {{"mimeType", "image/png"}, {"data", png}}}};
			const Json content = media ? Json::array({{{"text", "Remote native question"}}, image}) : Json("Remote native question");
			const Json result = media ? Json::array({{{"functionResponse", {{"id", "remote-read"}, {"name", "read_file"}, {"response", {{"output", "Remote native tool result"}}}}}}, image}) : Json("Remote native tool result");
			native_bytes = Json{{"sessionId", native_id}, {"projectHash", "project"},
			    {"startTime", "2020-10-18T08:00:00Z"}, {"lastUpdated", "2020-10-18T08:00:02Z"},
			    {"messages", Json::array({
			        {{"id", "remote-user"}, {"type", "user"}, {"timestamp", "2020-10-18T08:00:01Z"}, {"content", content}},
			        {{"id", "remote-answer"}, {"type", "gemini"}, {"timestamp", "2020-10-18T08:00:02Z"},
			            {"content", media ? "Remote answer with inline originals" : "Remote text-only answer"},
			            {"toolCalls", Json::array({{{"id", "remote-read"}, {"name", "read_file"}, {"status", "success"}, {"result", result}}})}}
			    })}}.dump();
			UAM_ASSERT(uam::io::WriteTextFile(NativeFile(), native_bytes));
		}

		void AssertPrivateTransport(std::size_t before) const
		{
			UAM_ASSERT(Launches() > before);
			const auto expected = uam::remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version);
			UAM_ASSERT(!expected.empty() && expected.front() == "ssh");
			std::string lines; for (std::size_t index = 1; index < expected.size(); ++index) lines += expected[index] + "\n";
			UAM_ASSERT_EQ(ReadFile(arguments), lines);
		}

		ChatSession ReadGenuineExpected(bool media)
		{
			const auto before = Launches(); std::string error;
			auto loaded = ChatHistorySyncService().LoadRemoteNativeTranscript(host, Current(), &error);
			if (!loaded || !error.empty()) throw std::runtime_error("Private actual remote reader prerequisite failed: " + error);
			AssertPrivateTransport(before);
			UAM_ASSERT_EQ(loaded->native_session_id, native_id); UAM_ASSERT_EQ(loaded->provider_id, std::string("gemini-cli"));
			UAM_ASSERT_EQ(loaded->workspace_directory, workspace); UAM_ASSERT_EQ(loaded->execution_host_id, host.id);
			UAM_ASSERT_EQ(loaded->messages.size(), std::size_t{2});
			UAM_ASSERT_EQ(loaded->messages.front().attachments.size(), media ? std::size_t{1} : std::size_t{0});
			UAM_ASSERT_EQ(loaded->messages.back().attachments.size(), media ? std::size_t{1} : std::size_t{0});
			if (media)
			{
				const auto& user = loaded->messages.front().attachments.front();
				const auto& tool = loaded->messages.back().attachments.front();
				for (const auto* part : {&user, &tool})
				{
					UAM_ASSERT(part->source == AttachmentSource::NativeInlinePending && part->native_inline_payload);
					UAM_ASSERT(part->path.empty() && part->native_asset_key.empty() && part->source_uri.empty());
					UAM_ASSERT_EQ(part->origin_execution_host_id, host.id); UAM_ASSERT_EQ(part->mime_type, std::string("image/png"));
					UAM_ASSERT_EQ(part->size_bytes, original.size()); UAM_ASSERT(!part->content_digest.empty());
				}
				UAM_ASSERT(user.native_tool_call_id.empty()); UAM_ASSERT_EQ(tool.native_tool_call_id, std::string("remote-read"));
				UAM_ASSERT(user.native_part_key != tool.native_part_key);
			}
			return std::move(*loaded);
		}

		CefRefPtr<RemoteHistoryQueryCallback> Refresh()
		{
			const auto before = Launches();
			CefRefPtr<RemoteHistoryQueryCallback> callback = new RemoteHistoryQueryCallback;
			pending_callbacks.push_back(callback); // Retain before actual dispatch, including assertion/timeout unwind.
			UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(*handler, "getChatMessages", {{"chatId", owner_id}, {"refreshNative", true}}, callback));
			UAM_ASSERT_EQ(callback->successes + callback->failures, 0); // Genuine async request, before the real UI pump.
			RunCreationLoopUntil([callback] { return callback->Completed(); }, std::chrono::seconds(15));
			UAM_ASSERT(callback->Completed());
			UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
			AssertPrivateTransport(before); // This request itself reached the actual helper.
			if (callback->failures) std::cerr << "REMOTE_GEMINI_REFRESH_REFUSAL " << Json{{"status", callback->status}, {"body", callback->body}}.dump() << std::endl;
			return callback;
		}

		void RequireSuccess(CefRefPtr<RemoteHistoryQueryCallback> callback) const
		{
			if (callback->failures) throw std::runtime_error("Actual remote CEF refresh refused (" + std::to_string(callback->status) + "): " + callback->body);
			UAM_ASSERT_EQ(callback->successes, 1); UAM_ASSERT_EQ(callback->failures, 0);
		}

		void AssertUnrelatedAndSource() const
		{
			UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), unrelated_id);
			UAM_ASSERT_EQ(app.composer_text, std::string("Retain selected desktop draft")); UAM_ASSERT_EQ(app.chats.size(), std::size_t{2});
			const auto* other = ChatDomainService().FindChatById(app, unrelated_id); UAM_ASSERT(other);
			UAM_ASSERT_EQ(other->messages, unrelated_messages);
			UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *other), unrelated_metadata);
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(app.data_root, unrelated_id)), unrelated_primary);
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatSummaryFilePath(app.data_root, unrelated_id)), unrelated_summary);
			UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), folders);
			UAM_ASSERT_EQ(ReadFile(NativeFile()), native_bytes);
			UAM_ASSERT_EQ(ReadFile(NativeFile().parent_path().parent_path() / ".project_root"), workspace);
			UAM_ASSERT(app.acp_sessions.empty() && app.cli_terminals.empty() && app.pending_chat_save_at_by_chat_id.empty());
			UAM_ASSERT(app.resolved_native_sessions_by_chat_id.empty());
		}

		void AssertPublished(const ChatSession& expected, bool media) const
		{
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, owner_id, true);
			const auto& live = Current(); UAM_ASSERT(saved && saved->messages_loaded && live.messages_loaded);
			UAM_ASSERT_EQ(saved->messages, live.messages); UAM_ASSERT_EQ(saved->messages.size(), expected.messages.size());
			UAM_ASSERT_EQ(saved->messages.front().content, expected.messages.front().content);
			UAM_ASSERT_EQ(saved->messages.back().content, expected.messages.back().content);
			UAM_ASSERT_EQ(saved->messages.back().tool_calls.front().result_text, std::string("Remote native tool result"));
			for (const auto* chat : {&*saved, &live})
			{
				UAM_ASSERT_EQ(chat->id, owner_id); UAM_ASSERT_EQ(chat->provider_id, std::string("gemini-cli"));
				UAM_ASSERT_EQ(chat->native_session_id, native_id); UAM_ASSERT_EQ(chat->execution_host_id, host.id); UAM_ASSERT_EQ(chat->workspace_directory, workspace);
				UAM_ASSERT(!chat->imported_read_only && chat->pinned); UAM_ASSERT_EQ(chat->title, std::string("Retain remote saved title"));
				UAM_ASSERT_EQ(chat->folder_id, std::string("remote-history-folder"));
			}
			std::size_t parts = 0;
			for (std::size_t row = 0; row < expected.messages.size(); ++row)
			{
				UAM_ASSERT_EQ(saved->messages[row].attachments.size(), expected.messages[row].attachments.size());
				for (std::size_t index = 0; index < expected.messages[row].attachments.size(); ++index)
				{
					++parts; const auto& before = expected.messages[row].attachments[index]; const auto& actual = saved->messages[row].attachments[index];
					UAM_ASSERT(actual.source == AttachmentSource::OwnedNativeAsset && !actual.native_inline_payload && actual.path.empty());
					UAM_ASSERT_EQ(actual.id, before.id); UAM_ASSERT_EQ(actual.name, before.name); UAM_ASSERT_EQ(actual.kind, before.kind);
					UAM_ASSERT_EQ(actual.mime_type, before.mime_type); UAM_ASSERT_EQ(actual.size_bytes, before.size_bytes); UAM_ASSERT_EQ(actual.content_digest, before.content_digest);
					UAM_ASSERT_EQ(actual.native_part_key, before.native_part_key); UAM_ASSERT_EQ(actual.native_tool_call_id, before.native_tool_call_id);
					UAM_ASSERT_EQ(actual.native_source_field, before.native_source_field); UAM_ASSERT_EQ(actual.origin_execution_host_id, host.id);
					UAM_ASSERT(uam::native_assets::IsKeyForOwner(owner_id, actual.native_asset_key));
					UAM_ASSERT(!uam::native_assets::IsKeyForOwner(native_id, actual.native_asset_key) && !uam::native_assets::IsKeyForOwner(unrelated_id, actual.native_asset_key));
					const auto bytes = uam::native_assets::ReadOwned(app.data_root, owner_id, actual.native_asset_key);
					UAM_ASSERT(bytes.ok && bytes.error.empty()); UAM_ASSERT_EQ(bytes.bytes, original);
				}
			}
			UAM_ASSERT_EQ(parts, media ? std::size_t{2} : std::size_t{0}); AssertUnrelatedAndSource();
		}

		void AssertResponse(CefRefPtr<RemoteHistoryQueryCallback> callback, const ChatSession& expected, bool media) const
		{
			const auto body = Json::parse(callback->body);
			UAM_ASSERT_EQ(body.at("chatId"), Json(owner_id)); UAM_ASSERT(!body.at("unchanged").get<bool>());
			UAM_ASSERT_EQ(body.at("messages").size(), expected.messages.size());
			UAM_ASSERT_EQ(body.at("messages").back().at("content"), Json(expected.messages.back().content));
			UAM_ASSERT(callback->body.find(png) == std::string::npos); // No original blob in the bridge DTO.
			std::size_t parts = 0;
			for (std::size_t row = 0; row < expected.messages.size(); ++row)
			{
				const auto& message = body.at("messages").at(row);
				if (expected.messages[row].attachments.empty() && !message.contains("attachments")) continue;
				const auto& actual = message.at("attachments");
				UAM_ASSERT(actual.is_array());
				UAM_ASSERT_EQ(actual.size(), expected.messages[row].attachments.size());
				for (std::size_t index = 0; index < actual.size(); ++index)
				{
					++parts; const auto& part = actual.at(index); const auto& saved = Current().messages[row].attachments[index];
					UAM_ASSERT_EQ(part.at("source"), Json("owned_native_asset"));
					UAM_ASSERT_EQ(part.at("nativeAssetKey"), Json(saved.native_asset_key));
					UAM_ASSERT_EQ(part.at("originExecutionHostId"), Json(host.id));
					UAM_ASSERT_EQ(part.at("nativePartKey"), Json(saved.native_part_key));
				}
			}
			UAM_ASSERT_EQ(parts, media ? std::size_t{2} : std::size_t{0});
		}

		void Settle(CefRefPtr<RemoteHistoryQueryCallback> callback)
		{
			UAM_ASSERT(callback->Completed());
			callback->completion.get(); // Actual callback, not a generic post-callback ACK/retirement witness.
			UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
		}

		void EstablishText()
		{
			const auto expected = ReadGenuineExpected(false); const auto callback = Refresh();
			RequireSuccess(callback); AssertPublished(expected, false); AssertResponse(callback, expected, false); Settle(callback);
		}
	};

	struct RemoteGeminiRetainedHandlerState
	{
		std::string primary, summary, metadata;
		std::optional<std::string> revision, summary_revision;
		std::vector<Message> messages;
		std::shared_ptr<const std::uint8_t> incarnation;
		explicit RemoteGeminiRetainedHandlerState(const RemoteGeminiHistoryHandlerFixture& fixture)
		{
			primary = ReadFile(fixture.Primary()); summary = ReadFile(fixture.Summary()); UAM_ASSERT(!primary.empty() && !summary.empty());
			metadata = ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, fixture.Current()); messages = fixture.Current().messages;
			revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.owner_id);
			summary_revision = ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, fixture.owner_id);
			incarnation = fixture.Current().metadata_incarnation;
		}
		void AssertRetained(const RemoteGeminiHistoryHandlerFixture& fixture) const
		{
			UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
			UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.owner_id), revision);
			UAM_ASSERT_EQ(ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, fixture.owner_id), summary_revision);
			UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, fixture.Current()), metadata);
			UAM_ASSERT_EQ(fixture.Current().messages, messages); UAM_ASSERT(fixture.Current().metadata_incarnation == incarnation);
			const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.owner_id, true); UAM_ASSERT(saved && saved->messages == messages);
			fixture.AssertUnrelatedAndSource();
		}
	};
}

UAM_TEST(RemoteNativeHistoryHandlerGeminiInlineOriginalsPublishAfterActualHelper)
{
	RemoteGeminiHistoryHandlerFixture fixture; fixture.EstablishText();
	const RemoteGeminiRetainedHandlerState before(fixture);
	fixture.WriteNative(true); const auto expected = fixture.ReadGenuineExpected(true);
	const auto callback = fixture.Refresh();
	if (callback->failures) { before.AssertRetained(fixture); fixture.Settle(callback); }
	fixture.RequireSuccess(callback);
	fixture.AssertPublished(expected, true); // Callback must already own saved/live exact original bytes.
	fixture.AssertResponse(callback, expected, true);
	fixture.Settle(callback);
}

UAM_TEST(RemoteNativeHistoryHandlerGeminiOriginalStoreRefusalAndFreshRetry)
{
	RemoteGeminiHistoryHandlerFixture fixture; fixture.EstablishText();
	const RemoteGeminiRetainedHandlerState before(fixture);
	fixture.WriteNative(true); const auto expected = fixture.ReadGenuineExpected(true);
	const auto store = fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory);
	UAM_ASSERT(!fs::exists(store)); UAM_ASSERT(uam::io::WriteTextFile(store, "Private CEF remote original-store obstruction"));
	const auto failed = fixture.Refresh();
	UAM_ASSERT_EQ(failed->successes, 0); UAM_ASSERT_EQ(failed->failures, 1); UAM_ASSERT(failed->status == 500 || failed->status == 502);
	before.AssertRetained(fixture);
	UAM_ASSERT(fs::is_regular_file(store)); UAM_ASSERT_EQ(ReadFile(store), std::string("Private CEF remote original-store obstruction"));
	fixture.Settle(failed);
	UAM_ASSERT(fs::remove(store));
	const auto retry = fixture.Refresh(); // Fresh real request/transport/source read after the refusal; no prepared/snapshot injection.
	if (retry->failures) { before.AssertRetained(fixture); fixture.Settle(retry); }
	fixture.RequireSuccess(retry); fixture.AssertPublished(expected, true); fixture.AssertResponse(retry, expected, true); fixture.Settle(retry);
}
#endif


#include "app/provider_resolution_service.h"

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_SSH && defined(__APPLE__)
namespace
{
	void AwaitRemoteCandidateCapacity()
	{
		// Source-qualified ONLY for the proposed remote R2 route's actual Shared
		// Work lease. Read real capacity, never manufacture a receipt or ACK.
		// Principal data/byte/callback assertions must already have passed.
		AwaitBranchFaultOwnerCount(0);
	}

	struct RemoteOwnedControlsFixture final : RemoteGeminiHistoryHandlerFixture
	{
		~RemoteOwnedControlsFixture() noexcept
		{
			// This source-pinned candidate uses Shared Work ownership. Keep the
			// private root/environment alive through actual callbacks FIRST, then
			// actual worker value destruction. Capacity is not an ACK/IO witness.
			try
			{
				RunCreationLoopUntil([this] { return AllQueriesCompleted(); }, std::chrono::seconds(90));
				if (!AllQueriesCompleted()) throw std::runtime_error("Actual remote control callbacks remain pending.");
				AwaitBranchFaultOwnerCount(0);
			}
			catch (...)
			{
				std::cerr << "FATAL_PRIVATE_REMOTE_CONTROL_RETIREMENT_TIMEOUT: callback or source-qualified Work retirement did not settle; private environment and files will not be unwound." << std::endl;
				std::abort();
			}
		}
	};

	std::vector<std::string> RemoteExistingOriginalKeys(const RemoteGeminiHistoryHandlerFixture& fixture)
	{
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.owner_id, true); UAM_ASSERT(saved);
		std::vector<std::string> keys;
		for (const auto& message : saved->messages) for (const auto& part : message.attachments)
		{
			UAM_ASSERT(part.source == AttachmentSource::OwnedNativeAsset && !part.native_inline_payload);
			UAM_ASSERT_EQ(part.origin_execution_host_id, fixture.host.id);
			UAM_ASSERT(uam::native_assets::IsKeyForOwner(fixture.owner_id, part.native_asset_key));
			const auto original = uam::native_assets::ReadOwned(fixture.app.data_root, fixture.owner_id, part.native_asset_key);
			UAM_ASSERT(original.ok && original.error.empty()); UAM_ASSERT_EQ(original.bytes, fixture.original);
			keys.push_back(part.native_asset_key);
		}
		UAM_ASSERT_EQ(keys.size(), std::size_t{2}); return keys;
	}

	void AssertRemoteOriginalKeysRemain(const RemoteGeminiHistoryHandlerFixture& fixture, const std::vector<std::string>& keys)
	{
		for (const auto& key : keys)
		{
			const auto original = uam::native_assets::ReadOwned(fixture.app.data_root, fixture.owner_id, key);
			UAM_ASSERT(original.ok && original.error.empty()); UAM_ASSERT_EQ(original.bytes, fixture.original);
		}
	}

	void EstablishActualRemoteOwnedMedia(RemoteGeminiHistoryHandlerFixture& fixture)
	{
		fixture.EstablishText(); fixture.WriteNative(true);
		const auto expected = fixture.ReadGenuineExpected(true); const auto callback = fixture.Refresh();
		fixture.RequireSuccess(callback); fixture.AssertPublished(expected, true); fixture.AssertResponse(callback, expected, true);
		fixture.Settle(callback); (void)RemoteExistingOriginalKeys(fixture);
		AwaitRemoteCandidateCapacity(); // Two real slots available for genuine supersession; not IO/durability authority.
	}

	void AdvanceActualRemoteAnswer(RemoteGeminiHistoryHandlerFixture& fixture, const std::string& answer)
	{
		auto document = nlohmann::json::parse(fixture.native_bytes);
		UAM_ASSERT(document.at("messages").size() == 2);
		UAM_ASSERT(document.at("messages").back().at("content") != nlohmann::json(answer));
		document["messages"][1]["content"] = answer;
		fixture.native_bytes = document.dump(); UAM_ASSERT(uam::io::WriteTextFile(fixture.NativeFile(), fixture.native_bytes));
	}

	CefRefPtr<RemoteHistoryQueryCallback> BeginActualRemoteRefresh(RemoteGeminiHistoryHandlerFixture& fixture)
	{
		CefRefPtr<RemoteHistoryQueryCallback> callback = new RemoteHistoryQueryCallback;
		fixture.pending_callbacks.push_back(callback); // Existing genuine callback RAII drain retains private env on failure.
		UAM_ASSERT(ChatIconHandlerTestAccess::InstructionInvoke(*fixture.handler, "getChatMessages",
		    {{"chatId", fixture.owner_id}, {"refreshNative", true}}, callback));
		UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
		return callback;
	}

	void AwaitActualRemoteRefusal(RemoteGeminiHistoryHandlerFixture& fixture, CefRefPtr<RemoteHistoryQueryCallback> callback)
	{
		RunCreationLoopUntil([callback] { return callback->Completed(); }, std::chrono::seconds(15));
		UAM_ASSERT(callback->Completed());
		UAM_ASSERT_EQ(callback->successes, 0); UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->status, 409);
		fixture.Settle(callback);
	}

	struct ActualRemoteLoadedBoundary
	{
		// Both guards are constructed AFTER the owning fixture, so unwind releases
		// blocked real queues before the fixture drains its actual callback futures.
		HandlerBranchQueueRelease first_release, preparation_release;
		std::shared_ptr<std::atomic<int>> first = std::make_shared<std::atomic<int>>(0);
		std::shared_ptr<std::atomic<int>> preparation = std::make_shared<std::atomic<int>>(0);

		CefRefPtr<RemoteHistoryQueryCallback> Start(RemoteGeminiHistoryHandlerFixture& fixture)
		{
			UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(first_release.promise.get_future().share(), first)));
			RunCreationLoopUntil([this] { return first->load() == 1; }, std::chrono::seconds(3)); UAM_ASSERT_EQ(first->load(), 1);
			const auto launches = fixture.Launches(); const auto callback = BeginActualRemoteRefresh(fixture);
			// Real FIFO order: first hold -> native reader -> preparation hold.
			UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(preparation_release.promise.get_future().share(), preparation)));
			first_release.Release();
			RunCreationLoopUntil([this] { return preparation->load() == 1; }, std::chrono::seconds(15)); UAM_ASSERT_EQ(preparation->load(), 1);
			fixture.AssertPrivateTransport(launches); // This actual query's native load returned before the hold entered.
			const auto ui = std::make_shared<std::atomic<int>>(0);
			// Load worker posts its UI continuation before the next FIFO task enters;
			// this real UI witness follows it and proves preparation was admitted.
			UAM_ASSERT(CefPostTask(TID_UI, new CreationQueueWitness(ui, 1)));
			RunCreationLoopUntil([ui] { return ui->load() == 1; }, std::chrono::seconds(3)); UAM_ASSERT_EQ(ui->load(), 1);
			UAM_ASSERT_EQ(preparation->load(), 1); UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
			UAM_ASSERT(!callback->Completed());
			return callback;
		}
		void Release() { preparation_release.Release(); }
	};
}

UAM_TEST(RemoteNativeHistoryHandlerGeminiLoadedOwnerDriftPreservesOriginalsAndRetries)
{
	for (const bool profile_drift : {false, true})
	{
		RemoteOwnedControlsFixture fixture; EstablishActualRemoteOwnedMedia(fixture);
		const auto keys = RemoteExistingOriginalKeys(fixture); const RemoteGeminiRetainedHandlerState before(fixture);
		AdvanceActualRemoteAnswer(fixture, profile_drift ? "Fresh remote answer after profile refusal" : "Fresh remote answer after host refusal");
		const auto expected = fixture.ReadGenuineExpected(true);
		ActualRemoteLoadedBoundary boundary; const auto callback = boundary.Start(fixture);
		auto host = std::ranges::find_if(fixture.app.settings.execution_hosts, [&](const auto& value) { return value.id == fixture.host.id; });
		UAM_ASSERT(host != fixture.app.settings.execution_hosts.end());
		const auto actual_profile = ProviderResolutionService().ProviderForChatOrDefault(fixture.app, fixture.Current());
		auto profile = std::ranges::find_if(fixture.app.provider_profiles, [&](const auto& value) { return value.id == actual_profile.id; });
		UAM_ASSERT(profile != fixture.app.provider_profiles.end()); const auto old_host = *host; const auto old_profile = *profile;
		if (profile_drift) profile->title += " changed during native refresh";
		else host->ssh_alias += "-changed";
		UAM_ASSERT_EQ(callback->successes + callback->failures, 0); boundary.Release();
		AwaitActualRemoteRefusal(fixture, callback); before.AssertRetained(fixture); AssertRemoteOriginalKeysRemain(fixture, keys);
		*host = old_host; *profile = old_profile;
		AwaitRemoteCandidateCapacity(); before.AssertRetained(fixture); AssertRemoteOriginalKeysRemain(fixture, keys);
		const auto fresh = fixture.Refresh(); fixture.RequireSuccess(fresh);
		fixture.AssertPublished(expected, true); fixture.AssertResponse(fresh, expected, true); fixture.Settle(fresh);
		UAM_ASSERT_EQ(RemoteExistingOriginalKeys(fixture), keys);
	}
}

UAM_TEST(RemoteNativeHistoryHandlerGeminiLoadedStorageDriftPreservesAuthorityAndRetries)
{
	for (const bool summary_only : {false, true})
	{
		RemoteOwnedControlsFixture fixture; EstablishActualRemoteOwnedMedia(fixture);
		const auto keys = RemoteExistingOriginalKeys(fixture);
		AdvanceActualRemoteAnswer(fixture, summary_only ? "Fresh answer after summary revision refusal" : "Fresh answer after primary revision refusal");
		const auto expected = fixture.ReadGenuineExpected(true);
		const auto old_primary = ReadFile(fixture.Primary()), old_summary = ReadFile(fixture.Summary());
		const auto old_revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.owner_id);
		const auto old_summary_revision = ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, fixture.owner_id);
		const auto old_live_messages = fixture.Current().messages;
		const auto old_live_metadata = ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, fixture.Current());
		ActualRemoteLoadedBoundary boundary; const auto callback = boundary.Start(fixture);
		auto external = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.owner_id, true); UAM_ASSERT(external);
		if (summary_only)
		{
			external->last_opened_at = "2030-10-18T09:00:00Z";
			UAM_ASSERT(ChatRepository::SaveLastOpenedAt(fixture.app.data_root, *external));
			UAM_ASSERT_EQ(ReadFile(fixture.Primary()), old_primary);
			UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.owner_id), old_revision);
			UAM_ASSERT(ReadFile(fixture.Summary()) != old_summary);
			UAM_ASSERT(ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, fixture.owner_id) != old_summary_revision);
		}
		else
		{
			external->model_id = "externally-selected-remote-model";
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *external));
			UAM_ASSERT(ReadFile(fixture.Primary()) != old_primary);
			UAM_ASSERT(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.owner_id) != old_revision);
		}
		// Capture genuine NEW primary/summary bytes and unchanged OLD live graph.
		// The rejection must preserve both; a timestamp is not a substitute witness.
		const RemoteGeminiRetainedHandlerState external_state(fixture);
		UAM_ASSERT_EQ(fixture.Current().messages, old_live_messages);
		UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, fixture.Current()), old_live_metadata);
		boundary.Release(); AwaitActualRemoteRefusal(fixture, callback);
		external_state.AssertRetained(fixture); AssertRemoteOriginalKeysRemain(fixture, keys);
		AwaitRemoteCandidateCapacity(); external_state.AssertRetained(fixture);
		// Fresh retry requires genuine saved/live reconciliation, never an operation
		// silently choosing disk over the stale live owner it originally captured.
		const auto reloaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.owner_id, true); UAM_ASSERT(reloaded);
		if (summary_only) UAM_ASSERT_EQ(reloaded->last_opened_at, external->last_opened_at);
		else UAM_ASSERT_EQ(reloaded->model_id, external->model_id);
		fixture.Current() = *reloaded;
		const auto fresh = fixture.Refresh(); fixture.RequireSuccess(fresh);
		fixture.AssertPublished(expected, true); fixture.AssertResponse(fresh, expected, true); fixture.Settle(fresh);
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.owner_id, true); UAM_ASSERT(saved);
		if (summary_only) { UAM_ASSERT_EQ(saved->last_opened_at, external->last_opened_at); UAM_ASSERT_EQ(fixture.Current().last_opened_at, external->last_opened_at); }
		else { UAM_ASSERT_EQ(saved->model_id, external->model_id); UAM_ASSERT_EQ(fixture.Current().model_id, external->model_id); }
		UAM_ASSERT_EQ(RemoteExistingOriginalKeys(fixture), keys);
	}
}

UAM_TEST(RemoteNativeHistoryHandlerGeminiLoadedRequestSupersededByNewestPublicRefresh)
{
	RemoteOwnedControlsFixture fixture; EstablishActualRemoteOwnedMedia(fixture);
	const auto keys = RemoteExistingOriginalKeys(fixture); const RemoteGeminiRetainedHandlerState before(fixture);
	AdvanceActualRemoteAnswer(fixture, "Loaded obsolete answer must not publish");
	ActualRemoteLoadedBoundary boundary; const auto first = boundary.Start(fixture);
	AdvanceActualRemoteAnswer(fixture, "Newest exact public refresh owns this answer");
	const auto expected = fixture.ReadGenuineExpected(true); const auto launches = fixture.Launches();
	const auto newest = BeginActualRemoteRefresh(fixture);
	UAM_ASSERT(!first->Completed() && !newest->Completed()); before.AssertRetained(fixture);
	boundary.Release();
	RunCreationLoopUntil([first, newest] { return first->Completed() && newest->Completed(); }, std::chrono::seconds(15));
	UAM_ASSERT(first->Completed() && newest->Completed());
	UAM_ASSERT_EQ(first->successes, 0); UAM_ASSERT_EQ(first->failures, 1); UAM_ASSERT_EQ(first->status, 409);
	fixture.AssertPrivateTransport(launches); fixture.RequireSuccess(newest);
	fixture.AssertPublished(expected, true); fixture.AssertResponse(newest, expected, true);
	fixture.Settle(first); fixture.Settle(newest);
	UAM_ASSERT_EQ(first->successes + first->failures, 1); UAM_ASSERT_EQ(newest->successes + newest->failures, 1);
	UAM_ASSERT_EQ(RemoteExistingOriginalKeys(fixture), keys); AssertRemoteOriginalKeysRemain(fixture, keys);
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_SSH && defined(__APPLE__)
namespace
{
	void EstablishRemoteTextWithoutWarning(RemoteOwnedControlsFixture& fixture)
	{
		fixture.app.status_line = "Earlier unrelated remote application status";
		const auto expected = fixture.ReadGenuineExpected(false); const auto callback = fixture.Refresh();
		fixture.RequireSuccess(callback); fixture.AssertPublished(expected, false); fixture.AssertResponse(callback, expected, false);
		UAM_ASSERT(!nlohmann::json::parse(callback->body).contains("warning"));
		UAM_ASSERT_EQ(fixture.app.status_line, std::string("Earlier unrelated remote application status"));
		fixture.Settle(callback);
		AwaitRemoteCandidateCapacity(); // Source-qualified Work retirement; not ACK or original-byte proof.
	}

	struct ActualRemoteAckBoundary
	{
		ActualRemoteLoadedBoundary loaded;
		HandlerBranchQueueRelease acknowledgement_release;
		std::shared_ptr<std::atomic<int>> acknowledgement = std::make_shared<std::atomic<int>>(0);

		CefRefPtr<RemoteHistoryQueryCallback> Start(RemoteOwnedControlsFixture& fixture, const ChatSession& expected)
		{
			const auto callback = loaded.Start(fixture); // Real A -> load -> B, then actual UI loaded fence.
			// Actual prepare is queued behind B. Queue C behind prepare so the UI
			// publication completes while genuine ACK is still queued behind C.
			UAM_ASSERT(CefPostTask(TID_FILE_BACKGROUND, new InstructionQueueHold(acknowledgement_release.promise.get_future().share(), acknowledgement)));
			loaded.Release();
			RunCreationLoopUntil([this] { return acknowledgement->load() == 1; }, std::chrono::seconds(15));
			UAM_ASSERT_EQ(acknowledgement->load(), 1);
			const auto ui = std::make_shared<std::atomic<int>>(0);
			UAM_ASSERT(CefPostTask(TID_UI, new CreationQueueWitness(ui, 1)));
			RunCreationLoopUntil([ui] { return ui->load() == 1; }, std::chrono::seconds(3)); UAM_ASSERT_EQ(ui->load(), 1);
			UAM_ASSERT_EQ(acknowledgement->load(), 1);
			UAM_ASSERT_EQ(callback->successes + callback->failures, 0); UAM_ASSERT(!callback->Completed());
			fixture.AssertPublished(expected, true); // Real primary/live two-image publication BEFORE actual ACK/callback.
			(void)RemoteExistingOriginalKeys(fixture);
			return callback;
		}
		void Release() { acknowledgement_release.Release(); }
	};

	fs::path RemoteFirstPublishedGeneration(RemoteOwnedControlsFixture& fixture)
	{
		const auto store = fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory);
		std::vector<fs::path> generations;
		for (const auto& owner : fs::directory_iterator(store))
			if (owner.is_directory()) for (const auto& generation : fs::directory_iterator(owner.path()))
				if (generation.is_directory()) generations.push_back(generation.path());
		UAM_ASSERT_EQ(generations.size(), std::size_t{1});
		UAM_ASSERT(fs::is_regular_file(generations.front() / "attempted.receipt"));
		UAM_ASSERT(!fs::exists(generations.front() / "published.receipt"));
		return generations.front();
	}
}

UAM_TEST(RemoteNativeHistoryHandlerGeminiAckUncertaintyRetainsPublishedOriginals)
{
	RemoteOwnedControlsFixture fixture; EstablishRemoteTextWithoutWarning(fixture);
	const auto store = fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory);
	UAM_ASSERT(!fs::exists(store)); // Genuine first PNG batch, not prior already-acknowledged reusable originals.
	const auto old_primary = ReadFile(fixture.Primary());
	fixture.WriteNative(true); AdvanceActualRemoteAnswer(fixture, "Committed remote answer survives real acknowledgement refusal");
	const auto expected = fixture.ReadGenuineExpected(true);
	ActualRemoteAckBoundary boundary; const auto callback = boundary.Start(fixture, expected);
	const auto keys = RemoteExistingOriginalKeys(fixture); const RemoteGeminiRetainedHandlerState committed(fixture);
	UAM_ASSERT(committed.primary != old_primary);
	const auto generation = RemoteFirstPublishedGeneration(fixture), obstruction = generation / "published.receipt";
	fs::create_directories(obstruction);
	const std::string obstruction_text = "Private actual remote acknowledgement obstruction";
	UAM_ASSERT(uam::io::WriteTextFile(obstruction / "keep", obstruction_text));
	UAM_ASSERT_EQ(boundary.acknowledgement->load(), 1); UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	UAM_ASSERT_EQ(fixture.app.status_line, std::string("Earlier unrelated remote application status"));
	boundary.Release();
	RunCreationLoopUntil([callback] { return callback->Completed(); }, std::chrono::seconds(15));
	UAM_ASSERT(callback->Completed()); fixture.RequireSuccess(callback); fixture.Settle(callback);
	const auto body = nlohmann::json::parse(callback->body);
	const std::string warning = "History and attached files were kept, but UAM could not confirm the final save. Reopen the chat to check it.";
	UAM_ASSERT_EQ(body.at("warning"), nlohmann::json(warning));
	UAM_ASSERT_EQ(fixture.app.status_line, warning);
	UAM_ASSERT_EQ(uam::StateSerializer::Serialize(fixture.app, true).at("statusLine"), nlohmann::json(warning));
	fixture.AssertResponse(callback, expected, true); fixture.AssertPublished(expected, true); committed.AssertRetained(fixture);
	UAM_ASSERT_EQ(RemoteExistingOriginalKeys(fixture), keys); AssertRemoteOriginalKeysRemain(fixture, keys);
	AwaitRemoteCandidateCapacity(); // Value destruction only AFTER actual failed-ACK callback and independent bytes.
	UAM_ASSERT_EQ(callback->successes + callback->failures, 1); committed.AssertRetained(fixture);
	fixture.AssertPublished(expected, true); UAM_ASSERT_EQ(RemoteExistingOriginalKeys(fixture), keys);
	UAM_ASSERT(fs::is_directory(obstruction)); UAM_ASSERT_EQ(ReadFile(obstruction / "keep"), obstruction_text);
	UAM_ASSERT(fs::is_regular_file(generation / "attempted.receipt"));
}

UAM_TEST(RemoteNativeHistoryHandlerGeminiAckHeldResponseUsesCurrentOwner)
{
	for (const bool replacement_owner : {false, true})
	{
		RemoteOwnedControlsFixture fixture; EstablishRemoteTextWithoutWarning(fixture);
		UAM_ASSERT(!fs::exists(fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory)));
		fixture.WriteNative(true); AdvanceActualRemoteAnswer(fixture, "Actual committed answer before current-owner response");
		const auto expected = fixture.ReadGenuineExpected(true);
		ActualRemoteAckBoundary boundary; const auto callback = boundary.Start(fixture, expected);
		const auto keys = RemoteExistingOriginalKeys(fixture);
		const auto generation = RemoteFirstPublishedGeneration(fixture);
		const auto committed_primary = ReadFile(fixture.Primary()), committed_summary = ReadFile(fixture.Summary());
		const auto committed_revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.owner_id);
		const auto committed_summary_revision = ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, fixture.owner_id);
		const auto old_incarnation = fixture.Current().metadata_incarnation;
		if (replacement_owner)
		{
			// Explicit live-graph replacement race, not a claim to exercise Delete UI.
			// A genuinely default-constructed owner supplies a new birth incarnation;
			// no native result or prepared transaction is fabricated or delivered.
			ChatSession replacement = fixture.Current();
			replacement.metadata_incarnation = ChatSession{}.metadata_incarnation;
			replacement.created_at = "2031-10-18T09:00:00Z";
			ChatDomainService().AddMessage(replacement, MessageRole::User, "PRIVATE_REPLACEMENT_OWNER_AUTHORED_CONTENT");
			UAM_ASSERT(replacement.metadata_incarnation != old_incarnation);
			fixture.Current() = std::move(replacement);
		}
		else
		{
			ChatDomainService().AddMessage(fixture.Current(), MessageRole::User, "Actual newer domain-authored question must remain current");
			UAM_ASSERT(fixture.Current().metadata_incarnation == old_incarnation);
		}
		const auto current_messages = fixture.Current().messages;
		const auto current_metadata = ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, fixture.Current());
		const auto current_incarnation = fixture.Current().metadata_incarnation;
		const auto current_birth = fixture.Current().created_at;
		const auto current_expected = fixture.Current();
		UAM_ASSERT_EQ(current_messages.size(), std::size_t{3});
		UAM_ASSERT_EQ(callback->successes + callback->failures, 0); boundary.Release();
		RunCreationLoopUntil([callback] { return callback->Completed(); }, std::chrono::seconds(15)); UAM_ASSERT(callback->Completed());
		if (replacement_owner)
		{
			UAM_ASSERT_EQ(callback->successes, 0); UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->status, 409);
			UAM_ASSERT(callback->body.find("already saved") != std::string::npos);
			UAM_ASSERT(callback->body.find("Reopen the chat") != std::string::npos);
			UAM_ASSERT(callback->body.find(expected.messages.back().content) == std::string::npos);
			UAM_ASSERT(callback->body.find("PRIVATE_REPLACEMENT_OWNER_AUTHORED_CONTENT") == std::string::npos);
			UAM_ASSERT(callback->body.find("nativeAssetKey") == std::string::npos);
		}
		else
		{
			fixture.RequireSuccess(callback); fixture.AssertResponse(callback, current_expected, true);
			UAM_ASSERT(!nlohmann::json::parse(callback->body).contains("warning"));
		}
		fixture.Settle(callback);
		UAM_ASSERT_EQ(fixture.app.status_line, std::string("Earlier unrelated remote application status"));
		AwaitRemoteCandidateCapacity(); // Real callback first; then known candidate Work retirement before private teardown.
		UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
		UAM_ASSERT(fs::is_regular_file(generation / "published.receipt")); // Real healthy ACK receipt, not a callback-only assumption.
		UAM_ASSERT_EQ(ReadFile(fixture.Primary()), committed_primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), committed_summary);
		UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.owner_id), committed_revision);
		UAM_ASSERT_EQ(ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, fixture.owner_id), committed_summary_revision);
		UAM_ASSERT_EQ(fixture.Current().messages, current_messages);
		UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, fixture.Current()), current_metadata);
		UAM_ASSERT(fixture.Current().metadata_incarnation == current_incarnation); UAM_ASSERT_EQ(fixture.Current().created_at, current_birth);
		UAM_ASSERT_EQ(RemoteExistingOriginalKeys(fixture), keys); AssertRemoteOriginalKeysRemain(fixture, keys);
		fixture.AssertUnrelatedAndSource();
	}
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
namespace
{
	struct NativeOpenPrivateReadDenial
	{
		fs::path path;
		fs::perms original;
		explicit NativeOpenPrivateReadDenial(const fs::path& value) : path(value), original(fs::status(value).permissions())
		{
			fs::permissions(path, fs::perms::none, fs::perm_options::replace);
		}
		~NativeOpenPrivateReadDenial()
		{
			std::error_code error;
			fs::permissions(path, original, fs::perm_options::replace, error);
		}
	};

	// One generic Open has one actual background preparation and a UI finish.
	// Drain both queues while handler/app/private environment are still alive.
	struct NativeOpenActualCompletion
	{
		CefRefPtr<RecordingInstructionCallback> callback = new RecordingInstructionCallback;
		bool settled = false;
		bool Drain()
		{
			RunCreationLoopUntil([this] { return callback->successes + callback->failures != 0; }, std::chrono::seconds(15));
			if (callback->successes + callback->failures == 0) return false;
			const auto background = std::make_shared<std::atomic<int>>(0);
			if (!CefPostTask(TID_FILE_BACKGROUND, new CreationQueueWitness(background, 1))) return false;
			RunCreationLoopUntil([background] { return background->load() == 1; }, std::chrono::seconds(15));
			if (background->load() != 1) return false;
			const auto ui = std::make_shared<std::atomic<int>>(0);
			if (!CefPostTask(TID_UI, new CreationQueueWitness(ui, 1))) return false;
			RunCreationLoopUntil([ui] { return ui->load() == 1; }, std::chrono::seconds(15));
			return ui->load() == 1;
		}
		void Await()
		{
			settled = Drain();
			UAM_ASSERT(settled);
			UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
		}
		~NativeOpenActualCompletion() noexcept
		{
			if (settled) return;
			try
			{
				if (Drain()) return;
			}
			catch (...) {}
			std::cerr << "Private native-open fixture cannot retire its actual query; refusing environment teardown." << std::endl;
			std::abort(); // Test-only fail-closed cleanup, never a production hook.
		}
	};
}

UAM_TEST(NativeChildLineageHandlerGeminiFailedSavedHydrationPreservesOwnerAndFreshOpenRetries)
{
	for (const bool fuller_saved : {false, true})
	{
		PassiveGeminiChildMediaFixture fixture(false); // Actual private canonical parent journal; no passive Open is invoked.
		fs::permissions(fixture.temp.root, fs::perms::owner_all, fs::perm_options::replace);
		const auto private_home = fixture.temp.root / "native-open-os-home";
		fs::create_directories(private_home);
		ScopedEnvVar home("HOME", uam::paths::Utf8PathString(private_home));
		ScopedEnvVar userprofile("USERPROFILE", uam::paths::Utf8PathString(private_home));
		auto* live = ChatDomainService().FindChatById(fixture.app, fixture.parent.id);
		UAM_ASSERT(live && live->messages_loaded && live->messages.size() == 2);
		live->messages.back().content = "Keep actual saved native-open answer";
		if (fuller_saved) live->messages.push_back({MessageRole::User, "Keep actual additional saved native-open turn"});
		live->updated_at = fuller_saved ? "2019-10-18T08:00:02Z" : "2026-10-16T08:00:02Z";
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *live));
		const auto saved_before = ChatRepository::LoadLocalChat(fixture.app.data_root, live->id, true);
		UAM_ASSERT(saved_before && saved_before->messages_loaded && saved_before->messages.size() == (fuller_saved ? 3U : 2U));
		*live = *saved_before;
		fixture.parent = *saved_before;
		const auto initial_native = ChatHistorySyncService().LoadNativeSessionChats(fixture.chats, ProviderProfileStore::DefaultGeminiProfile());
		UAM_ASSERT(initial_native.size() == 1 && initial_native.front().messages_loaded && initial_native.front().messages.size() == 2);
		UAM_ASSERT_EQ(initial_native.front().native_session_id, fixture.parent_native);
		UAM_ASSERT_EQ(initial_native.front().messages.back().content, std::string("Actual parent delegation"));
		const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, live->id);
		const auto primary_revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, live->id);
		const auto summary_revision = ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, live->id);
		const auto selected = ChatDomainService().SelectedChatId(fixture.app);
		const auto draft = fixture.app.composer_text;
		const auto mappings = fixture.app.resolved_native_sessions_by_chat_id;
		const auto before_messages = live->messages;
		const auto before_metadata = ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, *live);
		const auto* unrelated_before = ChatDomainService().FindChatById(fixture.app, fixture.unrelated.id);
		UAM_ASSERT(unrelated_before);
		const auto unrelated_messages = unrelated_before->messages;
		const auto unrelated_metadata = ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, *unrelated_before);
		struct File { fs::path path; std::string bytes; fs::perms mode; };
		std::vector<File> files;
		std::set<fs::path> paths;
		for (const auto& root : {fixture.app.data_root, fixture.storage})
			for (const auto& entry : fs::recursive_directory_iterator(root))
				if (entry.is_regular_file()) { files.push_back({entry.path(), ReadFile(entry.path()), entry.status().permissions()}); paths.insert(entry.path()); }
		auto handler = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		const nlohmann::json payload{{"chatId", fixture.parent.id}, {"nativeSessionId", fixture.parent_native}, {"selectChat", false}, {"refreshHistory", true}};
		CefRefPtr<RecordingInstructionCallback> refused;
		{
			NativeOpenPrivateReadDenial denied(primary);
			UAM_ASSERT_EQ(fs::status(primary).permissions(), fs::perms::none);
			std::string inaccessible;
			UAM_ASSERT(!uam::io::TryReadTextFile(primary, inaccessible)); // Root/ACL override is a failed fixture prerequisite.
			const auto cold = ChatRepository::LoadLocalChat(fixture.app.data_root, live->id, false);
			UAM_ASSERT(cold && !cold->messages_loaded && cold->messages.empty() && cold->persisted_message_count == saved_before->messages.size());
			auto attempted = *cold;
			std::string warning;
			UAM_ASSERT(!ChatRepository::HydrateChatMessages(fixture.app.data_root, attempted, &warning) && !warning.empty());
			UAM_ASSERT(live->messages_loaded && live->messages == before_messages); // Warm baseline avoids the earlier direct hydration branch.
			NativeOpenActualCompletion open;
			ChatIconHandlerTestAccess::OpenNative(*handler, payload, open.callback); // Actual HandleOpenNativeSessionChat, never Finish/snapshot injection.
			open.Await();
			refused = open.callback;
		}
		// Restore/read all private files and live authority before the failure oracle.
		std::set<fs::path> after_paths;
		for (const auto& root : {fixture.app.data_root, fixture.storage})
			for (const auto& entry : fs::recursive_directory_iterator(root)) if (entry.is_regular_file()) after_paths.insert(entry.path());
		UAM_ASSERT_EQ(after_paths, paths);
		for (const auto& file : files) { UAM_ASSERT_EQ(ReadFile(file.path), file.bytes); UAM_ASSERT_EQ(fs::status(file.path).permissions(), file.mode); }
		live = ChatDomainService().FindChatById(fixture.app, fixture.parent.id);
		UAM_ASSERT(live && live->messages_loaded && live->messages == before_messages);
		UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, *live), before_metadata);
		UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, live->id), primary_revision);
		UAM_ASSERT_EQ(ChatRepository::ChatSummaryStorageRevision(fixture.app.data_root, live->id), summary_revision);
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selected); UAM_ASSERT_EQ(fixture.app.composer_text, draft);
		UAM_ASSERT_EQ(fixture.app.resolved_native_sessions_by_chat_id, mappings);
		const auto* unrelated = ChatDomainService().FindChatById(fixture.app, fixture.unrelated.id);
		UAM_ASSERT(unrelated && unrelated->messages == unrelated_messages);
		UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, *unrelated), unrelated_metadata);
		UAM_ASSERT(fixture.app.cli_terminals.empty() && fixture.app.acp_sessions.empty() && fixture.app.pending_chat_save_at_by_chat_id.empty());
		UAM_ASSERT_EQ(refused->successes, 0); UAM_ASSERT_EQ(refused->failures, 1); UAM_ASSERT_EQ(refused->status, 409);
		UAM_ASSERT_EQ(refused->body, std::string("Child history could not be prepared.")); // Public worker reached the required local overlay, not a fake snapshot.
		const auto fresh_bytes = fixture.parent_bytes + PassiveGeminiChildMediaFixture::Line({{"$set", {{"lastUpdated", "2026-10-17T08:00:02Z"}}}}) +
		    PassiveGeminiChildMediaFixture::Line({{"$patch", {{"id", "parent-answer"}, {"content", "Fresh actual public native-open answer"}}}}) +
		    (fuller_saved ? PassiveGeminiChildMediaFixture::Line({{"id", "fresh-native-user"}, {"type", "user"}, {"timestamp", "2026-10-17T08:00:02Z"}, {"content", "Fresh additional native row"}}) : std::string{});
		UAM_ASSERT(uam::io::WriteTextFile(fixture.ParentFile(), fresh_bytes));
		const auto actual = ChatHistorySyncService().LoadNativeSessionChats(fixture.chats, ProviderProfileStore::DefaultGeminiProfile());
		UAM_ASSERT(actual.size() == 1 && actual.front().messages.size() == saved_before->messages.size());
		UAM_ASSERT_EQ(actual.front().messages[1].content, std::string("Fresh actual public native-open answer"));
		NativeOpenActualCompletion retry;
		ChatIconHandlerTestAccess::OpenNative(*handler, payload, retry.callback);
		retry.Await();
		if (retry.callback->failures) std::cerr << "Native-open hydration retry: " << retry.callback->status << " " << retry.callback->body << std::endl;
		UAM_ASSERT_EQ(retry.callback->successes, 1); UAM_ASSERT_EQ(retry.callback->failures, 0);
		UAM_ASSERT_EQ(nlohmann::json::parse(retry.callback->body).at("chatId"), nlohmann::json(fixture.parent.id));
		const auto saved_after = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.parent.id, true);
		live = ChatDomainService().FindChatById(fixture.app, fixture.parent.id);
		UAM_ASSERT(saved_after && live && saved_after->messages_loaded && live->messages_loaded && saved_after->messages == live->messages);
		UAM_ASSERT_EQ(saved_after->messages[1].content, actual.front().messages[1].content);
		UAM_ASSERT_EQ(saved_after->messages.size(), actual.front().messages.size());
		UAM_ASSERT_EQ(saved_after->title, saved_before->title); UAM_ASSERT_EQ(saved_after->pinned, saved_before->pinned);
		UAM_ASSERT_EQ(saved_after->native_session_id, fixture.parent_native); UAM_ASSERT_EQ(saved_after->provider_id, saved_before->provider_id);
		UAM_ASSERT_EQ(saved_after->workspace_directory, fixture.workspace); UAM_ASSERT_EQ(saved_after->execution_host_id, std::string("local"));
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selected); UAM_ASSERT_EQ(fixture.app.composer_text, draft);
		UAM_ASSERT_EQ(fixture.app.resolved_native_sessions_by_chat_id, mappings);
		UAM_ASSERT_EQ(ReadFile(fixture.ParentFile()), fresh_bytes);
		for (const auto& file : files) if (file.path == fixture.ChildFile() || file.path == AppPaths::UamChatFilePath(fixture.app.data_root, fixture.unrelated.id)) UAM_ASSERT_EQ(ReadFile(file.path), file.bytes);
		unrelated = ChatDomainService().FindChatById(fixture.app, fixture.unrelated.id);
		UAM_ASSERT(unrelated && unrelated->messages == unrelated_messages);
		UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, *unrelated), unrelated_metadata);
		UAM_ASSERT(fixture.app.cli_terminals.empty() && fixture.app.acp_sessions.empty());
	}
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	struct PassiveChildMaximumTestTimezone
	{
		std::unique_ptr<ScopedEnvVar> zone = std::make_unique<ScopedEnvVar>("TZ", "UTC0");
		static void Reload()
		{
#if defined(_WIN32)
			_tzset();
#else
			tzset();
#endif
		}
		PassiveChildMaximumTestTimezone() { Reload(); }
		~PassiveChildMaximumTestTimezone() { zone.reset(); Reload(); }
	};

	void CheckPassiveGeminiChildRefreshTimestampMaximum(bool native_is_newer)
	{
		// Known historical instants in a scoped UTC0 controller process, not an
		// assumption about the timezone provenance of existing real user records.
		PassiveChildMaximumTestTimezone timezone;
		const auto metadata_time = std::chrono::sys_days{std::chrono::year{2026} / 10 / 16} + std::chrono::hours{14};
		const auto metadata_epoch = std::chrono::system_clock::to_time_t(metadata_time);
		const auto native_epoch = metadata_epoch + (native_is_newer ? 3600 : -3600);
		UAM_ASSERT(native_is_newer ? native_epoch > metadata_epoch : native_epoch < metadata_epoch);
		const auto metadata_stamp = uam::time::detail::FormatTimestamp(uam::time::detail::LocalTimeSnapshot(metadata_epoch), uam::time::kLocalDisplayTimestampFormat);
		const auto native_stamp = uam::time::detail::FormatTimestamp(uam::time::detail::UtcTimeSnapshot(native_epoch), uam::time::kIsoUtcTimestampFormat);
		UAM_ASSERT_EQ(metadata_stamp, std::string("2026-10-16 14:00:00"));
		UAM_ASSERT_EQ(native_stamp, native_is_newer ? std::string("2026-10-16T15:00:00.000Z") : std::string("2026-10-16T13:00:00.000Z"));

		PassiveGeminiChildMediaFixture fixture(true);
		auto handler_owner = UamQueryHandlerTestAccess::Create(fixture.app, "file:///private/index.html");
		const auto initial = fixture.Open(*handler_owner);
		UAM_ASSERT_EQ(initial->successes, 1); UAM_ASSERT_EQ(initial->failures, 0);
		const auto initial_body = nlohmann::json::parse(initial->body);
		UAM_ASSERT_EQ(initial_body.at("chatId"), nlohmann::json(fixture.reference));
		UAM_ASSERT(!initial_body.contains("warning"));
		const auto original_keys = fixture.AssertSaved(true); // Real public import, passive owner and actual acknowledged PNGs first.
		UAM_ASSERT_EQ(original_keys.size(), std::size_t{2});
		auto* child = ChatDomainService().FindChatById(fixture.app, fixture.reference);
		UAM_ASSERT(child && child->messages_loaded && child->messages.size() == 2);
		UAM_ASSERT(child->imported_read_only && child->native_session_id.empty());
		UAM_ASSERT(child->passive_queued_prompt_archive);
		const auto archive_owner = child->passive_queued_prompt_archive->owner_chat_id;
		const auto archive_prompts = child->passive_queued_prompt_archive->canonical_prompts_json;
		UAM_ASSERT_EQ(archive_owner, fixture.reference);
		const auto original_messages = child->messages;
		const auto original_birth = child->metadata_incarnation;
		const auto original_created = child->created_at;
		// An actual warm metadata-only service save on the already-published
		// passive owner, preserving its transcript, lineage and empty resume ID.
		child->title = "Retain passive metadata maximum";
		child->updated_at = metadata_stamp;
		UAM_ASSERT(ChatHistorySyncService().SaveChatWithStatus(fixture.app, *child,
		    "Private passive metadata saved.", "Private passive metadata save failed."));
		const auto baseline = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.reference, true);
		const auto baseline_summary = ChatRepository::LoadLocalChatSummaryReadOnly(fixture.app.data_root, fixture.reference);
		UAM_ASSERT(baseline && baseline_summary && baseline->messages == original_messages && child->messages == original_messages);
		UAM_ASSERT_EQ(baseline->updated_at, metadata_stamp); UAM_ASSERT_EQ(baseline_summary->updated_at, metadata_stamp);
		UAM_ASSERT_EQ(baseline->title, child->title);
		UAM_ASSERT(child->metadata_incarnation == original_birth && child->created_at == original_created);
		UAM_ASSERT_EQ(fixture.AssertSaved(true), original_keys);
		const auto primary_path = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.reference);
		const auto baseline_primary = ReadFile(primary_path);

		fixture.WriteChild(true, native_is_newer ? "Actual newer15 passive answer" : "Actual native13 passive answer");
		// Patch intentionally does not update timestamps. Use the supported full
		// same-ID Put record, copying the actual complete answer/tools/PNG object
		// and changing only its timestamp. Header/project/parent/agent stay intact.
		const auto header_end = fixture.child_bytes.find('\n');
		UAM_ASSERT(header_end != std::string::npos);
		const auto user_end = fixture.child_bytes.find('\n', header_end + 1);
		UAM_ASSERT(user_end != std::string::npos && user_end + 1 < fixture.child_bytes.size());
		const auto answer_bytes = fixture.child_bytes.substr(user_end + 1);
		auto answer_row = nlohmann::json::parse(answer_bytes);
		UAM_ASSERT_EQ(PassiveGeminiChildMediaFixture::Line(answer_row), answer_bytes);
		UAM_ASSERT_EQ(answer_row.at("id"), nlohmann::json("child-answer"));
		UAM_ASSERT_EQ(answer_row.at("type"), nlohmann::json("gemini"));
		UAM_ASSERT_EQ(answer_row.at("content"), nlohmann::json(fixture.expected_answer));
		UAM_ASSERT(answer_row.contains("timestamp") && answer_row.at("toolCalls").size() == 1);
		answer_row["timestamp"] = native_stamp;
		fixture.child_bytes += PassiveGeminiChildMediaFixture::Line({{"$set", {{"lastUpdated", native_stamp}}}}) +
		    PassiveGeminiChildMediaFixture::Line(answer_row);
		UAM_ASSERT(uam::io::WriteTextFile(fixture.ChildFile(), fixture.child_bytes));
		const auto native = fixture.ReadNativeChild(); // Actual attested native reader; no supplied snapshot/seal.
		UAM_ASSERT(native && native->messages_loaded && native->messages.size() == original_messages.size());
		UAM_ASSERT_EQ(native->updated_at, native_stamp);
		UAM_ASSERT_EQ(native->messages.back().created_at, native_stamp);
		UAM_ASSERT_EQ(native->messages.back().content, fixture.expected_answer);
		UAM_ASSERT(native->messages.back().content != original_messages.back().content);
		UAM_ASSERT(native->imported_read_only && native->native_session_id.empty() && native->id == fixture.reference);
		UAM_ASSERT_EQ(native->parent_chat_id, fixture.parent_native);
		UAM_ASSERT_EQ(native->workspace_directory, fixture.workspace);
		UAM_ASSERT_EQ(fixture.parts.size(), std::size_t{2});
		for (const auto& [row, expected] : fixture.parts)
		{
			const auto& attachments = native->messages[row].attachments;
			const auto found = std::ranges::find_if(attachments, [&](const auto& part) { return part.id == expected.id; });
			UAM_ASSERT(found != attachments.end() && found->source == AttachmentSource::NativeInlinePending && found->native_inline_payload);
			UAM_ASSERT_EQ(found->native_part_key, expected.native_part_key);
		}
		const auto still_saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.reference, true);
		UAM_ASSERT(still_saved && still_saved->messages == original_messages && still_saved->updated_at == metadata_stamp);
		UAM_ASSERT_EQ(ReadFile(primary_path), baseline_primary);
		const auto refreshed = fixture.Open(*handler_owner, true); // One actual public refresh after the new real journal.
		UAM_ASSERT_EQ(refreshed->successes, 1); UAM_ASSERT_EQ(refreshed->failures, 0);
		const auto body = nlohmann::json::parse(refreshed->body);
		UAM_ASSERT_EQ(body.at("chatId"), nlohmann::json(fixture.reference));
		UAM_ASSERT(!body.contains("warning")); // Actual normal ACK path, not uncertain committed success.
		UAM_ASSERT_EQ(fixture.AssertSaved(true), original_keys); // New native answer, exact saved/live PNG bytes/associations BEFORE recency oracle.
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.reference, true);
		const auto summary = ChatRepository::LoadLocalChatSummaryReadOnly(fixture.app.data_root, fixture.reference);
		const auto* live = ChatDomainService().FindChatById(fixture.app, fixture.reference);
		UAM_ASSERT(saved && summary && live && live->metadata_incarnation == original_birth);
		UAM_ASSERT(saved->messages == live->messages && saved->messages != original_messages);
		UAM_ASSERT_EQ(saved->messages.back().created_at, native_stamp);
		UAM_ASSERT(ReadFile(primary_path) != baseline_primary);
		UAM_ASSERT(fixture.app.branch_preparations.empty()); // Open also waited actual worker retirement, not a fabricated completion.
		UAM_ASSERT_EQ(refreshed->successes + refreshed->failures, 1);
		for (const ChatSession* actual : {&*saved, &*summary, live})
		{
			UAM_ASSERT_EQ(actual->title, baseline->title);
			UAM_ASSERT_EQ(actual->pinned, baseline->pinned);
			UAM_ASSERT(actual->custom_icon == baseline->custom_icon);
			UAM_ASSERT_EQ(actual->folder_id, baseline->folder_id);
			UAM_ASSERT_EQ(actual->created_at, original_created);
			UAM_ASSERT_EQ(actual->provider_id, baseline->provider_id);
			UAM_ASSERT_EQ(actual->execution_host_id, std::string("local"));
			UAM_ASSERT_EQ(actual->workspace_directory, fixture.workspace);
			UAM_ASSERT(actual->imported_read_only && actual->native_session_id.empty());
			UAM_ASSERT_EQ(actual->parent_chat_id, fixture.parent_native);
			UAM_ASSERT_EQ(actual->subagent_parent_chat_id, fixture.parent.id);
			UAM_ASSERT_EQ(actual->subagent_parent_created_at, fixture.parent.created_at);
			UAM_ASSERT(actual->passive_queued_prompt_archive);
			UAM_ASSERT_EQ(actual->passive_queued_prompt_archive->owner_chat_id, archive_owner);
			UAM_ASSERT_EQ(actual->passive_queued_prompt_archive->canonical_prompts_json, archive_prompts);
		}
		fixture.AssertParentAndUnrelated(); // Exact parent draft/selection/files, native bytes and no execution/queue authority.
		// Both arms have already accepted the genuine changed native graph and
		// healthy original ACK. This maximum must not veto that graph admission.
		const auto& expected_maximum = native_is_newer ? native_stamp : metadata_stamp;
		for (const ChatSession* actual : {&*saved, &*summary, live})
			UAM_ASSERT_EQ(actual->updated_at, expected_maximum);
	}
}

UAM_TEST(NativeChildLineageHandlerGeminiRefreshKeepsNewerSavedTimestamp)
{
	CheckPassiveGeminiChildRefreshTimestampMaximum(false);
}

UAM_TEST(NativeChildLineageHandlerGeminiRefreshAdvancesNewerNativeTimestamp)
{
	CheckPassiveGeminiChildRefreshTimestampMaximum(true);
}
#endif
