#include "test_harness.h"
#include "cef_handler_fixture.h"
#include "cef/uam_query_handler_async.h"
#include "common/chat/native_attachment_assets.h"
#include "common/runtime/native_refresh_lifetime.h"
#include "app/transcript_retention_service.h"
#include "remote/native_history_snapshot.h"
#include "include/cef_api_hash.h"

#include <future>

using namespace uam_test;
using namespace uam::query_handler_async;

struct RemoteRefreshFeedbackTestAccess
{
	static void Invoke(UamQueryHandler& handler, const std::string& id, CefRefPtr<UamQueryHandler::Callback> cb)
	{
		handler.HandleGetChatMessages(nullptr, {{"chatId", id}, {"refreshNative", true}}, cb);
	}
	static void ReadSaved(UamQueryHandler& handler, const std::string& id, CefRefPtr<UamQueryHandler::Callback> cb, std::size_t limit = 50, const std::string& digest = "")
	{
		handler.HandleGetChatMessages(nullptr, {{"chatId", id}, {"limit", limit}, {"deferToolCallContent", true}, {"messagesDigest", digest}}, cb);
	}
	static void Select(UamQueryHandler& handler, const std::string& id, CefRefPtr<UamQueryHandler::Callback> cb)
	{
		handler.HandleSelectSession(nullptr, {{"chatId", id}}, cb);
	}
	static void ObserveDispatchThreads(UamQueryHandler& handler, std::function<void(CefThreadId)> observer)
	{
		handler.m_nativeMediaDispatchThreadObserverForTests = std::move(observer);
	}
	static void Detail(UamQueryHandler& handler, bool image, const nlohmann::json& payload, CefRefPtr<UamQueryHandler::Callback> cb)
	{
		if (image) handler.HandleGetAttachmentImagePreview(nullptr, payload, cb);
		else handler.HandleGetToolCallContent(nullptr, payload, cb);
	}
	static void Configure(UamQueryHandler& handler,
	    std::function<bool(CefRefPtr<UamQueryHandler::Callback>, std::function<AsyncCefResult()>, std::function<void(AsyncCefResult&)>)> dispatch,
	    std::function<std::optional<ChatSession>(const ExecutionHost&, const ChatSession&, std::string*, std::stop_token)> reader)
	{
		handler.m_attachmentPreviewDispatcherForTests = dispatch;
		handler.m_nativeMediaRefreshDispatcherForTests = std::move(dispatch);
		handler.m_remoteNativeHistoryReaderForTests = std::move(reader);
	}
	static void Fault(UamQueryHandler& handler, uam::io::AtomicWriteStage stage, bool throws)
	{
		handler.m_nativeMediaPublicationStageGateForTests = [stage, throws](auto reached)
		{
			if (reached != stage) return true;
			if (throws) throw std::runtime_error("Opaque injected native publication failure");
			return false;
		};
	}
};

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	const std::string kPng = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
	const std::string kPriorWarning = "Earlier warning must remain";
	const std::string kCommittedWarning = "History and attached files were kept, but UAM could not confirm the final save. Reopen the chat to check it.";

	class FeedbackCallback final : public UamQueryHandler::Callback
	{
	public:
		int successes = 0, failures = 0, status = 0;
		nlohmann::json body;
		std::string error;
		void Success(const CefString& value) override { ++successes; body = nlohmann::json::parse(value.ToString()); }
		void Success(const void*, size_t) override { ++successes; }
		void Failure(int code, const CefString& value) override { ++failures; status = code; error = value.ToString(); }
	private:
		IMPLEMENT_REFCOUNTING(FeedbackCallback);
	};
	struct FeedbackQuery
	{
		CefRefPtr<UamQueryHandler::Callback> callback;
		std::function<AsyncCefResult()> worker;
		std::function<void(AsyncCefResult&)> completion;
		AsyncCefResult Run() { return std::async(std::launch::async, worker).get(); }
		void Finish(AsyncCefResult result) { CompleteAsyncCefQuery(callback, result, completion); }
	};
	struct FeedbackFixture
	{
		TempDir temp{"uam-remote-refresh-feedback"};
		uam::AppState app;
		std::unique_ptr<UamQueryHandler> handler;
		std::vector<FeedbackQuery> pending;
		bool dispatch_available = true;
		std::shared_ptr<std::atomic<int>> reads = std::make_shared<std::atomic<int>>(0);
		FeedbackFixture(bool with_tools = false, std::size_t image_count = 1)
		{
			UAM_ASSERT(cef_api_hash(CEF_API_VERSION, 0) != nullptr);
			app.data_root = temp.root / "controller";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ExecutionHost host;
			host.id = "isolated-remote"; host.transport = "ssh"; host.ssh_alias = "must-never-be-launched";
			host.platform = uam::remote::native_history::Platform(); host.runner_status = "ready";
			host.runner_version = std::string(uam::constants::kAppVersion).substr(1);
			host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
			app.settings.execution_hosts.push_back(host);
			ChatSession chat;
			chat.id = "task-1055-isolated-owner"; chat.provider_id = "gemini-cli";
			chat.native_session_id = "77777777-7777-4777-8777-777777777777";
			chat.workspace_directory = host.platform.starts_with("windows") ? "C:/isolated/task-1055" : "/srv/task-1055";
			chat.execution_host_id = host.id; chat.branch_root_chat_id = chat.id;
			chat.created_at = chat.updated_at = "2026-10-18T08:00:00Z";
			chat.title = "Retained saved title"; chat.pinned = true;
			chat.messages.push_back(Message{MessageRole::User, "Recorded question"});
			if (with_tools)
			{
				ToolCall tool;
				tool.id = "reused-tool"; tool.name = "read_file"; tool.status = "completed"; tool.result_text = "OLD live tool output";
				chat.messages.front().tool_calls.push_back(tool);
			}
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
			UAM_ASSERT(saved); app.chats.push_back(*saved);
			ChatDomainService().SelectChatById(app, chat.id);
			app.composer_text = "Retained authored draft"; app.status_line = kPriorWarning;
			nlohmann::json content = nlohmann::json::array({{{"text", "Recorded question"}}});
			for (std::size_t index = 0; index < image_count; ++index) content.push_back({{"inlineData", {{"mimeType", "image/png"}, {"data", kPng}}}});
			const nlohmann::json native{{"sessionId", chat.native_session_id}, {"projectHash", "isolated-project"}, {"startTime", chat.created_at},
			    {"lastUpdated", "2026-10-18T08:00:02Z"}, {"messages", nlohmann::json::array({
			        {{"id", "user"}, {"type", "user"}, {"timestamp", chat.created_at},
			            {"content", content}},
			        {{"id", "answer"}, {"type", "gemini"}, {"timestamp", "2026-10-18T08:00:02Z"}, {"content", "New native answer"}}
			    })}};
			const nlohmann::json snapshot{{"providerId", chat.provider_id}, {"sessionId", chat.native_session_id},
			    {"workspace", chat.workspace_directory}, {"text", native.dump()}};
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
			RemoteRefreshFeedbackTestAccess::Configure(*handler,
			    [this](auto cb, auto worker, auto completion)
			    {
				    if (!dispatch_available) return false;
				    pending.push_back({cb, std::move(worker), std::move(completion)});
				    return true;
			    },
			    [snapshot, count = reads, with_tools](const ExecutionHost& owner, const ChatSession& identity, std::string* error, std::stop_token stop)
			    {
				    ++*count;
				    // Real bounded native decoder, isolated snapshot files; never a provider or SSH process.
				    auto parsed = ChatHistorySyncService().ParseRemoteNativeHistorySnapshot(owner, identity, snapshot, error, stop);
				    if (parsed && with_tools)
				    {
					    ToolCall tool;
					    tool.id = "reused-tool"; tool.name = "read_file"; tool.status = "completed"; tool.result_text = "NEW saved tool output";
					    parsed->messages.front().tool_calls.push_back(tool);
				    }
				    return parsed;
			    });
		}
		~FeedbackFixture()
		{
			pending.clear(); handler.reset();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (uam::native_history::detail::refresh_owners.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		}
		ChatSession& Chat() { return app.chats.front(); }
		fs::path Primary() const { return AppPaths::UamChatFilePath(app.data_root, app.chats.front().id); }
		fs::path Summary() const { return AppPaths::UamChatSummaryFilePath(app.data_root, app.chats.front().id); }
		CefRefPtr<FeedbackCallback> Start()
		{
			CefRefPtr<FeedbackCallback> cb = new FeedbackCallback;
			RemoteRefreshFeedbackTestAccess::Invoke(*handler, Chat().id, cb);
			return cb;
		}
		FeedbackQuery Take(std::size_t index) { return std::move(pending.at(index)); }
		void Complete(std::size_t index) { auto query = Take(index); query.Finish(query.Run()); }
		fs::path Generation() const
		{
			std::vector<fs::path> found;
			for (const auto& owner : fs::directory_iterator(app.data_root / std::string(uam::native_assets::kStoreDirectory)))
				for (const auto& generation : fs::directory_iterator(owner.path())) if (generation.is_directory()) found.push_back(generation.path());
			UAM_ASSERT_EQ(found.size(), std::size_t{1});
			return found.front();
		}
		void AssertDraft() const { UAM_ASSERT_EQ(app.composer_text, std::string("Retained authored draft")); }
		void AwaitRetirement() const
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (uam::native_history::detail::refresh_owners.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
			UAM_ASSERT_EQ(uam::native_history::detail::refresh_owners.load(), 0u);
		}
		void AssertPublished(CefRefPtr<FeedbackCallback> cb)
		{
			UAM_ASSERT_EQ(cb->successes, 1); UAM_ASSERT_EQ(cb->failures, 0);
			UAM_ASSERT_EQ(Chat().messages.size(), std::size_t{2});
			UAM_ASSERT_EQ(cb->body.at("messages").size(), std::size_t{2});
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, Chat().id, true, nullptr, false);
			UAM_ASSERT(saved); UAM_ASSERT_EQ(saved->messages, Chat().messages);
			const auto& attachment = Chat().messages.front().attachments.at(0);
			UAM_ASSERT_EQ(attachment.source, AttachmentSource::OwnedNativeAsset);
			const auto original = uam::native_assets::ReadOwned(app.data_root, Chat().id, attachment.native_asset_key);
			std::string expected; UAM_ASSERT(uam::base64::Decode(kPng, expected));
			UAM_ASSERT(original.ok); UAM_ASSERT_EQ(original.bytes, expected);
			UAM_ASSERT(Chat().pinned); UAM_ASSERT_EQ(Chat().title, std::string("Retained saved title")); AssertDraft();
		}
	};
}

UAM_TEST(RemoteRefreshFeedbackCleanGuardRefusalRetainsHistoryAndFreshRetry)
{
	FeedbackFixture fixture;
	const auto primary = ReadFile(fixture.Primary()), summary = ReadFile(fixture.Summary());
	const auto messages = fixture.Chat().messages;
	const auto cb = fixture.Start(); fixture.Complete(0);
	auto query = fixture.Take(1); auto result = query.Run(); UAM_ASSERT(result.ok);
	// A real journal save after preparation changes the repository's owned revision
	// without altering the live transcript or the primary/summary admission witnesses.
	auto journal_owner = fixture.Chat(); journal_owner.acp_queue_held = true;
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(fixture.app.data_root, journal_owner));
	const auto journal = fixture.app.data_root / "queue-outboxes" / (journal_owner.id + ".json");
	const auto journal_bytes = ReadFile(journal);
	query.Finish(std::move(result));
	UAM_ASSERT_EQ(cb->successes, 0); UAM_ASSERT_EQ(cb->failures, 1); UAM_ASSERT_EQ(cb->status, 409);
	UAM_ASSERT(cb->error.find("refresh was not saved") != std::string::npos);
	UAM_ASSERT(cb->error.find("Try refreshing again") != std::string::npos);
	UAM_ASSERT(cb->error.find("Reopen") == std::string::npos);
	UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
	UAM_ASSERT_EQ(ReadFile(journal), journal_bytes); UAM_ASSERT_EQ(fixture.Chat().messages, messages);
	UAM_ASSERT_EQ(fixture.app.status_line, kPriorWarning); fixture.AssertDraft();
	// Reconcile the independently saved journal through the actual repository, then
	// issue a fresh request; the refused prepared transaction itself is never replayed.
	journal_owner.acp_queue_held = false;
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(fixture.app.data_root, journal_owner));
	const auto reconciled = ChatRepository::LoadLocalChat(fixture.app.data_root, journal_owner.id, true, nullptr, false);
	UAM_ASSERT(reconciled); fixture.Chat() = *reconciled;
	const auto retry = fixture.Start(); fixture.Complete(2); fixture.Complete(3); fixture.Complete(4);
	fixture.AssertPublished(retry); UAM_ASSERT(!retry->body.contains("warning"));
	UAM_ASSERT_EQ(fixture.reads->load(), 2); UAM_ASSERT_EQ(cb->successes + cb->failures, 1);
}

UAM_TEST(RemoteRefreshFeedbackUnconfirmedPrimaryRequiresReopenBeforeRetry)
{
	for (const auto stage : {uam::io::AtomicWriteStage::BeforeReplace, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync})
		for (const bool throws : {false, true})
		{
			FeedbackFixture fixture;
			const auto primary = ReadFile(fixture.Primary()), summary = ReadFile(fixture.Summary());
			const auto messages = fixture.Chat().messages;
			RemoteRefreshFeedbackTestAccess::Fault(*fixture.handler, stage, throws);
			const auto cb = fixture.Start(); fixture.Complete(0); fixture.Complete(1);
			UAM_ASSERT_EQ(cb->successes, 0); UAM_ASSERT_EQ(cb->failures, 1); UAM_ASSERT_EQ(cb->status, 500);
			UAM_ASSERT(cb->error.find("could not confirm that the refreshed history was saved") != std::string::npos);
			UAM_ASSERT(cb->error.find("Reopen the chat before retrying") != std::string::npos);
			UAM_ASSERT(cb->error.find("not saved") == std::string::npos);
			UAM_ASSERT_EQ(fixture.Chat().messages, messages); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
			UAM_ASSERT_EQ(ReadFile(fixture.Primary()) == primary, stage == uam::io::AtomicWriteStage::BeforeReplace);
			UAM_ASSERT(fs::is_regular_file(fixture.Generation() / "attempted.receipt"));
			UAM_ASSERT(!fs::exists(fixture.Generation() / "published.receipt"));
			UAM_ASSERT_EQ(fixture.app.status_line, kPriorWarning); fixture.AssertDraft();
			UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{2}); // No ACK, adoption or automatic second publication.
		}
}

UAM_TEST(RemoteRefreshFeedbackRecoveryReadsSavedPrimaryWithoutAcknowledging)
{
	for (const auto stage : {uam::io::AtomicWriteStage::BeforeReplace, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync})
		for (const bool throws : {false, true})
		{
			FeedbackFixture fixture;
			const auto live = fixture.Chat().messages;
			RemoteRefreshFeedbackTestAccess::Fault(*fixture.handler, stage, throws);
			const auto failed = fixture.Start(); fixture.Complete(0); fixture.Complete(1);
			UAM_ASSERT_EQ(failed->status, 500);
			const auto primary = ReadFile(fixture.Primary()), summary = ReadFile(fixture.Summary());
			const std::size_t expected_count = stage == uam::io::AtomicWriteStage::BeforeReplace ? 1 : 2;
			// Exercise the exact ordinary request made by Reload history twice. Neither
			// read may fall back to the old loaded transcript or acknowledge the candidate.
			for (int attempt = 0; attempt < 2; ++attempt)
			{
				CefRefPtr<FeedbackCallback> read = new FeedbackCallback;
				const auto next = fixture.pending.size();
				RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, read);
				if (!read->successes && !read->failures) fixture.Complete(next);
				UAM_ASSERT_EQ(read->successes, 1); UAM_ASSERT_EQ(read->failures, 0);
				UAM_ASSERT_EQ(read->body.at("messages").size(), expected_count);
				if (expected_count == 2) UAM_ASSERT_EQ(read->body.at("messages").back().at("content"), nlohmann::json("New native answer"));
				UAM_ASSERT_EQ(fixture.Chat().messages, live);
				UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
				UAM_ASSERT(!fs::exists(fixture.Generation() / "published.receipt"));
				UAM_ASSERT(fs::is_regular_file(fixture.Generation() / "attempted.receipt"));
				UAM_ASSERT_EQ(fixture.reads->load(), 1); UAM_ASSERT_EQ(fixture.app.status_line, kPriorWarning); fixture.AssertDraft();
			}
		}
}

UAM_TEST(RemoteRefreshFeedbackRecoveryRejectsChangedOwnerOrPrimary)
{
	for (const std::string change : {"live owner", "primary bytes", "saved binding", "pending save"})
	{
		FeedbackFixture fixture; const auto live = fixture.Chat().messages;
		RemoteRefreshFeedbackTestAccess::Fault(*fixture.handler, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync, false);
		const auto failed = fixture.Start(); fixture.Complete(0); fixture.Complete(1); UAM_ASSERT_EQ(failed->status, 500);
		if (change == "saved binding")
		{
			auto foreign = nlohmann::json::parse(ReadFile(fixture.Primary()));
			foreign["native_session_id"] = "88888888-8888-4888-8888-888888888888";
			UAM_ASSERT(uam::io::WriteTextFile(fixture.Primary(), foreign.dump()));
		}
		CefRefPtr<FeedbackCallback> read = new FeedbackCallback;
		RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, read);
		UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{3});
		auto query = fixture.Take(2); auto response = query.Run(); UAM_ASSERT(response.ok);
		if (change == "live owner") fixture.Chat().metadata_incarnation = ChatSession{}.metadata_incarnation;
		if (change == "primary bytes") UAM_ASSERT(uam::io::WriteTextFile(fixture.Primary(), ReadFile(fixture.Primary()) + "\n "));
		if (change == "pending save") fixture.app.pending_chat_save_at_by_chat_id[fixture.Chat().id] = 1.0;
		const auto primary = ReadFile(fixture.Primary()), summary = ReadFile(fixture.Summary());
		query.Finish(std::move(response));
		UAM_ASSERT_EQ(read->successes, 0); UAM_ASSERT_EQ(read->failures, 1); UAM_ASSERT_EQ(read->status, 409);
		UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
		UAM_ASSERT_EQ(fixture.Chat().messages, live); UAM_ASSERT_EQ(fixture.reads->load(), 1);
		UAM_ASSERT(!fs::exists(fixture.Generation() / "published.receipt"));
		UAM_ASSERT_EQ(fixture.app.status_line, kPriorWarning); fixture.AssertDraft();
	}
}

UAM_TEST(RemoteRefreshFeedbackRecoveryRefusesMalformedPrimaryAndInvalidPage)
{
	for (const bool invalid_page : {false, true})
	{
		FeedbackFixture fixture; const auto live = fixture.Chat().messages;
		RemoteRefreshFeedbackTestAccess::Fault(*fixture.handler, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync, false);
		const auto failed = fixture.Start(); fixture.Complete(0); fixture.Complete(1); UAM_ASSERT_EQ(failed->status, 500);
		if (!invalid_page) UAM_ASSERT(uam::io::WriteTextFile(fixture.Primary(), "{invalid primary"));
		const auto primary = ReadFile(fixture.Primary()), summary = ReadFile(fixture.Summary());
		CefRefPtr<FeedbackCallback> read = new FeedbackCallback;
		RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, read, invalid_page ? 201 : 50);
		if (!read->failures) fixture.Complete(2);
		UAM_ASSERT_EQ(read->successes, 0); UAM_ASSERT_EQ(read->failures, 1); UAM_ASSERT_EQ(read->status, invalid_page ? 400 : 500);
		UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
		UAM_ASSERT_EQ(fixture.Chat().messages, live); UAM_ASSERT_EQ(fixture.reads->load(), 1);
		UAM_ASSERT(!fs::exists(fixture.Generation() / "published.receipt"));
		UAM_ASSERT_EQ(fixture.app.status_line, kPriorWarning); fixture.AssertDraft();
	}
}

UAM_TEST(RemoteRefreshFeedbackRecoveryKeepsFailureWhenConnectionChanges)
{
	FeedbackFixture fixture; const auto live = fixture.Chat().messages;
	RemoteRefreshFeedbackTestAccess::Fault(*fixture.handler, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync, false);
	const auto failed = fixture.Start(); fixture.Complete(0); fixture.Complete(1); UAM_ASSERT_EQ(failed->status, 500);
	const auto primary = ReadFile(fixture.Primary()), summary = ReadFile(fixture.Summary());
	fixture.app.settings.execution_hosts.front().runner_status = "offline";
	CefRefPtr<FeedbackCallback> refused = new FeedbackCallback;
	RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, refused);
	UAM_ASSERT_EQ(refused->successes, 0); UAM_ASSERT_EQ(refused->status, 409);
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{2});
	fixture.app.settings.execution_hosts.front().runner_status = "ready";
	CefRefPtr<FeedbackCallback> recovered = new FeedbackCallback;
	RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, recovered); fixture.Complete(2);
	UAM_ASSERT_EQ(recovered->successes, 1); UAM_ASSERT_EQ(recovered->body.at("messages").size(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.Chat().messages, live); UAM_ASSERT_EQ(fixture.reads->load(), 1);
	UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
	UAM_ASSERT(!fs::exists(fixture.Generation() / "published.receipt"));
	UAM_ASSERT_EQ(fixture.app.status_line, kPriorWarning); fixture.AssertDraft();
}

UAM_TEST(RemoteRefreshFeedbackRecoveryDoesNotReusePreviousIncarnationMarker)
{
	FeedbackFixture fixture;
	RemoteRefreshFeedbackTestAccess::Fault(*fixture.handler, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync, false);
	const auto failed = fixture.Start(); fixture.Complete(0); fixture.Complete(1); UAM_ASSERT_EQ(failed->status, 500);
	fixture.Chat().metadata_incarnation = ChatSession{}.metadata_incarnation;
	fixture.Chat().messages = {Message{MessageRole::User, "New owner's current history"}};
	CefRefPtr<FeedbackCallback> read = new FeedbackCallback;
	RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, read);
	UAM_ASSERT_EQ(read->successes, 1); UAM_ASSERT_EQ(read->body.at("messages").size(), std::size_t{1});
	UAM_ASSERT_EQ(read->body.at("messages").front().at("content"), nlohmann::json("New owner's current history"));
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{2}); UAM_ASSERT_EQ(fixture.reads->load(), 1);
	UAM_ASSERT(!fs::exists(fixture.Generation() / "published.receipt"));
}

UAM_TEST(RemoteRefreshFeedbackRecoverySurvivesActualReopening)
{
	FeedbackFixture fixture;
	RemoteRefreshFeedbackTestAccess::Fault(*fixture.handler, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync, false);
	const auto failed = fixture.Start(); fixture.Complete(0); fixture.Complete(1); UAM_ASSERT_EQ(failed->status, 500);
	const auto live = fixture.Chat().messages;
	fixture.AssertDraft();
	// Use the real selection handler, including its recency change. Complete a
	// queued metadata fallback through the real repository when CEF is absent.
	ChatSession other;
	other.id = "task-1055-other-selection"; other.provider_id = "gemini-cli";
	other.created_at = other.updated_at = "2020-01-01T00:00:00Z";
	fixture.app.chats.push_back(std::move(other));
	ChatDomainService().SelectChatById(fixture.app, "task-1055-other-selection");
	CefRefPtr<FeedbackCallback> selected = new FeedbackCallback;
	RemoteRefreshFeedbackTestAccess::Select(*fixture.handler, fixture.Chat().id, selected);
	UAM_ASSERT_EQ(selected->successes, 1);
	UAM_ASSERT(fixture.Chat().last_opened_at != fixture.Chat().updated_at);
	if (fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.Chat().id))
	{
		const auto prepared = ChatRepository::PrepareLastOpenedAt(fixture.app.data_root, fixture.Chat());
		UAM_ASSERT(ChatRepository::PublishPreparedChatSave(prepared, true, &fixture.Chat()));
		fixture.app.pending_chat_save_at_by_chat_id.erase(fixture.Chat().id);
	}
	const auto primary = ReadFile(fixture.Primary()), summary = ReadFile(fixture.Summary());
	const auto selection_draft = fixture.app.composer_text;
	CefRefPtr<FeedbackCallback> read = new FeedbackCallback;
	const auto next = fixture.pending.size();
	RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, read);
	if (!read->successes && !read->failures) fixture.Complete(next);
	UAM_ASSERT_EQ(read->successes, 1); UAM_ASSERT_EQ(read->body.at("messages").size(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.Chat().messages, live);
	UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
	UAM_ASSERT(!fs::exists(fixture.Generation() / "published.receipt"));
	UAM_ASSERT_EQ(fixture.reads->load(), 1); UAM_ASSERT_EQ(fixture.app.composer_text, selection_draft);
}

UAM_TEST(RemoteRefreshFeedbackRecoverySurvivesNormalTranscriptEviction)
{
	FeedbackFixture fixture;
	RemoteRefreshFeedbackTestAccess::Fault(*fixture.handler, uam::io::AtomicWriteStage::BeforeReplace, false);
	const auto failed = fixture.Start(); fixture.Complete(0); fixture.Complete(1); UAM_ASSERT_EQ(failed->status, 500);
	const auto primary = ReadFile(fixture.Primary()), summary = ReadFile(fixture.Summary());
	const auto persisted = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id, true, nullptr, false); UAM_ASSERT(persisted);
	ChatDomainService().SelectChatById(fixture.app, "");
	uam::TranscriptRetentionService retention;
	retention.SetVisibleChatIds({});
	const auto release = retention.BeginRelease(fixture.app, fixture.Chat().id); UAM_ASSERT(release);
	std::vector<Message> retired;
	UAM_ASSERT(retention.CompleteRelease(fixture.app, fixture.Chat().id, *release, *persisted, &retired));
	UAM_ASSERT(!fixture.Chat().messages_loaded);
	CefRefPtr<FeedbackCallback> read = new FeedbackCallback;
	const auto next = fixture.pending.size();
	RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, read);
	if (!read->successes && !read->failures) fixture.Complete(next);
	UAM_ASSERT_EQ(read->successes, 1); UAM_ASSERT_EQ(read->body.at("messages").size(), std::size_t{1});
	UAM_ASSERT(!fixture.Chat().messages_loaded); UAM_ASSERT(fixture.Chat().messages.empty());
	UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
	UAM_ASSERT(!fs::exists(fixture.Generation() / "published.receipt")); UAM_ASSERT_EQ(fixture.reads->load(), 1);
}

UAM_TEST(RemoteRefreshFeedbackRecoveryDetailsUseObservedPrimary)
{
	FeedbackFixture fixture{true};
	RemoteRefreshFeedbackTestAccess::Fault(*fixture.handler, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync, false);
	const auto failed = fixture.Start(); fixture.Complete(0); fixture.Complete(1); UAM_ASSERT_EQ(failed->status, 500);
	const auto live = fixture.Chat().messages;
	const auto primary = ReadFile(fixture.Primary()), summary = ReadFile(fixture.Summary());
	CefRefPtr<FeedbackCallback> read = new FeedbackCallback;
	RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, read); fixture.Complete(2);
	const auto digest = read->body.at("messagesDigest");
	const auto& image = read->body.at("messages").front().at("attachments").front();
	UAM_ASSERT_EQ(image.at("historyDigest"), digest);
	UAM_ASSERT_EQ(read->body.at("messages").front().at("toolCalls").front().at("historyDigest"), digest);
	CefRefPtr<FeedbackCallback> repeated = new FeedbackCallback;
	const auto repeat_index = fixture.pending.size();
	RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, repeated, 50, digest.get<std::string>());
	fixture.Complete(repeat_index);
	UAM_ASSERT_EQ(repeated->successes, 1); UAM_ASSERT_EQ(repeated->body.at("messages").size(), std::size_t{2});
	UAM_ASSERT_EQ(repeated->body.at("messages").front().at("attachments").front().at("historyDigest"), digest);
	for (const bool is_image : {true, false})
	{
		CefRefPtr<FeedbackCallback> detail = new FeedbackCallback;
		nlohmann::json payload{{"chatId", fixture.Chat().id}, {"messageIndex", 0}, {"historyDigest", digest}};
		payload[is_image ? "attachmentId" : "toolCallId"] = is_image ? image.at("id") : nlohmann::json("reused-tool");
		const auto next = fixture.pending.size();
		RemoteRefreshFeedbackTestAccess::Detail(*fixture.handler, is_image, payload, detail);
		if (!detail->successes && !detail->failures) fixture.Complete(next);
		UAM_ASSERT_EQ(detail->successes, 1);
		if (is_image) { UAM_ASSERT_EQ(detail->body.at("available"), nlohmann::json(true)); UAM_ASSERT_EQ(detail->body.at("width"), nlohmann::json(1)); UAM_ASSERT(!detail->body.at("dataBase64").get<std::string>().empty()); }
		else { UAM_ASSERT(detail->body.at("content").get<std::string>().find("NEW saved tool output") != std::string::npos); UAM_ASSERT(detail->body.at("content").get<std::string>().find("OLD live") == std::string::npos); }
	}
	UAM_ASSERT_EQ(fixture.Chat().messages, live);
	UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
	UAM_ASSERT(!fs::exists(fixture.Generation() / "published.receipt")); UAM_ASSERT_EQ(fixture.reads->load(), 1);
	UAM_ASSERT_EQ(fixture.app.status_line, kPriorWarning); fixture.AssertDraft();
}

UAM_TEST(RemoteRefreshFeedbackRecoveryDetailsRefuseStaleOrAmbiguousIdentity)
{
	for (const bool image : {true, false})
		for (const std::string mutation : {"digest", "missing-digest", "primary-before", "primary-after", "owner", "incarnation", "missing-id"})
		{
			FeedbackFixture fixture{true};
			RemoteRefreshFeedbackTestAccess::Fault(*fixture.handler, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync, false);
			const auto failed = fixture.Start(); fixture.Complete(0); fixture.Complete(1); UAM_ASSERT_EQ(failed->status, 500);
			CefRefPtr<FeedbackCallback> read = new FeedbackCallback;
			RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, read); fixture.Complete(2);
			nlohmann::json payload{{"chatId", fixture.Chat().id}, {"messageIndex", 0}, {"historyDigest", read->body.at("messagesDigest")}};
			payload[image ? "attachmentId" : "toolCallId"] = image ? read->body.at("messages").front().at("attachments").front().at("id") : nlohmann::json("reused-tool");
			if (mutation == "digest") payload["historyDigest"] = "0000000000000000";
			if (mutation == "missing-digest") payload.erase("historyDigest");
			if (mutation == "missing-id") payload[image ? "attachmentId" : "toolCallId"] = "absent-detail";
			if (mutation == "primary-before") UAM_ASSERT(uam::io::WriteTextFile(fixture.Primary(), ReadFile(fixture.Primary()) + "\n"));
			if (mutation == "incarnation") fixture.Chat().metadata_incarnation = ChatSession{}.metadata_incarnation;
			const auto next = fixture.pending.size();
			CefRefPtr<FeedbackCallback> detail = new FeedbackCallback;
			RemoteRefreshFeedbackTestAccess::Detail(*fixture.handler, image, payload, detail);
			if (!detail->successes && !detail->failures)
			{
				auto query = fixture.Take(next); auto result = query.Run();
				if (mutation == "primary-after") UAM_ASSERT(uam::io::WriteTextFile(fixture.Primary(), ReadFile(fixture.Primary()) + "\n"));
				if (mutation == "owner") fixture.Chat().native_session_id = "another-native-owner";
				query.Finish(std::move(result));
			}
			UAM_ASSERT_EQ(detail->successes, 0); UAM_ASSERT_EQ(detail->failures, 1);
			UAM_ASSERT_EQ(detail->status, mutation == "missing-id" ? 404 : 409);
			UAM_ASSERT(!fs::exists(fixture.Generation() / "published.receipt")); UAM_ASSERT_EQ(fixture.reads->load(), 1);
			UAM_ASSERT_EQ(fixture.Chat().messages.front().tool_calls.front().result_text, std::string("OLD live tool output"));
			UAM_ASSERT_EQ(fixture.app.status_line, kPriorWarning); fixture.AssertDraft();
		}
}

UAM_TEST(RemoteRefreshFeedbackPassiveReadsUseUserBlockingPriorityAndReleaseRefusedWork)
{
	FeedbackFixture fixture{true};
	std::vector<CefThreadId> threads;
	RemoteRefreshFeedbackTestAccess::ObserveDispatchThreads(*fixture.handler, [&](CefThreadId thread) { threads.push_back(thread); });
	RemoteRefreshFeedbackTestAccess::Fault(*fixture.handler, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync, false);
	const auto failed = fixture.Start(); fixture.Complete(0); fixture.Complete(1); UAM_ASSERT_EQ(failed->status, 500);
	UAM_ASSERT_EQ(threads.size(), std::size_t{2});
	UAM_ASSERT(std::ranges::all_of(threads, [](auto thread) { return thread == TID_FILE_BACKGROUND; }));
	fixture.AwaitRetirement();
	CefRefPtr<FeedbackCallback> read = new FeedbackCallback;
	RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, read); fixture.Complete(2);
	UAM_ASSERT_EQ(read->successes, 1); UAM_ASSERT_EQ(threads.back(), TID_FILE_USER_BLOCKING);
	for (const bool image : {true, false})
	{
		fixture.AwaitRetirement();
		CefRefPtr<FeedbackCallback> detail = new FeedbackCallback;
		nlohmann::json payload{{"chatId", fixture.Chat().id}, {"messageIndex", 0}, {"historyDigest", read->body.at("messagesDigest")}};
		payload[image ? "attachmentId" : "toolCallId"] = image ? read->body.at("messages").front().at("attachments").front().at("id") : nlohmann::json("reused-tool");
		const auto next = fixture.pending.size();
		RemoteRefreshFeedbackTestAccess::Detail(*fixture.handler, image, payload, detail); fixture.Complete(next);
		UAM_ASSERT_EQ(detail->successes, 1); UAM_ASSERT_EQ(threads.back(), TID_FILE_USER_BLOCKING);
	}
	fixture.AwaitRetirement(); fixture.dispatch_available = false;
	CefRefPtr<FeedbackCallback> refused = new FeedbackCallback;
	RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, refused);
	UAM_ASSERT_EQ(refused->failures, 1); UAM_ASSERT_EQ(refused->status, 503); UAM_ASSERT_EQ(threads.back(), TID_FILE_USER_BLOCKING);
	fixture.AwaitRetirement(); fixture.dispatch_available = true;
	CefRefPtr<FeedbackCallback> retry = new FeedbackCallback;
	const auto next = fixture.pending.size(); RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, retry); fixture.Complete(next);
	UAM_ASSERT_EQ(retry->successes, 1);
	UAM_ASSERT(!fs::exists(fixture.Generation() / "published.receipt")); UAM_ASSERT_EQ(fixture.reads->load(), 1); fixture.AssertDraft();
}

UAM_TEST(RemoteRefreshFeedbackPreviewCapacityRefusalCanRetrySameBoundImage)
{
	FeedbackFixture fixture{false, 3};
	RemoteRefreshFeedbackTestAccess::Fault(*fixture.handler, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync, false);
	const auto failed = fixture.Start(); fixture.Complete(0); fixture.Complete(1); UAM_ASSERT_EQ(failed->status, 500);
	fixture.AwaitRetirement();
	CefRefPtr<FeedbackCallback> read = new FeedbackCallback;
	RemoteRefreshFeedbackTestAccess::ReadSaved(*fixture.handler, fixture.Chat().id, read); fixture.Complete(2); UAM_ASSERT_EQ(read->successes, 1);
	const auto& images = read->body.at("messages").front().at("attachments"); UAM_ASSERT_EQ(images.size(), std::size_t{3});
	const auto primary = ReadFile(fixture.Primary()), summary = ReadFile(fixture.Summary());
	const auto live = fixture.Chat().messages;
	fixture.AwaitRetirement();
	const auto first = fixture.pending.size();
	std::vector<CefRefPtr<FeedbackCallback>> callbacks;
	std::vector<nlohmann::json> payloads;
	for (const auto& image : images)
	{
		payloads.push_back({{"chatId", fixture.Chat().id}, {"messageIndex", 0}, {"historyDigest", image.at("historyDigest")}, {"attachmentId", image.at("id")}});
		callbacks.push_back(new FeedbackCallback);
		RemoteRefreshFeedbackTestAccess::Detail(*fixture.handler, true, payloads.back(), callbacks.back());
	}
	UAM_ASSERT_EQ(fixture.pending.size(), first + 2);
	UAM_ASSERT_EQ(callbacks.back()->failures, 1); UAM_ASSERT_EQ(callbacks.back()->status, 503);
	fixture.Complete(first); fixture.Complete(first + 1); fixture.AwaitRetirement();
	for (std::size_t index = 0; index < 2; ++index) UAM_ASSERT_EQ(callbacks[index]->body.at("available"), nlohmann::json(true));
	CefRefPtr<FeedbackCallback> retry = new FeedbackCallback;
	const auto next = fixture.pending.size();
	RemoteRefreshFeedbackTestAccess::Detail(*fixture.handler, true, payloads.back(), retry); fixture.Complete(next);
	UAM_ASSERT_EQ(retry->successes, 1); UAM_ASSERT_EQ(retry->body.at("available"), nlohmann::json(true));
	UAM_ASSERT_EQ(fixture.Chat().messages, live);
	UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
	UAM_ASSERT(!fs::exists(fixture.Generation() / "published.receipt")); UAM_ASSERT_EQ(fixture.reads->load(), 1);
	UAM_ASSERT_EQ(fixture.app.status_line, kPriorWarning); fixture.AssertDraft();
}

UAM_TEST(RemoteRefreshFeedbackCommittedAckFailureKeepsOriginalWarningAndHistory)
{
	FeedbackFixture fixture; const auto cb = fixture.Start(); fixture.Complete(0); fixture.Complete(1);
	UAM_ASSERT_EQ(cb->successes + cb->failures, 0); UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{3});
	const auto primary = ReadFile(fixture.Primary()), summary = ReadFile(fixture.Summary());
	const auto obstruction = fixture.Generation() / "published.receipt";
	UAM_ASSERT(fs::create_directory(obstruction)); UAM_ASSERT(uam::io::WriteTextFile(obstruction / "keep", "Retained acknowledgement obstruction"));
	fixture.Complete(2); fixture.AssertPublished(cb);
	UAM_ASSERT_EQ(cb->body.at("warning"), nlohmann::json(kCommittedWarning));
	UAM_ASSERT_EQ(fixture.app.status_line, kCommittedWarning);
	UAM_ASSERT_EQ(uam::StateSerializer::Serialize(fixture.app, true).at("statusLine"), nlohmann::json(kCommittedWarning));
	UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
	UAM_ASSERT_EQ(ReadFile(obstruction / "keep"), std::string("Retained acknowledgement obstruction"));
}

UAM_TEST(RemoteRefreshFeedbackHealthyCommitKeepsPriorWarningAndReturnsHistory)
{
	FeedbackFixture fixture; const auto cb = fixture.Start(); fixture.Complete(0); fixture.Complete(1); fixture.Complete(2);
	fixture.AssertPublished(cb); UAM_ASSERT(!cb->body.contains("warning"));
	UAM_ASSERT_EQ(fixture.app.status_line, kPriorWarning);
	UAM_ASSERT(fs::is_regular_file(fixture.Generation() / "published.receipt"));
}

UAM_TEST(RemoteRefreshFeedbackChangedPostcommitOwnerNeverRedrawsOldHistory)
{
	FeedbackFixture fixture; const auto cb = fixture.Start(); fixture.Complete(0); fixture.Complete(1);
	const auto primary = ReadFile(fixture.Primary()), summary = ReadFile(fixture.Summary());
	fixture.Chat().metadata_incarnation = ChatSession{}.metadata_incarnation;
	fixture.Complete(2);
	UAM_ASSERT_EQ(cb->successes, 0); UAM_ASSERT_EQ(cb->failures, 1); UAM_ASSERT_EQ(cb->status, 409);
	UAM_ASSERT(cb->error.find("already saved") != std::string::npos);
	UAM_ASSERT(cb->error.find("Reopen the chat") != std::string::npos);
	UAM_ASSERT(cb->error.find("New native answer") == std::string::npos);
	UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary); UAM_ASSERT_EQ(ReadFile(fixture.Summary()), summary);
	UAM_ASSERT_EQ(fixture.app.status_line, kPriorWarning); fixture.AssertDraft();
}
#endif
