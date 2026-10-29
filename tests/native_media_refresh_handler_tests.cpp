#include "app/managed_parent_continuation_service.h"
#include "common/config/provider_availability.h"
#include "test_harness.h"
#include "app/uam_control_service.h"
#include "cef_handler_fixture.h"
#include "cef/uam_query_handler_async.h"
#include "cef/cef_push.h"
#include "common/chat/native_attachment_assets.h"
#include "common/chat/chat_message_page_index.h"
#include "common/runtime/native_refresh_lifetime.h"
#include "include/cef_api_hash.h"
#include <future>

using namespace uam_test;
using namespace uam::query_handler_async;

struct NativeMediaRefreshHandlerTestAccess
{
	static void Invoke(UamQueryHandler& handler, const std::string& id, CefRefPtr<UamQueryHandler::Callback> cb)
	{
		handler.HandleGetChatMessages(nullptr, {{"chatId", id}, {"refreshNative", true}}, cb);
	}
	static void Dispatch(UamQueryHandler& handler, std::function<bool(CefRefPtr<UamQueryHandler::Callback>, std::function<AsyncCefResult()>, std::function<void(AsyncCefResult&)>)> dispatch)
	{
		handler.m_nativeMediaRefreshDispatcherForTests = dispatch;
		handler.m_attachmentPreviewDispatcherForTests = std::move(dispatch);
	}
	static void Query(UamQueryHandler& handler, int64_t query_id) { handler.m_dispatchingQuery = {-1, "", query_id}; }
	static void Cancel(UamQueryHandler& handler, int64_t query_id) { handler.OnQueryCanceled(nullptr, nullptr, query_id); }
	static void Preview(UamQueryHandler& handler, const std::string& id, const std::string& attachment, CefRefPtr<UamQueryHandler::Callback> cb)
	{
		handler.HandleGetAttachmentImagePreview(nullptr, {{"chatId", id}, {"messageIndex", 0}, {"attachmentId", attachment}}, cb);
	}
	static void Send(UamQueryHandler& handler, const nlohmann::json& payload, CefRefPtr<UamQueryHandler::Callback> cb)
	{
		handler.HandleSendAcpPrompt(nullptr, payload, cb);
	}
	static void Rename(UamQueryHandler& handler, const std::string& id, CefRefPtr<UamQueryHandler::Callback> cb)
	{
		handler.HandleRenameSession(nullptr, {{"chatId", id}, {"title", "Renamed imported transcript"}}, cb);
	}
	static void Pin(UamQueryHandler& handler, const std::string& id, CefRefPtr<UamQueryHandler::Callback> cb)
	{
		handler.HandleSetChatPinned(nullptr, {{"chatId", id}, {"pinned", true}, {"expectedPinned", false}}, cb);
	}
	static void RestartInspectionAction(UamQueryHandler& handler, std::string_view action, const nlohmann::json& payload, CefRefPtr<UamQueryHandler::Callback> cb)
	{
		UAM_ASSERT(action == "listManagedSubagents" || action == "getManagedAgentTranscript" || action == "cancelManagedSubagent");
		UAM_ASSERT(handler.DispatchAction(action, nullptr, payload, cb));
	}
	static bool FollowupAction(UamQueryHandler& handler, std::string_view action, const nlohmann::json& payload, CefRefPtr<UamQueryHandler::Callback> cb)
	{
		UAM_ASSERT(action == "listManagedSubagents" || action == "submitManagedSubagentFollowup");
		return handler.DispatchAction(action, nullptr, payload, cb);
	}
	static bool FollowupQueryAction(UamQueryHandler& handler, int64_t query_id, const nlohmann::json& payload, CefRefPtr<UamQueryHandler::Callback> cb)
	{
		// The metadata-only transfer used by trusted OnQuery; no execution or scope is injected.
		handler.m_managedFollowupQueryIdentities[cb.get()] = UamQueryHandler::ChatIconQueryIdentity{-1, "", query_id};
		const bool handled = handler.DispatchAction("submitManagedSubagentFollowup", nullptr, payload, cb);
		handler.m_managedFollowupQueryIdentities.erase(cb.get());
		return handled;
	}
	static void RestartInspectionDispatch(UamQueryHandler& handler, std::function<bool(CefRefPtr<UamQueryHandler::Callback>, std::function<AsyncCefResult()>, std::function<void(AsyncCefResult&)>)> dispatch)
	{
		handler.m_managedSubagentDispatcherForTests = std::move(dispatch);
	}
	static void QueuedPromptAction(UamQueryHandler& handler, const nlohmann::json& payload, CefRefPtr<UamQueryHandler::Callback> cb)
	{
		UAM_ASSERT(handler.DispatchAction("manageQueuedAcpPrompt", nullptr, payload, cb));
	}
	static void QueuedPromptHistoryDispatch(UamQueryHandler& handler, std::function<bool(CefRefPtr<UamQueryHandler::Callback>, std::function<AsyncCefResult()>, std::function<void(AsyncCefResult&)>)> dispatch)
	{
		handler.m_queuedPromptHistoryDispatcherForTests = std::move(dispatch);
	}

};

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
namespace
{
	class MediaCallback final : public UamQueryHandler::Callback
	{
	  public:
		int successes = 0, failures = 0, status = 0;
		std::string failure;
		nlohmann::json body;
		void Success(const CefString& value) override
		{
			++successes;
			body = nlohmann::json::parse(value.ToString());
		}
		void Success(const void*, size_t) override
		{
			++successes;
		}
		void Failure(int code, const CefString& error) override
		{
			++failures;
			status = code;
			failure = error.ToString();
		}

	  private:
		IMPLEMENT_REFCOUNTING(MediaCallback);
	};
	struct MediaQuery
	{
		CefRefPtr<UamQueryHandler::Callback> cb;
		std::function<AsyncCefResult()> worker;
		std::function<void(AsyncCefResult&)> completion;
		AsyncCefResult Run()
		{
			return std::async(std::launch::async, worker).get();
		}
		void Finish(AsyncCefResult result)
		{
			CompleteAsyncCefQuery(cb, result, completion);
		}
	};
	struct MediaFixture
	{
		TempDir temp{"uam-native-media-refresh-handler"};
		ScopedEnvVar home{"GEMINI_CLI_HOME", (temp.root / "profile").string()};
		uam::AppState app;
		std::unique_ptr<UamQueryHandler> handler;
		std::vector<MediaQuery> pending;
		std::size_t reject_at = std::numeric_limits<std::size_t>::max();
		fs::path native_file;
		MediaFixture()
		{
			app.data_root = temp.root / "data";
			ChatSession chat;
			chat.id = "saved-UAM-media";
			chat.provider_id = "gemini-cli";
			chat.native_session_id = "11111111-1111-4111-8111-111111111111";
			chat.workspace_directory = (temp.root / "workspace").string();
			fs::create_directories(chat.workspace_directory);
			chat.created_at = chat.updated_at = "2026-10-16T12:00:00Z";
			Message row;
			row.role = MessageRole::User;
			row.content = "Recorded text";
			row.created_at = chat.created_at;
			chat.messages.push_back(row);
			app.chats.push_back(chat);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			const auto directory = temp.root / "profile" / ".gemini" / "tmp" / "project" / "chats";
			UAM_ASSERT(uam::io::WriteTextFile(directory.parent_path() / ".project_root", chat.workspace_directory));
			native_file = directory / "session.json";
			nlohmann::json document{{"sessionId", chat.native_session_id}, {"startTime", chat.created_at}, {"lastUpdated", chat.updated_at}, {"messages", nlohmann::json::array({{{"id", "recorded-user"}, {"type", "user"}, {"timestamp", chat.created_at}, {"content", nlohmann::json::array({{{"text", row.content}}, {{"inlineData", {{"mimeType", "image/png"}, {"data", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="}}}}})}}})}};
			UAM_ASSERT(uam::io::WriteTextFile(native_file, document.dump()));
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
			NativeMediaRefreshHandlerTestAccess::Dispatch(*handler,
			                                              [this](auto cb, auto worker, auto completion)
			                                              {
				                                              if (pending.size() == reject_at)
					                                              return false;
				                                              pending.push_back({cb, std::move(worker), std::move(completion)});
				                                              return true;
			                                              });
		}
		~MediaFixture()
		{
			pending.clear();
			handler.reset();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (uam::native_history::detail::refresh_owners.load() != 0 && std::chrono::steady_clock::now() < deadline)
				std::this_thread::yield();
		}
		CefRefPtr<MediaCallback> Start()
		{
			CefRefPtr<MediaCallback> cb = new MediaCallback;
			NativeMediaRefreshHandlerTestAccess::Invoke(*handler, app.chats.front().id, cb);
			return cb;
		}
		MediaQuery Take(std::size_t index)
		{
			return std::move(pending.at(index));
		}
		void Complete(std::size_t index)
		{
			auto query = Take(index);
			query.Finish(query.Run());
		}
		std::string Primary()
		{
			return ReadFile(AppPaths::UamChatFilePath(app.data_root, app.chats.front().id));
		}
	};
} // namespace

UAM_TEST(NativeMediaRefreshHandlerPublishesOnlyAfterBothWorkerPhases)
{
	MediaFixture fixture;
	const auto before = fixture.Primary();
	const auto incarnation = fixture.app.chats.front().metadata_incarnation;
	const auto cb = fixture.Start();
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	fixture.Complete(0);
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{2});
	UAM_ASSERT_EQ(cb->successes, 0);
	UAM_ASSERT_EQ(fixture.Primary(), before);
	fixture.Complete(1);
	UAM_ASSERT_EQ(cb->successes, 1);
	UAM_ASSERT_EQ(cb->failures, 0);
	const auto& current = fixture.app.chats.front();
	UAM_ASSERT_EQ(current.metadata_incarnation, incarnation);
	UAM_ASSERT_EQ(current.messages.front().attachments.size(), std::size_t{1});
	const auto& attachment = current.messages.front().attachments.front();
	UAM_ASSERT_EQ(attachment.source, AttachmentSource::OwnedNativeAsset);
	UAM_ASSERT(!attachment.native_inline_payload);
	UAM_ASSERT(uam::native_assets::ReadOwned(fixture.app.data_root, current.id, attachment.native_asset_key).ok);
	UAM_ASSERT(cb->body.dump().find("iVBOR") == std::string::npos);
}

UAM_TEST(NativeMediaRefreshHandlerCancelledQuerySavesWithoutDeliveryAndNextRequestReusesIt)
{
	MediaFixture fixture;
	(void)uam::PushStateUpdateIfChanged(nullptr, fixture.app);
	NativeMediaRefreshHandlerTestAccess::Query(*fixture.handler, 51);
	const auto cancelled = fixture.Start();
	NativeMediaRefreshHandlerTestAccess::Query(*fixture.handler, -1);
	NativeMediaRefreshHandlerTestAccess::Cancel(*fixture.handler, 51);
	fixture.Complete(0); fixture.Complete(1);
	UAM_ASSERT_EQ(cancelled->successes, 0); UAM_ASSERT_EQ(cancelled->failures, 1); UAM_ASSERT_EQ(cancelled->status, 409);
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, fixture.app)); // The cancelled request published nothing.
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id, true, nullptr, false);
	UAM_ASSERT(saved); UAM_ASSERT_EQ(saved->messages.front().attachments.size(), std::size_t{1});
	UAM_ASSERT_EQ(saved->messages.front().attachments.front().source, AttachmentSource::OwnedNativeAsset);
	const auto next = fixture.Start();
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{2}); // No second native read.
	UAM_ASSERT_EQ(next->successes, 1); UAM_ASSERT_EQ(next->failures, 0);
	UAM_ASSERT_EQ(next->body.at("messages").size(), std::size_t{1});
	UAM_ASSERT(next->body.dump().find("iVBOR") == std::string::npos);
}

UAM_TEST(NativeMediaRefreshHandlerRejectsChangedOwnerBeforePreparation)
{
	for (int change = 0; change < 5; ++change)
	{
		MediaFixture fixture;
		const auto before = fixture.Primary();
		const auto cb = fixture.Start();
		auto query = fixture.Take(0);
		auto result = query.Run();
		auto& current = fixture.app.chats.front();
		if (change == 0)
			current.metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		if (change == 1)
			current.workspace_directory += "/changed";
		if (change == 2)
			current.title = "Unsaved local title";
		if (change == 3)
			fixture.app.pending_chat_save_at_by_chat_id[current.id] = 1;
		if (change == 4)
			fixture.app.resolved_native_sessions_by_chat_id[current.id] = "replacement-session";
		query.Finish(std::move(result));
		UAM_ASSERT_EQ(cb->status, 409);
		UAM_ASSERT_EQ(cb->failures, 1);
		UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
		UAM_ASSERT_EQ(fixture.Primary(), before);
		UAM_ASSERT(!fs::exists(fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory)));
	}
}

UAM_TEST(NativeMediaRefreshHandlerRejectsChangedSummaryBeforePublication)
{
	MediaFixture fixture;
	const auto before = fixture.Primary();
	const auto cb = fixture.Start();
	fixture.Complete(0);
	auto query = fixture.Take(1);
	auto result = query.Run();
	fixture.app.chats.front().last_opened_at = "2026-10-16T13:00:00Z";
	UAM_ASSERT(ChatRepository::SaveLastOpenedAt(fixture.app.data_root, fixture.app.chats.front()));
	query.Finish(std::move(result));
	UAM_ASSERT_EQ(cb->status, 409);
	UAM_ASSERT_EQ(cb->failures, 1);
	UAM_ASSERT_EQ(fixture.Primary(), before);
	UAM_ASSERT(fixture.app.chats.front().messages.front().attachments.empty());
}

UAM_TEST(NativeMediaRefreshHandlerContinuationRejectionRespondsOnce)
{
	MediaFixture fixture;
	const auto before = fixture.Primary();
	fixture.reject_at = 1;
	const auto cb = fixture.Start();
	fixture.Complete(0);
	UAM_ASSERT_EQ(cb->status, 503);
	UAM_ASSERT_EQ(cb->failures, 1);
	UAM_ASSERT_EQ(cb->successes, 0);
	UAM_ASSERT_EQ(fixture.Primary(), before);
}

UAM_TEST(NativeMediaRefreshHandlerDiscardedPreparedCallbackPreservesPrimary)
{
	MediaFixture fixture;
	const auto before = fixture.Primary();
	const auto cb = fixture.Start();
	fixture.Complete(0);
	{
		auto discarded = fixture.Take(1);
		const auto result = discarded.Run();
		UAM_ASSERT(result.ok);
	}
	UAM_ASSERT_EQ(cb->successes, 0);
	UAM_ASSERT_EQ(cb->failures, 0);
	UAM_ASSERT_EQ(fixture.Primary(), before);
	UAM_ASSERT(fs::exists(fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory)));
}

UAM_TEST(NativeMediaRefreshHandlerShortSnapshotNeverPublishesOrAcknowledges)
{
	MediaFixture fixture;
	fixture.app.chats.front().messages.push_back(Message{MessageRole::Assistant, "Retained newer answer"});
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.app.chats.front()));
	const auto before = fixture.Primary();
	const auto cb = fixture.Start();
	fixture.Complete(0);
	UAM_ASSERT_EQ(cb->successes, 1);
	UAM_ASSERT_EQ(cb->failures, 0);
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Primary(), before);
	UAM_ASSERT(!fs::exists(fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory)));
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
UAM_TEST(NativeMediaRefreshHandlerPreservesNewComputerUseState)
{
	MediaFixture fixture;
	auto& current = fixture.app.chats.front();
	current.computer_use_enabled = true;
	current.computer_use_target_id = "old-target";
	const auto cb = fixture.Start();
	fixture.Complete(0);
	auto query = fixture.Take(1);
	auto result = query.Run();
	const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, current.id);
	const auto metadata = ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, current);
	current.computer_use_enabled = false;
	current.computer_use_target_id = "new-target";
	current.computer_use_target_title = "Current target";
	current.computer_use_target_process_id = "current-process";
	current.computer_use_target_input_mode = "current-mode";
	current.computer_use_globally_enabled = false;
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, current.id), revision);
	UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, current), metadata);
	query.Finish(std::move(result));
	UAM_ASSERT(!current.computer_use_enabled);
	UAM_ASSERT(!current.computer_use_globally_enabled);
	UAM_ASSERT_EQ(current.computer_use_target_id, std::string("new-target"));
	UAM_ASSERT_EQ(current.computer_use_target_title, std::string("Current target"));
	UAM_ASSERT_EQ(current.computer_use_target_process_id, std::string("current-process"));
	UAM_ASSERT_EQ(current.computer_use_target_input_mode, std::string("current-mode"));
	UAM_ASSERT(cb->failures == 1 || cb->successes == 1);
}

UAM_TEST(NativeMediaRefreshHandlerPreservesManagedRevisionDuringAdoption)
{
	MediaFixture fixture;
	const auto cb = fixture.Start();
	fixture.Complete(0);
	auto query = fixture.Take(1);
	auto result = query.Run();
	fixture.app.chats.front().managed_subagent_revision = 17;
	query.Finish(std::move(result));
	UAM_ASSERT_EQ(cb->successes, 1);
	UAM_ASSERT_EQ(fixture.app.chats.front().managed_subagent_revision, std::uint64_t{17});
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
UAM_TEST(NativeMediaPreviewHandlerAcceptsBoundedSavedProvenanceIdentity)
{
	MediaFixture fixture;
	const auto refresh = fixture.Start();
	fixture.Complete(0);
	fixture.Complete(1);
	UAM_ASSERT_EQ(refresh->successes, 1);
	auto& current = fixture.app.chats.front();
	auto& attachment = current.messages.front().attachments.front();
	attachment.id = std::string(16384, 'k');
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, current));
	CefRefPtr<MediaCallback> cb = new MediaCallback;
	NativeMediaRefreshHandlerTestAccess::Preview(*fixture.handler, current.id, attachment.id, cb);
	UAM_ASSERT_EQ(cb->failures, 0);
	fixture.Complete(2);
	UAM_ASSERT_EQ(cb->successes, 1);
	UAM_ASSERT(cb->body.value("available", false));
	UAM_ASSERT_EQ(cb->body.value("mimeType", std::string{}), std::string("image/png"));
}

UAM_TEST(NativeMediaPreviewHandlerRejectsChangedRootAndIncarnation)
{
	for (bool change_root : {false, true})
	{
		MediaFixture fixture;
		const auto refresh = fixture.Start();
		fixture.Complete(0);
		fixture.Complete(1);
		UAM_ASSERT_EQ(refresh->successes, 1);
		auto& current = fixture.app.chats.front();
		CefRefPtr<MediaCallback> cb = new MediaCallback;
		NativeMediaRefreshHandlerTestAccess::Preview(*fixture.handler, current.id, current.messages.front().attachments.front().id, cb);
		UAM_ASSERT_EQ(cb->failures, 0);
		auto query = fixture.Take(2);
		auto result = query.Run();
		if (change_root)
			fixture.app.data_root = fixture.temp.root / "replacement-data";
		else
			current.metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		query.Finish(std::move(result));
		UAM_ASSERT_EQ(cb->status, 409);
		UAM_ASSERT_EQ(cb->successes, 0);
	}
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
namespace
{
	struct InputFixture
	{
		TempDir temp{"uam-native-input-handler"};
		uam::AppState app;
		std::unique_ptr<UamQueryHandler> handler;
		uam::AcpSessionState* session = nullptr;
		InputFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
			ChatSession chat;
			chat.id = "input-owner";
			chat.provider_id = "gemini-cli";
			chat.workspace_directory = temp.root.string();
			chat.approval_mode.clear();
			chat.messages.push_back(Message{MessageRole::User, "Existing authored history"});
			app.chats.push_back(chat);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			auto owned = std::make_unique<uam::AcpSessionState>();
			owned->chat_id = chat.id;
			owned->provider_id = chat.provider_id;
			owned->session_id = "fixture-native-session";
			owned->protocol_kind = "gemini-acp";
			owned->current_mode_id = "default";
			// Inject busy runtime state: admission/queue tests need no provider process
			// or input handles, and must never send bytes or start a real CLI.
			owned->running = owned->session_ready = owned->processing = true;
			owned->prompt_request_id = 71;
			session = owned.get();
			app.acp_sessions.push_back(std::move(owned));
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
		}
		~InputFixture()
		{
			handler.reset();
		}
		CefRefPtr<MediaCallback> Send(const nlohmann::json& attachment, const std::string& text = "New authored request", bool steer = false)
		{
			CefRefPtr<MediaCallback> cb = new MediaCallback;
			NativeMediaRefreshHandlerTestAccess::Send(*handler, {{"chatId", app.chats.front().id}, {"text", text}, {"attachments", attachment.is_array() ? attachment : nlohmann::json::array({attachment})}, {"steerNow", steer}}, cb);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (cb->successes + cb->failures == 0 && std::chrono::steady_clock::now() < deadline)
			{
				uam::FlushPendingChatSaves(app);
				std::this_thread::yield();
			}
			return cb;
		}
	};
} // namespace

UAM_TEST(NativeInputHandlerRejectsRecordedUnknownAndMixedMetadataWithoutQueueMutation)
{
	std::vector<nlohmann::json> variants;
	for (const nlohmann::json source : {nlohmann::json("provider_uri"), nlohmann::json("owned_native_asset"), nlohmann::json("native_inline_pending"), nlohmann::json("unknown-source"), nlohmann::json(nullptr), nlohmann::json(7)})
	{
		for (const std::string path : {"", "sentinel.pdf"})
			variants.push_back({{"id", "recorded"}, {"name", "Recorded"}, {"kind", "file"}, {"path", path}, {"source", source}});
	}
	for (const std::string field : {"sourceUri", "originExecutionHostId", "nativePartKey", "nativeToolCallId", "nativeSourceField", "nativeAssetKey", "contentDigest"})
		for (const nlohmann::json value : {nlohmann::json("recorded"), nlohmann::json(false)})
			variants.push_back({{"id", "mixed"}, {"name", "Mixed"}, {"kind", "file"}, {"path", "sentinel.pdf"}, {"source", "legacy_path"}, {field, value}});
	for (const auto& attachment : variants)
	{
		InputFixture fixture;
		const auto before = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.app.chats.front().id));
		const auto messages = fixture.app.chats.front().messages;
		const auto cb = fixture.Send(attachment);
		UAM_ASSERT_EQ(cb->status, 400);
		UAM_ASSERT_EQ(cb->failures, 1);
		UAM_ASSERT_EQ(cb->successes, 0);
		UAM_ASSERT(!cb->failure.empty());
		UAM_ASSERT(fixture.session->queued_user_prompts.empty());
		UAM_ASSERT(fixture.app.chats.front().acp_queued_prompts.empty());
		UAM_ASSERT_EQ(fixture.app.chats.front().messages, messages);
		UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.app.chats.front().id)), before);
		UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	}
}

UAM_TEST(NativeInputHandlerRetainsCleanLegacyAttachmentInBusyQueue)
{
	for (const bool explicit_legacy : {false, true})
	for (const std::string text : {"New authored request", ""})
	for (const std::string kind : {"file", "directory"})
	{
		InputFixture fixture;
		nlohmann::json attachment{{"id", "authored"}, {"name", "Notes"}, {"kind", kind}, {"path", "notes.txt"}};
		if (explicit_legacy)
			attachment["source"] = "legacy_path";
		const auto cb = fixture.Send(attachment, text);
		UAM_ASSERT_EQ(cb->successes, 1);
		UAM_ASSERT_EQ(cb->failures, 0);
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
		const auto& attachments = fixture.session->queued_user_prompts.front().attachments;
		UAM_ASSERT_EQ(attachments.size(), std::size_t{1});
		UAM_ASSERT_EQ(attachments.front().id, std::string("authored"));
		UAM_ASSERT_EQ(attachments.front().path, std::string("notes.txt"));
		UAM_ASSERT_EQ(attachments.front().kind, kind);
		UAM_ASSERT_EQ(attachments.front().source, AttachmentSource::LegacyPath);
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().text, text);
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
		UAM_ASSERT(saved && saved->acp_queued_prompts.size() == 1);
		UAM_ASSERT_EQ(saved->acp_queued_prompts.front().attachments, attachments);
	}
}

UAM_TEST(NativeInputHandlerRejectsPathlessLegacyBeforeTextAdmission)
{
	InputFixture fixture;
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.app.chats.front().id);
	const auto before = ReadFile(primary);
	const auto messages = fixture.app.chats.front().messages;
	const int next_request = fixture.session->next_request_id;
	// Missing/null/non-string/blank paths must not decode into an empty selection.
	const std::vector<nlohmann::json> variants = {
	    nlohmann::json::object(), {{"path", nullptr}}, {{"path", 7}}, {{"path", ""}},
	    {{"path", " \t\r\n "}}, {{"path", std::string("bad\0path", 8)}}};
	for (const auto& fields : variants)
	for (const bool explicit_legacy : {false, true})
	for (const std::string text : {"New authored request", ""})
	for (const bool steer : {false, true})
	for (const bool mixed_selection : {false, true})
	{
		nlohmann::json attachment = fields;
		attachment["id"] = "malformed-legacy";
		attachment["name"] = "Unavailable file";
		attachment["kind"] = "file";
		// Passive preview metadata cannot supply the missing source path.
		attachment["previewOriginPath"] = "notes.txt";
		if (explicit_legacy) attachment["source"] = "legacy_path";
		const nlohmann::json selection = mixed_selection
		    ? nlohmann::json::array({{{"id", "valid"}, {"kind", "file"}, {"path", "notes.txt"}}, attachment}) : attachment;
		const auto cb = fixture.Send(selection, text, steer);
		UAM_ASSERT_EQ(cb->status, 400);
		UAM_ASSERT_EQ(cb->failures, 1);
		UAM_ASSERT_EQ(cb->successes, 0);
		UAM_ASSERT(cb->failure.find("invalid path") != std::string::npos);
		UAM_ASSERT(fixture.session->queued_user_prompts.empty());
		UAM_ASSERT(fixture.app.chats.front().acp_queued_prompts.empty());
		UAM_ASSERT_EQ(fixture.app.chats.front().messages, messages);
		UAM_ASSERT_EQ(fixture.session->next_request_id, next_request);
		UAM_ASSERT_EQ(fixture.session->prompt_request_id, 71);
		UAM_ASSERT_EQ(ReadFile(primary), before);
		UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	}
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
namespace
{
	void PrepareLegacyPassiveMediaChat(MediaFixture& fixture, bool include_messages)
	{
		const auto refreshed = fixture.Start();
		fixture.Complete(0);
		fixture.Complete(1);
		UAM_ASSERT_EQ(refreshed->successes, 1);
		fixture.pending.clear();
		auto document = nlohmann::json::parse(fixture.Primary());
		document["imported_read_only"] = true;
		document["native_session_id"] = "";
		document["workspace_directory"] = "";
		document["acp_queued_prompts"] = nlohmann::json::array({{{"id", "archived-input"}, {"text", "Archived queued input"}, {"attachments", document.at("messages").at(0).at("attachments")}}});
		const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.app.chats.front().id);
		UAM_ASSERT(uam::io::WriteTextFile(primary, document.dump()));
		// Historical passive saves predate the page index and the new archive carrier.
		fs::remove(uam::chat_pages::IndexPath(primary));
		fs::remove(AppPaths::UamChatSummaryFilePath(fixture.app.data_root, fixture.app.chats.front().id));
		auto passive = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id, include_messages);
		UAM_ASSERT(passive && passive->imported_read_only && passive->acp_queued_prompts.empty());
		fixture.app.chats.front() = std::move(*passive);
	}
} // namespace

UAM_TEST(NativePassiveMetadataHandlerMigratesLegacyRenameAndPinWithoutArchiveLoss)
{
	for (const bool include_messages : {false, true})
		for (const bool rename : {false, true})
		{
			MediaFixture fixture;
			PrepareLegacyPassiveMediaChat(fixture, include_messages);
			const auto before = fixture.Primary();
			const auto archive = nlohmann::json::parse(before).at("acp_queued_prompts");
			CefRefPtr<MediaCallback> cb = new MediaCallback;
			const auto id = fixture.app.chats.front().id;
			if (rename)
				NativeMediaRefreshHandlerTestAccess::Rename(*fixture.handler, id, cb);
			else
				NativeMediaRefreshHandlerTestAccess::Pin(*fixture.handler, id, cb);
			UAM_ASSERT_EQ(cb->successes + cb->failures, 0);
			UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
			UAM_ASSERT_EQ(fixture.Primary(), before);
			fixture.Complete(0);
			UAM_ASSERT_EQ(cb->successes, 1);
			UAM_ASSERT_EQ(cb->failures, 0);
			const auto saved = nlohmann::json::parse(fixture.Primary());
			UAM_ASSERT_EQ(saved.at("acp_queued_prompts"), archive);
			const auto loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, id, true);
			UAM_ASSERT(loaded && loaded->imported_read_only && loaded->acp_queued_prompts.empty());
			UAM_ASSERT_EQ(loaded->messages.size(), std::size_t{1});
			UAM_ASSERT_EQ(loaded->messages.front().attachments.size(), std::size_t{1});
			if (rename)
				UAM_ASSERT_EQ(loaded->title, std::string("Renamed imported transcript"));
			else
				UAM_ASSERT(loaded->pinned);
			const auto& part = loaded->messages.front().attachments.front();
			UAM_ASSERT(uam::native_assets::ReadOwned(fixture.app.data_root, id, part.native_asset_key).ok);
			UAM_ASSERT(fixture.app.chats.front().acp_queued_prompts.empty());
		}
}

UAM_TEST(NativePassiveMetadataHandlerRejectsChangedOwnerBeforePublication)
{
	for (int change = 0; change != 5; ++change)
	{
		MediaFixture fixture;
		PrepareLegacyPassiveMediaChat(fixture, false);
		const auto root = fixture.app.data_root;
		const auto id = fixture.app.chats.front().id;
		const auto primary = AppPaths::UamChatFilePath(root, id);
		const auto before = ReadFile(primary);
		CefRefPtr<MediaCallback> cb = new MediaCallback;
		NativeMediaRefreshHandlerTestAccess::Rename(*fixture.handler, id, cb);
		UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
		auto query = fixture.Take(0);
		auto result = query.Run();
		if (change == 0)
			fixture.app.data_root = fixture.temp.root / "replacement-root";
		if (change == 1)
			fixture.app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		if (change == 2)
			fixture.app.chats.front().title = "Newer unsaved title";
		if (change == 3)
		{
			auto opened = ChatRepository::LoadLocalChat(root, id, false);
			UAM_ASSERT(opened);
			opened->last_opened_at = "2026-10-17T14:00:00Z";
			UAM_ASSERT(ChatRepository::SaveLastOpenedAt(root, *opened));
		}
		std::string expected = before;
		if (change == 4)
		{
			auto external = nlohmann::json::parse(before);
			external["title"] = "Newer saved title";
			UAM_ASSERT(uam::io::WriteTextFile(primary, external.dump()));
			expected = ReadFile(primary);
		}
		query.Finish(std::move(result));
		UAM_ASSERT_EQ(cb->successes, 0);
		UAM_ASSERT_EQ(cb->failures, 1);
		UAM_ASSERT_EQ(cb->status, 409);
		UAM_ASSERT_EQ(ReadFile(primary), expected);
	}
}

UAM_TEST(NativePassiveMetadataHandlerRejectedAndDiscardedSavesRemainRetryable)
{
	MediaFixture fixture;
	PrepareLegacyPassiveMediaChat(fixture, false);
	const auto wait_for_cleanup = []()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (uam::native_history::detail::refresh_owners.load() != 0 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		UAM_ASSERT_EQ(uam::native_history::detail::refresh_owners.load(), 0U);
	};
	const auto id = fixture.app.chats.front().id;
	const auto before = fixture.Primary();
	const auto archive = nlohmann::json::parse(before).at("acp_queued_prompts");
	fixture.reject_at = 0;
	CefRefPtr<MediaCallback> rejected = new MediaCallback;
	NativeMediaRefreshHandlerTestAccess::Pin(*fixture.handler, id, rejected);
	UAM_ASSERT_EQ(rejected->failures, 1);
	UAM_ASSERT_EQ(rejected->status, 503);
	UAM_ASSERT_EQ(rejected->successes, 0);
	UAM_ASSERT(fixture.pending.empty());
	UAM_ASSERT_EQ(fixture.Primary(), before);
	UAM_ASSERT(!fixture.app.chats.front().pinned);
	wait_for_cleanup();
	fixture.reject_at = std::numeric_limits<std::size_t>::max();
	CefRefPtr<MediaCallback> discarded = new MediaCallback;
	NativeMediaRefreshHandlerTestAccess::Pin(*fixture.handler, id, discarded);
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	CefRefPtr<MediaCallback> overlapping = new MediaCallback;
	NativeMediaRefreshHandlerTestAccess::Rename(*fixture.handler, id, overlapping);
	UAM_ASSERT_EQ(overlapping->failures, 1);
	UAM_ASSERT_EQ(overlapping->status, 409);
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	{
		auto abandoned = fixture.Take(0);
		UAM_ASSERT(abandoned.Run().ok);
		// The callback task can be discarded after preparation without publishing.
	}
	UAM_ASSERT_EQ(discarded->successes + discarded->failures, 0);
	UAM_ASSERT_EQ(fixture.Primary(), before);
	UAM_ASSERT(!fixture.app.chats.front().pinned);
	wait_for_cleanup();
	CefRefPtr<MediaCallback> retry = new MediaCallback;
	NativeMediaRefreshHandlerTestAccess::Pin(*fixture.handler, id, retry);
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{2});
	fixture.Complete(1);
	UAM_ASSERT_EQ(retry->successes, 1);
	UAM_ASSERT_EQ(retry->failures, 0);
	UAM_ASSERT(fixture.app.chats.front().pinned);
	UAM_ASSERT_EQ(nlohmann::json::parse(fixture.Primary()).at("acp_queued_prompts"), archive);
}

UAM_TEST(NativePassiveMetadataHandlerProductionPostRejectionRepliesAndAllowsRetry)
{
	// This harness deliberately has no CEF runtime: the real production poster
	// must reject, rather than using the high-level fixture dispatcher.
	UAM_ASSERT(cef_api_hash(CEF_API_VERSION, 0) != nullptr);
	UAM_ASSERT(!CefTaskRunner::GetForThread(TID_FILE_BACKGROUND));
	for (const bool rename : {false, true})
	{
		MediaFixture fixture;
		PrepareLegacyPassiveMediaChat(fixture, true);
		const auto id = fixture.app.chats.front().id;
		const auto title = fixture.app.chats.front().title;
		const auto before = fixture.Primary();
		const auto archive = nlohmann::json::parse(before).at("acp_queued_prompts");
		const auto key = fixture.app.chats.front().messages.front().attachments.front().native_asset_key;
		const auto original = uam::native_assets::ReadOwned(fixture.app.data_root, id, key);
		UAM_ASSERT(original.ok);
		NativeMediaRefreshHandlerTestAccess::Dispatch(*fixture.handler, {});
		CefRefPtr<MediaCallback> rejected = new MediaCallback;
		if (rename)
			NativeMediaRefreshHandlerTestAccess::Rename(*fixture.handler, id, rejected);
		else
			NativeMediaRefreshHandlerTestAccess::Pin(*fixture.handler, id, rejected);
		UAM_ASSERT_EQ(rejected->failures, 1);
		UAM_ASSERT_EQ(rejected->status, 503);
		UAM_ASSERT_EQ(rejected->successes, 0);
		UAM_ASSERT(fixture.pending.empty());
		UAM_ASSERT_EQ(fixture.Primary(), before);
		UAM_ASSERT_EQ(fixture.app.chats.front().title, title);
		UAM_ASSERT(!fixture.app.chats.front().pinned);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (uam::native_history::detail::refresh_owners.load() != 0 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		UAM_ASSERT_EQ(uam::native_history::detail::refresh_owners.load(), 0U);
		NativeMediaRefreshHandlerTestAccess::Dispatch(*fixture.handler,
		                                              [&fixture](auto cb, auto worker, auto completion)
		                                              {
			                                              fixture.pending.push_back({cb, std::move(worker), std::move(completion)});
			                                              return true;
		                                              });
		CefRefPtr<MediaCallback> retry = new MediaCallback;
		if (rename)
			NativeMediaRefreshHandlerTestAccess::Rename(*fixture.handler, id, retry);
		else
			NativeMediaRefreshHandlerTestAccess::Pin(*fixture.handler, id, retry);
		UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
		fixture.Complete(0);
		UAM_ASSERT_EQ(retry->successes, 1);
		UAM_ASSERT_EQ(retry->failures, 0);
		UAM_ASSERT_EQ(nlohmann::json::parse(fixture.Primary()).at("acp_queued_prompts"), archive);
		const auto retained = uam::native_assets::ReadOwned(fixture.app.data_root, id, key);
		UAM_ASSERT(retained.ok);
		UAM_ASSERT_EQ(retained.bytes, original.bytes);
	}
}
#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI && !defined(_WIN32)
#include "common/utils/uuid.h"

namespace
{
	struct ManagedInitialFirstSnapshot
	{
		std::optional<ChatSession> root, transcript;
		std::optional<AgentRun> run;
		uam::AgentRunLoadResult ledger;
		nlohmann::json encoded_run;
		std::shared_ptr<const std::uint8_t> root_incarnation, transcript_incarnation;
		std::vector<Message> live_root_messages;
		bool private_cli_invoked = false;
		std::string error;
	};

	class ManagedInitialFirstCallback final : public UamQueryHandler::Callback
	{
	  public:
		int successes = 0, failures = 0;
		nlohmann::json body;
		std::string error;
		std::function<void()> capture;
		void Success(const CefString& value) override
		{
			++successes;
			body = nlohmann::json::parse(value.ToString());
			if (capture)
				capture();
		}
		void Success(const void*, size_t) override
		{
			++successes;
		}
		void Failure(int, const CefString& value) override
		{
			++failures;
			error = value.ToString();
		}

	  private:
		IMPLEMENT_REFCOUNTING(ManagedInitialFirstCallback);
	};

	struct ManagedInitialFirstFixture
	{
		TempDir temp{"uam-managed-initial-first-public"};
		// This private canonical executable is only a fail-closed fallback if a future
		// asynchronous producer needs scheduler pumping. No installed CLI can resolve.
		ScopedEnvVar path{"PATH", (temp.root / "bin").string()};
		uam::AppState app;
		std::unique_ptr<UamQueryHandler> handler;
		std::string root_id;
		uam::AgentDefinition definition;
		std::shared_ptr<const std::uint8_t> root_incarnation;
		std::vector<Message> original_messages;
		ManagedInitialFirstSnapshot at_acceptance;

		fs::path Cli() const
		{
			return temp.root / "bin" / "codex";
		}
		fs::path Invocations() const
		{
			return temp.root / "cli-invocations";
		}
		void ExecutableIsPrivate() const
		{
			UAM_ASSERT(fs::is_regular_file(Cli()) && !fs::is_symlink(Cli()));
			UAM_ASSERT((fs::status(Cli()).permissions() & fs::perms::owner_exec) != fs::perms::none);
			const char* test_path = std::getenv("PATH");
			UAM_ASSERT(test_path != nullptr);
			UAM_ASSERT_EQ(std::string(test_path), (temp.root / "bin").string());
		}

		ManagedInitialFirstFixture()
		{
			UAM_ASSERT(cef_api_hash(CEF_API_VERSION, 0) != nullptr);
			UAM_ASSERT(uam::io::WriteTextFile(Cli(), "#!/bin/sh\nprintf invoked >> " + ShellQuoteForTest(Invocations().string()) + "\nexit 97\n"));
			fs::permissions(Cli(), fs::perms::owner_all, fs::perm_options::replace);
			ExecutableIsPrivate(); // WriteTextFile may atomically replace mode; check AFTER it.
			app.data_root = temp.root / "data";
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
			app.provider_profiles.front().interactive_command = ShellQuoteForTest(Cli().string());
			UAM_ASSERT_EQ(app.provider_profiles.front().id, std::string("codex-cli"));
			UAM_ASSERT(app.provider_profiles.front().supports_structured);
			const fs::path workspace = temp.root / "workspace";
			fs::create_directories(workspace);
			definition.id = "managed-first-reviewer";
			definition.description = "Isolated first managed request";
			definition.mode = "subagent";
			definition.workspace_access = "read";
			definition.instructions = "Review only the original authored task; do not change files.";
			std::string error;
			UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(app.data_root, definition, &error));
			const auto catalog = uam::AgentDefinitionService::Load(app.data_root, workspace);
			const auto saved_definition = std::ranges::find(catalog.definitions, definition.id, &uam::AgentDefinition::id);
			UAM_ASSERT(saved_definition != catalog.definitions.end());
			definition = *saved_definition;
			UAM_ASSERT(!definition.definition_hash.empty() && !definition.markdown_snapshot.empty());
			ChatSession root = ChatDomainService().CreateNewChat({}, "codex-cli");
			root_id = root.id;
			root.execution_host_id = "local";
			root.routing_mode = "explicit";
			root.workspace_directory = workspace.string();
			root.model_id = "captured-requested-model"; // Requested policy, never native attestation.
			ChatDomainService().AddMessage(root, MessageRole::User, "Earlier authored parent request.");
			ChatDomainService().AddMessage(root, MessageRole::Assistant, "Earlier recorded parent answer.");
			UAM_ASSERT(root.metadata_incarnation && root.managed_followup_incarnation.empty());
			UAM_ASSERT(root.native_session_id.empty() && root.agent_run_id.empty());
			UAM_ASSERT(uam::paths::IsControllerLocalWorkspace(root));
			UAM_ASSERT(ProviderResolutionService().ChatProviderIsAvailable(app, root));
			UAM_ASSERT(uam::execution_hosts::Find(app.settings.execution_hosts, root.execution_host_id) != nullptr);
			app.chats.push_back(std::move(root));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			app.chats.front().title = "Saved visible UserRoot";
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			const auto primary = AppPaths::UamChatFilePath(app.data_root, root_id);
			UAM_ASSERT(fs::is_regular_file(primary) && fs::is_regular_file(uam::io::MakeBackupPath(primary)));
			const auto loaded = ChatRepository::LoadLocalChatBounded(app.data_root, root_id, 4 * 1024 * 1024);
			UAM_ASSERT(loaded && loaded->messages == app.chats.front().messages);
			UAM_ASSERT(loaded->managed_followup_incarnation.empty() && loaded->native_session_id.empty());
			root_incarnation = app.chats.front().metadata_incarnation;
			original_messages = app.chats.front().messages;
			UAM_ASSERT(app.agent_runs.empty() && app.queued_agent_run_ids.empty() && app.acp_sessions.empty());
			UAM_ASSERT(uam::AgentRunLedger::LoadAllReadOnly(app.data_root).runs.empty());
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
		}

		~ManagedInitialFirstFixture()
		{
			handler.reset();
			uam::FastStopAcpSessionsForExit(app);
			// app dies before private PATH, executable and temporary data root.
		}

		void CaptureAcceptance()
		{
			try
			{
				at_acceptance.private_cli_invoked = fs::exists(Invocations());
				at_acceptance.root = ChatRepository::LoadLocalChatBounded(app.data_root, root_id, 4 * 1024 * 1024);
				const auto live = std::ranges::find(app.chats, root_id, &ChatSession::id);
				if (live != app.chats.end())
				{
					at_acceptance.root_incarnation = live->metadata_incarnation;
					at_acceptance.live_root_messages = live->messages;
				}
				const auto run = std::ranges::find(app.agent_runs, root_id, &AgentRun::root_chat_id);
				if (run == app.agent_runs.end())
					return;
				at_acceptance.run = *run;
				at_acceptance.transcript = ChatRepository::LoadLocalChatBounded(app.data_root, run->transcript_chat_id, 4 * 1024 * 1024);
				const auto transcript = std::ranges::find(app.chats, run->transcript_chat_id, &ChatSession::id);
				if (transcript != app.chats.end())
					at_acceptance.transcript_incarnation = transcript->metadata_incarnation;
				at_acceptance.ledger = uam::AgentRunLedger::LoadAllReadOnly(app.data_root);
				at_acceptance.encoded_run = nlohmann::json::parse(ReadFile(app.data_root / "agent-runs" / (run->id + ".json")));
			}
			catch (const std::exception& error)
			{
				at_acceptance.error = error.what();
			}
		}
	};
} // namespace

UAM_TEST(ManagedInitialPublicMentionRecordsWitnessedBirthAndInitialReceipt)
{
	ManagedInitialFirstFixture fixture;
	const std::string text = "@managed-first-reviewer inspect the original saved parent policy";
	CefRefPtr<ManagedInitialFirstCallback> callback = new ManagedInitialFirstCallback;
	callback->capture = [&fixture] { fixture.CaptureAcceptance(); };
	fixture.ExecutableIsPrivate();
	NativeMediaRefreshHandlerTestAccess::Send(*fixture.handler, {{"chatId", fixture.root_id}, {"text", text}}, callback);
	// No immediate-response requirement: a future connected producer may defer.
	// Current legacy mention replies synchronously, so this loop is NOT entered.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (callback->successes + callback->failures == 0 && std::chrono::steady_clock::now() < deadline)
	{
		fixture.ExecutableIsPrivate(); // Before EVERY possible launch/owner pump.
		(void)uam::AgentRunScheduler::Poll(fixture.app);
		(void)uam::FlushPendingChatSaves(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (callback->failures != 0)
		throw TestFailure("Managed mention CEF prerequisite: " + callback->error.substr(0, 4096));
	UAM_ASSERT_EQ(callback->failures, 0);
	UAM_ASSERT_EQ(callback->successes, 1);
	const auto& saved = fixture.at_acceptance;
	UAM_ASSERT(saved.error.empty() && saved.root && saved.run && saved.transcript);
	UAM_ASSERT(!saved.private_cli_invoked); // No provider observation/launch is a birth prerequisite.
	UAM_ASSERT_EQ(saved.root_incarnation, fixture.root_incarnation);
	UAM_ASSERT_EQ(saved.root->messages.size(), fixture.original_messages.size() + 1);
	UAM_ASSERT(std::equal(fixture.original_messages.begin(), fixture.original_messages.end(), saved.root->messages.begin()));
	UAM_ASSERT_EQ(saved.root->messages.back().role, MessageRole::User);
	UAM_ASSERT_EQ(saved.root->messages.back().content, text);
	UAM_ASSERT(saved.root->messages.back().managed_origin_run_id == std::optional<std::string>{saved.run->id});
	UAM_ASSERT_EQ(std::ranges::count(saved.root->messages, text, &Message::content), 1);
	UAM_ASSERT_EQ(saved.live_root_messages, saved.root->messages);
	UAM_ASSERT_EQ(saved.run->definition_hash, fixture.definition.definition_hash);
	UAM_ASSERT_EQ(saved.run->definition_snapshot, fixture.definition.markdown_snapshot);
	UAM_ASSERT_EQ(saved.run->provider_id, std::string("codex-cli"));
	UAM_ASSERT_EQ(saved.run->model_id, saved.root->model_id);
	UAM_ASSERT_EQ(saved.run->effective_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(saved.transcript->execution_host_id, saved.root->execution_host_id);
	UAM_ASSERT_EQ(saved.transcript->workspace_directory, saved.root->workspace_directory);
	UAM_ASSERT(saved.transcript->native_session_id.empty());
	UAM_ASSERT_EQ(saved.ledger.runs.size(), std::size_t{1});
	UAM_ASSERT(saved.ledger.errors.empty());
	UAM_ASSERT_EQ(saved.ledger.runs.front().id, saved.run->id);

	// FIRST intended feature RED: today's real CEF Send returns {}, after all
	// saved-root/definition/policy/row/legacy-ledger prerequisites above pass.
	UAM_ASSERT(callback->body.contains("managedMention"));
	const auto& receipt = callback->body.at("managedMention");
	UAM_ASSERT(receipt.is_object() && receipt.at("inputRecorded").get<bool>());
	UAM_ASSERT_EQ(receipt.at("delegationState").get<std::string>(), std::string("committed"));
	UAM_ASSERT_EQ(receipt.at("parentMessageIndex").get<std::size_t>(), fixture.original_messages.size());
	UAM_ASSERT_EQ(receipt.at("parentMessage").at("content").get<std::string>(), text);
	UAM_ASSERT_EQ(receipt.at("parentMessage").at("managedOriginRunId").get<std::string>(), saved.run->id);
	UAM_ASSERT(uam::uuid::IsCanonicalUuid(saved.root->managed_followup_incarnation));
	UAM_ASSERT(uam::uuid::IsCanonicalUuid(saved.transcript->managed_followup_incarnation));
	UAM_ASSERT(saved.root->managed_followup_incarnation != saved.transcript->managed_followup_incarnation);
	UAM_ASSERT(saved.transcript_incarnation && saved.transcript_incarnation != fixture.root_incarnation);
	UAM_ASSERT_EQ(saved.transcript->agent_run_id, saved.run->id);
	UAM_ASSERT_EQ(saved.encoded_run.at("version").get<int>(), 2);
	UAM_ASSERT(saved.run->managed_continuation.has_value());
	UAM_ASSERT_EQ(saved.ledger.runs.front().managed_continuation, saved.run->managed_continuation);
	const auto& managed = *saved.run->managed_continuation;
	UAM_ASSERT_EQ(managed.owner.root_birth_id, saved.root->managed_followup_incarnation);
	UAM_ASSERT_EQ(managed.owner.transcript_birth_id, saved.transcript->managed_followup_incarnation);
	UAM_ASSERT(!managed.owner.delegation_grant && !managed.execution);
	UAM_ASSERT_EQ(managed.state_revision, std::uint64_t{1});
	UAM_ASSERT_EQ(managed.current_turn_id, saved.run->id);
	UAM_ASSERT_EQ(managed.current_turn_number, std::uint32_t{1});
	UAM_ASSERT_EQ(managed.turns.size(), std::size_t{1});
	const auto& turn = managed.turns.front();
	UAM_ASSERT_EQ(turn.turn_id, saved.run->id);
	UAM_ASSERT_EQ(turn.delivery_id, saved.run->id);
	UAM_ASSERT_EQ(turn.status, uam::ManagedRunTurnStatus::Queued);
	UAM_ASSERT_EQ(turn.principal.kind, uam::ManagedRunPrincipalKind::UserRoot);
	UAM_ASSERT_EQ(turn.principal.root_chat_id, fixture.root_id);
	UAM_ASSERT_EQ(turn.principal.root_birth_id, managed.owner.root_birth_id);
	UAM_ASSERT(turn.principal.parent_run_id.empty() && turn.principal.parent_transcript_birth_id.empty());
	UAM_ASSERT(turn.client_request_id.empty() && turn.after_turn_id.empty() && turn.text.empty());
	UAM_ASSERT(!turn.dispatch_attempted && turn.expected_turn_serial == 0 && turn.first_user_message_index == -1);
	UAM_ASSERT(turn.result_required && saved.run->deliver_result_to_root_chat);
	UAM_ASSERT(!turn.result_delivered && !saved.run->root_result_delivered);
	UAM_ASSERT_EQ(turn.result_delivery_attempts, 0);
}
UAM_TEST(ManagedInitialOriginLaterPublicationRefusalsNeverCreateChildAuthority)
{
	for (const bool refuse_transcript : {true, false})
	{
		ManagedInitialFirstFixture fixture;
		std::optional<uam::ManagedInitialMentionResult> result;
		std::string origin;
		bool injected = false;
		const std::string text = "@managed-first-reviewer keep the authored request after publication refusal";
		UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(fixture.app, fixture.root_id, text,
		    [&](const auto& value) { result = value; return true; }, nullptr,
		    [&](ChatPublicationFile file, uam::io::AtomicWriteStage stage)
		    {
			if (!injected && file == ChatPublicationFile::Primary && stage == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync)
			{
				const auto saved = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id)));
				origin = saved.at("messages").back().at("managed_origin_run_id").get<std::string>();
				UAM_ASSERT(uam::uuid::IsCanonicalUuid(origin));
				fs::path blocked = fixture.app.data_root / "agent-runs" / (origin + ".json");
				if (refuse_transcript)
				{
					// Read the actual already staged ledger; never invent a transcript identity.
					std::string transcript;
					for (const auto& entry : fs::directory_iterator(blocked.parent_path()))
					{
						if (!entry.is_regular_file()) continue;
						const auto staged = nlohmann::json::parse(ReadFile(entry.path()), nullptr, false);
						if (staged.is_object() && staged.value("id", std::string{}) == origin)
							transcript = staged.value("transcriptChatId", std::string{});
					}
					UAM_ASSERT(!transcript.empty());
					blocked = AppPaths::UamChatFilePath(fixture.app.data_root, transcript);
				}
				UAM_ASSERT(!fs::exists(blocked));
				fs::create_directory(blocked); // Occupied destination forces later publication refusal.
				injected = true;
			}
			return true;
		    }), uam::ManagedInitialMentionAdmission::Pending);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!result && std::chrono::steady_clock::now() < deadline)
		{
			fixture.ExecutableIsPrivate();
			(void)uam::AgentRunScheduler::Poll(fixture.app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT(injected && result && result->input_recorded);
		UAM_ASSERT_EQ(result->delegation_state, uam::ManagedInitialDelegationState::Rejected);
		UAM_ASSERT(result->run_id.empty() && result->transcript_chat_id.empty());
		UAM_ASSERT(result->parent_message && result->parent_message->managed_origin_run_id == std::optional<std::string>{origin});
		UAM_ASSERT_EQ(result->parent_message->role, MessageRole::User);
		UAM_ASSERT_EQ(result->parent_message->content, text);
		UAM_ASSERT_EQ(result->parent_message_index, fixture.original_messages.size());
		UAM_ASSERT(fixture.app.agent_runs.empty() && fixture.app.queued_agent_run_ids.empty() && fixture.app.acp_sessions.empty());
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
		UAM_ASSERT(!fs::exists(fixture.Invocations()));
	}
}

#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI && !defined(_WIN32)
UAM_TEST(ManagedMentionPreservesParentAcrossChatCatalogueGrowth)
{
	for (const bool reserve_catalogue : {true, false})
	{
		const std::string arm = reserve_catalogue ? "reserved-capacity control" : "forced catalogue growth";
		std::cout << "Managed mention parent arm: " << arm << '\n';
		ManagedInitialFirstFixture fixture;
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
		if (reserve_catalogue)
			fixture.app.chats.reserve(2);
		else
			UAM_ASSERT_EQ(fixture.app.chats.capacity(), fixture.app.chats.size());
		const auto before_data = reinterpret_cast<std::uintptr_t>(fixture.app.chats.data());
		const ChatSession before_root = fixture.app.chats.front();
		const std::string text = "@managed-first-reviewer retain this exact parent across catalogue growth";
		CefRefPtr<ManagedInitialFirstCallback> callback = new ManagedInitialFirstCallback;
		callback->capture = [&fixture] { fixture.CaptureAcceptance(); };
		fixture.ExecutableIsPrivate();
		NativeMediaRefreshHandlerTestAccess::Send(*fixture.handler, {{"chatId", fixture.root_id}, {"text", text}}, callback);
		// Witnessed creation acknowledges asynchronously after its actual publication barrier.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (callback->successes + callback->failures == 0 && std::chrono::steady_clock::now() < deadline)
		{
			fixture.ExecutableIsPrivate();
			(void)uam::AgentRunScheduler::Poll(fixture.app);
			(void)uam::FlushPendingChatSaves(fixture.app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const auto after_data = reinterpret_cast<std::uintptr_t>(fixture.app.chats.data());
		if (reserve_catalogue)
			UAM_ASSERT_EQ(after_data, before_data);
		else
			UAM_ASSERT(after_data != before_data);
		// Prove the actual root survives even if the callback says it disappeared.
		const auto root = std::ranges::find(fixture.app.chats, fixture.root_id, &ChatSession::id);
		UAM_ASSERT(root != fixture.app.chats.end());
		UAM_ASSERT_EQ(root->id, fixture.root_id);
		UAM_ASSERT_EQ(root->metadata_incarnation, fixture.root_incarnation);
		UAM_ASSERT(!fs::exists(fixture.Invocations()));
		UAM_ASSERT(fixture.app.acp_sessions.empty());
		if (callback->failures != 0)
			throw TestFailure("Managed mention " + arm + " CEF failure: " + callback->error.substr(0, 300));
		UAM_ASSERT_EQ(callback->failures, 0);
		UAM_ASSERT_EQ(callback->successes, 1);
		const auto& saved = fixture.at_acceptance;
		UAM_ASSERT(saved.error.empty() && saved.root && saved.run && saved.transcript);
		UAM_ASSERT(!saved.private_cli_invoked);
		UAM_ASSERT_EQ(saved.root_incarnation, fixture.root_incarnation);
		UAM_ASSERT_EQ(root->messages.size(), fixture.original_messages.size() + 1);
		UAM_ASSERT(std::equal(fixture.original_messages.begin(), fixture.original_messages.end(), root->messages.begin()));
		UAM_ASSERT_EQ(root->messages.back().role, MessageRole::User);
		UAM_ASSERT_EQ(root->messages.back().content, text);
		UAM_ASSERT_EQ(std::ranges::count(root->messages, text, &Message::content), 1);
		UAM_ASSERT_EQ(saved.root->messages, root->messages);
		UAM_ASSERT_EQ(saved.live_root_messages, root->messages);
		UAM_ASSERT_EQ(saved.root->id, fixture.root_id);
		UAM_ASSERT_EQ(saved.root->provider_id, before_root.provider_id);
		UAM_ASSERT_EQ(saved.root->execution_host_id, before_root.execution_host_id);
		UAM_ASSERT_EQ(saved.root->workspace_directory, before_root.workspace_directory);
		UAM_ASSERT_EQ(saved.root->model_id, before_root.model_id);
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{2});
		UAM_ASSERT_EQ(fixture.app.agent_runs.size(), std::size_t{1});
		UAM_ASSERT_EQ(fixture.app.queued_agent_run_ids.size(), std::size_t{1});
		UAM_ASSERT_EQ(fixture.app.queued_agent_run_ids.front(), saved.run->id);
		UAM_ASSERT_EQ(saved.run->root_chat_id, fixture.root_id);
		UAM_ASSERT_EQ(saved.run->parent_run_id, std::string{});
		UAM_ASSERT_EQ(saved.run->status, std::string("queued"));
		UAM_ASSERT_EQ(saved.run->task, std::string("retain this exact parent across catalogue growth"));
		UAM_ASSERT_EQ(saved.run->definition_hash, fixture.definition.definition_hash);
		UAM_ASSERT_EQ(saved.run->definition_snapshot, fixture.definition.markdown_snapshot);
		UAM_ASSERT_EQ(saved.run->definition_instructions, fixture.definition.instructions);
		UAM_ASSERT_EQ(saved.run->skills_snapshot, fixture.definition.skills);
		UAM_ASSERT_EQ(saved.run->delegates_snapshot, fixture.definition.delegates);
		UAM_ASSERT_EQ(saved.run->provider_id, before_root.provider_id);
		UAM_ASSERT_EQ(saved.run->model_id, before_root.model_id);
		UAM_ASSERT_EQ(saved.run->effective_workspace_access, std::string("read"));
		UAM_ASSERT_EQ(saved.transcript->agent_run_id, saved.run->id);
		UAM_ASSERT_EQ(saved.transcript->id, saved.run->transcript_chat_id);
		UAM_ASSERT_EQ(saved.transcript->execution_host_id, before_root.execution_host_id);
		UAM_ASSERT_EQ(saved.transcript->workspace_directory, before_root.workspace_directory);
		UAM_ASSERT_EQ(saved.transcript->model_id, before_root.model_id);
		UAM_ASSERT_EQ(saved.transcript->approval_mode, std::string(uam::approval_modes::kPlanApprovalMode));
		UAM_ASSERT(saved.transcript->native_session_id.empty() && saved.transcript->messages.empty());
		UAM_ASSERT_EQ(saved.ledger.runs.size(), std::size_t{1});
		UAM_ASSERT(saved.ledger.errors.empty());
		UAM_ASSERT_EQ(saved.ledger.runs.front().id, saved.run->id);
		UAM_ASSERT_EQ(saved.ledger.runs.front().root_chat_id, fixture.root_id);
		UAM_ASSERT_EQ(saved.ledger.runs.front().transcript_chat_id, saved.run->transcript_chat_id);
		std::cout << "Managed mention parent arm passed: " << arm << '\n';
	}
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
#include "app/managed_subagent_followup_service.h"
#include "common/utils/uuid.h"
#include <csignal>
#include <set>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
	class ManagedClaudeLifecycleCallback final : public UamQueryHandler::Callback
	{
	  public:
		int successes = 0, failures = 0;
		nlohmann::json body;
		std::string error;
		void Success(const CefString& value) override
		{
			++successes;
			body = nlohmann::json::parse(value.ToString());
		}
		void Success(const void*, size_t) override
		{
			++successes;
		}
		void Failure(int, const CefString& value) override
		{
			++failures;
			error = value.ToString();
		}

	  private:
		IMPLEMENT_REFCOUNTING(ManagedClaudeLifecycleCallback);
	};

	const char* ManagedClaudeLifecyclePeer()
	{
		return R"UAM_PEER(import json, os, pathlib, sys, time, uuid
root = pathlib.Path(__file__).resolve().parent
config = json.loads((root / 'peer-config.json').read_text())
data = pathlib.Path(config['data_root'])

def store(name, value):
    target = root / name
    temporary = root / (name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, sort_keys=True)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(target)

def event(kind, **fields):
    with (root / 'peer-events.jsonl').open('a') as stream:
        stream.write(json.dumps(dict(kind=kind, **fields), sort_keys=True) + '\n')
        stream.flush()
        os.fsync(stream.fileno())

def read_document(path):
    assert path.is_file() and not path.is_symlink() and path.stat().st_size <= 4 * 1024 * 1024
    return json.loads(path.read_text())

def state():
    runs = [read_document(path) for path in (data / 'agent-runs').glob('*.json')]
    runs = [run for run in runs if run['rootChatId'] == config['root_id']]
    assert len(runs) == 1, 'one actual private ledger must own this root'
    run = runs[0]
    assert run['version'] == 2, 'native execution requires the witnessed v2 producer'
    managed = run['managedContinuation']
    assert managed['currentTurnId'] == run['id'] and managed['currentTurnNumber'] == 1
    assert len(managed['turns']) == 1
    turn = managed['turns'][0]
    assert turn['status'] == run['status'] == 'running' and turn['dispatchAttempted']
    transcript = read_document(data / 'chats' / (run['transcriptChatId'] + '.json'))
    parent = read_document(data / 'chats' / (run['rootChatId'] + '.json'))
    assert transcript['agent_run_id'] == run['id']
    assert parent['managed_followup_incarnation'] == managed['owner']['rootBirthId']
    assert transcript['managed_followup_incarnation'] == managed['owner']['transcriptBirthId']
    return run, turn, transcript

def emit(value):
    print(json.dumps(value), flush=True)

try:
    if sys.argv[1:] == ['--version']:
        event('version', executable=sys.argv[0], pid=os.getpid())
        print('2.1.31 (Claude Code)', flush=True)
        sys.exit(0)
    args = sys.argv[1:]
    assert '-p' in args and args[args.index('--input-format') + 1] == 'stream-json'
    assert args[args.index('--output-format') + 1] == 'stream-json'
    assert args[args.index('--model') + 1] == config['model']
    assert args[args.index('--permission-mode') + 1] == 'plan'
    assert '--resume' not in args and '--mcp-config' not in args
    run, turn, transcript = state()
    store('launch-witness.json', dict(pid=os.getpid(), args=args, run=run))
    event('launch', pid=os.getpid())
    for line in sys.stdin:
        message = json.loads(line)
        if message.get('type') == 'control_request':
            subtype = message['request']['subtype']
            assert subtype == 'initialize', 'unexpected setup/control request: ' + subtype
            event('initialize', request_id=message['request_id'])
            emit(dict(type='control_response', response=dict(subtype='success',
                request_id=message['request_id'], response=dict(commands=[], models=[]))))
            continue
        assert message.get('type') == 'user', 'unexpected input envelope'
        with (root / 'task-inputs.jsonl').open('a') as stream:
            stream.write(json.dumps(message, sort_keys=True) + '\n')
            stream.flush()
            os.fsync(stream.fileno())
        run, turn, transcript = state()
        assert transcript.get('native_session_id', '') == ''
        assert run['managedContinuation']['execution'] is None
        assert turn['expectedTurnSerial'] > 0 and run['expectedTurnSerial'] == turn['expectedTurnSerial']
        index = turn['firstUserMessageIndex']
        assert index >= 0
        row = transcript['messages'][index]
        assert row['role'] == 'user' and row['content'] == config['task']
        assert row['created_at'] == turn['userMessageCreatedAt'] and row['created_at']
        assert not row.get('acp_prompt_not_sent', False), 'input preceded its persisted delivery intent'
        parts = message['message']['content']
        text = parts if isinstance(parts, str) else ''.join(part.get('text', '') for part in parts)
        assert text.count(config['task']) == 1
        for prompt in transcript.get('acp_queued_prompts', []):
            if prompt.get('managed_turn') is not None:
                assert prompt['managed_turn']['expected_native_session_id'], 'invalid empty-native managed_turn'
        store('input-witness.json', dict(pid=os.getpid(), run=run, turn=turn,
            transcript=transcript, message=message))
        event('task-input', pid=os.getpid())
        deadline = time.monotonic() + 45
        while not (root / 'allow-completion').exists():
            assert time.monotonic() < deadline, 'test did not release completion'
            time.sleep(0.005)
        native = str(uuid.uuid4())
        store('native-witness.json', dict(native_id=native, model_included=config['include_model']))
        init = dict(type='system', subtype='init', session_id=native, permissionMode='plan')
        if config['include_model']:
            init['model'] = config['model']
        emit(init)
        emit(dict(type='assistant', session_id=native, message=dict(id=str(uuid.uuid4()), role='assistant',
            content=[dict(type='text', text=config['answer'])])))
        emit(dict(type='result', subtype='success', is_error=False, session_id=native,
            result=config['answer'], duration_ms=1, num_turns=1))
        event('completion', native_id=native)
    event('eof', pid=os.getpid())
except Exception as error:
    store('peer-error.json', dict(error=str(error)))
    sys.exit(70)
)UAM_PEER";
	}

	struct ManagedClaudeLifecycleFixture
	{
		TempDir temp{"uam-managed-initial-claude-public"};
		ScopedEnvVar private_path{"PATH", (temp.root / "bin").string()};
		uam::AppState app;
		std::unique_ptr<UamQueryHandler> handler;
		std::string root_id, run_id;
		const std::string task = "Inspect the saved parent policy and return the private lifecycle answer.";
		const std::string answer = "PRIVATE-MANAGED-INITIAL-CLAUDE-COMPLETE";
		const std::string model = "private-configured-claude-model";
		uam::AgentDefinition definition;
		std::vector<Message> original;
		std::shared_ptr<const std::uint8_t> root_incarnation;
		std::set<pid_t> owned_pids;
		fs::path Executable() const
		{
			return temp.root / "bin" / "claude";
		}
		void AssertPeerFiles() const
		{
			for (const auto& file : {Executable(), temp.root / "peer.py"})
			{
				UAM_ASSERT(fs::is_regular_file(file) && !fs::is_symlink(file));
				UAM_ASSERT((fs::status(file).permissions() & fs::perms::owner_exec) != fs::perms::none);
			}
			const char* value = std::getenv("PATH");
			UAM_ASSERT(value && std::string(value) == (temp.root / "bin").string());
		}
		ManagedClaudeLifecycleFixture(bool include_model)
		{
			UAM_ASSERT(cef_api_hash(CEF_API_VERSION, 0) != nullptr);
			UAM_ASSERT(fs::is_regular_file("/usr/bin/python3"));
			const auto write_executable = [&](const fs::path& path, const std::string& text)
			{
				UAM_ASSERT(uam::io::WriteTextFile(path, text));
				fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
				UAM_ASSERT(fs::is_regular_file(path) && !fs::is_symlink(path));
				UAM_ASSERT((fs::status(path).permissions() & fs::perms::owner_exec) != fs::perms::none);
			};
			write_executable(temp.root / "peer.py", ManagedClaudeLifecyclePeer());
			write_executable(Executable(), "#!/bin/sh\nexec /usr/bin/python3 -I -S " + ShellQuoteForTest((temp.root / "peer.py").string()) + " \"$@\"\n");
			AssertPeerFiles();
			app.data_root = temp.root / "data";
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.settings.central_provider_configuration.enabled = false;
			app.settings.memory_enabled_default = false;
			app.settings.memory_level_default = "off";
			app.settings.memory_recall_budget_bytes = 0;
			app.settings.computer_use_globally_enabled = false; // This private peer deliberately exposes no MCP tools.
			app.provider_profiles = {ProviderProfileStore::DefaultClaudeProfile()};
			app.provider_profiles.front().interactive_command = ShellQuoteForTest(Executable().string());
			const auto workspace = temp.root / "workspace";
			fs::create_directories(workspace);
			definition.id = "private-stream-reviewer";
			definition.description = "Private native lifecycle peer";
			definition.mode = "subagent";
			definition.workspace_access = "read";
			definition.instructions = "Read only; answer the original task without using tools or delegates.";
			std::string error;
			UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(app.data_root, definition, &error));
			const auto catalog = uam::AgentDefinitionService::Load(app.data_root, workspace);
			const auto loaded = std::ranges::find(catalog.definitions, definition.id, &uam::AgentDefinition::id);
			UAM_ASSERT(loaded != catalog.definitions.end());
			definition = *loaded;
			ChatSession root = ChatDomainService().CreateNewChat({}, "claude-cli");
			root_id = root.id;
			root.execution_host_id = "local";
			root.routing_mode = "explicit";
			root.workspace_directory = workspace.string();
			root.model_id = model;
			root.reasoning_effort.clear();
			root.service_tier.clear();
			root.service_tier_explicit = false;
			root.memory_enabled = false;
			root.memory_level = "off";
			root.uam_control_enabled = false;
			root.uam_control_explicitly_disabled = true;
			ChatDomainService().AddMessage(root, MessageRole::User, "Original parent request remains separate.");
			ChatDomainService().AddMessage(root, MessageRole::Assistant, "Older parent answer must not become child output.");
			UAM_ASSERT(root.metadata_incarnation && root.managed_followup_incarnation.empty() && root.native_session_id.empty());
			UAM_ASSERT(ProviderResolutionService().ChatProviderIsAvailable(app, root));
			UAM_ASSERT(uam::paths::IsControllerLocalWorkspace(root));
			root_incarnation = root.metadata_incarnation;
			original = root.messages;
			app.chats.push_back(std::move(root));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			app.chats.front().title = "Saved private Claude UserRoot";
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			const auto saved = ChatRepository::LoadLocalChatBounded(app.data_root, root_id, 4 * 1024 * 1024);
			UAM_ASSERT(saved && saved->messages == original && saved->managed_followup_incarnation.empty());
			UAM_ASSERT(fs::is_regular_file(uam::io::MakeBackupPath(AppPaths::UamChatFilePath(app.data_root, root_id))));
			UAM_ASSERT(app.agent_runs.empty() && app.acp_sessions.empty());
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer-config.json", nlohmann::json{{"data_root", app.data_root.string()}, {"root_id", root_id}, {"task", task}, {"answer", answer}, {"model", model}, {"include_model", include_model}}.dump()));
			AssertPeerFiles();
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
		}
		void ObserveProcesses()
		{
			for (const auto& session : app.acp_sessions)
				if (session && session->provider_id == "claude-cli" && session->child_pid > 0)
					owned_pids.insert(session->child_pid);
		}
		void Pulse()
		{
			AssertPeerFiles();
			ObserveProcesses();
			(void)uam::PollAllAcpSessions(app);
			(void)uam::AgentRunScheduler::Poll(app);
			(void)uam::FlushPendingChatSaves(app);
			ObserveProcesses();
			if (fs::exists(temp.root / "peer-error.json"))
				throw TestFailure("Private Claude startup/input boundary: " + ReadFile(temp.root / "peer-error.json").substr(0, 512));
		}
		template <typename Predicate> void Await(Predicate done)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!done() && std::chrono::steady_clock::now() < deadline)
			{
				Pulse();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(done());
		}
		AgentRun ReadRun() const
		{
			const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(app.data_root);
			UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1);
			return ledger.runs.front();
		}
		bool StopAndReap()
		{
			ObserveProcesses();
			if (handler)
				handler.reset();
			uam::FastStopAcpSessionsForExit(app);
			auto& process = PlatformServicesFactory::Instance().process_service;
			for (const auto& session : app.acp_sessions)
				if (session)
				{
					process.StopStdioProcess(*session, true);
					process.CloseStdioProcessHandles(*session);
				}
			uam::ResetAsyncCommandTask(app.runtime_cli_version_check_task);
			bool reaped = true;
			for (const pid_t pid : owned_pids)
			{
				int status = 0;
				errno = 0;
				const pid_t result = waitpid(pid, &status, WNOHANG);
				if (result == pid)
					continue;
				if (result < 0 && errno == ECHILD)
				{
					errno = 0;
					if (kill(pid, 0) < 0 && errno == ESRCH)
						continue;
				}
				reaped = false;
			}
			return reaped;
		}
		~ManagedClaudeLifecycleFixture()
		{
			if (!StopAndReap())
				std::cerr << "Private Claude cleanup could not confirm reaping\n";
		}
	};
} // namespace

UAM_TEST(ManagedInitialClaudePeerCompletesOneOwnedNativeLifecycle)
{
	for (const bool include_model : {true, false})
	{
		ManagedClaudeLifecycleFixture fixture(include_model);
		const std::string text = "@private-stream-reviewer " + fixture.task;
		CefRefPtr<ManagedClaudeLifecycleCallback> callback = new ManagedClaudeLifecycleCallback;
		fixture.AssertPeerFiles();
		NativeMediaRefreshHandlerTestAccess::Send(*fixture.handler, {{"chatId", fixture.root_id}, {"text", text}}, callback);
		fixture.Await([&] { return callback->successes + callback->failures != 0; });
		if (callback->failures)
			throw TestFailure("Managed Claude producer prerequisite: " + callback->error.substr(0, 512));
		UAM_ASSERT_EQ(callback->successes, 1);
		// CURRENT intended RED. Native process/protocol assertions below are initially unexercised.
		UAM_ASSERT(callback->body.contains("managedMention"));
		const auto& receipt = callback->body.at("managedMention");
		UAM_ASSERT(receipt.at("inputRecorded").get<bool>());
		UAM_ASSERT_EQ(receipt.at("delegationState").get<std::string>(), std::string("committed"));
		UAM_ASSERT_EQ(receipt.at("parentMessageIndex").get<std::size_t>(), fixture.original.size());
		UAM_ASSERT_EQ(receipt.at("parentMessage").at("content").get<std::string>(), text);
		const AgentRun initial = fixture.ReadRun();
		fixture.run_id = initial.id;
		UAM_ASSERT_EQ(receipt.at("runId").get<std::string>(), initial.id);
		UAM_ASSERT_EQ(receipt.at("transcriptChatId").get<std::string>(), initial.transcript_chat_id);
		UAM_ASSERT(initial.managed_continuation && initial.managed_continuation->turns.size() == 1);
		UAM_ASSERT_EQ(initial.root_chat_id, fixture.root_id);
		UAM_ASSERT(initial.parent_run_id.empty() && initial.depth == 1);
		UAM_ASSERT_EQ(initial.agent_id, fixture.definition.id);
		UAM_ASSERT_EQ(initial.task, fixture.task);
		UAM_ASSERT_EQ(initial.definition_snapshot, fixture.definition.markdown_snapshot);
		UAM_ASSERT_EQ(initial.definition_hash, fixture.definition.definition_hash);
		UAM_ASSERT_EQ(initial.provider_id, std::string("claude-cli"));
		UAM_ASSERT_EQ(initial.model_id, fixture.model);
		UAM_ASSERT_EQ(initial.effective_workspace_access, std::string("read"));
		const auto initial_root = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		const auto initial_transcript = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, initial.transcript_chat_id, 4 * 1024 * 1024);
		UAM_ASSERT(initial_root && initial_transcript);
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(initial_root->managed_followup_incarnation));
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(initial_transcript->managed_followup_incarnation));
		UAM_ASSERT(initial_root->managed_followup_incarnation != initial_transcript->managed_followup_incarnation);
		UAM_ASSERT_EQ(initial.managed_continuation->owner.root_birth_id, initial_root->managed_followup_incarnation);
		UAM_ASSERT_EQ(initial.managed_continuation->owner.transcript_birth_id, initial_transcript->managed_followup_incarnation);
		UAM_ASSERT(!initial.managed_continuation->owner.delegation_grant);
		const auto& principal = initial.managed_continuation->turns.front().principal;
		UAM_ASSERT_EQ(principal.kind, uam::ManagedRunPrincipalKind::UserRoot);
		UAM_ASSERT_EQ(principal.root_chat_id, fixture.root_id);
		UAM_ASSERT_EQ(principal.root_birth_id, initial_root->managed_followup_incarnation);
		UAM_ASSERT(principal.parent_run_id.empty() && principal.parent_transcript_birth_id.empty());
		UAM_ASSERT(initial_transcript->native_session_id.empty() && !initial.managed_continuation->execution);
		fixture.Await([&] { return fs::exists(fixture.temp.root / "input-witness.json"); });
		const auto input = nlohmann::json::parse(ReadFile(fixture.temp.root / "input-witness.json"));
		const auto launch = nlohmann::json::parse(ReadFile(fixture.temp.root / "launch-witness.json"));
		UAM_ASSERT_EQ(launch.at("run").at("status").get<std::string>(), std::string("running"));
		UAM_ASSERT(launch.at("run").at("managedContinuation").at("turns").at(0).at("dispatchAttempted").get<bool>());
		const auto held = fixture.ReadRun();
		UAM_ASSERT_EQ(held.id, initial.id);
		UAM_ASSERT(held.managed_continuation && !held.managed_continuation->execution);
		const auto& held_turn = held.managed_continuation->turns.front();
		const auto* session = uam::FindAcpSessionForChat(fixture.app, initial.transcript_chat_id);
		UAM_ASSERT(session && session->running && session->processing && session->session_id.empty());
		UAM_ASSERT_EQ(session->provider_id, std::string("claude-cli"));
		UAM_ASSERT_EQ(session->process_execution_host_id, std::string("local"));
		UAM_ASSERT_EQ(session->child_pid, input.at("pid").get<pid_t>());
		UAM_ASSERT_EQ(session->turn_serial, input.at("turn").at("expectedTurnSerial").get<int>());
		UAM_ASSERT_EQ(held_turn.expected_turn_serial, session->turn_serial);
		UAM_ASSERT_EQ(held_turn.first_user_message_index, input.at("turn").at("firstUserMessageIndex").get<int>());
		UAM_ASSERT_EQ(held_turn.user_message_created_at, input.at("turn").at("userMessageCreatedAt").get<std::string>());
		UAM_ASSERT_EQ(held.status, std::string("running"));
		UAM_ASSERT(held_turn.dispatch_attempted && held_turn.expected_turn_serial > 0);
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-completion", "release owned private response\n"));
		fixture.AssertPeerFiles();
		fixture.Await(
		    [&]
		    {
			    const auto run = fixture.ReadRun();
			    return run.status == "completed" && run.root_result_delivered;
		    });
		const AgentRun completed = fixture.ReadRun();
		const auto native = nlohmann::json::parse(ReadFile(fixture.temp.root / "native-witness.json"));
		const auto native_id = native.at("native_id").get<std::string>();
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(native_id));
		const auto transcript = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, completed.transcript_chat_id, 4 * 1024 * 1024);
		const auto root = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		UAM_ASSERT(root && transcript && completed.managed_continuation);
		const auto& turn = completed.managed_continuation->turns.front();
		UAM_ASSERT_EQ(completed.id, initial.id);
		UAM_ASSERT_EQ(completed.managed_continuation->current_turn_id, initial.id);
		UAM_ASSERT_EQ(completed.managed_continuation->turns.size(), std::size_t{1});
		UAM_ASSERT_EQ(turn.status, uam::ManagedRunTurnStatus::Completed);
		UAM_ASSERT_EQ(turn.expected_turn_serial, held_turn.expected_turn_serial);
		UAM_ASSERT_EQ(completed.expected_turn_serial, turn.expected_turn_serial);
		UAM_ASSERT(completed.started_at_epoch_ms > 0 && completed.finished_at_epoch_ms >= completed.started_at_epoch_ms);
		UAM_ASSERT_EQ(turn.started_at_epoch_ms, completed.started_at_epoch_ms);
		UAM_ASSERT_EQ(turn.finished_at_epoch_ms, completed.finished_at_epoch_ms);
		UAM_ASSERT(turn.result_required && turn.result_delivered && completed.root_result_delivered);
		UAM_ASSERT_EQ(turn.result_delivery_attempts, completed.root_result_delivery_attempts);
		UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
		UAM_ASSERT_EQ(transcript->native_session_id, native_id);
		UAM_ASSERT_EQ(transcript->messages.at(static_cast<std::size_t>(turn.first_user_message_index)).content, fixture.task);
		UAM_ASSERT_EQ(transcript->messages.at(static_cast<std::size_t>(turn.first_user_message_index)).created_at, turn.user_message_created_at);
		UAM_ASSERT_EQ(std::ranges::count(transcript->messages, fixture.task, &Message::content), 1);
		UAM_ASSERT_EQ(std::ranges::count(transcript->messages, fixture.answer, &Message::content), 1);
		UAM_ASSERT_EQ(completed.managed_continuation->owner, initial.managed_continuation->owner);
		UAM_ASSERT_EQ(root->managed_followup_incarnation, initial_root->managed_followup_incarnation);
		UAM_ASSERT_EQ(transcript->managed_followup_incarnation, initial_transcript->managed_followup_incarnation);
		UAM_ASSERT_EQ(transcript->provider_id, initial.provider_id);
		UAM_ASSERT_EQ(transcript->workspace_directory, initial_transcript->workspace_directory);
		UAM_ASSERT_EQ(transcript->workspace_directory, initial_root->workspace_directory);
		UAM_ASSERT_EQ(transcript->model_id, fixture.model);
		UAM_ASSERT_EQ(transcript->execution_host_id, std::string("local"));
		UAM_ASSERT_EQ(transcript->approval_mode, std::string(uam::approval_modes::kPlanApprovalMode));
		UAM_ASSERT(root->messages.size() >= fixture.original.size() + 1);
		UAM_ASSERT(std::equal(fixture.original.begin(), fixture.original.end(), root->messages.begin()));
		UAM_ASSERT_EQ(root->messages.at(fixture.original.size()).content, text);
		UAM_ASSERT_EQ(std::ranges::count(root->messages, text, &Message::content), 1);
		const auto live_root = std::ranges::find(fixture.app.chats, fixture.root_id, &ChatSession::id);
		UAM_ASSERT(live_root != fixture.app.chats.end() && live_root->metadata_incarnation == fixture.root_incarnation);
		if (completed.managed_continuation->execution)
		{
			UAM_ASSERT(include_model); // Neither initialize catalog nor omitted init attests selected model.
			const auto& binding = *completed.managed_continuation->execution;
			UAM_ASSERT_EQ(binding.native_session_id, native_id);
			UAM_ASSERT_EQ(binding.effective_model_id, fixture.model);
			UAM_ASSERT_EQ(binding.model_identity_source, std::string("exact-requested"));
			UAM_ASSERT_EQ(binding.execution_host_id, std::string("local"));
		}
		if (!include_model)
			UAM_ASSERT(!completed.managed_continuation->execution);
		const auto results = [&]()
		{
			const auto current = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
			UAM_ASSERT(current);
			std::size_t count = 0;
			for (const auto& row : current->messages)
				for (const auto& tool : row.tool_calls)
					if (tool.id == "uam-agent-result-" + turn.delivery_id)
					{
						++count;
						UAM_ASSERT(tool.is_sub_agent && tool.sub_agent_id == initial.id);
						const auto result = nlohmann::json::parse(tool.result_text);
						UAM_ASSERT_EQ(result.at("result").get<std::string>(), fixture.answer);
					}
			return count;
		};
		UAM_ASSERT_EQ(results(), std::size_t{1});
		for (int poll = 0; poll < 100; ++poll)
		{
			fixture.Pulse();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT_EQ(results(), std::size_t{1});
		std::istringstream inputs(ReadFile(fixture.temp.root / "task-inputs.jsonl"));
		std::string line;
		std::size_t input_count = 0;
		while (std::getline(inputs, line))
			if (!line.empty())
			{
				(void)nlohmann::json::parse(line);
				++input_count;
			}
		UAM_ASSERT_EQ(input_count, std::size_t{1});
		UAM_ASSERT_EQ(fixture.ReadRun().id, initial.id);
		fixture.Await([&] { return fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty(); });
		const auto followup = uam::ManagedSubagentFollowupService::GetState(fixture.app, fixture.root_id, initial.id);
		UAM_ASSERT_EQ(followup.can_send, completed.managed_continuation->execution.has_value());
		UAM_ASSERT_EQ(followup.code, followup.can_send ? uam::ManagedFollowupCode::None : uam::ManagedFollowupCode::ModelIdentityUnverified);
		UAM_ASSERT(fixture.StopAndReap());
	}
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
namespace
{
	class ManagedInitialCancelCallback final : public UamQueryHandler::Callback
	{
	  public:
		int successes = 0, failures = 0;
		nlohmann::json body;
		std::string error;
		std::function<void()> recorded;
		void Success(const CefString& value) override
		{
			body = nlohmann::json::parse(value.ToString());
			++successes;
			if (body.contains("managedMention") && recorded)
				recorded();
		}
		void Success(const void*, size_t) override
		{
			++successes;
		}
		void Failure(int, const CefString& value) override
		{
			++failures;
			error = value.ToString();
		}

	  private:
		IMPLEMENT_REFCOUNTING(ManagedInitialCancelCallback);
	};

	void ManagedCancelledPeerAlreadyReaped(pid_t pid)
	{
		UAM_ASSERT(pid > 0);
		// Observe only: do not stop/reap here and mask incomplete production cancellation.
		errno = 0;
		UAM_ASSERT(kill(pid, 0) < 0 && errno == ESRCH);
		int status = 0;
		errno = 0;
		UAM_ASSERT(waitpid(pid, &status, WNOHANG) < 0 && errno == ECHILD);
	}
} // namespace

UAM_TEST(ManagedInitialCancelTreeSettlesQueuedAndOwnedRunningTurns)
{
	for (const bool running : {false, true})
	{
		ManagedClaudeLifecycleFixture fixture(false);
		const std::string text = "@private-stream-reviewer " + fixture.task;
		CefRefPtr<ManagedInitialCancelCallback> callback = new ManagedInitialCancelCallback;
		std::optional<AgentRun> at_receipt;
		bool first_cancel_called = false, first_cancel_success = false;
		std::string first_cancel_error;
		callback->recorded = [&]
		{
			const auto& receipt = callback->body.at("managedMention");
			if (!receipt.at("inputRecorded").get<bool>() || receipt.at("delegationState") != "committed")
				return;
			at_receipt = fixture.ReadRun();
			fixture.run_id = at_receipt->id;
			// This is the real CEF completion owner stack, before another scheduler turn.
			if (!running)
			{
				first_cancel_called = true;
				first_cancel_success = uam::AgentRunScheduler::CancelTree(fixture.app, fixture.run_id, &first_cancel_error);
			}
		};
		fixture.AssertPeerFiles();
		NativeMediaRefreshHandlerTestAccess::Send(*fixture.handler, {{"chatId", fixture.root_id}, {"text", text}}, callback);
		fixture.Await([&] { return callback->successes + callback->failures != 0; });
		if (callback->failures)
			throw TestFailure(std::string(running ? "Running" : "Queued") + " cancellation producer prerequisite: " + callback->error.substr(0, 512));
		UAM_ASSERT_EQ(callback->successes, 1);
		// CURRENT producer RED; cancellation-specific assertions are initially UNEXERCISED.
		UAM_ASSERT(callback->body.contains("managedMention"));
		const auto& receipt = callback->body.at("managedMention");
		UAM_ASSERT(receipt.at("inputRecorded").get<bool>() && receipt.at("delegationState") == "committed");
		UAM_ASSERT_EQ(receipt.at("parentMessageIndex").get<std::size_t>(), fixture.original.size());
		UAM_ASSERT_EQ(receipt.at("parentMessage").at("content").get<std::string>(), text);
		UAM_ASSERT(at_receipt && at_receipt->managed_continuation);
		const AgentRun initial = *at_receipt;
		UAM_ASSERT_EQ(receipt.at("runId").get<std::string>(), initial.id);
		UAM_ASSERT_EQ(receipt.at("transcriptChatId").get<std::string>(), initial.transcript_chat_id);
		UAM_ASSERT_EQ(initial.status, std::string("queued"));
		UAM_ASSERT_EQ(initial.root_chat_id, fixture.root_id);
		UAM_ASSERT(initial.parent_run_id.empty() && initial.depth == 1);
		UAM_ASSERT_EQ(initial.managed_continuation->turns.size(), std::size_t{1});
		UAM_ASSERT_EQ(initial.managed_continuation->current_turn_id, initial.id);
		UAM_ASSERT(!initial.managed_continuation->execution && !initial.managed_continuation->turns.front().dispatch_attempted);
		UAM_ASSERT(initial.deliver_result_to_root_chat && initial.managed_continuation->turns.front().result_required);
		UAM_ASSERT_EQ(initial.definition_snapshot, fixture.definition.markdown_snapshot);
		UAM_ASSERT_EQ(initial.definition_hash, fixture.definition.definition_hash);
		UAM_ASSERT_EQ(initial.provider_id, std::string("claude-cli"));
		UAM_ASSERT_EQ(initial.model_id, fixture.model);
		UAM_ASSERT_EQ(initial.effective_workspace_access, std::string("read"));
		AgentRun before = initial;
		pid_t peer_pid = 0;
		if (running)
		{
			fixture.Await([&] { return fs::exists(fixture.temp.root / "input-witness.json"); });
			const auto input = nlohmann::json::parse(ReadFile(fixture.temp.root / "input-witness.json"));
			before = fixture.ReadRun();
			const auto* session = uam::FindAcpSessionForChat(fixture.app, initial.transcript_chat_id);
			UAM_ASSERT(session && session->running && session->processing && session->session_id.empty());
			UAM_ASSERT_EQ(session->provider_id, std::string("claude-cli"));
			UAM_ASSERT_EQ(session->process_execution_host_id, std::string("local"));
			peer_pid = input.at("pid").get<pid_t>();
			UAM_ASSERT_EQ(session->child_pid, peer_pid);
			UAM_ASSERT_EQ(before.status, std::string("running"));
			UAM_ASSERT(before.managed_continuation && !before.managed_continuation->execution);
			const auto& held = before.managed_continuation->turns.front();
			UAM_ASSERT(held.dispatch_attempted && held.expected_turn_serial > 0 && before.started_at_epoch_ms > 0);
			UAM_ASSERT_EQ(held.expected_turn_serial, session->turn_serial);
			UAM_ASSERT_EQ(held.expected_turn_serial, input.at("turn").at("expectedTurnSerial").get<int>());
			UAM_ASSERT_EQ(held.first_user_message_index, input.at("turn").at("firstUserMessageIndex").get<int>());
			UAM_ASSERT_EQ(held.user_message_created_at, input.at("turn").at("userMessageCreatedAt").get<std::string>());
			fixture.ObserveProcesses();
			first_cancel_called = true;
			first_cancel_success = uam::AgentRunScheduler::CancelTree(fixture.app, initial.id, &first_cancel_error);
		}
		else
		{
			UAM_ASSERT(!fs::exists(fixture.temp.root / "launch-witness.json"));
			UAM_ASSERT(!fs::exists(fixture.temp.root / "task-inputs.jsonl"));
			UAM_ASSERT_EQ(initial.expected_turn_serial, 0);
			UAM_ASSERT_EQ(initial.started_at_epoch_ms, int64_t{0});
			UAM_ASSERT_EQ(initial.managed_continuation->turns.front().first_user_message_index, -1);
		}
		UAM_ASSERT(first_cancel_called);
		if (!first_cancel_success)
		{
			// Public cancellation may truthfully leave a retryable owned Stop intent.
			const auto* session = uam::FindAcpSessionForChat(fixture.app, initial.transcript_chat_id);
			UAM_ASSERT(!first_cancel_error.empty());
			UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.contains(initial.id) || (session && session->managed_cancellation_pending));
		}
		fixture.Await(
		    [&]
		    {
			    const auto current = fixture.ReadRun();
			    if (current.status != "cancelled")
				    return false;
			    if (running)
				    ManagedCancelledPeerAlreadyReaped(peer_pid);
			    return current.root_result_delivered;
		    });
		const AgentRun cancelled = fixture.ReadRun();
		UAM_ASSERT(cancelled.managed_continuation && !cancelled.managed_continuation->execution);
		const auto& turn = cancelled.managed_continuation->turns.front();
		const auto& prior_turn = before.managed_continuation->turns.front();
		UAM_ASSERT_EQ(cancelled.status, std::string("cancelled"));
		UAM_ASSERT_EQ(turn.status, uam::ManagedRunTurnStatus::Cancelled);
		UAM_ASSERT_EQ(cancelled.id, initial.id);
		UAM_ASSERT_EQ(cancelled.root_chat_id, initial.root_chat_id);
		UAM_ASSERT_EQ(cancelled.transcript_chat_id, initial.transcript_chat_id);
		UAM_ASSERT_EQ(cancelled.definition_snapshot, initial.definition_snapshot);
		UAM_ASSERT_EQ(cancelled.definition_hash, initial.definition_hash);
		UAM_ASSERT_EQ(cancelled.task, initial.task);
		UAM_ASSERT_EQ(cancelled.provider_id, initial.provider_id);
		UAM_ASSERT_EQ(cancelled.model_id, initial.model_id);
		UAM_ASSERT_EQ(cancelled.effective_workspace_access, initial.effective_workspace_access);
		UAM_ASSERT_EQ(cancelled.managed_continuation->owner, initial.managed_continuation->owner);
		UAM_ASSERT_EQ(cancelled.managed_continuation->current_turn_id, initial.id);
		UAM_ASSERT_EQ(cancelled.managed_continuation->turns.size(), std::size_t{1});
		UAM_ASSERT(cancelled.managed_continuation->state_revision > before.managed_continuation->state_revision);
		UAM_ASSERT_EQ(turn.principal, prior_turn.principal);
		UAM_ASSERT_EQ(turn.delivery_id, prior_turn.delivery_id);
		UAM_ASSERT_EQ(turn.dispatch_attempted, running);
		UAM_ASSERT_EQ(turn.expected_turn_serial, prior_turn.expected_turn_serial);
		UAM_ASSERT_EQ(cancelled.expected_turn_serial, turn.expected_turn_serial);
		UAM_ASSERT_EQ(turn.first_user_message_index, prior_turn.first_user_message_index);
		UAM_ASSERT_EQ(turn.user_message_created_at, prior_turn.user_message_created_at);
		UAM_ASSERT_EQ(cancelled.started_at_epoch_ms, before.started_at_epoch_ms);
		UAM_ASSERT_EQ(turn.started_at_epoch_ms, cancelled.started_at_epoch_ms);
		UAM_ASSERT(cancelled.finished_at_epoch_ms > 0 && cancelled.finished_at_epoch_ms >= cancelled.started_at_epoch_ms);
		UAM_ASSERT_EQ(turn.finished_at_epoch_ms, cancelled.finished_at_epoch_ms);
		UAM_ASSERT_EQ(cancelled.diagnostic_code, std::string("user_cancelled"));
		UAM_ASSERT_EQ(turn.diagnostic_code, cancelled.diagnostic_code);
		UAM_ASSERT(turn.result_required && turn.result_delivered && cancelled.root_result_delivered);
		UAM_ASSERT_EQ(turn.result_delivery_attempts, cancelled.root_result_delivery_attempts);
		UAM_ASSERT(turn.result_delivery_attempts > 0 && turn.result_delivery_attempts <= 3);
		const auto transcript = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, initial.transcript_chat_id, 4 * 1024 * 1024);
		const auto root = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		UAM_ASSERT(root && transcript && transcript->native_session_id.empty());
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(root->managed_followup_incarnation));
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(transcript->managed_followup_incarnation));
		UAM_ASSERT(root->managed_followup_incarnation != transcript->managed_followup_incarnation);
		UAM_ASSERT_EQ(root->managed_followup_incarnation, initial.managed_continuation->owner.root_birth_id);
		UAM_ASSERT_EQ(transcript->managed_followup_incarnation, initial.managed_continuation->owner.transcript_birth_id);
		UAM_ASSERT_EQ(transcript->agent_run_id, initial.id);
		UAM_ASSERT_EQ(transcript->provider_id, initial.provider_id);
		UAM_ASSERT_EQ(transcript->model_id, fixture.model);
		UAM_ASSERT_EQ(transcript->execution_host_id, std::string("local"));
		UAM_ASSERT_EQ(transcript->workspace_directory, root->workspace_directory);
		UAM_ASSERT_EQ(transcript->approval_mode, std::string(uam::approval_modes::kPlanApprovalMode));
		UAM_ASSERT_EQ(std::ranges::count(transcript->messages, fixture.task, &Message::content), running ? 1 : 0);
		if (running)
		{
			const auto& row = transcript->messages.at(static_cast<std::size_t>(turn.first_user_message_index));
			UAM_ASSERT_EQ(row.content, fixture.task);
			UAM_ASSERT_EQ(row.created_at, turn.user_message_created_at);
		}
		UAM_ASSERT_EQ(std::ranges::count(transcript->messages, fixture.answer, &Message::content), 0);
		UAM_ASSERT(root->messages.size() >= fixture.original.size() + 1);
		UAM_ASSERT(std::equal(fixture.original.begin(), fixture.original.end(), root->messages.begin()));
		UAM_ASSERT_EQ(root->messages.at(fixture.original.size()).content, text);
		UAM_ASSERT_EQ(std::ranges::count(root->messages, text, &Message::content), 1);
		const auto live_root = std::ranges::find(fixture.app.chats, fixture.root_id, &ChatSession::id);
		UAM_ASSERT(live_root != fixture.app.chats.end() && live_root->metadata_incarnation == fixture.root_incarnation);
		const auto result_count = [&]
		{
			const auto current = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
			UAM_ASSERT(current);
			std::size_t count = 0;
			for (const auto& row : current->messages)
				for (const auto& tool : row.tool_calls)
					if (tool.id == "uam-agent-result-" + turn.delivery_id)
					{
						++count;
						UAM_ASSERT(tool.is_sub_agent && tool.sub_agent_id == initial.id);
						const auto result = nlohmann::json::parse(tool.result_text);
						UAM_ASSERT_EQ(result.at("runId").get<std::string>(), initial.id);
						UAM_ASSERT_EQ(result.at("status").get<std::string>(), std::string("cancelled"));
						UAM_ASSERT_EQ(result.at("transcriptChatId").get<std::string>(), initial.transcript_chat_id);
						UAM_ASSERT_EQ(result.at("diagnosticCode").get<std::string>(), std::string("user_cancelled"));
						UAM_ASSERT(result.at("result").get<std::string>() != fixture.answer);
					}
			return count;
		};
		UAM_ASSERT_EQ(result_count(), std::size_t{1});
		const auto ledger_path = fixture.app.data_root / "agent-runs" / (initial.id + ".json");
		const auto backup_path = uam::io::MakeBackupPath(ledger_path);
		const auto primary_bytes = ReadFile(ledger_path);
		const std::optional<std::string> backup_bytes = fs::exists(backup_path) ? std::optional<std::string>{ReadFile(backup_path)} : std::nullopt;
		for (int repeat = 0; repeat < 3; ++repeat)
		{
			std::string error;
			UAM_ASSERT(uam::AgentRunScheduler::CancelTree(fixture.app, initial.id, &error));
			UAM_ASSERT(error.empty());
			for (int poll = 0; poll < 34; ++poll)
			{
				fixture.Pulse();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT_EQ(result_count(), std::size_t{1});
		}
		UAM_ASSERT_EQ(ReadFile(ledger_path), primary_bytes);
		UAM_ASSERT_EQ(fs::exists(backup_path), backup_bytes.has_value());
		if (backup_bytes)
			UAM_ASSERT_EQ(ReadFile(backup_path), *backup_bytes);
		std::size_t inputs = 0;
		if (fs::exists(fixture.temp.root / "task-inputs.jsonl"))
		{
			std::istringstream lines(ReadFile(fixture.temp.root / "task-inputs.jsonl"));
			std::string line;
			while (std::getline(lines, line))
				if (!line.empty())
				{
					(void)nlohmann::json::parse(line);
					++inputs;
				}
		}
		UAM_ASSERT_EQ(inputs, running ? std::size_t{1} : std::size_t{0});
		UAM_ASSERT(!fs::exists(fixture.temp.root / "allow-completion") && !fs::exists(fixture.temp.root / "native-witness.json"));
		if (running)
			ManagedCancelledPeerAlreadyReaped(peer_pid);
		else
			UAM_ASSERT(!fs::exists(fixture.temp.root / "launch-witness.json"));
		UAM_ASSERT(!fixture.app.pending_agent_run_cancellation_ids.contains(initial.id));
		UAM_ASSERT(std::ranges::find(fixture.app.queued_agent_run_ids, initial.id) == fixture.app.queued_agent_run_ids.end());
		UAM_ASSERT(fixture.StopAndReap());
	}
}
#endif

#if defined(__APPLE__) && (UAM_ENABLE_RUNTIME_CLAUDE_CLI || UAM_ENABLE_RUNTIME_GEMINI_CLI)
#include "common/runtime/acp/acp_session_internal.h"
#include "common/utils/uuid.h"
namespace
{
	ChatSession& InitialGuardRoot(uam::AppState& app, const std::string& id)
	{
		auto* root = ChatDomainService().FindChatById(app, id);
		UAM_ASSERT(root);
		return *root;
	}
	struct InitialGuardCleanup
	{
		uam::AppState& app;
		~InitialGuardCleanup()
		{
			// Resource cleanup only, including on a failed oracle; no shutdown outcome assertion.
			(void)uam::AgentRunScheduler::QuiesceInitialForShutdown(app);
			uam::FastStopAcpSessionsForExit(app);
			auto& process = PlatformServicesFactory::Instance().process_service;
			for (const auto& session : app.acp_sessions)
				if (session)
				{
					process.StopStdioProcess(*session, true);
					process.CloseStdioProcessHandles(*session);
				}
		}
	};
	template <class Predicate> void InitialGuardAwait(uam::AppState& app, const std::function<void()>& isolated, Predicate done)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!done() && std::chrono::steady_clock::now() < deadline)
		{
			isolated();
			(void)uam::PollAllAcpSessions(app);
			(void)uam::AgentRunScheduler::Poll(app);
			(void)uam::FlushPendingChatSaves(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT(done());
	}
	struct InitialGuardCompletion
	{
		std::optional<uam::ManagedInitialMentionResult> result;
		int calls = 0, faults = 0;
	};
	void InitialGuardBindRoot(uam::AppState& app, const std::string& id, const std::string& agent, const std::function<void()>& isolated)
	{
		auto completion = std::make_shared<InitialGuardCompletion>();
		const std::string text = "@" + agent + " Establish this real managed parent before the second request.";
		const auto prefix = InitialGuardRoot(app, id).messages;
		std::string error;
		UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
		                  app, id, text,
		                  [&, completion](const uam::ManagedInitialMentionResult& result)
		                  {
			                  ++completion->calls;
			                  completion->result = result;
			                  if (result.delegation_state == uam::ManagedInitialDelegationState::Committed)
			                  {
				                  std::string stop_error;
				                  (void)uam::AgentRunScheduler::CancelTree(app, result.run_id, &stop_error);
			                  }
			                  return true; // Actual classified completion delivery ACK, not storage authority.
		                  },
		                  &error),
		              uam::ManagedInitialMentionAdmission::Pending);
		InitialGuardAwait(app, isolated,
		                  [&]
		                  {
			                  if (!completion->result)
				                  return false;
			                  if (completion->result->delegation_state != uam::ManagedInitialDelegationState::Committed)
				                  return true;
			                  const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(app.data_root);
			                  return ledger.errors.empty() && ledger.runs.size() == 1 && ledger.runs.front().status == "cancelled" && ledger.runs.front().root_result_delivered && !uam::AgentRunScheduler::HasPendingInitialMention(app, id);
		                  });
		if (completion->result && !completion->result->input_recorded)
			throw TestFailure("Initial guard real creation prerequisite: " + completion->result->error.substr(0, 4096));
		UAM_ASSERT_EQ(completion->calls, 1);
		UAM_ASSERT(completion->result && completion->result->input_recorded);
		UAM_ASSERT_EQ(completion->result->delegation_state, uam::ManagedInitialDelegationState::Committed);
		UAM_ASSERT(completion->result->parent_message && completion->result->parent_message->content == text);
		UAM_ASSERT_EQ(completion->result->parent_message_index, prefix.size());
		const auto& live = InitialGuardRoot(app, id);
		const auto saved = ChatRepository::LoadLocalChatBounded(app.data_root, id, 4 * 1024 * 1024);
		UAM_ASSERT(saved && saved->messages == live.messages && saved->managed_followup_incarnation == live.managed_followup_incarnation);
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(saved->managed_followup_incarnation));
		UAM_ASSERT(live.messages.size() >= prefix.size() + 1 && std::equal(prefix.begin(), prefix.end(), live.messages.begin()));
		UAM_ASSERT_EQ(std::ranges::count(live.messages, text, &Message::content), 1);
		const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(app.data_root);
		UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1 && ledger.runs.front().managed_continuation);
		UAM_ASSERT_EQ(ledger.runs.front().id, completion->result->run_id);
		UAM_ASSERT_EQ(ledger.runs.front().managed_continuation->owner.root_birth_id, live.managed_followup_incarnation);
		UAM_ASSERT_EQ(ledger.runs.front().status, std::string("cancelled"));
		UAM_ASSERT(ledger.runs.front().root_result_delivered && app.acp_sessions.empty());
		UAM_ASSERT(!uam::AgentRunScheduler::HasUnconfirmedInitialRootPublication(app, live));
	}
	struct InitialGuardUncertain
	{
		ChatSession live;
		std::string text, primary;
		std::optional<std::string> backup;
		std::shared_ptr<InitialGuardCompletion> completion;
	};
	InitialGuardUncertain InitialGuardUnconfirmedRoot(uam::AppState& app, const std::string& id, const std::string& agent, const std::function<void()>& isolated)
	{
		InitialGuardUncertain state;
		state.live = InitialGuardRoot(app, id);
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(state.live.managed_followup_incarnation));
		state.text = "@" + agent + " Keep this second authored request in the unconfirmed parent prefix.";
		state.completion = std::make_shared<InitialGuardCompletion>();
		const auto completion = state.completion;
		std::string error;
		UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
		                  app, id, state.text,
		                  [completion](const uam::ManagedInitialMentionResult& result)
		                  {
			                  ++completion->calls;
			                  completion->result = result;
			                  return true;
		                  },
		                  &error,
		                  [completion](ChatPublicationFile file, uam::io::AtomicWriteStage stage)
		                  {
			                  if (file == ChatPublicationFile::Primary && stage == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync)
			                  {
				                  ++completion->faults;
				                  return false;
			                  }
			                  return true;
		                  }),
		              uam::ManagedInitialMentionAdmission::Pending);
		InitialGuardAwait(app, isolated, [&] { return uam::AgentRunScheduler::HasUnconfirmedInitialRootPublication(app, InitialGuardRoot(app, id)); });
		UAM_ASSERT_EQ(completion->faults, 1);
		UAM_ASSERT_EQ(completion->calls, 1);
		UAM_ASSERT(completion->result && completion->result->delegation_state == uam::ManagedInitialDelegationState::Uncertain);
		UAM_ASSERT(!completion->result->error.empty() && completion->result->run_id.empty() && completion->result->transcript_chat_id.empty());
		const auto& live = InitialGuardRoot(app, id);
		UAM_ASSERT_EQ(live.messages, state.live.messages);
		UAM_ASSERT_EQ(live.metadata_incarnation, state.live.metadata_incarnation);
		UAM_ASSERT_EQ(live.managed_followup_incarnation, state.live.managed_followup_incarnation);
		const auto disk = ChatRepository::LoadLocalChatBounded(app.data_root, id, 4 * 1024 * 1024);
		UAM_ASSERT(disk && disk->managed_followup_incarnation == live.managed_followup_incarnation);
		UAM_ASSERT_EQ(disk->messages.size(), live.messages.size() + 1);
		UAM_ASSERT(std::equal(live.messages.begin(), live.messages.end(), disk->messages.begin()));
		UAM_ASSERT_EQ(disk->messages.back().content, state.text);
		UAM_ASSERT_EQ(std::ranges::count(disk->messages, state.text, &Message::content), 1);
		UAM_ASSERT_EQ(std::ranges::count(live.messages, state.text, &Message::content), 0);
		const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(app.data_root);
		UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1 && ledger.runs.front().status == "cancelled");
		UAM_ASSERT(uam::AgentRunScheduler::HasPendingInitialMention(app, id));
		const auto primary = AppPaths::UamChatFilePath(app.data_root, id), backup = uam::io::MakeBackupPath(primary);
		state.primary = ReadFile(primary);
		if (fs::exists(backup))
			state.backup = ReadFile(backup);
		return state; // No further scheduler turn: uncertain reconciliation publishes only on owner Poll.
	}
	void InitialGuardPreserved(uam::AppState& app, const std::string& id, const InitialGuardUncertain& before)
	{
		const auto& live = InitialGuardRoot(app, id);
		UAM_ASSERT(uam::AgentRunScheduler::HasUnconfirmedInitialRootPublication(app, live));
		UAM_ASSERT_EQ(live.metadata_incarnation, before.live.metadata_incarnation);
		UAM_ASSERT_EQ(live.messages, before.live.messages);
		UAM_ASSERT_EQ(live.title, before.live.title);
		UAM_ASSERT_EQ(live.folder_id, before.live.folder_id);
		UAM_ASSERT_EQ(live.workspace_directory, before.live.workspace_directory);
		UAM_ASSERT_EQ(live.managed_followup_incarnation, before.live.managed_followup_incarnation);
		const auto primary = AppPaths::UamChatFilePath(app.data_root, id), backup = uam::io::MakeBackupPath(primary);
		UAM_ASSERT_EQ(ReadFile(primary), before.primary);
		UAM_ASSERT_EQ(fs::exists(backup), before.backup.has_value());
		if (before.backup)
			UAM_ASSERT_EQ(ReadFile(backup), *before.backup);
		UAM_ASSERT_EQ(before.completion->calls, 1);
	}
} // namespace
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
namespace
{
	void InitialGuardDirectWriterCase(int writer)
	{
		// Before ROOT permission, Rename remains eligible and makes the captured creation stale.
		if (writer == 0)
		{
			ManagedClaudeLifecycleFixture fixture(false);
			InitialGuardCleanup cleanup{fixture.app};
			const auto isolated = [&] { fixture.AssertPeerFiles(); };
			InitialGuardBindRoot(fixture.app, fixture.root_id, fixture.definition.id, isolated);
			const auto prefix = InitialGuardRoot(fixture.app, fixture.root_id).messages;
			auto completion = std::make_shared<InitialGuardCompletion>();
			std::string error;
			const std::string text = "@" + fixture.definition.id + " This pre-publication request must lose its stale metadata CAS.";
			UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
			                  fixture.app, fixture.root_id, text,
			                  [completion](const uam::ManagedInitialMentionResult& result)
			                  {
				                  ++completion->calls;
				                  completion->result = result;
				                  return true;
			                  },
			                  &error),
			              uam::ManagedInitialMentionAdmission::Pending);
			auto& root = InitialGuardRoot(fixture.app, fixture.root_id);
			UAM_ASSERT(!uam::AgentRunScheduler::HasUnconfirmedInitialRootPublication(fixture.app, root));
			UAM_ASSERT(ChatHistorySyncService().RenameChat(fixture.app, root, "Allowed before ROOT publication"));
			InitialGuardAwait(fixture.app, isolated, [&] { return completion->result && !uam::AgentRunScheduler::HasPendingInitialMention(fixture.app, fixture.root_id); });
			UAM_ASSERT_EQ(completion->calls, 1);
			UAM_ASSERT(!completion->result->input_recorded && completion->result->delegation_state == uam::ManagedInitialDelegationState::Rejected);
			const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
			UAM_ASSERT(saved && saved->messages == prefix && saved->title == "Allowed before ROOT publication");
			UAM_ASSERT_EQ(fixture.app.agent_runs.size(), std::size_t{1});
		}
		{
			ManagedClaudeLifecycleFixture fixture(false);
			InitialGuardCleanup cleanup{fixture.app};
			const auto isolated = [&] { fixture.AssertPeerFiles(); };
			InitialGuardBindRoot(fixture.app, fixture.root_id, fixture.definition.id, isolated);
			std::string folder;
			const auto destination = fixture.temp.root / "ordinary-move-target";
			fs::create_directories(destination);
			UAM_ASSERT(CreateFolder(fixture.app, "Ordinary destination", destination.string(), &folder));
			// The same full writer is allowed AFTER a real ROOT ACK and retains its recorded row/B.
			const auto prefix = InitialGuardRoot(fixture.app, fixture.root_id).messages;
			const auto birth = InitialGuardRoot(fixture.app, fixture.root_id).managed_followup_incarnation;
			auto apply = [&](bool blocked)
			{
				auto& root = InitialGuardRoot(fixture.app, fixture.root_id);
				if (writer == 0)
					return ChatHistorySyncService().RenameChat(fixture.app, root, blocked ? "Must not overwrite the private prefix" : "Ordinary bound rename");
				if (writer == 1)
					return ChatHistorySyncService().MoveChatToFolder(fixture.app, root, folder);
				return uam::acp_detail::SaveChatQuietly(fixture.app, root);
			};
			UAM_ASSERT(apply(false));
			const auto ordinary = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
			UAM_ASSERT(ordinary && ordinary->messages == prefix && ordinary->managed_followup_incarnation == birth);
			if (writer == 1)
			{
				// A distinct real destination makes the uncertain arm a write, not same-folder no-op.
				const auto second = fixture.temp.root / "blocked-move-target";
				fs::create_directories(second);
				UAM_ASSERT(CreateFolder(fixture.app, "Blocked destination", second.string(), &folder));
			}
			const auto uncertain = InitialGuardUnconfirmedRoot(fixture.app, fixture.root_id, fixture.definition.id, isolated);
			// Intended writer RED: all real same-B/private-prefix prerequisites precede this call.
			if (apply(true))
			{
				const auto after = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
				const bool retained = after && std::ranges::count(after->messages, uncertain.text, &Message::content) == 1;
				throw TestFailure("Unconfirmed ROOT writer was accepted: " + std::string(writer == 0 ? "RenameChat" : writer == 1 ? "MoveChatToFolder(local)" : "SaveChatQuietly") + "; authored request retained=" + (retained ? "true" : "false"));
			}
			InitialGuardPreserved(fixture.app, fixture.root_id, uncertain);
		}
	}
} // namespace
UAM_TEST(ManagedInitialUnconfirmedRootRejectsRenameButAllowsOrdinaryRename)
{
	InitialGuardDirectWriterCase(0);
}
UAM_TEST(ManagedInitialUnconfirmedRootRejectsLocalMoveButAllowsOrdinaryMove)
{
	InitialGuardDirectWriterCase(1);
}
UAM_TEST(ManagedInitialUnconfirmedRootRejectsQuietSaveButAllowsOrdinarySave)
{
	InitialGuardDirectWriterCase(2);
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
namespace
{
	struct InitialGuardMediaFixture
	{
		TempDir executable_root{"uam-managed-guard-inert-gemini"};
		ScopedEnvVar private_path{"PATH", (executable_root.root / "bin").string()};
		MediaFixture media;
		std::string root_id, agent_id = "private-root-media-reviewer";
		fs::path Executable() const
		{
			return executable_root.root / "bin" / "gemini";
		}
		void Isolated() const
		{
			UAM_ASSERT(fs::is_regular_file(Executable()) && !fs::is_symlink(Executable()));
			UAM_ASSERT((fs::status(Executable()).permissions() & fs::perms::owner_exec) != fs::perms::none);
			UAM_ASSERT(std::getenv("PATH") && std::string(std::getenv("PATH")) == (executable_root.root / "bin").string());
			UAM_ASSERT(!fs::exists(executable_root.root / "unexpected-cli"));
		}
		InitialGuardMediaFixture()
		{
			UAM_ASSERT(cef_api_hash(CEF_API_VERSION, 0) != nullptr);
			UAM_ASSERT(uam::io::WriteTextFile(Executable(), "#!/bin/sh\nprintf unexpected > " + ShellQuoteForTest((executable_root.root / "unexpected-cli").string()) + "\nexit 97\n"));
			fs::permissions(Executable(), fs::perms::owner_all, fs::perm_options::replace);
			Isolated();
			auto& app = media.app;
			auto& root = app.chats.front();
			root_id = root.id;
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.settings.central_provider_configuration.enabled = false;
			app.settings.memory_enabled_default = false;
			app.settings.memory_level_default = "off";
			app.settings.memory_recall_budget_bytes = 0;
			app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
			app.provider_profiles.front().interactive_command = ShellQuoteForTest(Executable().string());
			root.execution_host_id = "local";
			root.routing_mode = "explicit";
			root.model_id = "private-guard-requested-model";
			root.branch_root_chat_id = root.id; // Match normal saved-root lineage before genuine admission.
			root.reasoning_effort.clear();
			root.service_tier.clear();
			root.service_tier_explicit = false;
			root.memory_enabled = false;
			root.memory_level = "off";
			root.uam_control_enabled = false;
			root.uam_control_explicitly_disabled = true;
			UAM_ASSERT(root.metadata_incarnation && root.managed_followup_incarnation.empty());
			UAM_ASSERT(ProviderResolutionService().ChatProviderIsAvailable(app, root));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, root));
			uam::AgentDefinition definition;
			definition.id = agent_id;
			definition.mode = "subagent";
			definition.description = "Private media prefix review";
			definition.workspace_access = "read";
			definition.instructions = "Read only and do not delegate.";
			std::string error;
			UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(app.data_root, definition, &error));
		}
		void WriteJournalFromLivePrefix()
		{
			const auto& root = InitialGuardRoot(media.app, root_id);
			auto document = nlohmann::json::parse(ReadFile(media.native_file));
			const auto inline_image = document.at("messages").at(0).at("content").at(1);
			document["messages"] = nlohmann::json::array();
			std::size_t index = 0;
			for (const auto& row : root.messages)
			{
				nlohmann::json content = nlohmann::json::array({{{"text", row.content}}});
				if (index == 0)
					content.push_back(inline_image);
				document["messages"].push_back({{"id", "private-prefix-" + std::to_string(index++)}, {"type", row.role == MessageRole::User ? "user" : "gemini"}, {"timestamp", row.created_at}, {"content", content}});
			}
			document["messages"].push_back({{"id", "private-native-media-tail"}, {"type", "gemini"}, {"timestamp", "2026-10-17T16:00:00Z"}, {"content", "Private native media tail before the second managed request."}});
			UAM_ASSERT(uam::io::WriteTextFile(media.native_file, document.dump()));
			Isolated();
		}
		~InitialGuardMediaFixture()
		{
			(void)uam::AgentRunScheduler::QuiesceInitialForShutdown(media.app);
			uam::FastStopAcpSessionsForExit(media.app);
			auto& process = PlatformServicesFactory::Instance().process_service;
			for (const auto& session : media.app.acp_sessions)
				if (session)
				{
					process.StopStdioProcess(*session, true);
					process.CloseStdioProcessHandles(*session);
				}
		}
	};
} // namespace
UAM_TEST(ManagedInitialUnconfirmedRootPreventsNativeMediaPrefixReplacement)
{
	InitialGuardMediaFixture fixture;
	const auto isolated = [&] { fixture.Isolated(); };
	InitialGuardBindRoot(fixture.media.app, fixture.root_id, fixture.agent_id, isolated);
	fixture.WriteJournalFromLivePrefix();
	const auto birth = InitialGuardRoot(fixture.media.app, fixture.root_id).managed_followup_incarnation;
	// Actual read-only private journal -> both existing workers -> actual media publication, after ACK.
	const auto ordinary = fixture.media.Start();
	fixture.media.Complete(0);
	fixture.media.Complete(1);
	UAM_ASSERT_EQ(ordinary->successes, 1);
	UAM_ASSERT_EQ(ordinary->failures, 0);
	UAM_ASSERT_EQ(InitialGuardRoot(fixture.media.app, fixture.root_id).managed_followup_incarnation, birth);
	UAM_ASSERT_EQ(InitialGuardRoot(fixture.media.app, fixture.root_id).messages.front().attachments.size(), std::size_t{1});
	const auto saved = ChatRepository::LoadLocalChatBounded(fixture.media.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(saved && saved->messages == InitialGuardRoot(fixture.media.app, fixture.root_id).messages);
	UAM_ASSERT_EQ(saved->managed_followup_incarnation, birth);
	fixture.media.pending.clear();
	const auto uncertain = InitialGuardUnconfirmedRoot(fixture.media.app, fixture.root_id, fixture.agent_id, isolated);
	// Start AFTER uncertainty: its fresh revision must not authorize stale live-body replacement.
	const auto refresh = fixture.media.Start();
	for (std::size_t index = 0; index < fixture.media.pending.size(); ++index)
	{
		auto query = fixture.media.Take(index);
		auto result = query.Run();
		query.Finish(std::move(result));
	}
	UAM_ASSERT_EQ(refresh->successes + refresh->failures, 1);
	InitialGuardPreserved(fixture.media.app, fixture.root_id, uncertain);
	fixture.Isolated();
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
UAM_TEST(ManagedInitialPresentationChangesPreserveCapturedRequest)
{
	ManagedClaudeLifecycleFixture fixture(false);
	InitialGuardCleanup cleanup{fixture.app};
	const auto isolated = [&] { fixture.AssertPeerFiles(); };
	const auto prefix = InitialGuardRoot(fixture.app, fixture.root_id).messages;
	const std::string text = "@" + fixture.definition.id + " Keep this captured request while a sidebar preference changes.";
	auto completion = std::make_shared<InitialGuardCompletion>();
	std::string error;
	UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
	                  fixture.app, fixture.root_id, text,
	                  [&, completion](const uam::ManagedInitialMentionResult& result)
	                  {
		                  ++completion->calls;
		                  completion->result = result;
		                  if (result.delegation_state == uam::ManagedInitialDelegationState::Committed)
		                  {
			                  std::string stop_error;
			                  (void)uam::AgentRunScheduler::CancelTree(fixture.app, result.run_id, &stop_error);
		                  }
		                  return true;
	                  },
	                  &error),
	              uam::ManagedInitialMentionAdmission::Pending);
	fixture.app.settings.show_provider_icons_in_sidebar = !fixture.app.settings.show_provider_icons_in_sidebar;
	InitialGuardAwait(fixture.app, isolated, [&] { return completion->result.has_value(); });
	UAM_ASSERT_EQ(completion->calls, 1);
	UAM_ASSERT(completion->result->input_recorded);
	UAM_ASSERT_EQ(completion->result->delegation_state, uam::ManagedInitialDelegationState::Committed);
	const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(saved && saved->messages.size() == prefix.size() + 1);
	UAM_ASSERT_EQ(std::ranges::count(saved->messages, text, &Message::content), 1);
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
namespace
{
	struct RemainingInitialWriterFixture
	{
		TempDir native_home{"uam-managed-initial-remaining-private"};
		ScopedEnvVar claude_config{"CLAUDE_CONFIG_DIR", (native_home.root / "claude").string()};
		ManagedClaudeLifecycleFixture fixture{false};
		InitialGuardCleanup cleanup{fixture.app};
		RemainingInitialWriterFixture()
		{
			fs::create_directories(native_home.root / "claude");
		}
		void Isolated() const
		{
			fixture.AssertPeerFiles();
			UAM_ASSERT(!fs::exists(fixture.temp.root / "launch-witness.json") && !fs::exists(fixture.temp.root / "task-inputs.jsonl"));
			for (const auto& terminal : fixture.app.cli_terminals)
				UAM_ASSERT(terminal && !terminal->running && terminal->child_pid <= 0 && !terminal->input_writer);
		}
		std::string CreateUserRootBranch()
		{
			const std::string ancestor_id = fixture.root_id;
			UAM_ASSERT(!uam::paths::IsGitWorktreeIsolated(InitialGuardRoot(fixture.app, ancestor_id)));
			UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(fixture.app, ancestor_id, 0));
			const auto* selected = ChatDomainService().SelectedChat(fixture.app);
			UAM_ASSERT(selected && selected->id != ancestor_id && selected->agent_run_id.empty());
			const std::string branch_id = selected->id;
			UAM_ASSERT_EQ(selected->parent_chat_id, ancestor_id);
			UAM_ASSERT_EQ(selected->branch_root_chat_id, ancestor_id);
			UAM_ASSERT_EQ(selected->branch_from_message_index, 0);
			UAM_ASSERT(selected->managed_followup_incarnation.empty() && selected->native_session_id.empty());
			const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, branch_id, 4 * 1024 * 1024);
			UAM_ASSERT(saved && saved->messages == selected->messages && saved->parent_chat_id == ancestor_id);
			Isolated(); // Public branching may record an inert CLI-view launch intent, never a process.
			return branch_id;
		}
		void EnableFixtureMemory()
		{
			auto& root = InitialGuardRoot(fixture.app, fixture.root_id);
			root.memory_enabled = true;
			root.memory_level = "open";
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, root));
			UAM_ASSERT(fixture.app.memory_extraction_tasks.empty() && fixture.app.memory_extraction_queue.empty());
		}
	};
} // namespace

UAM_TEST(ManagedInitialUnconfirmedRootDefersTerminalHandoffWithoutBlockingOrdinaryHandoff)
{
	RemainingInitialWriterFixture owner;
	auto& fixture = owner.fixture;
	const auto isolated = [&] { owner.Isolated(); };
	InitialGuardBindRoot(fixture.app, fixture.root_id, fixture.definition.id, isolated);
	const auto prefix = InitialGuardRoot(fixture.app, fixture.root_id).messages;
	const auto birth = InitialGuardRoot(fixture.app, fixture.root_id).managed_followup_incarnation;
	std::string error;
	UAM_ASSERT(uam::PrepareAcpSessionForCliTerminalLaunch(fixture.app, InitialGuardRoot(fixture.app, fixture.root_id), &error));
	UAM_ASSERT(error.empty());
	owner.Isolated();
	const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(saved && saved->messages == prefix && saved->managed_followup_incarnation == birth);
	const auto uncertain = InitialGuardUnconfirmedRoot(fixture.app, fixture.root_id, fixture.definition.id, isolated);
	// Eligibility RED only: this test never calls terminal launch or creates a provider process.
	UAM_ASSERT(!uam::PrepareAcpSessionForCliTerminalLaunch(fixture.app, InitialGuardRoot(fixture.app, fixture.root_id), &error));
	InitialGuardPreserved(fixture.app, fixture.root_id, uncertain);
	owner.Isolated();
}

UAM_TEST(ManagedInitialUnconfirmedBranchDefersAncestorDeletionWithoutBlockingOrdinaryDelete)
{
	// Independent ordinary control: actual ancestor deletion reparents the genuine bound UserRoot.
	{
		RemainingInitialWriterFixture owner;
		auto& fixture = owner.fixture;
		const auto isolated = [&] { owner.Isolated(); };
		const auto branch_id = owner.CreateUserRootBranch();
		InitialGuardBindRoot(fixture.app, branch_id, fixture.definition.id, isolated);
		const auto prefix = InitialGuardRoot(fixture.app, branch_id).messages;
		const auto birth = InitialGuardRoot(fixture.app, branch_id).managed_followup_incarnation;
		UAM_ASSERT(RemoveChatsByIds(fixture.app, {fixture.root_id}));
		UAM_ASSERT(ChatDomainService().FindChatById(fixture.app, fixture.root_id) == nullptr);
		const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, branch_id, 4 * 1024 * 1024);
		UAM_ASSERT(saved && saved->messages == prefix && saved->managed_followup_incarnation == birth);
		UAM_ASSERT(saved->parent_chat_id.empty() && saved->branch_root_chat_id == branch_id && saved->branch_from_message_index == -1);
		UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id)));
		owner.Isolated();
	}
	{
		RemainingInitialWriterFixture owner;
		auto& fixture = owner.fixture;
		const auto isolated = [&] { owner.Isolated(); };
		const auto branch_id = owner.CreateUserRootBranch();
		InitialGuardBindRoot(fixture.app, branch_id, fixture.definition.id, isolated);
		const auto uncertain = InitialGuardUnconfirmedRoot(fixture.app, branch_id, fixture.definition.id, isolated);
		const auto ancestor_path = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id);
		const auto ancestor_backup = uam::io::MakeBackupPath(ancestor_path);
		const auto ancestor_bytes = ReadFile(ancestor_path);
		const std::optional<std::string> backup = fs::exists(ancestor_backup) ? std::optional<std::string>{ReadFile(ancestor_backup)} : std::nullopt;
		// Actual synchronous public production ordering: Prepare -> Stage -> Commit, with no owner yield.
		const bool removed = RemoveChatsByIds(fixture.app, {fixture.root_id});
		const auto observed = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, branch_id, 4 * 1024 * 1024);
		if (removed)
			throw TestFailure("Ancestor deletion accepted an unconfirmed surviving UserRoot; request retained=" + std::to_string(observed && std::ranges::count(observed->messages, uncertain.text, &Message::content) == 1));
		InitialGuardPreserved(fixture.app, branch_id, uncertain);
		UAM_ASSERT(ChatDomainService().FindChatById(fixture.app, fixture.root_id) != nullptr);
		const auto& branch = InitialGuardRoot(fixture.app, branch_id);
		UAM_ASSERT_EQ(branch.parent_chat_id, fixture.root_id);
		UAM_ASSERT_EQ(branch.branch_root_chat_id, fixture.root_id);
		UAM_ASSERT_EQ(branch.branch_from_message_index, 0);
		UAM_ASSERT_EQ(ReadFile(ancestor_path), ancestor_bytes);
		UAM_ASSERT_EQ(fs::exists(ancestor_backup), backup.has_value());
		if (backup)
			UAM_ASSERT_EQ(ReadFile(ancestor_backup), *backup);
		owner.Isolated();
	}
}

UAM_TEST(ManagedInitialUnconfirmedRootDefersPreparedMemoryProgressWithoutBlockingOrdinaryProgress)
{
	const std::string output = R"({"memories":[]})";
	// Real opaque preparation and final publication, with no model/AI worker or memory entries.
	{
		RemainingInitialWriterFixture owner;
		auto& fixture = owner.fixture;
		owner.EnableFixtureMemory();
		const auto isolated = [&] { owner.Isolated(); };
		InitialGuardBindRoot(fixture.app, fixture.root_id, fixture.definition.id, isolated);
		auto& root = InitialGuardRoot(fixture.app, fixture.root_id);
		const auto prefix = root.messages;
		const auto birth = root.managed_followup_incarnation;
		const fs::path workspace = root.workspace_directory;
		const int count = static_cast<int>(prefix.size());
		UAM_ASSERT(count > 0);
		std::string error;
		const auto prepared = MemoryService::PrepareWorkerOutput(fixture.app.data_root, root, workspace, output, count, &error);
		if (!prepared)
			throw TestFailure("Ordinary prepared memory prerequisite: " + error.substr(0, 512));
		UAM_ASSERT(MemoryService::PublishPreparedWorkerOutput(fixture.app, root, workspace, prepared, &error));
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id);
		UAM_ASSERT(saved && saved->messages == prefix && saved->managed_followup_incarnation == birth);
		UAM_ASSERT_EQ(saved->memory_last_processed_message_count, count);
		UAM_ASSERT(!saved->memory_last_processed_at.empty());
		UAM_ASSERT(fixture.app.memory_extraction_tasks.empty() && fixture.app.memory_extraction_queue.empty());
		owner.Isolated();
	}
	{
		RemainingInitialWriterFixture owner;
		auto& fixture = owner.fixture;
		owner.EnableFixtureMemory();
		const auto isolated = [&] { owner.Isolated(); };
		InitialGuardBindRoot(fixture.app, fixture.root_id, fixture.definition.id, isolated);
		auto& before = InitialGuardRoot(fixture.app, fixture.root_id);
		const fs::path workspace = before.workspace_directory;
		const int count = static_cast<int>(before.messages.size());
		const auto old_count = before.memory_last_processed_message_count;
		const auto old_time = before.memory_last_processed_at;
		std::string error;
		const auto prepared = MemoryService::PrepareWorkerOutput(fixture.app.data_root, before, workspace, output, count, &error);
		if (!prepared)
			throw TestFailure("Already-prepared memory prerequisite: " + error.substr(0, 512));
		const auto uncertain = InitialGuardUnconfirmedRoot(fixture.app, fixture.root_id, fixture.definition.id, isolated);
		const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id);
		const auto summary = nlohmann::json::parse(ReadFile(AppPaths::UamChatSummaryFilePath(fixture.app.data_root, fixture.root_id)));
		// The real ROOT exchange skipped its secondary publication; ordinary outbox progress needs full-save fallback.
		UAM_ASSERT(summary.at("summary_source_size").get<std::uintmax_t>() != fs::file_size(primary));
		const bool published = MemoryService::PublishPreparedWorkerOutput(fixture.app, InitialGuardRoot(fixture.app, fixture.root_id), workspace, prepared, &error);
		const auto observed = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		if (published)
			throw TestFailure("Prepared memory progress accepted unconfirmed ROOT; request retained=" + std::to_string(observed && std::ranges::count(observed->messages, uncertain.text, &Message::content) == 1));
		InitialGuardPreserved(fixture.app, fixture.root_id, uncertain);
		UAM_ASSERT_EQ(InitialGuardRoot(fixture.app, fixture.root_id).memory_last_processed_message_count, old_count);
		UAM_ASSERT_EQ(InitialGuardRoot(fixture.app, fixture.root_id).memory_last_processed_at, old_time);
		UAM_ASSERT(fixture.app.memory_extraction_tasks.empty() && fixture.app.memory_extraction_queue.empty());
		owner.Isolated();
	}
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
#include "app/markdown_store_service.h"
UAM_TEST(ManagedInitialExecutionSelectorChangesRejectCapturedRequest)
{
	for (int changed = 0; changed < 3; ++changed)
	{
		ManagedClaudeLifecycleFixture fixture(false);
		InitialGuardCleanup cleanup{fixture.app};
		const auto isolated = [&] { fixture.AssertPeerFiles(); };
		fixture.app.settings.permission_reviewer_provider_id = "claude-cli";
		fixture.app.settings.permission_reviewer_model_id = "captured-review-model";
		std::string next_store;
		if (changed == 2)
		{
			const auto first = fixture.temp.root / "first-private-skill-store";
			const auto second = fixture.temp.root / "second-private-skill-store";
			fs::create_directories(first);
			fs::create_directories(second);
			MarkdownStoreService::Draft draft;
			draft.title = "Captured Review Skill";
			draft.body = "First private captured skill body.";
			MarkdownStoreService::Entry first_entry, second_entry;
			std::string store_error;
			UAM_ASSERT(MarkdownStoreService::CreateEntry(first, draft, &first_entry, &store_error));
			draft.body = "Different private skill body behind the same selected name.";
			UAM_ASSERT(MarkdownStoreService::CreateEntry(second, draft, &second_entry, &store_error));
			UAM_ASSERT(!first_entry.command_name.empty() && first_entry.command_name == second_entry.command_name);
			UAM_ASSERT(first_entry.body != second_entry.body);
			fixture.definition.skills = {first_entry.command_name};
			UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(fixture.app.data_root, fixture.definition, &store_error));
			fixture.app.settings.central_provider_configuration.enabled = true;
			fixture.app.settings.markdown_store_directory = first.string();
			next_store = second.string();
		}
		const auto prefix = InitialGuardRoot(fixture.app, fixture.root_id).messages;
		const std::string text = "@" + fixture.definition.id + " Do not record this request after execution inputs change.";
		auto completion = std::make_shared<InitialGuardCompletion>();
		std::string error;
		UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
		                  fixture.app, fixture.root_id, text,
		                  [&, completion](const uam::ManagedInitialMentionResult& result)
		                  {
			                  ++completion->calls;
			                  completion->result = result;
			                  if (result.delegation_state == uam::ManagedInitialDelegationState::Committed)
			                  {
				                  std::string stop_error;
				                  (void)uam::AgentRunScheduler::CancelTree(fixture.app, result.run_id, &stop_error);
			                  }
			                  return true;
		                  },
		                  &error),
		              uam::ManagedInitialMentionAdmission::Pending);
		if (changed == 0)
			fixture.app.settings.permission_reviewer_provider_id = "codex-cli";
		else if (changed == 1)
			fixture.app.settings.permission_reviewer_model_id = "different-review-model";
		else
			fixture.app.settings.markdown_store_directory = next_store;
		InitialGuardAwait(fixture.app, isolated, [&] { return completion->result.has_value(); });
		UAM_ASSERT_EQ(completion->calls, 1);
		UAM_ASSERT(!completion->result->input_recorded);
		UAM_ASSERT_EQ(completion->result->delegation_state, uam::ManagedInitialDelegationState::Rejected);
		UAM_ASSERT(completion->result->run_id.empty() && completion->result->transcript_chat_id.empty());
		const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		UAM_ASSERT(saved && saved->messages == prefix);
		UAM_ASSERT(fixture.app.agent_runs.empty() && fixture.app.acp_sessions.empty());
	}
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
UAM_TEST(ManagedInitialUnconfirmedParentResultRejectsQuietSave)
{
	ManagedClaudeLifecycleFixture fixture(false);
	InitialGuardCleanup cleanup{fixture.app};
	const auto isolated = [&] { fixture.AssertPeerFiles(); };
	auto completion = std::make_shared<InitialGuardCompletion>();
	const std::string text = "@" + fixture.definition.id + " Record one genuine request before testing its cancellation result publication.";
	std::string error;
	UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
	                  fixture.app, fixture.root_id, text,
	                  [&, completion](const uam::ManagedInitialMentionResult& result)
	                  {
		                  ++completion->calls;
		                  completion->result = result;
		                  if (result.delegation_state == uam::ManagedInitialDelegationState::Committed)
		                  {
			                  std::string stop_error;
			                  (void)uam::AgentRunScheduler::CancelTree(fixture.app, result.run_id, &stop_error);
		                  }
		                  return true;
	                  },
	                  &error, {},
	                  [completion](ChatPublicationFile file, uam::io::AtomicWriteStage stage)
	                  {
		                  if (file == ChatPublicationFile::Primary && stage == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync)
		                  {
			                  ++completion->faults;
			                  return false;
		                  }
		                  return true;
	                  }),
	              uam::ManagedInitialMentionAdmission::Pending);
	InitialGuardAwait(fixture.app, isolated, [&] { return completion->faults == 1; });
	UAM_ASSERT_EQ(completion->calls, 1);
	UAM_ASSERT(completion->result && completion->result->input_recorded);
	UAM_ASSERT_EQ(completion->result->delegation_state, uam::ManagedInitialDelegationState::Committed);
	const auto& root = InitialGuardRoot(fixture.app, fixture.root_id);
	const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(saved && saved->managed_followup_incarnation == root.managed_followup_incarnation);
	UAM_ASSERT(uam::uuid::IsCanonicalUuid(root.managed_followup_incarnation));
	UAM_ASSERT_EQ(std::ranges::count(root.messages, text, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(saved->messages, text, &Message::content), 1);
	const std::string tool_id = "uam-agent-result-" + completion->result->run_id;
	const auto results = [&](const ChatSession& chat)
	{
		std::size_t count = 0;
		for (const auto& row : chat.messages)
			count += std::ranges::count(row.tool_calls, tool_id, &ToolCall::id);
		return count;
	};
	UAM_ASSERT_EQ(results(root), std::size_t{0});
	UAM_ASSERT_EQ(results(*saved), std::size_t{1});
	UAM_ASSERT_EQ(saved->messages.size(), root.messages.size() + 1);
	UAM_ASSERT(std::equal(root.messages.begin(), root.messages.end(), saved->messages.begin()));
	const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
	UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1);
	UAM_ASSERT_EQ(ledger.runs.front().status, std::string("cancelled"));
	UAM_ASSERT(!ledger.runs.front().root_result_delivered);
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id);
	const auto backup = uam::io::MakeBackupPath(primary);
	const std::string primary_before = ReadFile(primary);
	const auto backup_before = fs::exists(backup) ? std::optional<std::string>(ReadFile(backup)) : std::nullopt;
	// No further scheduler turn: assess the ordinary writer against the actual unknown result prefix.
	if (uam::acp_detail::SaveChatQuietly(fixture.app, root))
	{
		const auto after = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		throw TestFailure("Unconfirmed parent-result writer accepted; result retained=" + std::string(after && results(*after) == 1 ? "true" : "false"));
	}
	UAM_ASSERT(uam::AgentRunScheduler::HasUnconfirmedInitialRootPublication(fixture.app, root));
	UAM_ASSERT_EQ(ReadFile(primary), primary_before);
	UAM_ASSERT_EQ(fs::exists(backup), backup_before.has_value());
	if (backup_before)
		UAM_ASSERT_EQ(ReadFile(backup), *backup_before);
	UAM_ASSERT_EQ(results(InitialGuardRoot(fixture.app, fixture.root_id)), std::size_t{0});
	UAM_ASSERT_EQ(completion->calls, 1);
	constexpr std::uint64_t newer_revision = 501;
	InitialGuardRoot(fixture.app, fixture.root_id).managed_subagent_revision = newer_revision;
	uam::acp_detail::ScheduleChatSave(fixture.app, InitialGuardRoot(fixture.app, fixture.root_id), 0.0);
	InitialGuardAwait(fixture.app, isolated,
	                  [&]
	                  {
		                  const auto runs = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
		                  return runs.errors.empty() && runs.runs.size() == 1 && runs.runs.front().root_result_delivered && !fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.root_id);
	                  });
	UAM_ASSERT(InitialGuardRoot(fixture.app, fixture.root_id).managed_subagent_revision >= newer_revision);
	UAM_ASSERT_EQ(results(InitialGuardRoot(fixture.app, fixture.root_id)), std::size_t{1});
	UAM_ASSERT_EQ(completion->calls, 1);
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
UAM_TEST(ManagedInitialPublicationPreservesNewerLiveRevision)
{
	ManagedClaudeLifecycleFixture fixture(false);
	InitialGuardCleanup cleanup{fixture.app};
	auto completion = std::make_shared<InitialGuardCompletion>();
	std::string error;
	UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
	                  fixture.app, fixture.root_id, "@" + fixture.definition.id + " Preserve a newer live discovery revision.",
	                  [&, completion](const uam::ManagedInitialMentionResult& result)
	                  {
		                  ++completion->calls;
		                  completion->result = result;
		                  if (result.delegation_state == uam::ManagedInitialDelegationState::Committed)
		                  {
			                  std::string stop_error;
			                  (void)uam::AgentRunScheduler::CancelTree(fixture.app, result.run_id, &stop_error);
		                  }
		                  return true;
	                  },
	                  &error),
	              uam::ManagedInitialMentionAdmission::Pending);
	constexpr std::uint64_t newer_revision = 101;
	InitialGuardRoot(fixture.app, fixture.root_id).managed_subagent_revision = newer_revision;
	InitialGuardAwait(fixture.app, [&] { fixture.AssertPeerFiles(); }, [&] { return completion->result.has_value(); });
	UAM_ASSERT(completion->result->input_recorded);
	UAM_ASSERT_EQ(completion->result->delegation_state, uam::ManagedInitialDelegationState::Committed);
	UAM_ASSERT(InitialGuardRoot(fixture.app, fixture.root_id).managed_subagent_revision > newer_revision);
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
UAM_TEST(ManagedInitialRunningChildIgnoresParentModelChange)
{
	ManagedClaudeLifecycleFixture fixture(false);
	InitialGuardCleanup cleanup{fixture.app};
	std::optional<uam::ManagedInitialMentionResult> completion;
	std::string error;
	UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
	                  fixture.app, fixture.root_id, "@" + fixture.definition.id + " " + fixture.task,
	                  [&](const uam::ManagedInitialMentionResult& result)
	                  {
		                  completion = result;
		                  return true;
	                  },
	                  &error),
	              uam::ManagedInitialMentionAdmission::Pending);
	fixture.Await([&] { return completion.has_value(); });
	if (!completion->input_recorded || completion->delegation_state != uam::ManagedInitialDelegationState::Committed)
		throw TestFailure("Running-policy creation prerequisite: " + completion->error);
	fixture.run_id = completion->run_id;
	try
	{
		fixture.Await([&] { return fs::exists(fixture.temp.root / "input-witness.json"); });
	}
	catch (const std::exception& failure)
	{
		const auto run = fixture.ReadRun();
		const auto* session = uam::FindAcpSessionForChat(fixture.app, completion->transcript_chat_id);
		throw TestFailure(std::string("Running-policy input prerequisite: ") + failure.what() + " status=" + run.status + " diagnostic=" + run.diagnostic_code + ":" + run.diagnostic + " runtime=" + (session ? session->last_error : "missing"));
	}
	const auto input_before = ReadFile(fixture.temp.root / "task-inputs.jsonl");
	const auto* session_before = uam::FindAcpSessionForChat(fixture.app, completion->transcript_chat_id);
	UAM_ASSERT(session_before && session_before->running && session_before->processing);
	const auto process = session_before->last_process_id;
	InitialGuardRoot(fixture.app, fixture.root_id).model_id = "different-parent-model-after-child-input";
	UAM_ASSERT(uam::acp_detail::SaveChatQuietly(fixture.app, InitialGuardRoot(fixture.app, fixture.root_id)));
	for (int poll = 0; poll < 16; ++poll)
	{
		fixture.Pulse();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const auto* session = uam::FindAcpSessionForChat(fixture.app, completion->transcript_chat_id);
	UAM_ASSERT(session && session->running && session->processing && session->last_process_id == process);
	UAM_ASSERT(uam::AgentRunScheduler::InitialSessionOwned(fixture.app, *session, InitialGuardRoot(fixture.app, completion->transcript_chat_id)));
	UAM_ASSERT_EQ(fixture.ReadRun().status, std::string("running"));
	UAM_ASSERT_EQ(ReadFile(fixture.temp.root / "task-inputs.jsonl"), input_before);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-completion", "release"));
	fixture.Await(
	    [&]
	    {
		    const auto run = fixture.ReadRun();
		    return run.status == "completed" && run.root_result_delivered;
	    });
	UAM_ASSERT_EQ(ReadFile(fixture.temp.root / "task-inputs.jsonl"), input_before);
	UAM_ASSERT(fixture.StopAndReap());
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
UAM_TEST(ManagedInitialRootConfirmationPreservesNewerLiveRevision)
{
	ManagedClaudeLifecycleFixture fixture(false);
	InitialGuardCleanup cleanup{fixture.app};
	const auto isolated = [&] { fixture.AssertPeerFiles(); };
	InitialGuardBindRoot(fixture.app, fixture.root_id, fixture.definition.id, isolated);
	const auto pending = InitialGuardUnconfirmedRoot(fixture.app, fixture.root_id, fixture.definition.id, isolated);
	constexpr std::uint64_t newer_revision = 203;
	InitialGuardRoot(fixture.app, fixture.root_id).managed_subagent_revision = newer_revision;
	InitialGuardAwait(fixture.app, isolated, [&] { return !uam::AgentRunScheduler::HasUnconfirmedInitialRootPublication(fixture.app, InitialGuardRoot(fixture.app, fixture.root_id)); });
	UAM_ASSERT(InitialGuardRoot(fixture.app, fixture.root_id).managed_subagent_revision >= newer_revision);
	UAM_ASSERT_EQ(pending.completion->calls, 1);
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
UAM_TEST(ManagedInitialRealQueuedCrashSnapshotsStayInertWithoutBlockingLegacyAdmission)
{
	RemainingInitialWriterFixture owner;
	auto& fixture = owner.fixture;
	const auto isolated = [&] { owner.Isolated(); };
	struct QueuedSnapshot
	{
		std::string id, primary;
		std::optional<std::string> backup;
	};
	std::vector<QueuedSnapshot> snapshots;
	// Each snapshot is captured from the real committed creation callback BEFORE public cancellation.
	// No scheduler-owned status, owner, native identity or receipt is fabricated.
	for (int child = 0; child < 4; ++child)
	{
		auto completion = std::make_shared<InitialGuardCompletion>();
		std::exception_ptr callback_failure;
		std::string error;
		const std::string text = "@" + fixture.definition.id + " Capture genuine queued crash evidence " + std::to_string(child) + ".";
		UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
		                  fixture.app, fixture.root_id, text,
		                  [&, completion](const uam::ManagedInitialMentionResult& result)
		                  {
			                  ++completion->calls;
			                  completion->result = result;
			                  try
			                  {
				                  UAM_ASSERT(result.input_recorded && result.delegation_state == uam::ManagedInitialDelegationState::Committed);
				                  const auto inspected = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
				                  UAM_ASSERT(inspected.errors.empty());
				                  const auto run = std::ranges::find(inspected.runs, result.run_id, &AgentRun::id);
				                  UAM_ASSERT(run != inspected.runs.end() && run->managed_continuation);
				                  UAM_ASSERT_EQ(run->status, std::string("queued"));
				                  UAM_ASSERT_EQ(run->managed_continuation->turns.front().status, uam::ManagedRunTurnStatus::Queued);
				                  UAM_ASSERT(!run->managed_continuation->execution && !run->managed_continuation->turns.front().dispatch_attempted);
				                  const auto primary = fixture.app.data_root / "agent-runs" / (result.run_id + ".json");
				                  const auto backup = uam::io::MakeBackupPath(primary);
				                  snapshots.push_back({result.run_id, ReadFile(primary), fs::exists(backup) ? std::optional<std::string>{ReadFile(backup)} : std::nullopt});
				                  std::string stop_error;
				                  (void)uam::AgentRunScheduler::CancelTree(fixture.app, result.run_id, &stop_error);
			                  }
			                  catch (...)
			                  {
				                  callback_failure = std::current_exception();
				                  return false;
			                  }
			                  return true; // CancelTree already revoked the real owner before a queue/launch tick.
		                  },
		                  &error),
		              uam::ManagedInitialMentionAdmission::Pending);
		InitialGuardAwait(fixture.app, isolated,
		                  [&]
		                  {
			                  if (callback_failure)
				                  return true;
			                  if (!completion->result)
				                  return false;
			                  const auto inspected = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
			                  const auto run = std::ranges::find(inspected.runs, completion->result->run_id, &AgentRun::id);
			                  return inspected.errors.empty() && run != inspected.runs.end() && run->status == "cancelled" && run->root_result_delivered && !uam::AgentRunScheduler::HasPendingInitialMention(fixture.app, fixture.root_id);
		                  });
		if (callback_failure)
			std::rethrow_exception(callback_failure);
		UAM_ASSERT_EQ(completion->calls, 1);
		UAM_ASSERT_EQ(snapshots.size(), static_cast<std::size_t>(child + 1));
		UAM_ASSERT(fixture.app.acp_sessions.empty());
		owner.Isolated();
	}
	// A private crash-image directory contains the exact already durable queued bytes, not rewritten statuses.
	// It deliberately has no carried runtime owner or process. The original fixture was safely cancelled above.
	TempDir crash{"uam-managed-initial-real-queued-crash"};
	fs::create_directories(crash.root / "agent-runs");
	for (const auto& snapshot : snapshots)
	{
		const auto primary = crash.root / "agent-runs" / (snapshot.id + ".json");
		UAM_ASSERT(uam::io::WriteTextFile(primary, snapshot.primary));
		if (snapshot.backup)
			UAM_ASSERT(uam::io::WriteTextFile(uam::io::MakeBackupPath(primary), *snapshot.backup));
	}
	const auto preserve = [&]
	{
		const auto inspected = uam::AgentRunLedger::LoadAllReadOnly(crash.root);
		UAM_ASSERT(inspected.errors.empty());
		for (const auto& snapshot : snapshots)
		{
			const auto run = std::ranges::find(inspected.runs, snapshot.id, &AgentRun::id);
			UAM_ASSERT(run != inspected.runs.end() && run->managed_continuation && run->status == "queued");
			UAM_ASSERT_EQ(run->managed_continuation->turns.front().status, uam::ManagedRunTurnStatus::Queued);
			UAM_ASSERT(!run->managed_continuation->turns.front().dispatch_attempted);
			const auto primary = crash.root / "agent-runs" / (snapshot.id + ".json");
			const auto backup = uam::io::MakeBackupPath(primary);
			UAM_ASSERT_EQ(ReadFile(primary), snapshot.primary);
			UAM_ASSERT_EQ(fs::exists(backup), snapshot.backup.has_value());
			if (snapshot.backup)
				UAM_ASSERT_EQ(ReadFile(backup), *snapshot.backup);
		}
	};
	UAM_ASSERT_EQ(uam::AgentRunLedger::LoadAllReadOnly(crash.root).runs.size(), std::size_t{4});
	preserve();
	uam::AppState restarted;
	restarted.data_root = crash.root;
	auto startup = uam::AgentRunLedger::LoadAll(restarted.data_root);
	UAM_ASSERT(uam::AgentRunLedger::MarkNonterminalInterrupted(restarted.data_root, &startup.runs, "manager_restarted", &startup.errors));
	restarted.agent_runs = std::move(startup.runs); // Exact Application startup projection, never ReadOnly -> runtime.
	UAM_ASSERT(restarted.agent_runs.empty());
	UAM_ASSERT_EQ(startup.errors.size(), std::size_t{4});
	for (const auto& diagnostic : startup.errors)
		UAM_ASSERT(diagnostic.find("inert") != std::string::npos);
	UAM_ASSERT(restarted.managed_initial_runtime_owners.empty() && restarted.queued_agent_run_ids.empty() && restarted.acp_sessions.empty());
	preserve();
	// Genuine ordinary public admission remains available; never Poll it or launch a provider.
	restarted.settings = fixture.app.settings;
	restarted.provider_profiles = fixture.app.provider_profiles;
	const auto root = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(root && !root->managed_followup_incarnation.empty());
	restarted.chats.push_back(*root);
	UAM_ASSERT(ChatRepository::SaveChat(restarted.data_root, restarted.chats.front()));
	std::string error, ordinary_id;
	UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(restarted.data_root, fixture.definition, &error));
	UAM_ASSERT(uam::AgentRunScheduler::Enqueue(restarted, fixture.root_id, {}, fixture.definition.id, "Ordinary admission must remain available after inert startup exclusion.", &ordinary_id, &error));
	UAM_ASSERT(error.empty() && !ordinary_id.empty());
	UAM_ASSERT_EQ(restarted.agent_runs.size(), std::size_t{1});
	UAM_ASSERT(!restarted.agent_runs.front().managed_continuation && restarted.agent_runs.front().status == "queued");
	UAM_ASSERT_EQ(restarted.queued_agent_run_ids.size(), std::size_t{1});
	UAM_ASSERT_EQ(restarted.queued_agent_run_ids.front(), ordinary_id);
	UAM_ASSERT(restarted.acp_sessions.empty() && restarted.local_acp_launch_tasks.empty() && restarted.cli_terminals.empty());
	// The next exact startup pass keeps the legacy interruption contract and still excludes all four v2 primaries.
	auto legacy_startup = uam::AgentRunLedger::LoadAll(restarted.data_root);
	UAM_ASSERT_EQ(legacy_startup.runs.size(), std::size_t{1});
	UAM_ASSERT_EQ(legacy_startup.runs.front().id, ordinary_id);
	UAM_ASSERT(uam::AgentRunLedger::MarkNonterminalInterrupted(restarted.data_root, &legacy_startup.runs, "manager_restarted", &legacy_startup.errors));
	UAM_ASSERT_EQ(legacy_startup.runs.front().status, std::string("interrupted"));
	UAM_ASSERT_EQ(legacy_startup.runs.front().diagnostic_code, std::string("manager_restarted"));
	preserve();
	owner.Isolated();
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
void InitialDeferredSaveBarrierCase(bool force)
{
	ManagedClaudeLifecycleFixture fixture(false);
	InitialGuardCleanup cleanup{fixture.app};
	const auto isolated = [&] { fixture.AssertPeerFiles(); };
	InitialGuardBindRoot(fixture.app, fixture.root_id, fixture.definition.id, isolated);
	uam::acp_detail::ScheduleChatSave(fixture.app, InitialGuardRoot(fixture.app, fixture.root_id), 0.0);
	(void)uam::FlushPendingChatSaves(fixture.app, force);
	InitialGuardAwait(fixture.app, isolated, [&] { return !fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.root_id); });
	const auto pending = InitialGuardUnconfirmedRoot(fixture.app, fixture.root_id, fixture.definition.id, isolated);
	uam::acp_detail::ScheduleChatSave(fixture.app, InitialGuardRoot(fixture.app, fixture.root_id), 0.0);
	(void)uam::FlushPendingChatSaves(fixture.app, force);
	const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	if (!saved || std::ranges::count(saved->messages, pending.text, &Message::content) != 1)
		throw TestFailure("Forced save barrier replaced the actual unconfirmed authored request.");
	InitialGuardPreserved(fixture.app, fixture.root_id, pending);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.root_id));
	InitialGuardAwait(fixture.app, isolated, [&] { return !uam::AgentRunScheduler::HasUnconfirmedInitialRootPublication(fixture.app, InitialGuardRoot(fixture.app, fixture.root_id)) && !fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.root_id); });
	const auto confirmed = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(confirmed && std::ranges::count(confirmed->messages, pending.text, &Message::content) == 1);
}
UAM_TEST(ManagedInitialUnconfirmedRootDefersForcedSaveBarrier)
{
	InitialDeferredSaveBarrierCase(true);
	InitialDeferredSaveBarrierCase(false);
}

#endif

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI && defined(__APPLE__)
#include <cerrno>
#include <csignal>
#include <set>
#include <sys/wait.h>

namespace
{
	std::string ManagedOpenCodeCasingPeer()
	{
		return R"UAM_OC_PEER(#!/usr/bin/python3
import json, os, pathlib, sys, time
root = pathlib.Path(__file__).resolve().parent
config = json.loads((root / 'peer-config.json').read_text())
data = pathlib.Path(config['data_root'])
native = 'ses_private_managed_model_casing'

def store(name, value):
    target = root / name
    temporary = root / (name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, sort_keys=True)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(target)

def document(path):
    assert path.is_file() and not path.is_symlink() and path.stat().st_size <= 4 * 1024 * 1024
    return json.loads(path.read_text())

def state():
    runs = [document(path) for path in (data / 'agent-runs').glob('*.json')]
    assert len(runs) == 1
    run = runs[0]
    assert run['version'] == 2 and run['rootChatId'] == config['root_id']
    managed = run['managedContinuation']
    assert len(managed['turns']) == 1 and managed['currentTurnId'] == run['id']
    turn = managed['turns'][0]
    assert run['status'] == turn['status'] == 'running' and turn['dispatchAttempted']
    parent = document(data / 'chats' / (run['rootChatId'] + '.json'))
    transcript = document(data / 'chats' / (run['transcriptChatId'] + '.json'))
    assert parent['managed_followup_incarnation'] == managed['owner']['rootBirthId']
    assert transcript['managed_followup_incarnation'] == managed['owner']['transcriptBirthId']
    assert transcript['agent_run_id'] == run['id']
    assert run['modelId'] == config['requested_model']
    assert managed['execution'] is None
    return run, turn, transcript

def options(current):
    models = [config['canonical_model'], 'private/baseline']
    if config['ambiguous']:
        models.insert(1, 'private/Glm-4.6')
    return [dict(id='model', category='model', type='select', currentValue=current,
        options=[dict(value=value, name=value) for value in models]),
        dict(id='mode', category='mode', type='select', currentValue=agent,
        options=[dict(value=agent, name='Private captured native agent')])]

def emit(value):
    print(json.dumps(value), flush=True)

def respond(request, result):
    emit(dict(jsonrpc='2.0', id=request['id'], result=result))

try:
    if sys.argv[1:] == ['--version']:
        store('version-witness.json', dict(pid=os.getpid(), executable=sys.argv[0]))
        print('1.18.35', flush=True)
        sys.exit(0)
    assert sys.argv[1:] == ['acp'], 'private native-agent launch only'
    adapter = pathlib.Path(os.environ['OPENCODE_CONFIG_DIR']).resolve()
    assert adapter.is_relative_to(data.resolve())
    settings = document(adapter / 'opencode.json')
    agent = settings['default_agent']
    assert settings['subagent_depth'] == 0 and settings['permission']['task'] == 'deny'
    assert (adapter / 'agent' / (agent + '.md')).is_file()
    run, turn, transcript = state()
    store('launch-witness.json', dict(pid=os.getpid(), args=sys.argv[1:], run=run, agent=agent))
    model = 'private/baseline'
    for line in sys.stdin:
        request = json.loads(line)
        method = request.get('method', '')
        with (root / 'requests.jsonl').open('a') as stream:
            stream.write(json.dumps(request, sort_keys=True) + '\n')
            stream.flush()
            os.fsync(stream.fileno())
        if method == 'initialize':
            respond(request, dict(protocolVersion=1,
                agentInfo=dict(name='opencode', title='Private OpenCode ACP', version='1.18.35'),
                agentCapabilities=dict(loadSession=True, mcpCapabilities=dict(http=True, sse=True))))
            store('initialize-ack.json', dict(id=request['id']))
        elif method == 'session/new':
            assert pathlib.Path(request['params']['cwd']).resolve() == pathlib.Path(config['workspace']).resolve()
            store('native-ack.json', dict(id=request['id'], native_id=native))
            respond(request, dict(sessionId=native, configOptions=options(model)))
        elif method == 'session/set_model':
            assert not config['ambiguous'], 'ambiguous model must be refused without selecting one'
            assert request['params']['sessionId'] == native
            assert request['params']['modelId'] == config['canonical_model']
            model = config['canonical_model']
            store('model-ack.json', dict(id=request['id'], native_id=native, model=model))
            respond(request, dict(configOptions=options(model)))
        elif method == 'session/set_mode':
            assert request['params']['sessionId'] == native and request['params']['modeId'] == agent
            respond(request, dict(configOptions=options(model)))
        elif method == 'session/prompt':
            assert not config['ambiguous']
            assert request['params']['sessionId'] == native and model == config['canonical_model']
            run, turn, transcript = state()
            assert transcript['native_session_id'] == native
            assert transcript['model_id'] == config['canonical_model']
            assert turn['expectedTurnSerial'] > 0 and run['expectedTurnSerial'] == turn['expectedTurnSerial']
            row = transcript['messages'][turn['firstUserMessageIndex']]
            assert row['role'] == 'user' and row['content'] == config['task']
            assert row['created_at'] == turn['userMessageCreatedAt'] and not row.get('acp_prompt_not_sent', False)
            prompt = request['params']['prompt']
            assert sum(part.get('text', '').count(config['task']) for part in prompt) == 1
            with (root / 'task-inputs.jsonl').open('a') as stream:
                stream.write(json.dumps(request, sort_keys=True) + '\n')
                stream.flush()
                os.fsync(stream.fileno())
            store('input-witness.json', dict(pid=os.getpid(), run=run, turn=turn, transcript=transcript, request=request))
            deadline = time.monotonic() + 45
            while not (root / 'allow-completion').exists():
                assert time.monotonic() < deadline
                time.sleep(0.005)
            emit(dict(jsonrpc='2.0', method='session/update', params=dict(sessionId=native,
                update=dict(sessionUpdate='agent_message_chunk', content=dict(type='text', text=config['answer'])))))
            respond(request, dict(stopReason='end_turn'))
        elif method == 'session/cancel':
            if 'id' in request:
                respond(request, {})
        else:
            raise AssertionError('unexpected private ACP request: ' + method)
except Exception as error:
    store('peer-error.json', dict(error=str(error)))
    sys.exit(70)
)UAM_OC_PEER";
	}

	struct ManagedOpenCodeCasingFixture
	{
		TempDir temp{"uam-managed-opencode-casing"};
		ScopedEnvVar private_path{"PATH", (temp.root / "bin").string()};
		ScopedEnvVar private_config{"XDG_CONFIG_HOME", (temp.root / "private-config").string()};
		ScopedEnvVar private_inline{"OPENCODE_CONFIG_CONTENT", "{}"};
		uam::AppState app;
		std::string root_id;
		const std::string canonical = "private/glm-4.6";
		const std::string task = "Return the single private OpenCode casing answer.";
		const std::string answer = "PRIVATE-MANAGED-OPENCODE-CASING-COMPLETE";
		const std::string requested;
		uam::AgentDefinition definition;
		std::set<pid_t> pids;
		fs::path Executable() const
		{
			return temp.root / "bin" / "managed-opencode-peer";
		}
		void AssertIsolated() const
		{
			const char* path = std::getenv("PATH");
			UAM_ASSERT(path && std::string(path) == (temp.root / "bin").string());
			for (const auto& file : {Executable(), temp.root / "peer.py"})
			{
				UAM_ASSERT(fs::is_regular_file(file) && !fs::is_symlink(file));
				UAM_ASSERT((fs::status(file).permissions() & fs::perms::owner_exec) != fs::perms::none);
			}
		}
		ManagedOpenCodeCasingFixture(bool exact, bool ambiguous) : requested(exact ? "private/glm-4.6" : "private/GLM-4.6")
		{
			UAM_ASSERT(fs::is_regular_file("/usr/bin/python3"));
			fs::create_directories(temp.root / "bin");
			fs::create_directories(temp.root / "private-config");
			const auto executable = [&](const fs::path& file, const std::string& text)
			{
				UAM_ASSERT(uam::io::WriteTextFile(file, text));
				fs::permissions(file, fs::perms::owner_all, fs::perm_options::replace);
			};
			executable(temp.root / "peer.py", ManagedOpenCodeCasingPeer());
			executable(Executable(), "#!/bin/sh\nexec /usr/bin/python3 -I -S " + ShellQuoteForTest((temp.root / "peer.py").string()) + " \"$@\"\n");
			app.data_root = temp.root / "data";
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.settings.central_provider_configuration.enabled = false;
			app.settings.memory_enabled_default = false;
			app.settings.memory_level_default = "off";
			app.settings.memory_recall_budget_bytes = 0;
			app.settings.git_enabled = app.settings.git_auto_worktree = false;
			app.settings.computer_use_globally_enabled = false; // No MCP tool execution in this private protocol peer.
			app.provider_profiles = {ProviderProfileStore::DefaultOpenCodeProfile()};
			app.provider_profiles.front().interactive_command = ShellQuoteForTest(Executable().string());
			const auto workspace = temp.root / "workspace";
			fs::create_directories(workspace);
			definition.id = "private-casing-reviewer";
			definition.description = "Private model casing peer";
			definition.mode = "subagent";
			definition.workspace_access = "read";
			definition.instructions = "Read only; return the private answer without tools or delegates.";
			std::string error;
			UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(app.data_root, definition, &error));
			const auto catalog = uam::AgentDefinitionService::Load(app.data_root, workspace);
			const auto selected = std::ranges::find(catalog.definitions, definition.id, &uam::AgentDefinition::id);
			UAM_ASSERT(selected != catalog.definitions.end());
			definition = *selected;
			ChatSession root = ChatDomainService().CreateNewChat({}, "opencode-cli");
			root_id = root.id;
			root.execution_host_id = "local";
			root.routing_mode = "explicit";
			root.workspace_directory = workspace.string();
			root.model_id = requested;
			root.memory_enabled = false;
			root.memory_level = "off";
			root.uam_control_enabled = false;
			root.uam_control_explicitly_disabled = true;
			root.reasoning_effort.clear();
			root.service_tier.clear();
			root.service_tier_explicit = false;
			ChatDomainService().AddMessage(root, MessageRole::User, "Original parent request.");
			UAM_ASSERT(root.metadata_incarnation && root.native_session_id.empty() && root.managed_followup_incarnation.empty());
			UAM_ASSERT(ProviderResolutionService().ChatProviderIsAvailable(app, root));
			app.chats.push_back(std::move(root));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer-config.json", nlohmann::json{{"data_root", app.data_root.string()}, {"root_id", root_id}, {"workspace", workspace.string()}, {"requested_model", requested}, {"canonical_model", canonical}, {"ambiguous", ambiguous}, {"task", task}, {"answer", answer}}.dump()));
			AssertIsolated();
		}
		AgentRun ReadRun() const
		{
			const auto loaded = uam::AgentRunLedger::LoadAllReadOnly(app.data_root);
			UAM_ASSERT(loaded.errors.empty() && loaded.runs.size() == 1);
			return loaded.runs.front();
		}
		void Pulse()
		{
			AssertIsolated();
			for (const auto& session : app.acp_sessions)
				if (session && session->child_pid > 0)
					pids.insert(session->child_pid);
			(void)uam::PollAllAcpSessions(app);
			(void)uam::AgentRunScheduler::Poll(app);
			(void)uam::FlushPendingChatSaves(app);
			for (const auto& session : app.acp_sessions)
				if (session && session->child_pid > 0)
					pids.insert(session->child_pid);
			if (fs::exists(temp.root / "peer-error.json"))
				throw TestFailure("Private OpenCode prerequisite: " + ReadFile(temp.root / "peer-error.json").substr(0, 512));
		}
		template <class Predicate> void Await(Predicate done)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!done() && std::chrono::steady_clock::now() < deadline)
			{
				Pulse();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(done());
		}
		std::size_t PromptCount() const
		{
			if (!fs::exists(temp.root / "task-inputs.jsonl"))
				return 0;
			std::istringstream input(ReadFile(temp.root / "task-inputs.jsonl"));
			std::size_t count = 0;
			std::string line;
			while (std::getline(input, line))
				if (!line.empty())
				{
					UAM_ASSERT(nlohmann::json::parse(line).at("method") == "session/prompt");
					++count;
				}
			return count;
		}
		bool StopAndReap()
		{
			for (const auto& session : app.acp_sessions)
				if (session && session->child_pid > 0)
					pids.insert(session->child_pid);
			(void)uam::AgentRunScheduler::QuiesceInitialForShutdown(app);
			uam::FastStopAcpSessionsForExit(app);
			auto& process = PlatformServicesFactory::Instance().process_service;
			for (const auto& session : app.acp_sessions)
				if (session)
				{
					process.StopStdioProcess(*session, true);
					process.CloseStdioProcessHandles(*session);
				}
			uam::ResetAsyncCommandTask(app.runtime_cli_version_check_task);
			bool reaped = true;
			for (const pid_t pid : pids)
			{
				int status = 0;
				errno = 0;
				const pid_t result = waitpid(pid, &status, WNOHANG);
				if (result == pid)
					continue;
				if (result < 0 && errno == ECHILD)
				{
					errno = 0;
					if (kill(pid, 0) < 0 && errno == ESRCH)
						continue;
				}
				reaped = false;
			}
			return reaped;
		}
		~ManagedOpenCodeCasingFixture()
		{
			if (!StopAndReap())
				std::cerr << "Private OpenCode cleanup could not confirm reaping\n";
		}
	};

	void ManagedOpenCodeCasingCase(bool exact, bool ambiguous)
	{
		ManagedOpenCodeCasingFixture fixture(exact, ambiguous);
		struct Receipt
		{
			std::optional<uam::ManagedInitialMentionResult> result;
			int calls = 0;
		};
		auto receipt = std::make_shared<Receipt>();
		std::string error;
		const std::string text = "@" + fixture.definition.id + " " + fixture.task;
		UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
		                  fixture.app, fixture.root_id, text,
		                  [receipt](const uam::ManagedInitialMentionResult& result)
		                  {
			                  ++receipt->calls;
			                  receipt->result = result;
			                  return true;
		                  },
		                  &error),
		              uam::ManagedInitialMentionAdmission::Pending);
		fixture.Await([&] { return receipt->result.has_value(); });
		UAM_ASSERT_EQ(receipt->calls, 1);
		UAM_ASSERT(receipt->result->input_recorded);
		UAM_ASSERT_EQ(receipt->result->delegation_state, uam::ManagedInitialDelegationState::Committed);
		const auto initial = fixture.ReadRun();
		UAM_ASSERT_EQ(receipt->result->run_id, initial.id);
		UAM_ASSERT_EQ(initial.model_id, fixture.requested);
		UAM_ASSERT_EQ(initial.execution_capability, std::string("opencode-native-agent-config"));
		fixture.Await(
		    [&]
		    {
			    if (!fs::exists(fixture.temp.root / "native-ack.json"))
				    return false;
			    const auto* session = uam::FindAcpSessionForChat(fixture.app, initial.transcript_chat_id);
			    const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, initial.transcript_chat_id, 4 * 1024 * 1024);
			    return session && session->session_setup_request_id == 0 && saved && saved->native_session_id == "ses_private_managed_model_casing" && !session->pending_local_session_receipt;
		    });
		UAM_ASSERT(fs::exists(fixture.temp.root / "initialize-ack.json"));
		UAM_ASSERT(fs::exists(fixture.temp.root / "launch-witness.json"));
		if (ambiguous)
		{
			fixture.Await(
			    [&]
			    {
				    const auto run = fixture.ReadRun();
				    return run.status == "failed" && run.root_result_delivered;
			    });
			UAM_ASSERT_EQ(fixture.PromptCount(), std::size_t{0});
			UAM_ASSERT(!fs::exists(fixture.temp.root / "model-ack.json"));
			const auto run = fixture.ReadRun();
			UAM_ASSERT(run.diagnostic.find("matches multiple provider models") != std::string::npos);
			UAM_ASSERT(!run.managed_continuation->execution);
		}
		else
		{
			fixture.Await([&] { return fs::exists(fixture.temp.root / "input-witness.json") || fixture.ReadRun().status == "failed"; });
			// Intended alias RED: real native setup/model repair precedes absent task input.
			UAM_ASSERT(fs::exists(fixture.temp.root / "input-witness.json"));
			UAM_ASSERT(fs::exists(fixture.temp.root / "model-ack.json"));
			const auto* session = uam::FindAcpSessionForChat(fixture.app, initial.transcript_chat_id);
			UAM_ASSERT(session && session->startup_model_request_id == 0 && session->model_change_request_id == 0);
			UAM_ASSERT_EQ(session->current_model_id, fixture.canonical);
			UAM_ASSERT_EQ(fixture.PromptCount(), std::size_t{1});
			const auto witness = nlohmann::json::parse(ReadFile(fixture.temp.root / "input-witness.json"));
			UAM_ASSERT_EQ(witness.at("pid").get<pid_t>(), session->child_pid);
			UAM_ASSERT_EQ(witness.at("transcript").at("native_session_id").get<std::string>(), std::string("ses_private_managed_model_casing"));
			UAM_ASSERT_EQ(witness.at("transcript").at("model_id").get<std::string>(), fixture.canonical);
			UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-completion", "release actual private ACP result\n"));
			fixture.Await(
			    [&]
			    {
				    const auto run = fixture.ReadRun();
				    return run.status == "completed" && run.root_result_delivered;
			    });
			UAM_ASSERT_EQ(fixture.PromptCount(), std::size_t{1});
			const auto run = fixture.ReadRun();
			UAM_ASSERT_EQ(run.model_id, fixture.requested);
			UAM_ASSERT_EQ(run.managed_continuation->turns.size(), std::size_t{1});
			UAM_ASSERT_EQ(run.managed_continuation->turns.front().expected_turn_serial, run.expected_turn_serial);
			UAM_ASSERT(!run.managed_continuation->execution);
		}
		UAM_ASSERT_EQ(receipt->calls, 1);
		UAM_ASSERT(fixture.StopAndReap());
	}
} // namespace

UAM_TEST(ManagedInitialOpenCodeUniqueAdvertisedModelCasingDeliversOnce)
{
	ManagedOpenCodeCasingCase(false, false);
}
UAM_TEST(ManagedInitialOpenCodeExactAdvertisedModelDeliversOnce)
{
	ManagedOpenCodeCasingCase(true, false);
}
UAM_TEST(ManagedInitialOpenCodeAmbiguousAdvertisedModelRefusesInput)
{
	ManagedOpenCodeCasingCase(false, true);
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
UAM_TEST(ManagedInitialLateAncestorDeletionCommitRetainsUnknownBranchAndStagedTransaction)
{
	RemainingInitialWriterFixture owner;
	auto& fixture = owner.fixture;
	const auto isolated = [&] { owner.Isolated(); };
	const auto branch_id = owner.CreateUserRootBranch();
	InitialGuardBindRoot(fixture.app, branch_id, fixture.definition.id, isolated);
	UAM_ASSERT(!uam::AgentRunScheduler::HasUnconfirmedInitialRootPublication(fixture.app, InitialGuardRoot(fixture.app, branch_id)));
	// A real transaction is selected and staged while the survivor still has a confirmed prefix.
	uam::WorkspaceDeletionTask deletion;
	UAM_ASSERT(uam::PrepareChatDeletion(fixture.app, {fixture.root_id}, deletion));
	UAM_ASSERT(deletion.deleting_chats && deletion.deleted_ids.contains(fixture.root_id));
	UAM_ASSERT(!deletion.deleted_ids.contains(branch_id) && !deletion.commit_rejected && !deletion.metadata_saved);
	UAM_ASSERT(uam::StageWorkspaceDeletion(deletion));
	const auto intent = fixture.app.data_root / "deletion-transaction.json";
	const auto intent_backup = uam::io::MakeBackupPath(intent);
	const auto staged_ancestor = AppPaths::UamChatFilePath(fixture.app.data_root / ".deletion-transaction", fixture.root_id);
	const auto ancestor = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id);
	const auto ancestor_backup = uam::io::MakeBackupPath(ancestor);
	const std::string intent_bytes = ReadFile(intent), staged_bytes = ReadFile(staged_ancestor), ancestor_bytes = ReadFile(ancestor);
	const std::optional<std::string> intent_backup_bytes = fs::exists(intent_backup) ? std::optional<std::string>{ReadFile(intent_backup)} : std::nullopt;
	const std::optional<std::string> ancestor_backup_bytes = fs::exists(ancestor_backup) ? std::optional<std::string>{ReadFile(ancestor_backup)} : std::nullopt;
	const auto retained_keys = deletion.retained_native_keys;
	const auto retained_workspaces = deletion.retained_workspace_keys;
	const auto deleted_ids = deletion.deleted_ids;
	const auto folders = deletion.folder_ids;
	// Only now does a genuine second initial request produce the unknown private ROOT prefix.
	const auto uncertain = InitialGuardUnconfirmedRoot(fixture.app, branch_id, fixture.definition.id, isolated);
	const auto selected_id = ChatDomainService().SelectedChatId(fixture.app);
	const auto selected_index = fixture.app.selected_chat_index;
	const auto count = fixture.app.chats.size();
	const auto& branch_before = InitialGuardRoot(fixture.app, branch_id);
	const auto parent_id = branch_before.parent_chat_id;
	const auto branch_root = branch_before.branch_root_chat_id;
	const auto branch_index = branch_before.branch_from_message_index;
	UAM_ASSERT_EQ(parent_id, fixture.root_id);
	UAM_ASSERT_EQ(branch_root, fixture.root_id);
	UAM_ASSERT_EQ(branch_index, 0);
	UAM_ASSERT_EQ(selected_id, branch_id);
	uam::CommitWorkspaceDeletion(fixture.app, deletion);
	UAM_ASSERT(deletion.commit_rejected && !deletion.metadata_saved && !deletion.settings_saved);
	UAM_ASSERT(fixture.app.worktree_operation_chat_ids.contains(fixture.root_id));
	UAM_ASSERT_EQ(fixture.app.chats.size(), count);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selected_id);
	UAM_ASSERT_EQ(fixture.app.selected_chat_index, selected_index);
	UAM_ASSERT(ChatDomainService().FindChatById(fixture.app, fixture.root_id) != nullptr);
	const auto& branch_after = InitialGuardRoot(fixture.app, branch_id);
	UAM_ASSERT_EQ(branch_after.parent_chat_id, parent_id);
	UAM_ASSERT_EQ(branch_after.branch_root_chat_id, branch_root);
	UAM_ASSERT_EQ(branch_after.branch_from_message_index, branch_index);
	InitialGuardPreserved(fixture.app, branch_id, uncertain);
	UAM_ASSERT_EQ(deletion.deleted_ids, deleted_ids);
	UAM_ASSERT_EQ(deletion.folder_ids, folders);
	UAM_ASSERT_EQ(deletion.retained_native_keys, retained_keys);
	UAM_ASSERT_EQ(deletion.retained_workspace_keys, retained_workspaces);
	UAM_ASSERT_EQ(ReadFile(intent), intent_bytes);
	UAM_ASSERT_EQ(ReadFile(staged_ancestor), staged_bytes);
	UAM_ASSERT_EQ(ReadFile(ancestor), ancestor_bytes);
	UAM_ASSERT_EQ(fs::exists(intent_backup), intent_backup_bytes.has_value());
	UAM_ASSERT_EQ(fs::exists(ancestor_backup), ancestor_backup_bytes.has_value());
	if (intent_backup_bytes)
		UAM_ASSERT_EQ(ReadFile(intent_backup), *intent_backup_bytes);
	if (ancestor_backup_bytes)
		UAM_ASSERT_EQ(ReadFile(ancestor_backup), *ancestor_backup_bytes);
	// Existing cleanup cannot turn a rejected commit into deletion authority or discard its recovery evidence.
	UAM_ASSERT(!uam::CleanupWorkspaceDeletion(deletion));
	UAM_ASSERT_EQ(ReadFile(intent), intent_bytes);
	UAM_ASSERT_EQ(ReadFile(staged_ancestor), staged_bytes);
	UAM_ASSERT_EQ(ReadFile(ancestor), ancestor_bytes);
	InitialGuardPreserved(fixture.app, branch_id, uncertain);
	owner.Isolated();
	// Retain the transaction; no Recover/commit retry or manual deletion of evidence is performed.
	// Existing fixture RAII quiesces real initial work before its private temporary root is disposed.
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
namespace
{
	class RestartInspectionCallbackR91 final : public UamQueryHandler::Callback
	{
	  public:
		int successes = 0, failures = 0, status = 0;
		std::string error;
		nlohmann::json body;
		void Success(const CefString& value) override
		{
			++successes;
			body = nlohmann::json::parse(value.ToString());
		}
		void Success(const void*, size_t) override
		{
			++successes;
		}
		void Failure(int code, const CefString& value) override
		{
			++failures;
			status = code;
			error = value.ToString();
		}

	  private:
		IMPLEMENT_REFCOUNTING(RestartInspectionCallbackR91);
	};
	struct RestartInspectionFixtureR91
	{
		RemainingInitialWriterFixture owner;
		TempDir crash{"uam-managed-restart-inspection"};
		uam::AppState app;
		std::unique_ptr<UamQueryHandler> handler;
		struct Query
		{
			CefRefPtr<UamQueryHandler::Callback> callback;
			std::function<AsyncCefResult()> worker;
			std::function<void(AsyncCefResult&)> completion;
		};
		std::vector<Query> pending;
		AgentRun expected;
		std::map<fs::path, std::string> saved;
		std::string root_id;
		explicit RestartInspectionFixtureR91(bool queued)
		{
			auto& original = owner.fixture;
			root_id = original.root_id;
			const auto capture = [&](const std::string& run_id)
			{
				const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(original.app.data_root);
				UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1);
				expected = ledger.runs.front();
				UAM_ASSERT_EQ(expected.id, run_id);
				UAM_ASSERT(expected.managed_continuation && expected.managed_continuation->turns.size() == 1);
				const auto remember = [&](const fs::path& path)
				{
					saved.emplace(path.lexically_relative(original.app.data_root), ReadFile(path));
					const auto backup = uam::io::MakeBackupPath(path);
					if (fs::exists(backup))
						saved.emplace(backup.lexically_relative(original.app.data_root), ReadFile(backup));
				};
				remember(original.app.data_root / "agent-runs" / (run_id + ".json"));
				remember(AppPaths::UamChatFilePath(original.app.data_root, root_id));
				remember(AppPaths::UamChatFilePath(original.app.data_root, expected.transcript_chat_id));
			};
			if (!queued)
			{
				InitialGuardBindRoot(original.app, root_id, original.definition.id, [&] { owner.Isolated(); });
				const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(original.app.data_root);
				UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1);
				capture(ledger.runs.front().id);
				UAM_ASSERT_EQ(expected.status, std::string("cancelled"));
			}
			else
			{
				auto completion = std::make_shared<InitialGuardCompletion>();
				std::exception_ptr failure;
				std::string error;
				UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
				                  original.app, root_id, "@" + original.definition.id + " Preserve genuine queued restart history without replay.",
				                  [&, completion](const uam::ManagedInitialMentionResult& result)
				                  {
					                  ++completion->calls;
					                  completion->result = result;
					                  try
					                  {
						                  UAM_ASSERT(result.input_recorded && result.delegation_state == uam::ManagedInitialDelegationState::Committed);
						                  capture(result.run_id); // Exact durable queued bytes before public cancellation, never a rewritten status.
						                  UAM_ASSERT_EQ(expected.status, std::string("queued"));
						                  UAM_ASSERT(!expected.managed_continuation->turns.front().dispatch_attempted);
						                  std::string stop_error;
						                  (void)uam::AgentRunScheduler::CancelTree(original.app, result.run_id, &stop_error);
					                  }
					                  catch (...)
					                  {
						                  failure = std::current_exception();
						                  return false;
					                  }
					                  return true;
				                  },
				                  &error),
				              uam::ManagedInitialMentionAdmission::Pending);
				InitialGuardAwait(
				    original.app, [&] { owner.Isolated(); },
				    [&]
				    {
					    if (failure)
						    return true;
					    if (!completion->result)
						    return false;
					    const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(original.app.data_root);
					    return ledger.errors.empty() && ledger.runs.size() == 1 && ledger.runs.front().status == "cancelled" && ledger.runs.front().root_result_delivered && !uam::AgentRunScheduler::HasPendingInitialMention(original.app, root_id);
				    });
				if (failure)
					std::rethrow_exception(failure);
				UAM_ASSERT_EQ(completion->calls, 1);
			}
			owner.Isolated();
			for (const auto& [relative, bytes] : saved)
			{
				fs::create_directories((crash.root / relative).parent_path());
				UAM_ASSERT(uam::io::WriteTextFile(crash.root / relative, bytes));
			}
			app.data_root = crash.root;
			app.settings = original.app.settings;
			app.provider_profiles = original.app.provider_profiles;
			auto startup = uam::AgentRunLedger::LoadAll(app.data_root);
			UAM_ASSERT(uam::AgentRunLedger::MarkNonterminalInterrupted(app.data_root, &startup.runs, "manager_restarted", &startup.errors));
			app.agent_runs = std::move(startup.runs); // Application's actual runtime projection; no ReadOnly -> runtime assignment.
			UAM_ASSERT(app.agent_runs.empty());
			UAM_ASSERT(std::ranges::any_of(startup.errors, [](const auto& error) { return error.find("inert") != std::string::npos; }));
			ChatHistorySyncService().LoadSidebarChats(app); // Existing Application startup entry; future catalogue must initialize here, never via a fake setter.
			const auto* root = ChatDomainService().FindChatById(app, root_id);
			UAM_ASSERT(root && root->managed_followup_incarnation == expected.managed_continuation->owner.root_birth_id);
			const auto transcript = ChatRepository::LoadLocalChatBounded(app.data_root, expected.transcript_chat_id, 4 * 1024 * 1024);
			UAM_ASSERT(transcript && transcript->agent_run_id == expected.id);
			UAM_ASSERT_EQ(transcript->managed_followup_incarnation, expected.managed_continuation->owner.transcript_birth_id);
			UAM_ASSERT(!ChatDomainService().FindChatById(app, expected.transcript_chat_id));
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
			NativeMediaRefreshHandlerTestAccess::RestartInspectionDispatch(*handler,
			                                                               [this](auto callback, auto worker, auto completion)
			                                                               {
				                                                               pending.push_back({callback, std::move(worker), std::move(completion)});
				                                                               return true;
			                                                               });
			Unchanged();
		}
		CefRefPtr<RestartInspectionCallbackR91> Request(std::string_view action, const nlohmann::json& payload)
		{
			CefRefPtr<RestartInspectionCallbackR91> callback = new RestartInspectionCallbackR91;
			NativeMediaRefreshHandlerTestAccess::RestartInspectionAction(*handler, action, payload, callback);
			while (!pending.empty())
			{
				auto query = std::move(pending.front());
				pending.erase(pending.begin());
				auto result = std::async(std::launch::async, query.worker).get(); // Actual captured worker off owner, actual completion back on owner.
				CompleteAsyncCefQuery(query.callback, result, query.completion);
			}
			UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
			return callback;
		}
		void NoAuthority() const
		{
			UAM_ASSERT(app.agent_runs.empty() && app.queued_agent_run_ids.empty() && app.managed_initial_runtime_owners.empty());
			UAM_ASSERT(app.acp_sessions.empty() && app.cli_terminals.empty() && app.local_acp_launch_tasks.empty());
			UAM_ASSERT(app.managed_initial_mentions.empty() && app.pending_agent_run_cancellation_ids.empty() && app.managed_subagent_cancellation_receipts.empty());
			UAM_ASSERT(!ChatDomainService().FindChatById(app, expected.transcript_chat_id));
		}
		void Unchanged() const
		{
			for (const auto& [relative, bytes] : saved)
				UAM_ASSERT_EQ(ReadFile(crash.root / relative), bytes);
			const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(crash.root);
			UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1);
			UAM_ASSERT_EQ(ledger.runs.front().status, expected.status);
			UAM_ASSERT(ledger.runs.front().managed_continuation == expected.managed_continuation);
			NoAuthority();
			owner.Isolated();
		}
	};
} // namespace

UAM_TEST(ManagedInitialRestartListsRealCancelledHistoryWithoutRuntimeAdmission)
{
	RestartInspectionFixtureR91 fixture(false);
	const auto list = fixture.Request("listManagedSubagents", {{"chatId", fixture.root_id}});
	UAM_ASSERT_EQ(list->successes, 1);
	UAM_ASSERT_EQ(list->body.at("totalCount").get<std::size_t>(), std::size_t{1}); // Intended old-source missing-history RED.
	UAM_ASSERT_EQ(list->body.at("runs").size(), std::size_t{1});
	const auto& row = list->body.at("runs").front();
	UAM_ASSERT_EQ(row.at("runId").get<std::string>(), fixture.expected.id);
	UAM_ASSERT_EQ(row.at("transcriptChatId").get<std::string>(), fixture.expected.transcript_chat_id);
	UAM_ASSERT_EQ(row.at("status").get<std::string>(), std::string("cancelled"));
	UAM_ASSERT(row.at("lastReported").get<bool>() && !row.at("canCancel").get<bool>());
	fixture.Unchanged();
}

UAM_TEST(ManagedInitialRestartOpensRealCancelledTranscriptAndRefusesReboundBirth)
{
	RestartInspectionFixtureR91 fixture(false);
	const nlohmann::json payload = {{"chatId", fixture.root_id}, {"transcriptChatId", fixture.expected.transcript_chat_id}};
	const auto opened = fixture.Request("getManagedAgentTranscript", payload);
	UAM_ASSERT_EQ(opened->successes, 1); // Independent old-source 404 missing-history RED, not gated by list success.
	UAM_ASSERT_EQ(opened->body.at("runId").get<std::string>(), fixture.expected.id);
	UAM_ASSERT_EQ(opened->body.at("status").get<std::string>(), fixture.expected.status);
	UAM_ASSERT(opened->body.at("lastReported").get<bool>());
	const auto transcript = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.expected.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(transcript);
	UAM_ASSERT_EQ(opened->body.at("messages"), uam::StateSerializer::SerializeSession(*transcript).value("messages", nlohmann::json::array()));
	fixture.Unchanged();
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.expected.transcript_chat_id);
	const auto bytes = ReadFile(primary);
	auto rebound = nlohmann::json::parse(bytes);
	UAM_ASSERT(rebound.contains("managed_followup_incarnation"));
	rebound["managed_followup_incarnation"] = fixture.expected.managed_continuation->owner.root_birth_id; // Deliberately foreign existing canonical birth, never a grant.
	UAM_ASSERT(uam::io::WriteTextFile(primary, rebound.dump(2)));
	const auto rejected = fixture.Request("getManagedAgentTranscript", payload);
	UAM_ASSERT_EQ(rejected->successes, 0);
	UAM_ASSERT_EQ(rejected->failures, 1);
	UAM_ASSERT(!rejected->error.empty());
	fixture.NoAuthority();
	UAM_ASSERT(uam::io::WriteTextFile(primary, bytes)); // Restore only for survivor checks and teardown; no Retry or recapture.
	fixture.Unchanged();
}

UAM_TEST(ManagedInitialRestartQueuedCrashHistoryIsLastReportedWithoutControlOrMutation)
{
	RestartInspectionFixtureR91 fixture(true);
	const auto list = fixture.Request("listManagedSubagents", {{"chatId", fixture.root_id}});
	UAM_ASSERT_EQ(list->successes, 1);
	UAM_ASSERT_EQ(list->body.at("totalCount").get<std::size_t>(), std::size_t{1}); // Genuine captured queued primary must be inspectable, never runtime admission.
	const auto& row = list->body.at("runs").front();
	UAM_ASSERT_EQ(row.at("status").get<std::string>(), std::string("queued"));
	UAM_ASSERT(row.at("lastReported").get<bool>() && !row.at("canCancel").get<bool>());
	const auto opened = fixture.Request("getManagedAgentTranscript", {{"chatId", fixture.root_id}, {"transcriptChatId", fixture.expected.transcript_chat_id}});
	UAM_ASSERT_EQ(opened->successes, 1);
	UAM_ASSERT_EQ(opened->body.at("status").get<std::string>(), std::string("queued"));
	UAM_ASSERT(opened->body.at("lastReported").get<bool>() && opened->body.at("messages").is_array());
	const auto stopped = fixture.Request("cancelManagedSubagent", {{"chatId", fixture.root_id}, {"runId", fixture.expected.id}, {"controlScopeId", list->body.at("controlScopeId")}});
	UAM_ASSERT_EQ(stopped->successes, 0);
	UAM_ASSERT_EQ(stopped->failures, 1);
	std::string error, resumed;
	UAM_ASSERT(!uam::AgentRunScheduler::ResumeInterrupted(fixture.app, fixture.expected.id, &resumed, &error));
	UAM_ASSERT(resumed.empty());
	UAM_ASSERT(!uam::SendAcpPrompt(fixture.app, fixture.expected.transcript_chat_id, "Inspection must not grant execution.", {}, {}, false, &error));
	for (int poll = 0; poll < 3; ++poll)
		(void)uam::AgentRunScheduler::Poll(fixture.app);
	fixture.Unchanged();
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
UAM_TEST(ManagedInitialRestartProjectsSavedChildrenIntoBothSidebarStatesWithoutRuntimeAdmission)
{
	for (const bool queued : {false, true})
	{
		RestartInspectionFixtureR91 fixture(queued);
		const auto assert_count = [&](const nlohmann::json& state, std::size_t expected)
		{
			const auto& chats = state.at("chats");
			const auto root = std::ranges::find_if(chats, [&](const auto& row) { return row.value("id", "") == fixture.root_id; });
			UAM_ASSERT(root != chats.end());
			UAM_ASSERT_EQ(root->at("managedSubagentCount").template get<std::size_t>(), expected);
			UAM_ASSERT(std::ranges::none_of(chats, [&](const auto& row) { return row.value("id", "") == fixture.expected.transcript_chat_id; }));
		};
		assert_count(uam::StateSerializer::Serialize(fixture.app, true), 1);
		assert_count(uam::StateSerializer::SerializeFingerprint(fixture.app), 1);
		fixture.Unchanged();
		auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
		UAM_ASSERT(root);
		const auto birth = root->managed_followup_incarnation;
		root->managed_followup_incarnation = fixture.expected.managed_continuation->owner.transcript_birth_id;
		assert_count(uam::StateSerializer::Serialize(fixture.app, true), 0);
		assert_count(uam::StateSerializer::SerializeFingerprint(fixture.app), 0);
		root->managed_followup_incarnation = birth;
		fixture.Unchanged();
	}
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
UAM_TEST(ManagedInitialRestartHeldTranscriptCompletionRejectsChangedParentOwner)
{
	for (const std::string changed : {"birth", "incarnation", "data-root", "erasure"})
	{
		RestartInspectionFixtureR91 fixture(false);
		CefRefPtr<RestartInspectionCallbackR91> callback = new RestartInspectionCallbackR91;
		NativeMediaRefreshHandlerTestAccess::RestartInspectionAction(*fixture.handler, "getManagedAgentTranscript", {{"chatId", fixture.root_id}, {"transcriptChatId", fixture.expected.transcript_chat_id}}, callback);
		UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
		auto query = std::move(fixture.pending.front());
		fixture.pending.clear();
		auto result = std::async(std::launch::async, query.worker).get();
		if (!result.ok)
			throw TestFailure("Held history completion needs actual successful detached read first: " + result.error);
		UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
		const auto* current = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
		UAM_ASSERT(current);
		const ChatSession saved_parent = *current;
		const auto data_root = fixture.app.data_root;
		if (changed == "birth")
			InitialGuardRoot(fixture.app, fixture.root_id).managed_followup_incarnation = fixture.expected.managed_continuation->owner.transcript_birth_id;
		else if (changed == "incarnation")
			InitialGuardRoot(fixture.app, fixture.root_id).metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		else if (changed == "data-root")
			fixture.app.data_root = fixture.crash.root / "different-root";
		else
			std::erase_if(fixture.app.chats, [&](const auto& chat) { return chat.id == fixture.root_id; });
		CompleteAsyncCefQuery(query.callback, result, query.completion); // The actual held owner completion, never a replacement response.
		UAM_ASSERT_EQ(callback->successes, 0);
		UAM_ASSERT_EQ(callback->failures, 1);
		UAM_ASSERT_EQ(callback->status, 409);
		UAM_ASSERT(callback->body.is_null() && !callback->error.empty());
		fixture.NoAuthority();
		fixture.app.data_root = data_root;
		if (changed == "erasure")
			fixture.app.chats.push_back(saved_parent);
		else
			InitialGuardRoot(fixture.app, fixture.root_id) = saved_parent;
		fixture.Unchanged(); // RAM restoration is only for teardown/preservation, never execution or a recaptured request.
	}
}

UAM_TEST(ManagedInitialRestartTerminalHistoryCancelRefusesBeforeCreatingReceipt)
{
	RestartInspectionFixtureR91 fixture(false);
	UAM_ASSERT_EQ(fixture.expected.status, std::string("cancelled"));
	const auto listed = fixture.Request("listManagedSubagents", {{"chatId", fixture.root_id}});
	if (listed->successes != 1)
		throw TestFailure("Terminal history control needs successful genuine listing: " + listed->error);
	UAM_ASSERT_EQ(listed->body.at("totalCount").get<std::size_t>(), std::size_t{1});
	const auto& row = listed->body.at("runs").front();
	UAM_ASSERT(row.at("lastReported").get<bool>() && !row.at("canCancel").get<bool>());
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		const auto refused = fixture.Request("cancelManagedSubagent", {{"chatId", fixture.root_id}, {"runId", fixture.expected.id}, {"controlScopeId", listed->body.at("controlScopeId")}});
		UAM_ASSERT_EQ(refused->successes, 0);
		UAM_ASSERT_EQ(refused->failures, 1);
		UAM_ASSERT_EQ(refused->status, 409);
		UAM_ASSERT(!refused->error.empty());
		UAM_ASSERT(fixture.app.managed_subagent_cancellation_receipts.empty() && fixture.app.pending_agent_run_cancellation_ids.empty());
		(void)uam::AgentRunScheduler::Poll(fixture.app);
		fixture.Unchanged();
	}
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_RUNTIME_OPENCODE_CLI && defined(__APPLE__)
UAM_TEST(ManagedInitialRestartOpenCodeKeepsObservedCanonicalModelWithoutRuntimeAliasAuthority)
{
	UAM_ASSERT(cef_api_hash(CEF_API_VERSION, 0) != nullptr);
	ManagedOpenCodeCasingFixture original(false, false);
	auto completion = std::make_shared<InitialGuardCompletion>();
	std::string error;
	UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
	                  original.app, original.root_id, "@" + original.definition.id + " " + original.task,
	                  [completion](const uam::ManagedInitialMentionResult& result)
	                  {
		                  ++completion->calls;
		                  completion->result = result;
		                  return true;
	                  },
	                  &error),
	              uam::ManagedInitialMentionAdmission::Pending);
	original.Await([&] { return completion->result.has_value(); });
	UAM_ASSERT_EQ(completion->calls, 1);
	UAM_ASSERT(completion->result->input_recorded && completion->result->delegation_state == uam::ManagedInitialDelegationState::Committed);
	original.Await([&] { return fs::exists(original.temp.root / "input-witness.json") || original.ReadRun().status == "failed"; });
	UAM_ASSERT(fs::exists(original.temp.root / "native-ack.json") && fs::exists(original.temp.root / "model-ack.json") && fs::exists(original.temp.root / "input-witness.json"));
	UAM_ASSERT_EQ(original.PromptCount(), std::size_t{1});
	const auto witness = nlohmann::json::parse(ReadFile(original.temp.root / "input-witness.json"));
	UAM_ASSERT_EQ(witness.at("transcript").at("model_id").get<std::string>(), original.canonical);
	UAM_ASSERT(uam::io::WriteTextFile(original.temp.root / "allow-completion", "Release the actual private completed model-casing turn."));
	original.Await(
	    [&]
	    {
		    const auto run = original.ReadRun();
		    return run.status == "completed" && run.root_result_delivered;
	    });
	UAM_ASSERT(original.StopAndReap());
	const auto expected = original.ReadRun();
	UAM_ASSERT(expected.status == "completed" && expected.root_result_delivered);
	UAM_ASSERT_EQ(expected.model_id, original.requested);
	UAM_ASSERT(expected.model_id != original.canonical && expected.managed_continuation && !expected.managed_continuation->execution);
	TempDir crash{"uam-managed-opencode-real-restart-model"};
	std::map<fs::path, std::string> saved;
	for (const auto& source : {original.app.data_root / "agent-runs" / (expected.id + ".json"), AppPaths::UamChatFilePath(original.app.data_root, original.root_id), AppPaths::UamChatFilePath(original.app.data_root, expected.transcript_chat_id)})
	{
		for (const auto& path : {source, uam::io::MakeBackupPath(source)})
		{
			if (!fs::exists(path))
				continue;
			const auto relative = path.lexically_relative(original.app.data_root);
			saved.emplace(relative, ReadFile(path));
			fs::create_directories((crash.root / relative).parent_path());
			UAM_ASSERT(uam::io::WriteTextFile(crash.root / relative, saved.at(relative)));
		}
	}
	uam::AppState app;
	app.data_root = crash.root;
	app.settings = original.app.settings;
	app.provider_profiles = original.app.provider_profiles;
	auto startup = uam::AgentRunLedger::LoadAll(app.data_root);
	UAM_ASSERT(uam::AgentRunLedger::MarkNonterminalInterrupted(app.data_root, &startup.runs, "manager_restarted", &startup.errors));
	app.agent_runs = std::move(startup.runs);
	ChatHistorySyncService().LoadSidebarChats(app);
	const auto* root = ChatDomainService().FindChatById(app, original.root_id);
	UAM_ASSERT(root && root->managed_followup_incarnation == expected.managed_continuation->owner.root_birth_id);
	const auto transcript = ChatRepository::LoadLocalChatBounded(app.data_root, expected.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(transcript && transcript->managed_followup_incarnation == expected.managed_continuation->owner.transcript_birth_id);
	UAM_ASSERT_EQ(transcript->model_id, original.canonical);
	UAM_ASSERT_EQ(std::ranges::count(transcript->messages, original.answer, &Message::content), 1);
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	auto& handler = *handler_owner;
	std::vector<RestartInspectionFixtureR91::Query> pending;
	NativeMediaRefreshHandlerTestAccess::RestartInspectionDispatch(handler,
	                                                               [&](auto callback, auto worker, auto done)
	                                                               {
		                                                               pending.push_back({callback, std::move(worker), std::move(done)});
		                                                               return true;
	                                                               });
	const auto request = [&](std::string_view action, const nlohmann::json& payload)
	{
		CefRefPtr<RestartInspectionCallbackR91> callback = new RestartInspectionCallbackR91;
		NativeMediaRefreshHandlerTestAccess::RestartInspectionAction(handler, action, payload, callback);
		while (!pending.empty())
		{
			auto query = std::move(pending.front());
			pending.erase(pending.begin());
			auto result = std::async(std::launch::async, query.worker).get();
			CompleteAsyncCefQuery(query.callback, result, query.completion);
		}
		UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
		return callback;
	};
	const auto listed = request("listManagedSubagents", {{"chatId", original.root_id}});
	if (listed->successes != 1)
		throw TestFailure("Actual completed OpenCode restart needs readable canonical saved metadata: " + listed->error);
	UAM_ASSERT_EQ(listed->body.at("totalCount").get<std::size_t>(), std::size_t{1});
	const auto& row = listed->body.at("runs").front();
	UAM_ASSERT_EQ(row.at("modelId").get<std::string>(), original.requested); // Immutable requested spelling is preserved, not rewritten by inspection.
	UAM_ASSERT(row.at("lastReported").get<bool>() && !row.at("canCancel").get<bool>());
	const nlohmann::json payload = {{"chatId", original.root_id}, {"transcriptChatId", expected.transcript_chat_id}};
	const auto opened = request("getManagedAgentTranscript", payload);
	UAM_ASSERT_EQ(opened->successes, 1);
	UAM_ASSERT(opened->body.at("lastReported").get<bool>());
	UAM_ASSERT_EQ(opened->body.at("messages"), uam::StateSerializer::SerializeSession(*transcript).value("messages", nlohmann::json::array()));
	const auto observed_after = ChatRepository::LoadLocalChatBounded(app.data_root, expected.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(observed_after && observed_after->model_id == original.canonical);
	const auto path = AppPaths::UamChatFilePath(app.data_root, expected.transcript_chat_id);
	const auto bytes = ReadFile(path);
	auto changed_model = nlohmann::json::parse(bytes);
	changed_model["model_id"] = original.requested;
	UAM_ASSERT(uam::io::WriteTextFile(path, changed_model.dump(2)));
	const auto refused = request("getManagedAgentTranscript", payload);
	UAM_ASSERT_EQ(refused->successes, 0);
	UAM_ASSERT_EQ(refused->failures, 1);
	UAM_ASSERT(!refused->error.empty());
	UAM_ASSERT(uam::io::WriteTextFile(path, bytes)); // Restore solely for exact evidence/survivor checks, never runtime alias resolution.
	for (const auto& [relative, content] : saved)
	{
		UAM_ASSERT_EQ(ReadFile(crash.root / relative), content);
		UAM_ASSERT_EQ(ReadFile(original.app.data_root / relative), content);
	}
	UAM_ASSERT_EQ(original.PromptCount(), std::size_t{1});
	UAM_ASSERT(app.agent_runs.empty() && app.queued_agent_run_ids.empty() && app.managed_initial_runtime_owners.empty());
	UAM_ASSERT(app.acp_sessions.empty() && app.local_acp_launch_tasks.empty() && app.managed_subagent_cancellation_receipts.empty());
	UAM_ASSERT(!ChatDomainService().FindChatById(app, expected.transcript_chat_id));
	original.AssertIsolated();
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
UAM_TEST(ManagedInitialClaudePersistsOwnedExecutionBindingAfterCompletion)
{
	// Run the omission control before the accepted-operation binding assertion.
	for (const bool include_model : {false, true})
	{
		ManagedClaudeLifecycleFixture fixture(include_model);
		const std::string arm = include_model ? "present-model" : "omitted-model";
		const std::string text = "@" + fixture.definition.id + " " + fixture.task;
		std::optional<uam::ManagedInitialMentionResult> completion;
		std::size_t completion_count = 0;
		std::string error;
		fixture.AssertPeerFiles();
		UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
		                  fixture.app, fixture.root_id, text,
		                  [&](const uam::ManagedInitialMentionResult& result)
		                  {
			                  ++completion_count;
			                  completion = result;
			                  return true;
		                  },
		                  &error),
		              uam::ManagedInitialMentionAdmission::Pending);
		fixture.Await([&] { return completion.has_value(); });
		if (!completion->input_recorded || completion->delegation_state != uam::ManagedInitialDelegationState::Committed)
			throw TestFailure("Execution-binding " + arm + " creation prerequisite: " + completion->error.substr(0, 512));
		UAM_ASSERT_EQ(completion_count, std::size_t{1});
		UAM_ASSERT(completion->parent_message && completion->parent_message->content == text);
		UAM_ASSERT_EQ(completion->parent_message_index, fixture.original.size());
		const AgentRun initial = fixture.ReadRun();
		fixture.run_id = initial.id;
		UAM_ASSERT_EQ(completion->run_id, initial.id);
		UAM_ASSERT_EQ(completion->transcript_chat_id, initial.transcript_chat_id);
		UAM_ASSERT(initial.managed_continuation && !initial.managed_continuation->execution);
		UAM_ASSERT_EQ(initial.managed_continuation->turns.size(), std::size_t{1});
		const auto initial_turn = initial.managed_continuation->turns.front();
		const auto initial_root = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		const auto initial_transcript = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, initial.transcript_chat_id, 4 * 1024 * 1024);
		UAM_ASSERT(initial_root && initial_transcript && initial_transcript->native_session_id.empty());
		UAM_ASSERT_EQ(initial.root_chat_id, fixture.root_id);
		UAM_ASSERT(initial.parent_run_id.empty() && initial.depth == 1);
		UAM_ASSERT_EQ(initial.provider_id, std::string("claude-cli"));
		UAM_ASSERT_EQ(initial.model_id, fixture.model);
		UAM_ASSERT_EQ(initial.definition_snapshot, fixture.definition.markdown_snapshot);
		UAM_ASSERT_EQ(initial.definition_hash, fixture.definition.definition_hash);
		UAM_ASSERT_EQ(initial.effective_workspace_access, std::string("read"));
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(initial_root->managed_followup_incarnation));
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(initial_transcript->managed_followup_incarnation));
		UAM_ASSERT(initial_root->managed_followup_incarnation != initial_transcript->managed_followup_incarnation);
		UAM_ASSERT_EQ(initial.managed_continuation->owner.root_birth_id, initial_root->managed_followup_incarnation);
		UAM_ASSERT_EQ(initial.managed_continuation->owner.transcript_birth_id, initial_transcript->managed_followup_incarnation);
		UAM_ASSERT(!initial.managed_continuation->owner.delegation_grant);
		fixture.Await([&] { return fs::exists(fixture.temp.root / "input-witness.json"); });
		const auto input = nlohmann::json::parse(ReadFile(fixture.temp.root / "input-witness.json"));
		const auto launch = nlohmann::json::parse(ReadFile(fixture.temp.root / "launch-witness.json"));
		const auto held = fixture.ReadRun();
		UAM_ASSERT(held.managed_continuation && !held.managed_continuation->execution);
		UAM_ASSERT(input.at("run").at("managedContinuation").at("execution").is_null());
		UAM_ASSERT_EQ(launch.at("run").at("status").get<std::string>(), std::string("running"));
		UAM_ASSERT(launch.at("run").at("managedContinuation").at("turns").at(0).at("dispatchAttempted").get<bool>());
		const auto& held_turn = held.managed_continuation->turns.front();
		const auto* session = uam::FindAcpSessionForChat(fixture.app, initial.transcript_chat_id);
		UAM_ASSERT(session && session->running && session->processing && session->session_id.empty());
		UAM_ASSERT_EQ(session->child_pid, input.at("pid").get<pid_t>());
		UAM_ASSERT_EQ(session->process_execution_host_id, std::string("local"));
		UAM_ASSERT_EQ(session->turn_serial, held_turn.expected_turn_serial);
		UAM_ASSERT_EQ(held_turn.expected_turn_serial, input.at("turn").at("expectedTurnSerial").get<int>());
		UAM_ASSERT(held_turn.dispatch_attempted && held_turn.expected_turn_serial > 0 && held_turn.first_user_message_index >= 0);
		UAM_ASSERT_EQ(held_turn.user_message_created_at, input.at("turn").at("userMessageCreatedAt").get<std::string>());
		const auto inputs_before_completion = ReadFile(fixture.temp.root / "task-inputs.jsonl");
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-completion", "release owned binding observation\n"));
		fixture.AssertPeerFiles();
		fixture.Await(
		    [&]
		    {
			    const auto run = fixture.ReadRun();
			    return run.status == "completed" && run.root_result_delivered;
		    });
		const AgentRun completed = fixture.ReadRun();
		const auto native = nlohmann::json::parse(ReadFile(fixture.temp.root / "native-witness.json"));
		const auto native_id = native.at("native_id").get<std::string>();
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(native_id));
		UAM_ASSERT_EQ(native.at("model_included").get<bool>(), include_model);
		const auto transcript = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, initial.transcript_chat_id, 4 * 1024 * 1024);
		const auto root = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		UAM_ASSERT(root && transcript && completed.managed_continuation);
		const auto& turn = completed.managed_continuation->turns.front();
		UAM_ASSERT_EQ(completed.id, initial.id);
		UAM_ASSERT_EQ(completed.root_chat_id, initial.root_chat_id);
		UAM_ASSERT_EQ(completed.transcript_chat_id, initial.transcript_chat_id);
		UAM_ASSERT_EQ(completed.parent_run_id, initial.parent_run_id);
		UAM_ASSERT_EQ(completed.depth, initial.depth);
		UAM_ASSERT_EQ(completed.task, initial.task);
		UAM_ASSERT_EQ(completed.provider_id, initial.provider_id);
		UAM_ASSERT_EQ(completed.model_id, initial.model_id);
		UAM_ASSERT_EQ(completed.definition_snapshot, initial.definition_snapshot);
		UAM_ASSERT_EQ(completed.definition_instructions, initial.definition_instructions);
		UAM_ASSERT_EQ(completed.definition_hash, initial.definition_hash);
		UAM_ASSERT_EQ(completed.skills_snapshot, initial.skills_snapshot);
		UAM_ASSERT_EQ(completed.delegates_snapshot, initial.delegates_snapshot);
		UAM_ASSERT_EQ(completed.effective_workspace_access, initial.effective_workspace_access);
		UAM_ASSERT_EQ(completed.managed_continuation->owner, initial.managed_continuation->owner);
		UAM_ASSERT_EQ(completed.managed_continuation->current_turn_id, initial.id);
		UAM_ASSERT_EQ(completed.managed_continuation->current_turn_number, std::uint32_t{1});
		UAM_ASSERT_EQ(completed.managed_continuation->turns.size(), std::size_t{1});
		UAM_ASSERT(completed.managed_continuation->state_revision > held.managed_continuation->state_revision);
		UAM_ASSERT_EQ(turn.turn_id, initial_turn.turn_id);
		UAM_ASSERT_EQ(turn.turn_number, initial_turn.turn_number);
		UAM_ASSERT_EQ(turn.client_request_id, initial_turn.client_request_id);
		UAM_ASSERT_EQ(turn.after_turn_id, initial_turn.after_turn_id);
		UAM_ASSERT_EQ(turn.text, initial_turn.text);
		UAM_ASSERT_EQ(turn.principal, initial_turn.principal);
		UAM_ASSERT_EQ(turn.continuity, initial_turn.continuity);
		UAM_ASSERT_EQ(turn.recovery_decision, initial_turn.recovery_decision);
		UAM_ASSERT_EQ(turn.retry_of_turn_id, initial_turn.retry_of_turn_id);
		UAM_ASSERT_EQ(turn.delivery_id, initial.id);
		UAM_ASSERT_EQ(turn.status, uam::ManagedRunTurnStatus::Completed);
		UAM_ASSERT(turn.dispatch_attempted && turn.result_required && turn.result_delivered);
		UAM_ASSERT_EQ(turn.expected_turn_serial, held_turn.expected_turn_serial);
		UAM_ASSERT_EQ(completed.expected_turn_serial, turn.expected_turn_serial);
		UAM_ASSERT_EQ(turn.first_user_message_index, held_turn.first_user_message_index);
		UAM_ASSERT_EQ(turn.user_message_created_at, held_turn.user_message_created_at);
		UAM_ASSERT(completed.started_at_epoch_ms > 0 && completed.finished_at_epoch_ms >= completed.started_at_epoch_ms);
		UAM_ASSERT_EQ(turn.started_at_epoch_ms, completed.started_at_epoch_ms);
		UAM_ASSERT_EQ(turn.finished_at_epoch_ms, completed.finished_at_epoch_ms);
		UAM_ASSERT_EQ(turn.result_delivery_attempts, completed.root_result_delivery_attempts);
		UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
		UAM_ASSERT_EQ(transcript->native_session_id, native_id);
		UAM_ASSERT_EQ(transcript->agent_run_id, initial.id);
		UAM_ASSERT_EQ(transcript->provider_id, initial.provider_id);
		UAM_ASSERT_EQ(transcript->model_id, initial_transcript->model_id);
		UAM_ASSERT_EQ(transcript->execution_host_id, initial_transcript->execution_host_id);
		UAM_ASSERT_EQ(transcript->workspace_directory, initial_transcript->workspace_directory);
		UAM_ASSERT_EQ(transcript->approval_mode, initial_transcript->approval_mode);
		UAM_ASSERT_EQ(root->managed_followup_incarnation, initial_root->managed_followup_incarnation);
		UAM_ASSERT_EQ(transcript->managed_followup_incarnation, initial_transcript->managed_followup_incarnation);
		UAM_ASSERT_EQ(transcript->messages.at(static_cast<std::size_t>(turn.first_user_message_index)).created_at, turn.user_message_created_at);
		UAM_ASSERT_EQ(std::ranges::count(transcript->messages, fixture.task, &Message::content), 1);
		UAM_ASSERT_EQ(std::ranges::count(transcript->messages, fixture.answer, &Message::content), 1);
		UAM_ASSERT(root->messages.size() >= fixture.original.size() + 1);
		UAM_ASSERT(std::equal(fixture.original.begin(), fixture.original.end(), root->messages.begin()));
		UAM_ASSERT_EQ(std::ranges::count(root->messages, text, &Message::content), 1);
		const auto live_root = std::ranges::find(fixture.app.chats, fixture.root_id, &ChatSession::id);
		UAM_ASSERT(live_root != fixture.app.chats.end() && live_root->metadata_incarnation == fixture.root_incarnation);
		const auto result_count = [&]
		{
			const auto saved_root = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
			UAM_ASSERT(saved_root);
			std::size_t count = 0;
			for (const auto& message : saved_root->messages)
				for (const auto& tool : message.tool_calls)
					if (tool.id == "uam-agent-result-" + turn.delivery_id)
					{
						++count;
						UAM_ASSERT(tool.is_sub_agent && tool.sub_agent_id == initial.id);
						UAM_ASSERT_EQ(nlohmann::json::parse(tool.result_text).at("result").get<std::string>(), fixture.answer);
					}
			return count;
		};
		UAM_ASSERT_EQ(result_count(), std::size_t{1});
		for (int poll = 0; poll < 100; ++poll)
		{
			fixture.Pulse();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT_EQ(result_count(), std::size_t{1});
		UAM_ASSERT_EQ(ReadFile(fixture.temp.root / "task-inputs.jsonl"), inputs_before_completion);
		std::istringstream inputs(inputs_before_completion);
		std::size_t input_count = 0;
		std::string line;
		while (std::getline(inputs, line))
			if (!line.empty())
			{
				(void)nlohmann::json::parse(line);
				++input_count;
			}
		UAM_ASSERT_EQ(input_count, std::size_t{1});
		fixture.Await([&] { return fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty(); });
		UAM_ASSERT(fixture.StopAndReap()); // Genuine completed/delivered/reaped prerequisites precede the binding RED.
		const auto after_reap = fixture.ReadRun();
		UAM_ASSERT(after_reap.managed_continuation == completed.managed_continuation);
		const auto state = uam::ManagedSubagentFollowupService::GetState(fixture.app, fixture.root_id, initial.id);
		UAM_ASSERT_EQ(state.can_send, completed.managed_continuation->execution.has_value());
		UAM_ASSERT_EQ(state.code, state.can_send ? uam::ManagedFollowupCode::None : uam::ManagedFollowupCode::ModelIdentityUnverified);
		uam::ManagedFollowupRequest request;
		request.after_turn_id = completed.managed_continuation->current_turn_id;
		request.client_request_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		request.text = "This storage prerequisite must not enable a second execution.";
		const auto submitted = uam::ManagedSubagentFollowupService::Submit(fixture.app, fixture.root_id, initial.id, request);
		UAM_ASSERT(!submitted.accepted && !submitted.replayed && !submitted.acceptance && submitted.code == uam::ManagedFollowupCode::NotImplemented);
		UAM_ASSERT(fixture.ReadRun().managed_continuation == completed.managed_continuation);
		UAM_ASSERT(fixture.app.queued_agent_run_ids.empty() && fixture.app.acp_sessions.empty());
		if (!include_model)
		{
			UAM_ASSERT(!completed.managed_continuation->execution);
			continue;
		}
		// Intended current feature RED, only after the actual owned initial lifecycle above.
		UAM_ASSERT(completed.managed_continuation->execution.has_value());
		const auto& binding = *completed.managed_continuation->execution;
		UAM_ASSERT_EQ(binding.native_session_id, native_id);
		UAM_ASSERT_EQ(binding.effective_model_id, fixture.model);
		UAM_ASSERT_EQ(binding.model_identity_source, std::string("exact-requested"));
		UAM_ASSERT_EQ(binding.execution_host_id, transcript->execution_host_id);
		UAM_ASSERT_EQ(binding.workspace_directory, transcript->workspace_directory);
		UAM_ASSERT_EQ(binding.workspace_worktree_directory, transcript->workspace_worktree_directory);
		UAM_ASSERT_EQ(binding.workspace_source_directory, transcript->workspace_source_directory);
		UAM_ASSERT_EQ(binding.routing_mode, transcript->routing_mode);
		UAM_ASSERT_EQ(binding.routing_source_folder_id, transcript->routing_source_folder_id);
		UAM_ASSERT_EQ(binding.routing_source_directory, transcript->routing_source_directory);
		UAM_ASSERT_EQ(binding.reasoning_effort, transcript->reasoning_effort);
		UAM_ASSERT_EQ(binding.service_tier, transcript->service_tier);
		UAM_ASSERT_EQ(binding.service_tier_explicit, transcript->service_tier_explicit);
		UAM_ASSERT_EQ(binding.approval_mode, transcript->approval_mode);
		UAM_ASSERT_EQ(binding.command_safety_tier, transcript->command_safety_tier);
		const auto canonical_projection = [](const std::string& projection)
		{
			UAM_ASSERT(!projection.empty() && projection.size() <= 64U * 1024U && projection.find('\0') == std::string::npos);
			const auto value = nlohmann::json::parse(projection);
			UAM_ASSERT(value.is_object() || value.is_array());
			UAM_ASSERT_EQ(value.dump(), projection);
		};
		canonical_projection(binding.connection_configuration_json);
		canonical_projection(binding.provider_configuration_json);
		// These are stored observations, not fixture-created configuration or native authority.
		const auto roundtrip = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
		UAM_ASSERT(roundtrip.errors.empty() && roundtrip.runs.size() == 1);
		UAM_ASSERT(roundtrip.runs.front().managed_continuation->execution == completed.managed_continuation->execution);
		UAM_ASSERT_EQ(roundtrip.runs.front().id, initial.id);
		UAM_ASSERT_EQ(roundtrip.runs.front().transcript_chat_id, initial.transcript_chat_id);
	}
}
#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_RUNTIME_OPENCODE_CLI && !defined(_WIN32)
#include <fstream>
namespace
{
	// Admission-accounting fixture only: the genuine public Send path retains its
	// input in the existing unfinished OpenCode version wait, before any launch.
	struct ManagedCapacityVersionWaitFixture
	{
		// Declared before fixture so this unfinished state survives AppState teardown.
		std::shared_ptr<AsyncProcessTaskState> unfinished = std::make_shared<AsyncProcessTaskState>();
		ManagedInitialFirstFixture fixture;
		std::string other_root_id;

		std::string SentinelScript() const
		{
			// Shell builtins only, at most16 arguments with256 bytes per argument.
			return "#!/bin/sh\n{\nprintf 'pid=%s ppid=%s executable=%.256s argc=%s\\n' \"$$\" \"${PPID:-unknown}\" \"$0\" \"$#\"\nuam_diag_i=0\nfor uam_diag_arg in \"$@\"; do\n[ \"$uam_diag_i\" -ge 16 ] && break\nprintf 'arg[%s]=%.256s\\n' \"$uam_diag_i\" \"$uam_diag_arg\"\nuam_diag_i=$((uam_diag_i + 1))\ndone\n} >> " + ShellQuoteForTest(fixture.Invocations().string()) + "\nexit 97\n";
		}
		void AssertInvocationFileAbsent(const char* phase) const
		{
			if (fs::exists(fixture.Invocations()))
			{
				std::ifstream input(fixture.Invocations(), std::ios::binary);
				std::string evidence(8192, '\0');
				input.read(evidence.data(), static_cast<std::streamsize>(evidence.size()));
				evidence.resize(static_cast<std::size_t>(input.gcount()));
				throw TestFailure(std::string("Managed capacity private invocation; phase=") + phase + "; bounded witness=" + evidence);
			}
			UAM_ASSERT(!fs::exists(fixture.Invocations()));
		}
		ManagedCapacityVersionWaitFixture()
		{
			AssertInvocationFileAbsent("constructor:entry");
			UAM_ASSERT(uam::io::WriteTextFile(fixture.Cli(), SentinelScript()));
			fs::permissions(fixture.Cli(), fs::perms::owner_all, fs::perm_options::replace);
			const fs::path executable = fixture.temp.root / "bin" / "opencode";
			UAM_ASSERT(uam::io::WriteTextFile(executable, SentinelScript()));
			fs::permissions(executable, fs::perms::owner_all, fs::perm_options::replace);
			fixture.app.provider_profiles = {ProviderProfileStore::DefaultOpenCodeProfile()};
			fixture.app.provider_profiles.front().interactive_command = ShellQuoteForTest(executable.string());
			AssertInvocationFileAbsent("constructor:profile");
			auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
			UAM_ASSERT(root && root->native_session_id.empty() && root->managed_followup_incarnation.empty());
			root->provider_id = "opencode-cli";
			root->model_id = "private/capacity-model";
			UAM_ASSERT(ProviderResolutionService().ChatProviderIsAvailable(fixture.app, *root));
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *root));
			AssertInvocationFileAbsent("constructor:root-save");
			ChatSession other = ChatDomainService().CreateNewChat({}, "opencode-cli");
			AssertInvocationFileAbsent("constructor:other-created");
			other_root_id = other.id;
			other.execution_host_id = root->execution_host_id;
			other.routing_mode = root->routing_mode;
			other.workspace_directory = root->workspace_directory;
			other.model_id = root->model_id;
			ChatDomainService().AddMessage(other, MessageRole::User, "Independent legacy capacity parent.");
			ChatDomainService().AddMessage(other, MessageRole::Assistant, "Earlier saved parent answer.");
			fixture.app.chats.push_back(std::move(other));
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.app.chats.back()));
			AssertInvocationFileAbsent("constructor:other-save");
			const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, other_root_id, 4 * 1024 * 1024);
			UAM_ASSERT(saved && saved->messages == fixture.app.chats.back().messages);
			AssertInvocationFileAbsent("constructor:other-reload");
			auto& version = fixture.app.runtime_cli_versions_by_provider_id["opencode-cli"];
			version.provider_id = "opencode-cli";
			version.execution_host = uam::execution_hosts::LocalHost();
			version.checked = false;
			version.supported = false;
			fixture.app.runtime_cli_version_provider_id = "opencode-cli";
			fixture.app.runtime_cli_version_check_task.execution_host = version.execution_host;
			fixture.app.runtime_cli_version_check_task.running = true;
			fixture.app.runtime_cli_version_check_task.state = unfinished;
			AssertNoInvocation("constructor:barrier-armed");
		}
		~ManagedCapacityVersionWaitFixture()
		{
			// Resource cleanup only, including after a failed oracle. Never finish the
			// version promise or authorize a launch to make teardown pass.
			(void)uam::AgentRunScheduler::QuiesceInitialForShutdown(fixture.app);
		}
		void AssertNoInvocation(const char* phase = "state-oracle") const
		{
			fixture.ExecutableIsPrivate();
			const auto executable = fixture.temp.root / "bin" / "opencode";
			UAM_ASSERT(fs::is_regular_file(executable) && !fs::is_symlink(executable));
			UAM_ASSERT((fs::status(executable).permissions() & fs::perms::owner_exec) != fs::perms::none);
			AssertInvocationFileAbsent(phase);
			const auto& task = fixture.app.runtime_cli_version_check_task;
			UAM_ASSERT(task.running && task.state == unfinished && !task.worker);
			UAM_ASSERT(!unfinished->completed.load() && unfinished->result.output.empty() && unfinished->result.error.empty());
			UAM_ASSERT_EQ(fixture.app.runtime_cli_version_provider_id, std::string("opencode-cli"));
			UAM_ASSERT_EQ(task.execution_host.id, std::string("local"));
			const auto& version = fixture.app.runtime_cli_versions_by_provider_id.at("opencode-cli");
			UAM_ASSERT(!version.checked && !version.supported && version.installed_version.empty());
			UAM_ASSERT(fixture.app.pending_acp_remote_stops.empty() && fixture.app.acp_process_stop_tasks.empty());
			for (const auto& session : fixture.app.acp_sessions)
			{
				UAM_ASSERT(session && !session->running && !session->processing && !session->session_ready && !session->managed_launch_attempted);
				UAM_ASSERT(session->last_process_id.empty() && !session->stdin_writer && session->session_id.empty());
				UAM_ASSERT(session->child_pid == -1 && session->watchdog_pid == -1);
				UAM_ASSERT(session->stdin_write_fd == -1 && session->stdout_read_fd == -1 && session->stderr_read_fd == -1);
				UAM_ASSERT(session->stdout_buffer.empty() && session->stderr_buffer.empty());
				UAM_ASSERT_EQ(session->prompt_request_id, 0);
				UAM_ASSERT(session->pending_request_methods.empty());
			}
		}
		const AgentRun& Run(const std::string& id) const
		{
			const auto found = std::ranges::find(fixture.app.agent_runs, id, &AgentRun::id);
			UAM_ASSERT(found != fixture.app.agent_runs.end());
			return *found;
		}
		std::string Enqueue(const std::string& root_id, const std::string& task)
		{
			std::string id, error;
			AssertNoInvocation("enqueue:pre");
			UAM_ASSERT(uam::AgentRunScheduler::Enqueue(fixture.app, root_id, {}, fixture.definition.id, task, &id, &error));
			AssertNoInvocation("enqueue:post");
			UAM_ASSERT(error.empty() && !id.empty());
			UAM_ASSERT(!Run(id).managed_continuation && Run(id).status == "queued");
			return id;
		}
		void Pulse()
		{
			AssertNoInvocation("poll:pre");
			(void)uam::AgentRunScheduler::Poll(fixture.app);
			AssertNoInvocation("poll:post");
			AssertNoInvocation("flush:pre");
			(void)uam::FlushPendingChatSaves(fixture.app);
			AssertNoInvocation("flush:post");
		}
		template <class Predicate> void Await(Predicate done)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!done() && std::chrono::steady_clock::now() < deadline)
			{
				Pulse();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(done());
		}
		void AssertRunningIsPersistedVersionWait(const std::string& id)
		{
			// Public Send accepts this version wait before its full queue save ACK.
			// Flush only: preserve scheduler admission order and never advance ACP.
			const std::string transcript_id = Run(id).transcript_chat_id;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (fixture.app.pending_chat_save_at_by_chat_id.contains(transcript_id) && std::chrono::steady_clock::now() < deadline)
			{
				AssertNoInvocation("admission-flush:pre");
				(void)uam::FlushPendingChatSaves(fixture.app);
				AssertNoInvocation("admission-flush:post");
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.contains(transcript_id));
			AssertNoInvocation("admission-save-settled");
			const auto& run = Run(id);
			UAM_ASSERT_EQ(run.status, std::string("running"));
			const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
			UAM_ASSERT(ledger.errors.empty());
			const auto saved = std::ranges::find(ledger.runs, id, &AgentRun::id);
			UAM_ASSERT(saved != ledger.runs.end() && saved->status == "running");
			UAM_ASSERT_EQ(saved->root_chat_id, run.root_chat_id);
			UAM_ASSERT_EQ(saved->transcript_chat_id, run.transcript_chat_id);
			UAM_ASSERT_EQ(saved->task, run.task);
			UAM_ASSERT_EQ(saved->managed_continuation, run.managed_continuation);
			const auto* session = uam::FindAcpSessionForChat(fixture.app, run.transcript_chat_id);
			UAM_ASSERT(session && session->reconnect_pending && !session->queue_held);
			UAM_ASSERT_EQ(session->queued_user_prompts.size(), std::size_t{1});
			UAM_ASSERT_EQ(session->queued_user_prompts.front().text, run.task);
			const auto transcript = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, run.transcript_chat_id, 4 * 1024 * 1024);
			UAM_ASSERT(transcript && transcript->agent_run_id == id && transcript->native_session_id.empty());
			UAM_ASSERT_EQ(transcript->acp_queued_prompts.size(), std::size_t{1});
			UAM_ASSERT_EQ(transcript->acp_queued_prompts.front().text, run.task);
			UAM_ASSERT(transcript->remote_pending_requests.empty() && !transcript->remote_process_exists);
			AssertNoInvocation();
		}
	};
} // namespace

UAM_TEST(ManagedPureLegacyVersionWaitRetainsFourRunningAdmissionLimit)
{
	ManagedCapacityVersionWaitFixture capacity;
	std::vector<std::string> first_four;
	for (int index = 0; index < 4; ++index)
		first_four.push_back(capacity.Enqueue(capacity.other_root_id, "Legacy held admission " + std::to_string(index)));
	const auto fifth = capacity.Enqueue(capacity.fixture.root_id, "Fifth legacy admission must wait.");
	capacity.Pulse();
	for (const auto& id : first_four)
		capacity.AssertRunningIsPersistedVersionWait(id);
	for (int index = 0; index < 3; ++index)
		capacity.Pulse();
	const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(capacity.fixture.app.data_root);
	UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 5);
	UAM_ASSERT_EQ(std::ranges::count(ledger.runs, std::string("running"), &AgentRun::status), 4);
	UAM_ASSERT_EQ(std::ranges::count(capacity.fixture.app.agent_runs, std::string("running"), &AgentRun::status), 4);
	UAM_ASSERT_EQ(capacity.Run(fifth).status, std::string("queued"));
	const auto saved_fifth = std::ranges::find(ledger.runs, fifth, &AgentRun::id);
	UAM_ASSERT(saved_fifth != ledger.runs.end() && saved_fifth->status == "queued");
	UAM_ASSERT_EQ(capacity.fixture.app.queued_agent_run_ids.size(), std::size_t{1});
	UAM_ASSERT_EQ(capacity.fixture.app.queued_agent_run_ids.front(), fifth);
	UAM_ASSERT(uam::FindAcpSessionForChat(capacity.fixture.app, capacity.Run(fifth).transcript_chat_id) == nullptr);
	UAM_ASSERT_EQ(capacity.fixture.app.acp_sessions.size(), std::size_t{4});
	capacity.AssertNoInvocation();
}

UAM_TEST(ManagedMixedInitialLegacyVersionWaitSharesFourRunningAdmissionSlots)
{
	ManagedCapacityVersionWaitFixture capacity;
	std::vector<std::string> initial_legacy;
	for (int index = 0; index < 3; ++index)
		initial_legacy.push_back(capacity.Enqueue(capacity.other_root_id, "Existing legacy held admission " + std::to_string(index)));
	capacity.Pulse();
	for (const auto& id : initial_legacy)
		capacity.AssertRunningIsPersistedVersionWait(id);
	UAM_ASSERT_EQ(capacity.fixture.app.acp_sessions.size(), std::size_t{3});
	UAM_ASSERT(capacity.fixture.app.queued_agent_run_ids.empty());
	struct Completion
	{
		int calls = 0;
		std::optional<uam::ManagedInitialMentionResult> result;
		bool competitor_enqueued = false;
		std::string competitor_id, error;
	};
	const auto completion = std::make_shared<Completion>();
	std::string error;
	const std::string text = "@" + capacity.fixture.definition.id + " Hold the fourth real managed admission without launching.";
	UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
	                  capacity.fixture.app, capacity.fixture.root_id, text,
	                  [&, completion](const uam::ManagedInitialMentionResult& result)
	                  {
		                  ++completion->calls;
		                  completion->result = result;
		                  if (result.delegation_state == uam::ManagedInitialDelegationState::Committed)
			                  completion->competitor_enqueued = uam::AgentRunScheduler::Enqueue(capacity.fixture.app, capacity.other_root_id, {}, capacity.fixture.definition.id, "Legacy competitor queued from the genuine committed creation callback.", &completion->competitor_id, &completion->error);
		                  return true; // Genuine classified delivery ACK; no fabricated ledger or runtime owner.
	                  },
	                  &error),
	              uam::ManagedInitialMentionAdmission::Pending);
	UAM_ASSERT(error.empty());
	capacity.Await([&] { return completion->calls != 0; });
	UAM_ASSERT_EQ(completion->calls, 1);
	UAM_ASSERT(completion->result && completion->result->input_recorded && completion->result->parent_message);
	UAM_ASSERT_EQ(completion->result->delegation_state, uam::ManagedInitialDelegationState::Committed);
	UAM_ASSERT_EQ(completion->result->parent_message->content, text);
	UAM_ASSERT(completion->competitor_enqueued && completion->error.empty() && !completion->competitor_id.empty());
	const std::string initial_id = completion->result->run_id;
	const std::string transcript_id = completion->result->transcript_chat_id;
	UAM_ASSERT(!initial_id.empty() && !transcript_id.empty());
	UAM_ASSERT_EQ(capacity.Run(initial_id).root_chat_id, capacity.fixture.root_id);
	UAM_ASSERT_EQ(capacity.Run(completion->competitor_id).root_chat_id, capacity.other_root_id);
	UAM_ASSERT(capacity.Run(initial_id).managed_continuation && !capacity.Run(completion->competitor_id).managed_continuation);
	const auto saved_root = ChatRepository::LoadLocalChatBounded(capacity.fixture.app.data_root, capacity.fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(saved_root && uam::uuid::IsCanonicalUuid(saved_root->managed_followup_incarnation));
	UAM_ASSERT_EQ(std::ranges::count(saved_root->messages, text, &Message::content), 1);
	capacity.Await(
	    [&]
	    {
		    const auto* session = uam::FindAcpSessionForChat(capacity.fixture.app, transcript_id);
		    return capacity.Run(initial_id).status == "running" && session && session->reconnect_pending && session->queued_user_prompts.size() == 1 && !uam::AgentRunScheduler::HasPendingInitialMention(capacity.fixture.app, transcript_id);
	    });
	for (const auto& id : initial_legacy)
		capacity.AssertRunningIsPersistedVersionWait(id);
	capacity.AssertRunningIsPersistedVersionWait(initial_id);
	const auto& turn = capacity.Run(initial_id).managed_continuation->turns.front();
	UAM_ASSERT(turn.dispatch_attempted && !capacity.Run(initial_id).managed_continuation->execution);
	const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(capacity.fixture.app.data_root);
	UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 5);
	const auto live_running = std::ranges::count(capacity.fixture.app.agent_runs, std::string("running"), &AgentRun::status);
	const auto saved_running = std::ranges::count(ledger.runs, std::string("running"), &AgentRun::status);
	UAM_ASSERT_EQ(live_running, saved_running);
	const auto saved_competitor = std::ranges::find(ledger.runs, completion->competitor_id, &AgentRun::id);
	UAM_ASSERT(saved_competitor != ledger.runs.end() && saved_competitor->status == capacity.Run(completion->competitor_id).status);
	capacity.AssertNoInvocation();
	// Intended feature RED: the real v2 Running preparation reserved slot four,
	// but the later legacy loop counted only persisted Running and admitted a fifth.
	UAM_ASSERT(saved_running <= 4);
	UAM_ASSERT_EQ(capacity.Run(completion->competitor_id).status, std::string("queued"));
	UAM_ASSERT(std::ranges::find(capacity.fixture.app.queued_agent_run_ids, completion->competitor_id) != capacity.fixture.app.queued_agent_run_ids.end());
	UAM_ASSERT(uam::FindAcpSessionForChat(capacity.fixture.app, capacity.Run(completion->competitor_id).transcript_chat_id) == nullptr);
	UAM_ASSERT_EQ(capacity.fixture.app.acp_sessions.size(), std::size_t{4});
	UAM_ASSERT_EQ(completion->calls, 1);
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
namespace
{
	struct BoundRestartInspectionFixtureR96
	{
		ManagedClaudeLifecycleFixture original{true};
		TempDir crash{"uam-managed-bound-restart-inspection"};
		uam::AppState app;
		std::unique_ptr<UamQueryHandler> handler;
		std::vector<RestartInspectionFixtureR91::Query> pending;
		AgentRun expected;
		ChatSession transcript;
		std::map<fs::path, std::string> saved, copied;
		std::string input_bytes;

		explicit BoundRestartInspectionFixtureR96(bool mismatch_native_before_startup = false)
		{
			auto completion = std::make_shared<InitialGuardCompletion>();
			std::string error;
			original.AssertPeerFiles();
			UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
			                  original.app, original.root_id, "@" + original.definition.id + " " + original.task,
			                  [completion](const uam::ManagedInitialMentionResult& result)
			                  {
				                  ++completion->calls;
				                  completion->result = result;
				                  return true;
			                  },
			                  &error),
			              uam::ManagedInitialMentionAdmission::Pending);
			original.Await([&] { return completion->result.has_value(); });
			UAM_ASSERT_EQ(completion->calls, 1);
			UAM_ASSERT(completion->result->input_recorded && completion->result->delegation_state == uam::ManagedInitialDelegationState::Committed);
			const auto initial = original.ReadRun();
			original.run_id = initial.id;
			UAM_ASSERT(initial.managed_continuation && !initial.managed_continuation->execution);
			UAM_ASSERT_EQ(completion->result->run_id, initial.id);
			UAM_ASSERT_EQ(completion->result->transcript_chat_id, initial.transcript_chat_id);
			original.Await([&] { return fs::exists(original.temp.root / "input-witness.json"); });
			const auto input = nlohmann::json::parse(ReadFile(original.temp.root / "input-witness.json"));
			UAM_ASSERT(input.at("run").at("managedContinuation").at("execution").is_null());
			const auto* session = uam::FindAcpSessionForChat(original.app, initial.transcript_chat_id);
			UAM_ASSERT(session && session->running && session->processing && session->child_pid == input.at("pid").get<pid_t>());
			input_bytes = ReadFile(original.temp.root / "task-inputs.jsonl");
			std::istringstream lines(input_bytes);
			std::string line;
			std::size_t count = 0;
			while (std::getline(lines, line))
				if (!line.empty())
				{
					(void)nlohmann::json::parse(line);
					++count;
				}
			UAM_ASSERT_EQ(count, std::size_t{1});
			UAM_ASSERT(uam::io::WriteTextFile(original.temp.root / "allow-completion", "Release the genuine completed bound-history prerequisite.\n"));
			original.Await(
			    [&]
			    {
				    const auto run = original.ReadRun();
				    return run.status == "completed" && run.root_result_delivered;
			    });
			expected = original.ReadRun();
			const auto native = nlohmann::json::parse(ReadFile(original.temp.root / "native-witness.json"));
			UAM_ASSERT(native.at("model_included").get<bool>());
			const auto native_id = native.at("native_id").get<std::string>();
			UAM_ASSERT(uam::uuid::IsCanonicalUuid(native_id));
			UAM_ASSERT(original.StopAndReap());
			UAM_ASSERT(original.ReadRun().managed_continuation == expected.managed_continuation);
			UAM_ASSERT(expected.managed_continuation && expected.managed_continuation->turns.size() == 1);
			if (!expected.managed_continuation->execution)
				throw TestFailure("Bound restart history needs the genuine present-model completed producer binding first; missing binding is a prerequisite failure, not the history RED.");
			const auto& binding = *expected.managed_continuation->execution;
			UAM_ASSERT_EQ(binding.native_session_id, native_id);
			UAM_ASSERT_EQ(binding.effective_model_id, original.model);
			UAM_ASSERT_EQ(binding.model_identity_source, std::string("exact-requested"));
			UAM_ASSERT(!binding.connection_configuration_json.empty() && !binding.provider_configuration_json.empty());
			const auto loaded = ChatRepository::LoadLocalChatBounded(original.app.data_root, expected.transcript_chat_id, 4 * 1024 * 1024);
			const auto root = ChatRepository::LoadLocalChatBounded(original.app.data_root, original.root_id, 4 * 1024 * 1024);
			UAM_ASSERT(loaded && root && loaded->native_session_id == native_id);
			transcript = *loaded;
			UAM_ASSERT_EQ(root->managed_followup_incarnation, expected.managed_continuation->owner.root_birth_id);
			UAM_ASSERT_EQ(transcript.managed_followup_incarnation, expected.managed_continuation->owner.transcript_birth_id);
			UAM_ASSERT_EQ(std::ranges::count(transcript.messages, original.task, &Message::content), 1);
			UAM_ASSERT_EQ(std::ranges::count(transcript.messages, original.answer, &Message::content), 1);
			std::size_t results = 0;
			for (const auto& message : root->messages)
				for (const auto& tool : message.tool_calls)
					if (tool.id == "uam-agent-result-" + expected.id)
						++results;
			UAM_ASSERT_EQ(results, std::size_t{1});
			UAM_ASSERT_EQ(ReadFile(original.temp.root / "task-inputs.jsonl"), input_bytes);
			for (const auto& primary : {original.app.data_root / "agent-runs" / (expected.id + ".json"), AppPaths::UamChatFilePath(original.app.data_root, original.root_id), AppPaths::UamChatFilePath(original.app.data_root, expected.transcript_chat_id)})
				for (const auto& path : {primary, uam::io::MakeBackupPath(primary)})
					if (fs::exists(path))
					{
						const auto relative = path.lexically_relative(original.app.data_root);
						saved.emplace(relative, ReadFile(path));
						fs::create_directories((crash.root / relative).parent_path());
						UAM_ASSERT(uam::io::WriteTextFile(crash.root / relative, saved.at(relative)));
					}
			copied = saved;
			if (mismatch_native_before_startup)
			{
				const auto relative = AppPaths::UamChatFilePath(original.app.data_root, expected.transcript_chat_id).lexically_relative(original.app.data_root);
				auto changed = nlohmann::json::parse(copied.at(relative));
				UAM_ASSERT_EQ(changed.at("native_session_id").get<std::string>(), binding.native_session_id);
				// A real different canonical birth is only a negative on-disk identity; never a native attestation.
				const auto foreign_native = expected.managed_continuation->owner.root_birth_id;
				UAM_ASSERT(uam::uuid::IsCanonicalUuid(foreign_native) && foreign_native != binding.native_session_id);
				changed["native_session_id"] = foreign_native;
				auto restored_field = changed;
				restored_field["native_session_id"] = binding.native_session_id;
				UAM_ASSERT_EQ(restored_field, nlohmann::json::parse(saved.at(relative))); // Only this copied field changes.
				copied[relative] = changed.dump(2);
				UAM_ASSERT(uam::io::WriteTextFile(crash.root / relative, copied.at(relative)));
				const auto changed_primary = ChatRepository::LoadLocalChatBounded(crash.root, expected.transcript_chat_id, 4 * 1024 * 1024);
				UAM_ASSERT(changed_primary && changed_primary->native_session_id == foreign_native);
				UAM_ASSERT_EQ(changed_primary->managed_followup_incarnation, expected.managed_continuation->owner.transcript_birth_id);
			}
			app.data_root = crash.root;
			app.settings = original.app.settings;
			app.provider_profiles = original.app.provider_profiles;
			auto startup = uam::AgentRunLedger::LoadAll(app.data_root);
			UAM_ASSERT(uam::AgentRunLedger::MarkNonterminalInterrupted(app.data_root, &startup.runs, "manager_restarted", &startup.errors));
			app.agent_runs = std::move(startup.runs); // Actual Application runtime projection, never ReadOnly records.
			UAM_ASSERT(app.agent_runs.empty());
			UAM_ASSERT(std::ranges::any_of(startup.errors, [](const auto& value) { return value.find("inert") != std::string::npos; }));
			ChatHistorySyncService().LoadSidebarChats(app);
			if (mismatch_native_before_startup)
			{
				const auto hidden = std::ranges::find(app.hidden_agent_chat_ownership.chats, expected.transcript_chat_id, &ChatSession::id);
				UAM_ASSERT(hidden != app.hidden_agent_chat_ownership.chats.end());
				UAM_ASSERT_EQ(hidden->native_session_id, expected.managed_continuation->owner.root_birth_id); // Actual startup observed B while immutable binding still owns A.
			}
			const auto* visible = ChatDomainService().FindChatById(app, original.root_id);
			UAM_ASSERT(visible && visible->managed_followup_incarnation == expected.managed_continuation->owner.root_birth_id);
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
			NativeMediaRefreshHandlerTestAccess::RestartInspectionDispatch(*handler,
			                                                               [this](auto callback, auto worker, auto completion)
			                                                               {
				                                                               pending.push_back({callback, std::move(worker), std::move(completion)});
				                                                               return true;
			                                                               });
			Unchanged();
		}
		CefRefPtr<RestartInspectionCallbackR91> Begin(std::string_view action, const nlohmann::json& payload)
		{
			CefRefPtr<RestartInspectionCallbackR91> callback = new RestartInspectionCallbackR91;
			NativeMediaRefreshHandlerTestAccess::RestartInspectionAction(*handler, action, payload, callback);
			return callback;
		}
		void Drain()
		{
			while (!pending.empty())
			{
				auto query = std::move(pending.front());
				pending.erase(pending.begin());
				auto result = std::async(std::launch::async, query.worker).get();
				CompleteAsyncCefQuery(query.callback, result, query.completion);
			}
		}
		CefRefPtr<RestartInspectionCallbackR91> Request(std::string_view action, const nlohmann::json& payload)
		{
			auto callback = Begin(action, payload);
			Drain();
			UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
			return callback;
		}
		nlohmann::json OpenPayload() const
		{
			return {{"chatId", original.root_id}, {"transcriptChatId", expected.transcript_chat_id}};
		}
		void NoAuthority() const
		{
			UAM_ASSERT(app.agent_runs.empty() && app.queued_agent_run_ids.empty() && app.managed_initial_runtime_owners.empty() && app.managed_initial_mentions.empty());
			UAM_ASSERT(app.acp_sessions.empty() && app.cli_terminals.empty() && app.local_acp_launch_tasks.empty());
			UAM_ASSERT(app.pending_agent_run_cancellation_ids.empty() && app.managed_subagent_cancellation_receipts.empty());
			UAM_ASSERT(!ChatDomainService().FindChatById(app, expected.transcript_chat_id));
		}
		void Unchanged() const
		{
			for (const auto& [relative, bytes] : saved)
			{
				UAM_ASSERT_EQ(ReadFile(crash.root / relative), copied.at(relative));
				UAM_ASSERT_EQ(ReadFile(original.app.data_root / relative), bytes);
			}
			const auto read = uam::AgentRunLedger::LoadAllReadOnly(crash.root);
			UAM_ASSERT(read.errors.empty() && read.runs.size() == 1 && read.runs.front().managed_continuation == expected.managed_continuation);
			UAM_ASSERT_EQ(ReadFile(original.temp.root / "task-inputs.jsonl"), input_bytes);
			NoAuthority();
		}
	};
} // namespace

UAM_TEST(ManagedInitialBoundRestartListsAndOpensWithoutExecutionAuthority)
{
	BoundRestartInspectionFixtureR96 fixture;
	const auto listed = fixture.Request("listManagedSubagents", {{"chatId", fixture.original.root_id}});
	UAM_ASSERT_EQ(listed->successes, 1);
	UAM_ASSERT_EQ(listed->body.at("totalCount").get<std::size_t>(), std::size_t{1}); // Intended history RED after genuine binding prerequisite.
	const auto& row = listed->body.at("runs").front();
	UAM_ASSERT_EQ(row.at("runId").get<std::string>(), fixture.expected.id);
	UAM_ASSERT_EQ(row.at("status").get<std::string>(), std::string("completed"));
	UAM_ASSERT(row.at("lastReported").get<bool>() && !row.at("canCancel").get<bool>());
	UAM_ASSERT(!row.contains("execution") && !row.contains("connectionConfigurationJson") && !row.contains("providerConfigurationJson"));
	const auto opened = fixture.Request("getManagedAgentTranscript", fixture.OpenPayload());
	UAM_ASSERT_EQ(opened->successes, 1);
	UAM_ASSERT(opened->body.at("lastReported").get<bool>());
	UAM_ASSERT_EQ(opened->body.at("messages"), uam::StateSerializer::SerializeSession(fixture.transcript).value("messages", nlohmann::json::array()));
	UAM_ASSERT(!opened->body.contains("execution") && !opened->body.contains("connectionConfigurationJson") && !opened->body.contains("providerConfigurationJson"));
	const auto refused = fixture.Request("cancelManagedSubagent", {{"chatId", fixture.original.root_id}, {"runId", fixture.expected.id}, {"controlScopeId", listed->body.at("controlScopeId")}});
	UAM_ASSERT_EQ(refused->successes, 0);
	UAM_ASSERT_EQ(refused->failures, 1);
	UAM_ASSERT_EQ(refused->status, 409);
	fixture.Unchanged();
}

UAM_TEST(ManagedInitialBoundRestartRejectsPrivateConfigurationDriftAgainstCapturedRead)
{
	BoundRestartInspectionFixtureR96 fixture;
	const auto opened = fixture.Request("getManagedAgentTranscript", fixture.OpenPayload());
	UAM_ASSERT_EQ(opened->successes, 1); // Independent old history-shape 404, after genuine completed binding.
	UAM_ASSERT(opened->body.at("lastReported").get<bool>());
	UAM_ASSERT_EQ(opened->body.at("messages"), uam::StateSerializer::SerializeSession(fixture.transcript).value("messages", nlohmann::json::array()));
	fixture.Unchanged();
	const auto callback = fixture.Begin("getManagedAgentTranscript", fixture.OpenPayload());
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1}); // Actual CEF-owned immutable read token, before its worker reads primary.
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	const auto path = fixture.crash.root / "agent-runs" / (fixture.expected.id + ".json");
	const auto bytes = ReadFile(path);
	auto changed = nlohmann::json::parse(bytes);
	auto& execution = changed.at("managedContinuation").at("execution");
	auto configuration = nlohmann::json::parse(execution.at("connectionConfigurationJson").get<std::string>());
	if (configuration.is_array())
		configuration.push_back("private-bound-history-drift");
	else
	{
		UAM_ASSERT(configuration.is_object());
		configuration["privateBoundHistoryDrift"] = true;
	}
	execution["connectionConfigurationJson"] = configuration.dump();
	const auto changed_bytes = changed.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(path, changed_bytes));
	const auto altered = uam::AgentRunLedger::LoadAllReadOnly(fixture.crash.root);
	UAM_ASSERT(altered.errors.empty() && altered.runs.size() == 1 && altered.runs.front().managed_continuation->execution);
	UAM_ASSERT(altered.runs.front().managed_continuation->execution->connection_configuration_json != fixture.expected.managed_continuation->execution->connection_configuration_json);
	auto canonical_except_private = altered.runs.front().managed_continuation;
	canonical_except_private->execution->connection_configuration_json = fixture.expected.managed_continuation->execution->connection_configuration_json;
	UAM_ASSERT(canonical_except_private == fixture.expected.managed_continuation); // All typed receipt/owner/other binding fields are unchanged.
	auto before_public = nlohmann::json::parse(bytes);
	before_public.at("managedContinuation").at("execution") = nullptr;
	changed.at("managedContinuation").at("execution") = nullptr;
	UAM_ASSERT_EQ(changed, before_public); // The sanitized record is identical; private digest must still protect the captured read.
	fixture.Drain();
	UAM_ASSERT_EQ(callback->successes, 0);
	UAM_ASSERT_EQ(callback->failures, 1);
	UAM_ASSERT(!callback->error.empty() && callback->body.is_null());
	fixture.NoAuthority();
	UAM_ASSERT_EQ(ReadFile(path), changed_bytes);    // No repair or fallback rewrites the tampered canonical record.
	UAM_ASSERT(uam::io::WriteTextFile(path, bytes)); // Restore evidence only; never admit execution or refresh the catalogue.
	fixture.Unchanged();
	const auto restored = fixture.Request("getManagedAgentTranscript", fixture.OpenPayload());
	UAM_ASSERT_EQ(restored->successes, 1); // Defeat blanket refusal, using the unchanged catalogue and original producer bytes.
	fixture.Unchanged();
}

UAM_TEST(ManagedInitialBoundRestartRejectsTranscriptNativeMismatchCapturedBeforeStartup)
{
	{
		BoundRestartInspectionFixtureR96 healthy;
		const auto listed = healthy.Request("listManagedSubagents", {{"chatId", healthy.original.root_id}});
		UAM_ASSERT_EQ(listed->successes, 1);
		UAM_ASSERT_EQ(listed->body.at("totalCount").get<std::size_t>(), std::size_t{1});
		const auto opened = healthy.Request("getManagedAgentTranscript", healthy.OpenPayload());
		UAM_ASSERT_EQ(opened->successes, 1);
		UAM_ASSERT(opened->body.at("lastReported").get<bool>());
		UAM_ASSERT_EQ(opened->body.at("messages"), uam::StateSerializer::SerializeSession(healthy.transcript).value("messages", nlohmann::json::array()));
		healthy.Unchanged();
	} // Dispose the healthy private peer/AppState/PATH before the negative fixture.
	BoundRestartInspectionFixtureR96 changed(true);
	const auto primary = AppPaths::UamChatFilePath(changed.crash.root, changed.expected.transcript_chat_id);
	const auto copied = ChatRepository::LoadLocalChatBounded(changed.crash.root, changed.expected.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(copied && copied->native_session_id != changed.expected.managed_continuation->execution->native_session_id);
	const auto before = ReadFile(primary);
	const auto listed = changed.Request("listManagedSubagents", {{"chatId", changed.original.root_id}});
	UAM_ASSERT_EQ(listed->successes, 0); // A startup-captured transcript must still match the original binding, not merely itself.
	UAM_ASSERT_EQ(listed->failures, 1);
	UAM_ASSERT(!listed->error.empty() && listed->body.is_null());
	const auto opened = changed.Request("getManagedAgentTranscript", changed.OpenPayload());
	UAM_ASSERT_EQ(opened->successes, 0);
	UAM_ASSERT_EQ(opened->failures, 1);
	UAM_ASSERT(!opened->error.empty() && opened->body.is_null());
	UAM_ASSERT_EQ(ReadFile(primary), before);
	changed.Unchanged(); // Original producer bytes remain exact; only the copied negative primary stays changed.
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
namespace
{
	struct ManagedBindingAttemptR96
	{
		struct Completion
		{
			std::optional<uam::ManagedInitialMentionResult> value;
			std::size_t count = 0;
		};
		std::shared_ptr<Completion> completion = std::make_shared<Completion>();
		AgentRun initial, held;
		std::string text, inputs;
		pid_t pid = 0;
	};

	void ManagedBindingRuntimePulseR96(ManagedClaudeLifecycleFixture& fixture)
	{
		fixture.AssertPeerFiles();
		fixture.ObserveProcesses();
		(void)uam::PollAllAcpSessions(fixture.app);
		(void)uam::FlushPendingChatSaves(fixture.app);
		fixture.ObserveProcesses();
		if (fs::exists(fixture.temp.root / "peer-error.json"))
			throw TestFailure("Binding private peer prerequisite: " + ReadFile(fixture.temp.root / "peer-error.json").substr(0, 512));
	}

	template <typename Predicate> void ManagedBindingAwaitRuntimeR96(ManagedClaudeLifecycleFixture& fixture, Predicate done)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!done() && std::chrono::steady_clock::now() < deadline)
		{
			ManagedBindingRuntimePulseR96(fixture); // Deliberately no scheduler turn.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT(done());
	}

	ManagedBindingAttemptR96 ManagedBindingBeginR96(ManagedClaudeLifecycleFixture& fixture)
	{
		ManagedBindingAttemptR96 attempt;
		attempt.text = "@" + fixture.definition.id + " " + fixture.task;
		std::string error;
		fixture.AssertPeerFiles();
		UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(
		                  fixture.app, fixture.root_id, attempt.text,
		                  [completion = attempt.completion](const uam::ManagedInitialMentionResult& result)
		                  {
			                  ++completion->count;
			                  completion->value = result;
			                  return true;
		                  },
		                  &error),
		              uam::ManagedInitialMentionAdmission::Pending);
		fixture.Await([&] { return attempt.completion->value.has_value(); });
		const auto& receipt = *attempt.completion->value;
		if (!receipt.input_recorded || receipt.delegation_state != uam::ManagedInitialDelegationState::Committed)
			throw TestFailure("Binding creation prerequisite: " + receipt.error.substr(0, 512));
		UAM_ASSERT_EQ(attempt.completion->count, std::size_t{1});
		UAM_ASSERT(receipt.parent_message && receipt.parent_message->content == attempt.text);
		UAM_ASSERT_EQ(receipt.parent_message_index, fixture.original.size());
		attempt.initial = fixture.ReadRun();
		fixture.run_id = attempt.initial.id;
		UAM_ASSERT_EQ(receipt.run_id, attempt.initial.id);
		UAM_ASSERT_EQ(receipt.transcript_chat_id, attempt.initial.transcript_chat_id);
		UAM_ASSERT(attempt.initial.managed_continuation && !attempt.initial.managed_continuation->execution);
		UAM_ASSERT_EQ(attempt.initial.managed_continuation->turns.size(), std::size_t{1});
		fixture.Await([&] { return fs::exists(fixture.temp.root / "input-witness.json"); });
		attempt.held = fixture.ReadRun();
		const auto input = nlohmann::json::parse(ReadFile(fixture.temp.root / "input-witness.json"));
		attempt.pid = input.at("pid").get<pid_t>();
		const auto* session = uam::FindAcpSessionForChat(fixture.app, attempt.initial.transcript_chat_id);
		UAM_ASSERT(session && session->running && session->processing && session->session_id.empty());
		UAM_ASSERT_EQ(session->child_pid, attempt.pid);
		UAM_ASSERT(attempt.pid > 0 && attempt.held.status == "running" && !attempt.held.managed_continuation->execution);
		UAM_ASSERT(input.at("run").at("managedContinuation").at("execution").is_null());
		UAM_ASSERT_EQ(session->turn_serial, attempt.held.expected_turn_serial);
		UAM_ASSERT_EQ(attempt.held.expected_turn_serial, input.at("turn").at("expectedTurnSerial").get<int>());
		const auto& turn = attempt.held.managed_continuation->turns.front();
		UAM_ASSERT(turn.dispatch_attempted && turn.expected_turn_serial > 0 && turn.first_user_message_index >= 0);
		UAM_ASSERT_EQ(turn.user_message_created_at, input.at("turn").at("userMessageCreatedAt").get<std::string>());
		attempt.inputs = ReadFile(fixture.temp.root / "task-inputs.jsonl");
		std::istringstream stream(attempt.inputs);
		std::string line;
		std::size_t count = 0;
		while (std::getline(stream, line))
			if (!line.empty())
			{
				(void)nlohmann::json::parse(line);
				++count;
			}
		UAM_ASSERT_EQ(count, std::size_t{1});
		return attempt;
	}

	void ManagedBindingReleaseAndSettleR96(ManagedClaudeLifecycleFixture& fixture, const ManagedBindingAttemptR96& attempt)
	{
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-completion", "release the actual one-shot native proof\n"));
		ManagedBindingAwaitRuntimeR96(fixture,
		                              [&]
		                              {
			                              const auto* session = uam::FindAcpSessionForChat(fixture.app, attempt.initial.transcript_chat_id);
			                              return session && !session->processing && session->last_settled_turn_serial == attempt.held.expected_turn_serial && session->last_turn_outcome == "completed" && fs::exists(fixture.temp.root / "native-witness.json");
		                              });
		const auto* session = uam::FindAcpSessionForChat(fixture.app, attempt.initial.transcript_chat_id);
		UAM_ASSERT(session && session->last_turn_error.empty());
		UAM_ASSERT_EQ(fixture.ReadRun().status, std::string("running")); // Native settlement has not itself published terminal storage.
	}

	AgentRun ManagedBindingFinishR96(ManagedClaudeLifecycleFixture& fixture, const ManagedBindingAttemptR96& attempt, bool expect_binding)
	{
		fixture.Await(
		    [&]
		    {
			    const auto run = fixture.ReadRun();
			    return run.status == "completed" && run.root_result_delivered;
		    });
		const auto completed = fixture.ReadRun();
		const auto native = nlohmann::json::parse(ReadFile(fixture.temp.root / "native-witness.json")).at("native_id").get<std::string>();
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(native));
		UAM_ASSERT_EQ(completed.id, attempt.initial.id);
		UAM_ASSERT_EQ(completed.root_chat_id, attempt.initial.root_chat_id);
		UAM_ASSERT_EQ(completed.transcript_chat_id, attempt.initial.transcript_chat_id);
		UAM_ASSERT_EQ(completed.task, attempt.initial.task);
		UAM_ASSERT_EQ(completed.provider_id, attempt.initial.provider_id);
		UAM_ASSERT_EQ(completed.model_id, attempt.initial.model_id);
		UAM_ASSERT_EQ(completed.definition_hash, fixture.definition.definition_hash);
		UAM_ASSERT_EQ(completed.definition_snapshot, attempt.initial.definition_snapshot);
		UAM_ASSERT_EQ(completed.definition_instructions, attempt.initial.definition_instructions);
		UAM_ASSERT_EQ(completed.skills_snapshot, attempt.initial.skills_snapshot);
		UAM_ASSERT_EQ(completed.delegates_snapshot, attempt.initial.delegates_snapshot);
		UAM_ASSERT_EQ(completed.effective_workspace_access, std::string("read"));
		UAM_ASSERT(completed.diagnostic_code.empty() && completed.diagnostic.empty());
		UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
		UAM_ASSERT(completed.managed_continuation && completed.managed_continuation->owner == attempt.initial.managed_continuation->owner);
		UAM_ASSERT_EQ(completed.managed_continuation->current_turn_id, completed.id);
		UAM_ASSERT_EQ(completed.managed_continuation->current_turn_number, std::uint32_t{1});
		UAM_ASSERT_EQ(completed.managed_continuation->turns.size(), std::size_t{1});
		const auto& turn = completed.managed_continuation->turns.front();
		const auto& held_turn = attempt.held.managed_continuation->turns.front();
		UAM_ASSERT_EQ(turn.status, uam::ManagedRunTurnStatus::Completed);
		UAM_ASSERT(turn.dispatch_attempted && turn.result_required && turn.result_delivered);
		UAM_ASSERT_EQ(turn.expected_turn_serial, held_turn.expected_turn_serial);
		UAM_ASSERT_EQ(completed.expected_turn_serial, held_turn.expected_turn_serial);
		UAM_ASSERT_EQ(turn.first_user_message_index, held_turn.first_user_message_index);
		UAM_ASSERT_EQ(turn.user_message_created_at, held_turn.user_message_created_at);
		UAM_ASSERT_EQ(turn.delivery_id, attempt.initial.id);
		UAM_ASSERT_EQ(turn.principal, held_turn.principal);
		UAM_ASSERT(turn.text.empty() && turn.client_request_id.empty() && turn.after_turn_id.empty());
		UAM_ASSERT(completed.started_at_epoch_ms > 0 && completed.finished_at_epoch_ms >= completed.started_at_epoch_ms);
		UAM_ASSERT_EQ(turn.started_at_epoch_ms, completed.started_at_epoch_ms);
		UAM_ASSERT_EQ(turn.finished_at_epoch_ms, completed.finished_at_epoch_ms);
		const auto transcript = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, completed.transcript_chat_id, 4 * 1024 * 1024);
		const auto root = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		UAM_ASSERT(root && transcript);
		UAM_ASSERT_EQ(root->managed_followup_incarnation, completed.managed_continuation->owner.root_birth_id);
		UAM_ASSERT_EQ(transcript->managed_followup_incarnation, completed.managed_continuation->owner.transcript_birth_id);
		UAM_ASSERT_EQ(transcript->agent_run_id, completed.id);
		UAM_ASSERT_EQ(transcript->native_session_id, native);
		UAM_ASSERT_EQ(transcript->model_id, fixture.model);
		UAM_ASSERT_EQ(transcript->execution_host_id, std::string("local"));
		UAM_ASSERT_EQ(transcript->workspace_directory, (fixture.temp.root / "workspace").string());
		UAM_ASSERT_EQ(transcript->approval_mode, std::string("plan"));
		UAM_ASSERT_EQ(std::ranges::count(transcript->messages, fixture.task, &Message::content), 1);
		UAM_ASSERT_EQ(std::ranges::count(transcript->messages, fixture.answer, &Message::content), 1);
		UAM_ASSERT(root->messages.size() >= fixture.original.size() + 1);
		UAM_ASSERT(std::equal(fixture.original.begin(), fixture.original.end(), root->messages.begin()));
		UAM_ASSERT_EQ(std::ranges::count(root->messages, attempt.text, &Message::content), 1);
		const auto result_count = [&]
		{
			const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
			UAM_ASSERT(saved);
			std::size_t count = 0;
			for (const auto& message : saved->messages)
				for (const auto& tool : message.tool_calls)
					if (tool.id == "uam-agent-result-" + turn.delivery_id)
					{
						++count;
						UAM_ASSERT(tool.is_sub_agent && tool.sub_agent_id == completed.id);
						UAM_ASSERT_EQ(nlohmann::json::parse(tool.result_text).at("result").get<std::string>(), fixture.answer);
					}
			return count;
		};
		UAM_ASSERT_EQ(result_count(), std::size_t{1});
		for (int poll = 0; poll < 100; ++poll)
		{
			fixture.Pulse();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT_EQ(result_count(), std::size_t{1});
		UAM_ASSERT_EQ(ReadFile(fixture.temp.root / "task-inputs.jsonl"), attempt.inputs);
		UAM_ASSERT_EQ(attempt.completion->count, std::size_t{1});
		fixture.Await([&] { return fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty(); });
		UAM_ASSERT(fixture.StopAndReap());
		UAM_ASSERT(fixture.ReadRun().managed_continuation == completed.managed_continuation);
		UAM_ASSERT_EQ(completed.managed_continuation->execution.has_value(), expect_binding);
		if (expect_binding)
		{
			const auto& binding = *completed.managed_continuation->execution;
			UAM_ASSERT_EQ(binding.native_session_id, native);
			UAM_ASSERT_EQ(binding.effective_model_id, fixture.model);
			UAM_ASSERT_EQ(binding.model_identity_source, std::string("exact-requested"));
			UAM_ASSERT_EQ(binding.approval_mode, std::string("plan"));
		}
		const auto state = uam::ManagedSubagentFollowupService::GetState(fixture.app, fixture.root_id, completed.id);
		UAM_ASSERT_EQ(state.can_send, expect_binding);
		UAM_ASSERT_EQ(state.code, expect_binding ? uam::ManagedFollowupCode::None : uam::ManagedFollowupCode::ModelIdentityUnverified);
		return completed;
	}

	void ManagedBindingInstallRawPeerR96(ManagedClaudeLifecycleFixture& fixture, const std::string& fault)
	{
		std::string peer = ManagedClaudeLifecyclePeer();
		const auto begin = peer.find("        emit(init)\n");
		const auto end = peer.find("        event('completion', native_id=native)\n", begin);
		UAM_ASSERT(begin != std::string::npos && end != std::string::npos);
		const std::string replacement = "        fault = " + nlohmann::json(fault).dump() + R"PEER(
        inits = [dict(init)]
        if fault == 'wrong-mode':
            inits[0]['permissionMode'] = 'default'
            inits.append(dict(init))
        elif fault == 'duplicate-init':
            conflict = dict(init)
            conflict['model'] = config['model'] + '-contradictory'
            inits.extend([conflict, dict(init)])
        result = dict(type='result', subtype='success', is_error=False, session_id=native,
            result=config['answer'], duration_ms=1, num_turns=1)
        if fault == 'result-model':
            result['model'] = config['model'] + '-contradictory'
        assistant = dict(type='assistant', session_id=native, message=dict(id=str(uuid.uuid4()), role='assistant',
            content=[dict(type='text', text=config['answer'])]))
        protocol = inits + [assistant, result]
        if fault == 'result-model':
            protocol.append(dict(init))
        store('raw-evidence-witness.json', dict(fault=fault, protocol=protocol, native_id=native))
        # One bounded real pipe write keeps the contradictory event and repair in one native burst.
        wire = ''.join(json.dumps(value) + '\n' for value in protocol).encode('utf-8')
        assert len(wire) <= 4096
        assert os.write(sys.stdout.fileno(), wire) == len(wire)
)PEER";
		peer.replace(begin, end - begin, replacement);
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "peer.py", peer));
		fs::permissions(fixture.temp.root / "peer.py", fs::perms::owner_all, fs::perm_options::replace);
		fixture.AssertPeerFiles(); // Atomic rewrites do not inherit the previous executable mode.
	}

	struct ManagedBindingStagedR96
	{
		fs::path path;
		std::string bytes;
		nlohmann::json value;
	};

	ManagedBindingStagedR96 ManagedBindingHoldTerminalCandidateR96(ManagedClaudeLifecycleFixture& fixture, const ManagedBindingAttemptR96& attempt)
	{
		fixture.AssertPeerFiles();
		(void)uam::StopAcpSession(fixture.app, attempt.initial.transcript_chat_id); // Native completed settlement already happened; retain its real Stop receipt.
		ManagedBindingAwaitRuntimeR96(fixture,
		                              [&]
		                              {
			                              const auto* session = uam::FindAcpSessionForChat(fixture.app, attempt.initial.transcript_chat_id);
			                              return session && !session->running && !session->local_stop_pending && !session->remote_stop_pending && !session->remote_stop_unconfirmed && fixture.app.acp_process_stop_tasks.empty() && !fixture.app.pending_chat_save_at_by_chat_id.contains(attempt.initial.transcript_chat_id);
		                              });
		errno = 0;
		UAM_ASSERT(kill(attempt.pid, 0) < 0 && errno == ESRCH);
		UAM_ASSERT_EQ(fixture.ReadRun().status, std::string("running"));
		fixture.AssertPeerFiles();
		(void)uam::AgentRunScheduler::Poll(fixture.app); // Exactly one owner turn starts detached terminal preparation.
		const auto directory = fixture.app.data_root / "agent-runs";
		const auto prefix = attempt.initial.id + ".json.tmp.";
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (std::chrono::steady_clock::now() < deadline)
		{
			fixture.AssertPeerFiles();
			UAM_ASSERT_EQ(fixture.ReadRun().status, std::string("running"));
			for (const auto& entry : fs::directory_iterator(directory))
			{
				if (!entry.path().filename().string().starts_with(prefix) || !entry.is_regular_file() || entry.is_symlink() || entry.file_size() > 512U * 1024U)
					continue;
				const auto bytes = ReadFile(entry.path());
				const auto value = nlohmann::json::parse(bytes, nullptr, false);
				if (!value.is_object() || value.value("id", std::string{}) != attempt.initial.id || value.value("status", std::string{}) != "completed")
					continue;
				const auto& continuation = value.at("managedContinuation");
				UAM_ASSERT_EQ(value.at("version").get<int>(), 2);
				UAM_ASSERT(continuation.at("execution").is_object()); // Genuine positive producer/worker prerequisite before ANY drift.
				UAM_ASSERT_EQ(continuation.at("execution").at("nativeSessionId"), nlohmann::json::parse(ReadFile(fixture.temp.root / "native-witness.json")).at("native_id"));
				UAM_ASSERT_EQ(continuation.at("execution").at("effectiveModelId").get<std::string>(), fixture.model);
				UAM_ASSERT_EQ(continuation.at("stateRevision").get<std::uint64_t>(), attempt.held.managed_continuation->state_revision + 1);
				UAM_ASSERT_EQ(continuation.at("turns").at(0).at("status").get<std::string>(), std::string("completed"));
				UAM_ASSERT(!value.at("rootResultDelivered").get<bool>());
				UAM_ASSERT(!fixture.ReadRun().managed_continuation->execution);
				return {entry.path(), bytes, value};
			}
			// No scheduler, runtime, or Flush call may publish/rewrite canonical state in this held interval.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		throw TestFailure("No actual completed bound ledger candidate appeared before terminal publication.");
	}
} // namespace

UAM_TEST(ManagedInitialClaudeContradictoryRawEvidenceCannotRepairBinding)
{
	for (const std::string fault : {"none", "wrong-mode", "duplicate-init", "result-model"})
	{
		std::cout << "Managed binding raw-evidence arm: " << fault << '\n';
		ManagedClaudeLifecycleFixture fixture(true);
		ManagedBindingInstallRawPeerR96(fixture, fault);
		const auto attempt = ManagedBindingBeginR96(fixture);
		ManagedBindingReleaseAndSettleR96(fixture, attempt);
		ManagedBindingAwaitRuntimeR96(fixture,
		                              [&]
		                              {
			                              const auto* session = uam::FindAcpSessionForChat(fixture.app, attempt.initial.transcript_chat_id);
			                              return fs::exists(fixture.temp.root / "raw-evidence-witness.json") && session && session->current_model_id == fixture.model && session->current_mode_id == "plan";
		                              });
		const auto witness = nlohmann::json::parse(ReadFile(fixture.temp.root / "raw-evidence-witness.json"));
		const auto& protocol = witness.at("protocol");
		UAM_ASSERT_EQ(witness.at("fault").get<std::string>(), fault);
		if (fault == "wrong-mode")
		{
			UAM_ASSERT_EQ(protocol.at(0).at("permissionMode").get<std::string>(), std::string("default"));
			UAM_ASSERT_EQ(protocol.at(1).at("permissionMode").get<std::string>(), std::string("plan"));
		}
		else if (fault == "duplicate-init")
		{
			UAM_ASSERT_EQ(protocol.at(0).at("model").get<std::string>(), fixture.model);
			UAM_ASSERT(protocol.at(1).at("model").get<std::string>() != fixture.model);
			UAM_ASSERT_EQ(protocol.at(2).at("model").get<std::string>(), fixture.model);
		}
		else if (fault == "result-model")
		{
			UAM_ASSERT_EQ(protocol.at(2).at("type").get<std::string>(), std::string("result"));
			UAM_ASSERT(protocol.at(2).at("model").get<std::string>() != fixture.model);
			UAM_ASSERT_EQ(protocol.at(3).at("model").get<std::string>(), fixture.model);
		}
		(void)ManagedBindingFinishR96(fixture, attempt, fault == "none");
	}
}

UAM_TEST(ManagedInitialPreparedBindingDriftCompletesWithoutReplay)
{
	for (const std::string drift : {"none", "root-revision", "child-configuration"})
	{
		std::cout << "Managed binding prepared-candidate arm: " << drift << '\n';
		ManagedClaudeLifecycleFixture fixture(true);
		const auto attempt = ManagedBindingBeginR96(fixture);
		ManagedBindingReleaseAndSettleR96(fixture, attempt);
		const auto staged = ManagedBindingHoldTerminalCandidateR96(fixture, attempt);
		const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.root_id);
		UAM_ASSERT(revision && *revision != "missing");
		if (drift == "root-revision")
		{
			auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
			UAM_ASSERT(root && root->metadata_incarnation == fixture.root_incarnation);
			const auto messages = root->messages;
			const auto birth = root->managed_followup_incarnation;
			UAM_ASSERT(ChatHistorySyncService().RenameChat(fixture.app, *root, "Allowed ordinary parent rename after terminal binding preparation"));
			UAM_ASSERT(root->messages == messages && root->managed_followup_incarnation == birth);
			UAM_ASSERT(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.root_id) != revision);
		}
		else if (drift == "child-configuration")
		{
			auto* child = ChatDomainService().FindChatById(fixture.app, attempt.initial.transcript_chat_id);
			UAM_ASSERT(child && child->model_id == fixture.model && child->reasoning_effort.empty());
			child->reasoning_effort = "high"; // A real live child setting, never an owner/native/binding setter.
			UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.root_id), revision);
		}
		UAM_ASSERT_EQ(ReadFile(staged.path), staged.bytes); // Test never edits the owned immutable preparation.
		UAM_ASSERT_EQ(fixture.ReadRun().status, std::string("running"));
		const auto completed = ManagedBindingFinishR96(fixture, attempt, drift == "none");
		if (drift == "root-revision")
		{
			const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
			UAM_ASSERT(saved && saved->title == "Allowed ordinary parent rename after terminal binding preparation");
		}
		UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
	}
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
#include <iterator>
namespace
{
	// Separate credential-free transport. The original initial peer and all old callers stay unchanged.
	const char* ManagedFollowupResumePeerR98()
	{
		return R"UAM_FOLLOWUP(import json, os, pathlib, sys, time, uuid
root = pathlib.Path(__file__).resolve().parent
config = json.loads((root / 'peer-config.json').read_text())
data = pathlib.Path(config['data_root'])

def store(name, value):
    target = root / name
    temporary = root / (name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, sort_keys=True)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(target)

def event(kind, **fields):
    with (root / 'peer-events.jsonl').open('a') as stream:
        stream.write(json.dumps(dict(kind=kind, **fields), sort_keys=True) + '\n')
        stream.flush()
        os.fsync(stream.fileno())

def read_document(path):
    assert path.is_file() and not path.is_symlink() and path.stat().st_size <= 4 * 1024 * 1024
    return json.loads(path.read_text())

def state(number):
    runs = [read_document(path) for path in (data / 'agent-runs').glob('*.json')]
    runs = [run for run in runs if run['rootChatId'] == config['root_id']]
    assert len(runs) == 1, 'one real ledger, not a new run per follow-up'
    run = runs[0]
    assert run['version'] == 2
    managed = run['managedContinuation']
    assert managed['currentTurnNumber'] == number and len(managed['turns']) == number
    turn = managed['turns'][-1]
    assert managed['currentTurnId'] == turn['turnId'] and turn['turnNumber'] == number
    assert turn['status'] == run['status'] == 'running' and turn['dispatchAttempted']
    transcript = read_document(data / 'chats' / (run['transcriptChatId'] + '.json'))
    parent = read_document(data / 'chats' / (run['rootChatId'] + '.json'))
    assert transcript['agent_run_id'] == run['id']
    assert parent['managed_followup_incarnation'] == managed['owner']['rootBirthId']
    assert transcript['managed_followup_incarnation'] == managed['owner']['transcriptBirthId']
    assert transcript['provider_id'] == run['providerId'] == 'claude-cli'
    assert transcript['model_id'] == run['modelId'] == config['model']
    assert transcript['approval_mode'] == 'plan' and transcript['execution_host_id'] == 'local'
    if number == 1:
        assert turn['turnId'] == run['id'] and managed['execution'] is None
        assert transcript.get('native_session_id', '') == ''
    else:
        history = read_document(root / 'private-native-history.json')
        assert history['run_id'] == run['id'] and history['transcript_id'] == run['transcriptChatId']
        assert history['task'] == config['task'] and history['answer'] == config['answer']
        assert managed['turns'][0]['status'] == 'completed' and managed['turns'][0]['resultDelivered']
        assert turn['afterTurnId'] == managed['turns'][0]['turnId'] and turn['text'] == config['followup_task']
        assert managed['execution']['nativeSessionId'] == history['native_id'] == transcript['native_session_id']
        assert managed['execution']['effectiveModelId'] == config['model']
        assert managed['execution']['approvalMode'] == 'plan'
        assert any(row['role'] == 'user' and row['content'] == history['task'] for row in transcript['messages'])
        assert any(row['role'] == 'assistant' and row['content'] == history['answer'] for row in transcript['messages'])
    return run, turn, transcript

def emit(value):
    print(json.dumps(value), flush=True)

try:
    if sys.argv[1:] == ['--version']:
        event('version', executable=sys.argv[0], pid=os.getpid())
        print('2.1.31 (Claude Code)', flush=True)
        sys.exit(0)
    args = sys.argv[1:]
    assert '-p' in args and args[args.index('--input-format') + 1] == 'stream-json'
    assert args[args.index('--output-format') + 1] == 'stream-json'
    assert args[args.index('--model') + 1] == config['model']
    assert args[args.index('--permission-mode') + 1] == 'plan' and '--mcp-config' not in args
    number = 2 if (root / 'private-native-history.json').exists() else 1
    run, turn, transcript = state(number)
    if number == 1:
        assert '--resume' not in args
        store('launch-witness.json', dict(pid=os.getpid(), args=args, run=run))
    else:
        history = read_document(root / 'private-native-history.json')
        assert args.count('--resume') == 1 and args[args.index('--resume') + 1] == history['native_id']
        # Existing runtime may retire the outbox before initialize. Validate its carrier only if observed.
        prompts = [prompt for prompt in transcript.get('acp_queued_prompts', []) if prompt.get('id') == turn['turnId']]
        assert len(prompts) <= 1
        if prompts:
            prompt = prompts[0]
            assert prompt['text'] == config['followup_task'] and prompt['managed_turn'] == dict(
                run_id=run['id'], turn_id=turn['turnId'], turn_number=2,
                expected_native_session_id=history['native_id'], continuity='native')
        store('resume-launch-witness.json', dict(pid=os.getpid(), args=args, run=run,
            transcript=transcript, prior_history=history))
    event('launch', pid=os.getpid(), turn_number=number)
    inputs = 0
    for line in sys.stdin:
        message = json.loads(line)
        if message.get('type') == 'control_request':
            subtype = message['request']['subtype']
            assert subtype == 'initialize', 'unexpected setup/control request: ' + subtype
            event('initialize', request_id=message['request_id'], turn_number=number)
            emit(dict(type='control_response', response=dict(subtype='success',
                request_id=message['request_id'], response=dict(commands=[], models=[]))))
            continue
        assert message.get('type') == 'user'
        inputs += 1
        assert inputs == 1, 'no original-task resend, fallback, or duplicate input on this owned process'
        with (root / 'task-inputs.jsonl').open('a') as stream:
            stream.write(json.dumps(message, sort_keys=True) + '\n')
            stream.flush()
            os.fsync(stream.fileno())
        run, turn, transcript = state(number)
        assert turn['expectedTurnSerial'] > 0 and run['expectedTurnSerial'] == turn['expectedTurnSerial']
        index = turn['firstUserMessageIndex']
        assert index >= 0
        task = config['task'] if number == 1 else config['followup_task']
        answer = config['answer'] if number == 1 else config['followup_answer']
        row = transcript['messages'][index]
        assert row['role'] == 'user' and row['content'] == task
        assert row['created_at'] == turn['userMessageCreatedAt'] and row['created_at']
        assert not row.get('acp_prompt_not_sent', False), 'wire input before persisted owned delivery intent'
        parts = message['message']['content']
        text = parts if isinstance(parts, str) else ''.join(part.get('text', '') for part in parts)
        assert text.count(task) == 1
        if number == 2:
            assert config['task'] not in text, 'native continuation must not resend the original task'
        for prompt in transcript.get('acp_queued_prompts', []):
            if prompt.get('managed_turn') is not None:
                assert prompt['managed_turn']['expected_native_session_id']
        witness = 'input-witness.json' if number == 1 else 'followup-input-witness.json'
        store(witness, dict(pid=os.getpid(), run=run, turn=turn, transcript=transcript, message=message))
        event('task-input', pid=os.getpid(), turn_number=number)
        release = 'allow-completion' if number == 1 else 'allow-followup-completion'
        deadline = time.monotonic() + 45
        while not (root / release).exists():
            assert time.monotonic() < deadline, 'test did not release actual native completion'
            time.sleep(0.005)
        if number == 1:
            native = str(uuid.uuid4())
            store('private-native-history.json', dict(native_id=native, run_id=run['id'],
                transcript_id=run['transcriptChatId'], task=task, answer=answer, inputs=[task], answers=[answer]))
            store('native-witness.json', dict(native_id=native, model_included=True))
        else:
            history = read_document(root / 'private-native-history.json')
            native = history['native_id']
            assert history['inputs'] == [config['task']] and history['answers'] == [config['answer']]
            history['inputs'].append(task)
            history['answers'].append(answer)
            store('private-native-history.json', history)
        # Raw correlated owned observations, not initialize catalogues or optimistic session fields.
        emit(dict(type='system', subtype='init', session_id=native, model=config['model'], permissionMode='plan'))
        emit(dict(type='assistant', session_id=native, message=dict(id=str(uuid.uuid4()), role='assistant',
            content=[dict(type='text', text=answer)])))
        emit(dict(type='result', subtype='success', is_error=False, session_id=native,
            result=answer, duration_ms=1, num_turns=1))
        event('completion', native_id=native, turn_number=number)
    event('eof', pid=os.getpid(), turn_number=number)
except Exception as error:
    store('peer-error.json', dict(error=str(error)))
    sys.exit(70)
)UAM_FOLLOWUP";
	}

	std::vector<nlohmann::json> ManagedFollowupInputsR98(const ManagedClaudeLifecycleFixture& fixture)
	{
		std::vector<nlohmann::json> inputs;
		std::istringstream stream(ReadFile(fixture.temp.root / "task-inputs.jsonl"));
		std::string line;
		while (std::getline(stream, line))
			if (!line.empty())
				inputs.push_back(nlohmann::json::parse(line));
		return inputs;
	}

	void ManagedFollowupResultR98(const ChatSession& root, const std::string& run_id,
	    const std::string& delivery_id, const std::string& answer, const std::string& turn_id = {},
	    std::uint32_t turn_number = 0)
	{
		std::size_t count = 0;
		for (const auto& message : root.messages)
			for (const auto& tool : message.tool_calls)
				if (tool.id == "uam-agent-result-" + delivery_id)
				{
					++count;
					UAM_ASSERT(tool.is_sub_agent && tool.sub_agent_id == run_id);
					const auto result = nlohmann::json::parse(tool.result_text);
					UAM_ASSERT_EQ(result.at("runId").get<std::string>(), run_id);
					UAM_ASSERT_EQ(result.at("result").get<std::string>(), answer);
					if (turn_number > 1)
					{
						UAM_ASSERT_EQ(result.at("turnId").get<std::string>(), turn_id);
						UAM_ASSERT_EQ(result.at("turnNumber").get<std::uint32_t>(), turn_number);
					}
				}
		UAM_ASSERT_EQ(count, std::size_t{1});
	}
} // namespace

UAM_TEST(ManagedFollowupAsyncResumesTheGenuineCompletedNativeChild)
{
	ManagedClaudeLifecycleFixture fixture(true); // Existing isolated handler factory; no account readers.
	const std::string next_task = "Continue the existing private native conversation and report only the second answer.";
	const std::string next_answer = "PRIVATE-MANAGED-SAME-NATIVE-SECOND-ANSWER";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "peer.py", ManagedFollowupResumePeerR98()));
	fs::permissions(fixture.temp.root / "peer.py", fs::perms::owner_all, fs::perm_options::replace);
	fixture.AssertPeerFiles(); // Restored executable mode after the atomic rewrite, before any launch.
	auto config = nlohmann::json::parse(ReadFile(fixture.temp.root / "peer-config.json"));
	config["followup_task"] = next_task;
	config["followup_answer"] = next_answer;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "peer-config.json", config.dump()));

	// Genuine original producer, actual owned input, raw init/model/mode/native and canonical save.
	const auto first = ManagedBindingBeginR96(fixture);
	ManagedBindingReleaseAndSettleR96(fixture, first);
	fixture.Await([&]
	{
		const auto run = fixture.ReadRun();
		return run.status == "completed" && run.root_result_delivered && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty();
	});
	const AgentRun predecessor = fixture.ReadRun();
	UAM_ASSERT(predecessor.managed_continuation && predecessor.managed_continuation->execution);
	const auto& before = *predecessor.managed_continuation;
	UAM_ASSERT_EQ(before.turns.size(), std::size_t{1});
	UAM_ASSERT_EQ(before.current_turn_id, predecessor.id);
	UAM_ASSERT_EQ(before.current_turn_number, std::uint32_t{1});
	UAM_ASSERT_EQ(before.turns.front().status, uam::ManagedRunTurnStatus::Completed);
	UAM_ASSERT(before.turns.front().dispatch_attempted && before.turns.front().result_required && before.turns.front().result_delivered);
	UAM_ASSERT_EQ(before.turns.front().expected_turn_serial, first.held.expected_turn_serial);
	UAM_ASSERT_EQ(before.turns.front().first_user_message_index, first.held.managed_continuation->turns.front().first_user_message_index);
	UAM_ASSERT_EQ(predecessor.task, fixture.task);
	UAM_ASSERT_EQ(predecessor.result_excerpt, fixture.answer);
	UAM_ASSERT_EQ(predecessor.definition_hash, fixture.definition.definition_hash);
	UAM_ASSERT_EQ(predecessor.definition_snapshot, fixture.definition.markdown_snapshot);
	UAM_ASSERT_EQ(predecessor.effective_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(before.owner, first.initial.managed_continuation->owner);
	UAM_ASSERT(!before.owner.delegation_grant);
	const auto native_history = nlohmann::json::parse(ReadFile(fixture.temp.root / "private-native-history.json"));
	const std::string native = native_history.at("native_id").get<std::string>();
	UAM_ASSERT(uam::uuid::IsCanonicalUuid(native));
	UAM_ASSERT_EQ(before.execution->native_session_id, native);
	UAM_ASSERT_EQ(before.execution->effective_model_id, fixture.model);
	UAM_ASSERT_EQ(before.execution->model_identity_source, std::string("exact-requested"));
	UAM_ASSERT_EQ(before.execution->approval_mode, std::string("plan"));
	UAM_ASSERT_EQ(before.execution->execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(before.execution->workspace_directory, (fixture.temp.root / "workspace").string());
	UAM_ASSERT(!before.execution->connection_configuration_json.empty() && !before.execution->provider_configuration_json.empty());
	const auto parent_before = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	const auto transcript_before = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, predecessor.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(parent_before && transcript_before);
	UAM_ASSERT_EQ(parent_before->managed_followup_incarnation, before.owner.root_birth_id);
	const auto* live_parent = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
	UAM_ASSERT(live_parent && live_parent->metadata_incarnation == fixture.root_incarnation);
	UAM_ASSERT_EQ(transcript_before->managed_followup_incarnation, before.owner.transcript_birth_id);
	UAM_ASSERT_EQ(transcript_before->native_session_id, native);
	UAM_ASSERT_EQ(transcript_before->agent_run_id, predecessor.id);
	UAM_ASSERT_EQ(std::ranges::count(transcript_before->messages, fixture.task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(transcript_before->messages, fixture.answer, &Message::content), 1);
	ManagedFollowupResultR98(*parent_before, predecessor.id, before.turns.front().delivery_id, fixture.answer);
	UAM_ASSERT_EQ(ManagedFollowupInputsR98(fixture).size(), std::size_t{1});
	UAM_ASSERT(fixture.app.queued_agent_run_ids.empty() && fixture.app.local_acp_launch_tasks.empty());
	UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.contains(predecessor.transcript_chat_id));
	UAM_ASSERT(!ChatDomainService().FindChatById(fixture.app, predecessor.transcript_chat_id));
	errno = 0;
	UAM_ASSERT(first.pid > 0 && kill(first.pid, 0) < 0 && errno == ESRCH);
	UAM_ASSERT(fixture.StopAndReap()); // Teardown check follows normal completed-owner retirement, not a substitute for it.
	UAM_ASSERT_EQ(fixture.ReadRun().managed_continuation, predecessor.managed_continuation);
	UAM_ASSERT_EQ(first.completion->count, std::size_t{1});

	uam::ManagedFollowupRequest request;
	request.after_turn_id = before.current_turn_id;
	request.client_request_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	request.text = next_task;
	UAM_ASSERT(uam::uuid::IsCanonicalUuid(request.client_request_id));
	struct Completion
	{
		std::size_t count = 0;
		std::optional<uam::ManagedFollowupSubmitResult> value;
		std::optional<AgentRun> at_ack;
	};
	const auto completion = std::make_shared<Completion>();
	const auto root_file = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id);
	const auto transcript_file = AppPaths::UamChatFilePath(fixture.app.data_root, predecessor.transcript_chat_id);
	const auto ledger_file = fixture.app.data_root / "agent-runs" / (predecessor.id + ".json");
	const std::string root_bytes = ReadFile(root_file), transcript_bytes = ReadFile(transcript_file), ledger_bytes = ReadFile(ledger_file);
	const std::string initial_inputs = ReadFile(fixture.temp.root / "task-inputs.jsonl");
	uam::ManagedFollowupCode rejection = uam::ManagedFollowupCode::None;
	const auto admission = uam::ManagedSubagentFollowupService::TrySubmit(
	    fixture.app, fixture.root_id, predecessor.id, request,
	    [&, completion](const uam::ManagedFollowupSubmitResult& result)
	    {
		    ++completion->count;
		    completion->value = result;
		    completion->at_ack = fixture.ReadRun();
		    return true; // Classified receipt delivery ACK only; no fabricated publication or input authority.
	    }, &rejection);
	// First intended feature RED: the inert source-current stub returns Rejected/NotImplemented.
	if (admission == uam::ManagedFollowupAdmission::Rejected)
		std::cerr << "Managed follow-up admission rejected after genuine binding; code=" << static_cast<int>(rejection) << '\n';
	UAM_ASSERT_EQ(admission, uam::ManagedFollowupAdmission::Pending);
	UAM_ASSERT_EQ(completion->count, std::size_t{0}); // Pending is not acceptance or an inline success.
	UAM_ASSERT_EQ(ReadFile(root_file), root_bytes);
	UAM_ASSERT_EQ(ReadFile(transcript_file), transcript_bytes);
	UAM_ASSERT_EQ(ReadFile(ledger_file), ledger_bytes);
	UAM_ASSERT_EQ(ReadFile(fixture.temp.root / "task-inputs.jsonl"), initial_inputs);
	UAM_ASSERT(!fs::exists(fixture.temp.root / "resume-launch-witness.json"));
	fixture.Await([&] { return completion->value.has_value(); });
	UAM_ASSERT_EQ(completion->count, std::size_t{1});
	const auto accepted = *completion->value;
	UAM_ASSERT(accepted.accepted && !accepted.replayed && accepted.code == uam::ManagedFollowupCode::None && accepted.acceptance);
	const auto& receipt = *accepted.acceptance;
	UAM_ASSERT_EQ(receipt.root_chat_id, fixture.root_id);
	UAM_ASSERT_EQ(receipt.run_id, predecessor.id);
	UAM_ASSERT_EQ(receipt.transcript_chat_id, predecessor.transcript_chat_id);
	UAM_ASSERT_EQ(receipt.client_request_id, request.client_request_id);
	UAM_ASSERT_EQ(receipt.turn_number, std::uint32_t{2});
	UAM_ASSERT_EQ(receipt.status_at_admission, uam::ManagedRunTurnStatus::Queued);
	UAM_ASSERT(uam::uuid::IsCanonicalUuid(receipt.turn_id) && receipt.turn_id != before.current_turn_id);
	UAM_ASSERT(completion->at_ack && completion->at_ack->managed_continuation);
	const auto& admitted = *completion->at_ack->managed_continuation;
	UAM_ASSERT_EQ(admitted.turns.size(), std::size_t{2});
	UAM_ASSERT_EQ(admitted.current_turn_id, receipt.turn_id);
	UAM_ASSERT_EQ(admitted.turns.front(), before.turns.front());
	UAM_ASSERT_EQ(admitted.turns.back().status, uam::ManagedRunTurnStatus::Queued);
	UAM_ASSERT(!admitted.turns.back().dispatch_attempted);

	fixture.Await([&] { return fs::exists(fixture.temp.root / "followup-input-witness.json"); });
	const auto input = nlohmann::json::parse(ReadFile(fixture.temp.root / "followup-input-witness.json"));
	const auto launch = nlohmann::json::parse(ReadFile(fixture.temp.root / "resume-launch-witness.json"));
	const auto running = fixture.ReadRun();
	UAM_ASSERT(running.managed_continuation && running.status == "running");
	UAM_ASSERT_EQ(running.id, predecessor.id);
	UAM_ASSERT_EQ(running.managed_continuation->execution, before.execution);
	UAM_ASSERT_EQ(running.managed_continuation->turns.front(), before.turns.front());
	const auto& held_turn = running.managed_continuation->turns.back();
	UAM_ASSERT_EQ(held_turn.turn_id, receipt.turn_id);
	UAM_ASSERT_EQ(held_turn.status, uam::ManagedRunTurnStatus::Running);
	UAM_ASSERT(held_turn.dispatch_attempted && !held_turn.result_delivered);
	UAM_ASSERT_EQ(held_turn.text, next_task);
	UAM_ASSERT_EQ(held_turn.client_request_id, request.client_request_id);
	UAM_ASSERT_EQ(held_turn.after_turn_id, before.current_turn_id);
	UAM_ASSERT_EQ(held_turn.delivery_id, receipt.turn_id);
	UAM_ASSERT_EQ(held_turn.principal.kind, uam::ManagedRunPrincipalKind::UserRoot);
	UAM_ASSERT_EQ(held_turn.principal.root_chat_id, fixture.root_id);
	UAM_ASSERT_EQ(held_turn.principal.root_birth_id, before.owner.root_birth_id);
	UAM_ASSERT_EQ(held_turn.continuity, uam::ManagedRunContinuity::Native);
	UAM_ASSERT_EQ(held_turn.recovery_decision, uam::ManagedRunRecoveryDecision::None);
	UAM_ASSERT(held_turn.retry_of_turn_id.empty() && held_turn.principal.parent_run_id.empty() && held_turn.principal.parent_transcript_birth_id.empty());
	UAM_ASSERT_EQ(held_turn.expected_turn_serial, input.at("turn").at("expectedTurnSerial").get<int>());
	UAM_ASSERT_EQ(held_turn.first_user_message_index, input.at("turn").at("firstUserMessageIndex").get<int>());
	UAM_ASSERT(held_turn.first_user_message_index >= static_cast<int>(transcript_before->messages.size()));
	UAM_ASSERT_EQ(held_turn.user_message_created_at, input.at("turn").at("userMessageCreatedAt").get<std::string>());
	const auto* session = uam::FindAcpSessionForChat(fixture.app, predecessor.transcript_chat_id);
	UAM_ASSERT(session && session->running && session->processing && session->child_pid == input.at("pid").get<pid_t>());
	UAM_ASSERT_EQ(session->turn_serial, held_turn.expected_turn_serial); // A new process may legitimately reuse serial1.
	UAM_ASSERT_EQ(session->provider_id, predecessor.provider_id);
	UAM_ASSERT_EQ(session->process_execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(launch.at("prior_history").at("native_id").get<std::string>(), native);
	const auto args = launch.at("args").get<std::vector<std::string>>();
	const auto resume = std::ranges::find(args, std::string("--resume"));
	UAM_ASSERT(resume != args.end() && std::next(resume) != args.end() && *std::next(resume) == native);
	UAM_ASSERT_EQ(std::ranges::count(args, std::string("--resume")), 1);
	UAM_ASSERT_EQ(ManagedFollowupInputsR98(fixture).size(), std::size_t{2});
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-followup-completion", "release only this owned resumed turn\n"));
	fixture.AssertPeerFiles();
	fixture.Await([&]
	{
		const auto run = fixture.ReadRun();
		return run.managed_continuation && run.managed_continuation->turns.size() == 2 && run.status == "completed" && run.managed_continuation->turns.back().result_delivered && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty();
	});
	const auto completed = fixture.ReadRun();
	const auto& after = *completed.managed_continuation;
	UAM_ASSERT_EQ(completed.id, predecessor.id);
	UAM_ASSERT_EQ(completed.root_chat_id, predecessor.root_chat_id);
	UAM_ASSERT_EQ(completed.transcript_chat_id, predecessor.transcript_chat_id);
	UAM_ASSERT_EQ(completed.parent_run_id, predecessor.parent_run_id);
	UAM_ASSERT_EQ(completed.depth, predecessor.depth);
	UAM_ASSERT_EQ(completed.task, predecessor.task);
	UAM_ASSERT_EQ(completed.created_at, predecessor.created_at);
	UAM_ASSERT_EQ(completed.provider_id, predecessor.provider_id);
	UAM_ASSERT_EQ(completed.model_id, predecessor.model_id);
	UAM_ASSERT_EQ(completed.definition_snapshot, predecessor.definition_snapshot);
	UAM_ASSERT_EQ(completed.definition_instructions, predecessor.definition_instructions);
	UAM_ASSERT_EQ(completed.definition_hash, predecessor.definition_hash);
	UAM_ASSERT_EQ(completed.skills_snapshot, predecessor.skills_snapshot);
	UAM_ASSERT_EQ(completed.delegates_snapshot, predecessor.delegates_snapshot);
	UAM_ASSERT_EQ(completed.effective_workspace_access, predecessor.effective_workspace_access);
	UAM_ASSERT_EQ(completed.result_delivery_id, predecessor.result_delivery_id);
	UAM_ASSERT_EQ(completed.root_result_delivered, predecessor.root_result_delivered);
	UAM_ASSERT_EQ(completed.root_result_delivery_attempts, predecessor.root_result_delivery_attempts);
	UAM_ASSERT_EQ(after.owner, before.owner);
	UAM_ASSERT_EQ(after.execution, before.execution);
	UAM_ASSERT_EQ(after.turns.size(), std::size_t{2});
	UAM_ASSERT_EQ(after.turns.front(), before.turns.front());
	UAM_ASSERT_EQ(after.current_turn_id, receipt.turn_id);
	UAM_ASSERT_EQ(after.current_turn_number, std::uint32_t{2});
	UAM_ASSERT(after.state_revision > running.managed_continuation->state_revision);
	const auto& turn = after.turns.back();
	UAM_ASSERT_EQ(turn.status, uam::ManagedRunTurnStatus::Completed);
	UAM_ASSERT_EQ(turn.turn_id, held_turn.turn_id);
	UAM_ASSERT_EQ(turn.turn_number, held_turn.turn_number);
	UAM_ASSERT_EQ(turn.client_request_id, held_turn.client_request_id);
	UAM_ASSERT_EQ(turn.after_turn_id, held_turn.after_turn_id);
	UAM_ASSERT_EQ(turn.text, held_turn.text);
	UAM_ASSERT_EQ(turn.principal, held_turn.principal);
	UAM_ASSERT_EQ(turn.delivery_id, held_turn.delivery_id);
	UAM_ASSERT_EQ(turn.continuity, held_turn.continuity);
	UAM_ASSERT_EQ(turn.recovery_decision, held_turn.recovery_decision);
	UAM_ASSERT_EQ(turn.retry_of_turn_id, held_turn.retry_of_turn_id);
	UAM_ASSERT(turn.dispatch_attempted && turn.result_required && turn.result_delivered && turn.result_delivery_attempts > 0);
	UAM_ASSERT_EQ(turn.expected_turn_serial, held_turn.expected_turn_serial);
	UAM_ASSERT_EQ(turn.first_user_message_index, held_turn.first_user_message_index);
	UAM_ASSERT_EQ(turn.user_message_created_at, held_turn.user_message_created_at);
	UAM_ASSERT_EQ(turn.started_at_epoch_ms, held_turn.started_at_epoch_ms);
	UAM_ASSERT(turn.started_at_epoch_ms > 0 && turn.finished_at_epoch_ms >= turn.started_at_epoch_ms);
	UAM_ASSERT_EQ(completed.expected_turn_serial, turn.expected_turn_serial);
	UAM_ASSERT_EQ(completed.started_at_epoch_ms, turn.started_at_epoch_ms);
	UAM_ASSERT_EQ(completed.finished_at_epoch_ms, turn.finished_at_epoch_ms);
	UAM_ASSERT(turn.diagnostic_code.empty() && completed.diagnostic_code.empty() && completed.diagnostic.empty());
	UAM_ASSERT_EQ(completed.result_excerpt, next_answer);
	const auto parent_after = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	const auto transcript_after = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, predecessor.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(parent_after && transcript_after);
	UAM_ASSERT_EQ(parent_after->managed_followup_incarnation, before.owner.root_birth_id);
	UAM_ASSERT_EQ(transcript_after->managed_followup_incarnation, before.owner.transcript_birth_id);
	UAM_ASSERT_EQ(parent_after->created_at, parent_before->created_at);
	UAM_ASSERT_EQ(transcript_after->created_at, transcript_before->created_at);
	UAM_ASSERT_EQ(transcript_after->native_session_id, native);
	UAM_ASSERT_EQ(transcript_after->agent_run_id, predecessor.id);
	UAM_ASSERT_EQ(transcript_after->model_id, transcript_before->model_id);
	UAM_ASSERT_EQ(transcript_after->approval_mode, transcript_before->approval_mode);
	UAM_ASSERT_EQ(transcript_after->workspace_directory, transcript_before->workspace_directory);
	UAM_ASSERT_EQ(transcript_after->execution_host_id, transcript_before->execution_host_id);
	UAM_ASSERT(parent_after->messages.size() >= parent_before->messages.size());
	UAM_ASSERT(std::equal(parent_before->messages.begin(), parent_before->messages.end(), parent_after->messages.begin()));
	UAM_ASSERT(transcript_after->messages.size() >= transcript_before->messages.size());
	UAM_ASSERT(std::equal(transcript_before->messages.begin(), transcript_before->messages.end(), transcript_after->messages.begin()));
	UAM_ASSERT_EQ(std::ranges::count(parent_after->messages, first.text, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(transcript_after->messages, fixture.task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(transcript_after->messages, fixture.answer, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(transcript_after->messages, next_task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(transcript_after->messages, next_answer, &Message::content), 1);
	ManagedFollowupResultR98(*parent_after, predecessor.id, before.turns.front().delivery_id, fixture.answer);
	ManagedFollowupResultR98(*parent_after, predecessor.id, turn.delivery_id, next_answer, turn.turn_id, turn.turn_number);
	const auto history_after = nlohmann::json::parse(ReadFile(fixture.temp.root / "private-native-history.json"));
	UAM_ASSERT_EQ(history_after.at("native_id").get<std::string>(), native);
	UAM_ASSERT_EQ(history_after.at("inputs"), nlohmann::json::array({fixture.task, next_task}));
	UAM_ASSERT_EQ(history_after.at("answers"), nlohmann::json::array({fixture.answer, next_answer}));
	const auto saved_inputs = ReadFile(fixture.temp.root / "task-inputs.jsonl");
	const auto saved_root = ReadFile(root_file);
	for (int poll = 0; poll < 100; ++poll)
	{
		fixture.Pulse();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT_EQ(ManagedFollowupInputsR98(fixture).size(), std::size_t{2});
	UAM_ASSERT_EQ(ReadFile(fixture.temp.root / "task-inputs.jsonl"), saved_inputs);
	UAM_ASSERT_EQ(ReadFile(root_file), saved_root);
	UAM_ASSERT_EQ(fixture.ReadRun().managed_continuation, completed.managed_continuation);
	UAM_ASSERT_EQ(completion->count, std::size_t{1});
	UAM_ASSERT(fixture.app.queued_agent_run_ids.empty());
	const auto* final_parent = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
	UAM_ASSERT(final_parent && final_parent->metadata_incarnation == fixture.root_incarnation);
	errno = 0;
	UAM_ASSERT(kill(input.at("pid").get<pid_t>(), 0) < 0 && errno == ESRCH);
	UAM_ASSERT(fixture.StopAndReap());
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
namespace
{
	struct FollowupRestartInspectionFixtureR98
	{
		ManagedClaudeLifecycleFixture original{true}; // Private PATH outlives both AppStates and handlers.
		struct RetireOriginal
		{
			uam::AppState& app;
			~RetireOriginal()
			{
				(void)uam::AgentRunScheduler::QuiesceInitialForShutdown(app);
			}
		} retirement{original.app}; // Also drains genuine pending work if a constructor prerequisite throws.
		TempDir crash{"uam-managed-followup-restart"};
		uam::AppState app;
		std::unique_ptr<UamQueryHandler> handler;
		std::vector<RestartInspectionFixtureR91::Query> pending;
		AgentRun expected;
		ChatSession transcript;
		std::map<fs::path, std::string> saved;
		std::string input_bytes;
		const std::string next_task = "PRIVATE-FOLLOWUP-HISTORY-PROMPT: continue this same native conversation once.";
		const std::string next_answer = "PRIVATE-FOLLOWUP-HISTORY-SECOND-ANSWER";

		FollowupRestartInspectionFixtureR98()
		{
			UAM_ASSERT(uam::io::WriteTextFile(original.temp.root / "peer.py", ManagedFollowupResumePeerR98()));
			fs::permissions(original.temp.root / "peer.py", fs::perms::owner_all, fs::perm_options::replace);
			original.AssertPeerFiles();
			auto config = nlohmann::json::parse(ReadFile(original.temp.root / "peer-config.json"));
			config["followup_task"] = next_task;
			config["followup_answer"] = next_answer;
			UAM_ASSERT(uam::io::WriteTextFile(original.temp.root / "peer-config.json", config.dump()));
			const auto first = ManagedBindingBeginR96(original);
			ManagedBindingReleaseAndSettleR96(original, first);
			original.Await([&]
			{
				const auto run = original.ReadRun();
				return run.status == "completed" && run.root_result_delivered && original.app.managed_initial_runtime_owners.empty() && original.app.acp_sessions.empty();
			});
			const auto predecessor = original.ReadRun();
			UAM_ASSERT(predecessor.managed_continuation && predecessor.managed_continuation->execution);
			const auto& before = *predecessor.managed_continuation;
			UAM_ASSERT_EQ(before.turns.size(), std::size_t{1});
			UAM_ASSERT_EQ(before.turns.front().status, uam::ManagedRunTurnStatus::Completed);
			UAM_ASSERT(before.turns.front().result_delivered);
			const auto native_history = nlohmann::json::parse(ReadFile(original.temp.root / "private-native-history.json"));
			const auto native = native_history.at("native_id").get<std::string>();
			UAM_ASSERT(uam::uuid::IsCanonicalUuid(native));
			UAM_ASSERT_EQ(before.execution->native_session_id, native);
			UAM_ASSERT_EQ(before.execution->effective_model_id, original.model);
			UAM_ASSERT_EQ(before.execution->model_identity_source, std::string("exact-requested"));
			UAM_ASSERT_EQ(ManagedFollowupInputsR98(original).size(), std::size_t{1});
			errno = 0;
			UAM_ASSERT(first.pid > 0 && kill(first.pid, 0) < 0 && errno == ESRCH);
			UAM_ASSERT(original.StopAndReap()); // Normal original owner retirement was already observed.
			UAM_ASSERT_EQ(original.ReadRun().managed_continuation, predecessor.managed_continuation);

			struct Completion
			{
				std::size_t count = 0;
				std::optional<uam::ManagedFollowupSubmitResult> result;
			};
			const auto completion = std::make_shared<Completion>();
			uam::ManagedFollowupRequest request;
			request.after_turn_id = before.current_turn_id;
			request.client_request_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
			request.text = next_task;
			UAM_ASSERT(uam::uuid::IsCanonicalUuid(request.client_request_id));
			uam::ManagedFollowupCode rejected = uam::ManagedFollowupCode::None;
			const auto admission = uam::ManagedSubagentFollowupService::TrySubmit(original.app, original.root_id, predecessor.id, request,
			    [completion](const uam::ManagedFollowupSubmitResult& result)
			    {
				    ++completion->count;
				    completion->result = result;
				    return true;
			    }, &rejected);
			if (admission != uam::ManagedFollowupAdmission::Pending)
				throw TestFailure("Two-turn history needs genuine connected follow-up first; admission code=" + std::to_string(static_cast<int>(rejected)));
			UAM_ASSERT_EQ(completion->count, std::size_t{0});
			original.Await([&] { return completion->result.has_value(); });
			UAM_ASSERT_EQ(completion->count, std::size_t{1});
			UAM_ASSERT(completion->result->accepted && !completion->result->replayed && completion->result->code == uam::ManagedFollowupCode::None && completion->result->acceptance);
			const auto receipt = *completion->result->acceptance;
			UAM_ASSERT_EQ(receipt.run_id, predecessor.id);
			UAM_ASSERT_EQ(receipt.transcript_chat_id, predecessor.transcript_chat_id);
			UAM_ASSERT_EQ(receipt.client_request_id, request.client_request_id);
			UAM_ASSERT_EQ(receipt.turn_number, std::uint32_t{2});
			original.Await([&] { return fs::exists(original.temp.root / "followup-input-witness.json"); });
			const auto input = nlohmann::json::parse(ReadFile(original.temp.root / "followup-input-witness.json"));
			const auto launch = nlohmann::json::parse(ReadFile(original.temp.root / "resume-launch-witness.json"));
			const auto args = launch.at("args").get<std::vector<std::string>>();
			const auto resume = std::ranges::find(args, std::string("--resume"));
			UAM_ASSERT(resume != args.end() && std::next(resume) != args.end() && *std::next(resume) == native);
			UAM_ASSERT_EQ(std::ranges::count(args, std::string("--resume")), 1);
			UAM_ASSERT_EQ(input.at("turn").at("turnId").get<std::string>(), receipt.turn_id);
			UAM_ASSERT(input.at("turn").at("expectedTurnSerial").get<int>() > 0);
			UAM_ASSERT(uam::io::WriteTextFile(original.temp.root / "allow-followup-completion", "Complete only this actual resumed input.\n"));
			original.Await([&]
			{
				const auto run = original.ReadRun();
				return run.managed_continuation && run.managed_continuation->turns.size() == 2 && run.status == "completed" && run.managed_continuation->turns.back().result_delivered && original.app.managed_initial_runtime_owners.empty() && original.app.acp_sessions.empty();
			});
			expected = original.ReadRun();
			UAM_ASSERT_EQ(expected.managed_continuation->execution, before.execution);
			UAM_ASSERT_EQ(expected.managed_continuation->owner, before.owner);
			UAM_ASSERT_EQ(expected.managed_continuation->turns.front(), before.turns.front());
			const auto& turn = expected.managed_continuation->turns.back();
			UAM_ASSERT_EQ(turn.turn_id, receipt.turn_id);
			UAM_ASSERT_EQ(turn.text, next_task);
			UAM_ASSERT_EQ(turn.status, uam::ManagedRunTurnStatus::Completed);
			UAM_ASSERT_EQ(turn.principal.kind, uam::ManagedRunPrincipalKind::UserRoot);
			UAM_ASSERT(turn.result_delivered && turn.dispatch_attempted);
			UAM_ASSERT_EQ(expected.result_excerpt, next_answer);
			UAM_ASSERT_EQ(ManagedFollowupInputsR98(original).size(), std::size_t{2});
			const auto history = nlohmann::json::parse(ReadFile(original.temp.root / "private-native-history.json"));
			UAM_ASSERT_EQ(history.at("native_id").get<std::string>(), native);
			UAM_ASSERT_EQ(history.at("inputs"), nlohmann::json::array({original.task, next_task}));
			UAM_ASSERT_EQ(history.at("answers"), nlohmann::json::array({original.answer, next_answer}));
			errno = 0;
			UAM_ASSERT(kill(input.at("pid").get<pid_t>(), 0) < 0 && errno == ESRCH);
			UAM_ASSERT(original.StopAndReap());
			UAM_ASSERT_EQ(original.ReadRun().managed_continuation, expected.managed_continuation);
			input_bytes = ReadFile(original.temp.root / "task-inputs.jsonl");
			const auto loaded = ChatRepository::LoadLocalChatBounded(original.app.data_root, expected.transcript_chat_id, 4 * 1024 * 1024);
			const auto parent = ChatRepository::LoadLocalChatBounded(original.app.data_root, original.root_id, 4 * 1024 * 1024);
			UAM_ASSERT(loaded && parent && loaded->native_session_id == native);
			transcript = *loaded;
			UAM_ASSERT_EQ(parent->managed_followup_incarnation, before.owner.root_birth_id);
			UAM_ASSERT_EQ(transcript.managed_followup_incarnation, before.owner.transcript_birth_id);
			UAM_ASSERT_EQ(std::ranges::count(transcript.messages, next_task, &Message::content), 1);
			UAM_ASSERT_EQ(std::ranges::count(transcript.messages, next_answer, &Message::content), 1);
			ManagedFollowupResultR98(*parent, expected.id, before.turns.front().delivery_id, original.answer);
			ManagedFollowupResultR98(*parent, expected.id, turn.delivery_id, next_answer, turn.turn_id, turn.turn_number);

			for (const auto& primary : {original.app.data_root / "agent-runs" / (expected.id + ".json"), AppPaths::UamChatFilePath(original.app.data_root, original.root_id), AppPaths::UamChatFilePath(original.app.data_root, expected.transcript_chat_id)})
				for (const auto& path : {primary, uam::io::MakeBackupPath(primary)})
					if (fs::exists(path))
					{
						const auto relative = path.lexically_relative(original.app.data_root);
						saved.emplace(relative, ReadFile(path));
						fs::create_directories((crash.root / relative).parent_path());
						UAM_ASSERT(uam::io::WriteTextFile(crash.root / relative, saved.at(relative)));
					}
			app.data_root = crash.root;
			app.settings = original.app.settings;
			app.provider_profiles = original.app.provider_profiles;
			auto startup = uam::AgentRunLedger::LoadAll(app.data_root);
			UAM_ASSERT(uam::AgentRunLedger::MarkNonterminalInterrupted(app.data_root, &startup.runs, "manager_restarted", &startup.errors));
			app.agent_runs = std::move(startup.runs); // Actual startup runtime projection, never ReadOnly authority.
			UAM_ASSERT(app.agent_runs.empty());
			UAM_ASSERT(std::ranges::any_of(startup.errors, [](const auto& value) { return value.find("inert") != std::string::npos; }));
			ChatHistorySyncService().LoadSidebarChats(app);
			const auto* visible = ChatDomainService().FindChatById(app, original.root_id);
			UAM_ASSERT(visible && visible->managed_followup_incarnation == before.owner.root_birth_id);
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
			NativeMediaRefreshHandlerTestAccess::RestartInspectionDispatch(*handler,
			    [this](auto callback, auto worker, auto completion)
			    {
				    pending.push_back({callback, std::move(worker), std::move(completion)});
				    return true;
			    });
			Unchanged();
		}
		CefRefPtr<RestartInspectionCallbackR91> Begin(std::string_view action, const nlohmann::json& payload)
		{
			CefRefPtr<RestartInspectionCallbackR91> callback = new RestartInspectionCallbackR91;
			NativeMediaRefreshHandlerTestAccess::RestartInspectionAction(*handler, action, payload, callback);
			return callback;
		}
		void Drain()
		{
			while (!pending.empty())
			{
				auto query = std::move(pending.front());
				pending.erase(pending.begin());
				auto result = std::async(std::launch::async, query.worker).get();
				CompleteAsyncCefQuery(query.callback, result, query.completion);
			}
		}
		CefRefPtr<RestartInspectionCallbackR91> Request(std::string_view action, const nlohmann::json& payload)
		{
			auto callback = Begin(action, payload);
			Drain();
			UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
			return callback;
		}
		nlohmann::json OpenPayload() const
		{
			return {{"chatId", original.root_id}, {"transcriptChatId", expected.transcript_chat_id}};
		}
		void NoAuthority() const
		{
			UAM_ASSERT(app.agent_runs.empty() && app.queued_agent_run_ids.empty() && app.managed_initial_runtime_owners.empty() && app.managed_initial_mentions.empty());
			UAM_ASSERT(app.acp_sessions.empty() && app.cli_terminals.empty() && app.local_acp_launch_tasks.empty());
			UAM_ASSERT(app.pending_agent_run_cancellation_ids.empty() && app.managed_subagent_cancellation_receipts.empty());
			UAM_ASSERT(!ChatDomainService().FindChatById(app, expected.transcript_chat_id));
		}
		void Unchanged() const
		{
			for (const auto& [relative, bytes] : saved)
			{
				UAM_ASSERT_EQ(ReadFile(crash.root / relative), bytes);
				UAM_ASSERT_EQ(ReadFile(original.app.data_root / relative), bytes);
			}
			const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(crash.root);
			UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1);
			UAM_ASSERT_EQ(ledger.runs.front().managed_continuation, expected.managed_continuation);
			UAM_ASSERT_EQ(ReadFile(original.temp.root / "task-inputs.jsonl"), input_bytes);
			NoAuthority();
		}
	};
} // namespace

UAM_TEST(ManagedFollowupRestartListsAndOpensGenuineTwoTurnHistoryWithoutAuthority)
{
	FollowupRestartInspectionFixtureR98 fixture;
	const auto listed = fixture.Request("listManagedSubagents", {{"chatId", fixture.original.root_id}});
	UAM_ASSERT_EQ(listed->successes, 1);
	UAM_ASSERT_EQ(listed->body.at("totalCount").get<std::size_t>(), std::size_t{1}); // Intended missing multi-turn history RED after genuine consumer completion.
	UAM_ASSERT_EQ(listed->body.at("runs").size(), std::size_t{1});
	const auto& row = listed->body.at("runs").front();
	UAM_ASSERT_EQ(row.at("runId").get<std::string>(), fixture.expected.id);
	UAM_ASSERT_EQ(row.at("status").get<std::string>(), fixture.expected.status);
	UAM_ASSERT(row.at("lastReported").get<bool>() && !row.at("canCancel").get<bool>());
	const auto visible = listed->body.dump();
	UAM_ASSERT(visible.find(fixture.next_task) == std::string::npos);
	UAM_ASSERT(visible.find(fixture.original.definition.instructions) == std::string::npos);
	UAM_ASSERT(!row.contains("managedContinuation") && !row.contains("execution") && !row.contains("connectionConfigurationJson") && !row.contains("providerConfigurationJson"));
	uam::ManagedSubagentSnapshot snapshot;
	std::string error;
	UAM_ASSERT(uam::ManagedSubagentControlService::Capture(fixture.app, fixture.original.root_id, &snapshot, &error, fixture.expected.id, 0, 1));
	const auto read = std::async(std::launch::async, [&] { return uam::ManagedSubagentControlService::ReadDetached(snapshot); }).get();
	UAM_ASSERT(read.ready && !read.history_transcript); // A selected list/read token still carries no conversation body.
	UAM_ASSERT(uam::ManagedSubagentControlService::MatchesSnapshot(fixture.app, snapshot));
	const auto opened = fixture.Request("getManagedAgentTranscript", fixture.OpenPayload());
	UAM_ASSERT_EQ(opened->successes, 1);
	UAM_ASSERT(opened->body.at("lastReported").get<bool>());
	UAM_ASSERT_EQ(opened->body.at("runId").get<std::string>(), fixture.expected.id);
	UAM_ASSERT_EQ(opened->body.at("status").get<std::string>(), fixture.expected.status);
	UAM_ASSERT_EQ(opened->body.at("messages"), uam::StateSerializer::SerializeSession(fixture.transcript).value("messages", nlohmann::json::array()));
	UAM_ASSERT(!opened->body.contains("managedContinuation") && !opened->body.contains("execution") && !opened->body.contains("connectionConfigurationJson") && !opened->body.contains("providerConfigurationJson"));
	const auto refused = fixture.Request("cancelManagedSubagent", {{"chatId", fixture.original.root_id}, {"runId", fixture.expected.id}, {"controlScopeId", listed->body.at("controlScopeId")}});
	UAM_ASSERT_EQ(refused->successes, 0);
	UAM_ASSERT_EQ(refused->failures, 1);
	UAM_ASSERT_EQ(refused->status, 409);
	fixture.NoAuthority(); // Includes absence of cancellation receipts, even for this terminal child.
	(void)uam::AgentRunScheduler::Poll(fixture.app);
	fixture.Unchanged();
}

UAM_TEST(ManagedFollowupRestartRejectsPrivateReceiptTextDriftAgainstCapturedRead)
{
	FollowupRestartInspectionFixtureR98 fixture;
	const auto opened = fixture.Request("getManagedAgentTranscript", fixture.OpenPayload());
	UAM_ASSERT_EQ(opened->successes, 1); // Independent old multi-turn history 404, after genuine consumer prerequisites.
	UAM_ASSERT(opened->body.at("lastReported").get<bool>());
	UAM_ASSERT_EQ(opened->body.at("messages"), uam::StateSerializer::SerializeSession(fixture.transcript).value("messages", nlohmann::json::array()));
	fixture.Unchanged();
	const auto callback = fixture.Begin("getManagedAgentTranscript", fixture.OpenPayload());
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	const auto primary = fixture.crash.root / "agent-runs" / (fixture.expected.id + ".json");
	const auto bytes = ReadFile(primary);
	auto changed = nlohmann::json::parse(bytes);
	auto& text = changed.at("managedContinuation").at("turns").at(1).at("text");
	UAM_ASSERT_EQ(text.get<std::string>(), fixture.next_task);
	std::string altered_text = fixture.next_task;
	altered_text.front() = 'X'; // Equal length, valid nonempty text; this is only negative canonical tampering.
	text = altered_text;
	const auto changed_bytes = changed.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(primary, changed_bytes));
	const auto altered = uam::AgentRunLedger::LoadAllReadOnly(fixture.crash.root);
	UAM_ASSERT(altered.errors.empty() && altered.runs.size() == 1);
	UAM_ASSERT_EQ(altered.runs.front().managed_continuation->turns.back().text, altered_text);
	auto original_except_text = altered.runs.front().managed_continuation;
	original_except_text->turns.back().text = fixture.next_task;
	UAM_ASSERT_EQ(original_except_text, fixture.expected.managed_continuation);
	auto original_json = nlohmann::json::parse(bytes);
	for (auto* document : {&original_json, &changed})
	{
		document->at("managedContinuation").at("execution") = nullptr;
		for (auto& turn : document->at("managedContinuation").at("turns"))
			turn.at("text") = "";
	}
	UAM_ASSERT_EQ(changed, original_json); // Projected metadata is identical: the original private text digest must reject.
	fixture.Drain();
	UAM_ASSERT_EQ(callback->successes, 0);
	UAM_ASSERT_EQ(callback->failures, 1);
	UAM_ASSERT(!callback->error.empty() && callback->body.is_null());
	fixture.NoAuthority();
	UAM_ASSERT_EQ(ReadFile(primary), changed_bytes); // No repair, replay, fallback or adoption.
	UAM_ASSERT(uam::io::WriteTextFile(primary, bytes));
	fixture.Unchanged();
	const auto restored = fixture.Request("getManagedAgentTranscript", fixture.OpenPayload());
	UAM_ASSERT_EQ(restored->successes, 1); // Same catalogue/token authority and original bytes defeat blanket refusal.
	UAM_ASSERT_EQ(restored->body.at("messages"), opened->body.at("messages"));
	fixture.Unchanged();
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
namespace
{
	std::string ManagedFollowupNegativePeerR98()
	{
		std::string peer = ManagedFollowupResumePeerR98();
		const auto replace = [&](const std::string& before, const std::string& after)
		{
			const auto at = peer.find(before);
			UAM_ASSERT(at != std::string::npos && peer.find(before, at + before.size()) == std::string::npos);
			peer.replace(at, before.size(), after);
		};
		// Modify only this private copy. Every original R98 launch/input/ledger assertion remains.
		replace("            event('initialize', request_id=message['request_id'], turn_number=number)\n",
		    "            event('initialize', request_id=message['request_id'], turn_number=number)\n"
		    "            if number == 2 and config.get('negative_mode') == 'refuse-resume':\n"
		    "                event('resume-refused', pid=os.getpid(), turn_number=number)\n"
		    "                sys.exit(71) # Actual peer refusal before its initialize response, not a fake readiness flag.\n");
		replace("        # Raw correlated owned observations, not initialize catalogues or optimistic session fields.\n",
		    "        # Raw correlated owned observations, not initialize catalogues or optimistic session fields.\n"
		    "        if number == 2 and config.get('negative_mode') in ('foreign-init', 'foreign-result'):\n"
		    "            foreign = str(uuid.uuid4())\n"
		    "            assert foreign != native\n"
		    "            event('foreign-native', native_id=foreign, original_native=native, kind_of_event=config['negative_mode'])\n"
		    "            if config['negative_mode'] == 'foreign-init':\n"
		    "                emit(dict(type='system', subtype='init', session_id=foreign, model=config['model'], permissionMode='plan'))\n"
		    "            else:\n"
		    "                emit(dict(type='system', subtype='init', session_id=native, model=config['model'], permissionMode='plan'))\n"
		    "                emit(dict(type='result', subtype='success', is_error=False, session_id=foreign, result=answer, duration_ms=1, num_turns=1))\n"
		    "            continue # Let the real runtime close/reap this same process; do not fake a completion.\n");
		return peer;
	}

	struct ManagedFollowupNegativeFixtureR98
	{
		ManagedClaudeLifecycleFixture fixture{true};
		const std::string text = "Continue the same private native child for the bounded negative control.";
		const std::string answer = "PRIVATE-MANAGED-FOLLOWUP-NEGATIVE-SECOND-ANSWER";
		AgentRun predecessor;
		ChatSession parent, transcript;
		std::string native;
		struct Completion
		{
			std::size_t count = 0;
			std::optional<uam::ManagedFollowupSubmitResult> value;
			std::optional<AgentRun> at_ack;
		};

		explicit ManagedFollowupNegativeFixtureR98(const std::string& mode = {})
		{
			UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "peer.py", ManagedFollowupNegativePeerR98()));
			fs::permissions(fixture.temp.root / "peer.py", fs::perms::owner_all, fs::perm_options::replace);
			fixture.AssertPeerFiles();
			auto config = nlohmann::json::parse(ReadFile(fixture.temp.root / "peer-config.json"));
			config["followup_task"] = text;
			config["followup_answer"] = answer;
			config["negative_mode"] = mode;
			UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "peer-config.json", config.dump()));
			const auto first = ManagedBindingBeginR96(fixture);
			ManagedBindingReleaseAndSettleR96(fixture, first);
			fixture.Await([&]
			{
				const auto run = fixture.ReadRun();
				return run.status == "completed" && run.root_result_delivered && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty();
			});
			predecessor = fixture.ReadRun();
			UAM_ASSERT(predecessor.managed_continuation && predecessor.managed_continuation->execution && predecessor.managed_continuation->turns.size() == 1);
			const auto& before = *predecessor.managed_continuation;
			UAM_ASSERT_EQ(before.turns.front().status, uam::ManagedRunTurnStatus::Completed);
			UAM_ASSERT(before.turns.front().dispatch_attempted && before.turns.front().result_delivered);
			UAM_ASSERT_EQ(before.turns.front().expected_turn_serial, first.held.expected_turn_serial);
			UAM_ASSERT_EQ(before.owner, first.initial.managed_continuation->owner);
			UAM_ASSERT(!before.owner.delegation_grant && predecessor.parent_run_id.empty() && predecessor.depth == 1);
			UAM_ASSERT_EQ(predecessor.definition_snapshot, fixture.definition.markdown_snapshot);
			UAM_ASSERT_EQ(predecessor.definition_hash, fixture.definition.definition_hash);
			UAM_ASSERT_EQ(predecessor.effective_workspace_access, std::string("read"));
			native = nlohmann::json::parse(ReadFile(fixture.temp.root / "private-native-history.json")).at("native_id").get<std::string>();
			UAM_ASSERT(uam::uuid::IsCanonicalUuid(native));
			UAM_ASSERT_EQ(before.execution->native_session_id, native);
			UAM_ASSERT_EQ(before.execution->effective_model_id, fixture.model);
			UAM_ASSERT_EQ(before.execution->model_identity_source, std::string("exact-requested"));
			UAM_ASSERT_EQ(before.execution->approval_mode, std::string("plan"));
			UAM_ASSERT_EQ(before.execution->execution_host_id, std::string("local"));
			UAM_ASSERT_EQ(before.execution->workspace_directory, (fixture.temp.root / "workspace").string());
			UAM_ASSERT(!before.execution->connection_configuration_json.empty() && !before.execution->provider_configuration_json.empty());
			const auto saved_parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
			const auto saved_child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, predecessor.transcript_chat_id, 4 * 1024 * 1024);
			UAM_ASSERT(saved_parent && saved_child);
			parent = *saved_parent;
			transcript = *saved_child;
			UAM_ASSERT_EQ(parent.managed_followup_incarnation, before.owner.root_birth_id);
			UAM_ASSERT_EQ(transcript.managed_followup_incarnation, before.owner.transcript_birth_id);
			UAM_ASSERT_EQ(transcript.native_session_id, native);
			UAM_ASSERT_EQ(transcript.agent_run_id, predecessor.id);
			UAM_ASSERT_EQ(std::ranges::count(transcript.messages, fixture.task, &Message::content), 1);
			UAM_ASSERT_EQ(std::ranges::count(transcript.messages, fixture.answer, &Message::content), 1);
			ManagedFollowupResultR98(parent, predecessor.id, before.turns.front().delivery_id, fixture.answer);
			UAM_ASSERT_EQ(ManagedFollowupInputsR98(fixture).size(), std::size_t{1});
			UAM_ASSERT(fixture.app.queued_agent_run_ids.empty() && fixture.app.local_acp_launch_tasks.empty());
			UAM_ASSERT(!ChatDomainService().FindChatById(fixture.app, predecessor.transcript_chat_id));
			const auto* live_parent = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
			UAM_ASSERT(live_parent && live_parent->metadata_incarnation == fixture.root_incarnation);
			errno = 0;
			UAM_ASSERT(first.pid > 0 && kill(first.pid, 0) < 0 && errno == ESRCH); // Real initial runtime reaped before follow-up.
		}

		uam::ManagedFollowupRequest Request() const
		{
			uam::ManagedFollowupRequest request;
			request.after_turn_id = predecessor.managed_continuation->current_turn_id;
			request.client_request_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
			request.text = text;
			UAM_ASSERT(uam::uuid::IsCanonicalUuid(request.client_request_id));
			return request;
		}

		std::shared_ptr<Completion> Begin(const uam::ManagedFollowupRequest& request, int ack = 1)
		{
			const auto completion = std::make_shared<Completion>();
			const auto before = fixture.ReadRun();
			const auto root_file = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id);
			const auto child_file = AppPaths::UamChatFilePath(fixture.app.data_root, predecessor.transcript_chat_id);
			const std::string root_bytes = ReadFile(root_file), child_bytes = ReadFile(child_file);
			UAM_ASSERT_EQ(uam::ManagedSubagentFollowupService::TrySubmit(fixture.app, fixture.root_id, predecessor.id, request,
			                  [this, completion, ack](const uam::ManagedFollowupSubmitResult& result)
			                  {
				                  ++completion->count;
				                  completion->value = result;
				                  completion->at_ack = fixture.ReadRun();
				                  if (ack < 0) throw std::runtime_error("private classified receipt callback refused");
				                  return ack > 0;
			                  }), uam::ManagedFollowupAdmission::Pending);
			UAM_ASSERT_EQ(completion->count, std::size_t{0});
			UAM_ASSERT_EQ(fixture.ReadRun().managed_continuation, before.managed_continuation);
			UAM_ASSERT_EQ(ReadFile(root_file), root_bytes);
			UAM_ASSERT_EQ(ReadFile(child_file), child_bytes);
			UAM_ASSERT_EQ(ManagedFollowupInputsR98(fixture).size(), std::size_t{1});
			UAM_ASSERT(!fs::exists(fixture.temp.root / "resume-launch-witness.json"));
			return completion;
		}

		uam::ManagedFollowupAcceptance Accepted(const std::shared_ptr<Completion>& completion)
		{
			fixture.Await([&] { return completion->value.has_value(); });
			UAM_ASSERT_EQ(completion->count, std::size_t{1});
			const auto& result = *completion->value;
			UAM_ASSERT(result.accepted && !result.replayed && result.code == uam::ManagedFollowupCode::None && result.acceptance);
			UAM_ASSERT(completion->at_ack && completion->at_ack->managed_continuation);
			const auto& at_ack = *completion->at_ack->managed_continuation;
			UAM_ASSERT_EQ(at_ack.turns.size(), std::size_t{2});
			UAM_ASSERT_EQ(at_ack.turns.front(), predecessor.managed_continuation->turns.front());
			UAM_ASSERT_EQ(at_ack.turns.back().status, uam::ManagedRunTurnStatus::Queued);
			UAM_ASSERT(!at_ack.turns.back().dispatch_attempted);
			UAM_ASSERT_EQ(result.acceptance->status_at_admission, uam::ManagedRunTurnStatus::Queued);
			UAM_ASSERT_EQ(result.acceptance->turn_id, at_ack.turns.back().turn_id);
			UAM_ASSERT_EQ(result.acceptance->turn_number, std::uint32_t{2});
			return *result.acceptance;
		}

		pid_t HoldInput()
		{
			fixture.Await([&] { return fs::exists(fixture.temp.root / "followup-input-witness.json"); });
			const auto witness = nlohmann::json::parse(ReadFile(fixture.temp.root / "followup-input-witness.json"));
			const auto run = fixture.ReadRun();
			UAM_ASSERT(run.managed_continuation && run.status == "running");
			const auto& turn = run.managed_continuation->turns.back();
			UAM_ASSERT(turn.dispatch_attempted && turn.expected_turn_serial > 0 && turn.first_user_message_index >= static_cast<int>(transcript.messages.size()));
			UAM_ASSERT_EQ(turn.text, text);
			UAM_ASSERT_EQ(turn.user_message_created_at, witness.at("turn").at("userMessageCreatedAt").get<std::string>());
			UAM_ASSERT_EQ(turn.expected_turn_serial, witness.at("turn").at("expectedTurnSerial").get<int>());
			const auto* session = uam::FindAcpSessionForChat(fixture.app, predecessor.transcript_chat_id);
			const pid_t pid = witness.at("pid").get<pid_t>();
			UAM_ASSERT(session && session->running && session->processing && session->child_pid == pid && pid > 0);
			UAM_ASSERT_EQ(session->turn_serial, turn.expected_turn_serial);
			UAM_ASSERT_EQ(session->session_id, native);
			UAM_ASSERT_EQ(session->process_execution_host_id, std::string("local"));
			UAM_ASSERT_EQ(ManagedFollowupInputsR98(fixture).size(), std::size_t{2});
			const auto launch = nlohmann::json::parse(ReadFile(fixture.temp.root / "resume-launch-witness.json"));
			UAM_ASSERT_EQ(launch.at("pid").get<pid_t>(), pid);
			const auto args = launch.at("args").get<std::vector<std::string>>();
			const auto resume = std::ranges::find(args, std::string("--resume"));
			UAM_ASSERT(resume != args.end() && std::next(resume) != args.end() && *std::next(resume) == native);
			UAM_ASSERT_EQ(std::ranges::count(args, std::string("--resume")), 1);
			UAM_ASSERT_EQ(run.managed_continuation->execution, predecessor.managed_continuation->execution);
			UAM_ASSERT_EQ(run.managed_continuation->turns.front(), predecessor.managed_continuation->turns.front());
			return pid;
		}

		void Preserved(const AgentRun& run, std::size_t input_count) const
		{
			UAM_ASSERT(run.managed_continuation && run.managed_continuation->execution);
			UAM_ASSERT_EQ(run.id, predecessor.id);
			UAM_ASSERT_EQ(run.transcript_chat_id, predecessor.transcript_chat_id);
			UAM_ASSERT_EQ(run.task, predecessor.task);
			UAM_ASSERT_EQ(run.definition_snapshot, predecessor.definition_snapshot);
			UAM_ASSERT_EQ(run.definition_hash, predecessor.definition_hash);
			UAM_ASSERT_EQ(run.definition_instructions, predecessor.definition_instructions);
			UAM_ASSERT_EQ(run.skills_snapshot, predecessor.skills_snapshot);
			UAM_ASSERT_EQ(run.delegates_snapshot, predecessor.delegates_snapshot);
			UAM_ASSERT_EQ(run.effective_workspace_access, predecessor.effective_workspace_access);
			UAM_ASSERT_EQ(run.managed_continuation->owner, predecessor.managed_continuation->owner);
			UAM_ASSERT_EQ(run.managed_continuation->execution, predecessor.managed_continuation->execution);
			UAM_ASSERT_EQ(run.managed_continuation->turns.front(), predecessor.managed_continuation->turns.front());
			UAM_ASSERT_EQ(run.root_result_delivered, predecessor.root_result_delivered);
			UAM_ASSERT_EQ(run.root_result_delivery_attempts, predecessor.root_result_delivery_attempts);
			const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, predecessor.transcript_chat_id, 4 * 1024 * 1024);
			UAM_ASSERT(child && child->native_session_id == native && child->managed_followup_incarnation == transcript.managed_followup_incarnation);
			UAM_ASSERT(child->messages.size() >= transcript.messages.size() && std::equal(transcript.messages.begin(), transcript.messages.end(), child->messages.begin()));
			UAM_ASSERT_EQ(child->model_id, transcript.model_id);
			UAM_ASSERT_EQ(child->approval_mode, transcript.approval_mode);
			UAM_ASSERT_EQ(child->workspace_directory, transcript.workspace_directory);
			UAM_ASSERT_EQ(child->execution_host_id, transcript.execution_host_id);
			const auto saved_parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
			UAM_ASSERT(saved_parent && saved_parent->managed_followup_incarnation == parent.managed_followup_incarnation);
			UAM_ASSERT(saved_parent->messages.size() >= parent.messages.size() && std::equal(parent.messages.begin(), parent.messages.end(), saved_parent->messages.begin()));
			ManagedFollowupResultR98(*saved_parent, predecessor.id, predecessor.managed_continuation->turns.front().delivery_id, fixture.answer);
			UAM_ASSERT_EQ(ManagedFollowupInputsR98(fixture).size(), input_count);
			const auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
			UAM_ASSERT(root && root->metadata_incarnation == fixture.root_incarnation);
		}

		void Retired()
		{
			fixture.Await([&]
			{
				const auto run = fixture.ReadRun();
				return run.status != "running" && run.status != "queued" && !uam::AgentRunScheduler::HasPendingInitialMention(fixture.app, fixture.root_id) && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty();
			});
		}

		uam::ManagedFollowupSubmitResult SubmitClassified(const uam::ManagedFollowupRequest& request)
		{
			const auto completion = std::make_shared<Completion>();
			uam::ManagedFollowupCode rejection = uam::ManagedFollowupCode::None;
			const auto admission = uam::ManagedSubagentFollowupService::TrySubmit(fixture.app, fixture.root_id, predecessor.id, request,
			    [completion](const uam::ManagedFollowupSubmitResult& result) { ++completion->count; completion->value = result; return true; }, &rejection);
			if (admission == uam::ManagedFollowupAdmission::Rejected)
			{
				UAM_ASSERT_EQ(completion->count, std::size_t{0});
				UAM_ASSERT(rejection != uam::ManagedFollowupCode::None);
				uam::ManagedFollowupSubmitResult result;
				result.code = rejection;
				return result;
			}
			fixture.Await([&] { return completion->value.has_value(); });
			UAM_ASSERT_EQ(completion->count, std::size_t{1});
			return *completion->value;
		}

		void PumpNoReplay(std::size_t inputs)
		{
			const auto stable = fixture.ReadRun();
			const auto root_bytes = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id));
			for (int poll = 0; poll < 100; ++poll)
			{
				fixture.Pulse();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT_EQ(fixture.ReadRun().managed_continuation, stable.managed_continuation);
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id)), root_bytes);
			Preserved(fixture.ReadRun(), inputs);
		}

		void LaunchCount(std::size_t expected) const
		{
			std::istringstream stream(ReadFile(fixture.temp.root / "peer-events.jsonl"));
			std::string line;
			std::size_t actual = 0;
			while (std::getline(stream, line))
				if (!line.empty() && nlohmann::json::parse(line).value("kind", "") == "launch") ++actual;
			UAM_ASSERT_EQ(actual, expected);
		}
	};
} // namespace

UAM_TEST(ManagedFollowupReplayConflictAndStalePredecessorNeverDispatchAgain)
{
	ManagedFollowupNegativeFixtureR98 owner;
	auto request = owner.Request();
	const auto receipt = owner.Accepted(owner.Begin(request));
	const pid_t pid = owner.HoldInput();
	UAM_ASSERT(uam::io::WriteTextFile(owner.fixture.temp.root / "allow-followup-completion", "release the actual second turn\n"));
	owner.Retired();
	const auto completed = owner.fixture.ReadRun();
	UAM_ASSERT_EQ(completed.status, std::string("completed"));
	UAM_ASSERT(completed.managed_continuation->turns.back().result_delivered);
	UAM_ASSERT_EQ(completed.result_excerpt, owner.answer);
	const auto parent = ChatRepository::LoadLocalChatBounded(owner.fixture.app.data_root, owner.fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(parent);
	ManagedFollowupResultR98(*parent, completed.id, receipt.turn_id, owner.answer, receipt.turn_id, 2);
	errno = 0;
	UAM_ASSERT(kill(pid, 0) < 0 && errno == ESRCH);
	const auto ledger_file = owner.fixture.app.data_root / "agent-runs" / (completed.id + ".json");
	const auto parent_file = AppPaths::UamChatFilePath(owner.fixture.app.data_root, owner.fixture.root_id);
	const auto child_file = AppPaths::UamChatFilePath(owner.fixture.app.data_root, completed.transcript_chat_id);
	const std::string ledger_bytes = ReadFile(ledger_file), parent_bytes = ReadFile(parent_file), child_bytes = ReadFile(child_file);
	const auto replay = owner.SubmitClassified(request);
	UAM_ASSERT(replay.accepted && replay.replayed && replay.acceptance && *replay.acceptance == receipt);
	UAM_ASSERT_EQ(replay.acceptance->status_at_admission, uam::ManagedRunTurnStatus::Queued);
	auto conflict = request;
	conflict.text += " Different authored request.";
	const auto refused = owner.SubmitClassified(conflict);
	UAM_ASSERT(!refused.accepted && !refused.acceptance && refused.code == uam::ManagedFollowupCode::IdempotencyConflict);
	auto stale = owner.Request(); // New client UUID, genuinely old turn1 predecessor.
	const auto stale_result = owner.SubmitClassified(stale);
	UAM_ASSERT(!stale_result.accepted && !stale_result.acceptance && stale_result.code != uam::ManagedFollowupCode::None);
	UAM_ASSERT_EQ(owner.fixture.ReadRun().managed_continuation, completed.managed_continuation);
	UAM_ASSERT_EQ(ReadFile(ledger_file), ledger_bytes);
	UAM_ASSERT_EQ(ReadFile(parent_file), parent_bytes);
	UAM_ASSERT_EQ(ReadFile(child_file), child_bytes);
	owner.PumpNoReplay(2);
	owner.LaunchCount(2);
	UAM_ASSERT(owner.fixture.StopAndReap());
}

UAM_TEST(ManagedFollowupUnacknowledgedAppendCannotStartOrReplayNativeInput)
{
	for (const int ack : {0, -1})
	{
		ManagedFollowupNegativeFixtureR98 owner;
		const auto request = owner.Request();
		const auto completion = owner.Begin(request, ack);
		const auto receipt = owner.Accepted(completion); // Real committed append is inspected inside the rejecting/throwing callback.
		owner.Retired();
		const auto refused = owner.fixture.ReadRun();
		UAM_ASSERT_EQ(refused.managed_continuation->turns.size(), std::size_t{2});
		UAM_ASSERT(refused.status != "completed");
		UAM_ASSERT(refused.result_excerpt != owner.fixture.answer);
		UAM_ASSERT_EQ(refused.managed_continuation->turns.back().turn_id, receipt.turn_id);
		UAM_ASSERT(!refused.managed_continuation->turns.back().dispatch_attempted);
		UAM_ASSERT(!fs::exists(owner.fixture.temp.root / "resume-launch-witness.json"));
		owner.Preserved(refused, 1);
		const auto replay = owner.SubmitClassified(request);
		UAM_ASSERT(replay.accepted && replay.replayed && replay.acceptance && *replay.acceptance == receipt);
		UAM_ASSERT_EQ(completion->count, std::size_t{1});
		owner.PumpNoReplay(1);
		owner.LaunchCount(1);
		UAM_ASSERT(!fs::exists(owner.fixture.temp.root / "resume-launch-witness.json"));
		UAM_ASSERT(owner.fixture.StopAndReap());
	}
}

UAM_TEST(ManagedFollowupResumeRefusalAndForeignNativeEventsPreserveSavedIdentity)
{
	for (const std::string mode : {"refuse-resume", "foreign-init", "foreign-result"})
	{
		ManagedFollowupNegativeFixtureR98 owner(mode);
		const auto receipt = owner.Accepted(owner.Begin(owner.Request()));
		pid_t pid = 0;
		if (mode == "refuse-resume")
		{
			owner.fixture.Await([&] { return ReadFile(owner.fixture.temp.root / "peer-events.jsonl").find("resume-refused") != std::string::npos; });
			const auto launch = nlohmann::json::parse(ReadFile(owner.fixture.temp.root / "resume-launch-witness.json"));
			pid = launch.at("pid").get<pid_t>();
			UAM_ASSERT(!fs::exists(owner.fixture.temp.root / "followup-input-witness.json"));
		}
		else
		{
			pid = owner.HoldInput(); // These contradictions occur AFTER two actual inputs; no prevented-delivery claim.
			UAM_ASSERT(uam::io::WriteTextFile(owner.fixture.temp.root / "allow-followup-completion", "release the selected foreign raw observation\n"));
			owner.fixture.Await([&] { return ReadFile(owner.fixture.temp.root / "peer-events.jsonl").find("foreign-native") != std::string::npos; });
		}
		owner.Retired();
		const auto failed = owner.fixture.ReadRun();
		UAM_ASSERT_EQ(failed.managed_continuation->turns.size(), std::size_t{2});
		UAM_ASSERT(failed.status != "completed" && failed.managed_continuation->turns.back().status != uam::ManagedRunTurnStatus::Completed);
		UAM_ASSERT(failed.result_excerpt != owner.fixture.answer);
		UAM_ASSERT_EQ(failed.managed_continuation->turns.back().turn_id, receipt.turn_id);
		UAM_ASSERT(!failed.diagnostic_code.empty() || !failed.diagnostic.empty());
		owner.Preserved(failed, mode == "refuse-resume" ? 1 : 2);
		UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(owner.fixture.temp.root / "private-native-history.json")).at("native_id").get<std::string>(), owner.native);
		errno = 0;
		UAM_ASSERT(pid > 0 && kill(pid, 0) < 0 && errno == ESRCH);
		owner.PumpNoReplay(mode == "refuse-resume" ? 1 : 2);
		owner.LaunchCount(2);
		UAM_ASSERT(owner.fixture.StopAndReap());
	}
}

UAM_TEST(ManagedFollowupCancelOwnsPendingPreparationAndRunningNativeTurn)
{
	for (const bool running : {false, true})
	{
		ManagedFollowupNegativeFixtureR98 owner;
		const auto completion = owner.Begin(owner.Request());
		pid_t pid = 0;
		if (running)
		{
			(void)owner.Accepted(completion);
			pid = owner.HoldInput();
		}
		else
		{
			// No scheduler Poll has run after Pending; the canonical predecessor is still genuinely completed.
			UAM_ASSERT_EQ(owner.fixture.ReadRun().managed_continuation, owner.predecessor.managed_continuation);
			UAM_ASSERT(uam::AgentRunScheduler::HasPendingInitialMention(owner.fixture.app, owner.fixture.root_id));
			UAM_ASSERT(uam::AgentRunScheduler::HasPendingInitialMention(owner.fixture.app, owner.predecessor.transcript_chat_id));
		}
		std::string error;
		// Cancellation is initiated now; false truthfully means Stop/preparation bookkeeping is still pending.
		const bool cancelled_now = uam::AgentRunScheduler::CancelTree(owner.fixture.app, owner.predecessor.id, &error);
		UAM_ASSERT(!cancelled_now);
		UAM_ASSERT(error.find("waiting") != std::string::npos);
		UAM_ASSERT(error.find(running ? "provider" : "preparation") != std::string::npos);
		owner.Retired();
		const auto cancelled = owner.fixture.ReadRun();
		owner.Preserved(cancelled, running ? 2 : 1);
		if (running)
		{
			UAM_ASSERT_EQ(cancelled.status, std::string("cancelled"));
			UAM_ASSERT_EQ(cancelled.managed_continuation->turns.back().status, uam::ManagedRunTurnStatus::Cancelled);
			UAM_ASSERT(cancelled.managed_continuation->turns.back().result_delivered);
			UAM_ASSERT_EQ(cancelled.managed_continuation->turns.back().diagnostic_code, std::string("user_cancelled"));
			UAM_ASSERT(cancelled.result_excerpt != owner.fixture.answer);
			errno = 0;
			UAM_ASSERT(pid > 0 && kill(pid, 0) < 0 && errno == ESRCH); // Assert real runtime Stop/reap before cleanup.
		}
		else
		{
			UAM_ASSERT_EQ(cancelled.managed_continuation, owner.predecessor.managed_continuation);
			UAM_ASSERT(completion->value && !completion->value->accepted && !completion->value->acceptance);
			UAM_ASSERT(!fs::exists(owner.fixture.temp.root / "resume-launch-witness.json"));
		}
		UAM_ASSERT_EQ(completion->count, std::size_t{1});
		owner.PumpNoReplay(running ? 2 : 1);
		owner.LaunchCount(running ? 2 : 1);
		UAM_ASSERT(owner.fixture.StopAndReap());
	}
}

UAM_TEST(ManagedFollowupShutdownCancelsRealPreparationBeforeForceBarrier)
{
	ManagedFollowupNegativeFixtureR98 owner;
	const auto root_file = AppPaths::UamChatFilePath(owner.fixture.app.data_root, owner.fixture.root_id);
	const auto child_file = AppPaths::UamChatFilePath(owner.fixture.app.data_root, owner.predecessor.transcript_chat_id);
	const std::string root_bytes = ReadFile(root_file), child_bytes = ReadFile(child_file);
	const auto completion = owner.Begin(owner.Request());
	UAM_ASSERT(uam::AgentRunScheduler::HasPendingInitialMention(owner.fixture.app, owner.fixture.root_id));
	UAM_ASSERT_EQ(owner.fixture.ReadRun().managed_continuation, owner.predecessor.managed_continuation);
	bool quiesced = uam::AgentRunScheduler::QuiesceInitialForShutdown(owner.fixture.app); // Application's real pre-first-force-barrier hook.
	owner.fixture.Await([&]
	{
		if (!quiesced) quiesced = uam::AgentRunScheduler::QuiesceInitialForShutdown(owner.fixture.app);
		return quiesced && !uam::AgentRunScheduler::HasPendingInitialMention(owner.fixture.app, owner.fixture.root_id);
	});
	UAM_ASSERT_EQ(owner.fixture.ReadRun().managed_continuation, owner.predecessor.managed_continuation);
	UAM_ASSERT_EQ(ReadFile(root_file), root_bytes);
	UAM_ASSERT_EQ(ReadFile(child_file), child_bytes);
	UAM_ASSERT(completion->count <= 1 && (!completion->value || (!completion->value->accepted && !completion->value->acceptance)));
	UAM_ASSERT(owner.fixture.app.managed_initial_runtime_owners.empty() && owner.fixture.app.acp_sessions.empty() && owner.fixture.app.queued_agent_run_ids.empty());
	UAM_ASSERT(!fs::exists(owner.fixture.temp.root / "resume-launch-witness.json"));
	owner.PumpNoReplay(1);
	owner.LaunchCount(1);
	UAM_ASSERT(uam::AgentRunScheduler::InitialForceSaveAllowed(owner.fixture.app, owner.fixture.root_id));
	UAM_ASSERT(uam::AgentRunScheduler::InitialForceSaveAllowed(owner.fixture.app, owner.predecessor.transcript_chat_id));
	UAM_ASSERT(owner.fixture.StopAndReap());
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
namespace
{
	std::string ManagedFollowupThreeTurnPeerR100()
	{
		std::string peer = ManagedFollowupResumePeerR98();
		const auto replace = [&](const std::string& before, const std::string& after)
		{
			const auto at = peer.find(before);
			UAM_ASSERT(at != std::string::npos && peer.find(before, at + before.size()) == std::string::npos);
			peer.replace(at, before.size(), after);
		};
		// Private copy only: derive the next turn from the peer's genuinely persisted inputs.
		replace("    number = 2 if (root / 'private-native-history.json').exists() else 1\n",
		    "    number = len(read_document(root / 'private-native-history.json')['inputs']) + 1 if (root / 'private-native-history.json').exists() else 1\n"
		    "    assert 1 <= number <= 3\n");
		replace("        assert turn['afterTurnId'] == managed['turns'][0]['turnId'] and turn['text'] == config['followup_task']\n",
		    "        assert turn['afterTurnId'] == managed['turns'][-2]['turnId'] and turn['text'] == config['followup_task']\n"
		    "        assert all(old['status'] == 'completed' and old['resultDelivered'] for old in managed['turns'][:-1])\n");
		replace("run_id=run['id'], turn_id=turn['turnId'], turn_number=2,\n",
		    "run_id=run['id'], turn_id=turn['turnId'], turn_number=number,\n");
		replace("        if number == 2:\n", "        if number > 1:\n");
		replace("            assert history['inputs'] == [config['task']] and history['answers'] == [config['answer']]\n",
		    "            assert len(history['inputs']) == len(history['answers']) == number - 1\n"
		    "            assert history['inputs'][0] == config['task'] and history['answers'][0] == config['answer']\n");
		return peer;
	}

	struct ManagedFollowupCompletedTwoR100
	{
		uam::ManagedFollowupRequest request;
		uam::ManagedFollowupAcceptance acceptance;
		AgentRun completed;
	};

	ManagedFollowupCompletedTwoR100 ManagedFollowupCompleteTwoR100(ManagedFollowupNegativeFixtureR98& owner)
	{
		UAM_ASSERT(uam::io::WriteTextFile(owner.fixture.temp.root / "peer.py", ManagedFollowupThreeTurnPeerR100()));
		fs::permissions(owner.fixture.temp.root / "peer.py", fs::perms::owner_all, fs::perm_options::replace);
		owner.fixture.AssertPeerFiles();
		ManagedFollowupCompletedTwoR100 proof;
		proof.request = owner.Request();
		proof.acceptance = owner.Accepted(owner.Begin(proof.request));
		const pid_t pid = owner.HoldInput();
		UAM_ASSERT(uam::io::WriteTextFile(owner.fixture.temp.root / "allow-followup-completion", "release the genuine second turn\n"));
		owner.Retired();
		proof.completed = owner.fixture.ReadRun();
		UAM_ASSERT_EQ(proof.completed.status, std::string("completed"));
		UAM_ASSERT(proof.completed.managed_continuation && proof.completed.managed_continuation->execution);
		const auto& turns = proof.completed.managed_continuation->turns;
		UAM_ASSERT_EQ(turns.size(), std::size_t{2});
		UAM_ASSERT_EQ(turns.front(), owner.predecessor.managed_continuation->turns.front());
		UAM_ASSERT_EQ(turns.back().turn_id, proof.acceptance.turn_id);
		UAM_ASSERT(turns.back().result_delivered && turns.back().dispatch_attempted);
		UAM_ASSERT_EQ(turns.back().status, uam::ManagedRunTurnStatus::Completed);
		UAM_ASSERT_EQ(proof.completed.result_excerpt, owner.answer);
		UAM_ASSERT_EQ(proof.completed.managed_continuation->execution, owner.predecessor.managed_continuation->execution);
		owner.Preserved(proof.completed, 2);
		const auto parent = ChatRepository::LoadLocalChatBounded(owner.fixture.app.data_root, owner.fixture.root_id, 4 * 1024 * 1024);
		UAM_ASSERT(parent);
		ManagedFollowupResultR98(*parent, proof.completed.id, turns.back().delivery_id, owner.answer, turns.back().turn_id, 2);
		const auto history = nlohmann::json::parse(ReadFile(owner.fixture.temp.root / "private-native-history.json"));
		UAM_ASSERT_EQ(history.at("native_id").get<std::string>(), owner.native);
		UAM_ASSERT_EQ(history.at("inputs"), nlohmann::json::array({owner.fixture.task, owner.text}));
		UAM_ASSERT_EQ(history.at("answers"), nlohmann::json::array({owner.fixture.answer, owner.answer}));
		errno = 0;
		UAM_ASSERT(pid > 0 && kill(pid, 0) < 0 && errno == ESRCH);
		UAM_ASSERT(owner.fixture.app.managed_initial_runtime_owners.empty() && owner.fixture.app.acp_sessions.empty());
		UAM_ASSERT(owner.fixture.app.managed_followups.empty() && owner.fixture.app.queued_agent_run_ids.empty());
		owner.LaunchCount(2);
		return proof;
	}
} // namespace

UAM_TEST(ManagedFollowupRetiredWholePrefixRejectsEarlierAnswerRewrite)
{
	for (const bool change_earlier_answer : {false, true})
	{
		ManagedFollowupNegativeFixtureR98 owner;
		const auto proof = ManagedFollowupCompleteTwoR100(owner);
		const auto& completed = proof.completed;
		const auto ledger_file = owner.fixture.app.data_root / "agent-runs" / (completed.id + ".json");
		const auto root_file = AppPaths::UamChatFilePath(owner.fixture.app.data_root, owner.fixture.root_id);
		const auto child_file = AppPaths::UamChatFilePath(owner.fixture.app.data_root, completed.transcript_chat_id);
		const auto child = ChatRepository::LoadLocalChatBounded(owner.fixture.app.data_root, completed.transcript_chat_id, 4 * 1024 * 1024);
		UAM_ASSERT(child && !ChatDomainService().FindChatById(owner.fixture.app, completed.transcript_chat_id));
		UAM_ASSERT_EQ(std::ranges::count(child->messages, owner.fixture.answer, &Message::content), 1);
		UAM_ASSERT_EQ(std::ranges::count(child->messages, owner.answer, &Message::content), 1);
		UAM_ASSERT(!owner.fixture.app.pending_chat_save_at_by_chat_id.contains(completed.transcript_chat_id));
		uam::ManagedFollowupRequest next;
		next.after_turn_id = completed.managed_continuation->current_turn_id;
		next.client_request_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		next.text = "Continue this same native conversation for the third distinct answer.";
		const std::string third_answer = "PRIVATE-SAME-NATIVE-THIRD-ANSWER";
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(next.client_request_id));
		auto config = nlohmann::json::parse(ReadFile(owner.fixture.temp.root / "peer-config.json"));
		config["followup_task"] = next.text;
		config["followup_answer"] = third_answer;
		UAM_ASSERT(uam::io::WriteTextFile(owner.fixture.temp.root / "peer-config.json", config.dump()));
		owner.fixture.AssertPeerFiles();
		UAM_ASSERT(fs::remove(owner.fixture.temp.root / "followup-input-witness.json"));
		UAM_ASSERT(fs::remove(owner.fixture.temp.root / "allow-followup-completion"));
		if (change_earlier_answer)
		{
			ChatSession changed = *child;
			const auto old_answer = std::ranges::find_if(changed.messages, [&](const Message& message) { return message.role == MessageRole::Assistant && message.content == owner.fixture.answer; });
			UAM_ASSERT(old_answer != changed.messages.end());
			const auto index = static_cast<std::size_t>(std::distance(changed.messages.begin(), old_answer));
			changed.messages[index].content = "Earlier turn1 assistant answer changed through public same-birth SaveChat.";
			UAM_ASSERT(ChatRepository::SaveChat(owner.fixture.app.data_root, changed));
			const auto reread = ChatRepository::LoadLocalChatBounded(owner.fixture.app.data_root, changed.id, 4 * 1024 * 1024);
			UAM_ASSERT(reread && reread->messages == changed.messages);
			UAM_ASSERT_EQ(reread->managed_followup_incarnation, child->managed_followup_incarnation);
			UAM_ASSERT_EQ(reread->native_session_id, owner.native);
			UAM_ASSERT_EQ(reread->agent_run_id, completed.id);
			UAM_ASSERT_EQ(std::ranges::count(reread->messages, owner.answer, &Message::content), 1);
			auto restored = reread->messages;
			restored[index].content = child->messages[index].content;
			UAM_ASSERT_EQ(restored, child->messages); // The public mutation changed only the earlier assistant content.
			UAM_ASSERT_EQ(owner.fixture.ReadRun().managed_continuation, completed.managed_continuation);
		}
		const std::string root_bytes = ReadFile(root_file), child_bytes = ReadFile(child_file), ledger_bytes = ReadFile(ledger_file);
		const auto backup = uam::io::MakeBackupPath(child_file);
		const std::optional<std::string> backup_bytes = fs::exists(backup) ? std::optional<std::string>(ReadFile(backup)) : std::nullopt;
		const auto inputs = ReadFile(owner.fixture.temp.root / "task-inputs.jsonl");
		const auto history_bytes = ReadFile(owner.fixture.temp.root / "private-native-history.json");
		const auto completion = std::make_shared<ManagedFollowupNegativeFixtureR98::Completion>();
		uam::ManagedFollowupCode rejection = uam::ManagedFollowupCode::None;
		const auto admission = uam::ManagedSubagentFollowupService::TrySubmit(owner.fixture.app, owner.fixture.root_id, completed.id, next,
		    [&, completion](const uam::ManagedFollowupSubmitResult& result) { ++completion->count; completion->value = result; completion->at_ack = owner.fixture.ReadRun(); return true; }, &rejection);
		if (change_earlier_answer)
		{
			if (admission == uam::ManagedFollowupAdmission::Pending)
			{
				owner.fixture.Await([&] { return completion->value.has_value(); });
				UAM_ASSERT_EQ(completion->count, std::size_t{1});
				UAM_ASSERT(!completion->value->accepted && !completion->value->acceptance);
				UAM_ASSERT(completion->value->code != uam::ManagedFollowupCode::None);
			}
			else
			{
				UAM_ASSERT_EQ(completion->count, std::size_t{0});
				UAM_ASSERT(rejection != uam::ManagedFollowupCode::None);
			}
			UAM_ASSERT_EQ(owner.fixture.ReadRun().managed_continuation, completed.managed_continuation);
			UAM_ASSERT(owner.fixture.app.managed_followups.empty() && owner.fixture.app.managed_initial_runtime_owners.empty() && owner.fixture.app.acp_sessions.empty() && owner.fixture.app.queued_agent_run_ids.empty());
			for (int poll = 0; poll < 100; ++poll) { owner.fixture.Pulse(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
			UAM_ASSERT_EQ(ReadFile(root_file), root_bytes);
			UAM_ASSERT_EQ(ReadFile(child_file), child_bytes);
			UAM_ASSERT_EQ(ReadFile(ledger_file), ledger_bytes);
			UAM_ASSERT_EQ(fs::exists(backup), backup_bytes.has_value());
			if (backup_bytes) UAM_ASSERT_EQ(ReadFile(backup), *backup_bytes);
			UAM_ASSERT_EQ(ReadFile(owner.fixture.temp.root / "task-inputs.jsonl"), inputs);
			UAM_ASSERT_EQ(ReadFile(owner.fixture.temp.root / "private-native-history.json"), history_bytes);
			UAM_ASSERT(!fs::exists(owner.fixture.temp.root / "followup-input-witness.json"));
			owner.LaunchCount(2);
		}
		else
		{
			UAM_ASSERT_EQ(admission, uam::ManagedFollowupAdmission::Pending);
			owner.fixture.Await([&] { return completion->value.has_value(); });
			UAM_ASSERT_EQ(completion->count, std::size_t{1});
			UAM_ASSERT(completion->value->accepted && !completion->value->replayed && completion->value->acceptance);
			UAM_ASSERT(completion->at_ack && completion->at_ack->managed_continuation);
			const auto& queued = completion->at_ack->managed_continuation->turns;
			UAM_ASSERT_EQ(queued.size(), std::size_t{3});
			UAM_ASSERT(std::equal(completed.managed_continuation->turns.begin(), completed.managed_continuation->turns.end(), queued.begin()));
			UAM_ASSERT_EQ(queued.back().status, uam::ManagedRunTurnStatus::Queued);
			owner.fixture.Await([&] { return fs::exists(owner.fixture.temp.root / "followup-input-witness.json"); });
			const auto wire = nlohmann::json::parse(ReadFile(owner.fixture.temp.root / "followup-input-witness.json"));
			UAM_ASSERT_EQ(wire.at("turn").at("turnNumber").get<int>(), 3);
			UAM_ASSERT_EQ(wire.at("turn").at("text").get<std::string>(), next.text);
			UAM_ASSERT_EQ(ManagedFollowupInputsR98(owner.fixture).size(), std::size_t{3});
			UAM_ASSERT(uam::io::WriteTextFile(owner.fixture.temp.root / "allow-followup-completion", "release only the actual third input\n"));
			owner.Retired();
			const auto third = owner.fixture.ReadRun();
			UAM_ASSERT_EQ(third.status, std::string("completed"));
			UAM_ASSERT_EQ(third.result_excerpt, third_answer);
			UAM_ASSERT_EQ(third.managed_continuation->execution, completed.managed_continuation->execution);
			UAM_ASSERT_EQ(third.managed_continuation->owner, completed.managed_continuation->owner);
			UAM_ASSERT_EQ(third.managed_continuation->turns.size(), std::size_t{3});
			UAM_ASSERT(std::equal(completed.managed_continuation->turns.begin(), completed.managed_continuation->turns.end(), third.managed_continuation->turns.begin()));
			const auto& turn = third.managed_continuation->turns.back();
			UAM_ASSERT(turn.result_delivered && turn.dispatch_attempted && turn.status == uam::ManagedRunTurnStatus::Completed);
			UAM_ASSERT_EQ(turn.text, next.text);
			const auto parent = ChatRepository::LoadLocalChatBounded(owner.fixture.app.data_root, owner.fixture.root_id, 4 * 1024 * 1024);
			const auto saved = ChatRepository::LoadLocalChatBounded(owner.fixture.app.data_root, third.transcript_chat_id, 4 * 1024 * 1024);
			UAM_ASSERT(parent && saved && saved->messages.size() > child->messages.size());
			UAM_ASSERT(std::equal(child->messages.begin(), child->messages.end(), saved->messages.begin()));
			ManagedFollowupResultR98(*parent, third.id, turn.delivery_id, third_answer, turn.turn_id, 3);
			const auto history = nlohmann::json::parse(ReadFile(owner.fixture.temp.root / "private-native-history.json"));
			UAM_ASSERT_EQ(history.at("native_id").get<std::string>(), owner.native);
			UAM_ASSERT_EQ(history.at("inputs"), nlohmann::json::array({owner.fixture.task, owner.text, next.text}));
			UAM_ASSERT_EQ(history.at("answers"), nlohmann::json::array({owner.fixture.answer, owner.answer, third_answer}));
			errno = 0;
			UAM_ASSERT(kill(wire.at("pid").get<pid_t>(), 0) < 0 && errno == ESRCH);
			owner.PumpNoReplay(3);
			owner.LaunchCount(3);
		}
		UAM_ASSERT(owner.fixture.StopAndReap());
	}
}

UAM_TEST(ManagedFollowupAuthorizesCurrentRootBeforeClientConflictDisclosure)
{
	ManagedFollowupNegativeFixtureR98 owner;
	const auto proof = ManagedFollowupCompleteTwoR100(owner);
	const auto* root = ChatDomainService().FindChatById(owner.fixture.app, owner.fixture.root_id);
	UAM_ASSERT(root && root->metadata_incarnation == owner.fixture.root_incarnation);
	const ChatSession original = *root;
	UAM_ASSERT_EQ(owner.fixture.app.chats.size(), std::size_t{1});
	const auto ledger_file = owner.fixture.app.data_root / "agent-runs" / (proof.completed.id + ".json");
	const auto root_file = AppPaths::UamChatFilePath(owner.fixture.app.data_root, owner.fixture.root_id);
	const auto child_file = AppPaths::UamChatFilePath(owner.fixture.app.data_root, proof.completed.transcript_chat_id);
	const std::string ledger_bytes = ReadFile(ledger_file), root_bytes = ReadFile(root_file), child_bytes = ReadFile(child_file);
	const auto events = ReadFile(owner.fixture.temp.root / "peer-events.jsonl");
	const auto inputs = ReadFile(owner.fixture.temp.root / "task-inputs.jsonl");
	auto conflict = proof.request;
	conflict.text += " Different text for the genuinely committed client UUID.";
	for (const std::string arm : {"missing", "changed-birth", "replaced-incarnation"})
	{
		if (arm == "missing") owner.fixture.app.chats.clear();
		else
		{
			owner.fixture.app.chats.front() = original;
			if (arm == "changed-birth") owner.fixture.app.chats.front().managed_followup_incarnation = PlatformServicesFactory::Instance().process_service.GenerateUuid();
			else
			{
				// A new ordinary ChatSession supplies a distinct volatile namespace token, never execution authority.
				const auto replacement = ChatDomainService().CreateNewChat({}, "claude-cli");
				UAM_ASSERT(replacement.metadata_incarnation != original.metadata_incarnation);
				owner.fixture.app.chats.front().metadata_incarnation = replacement.metadata_incarnation;
			}
		}
		const auto callbacks = std::make_shared<std::size_t>(0);
		uam::ManagedFollowupCode rejection = uam::ManagedFollowupCode::None;
		const auto admission = uam::ManagedSubagentFollowupService::TrySubmit(owner.fixture.app, owner.fixture.root_id, proof.completed.id, conflict,
		    [callbacks](const uam::ManagedFollowupSubmitResult&) { ++*callbacks; return true; }, &rejection);
		if (rejection != uam::ManagedFollowupCode::ScopeChanged) std::cerr << "Root scope arm " << arm << " disclosed code " << static_cast<int>(rejection) << '\n';
		UAM_ASSERT_EQ(admission, uam::ManagedFollowupAdmission::Rejected);
		UAM_ASSERT_EQ(rejection, uam::ManagedFollowupCode::ScopeChanged);
		UAM_ASSERT_EQ(*callbacks, std::size_t{0});
		UAM_ASSERT(owner.fixture.app.managed_followups.empty() && owner.fixture.app.managed_initial_runtime_owners.empty() && owner.fixture.app.acp_sessions.empty() && owner.fixture.app.local_acp_launch_tasks.empty() && owner.fixture.app.queued_agent_run_ids.empty());
		UAM_ASSERT_EQ(owner.fixture.ReadRun().managed_continuation, proof.completed.managed_continuation);
		UAM_ASSERT_EQ(ReadFile(ledger_file), ledger_bytes);
		UAM_ASSERT_EQ(ReadFile(root_file), root_bytes);
		UAM_ASSERT_EQ(ReadFile(child_file), child_bytes);
		UAM_ASSERT_EQ(ReadFile(owner.fixture.temp.root / "peer-events.jsonl"), events);
		UAM_ASSERT_EQ(ReadFile(owner.fixture.temp.root / "task-inputs.jsonl"), inputs);
		owner.fixture.app.chats.assign(1, original); // Restore the genuine original object/token before any owner Poll.
	}
	const auto rejected = owner.SubmitClassified(conflict);
	UAM_ASSERT(!rejected.accepted && !rejected.acceptance && rejected.code == uam::ManagedFollowupCode::IdempotencyConflict);
	const auto replay = owner.SubmitClassified(proof.request);
	UAM_ASSERT(replay.accepted && replay.replayed && replay.acceptance && *replay.acceptance == proof.acceptance);
	UAM_ASSERT_EQ(replay.acceptance->status_at_admission, uam::ManagedRunTurnStatus::Queued);
	UAM_ASSERT_EQ(ReadFile(ledger_file), ledger_bytes);
	UAM_ASSERT_EQ(ReadFile(root_file), root_bytes);
	UAM_ASSERT_EQ(ReadFile(child_file), child_bytes);
	owner.PumpNoReplay(2);
	owner.LaunchCount(2);
	UAM_ASSERT(owner.fixture.StopAndReap());
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
namespace
{
	struct ManagedFollowupCefFixtureR104
	{
		ManagedFollowupNegativeFixtureR98 owner;
		struct Query
		{
			CefRefPtr<UamQueryHandler::Callback> callback;
			std::function<AsyncCefResult()> worker;
			std::function<void(AsyncCefResult&)> completion;
		};
		std::vector<Query> pending;
		std::string scope;

		ManagedFollowupCefFixtureR104()
		{
			auto& fixture = owner.fixture;
			const auto state = uam::ManagedSubagentFollowupService::GetState(fixture.app, fixture.root_id, owner.predecessor.id);
			UAM_ASSERT(state.can_send && state.code == uam::ManagedFollowupCode::None);
			UAM_ASSERT_EQ(state.current_turn_id, owner.predecessor.managed_continuation->current_turn_id);
			UAM_ASSERT_EQ(state.current_turn_number, std::uint32_t{1});
			UAM_ASSERT_EQ(owner.predecessor.root_chat_id, fixture.root_id);
			UAM_ASSERT_EQ(owner.predecessor.managed_continuation->execution->native_session_id, owner.native);
			UAM_ASSERT(fixture.handler && fixture.app.managed_followups.empty() && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty());
			NativeMediaRefreshHandlerTestAccess::RestartInspectionDispatch(*fixture.handler,
			    [this](auto callback, auto worker, auto completion)
			    {
				    pending.push_back({callback, std::move(worker), std::move(completion)});
				    return true;
			    });
			CefRefPtr<RestartInspectionCallbackR91> listed = new RestartInspectionCallbackR91;
			UAM_ASSERT(NativeMediaRefreshHandlerTestAccess::FollowupAction(*fixture.handler, "listManagedSubagents",
			    {{"chatId", fixture.root_id}, {"runId", owner.predecessor.id}}, listed));
			UAM_ASSERT_EQ(listed->successes + listed->failures, 0);
			UAM_ASSERT_EQ(pending.size(), std::size_t{1});
			DrainReads();
			UAM_ASSERT_EQ(listed->failures, 0);
			UAM_ASSERT_EQ(listed->successes, 1);
			UAM_ASSERT_EQ(listed->body.at("chatId").get<std::string>(), fixture.root_id);
			UAM_ASSERT_EQ(listed->body.at("runs").size(), std::size_t{1});
			const auto& row = listed->body.at("runs").front();
			UAM_ASSERT_EQ(row.at("runId").get<std::string>(), owner.predecessor.id);
			UAM_ASSERT_EQ(row.at("transcriptChatId").get<std::string>(), owner.predecessor.transcript_chat_id);
			UAM_ASSERT_EQ(row.at("status").get<std::string>(), std::string("completed"));
			UAM_ASSERT(!row.at("lastReported").get<bool>());
			scope = listed->body.at("controlScopeId").get<std::string>();
			UAM_ASSERT(!scope.empty()); // Actual server-held opaque scope; never injected or reconstructed.
			owner.Preserved(fixture.ReadRun(), 1);
		}
		~ManagedFollowupCefFixtureR104()
		{
			// The dispatcher captures this fixture; destroy its handler before member teardown.
			owner.fixture.handler.reset();
			pending.clear();
		}
		void DrainReads()
		{
			while (!pending.empty())
			{
				auto query = std::move(pending.front());
				pending.erase(pending.begin());
				auto result = std::async(std::launch::async, query.worker).get();
				CompleteAsyncCefQuery(query.callback, result, query.completion);
			}
		}
		nlohmann::json Payload(const uam::ManagedFollowupRequest& request) const
		{
			return {{"chatId", owner.fixture.root_id}, {"runId", owner.predecessor.id}, {"controlScopeId", scope},
			    {"afterTurnId", request.after_turn_id}, {"clientRequestId", request.client_request_id}, {"text", request.text}};
		}
	};

	class ManagedFollowupCefCallbackR104 final : public UamQueryHandler::Callback
	{
	  public:
		int successes = 0, failures = 0, status = 0;
		std::string error;
		nlohmann::json body;
		std::function<void()> capture;
		void Success(const CefString& value) override
		{
			++successes;
			body = nlohmann::json::parse(value.ToString());
			if (capture) capture();
		}
		void Success(const void*, size_t) override { throw TestFailure("Follow-up requires the structured raw JSON receipt."); }
		void Failure(int code, const CefString& value) override
		{
			++failures;
			status = code;
			error = value.ToString();
		}
	  private:
		IMPLEMENT_REFCOUNTING(ManagedFollowupCefCallbackR104);
	};
}

UAM_TEST(ManagedFollowupCefActionResumesTheGenuineScopedNativeChild)
{
	ManagedFollowupCefFixtureR104 bridge;
	auto& owner = bridge.owner;
	auto& fixture = owner.fixture;
	const auto request = owner.Request();
	const auto payload = bridge.Payload(request);
	std::optional<AgentRun> at_ack;
	CefRefPtr<ManagedFollowupCefCallbackR104> callback = new ManagedFollowupCefCallbackR104;
	callback->capture = [&] { at_ack = fixture.ReadRun(); };
	// First missing-route feature oracle, after genuine binding/reap/list/scope prerequisites.
	UAM_ASSERT(NativeMediaRefreshHandlerTestAccess::FollowupAction(*fixture.handler, "submitManagedSubagentFollowup", payload, callback));
	fixture.Await([&] { bridge.DrainReads(); return callback->successes + callback->failures != 0; });
	if (callback->failures) throw TestFailure("Managed follow-up CEF refusal: " + callback->error.substr(0, 300));
	UAM_ASSERT_EQ(callback->successes, 1);
	UAM_ASSERT_EQ(callback->failures, 0);
	UAM_ASSERT(callback->body.is_object() && !callback->body.contains("ok"));
	const auto& result = callback->body.at("followup");
	UAM_ASSERT(result.at("accepted").get<bool>() && !result.at("replayed").get<bool>());
	UAM_ASSERT(result.at("code").is_string() && !result.at("code").get<std::string>().empty());
	const auto& receipt = result.at("acceptance");
	UAM_ASSERT(receipt.is_object());
	UAM_ASSERT_EQ(receipt.at("rootChatId").get<std::string>(), fixture.root_id);
	UAM_ASSERT_EQ(receipt.at("runId").get<std::string>(), owner.predecessor.id);
	UAM_ASSERT_EQ(receipt.at("transcriptChatId").get<std::string>(), owner.predecessor.transcript_chat_id);
	UAM_ASSERT_EQ(receipt.at("clientRequestId").get<std::string>(), request.client_request_id);
	const auto turn_id = receipt.at("turnId").get<std::string>();
	UAM_ASSERT(uam::uuid::IsCanonicalUuid(turn_id) && turn_id != owner.predecessor.id);
	UAM_ASSERT_EQ(receipt.at("turnNumber").get<std::uint32_t>(), std::uint32_t{2});
	UAM_ASSERT_EQ(receipt.at("statusAtAdmission").get<std::string>(), std::string("queued"));
	UAM_ASSERT(at_ack && at_ack->managed_continuation && at_ack->managed_continuation->turns.size() == 2);
	const auto& admitted = at_ack->managed_continuation->turns.back();
	UAM_ASSERT_EQ(admitted.turn_id, turn_id);
	UAM_ASSERT_EQ(admitted.status, uam::ManagedRunTurnStatus::Queued);
	UAM_ASSERT(!admitted.dispatch_attempted);
	UAM_ASSERT_EQ(admitted.text, request.text);
	UAM_ASSERT_EQ(admitted.after_turn_id, request.after_turn_id);
	UAM_ASSERT_EQ(admitted.client_request_id, request.client_request_id);
	UAM_ASSERT_EQ(admitted.principal.kind, uam::ManagedRunPrincipalKind::UserRoot);
	UAM_ASSERT_EQ(admitted.principal.root_chat_id, fixture.root_id);
	UAM_ASSERT_EQ(admitted.principal.root_birth_id, owner.predecessor.managed_continuation->owner.root_birth_id);
	UAM_ASSERT_EQ(admitted.delivery_id, turn_id);
	UAM_ASSERT_EQ(admitted.continuity, uam::ManagedRunContinuity::Native);
	UAM_ASSERT_EQ(admitted.recovery_decision, uam::ManagedRunRecoveryDecision::None);
	UAM_ASSERT(admitted.retry_of_turn_id.empty() && admitted.principal.parent_run_id.empty());
	UAM_ASSERT_EQ(at_ack->managed_continuation->turns.front(), owner.predecessor.managed_continuation->turns.front());
	UAM_ASSERT_EQ(at_ack->managed_continuation->execution, owner.predecessor.managed_continuation->execution);
	const pid_t pid = owner.HoldInput(); // Actual peer verifies durable row/serial/intent before this second wire input.
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-followup-completion", "release CEF-owned native continuation\n"));
	owner.Retired();
	const auto completed = fixture.ReadRun();
	UAM_ASSERT_EQ(completed.status, std::string("completed"));
	UAM_ASSERT_EQ(completed.managed_continuation->turns.size(), std::size_t{2});
	const auto& turn = completed.managed_continuation->turns.back();
	UAM_ASSERT_EQ(turn.turn_id, turn_id);
	UAM_ASSERT_EQ(turn.status, uam::ManagedRunTurnStatus::Completed);
	UAM_ASSERT(turn.dispatch_attempted && turn.result_required && turn.result_delivered);
	UAM_ASSERT(turn.expected_turn_serial > 0 && turn.started_at_epoch_ms > 0 && turn.finished_at_epoch_ms >= turn.started_at_epoch_ms);
	UAM_ASSERT_EQ(turn.text, request.text);
	UAM_ASSERT_EQ(completed.result_excerpt, owner.answer);
	const auto parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(parent);
	ManagedFollowupResultR98(*parent, completed.id, turn_id, owner.answer, turn_id, 2);
	errno = 0;
	UAM_ASSERT(pid > 0 && kill(pid, 0) < 0 && errno == ESRCH);
	owner.Preserved(completed, 2);
	owner.PumpNoReplay(2);
	owner.LaunchCount(2);
	UAM_ASSERT_EQ(callback->successes, 1);
	UAM_ASSERT_EQ(callback->failures, 0);
	UAM_ASSERT(fixture.StopAndReap());
}

UAM_TEST(ManagedFollowupCefActionRejectsForeignAndReplacedControlScopeBeforeInput)
{
	ManagedFollowupCefFixtureR104 bridge;
	auto& owner = bridge.owner;
	auto& fixture = owner.fixture;
	const ChatSession original = *ChatDomainService().FindChatById(fixture.app, fixture.root_id);
	for (const bool replace_root : {false, true})
	{
		const auto request = owner.Request();
		auto payload = bridge.Payload(request);
		if (replace_root)
		{
			// Genuine public new-chat incarnation is a negative scope replacement, never a birth grant.
			const auto replacement = ChatDomainService().CreateNewChat({}, "claude-cli");
			UAM_ASSERT(replacement.metadata_incarnation != original.metadata_incarnation);
			ChatDomainService().FindChatById(fixture.app, fixture.root_id)->metadata_incarnation = replacement.metadata_incarnation;
		}
		else
		{
			payload["controlScopeId"] = "foreign-" + bridge.scope; // Deliberately not the actual server-held token.
		}
		const auto ledger_file = fixture.app.data_root / "agent-runs" / (owner.predecessor.id + ".json");
		const auto root_file = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id);
		const auto child_file = AppPaths::UamChatFilePath(fixture.app.data_root, owner.predecessor.transcript_chat_id);
		const std::string ledger = ReadFile(ledger_file), root = ReadFile(root_file), child = ReadFile(child_file);
		const std::string inputs = ReadFile(fixture.temp.root / "task-inputs.jsonl"), events = ReadFile(fixture.temp.root / "peer-events.jsonl");
		CefRefPtr<ManagedFollowupCefCallbackR104> callback = new ManagedFollowupCefCallbackR104;
		UAM_ASSERT(NativeMediaRefreshHandlerTestAccess::FollowupAction(*fixture.handler, "submitManagedSubagentFollowup", payload, callback));
		fixture.Await([&] { bridge.DrainReads(); return callback->successes + callback->failures != 0; });
		UAM_ASSERT_EQ(callback->successes + callback->failures, 1);
		if (callback->failures)
			UAM_ASSERT_EQ(callback->status, 409); // Invalid/stale listing scope may be a bridge ownership error.
		else
		{
			const auto& result = callback->body.at("followup");
			UAM_ASSERT(!result.at("accepted").get<bool>() && !result.at("replayed").get<bool>());
			UAM_ASSERT(result.at("acceptance").is_null());
			UAM_ASSERT(result.at("code").is_string() && !result.at("code").get<std::string>().empty());
		}
		UAM_ASSERT(fixture.app.managed_followups.empty() && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty() && fixture.app.queued_agent_run_ids.empty() && fixture.app.local_acp_launch_tasks.empty());
		UAM_ASSERT_EQ(fixture.ReadRun().managed_continuation, owner.predecessor.managed_continuation);
		UAM_ASSERT_EQ(ReadFile(ledger_file), ledger);
		UAM_ASSERT_EQ(ReadFile(root_file), root);
		UAM_ASSERT_EQ(ReadFile(child_file), child);
		UAM_ASSERT_EQ(ReadFile(fixture.temp.root / "task-inputs.jsonl"), inputs);
		UAM_ASSERT_EQ(ReadFile(fixture.temp.root / "peer-events.jsonl"), events);
		UAM_ASSERT(!fs::exists(fixture.temp.root / "resume-launch-witness.json"));
		*ChatDomainService().FindChatById(fixture.app, fixture.root_id) = original;
		owner.PumpNoReplay(1);
		owner.LaunchCount(1);
	}
	UAM_ASSERT(fixture.StopAndReap());
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
namespace
{
	struct ManagedFollowupCefNoInputProofR105
	{
		std::filesystem::path child_file;
		std::string child_bytes, native_history, inputs, events;

		explicit ManagedFollowupCefNoInputProofR105(const ManagedFollowupCefFixtureR104& bridge)
		{
			const auto& owner = bridge.owner;
			const auto& fixture = owner.fixture;
			child_file = AppPaths::UamChatFilePath(fixture.app.data_root, owner.predecessor.transcript_chat_id);
			child_bytes = ReadFile(child_file);
			native_history = ReadFile(fixture.temp.root / "private-native-history.json");
			inputs = ReadFile(fixture.temp.root / "task-inputs.jsonl");
			events = ReadFile(fixture.temp.root / "peer-events.jsonl");
			UAM_ASSERT_EQ(ManagedFollowupInputsR98(fixture).size(), std::size_t{1});
			UAM_ASSERT(!fs::exists(fixture.temp.root / "resume-launch-witness.json"));
		}

		void Preserved(const ManagedFollowupCefFixtureR104& bridge) const
		{
			const auto& owner = bridge.owner;
			const auto& fixture = owner.fixture;
			owner.Preserved(fixture.ReadRun(), 1);
			UAM_ASSERT_EQ(ReadFile(child_file), child_bytes);
			UAM_ASSERT_EQ(ReadFile(fixture.temp.root / "private-native-history.json"), native_history);
			UAM_ASSERT_EQ(ReadFile(fixture.temp.root / "task-inputs.jsonl"), inputs);
			UAM_ASSERT_EQ(ReadFile(fixture.temp.root / "peer-events.jsonl"), events);
			UAM_ASSERT(!fs::exists(fixture.temp.root / "resume-launch-witness.json"));
			UAM_ASSERT(!fs::exists(fixture.temp.root / "followup-input-witness.json"));
			UAM_ASSERT(fixture.app.managed_followups.empty() && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty());
			UAM_ASSERT(fixture.app.queued_agent_run_ids.empty() && fixture.app.local_acp_launch_tasks.empty());
			owner.LaunchCount(1);
		}
	};

	void ManagedFollowupCefAwaitRetiredR105(ManagedFollowupCefFixtureR104& bridge)
	{
		auto& fixture = bridge.owner.fixture;
		fixture.Await([&]
		{
			bridge.DrainReads();
			return fixture.app.managed_followups.empty() && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty() &&
			    !uam::AgentRunScheduler::HasPendingInitialMention(fixture.app, fixture.root_id);
		});
	}
}

UAM_TEST(ManagedFollowupCefDestroyedHandlerCannotAcknowledgeOrDispatchPendingInput)
{
	ManagedFollowupCefFixtureR104 bridge;
	auto& owner = bridge.owner;
	auto& fixture = owner.fixture;
	const ManagedFollowupCefNoInputProofR105 proof(bridge);
	const auto request = owner.Request();
	const auto payload = bridge.Payload(request);
	const auto root_file = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id);
	const auto ledger_file = fixture.app.data_root / "agent-runs" / (owner.predecessor.id + ".json");
	const auto root_bytes = ReadFile(root_file), ledger_bytes = ReadFile(ledger_file);
	CefRefPtr<ManagedFollowupCefCallbackR104> callback = new ManagedFollowupCefCallbackR104;
	// Existing public dispatch, then genuine Pending, before the first owner Poll.
	UAM_ASSERT(NativeMediaRefreshHandlerTestAccess::FollowupAction(*fixture.handler, "submitManagedSubagentFollowup", payload, callback));
	UAM_ASSERT(fixture.app.managed_followups.contains(owner.predecessor.id));
	UAM_ASSERT(bridge.pending.empty());
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	UAM_ASSERT_EQ(fixture.ReadRun().managed_continuation, owner.predecessor.managed_continuation);
	UAM_ASSERT_EQ(ReadFile(root_file), root_bytes);
	UAM_ASSERT_EQ(ReadFile(ledger_file), ledger_bytes);
	fixture.handler.reset(); // Destroy the real weak-lifetime owner, not its task/receipt or scope map.
	UAM_ASSERT(!fixture.handler);
	ManagedFollowupCefAwaitRetiredR105(bridge);
	UAM_ASSERT_EQ(callback->successes, 0);
	UAM_ASSERT_EQ(callback->failures, 0);
	const auto settled = fixture.ReadRun();
	UAM_ASSERT(settled.managed_continuation);
	const auto& turns = settled.managed_continuation->turns;
	if (turns.size() == 1)
	{
		// Cancelling preparation before publication may leave the exact original record.
		UAM_ASSERT_EQ(settled.managed_continuation, owner.predecessor.managed_continuation);
		UAM_ASSERT_EQ(ReadFile(ledger_file), ledger_bytes);
		UAM_ASSERT_EQ(ReadFile(root_file), root_bytes);
	}
	else
	{
		// A committed append survives loss of its ACK owner as inert failed bookkeeping.
		UAM_ASSERT_EQ(turns.size(), std::size_t{2});
		const auto& turn = turns.back();
		UAM_ASSERT_EQ(turn.text, request.text);
		UAM_ASSERT_EQ(turn.client_request_id, request.client_request_id);
		UAM_ASSERT_EQ(turn.after_turn_id, request.after_turn_id);
		UAM_ASSERT(!turn.dispatch_attempted && turn.result_delivered);
		UAM_ASSERT(turn.status != uam::ManagedRunTurnStatus::Queued && turn.status != uam::ManagedRunTurnStatus::Running && turn.status != uam::ManagedRunTurnStatus::Completed);
		UAM_ASSERT(settled.status != "queued" && settled.status != "running" && settled.status != "completed");
	}
	proof.Preserved(bridge);
	owner.PumpNoReplay(1); // One hundred normal polls, no reinvocation or new UUID/request.
	proof.Preserved(bridge);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	UAM_ASSERT(fixture.StopAndReap());
}

UAM_TEST(ManagedFollowupCefThrowingSuccessCannotAcknowledgeOrDispatchRecordedInput)
{
	ManagedFollowupCefFixtureR104 bridge;
	auto& owner = bridge.owner;
	auto& fixture = owner.fixture;
	const ManagedFollowupCefNoInputProofR105 proof(bridge);
	const auto request = owner.Request();
	std::optional<AgentRun> at_ack;
	CefRefPtr<ManagedFollowupCefCallbackR104> callback = new ManagedFollowupCefCallbackR104;
	callback->capture = [&]
	{
		at_ack = fixture.ReadRun();
		throw std::runtime_error("Private CEF classified Success refused its acknowledgment");
	};
	UAM_ASSERT(NativeMediaRefreshHandlerTestAccess::FollowupAction(*fixture.handler, "submitManagedSubagentFollowup", bridge.Payload(request), callback));
	UAM_ASSERT(fixture.app.managed_followups.contains(owner.predecessor.id));
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	ManagedFollowupCefAwaitRetiredR105(bridge);
	UAM_ASSERT_EQ(callback->successes, 1);
	UAM_ASSERT_EQ(callback->failures, 0); // A throwing Success must never produce a second Failure callback.
	const auto& result = callback->body.at("followup");
	UAM_ASSERT(result.at("accepted").get<bool>() && !result.at("replayed").get<bool>());
	const auto& receipt = result.at("acceptance");
	UAM_ASSERT_EQ(receipt.at("rootChatId").get<std::string>(), fixture.root_id);
	UAM_ASSERT_EQ(receipt.at("runId").get<std::string>(), owner.predecessor.id);
	UAM_ASSERT_EQ(receipt.at("transcriptChatId").get<std::string>(), owner.predecessor.transcript_chat_id);
	UAM_ASSERT_EQ(receipt.at("clientRequestId").get<std::string>(), request.client_request_id);
	const auto turn_id = receipt.at("turnId").get<std::string>();
	UAM_ASSERT(uam::uuid::IsCanonicalUuid(turn_id));
	UAM_ASSERT_EQ(receipt.at("turnNumber").get<std::uint32_t>(), std::uint32_t{2});
	UAM_ASSERT_EQ(receipt.at("statusAtAdmission").get<std::string>(), std::string("queued"));
	UAM_ASSERT(at_ack && at_ack->managed_continuation && at_ack->managed_continuation->turns.size() == 2);
	const auto& recorded = at_ack->managed_continuation->turns.back();
	UAM_ASSERT_EQ(recorded.turn_id, turn_id);
	UAM_ASSERT_EQ(recorded.status, uam::ManagedRunTurnStatus::Queued);
	UAM_ASSERT(!recorded.dispatch_attempted);
	UAM_ASSERT_EQ(recorded.text, request.text);
	UAM_ASSERT_EQ(recorded.client_request_id, request.client_request_id);
	UAM_ASSERT_EQ(recorded.after_turn_id, request.after_turn_id);
	const auto settled = fixture.ReadRun();
	UAM_ASSERT_EQ(settled.managed_continuation->turns.size(), std::size_t{2});
	const auto& turn = settled.managed_continuation->turns.back();
	UAM_ASSERT_EQ(turn.turn_id, turn_id);
	UAM_ASSERT(!turn.dispatch_attempted && turn.result_delivered);
	UAM_ASSERT(turn.status != uam::ManagedRunTurnStatus::Queued && turn.status != uam::ManagedRunTurnStatus::Running && turn.status != uam::ManagedRunTurnStatus::Completed);
	UAM_ASSERT(settled.status != "queued" && settled.status != "running" && settled.status != "completed");
	proof.Preserved(bridge);
	owner.PumpNoReplay(1);
	proof.Preserved(bridge);
	UAM_ASSERT_EQ(callback->successes, 1);
	UAM_ASSERT_EQ(callback->failures, 0);
	UAM_ASSERT(fixture.StopAndReap());
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
UAM_TEST(ManagedFollowupCefQueryCancellationTargetsOnlyItsAdmittedInvocation)
{
	for (const bool matching : {false, true})
	{
		ManagedFollowupCefFixtureR104 bridge;
		auto& owner = bridge.owner;
		auto& fixture = owner.fixture;
		const ManagedFollowupCefNoInputProofR105 proof(bridge);
		const auto request = owner.Request();
		CefRefPtr<ManagedFollowupCefCallbackR104> callback = new ManagedFollowupCefCallbackR104;
		constexpr int64_t query_id = 10457;
		UAM_ASSERT(NativeMediaRefreshHandlerTestAccess::FollowupQueryAction(*fixture.handler, query_id, bridge.Payload(request), callback));
		UAM_ASSERT(fixture.app.managed_followups.contains(owner.predecessor.id));
		UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
		// Drive the actual public cancellation method. Browser/frame origin validation is separate.
		fixture.handler->OnQueryCanceled(nullptr, nullptr, matching ? query_id : query_id + 1);
		if (matching)
		{
			ManagedFollowupCefAwaitRetiredR105(bridge);
			UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
			proof.Preserved(bridge);
			owner.PumpNoReplay(1);
			proof.Preserved(bridge);
		}
		else
		{
			fixture.Await([&] { return callback->successes + callback->failures != 0; });
			UAM_ASSERT_EQ(callback->successes, 1);
			UAM_ASSERT_EQ(callback->failures, 0);
			const auto& result = callback->body.at("followup");
			UAM_ASSERT(result.at("accepted").get<bool>());
			UAM_ASSERT_EQ(result.at("acceptance").at("clientRequestId").get<std::string>(), request.client_request_id);
			const pid_t pid = owner.HoldInput();
			UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-followup-completion", "release after unrelated query cancellation\n"));
			owner.Retired();
			const auto completed = fixture.ReadRun();
			UAM_ASSERT_EQ(completed.status, std::string("completed"));
			UAM_ASSERT_EQ(completed.managed_continuation->turns.size(), std::size_t{2});
			UAM_ASSERT(completed.managed_continuation->turns.back().result_delivered);
			owner.Preserved(completed, 2);
			errno = 0;
			UAM_ASSERT(pid > 0 && kill(pid, 0) < 0 && errno == ESRCH);
			owner.PumpNoReplay(2);
			owner.LaunchCount(2);
			UAM_ASSERT_EQ(callback->successes, 1);
			UAM_ASSERT_EQ(callback->failures, 0);
		}
		UAM_ASSERT(fixture.StopAndReap());
	}
}

UAM_TEST(ManagedFollowupCefProjectsOnlySelectedLiveContinuationHints)
{
	ManagedFollowupCefFixtureR104 bridge;
	auto& owner = bridge.owner;
	auto& fixture = owner.fixture;
	for (const bool selected : {false, true})
	{
		CefRefPtr<RestartInspectionCallbackR91> callback = new RestartInspectionCallbackR91;
		nlohmann::json payload{{"chatId", fixture.root_id}};
		if (selected) payload["runId"] = owner.predecessor.id;
		UAM_ASSERT(NativeMediaRefreshHandlerTestAccess::FollowupAction(*fixture.handler, "listManagedSubagents", payload, callback));
		bridge.DrainReads();
		UAM_ASSERT_EQ(callback->successes, 1);
		UAM_ASSERT_EQ(callback->failures, 0);
		UAM_ASSERT_EQ(callback->body.at("runs").size(), std::size_t{1});
		const auto& row = callback->body.at("runs").front();
		UAM_ASSERT_EQ(row.contains("followup"), selected);
		if (selected)
		{
			const auto& hint = row.at("followup");
			UAM_ASSERT_EQ(hint.size(), std::size_t{4});
			UAM_ASSERT(hint.at("canSend").get<bool>());
			UAM_ASSERT_EQ(hint.at("code").get<std::string>(), std::string("none"));
			UAM_ASSERT_EQ(hint.at("currentTurnId").get<std::string>(), owner.predecessor.managed_continuation->current_turn_id);
			UAM_ASSERT_EQ(hint.at("currentTurnNumber").get<std::uint32_t>(), std::uint32_t{1});
		}
	}
	CefRefPtr<RestartInspectionCallbackR91> transcript = new RestartInspectionCallbackR91;
	NativeMediaRefreshHandlerTestAccess::RestartInspectionAction(*fixture.handler, "getManagedAgentTranscript",
		{{"chatId", fixture.root_id}, {"transcriptChatId", owner.predecessor.transcript_chat_id}}, transcript);
	bridge.DrainReads();
	UAM_ASSERT_EQ(transcript->successes, 1);
	UAM_ASSERT_EQ(transcript->failures, 0);
	UAM_ASSERT(transcript->body.at("followup").at("canSend").get<bool>());
	UAM_ASSERT_EQ(transcript->body.at("followup").size(), std::size_t{4});
	const auto before = fixture.ReadRun();
	owner.PumpNoReplay(1);
	UAM_ASSERT_EQ(fixture.ReadRun().managed_continuation, before.managed_continuation);
	owner.LaunchCount(1);
	UAM_ASSERT(fixture.StopAndReap());
}
#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI && defined(__APPLE__)
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
namespace
{
	std::string ManagedCodexBindingPeerR109()
	{
		return R"UAM_CODEX(#!/usr/bin/python3
import json, os, pathlib, secrets, sys, time, uuid
root = pathlib.Path(__file__).resolve().parent
config = json.loads((root / 'codex-peer-config.json').read_text())
def read(path):
    return json.loads(path.read_text())
def store(name, value):
    temporary = root / (name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, sort_keys=True)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(root / name)
def event(kind, **values):
    with (root / 'codex-peer-events.jsonl').open('a') as stream:
        stream.write(json.dumps(dict(kind=kind, **values), sort_keys=True) + '\n')
        stream.flush()
        os.fsync(stream.fileno())
def emit(value):
    print(json.dumps(value), flush=True)
def result(message, value):
    emit(dict(jsonrpc='2.0', id=message['id'], result=value))
def notice(method, params):
    emit(dict(jsonrpc='2.0', method=method, params=params))
def state():
    data = pathlib.Path(config['data_root'])
    runs = [read(path) for path in (data / 'agent-runs').glob('*.json')]
    runs = [run for run in runs if run['rootChatId'] == config['root_id']]
    assert len(runs) == 1 and runs[0]['version'] == 2
    run = runs[0]
    managed = run['managedContinuation']
    assert managed['currentTurnId'] == run['id'] and managed['currentTurnNumber'] == 1
    assert len(managed['turns']) == 1 and managed['execution'] is None
    turn = managed['turns'][0]
    assert run['status'] == turn['status'] == 'running' and turn['dispatchAttempted']
    child = read(data / 'chats' / (run['transcriptChatId'] + '.json'))
    parent = read(data / 'chats' / (run['rootChatId'] + '.json'))
    assert parent['managed_followup_incarnation'] == managed['owner']['rootBirthId']
    assert child['managed_followup_incarnation'] == managed['owner']['transcriptBirthId']
    assert child['agent_run_id'] == run['id']
    return run, turn, child
try:
    if sys.argv[1:] == ['--version']:
        event('version', pid=os.getpid())
        print('codex-cli 0.162.1', flush=True)
        sys.exit(0)
    args = sys.argv[1:]
    assert args[-3:] == ['app-server', '--listen', 'stdio://'] and 'exec' not in args
    event('launch', pid=os.getpid(), args=args)
    native = str(uuid.UUID(int=(int(time.time() * 1000) << 80) | (7 << 76) | (secrets.randbits(12) << 64) | (2 << 62) | secrets.randbits(62)))
    turn_id = 'private-native-turn-' + str(uuid.uuid4())
    setup_count = inputs = 0
    for line in sys.stdin:
        message = json.loads(line)
        method = message['method']
        event('request', method=method)
        if method == 'initialize':
            assert message['params']['capabilities']['experimentalApi'] is True
            result(message, dict(userAgent='codex-cli/0.162.1'))
        elif method == 'initialized':
            assert 'id' not in message
        elif method == 'model/list':
            result(message, dict(data=[dict(id=config['model'], displayName='Private named model', isDefault=True)], nextCursor=None))
        elif method == 'account/rateLimits/read':
            result(message, dict(rateLimits=None)) # Observational request served only by this credential-free peer.
        elif method == 'thread/start':
            setup_count += 1
            assert setup_count == 1 and inputs == 0
            params = message['params']
            assert params['cwd'] == config['workspace'] and params['model'] == config['model']
            assert params['approvalPolicy'] == 'on-request' and params['approvalsReviewer'] == 'user'
            assert params['sandbox'] == 'read-only' and params['persistExtendedHistory'] is True
            assert 'threadId' not in params
            run, turn, child = state()
            assert not child.get('native_session_id', '')
            now = int(time.time())
            thread = dict(id=native, sessionId=native, cliVersion='0.162.1', createdAt=now,
                updatedAt=now, cwd=config['workspace'], ephemeral=False, modelProvider='openai',
                preview='', projectId=None, source='appServer', status=dict(type='idle'), turns=[])
            response = dict(thread=thread, model=config['model'], modelProvider='openai',
                cwd=config['workspace'], approvalPolicy='on-request', approvalsReviewer='user',
                sandbox=dict(type='readOnly', networkAccess=False))
            mode = config['evidence']
            if mode == 'missing-model': del response['model']
            elif mode == 'wrong-model': response['model'] = 'contradictory-private-model'
            elif mode == 'missing-sandbox': del response['sandbox']
            elif mode == 'wrong-sandbox': response['sandbox'] = dict(type='workspaceWrite', writableRoots=[config['workspace']], networkAccess=False)
            store('codex-setup-witness.json', dict(pid=os.getpid(), request=message, response=response, native_id=native))
            result(message, response)
        elif method == 'turn/start':
            inputs += 1
            assert inputs == 1 and setup_count == 1
            params = message['params']
            assert params['threadId'] == native and params['model'] == config['model']
            assert params['approvalPolicy'] == 'on-request' and params['approvalsReviewer'] == 'user'
            assert params['sandboxPolicy'] == dict(type='readOnly', networkAccess=False)
            assert params['collaborationMode']['mode'] == 'plan'
            assert params['collaborationMode']['settings']['model'] == config['model']
            text = ''.join(part.get('text', '') for part in params['input'])
            assert text.count(config['task']) == 1
            run, turn, child = state()
            assert child['native_session_id'] == native
            assert turn['expectedTurnSerial'] > 0 and run['expectedTurnSerial'] == turn['expectedTurnSerial']
            index = turn['firstUserMessageIndex']
            assert index >= 0
            row = child['messages'][index]
            assert row['role'] == 'user' and row['content'] == config['task']
            assert row['created_at'] == turn['userMessageCreatedAt'] and row['created_at']
            assert not row.get('acp_prompt_not_sent', False)
            with (root / 'codex-task-inputs.jsonl').open('a') as stream:
                stream.write(json.dumps(message, sort_keys=True) + '\n')
                stream.flush()
                os.fsync(stream.fileno())
            store('codex-input-witness.json', dict(pid=os.getpid(), request=message, run=run, turn=turn, transcript=child, native_id=native))
            result(message, dict(turn=dict(id=turn_id, items=[], status='inProgress', error=None)))
            notice('turn/started', dict(threadId=native, turn=dict(id=turn_id, items=[], status='inProgress', error=None)))
            deadline = time.monotonic() + 45
            while not (root / 'allow-codex-completion').exists():
                assert time.monotonic() < deadline, 'test did not release owned native completion'
                time.sleep(0.005)
            if config['evidence'] in ['matching-collaboration-model', 'missing-collaboration-model']:
                settings = dict(model=config['model'], modelProvider='openai', cwd=config['workspace'],
                    approvalPolicy='on-request', approvalsReviewer='user',
                    sandboxPolicy=dict(type='readOnly', networkAccess=False),
                    collaborationMode=dict(mode='plan', settings=dict(model=config['model'])))
                if config['evidence'] == 'missing-collaboration-model':
                    del settings['collaborationMode']['settings']['model']
                store('codex-settings-witness.json', settings)
                notice('thread/settings/updated', dict(threadId=native, threadSettings=settings))
            notice('item/agentMessage/delta', dict(threadId=native, turnId=turn_id, itemId='private-answer', delta=config['answer']))
            notice('turn/completed', dict(threadId=native, turn=dict(id=turn_id, items=[], status='completed', error=None)))
            store('codex-completed-witness.json', dict(native_id=native, turn_id=turn_id, pid=os.getpid()))
        else:
            raise AssertionError('unexpected native method: ' + method)
    event('eof', pid=os.getpid(), inputs=inputs)
except Exception as error:
    store('codex-peer-error.json', dict(error=str(error)))
    sys.exit(70)
)UAM_CODEX";
	}

	struct ManagedCodexBindingFixtureR109 : ManagedInitialFirstFixture
	{
		const std::string task = "Read the private managed Codex input and report the bounded answer.";
		const std::string answer = "PRIVATE-MANAGED-CODEX-INITIAL-ANSWER";
		std::set<pid_t> owned_pids;
		explicit ManagedCodexBindingFixtureR109(const std::string& evidence = "complete")
		{
			UAM_ASSERT(fs::is_regular_file("/usr/bin/python3"));
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "codex-peer.py", ManagedCodexBindingPeerR109()));
			fs::permissions(temp.root / "codex-peer.py", fs::perms::owner_all, fs::perm_options::replace);
			UAM_ASSERT(uam::io::WriteTextFile(Cli(), "#!/bin/sh\nexec /usr/bin/python3 -I -S " + ShellQuoteForTest((temp.root / "codex-peer.py").string()) + " \"$@\"\n"));
			fs::permissions(Cli(), fs::perms::owner_all, fs::perm_options::replace);
			app.settings.central_provider_configuration.enabled = false;
			app.settings.computer_use_globally_enabled = false;
			app.settings.memory_enabled_default = false;
			app.settings.memory_level_default = "off";
			app.settings.memory_recall_budget_bytes = 0;
			auto* root = ChatDomainService().FindChatById(app, root_id);
			UAM_ASSERT(root && root->metadata_incarnation == root_incarnation);
			root->uam_control_enabled = false;
			root->uam_control_explicitly_disabled = true;
			root->memory_enabled = false;
			root->memory_level = "off";
			root->reasoning_effort.clear();
			root->service_tier.clear();
			root->service_tier_explicit = false;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *root));
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "codex-peer-config.json", nlohmann::json{{"data_root", app.data_root.string()}, {"root_id", root_id}, {"workspace", root->workspace_directory}, {"model", root->model_id}, {"task", task}, {"answer", answer}, {"evidence", evidence}}.dump()));
			AssertPeer();
		}
		void AssertPeer() const
		{
			ExecutableIsPrivate();
			UAM_ASSERT(!fs::is_symlink(Cli()));
			const auto peer = temp.root / "codex-peer.py";
			UAM_ASSERT(fs::is_regular_file(peer) && !fs::is_symlink(peer));
			UAM_ASSERT((fs::status(peer).permissions() & fs::perms::owner_exec) != fs::perms::none);
		}
		void Observe()
		{
			for (const auto& session : app.acp_sessions)
				if (session && session->provider_id == "codex-cli" && session->child_pid > 0) owned_pids.insert(session->child_pid);
		}
		void Pulse()
		{
			AssertPeer();
			Observe();
			(void)uam::PollAllAcpSessions(app);
			(void)uam::AgentRunScheduler::Poll(app);
			(void)uam::FlushPendingChatSaves(app);
			Observe();
			if (fs::exists(temp.root / "codex-peer-error.json")) throw TestFailure("Private Codex prerequisite: " + ReadFile(temp.root / "codex-peer-error.json").substr(0, 512));
		}
		template <typename Predicate> void Await(Predicate done)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!done() && std::chrono::steady_clock::now() < deadline)
			{
				Pulse();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(done());
		}
		AgentRun ReadRun() const
		{
			const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(app.data_root);
			UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1);
			return ledger.runs.front();
		}
		std::size_t Inputs() const
		{
			if (!fs::exists(temp.root / "codex-task-inputs.jsonl")) return 0;
			std::istringstream stream(ReadFile(temp.root / "codex-task-inputs.jsonl"));
			std::string line;
			std::size_t count = 0;
			while (std::getline(stream, line)) if (!line.empty()) { (void)nlohmann::json::parse(line); ++count; }
			return count;
		}
		std::size_t Launches() const
		{
			std::istringstream stream(ReadFile(temp.root / "codex-peer-events.jsonl"));
			std::string line;
			std::size_t count = 0;
			while (std::getline(stream, line)) if (!line.empty() && nlohmann::json::parse(line).value("kind", "") == "launch") ++count;
			return count;
		}
		bool StopAndReap()
		{
			Observe();
			handler.reset();
			uam::FastStopAcpSessionsForExit(app);
			auto& process = PlatformServicesFactory::Instance().process_service;
			for (const auto& session : app.acp_sessions) if (session) { process.StopStdioProcess(*session, true); process.CloseStdioProcessHandles(*session); }
			uam::ResetAsyncCommandTask(app.runtime_cli_version_check_task);
			bool reaped = true;
			for (const pid_t pid : owned_pids)
			{
				int status = 0;
				errno = 0;
				const pid_t waited = waitpid(pid, &status, WNOHANG);
				if (waited == pid) continue;
				if (waited < 0 && errno == ECHILD) { errno = 0; if (kill(pid, 0) < 0 && errno == ESRCH) continue; }
				reaped = false;
			}
			return reaped;
		}
		~ManagedCodexBindingFixtureR109()
		{
			if (!StopAndReap()) std::cerr << "Private Codex fixture could not confirm reaping\n";
		}
	};

	AgentRun ManagedCodexBeginR109(ManagedCodexBindingFixtureR109& fixture)
	{
		struct Completion { std::size_t count = 0; std::optional<uam::ManagedInitialMentionResult> value; };
		const auto completion = std::make_shared<Completion>();
		std::string error;
		const std::string mention = "@" + fixture.definition.id + " " + fixture.task;
		UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(fixture.app, fixture.root_id, mention,
		    [completion](const auto& result) { ++completion->count; completion->value = result; return true; }, &error), uam::ManagedInitialMentionAdmission::Pending);
		fixture.Await([&] { return completion->value.has_value(); });
		const auto& receipt = *completion->value;
		if (!receipt.input_recorded || receipt.delegation_state != uam::ManagedInitialDelegationState::Committed) throw TestFailure("Codex witnessed creation prerequisite: " + receipt.error.substr(0, 512));
		UAM_ASSERT_EQ(completion->count, std::size_t{1});
		UAM_ASSERT(receipt.parent_message && receipt.parent_message->content == mention);
		UAM_ASSERT_EQ(receipt.parent_message_index, fixture.original_messages.size());
		const auto initial = fixture.ReadRun();
		UAM_ASSERT_EQ(receipt.run_id, initial.id);
		UAM_ASSERT_EQ(receipt.transcript_chat_id, initial.transcript_chat_id);
		UAM_ASSERT(initial.managed_continuation && !initial.managed_continuation->execution);
		UAM_ASSERT_EQ(initial.managed_continuation->turns.size(), std::size_t{1});
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(initial.managed_continuation->owner.root_birth_id) && uam::uuid::IsCanonicalUuid(initial.managed_continuation->owner.transcript_birth_id));
		UAM_ASSERT(initial.managed_continuation->owner.root_birth_id != initial.managed_continuation->owner.transcript_birth_id && !initial.managed_continuation->owner.delegation_grant);
		UAM_ASSERT_EQ(initial.managed_continuation->turns.front().principal.kind, uam::ManagedRunPrincipalKind::UserRoot);
		UAM_ASSERT_EQ(initial.provider_id, std::string("codex-cli"));
		UAM_ASSERT_EQ(initial.task, fixture.task);
		return initial;
	}

	AgentRun ManagedCodexFinishR109(ManagedCodexBindingFixtureR109& fixture, const AgentRun& initial)
	{
		fixture.Await([&] { const auto run = fixture.ReadRun(); return run.status != "queued" && run.status != "running" && run.managed_continuation->turns.front().result_delivered && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty(); });
		const auto run = fixture.ReadRun();
		UAM_ASSERT_EQ(run.id, initial.id);
		UAM_ASSERT_EQ(run.root_chat_id, initial.root_chat_id);
		UAM_ASSERT_EQ(run.transcript_chat_id, initial.transcript_chat_id);
		UAM_ASSERT_EQ(run.definition_hash, fixture.definition.definition_hash);
		UAM_ASSERT_EQ(run.definition_snapshot, fixture.definition.markdown_snapshot);
		UAM_ASSERT_EQ(run.definition_instructions, fixture.definition.instructions);
		UAM_ASSERT_EQ(run.effective_workspace_access, std::string("read"));
		UAM_ASSERT_EQ(run.model_id, initial.model_id);
		UAM_ASSERT_EQ(run.managed_continuation->owner, initial.managed_continuation->owner);
		UAM_ASSERT_EQ(run.managed_continuation->turns.size(), std::size_t{1});
		UAM_ASSERT_EQ(run.managed_continuation->current_turn_id, run.id);
		UAM_ASSERT(run.root_result_delivered && run.managed_continuation->turns.front().result_delivered);
		const auto parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, run.transcript_chat_id, 4 * 1024 * 1024);
		UAM_ASSERT(parent && child);
		UAM_ASSERT_EQ(parent->managed_followup_incarnation, run.managed_continuation->owner.root_birth_id);
		UAM_ASSERT_EQ(child->managed_followup_incarnation, run.managed_continuation->owner.transcript_birth_id);
		UAM_ASSERT(parent->messages.size() >= fixture.original_messages.size() + 1 && std::equal(fixture.original_messages.begin(), fixture.original_messages.end(), parent->messages.begin()));
		UAM_ASSERT_EQ(std::ranges::count(parent->messages, "@" + fixture.definition.id + " " + fixture.task, &Message::content), 1);
		std::size_t results = 0;
		for (const auto& row : parent->messages) for (const auto& tool : row.tool_calls) if (tool.id == "uam-agent-result-" + run.id)
		{
			++results;
			UAM_ASSERT(tool.is_sub_agent && tool.sub_agent_id == run.id);
			const auto result = nlohmann::json::parse(tool.result_text);
			UAM_ASSERT_EQ(result.at("runId").get<std::string>(), run.id);
			UAM_ASSERT_EQ(result.at("result").get<std::string>(), run.result_excerpt);
		}
		UAM_ASSERT_EQ(results, std::size_t{1});
		const auto* live = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
		UAM_ASSERT(live && live->metadata_incarnation == fixture.root_incarnation);
		UAM_ASSERT(!ChatDomainService().FindChatById(fixture.app, run.transcript_chat_id));
		UAM_ASSERT(fixture.app.queued_agent_run_ids.empty() && fixture.app.local_acp_launch_tasks.empty());
		UAM_ASSERT_EQ(fixture.Launches(), std::size_t{1});
		const auto setup = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-setup-witness.json"));
		const pid_t pid = setup.at("pid").get<pid_t>();
		errno = 0;
		UAM_ASSERT(pid > 0 && kill(pid, 0) < 0 && errno == ESRCH);
		return run;
	}
}

UAM_TEST(ManagedInitialCodexRawSetupBindsOnlyAfterOwnedCompletionSaveAndReap)
{
	ManagedCodexBindingFixtureR109 fixture;
	const auto initial = ManagedCodexBeginR109(fixture);
	fixture.Await([&] { return fs::exists(fixture.temp.root / "codex-input-witness.json"); });
	const auto setup = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-setup-witness.json"));
	const auto input = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-input-witness.json"));
	const auto& raw = setup.at("response");
	const std::string native = setup.at("native_id").get<std::string>();
	const pid_t pid = input.at("pid").get<pid_t>();
	UAM_ASSERT(uam::uuid::IsCanonicalUuid(native) && pid > 0);
	UAM_ASSERT_EQ(raw.at("thread").at("id").get<std::string>(), native);
	UAM_ASSERT_EQ(raw.at("model").get<std::string>(), initial.model_id);
	UAM_ASSERT_EQ(raw.at("modelProvider").get<std::string>(), std::string("openai"));
	UAM_ASSERT_EQ(raw.at("approvalPolicy"), nlohmann::json("on-request"));
	UAM_ASSERT_EQ(raw.at("approvalsReviewer"), nlohmann::json("user"));
	UAM_ASSERT_EQ(raw.at("sandbox"), (nlohmann::json{{"type", "readOnly"}, {"networkAccess", false}}));
	const auto held = fixture.ReadRun();
	UAM_ASSERT(!held.managed_continuation->execution && held.status == "running");
	const auto* session = uam::FindAcpSessionForChat(fixture.app, initial.transcript_chat_id);
	UAM_ASSERT(session && session->running && session->processing && session->child_pid == pid);
	UAM_ASSERT_EQ(session->session_id, native);
	UAM_ASSERT_EQ(session->turn_serial, held.expected_turn_serial);
	UAM_ASSERT_EQ(held.expected_turn_serial, input.at("turn").at("expectedTurnSerial").get<int>());
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-codex-completion", "release the actual owned initial Codex turn\n"));
	const auto completed = ManagedCodexFinishR109(fixture, initial);
	UAM_ASSERT_EQ(completed.status, std::string("completed"));
	UAM_ASSERT_EQ(completed.managed_continuation->turns.front().status, uam::ManagedRunTurnStatus::Completed);
	UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
	UAM_ASSERT(completed.managed_continuation->turns.front().dispatch_attempted && completed.managed_continuation->turns.front().expected_turn_serial == held.expected_turn_serial);
	UAM_ASSERT(completed.finished_at_epoch_ms >= completed.started_at_epoch_ms && completed.started_at_epoch_ms > 0);
	const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, completed.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(child && child->native_session_id == native && child->model_id == initial.model_id && child->approval_mode == "plan");
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.answer, &Message::content), 1);
	errno = 0;
	UAM_ASSERT(kill(pid, 0) < 0 && errno == ESRCH);
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	const auto state = uam::ManagedSubagentFollowupService::GetState(fixture.app, fixture.root_id, completed.id);
	UAM_ASSERT(fixture.StopAndReap());
	// FIRST feature RED only after genuine setup/input/completed/delivered/saved/reaped prerequisites.
	UAM_ASSERT(completed.managed_continuation->execution.has_value());
	const auto& binding = *completed.managed_continuation->execution;
	UAM_ASSERT_EQ(binding.native_session_id, native);
	UAM_ASSERT_EQ(binding.effective_model_id, initial.model_id);
	UAM_ASSERT_EQ(binding.model_identity_source, std::string("exact-requested"));
	UAM_ASSERT_EQ(binding.execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(binding.workspace_directory, raw.at("cwd").get<std::string>());
	UAM_ASSERT_EQ(binding.approval_mode, std::string("plan"));
	UAM_ASSERT_EQ(binding.routing_mode, std::string("explicit"));
	UAM_ASSERT(!binding.connection_configuration_json.empty() && !binding.provider_configuration_json.empty());
	UAM_ASSERT(state.can_send && state.code == uam::ManagedFollowupCode::None); // The genuine retained binding now enables the separately verified same-child continuation.
}

UAM_TEST(ManagedInitialCodexMissingOrContradictoryRawSetupCannotMintBinding)
{
	for (const std::string mode : {"missing-model", "wrong-model", "missing-sandbox", "wrong-sandbox"})
	{
		ManagedCodexBindingFixtureR109 fixture(mode);
		const auto initial = ManagedCodexBeginR109(fixture);
		fixture.Await([&] { return fs::exists(fixture.temp.root / "codex-setup-witness.json"); });
		const auto raw = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-setup-witness.json")).at("response");
		if (mode == "missing-model") UAM_ASSERT(!raw.contains("model"));
		if (mode == "wrong-model") UAM_ASSERT(raw.at("model").get<std::string>() != initial.model_id);
		if (mode == "missing-sandbox") UAM_ASSERT(!raw.contains("sandbox"));
		if (mode == "wrong-sandbox") UAM_ASSERT_EQ(raw.at("sandbox").at("type").get<std::string>(), std::string("workspaceWrite"));
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-codex-completion", "finish any actually admitted one-shot input without granting binding\n"));
		const auto terminal = ManagedCodexFinishR109(fixture, initial);
		UAM_ASSERT(!terminal.managed_continuation->execution);
		UAM_ASSERT(fixture.Inputs() <= 1); // A new preinput policy refusal is allowed; unsafe proof must never grant a binding.
		if (terminal.status == "completed")
		{
			UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
			UAM_ASSERT_EQ(terminal.result_excerpt, fixture.answer);
		}
		else UAM_ASSERT_EQ(terminal.status, std::string("failed"));
		const auto bytes = ReadFile(fixture.app.data_root / "agent-runs" / (terminal.id + ".json"));
		const auto inputs = fixture.Inputs();
		for (int poll = 0; poll < 100; ++poll) { fixture.Pulse(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
		UAM_ASSERT_EQ(ReadFile(fixture.app.data_root / "agent-runs" / (terminal.id + ".json")), bytes);
		UAM_ASSERT_EQ(fixture.Inputs(), inputs);
		UAM_ASSERT(fixture.StopAndReap());
	}
}

UAM_TEST(ManagedInitialCodexMalformedCollaborationCannotPreserveBinding)
{
	for (const std::string mode : {"matching-collaboration-model", "missing-collaboration-model"})
	{
		ManagedCodexBindingFixtureR109 fixture(mode);
		const auto initial = ManagedCodexBeginR109(fixture);
		fixture.Await([&] { return fs::exists(fixture.temp.root / "codex-input-witness.json"); });
		UAM_ASSERT(!fixture.ReadRun().managed_continuation->execution);
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-codex-completion", "settle the actual owned input after the recorded native settings update\n"));
		const auto completed = ManagedCodexFinishR109(fixture, initial);
		UAM_ASSERT_EQ(completed.status, std::string("completed"));
		UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
		UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
		const auto settings = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-settings-witness.json"));
		const auto& nested = settings.at("collaborationMode").at("settings");
		if (mode == "matching-collaboration-model") UAM_ASSERT_EQ(nested.at("model").get<std::string>(), initial.model_id);
		else UAM_ASSERT(!nested.contains("model"));
		UAM_ASSERT(fixture.StopAndReap());
		UAM_ASSERT_EQ(completed.managed_continuation->execution.has_value(), mode == "matching-collaboration-model");
	}
}
#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI && defined(__APPLE__)
namespace
{
	std::string ManagedCodexResumePeerR113()
	{
		return R"UAM_RESUME(#!/usr/bin/python3
import json, os, pathlib, select, sys, time, uuid
root = pathlib.Path(__file__).resolve().parent
config = json.loads((root / 'codex-peer-config.json').read_text())
def read(name):
    return json.loads((root / name).read_text())
def native_line():
    data = bytearray()
    while True:
        part = os.read(0, 1)
        if not part:
            assert not data, 'truncated native request'
            return b''
        data.extend(part)
        if part == b'\n':
            return bytes(data)
def store(name, value):
    p = root / (name + '.tmp')
    with p.open('w') as f:
        json.dump(value, f, sort_keys=True)
        f.flush()
        os.fsync(f.fileno())
    p.replace(root / name)
def event(kind, **values):
    with (root / 'codex-peer-events.jsonl').open('a') as f:
        f.write(json.dumps(dict(kind=kind, **values), sort_keys=True) + '\n')
        f.flush()
        os.fsync(f.fileno())
def reply(message, value):
    print(json.dumps(dict(id=message['id'], result=value)), flush=True)
def notice(method, params):
    print(json.dumps(dict(method=method, params=params)), flush=True)
try:
    if sys.argv[1:] == ['--version']:
        print('codex-cli 0.162.1', flush=True)
        sys.exit(0)
    assert sys.argv[-3:] == ['app-server', '--listen', 'stdio://']
    first = read('codex-input-witness.json')
    completed = read('codex-completed-witness.json')
    native = completed['native_id']
    assert first['native_id'] == native and first['request']['params']['threadId'] == native
    assert first['request']['params']['input']
    event('launch', pid=os.getpid(), resumed_native=native)
    resumed = inputs = 0
    for line in iter(native_line, b''):
        message = json.loads(line)
        method = message['method']
        event('request', method=method)
        if method == 'initialize':
            assert message['params']['capabilities']['experimentalApi'] is True
            reply(message, dict(userAgent='codex-cli/0.162.1'))
        elif method == 'initialized':
            assert 'id' not in message
        elif method == 'model/list':
            reply(message, dict(data=[dict(id=config['model'], displayName='Private named model', isDefault=True)], nextCursor=None))
        elif method == 'account/rateLimits/read':
            reply(message, dict(rateLimits=None))
        elif method == 'thread/resume':
            resumed += 1
            assert resumed == 1 and inputs == 0
            params = message['params']
            assert params['threadId'] == native and params['model'] == config['model']
            assert params['cwd'] == config['workspace']
            assert params['approvalPolicy'] == 'on-request' and params['approvalsReviewer'] == 'user'
            assert params['sandbox'] == 'read-only'
            response = read('codex-setup-witness.json')['response']
            assert response['thread']['id'] == native
            response['collaborationMode'] = dict(mode='plan', settings=dict(model=config['model'], reasoning_effort=None, developer_instructions=None))
            evidence = config.get('resume_evidence', 'complete')
            if evidence in ['missing-mode', 'live-mode']:
                del response['collaborationMode']
            elif evidence == 'foreign-id':
                response['thread']['id'] = str(uuid.uuid4())
            elif evidence == 'wrong-model':
                response['model'] = 'contradictory-resume-model'
            elif evidence == 'missing-model':
                del response['model']
            elif evidence == 'wrong-policy':
                response['sandbox'] = dict(type='workspaceWrite', writableRoots=[config['workspace']], networkAccess=False)
            elif evidence == 'malformed-mode':
                del response['collaborationMode']['settings']['model']
            elif evidence == 'wrong-mode':
                response['collaborationMode']['mode'] = 'default'
            store('codex-resume-witness.json', dict(pid=os.getpid(), request=message, response=response, initial_input=first['request'], completed=completed, evidence=evidence))
            if evidence in ['refused', 'invalid-thread']:
                error = dict(code=-32000, message='Private native resume refused') if evidence == 'refused' else dict(code=-32600, message='invalid thread id')
                print(json.dumps(dict(id=message['id'], error=error)), flush=True)
            else:
                reply(message, response)
                if evidence == 'live-mode':
                    deadline = time.monotonic() + 45
                    while not (root / 'allow-codex-live-mode').exists():
                        assert time.monotonic() < deadline, 'test did not release authoritative live mode'
                        if select.select([0], [], [], 0.005)[0]:
                            premature = json.loads(native_line())
                            raise AssertionError('native request before authoritative live mode: ' + premature['method'])
                    if select.select([0], [], [], 0)[0]:
                        premature = json.loads(native_line())
                        raise AssertionError('native request queued before authoritative live mode: ' + premature['method'])
                    settings = dict(model=config['model'], modelProvider='openai', cwd=config['workspace'],
                        approvalPolicy='on-request', approvalsReviewer='user',
                        sandboxPolicy=dict(type='readOnly', networkAccess=False),
                        collaborationMode=dict(mode='plan', settings=dict(model=config['model'])))
                    store('codex-resume-live-settings.json', settings)
                    notice('thread/settings/updated', dict(threadId=native, threadSettings=settings))
        elif method == 'turn/start':
            inputs += 1
            assert resumed == inputs == 1
            params = message['params']
            assert params['threadId'] == native and params['model'] == config['model']
            assert params['approvalPolicy'] == 'on-request' and params['approvalsReviewer'] == 'user'
            assert params['sandboxPolicy'] == dict(type='readOnly', networkAccess=False)
            assert params['collaborationMode']['mode'] == 'plan'
            assert params['collaborationMode']['settings']['model'] == config['model']
            text = ''.join(part.get('text', '') for part in params['input'])
            assert text.count(config['followup']) == 1 and config['task'] not in text
            data = pathlib.Path(config['data_root'])
            runs = [json.loads(p.read_text()) for p in (data / 'agent-runs').glob('*.json')]
            runs = [run for run in runs if run['rootChatId'] == config['root_id']]
            assert len(runs) == 1
            run = runs[0]
            managed = run['managedContinuation']
            assert managed['currentTurnNumber'] == 2 and len(managed['turns']) == 2
            turn = managed['turns'][-1]
            assert turn['dispatchAttempted'] and turn['expectedTurnSerial'] > 0
            child = json.loads((data / 'chats' / (run['transcriptChatId'] + '.json')).read_text())
            row = child['messages'][turn['firstUserMessageIndex']]
            assert row['role'] == 'user' and row['content'] == config['followup']
            assert child['native_session_id'] == native and not row.get('acp_prompt_not_sent', False)
            with (root / 'codex-task-inputs.jsonl').open('a') as f:
                f.write(json.dumps(message, sort_keys=True) + '\n')
                f.flush()
                os.fsync(f.fileno())
            store('codex-followup-input-witness.json', dict(pid=os.getpid(), request=message, run=run, child=child))
            turn_id = 'private-resumed-turn-' + str(uuid.uuid4())
            reply(message, dict(turn=dict(id=turn_id, items=[], status='inProgress', error=None)))
            notice('turn/started', dict(threadId=native, turn=dict(id=turn_id, items=[], status='inProgress', error=None)))
            notice('item/agentMessage/delta', dict(threadId=native, turnId=turn_id, itemId='resumed-answer', delta=config['followup_answer']))
            notice('turn/completed', dict(threadId=native, turn=dict(id=turn_id, items=[], status='completed', error=None)))
        else:
            raise AssertionError('unexpected resume method (including forbidden fallback): ' + method)
except Exception as error:
    store('codex-peer-error.json', dict(error=str(error)))
    sys.exit(70)
)UAM_RESUME";
	}
}

void ManagedCodexFollowupPositiveR115(const std::string& evidence)
{
	ManagedCodexBindingFixtureR109 fixture;
	const auto initial = ManagedCodexBeginR109(fixture);
	fixture.Await([&] { return fs::exists(fixture.temp.root / "codex-input-witness.json"); });
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-codex-completion", "complete actual initial input before explicit follow-up\n"));
	const auto predecessor = ManagedCodexFinishR109(fixture, initial);
	UAM_ASSERT_EQ(predecessor.status, std::string("completed"));
	UAM_ASSERT(predecessor.managed_continuation->execution);
	const auto before = *predecessor.managed_continuation;
	const auto parent_file = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id);
	const auto child_file = AppPaths::UamChatFilePath(fixture.app.data_root, predecessor.transcript_chat_id);
	const auto ledger_file = fixture.app.data_root / "agent-runs" / (predecessor.id + ".json");
	const auto parent_before = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	const auto child_before = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, predecessor.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(parent_before && child_before);
	const std::string next_task = "Continue the same private Codex child without replaying the first input.";
	const std::string next_answer = "PRIVATE-MANAGED-CODEX-CONTINUED-ANSWER";
	auto config = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-peer-config.json"));
	config["followup"] = next_task;
	config["resume_evidence"] = evidence;
	config["followup_answer"] = next_answer;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "codex-peer-config.json", config.dump()));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "codex-peer.py", ManagedCodexResumePeerR113()));
	fs::permissions(fixture.temp.root / "codex-peer.py", fs::perms::owner_all, fs::perm_options::replace);
	fixture.AssertPeer();
	uam::ManagedFollowupRequest request;
	request.after_turn_id = before.current_turn_id;
	request.client_request_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	request.text = next_task;
	std::optional<uam::ManagedFollowupSubmitResult> accepted;
	std::size_t callbacks = 0;
	uam::ManagedFollowupCode rejection = uam::ManagedFollowupCode::None;
	const auto admission = uam::ManagedSubagentFollowupService::TrySubmit(fixture.app, fixture.root_id, predecessor.id, request,
	    [&](const auto& result) { ++callbacks; accepted = result; return true; }, &rejection);
	if (admission == uam::ManagedFollowupAdmission::Rejected)
		std::cerr << "Codex genuine same-child follow-up rejected; code=" << static_cast<int>(rejection) << '\n';
	UAM_ASSERT_EQ(admission, uam::ManagedFollowupAdmission::Pending);
	fixture.Await([&] { return accepted.has_value(); });
	UAM_ASSERT_EQ(callbacks, std::size_t{1});
	UAM_ASSERT(accepted->accepted && !accepted->replayed && accepted->acceptance);
	if (evidence == "live-mode")
	{
		fixture.Await([&]
		{
			const auto* session = uam::FindAcpSessionForChat(fixture.app, predecessor.transcript_chat_id);
			return fs::exists(fixture.temp.root / "codex-resume-witness.json") && session && session->running && session->session_setup_request_id == 0;
		});
		for (int poll = 0; poll < 32; ++poll)
		{
			fixture.Pulse();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
		UAM_ASSERT(!fs::exists(fixture.temp.root / "codex-followup-input-witness.json"));
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-codex-live-mode", "publish actual mode after the no-second-input barrier\n"));
	}
	fixture.Await([&]
	{
		const auto run = fixture.ReadRun();
		return run.status == "completed" && run.managed_continuation->turns.size() == 2 &&
		    run.managed_continuation->turns.back().result_delivered && fixture.app.acp_sessions.empty() && fixture.app.managed_initial_runtime_owners.empty();
	});
	const auto completed = fixture.ReadRun();
	UAM_ASSERT_EQ(completed.id, predecessor.id);
	UAM_ASSERT_EQ(completed.transcript_chat_id, predecessor.transcript_chat_id);
	UAM_ASSERT_EQ(completed.managed_continuation->execution, before.execution);
	UAM_ASSERT_EQ(completed.managed_continuation->turns.front(), before.turns.front());
	UAM_ASSERT_EQ(completed.result_excerpt, next_answer);
	const auto& second = completed.managed_continuation->turns.back();
	UAM_ASSERT_EQ(second.turn_id, accepted->acceptance->turn_id);
	UAM_ASSERT_EQ(second.client_request_id, request.client_request_id);
	UAM_ASSERT_EQ(second.after_turn_id, request.after_turn_id);
	UAM_ASSERT_EQ(second.text, request.text);
	UAM_ASSERT_EQ(second.status, uam::ManagedRunTurnStatus::Completed);
	UAM_ASSERT_EQ(accepted->acceptance->turn_number, std::uint32_t{2});
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.Launches(), std::size_t{2});
	const auto resume = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-resume-witness.json"));
	if (evidence == "live-mode")
	{
		UAM_ASSERT(!resume.at("response").contains("collaborationMode"));
		UAM_ASSERT(fs::exists(fixture.temp.root / "codex-resume-live-settings.json"));
	}
	UAM_ASSERT_EQ(resume.at("request").at("params").at("threadId").get<std::string>(), before.execution->native_session_id);
	const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, completed.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(child && child->native_session_id == before.execution->native_session_id);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, next_task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.answer, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, next_answer, &Message::content), 1);
	UAM_ASSERT(child->messages.size() >= child_before->messages.size() && std::equal(child_before->messages.begin(), child_before->messages.end(), child->messages.begin()));
	const auto parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(parent && parent->messages.size() >= parent_before->messages.size() && std::equal(parent_before->messages.begin(), parent_before->messages.end(), parent->messages.begin()));
	const auto result_count = [&](const std::string& delivery, const std::string& answer)
	{
		std::size_t count = 0;
		for (const auto& message : parent->messages) for (const auto& tool : message.tool_calls)
			if (tool.id == "uam-agent-result-" + delivery)
			{
				++count;
				UAM_ASSERT(tool.is_sub_agent && tool.sub_agent_id == predecessor.id);
				const auto result = nlohmann::json::parse(tool.result_text);
				UAM_ASSERT_EQ(result.at("runId").get<std::string>(), predecessor.id);
				UAM_ASSERT_EQ(result.at("result").get<std::string>(), answer);
				if (delivery != predecessor.id)
				{
					UAM_ASSERT_EQ(result.at("turnId").get<std::string>(), second.turn_id);
					UAM_ASSERT_EQ(result.at("turnNumber").get<std::uint32_t>(), std::uint32_t{2});
				}
			}
		return count;
	};
	UAM_ASSERT_EQ(result_count(predecessor.id, fixture.answer), std::size_t{1});
	UAM_ASSERT_EQ(result_count(second.turn_id, next_answer), std::size_t{1});
	const std::string ledger_bytes = ReadFile(ledger_file), parent_bytes = ReadFile(parent_file), child_bytes = ReadFile(child_file);
	const auto ready_again = uam::ManagedSubagentFollowupService::GetState(fixture.app, fixture.root_id, predecessor.id);
	UAM_ASSERT(ready_again.can_send && ready_again.code == uam::ManagedFollowupCode::None);
	UAM_ASSERT_EQ(ready_again.current_turn_id, second.turn_id);
	UAM_ASSERT_EQ(ready_again.current_turn_number, std::uint32_t{2});
	const auto launches = fixture.Launches();
	std::optional<uam::ManagedFollowupSubmitResult> replay;
	std::size_t replay_callbacks = 0;
	UAM_ASSERT_EQ(uam::ManagedSubagentFollowupService::TrySubmit(fixture.app, fixture.root_id, predecessor.id, request,
	    [&](const auto& result) { ++replay_callbacks; replay = result; return true; }), uam::ManagedFollowupAdmission::Pending);
	fixture.Await([&] { return replay.has_value(); });
	UAM_ASSERT(replay->accepted && replay->replayed && replay->acceptance == accepted->acceptance);
	fixture.Await([&] { return fixture.app.managed_followups.empty(); });
	for (int poll = 0; poll < 32; ++poll)
	{
		fixture.Pulse();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT_EQ(replay_callbacks, std::size_t{1});
	UAM_ASSERT_EQ(ReadFile(ledger_file), ledger_bytes);
	UAM_ASSERT_EQ(ReadFile(parent_file), parent_bytes);
	UAM_ASSERT_EQ(ReadFile(child_file), child_bytes);
	UAM_ASSERT_EQ(fixture.ReadRun().managed_continuation, completed.managed_continuation);
	UAM_ASSERT(fixture.app.local_acp_launch_tasks.empty() && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty());
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.Launches(), launches);
	UAM_ASSERT(fixture.StopAndReap());
}

UAM_TEST(ManagedCodexFollowupResumesGenuineCompletedChild)
{
	ManagedCodexFollowupPositiveR115("complete");
}

UAM_TEST(ManagedCodexFollowupWaitsForAuthoritativeLiveMode)
{
	ManagedCodexFollowupPositiveR115("live-mode");
}

UAM_TEST(ManagedCodexFollowupUnverifiedOrRefusedResumeCannotSend)
{
	for (const std::string evidence : {"refused", "invalid-thread", "foreign-id", "missing-mode", "wrong-model", "missing-model", "wrong-policy", "malformed-mode", "wrong-mode"})
	{
		ManagedCodexBindingFixtureR109 fixture;
		const auto initial = ManagedCodexBeginR109(fixture);
		fixture.Await([&] { return fs::exists(fixture.temp.root / "codex-input-witness.json"); });
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-codex-completion", "complete the genuine initial lifecycle before negative resume\n"));
		const auto predecessor = ManagedCodexFinishR109(fixture, initial);
		UAM_ASSERT(predecessor.status == "completed" && predecessor.managed_continuation->execution);
		const auto before = *predecessor.managed_continuation;
		const auto original_child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, predecessor.transcript_chat_id, 4 * 1024 * 1024);
		UAM_ASSERT(original_child);
		const std::string task = "This explicit follow-up must remain unsent without verified native resume.";
		auto config = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-peer-config.json"));
		config["followup"] = task;
		config["followup_answer"] = "UNVERIFIED-RESUME-MUST-NOT-ANSWER";
		config["resume_evidence"] = evidence;
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "codex-peer-config.json", config.dump()));
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "codex-peer.py", ManagedCodexResumePeerR113()));
		fs::permissions(fixture.temp.root / "codex-peer.py", fs::perms::owner_all, fs::perm_options::replace);
		fixture.AssertPeer();
		uam::ManagedFollowupRequest request;
		request.after_turn_id = before.current_turn_id;
		request.client_request_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		request.text = task;
		std::optional<uam::ManagedFollowupSubmitResult> accepted;
		std::size_t callbacks = 0;
		uam::ManagedFollowupCode rejection = uam::ManagedFollowupCode::None;
		const auto admission = uam::ManagedSubagentFollowupService::TrySubmit(fixture.app, fixture.root_id, predecessor.id, request,
		    [&](const auto& result) { ++callbacks; accepted = result; return true; }, &rejection);
		if (admission == uam::ManagedFollowupAdmission::Rejected)
			std::cerr << "Negative Codex resume prerequisite rejected before native observation: " << evidence << " code=" << static_cast<int>(rejection) << '\n';
		UAM_ASSERT_EQ(admission, uam::ManagedFollowupAdmission::Pending);
		fixture.Await([&] { return accepted.has_value(); });
		UAM_ASSERT(callbacks == 1 && accepted->accepted && !accepted->replayed && accepted->acceptance);
		fixture.Await([&] { return fs::exists(fixture.temp.root / "codex-resume-witness.json"); });
		if (evidence == "missing-mode")
		{
			fixture.Await([&]
			{
				const auto* session = uam::FindAcpSessionForChat(fixture.app, predecessor.transcript_chat_id);
				return session && session->running && session->session_setup_request_id == 0;
			});
			for (int poll = 0; poll < 32; ++poll)
			{
				fixture.Pulse();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
			std::string error;
			const bool stopped = uam::AgentRunScheduler::CancelTree(fixture.app, predecessor.id, &error);
			UAM_ASSERT(!stopped && error == "Cancellation is waiting for the provider to stop and the result to be saved."); // Pending Stop is not a terminal acknowledgment.
		}
		fixture.Await([&]
		{
			const auto run = fixture.ReadRun();
			return run.status != "queued" && run.status != "running" && run.managed_continuation->turns.size() == 2 &&
			    run.managed_continuation->turns.back().result_delivered && fixture.app.acp_sessions.empty() && fixture.app.managed_initial_runtime_owners.empty();
		});
		const auto ended = fixture.ReadRun();
		UAM_ASSERT_EQ(ended.status, evidence == "missing-mode" ? std::string("cancelled") : std::string("failed"));
		UAM_ASSERT_EQ(ended.managed_continuation->execution, before.execution);
		UAM_ASSERT_EQ(ended.managed_continuation->turns.front(), before.turns.front());
		UAM_ASSERT_EQ(ended.managed_continuation->turns.back().turn_id, accepted->acceptance->turn_id);
		UAM_ASSERT(ended.managed_continuation->turns.back().dispatch_attempted); // Owned launch attempt; actual input is counted separately below.
		UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
		UAM_ASSERT_EQ(fixture.Launches(), std::size_t{2});
		UAM_ASSERT(!fs::exists(fixture.temp.root / "codex-followup-input-witness.json"));
		const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, predecessor.transcript_chat_id, 4 * 1024 * 1024);
		UAM_ASSERT(child && child->native_session_id == before.execution->native_session_id && child->model_id == predecessor.model_id);
		UAM_ASSERT(child->messages.size() >= original_child->messages.size() && std::equal(original_child->messages.begin(), original_child->messages.end(), child->messages.begin()));
		UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.task, &Message::content), 1);
		UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.answer, &Message::content), 1);
		UAM_ASSERT_EQ(std::ranges::count(child->messages, config.at("followup_answer").get<std::string>(), &Message::content), 0);
		UAM_ASSERT(fixture.StopAndReap());
	}
}
#endif


#if UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_RUNTIME_COPILOT_CLI && defined(__APPLE__)
namespace
{
	std::string ManagedCopilotBindingPeerR124()
	{
		return R"UAM_COPILOT(#!/usr/bin/python3
import json, os, pathlib, select, sys, time, uuid
root = pathlib.Path(__file__).resolve().parent
config = json.loads((root / 'copilot-peer-config.json').read_text())
def read(path): return json.loads(path.read_text())
def store(name, value):
    temporary = root / (name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, sort_keys=True)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(root / name)
def event(kind, **values):
    with (root / 'copilot-peer-events.jsonl').open('a') as stream:
        stream.write(json.dumps(dict(kind=kind, **values), sort_keys=True) + '\n')
        stream.flush()
        os.fsync(stream.fileno())
def emit(value):
    body = json.dumps(value, separators=(',', ':')).encode('utf-8')
    sys.stdout.buffer.write(('Content-Length: '+str(len(body))+'\r\n\r\n').encode('ascii')+body)
    sys.stdout.buffer.flush()
def result(message, value): emit(dict(jsonrpc='2.0', id=message['id'], result=value))
def notice(kind, data, child=False, native_override=None):
    value = dict(id=str(uuid.uuid4()), type=kind, timestamp='2026-10-17T20:00:00.000Z', data=data)
    if child: value['agentId'] = 'actual-private-child-event'
    emit(dict(jsonrpc='2.0', method='session.event', params=dict(sessionId=native_override or native, event=value)))
    return value['id']
def state():
    data = pathlib.Path(config['data_root'])
    runs = [read(path) for path in (data / 'agent-runs').glob('*.json')]
    runs = [run for run in runs if run['rootChatId'] == config['root_id']]
    assert len(runs) == 1 and runs[0]['version'] == 2
    run = runs[0]
    managed = run['managedContinuation']
    assert managed['currentTurnId'] == run['id'] and managed['currentTurnNumber'] == 1
    assert len(managed['turns']) == 1 and managed['execution'] is None
    turn = managed['turns'][0]
    assert run['status'] == turn['status'] == 'running' and turn['dispatchAttempted']
    child = read(data / 'chats' / (run['transcriptChatId'] + '.json'))
    parent = read(data / 'chats' / (run['rootChatId'] + '.json'))
    assert parent['managed_followup_incarnation'] == managed['owner']['rootBirthId']
    assert child['managed_followup_incarnation'] == managed['owner']['transcriptBirthId']
    assert child['agent_run_id'] == run['id']
    return run, turn, child
try:
    if sys.argv[1:] == ['--version']:
        event('version', pid=os.getpid())
        print('GitHub Copilot CLI 1.0.98', flush=True)
        sys.exit(0)
    args = sys.argv[1:]
    assert '--headless' in args and '--stdio' in args and '--no-auto-update' in args
    event('launch', pid=os.getpid(), args=args)
    native = str(uuid.uuid4())
    inputs = setups = 0
    completed = proof_emitted = False
    held_model = None
    proof_queries = 0
    buffer = b''
    def request(message):
        global inputs, setups, held_model, proof_queries
        method = message.get('method','')
        event('request', method=method, id=message.get('id'))
        if method == 'ping': result(message, dict(protocolVersion=3))
        elif method == 'models.list': result(message, dict(models=[dict(id=config['model'], name='Private named model')]))
        elif method == 'session.model.list': result(message, dict(list=[dict(id=config['model'], name='Private named model')]))
        elif method == 'session.model.getCurrent':
            # Auto discovery may request this before the actual input; that response is not proof.
            if not inputs: result(message, dict(modelId=config['model']))
            else:
                proof_queries += 1
                assert proof_queries == 1 and held_model is None
                held_model = message
                store('copilot-query-witness.json', dict(pid=os.getpid(), request=message))
        elif method == 'session.create':
            setups += 1
            assert setups == 1 and inputs == 0
            assert message['params']['workingDirectory'] == config['workspace']
            assert message['params']['model'] == config['model']
            run, turn, child = state()
            assert not child.get('native_session_id','')
            response = dict(sessionId=native)
            store('copilot-setup-witness.json', dict(pid=os.getpid(), request=message, response=response, native_id=native))
            if config['evidence'] == 'unready-mode': notice('session.mode_changed', dict(newMode='plan'))
            result(message, response)
        elif method == 'session.send':
            inputs += 1
            assert inputs == 1
            params = message['params']
            assert params['sessionId'] == native and params['agentMode'] == 'plan' and params['mode'] == 'enqueue'
            assert config['task'] in params['prompt']
            run, turn, child = state()
            assert child['native_session_id'] == native and child['approval_mode'] == 'plan'
            assert turn['expectedTurnSerial'] > 0 and run['expectedTurnSerial'] == turn['expectedTurnSerial']
            index = turn['firstUserMessageIndex']
            assert index >= 0
            row = child['messages'][index]
            assert row['role'] == 'user' and row['content'] == config['task']
            assert row['created_at'] == turn['userMessageCreatedAt'] and row['created_at']
            assert not row.get('acp_prompt_not_sent', False)
            assert len([row for row in child['messages'] if row['role'] == 'user' and row['content'] == config['task']]) == 1
            with (root / 'copilot-task-inputs.jsonl').open('a') as stream:
                stream.write(json.dumps(message, sort_keys=True)+'\n')
                stream.flush()
                os.fsync(stream.fileno())
            store('copilot-input-witness.json', dict(pid=os.getpid(), request=message, turn=turn, native_id=native))
            result(message, dict(messageId=str(uuid.uuid4())))
            notice('assistant.turn_start', {})
            mode = config['evidence']
            if mode not in ('missing-mode', 'unready-mode'):
                notice('session.mode_changed', dict(newMode='interactive' if mode=='wrong-mode' else 'plan'),
                       child=mode=='child-mode', native_override=str(uuid.uuid4()) if mode=='foreign-session' else None)
            fence = notice('session.usage_info', {})
            store('copilot-observation-sent.json', dict(mode=mode, fence=fence))
        elif method == 'session.abort': result(message, {})
        elif method == 'session.destroy': result(message, {})
        # The old mode/model-only peer has no native policy surface. Report an
        # unsupported optional query rather than crashing its truthful one-shot.
        elif method == 'status.get': emit(dict(jsonrpc='2.0', id=message['id'], error=dict(code=-32601, message='Private legacy peer has no native policy status')))
        else: raise RuntimeError('Unexpected private SDK method: '+method)
    while True:
        if not proof_emitted and (root/'allow-copilot-proof').exists():
            mode = config['evidence']
            if held_model is not None:
                message, held_model = held_model, None
                if mode == 'unsupported-model':
                    emit(dict(jsonrpc='2.0', id=message['id'], error=dict(code=-32601, message='Private peer does not support this optional query')))
                else:
                    response = {} if mode == 'missing-model' else dict(modelId='other-private-model' if mode in ('wrong-model','repair-model') else config['model'])
                    result(message, response)
                    if mode == 'repair-model':
                        # A matching duplicate cannot create a new owned observation after contradiction.
                        result(message, dict(modelId=config['model']))
                        notice('session.mode_changed', dict(newMode='plan'))
            proof_emitted = True
            fence = notice('session.usage_info', {})
            store('copilot-proof-replied.json', dict(mode=mode, fence=fence, queries=proof_queries))
        if inputs and not completed and (root/'allow-copilot-completion').exists():
            notice('assistant.message', dict(messageId=str(uuid.uuid4()), content=config['answer']))
            notice('session.idle', {})
            completed = True
            store('copilot-completion-sent.json', dict(pid=os.getpid()))
        readable, _, _ = select.select([sys.stdin.fileno()], [], [], .005)
        if not readable: continue
        data = os.read(sys.stdin.fileno(), 65536)
        if not data: break
        buffer += data
        while b'\r\n\r\n' in buffer:
            end = buffer.index(b'\r\n\r\n')
            assert end <= 8192
            headers = buffer[:end].decode('ascii').split('\r\n')
            lengths = [int(line.split(':',1)[1].strip()) for line in headers if line.lower().startswith('content-length:')]
            assert len(lengths)==1 and 0 < lengths[0] <= 16*1024*1024
            consumed = end+4+lengths[0]
            if len(buffer)<consumed: break
            frame, buffer = buffer[:consumed], buffer[consumed:]
            request(json.loads(frame[end+4:]))
except BaseException as error:
    if not isinstance(error, SystemExit):
        store('copilot-peer-error.json', dict(type=type(error).__name__, error=str(error)))
    raise
)UAM_COPILOT";
	}
	struct ManagedCopilotBindingFixtureR124 : ManagedInitialFirstFixture
	{
		const std::string task = "Read the private managed Copilot input and report the bounded answer.";
		const std::string answer = "PRIVATE-MANAGED-CODEX-INITIAL-ANSWER";
		std::set<pid_t> owned_pids;
		explicit ManagedCopilotBindingFixtureR124(const std::string& evidence = "complete")
		{
			UAM_ASSERT(fs::is_regular_file("/usr/bin/python3"));
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "copilot-peer.py", ManagedCopilotBindingPeerR124()));
			fs::permissions(temp.root / "copilot-peer.py", fs::perms::owner_all, fs::perm_options::replace);
			UAM_ASSERT(uam::io::WriteTextFile(Cli(), "#!/bin/sh\nexec /usr/bin/python3 -I -S " + ShellQuoteForTest((temp.root / "copilot-peer.py").string()) + " \"$@\"\n"));
			fs::permissions(Cli(), fs::perms::owner_all, fs::perm_options::replace);
			app.provider_profiles = {ProviderProfileStore::DefaultCopilotProfile()};
			app.provider_profiles.front().interactive_command = ShellQuoteForTest(Cli().string());
			app.settings.central_provider_configuration.enabled = false;
			app.settings.computer_use_globally_enabled = false;
			app.settings.memory_enabled_default = false;
			app.settings.memory_level_default = "off";
			app.settings.memory_recall_budget_bytes = 0;
			auto* root = ChatDomainService().FindChatById(app, root_id);
			UAM_ASSERT(root && root->metadata_incarnation == root_incarnation);
			root->provider_id = "copilot-cli";
			root->uam_control_enabled = false;
			root->uam_control_explicitly_disabled = true;
			root->memory_enabled = false;
			root->memory_level = "off";
			root->reasoning_effort.clear();
			root->service_tier.clear();
			root->service_tier_explicit = false;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *root));
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "copilot-peer-config.json", nlohmann::json{{"data_root", app.data_root.string()}, {"root_id", root_id}, {"workspace", root->workspace_directory}, {"model", root->model_id}, {"task", task}, {"answer", answer}, {"evidence", evidence}}.dump()));
			AssertPeer();
		}
		fs::path Cli() const { return temp.root / "bin" / "copilot"; }
		void AssertPeer() const
		{
			UAM_ASSERT(fs::is_regular_file(Cli()) && !fs::is_symlink(Cli()));
			UAM_ASSERT((fs::status(Cli()).permissions() & fs::perms::owner_exec) != fs::perms::none);
			UAM_ASSERT_EQ(std::string(std::getenv("PATH")), (temp.root / "bin").string());
			UAM_ASSERT(!fs::is_symlink(Cli()));
			const auto peer = temp.root / "copilot-peer.py";
			UAM_ASSERT(fs::is_regular_file(peer) && !fs::is_symlink(peer));
			UAM_ASSERT((fs::status(peer).permissions() & fs::perms::owner_exec) != fs::perms::none);
		}
		void Observe()
		{
			for (const auto& session : app.acp_sessions)
				if (session && session->provider_id == "copilot-cli" && session->child_pid > 0) owned_pids.insert(session->child_pid);
		}
		void Pulse()
		{
			AssertPeer();
			Observe();
			(void)uam::PollAllAcpSessions(app);
			(void)uam::AgentRunScheduler::Poll(app);
			(void)uam::FlushPendingChatSaves(app);
			Observe();
			if (fs::exists(temp.root / "copilot-peer-error.json")) throw TestFailure("Private Copilot prerequisite: " + ReadFile(temp.root / "copilot-peer-error.json").substr(0, 512));
		}
		template <typename Predicate> void Await(Predicate done)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!done() && std::chrono::steady_clock::now() < deadline)
			{
				Pulse();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(done());
		}
		void AwaitFence(const char* filename)
		{
			Await([&] { return fs::exists(temp.root / filename); });
			const auto fence = nlohmann::json::parse(ReadFile(temp.root / filename)).at("fence").get<std::string>();
			UAM_ASSERT(!fence.empty());
			Await([&] { const auto run = ReadRun(); const auto* session = uam::FindAcpSessionForChat(app, run.transcript_chat_id); return session && session->copilot_native_state.contains("events") && session->copilot_native_state.at("events").contains(fence); });
		}
		void FlushPeerRequests()
		{
			const auto run = ReadRun();
			auto* session = uam::FindAcpSessionForChat(app, run.transcript_chat_id);
			UAM_ASSERT(session && session->running && session->processing);
			const int id = uam::acp_detail::NextAcpRequestId(*session, "ping");
			UAM_ASSERT(uam::acp_detail::WriteAcpMessage(*session, {{"jsonrpc", "2.0"}, {"id", id}, {"method", "ping"}, {"params", nlohmann::json::object()}}));
			Await([&] { const auto* current = uam::FindAcpSessionForChat(app, run.transcript_chat_id); return current && !current->pending_request_methods.contains(id); });
		}
		AgentRun ReadRun() const
		{
			const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(app.data_root);
			UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1);
			return ledger.runs.front();
		}
		std::size_t Inputs() const
		{
			if (!fs::exists(temp.root / "copilot-task-inputs.jsonl")) return 0;
			std::istringstream stream(ReadFile(temp.root / "copilot-task-inputs.jsonl"));
			std::string line;
			std::size_t count = 0;
			while (std::getline(stream, line)) if (!line.empty()) { (void)nlohmann::json::parse(line); ++count; }
			return count;
		}
		std::size_t Launches() const
		{
			std::istringstream stream(ReadFile(temp.root / "copilot-peer-events.jsonl"));
			std::string line;
			std::size_t count = 0;
			while (std::getline(stream, line)) if (!line.empty() && nlohmann::json::parse(line).value("kind", "") == "launch") ++count;
			return count;
		}
		bool StopAndReap()
		{
			Observe();
			handler.reset();
			uam::FastStopAcpSessionsForExit(app);
			auto& process = PlatformServicesFactory::Instance().process_service;
			for (const auto& session : app.acp_sessions) if (session) { process.StopStdioProcess(*session, true); process.CloseStdioProcessHandles(*session); }
			uam::ResetAsyncCommandTask(app.runtime_cli_version_check_task);
			bool reaped = true;
			for (const pid_t pid : owned_pids)
			{
				int status = 0;
				errno = 0;
				const pid_t waited = waitpid(pid, &status, WNOHANG);
				if (waited == pid) continue;
				if (waited < 0 && errno == ECHILD) { errno = 0; if (kill(pid, 0) < 0 && errno == ESRCH) continue; }
				reaped = false;
			}
			return reaped;
		}
		~ManagedCopilotBindingFixtureR124()
		{
			if (!StopAndReap()) std::cerr << "Private Copilot fixture could not confirm reaping\n";
		}
	};

	AgentRun ManagedCopilotBeginR124(ManagedCopilotBindingFixtureR124& fixture)
	{
		struct Completion { std::size_t count = 0; std::optional<uam::ManagedInitialMentionResult> value; };
		const auto completion = std::make_shared<Completion>();
		std::string error;
		const std::string mention = "@" + fixture.definition.id + " " + fixture.task;
		UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(fixture.app, fixture.root_id, mention,
		    [completion](const auto& result) { ++completion->count; completion->value = result; return true; }, &error), uam::ManagedInitialMentionAdmission::Pending);
		fixture.Await([&] { return completion->value.has_value(); });
		const auto& receipt = *completion->value;
		if (!receipt.input_recorded || receipt.delegation_state != uam::ManagedInitialDelegationState::Committed) throw TestFailure("Copilot witnessed creation prerequisite: " + receipt.error.substr(0, 512));
		UAM_ASSERT_EQ(completion->count, std::size_t{1});
		UAM_ASSERT(receipt.parent_message && receipt.parent_message->content == mention);
		UAM_ASSERT_EQ(receipt.parent_message_index, fixture.original_messages.size());
		const auto initial = fixture.ReadRun();
		UAM_ASSERT_EQ(receipt.run_id, initial.id);
		UAM_ASSERT_EQ(receipt.transcript_chat_id, initial.transcript_chat_id);
		UAM_ASSERT(initial.managed_continuation && !initial.managed_continuation->execution);
		UAM_ASSERT_EQ(initial.managed_continuation->turns.size(), std::size_t{1});
		UAM_ASSERT(uam::uuid::IsCanonicalUuid(initial.managed_continuation->owner.root_birth_id) && uam::uuid::IsCanonicalUuid(initial.managed_continuation->owner.transcript_birth_id));
		UAM_ASSERT(initial.managed_continuation->owner.root_birth_id != initial.managed_continuation->owner.transcript_birth_id && !initial.managed_continuation->owner.delegation_grant);
		UAM_ASSERT_EQ(initial.managed_continuation->turns.front().principal.kind, uam::ManagedRunPrincipalKind::UserRoot);
		UAM_ASSERT_EQ(initial.provider_id, std::string("copilot-cli"));
		UAM_ASSERT_EQ(initial.task, fixture.task);
		return initial;
	}

	AgentRun ManagedCopilotFinishR124(ManagedCopilotBindingFixtureR124& fixture, const AgentRun& initial)
	{
		fixture.Await([&] { const auto run = fixture.ReadRun(); return run.status != "queued" && run.status != "running" && run.managed_continuation->turns.front().result_delivered && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty(); });
		const auto run = fixture.ReadRun();
		UAM_ASSERT_EQ(run.id, initial.id);
		UAM_ASSERT_EQ(run.root_chat_id, initial.root_chat_id);
		UAM_ASSERT_EQ(run.transcript_chat_id, initial.transcript_chat_id);
		UAM_ASSERT_EQ(run.definition_hash, fixture.definition.definition_hash);
		UAM_ASSERT_EQ(run.definition_snapshot, fixture.definition.markdown_snapshot);
		UAM_ASSERT_EQ(run.definition_instructions, fixture.definition.instructions);
		UAM_ASSERT_EQ(run.effective_workspace_access, std::string("read"));
		UAM_ASSERT_EQ(run.model_id, initial.model_id);
		UAM_ASSERT_EQ(run.managed_continuation->owner, initial.managed_continuation->owner);
		UAM_ASSERT_EQ(run.managed_continuation->turns.size(), std::size_t{1});
		UAM_ASSERT_EQ(run.managed_continuation->current_turn_id, run.id);
		UAM_ASSERT(run.root_result_delivered && run.managed_continuation->turns.front().result_delivered);
		const auto parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, run.transcript_chat_id, 4 * 1024 * 1024);
		UAM_ASSERT(parent && child);
		UAM_ASSERT_EQ(parent->managed_followup_incarnation, run.managed_continuation->owner.root_birth_id);
		UAM_ASSERT_EQ(child->managed_followup_incarnation, run.managed_continuation->owner.transcript_birth_id);
		UAM_ASSERT(parent->messages.size() >= fixture.original_messages.size() + 1 && std::equal(fixture.original_messages.begin(), fixture.original_messages.end(), parent->messages.begin()));
		UAM_ASSERT_EQ(std::ranges::count(parent->messages, "@" + fixture.definition.id + " " + fixture.task, &Message::content), 1);
		std::size_t results = 0;
		for (const auto& row : parent->messages) for (const auto& tool : row.tool_calls) if (tool.id == "uam-agent-result-" + run.id)
		{
			++results;
			UAM_ASSERT(tool.is_sub_agent && tool.sub_agent_id == run.id);
			const auto result = nlohmann::json::parse(tool.result_text);
			UAM_ASSERT_EQ(result.at("runId").get<std::string>(), run.id);
			UAM_ASSERT_EQ(result.at("result").get<std::string>(), run.result_excerpt);
		}
		UAM_ASSERT_EQ(results, std::size_t{1});
		const auto* live = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
		UAM_ASSERT(live && live->metadata_incarnation == fixture.root_incarnation);
		UAM_ASSERT(!ChatDomainService().FindChatById(fixture.app, run.transcript_chat_id));
		UAM_ASSERT(fixture.app.queued_agent_run_ids.empty() && fixture.app.local_acp_launch_tasks.empty());
		UAM_ASSERT_EQ(fixture.Launches(), std::size_t{1});
		const auto setup = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-setup-witness.json"));
		const pid_t pid = setup.at("pid").get<pid_t>();
		errno = 0;
		UAM_ASSERT(pid > 0 && kill(pid, 0) < 0 && errno == ESRCH);
		return run;
	}
}

UAM_TEST(ManagedInitialCopilotModeAndModelHintsWithoutPolicyRemainUnbound)
{
	ManagedCopilotBindingFixtureR124 fixture;
	const auto initial = ManagedCopilotBeginR124(fixture);
	fixture.AwaitFence("copilot-observation-sent.json");
	fixture.FlushPeerRequests();
	const auto setup = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-setup-witness.json"));
	const auto input = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-input-witness.json"));
	const auto native = setup.at("native_id").get<std::string>();
	UAM_ASSERT(uam::uuid::IsCanonicalUuid(native));
	UAM_ASSERT_EQ(setup.at("response").at("sessionId"), nlohmann::json(native));
	UAM_ASSERT_EQ(input.at("request").at("params").at("agentMode"), nlohmann::json("plan"));
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	UAM_ASSERT(!fixture.ReadRun().managed_continuation->execution);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-proof", "release optional native model observation\n"));
	// FIFO ping and accepted-event fences do not require the new query on old source.
	// Its genuine one-shot still completes before the final feature assertion.
	fixture.AwaitFence("copilot-proof-replied.json");
	UAM_ASSERT(!fixture.ReadRun().managed_continuation->execution);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-completion", "complete actual initial Copilot turn\n"));
	const auto completed = ManagedCopilotFinishR124(fixture, initial);
	UAM_ASSERT_EQ(completed.status, std::string("completed"));
	UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	UAM_ASSERT(fixture.StopAndReap());
	const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, completed.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(child && child->native_session_id == native && child->model_id == initial.model_id && child->approval_mode == "plan");
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.answer, &Message::content), 1);
	// Matching mode/model hints do not establish an enforced native read ceiling.
	// Preserve the successful one-shot without manufacturing continuation authority.
	UAM_ASSERT(!completed.managed_continuation->execution);
	UAM_ASSERT_EQ(completed.managed_continuation->execution, initial.managed_continuation->execution);
	UAM_ASSERT(fs::exists(fixture.temp.root / "copilot-proof-replied.json"));
	const auto state = uam::ManagedSubagentFollowupService::GetState(fixture.app, fixture.root_id, completed.id);
	UAM_ASSERT(!state.can_send);
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
}

UAM_TEST(ManagedInitialCopilotMissingOrContradictoryObservationsRemainTruthfulOneShot)
{
	for (const std::string mode : {"missing-mode", "unready-mode", "child-mode", "foreign-session", "wrong-mode", "missing-model", "wrong-model", "unsupported-model", "repair-model"})
	{
		ManagedCopilotBindingFixtureR124 fixture(mode);
		const auto initial = ManagedCopilotBeginR124(fixture);
		fixture.AwaitFence("copilot-observation-sent.json");
		fixture.FlushPeerRequests();
		UAM_ASSERT(!fixture.ReadRun().managed_continuation->execution);
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-proof", "release optional proof without supplying absent facts\n"));
		fixture.AwaitFence("copilot-proof-replied.json");
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-completion", "finish truthful one-shot without granting continuation\n"));
		const auto completed = ManagedCopilotFinishR124(fixture, initial);
		UAM_ASSERT_EQ(completed.status, std::string("completed"));
		UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
		UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
		UAM_ASSERT(!completed.managed_continuation->execution);
		const auto state = uam::ManagedSubagentFollowupService::GetState(fixture.app, fixture.root_id, completed.id);
		UAM_ASSERT(!state.can_send);
		UAM_ASSERT(fixture.StopAndReap());
	}
}
#endif


#if UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_RUNTIME_COPILOT_CLI && defined(__APPLE__)
namespace
{
	std::string ManagedCopilotSupportedReadPolicyPeer(bool record_status_order = false)
	{
		// Separate constructed private protocol fixture. Preserve the original R124 peer
		// and its mode/model-only negatives; no installed provider or saved account is read.
		std::string peer = ManagedCopilotBindingPeerR124();
		const auto replace_once = [&](const std::string& before, const std::string& after)
		{
			const auto index = peer.find(before);
			UAM_ASSERT(index != std::string::npos && peer.find(before, index + before.size()) == std::string::npos);
			peer.replace(index, before.size(), after);
		};
		replace_once("    held_model = None\n    proof_queries = 0\n", "    held_model = None\n    held_policy = []\n    proof_queries = 0\n");
		replace_once(R"UAM_BEFORE(        elif method == 'session.model.getCurrent':
            # Auto discovery may request this before the actual input; that response is not proof.
            if not inputs: result(message, dict(modelId=config['model']))
            else:
                proof_queries += 1
                assert proof_queries == 1 and held_model is None
                held_model = message
                store('copilot-query-witness.json', dict(pid=os.getpid(), request=message))
)UAM_BEFORE", R"UAM_AFTER(        elif method == 'status.get':
            response = dict(protocolVersion=3, version='1.0.98')
            store('copilot-policy-status-witness.json', dict(pid=os.getpid(), request=message, response=response))
            result(message, response)
        elif method in ('session.model.getCurrent', 'session.mode.get', 'session.permissions.getMode',
                        'session.managedSettings.get', 'session.metadata.snapshot',
                        'session.tools.initializeAndValidate', 'session.tools.getCurrentMetadata'):
            assert setups == 1 and message['params']['sessionId'] == native
            if method == 'session.model.getCurrent':
                proof_queries += 1
                assert proof_queries <= 16
                response = dict(modelId=config['model'])
                store('copilot-query-witness.json', dict(pid=os.getpid(), request=message))
            elif method == 'session.mode.get': response = 'plan' if inputs else 'interactive'
            elif method == 'session.permissions.getMode': response = dict(mode='manual')
            elif method == 'session.managedSettings.get':
                # Public schema/provenance fixture, not a claimed captured CLI serialization.
                # Do not invent settings.permissions.deny readback or decode the opaque context.
                response = dict(bypassPermissionsDisabled=True, clientManaged=True, deviceManaged=False,
                                serverManaged=False, failClosed=False, managedKeys=['permissions'], source='client',
                                settings=dict(privateFixtureOpaqueSettings=['retain', 'verbatim']),
                                permissionsContext=dict(failClosed=False,
                                    permissions=dict(privateFixtureOpaqueContext=['retain', 'verbatim'])))
            elif method == 'session.metadata.snapshot':
                response = dict(sessionId=native, startTime='2026-10-17T20:00:00.000Z',
                                modifiedTime='2026-10-17T20:00:00.000Z', isRemote=False, alreadyInUse=False,
                                workspacePath=str(root), workingDirectory=config['workspace'],
                                currentMode='plan' if inputs else 'interactive', selectedModel=config['model'],
                                sessionLimits=None)
            elif method == 'session.tools.initializeAndValidate': response = {}
            else:
                response = dict(tools=[dict(name=name, description='Private read-only protocol fixture '+name)
                                       for name in ('view', 'grep', 'glob')])
            observation = dict(pid=os.getpid(), request=message, response=response, inputs=inputs)
            with (root/'copilot-policy-native-observations.jsonl').open('a') as stream:
                stream.write(json.dumps(observation, sort_keys=True)+'\n')
                stream.flush()
                os.fsync(stream.fileno())
            if inputs and not (root/'allow-copilot-proof').exists(): held_policy.append((message, response))
            else: result(message, response)
)UAM_AFTER");
		replace_once("        if not proof_emitted and (root/'allow-copilot-proof').exists():\n            mode = config['evidence']\n", "        if not proof_emitted and (root/'allow-copilot-proof').exists():\n            for message, response in held_policy: result(message, response)\n            held_policy.clear()\n            mode = config['evidence']\n");
		if (record_status_order)
		{
			// Record compatibility proof timing without preventing the legacy initial
			// one-shot prerequisites from completing when the new status query is absent.
			replace_once("    inputs = setups = 0\n", "    inputs = setups = 0\n    status_seen = False\n");
			replace_once("        global inputs, setups, held_model, proof_queries\n", "        global inputs, setups, held_model, proof_queries, status_seen\n");
			replace_once("            store('copilot-policy-status-witness.json', dict(pid=os.getpid(), request=message, response=response))\n            result(message, response)\n",
			    "            store('copilot-policy-status-witness.json', dict(pid=os.getpid(), request=message, response=response))\n            result(message, response)\n            status_seen = True\n");
			replace_once("            store('copilot-setup-witness.json', dict(pid=os.getpid(), request=message, response=response, native_id=native))\n",
			    "            store('copilot-setup-witness.json', dict(pid=os.getpid(), request=message, response=response, native_id=native, status_seen=status_seen))\n");
			replace_once("            store('copilot-input-witness.json', dict(pid=os.getpid(), request=message, turn=turn, native_id=native))\n",
			    "            store('copilot-input-witness.json', dict(pid=os.getpid(), request=message, turn=turn, native_id=native, status_seen=status_seen))\n");
		}
		return peer;
	}

	std::vector<nlohmann::json> CopilotSupportedReadPolicyObservations(const fs::path& root)
	{
		std::vector<nlohmann::json> observations;
		const auto path = root / "copilot-policy-native-observations.jsonl";
		if (!fs::exists(path)) return observations;
		std::istringstream stream(ReadFile(path));
		std::string line;
		while (std::getline(stream, line)) if (!line.empty()) observations.push_back(nlohmann::json::parse(line));
		return observations;
	}
}

namespace
{
AgentRun CompleteManagedCopilotSupportedReadPolicyInitial(ManagedCopilotBindingFixtureR124& fixture, bool omit_optional_policy_json = false)
{
	std::string peer = ManagedCopilotSupportedReadPolicyPeer(true);
	if (omit_optional_policy_json)
	{
		// Both payloads are optional public-schema JSON values, not adoption tokens.
		const std::string anchor = "            observation = dict(pid=os.getpid(), request=message, response=response, inputs=inputs)\n";
		const auto at = peer.find(anchor);
		UAM_ASSERT(at != std::string::npos && peer.find(anchor, at + anchor.size()) == std::string::npos);
		peer.insert(at, "            if method == 'session.managedSettings.get':\n                response.pop('settings', None)\n                response.pop('permissionsContext', None)\n");
	}
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "copilot-peer.py", peer));
	fs::permissions(fixture.temp.root / "copilot-peer.py", fs::perms::owner_all, fs::perm_options::replace);
	fixture.AssertPeer();
	const auto initial = ManagedCopilotBeginR124(fixture);
	fixture.AwaitFence("copilot-observation-sent.json");
	fixture.FlushPeerRequests();
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	UAM_ASSERT(!fixture.ReadRun().managed_continuation->execution);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-proof", "Release constructed native observations for the actual owned initial turn.\n"));
	fixture.AwaitFence("copilot-proof-replied.json");
	fixture.FlushPeerRequests();
	UAM_ASSERT(!fixture.ReadRun().managed_continuation->execution);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-completion", "Complete the actual initial turn before any policy feature assertion.\n"));
	const auto completed = ManagedCopilotFinishR124(fixture, initial);
	UAM_ASSERT_EQ(completed.status, std::string("completed"));
	UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	UAM_ASSERT(fixture.StopAndReap());
	const auto setup = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-setup-witness.json"));
	const auto native = setup.at("native_id").get<std::string>();
	const auto pid = setup.at("pid");
	const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, completed.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(child && child->native_session_id == native && child->model_id == initial.model_id && child->approval_mode == "plan");
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.answer, &Message::content), 1);

	// First feature RED is after all genuine public creation/input/completion/save/reap
	// prerequisites. The native request, not a copied ledger binding, must carry the ceiling.
	const auto& params = setup.at("request").at("params");
	UAM_ASSERT_EQ(params.value("availableTools", nlohmann::json()), nlohmann::json::array({"builtin:view", "builtin:grep", "builtin:glob"}));
	UAM_ASSERT_EQ(params.value("excludedTools", nlohmann::json()), nlohmann::json::array({"mcp:*", "custom:*"}));
	UAM_ASSERT_EQ(params.value("toolFilterPrecedence", std::string{}), std::string("excluded"));
	UAM_ASSERT(params.contains("managedSettings") && params.at("managedSettings").is_object());
	const auto& permissions = params.at("managedSettings").at("permissions");
	UAM_ASSERT_EQ(permissions.at("disableBypassPermissionsMode"), nlohmann::json("disable"));
	UAM_ASSERT_EQ(permissions.at("deny"), nlohmann::json::array({"shell", "write"}));
	const auto input = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-input-witness.json"));
	UAM_ASSERT_EQ(setup.at("status_seen"), nlohmann::json(true));
	UAM_ASSERT_EQ(input.at("status_seen"), nlohmann::json(true));
	UAM_ASSERT_EQ(input.at("pid"), pid);
	UAM_ASSERT_EQ(input.at("native_id"), nlohmann::json(native));
	const auto status = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-policy-status-witness.json"));
	UAM_ASSERT_EQ(status.at("pid"), pid);
	UAM_ASSERT_EQ(status.at("request").at("method"), nlohmann::json("status.get"));
	UAM_ASSERT_EQ(status.at("response"), (nlohmann::json{{"protocolVersion", 3}, {"version", "1.0.98"}}));
	const auto observations = CopilotSupportedReadPolicyObservations(fixture.temp.root);
	for (const std::string method : {"session.managedSettings.get", "session.permissions.getMode", "session.tools.initializeAndValidate", "session.tools.getCurrentMetadata"})
	{
		UAM_ASSERT(std::ranges::any_of(observations, [&](const auto& observation)
		{
			return observation.at("pid") == pid && observation.at("request").at("method") == method && observation.at("request").at("params").at("sessionId") == native;
		}));
	}
	UAM_ASSERT(std::ranges::any_of(observations, [&](const auto& observation)
	{
		return observation.at("pid") == pid && observation.at("inputs") == 1 && observation.at("request").at("method") == "session.metadata.snapshot" && observation.at("request").at("params").at("sessionId") == native && observation.at("response").at("sessionId") == native && observation.at("response").at("currentMode") == "plan" && observation.at("response").at("selectedModel") == initial.model_id && observation.at("response").at("workingDirectory") == child->workspace_directory;
	}));
	UAM_ASSERT(completed.managed_continuation->execution.has_value());
	const auto& binding = *completed.managed_continuation->execution;
	UAM_ASSERT_EQ(binding.native_session_id, native);
	UAM_ASSERT_EQ(binding.effective_model_id, initial.model_id);
	UAM_ASSERT_EQ(binding.workspace_directory, child->workspace_directory);
	UAM_ASSERT_EQ(binding.execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(binding.approval_mode, std::string("plan"));
	return completed;
}
}

UAM_TEST(ManagedInitialCopilotSupportedNativeReadPolicyBindsOnlyAfterOwnedCompletionSaveAndReap)
{
	for (const bool omit_optional_policy_json : {false, true})
	{
		ManagedCopilotBindingFixtureR124 fixture;
		(void)CompleteManagedCopilotSupportedReadPolicyInitial(fixture, omit_optional_policy_json);
	}
}
#endif


#if UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_RUNTIME_COPILOT_CLI && defined(__APPLE__)
namespace
{
	std::string ManagedCopilotSupportedReadResumePeer()
	{
		// Separate second process; native identity comes only from the genuine initial
		// setup/input witnesses. No ledger record, birth, binding or receipt is seeded.
		std::string peer = ManagedCopilotSupportedReadPolicyPeer();
		const auto replace_once = [&](const std::string& before, const std::string& after)
		{
			const auto index = peer.find(before);
			UAM_ASSERT(index != std::string::npos && peer.find(before, index + before.size()) == std::string::npos);
			peer.replace(index, before.size(), after);
		};
		replace_once(R"RESUME_BEFORE(    assert managed['currentTurnId'] == run['id'] and managed['currentTurnNumber'] == 1
    assert len(managed['turns']) == 1 and managed['execution'] is None
    turn = managed['turns'][0]
)RESUME_BEFORE", R"RESUME_AFTER(    assert managed['currentTurnNumber'] == 2 and len(managed['turns']) == 2
    turn = managed['turns'][-1]
    assert managed['currentTurnId'] == turn['turnId'] and turn['turnNumber'] == 2
    assert managed['execution']['nativeSessionId'] == native
    assert managed['turns'][0]['status'] == 'completed' and managed['turns'][0]['resultDelivered']
    assert turn['text'] == config['task'] and turn['afterTurnId'] == managed['turns'][0]['turnId']
)RESUME_AFTER");
		replace_once("    native = str(uuid.uuid4())\n", R"RESUME_ID(    native = read(root/'copilot-setup-witness.json')['native_id']
    assert read(root/'copilot-input-witness.json')['native_id'] == native
    assert read(root/'copilot-input-witness.json')['request']['params']['sessionId'] == native
)RESUME_ID");
		replace_once("    held_policy = []\n", R"RESUME_STATE(    held_policy = []
    held_tools = []
    delivered = set()
    status_seen = False
    has_policy = False
    def proof_reply(message, response):
        result(message, response)
        delivered.add(message['method'])
        store('copilot-resume-proof-state.json', dict(pid=os.getpid(), sessionId=native, delivered=sorted(delivered)))
        event('proof_reply', pid=os.getpid(), method=message['method'], id=message['id'], sessionId=native)
)RESUME_STATE");
		replace_once("        global inputs, setups, held_model, proof_queries\n", "        global inputs, setups, held_model, proof_queries, status_seen, has_policy\n");
		replace_once("        elif method == 'status.get':\n            response = dict(protocolVersion=3, version='1.0.98')\n", "        elif method == 'status.get':\n            status_seen = True\n            response = dict(protocolVersion=3, version='1.0.98')\n");
		replace_once("            elif method == 'session.mode.get': response = 'plan' if inputs else 'interactive'\n", "            elif method == 'session.mode.get': response = 'plan'\n");
		replace_once("                                currentMode='plan' if inputs else 'interactive', selectedModel=config['model'],\n", "                                currentMode='plan', selectedModel=config['model'],\n");
		replace_once("            elif method == 'session.metadata.snapshot':\n", R"RESUME_POLICY(                if not has_policy:
                    response = dict(bypassPermissionsDisabled=False, clientManaged=False, deviceManaged=False,
                                    serverManaged=False, failClosed=False, managedKeys=[], source='none')
            elif method == 'session.metadata.snapshot':
)RESUME_POLICY");
		replace_once(R"RESUME_HOLD(            if inputs and not (root/'allow-copilot-proof').exists(): held_policy.append((message, response))
            else: result(message, response)
)RESUME_HOLD", R"RESUME_RELEASE(            if method == 'session.tools.getCurrentMetadata' and not (root/'allow-copilot-resume-tools').exists():
                held_tools.append((message, response))
                store('copilot-resume-tools-held.json', dict(pid=os.getpid(), request=message, response=response))
            elif not (root/'allow-copilot-resume-proofs').exists(): held_policy.append((message, response))
            else: proof_reply(message, response)
)RESUME_RELEASE");
		replace_once(R"RESUME_SETUP(        elif method == 'session.create':
            setups += 1
            assert setups == 1 and inputs == 0
            assert message['params']['workingDirectory'] == config['workspace']
            assert message['params']['model'] == config['model']
            run, turn, child = state()
            assert not child.get('native_session_id','')
            response = dict(sessionId=native)
            store('copilot-setup-witness.json', dict(pid=os.getpid(), request=message, response=response, native_id=native))
            if config['evidence'] == 'unready-mode': notice('session.mode_changed', dict(newMode='plan'))
            result(message, response)
)RESUME_SETUP", R"RESUME_NATIVE(        elif method == 'session.create': raise RuntimeError('A genuine managed resume must not create a different conversation')
        elif method == 'session.resume':
            setups += 1
            assert setups == 1 and inputs == 0
            params = message['params']
            assert params['sessionId'] == native and params['workingDirectory'] == config['workspace']
            assert params['model'] == config['model']
            run, turn, child = state()
            assert child['native_session_id'] == native and child['approval_mode'] == 'plan'
            policy = params.get('managedSettings', {}).get('permissions', {})
            has_policy = (params.get('availableTools') == ['builtin:view', 'builtin:grep', 'builtin:glob'] and
                          params.get('excludedTools') == ['mcp:*', 'custom:*'] and
                          params.get('toolFilterPrecedence') == 'excluded' and
                          policy.get('disableBypassPermissionsMode') == 'disable' and
                          policy.get('deny') == ['shell', 'write'])
            response = dict(sessionId=native)
            store('copilot-resume-witness.json', dict(pid=os.getpid(), request=message, response=response, native_id=native))
            result(message, response)
)RESUME_NATIVE");
		replace_once("        elif method == 'session.send':\n            inputs += 1\n", R"RESUME_INPUT(        elif method == 'session.send':
            assert inputs == 0 and status_seen and has_policy
            required = {'session.managedSettings.get', 'session.permissions.getMode', 'session.metadata.snapshot',
                        'session.tools.initializeAndValidate', 'session.tools.getCurrentMetadata'}
            assert required <= delivered, 'Resumed input preceded complete fresh native evidence'
            assert (root/'allow-copilot-resume-tools').exists(), 'Resumed input preceded the last native evidence release'
            inputs += 1
)RESUME_INPUT");
		const std::string proof_before = R"RESUME_LOOP(        if not proof_emitted and (root/'allow-copilot-proof').exists():
            for message, response in held_policy: result(message, response)
            held_policy.clear()
            mode = config['evidence']
            if held_model is not None:
                message, held_model = held_model, None
                if mode == 'unsupported-model':
                    emit(dict(jsonrpc='2.0', id=message['id'], error=dict(code=-32601, message='Private peer does not support this optional query')))
                else:
                    response = {} if mode == 'missing-model' else dict(modelId='other-private-model' if mode in ('wrong-model','repair-model') else config['model'])
                    result(message, response)
                    if mode == 'repair-model':
                        # A matching duplicate cannot create a new owned observation after contradiction.
                        result(message, dict(modelId=config['model']))
                        notice('session.mode_changed', dict(newMode='plan'))
            proof_emitted = True
            fence = notice('session.usage_info', {})
            store('copilot-proof-replied.json', dict(mode=mode, fence=fence, queries=proof_queries))
)RESUME_LOOP";
		replace_once(proof_before, R"RESUME_FENCES(        if (root/'allow-copilot-resume-proofs').exists() and held_policy:
            for message, response in held_policy: proof_reply(message, response)
            held_policy.clear()
        if (root/'allow-copilot-resume-tools').exists() and held_tools:
            for message, response in held_tools: proof_reply(message, response)
            held_tools.clear()
            fence = notice('session.usage_info', {})
            store('copilot-resume-tools-replied.json', dict(pid=os.getpid(), fence=fence))
)RESUME_FENCES");
		for (const auto& names : {
		         std::pair{"copilot-policy-status-witness.json", "copilot-resume-status-witness.json"},
		         std::pair{"copilot-policy-native-observations.jsonl", "copilot-resume-native-observations.jsonl"},
		         std::pair{"copilot-query-witness.json", "copilot-resume-model-query-witness.json"},
		         std::pair{"copilot-input-witness.json', dict", "copilot-followup-input-witness.json', dict"},
		         std::pair{"copilot-observation-sent.json", "copilot-resume-observation-sent.json"},
		         std::pair{"allow-copilot-completion", "allow-copilot-resume-completion"},
		         std::pair{"copilot-completion-sent.json", "copilot-resume-completion-sent.json"}})
			replace_once(names.first, names.second);
		return peer;
	}

	void FlushCopilotSupportedResumePeer(ManagedCopilotBindingFixtureR124& fixture, const AgentRun& run)
	{
		auto* session = uam::FindAcpSessionForChat(fixture.app, run.transcript_chat_id);
		UAM_ASSERT(session && session->running && session->session_id == run.managed_continuation->execution->native_session_id);
		const int id = uam::acp_detail::NextAcpRequestId(*session, "ping");
		UAM_ASSERT(uam::acp_detail::WriteAcpMessage(*session, {{"jsonrpc", "2.0"}, {"id", id}, {"method", "ping"}, {"params", nlohmann::json::object()}}));
		fixture.Await([&] { const auto* current = uam::FindAcpSessionForChat(fixture.app, run.transcript_chat_id); return current && !current->pending_request_methods.contains(id); });
	}
}

UAM_TEST(ManagedCopilotFollowupReinjectsReadPolicyBeforeExactNativeResumeInput)
{
	ManagedCopilotBindingFixtureR124 fixture;
	AgentRun predecessor;
	try { predecessor = CompleteManagedCopilotSupportedReadPolicyInitial(fixture); }
	catch (const TestFailure& error)
	{
		throw TestFailure(std::string("Copilot resumed-boundary prerequisite failed in actual initial policy/binding/completion: ") + error.what());
	}
	// Every initial-positive assertion ran, including actual immutable birth, saved
	// input/result, enforced setup request, native observations and process reap.
	// Failure above is an initial prerequisite, never a claimed resumed-boundary RED.
	UAM_ASSERT(predecessor.managed_continuation && predecessor.managed_continuation->execution);
	const auto before = *predecessor.managed_continuation;
	const auto root_file = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id);
	const auto child_file = AppPaths::UamChatFilePath(fixture.app.data_root, predecessor.transcript_chat_id);
	const auto ledger_file = fixture.app.data_root / "agent-runs" / (predecessor.id + ".json");
	const auto parent_before = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	const auto child_before = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, predecessor.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(parent_before && child_before);
	const std::string task = "Continue the exact private Copilot child without repeating the initial task.";
	const std::string answer = "PRIVATE-MANAGED-COPILOT-CONTINUED-ANSWER";
	auto config = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-peer-config.json"));
	config["task"] = task;
	config["answer"] = answer;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "copilot-peer-config.json", config.dump()));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "copilot-peer.py", ManagedCopilotSupportedReadResumePeer()));
	fs::permissions(fixture.temp.root / "copilot-peer.py", fs::perms::owner_all, fs::perm_options::replace);
	fixture.AssertPeer();
	uam::ManagedFollowupRequest request;
	request.after_turn_id = before.current_turn_id;
	request.client_request_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	request.text = task;
	std::optional<uam::ManagedFollowupSubmitResult> accepted;
	std::size_t callbacks = 0;
	const std::string initial_ledger = ReadFile(ledger_file), initial_parent = ReadFile(root_file), initial_child = ReadFile(child_file);
	UAM_ASSERT_EQ(uam::ManagedSubagentFollowupService::TrySubmit(fixture.app, fixture.root_id, predecessor.id, request,
	    [&](const auto& result) { ++callbacks; accepted = result; return true; }), uam::ManagedFollowupAdmission::Pending);
	UAM_ASSERT_EQ(callbacks, std::size_t{0});
	UAM_ASSERT_EQ(ReadFile(ledger_file), initial_ledger);
	UAM_ASSERT_EQ(ReadFile(root_file), initial_parent);
	UAM_ASSERT_EQ(ReadFile(child_file), initial_child);
	fixture.Await([&] { return accepted.has_value(); });
	UAM_ASSERT_EQ(callbacks, std::size_t{1});
	UAM_ASSERT(accepted->accepted && !accepted->replayed && accepted->acceptance);
	fixture.Await([&] { return fs::exists(fixture.temp.root / "copilot-resume-witness.json") && fs::exists(fixture.temp.root / "copilot-resume-native-observations.jsonl"); });
	FlushCopilotSupportedResumePeer(fixture, predecessor);
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	UAM_ASSERT(!fs::exists(fixture.temp.root / "copilot-followup-input-witness.json"));
	const auto resume = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-resume-witness.json"));
	const auto initial_setup = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-setup-witness.json"));
	UAM_ASSERT(resume.at("pid") != initial_setup.at("pid"));
	UAM_ASSERT_EQ(resume.at("native_id"), nlohmann::json(before.execution->native_session_id));
	UAM_ASSERT_EQ(resume.at("response").at("sessionId"), nlohmann::json(before.execution->native_session_id));
	const auto& params = resume.at("request").at("params");
	UAM_ASSERT_EQ(resume.at("request").at("method"), nlohmann::json("session.resume"));
	UAM_ASSERT_EQ(params.at("sessionId"), nlohmann::json(before.execution->native_session_id));
	UAM_ASSERT_EQ(params.at("workingDirectory"), nlohmann::json(before.execution->workspace_directory));
	UAM_ASSERT_EQ(params.at("model"), nlohmann::json(before.execution->effective_model_id));
	for (const char* field : {"availableTools", "excludedTools", "toolFilterPrecedence", "managedSettings"})
		UAM_ASSERT_EQ(params.at(field), initial_setup.at("request").at("params").at(field));
	const auto status = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-resume-status-witness.json"));
	UAM_ASSERT_EQ(status.at("pid"), resume.at("pid"));
	UAM_ASSERT_EQ(status.at("response"), (nlohmann::json{{"protocolVersion", 3}, {"version", "1.0.98"}}));

	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-resume-proofs", "Release current-process native observations except the final tool metadata.\n"));
	fixture.Await([&]
	{
		if (!fs::exists(fixture.temp.root / "copilot-resume-tools-held.json") || !fs::exists(fixture.temp.root / "copilot-resume-proof-state.json")) return false;
		const auto delivered = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-resume-proof-state.json")).at("delivered").get<std::vector<std::string>>();
		for (const char* method : {"session.managedSettings.get", "session.permissions.getMode", "session.metadata.snapshot", "session.tools.initializeAndValidate"})
			if (std::ranges::find(delivered, method) == delivered.end()) return false;
		return true;
	});
	FlushCopilotSupportedResumePeer(fixture, predecessor);
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	UAM_ASSERT(!fs::exists(fixture.temp.root / "copilot-followup-input-witness.json"));
	const auto tools = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-resume-tools-held.json"));
	UAM_ASSERT_EQ(tools.at("pid"), resume.at("pid"));
	UAM_ASSERT_EQ(tools.at("request").at("params").at("sessionId"), nlohmann::json(before.execution->native_session_id));
	UAM_ASSERT_EQ(tools.at("request").at("method"), nlohmann::json("session.tools.getCurrentMetadata"));
	std::vector<nlohmann::json> resumed_observations;
	std::istringstream observations_stream(ReadFile(fixture.temp.root / "copilot-resume-native-observations.jsonl"));
	std::string observation_line;
	while (std::getline(observations_stream, observation_line)) if (!observation_line.empty()) resumed_observations.push_back(nlohmann::json::parse(observation_line));
	for (const std::string method : {"session.managedSettings.get", "session.permissions.getMode", "session.tools.initializeAndValidate", "session.tools.getCurrentMetadata"})
		UAM_ASSERT(std::ranges::any_of(resumed_observations, [&](const auto& observation)
		{
			return observation.at("pid") == resume.at("pid") && observation.at("inputs") == 0 && observation.at("request").at("method") == method && observation.at("request").at("params").at("sessionId") == before.execution->native_session_id;
		}));
	UAM_ASSERT(std::ranges::any_of(resumed_observations, [&](const auto& observation)
	{
		return observation.at("pid") == resume.at("pid") && observation.at("inputs") == 0 && observation.at("request").at("method") == "session.metadata.snapshot" && observation.at("request").at("params").at("sessionId") == before.execution->native_session_id && observation.at("response").at("sessionId") == before.execution->native_session_id && observation.at("response").at("currentMode") == "plan" && observation.at("response").at("selectedModel") == before.execution->effective_model_id && observation.at("response").at("workingDirectory") == before.execution->workspace_directory;
	}));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-resume-tools", "Release the last required native evidence before the exact follow-up input.\n"));
	fixture.AwaitFence("copilot-resume-tools-replied.json");
	fixture.AwaitFence("copilot-resume-observation-sent.json");
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.Launches(), std::size_t{2});
	const auto input = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-followup-input-witness.json"));
	UAM_ASSERT_EQ(input.at("pid"), resume.at("pid"));
	UAM_ASSERT_EQ(input.at("native_id"), nlohmann::json(before.execution->native_session_id));
	UAM_ASSERT_EQ(input.at("request").at("params").at("sessionId"), nlohmann::json(before.execution->native_session_id));
	UAM_ASSERT_EQ(input.at("turn").at("turnId"), nlohmann::json(accepted->acceptance->turn_id));
	UAM_ASSERT_EQ(input.at("turn").at("clientRequestId"), nlohmann::json(request.client_request_id));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-resume-completion", "Complete the genuine second turn and let ordinary save/result/reap retire its owner.\n"));
	fixture.Await([&]
	{
		const auto run = fixture.ReadRun();
		return run.status == "completed" && run.managed_continuation->turns.size() == 2 && run.managed_continuation->turns.back().result_delivered && fixture.app.acp_sessions.empty() && fixture.app.managed_initial_runtime_owners.empty();
	});
	const auto completed = fixture.ReadRun();
	UAM_ASSERT_EQ(completed.id, predecessor.id);
	UAM_ASSERT_EQ(completed.root_chat_id, predecessor.root_chat_id);
	UAM_ASSERT_EQ(completed.transcript_chat_id, predecessor.transcript_chat_id);
	UAM_ASSERT_EQ(completed.managed_continuation->execution, before.execution);
	UAM_ASSERT_EQ(completed.managed_continuation->owner, before.owner);
	UAM_ASSERT_EQ(completed.managed_continuation->turns.front(), before.turns.front());
	UAM_ASSERT_EQ(completed.result_excerpt, answer);
	const auto& second = completed.managed_continuation->turns.back();
	UAM_ASSERT_EQ(second.turn_id, accepted->acceptance->turn_id);
	UAM_ASSERT_EQ(second.turn_number, std::uint32_t{2});
	UAM_ASSERT_EQ(second.after_turn_id, request.after_turn_id);
	UAM_ASSERT_EQ(second.client_request_id, request.client_request_id);
	UAM_ASSERT_EQ(second.text, task);
	UAM_ASSERT_EQ(second.status, uam::ManagedRunTurnStatus::Completed);
	const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, completed.transcript_chat_id, 4 * 1024 * 1024);
	const auto parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(parent && child && child->native_session_id == before.execution->native_session_id);
	UAM_ASSERT(child->messages.size() >= child_before->messages.size() && std::equal(child_before->messages.begin(), child_before->messages.end(), child->messages.begin()));
	UAM_ASSERT(parent->messages.size() >= parent_before->messages.size() && std::equal(parent_before->messages.begin(), parent_before->messages.end(), parent->messages.begin()));
	for (const std::string& content : {fixture.task, fixture.answer, task, answer})
		UAM_ASSERT_EQ(std::ranges::count(child->messages, content, &Message::content), 1);
	for (const auto& delivery : {std::pair{predecessor.id, fixture.answer}, std::pair{second.turn_id, answer}})
	{
		std::size_t count = 0;
		for (const auto& message : parent->messages) for (const auto& tool : message.tool_calls) if (tool.id == "uam-agent-result-" + delivery.first)
		{
			++count;
			UAM_ASSERT(tool.is_sub_agent && tool.sub_agent_id == predecessor.id);
			const auto result = nlohmann::json::parse(tool.result_text);
			UAM_ASSERT_EQ(result.at("runId"), nlohmann::json(predecessor.id));
			UAM_ASSERT_EQ(result.at("result"), nlohmann::json(delivery.second));
			if (delivery.first == second.turn_id)
			{
				UAM_ASSERT_EQ(result.at("turnId"), nlohmann::json(second.turn_id));
				UAM_ASSERT_EQ(result.at("turnNumber"), nlohmann::json(2));
			}
		}
		UAM_ASSERT_EQ(count, std::size_t{1});
	}
	UAM_ASSERT(fixture.app.queued_agent_run_ids.empty() && fixture.app.local_acp_launch_tasks.empty());
	UAM_ASSERT(!ChatDomainService().FindChatById(fixture.app, completed.transcript_chat_id));
	errno = 0;
	const auto resumed_pid = resume.at("pid").get<pid_t>();
	UAM_ASSERT(resumed_pid > 0 && kill(resumed_pid, 0) < 0 && errno == ESRCH);
	const std::string final_ledger = ReadFile(ledger_file), final_parent = ReadFile(root_file), final_child = ReadFile(child_file);
	std::optional<uam::ManagedFollowupSubmitResult> replay;
	std::size_t replay_callbacks = 0;
	UAM_ASSERT_EQ(uam::ManagedSubagentFollowupService::TrySubmit(fixture.app, fixture.root_id, predecessor.id, request,
	    [&](const auto& result) { ++replay_callbacks; replay = result; return true; }), uam::ManagedFollowupAdmission::Pending);
	fixture.Await([&] { return replay.has_value() && fixture.app.managed_followups.empty(); });
	UAM_ASSERT_EQ(replay_callbacks, std::size_t{1});
	UAM_ASSERT(replay->accepted && replay->replayed && replay->acceptance == accepted->acceptance);
	for (int poll = 0; poll < 32; ++poll)
	{
		fixture.Pulse();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT_EQ(ReadFile(ledger_file), final_ledger);
	UAM_ASSERT_EQ(ReadFile(root_file), final_parent);
	UAM_ASSERT_EQ(ReadFile(child_file), final_child);
	UAM_ASSERT_EQ(fixture.ReadRun().managed_continuation, completed.managed_continuation);
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.Launches(), std::size_t{2});
	UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.local_acp_launch_tasks.empty());
	UAM_ASSERT(fixture.StopAndReap());
}

namespace
{
	std::string ManagedCopilotNegativeReadPolicyPeer(bool resume)
	{
		std::string peer = resume ? ManagedCopilotSupportedReadResumePeer() : ManagedCopilotSupportedReadPolicyPeer();
		const std::string before = "def result(message, value): emit(dict(jsonrpc='2.0', id=message['id'], result=value))\n";
		const std::string after = R"NEGATIVE_RESULT(def result(message, value):
    method = message.get('method', '')
    evidence = config.get('policy_evidence', '')
    original = json.loads(json.dumps(value))
    targeted = False
    error = None
    if method == 'status.get' and evidence.startswith('status-'):
        targeted = True
        if evidence == 'status-unsupported': error = dict(code=-32601, message='Private native policy query unavailable')
        elif evidence == 'status-missing-version': value.pop('version', None)
        elif evidence == 'status-malformed-version': value['version'] = '1.0.98-not-a-stable-version'
        elif evidence == 'status-old-version': value['version'] = '1.0.97'
        elif evidence == 'status-wrong-protocol': value['protocolVersion'] = 2
    elif method == 'session.managedSettings.get' and evidence.startswith('policy-'):
        targeted = True
        if evidence == 'policy-missing-provenance':
            value.pop('clientManaged', None)
            value['source'] = 'none'
            value['managedKeys'] = []
        elif evidence == 'policy-bypass-enabled': value['bypassPermissionsDisabled'] = False
    elif method == 'session.tools.getCurrentMetadata' and evidence in ('tools-null', 'tools-expanded'):
        targeted = True
        if evidence == 'tools-null': value['tools'] = None
        else: value['tools'].append(dict(name='shell', description='An excluded executable tool'))
    elif method == 'session.metadata.snapshot' and evidence.startswith('snapshot-'):
        targeted = True
        if evidence == 'snapshot-foreign-session': value['sessionId'] = str(uuid.uuid4())
        elif evidence in ('snapshot-wrong-model', 'snapshot-contradiction-then-valid'): value['selectedModel'] = 'different-private-model'
        elif evidence == 'snapshot-wrong-workspace': value['workingDirectory'] = str(root/'different-workspace')
        elif evidence == 'snapshot-wrong-mode': value['currentMode'] = 'interactive'
    wire = dict(jsonrpc='2.0', id=message['id'], error=error) if error else dict(jsonrpc='2.0', id=message['id'], result=value)
    frames = [wire]
    if targeted and evidence == 'snapshot-contradiction-then-valid':
        frames.append(dict(jsonrpc='2.0', id=message['id'], result=original))
    # Queue the contradictory and matching duplicate in the same flushed write.
    # The receiver must not resurrect a consumed/rejected native request ID.
    packet = b''
    for frame in frames:
        body = json.dumps(frame, separators=(',', ':')).encode('utf-8')
        packet += ('Content-Length: '+str(len(body))+'\r\n\r\n').encode('ascii')+body
    sys.stdout.buffer.write(packet)
    sys.stdout.buffer.flush()
    if targeted:
        with (root/'copilot-negative-native-replies.jsonl').open('a') as stream:
            for frame in frames:
                stream.write(json.dumps(dict(pid=os.getpid(), evidence=evidence, request=message, wire=frame), sort_keys=True)+'\n')
            stream.flush()
            os.fsync(stream.fileno())
)NEGATIVE_RESULT";
		const auto index = peer.find(before);
		UAM_ASSERT(index != std::string::npos && peer.find(before, index + before.size()) == std::string::npos);
		peer.replace(index, before.size(), after);
		return peer;
	}

	void ConfigureCopilotNegativeReadPolicyPeer(ManagedCopilotBindingFixtureR124& fixture, const std::string& evidence, bool resume,
	    const std::string& task = {}, const std::string& answer = {})
	{
		auto config = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-peer-config.json"));
		config["policy_evidence"] = evidence;
		if (!task.empty()) config["task"] = task;
		if (!answer.empty()) config["answer"] = answer;
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "copilot-peer-config.json", config.dump()));
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "copilot-peer.py", ManagedCopilotNegativeReadPolicyPeer(resume)));
		fs::permissions(fixture.temp.root / "copilot-peer.py", fs::perms::owner_all, fs::perm_options::replace);
		fixture.AssertPeer();
	}

	std::vector<nlohmann::json> CopilotNegativeReadPolicyReplies(const ManagedCopilotBindingFixtureR124& fixture,
	    const std::string& evidence, const nlohmann::json& pid)
	{
		const auto path = fixture.temp.root / "copilot-negative-native-replies.jsonl";
		UAM_ASSERT(fs::exists(path)); // Missing queries are not successful negative evidence.
		std::istringstream stream(ReadFile(path));
		std::string line;
		std::vector<nlohmann::json> replies;
		while (std::getline(stream, line)) if (!line.empty())
		{
			const auto reply = nlohmann::json::parse(line);
			UAM_ASSERT_EQ(reply.at("evidence"), nlohmann::json(evidence));
			UAM_ASSERT_EQ(reply.at("pid"), pid);
			UAM_ASSERT_EQ(reply.at("wire").at("id"), reply.at("request").at("id"));
			replies.push_back(reply);
		}
		UAM_ASSERT(!replies.empty());
		return replies;
	}
}

UAM_TEST(ManagedInitialCopilotUnsupportedOrIncompleteReadPolicyRemainsTruthfulOneShot)
{
	for (const std::string evidence : {"status-unsupported", "status-missing-version", "status-malformed-version", "status-old-version", "status-wrong-protocol", "policy-missing-provenance", "policy-bypass-enabled"})
	{
		ManagedCopilotBindingFixtureR124 fixture;
		ConfigureCopilotNegativeReadPolicyPeer(fixture, evidence, false);
		const auto initial = ManagedCopilotBeginR124(fixture);
		fixture.AwaitFence("copilot-observation-sent.json");
		fixture.FlushPeerRequests();
		UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
		UAM_ASSERT(!fixture.ReadRun().managed_continuation->execution);
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-proof", "Release all constructed initial native evidence, including the negative observation.\n"));
		fixture.AwaitFence("copilot-proof-replied.json");
		fixture.FlushPeerRequests();
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-completion", "Complete the truthful initial one-shot despite unavailable continuation policy.\n"));
		const auto completed = ManagedCopilotFinishR124(fixture, initial);
		UAM_ASSERT_EQ(completed.status, std::string("completed"));
		UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
		UAM_ASSERT(!completed.managed_continuation->execution);
		UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
		const auto setup = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-setup-witness.json"));
		(void)CopilotNegativeReadPolicyReplies(fixture, evidence, setup.at("pid"));
		const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, completed.transcript_chat_id, 4 * 1024 * 1024);
		UAM_ASSERT(child && child->native_session_id == setup.at("native_id").get<std::string>());
		UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.task, &Message::content), 1);
		UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.answer, &Message::content), 1);
		UAM_ASSERT(!uam::ManagedSubagentFollowupService::GetState(fixture.app, fixture.root_id, completed.id).can_send);
		UAM_ASSERT(fixture.StopAndReap());
	}
}

UAM_TEST(ManagedCopilotFollowupRejectsIncompleteOrContradictoryFreshNativePolicyWithoutInput)
{
	for (const std::string evidence : {"tools-null", "tools-expanded", "snapshot-foreign-session", "snapshot-wrong-model", "snapshot-wrong-workspace", "snapshot-wrong-mode", "snapshot-contradiction-then-valid"})
	{
		ManagedCopilotBindingFixtureR124 fixture;
		AgentRun predecessor;
		try { predecessor = CompleteManagedCopilotSupportedReadPolicyInitial(fixture); }
		catch (const TestFailure& error)
		{
			throw TestFailure(std::string("Copilot negative resume prerequisite failed in genuine initial policy/binding/completion: ") + error.what());
		}
		const auto before = *predecessor.managed_continuation;
		const auto old_child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, predecessor.transcript_chat_id, 4 * 1024 * 1024);
		const auto old_parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		UAM_ASSERT(old_child && old_parent && before.execution);
		const std::string task = "Keep this genuine managed follow-up unsent until exact native read policy is proven.";
		const std::string forbidden_answer = "UNVERIFIED-COPILOT-RESUME-MUST-NOT-ANSWER";
		ConfigureCopilotNegativeReadPolicyPeer(fixture, evidence, true, task, forbidden_answer);
		uam::ManagedFollowupRequest request;
		request.after_turn_id = before.current_turn_id;
		request.client_request_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		request.text = task;
		std::optional<uam::ManagedFollowupSubmitResult> accepted;
		std::size_t callbacks = 0;
		UAM_ASSERT_EQ(uam::ManagedSubagentFollowupService::TrySubmit(fixture.app, fixture.root_id, predecessor.id, request,
		    [&](const auto& result) { ++callbacks; accepted = result; return true; }), uam::ManagedFollowupAdmission::Pending);
		fixture.Await([&] { return accepted.has_value(); });
		UAM_ASSERT_EQ(callbacks, std::size_t{1});
		UAM_ASSERT(accepted->accepted && !accepted->replayed && accepted->acceptance);
		fixture.Await([&] { return fs::exists(fixture.temp.root / "copilot-resume-witness.json"); });
		const auto resume = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-resume-witness.json"));
		UAM_ASSERT_EQ(resume.at("native_id"), nlohmann::json(before.execution->native_session_id));
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-resume-proofs", "Release every current-process native policy observation, including the contradiction.\n"));
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-resume-tools", "Release final tool metadata; missing evidence cannot be mistaken for successful rejection.\n"));
		fixture.Await([&]
		{
			const auto run = fixture.ReadRun();
			return fixture.Inputs() != 1 || (run.status != "queued" && run.status != "running" && run.managed_continuation->turns.size() == 2 && run.managed_continuation->turns.back().result_delivered && fixture.app.acp_sessions.empty() && fixture.app.managed_initial_runtime_owners.empty());
		});
		UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
		UAM_ASSERT(!fs::exists(fixture.temp.root / "copilot-followup-input-witness.json"));
		UAM_ASSERT_EQ(fixture.Launches(), std::size_t{2});
		const auto replies = CopilotNegativeReadPolicyReplies(fixture, evidence, resume.at("pid"));
		for (const auto& reply : replies)
			UAM_ASSERT_EQ(reply.at("request").at("params").at("sessionId"), nlohmann::json(before.execution->native_session_id));
		if (evidence == "snapshot-contradiction-then-valid")
		{
			UAM_ASSERT_EQ(replies.size(), std::size_t{2});
			UAM_ASSERT_EQ(replies[0].at("wire").at("id"), replies[1].at("wire").at("id"));
			UAM_ASSERT_EQ(replies[0].at("wire").at("result").at("selectedModel"), nlohmann::json("different-private-model"));
			UAM_ASSERT_EQ(replies[1].at("wire").at("result").at("selectedModel"), nlohmann::json(before.execution->effective_model_id));
		}
		const auto ended = fixture.ReadRun();
		UAM_ASSERT_EQ(ended.status, std::string("failed"));
		UAM_ASSERT_EQ(ended.id, predecessor.id);
		UAM_ASSERT_EQ(ended.transcript_chat_id, predecessor.transcript_chat_id);
		UAM_ASSERT_EQ(ended.managed_continuation->execution, before.execution);
		UAM_ASSERT_EQ(ended.managed_continuation->owner, before.owner);
		UAM_ASSERT_EQ(ended.managed_continuation->turns.front(), before.turns.front());
		const auto& second = ended.managed_continuation->turns.back();
		UAM_ASSERT_EQ(second.turn_id, accepted->acceptance->turn_id);
		UAM_ASSERT_EQ(second.status, uam::ManagedRunTurnStatus::Failed);
		UAM_ASSERT(second.dispatch_attempted && second.result_delivered);
		const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, predecessor.transcript_chat_id, 4 * 1024 * 1024);
		const auto parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		UAM_ASSERT(child && parent && child->native_session_id == before.execution->native_session_id);
		UAM_ASSERT(child->messages.size() >= old_child->messages.size() && std::equal(old_child->messages.begin(), old_child->messages.end(), child->messages.begin()));
		UAM_ASSERT(parent->messages.size() >= old_parent->messages.size() && std::equal(old_parent->messages.begin(), old_parent->messages.end(), parent->messages.begin()));
		UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.task, &Message::content), 1);
		UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.answer, &Message::content), 1);
		UAM_ASSERT_EQ(std::ranges::count(child->messages, forbidden_answer, &Message::content), 0);
		UAM_ASSERT(!ChatDomainService().FindChatById(fixture.app, predecessor.transcript_chat_id));
		UAM_ASSERT(fixture.app.queued_agent_run_ids.empty() && fixture.app.local_acp_launch_tasks.empty());
		errno = 0;
		const auto pid = resume.at("pid").get<pid_t>();
		UAM_ASSERT(pid > 0 && kill(pid, 0) < 0 && errno == ESRCH);
		UAM_ASSERT(fixture.StopAndReap());
	}
}
#endif


#if UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_RUNTIME_COPILOT_CLI && defined(__APPLE__)
UAM_TEST(ManagedInitialCopilotTransientConfigurationDriftCannotRestoreObservedBinding)
{
	ManagedCopilotBindingFixtureR124 fixture;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "copilot-peer.py", ManagedCopilotSupportedReadPolicyPeer(true)));
	fs::permissions(fixture.temp.root / "copilot-peer.py", fs::perms::owner_all, fs::perm_options::replace);
	fixture.AssertPeer();
	const auto initial = ManagedCopilotBeginR124(fixture);
	fixture.AwaitFence("copilot-observation-sent.json");
	fixture.FlushPeerRequests();
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-proof", "Release genuine current-process native read-policy evidence before transient configuration drift.\n"));
	fixture.AwaitFence("copilot-proof-replied.json");
	const auto setup = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-setup-witness.json"));
	const auto native = setup.at("native_id").get<std::string>();
	const auto pid = setup.at("pid");
	UAM_ASSERT(uam::uuid::IsCanonicalUuid(native));
	fixture.Await([&]
	{
		const auto observations = CopilotSupportedReadPolicyObservations(fixture.temp.root);
		for (const std::string method : {"session.managedSettings.get", "session.permissions.getMode", "session.metadata.snapshot", "session.tools.initializeAndValidate", "session.tools.getCurrentMetadata"})
			if (!std::ranges::any_of(observations, [&](const auto& observation)
			{
				return observation.at("pid") == pid && observation.at("inputs") == 1 && observation.at("request").at("method") == method && observation.at("request").at("params").at("sessionId") == native;
			})) return false;
		return true;
	});
	// The peer replies before reading this ping, so its FIFO response consumes the
	// complete actual post-input proof, not merely the query-witness file write.
	fixture.FlushPeerRequests();
	const auto observations = CopilotSupportedReadPolicyObservations(fixture.temp.root);
	UAM_ASSERT(std::ranges::any_of(observations, [&](const auto& observation)
	{
		return observation.at("pid") == pid && observation.at("inputs") == 1 && observation.at("request").at("method") == "session.metadata.snapshot" && observation.at("response").at("sessionId") == native && observation.at("response").at("currentMode") == "plan" && observation.at("response").at("selectedModel") == initial.model_id;
	}));
	const auto* child_before = ChatDomainService().FindChatById(fixture.app, initial.transcript_chat_id);
	UAM_ASSERT(child_before && child_before->native_session_id == native);
	const auto child_prefix = child_before->messages;
	UAM_ASSERT(!fixture.ReadRun().managed_continuation->execution);

	// Exercise the actual polling/provider hook while execution configuration differs,
	// then restore it before the genuine completion can publish any continuation binding.
	const auto recall_budget = fixture.app.settings.memory_recall_budget_bytes;
	fixture.app.settings.memory_recall_budget_bytes = recall_budget == 0 ? 1 : 0;
	fixture.Pulse();
	fixture.app.settings.memory_recall_budget_bytes = recall_budget;
	fixture.FlushPeerRequests();
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-completion", "Finish the truthful original one-shot after restoring configuration; revoked proof must remain unusable.\n"));
	const auto completed = ManagedCopilotFinishR124(fixture, initial);
	UAM_ASSERT_EQ(completed.status, std::string("completed"));
	UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
	UAM_ASSERT(!completed.managed_continuation->execution);
	UAM_ASSERT(!uam::ManagedSubagentFollowupService::GetState(fixture.app, fixture.root_id, completed.id).can_send);
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, completed.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(child && child->native_session_id == native);
	UAM_ASSERT(child->messages.size() >= child_prefix.size() && std::equal(child_prefix.begin(), child_prefix.end(), child->messages.begin()));
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.answer, &Message::content), 1);
	UAM_ASSERT(fixture.StopAndReap());
}
#endif


#if UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_RUNTIME_COPILOT_CLI && defined(__APPLE__)
namespace
{
	std::string ManagedCopilotReadbackDriftResumePeer()
	{
		std::string peer = ManagedCopilotSupportedReadResumePeer();
		const std::string before = "    def proof_reply(message, response):\n        result(message, response)\n";
		const std::string after = R"DRIFT(    def proof_reply(message, response):
        if message['method'] == 'session.tools.getCurrentMetadata':
            required = {'session.managedSettings.get', 'session.permissions.getMode', 'session.metadata.snapshot',
                        'session.tools.initializeAndValidate'}
            assert inputs == 0 and required <= delivered
            kind = config['drift_event']
            data = dict(newModel='different-private-model', autoTier=None) if kind == 'session.model_change' else dict(
                clientManaged=False, bypassPermissionsDisabled=False, deviceManaged=False, serverManaged=False,
                failClosed=False, managedKeys=[], source='none')
            drift = dict(id=str(uuid.uuid4()), parentId=None, ephemeral=True, type=kind, timestamp='2026-10-18T02:00:00.000Z', data=data)
            frames = [dict(jsonrpc='2.0', method='session.event', params=dict(sessionId=native, event=drift)),
                      dict(jsonrpc='2.0', id=message['id'], result=response)]
            store('copilot-resume-readback-drift.json', dict(pid=os.getpid(), sessionId=native,
                  inputs=inputs, delivered=sorted(delivered), request=message, frames=frames))
            # One ordered write: a fresh root contradiction precedes the last
            # otherwise-valid reply. Neither shutdown nor polling can reorder it.
            packet = b''
            for frame in frames:
                raw = json.dumps(frame, separators=(',',':')).encode()
                packet += ('Content-Length: %d\r\n\r\n' % len(raw)).encode() + raw
            sys.stdout.buffer.write(packet)
            sys.stdout.buffer.flush()
        else: result(message, response)
)DRIFT";
		const auto index = peer.find(before);
		UAM_ASSERT(index != std::string::npos && peer.find(before, index + before.size()) == std::string::npos);
		peer.replace(index, before.size(), after);
		return peer;
	}

	void AssertManagedCopilotReadbackDriftCannotSend(const std::string& event_type)
	{
		ManagedCopilotBindingFixtureR124 fixture;
		AgentRun predecessor;
		try { predecessor = CompleteManagedCopilotSupportedReadPolicyInitial(fixture); }
		catch (const TestFailure& error)
		{
			throw TestFailure(std::string("Copilot readback-drift prerequisite failed in genuine initial binding/completion: ") + error.what());
		}
		const auto before = *predecessor.managed_continuation;
		UAM_ASSERT(before.execution);
		const auto old_child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, predecessor.transcript_chat_id, 4 * 1024 * 1024);
		const auto old_parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		UAM_ASSERT(old_child && old_parent);
		const std::string task = "Do not send this follow-up after the current native readback is contradicted.";
		const std::string forbidden_answer = "STALE-COPILOT-READBACK-MUST-NOT-ANSWER";
		auto config = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-peer-config.json"));
		config["task"] = task;
		config["answer"] = forbidden_answer;
		config["drift_event"] = event_type;
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "copilot-peer-config.json", config.dump()));
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "copilot-peer.py", ManagedCopilotReadbackDriftResumePeer()));
		fs::permissions(fixture.temp.root / "copilot-peer.py", fs::perms::owner_all, fs::perm_options::replace);
		fixture.AssertPeer();
		uam::ManagedFollowupRequest request;
		request.after_turn_id = before.current_turn_id;
		request.client_request_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		request.text = task;
		std::optional<uam::ManagedFollowupSubmitResult> accepted;
		std::size_t callbacks = 0;
		UAM_ASSERT_EQ(uam::ManagedSubagentFollowupService::TrySubmit(fixture.app, fixture.root_id, predecessor.id, request,
		    [&](const auto& result) { ++callbacks; accepted = result; return true; }), uam::ManagedFollowupAdmission::Pending);
		fixture.Await([&] { return accepted.has_value(); });
		UAM_ASSERT_EQ(callbacks, std::size_t{1});
		UAM_ASSERT(accepted->accepted && !accepted->replayed && accepted->acceptance);
		fixture.Await([&] { return fs::exists(fixture.temp.root / "copilot-resume-witness.json"); });
		const auto resume = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-resume-witness.json"));
		UAM_ASSERT_EQ(resume.at("native_id"), nlohmann::json(before.execution->native_session_id));
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-resume-proofs", "Release fresh native readbacks except final tool metadata.\n"));
		fixture.Await([&]
		{
			if (!fs::exists(fixture.temp.root / "copilot-resume-tools-held.json") || !fs::exists(fixture.temp.root / "copilot-resume-proof-state.json")) return false;
			const auto delivered = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-resume-proof-state.json")).at("delivered").get<std::vector<std::string>>();
			for (const char* method : {"session.managedSettings.get", "session.permissions.getMode", "session.metadata.snapshot", "session.tools.initializeAndValidate"})
				if (std::ranges::find(delivered, method) == delivered.end()) return false;
			return true;
		});
		FlushCopilotSupportedResumePeer(fixture, predecessor);
		const auto* session = uam::FindAcpSessionForChat(fixture.app, predecessor.transcript_chat_id);
		UAM_ASSERT(session);
		const auto& policy = session->copilot_native_state.at("managedReadPolicy");
		UAM_ASSERT_EQ(policy.at("phase"), nlohmann::json("before"));
		UAM_ASSERT(!policy.value("beforeReady", false));
		UAM_ASSERT(policy.at("observations").at("session.metadata.snapshot").get<bool>());
		UAM_ASSERT(policy.at("observations").at("session.managedSettings.get").get<bool>());
		UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-copilot-resume-tools", "Deliver native drift before final otherwise-valid metadata in one ordered packet.\n"));
		fixture.Await([&]
		{
			const auto run = fixture.ReadRun();
			return fixture.Inputs() != 1 || (run.status == "failed" && run.managed_continuation->turns.size() == 2 && run.managed_continuation->turns.back().result_delivered && fixture.app.acp_sessions.empty() && fixture.app.managed_initial_runtime_owners.empty());
		});
		const auto drift = nlohmann::json::parse(ReadFile(fixture.temp.root / "copilot-resume-readback-drift.json"));
		UAM_ASSERT_EQ(drift.at("pid"), resume.at("pid"));
		UAM_ASSERT_EQ(drift.at("sessionId"), nlohmann::json(before.execution->native_session_id));
		UAM_ASSERT_EQ(drift.at("inputs"), nlohmann::json(0));
		UAM_ASSERT_EQ(drift.at("frames").at(0).at("params").at("event").at("type"), nlohmann::json(event_type));
		UAM_ASSERT_EQ(drift.at("frames").at(1).at("id"), drift.at("request").at("id"));
		UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
		UAM_ASSERT(!fs::exists(fixture.temp.root / "copilot-followup-input-witness.json"));
		const auto ended = fixture.ReadRun();
		UAM_ASSERT_EQ(ended.status, std::string("failed"));
		UAM_ASSERT_EQ(ended.managed_continuation->execution, before.execution);
		UAM_ASSERT_EQ(ended.managed_continuation->owner, before.owner);
		UAM_ASSERT_EQ(ended.managed_continuation->turns.front(), before.turns.front());
		UAM_ASSERT_EQ(ended.managed_continuation->turns.back().turn_id, accepted->acceptance->turn_id);
		UAM_ASSERT_EQ(ended.managed_continuation->turns.back().status, uam::ManagedRunTurnStatus::Failed);
		const auto child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, predecessor.transcript_chat_id, 4 * 1024 * 1024);
		const auto parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		UAM_ASSERT(child && parent && child->native_session_id == before.execution->native_session_id);
		UAM_ASSERT(child->messages.size() >= old_child->messages.size() && std::equal(old_child->messages.begin(), old_child->messages.end(), child->messages.begin()));
		UAM_ASSERT(parent->messages.size() >= old_parent->messages.size() && std::equal(old_parent->messages.begin(), old_parent->messages.end(), parent->messages.begin()));
		UAM_ASSERT_EQ(std::ranges::count(child->messages, forbidden_answer, &Message::content), 0);
		UAM_ASSERT_EQ(fixture.Launches(), std::size_t{2});
		UAM_ASSERT(fixture.StopAndReap());
	}
}

UAM_TEST(ManagedCopilotFollowupModelDriftBeforeFinalToolsCannotSend)
{
	AssertManagedCopilotReadbackDriftCannotSend("session.model_change");
}

UAM_TEST(ManagedCopilotFollowupManagedSettingsDriftBeforeFinalToolsCannotSend)
{
	AssertManagedCopilotReadbackDriftCannotSend("session.managed_settings_resolved");
}
#endif

#include "common/runtime/acp/acp_control_capability_retention.h"

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
namespace
{
	const char* JoinedClaudeMcpPeer()
	{
		return R"UAM_PEER(import json, os, pathlib, signal, subprocess, sys, time, uuid
root = pathlib.Path(__file__).resolve().parent
cfg = json.loads((root / 'peer-config.json').read_text())
data = pathlib.Path(cfg['data_root'])
server = None

def store(name, value):
    p = root / name
    with (root / (name + '.tmp')).open('w') as f:
        json.dump(value, f, sort_keys=True); f.flush(); os.fsync(f.fileno())
    (root / (name + '.tmp')).replace(p)

def read(p):
    assert p.is_file() and not p.is_symlink() and p.stat().st_size <= 4 * 1024 * 1024, 'private.assert.line0014'
    return json.loads(p.read_text())

def emit(v):
    print(json.dumps(v), flush=True)

def directory_tag(value):
    value_hash = 1469598103934665603
    for byte in value.encode('utf-8'):
        value_hash = ((value_hash ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return format(value_hash, '016x')

def wait_file(name):
    end = time.monotonic() + 30
    while not (root / name).is_file():
        assert time.monotonic() < end, 'private.assert.line0029'
        time.sleep(0.005)

def mcp(request):
    server.stdin.write(json.dumps(request) + '\n'); server.stdin.flush()
    line = server.stdout.readline()
    assert line, 'private.assert.line0035'
    value = json.loads(line)
    assert value['id'] == request['id'] and 'error' not in value, 'private.assert.line0037'
    return value['result']

def tool(number, name, args):
    result = mcp(dict(jsonrpc='2.0', id=number, method='tools/call',
        params=dict(name=name, arguments=args)))
    assert result['isError'] is False, 'private.assert.line0043'
    assert len(result['content']) == 1 and result['content'][0]['type'] == 'text', 'private.assert.line0044'
    return json.loads(result['content'][0]['text'])

def close_server():
    global server
    if server is not None:
        server.stdin.close()
        try:
            code = server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill(); server.wait(); raise
        assert code == 0, 'private.assert.line0055'
        server.stdout.close()
        store('mcp-reaped.json', dict(pid=server.pid, code=code))
        server = None

signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
try:
    if sys.argv[1:] == ['--version']:
        print('2.1.31 (Claude Code)', flush=True); sys.exit(0)
    args = sys.argv[1:]
    assert args[args.index('--input-format') + 1] == 'stream-json', 'private.assert.line0065'
    assert args[args.index('--output-format') + 1] == 'stream-json', 'private.assert.line0066'
    assert args[args.index('--model') + 1] == cfg['model'] and '--resume' not in args, 'private.assert.line0067'
    mode = args[args.index('--permission-mode') + 1]
    assert mode in ('default', 'plan'), 'private.assert.line0069'
    role = 'child' if mode == 'plan' else 'parent'
    native = str(uuid.uuid4())
    store(role + '-launch.json', dict(pid=os.getpid(), args=args, native=native,
        cwd=os.getcwd(), mode=mode))
    for line in sys.stdin:
        message = json.loads(line)
        if message.get('type') == 'control_request':
            assert message['request']['subtype'] == 'initialize', 'private.assert.line0077'
            emit(dict(type='control_response', response=dict(subtype='success',
                request_id=message['request_id'], response=dict(commands=[],
                models=[dict(value=cfg['model'], displayName='Private Claude peer')]))))
            emit(dict(type='system', subtype='init', session_id=native,
                model=cfg['model'], permissionMode=mode))
            continue
        assert message['type'] == 'user', 'private.assert.line0084'
        with (root / (role + '-inputs.jsonl')).open('a') as f:
            f.write(json.dumps(message) + '\n'); f.flush(); os.fsync(f.fileno())
        parts = message['message']['content']
        text = parts if isinstance(parts, str) else ''.join(p.get('text', '') for p in parts)
        if role == 'parent':
            assert text.count(cfg['parent_task']) == 1 and cfg['task'] not in text, 'private.assert.line0090'
            assert read(data / 'chats' / (cfg['root_id'] + '.json'))['uamControlEnabled'], 'private.assert.line0091'
            raw = args[args.index('--mcp-config') + 1]
            config = json.loads(raw) if raw.startswith('{') else read(pathlib.Path(raw))
            definition = config['mcpServers']['uam-control']
            assert definition['args'] == ['--uam-control-mcp'], 'private.assert.line0095'
            env = dict(os.environ); env.update(definition['env'])
            capability_directory = pathlib.Path(env['UAM_CONTROL_CAPABILITY_DIR'])
            store('mcp-manager-boundary.json', dict(directory_tag=directory_tag(str(capability_directory)),
                directory_exists=capability_directory.is_dir(), directory_is_link=capability_directory.is_symlink(),
                requests_directory_exists=(capability_directory / 'requests').is_dir(),
                responses_directory_exists=(capability_directory / 'responses').is_dir()))
            server = subprocess.Popen([definition['command'], *definition['args']],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
            store('mcp-launch.json', dict(pid=server.pid, command=definition['command'], args=definition['args']))
            hello = mcp(dict(jsonrpc='2.0', id=1, method='initialize',
                params=dict(protocolVersion='2025-03-26', capabilities={}, clientInfo=dict(name='private-claude-peer', version='1'))))
            assert hello['serverInfo']['name'] == 'uam-control', 'private.assert.line0107'
            server.stdin.write(json.dumps(dict(jsonrpc='2.0', method='notifications/initialized')) + '\n'); server.stdin.flush()
            delegated = tool(2, 'agent_delegate', dict(agentId='private-stream-reviewer',
                task=cfg['task'], providerId='claude-cli', modelId=cfg['model']))
            assert delegated['status'] == 'queued' and delegated['runId'], 'private.assert.line0111'
            store('delegated.json', delegated)
            wait_file('request-terminal-status')
            status = tool(3, 'agent_status', dict(runId=delegated['runId']))
            assert status['runId'] == delegated['runId'] and status['status'] == 'completed', 'private.assert.line0115'
            assert status['providerId'] == 'claude-cli' and status['modelId'] == cfg['model'], 'private.assert.line0116'
            assert status['result'] == cfg['answer'], 'private.assert.line0117'
            store('terminal-status.json', status)
            close_server()
            answer = cfg['parent_answer']
        else:
            runs = [read(p) for p in (data / 'agent-runs').glob('*.json')]
            assert len(runs) == 1, 'private.assert.line0123'
            run = runs[0]
            assert run['rootChatId'] == cfg['root_id'] and run['status'] == 'running', 'private.assert.line0125'
            assert run['providerId'] == 'claude-cli' and run['modelId'] == cfg['model'], 'private.assert.line0126'
            assert run['effectiveWorkspaceAccess'] == 'read' and run['task'] == cfg['task'], 'private.assert.line0127'
            chat = read(data / 'chats' / (run['transcriptChatId'] + '.json'))
            assert chat['agent_run_id'] == run['id'] and chat['execution_host_id'] == 'local', 'private.assert.line0129'
            assert chat['approval_mode'] == 'plan' and chat['workspace_directory'] == os.getcwd(), 'private.assert.line0130'
            assert any(row['role'] == 'user' and row['content'] == cfg['task'] for row in chat['messages']), 'private.assert.line0131'
            assert text.count(cfg['task']) == 1, 'private.assert.line0132'
            store('child-input-witness.json', dict(pid=os.getpid(), run=run, chat=chat, native=native))
            answer = cfg['answer']
        emit(dict(type='assistant', session_id=native, message=dict(id=str(uuid.uuid4()),
            role='assistant', content=[dict(type='text', text=answer)])))
        emit(dict(type='result', subtype='success', is_error=False, session_id=native,
            model=cfg['model'], result=answer, duration_ms=1, num_turns=1))
except Exception as error:
    failure_line = 0
    traceback_tail = sys.exc_info()[2]
    final_frame_locals = {}
    for _ in range(16):
        if traceback_tail is None:
            break
        failure_line = traceback_tail.tb_lineno
        final_frame_locals = traceback_tail.tb_frame.f_locals
        traceback_tail = traceback_tail.tb_next
    label = error.args[0] if isinstance(error, AssertionError) and len(error.args) == 1 and isinstance(error.args[0], str) and error.args[0].startswith('private.assert.line') else 'private.peer.exception'
    predicates = {}
    saved_run = locals().get('run')
    saved_chat = locals().get('chat')
    if isinstance(saved_run, dict):
        predicates.update(root_matches=saved_run.get('rootChatId') == cfg['root_id'],
            ledger_running=saved_run.get('status') == 'running', provider_matches=saved_run.get('providerId') == 'claude-cli',
            model_matches=saved_run.get('modelId') == cfg['model'], captured_read=saved_run.get('effectiveWorkspaceAccess') == 'read',
            task_matches=saved_run.get('task') == cfg['task'])
    if isinstance(saved_chat, dict):
        rows = saved_chat.get('messages')
        predicates.update(chat_run_matches=isinstance(saved_run, dict) and saved_chat.get('agent_run_id') == saved_run.get('id'),
            host_local=saved_chat.get('execution_host_id') == 'local', approval_plan=saved_chat.get('approval_mode') == 'plan',
            workspace_matches=saved_chat.get('workspace_directory') == os.getcwd(),
            authored_task_present=isinstance(rows, list) and any(isinstance(row, dict) and row.get('role') == 'user' and row.get('content') == cfg['task'] for row in rows))
    observed_text = locals().get('text')
    if isinstance(observed_text, str):
        predicates['exactly_one_child_task'] = observed_text.count(cfg['task']) == 1
    observed_result = final_frame_locals.get('result')
    if isinstance(observed_result, dict) and isinstance(observed_result.get('isError'), bool):
        predicates['mcp_is_error'] = observed_result['isError']
    safe_role = locals().get('role')
    store('peer-error.json', dict(error=label, exception_type=type(error).__name__[:20],
        final_numeric_line=failure_line, traceback_truncated=traceback_tail is not None,
        role=safe_role if safe_role in ('parent', 'child') else 'before-role', predicates=predicates))
    sys.exit(70)
finally:
    close_server()
)UAM_PEER";
	}

	struct JoinedClaudeControlCleanup
	{
		ManagedClaudeLifecycleFixture& fixture;
		~JoinedClaudeControlCleanup() noexcept
		{
			try
			{
				const auto launch = fixture.temp.root / "mcp-launch.json";
				if (fs::is_regular_file(launch)) fixture.owned_pids.insert(nlohmann::json::parse(ReadFile(launch)).at("pid").get<pid_t>());
				if (!fixture.StopAndReap()) { std::cerr << "FATAL_PRIVATE_JOINED_CLAUDE_NOT_REAPED\n"; std::abort(); }
				uam::UamControlService::Shutdown(fixture.app);
			}
			catch (...) { std::cerr << "FATAL_PRIVATE_JOINED_CLAUDE_CLEANUP\n"; std::abort(); }
		}
	};
}

UAM_TEST(ManagedClaudeOrdinaryMcpDelegateReturnsTerminalNativeChildResult)
{
	ManagedClaudeLifecycleFixture fixture(true);
	JoinedClaudeControlCleanup cleanup{fixture};
	const std::string parent_task = "Delegate one private read-only Claude review and report its terminal status.";
	const std::string parent_answer = "PRIVATE-PARENT-OBSERVED-TERMINAL-CHILD-RESULT";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "peer.py", JoinedClaudeMcpPeer()));
	fs::permissions(fixture.temp.root / "peer.py", fs::perms::owner_all, fs::perm_options::replace);
	const auto config = fixture.temp.root / "peer-config.json";
	auto payload = nlohmann::json::parse(ReadFile(config));
	payload["parent_task"] = parent_task; payload["parent_answer"] = parent_answer;
	UAM_ASSERT(uam::io::WriteTextFile(config, payload.dump()));
	auto& root = fixture.app.chats.front();
	// This joined MCP fixture selects its existing private workspace, not a Git worktree.
	fixture.app.settings.git_enabled = fixture.app.settings.git_auto_worktree = false;
	root.workspace_isolation_kind.clear();
	// Persist the physical directory spelling so the unchanged peer getcwd equality is exact on macOS.
	root.workspace_directory = fs::canonical(fs::path(root.workspace_directory)).string();
	root.uam_control_enabled = true; root.uam_control_explicitly_disabled = false;
	root.uam_agent_id = "build"; root.approval_mode = "default"; root.command_safety_tier = "yolo";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, root));
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(fixture.app, &error) && error.empty());
	struct WatchedControlCapability
	{
		std::size_t ordinal;
		fs::path directory;
		std::string session_chat_id, provider_id;
		const uam::AcpSessionState* original_session;
	};
	std::map<std::string, WatchedControlCapability> watched_control;
	const auto control_boundary = [&]()
	{
		using Json = nlohmann::json;
		for (const auto& capability : fixture.app.uam_control_capabilities)
			if (!watched_control.contains(capability.id) && watched_control.size() < 8)
				watched_control.emplace(capability.id, WatchedControlCapability{watched_control.size() + 1,
				    capability.directory, capability.session_chat_id, capability.provider_id,
				    uam::FindAcpSessionForChat(fixture.app, capability.session_chat_id)});
		const auto* root_chat = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
		Json state{{"manager_initialized", !fixture.app.uam_control_manager_id.empty()},
		    {"root_exists", root_chat != nullptr}, {"root_birth_current", root_chat && root_chat->metadata_incarnation == fixture.root_incarnation},
		    {"local_launch_tasks", fixture.app.local_acp_launch_tasks.size()},
		    {"capability_count", fixture.app.uam_control_capabilities.size()}, {"watched", Json::array()}};
		for (const auto& [id, observed] : watched_control)
		{
			const auto current = std::ranges::find(fixture.app.uam_control_capabilities, id, &uam::UamControlCapability::id);
			const auto* session = uam::FindAcpSessionForChat(fixture.app, observed.session_chat_id);
			Json row{{"ordinal", observed.ordinal}, {"directory_tag", uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(observed.directory.string()))},
			    {"retained", current != fixture.app.uam_control_capabilities.end()},
			    {"directory_exists", fs::is_directory(observed.directory)}, {"directory_is_link", fs::is_symlink(observed.directory)},
			    {"requests_directory_exists", fs::is_directory(observed.directory / "requests")},
			    {"responses_directory_exists", fs::is_directory(observed.directory / "responses")},
			    {"session_exists", session != nullptr}, {"same_session", session && session == observed.original_session}};
			if (current != fixture.app.uam_control_capabilities.end())
			{
				row["not_expired"] = uam::time::SystemEpochMillisecondsNow() < current->expires_at_epoch_ms;
				row["mcp_initialized"] = current->mcp_initialized;
			}
			if (session)
			{
				row["session_capability_matches"] = session->uam_control_capability_id == id;
				row["provider_matches"] = session->provider_id == observed.provider_id;
				row["running"] = session->running; row["processing"] = session->processing;
				row["pending_local_launch"] = bool(session->pending_local_launch);
				row["lifecycle"] = session->lifecycle_state; row["turn_serial"] = session->turn_serial;
				row["process_present"] = session->child_pid > 0; row["native_identity_present"] = !session->session_id.empty();
			}
			state["watched"].push_back(std::move(row));
		}
		return state;
	};
	nlohmann::json previous_control_pair;
	std::size_t control_poll_ordinal = 0, control_changes_reported = 0, retained_pending_observations = 0;
	struct EmitNativeControlBoundary
	{
		fs::path path;
		~EmitNativeControlBoundary() noexcept
		{
			try
			{
				if (fs::is_regular_file(path) && !fs::is_symlink(path) && fs::file_size(path) < 2048)
					std::cerr << "PRIVATE_CLAUDE_NATIVE_CONTROL_BOUNDARY " << ReadFile(path) << std::endl;
			}
			catch (...) { std::cerr << "PRIVATE_CLAUDE_NATIVE_CONTROL_BOUNDARY unavailable" << std::endl; }
		}
	} native_control_boundary{fixture.temp.root / "mcp-manager-boundary.json"};
	const auto await = [&](auto done)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!done() && std::chrono::steady_clock::now() < deadline)
		{
			const auto before_control = control_boundary();
			(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
			const auto after_control = control_boundary();
			for (const auto& row : before_control.at("watched"))
				if (before_control.at("root_birth_current") == true && row.value("retained", false) &&
				    row.value("same_session", false) && row.value("provider_matches", false) && row.value("session_capability_matches", false) &&
				    row.value("not_expired", false) && row.value("pending_local_launch", false) && !row.value("running", true))
				{
					const auto retained = std::ranges::find_if(after_control.at("watched"), [&](const auto& entry) { return entry.at("ordinal") == row.at("ordinal"); });
					UAM_ASSERT(retained != after_control.at("watched").end() && retained->value("retained", false) && retained->value("directory_exists", false));
					++retained_pending_observations;
				}
			const nlohmann::json pair{{"before", before_control}, {"after", after_control}};
			++control_poll_ordinal;
			if (pair != previous_control_pair && control_changes_reported < 64)
			{
				std::cerr << "PRIVATE_CLAUDE_MANAGER_POLL_BOUNDARY " << nlohmann::json{{"poll", control_poll_ordinal}, {"state", pair}}.dump() << std::endl;
				++control_changes_reported;
			}
			previous_control_pair = pair;
			fixture.Pulse();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT(done());
	};
	CefRefPtr<ManagedClaudeLifecycleCallback> callback = new ManagedClaudeLifecycleCallback;
	NativeMediaRefreshHandlerTestAccess::Send(*fixture.handler, {{"chatId", fixture.root_id}, {"text", parent_task}}, callback);
	await([&] { return callback->successes + callback->failures != 0; });
	if (callback->failures) throw TestFailure("Joined Claude ordinary Send: " + callback->error.substr(0, 512));
	UAM_ASSERT_EQ(callback->successes, 1);
	UAM_ASSERT(!callback->body.contains("managedMention"));
	await([&] { return fs::is_regular_file(fixture.temp.root / "delegated.json"); });
	const auto delegated = nlohmann::json::parse(ReadFile(fixture.temp.root / "delegated.json"));
	fixture.run_id = delegated.at("runId").get<std::string>();
	await([&] { const auto run = fixture.ReadRun(); return run.id == fixture.run_id && run.status == "completed"; });
	UAM_ASSERT(fs::is_regular_file(fixture.temp.root / "child-input-witness.json"));
	const auto child = nlohmann::json::parse(ReadFile(fixture.temp.root / "child-input-witness.json"));
	fixture.owned_pids.insert(child.at("pid").get<pid_t>());
	const auto run = fixture.ReadRun();
	UAM_ASSERT(!run.managed_continuation); // Actual native MCP delegate route, not the distinct UserRoot mention producer.
	UAM_ASSERT(!run.deliver_result_to_root_chat && !run.root_result_delivered); // This route requires public agent_status; automatic wake/result delivery is a separate gap.
	UAM_ASSERT_EQ(run.provider_id, std::string("claude-cli"));
	UAM_ASSERT_EQ(run.model_id, fixture.model);
	UAM_ASSERT_EQ(run.effective_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(run.result_excerpt, fixture.answer);
	const auto saved_child = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, run.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(saved_child && saved_child->native_session_id == child.at("native").get<std::string>());
	UAM_ASSERT_EQ(saved_child->execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(saved_child->workspace_directory, fixture.app.chats.front().workspace_directory);
	UAM_ASSERT_EQ(saved_child->model_id, fixture.model);
	UAM_ASSERT_EQ(saved_child->approval_mode, std::string("plan"));
	UAM_ASSERT_EQ(std::ranges::count(saved_child->messages, fixture.task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(saved_child->messages, fixture.answer, &Message::content), 1);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "request-terminal-status", "owner observed actual durable child completion\n"));
	await([&] { return fs::is_regular_file(fixture.temp.root / "terminal-status.json") && fs::is_regular_file(fixture.temp.root / "mcp-reaped.json"); });
	const auto status = nlohmann::json::parse(ReadFile(fixture.temp.root / "terminal-status.json"));
	UAM_ASSERT_EQ(status.at("runId").get<std::string>(), run.id);
	UAM_ASSERT_EQ(status.at("status").get<std::string>(), std::string("completed"));
	UAM_ASSERT_EQ(status.at("result").get<std::string>(), fixture.answer);
	UAM_ASSERT_EQ(status.at("transcriptChatId").get<std::string>(), run.transcript_chat_id);
	await([&]
	{
		const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
		return saved && std::ranges::count(saved->messages, parent_answer, &Message::content) == 1;
	});
	const auto saved_parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	const auto parent_launch = nlohmann::json::parse(ReadFile(fixture.temp.root / "parent-launch.json"));
	UAM_ASSERT(saved_parent && saved_parent->native_session_id == parent_launch.at("native").get<std::string>());
	UAM_ASSERT(saved_parent->native_session_id != saved_child->native_session_id);
	UAM_ASSERT_EQ(saved_parent->execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(saved_parent->model_id, fixture.model);
	UAM_ASSERT(saved_parent->messages.size() >= fixture.original.size() + 2);
	UAM_ASSERT(std::equal(fixture.original.begin(), fixture.original.end(), saved_parent->messages.begin()));
	UAM_ASSERT_EQ(std::ranges::count(saved_parent->messages, parent_task, &Message::content), 1);
	std::size_t unsolicited_results = 0;
	for (const auto& message : saved_parent->messages)
		for (const auto& tool : message.tool_calls)
			if (tool.id == "uam-agent-result-" + run.id) ++unsolicited_results;
	UAM_ASSERT_EQ(unsolicited_results, std::size_t{0});
	for (const std::string role : {"parent", "child"})
	{
		std::istringstream lines(ReadFile(fixture.temp.root / (role + "-inputs.jsonl")));
		std::string line; std::size_t count = 0;
		while (std::getline(lines, line)) if (!line.empty()) { (void)nlohmann::json::parse(line); ++count; }
		UAM_ASSERT_EQ(count, std::size_t{1});
	}
	UAM_ASSERT_EQ(fixture.ReadRun().id, fixture.run_id);
	UAM_ASSERT(retained_pending_observations > 0);
	UAM_ASSERT_EQ(fixture.app.agent_runs.size(), std::size_t{1});
	UAM_ASSERT(fixture.StopAndReap());
}

namespace
{
	// Each negative starts with a real ordinary Send and captures its actual deferred launch,
	// immediately before the same public housekeeping boundary. No running capability is fabricated.
	uam::UamControlCapability BeginPrivatePendingClaudeControl(ManagedClaudeLifecycleFixture& fixture, bool fail_configuration = false)
	{
		auto& app = fixture.app;
		app.settings.git_enabled = app.settings.git_auto_worktree = app.settings.git_snapshot_plain_folders = false;
		auto& root = app.chats.front();
		root.uam_control_enabled = true; root.uam_control_explicitly_disabled = false;
		root.uam_agent_id = "build"; root.approval_mode = "default"; root.command_safety_tier = "yolo";
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, root));
		if (fail_configuration)
			UAM_ASSERT(uam::io::WriteTextFile(app.data_root / "runtime" / "provider-setup", "Private deliberate regular-file configuration blocker.\n"));
		std::string error;
		UAM_ASSERT(uam::UamControlService::Initialize(app, &error) && error.empty());
		UAM_ASSERT(uam::SendAcpPrompt(app, fixture.root_id, "Prepare one private ordinary Claude launch.", &error));
		const auto capture = [&]() -> std::optional<uam::UamControlCapability>
		{
			const auto* session = uam::FindAcpSessionForChat(app, fixture.root_id);
			if (!session || !session->pending_local_launch || session->running || session->uam_control_capability_id.empty()) return std::nullopt;
			const auto control = std::ranges::find(app.uam_control_capabilities, session->uam_control_capability_id, &uam::UamControlCapability::id);
			UAM_ASSERT(control != app.uam_control_capabilities.end());
			UAM_ASSERT(session->child_pid <= 0 && fs::is_directory(control->directory));
			return *control;
		};
		if (const auto control = capture()) return *control;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (std::chrono::steady_clock::now() < deadline)
		{
			fixture.Pulse(); ProviderCliCompatibilityService().Poll(app);
			if (const auto control = capture()) return *control; // Immediately before public housekeeping.
			(void)uam::UamControlService::ProcessPendingRequests(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		throw TestFailure("The real ordinary Claude Send never reached its deferred local configuration boundary.");
	}
}

UAM_TEST(ClaudePendingControlRetainsRecordWithoutCallableAuthority)
{
	ManagedClaudeLifecycleFixture fixture(true);
	JoinedClaudeControlCleanup cleanup{fixture};
	const auto control = BeginPrivatePendingClaudeControl(fixture);
	const auto now = uam::time::SystemEpochMillisecondsNow();
	UAM_ASSERT(uam::acp_detail::ShouldRetainPendingLocalAcpControlCapability(fixture.app, control, now, uam::GetAppTimeSeconds()));
	const auto response = uam::UamControlService::HandleRequestForTests(fixture.app, control.id,
	    {{"requestId", "pending-record-is-not-authority"}, {"method", "agent_list"}, {"arguments", nlohmann::json::object()}}, now);
	UAM_ASSERT(!response.value("ok", false));
	UAM_ASSERT(response.at("error").get<std::string>().find("stale, expired, revoked") != std::string::npos);
	(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
	UAM_ASSERT(std::ranges::find(fixture.app.uam_control_capabilities, control.id, &uam::UamControlCapability::id) != fixture.app.uam_control_capabilities.end());
	UAM_ASSERT(fs::is_directory(control.directory));
	UAM_ASSERT(fixture.app.agent_runs.empty());
	// Query-only future time proves the captured, normalized setup deadline is finite; this
	// does not accelerate or shorten a runtime timeout or grant a request any authority.
	UAM_ASSERT(!uam::acp_detail::ShouldRetainPendingLocalAcpControlCapability(fixture.app, control, now,
	    uam::GetAppTimeSeconds() + uam::settings::kMaxAcpSetupInactivityTimeoutSeconds + 1));
	UAM_ASSERT(uam::StopAcpSession(fixture.app, fixture.root_id));
	(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
	UAM_ASSERT(fixture.app.uam_control_capabilities.empty() && !fs::exists(control.directory));
}

UAM_TEST(ClaudePendingControlDropsStaleCanceledExpiredAndUnreservedOwners)
{
	for (const std::string mutation : {"root-birth", "workspace", "manager-generation", "cancel", "expired", "reservation", "stop"})
	{
		ManagedClaudeLifecycleFixture fixture(true);
		JoinedClaudeControlCleanup cleanup{fixture};
		const auto control = BeginPrivatePendingClaudeControl(fixture);
		auto& app = fixture.app;
		auto* session = uam::FindAcpSessionForChat(app, fixture.root_id);
		UAM_ASSERT(session && !session->running);
		UAM_ASSERT(uam::acp_detail::ShouldRetainPendingLocalAcpControlCapability(app, control, uam::time::SystemEpochMillisecondsNow(), uam::GetAppTimeSeconds()));
		if (mutation == "root-birth") app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		else if (mutation == "workspace") app.chats.front().workspace_directory += "/replaced-private-workspace";
		else if (mutation == "manager-generation") app.uam_control_manager_id += "-replaced";
		else if (mutation == "cancel") session->cancel_requested = true;
		else if (mutation == "expired") app.uam_control_capabilities.front().expires_at_epoch_ms = 0;
		else if (mutation == "reservation") app.local_acp_launch_tasks.clear();
		else UAM_ASSERT(uam::StopAcpSession(app, fixture.root_id));
		(void)uam::UamControlService::ProcessPendingRequests(app);
		UAM_ASSERT(std::ranges::find(app.uam_control_capabilities, control.id, &uam::UamControlCapability::id) == app.uam_control_capabilities.end());
		UAM_ASSERT(!fs::exists(control.directory));
		if (mutation == "expired")
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (session->pending_local_launch && std::chrono::steady_clock::now() < deadline)
			{
				std::string error;
				(void)uam::acp_detail::StartAcpProcessForChat(app, *session, app.chats.front(), &error);
				UAM_ASSERT(!session->running && session->child_pid <= 0);
				if (session->pending_local_launch) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(!session->pending_local_launch && session->last_error.find("UAM control setup expired") != std::string::npos);
		}
		UAM_ASSERT(app.agent_runs.empty());
		const auto* remaining = uam::FindAcpSessionForChat(app, fixture.root_id);
		UAM_ASSERT(!remaining || (!remaining->running && remaining->child_pid <= 0));
	}
}

UAM_TEST(ClaudeFailedPendingConfigurationDropsControlAndNeverStartsNativeProcess)
{
	ManagedClaudeLifecycleFixture fixture(true);
	JoinedClaudeControlCleanup cleanup{fixture};
	const auto control = BeginPrivatePendingClaudeControl(fixture, true);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (uam::acp_detail::ShouldRetainPendingLocalAcpControlCapability(fixture.app, control,
	           uam::time::SystemEpochMillisecondsNow(), uam::GetAppTimeSeconds()) && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(!uam::acp_detail::ShouldRetainPendingLocalAcpControlCapability(fixture.app, control,
	    uam::time::SystemEpochMillisecondsNow(), uam::GetAppTimeSeconds()));
	(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
	UAM_ASSERT(fixture.app.uam_control_capabilities.empty() && !fs::exists(control.directory));
	auto* session = uam::FindAcpSessionForChat(fixture.app, fixture.root_id);
	auto* chat = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
	UAM_ASSERT(session && chat && session->pending_local_launch && !session->running);
	std::string error;
	UAM_ASSERT(!uam::acp_detail::StartAcpProcessForChat(fixture.app, *session, *chat, &error));
	UAM_ASSERT(!error.empty() && error.find("UAM control setup expired") == std::string::npos);
	UAM_ASSERT(!session->pending_local_launch && !session->running && session->child_pid <= 0);
	UAM_ASSERT(fixture.app.agent_runs.empty());
}
#endif

// Private protocol peers exercise the genuine UAM control implementation, never a live account.
#if UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_RUNTIME_COPILOT_CLI && defined(__APPLE__)
#include "app/uam_control_service.h"
namespace uam
{
	bool PendingChatSavePreparationForTests(const AppState&, const std::string&, bool&);
	bool PendingChatSavePublicationForTests(const AppState&, const std::string&, bool&);
	bool PendingOwnedChatPublicationForTests(const AppState&, const std::string&, bool&);
}
namespace
{
	std::string CopilotJoinedDelegatePeer()
	{
		return R"UAM_JOINED(#!/usr/bin/python3
import json, os, pathlib, select, signal, subprocess, sys, threading, time, uuid
root = pathlib.Path(__file__).resolve().parent
config = json.loads((root/'copilot-peer-config.json').read_text())
lock = threading.Lock()
stop = threading.Event()
mcp = None
worker = None
native = str(uuid.uuid4())
setup = None
inputs = 0
last_signal = 0
input_eof = False
role = "pre-input"
def store(name, value):
    target = root/(name+'-'+str(os.getpid())+'.json')
    temporary = target.with_suffix('.tmp')
    with temporary.open('w') as f:
        json.dump(value, f, sort_keys=True); f.flush(); os.fsync(f.fileno())
    os.replace(temporary, target)
def emit(value):
    raw = json.dumps(value, separators=(',',':')).encode()
    with lock:
        sys.stdout.buffer.write(('Content-Length: '+str(len(raw))+'\r\n\r\n').encode()+raw)
        sys.stdout.buffer.flush()
def reply(message, value): emit(dict(jsonrpc='2.0', id=message['id'], result=value))
def event(kind, data):
    emit(dict(jsonrpc='2.0', method='session.event', params=dict(sessionId=native,
        event=dict(id=str(uuid.uuid4()),parentId=None,timestamp='2026-10-19T08:00:00Z',type=kind,data=data))))
def text_result(response):
    assert not response.get('error') and not response['result'].get('isError',False), response
    text = response['result']['content']
    assert len(text)==1 and text[0]['type']=='text'
    return json.loads(text[0]['text'])
def parent_work():
    global mcp
    try:
        server = setup['mcpServers']['uam-control']
        assert server['command']==config['test_executable'] and server['args']==['--uam-control-mcp']
        env = dict(os.environ); env.update(server.get('env',{}))
        mcp = subprocess.Popen([server['command']]+server['args'], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, bufsize=0)
        store('mcp-launch',dict(pid=mcp.pid,command=server['command'],args=server['args']))
        buffered = b''
        def rpc(i, method, params):
            nonlocal buffered
            mcp.stdin.write((json.dumps(dict(jsonrpc='2.0',id=i,method=method,params=params))+'\n').encode()); mcp.stdin.flush()
            deadline = time.monotonic()+20
            while time.monotonic()<deadline and not stop.is_set():
                while b'\n' in buffered:
                    line,buffered=buffered.split(b'\n',1)
                    response=json.loads(line)
                    if response.get('id')==i: return response
                assert mcp.poll() is None, 'actual MCP server exited before response'
                ready,_,_=select.select([mcp.stdout.fileno()],[],[],.005)
                if ready:
                    chunk=os.read(mcp.stdout.fileno(),65536)
                    assert chunk and len(buffered)+len(chunk)<=1024*1024
                    buffered+=chunk
            raise RuntimeError('bounded actual MCP response did not complete')
        initialized=rpc(1,'initialize',dict(protocolVersion='2025-03-26',capabilities={},clientInfo=dict(name='private-copilot-peer',version='1')))
        assert initialized['result']['serverInfo']['name']=='uam-control'
        mcp.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n'); mcp.stdin.flush()
        tools=rpc(2,'tools/list',{})['result']['tools']
        assert any(t['name']=='agent_delegate' for t in tools)
        delegated=text_result(rpc(3,'tools/call',dict(name='agent_delegate',arguments=dict(
            agentId='build',task=config['child_task'],providerId='copilot-cli',modelId=config['model']))))
        assert delegated['status']=='queued' and delegated['runId']
        store('delegate',dict(native=native,response=delegated))
        run_id=delegated['runId']; status=None; sequence=4; deadline=time.monotonic()+25
        while time.monotonic()<deadline and not stop.is_set():
            status=text_result(rpc(sequence,'tools/call',dict(name='agent_status',arguments=dict(runId=run_id))))
            sequence+=1
            if status['status'] not in ('queued','running'): break
            time.sleep(1)
        assert status and status['status']=='completed' and status['result']==config['child_answer'], status
        assert status['runId']==run_id and status['providerId']=='copilot-cli' and status['modelId']==config['model']
        store('parent-visible-result',dict(native=native,response=status))
        mcp.stdin.close()
        code=mcp.wait(timeout=10)
        assert code==0
        store('mcp-exit',dict(pid=mcp.pid,exit=code,forced=False))
        event('assistant.message',dict(messageId=str(uuid.uuid4()),content=config['parent_answer']+status['result']))
        event('session.idle',{})
        store('parent-complete',dict(native=native,run_id=run_id))
    except BaseException as error:
        store('joined-error',dict(error=type(error).__name__+': '+str(error)[:400]))
        stop.set()
def request(message):
    global setup,inputs,worker,role
    method=message.get('method','')
    if method=='ping': reply(message,dict(protocolVersion=3))
    elif method=='models.list': reply(message,dict(models=[dict(id=config['model'],name='Private explicit model')]))
    elif method=='session.model.list': reply(message,dict(list=[dict(id=config['model'],name='Private explicit model')]))
    elif method=='session.model.getCurrent': reply(message,dict(modelId=config['model']))
    elif method=='session.create':
        assert setup is None and not inputs, 'session.create repeated or follows native input'
        setup=message['params']
        assert setup['workingDirectory']==config['workspace'], 'session.create workspace mismatches selected private workspace'
        assert setup['model']==config['model'], 'session.create model mismatches explicit selected model'
        assert 'uam-control' in setup['mcpServers'], 'session.create lacks advertised UAM control MCP server'
        store('setup',dict(native=native,request=message,pid=os.getpid()))
        reply(message,dict(sessionId=native))
    elif method=='session.send':
        inputs+=1; assert inputs==1 and setup
        p=message['params']; assert p['sessionId']==native and p['mode']=='enqueue' and p['agentMode']=='interactive'
        parent=config['parent_task'] in p['prompt']; expected=config['parent_task'] if parent else config['child_task']
        role = 'parent' if parent else 'managed-child'
        assert expected in p['prompt']
        # Read actual persisted dispatch evidence; never write a receipt or construct readiness.
        data=pathlib.Path(config['data_root'])
        if parent: chat_id=config['root_id']
        else:
            runs=[json.loads(f.read_text()) for f in (data/'agent-runs').glob('*.json')]
            runs=[r for r in runs if r['rootChatId']==config['root_id']]
            assert len(runs)==1 and runs[0]['providerId']=='copilot-cli' and runs[0]['modelId']==config['model']
            assert runs[0]['status']=='running' and runs[0]['effectiveWorkspaceAccess']=='write'
            chat_id=runs[0]['transcriptChatId']
        chat=json.loads((data/'chats'/(chat_id+'.json')).read_text())
        assert chat['native_session_id']==native and chat['execution_host_id']=='local'
        assert chat['workspace_directory']==config['workspace'] and chat['model_id']==config['model']
        rows=[r for r in chat['messages'] if r['role']=='user' and r['content']==expected]
        assert len(rows)==1 and not rows[0].get('acp_prompt_not_sent',False)
        store('input',dict(parent=parent,native=native,chat_id=chat_id,request=message,pid=os.getpid()))
        reply(message,dict(messageId=str(uuid.uuid4())))
        event('assistant.turn_start',{})
        if parent:
            worker=threading.Thread(target=parent_work); worker.start()
        else:
            event('assistant.message',dict(messageId=str(uuid.uuid4()),content=config['child_answer']))
            event('session.idle',{})
    elif method in ('session.abort','session.destroy'):
        stop.set(); reply(message,{})
    else: raise RuntimeError('unexpected private SDK method: '+method)
try:
    if sys.argv[1:]==['--version']:
        print('GitHub Copilot CLI 1.0.98',flush=True); sys.exit(0)
    assert '--headless' in sys.argv and '--stdio' in sys.argv and '--no-auto-update' in sys.argv
    def stopped_by_signal(signum, frame):
        global last_signal
        last_signal = signum; stop.set()
    signal.signal(signal.SIGTERM,stopped_by_signal)
    store('launch',dict(pid=os.getpid(),args=sys.argv[1:]))
    buffer=b''
    while not stop.is_set():
        ready,_,_=select.select([sys.stdin.fileno()],[],[],.005)
        if not ready: continue
        chunk=os.read(sys.stdin.fileno(),65536)
        if not chunk: input_eof = True; break
        buffer+=chunk
        assert len(buffer)<=16*1024*1024+8192
        while b'\r\n\r\n' in buffer:
            end=buffer.index(b'\r\n\r\n'); assert end<=8192
            sizes=[int(x.split(':',1)[1].strip()) for x in buffer[:end].decode('ascii').split('\r\n') if x.lower().startswith('content-length:')]
            assert len(sizes)==1 and 0<sizes[0]<=16*1024*1024
            consumed=end+4+sizes[0]
            if len(buffer)<consumed: break
            frame,buffer=buffer[:consumed],buffer[consumed:]
            request(json.loads(frame[end+4:]))
except BaseException as error:
    if not isinstance(error,SystemExit): store('joined-error',dict(error=type(error).__name__+': '+str(error)[:400]))
    raise
finally:
    if sys.argv[1:] != ['--version']:
        try:
            store('peer-exit',dict(pid=os.getpid(),role=role,inputs=inputs,setup_seen=setup is not None,
                signal=last_signal,stdin_eof=input_eof,stop_requested=stop.is_set()))
        except BaseException:
            pass # Diagnostic failure must never skip mandatory owned process cleanup.
    stop.set()
    if mcp is not None and mcp.poll() is None:
        mcp.terminate()
        try: mcp.wait(timeout=2)
        except subprocess.TimeoutExpired: mcp.kill(); mcp.wait(timeout=2)
    if worker is not None: worker.join(timeout=3)
)UAM_JOINED";
	}
}

UAM_TEST(CopilotLocalSdkParentDelegatesThroughActualMcpAndReadsChildResult)
{
	ManagedCopilotBindingFixtureR124 fixture;
	const std::string parent_task = "Use the real UAM delegate tool and inspect its returned child result.";
	const std::string child_task = "Return the private Copilot joined child result.";
	const std::string child_answer = "PRIVATE-COPILOT-JOINED-CHILD-RESULT";
	const std::string parent_answer = "The actual UAM control tool returned: ";
	auto* root = ChatDomainService().FindChatById(fixture.app, fixture.root_id);
	UAM_ASSERT(root);
	// This joined SDK/MCP fixture selects the existing private workspace, not a Git worktree.
	fixture.app.settings.git_enabled = fixture.app.settings.git_auto_worktree = false;
	root->workspace_isolation_kind.clear();
	root->uam_control_enabled = true; root->uam_control_explicitly_disabled = false;
	root->uam_agent_id = "build"; root->approval_mode = "off"; root->command_safety_tier = "yolo";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *root));
	const auto model = root->model_id;
	const auto workspace = root->workspace_directory;
	const auto executable = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "copilot-peer.py", CopilotJoinedDelegatePeer()));
	fs::permissions(fixture.temp.root / "copilot-peer.py", fs::perms::owner_all, fs::perm_options::replace);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "copilot-peer-config.json", nlohmann::json{
	    {"data_root", fixture.app.data_root.string()}, {"root_id", fixture.root_id}, {"workspace", workspace},
	    {"model", model}, {"parent_task", parent_task}, {"child_task", child_task}, {"child_answer", child_answer},
	    {"parent_answer", parent_answer}, {"test_executable", executable.string()}}.dump()));
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(fixture.app, &error));
	std::set<pid_t> pids;
	const auto observe = [&]
	{
		for (const auto& session : fixture.app.acp_sessions)
			if (session && session->child_pid > 0) pids.insert(session->child_pid);
	};
	const std::string original_manager = fixture.app.uam_control_manager_id;
	nlohmann::json first_failed_phases = nullptr;
	const auto snapshot = [&](const char* phase)
	{
		nlohmann::json facts{{"phase", phase}, {"manager_present", !fixture.app.uam_control_manager_id.empty()},
		    {"manager_same", fixture.app.uam_control_manager_id == original_manager}, {"capabilities", fixture.app.uam_control_capabilities.size()},
		    {"save_tasks", fixture.app.pending_chat_save_tasks.size()}, {"save_due", fixture.app.pending_chat_save_at_by_chat_id.size()}, {"sessions", nlohmann::json::array()}};
		for (const auto& session : fixture.app.acp_sessions)
		{
			if (!session || session->provider_id != "copilot-cli" || facts["sessions"].size() >= 4) continue;
			const auto cap = std::ranges::find(fixture.app.uam_control_capabilities, session->uam_control_capability_id, &uam::UamControlCapability::id);
			facts["sessions"].push_back({{"role", session->chat_id == fixture.root_id ? "parent" : "managed-child"}, {"pid", session->child_pid},
			    {"last_process_id", session->last_process_id}, {"running", session->running}, {"initialized", session->initialized}, {"ready", session->session_ready},
			    {"processing", session->processing}, {"lifecycle", session->lifecycle_state}, {"native_present", !session->session_id.empty()},
			    {"setup_pending", session->session_setup_request_id != 0}, {"pending_requests", session->pending_request_methods.size()},
			    {"local_launch", static_cast<bool>(session->pending_local_launch)}, {"local_receipt", static_cast<bool>(session->pending_local_session_receipt)},
			    {"launch_attempted", session->managed_launch_attempted}, {"capability_present", cap != fixture.app.uam_control_capabilities.end()},
			    {"capability_owner_matches", cap != fixture.app.uam_control_capabilities.end() && cap->session_chat_id == session->chat_id && cap->provider_id == session->provider_id},
			    {"capability_initialized", cap != fixture.app.uam_control_capabilities.end() && cap->mcp_initialized},
			    {"has_last_exit", session->has_last_exit_code}, {"last_exit", session->last_exit_code},
			    {"exit_observed", session->process_exit_observed}, {"observed_exit", session->observed_process_exit_code},
			    {"stop_outcome", static_cast<int>(session->stop_outcome)}, {"local_stop_pending", session->local_stop_pending},
			    {"last_error", session->last_error.substr(0, 768)}, {"stderr_tail", session->recent_stderr.substr(session->recent_stderr.size() > 2048 ? session->recent_stderr.size() - 2048 : 0)}});
		}
		return facts;
	};
	const auto pulse = [&]
	{
		fixture.AssertPeer(); observe();
		ProviderCliCompatibilityService().Poll(fixture.app);
		auto phases = nlohmann::json::array({snapshot("before_control")});
		(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
		phases.push_back(snapshot("after_control"));
		(void)uam::PollAllAcpSessions(fixture.app);
		phases.push_back(snapshot("after_provider_poll"));
		(void)uam::AgentRunScheduler::Poll(fixture.app);
		phases.push_back(snapshot("after_scheduler"));
		if (first_failed_phases.is_null() && std::ranges::any_of(fixture.app.agent_runs, [](const auto& run) { return run.status == "failed"; })) first_failed_phases = phases;
		(void)uam::FlushPendingChatSaves(fixture.app);
		observe();
		std::string peer_failures;
		auto files = nlohmann::json::array(), exits = nlohmann::json::array();
		unsigned final_errors = 0, temporary_errors = 0;
		for (const auto& entry : fs::directory_iterator(fixture.temp.root))
		{
			const auto name = entry.path().filename().string();
			if (name.starts_with("joined-error-"))
			{
				if (entry.path().extension() == ".json") { ++final_errors; peer_failures += name + ": " + ReadFile(entry.path()) + "\n"; }
				else ++temporary_errors; // Never parse incomplete atomic-publication files.
			}
			if (files.size() < 32 && entry.path().extension() == ".json" &&
			    (name.starts_with("launch-") || name.starts_with("setup-") || name.starts_with("input-") || name.starts_with("peer-exit-")))
				files.push_back(name); // Private PID filenames correlate the actual parent and child.
		}
		if (!peer_failures.empty())
		{
			auto saved = nlohmann::json::array();
			for (const auto& chat : fixture.app.chats)
			{
				if (saved.size() >= 4) break;
				nlohmann::json row{{"role", chat.id == fixture.root_id ? "parent" : "managed-child"}, {"readable", false}};
				try
				{
					const auto raw = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, chat.id)));
					row.update({{"readable", true}, {"native_present", !raw.value("native_session_id", "").empty()},
					    {"native_matches_live", raw.value("native_session_id", "") == chat.native_session_id},
					    {"model_matches_live", raw.value("model_id", "") == chat.model_id}, {"host_matches_live", raw.value("execution_host_id", "") == chat.execution_host_id},
					    {"saved_messages", raw.contains("messages") && raw["messages"].is_array() ? raw["messages"].size() : 0},
					    {"live_messages", chat.messages.size()}, {"save_pending", fixture.app.pending_chat_save_tasks.contains(chat.id)}});
				}
				catch (...) {}
				saved.push_back(std::move(row));
			}
			for (const auto& name : files) if (name.get<std::string>().starts_with("peer-exit-"))
				try { exits.push_back(nlohmann::json::parse(ReadFile(fixture.temp.root / name.get<std::string>()))); } catch (...) {}
			std::cerr << "copilot_joined_private_failure_facts=" << nlohmann::json{{"first_failed_phases", first_failed_phases}, {"current_phases", phases},
			    {"final_error_files", final_errors}, {"temporary_error_files", temporary_errors}, {"private_files", files}, {"peer_exits", exits}, {"saved_chats", saved}}.dump() << std::endl;
			throw TestFailure("Private joined Copilot peers failed: " + peer_failures);
		}
	};
	// Ordinary Interrupt does not publish a native stop-outcome receipt. Prove the
	// two actually observed native processes are gone, independently of that label.
	const auto native_processes_reaped = [&]
	{
		if (pids.size() != 2) return false;
		return std::ranges::all_of(pids, [](pid_t pid)
		{
			errno = 0;
			return pid > 0 && kill(pid, 0) < 0 && errno == ESRCH;
		});
	};
	const auto wait = [&](const char* phase, auto predicate)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
		while (!predicate() && std::chrono::steady_clock::now() < deadline)
		{ pulse(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
		const bool completed = predicate();
		if (!completed)
		{
			try
			{
				// Failure-only observations; never harvest, force-save, clear work or change the deadline.
				const auto* parent = uam::FindAcpSessionForChat(fixture.app, fixture.root_id);
				nlohmann::json facts{{"phase", phase}, {"latched_predicate", completed},
				    {"parent_present", parent != nullptr}, {"parent_idle", parent && !parent->processing},
				    {"run_count", fixture.app.agent_runs.size()},
				    {"one_completed_run", fixture.app.agent_runs.size() == 1 && fixture.app.agent_runs.front().status == "completed"},
				    {"save_tasks_empty", fixture.app.pending_chat_save_tasks.empty()},
				    {"save_due_empty", fixture.app.pending_chat_save_at_by_chat_id.empty()},
				    {"save_task_count", fixture.app.pending_chat_save_tasks.size()},
				    {"save_due_count", fixture.app.pending_chat_save_at_by_chat_id.size()},
				    {"retry_owner_count", fixture.app.pending_chat_save_retry_owners.size()},
				    {"durable_parent_readable", false}, {"durable_answer_count", 0},
				    {"durable_read_exception", false}, {"live_answer_count", 0}, {"chats", nlohmann::json::array()}};
				try
				{
					const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id);
					facts["durable_parent_readable"] = static_cast<bool>(saved);
					if (saved) facts["durable_answer_count"] = std::ranges::count(saved->messages, parent_answer + child_answer, &Message::content);
				}
				catch (...) { facts["durable_read_exception"] = true; }
				for (const auto& chat : fixture.app.chats)
				{
					if (facts["chats"].size() >= 8) break;
					if (chat.id == fixture.root_id) facts["live_answer_count"] = std::ranges::count(chat.messages, parent_answer + child_answer, &Message::content);
					bool prepared_ready = false, published_ready = false, owned_ready = false;
					const bool prepared_present = uam::PendingChatSavePreparationForTests(fixture.app, chat.id, prepared_ready);
					const bool published_present = uam::PendingChatSavePublicationForTests(fixture.app, chat.id, published_ready);
					const bool owned_present = uam::PendingOwnedChatPublicationForTests(fixture.app, chat.id, owned_ready);
					const auto due = fixture.app.pending_chat_save_at_by_chat_id.find(chat.id);
					const auto* session = uam::FindAcpSessionForChat(fixture.app, chat.id);
					facts["chats"].push_back({{"parent", chat.id == fixture.root_id},
					    {"save_task_present", fixture.app.pending_chat_save_tasks.contains(chat.id)},
					    {"save_due_present", due != fixture.app.pending_chat_save_at_by_chat_id.end()},
					    {"save_due_now", due != fixture.app.pending_chat_save_at_by_chat_id.end() && due->second <= uam::GetAppTimeSeconds()},
					    {"retry_owner_present", fixture.app.pending_chat_save_retry_owners.contains(chat.id)},
					    {"preparation_present", prepared_present}, {"preparation_ready", prepared_ready},
					    {"publication_present", published_present}, {"publication_ready", published_ready},
					    {"owned_publication_present", owned_present}, {"owned_publication_ready", owned_ready},
					    {"queue_storage_error", chat.acp_queue_storage_error},
					    {"worktree_operation", fixture.app.worktree_operation_chat_ids.contains(chat.id)},
					    {"session_present", session != nullptr}, {"session_error_present", session && !session->last_error.empty()},
					    {"processing", session && session->processing}, {"running", session && session->running},
					    {"pending_requests", session ? session->pending_request_methods.size() : 0}});
				}
				facts["stop_sessions"] = nlohmann::json::array();
				facts["all_stop_sessions_satisfy"] = std::ranges::all_of(fixture.app.acp_sessions, [](const auto& session)
				{ return !session || (!session->running && !session->local_stop_pending && !session->remote_stop_pending && !session->remote_stop_unconfirmed && !session->pending_local_session_receipt &&
				    session->child_pid <= 0 && !session->stdin_writer &&
				    session->stdin_write_fd < 0 && session->stdout_read_fd < 0 && session->stderr_read_fd < 0 &&
				    session->stop_outcome != uam::platform::ProcessStopOutcome::Failed); });
				facts["native_pid_count"] = pids.size();
				facts["native_processes_reaped"] = native_processes_reaped();
				facts["stop_tasks_empty"] = fixture.app.acp_process_stop_tasks.empty();
				facts["local_launch_tasks_empty"] = fixture.app.local_acp_launch_tasks.empty();
				facts["local_launch_task_count"] = fixture.app.local_acp_launch_tasks.size();
				facts["remote_stop_count"] = fixture.app.pending_acp_remote_stops.size();
				for (const auto& session : fixture.app.acp_sessions)
				{
					if (!session || facts["stop_sessions"].size() >= 8) continue;
					facts["stop_sessions"].push_back({{"parent", session->chat_id == fixture.root_id},
					    {"running", session->running}, {"local_stop_pending", session->local_stop_pending},
					    {"remote_stop_pending", session->remote_stop_pending}, {"remote_stop_unconfirmed", session->remote_stop_unconfirmed},
					    {"local_receipt_present", static_cast<bool>(session->pending_local_session_receipt)},
					    {"local_launch_present", static_cast<bool>(session->pending_local_launch)},
					    {"stop_outcome", static_cast<int>(session->stop_outcome)},
					    {"stop_outcome_known", session->stop_outcome != uam::platform::ProcessStopOutcome::Unknown},
					    {"stop_outcome_not_failed", session->stop_outcome != uam::platform::ProcessStopOutcome::Failed},
					    {"process_handle_label_present", !session->last_process_id.empty()},
					    {"child_pid_present", session->child_pid > 0}, {"exit_observed", session->process_exit_observed},
					    {"has_last_exit", session->has_last_exit_code}});
				}
				facts["stop_tasks"] = nlohmann::json::array();
				for (const auto& task : fixture.app.acp_process_stop_tasks)
				{
					if (facts["stop_tasks"].size() >= 8) break;
					const bool finished = task.finished && task.finished->load(std::memory_order_acquire);
					nlohmann::json row{{"parent", task.chat_id == fixture.root_id}, {"finished_flag_present", static_cast<bool>(task.finished)},
					    {"finished", finished}, {"result_present", static_cast<bool>(task.result)},
					    {"owned_process_present", static_cast<bool>(task.owned_process)}, {"observe_exit", task.observe_exit},
					    {"purpose", static_cast<int>(task.purpose)}, {"result_consumed", task.result_consumed},
					    {"worker_present", static_cast<bool>(task.worker)}};
					// Worker result fields are observed only after its actual release/acquire completion.
					if (finished && task.result) row.update({{"exit_confirmed", task.result->exit_confirmed}, {"outcome", static_cast<int>(task.result->outcome)}});
					facts["stop_tasks"].push_back(std::move(row));
				}
				std::cerr << "copilot_joined_timeout_predicate_facts=" << facts.dump() << std::endl;
			}
			catch (...) {} // Diagnostics must not replace or repair the latched failure.
		}
		UAM_ASSERT(completed);
	};
	struct Cleanup
	{
		ManagedCopilotBindingFixtureR124& fixture;
		~Cleanup()
		{
			const bool native_reaped = fixture.StopAndReap();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			bool mcp_gone = false;
			do
			{
				mcp_gone = true;
				for (const auto& entry : fs::directory_iterator(fixture.temp.root))
				{
					if (!entry.path().filename().string().starts_with("mcp-launch-")) continue;
					const auto witness = nlohmann::json::parse(ReadFile(entry.path()));
					const auto pid = witness.at("pid").get<pid_t>();
					errno = 0; if (pid > 0 && (kill(pid, 0) == 0 || errno != ESRCH)) mcp_gone = false;
				}
				if (mcp_gone) break;
				(void)uam::UamControlService::ProcessPendingRequests(fixture.app);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while (std::chrono::steady_clock::now() < deadline);
			if (!native_reaped || !mcp_gone)
			{
				std::cerr << "Joined fixture cleanup unconfirmed; retain private environment instead of claiming Stop." << std::endl;
				std::abort();
			}
			uam::UamControlService::Shutdown(fixture.app);
		}
	} cleanup{fixture};
	UAM_ASSERT(uam::SendAcpPrompt(fixture.app, fixture.root_id, parent_task, {}, {}, false, &error));
	UAM_ASSERT(error.empty());
	wait("completion", [&]
	{
		const auto* parent = uam::FindAcpSessionForChat(fixture.app, fixture.root_id);
		if (!parent || parent->processing || fixture.app.agent_runs.size() != 1 || fixture.app.agent_runs.front().status != "completed" ||
		    !fixture.app.pending_chat_save_tasks.empty() || !fixture.app.pending_chat_save_at_by_chat_id.empty()) return false;
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id);
		return saved && std::ranges::count(saved->messages, parent_answer + child_answer, &Message::content) == 1;
	});
	const auto run = fixture.app.agent_runs.front();
	UAM_ASSERT_EQ(run.root_chat_id, fixture.root_id);
	UAM_ASSERT_EQ(run.provider_id, std::string("copilot-cli")); UAM_ASSERT_EQ(run.model_id, model);
	UAM_ASSERT_EQ(run.effective_workspace_access, std::string("write"));
	UAM_ASSERT_EQ(run.result_excerpt, child_answer);
	const auto child = ChatRepository::LoadLocalChat(fixture.app.data_root, run.transcript_chat_id);
	const auto parent = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id);
	UAM_ASSERT(child && parent);
	UAM_ASSERT_EQ(child->execution_host_id, std::string("local")); UAM_ASSERT_EQ(child->workspace_directory, workspace);
	UAM_ASSERT_EQ(child->model_id, model); UAM_ASSERT(!child->native_session_id.empty());
	UAM_ASSERT(!parent->native_session_id.empty() && parent->native_session_id != child->native_session_id);
	const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
	UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1);
	UAM_ASSERT_EQ(ledger.runs.front().status, std::string("completed"));
	UAM_ASSERT_EQ(ledger.runs.front().result_excerpt, child_answer);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, child_task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, child_answer, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(parent->messages, parent_task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(parent->messages, parent_answer + child_answer, &Message::content), 1);
	std::size_t visible = 0, inputs = 0, mcp_exits = 0;
	for (const auto& entry : fs::directory_iterator(fixture.temp.root))
	{
		const auto name = entry.path().filename().string();
		if (name.starts_with("input-")) ++inputs;
		if (name.starts_with("parent-visible-result-"))
		{
			const auto value = nlohmann::json::parse(ReadFile(entry.path()));
			UAM_ASSERT_EQ(value.at("response").at("runId"), nlohmann::json(run.id));
			UAM_ASSERT_EQ(value.at("response").at("result"), nlohmann::json(child_answer));
			++visible;
		}
		if (name.starts_with("mcp-exit-"))
		{
			const auto value = nlohmann::json::parse(ReadFile(entry.path()));
			UAM_ASSERT(value.at("exit") == 0 && value.at("forced") == false);
			const auto pid = value.at("pid").get<pid_t>();
			errno = 0; UAM_ASSERT(kill(pid, 0) < 0 && errno == ESRCH); ++mcp_exits;
		}
	}
	UAM_ASSERT_EQ(inputs, std::size_t{2}); UAM_ASSERT_EQ(visible, std::size_t{1}); UAM_ASSERT_EQ(mcp_exits, std::size_t{1});
	for (const auto& session : fixture.app.acp_sessions)
		if (session && session->running) (void)uam::StopAcpSession(fixture.app, session->chat_id);
	wait("stop", [&]
	{
		return native_processes_reaped() && std::ranges::all_of(fixture.app.acp_sessions, [](const auto& session)
		{ return !session || (!session->running && !session->local_stop_pending && !session->remote_stop_pending && !session->remote_stop_unconfirmed && !session->pending_local_session_receipt &&
		    session->child_pid <= 0 && !session->stdin_writer &&
		    session->stdin_write_fd < 0 && session->stdout_read_fd < 0 && session->stderr_read_fd < 0 &&
		    session->stop_outcome != uam::platform::ProcessStopOutcome::Failed); }) &&
		    fixture.app.acp_process_stop_tasks.empty() && fixture.app.pending_acp_remote_stops.empty() &&
		    fixture.app.local_acp_launch_tasks.empty() && fixture.app.pending_chat_save_tasks.empty() && fixture.app.pending_chat_save_at_by_chat_id.empty();
	});
	for (const auto pid : pids) { errno = 0; UAM_ASSERT(kill(pid, 0) < 0 && errno == ESRCH); }
	UAM_ASSERT(fixture.StopAndReap());
}
#endif

// Ordinary public UAM tool delegation, distinct from the V2 @mention binding cases above.
#include "app/uam_control_service.h"
#include <cstdlib>
#if UAM_ENABLE_RUNTIME_CODEX_CLI && defined(__APPLE__)
namespace
{
	void CodexToolReplaceOnceR1(std::string& text, std::string_view old, std::string_view replacement)
	{
		const auto position = text.find(old);
		UAM_ASSERT(position != std::string::npos && text.find(old, position + old.size()) == std::string::npos);
		text.replace(position, old.size(), replacement);
	}

	std::string OrdinaryCodexChildPeerR1()
	{
		// Reuse the genuine existing JSONRPC peer, replacing only its V2-mention ledger oracle
		// with checks of the actual ordinary agent_delegate legacy run and raw saved user row.
		std::string peer = ManagedCodexBindingPeerR109();
		const auto begin = peer.find("def state():\n"), end = peer.find("try:\n", begin);
		UAM_ASSERT(begin != std::string::npos && end != std::string::npos);
		peer.replace(begin, end - begin, R"PY(def state():
    data = pathlib.Path(config['data_root'])
    runs = [read(path) for path in (data / 'agent-runs').glob('*.json')]
    runs = [run for run in runs if run['rootChatId'] == config['root_id']]
    assert len(runs) == 1 and runs[0]['version'] == 1
    run = runs[0]
    assert 'managedContinuation' not in run and run['status'] == 'running'
    assert run['providerId'] == 'codex-cli' and run['modelId'] == config['model']
    assert run['task'] == config['task'] and run['effectiveWorkspaceAccess'] == 'read'
    child = read(data / 'chats' / (run['transcriptChatId'] + '.json'))
    assert child['agent_run_id'] == run['id'] and child['execution_host_id'] == 'local'
    assert child['workspace_directory'] == config['workspace']
    return run, child
)PY");
		CodexToolReplaceOnceR1(peer, "run, turn, child = state()\n            assert not child", "run, child = state()\n            assert not child");
		CodexToolReplaceOnceR1(peer, "run, turn, child = state()\n            assert child", "run, child = state()\n            assert child");
		CodexToolReplaceOnceR1(peer,
		    "            assert turn['expectedTurnSerial'] > 0 and run['expectedTurnSerial'] == turn['expectedTurnSerial']\n            index = turn['firstUserMessageIndex']\n            assert index >= 0\n            row = child['messages'][index]\n            assert row['role'] == 'user' and row['content'] == config['task']\n            assert row['created_at'] == turn['userMessageCreatedAt'] and row['created_at']\n",
		    "            assert run['expectedTurnSerial'] > 0\n            rows = [(index, row) for index, row in enumerate(child['messages']) if row['role'] == 'user' and row['content'] == config['task']]\n            assert len(rows) == 1\n            index, row = rows[0]\n            assert row['created_at']\n");
		CodexToolReplaceOnceR1(peer, "request=message, run=run, turn=turn, transcript=child", "request=message, run=run, transcript=child");
		return peer;
	}

	std::string OrdinaryCodexParentPeerR1()
	{
		return R"PY(#!/usr/bin/python3
import json, os, pathlib, sys, uuid
root = pathlib.Path(__file__).resolve().parent
config = json.loads((root / 'codex-peer-config.json').read_text())
def store(name, value):
    temporary = root / (name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, sort_keys=True)
        stream.flush(); os.fsync(stream.fileno())
    temporary.replace(root / name)
def emit(value): print(json.dumps(value), flush=True)
def result(message, value): emit(dict(jsonrpc='2.0', id=message['id'], result=value))
try:
    if sys.argv[1:] == ['--version']:
        print('codex-cli 0.162.1', flush=True); sys.exit(0)
    assert sys.argv[-3:] == ['app-server', '--listen', 'stdio://']
    native = str(uuid.uuid4())
    inputs = 0
    for line in sys.stdin:
        message = json.loads(line); method = message['method']
        if method == 'initialize': result(message, dict(userAgent='codex-cli/0.162.1'))
        elif method == 'initialized': assert 'id' not in message
        elif method == 'model/list': result(message, dict(data=[dict(id='private-parent-model', displayName='Parent', isDefault=True)], nextCursor=None))
        elif method == 'account/rateLimits/read': result(message, dict(rateLimits=None))
        elif method == 'thread/start':
            params = message['params']
            assert params['model'] == 'private-parent-model' and params['cwd'] == config['workspace']
            server = params['config']['mcp_servers.uam-control']
            assert server['required'] and server['enabled'] and '--uam-control-mcp' in server['args']
            store('codex-parent-setup.json', dict(pid=os.getpid(), request=message, server=server, native_id=native))
            result(message, dict(thread=dict(id=native, cwd=params['cwd']), model=params['model']))
        elif method == 'turn/start':
            inputs += 1
            assert inputs == 1 and message['params']['threadId'] == native
            turn = 'parent-turn-' + str(uuid.uuid4())
            result(message, dict(turn=dict(id=turn, status='inProgress')))
            emit(dict(method='turn/started', params=dict(threadId=native, turn=dict(id=turn, status='inProgress'))))
            emit(dict(method='item/agentMessage/delta', params=dict(threadId=native, turnId=turn, itemId='parent-answer', delta='PRIVATE-PARENT-READY')))
            emit(dict(method='turn/completed', params=dict(threadId=native, turn=dict(id=turn, status='completed', error=None))))
        else: raise AssertionError('unexpected parent method: ' + method)
except Exception as error:
    trace = error.__traceback__
    while trace is not None and trace.tb_next is not None: trace = trace.tb_next
    store('codex-peer-error.json', dict(error=str(error)[:256], exception=type(error).__name__,
        line=trace.tb_lineno if trace is not None else None,
        function=trace.tb_frame.f_code.co_name if trace is not None else None)); sys.exit(70)
)PY";
	}
}

UAM_TEST(CodexOrdinaryMcpDelegateReturnsActualChildThroughAgentStatus)
{
	ManagedCodexBindingFixtureR109 fixture;
	// State precedes its guard; both precede the first native launch and outlive retirement.
	uam::AcpSessionState mcp;
	struct OwnedProcessesR2
	{
		ManagedCodexBindingFixtureR109& fixture;
		uam::AcpSessionState& mcp;
		bool retired = false;
		static bool Reaped(const pid_t pid) noexcept
		{
			if (pid <= 0) return true; // No client was launched into this typed state.
			int status = 0;
			errno = 0;
			const pid_t waited = waitpid(pid, &status, WNOHANG);
			if (waited == pid) return true;
			if (waited < 0 && errno == ECHILD)
			{
				errno = 0;
				return kill(pid, 0) < 0 && errno == ESRCH;
			}
			return false;
		}
		bool Retire() noexcept
		{
			if (retired) return true;
			try
			{
				// Capture directly before Stop clears it, even if owned_pids insertion threw.
				const pid_t client_pid = mcp.child_pid;
				auto& process = PlatformServicesFactory::Instance().process_service;
				process.StopStdioProcess(mcp, true);
				process.CloseStdioProcessHandles(mcp);
				const bool client_reaped = Reaped(client_pid);
				const bool native_reaped = fixture.StopAndReap();
				// Never destroy private environment/app storage with unconfirmed owned workers.
				if (!client_reaped || !native_reaped) std::abort();
				uam::UamControlService::Shutdown(fixture.app);
				retired = true;
				return true;
			}
			catch (...) { std::abort(); }
		}
		~OwnedProcessesR2() noexcept { (void)Retire(); }
	} cleanup{fixture, mcp};
	auto& app = fixture.app;
	auto* parent = ChatDomainService().FindChatById(app, fixture.root_id);
	UAM_ASSERT(parent);
	parent->uam_control_enabled = true;
	parent->uam_control_explicitly_disabled = false;
	parent->command_safety_tier = "yolo";
	parent->approval_mode = "default";
	parent->model_id = "private-parent-model";
	app.settings.git_snapshot_plain_folders = false; // This protocol fixture pins the ordinary private folder, not optional VCS isolation.
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *parent));
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error) && error.empty());
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "ordinary-codex-parent.py", OrdinaryCodexParentPeerR1()));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "ordinary-codex-child.py", OrdinaryCodexChildPeerR1()));
	// The actual persisted ordinary run selects the child peer; no runtime flags or futures are injected.
	const std::string router = "#!/usr/bin/python3\nimport json, os, pathlib, sys\nr=pathlib.Path(__file__).resolve().parent\nc=json.loads((r/'codex-peer-config.json').read_text())\nchild=any((pathlib.Path(c['data_root'])/'agent-runs').glob('*.json'))\np=r/('ordinary-codex-child.py' if child else 'ordinary-codex-parent.py')\nos.execv('/usr/bin/python3',['/usr/bin/python3','-I','-S',str(p)]+sys.argv[1:])\n";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "codex-peer.py", router));
	fs::permissions(fixture.temp.root / "codex-peer.py", fs::perms::owner_all, fs::perm_options::replace);
	auto config = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-peer-config.json"));
	config["model"] = "private-requested-child-model";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "codex-peer-config.json", config.dump()));
	UAM_ASSERT(uam::SendAcpPrompt(app, fixture.root_id, "Prepare to receive a delegated result.", {}, {}, false, &error));
	fixture.Await([&] { const auto* session = uam::FindAcpSessionForChat(app, fixture.root_id); return session && session->running && session->session_ready && !session->processing && fs::exists(fixture.temp.root / "codex-parent-setup.json"); });
	const auto setup = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-parent-setup.json"));
	const auto& server = setup.at("server");
	const auto* parent_session = uam::FindAcpSessionForChat(app, fixture.root_id);
	UAM_ASSERT(parent_session && parent_session->session_id == setup.at("native_id").get<std::string>());
	UAM_ASSERT(!parent_session->uam_control_capability_id.empty());
	const std::string capability = parent_session->uam_control_capability_id;
	UAM_ASSERT(std::ranges::any_of(app.uam_control_capabilities, [&](const auto& item) { return item.id == capability && item.chat_id == fixture.root_id && item.session_chat_id == fixture.root_id; }));
	UAM_ASSERT_EQ(server.at("command").get<std::string>(), PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().string());
	std::vector<std::string> argv{server.at("command").get<std::string>()};
	for (const auto& arg : server.at("args")) argv.push_back(arg.get<std::string>());
	std::vector<std::pair<std::string, std::string>> environment;
	for (const auto& entry : server.at("env").items()) environment.emplace_back(entry.key(), entry.value().get<std::string>());
	auto& process = PlatformServicesFactory::Instance().process_service;
	UAM_ASSERT(process.StartStdioProcess(mcp, fixture.temp.root, argv, &error, environment));
	UAM_ASSERT(mcp.child_pid > 0);
	fixture.owned_pids.insert(mcp.child_pid);
	std::string output;
	const auto call = [&](int id, const std::string& method, const nlohmann::json& params)
	{
		const auto line = nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump() + "\n";
		UAM_ASSERT(process.WriteToStdioProcess(mcp, line.data(), line.size(), &error));
		nlohmann::json response;
		fixture.Await([&]
		{
			if (!response.is_null()) return true; // Await checks its predicate again after success.
			(void)uam::UamControlService::ProcessPendingRequests(app);
			std::array<char, 16384> bytes{};
			const auto count = process.ReadStdioProcessStdout(mcp, bytes.data(), bytes.size(), &error);
			UAM_ASSERT(count >= 0 || count == -2); // Existing platform would-block contract.
			if (count > 0) output.append(bytes.data(), static_cast<std::size_t>(count));
			const auto end = output.find('\n');
			if (end == std::string::npos) return false;
			response = nlohmann::json::parse(output.substr(0, end)); output.erase(0, end + 1);
			UAM_ASSERT_EQ(response.at("id"), nlohmann::json(id));
			return true;
		});
		UAM_ASSERT(response.contains("result") && !response.contains("error"));
		return response.at("result");
	};
	UAM_ASSERT(call(1, "initialize", {{"protocolVersion", "2025-03-26"}, {"capabilities", nlohmann::json::object()}, {"clientInfo", {{"name", "private-codex-probe"}, {"version", "1"}}}}).contains("capabilities"));
	const std::string initialized = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n";
	UAM_ASSERT(process.WriteToStdioProcess(mcp, initialized.data(), initialized.size(), &error));
	const auto listed = call(2, "tools/list", nlohmann::json::object());
	UAM_ASSERT(std::ranges::any_of(listed.at("tools"), [](const auto& item) { return item.value("name", "") == "agent_delegate"; }));
	const auto payload = [](const nlohmann::json& result)
	{
		UAM_ASSERT(!result.value("isError", true));
		UAM_ASSERT_EQ(result.at("content").size(), std::size_t{1});
		return nlohmann::json::parse(result.at("content")[0].at("text").get<std::string>());
	};
	const auto delegated = payload(call(3, "tools/call", {{"name", "agent_delegate"}, {"arguments", {{"agentId", fixture.definition.id}, {"task", fixture.task}, {"providerId", "codex-cli"}, {"modelId", "private-requested-child-model"}}}}));
	const std::string run_id = delegated.at("runId").get<std::string>();
	UAM_ASSERT_EQ(delegated.at("status"), nlohmann::json("queued"));
	UAM_ASSERT(app.agent_runs.size() == 1 && app.agent_runs.front().id == run_id && !app.agent_runs.front().managed_continuation);
	fixture.Await([&] { (void)uam::UamControlService::ProcessPendingRequests(app); return fs::exists(fixture.temp.root / "codex-input-witness.json"); });
	const auto input = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-input-witness.json"));
	const auto native = input.at("native_id").get<std::string>();
	UAM_ASSERT(uam::uuid::IsCanonicalUuid(native));
	UAM_ASSERT_EQ(input.at("request").at("params").at("model"), nlohmann::json("private-requested-child-model"));
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	const auto running = payload(call(4, "tools/call", {{"name", "agent_status"}, {"arguments", {{"runId", run_id}}}}));
	UAM_ASSERT_EQ(running.at("runId"), nlohmann::json(run_id));
	UAM_ASSERT_EQ(running.at("status"), nlohmann::json("running"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-codex-completion", "release the actual delegated native child\n"));
	fixture.Await([&] { (void)uam::UamControlService::ProcessPendingRequests(app); return fixture.ReadRun().status == "completed"; });
	const auto terminal = payload(call(5, "tools/call", {{"name", "agent_status"}, {"arguments", {{"runId", run_id}}}}));
	UAM_ASSERT_EQ(terminal.at("runId"), nlohmann::json(run_id));
	UAM_ASSERT_EQ(terminal.at("status"), nlohmann::json("completed"));
	UAM_ASSERT_EQ(terminal.at("result"), nlohmann::json(fixture.answer));
	UAM_ASSERT_EQ(terminal.at("providerId"), nlohmann::json("codex-cli"));
	UAM_ASSERT_EQ(terminal.at("modelId"), nlohmann::json("private-requested-child-model"));
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	const auto run = fixture.ReadRun();
	UAM_ASSERT_EQ(run.root_chat_id, fixture.root_id);
	UAM_ASSERT_EQ(run.id, run_id);
	UAM_ASSERT_EQ(run.result_excerpt, fixture.answer);
	const auto child = ChatRepository::LoadLocalChat(app.data_root, run.transcript_chat_id);
	UAM_ASSERT(child && child->native_session_id == native && child->execution_host_id == "local");
	UAM_ASSERT_EQ(child->model_id, std::string("private-requested-child-model"));
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.answer, &Message::content), 1);
	UAM_ASSERT_EQ(fixture.Launches(), std::size_t{1}); // Child peer only; ordinary parent has its own witness.
	UAM_ASSERT(cleanup.Retire());
}

namespace
{
	std::string OrdinaryCodexEndedTurnParentPeerR1()
	{
		return R"UAM_WAKE(#!/usr/bin/python3
import json, os, pathlib, select, signal, subprocess, sys, uuid
root = pathlib.Path(__file__).resolve().parent
config = json.loads((root / 'codex-peer-config.json').read_text())
mcp = None
def store(name, value):
    temporary = root / (name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, sort_keys=True)
        stream.flush(); os.fsync(stream.fileno())
    temporary.replace(root / name)
def emit(value): print(json.dumps(value), flush=True)
def reply(message, value): emit(dict(jsonrpc='2.0', id=message['id'], result=value))
def stop(signum, frame): raise SystemExit(0)
for signum in (signal.SIGTERM, signal.SIGINT): signal.signal(signum, stop)
def request(identifier, method, params):
    mcp.stdin.write(json.dumps(dict(jsonrpc='2.0', id=identifier, method=method, params=params)) + '\n')
    mcp.stdin.flush()
    assert select.select([mcp.stdout], [], [], 20)[0], 'owned MCP reply missing'
    response = json.loads(mcp.stdout.readline())
    assert response['id'] == identifier and 'error' not in response
    return response['result']
try:
    if sys.argv[1:] == ['--version']:
        print('codex-cli 0.162.1', flush=True); sys.exit(0)
    assert sys.argv[-3:] == ['app-server', '--listen', 'stdio://']
    native = str(uuid.uuid4())
    inputs = setups = 0
    delegated = None
    for line in sys.stdin:
        message = json.loads(line); method = message['method']
        if method == 'initialize': reply(message, dict(userAgent='codex-cli/0.162.1'))
        elif method == 'initialized': assert 'id' not in message
        elif method == 'model/list': reply(message, dict(data=[dict(id='private-parent-model', displayName='Parent', isDefault=True)], nextCursor=None))
        elif method == 'account/rateLimits/read': reply(message, dict(rateLimits=None))
        elif method == 'thread/start':
            setups += 1
            assert setups == 1 and inputs == 0
            params = message['params']
            assert params['model'] == 'private-parent-model' and params['cwd'] == config['workspace']
            server = params['config']['mcp_servers.uam-control']
            assert server['required'] and server['enabled'] and '--uam-control-mcp' in server['args']
            store('wake-parent-setup.json', dict(pid=os.getpid(), request=message, native_id=native))
            reply(message, dict(thread=dict(id=native, cwd=params['cwd']), model=params['model']))
        elif method == 'turn/start':
            inputs += 1
            assert inputs <= 2 and setups == 1 and message['params']['threadId'] == native
            with (root / 'wake-parent-inputs.jsonl').open('a') as stream:
                stream.write(json.dumps(message, sort_keys=True) + '\n'); stream.flush(); os.fsync(stream.fileno())
            turn = 'wake-parent-turn-' + str(uuid.uuid4())
            reply(message, dict(turn=dict(id=turn, status='inProgress')))
            emit(dict(method='turn/started', params=dict(threadId=native, turn=dict(id=turn, status='inProgress'))))
            if inputs == 1:
                # Use only the exact server specification received by this native parent.
                environment = dict(os.environ); environment.update(server['env'])
                mcp = subprocess.Popen([server['command']] + server['args'], stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=environment)
                store('wake-mcp-process.json', dict(pid=mcp.pid))
                assert 'capabilities' in request(1, 'initialize', dict(protocolVersion='2025-03-26', capabilities={}, clientInfo=dict(name='private-ended-turn-parent', version='1')))
                mcp.stdin.write(json.dumps(dict(jsonrpc='2.0', method='notifications/initialized')) + '\n'); mcp.stdin.flush()
                assert any(item['name'] == 'agent_delegate' for item in request(2, 'tools/list', {})['tools'])
                result = request(3, 'tools/call', dict(name='agent_delegate', arguments=dict(agentId=config['agent_id'],
                    task=config['task'], providerId='codex-cli', modelId=config['model'])))
                assert not result.get('isError', True) and len(result['content']) == 1
                delegated = json.loads(result['content'][0]['text'])
                assert delegated['status'] == 'queued' and str(uuid.UUID(delegated['runId'])) == delegated['runId']
                store('wake-delegated.json', delegated)
                mcp.stdin.close(); mcp.stdin = None
                mcp.communicate(timeout=2); assert mcp.returncode == 0
                store('wake-mcp-exit.json', dict(pid=mcp.pid, exit=mcp.returncode))
                mcp = None
                text = 'PRIVATE-PARENT-ENDED-WITH-CHILD-RUNNING'
            else:
                assert delegated is not None
                text = ''.join(part.get('text', '') for part in message['params']['input'] if part.get('type') == 'text')
                # Both values must arrive in this native frame. The ledger is not an input oracle.
                run_at, result_at = text.find(delegated['runId']), text.find(config['answer'])
                assert run_at >= 0 and result_at >= 0
                actual_run = text[run_at:run_at + len(delegated['runId'])]
                actual_result = text[result_at:result_at + len(config['answer'])]
                text = 'PRIVATE-PARENT-CONSUMED ' + actual_run + ' ' + actual_result
                store('wake-parent-consumption.json', dict(pid=os.getpid(), native_id=native,
                    request=message, run_id=actual_run, result=actual_result, response=text))
            emit(dict(method='item/agentMessage/delta', params=dict(threadId=native, turnId=turn, itemId='wake-answer-' + str(inputs), delta=text)))
            emit(dict(method='turn/completed', params=dict(threadId=native, turn=dict(id=turn, status='completed', error=None))))
            store('wake-parent-completed-' + str(inputs) + '.json', dict(native_id=native, turn_id=turn))
        else: raise AssertionError('unexpected owned parent method: ' + method)
except Exception as error:
    store('codex-peer-error.json', dict(error=type(error).__name__ + ': ' + str(error)[:256]))
    sys.exit(70)
finally:
    if mcp is not None:
        if mcp.poll() is None:
            mcp.terminate()
            try: mcp.wait(timeout=2)
            except subprocess.TimeoutExpired: mcp.kill(); mcp.wait(timeout=2)
)UAM_WAKE";
	}
}

UAM_TEST(CodexOrdinaryMcpChildCompletionWakesEndedParentExactlyOnce)
{
	ManagedCodexBindingFixtureR109 fixture;
	struct Cleanup
	{
		ManagedCodexBindingFixtureR109& fixture;
		~Cleanup() noexcept
		{
			try
			{
				if (!fixture.StopAndReap()) std::abort();
				const auto witness = fixture.temp.root / "wake-mcp-process.json";
				if (fs::exists(witness))
				{
					const auto pid = nlohmann::json::parse(ReadFile(witness)).at("pid").get<pid_t>();
					errno = 0;
					if (pid <= 0 || kill(pid, 0) == 0 || errno != ESRCH) std::abort();
				}
				uam::UamControlService::Shutdown(fixture.app);
			}
			catch (...) { std::abort(); }
		}
	} cleanup{fixture};
	auto& app = fixture.app;
	auto* parent = ChatDomainService().FindChatById(app, fixture.root_id);
	UAM_ASSERT(parent);
	parent->uam_control_enabled = true;
	parent->uam_control_explicitly_disabled = false;
	parent->command_safety_tier = "yolo";
	parent->approval_mode = "default";
	parent->model_id = "private-parent-model";
	UAM_ASSERT(parent->active_goal_id.empty());
	app.settings.git_snapshot_plain_folders = false;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *parent));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "ordinary-codex-parent.py", OrdinaryCodexEndedTurnParentPeerR1()));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "ordinary-codex-child.py", OrdinaryCodexChildPeerR1()));
	const std::string router = "#!/usr/bin/python3\nimport json, os, pathlib, sys\nr=pathlib.Path(__file__).resolve().parent\nc=json.loads((r/'codex-peer-config.json').read_text())\nchild=any((pathlib.Path(c['data_root'])/'agent-runs').glob('*.json'))\np=r/('ordinary-codex-child.py' if child else 'ordinary-codex-parent.py')\nos.execv('/usr/bin/python3',['/usr/bin/python3','-I','-S',str(p)]+sys.argv[1:])\n";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "codex-peer.py", router));
	fs::permissions(fixture.temp.root / "codex-peer.py", fs::perms::owner_all, fs::perm_options::replace);
	auto config = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-peer-config.json"));
	config["model"] = "private-requested-child-model";
	config["agent_id"] = fixture.definition.id;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "codex-peer-config.json", config.dump()));
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error) && error.empty());
	// The only test-side parent Send starts the original turn, before delegation.
	UAM_ASSERT(uam::SendAcpPrompt(app, fixture.root_id, "Delegate once, end this turn, and consume the child completion when delivered.", {}, {}, false, &error));
	const auto pulse = [&]
	{
		// Same runtime/control/scheduler/save order as Application::PollTick; no forced save or delivery call.
		fixture.AssertPeer(); fixture.Observe();
		(void)uam::PollAllAcpSessions(app);
		(void)uam::UamControlService::ProcessPendingRequests(app);
		(void)uam::AgentRunScheduler::Poll(app);
		(void)uam::FlushPendingChatSaves(app);
		fixture.Observe();
		if (fs::exists(fixture.temp.root / "codex-peer-error.json")) throw TestFailure("Private ended-turn peer prerequisite: " + ReadFile(fixture.temp.root / "codex-peer-error.json").substr(0, 512));
	};
	const auto await = [&](auto done)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!done() && std::chrono::steady_clock::now() < deadline)
		{ pulse(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
		UAM_ASSERT(done());
	};
	await([&]
	{
		const auto* session = uam::FindAcpSessionForChat(app, fixture.root_id);
		return fs::exists(fixture.temp.root / "wake-parent-completed-1.json") && fs::exists(fixture.temp.root / "codex-input-witness.json") &&
		    session && session->running && session->session_ready && !session->processing;
	});
	const auto setup = nlohmann::json::parse(ReadFile(fixture.temp.root / "wake-parent-setup.json"));
	const auto delegated = nlohmann::json::parse(ReadFile(fixture.temp.root / "wake-delegated.json"));
	const std::string native = setup.at("native_id").get<std::string>();
	const std::string run_id = delegated.at("runId").get<std::string>();
	const auto* session = uam::FindAcpSessionForChat(app, fixture.root_id);
	UAM_ASSERT(session && session->session_id == native && !session->processing && !session->queue_held);
	const auto parent_pid = session->child_pid;
	const auto capability = session->uam_control_capability_id;
	UAM_ASSERT(!capability.empty() && session->queued_user_prompts.empty());
	UAM_ASSERT(app.agent_runs.size() == 1 && app.agent_runs.front().id == run_id && !app.agent_runs.front().managed_continuation);
	const auto running = fixture.ReadRun();
	UAM_ASSERT_EQ(running.status, std::string("running"));
	UAM_ASSERT_EQ(running.root_chat_id, fixture.root_id);
	UAM_ASSERT(running.goal_id.empty());
	UAM_ASSERT_EQ(running.effective_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	const auto mcp_exit = nlohmann::json::parse(ReadFile(fixture.temp.root / "wake-mcp-exit.json"));
	UAM_ASSERT_EQ(mcp_exit.at("exit"), nlohmann::json(0));
	// Release this exact native child only after the parent has genuinely ended.
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-codex-completion", "release the owned child after parent turn completion\n"));
	await([&] { return fixture.ReadRun().status == "completed"; });
	UAM_ASSERT_EQ(fixture.ReadRun().result_excerpt, fixture.answer);
	// Missing successor input must fail here, even if a synthetic UAM result row exists.
	await([&]
	{
		const auto* current = uam::FindAcpSessionForChat(app, fixture.root_id);
		return fs::exists(fixture.temp.root / "wake-parent-consumption.json") && fs::exists(fixture.temp.root / "wake-parent-completed-2.json") &&
		    current && !current->processing && app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty();
	});
	for (unsigned index = 0; index < 20; ++index) pulse();
	const auto consumed = nlohmann::json::parse(ReadFile(fixture.temp.root / "wake-parent-consumption.json"));
	UAM_ASSERT_EQ(consumed.at("run_id"), nlohmann::json(run_id));
	UAM_ASSERT_EQ(consumed.at("result"), nlohmann::json(fixture.answer));
	UAM_ASSERT_EQ(consumed.at("native_id"), nlohmann::json(native));
	UAM_ASSERT_EQ(consumed.at("pid").get<pid_t>(), parent_pid);
	std::vector<nlohmann::json> frames;
	std::istringstream stream(ReadFile(fixture.temp.root / "wake-parent-inputs.jsonl"));
	std::string line;
	while (std::getline(stream, line)) if (!line.empty()) frames.push_back(nlohmann::json::parse(line));
	UAM_ASSERT_EQ(frames.size(), std::size_t{2}); // One original + exactly one successor on the same native binding.
	for (const auto& frame : frames)
	{
		UAM_ASSERT_EQ(frame.at("method"), nlohmann::json("turn/start"));
		UAM_ASSERT_EQ(frame.at("params").at("threadId"), nlohmann::json(native));
	}
	const auto* current = uam::FindAcpSessionForChat(app, fixture.root_id);
	UAM_ASSERT(current && current->session_id == native && current->child_pid == parent_pid && current->uam_control_capability_id == capability);
	const auto saved_parent = ChatRepository::LoadLocalChat(app.data_root, fixture.root_id);
	UAM_ASSERT(saved_parent);
	UAM_ASSERT_EQ(std::ranges::count(saved_parent->messages, consumed.at("response").get<std::string>(), &Message::content), 1);
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Launches(), std::size_t{1});
	UAM_ASSERT_EQ(app.agent_runs.size(), std::size_t{1});
}

namespace
{
	std::string OrdinaryCodexEndedTurnControlPeerR6()
	{
		std::string peer = OrdinaryCodexEndedTurnParentPeerR1();
		const auto count_at = peer.find("assert inputs <= 2 and setups == 1");
		UAM_ASSERT(count_at != std::string::npos);
		peer.replace(count_at, std::string("assert inputs <= 2 and setups == 1").size(), "assert inputs <= 3 and setups == 1");
		const auto process_at = peer.find("mcp = None\n");
		UAM_ASSERT(process_at != std::string::npos);
		peer.insert(process_at, "restarted = config.get('continuation_mode') == 8 and (root / 'wake-parent-setup.json').exists()\n");
		const std::string setup = "        elif method == 'thread/start':";
		const auto setup_at = peer.find(setup); UAM_ASSERT(setup_at != std::string::npos);
		peer.replace(setup_at, setup.size(), "        elif method in ('thread/start', 'thread/resume'):");
		const std::string params = "            params = message['params']\n";
		const auto params_at = peer.find(params); UAM_ASSERT(params_at != std::string::npos);
		peer.insert(params_at + params.size(), "            if method == 'thread/resume': native = params['threadId']\n");
		const std::string needle = "            reply(message, dict(turn=dict(id=turn, status='inProgress')))";
		const auto at = peer.find(needle);
		UAM_ASSERT(at != std::string::npos && peer.find(needle, at + needle.size()) == std::string::npos);
		peer.insert(at, R"CONTROL(            if config['continuation_mode'] == 8 and restarted:
                incoming = ''.join(part.get('text', '') for part in message['params']['input'] if part.get('type') == 'text')
                assert incoming == 'PRIVATE-NEW-HUMAN-AFTER-UNKNOWN'
                reply(message, dict(turn=dict(id=turn, status='inProgress')))
                emit(dict(method='turn/started', params=dict(threadId=native, turn=dict(id=turn, status='inProgress'))))
                text = 'PRIVATE-NEW-HUMAN-COMPLETED ' + incoming
                emit(dict(method='item/agentMessage/delta', params=dict(threadId=native, turnId=turn, itemId='restarted-human-answer', delta=text)))
                emit(dict(method='turn/completed', params=dict(threadId=native, turn=dict(id=turn, status='completed', error=None))))
                store('wake-control-human-result.json', dict(text=text, native_id=native, pid=os.getpid()))
                continue
            if inputs == 3 and config['continuation_mode'] in (6, 7):
                incoming = ''.join(part.get('text', '') for part in message['params']['input'] if part.get('type') == 'text')
                assert incoming == 'PRIVATE-NEW-HUMAN-AFTER-UNKNOWN'
                reply(message, dict(turn=dict(id=turn, status='inProgress')))
                emit(dict(method='turn/started', params=dict(threadId=native, turn=dict(id=turn, status='inProgress'))))
                # The old correlated receipt arrives while the NEW human turn is genuinely active.
                if config['continuation_mode'] == 6:
                    emit(dict(jsonrpc='2.0', id=retired_request, error=dict(code=-32603, message='owned delayed old RPC refusal')))
                else:
                    emit(dict(jsonrpc='2.0', id=retired_request, result=dict(turn=dict(id=retired_turn, status='inProgress'))))
                emit(dict(method='turn/started', params=dict(threadId=native, turn=dict(id=retired_turn, status='inProgress'))))
                emit(dict(method='turn/completed', params=dict(threadId=native, turn=dict(id=retired_turn, status='completed', error=None))))
                text = 'PRIVATE-NEW-HUMAN-COMPLETED ' + incoming
                emit(dict(method='item/agentMessage/delta', params=dict(threadId=native, turnId=turn, itemId='new-human-answer', delta=text)))
                emit(dict(method='turn/completed', params=dict(threadId=native, turn=dict(id=turn, status='completed', error=None))))
                store('wake-control-human-result.json', dict(text=text, native_id=native, pid=os.getpid(), retired_turn=retired_turn))
                continue
            if inputs == 2:
                mode = config['continuation_mode']
                store('wake-control-second-input.json', message)
                if mode in (9, 10, 11):
                    import time
                    deadline = time.monotonic() + 30
                    while not (root / 'release-written-receipt').exists() and time.monotonic() < deadline:
                        time.sleep(0.001)
                    assert (root / 'release-written-receipt').exists(), 'owned receipt release timed out'
                    if mode == 9:
                        emit(dict(jsonrpc='2.0', id=message['id'], error=dict(code=-32603, message='owned post-disable RPC refusal')))
                        store('wake-control-observed.json', dict(mode=mode))
                        continue
                    incoming = ''.join(part.get('text', '') for part in message['params']['input'] if part.get('type') == 'text')
                    assert delegated['runId'] in incoming and config['answer'] in incoming
                    text = 'PRIVATE-POST-DISABLE-CONSUMED ' + delegated['runId'] + ' ' + config['answer']
                    reply(message, dict(turn=dict(id=turn, status='inProgress')))
                    emit(dict(method='turn/started', params=dict(threadId=native, turn=dict(id=turn, status='inProgress'))))
                    emit(dict(method='item/agentMessage/delta', params=dict(threadId=native, turnId=turn, itemId='post-disable-answer', delta=text)))
                    emit(dict(method='turn/completed', params=dict(threadId=native, turn=dict(id=turn, status='completed', error=None))))
                    store('wake-control-observed.json', dict(mode=mode, turn=turn, text=text))
                    continue
                if mode in (6, 7, 8):
                    retired_request, retired_turn = message['id'], turn
                    emit(dict(method='turn/started', params=dict(threadId=native, turn=dict(id=turn, status='inProgress'))))
                    emit(dict(method='turn/completed', params=dict(threadId=native, turn=dict(id=turn, status='completed', error=None))))
                    store('wake-control-observed.json', dict(mode=mode))
                    continue
                if mode == 0:
                    # Genuine live peer keeps stdin open but supplies NO RPC ACK or completion.
                    continue
                if mode == 1:
                    old = 'old-delayed-' + str(uuid.uuid4())
                    emit(dict(method='turn/started', params=dict(threadId=str(uuid.uuid4()), turn=dict(id=old))))
                    emit(dict(method='turn/started', params=dict(threadId=native, turn=dict(id=old))))
                    emit(dict(method='turn/completed', params=dict(threadId=native, turn=dict(id=old, status='completed', error=None))))
                    store('wake-control-observed.json', dict(mode=mode))
                    continue
                if mode == 2:
                    emit(dict(jsonrpc='2.0', id=message['id'], error=dict(code=-32603, message='owned private RPC refusal')))
                    store('wake-control-observed.json', dict(mode=mode))
                    continue
                if mode == 3:
                    incoming = ''.join(part.get('text', '') for part in message['params']['input'] if part.get('type') == 'text')
                    assert delegated['runId'] in incoming and config['answer'] in incoming
                    text = 'PRIVATE-PARENT-EARLY-CONSUMED ' + delegated['runId'] + ' ' + config['answer']
                    emit(dict(method='turn/started', params=dict(threadId=native, turn=dict(id=turn, status='inProgress'))))
                    emit(dict(method='item/agentMessage/delta', params=dict(threadId=native, turnId=turn, itemId='wake-early-result', delta=text)))
                    emit(dict(method='turn/completed', params=dict(threadId=native, turn=dict(id=turn, status='completed', error=None))))
                    reply(message, dict(turn=dict(id=turn, status='inProgress')))
                    store('wake-control-observed.json', dict(mode=mode, turn=turn, text=text))
                    continue
                if mode == 5:
                    reply(message, dict(turn=dict(id=42, status='inProgress')))
                    store('wake-control-observed.json', dict(mode=mode))
                    continue
                if mode == 4:
                    reply(message, dict(turn=dict(id=turn, status='inProgress')))
                    wrong = 'wrong-after-ack-' + str(uuid.uuid4())
                    emit(dict(method='turn/started', params=dict(threadId=native, turn=dict(id=wrong, status='inProgress'))))
                    emit(dict(method='turn/completed', params=dict(threadId=native, turn=dict(id=wrong, status='completed', error=None))))
                    store('wake-control-observed.json', dict(mode=mode))
                    continue
)CONTROL");
		const std::string stop_at = "        else: raise AssertionError('unexpected owned parent method: ' + method)";
		const auto stop_position = peer.find(stop_at);
		UAM_ASSERT(stop_position != std::string::npos);
		peer.insert(stop_position, "        elif method == 'turn/interrupt':\n            reply(message, {})\n            emit(dict(method='turn/completed', params=dict(threadId=native, turn=dict(id=turn, status='interrupted', error=None))))\n");
		return peer;
	}
	void RunOrdinaryEndedTurnNativeReceiptControl(int mode)
	{
	ManagedCodexBindingFixtureR109 fixture;
	struct Cleanup
	{
		ManagedCodexBindingFixtureR109& fixture;
		~Cleanup() noexcept
		{
			try
			{
				if (!fixture.StopAndReap()) std::abort();
				const auto witness = fixture.temp.root / "wake-mcp-process.json";
				if (fs::exists(witness))
				{
					const auto pid = nlohmann::json::parse(ReadFile(witness)).at("pid").get<pid_t>();
					errno = 0;
					if (pid <= 0 || kill(pid, 0) == 0 || errno != ESRCH) std::abort();
				}
				uam::UamControlService::Shutdown(fixture.app);
			}
			catch (...) { std::abort(); }
		}
	} cleanup{fixture};
	auto& app = fixture.app;
	auto* parent = ChatDomainService().FindChatById(app, fixture.root_id);
	UAM_ASSERT(parent);
	parent->uam_control_enabled = true;
	parent->uam_control_explicitly_disabled = false;
	parent->command_safety_tier = "yolo";
	parent->approval_mode = "default";
	parent->model_id = "private-parent-model";
	UAM_ASSERT(parent->active_goal_id.empty());
	app.settings.git_snapshot_plain_folders = false;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *parent));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "ordinary-codex-parent.py", OrdinaryCodexEndedTurnControlPeerR6()));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "ordinary-codex-child.py", OrdinaryCodexChildPeerR1()));
	const std::string router = "#!/usr/bin/python3\nimport json, os, pathlib, sys\nr=pathlib.Path(__file__).resolve().parent\nc=json.loads((r/'codex-peer-config.json').read_text())\nchild=any((pathlib.Path(c['data_root'])/'agent-runs').glob('*.json'))\np=r/('ordinary-codex-child.py' if child else 'ordinary-codex-parent.py')\nos.execv('/usr/bin/python3',['/usr/bin/python3','-I','-S',str(p)]+sys.argv[1:])\n";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "codex-peer.py", router));
	fs::permissions(fixture.temp.root / "codex-peer.py", fs::perms::owner_all, fs::perm_options::replace);
	auto config = nlohmann::json::parse(ReadFile(fixture.temp.root / "codex-peer-config.json"));
	config["model"] = "private-requested-child-model";
	config["agent_id"] = fixture.definition.id;
	config["continuation_mode"] = mode;
	if (mode == 8)
	{
		const std::string replacement_router = "#!/usr/bin/python3\nimport os,pathlib,sys\nr=pathlib.Path(__file__).resolve().parent\nif sys.argv[1:]==['--version']: print('codex-cli 0.162.1');sys.exit(0)\nc=r/'ordinary-role-counter'\nn=int(c.read_text())+1 if c.exists() else 1\nc.write_text(str(n))\np=r/('ordinary-codex-child.py' if n==2 else 'ordinary-codex-parent.py')\nos.execv('/usr/bin/python3',['/usr/bin/python3','-I','-S',str(p)]+sys.argv[1:])\n";
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "codex-peer.py", replacement_router));
		fs::permissions(fixture.temp.root / "codex-peer.py", fs::perms::owner_all, fs::perm_options::replace);
	}
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "codex-peer-config.json", config.dump()));
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error) && error.empty());
	// The only test-side parent Send starts the original turn, before delegation.
	UAM_ASSERT(uam::SendAcpPrompt(app, fixture.root_id, "Delegate once, end this turn, and consume the child completion when delivered.", {}, {}, false, &error));
	const auto pulse = [&]
	{
		// Same runtime/control/scheduler/save order as Application::PollTick; no forced save or delivery call.
		fixture.AssertPeer(); fixture.Observe();
		(void)uam::PollAllAcpSessions(app);
		(void)uam::UamControlService::ProcessPendingRequests(app);
		(void)uam::AgentRunScheduler::Poll(app);
		(void)uam::FlushPendingChatSaves(app);
		fixture.Observe();
		if (fs::exists(fixture.temp.root / "codex-peer-error.json")) throw TestFailure("Private ended-turn peer prerequisite: " + ReadFile(fixture.temp.root / "codex-peer-error.json").substr(0, 512));
	};
	const auto await = [&](auto done)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!done() && std::chrono::steady_clock::now() < deadline)
		{ pulse(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
		UAM_ASSERT(done());
	};
	await([&]
	{
		const auto* session = uam::FindAcpSessionForChat(app, fixture.root_id);
		return fs::exists(fixture.temp.root / "wake-parent-completed-1.json") && fs::exists(fixture.temp.root / "codex-input-witness.json") &&
		    session && session->running && session->session_ready && !session->processing;
	});
	const auto setup = nlohmann::json::parse(ReadFile(fixture.temp.root / "wake-parent-setup.json"));
	const auto delegated = nlohmann::json::parse(ReadFile(fixture.temp.root / "wake-delegated.json"));
	const std::string native = setup.at("native_id").get<std::string>();
	const std::string run_id = delegated.at("runId").get<std::string>();
	const auto* session = uam::FindAcpSessionForChat(app, fixture.root_id);
	UAM_ASSERT(session && session->session_id == native && !session->processing && !session->queue_held);
	const auto parent_pid = session->child_pid;
	const auto capability = session->uam_control_capability_id;
	UAM_ASSERT(!capability.empty() && session->queued_user_prompts.empty());
	UAM_ASSERT(app.agent_runs.size() == 1 && app.agent_runs.front().id == run_id && !app.agent_runs.front().managed_continuation);
	const auto running = fixture.ReadRun();
	UAM_ASSERT_EQ(running.status, std::string("running"));
	UAM_ASSERT_EQ(running.root_chat_id, fixture.root_id);
	UAM_ASSERT(running.goal_id.empty());
	UAM_ASSERT_EQ(running.effective_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
	const auto mcp_exit = nlohmann::json::parse(ReadFile(fixture.temp.root / "wake-mcp-exit.json"));
	UAM_ASSERT_EQ(mcp_exit.at("exit"), nlohmann::json(0));

		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-codex-completion", "release the owned child after normal parent completion\n"));
		await([&] { return fixture.ReadRun().status == "completed"; });
		await([&] { return fs::exists(fixture.temp.root / "wake-control-second-input.json"); });
		const auto retained_primary = fixture.temp.root / "owned-pre-ack-primary";
		const auto primary = fixture.app.data_root / "chats" / (fixture.root_id + ".json");
		std::vector<ManagedParentInputReceipt> before_ack_receipts;
		if (mode == 11)
		{
			const auto* owner = ChatDomainService().FindChatById(app, fixture.root_id); UAM_ASSERT(owner);
			before_ack_receipts = owner->managed_parent_input_receipts;
			UAM_ASSERT(!before_ack_receipts.empty() && before_ack_receipts.back().accepted_turn_id.empty());
			fs::rename(primary, retained_primary); fs::create_directory(primary); // Real own full-primary publication refusal.
			UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "release-written-receipt", "release one exact ACK after owned storage refusal\\n"));
		}
		if (mode == 9 || mode == 10)
		{
			// The actual peer received the second wire; no reply/notification has been sent yet.
			const auto* written = uam::FindAcpSessionForChat(app, fixture.root_id);
			UAM_ASSERT(written && written->processing && written->prompt_request_id != 0);
			UAM_ASSERT(uam::provider_availability::IsEnabled(app.settings, "codex-cli", "local"));
			UAM_ASSERT(uam::provider_availability::Set(app.settings, "codex-cli", "local", false));
			UAM_ASSERT(!uam::provider_availability::IsEnabled(app.settings, "codex-cli", "local"));
			UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "release-written-receipt", "release only the already-written receipt\n"));
		}
		if (mode != 0) await([&] { return fs::exists(fixture.temp.root / "wake-control-observed.json"); });
		await([&]
		{
			const auto* owner = ChatDomainService().FindChatById(app, fixture.root_id);
			if (!owner || !owner->managed_parent_result) return false;
			const std::string stage = owner->managed_parent_result->stage;
			if (mode == 0 || mode == 1 || mode == 6 || mode == 7 || mode == 8) return stage == "write-unconfirmed";
			if (mode == 3) return stage == "settled";
			return stage == "held";
		});
		if (mode == 2 || mode == 5 || mode == 9 || mode == 10 || mode == 11)
		{
			await([&]
			{
				const auto* current = uam::FindAcpSessionForChat(app, fixture.root_id);
				return current && !current->processing && current->prompt_request_id == 0 &&
				    current->lifecycle_state == (mode == 10 || mode == 11 ? "ready" : "error");
			});
		}
		if (mode == 6 || mode == 7 || mode == 8)
		{
			await([&]
			{
				const auto* current = uam::FindAcpSessionForChat(app, fixture.root_id);
				return current && !current->processing && current->lifecycle_state == "ready";
			});
			if (mode == 8)
			{
				UAM_ASSERT(uam::StopAcpSession(app, fixture.root_id));
				await([&]
				{
					const auto* stopped = uam::FindAcpSessionForChat(app, fixture.root_id);
					if (!stopped || stopped->running || stopped->local_stop_pending || uam::AcpStopInProgress(app, fixture.root_id)) return false;
					errno = 0;
					return kill(parent_pid, 0) < 0 && errno == ESRCH;
				});
				errno = 0; UAM_ASSERT(kill(parent_pid, 0) < 0 && errno == ESRCH);
			}
			std::string new_error;
			// Supported ordinary public admission after a native completed-but-unacknowledged turn.
			UAM_ASSERT(uam::SendAcpPrompt(app, fixture.root_id, "PRIVATE-NEW-HUMAN-AFTER-UNKNOWN", {}, {}, false, &new_error));
			await([&]
			{
				const auto* current = uam::FindAcpSessionForChat(app, fixture.root_id);
				return fs::exists(fixture.temp.root / "wake-control-human-result.json") && current &&
				    !current->processing && current->lifecycle_state == "ready";
			});
			const auto observed = nlohmann::json::parse(ReadFile(fixture.temp.root / "wake-control-human-result.json"));
			const auto* parent = ChatDomainService().FindChatById(app, fixture.root_id); UAM_ASSERT(parent);
			UAM_ASSERT_EQ(std::ranges::count(parent->messages, observed["text"].get<std::string>(), &Message::content), 1);
			if (mode == 7)
			{
				const auto bound = observed["retired_turn"].get<std::string>();
				await([&]
				{
					const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.root_id, false);
					return saved && !saved->managed_parent_input_receipts.empty() &&
					    saved->managed_parent_input_receipts.back().accepted_turn_id == bound;
				});
				const auto* still_human = uam::FindAcpSessionForChat(app, fixture.root_id);
				UAM_ASSERT(still_human && !still_human->processing && still_human->lifecycle_state == "ready");
				const auto durable = ChatRepository::LoadLocalChat(app.data_root, fixture.root_id);
				UAM_ASSERT(durable && durable->managed_parent_input_receipts.back().accepted_turn_id == bound);
				UAM_ASSERT_EQ(std::ranges::count(durable->messages, observed["text"].get<std::string>(), &Message::content), 1);
			}
		}
		for (unsigned index = 0; index < 20; ++index) pulse();
		const auto* owner = ChatDomainService().FindChatById(app, fixture.root_id);
		const auto* current = uam::FindAcpSessionForChat(app, fixture.root_id);
		UAM_ASSERT(owner && owner->managed_parent_result && current && current->session_id == native && (mode == 8 ? current->child_pid != parent_pid : current->child_pid == parent_pid));
		if (mode == 3)
		{
			UAM_ASSERT_EQ(owner->managed_parent_result->delivered_ids, std::vector<std::string>{run_id});
			UAM_ASSERT(!current->processing);
			const auto observed = nlohmann::json::parse(ReadFile(fixture.temp.root / "wake-control-observed.json"));
			UAM_ASSERT_EQ(owner->managed_parent_result->accepted_turn_id, observed["turn"].get<std::string>());
			UAM_ASSERT_EQ(std::ranges::count(owner->messages, observed["text"].get<std::string>(), &Message::content), 1);
		}
		else
		{
			UAM_ASSERT(owner->managed_parent_result->delivered_ids.empty());
			UAM_ASSERT(owner->managed_parent_result->stage != "settled");
			if (mode == 0 || mode == 1) UAM_ASSERT(owner->managed_parent_result->accepted_turn_id.empty());
		}
		if (mode == 9 || mode == 10)
		{
			UAM_ASSERT(current->managed_parent_continuation && current->managed_parent_continuation->revoked);
			UAM_ASSERT_EQ(owner->managed_parent_result->stage, std::string("held"));
			UAM_ASSERT(owner->managed_parent_result->delivered_ids.empty());
			UAM_ASSERT(!uam::provider_availability::IsEnabled(app.settings, "codex-cli", "local"));
			if (mode == 10)
			{
				const auto observed = nlohmann::json::parse(ReadFile(fixture.temp.root / "wake-control-observed.json"));
				UAM_ASSERT_EQ(owner->managed_parent_result->accepted_turn_id, observed["turn"].get<std::string>());
				UAM_ASSERT_EQ(std::ranges::count(owner->messages, observed["text"].get<std::string>(), &Message::content), 1);
			}
		}
		if (mode == 11)
		{
			UAM_ASSERT(current->managed_parent_continuation && current->managed_parent_continuation->revoked);
			UAM_ASSERT_EQ(owner->managed_parent_result->stage, std::string("held"));
			UAM_ASSERT(owner->managed_parent_result->delivered_ids.empty());
			UAM_ASSERT_EQ(owner->managed_parent_input_receipts.size(), before_ack_receipts.size());
			auto expected = before_ack_receipts;
			const auto observed = nlohmann::json::parse(ReadFile(fixture.temp.root / "wake-control-observed.json"));
			expected.back().accepted_turn_id = observed["turn"].get<std::string>();
			UAM_ASSERT_EQ(owner->managed_parent_input_receipts, expected);
			const auto pre_ack = nlohmann::json::parse(ReadFile(retained_primary));
			UAM_ASSERT(pre_ack["managedParentInputReceipts"].back()["acceptedTurnId"].get<std::string>().empty());
			UAM_ASSERT_EQ(owner->managed_parent_result->payload, expected.back().payload); // No next batch overwrote its unconfirmed binding.
			fs::remove(primary); fs::rename(retained_primary, primary); // Restore only this test's original primary.
		}
		std::istringstream frames(ReadFile(fixture.temp.root / "wake-parent-inputs.jsonl"));
		std::string line; std::size_t count = 0;
		while (std::getline(frames, line)) if (!line.empty()) ++count;
		UAM_ASSERT_EQ(count, (mode == 6 || mode == 7 || mode == 8) ? std::size_t{3} : std::size_t{2}); // One explicit new human turn is distinct from replay.
		UAM_ASSERT_EQ(fixture.Inputs(), std::size_t{1});
		UAM_ASSERT_EQ(fixture.Launches(), std::size_t{1});
	}
}

UAM_TEST(CodexManagedParentContinuationRequiresExactRpcReceiptAcrossNotificationOrder)
{
	for (int mode = 0; mode < 12; ++mode) RunOrdinaryEndedTurnNativeReceiptControl(mode);
}

#endif

#if defined(__APPLE__) && UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_OPENCODE_CLI
#include "app/uam_control_service.h"
namespace
{
	// Reuse the existing actual-process pump, isolated PATH, durable global agent,
	// private executable wrapper, 30-second waits and Stop/reap machinery unchanged.
	struct ManagedGeminiDelegatePeerFixture : ManagedOpenCodeCasingFixture
	{
		ScopedEnvVar private_gemini_home{"GEMINI_CLI_HOME", (temp.root / "gemini-home").string()};
		struct RequiredProcessRetirement
		{
			ManagedOpenCodeCasingFixture& fixture;
			~RequiredProcessRetirement() noexcept
			{
				try
				{
					if (!fixture.StopAndReap())
					{
						std::cerr << "FATAL_PRIVATE_GEMINI_PROCESS_RETIREMENT_UNCONFIRMED\n";
						std::abort();
					}
					uam::UamControlService::Shutdown(fixture.app);
				}
				catch (...)
				{
					std::cerr << "FATAL_PRIVATE_GEMINI_PROCESS_RETIREMENT_EXCEPTION\n";
					std::abort();
				}
			}
		} required_retirement{*this}; // Constructed before launching body; destroyed before environment/base storage.
		const std::string task = "Return the single private Gemini delegated answer.";
		const std::string answer = "PRIVATE-MANAGED-GEMINI-DELEGATE-COMPLETE";
		const std::string child_model = "gemini-private-requested-model";
		const std::string native = "b418dce0-6b32-43eb-9f4e-79d4f18f9197";
		std::string capability_id;
		std::vector<Message> parent_messages;

		ManagedGeminiDelegatePeerFixture() : ManagedOpenCodeCasingFixture(true, false)
		{
			app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
			app.provider_profiles.front().interactive_command = ShellQuoteForTest(Executable().string());
			auto& root = app.chats.front();
			root.provider_id = "gemini-cli";
			root.uam_agent_id = "build";
			root.uam_control_enabled = true;
			root.uam_control_explicitly_disabled = false;
			root.command_safety_tier = "yolo"; // Existing ordinary agent_delegate admission requires this.
			root.approval_mode = "default";
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, root));
			parent_messages = root.messages;
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer-config.json", nlohmann::json{
			    {"data_root", app.data_root.string()}, {"root_id", root_id}, {"workspace", root.workspace_directory},
			    {"requested_model", child_model}, {"native", native}, {"task", task}, {"answer", answer}}.dump()));
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer.py", Peer()));
			fs::permissions(temp.root / "peer.py", fs::perms::owner_all, fs::perm_options::replace);
			const fs::path gemini = temp.root / "bin" / "gemini";
			UAM_ASSERT(uam::io::WriteTextFile(gemini, ReadFile(Executable())));
			fs::permissions(gemini, fs::perms::owner_all, fs::perm_options::replace);
			UAM_ASSERT(fs::is_regular_file(gemini) && !fs::is_symlink(gemini));
			AssertIsolated();

			std::string error;
			UAM_ASSERT(uam::UamControlService::Initialize(app, &error));
			auto parent = std::make_unique<uam::AcpSessionState>();
			parent->chat_id = root_id;
			parent->provider_id = "gemini-cli";
			parent->protocol_kind = "gemini-acp";
			parent->active_uam_agent_id = "build";
			parent->active_uam_agent_workspace_access = "write";
			// Only parent control transport is a sink; the delegated Gemini process below
			// must actually initialize, negotiate, accept one task and return a result.
			// Register the owner before launch so constructor failure cannot strand a
			// locally held positive process outside the mandatory retirement guard.
			app.acp_sessions.push_back(std::move(parent));
			auto& owned_parent = *app.acp_sessions.back();
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
			    owned_parent, temp.root, {"/bin/cat"}, &error));
			owned_parent.running = true;
			pids.insert(owned_parent.child_pid);
			nlohmann::json setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
			UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(
			    app, *app.acp_sessions.back(), root, "session/new", setup, &error));
			capability_id = app.acp_sessions.back()->uam_control_capability_id;
			UAM_ASSERT(!capability_id.empty() && app.uam_control_capabilities.size() == 1);
			// No child native identity, initialization, policy grant or positive handle is injected.
		}
		~ManagedGeminiDelegatePeerFixture() noexcept = default;
		// Failure-only observation before mandatory retirement; never changes startup gates.
		void DumpStartupBoundary() const noexcept
		{
			try
			{
				nlohmann::json report = {{"scope", "private-gemini-only-before-cleanup"}, {"waitBudgetSeconds", 30}, {"sessions", nlohmann::json::array()}};
				for (const auto& pointer : app.acp_sessions)
				{
					if (!pointer) continue;
					const auto& session = *pointer;
					// Only this generated private fixture can reach this failure-only observer.
					std::string private_error = session.chat_id == root_id ? std::string{} : session.last_error.substr(0, 256);
					for (char& character : private_error)
						if (static_cast<unsigned char>(character) < 32 || static_cast<unsigned char>(character) == 127) character = ' ';
					nlohmann::json private_reasons = nlohmann::json::array();
					for (std::size_t index = session.diagnostics.size() > 8 ? session.diagnostics.size() - 8 : 0; index < session.diagnostics.size(); ++index)
						private_reasons.push_back(session.diagnostics[index].reason);
					const int saved_errno = errno;
					const int signal_zero = session.child_pid > 0 ? kill(session.child_pid, 0) : -2;
					const int signal_errno = session.child_pid > 0 && signal_zero < 0 ? errno : 0;
					errno = saved_errno;
					report["sessions"].push_back({
					    {"role", session.chat_id == root_id ? "root-control-sink" : "delegated-source"},
					    {"provider", session.provider_id}, {"protocol", session.protocol_kind},
					    {"host", session.process_execution_host_id}, {"pid", session.child_pid}, {"hasProcessId", !session.last_process_id.empty()},
					    {"signalZeroResult", signal_zero}, {"signalZeroErrno", signal_errno},
					    {"running", session.running}, {"initialized", session.initialized}, {"sessionReady", session.session_ready},
					    {"lifecycle", session.lifecycle_state}, {"processing", session.processing}, {"hasNativeId", !session.session_id.empty()},
					    {"model", session.current_model_id}, {"mode", session.current_mode_id},
					    {"modelCount", session.available_models.size()}, {"modeCount", session.available_modes.size()},
					    {"configOptionCount", session.available_config_options.size()}, {"pendingRequestCount", session.pending_request_methods.size()},
					    {"queuedPromptBytes", session.queued_prompt.size()}, {"queuedPromptCount", session.queued_user_prompts.size()},
					    {"goalQueueAdmission", static_cast<int>(session.goal_queue_admission)}, {"queueHeld", session.queue_held},
					    {"pendingLocalReceipt", static_cast<bool>(session.pending_local_session_receipt)},
					    {"pendingPromptDelivery", static_cast<bool>(session.pending_prompt_delivery)},
					    {"pendingChatSave", app.pending_chat_save_at_by_chat_id.contains(session.chat_id)},
					    {"pendingChatSaveTask", app.pending_chat_save_tasks.contains(session.chat_id)},
					    {"checkpointPreflight", session.turn_checkpoint_preflight_pending},
					    {"waitingPermission", session.waiting_for_permission}, {"waitingInput", session.waiting_for_user_input},
					    {"cancelRequested", session.cancel_requested}, {"localStop", session.local_stop_pending},
					    {"remoteStop", session.remote_stop_pending}, {"remoteStopUnconfirmed", session.remote_stop_unconfirmed},
					    {"sourceExitObserved", session.process_exit_observed}, {"observedSourceExitCode", session.observed_process_exit_code},
					    {"reapedExitCode", session.reaped_exit_code}, {"hasLastExitCode", session.has_last_exit_code}, {"lastExitCode", session.last_exit_code},
					    {"remoteSourceExit", session.remote_source_exit_reported}, {"remoteSourceExitCode", session.remote_source_exit_code},
					    {"stdinFd", session.stdin_write_fd}, {"stdoutFd", session.stdout_read_fd}, {"stderrFd", session.stderr_read_fd},
					    {"stdoutPending", session.stdout_poll_pending}, {"stderrPending", session.stderr_poll_pending},
					    {"initializeRequest", session.initialize_request_id}, {"setupRequest", session.session_setup_request_id},
					    {"modeRequest", session.mode_change_request_id}, {"modelRequest", session.model_change_request_id},
					    {"startupModelRequest", session.startup_model_request_id}, {"reasoningRequest", session.reasoning_change_request_id},
					    {"configRequest", session.config_option_change_request_id}, {"awaitingModelConfig", session.awaiting_model_config_options},
					    {"promptRequest", session.prompt_request_id}, {"hasLastError", !session.last_error.empty()},
					    {"privateLastError256", private_error}, {"diagnosticReasonCodes", std::move(private_reasons)},
					    {"hasTransportOwnershipError", !session.transport_ownership_error.empty()}});
				}
				report["witnesses"] = nlohmann::json::object();
				for (const auto* name : {"launch-witness.json", "initialize-ack.json", "native-ack.json", "mode-ack.json", "model-ack.json", "input-witness.json", "permission-denial.json", "peer-error.json"})
					report["witnesses"][name] = fs::exists(temp.root / name);
				std::cerr << "PRIVATE_GEMINI_STARTUP_BOUNDARY " << report.dump() << '\n';
			}
			catch (...) { std::cerr << "PRIVATE_GEMINI_STARTUP_BOUNDARY_CAPTURE_FAILED\n"; }
		}
		static std::string Peer()
		{
			return R"UAM_GEMINI_PEER(#!/usr/bin/python3
import json, os, pathlib, sys, time
root = pathlib.Path(__file__).resolve().parent
config = json.loads((root / 'peer-config.json').read_text())
data = pathlib.Path(config['data_root'])
native = config['native']

def store(name, value):
    temporary = root / (name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, sort_keys=True)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(root / name)

def document(path):
    assert path.is_file() and not path.is_symlink() and path.stat().st_size <= 4 * 1024 * 1024
    return json.loads(path.read_text())

def state():
    runs = [document(path) for path in (data / 'agent-runs').glob('*.json')]
    assert len(runs) == 1
    run = runs[0]
    assert run['version'] == 1 and run['status'] == 'running'
    assert run['rootChatId'] == config['root_id'] and run['parentRunId'] == ''
    assert run['providerId'] == 'gemini-cli' and run['modelId'] == config['requested_model']
    assert run['effectiveWorkspaceAccess'] == 'read'
    transcript = document(data / 'chats' / (run['transcriptChatId'] + '.json'))
    assert transcript['agent_run_id'] == run['id'] and transcript['provider_id'] == 'gemini-cli'
    assert transcript['execution_host_id'] == 'local' and transcript['model_id'] == config['requested_model']
    assert transcript['approval_mode'] == 'plan' and transcript['commandSafetyTier'] == 'yolo'
    assert pathlib.Path(transcript['workspace_directory']).resolve() == pathlib.Path(config['workspace']).resolve()
    return run, transcript

def emit(value):
    print(json.dumps(value), flush=True)

def respond(request, result):
    emit(dict(jsonrpc='2.0', id=request['id'], result=result))

try:
    if sys.argv[1:] == ['--version']:
        store('version-witness.json', dict(pid=os.getpid(), executable=sys.argv[0]))
        print('0.55.1', flush=True)
        sys.exit(0)
    args = sys.argv[1:]
    assert args == ['--acp', '--approval-mode', 'plan', '--model', config['requested_model']]
    run, transcript = state()
    store('launch-witness.json', dict(pid=os.getpid(), args=args, run=run))
    model, mode, prompt = 'gemini-private-baseline', 'default', None
    for raw in sys.stdin.buffer:
        # Journal every complete received input before parsing or classifying it.
        with (root / 'raw-input.jsonl').open('ab') as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        request = json.loads(raw)
        if request.get('id') == 'private-denied-edit' and 'method' not in request:
            outcome = request.get('result', {}).get('outcome', {})
            assert outcome == dict(outcome='cancelled') or outcome == dict(outcome='selected', optionId='deny')
            store('permission-denial.json', request)
            assert prompt is not None
            deadline = time.monotonic() + 45
            while not (root / 'allow-completion').exists():
                assert time.monotonic() < deadline
                time.sleep(0.005)
            emit(dict(jsonrpc='2.0', method='session/update', params=dict(sessionId=native,
                update=dict(sessionUpdate='agent_message_chunk', content=dict(type='text', text=config['answer'])))))
            respond(prompt, dict(stopReason='end_turn'))
            continue
        method = request.get('method', '')
        if method == 'initialize':
            respond(request, dict(protocolVersion=1, agentInfo=dict(name='gemini', version='0.55.1'),
                agentCapabilities=dict(loadSession=True, mcpCapabilities=dict(http=True, sse=True))))
            store('initialize-ack.json', dict(id=request['id']))
        elif method == 'session/new':
            assert pathlib.Path(request['params']['cwd']).resolve() == pathlib.Path(config['workspace']).resolve()
            servers = request['params']['mcpServers']
            assert sum(server.get('name') == 'uam-control' for server in servers) == 1
            respond(request, dict(sessionId=native,
                models=dict(currentModelId=model, availableModels=[dict(modelId=model, name=model), dict(modelId=config['requested_model'], name='Exact private model')]),
                modes=dict(currentModeId=mode, availableModes=[dict(id='default', name='Default'), dict(id='plan', name='Plan')])))
            store('native-ack.json', dict(id=request['id'], native_id=native))
        elif method == 'session/set_model':
            assert request['params'] == dict(sessionId=native, modelId=config['requested_model'])
            model = config['requested_model']
            respond(request, {})
            store('model-ack.json', dict(id=request['id'], native_id=native, model=model))
        elif method == 'session/set_mode':
            assert request['params'] == dict(sessionId=native, modeId='plan')
            mode = 'plan'
            respond(request, {})
            store('mode-ack.json', dict(id=request['id'], native_id=native, mode=mode))
        elif method == 'session/prompt':
            assert prompt is None and model == config['requested_model'] and mode == 'plan'
            assert request['params']['sessionId'] == native
            run, transcript = state()
            assert transcript['native_session_id'] == native and run['expectedTurnSerial'] > 0
            users = [row for row in transcript['messages'] if row['role'] == 'user']
            assert len(users) == 1 and users[0]['content'] == config['task'] and not users[0].get('acp_prompt_not_sent', False)
            assert sum(part.get('text', '').count(config['task']) for part in request['params']['prompt']) == 1
            with (root / 'task-inputs.jsonl').open('a') as stream:
                stream.write(json.dumps(request, sort_keys=True) + '\n')
                stream.flush()
                os.fsync(stream.fileno())
            prompt = request
            store('input-witness.json', dict(pid=os.getpid(), run=run, transcript=transcript, request=request))
            emit(dict(jsonrpc='2.0', id='private-denied-edit', method='session/request_permission', params=dict(
                sessionId=native, toolCall=dict(toolCallId='private-edit', title='Edit private fixture', kind='edit', status='pending',
                    rawInput=dict(path=str(pathlib.Path(config['workspace']) / 'must-remain-absent'), content='forbidden')),
                options=[dict(optionId='allow', name='Allow', kind='allow_once'), dict(optionId='deny', name='Deny', kind='reject_once')])))
        elif method == 'session/cancel':
            if 'id' in request:
                respond(request, {})
        else:
            raise AssertionError('unexpected private Gemini ACP input')
except Exception as error:
    store('peer-error.json', dict(error=str(error)))
    sys.exit(70)
)UAM_GEMINI_PEER";
		}
	};
}

UAM_TEST(GeminiManagedAgentDelegateRunsExactPrivateNativeLifecycle)
{
	ManagedGeminiDelegatePeerFixture fixture;
	const auto delegated = uam::UamControlService::HandleRequestForTests(fixture.app, fixture.capability_id,
	    {{"requestId", "gemini-private-delegate"}, {"method", "agent_delegate"}, {"arguments", {
	        {"agentId", fixture.definition.id}, {"task", fixture.task}, {"providerId", "gemini-cli"}, {"modelId", fixture.child_model}}}},
	    uam::time::SystemEpochMillisecondsNow());
	UAM_ASSERT(delegated.value("ok", false));
	const std::string run_id = delegated.at("result").at("runId").get<std::string>();
	const auto queued = fixture.ReadRun();
	UAM_ASSERT_EQ(queued.id, run_id);
	UAM_ASSERT(!queued.managed_continuation && !queued.deliver_result_to_root_chat);
	UAM_ASSERT_EQ(queued.status, std::string("queued"));
	UAM_ASSERT_EQ(queued.provider_id, std::string("gemini-cli"));
	UAM_ASSERT_EQ(queued.model_id, fixture.child_model);
	UAM_ASSERT_EQ(queued.effective_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(queued.execution_capability, std::string("uam-prompt-injected"));
	UAM_ASSERT(!fs::exists(fixture.temp.root / "launch-witness.json"));
	try
	{
		fixture.Await([&] { return fs::exists(fixture.temp.root / "permission-denial.json"); });
	}
	catch (...)
	{
		fixture.DumpStartupBoundary();
		throw; // Preserve the original 30-second RED assertion and mandatory retirement.
	}
	UAM_ASSERT(fs::exists(fixture.temp.root / "initialize-ack.json"));
	UAM_ASSERT(fs::exists(fixture.temp.root / "native-ack.json"));
	UAM_ASSERT(fs::exists(fixture.temp.root / "mode-ack.json"));
	UAM_ASSERT(fs::exists(fixture.temp.root / "model-ack.json"));
	UAM_ASSERT_EQ(fixture.PromptCount(), std::size_t{1});
	const auto input = nlohmann::json::parse(ReadFile(fixture.temp.root / "input-witness.json"));
	const pid_t actual_child = input.at("pid").get<pid_t>();
	const auto* live = uam::FindAcpSessionForChat(fixture.app, queued.transcript_chat_id);
	UAM_ASSERT(live && live->running && live->processing && live->initialized && live->session_ready);
	UAM_ASSERT_EQ(live->child_pid, actual_child);
	UAM_ASSERT_EQ(live->protocol_kind, std::string("gemini-acp"));
	UAM_ASSERT_EQ(live->process_execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(live->session_id, fixture.native);
	UAM_ASSERT_EQ(live->current_model_id, fixture.child_model);
	UAM_ASSERT_EQ(live->current_mode_id, std::string("plan"));
	UAM_ASSERT_EQ(live->active_uam_agent_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(live->turn_serial, fixture.ReadRun().expected_turn_serial);
	UAM_ASSERT(!live->waiting_for_permission && !live->waiting_for_user_input && !live->cancel_requested);
	UAM_ASSERT(std::ranges::any_of(live->diagnostics, [](const uam::AcpDiagnosticEntryState& entry) {
		return entry.reason == "rejected_by_access_ceiling";
	}));
	UAM_ASSERT_EQ(input.at("transcript").at("native_session_id").get<std::string>(), fixture.native);
	UAM_ASSERT(!fs::exists(fixture.temp.root / "workspace" / "must-remain-absent"));
	const auto parent = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(parent && parent->messages == fixture.parent_messages && parent->provider_id == "gemini-cli");
	UAM_ASSERT_EQ(parent->model_id, fixture.requested);
	UAM_ASSERT_EQ(parent->command_safety_tier, std::string("yolo"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-completion", "release actual private Gemini result\n"));
	fixture.Await([&] { return fixture.ReadRun().status == "completed"; });
	const auto completed = fixture.ReadRun();
	UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
	UAM_ASSERT_EQ(completed.provider_id, std::string("gemini-cli"));
	UAM_ASSERT_EQ(completed.model_id, fixture.child_model);
	UAM_ASSERT(completed.diagnostic.empty() && completed.diagnostic_code.empty());
	UAM_ASSERT(completed.started_at_epoch_ms > 0 && completed.finished_at_epoch_ms >= completed.started_at_epoch_ms);
	const auto saved = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, queued.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(saved && saved->native_session_id == fixture.native && saved->model_id == fixture.child_model);
	UAM_ASSERT(std::ranges::any_of(saved->messages, [&](const Message& message) { return message.role == MessageRole::Assistant && message.content == fixture.answer; }));
	const auto status = uam::UamControlService::HandleRequestForTests(fixture.app, fixture.capability_id,
	    {{"requestId", "gemini-private-status"}, {"method", "agent_status"}, {"arguments", {{"runId", run_id}}}},
	    uam::time::SystemEpochMillisecondsNow());
	UAM_ASSERT(status.value("ok", false));
	UAM_ASSERT_EQ(status.at("result").at("status").get<std::string>(), std::string("completed"));
	UAM_ASSERT_EQ(status.at("result").at("result").get<std::string>(), fixture.answer);
	UAM_ASSERT_EQ(status.at("result").at("providerId").get<std::string>(), std::string("gemini-cli"));
	UAM_ASSERT_EQ(status.at("result").at("modelId").get<std::string>(), fixture.child_model);
	UAM_ASSERT_EQ(fixture.PromptCount(), std::size_t{1});
	// The ordinary scheduler must already have stopped/reaped its child; the
	// general fixture cleanup below cannot manufacture this success witness.
	UAM_ASSERT(uam::FindAcpSessionForChat(fixture.app, queued.transcript_chat_id) == nullptr);
	int exit_status = 0;
	errno = 0;
	UAM_ASSERT(waitpid(actual_child, &exit_status, WNOHANG) == -1 && errno == ECHILD);
	errno = 0;
	UAM_ASSERT(kill(actual_child, 0) == -1 && errno == ESRCH);
	UAM_ASSERT(fixture.pids.contains(actual_child));
	UAM_ASSERT(fixture.StopAndReap());
	UAM_ASSERT_EQ(fixture.PromptCount(), std::size_t{1});
	const std::string journal = ReadFile(fixture.temp.root / "raw-input.jsonl");
	UAM_ASSERT(!journal.empty() && journal.back() == '\n');
	std::istringstream lines(journal);
	std::size_t prompts = 0, denials = 0;
	for (std::string line; std::getline(lines, line);)
	{
		const auto frame = nlohmann::json::parse(line);
		if (frame.value("method", "") == "session/prompt") ++prompts;
		if (frame.contains("id") && frame["id"] == "private-denied-edit" && !frame.contains("method")) ++denials;
	}
	UAM_ASSERT_EQ(prompts, std::size_t{1});
	UAM_ASSERT_EQ(denials, std::size_t{1});
}
#endif

#if defined(__APPLE__) && UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_OPENCODE_CLI
#include "app/transcript_retention_service.h"
namespace
{
	struct ManagedGeminiInitialBindingFixture : ManagedOpenCodeCasingFixture
	{
		ScopedEnvVar private_home{"GEMINI_CLI_HOME", (temp.root / "gemini-home").string()};
		struct Retirement
		{
			ManagedOpenCodeCasingFixture& fixture;
			~Retirement() noexcept
			{
				try
				{
					if (!fixture.StopAndReap()) std::abort();
				}
				catch (...)
				{
					std::abort();
				}
			}
		} retirement{*this}; // Stop before private home/PATH/data disappear, including failed assertions.
		const std::string native = "b418dce0-6b32-43eb-9f4e-79d4f18f9197";

		explicit ManagedGeminiInitialBindingFixture(const std::string& fault, const std::string& version) : ManagedOpenCodeCasingFixture(true, false)
		{
			app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
			app.provider_profiles.front().interactive_command = ShellQuoteForTest(Executable().string());
			app.chats.front().provider_id = "gemini-cli";
			app.chats.front().model_id = "gemini-3.1-pro-preview";
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			auto config = nlohmann::json::parse(ReadFile(temp.root / "peer-config.json"));
			config["fault"] = fault;
			config["native_version"] = version;
			config["canonical_model"] = config["requested_model"] = app.chats.front().model_id;
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer-config.json", config.dump()));
			std::string peer = ManagedOpenCodeCasingPeer();
			const auto replace = [&](const std::string& before, const std::string& after)
			{
				const auto position = peer.find(before);
				UAM_ASSERT(position != std::string::npos && peer.find(before, position + before.size()) == std::string::npos);
				peer.replace(position, before.size(), after);
			};
			replace("native = 'ses_private_managed_model_casing'", "native = '" + native + "' if config['fault'] != 'noncanonical-id' else 'not-a-native-uuid'");
			replace("print('1.18.35', flush=True)", "print(config['native_version'], flush=True)");
			const auto begin = peer.find("    assert sys.argv[1:] == ['acp']");
			const auto end = peer.find("    for line in sys.stdin:", begin);
			UAM_ASSERT(begin != std::string::npos && end != std::string::npos);
			peer.replace(begin, end - begin, R"GEMINI(
    assert sys.argv[1:] == ['--acp', '--approval-mode', 'plan', '--model', config['requested_model']]
    agent = 'plan'
    model = config['canonical_model']
    run, turn, transcript = state()
    store('launch-witness.json', dict(pid=os.getpid(), args=sys.argv[1:], run=run))
)GEMINI");
			replace("agentInfo=dict(name='opencode', title='Private OpenCode ACP', version='1.18.35')",
			    "agentInfo=dict(name='gemini-cli' if config['fault'] != 'wrong-agent' else 'other-agent', title='Gemini CLI', version=config['native_version'])");
			replace("            respond(request, dict(sessionId=native, configOptions=options(model)))", R"GEMINI(            result = dict(sessionId=native,
                models=dict(currentModelId=model, availableModels=[dict(modelId=model, name=model)]),
                modes=dict(currentModeId='plan', availableModes=[dict(id='plan', name='Plan')]))
            if config['fault'] == 'missing-model':
                del result['models']['currentModelId']
            elif config['fault'] == 'missing-policy':
                del result['modes']['currentModeId']
            elif config['fault'] == 'normalized-model':
                result['models']['currentModelId'] = ' ' + model + ' '
            elif config['fault'] == 'config-options':
                result['configOptions'] = options(model)
            elif config['fault'] == 'unadvertised-model':
                result['models']['availableModels'] = []
            elif config['fault'] == 'duplicate-model':
                result['models']['availableModels'] *= 2
            elif config['fault'] == 'unadvertised-plan':
                result['modes']['availableModes'] = []
            store('native-proof.json', result)
            respond(request, result))GEMINI");
			// Native Gemini setters return empty acknowledgements, unlike the base
			// OpenCode peer's configuration readback. They cannot fill missing proof.
			replace("            store('model-ack.json', dict(id=request['id'], native_id=native, model=model))\n            respond(request, dict(configOptions=options(model)))",
			    "            store('model-ack.json', dict(id=request['id'], native_id=native, model=model))\n            respond(request, {})");
			replace("            assert request['params']['sessionId'] == native and request['params']['modeId'] == agent\n            respond(request, dict(configOptions=options(model)))",
			    "            assert request['params']['sessionId'] == native and request['params']['modeId'] == agent\n            respond(request, {})");
			replace("            emit(dict(jsonrpc='2.0', method='session/update', params=dict(sessionId=native,\n                update=dict(sessionUpdate='agent_message_chunk'", R"GEMINI(            if config['fault'] == 'transient-policy':
                for mode in ['default', 'plan']:
                    emit(dict(jsonrpc='2.0', method='session/update', params=dict(sessionId=native,
                        update=dict(sessionUpdate='current_mode_update', currentModeId=mode))))
            elif config['fault'] == 'late-config':
                emit(dict(jsonrpc='2.0', method='session/update', params=dict(sessionId=native,
                    update=dict(sessionUpdate='config_option_update', configOptions=options(model)))))
            elif config['fault'] == 'foreign-update':
                emit(dict(jsonrpc='2.0', method='session/update', params=dict(sessionId='44444444-4444-4444-8444-444444444444',
                    update=dict(sessionUpdate='agent_message_chunk', content=dict(type='text', text='')))))
            elif config['fault'] == 'native-text-policy':
                for mode in ['default', 'plan']:
                    emit(dict(jsonrpc='2.0', method='session/update', params=dict(sessionId=native,
                        update=dict(sessionUpdate='agent_message_chunk', content=dict(type='text', text='[MODE_UPDATE] ' + mode)))))
            emit(dict(jsonrpc='2.0', method='session/update', params=dict(sessionId=native,
                update=dict(sessionUpdate='agent_message_chunk')GEMINI");
			// Tagged 0.55.1/0.64.0 prompt responses report executed per-model usage;
			// the earlier setup selection is deliberately insufficient for publication.
			replace("            respond(request, dict(stopReason='end_turn'))", R"GEMINI(            counts = dict(input_tokens=11, output_tokens=7)
            result = dict(stopReason='end_turn', _meta=dict(quota=dict(token_count=dict(counts),
                model_usage=[dict(model=model, token_count=dict(counts))])))
            if config['native_version'] == '0.64.0':
                result['usage'] = dict(inputTokens=11, outputTokens=7, totalTokens=18)
            if config['fault'] == 'missing-execution-model':
                del result['_meta']['quota']['model_usage']
            elif config['fault'] == 'changed-execution-model':
                result['_meta']['quota']['model_usage'][0]['model'] = 'different-native-model'
            elif config['fault'] == 'mixed-execution-models':
                result['_meta']['quota']['model_usage'].append(dict(model='other-native-model', token_count=dict(counts)))
            elif config['fault'] == 'zero-work':
                result['_meta']['quota']['token_count'] = dict(input_tokens=0, output_tokens=0)
                result['_meta']['quota']['model_usage'][0]['token_count'] = dict(input_tokens=0, output_tokens=0)
            elif config['fault'] == 'contradictory-counts':
                result['_meta']['quota']['token_count']['input_tokens'] += 1
            elif config['fault'] == 'contradictory-standard-usage':
                result['usage']['inputTokens'] += 1
            store('execution-model-proof.json', result)
            respond(request, result))GEMINI");
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer.py", peer));
			fs::permissions(temp.root / "peer.py", fs::perms::owner_all, fs::perm_options::replace);
			const auto executable = temp.root / "bin" / "gemini";
			UAM_ASSERT(uam::io::WriteTextFile(executable, ReadFile(Executable())));
			fs::permissions(executable, fs::perms::owner_all, fs::perm_options::replace);
			UAM_ASSERT(fs::is_regular_file(executable) && !fs::is_symlink(executable));
			AssertIsolated();
		}
	};

	void CheckManagedGeminiInitialBinding(const std::string& fault, const std::string& version = "0.55.1")
	{
		ManagedGeminiInitialBindingFixture fixture(fault, version);
		auto completion = std::make_shared<std::optional<uam::ManagedInitialMentionResult>>();
		std::string error;
		UAM_ASSERT_EQ(uam::AgentRunScheduler::TryBeginInitialMention(fixture.app, fixture.root_id,
		    "@" + fixture.definition.id + " " + fixture.task,
		    [completion](const auto& result) { *completion = result; return true; }, &error), uam::ManagedInitialMentionAdmission::Pending);
		fixture.Await([&] { return completion->has_value(); });
		UAM_ASSERT((*completion)->input_recorded && (*completion)->delegation_state == uam::ManagedInitialDelegationState::Committed);
		const auto initial = fixture.ReadRun();
		UAM_ASSERT(initial.managed_continuation && !initial.managed_continuation->execution);
		fixture.Await([&] { return fs::exists(fixture.temp.root / "input-witness.json"); });
		const auto input = nlohmann::json::parse(ReadFile(fixture.temp.root / "input-witness.json"));
		const auto child_pid = input.at("pid").get<pid_t>();
		UAM_ASSERT(child_pid > 0 && !fixture.ReadRun().managed_continuation->execution);
		UAM_ASSERT_EQ(fixture.PromptCount(), std::size_t{1});
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-completion", "release owned Gemini initial proof\n"));
		fixture.Await([&]
		{
			const auto run = fixture.ReadRun();
			return run.status == "completed" && run.root_result_delivered && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.acp_sessions.empty();
		});
		const auto completed = fixture.ReadRun();
		const auto proof = nlohmann::json::parse(ReadFile(fixture.temp.root / "native-proof.json"));
		const auto execution_proof = nlohmann::json::parse(ReadFile(fixture.temp.root / "execution-model-proof.json"));
		const bool bound = fault == "none";
		UAM_ASSERT_EQ(completed.managed_continuation->execution.has_value(), bound);
		UAM_ASSERT_EQ(completed.id, initial.id);
		UAM_ASSERT_EQ(completed.managed_continuation->owner, initial.managed_continuation->owner);
		UAM_ASSERT_EQ(completed.managed_continuation->turns.size(), std::size_t{1});
		if (fault == "native-text-policy")
			UAM_ASSERT(completed.result_excerpt.find(fixture.answer) != std::string::npos);
		else
			UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
		UAM_ASSERT_EQ(fixture.PromptCount(), std::size_t{1});
		if (bound)
		{
			const auto& binding = *completed.managed_continuation->execution;
			UAM_ASSERT_EQ(binding.native_session_id, proof.at("sessionId").get<std::string>());
			UAM_ASSERT_EQ(binding.effective_model_id, proof.at("models").at("currentModelId").get<std::string>());
			UAM_ASSERT_EQ(binding.effective_model_id, execution_proof.at("_meta").at("quota").at("model_usage").front().at("model").get<std::string>());
			UAM_ASSERT_EQ(binding.approval_mode, proof.at("modes").at("currentModeId").get<std::string>());
			UAM_ASSERT_EQ(binding.workspace_directory, input.at("transcript").at("workspace_directory").get<std::string>());
			UAM_ASSERT_EQ(binding.model_identity_source, std::string("exact-requested"));
		}
		const auto state = uam::ManagedSubagentFollowupService::GetState(fixture.app, fixture.root_id, completed.id);
		UAM_ASSERT(!state.can_send);
		UAM_ASSERT_EQ(state.code, bound ? uam::ManagedFollowupCode::NativeResumeUnavailable : uam::ManagedFollowupCode::ModelIdentityUnverified);
		int status = 0;
		errno = 0;
		UAM_ASSERT(waitpid(child_pid, &status, WNOHANG) == -1 && errno == ECHILD);
		errno = 0;
		UAM_ASSERT(kill(child_pid, 0) == -1 && errno == ESRCH);
		if (!bound) return;
		// Cross the production producer -> saved ledger -> restart inspection route.
		// Copy only this fixture's three owned primary/backup pairs, never provider history.
		TempDir restart_root("uam-gemini-bound-inspection");
		std::map<fs::path, std::string> originals;
		for (const auto& primary : {fixture.app.data_root / "agent-runs" / (completed.id + ".json"), AppPaths::UamChatFilePath(fixture.app.data_root, fixture.root_id), AppPaths::UamChatFilePath(fixture.app.data_root, completed.transcript_chat_id)})
			for (const auto& path : {primary, uam::io::MakeBackupPath(primary)})
				if (fs::exists(path))
				{
					const auto relative = path.lexically_relative(fixture.app.data_root);
					originals.emplace(relative, ReadFile(path));
					UAM_ASSERT(uam::io::WriteTextFile(restart_root.root / relative, originals.at(relative)));
				}
		const auto refuse_continuation = [&](uam::AppState& app, uam::ManagedFollowupCode expected)
		{
			uam::ManagedFollowupRequest request;
			request.after_turn_id = completed.managed_continuation->current_turn_id;
			request.client_request_id = "1dd8a611-7462-4ac4-869c-3d80c4bab477";
			request.text = "This initial binding does not authorize another input.";
			bool called = false;
			uam::ManagedFollowupCode rejection = uam::ManagedFollowupCode::None;
			UAM_ASSERT_EQ(uam::ManagedSubagentFollowupService::TrySubmit(app, fixture.root_id, completed.id, request,
			    [&](const auto&) { called = true; return true; }, &rejection), uam::ManagedFollowupAdmission::Rejected);
			UAM_ASSERT_EQ(rejection, expected);
			UAM_ASSERT(!called && app.managed_followups.empty() && app.managed_followup_completed_owners.empty() && app.queued_agent_run_ids.empty() && app.acp_sessions.empty());
		};
		refuse_continuation(fixture.app, uam::ManagedFollowupCode::NativeResumeUnavailable);
		uam::AppState restarted;
		restarted.data_root = restart_root.root;
		restarted.settings = fixture.app.settings;
		restarted.provider_profiles = fixture.app.provider_profiles;
		auto startup = uam::AgentRunLedger::LoadAll(restarted.data_root);
		UAM_ASSERT(uam::AgentRunLedger::MarkNonterminalInterrupted(restarted.data_root, &startup.runs, "manager_restarted", &startup.errors));
		restarted.agent_runs = std::move(startup.runs);
		UAM_ASSERT(restarted.agent_runs.empty());
		ChatHistorySyncService().LoadSidebarChats(restarted);
		UAM_ASSERT(restarted.managed_initial_history);
		std::unordered_map<std::string, std::size_t> history_counts;
		uam::ManagedSubagentControlService::AddRestartInspectionCounts(restarted, history_counts);
		UAM_ASSERT_EQ(history_counts.at(fixture.root_id), std::size_t{1});
		uam::ManagedSubagentPage page;
		if (!uam::ManagedSubagentControlService::List(restarted, fixture.root_id, &page, &error))
			throw TestFailure("Gemini restart inspection: " + error);
		UAM_ASSERT_EQ(page.total_count, std::size_t{1});
		UAM_ASSERT_EQ(page.runs.size(), std::size_t{1});
		UAM_ASSERT_EQ(page.runs.front().run_id, completed.id);
		UAM_ASSERT_EQ(page.runs.front().provider_id, completed.provider_id);
		UAM_ASSERT_EQ(page.runs.front().model_id, completed.model_id);
		UAM_ASSERT_EQ(page.runs.front().status, completed.status);
		UAM_ASSERT_EQ(page.runs.front().task_title, fixture.task);
		UAM_ASSERT(page.runs.front().last_reported && !page.runs.front().can_cancel);
		const auto display = uam::ManagedSubagentControlService::ToJson(page).dump();
		UAM_ASSERT(display.find("connectionConfigurationJson") == std::string::npos && display.find("providerConfigurationJson") == std::string::npos);
		const auto identity = uam::CaptureManagedAgentTranscriptRead(restarted, fixture.root_id, completed.transcript_chat_id);
		UAM_ASSERT(identity && identity->initial_history && identity->history_error.empty());
		const auto read = std::async(std::launch::async, [&] { return uam::LoadManagedAgentTranscriptDetached(restarted.data_root, *identity); }).get();
		UAM_ASSERT_EQ(read.status, uam::ManagedAgentTranscriptReadStatus::Ready);
		UAM_ASSERT(uam::MatchesManagedAgentTranscriptRead(restarted, *identity, read.transcript));
		UAM_ASSERT_EQ(read.transcript.native_session_id, fixture.native);
		UAM_ASSERT_EQ(std::ranges::count(read.transcript.messages, fixture.task, &Message::content), 1);
		UAM_ASSERT_EQ(std::ranges::count(read.transcript.messages, fixture.answer, &Message::content), 1);
		UAM_ASSERT(!uam::ManagedSubagentFollowupService::GetState(restarted, fixture.root_id, completed.id).can_send);
		refuse_continuation(restarted, uam::ManagedFollowupCode::ScopeChanged);
		UAM_ASSERT(restarted.agent_runs.empty() && restarted.queued_agent_run_ids.empty() && restarted.acp_sessions.empty() && restarted.managed_initial_runtime_owners.empty() && restarted.managed_followup_completed_owners.empty());
		for (const auto& [relative, bytes] : originals)
		{
			UAM_ASSERT_EQ(ReadFile(restart_root.root / relative), bytes);
			UAM_ASSERT_EQ(ReadFile(fixture.app.data_root / relative), bytes);
		}
		const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(restarted.data_root);
		UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1);
		UAM_ASSERT_EQ(ledger.runs.front().managed_continuation, completed.managed_continuation);
		UAM_ASSERT_EQ(fixture.PromptCount(), std::size_t{1});
	}
}

UAM_TEST(ManagedInitialGeminiRawPlanReplyBindsOnlyAfterOwnedCompletionSaveAndReap)
{
	for (const std::string version : {"0.55.1", "0.64.0"})
		CheckManagedGeminiInitialBinding("none", version);
}

UAM_TEST(ManagedInitialGeminiMissingNormalizedOrChangedProofStaysUnbound)
{
	for (const std::string fault : {"missing-model", "missing-policy", "normalized-model", "config-options", "transient-policy", "late-config", "noncanonical-id", "foreign-update", "unadvertised-model", "duplicate-model", "unadvertised-plan", "wrong-agent", "native-text-policy", "missing-execution-model", "changed-execution-model", "mixed-execution-models", "zero-work", "contradictory-counts"})
		CheckManagedGeminiInitialBinding(fault);
	CheckManagedGeminiInitialBinding("unqualified-version", "0.55.2");
	CheckManagedGeminiInitialBinding("contradictory-standard-usage", "0.64.0");
}
#endif

#if defined(__APPLE__) && UAM_ENABLE_RUNTIME_OPENCODE_CLI
#include "app/uam_control_service.h"
#include <cstdlib>
namespace
{
	// Ordinary public version-1 run adapter of the existing actual ACP peer.
	// This reads scheduler-produced state; it never writes a run or completed result.
	std::string OrdinaryOpenCodePublicPeer()
	{
		return R"UAM_OC_PUBLIC(#!/usr/bin/python3
import json, os, pathlib, sys, time
root = pathlib.Path(__file__).resolve().parent
config = json.loads((root / 'peer-config.json').read_text())
data = pathlib.Path(config['data_root'])
native = 'ses_private_managed_model_casing'

def store(name, value):
    target = root / name
    temporary = root / (name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, sort_keys=True)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(target)

def document(path):
    assert path.is_file() and not path.is_symlink() and path.stat().st_size <= 4 * 1024 * 1024
    return json.loads(path.read_text())

def state():
    runs = [document(path) for path in (data / 'agent-runs').glob('*.json')]
    assert len(runs) == 1
    run = runs[0]
    assert run['version'] == 1 and run['status'] == 'running'
    assert run['rootChatId'] == config['root_id'] and run['parentRunId'] == ''
    assert run['providerId'] == 'opencode-cli' and run['modelId'] == config['requested_model']
    assert run['executionCapability'] == 'opencode-native-agent-config'
    assert run['effectiveWorkspaceAccess'] == 'read'
    transcript = document(data / 'chats' / (run['transcriptChatId'] + '.json'))
    assert transcript['agent_run_id'] == run['id'] and transcript['provider_id'] == 'opencode-cli'
    assert transcript['execution_host_id'] == 'local' and transcript['model_id'] == config['requested_model']
    assert transcript['approval_mode'] == 'plan' and transcript['commandSafetyTier'] == 'yolo'
    assert pathlib.Path(transcript['workspace_directory']).resolve() == pathlib.Path(config['workspace']).resolve()
    return run, transcript

def options(current):
    models = [config['canonical_model'], 'private/baseline']
    if config['ambiguous']:
        models.insert(1, 'private/Glm-4.6')
    return [dict(id='model', category='model', type='select', currentValue=current,
        options=[dict(value=value, name=value) for value in models]),
        dict(id='mode', category='mode', type='select', currentValue=agent,
        options=[dict(value=agent, name='Private captured native agent')])]

def emit(value):
    print(json.dumps(value), flush=True)

def respond(request, result):
    emit(dict(jsonrpc='2.0', id=request['id'], result=result))

try:
    if sys.argv[1:] == ['--version']:
        store('version-witness.json', dict(pid=os.getpid(), executable=sys.argv[0]))
        print('1.18.35', flush=True)
        sys.exit(0)
    assert sys.argv[1:] == ['acp'], 'private native-agent launch only'
    adapter = pathlib.Path(os.environ['OPENCODE_CONFIG_DIR']).resolve()
    assert adapter.is_relative_to(data.resolve())
    settings = document(adapter / 'opencode.json')
    agent = settings['default_agent']
    assert settings['subagent_depth'] == 0 and settings['permission']['task'] == 'deny'
    assert (adapter / 'agent' / (agent + '.md')).is_file()
    run, transcript = state()
    store('launch-witness.json', dict(pid=os.getpid(), args=sys.argv[1:], run=run, agent=agent))
    model = 'private/baseline'
    prompt = None
    for line in sys.stdin:
        request = json.loads(line)
        method = request.get('method', '')
        with (root / 'requests.jsonl').open('a') as stream:
            stream.write(json.dumps(request, sort_keys=True) + '\n')
            stream.flush()
            os.fsync(stream.fileno())
        if request.get('id') == 'private-opencode-denied-edit' and 'method' not in request:
            outcome = request.get('result', {}).get('outcome', {})
            assert outcome == dict(outcome='cancelled') or outcome == dict(outcome='selected', optionId='deny')
            assert prompt is not None
            store('permission-denial.json', request)
            deadline = time.monotonic() + 45
            while not (root / 'allow-completion').exists():
                assert time.monotonic() < deadline
                time.sleep(0.005)
            emit(dict(jsonrpc='2.0', method='session/update', params=dict(sessionId=native,
                update=dict(sessionUpdate='agent_message_chunk', content=dict(type='text', text=config['answer'])))))
            respond(prompt, dict(stopReason='end_turn'))
            continue
        if method == 'initialize':
            respond(request, dict(protocolVersion=1,
                agentInfo=dict(name='opencode', title='Private OpenCode ACP', version='1.18.35'),
                agentCapabilities=dict(loadSession=True, mcpCapabilities=dict(http=True, sse=True))))
            store('initialize-ack.json', dict(id=request['id']))
        elif method == 'session/new':
            assert pathlib.Path(request['params']['cwd']).resolve() == pathlib.Path(config['workspace']).resolve()
            store('native-ack.json', dict(id=request['id'], native_id=native))
            respond(request, dict(sessionId=native, configOptions=options(model)))
        elif method == 'session/set_model':
            assert not config['ambiguous'], 'ambiguous model must be refused without selecting one'
            assert request['params']['sessionId'] == native
            assert request['params']['modelId'] == config['canonical_model']
            model = config['canonical_model']
            store('model-ack.json', dict(id=request['id'], native_id=native, model=model))
            respond(request, dict(configOptions=options(model)))
        elif method == 'session/set_mode':
            assert request['params']['sessionId'] == native and request['params']['modeId'] == agent
            respond(request, dict(configOptions=options(model)))
        elif method == 'session/prompt':
            assert not config['ambiguous']
            assert request['params']['sessionId'] == native and model == config['canonical_model']
            run, transcript = state()
            assert transcript['native_session_id'] == native
            assert transcript['model_id'] == config['canonical_model']
            assert prompt is None and run['expectedTurnSerial'] > 0
            users = [row for row in transcript['messages'] if row['role'] == 'user']
            assert len(users) == 1 and users[0]['content'] == config['task']
            assert not users[0].get('acp_prompt_not_sent', False)
            prompt = request['params']['prompt']
            assert sum(part.get('text', '').count(config['task']) for part in prompt) == 1
            with (root / 'task-inputs.jsonl').open('a') as stream:
                stream.write(json.dumps(request, sort_keys=True) + '\n')
                stream.flush()
                os.fsync(stream.fileno())
            store('input-witness.json', dict(pid=os.getpid(), run=run, transcript=transcript, request=request))
            prompt = request
            emit(dict(jsonrpc='2.0', id='private-opencode-denied-edit', method='session/request_permission', params=dict(
                sessionId=native, toolCall=dict(toolCallId='private-opencode-edit', title='Private denied edit', kind='edit', status='pending',
                    rawInput=dict(path=str(pathlib.Path(config['workspace']) / 'must-remain-absent'), content='forbidden')),
                options=[dict(optionId='allow', name='Allow', kind='allow_once'), dict(optionId='deny', name='Deny', kind='reject_once')])))
        elif method == 'session/cancel':
            if 'id' in request:
                respond(request, {})
        else:
            raise AssertionError('unexpected private ACP request: ' + method)
except Exception as error:
    store('peer-error.json', dict(error=str(error)))
    sys.exit(70)
)UAM_OC_PUBLIC";
	}
}

UAM_TEST(OpenCodeCppMcpDelegateReturnsOwnedNativeChildResult)
{
	const std::optional<std::string> original_home = std::getenv("HOME") ? std::optional<std::string>(std::getenv("HOME")) : std::nullopt;
	ManagedOpenCodeCasingFixture fixture(true, false);
	ScopedEnvVar private_data_root{"UAM_DATA_DIR", fixture.app.data_root.string()};
	uam::AcpSessionState mcp;
	struct RequiredRetirement
	{
		ManagedOpenCodeCasingFixture& fixture;
		uam::AcpSessionState& mcp;
		bool retired = false;
		static bool Reaped(const pid_t pid) noexcept
		{
			if (pid <= 0) return true;
			int status = 0;
			errno = 0;
			const pid_t observed = waitpid(pid, &status, WNOHANG);
			if (observed == pid) return true;
			if (observed < 0 && errno == ECHILD)
			{
				errno = 0;
				return kill(pid, 0) < 0 && errno == ESRCH;
			}
			return false;
		}
		bool Retire() noexcept
		{
			if (retired) return true;
			try
			{
				const pid_t client_pid = mcp.child_pid;
				auto& process = PlatformServicesFactory::Instance().process_service;
				process.StopStdioProcess(mcp, true);
				process.CloseStdioProcessHandles(mcp);
				const bool client_reaped = Reaped(client_pid);
				const bool native_reaped = fixture.StopAndReap();
				if (!client_reaped || !native_reaped) std::abort();
				uam::UamControlService::Shutdown(fixture.app);
				retired = true;
				return true;
			}
			catch (...)
			{
				std::abort();
			}
		}
		~RequiredRetirement() noexcept
		{
			(void)Retire();
		}
	} cleanup{fixture, mcp};
	auto& app = fixture.app;
	auto& root = app.chats.front();
	root.uam_agent_id = "build";
	root.uam_control_enabled = true;
	root.uam_control_explicitly_disabled = false;
	root.command_safety_tier = "yolo";
	root.approval_mode = "default";
	app.settings.git_snapshot_plain_folders = false; // This protocol case pins the private folder, not VCS isolation.
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, root));
	const auto parent_messages = root.messages;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "peer.py", OrdinaryOpenCodePublicPeer()));
	fs::permissions(fixture.temp.root / "peer.py", fs::perms::owner_all, fs::perm_options::replace);
	fixture.AssertIsolated();
	UAM_ASSERT_EQ(std::string(std::getenv("UAM_DATA_DIR")), app.data_root.string());
	UAM_ASSERT_EQ(std::getenv("HOME") ? std::optional<std::string>(std::getenv("HOME")) : std::nullopt, original_home);
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error));
	// The parent is explicitly a C++ MCP client with a scoped control sink.
	// No native parent-model invocation or parent-model result consumption is claimed.
	auto parent = std::make_unique<uam::AcpSessionState>();
	parent->chat_id = fixture.root_id;
	parent->provider_id = "opencode-cli";
	parent->protocol_kind = "opencode-acp";
	parent->active_uam_agent_id = "build";
	parent->active_uam_agent_workspace_access = "write";
	app.acp_sessions.push_back(std::move(parent));
	auto& parent_sink = *app.acp_sessions.back();
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(parent_sink, fixture.temp.root, {"/bin/cat"}, &error));
	UAM_ASSERT(parent_sink.child_pid > 0);
	UAM_ASSERT(kill(parent_sink.child_pid, 0) == 0);
	parent_sink.running = true;
	fixture.pids.insert(parent_sink.child_pid);
	nlohmann::json setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, parent_sink, root, "session/new", setup, &error));
	const std::string capability = parent_sink.uam_control_capability_id;
	UAM_ASSERT(!capability.empty());
	UAM_ASSERT(std::ranges::any_of(app.uam_control_capabilities, [&](const auto& item)
	{
		return item.id == capability && item.chat_id == fixture.root_id && item.session_chat_id == fixture.root_id;
	}));
	const auto& servers = setup.at("params").at("mcpServers");
	UAM_ASSERT_EQ(servers.size(), std::size_t{1});
	const auto& server = servers.front();
	UAM_ASSERT_EQ(server.at("name"), nlohmann::json("uam-control"));
	UAM_ASSERT_EQ(server.at("command").get<std::string>(), PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().string());
	std::vector<std::string> argv{server.at("command").get<std::string>()};
	for (const auto& arg : server.at("args")) argv.push_back(arg.get<std::string>());
	UAM_ASSERT_EQ(argv.size(), std::size_t{2});
	UAM_ASSERT_EQ(argv.back(), std::string("--uam-control-mcp"));
	std::vector<std::pair<std::string, std::string>> environment;
	for (const auto& entry : server.at("env")) environment.emplace_back(entry.at("name").get<std::string>(), entry.at("value").get<std::string>());
	auto& process = PlatformServicesFactory::Instance().process_service;
	UAM_ASSERT(process.StartStdioProcess(mcp, fixture.temp.root, argv, &error, environment));
	UAM_ASSERT(mcp.child_pid > 0);
	fixture.pids.insert(mcp.child_pid);
	std::string output;
	const auto call = [&](int id, const std::string& method, const nlohmann::json& params)
	{
		const auto line = nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump() + "\n";
		UAM_ASSERT(process.WriteToStdioProcess(mcp, line.data(), line.size(), &error));
		nlohmann::json response;
		fixture.Await([&]
		{
			if (!response.is_null()) return true;
			(void)uam::UamControlService::ProcessPendingRequests(app);
			std::array<char, 16384> bytes{};
			const auto count = process.ReadStdioProcessStdout(mcp, bytes.data(), bytes.size(), &error);
			UAM_ASSERT(count >= 0 || count == -2);
			if (count > 0) output.append(bytes.data(), static_cast<std::size_t>(count));
			UAM_ASSERT(output.size() <= 1024 * 1024);
			const auto end = output.find('\n');
			if (end == std::string::npos) return false;
			response = nlohmann::json::parse(output.substr(0, end));
			output.erase(0, end + 1);
			UAM_ASSERT_EQ(response.at("id"), nlohmann::json(id));
			return true;
		});
		UAM_ASSERT(response.contains("result") && !response.contains("error"));
		return response.at("result");
	};
	UAM_ASSERT(call(1, "initialize", {{"protocolVersion", "2025-03-26"}, {"capabilities", nlohmann::json::object()}, {"clientInfo", {{"name", "private-opencode-cpp-client"}, {"version", "1"}}}}).contains("capabilities"));
	const std::string initialized = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n";
	UAM_ASSERT(process.WriteToStdioProcess(mcp, initialized.data(), initialized.size(), &error));
	const auto tools = call(2, "tools/list", nlohmann::json::object());
	UAM_ASSERT(std::ranges::any_of(tools.at("tools"), [](const auto& item)
	{
		return item.value("name", "") == "agent_delegate";
	}));
	const auto payload = [](const nlohmann::json& value)
	{
		UAM_ASSERT(!value.value("isError", true));
		UAM_ASSERT_EQ(value.at("content").size(), std::size_t{1});
		return nlohmann::json::parse(value.at("content")[0].at("text").get<std::string>());
	};
	const auto delegated = payload(call(3, "tools/call", {{"name", "agent_delegate"}, {"arguments", {{"agentId", fixture.definition.id}, {"task", fixture.task}, {"providerId", "opencode-cli"}, {"modelId", fixture.requested}}}}));
	const std::string run_id = delegated.at("runId").get<std::string>();
	UAM_ASSERT_EQ(delegated.at("status"), nlohmann::json("queued"));
	const auto queued = fixture.ReadRun();
	UAM_ASSERT_EQ(queued.id, run_id);
	UAM_ASSERT_EQ(queued.root_chat_id, fixture.root_id);
	UAM_ASSERT_EQ(queued.provider_id, std::string("opencode-cli"));
	UAM_ASSERT_EQ(queued.model_id, fixture.requested);
	UAM_ASSERT_EQ(queued.execution_capability, std::string("opencode-native-agent-config"));
	UAM_ASSERT_EQ(queued.effective_workspace_access, std::string("read"));
	UAM_ASSERT(!queued.managed_continuation && !queued.deliver_result_to_root_chat);
	fixture.Await([&]
	{
		(void)uam::UamControlService::ProcessPendingRequests(app);
		return fs::exists(fixture.temp.root / "permission-denial.json");
	});
	UAM_ASSERT(fs::exists(fixture.temp.root / "initialize-ack.json"));
	UAM_ASSERT(fs::exists(fixture.temp.root / "native-ack.json"));
	UAM_ASSERT(fs::exists(fixture.temp.root / "model-ack.json"));
	const auto witness = nlohmann::json::parse(ReadFile(fixture.temp.root / "input-witness.json"));
	const auto launch = nlohmann::json::parse(ReadFile(fixture.temp.root / "launch-witness.json"));
	const pid_t child_pid = witness.at("pid").get<pid_t>();
	UAM_ASSERT_EQ(launch.at("pid").get<pid_t>(), child_pid);
	const auto* live = uam::FindAcpSessionForChat(app, queued.transcript_chat_id);
	UAM_ASSERT(live && live->running && live->initialized && live->session_ready && live->processing);
	UAM_ASSERT_EQ(live->child_pid, child_pid);
	UAM_ASSERT_EQ(live->protocol_kind, std::string("opencode-acp"));
	UAM_ASSERT_EQ(live->process_execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(live->session_id, std::string("ses_private_managed_model_casing"));
	UAM_ASSERT_EQ(live->current_model_id, fixture.canonical);
	UAM_ASSERT_EQ(live->active_uam_agent_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(live->active_uam_agent_execution_capability, std::string("opencode-native-agent-config"));
	UAM_ASSERT(!live->waiting_for_permission && !live->waiting_for_user_input && !live->cancel_requested);
	UAM_ASSERT(std::ranges::any_of(live->diagnostics, [](const auto& item)
	{
		return item.reason == "rejected_by_access_ceiling";
	}));
	UAM_ASSERT(!fs::exists(fixture.temp.root / "workspace" / "must-remain-absent"));
	UAM_ASSERT_EQ(fixture.PromptCount(), std::size_t{1});
	const auto running = payload(call(4, "tools/call", {{"name", "agent_status"}, {"arguments", {{"runId", run_id}}}}));
	UAM_ASSERT_EQ(running.at("runId"), nlohmann::json(run_id));
	UAM_ASSERT_EQ(running.at("status"), nlohmann::json("running"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "allow-completion", "release actual private OpenCode result\n"));
	fixture.Await([&]
	{
		(void)uam::UamControlService::ProcessPendingRequests(app);
		return fixture.ReadRun().status == "completed";
	});
	const auto terminal = payload(call(5, "tools/call", {{"name", "agent_status"}, {"arguments", {{"runId", run_id}}}}));
	UAM_ASSERT_EQ(terminal.at("runId"), nlohmann::json(run_id));
	UAM_ASSERT_EQ(terminal.at("status"), nlohmann::json("completed"));
	UAM_ASSERT_EQ(terminal.at("result"), nlohmann::json(fixture.answer));
	UAM_ASSERT_EQ(terminal.at("providerId"), nlohmann::json("opencode-cli"));
	UAM_ASSERT_EQ(terminal.at("modelId"), nlohmann::json(fixture.canonical));
	UAM_ASSERT_EQ(terminal.at("executionCapability"), nlohmann::json("opencode-native-agent-config"));
	const auto completed = fixture.ReadRun();
	UAM_ASSERT_EQ(completed.id, run_id);
	UAM_ASSERT_EQ(completed.root_chat_id, fixture.root_id);
	UAM_ASSERT_EQ(completed.result_excerpt, fixture.answer);
	UAM_ASSERT(completed.diagnostic.empty() && completed.diagnostic_code.empty());
	UAM_ASSERT(completed.started_at_epoch_ms > 0 && completed.finished_at_epoch_ms >= completed.started_at_epoch_ms);
	const auto child = ChatRepository::LoadLocalChatBounded(app.data_root, completed.transcript_chat_id, 4 * 1024 * 1024);
	UAM_ASSERT(child && child->native_session_id == "ses_private_managed_model_casing" && child->execution_host_id == "local");
	UAM_ASSERT_EQ(child->model_id, fixture.canonical);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.task, &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(child->messages, fixture.answer, &Message::content), 1);
	const auto saved_parent = ChatRepository::LoadLocalChatBounded(app.data_root, fixture.root_id, 4 * 1024 * 1024);
	UAM_ASSERT(saved_parent && saved_parent->messages == parent_messages && saved_parent->model_id == fixture.requested);
	UAM_ASSERT_EQ(saved_parent->command_safety_tier, std::string("yolo"));
	UAM_ASSERT_EQ(fixture.PromptCount(), std::size_t{1});
	const std::string journal = ReadFile(fixture.temp.root / "requests.jsonl");
	UAM_ASSERT(!journal.empty() && journal.back() == '\n');
	std::istringstream frames(journal);
	std::size_t initializes = 0, creations = 0, models = 0, prompts = 0, denials = 0;
	for (std::string frame; std::getline(frames, frame);)
	{
		const auto item = nlohmann::json::parse(frame);
		const std::string method = item.value("method", "");
		if (method == "initialize") ++initializes;
		if (method == "session/new") ++creations;
		if (method == "session/set_model") ++models;
		if (method == "session/prompt") ++prompts;
		if (item.contains("id") && item["id"] == "private-opencode-denied-edit" && !item.contains("method")) ++denials;
	}
	UAM_ASSERT_EQ(initializes, std::size_t{1});
	UAM_ASSERT_EQ(creations, std::size_t{1});
	UAM_ASSERT_EQ(models, std::size_t{1});
	UAM_ASSERT_EQ(prompts, std::size_t{1});
	UAM_ASSERT_EQ(denials, std::size_t{1});
	// Result retrieval is through public MCP. Before general cleanup, the ordinary
	// scheduler must already have removed and physically reaped the native child.
	UAM_ASSERT(uam::FindAcpSessionForChat(app, completed.transcript_chat_id) == nullptr);
	int child_status = 0;
	errno = 0;
	UAM_ASSERT(waitpid(child_pid, &child_status, WNOHANG) == -1 && errno == ECHILD);
	errno = 0;
	UAM_ASSERT(kill(child_pid, 0) == -1 && errno == ESRCH);
	UAM_ASSERT_EQ(std::getenv("HOME") ? std::optional<std::string>(std::getenv("HOME")) : std::nullopt, original_home);
	UAM_ASSERT(cleanup.Retire());
}
#endif


#if UAM_ENABLE_RUNTIME_CODEX_CLI
namespace
{
	class ParentInputRefreshCallbackR1 final : public UamQueryHandler::Callback
	{
	  public:
		int successes = 0, failures = 0, status = 0;
		void Success(const CefString&) override { ++successes; }
		void Success(const void*, size_t) override { ++successes; }
		void Failure(int code, const CefString&) override { ++failures; status = code; }
	  private:
		IMPLEMENT_REFCOUNTING(ParentInputRefreshCallbackR1);
	};
	struct ParentInputRefreshFixtureR1
	{
		TempDir temp{"uam-cef-parent-input-history"};
		ScopedEnvVar home{"CODEX_HOME", (temp.root / "codex").string()};
		uam::AppState app;
		std::unique_ptr<UamQueryHandler> handler;
		struct Query
		{
			CefRefPtr<UamQueryHandler::Callback> cb;
			std::function<AsyncCefResult()> worker;
			std::function<void(AsyncCefResult&)> completion;
		};
		std::vector<Query> pending;
		std::string payload = "PRIVATE-MANAGED-INPUT-DATA";
		ParentInputRefreshFixtureR1()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatSession chat;
			chat.id = "cef-owned-parent"; chat.provider_id = "codex-cli"; chat.execution_host_id = "local";
			chat.native_session_id = "11111111-2222-4333-8444-555555555555";
			chat.workspace_directory = (temp.root / "workspace").string(); fs::create_directories(chat.workspace_directory);
			chat.created_at = chat.updated_at = "2026-01-01T00:00:00Z";
			Message human{MessageRole::User, "PRIVATE-ORIGINAL-HUMAN"}; human.created_at = chat.created_at;
			chat.messages.push_back(human);
			ManagedParentInputReceipt receipt;
			receipt.id = "11111111-2222-4333-8444-666666666666"; receipt.origin_id = "original-human-origin";
			receipt.root_id = chat.id; receipt.provider_id = chat.provider_id; receipt.host_id = chat.execution_host_id;
			receipt.workspace = chat.workspace_directory; receipt.native_thread = chat.native_session_id;
			receipt.origin_turn = "original-human"; receipt.accepted_turn_id = "accepted-machine-input";
			receipt.native_request_id = "71"; receipt.payload = payload;
			receipt.run_ids = {"11111111-2222-4333-8444-777777777777"};
			chat.managed_parent_input_receipts = {receipt}; chat.managed_parent_input_receipts_present = true;
			app.chats.push_back(chat); UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			std::string rollout;
			const auto line = [&](const nlohmann::json& value) { rollout += value.dump() + "\n"; };
			line({{"type", "session_meta"}, {"timestamp", chat.created_at}, {"payload", {{"id", chat.native_session_id}, {"cwd", chat.workspace_directory}}}});
			unsigned tick = 0;
			const auto turn = [&](const std::string& id, const std::string& text, const std::string& answer)
			{
				line({{"type", "event_msg"}, {"payload", {{"type", "task_started"}, {"turn_id", id}}}});
				line({{"type", "turn_context"}, {"payload", {{"turn_id", id}}}});
				for (const auto& [role, content] : std::vector<std::pair<std::string, std::string>>{{"user", text}, {"assistant", answer}})
					line({{"type", "response_item"}, {"timestamp", "2026-01-01T00:01:" + std::to_string(10 + tick++) + "Z"},
					    {"payload", {{"type", "message"}, {"role", role}, {"content", nlohmann::json::array({{{"type", "input_text"}, {"text", content}}})}}}});
				line({{"type", "event_msg"}, {"payload", {{"type", "task_complete"}, {"turn_id", id}}}});
			};
			turn("original-human", human.content, "PRIVATE-ORIGINAL-ANSWER");
			turn(receipt.accepted_turn_id, payload, "PRIVATE-MACHINE-ANSWER");
			turn("human-copy-on-another-turn", payload, "PRIVATE-HUMAN-COPY-ANSWER");
			fs::create_directories(temp.root / "codex" / "sessions");
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "codex" / "sessions" / ("rollout-" + chat.native_session_id + ".jsonl"), rollout));
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
			NativeMediaRefreshHandlerTestAccess::Dispatch(*handler, [this](auto cb, auto worker, auto completion)
			{
				pending.push_back({cb, std::move(worker), std::move(completion)}); return true;
			});
		}
		CefRefPtr<ParentInputRefreshCallbackR1> Start()
		{
			CefRefPtr<ParentInputRefreshCallbackR1> cb = new ParentInputRefreshCallbackR1;
			NativeMediaRefreshHandlerTestAccess::Invoke(*handler, app.chats.front().id, cb);
			UAM_ASSERT_EQ(pending.size(), std::size_t{1}); return cb;
		}
		std::string Primary() const { return ReadFile(AppPaths::UamChatFilePath(app.data_root, app.chats.front().id)); }
	};
}

UAM_TEST(CodexCefNativeRefreshRetainsOwnedInputDataAndHumanCopies)
{
	ParentInputRefreshFixtureR1 fixture;
	const auto receipts = fixture.app.chats.front().managed_parent_input_receipts;
	const auto cb = fixture.Start(); auto query = std::move(fixture.pending.front());
	auto result = std::async(std::launch::async, query.worker).get(); UAM_ASSERT(result.ok);
	CompleteAsyncCefQuery(query.cb, result, query.completion);
	UAM_ASSERT_EQ(cb->successes, 1); UAM_ASSERT_EQ(cb->failures, 0);
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
	UAM_ASSERT(saved); UAM_ASSERT_EQ(saved->managed_parent_input_receipts, receipts);
	std::vector<std::string> users;
	for (const auto& message : saved->messages) if (message.role == MessageRole::User) users.push_back(message.content);
	UAM_ASSERT_EQ(users, (std::vector<std::string>{"PRIVATE-ORIGINAL-HUMAN", fixture.payload}));
	UAM_ASSERT_EQ(std::ranges::count(saved->messages, std::string("PRIVATE-MACHINE-ANSWER"), &Message::content), 1);
	UAM_ASSERT_EQ(std::ranges::count(saved->messages, std::string("PRIVATE-HUMAN-COPY-ANSWER"), &Message::content), 1);
	UAM_ASSERT(fixture.app.acp_sessions.empty() && saved->acp_queued_prompts.empty());
}

UAM_TEST(CodexCefNativeRefreshCancelledQuerySavesWithoutDeliveryAndNextRequestReusesIt)
{
	ParentInputRefreshFixtureR1 fixture;
	(void)uam::PushStateUpdateIfChanged(nullptr, fixture.app);
	NativeMediaRefreshHandlerTestAccess::Query(*fixture.handler, 61);
	const auto cancelled = fixture.Start();
	NativeMediaRefreshHandlerTestAccess::Query(*fixture.handler, -1);
	NativeMediaRefreshHandlerTestAccess::Cancel(*fixture.handler, 61);
	auto query = std::move(fixture.pending.front());
	auto result = std::async(std::launch::async, query.worker).get(); UAM_ASSERT(result.ok);
	CompleteAsyncCefQuery(query.cb, result, query.completion);
	UAM_ASSERT_EQ(cancelled->successes, 0); UAM_ASSERT_EQ(cancelled->failures, 1); UAM_ASSERT_EQ(cancelled->status, 409);
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, fixture.app)); // The cancelled request published nothing.
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
	UAM_ASSERT(saved); UAM_ASSERT_EQ(std::ranges::count(saved->messages, std::string("PRIVATE-MACHINE-ANSWER"), &Message::content), 1);
	CefRefPtr<ParentInputRefreshCallbackR1> next = new ParentInputRefreshCallbackR1;
	NativeMediaRefreshHandlerTestAccess::Invoke(*fixture.handler, fixture.app.chats.front().id, next);
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1}); // No second native read.
	UAM_ASSERT_EQ(next->successes, 1); UAM_ASSERT_EQ(next->failures, 0);
}

UAM_TEST(CodexCefNativeRefreshRejectsChangedPrimaryOrInputReceipt)
{
	for (int change = 0; change < 6; ++change)
	{
		ParentInputRefreshFixtureR1 fixture;
		if (change == 4)
			fixture.app.chats.front().managed_parent_input_receipts.front().accepted_turn_id = "unpublished-live-ack";
		const auto cb = fixture.Start(); auto query = std::move(fixture.pending.front());
		if (change == 5)
		{
			auto canonical = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id); UAM_ASSERT(canonical);
			canonical->title = "Changed while the worker was queued";
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *canonical));
		}
		auto result = std::async(std::launch::async, query.worker).get();
		UAM_ASSERT_EQ(result.ok, change < 4);
		if (change == 0) fixture.app.chats.front().managed_parent_input_receipts.front().payload += "-changed-live";
		else if (change == 1) fixture.app.chats.front().managed_parent_input_receipts_present = false;
		else if (change == 2 || change == 3)
		{
			auto canonical = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id); UAM_ASSERT(canonical);
			if (change == 2) canonical->title = "A new same-owner primary revision";
			else canonical->managed_parent_input_receipts.front().accepted_turn_id = "changed-canonical-turn";
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *canonical));
		}
		const auto primary = fixture.Primary();
		const auto live_receipts = fixture.app.chats.front().managed_parent_input_receipts;
		const bool live_present = fixture.app.chats.front().managed_parent_input_receipts_present;
		CompleteAsyncCefQuery(query.cb, result, query.completion);
		UAM_ASSERT_EQ(cb->successes, 0); UAM_ASSERT_EQ(cb->failures, 1); UAM_ASSERT_EQ(cb->status, 409);
		UAM_ASSERT_EQ(fixture.Primary(), primary);
		UAM_ASSERT_EQ(fixture.app.chats.front().managed_parent_input_receipts, live_receipts);
		UAM_ASSERT_EQ(fixture.app.chats.front().managed_parent_input_receipts_present, live_present);
		UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{1});
		UAM_ASSERT(fixture.app.acp_sessions.empty());
	}
}
#endif


#include "app/agent_definition_service.h"
#include "common/chat/acp_prompt_recovery.h"
#include <array>
#include <fstream>

#if UAM_ENABLE_RUNTIME_CODEX_CLI
namespace
{
	class ColdQueuedRemoveCallbackR1 final : public UamQueryHandler::Callback
	{
	public:
		int successes = 0, failures = 0, status = 0;
		void Success(const CefString&) override { ++successes; }
		void Success(const void*, size_t) override { ++successes; }
		void Failure(int code, const CefString&) override { ++failures; status = code; }
	private:
		IMPLEMENT_REFCOUNTING(ColdQueuedRemoveCallbackR1);
	};
	struct ColdQueuedRemoveFixtureR1
	{
		TempDir temp{"uam-cef-cold-direct-remove"};
		uam::AppState app;
		std::unique_ptr<UamQueryHandler> handler;
		struct Query
		{
			CefRefPtr<UamQueryHandler::Callback> cb;
			std::function<AsyncCefResult()> worker;
			std::function<void(AsyncCefResult&)> completion;
			AsyncCefResult Run() { return std::async(std::launch::async, worker).get(); }
			void Finish(AsyncCefResult result) { CompleteAsyncCefQuery(cb, result, completion); }
		};
		std::vector<Query> pending;
		ColdQueuedRemoveFixtureR1()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
			UAM_ASSERT(uam::provider_availability::Set(app.settings, "codex-cli", "local", false));
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			ChatSession chat;
			chat.id = "cold-direct-remove-owner";
			chat.provider_id = "codex-cli";
			chat.execution_host_id = "local";
			chat.created_at = chat.updated_at = "2026-10-22T12:00:00Z";
			chat.workspace_directory = (temp.root / "workspace").string();
			chat.workspace_source_directory = chat.workspace_directory;
			chat.workspace_isolation_kind = uam::paths::kPendingGitWorktreeIsolationKind;
			chat.memory_enabled = false;
			fs::create_directories(chat.workspace_directory);
			Message row{MessageRole::User, "Synthetic retained input: remove only this exact receipt."};
			row.created_at = chat.created_at;
			row.provider = chat.provider_id;
			row.interrupted = row.acp_prompt_not_sent = true;
			chat.messages.push_back(row);
			uam::SelectedPrimaryAgentSnapshot policy;
			std::string error;
			UAM_ASSERT(uam::AgentDefinitionService::CaptureSelectedPrimary(app.data_root,
			    temp.root, "plan", policy, error));
			uam::AcpQueuedUserPromptState prompt;
			prompt.id = "exact-cold-direct-receipt";
			prompt.revision = 1;
			prompt.text = row.content;
			prompt.direct_admission = true;
			prompt.append_user_message = false;
			prompt.existing_user_message_index = 0;
			prompt.existing_user_message_created_at = row.created_at;
			prompt.uam_agent_id = policy.definition.id;
			prompt.uam_agent_definition_hash = policy.definition.definition_hash;
			prompt.uam_agent_definition_snapshot = policy.definition.markdown_snapshot;
			prompt.uam_agent_instructions = policy.definition.instructions;
			prompt.uam_agent_skills = policy.definition.skills;
			prompt.uam_agent_delegates = policy.definition.delegates;
			prompt.uam_agent_workspace_access = policy.definition.workspace_access;
			prompt.uam_agent_execution_capability = uam::AgentDefinitionService::ExecutionCapabilityForProvider(chat.provider_id);
			UAM_ASSERT(uam::AgentDefinitionService::ValidateCapturedRuntimePolicy(prompt.uam_agent_id,
			    prompt.uam_agent_definition_snapshot, chat.provider_id, prompt.uam_agent_execution_capability, &error));
			chat.acp_queued_prompts = {prompt};
			chat.acp_queue_held = true;
			chat.acp_dispatched_queued_prompt_count = 0;
			chat.acp_unsent_prompt_recoveries = {uam::prompt_recovery::Capture(chat, 0, prompt)};
			UAM_ASSERT(uam::prompt_recovery::BindingsMatch(chat));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			app.chats = ChatRepository::LoadLocalChatSummaries(app.data_root);
			UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
			UAM_ASSERT(!app.chats.front().messages_loaded && app.chats.front().messages.empty());
			UAM_ASSERT_EQ(app.chats.front().acp_queued_prompts.size(), std::size_t{1});
			UAM_ASSERT_EQ(app.chats.front().acp_unsent_prompt_recoveries.size(), std::size_t{1});
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
			NativeMediaRefreshHandlerTestAccess::QueuedPromptHistoryDispatch(*handler,
			    [this](auto cb, auto worker, auto completion)
			    {
				pending.push_back({cb, std::move(worker), std::move(completion)});
				return true;
			    });
		}
		fs::path Source() const { return AppPaths::UamChatFilePath(app.data_root, app.chats.front().id); }
		std::string Primary() const { return ReadFile(Source()); }
		CefRefPtr<ColdQueuedRemoveCallbackR1> Start(const std::string& operation = "remove")
		{
			CefRefPtr<ColdQueuedRemoveCallbackR1> cb = new ColdQueuedRemoveCallbackR1;
			NativeMediaRefreshHandlerTestAccess::QueuedPromptAction(*handler,
			    {{"chatId", app.chats.front().id}, {"operation", operation}, {"index", 0},
			     {"expected", {{"id", "exact-cold-direct-receipt"}, {"revision", 1}}}}, cb);
			return cb;
		}
		void NoExecution() const
		{
			UAM_ASSERT(app.cli_terminals.empty() && app.local_acp_launch_tasks.empty());
			for (const auto& session : app.acp_sessions)
				UAM_ASSERT(!session->running && !session->processing && session->last_process_id.empty());
		}
		void Flush()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			do
			{
				(void)uam::FlushPendingChatSaves(app);
				UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
				if (!app.pending_chat_save_tasks.empty()) std::this_thread::yield();
			} while (!app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty());
		}
	};
}

UAM_TEST(ColdDirectReceiptRemoveHandlerDefersReadThenCommitsExactRemoval)
{
	ColdQueuedRemoveFixtureR1 fixture;
	const std::string before = fixture.Primary();
	const auto cb = fixture.Start();
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	UAM_ASSERT_EQ(cb->successes + cb->failures, 0);
	UAM_ASSERT_EQ(fixture.Primary(), before);
	UAM_ASSERT(!fixture.app.chats.front().messages_loaded);
	fixture.NoExecution();
	auto query = std::move(fixture.pending.front());
	auto result = query.Run();
	UAM_ASSERT(result.ok);
	UAM_ASSERT_EQ(fixture.Primary(), before);
	UAM_ASSERT_EQ(cb->successes + cb->failures, 0);
	query.Finish(std::move(result));
	UAM_ASSERT(fixture.app.branch_preparations.empty());
	fixture.Flush();
	UAM_ASSERT_EQ(cb->successes, 1);
	UAM_ASSERT_EQ(cb->failures, 0);
	const auto& current = fixture.app.chats.front();
	UAM_ASSERT(current.messages_loaded && current.messages.empty());
	UAM_ASSERT(current.acp_queued_prompts.empty() && current.acp_unsent_prompt_recoveries.empty());
	UAM_ASSERT(!current.acp_queue_held);
	UAM_ASSERT_EQ(current.workspace_isolation_kind, std::string(uam::paths::kPendingGitWorktreeIsolationKind));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, current.id);
	UAM_ASSERT(saved && saved->messages.empty() && saved->acp_queued_prompts.empty() && saved->acp_unsent_prompt_recoveries.empty());
	fixture.NoExecution();
}

UAM_TEST(ColdDirectReceiptRemoveHandlerRejectsChangedHeldRevision)
{
	ColdQueuedRemoveFixtureR1 fixture;
	const std::string before = fixture.Primary();
	const auto cb = fixture.Start();
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	auto query = std::move(fixture.pending.front());
	auto result = query.Run();
	UAM_ASSERT(result.ok);
	auto& current = fixture.app.chats.front();
	++current.acp_queued_prompts.front().revision;
	const auto changed = current.acp_queued_prompts;
	query.Finish(std::move(result));
	UAM_ASSERT(fixture.app.branch_preparations.empty());
	UAM_ASSERT_EQ(cb->successes, 0);
	UAM_ASSERT_EQ(cb->failures, 1);
	UAM_ASSERT_EQ(cb->status, 409);
	UAM_ASSERT_EQ(current.acp_queued_prompts, changed);
	UAM_ASSERT(current.acp_queue_held && !current.messages_loaded);
	UAM_ASSERT_EQ(current.acp_unsent_prompt_recoveries.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Primary(), before);
	fixture.NoExecution();
}

UAM_TEST(ColdDirectReceiptRemoveHandlerRejectsChangedPrimaryDuringRead)
{
	ColdQueuedRemoveFixtureR1 fixture;
	const auto cb = fixture.Start();
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	auto query = std::move(fixture.pending.front());
	auto result = query.Run();
	UAM_ASSERT(result.ok);
	{ std::ofstream file(fixture.Source(), std::ios::binary | std::ios::app); file << "\n "; UAM_ASSERT(file.good()); }
	const std::string changed = fixture.Primary();
	query.Finish(std::move(result));
	UAM_ASSERT(fixture.app.branch_preparations.empty());
	UAM_ASSERT_EQ(cb->successes, 0);
	UAM_ASSERT_EQ(cb->failures, 1);
	UAM_ASSERT_EQ(cb->status, 409);
	UAM_ASSERT_EQ(fixture.Primary(), changed);
	const auto& current = fixture.app.chats.front();
	UAM_ASSERT(current.acp_queue_held && !current.messages_loaded);
	UAM_ASSERT_EQ(current.acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(current.acp_unsent_prompt_recoveries.size(), std::size_t{1});
	fixture.NoExecution();
}

UAM_TEST(ColdDirectReceiptRemoveHandlerUnavailableHistoryRetainsInput)
{
	ColdQueuedRemoveFixtureR1 fixture;
	const std::string before = fixture.Primary();
	const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.app.chats.front().id);
	const auto cb = fixture.Start();
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	// Make this private primary unavailable only while the action-owned worker reads it.
	const auto held = fixture.Source().string() + ".private-read-failure";
	fs::rename(fixture.Source(), held);
	auto query = std::move(fixture.pending.front());
	auto result = query.Run();
	fs::rename(held, fixture.Source());
	UAM_ASSERT(!result.ok);
	query.Finish(std::move(result));
	UAM_ASSERT(fixture.app.branch_preparations.empty());
	UAM_ASSERT_EQ(cb->successes, 0);
	UAM_ASSERT_EQ(cb->failures, 1);
	UAM_ASSERT_EQ(fixture.Primary(), before);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.app.chats.front().id), revision);
	const auto& current = fixture.app.chats.front();
	UAM_ASSERT(current.acp_queue_held && !current.messages_loaded);
	UAM_ASSERT_EQ(current.acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(current.acp_unsent_prompt_recoveries.size(), std::size_t{1});
	fixture.NoExecution();
}
#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI
UAM_TEST(ColdDirectReceiptRemoveHandlerLargeHistoryUsesNormalExplicitActionRead)
{
	ColdQueuedRemoveFixtureR1 fixture;
	{
		std::ofstream file(fixture.Source(), std::ios::binary | std::ios::app);
		std::array<char, 64 * 1024> whitespace{}; whitespace.fill(' ');
		for (int index = 0; index < 129; ++index)
			file.write(whitespace.data(), static_cast<std::streamsize>(whitespace.size()));
		UAM_ASSERT(file.good());
	}
	UAM_ASSERT(fs::file_size(fixture.Source()) > 8U * 1024U * 1024U);
	const auto cb = fixture.Start();
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	auto query = std::move(fixture.pending.front());
	auto result = query.Run();
	// An explicit user action uses the normal existing cold-history read, not a new permanent size gate.
	UAM_ASSERT(result.ok);
	query.Finish(std::move(result));
	UAM_ASSERT(fixture.app.branch_preparations.empty());
	fixture.Flush();
	UAM_ASSERT_EQ(cb->successes, 1); UAM_ASSERT_EQ(cb->failures, 0);
	UAM_ASSERT(fixture.app.chats.front().messages.empty() && fixture.app.chats.front().acp_queued_prompts.empty());
	fixture.NoExecution();
}
#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI
UAM_TEST(ColdDirectReceiptRetryHandlerReadsSavedInputThenSchedulesOnlyThatInput)
{
	ColdQueuedRemoveFixtureR1 fixture;
	UAM_ASSERT(uam::provider_availability::Set(fixture.app.settings, "codex-cli", "local", true));
	fixture.app.provider_profiles.front().interactive_command.clear();
	const auto saved_before = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
	UAM_ASSERT(saved_before && saved_before->messages.size() == 1);
	const auto rows = saved_before->messages;
	const auto receipts = fixture.app.chats.front().acp_queued_prompts;
	const auto captures = fixture.app.chats.front().acp_unsent_prompt_recoveries;
	const auto projection = uam::StateSerializer::Serialize(fixture.app);
	const auto& visible = projection["chats"][0]["acpSession"]["unsentDirectPrompts"];
	UAM_ASSERT_EQ(visible.size(), std::size_t{1});
	UAM_ASSERT_EQ(visible[0]["id"].get<std::string>(), receipts.front().id);
	UAM_ASSERT_EQ(visible[0]["revision"].get<int>(), receipts.front().revision);
	UAM_ASSERT_EQ(visible[0]["existingUserMessageIndex"].get<int>(), 0);
	const auto cb = fixture.Start("retry");
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	UAM_ASSERT_EQ(cb->successes + cb->failures, 0);
	UAM_ASSERT(!fixture.app.chats.front().messages_loaded);
	fixture.NoExecution();
	auto query = std::move(fixture.pending.front());
	auto result = query.Run(); UAM_ASSERT(result.ok);
	query.Finish(std::move(result)); fixture.Flush();
	UAM_ASSERT(fixture.app.branch_preparations.empty());
	UAM_ASSERT_EQ(cb->successes, 1); UAM_ASSERT_EQ(cb->failures, 0);
	const auto& current = fixture.app.chats.front();
	UAM_ASSERT(current.messages_loaded && !current.acp_queue_held && current.acp_dispatched_queued_prompt_count == 0);
	UAM_ASSERT_EQ(current.messages, rows); UAM_ASSERT_EQ(current.acp_queued_prompts, receipts);
	UAM_ASSERT_EQ(current.acp_unsent_prompt_recoveries, captures);
	const auto* session = uam::FindAcpSessionForChat(fixture.app, current.id);
	UAM_ASSERT(session && session->reconnect_pending && !session->queue_held && session->prompt_request_id == 0);
	fixture.NoExecution();
	const auto duplicate = fixture.Start("retry");
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	UAM_ASSERT_EQ(duplicate->successes, 0); UAM_ASSERT_EQ(duplicate->failures, 1);
	UAM_ASSERT_EQ(current.messages, rows); UAM_ASSERT_EQ(current.acp_queued_prompts, receipts);
	const auto saved_after = ChatRepository::LoadLocalChat(fixture.app.data_root, current.id);
	UAM_ASSERT(saved_after && saved_after->messages == rows && saved_after->acp_queued_prompts == receipts && !saved_after->acp_queue_held);
	fixture.NoExecution();
}
#endif
