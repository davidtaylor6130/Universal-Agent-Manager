#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include <chrono>

using namespace uam_test;

namespace
{
	ChatSession AsyncIconChat()
	{
		ChatSession chat;
		chat.id = "async-icon";
		chat.provider_id = "codex-cli";
		chat.created_at = "2026-10-13T00:00:00.000Z";
		chat.updated_at = "2026-10-13T00:00:01.000Z";
		chat.title = "Private icon fixture";
		chat.messages.push_back({.role = MessageRole::User, .content = "Retained message"});
		return chat;
	}

	void AwaitIconSaves(uam::AppState& app, const std::function<bool()>& finished)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while ((!finished() || !app.pending_chat_save_at_by_chat_id.empty()) && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT(finished());
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
	}

	void QueueIcon(uam::AppState& app, const CustomIcon& icon, std::function<bool()> owned, std::function<void(bool)> completion)
	{
		app.chats.front().custom_icon = icon;
		uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
		UAM_ASSERT(uam::DeferChatIconSaveResult(app, app.chats.front().id, icon, std::move(owned), std::move(completion)));
	}
}

UAM_TEST(ChatIconAsyncReceiptsSupersedeABAWithoutAcknowledgingOldIntents)
{
	TempDir temp("uam-icon-aba");
	int current = 0;
	std::vector<std::pair<int, bool>> results;
	uam::AppState app; app.data_root = temp.root; app.chats.push_back(AsyncIconChat());
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	for (int operation = 1; operation <= 3; ++operation)
	{
		current = operation;
		QueueIcon(app, {"text", operation == 2 ? "B" : "A"}, [&current, operation]() { return current == operation; },
		    [&results, operation](bool saved) { results.emplace_back(operation, saved); });
	}
	AwaitIconSaves(app, [&results]() { return results.size() == 3; });
	UAM_ASSERT_EQ(results[0], (std::pair<int, bool>{1, false}));
	UAM_ASSERT_EQ(results[1], (std::pair<int, bool>{2, false}));
	UAM_ASSERT_EQ(results[2], (std::pair<int, bool>{3, true}));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(app.data_root, "async-icon")->custom_icon.value, std::string("A"));
}

UAM_TEST(ChatIconAsyncReceiptsRejectRootSwitchAndRecreatedChat)
{
	for (const bool root_switch : {false, true})
	{
		TempDir temp("uam-icon-owner");
		std::vector<bool> results;
		uam::AppState app; app.data_root = temp.root / "first"; app.chats.push_back(AsyncIconChat());
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
		QueueIcon(app, {"text", "A"}, []() { return true; }, [&results](bool saved) { results.push_back(saved); });
		if (root_switch) app.data_root = temp.root / "second";
		else
		{
			ChatSession recreated = AsyncIconChat(); recreated.custom_icon = {"text", "A"};
			UAM_ASSERT(recreated.metadata_incarnation != app.chats.front().metadata_incarnation);
			app.chats.front() = std::move(recreated);
		}
		uam::FlushPendingChatSaves(app);
		UAM_ASSERT_EQ(results.size(), std::size_t{1}); UAM_ASSERT(!results.front());
		uam::FlushPendingChatSaves(app, true);
		UAM_ASSERT_EQ(results.size(), std::size_t{1});
	}
}

UAM_TEST(ChatIconAsyncReceiptsCancelDeletionAndWorkerFailureExactlyOnce)
{
	for (const int scenario : {0, 1, 2})
	{
		TempDir temp("uam-icon-failure");
		bool owned = true;
		std::vector<bool> results;
		uam::AppState app; app.data_root = temp.root / "data"; app.chats.push_back(AsyncIconChat());
		QueueIcon(app, {"text", "A"}, [&owned]() { return owned; }, [&results](bool saved) { results.push_back(saved); });
		if (scenario == 0) owned = false;
		else if (scenario == 1) app.chats.clear();
		else UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "Blocks directory creation"));
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (results.empty() && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT_EQ(results.size(), std::size_t{1}); UAM_ASSERT(!results.front());
		uam::ResolvePendingChatSaveResults(app, "async-icon", false);
		UAM_ASSERT_EQ(results.size(), std::size_t{1});
	}
}

UAM_TEST(ChatIconAsyncPublicationDoesNotReuseAnEarlierPromptMetadataReceipt)
{
	TempDir temp("uam-icon-prompt-receipt");
	std::optional<bool> prompt_result, icon_result;
	bool matching_disk_icon = false;
	uam::AppState app; app.data_root = temp.root / "data"; app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat = AsyncIconChat(); chat.custom_icon = {"text", "A"}; chat.workspace_directory = temp.root.string(); chat.memory_enabled = false;
	chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-444444444444";
	chat.messages.push_back({.role = MessageRole::Assistant, .content = std::string(8 * 1024 * 1024, 'x')});
	app.chats.push_back(std::move(chat)); UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "async-icon"; session->provider_id = "codex-cli"; session->session_id = app.chats.front().native_session_id;
	session->protocol_kind = "codex-app-server"; session->codex_thread_id = session->session_id; session->codex_turn_id = "private-active-turn";
	session->current_mode_id = "default";
	session->session_ready = true; session->processing = true; session->prompt_request_id = 17; session->turn_serial = 7;
	session->turn_user_message_index = session->turn_first_user_message_index = 0;
	session->current_assistant_message_index = session->turn_assistant_message_index = 1;
	IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
	std::string error;
#if defined(_WIN32)
	const std::vector<std::string> sink{"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink{"/bin/sh", "-c", "cat >/dev/null"};
#endif
	UAM_ASSERT(process.StartStdioProcess(*session, temp.root, sink, &error)); session->running = true;
	app.acp_sessions.push_back(std::move(session));
	struct StopSink
	{
		IPlatformProcessService& process; uam::AcpSessionState& session;
		~StopSink() { process.StopStdioProcess(session, true); process.CloseStdioProcessHandles(session); }
	} stop{process, *app.acp_sessions.front()};
	UAM_ASSERT(uam::SteerAcpPrompt(app, "async-icon", "Owned prompt while icon changes", {}, {}, false, &error));
	UAM_ASSERT(uam::DeferChatSaveResult(app, "async-icon", [&prompt_result](bool saved) { prompt_result = saved; }));
	const fs::path primary = AppPaths::UamChatFilePath(app.data_root, "async-icon");
	bool saw_old_snapshot = false;
	const std::chrono::steady_clock::time_point capture_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!saw_old_snapshot && std::chrono::steady_clock::now() < capture_deadline)
	{
		for (const fs::directory_entry& entry : fs::directory_iterator(primary.parent_path()))
			if (entry.path().filename().string().starts_with(primary.filename().string() + ".tmp."))
			{
				const nlohmann::json snapshot = nlohmann::json::parse(ReadFile(entry.path()), nullptr, false);
				saw_old_snapshot = !snapshot.is_discarded() && snapshot.value("custom_icon", nlohmann::json()).value("value", "") == "A";
			}
		if (!saw_old_snapshot) { uam::FlushPendingChatSaves(app); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
	}
	UAM_ASSERT(saw_old_snapshot);
	QueueIcon(app, {"text", "B"}, []() { return true; }, [&app, &icon_result, &matching_disk_icon](bool saved)
	{
		icon_result = saved;
		if (!saved) return;
		const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, "async-icon");
		matching_disk_icon = saved && persisted && persisted->custom_icon.value == "B";
	});
	UAM_ASSERT(!icon_result.has_value());
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while ((!icon_result || !prompt_result) && std::chrono::steady_clock::now() < deadline)
	{
		app.chats.front().messages[1].content += " streamed-output";
		uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
		uam::FlushPendingChatSaves(app); std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(icon_result && *icon_result && matching_disk_icon); UAM_ASSERT(prompt_result && *prompt_result);
	UAM_ASSERT_EQ(app.acp_sessions.front()->pending_request_methods.size(), std::size_t{1});
	AwaitIconSaves(app, []() { return true; });
}

UAM_TEST(ChatIconAsyncAdmissionOf260MiBUnloadedHistoryStaysWithin100ms)
{
	TempDir temp("uam-icon-unloaded-budget");
	std::optional<bool> result;
	uam::AppState app; app.data_root = temp.root; ChatSession metadata = AsyncIconChat(); metadata.messages.clear();
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareImportedChatSave(app.data_root, metadata,
	    [](const std::function<bool(const Message&)>& emit, ChatSession&)
	    {
		    Message message{.role = MessageRole::User, .content = std::string(256 * 1024, 'x')};
		    for (int index = 0; index < 1040; ++index) if (!emit(message)) return false;
		    return true;
	    });
	UAM_ASSERT(prepared); UAM_ASSERT(ChatRepository::PublishPreparedChatSave(prepared, true, &metadata));
	const fs::path primary = AppPaths::UamChatFilePath(app.data_root, metadata.id);
	UAM_ASSERT(fs::file_size(primary) >= 260ULL * 1024 * 1024);
	app.chats.push_back(std::move(metadata)); UAM_ASSERT(!app.chats.front().messages_loaded && app.chats.front().messages.empty());
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	QueueIcon(app, {"text", "A"}, []() { return true; }, [&result](bool saved) { result = saved; });
	uam::FlushPendingChatSaves(app);
	UAM_ASSERT(std::chrono::steady_clock::now() - start <= std::chrono::milliseconds(100));
	AwaitIconSaves(app, [&result]() { return result.has_value(); }); UAM_ASSERT(*result);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, "async-icon");
	UAM_ASSERT(saved && saved->custom_icon.value == "A"); UAM_ASSERT_EQ(saved->persisted_message_count, std::size_t{1040});
	UAM_ASSERT(!app.chats.front().messages_loaded && app.chats.front().messages.empty());
}

UAM_TEST(ChatIconRuntimeIncarnationSurvivesCopiesAndHydrationWithoutPersistence)
{
	TempDir temp("uam-icon-incarnation");
	ChatSession chat = AsyncIconChat(); const std::shared_ptr<const std::uint8_t> incarnation = chat.metadata_incarnation;
	std::vector<ChatSession> relocated{chat}; for (int index = 0; index < 100; ++index) relocated.push_back(AsyncIconChat());
	UAM_ASSERT(relocated.front().metadata_incarnation == incarnation);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	ChatSession metadata = *ChatRepository::LoadLocalChat(temp.root, chat.id, false); metadata.metadata_incarnation = incarnation;
	UAM_ASSERT(ChatRepository::HydrateChatMessages(temp.root, metadata)); UAM_ASSERT(metadata.metadata_incarnation == incarnation);
	const nlohmann::json wire = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id)));
	UAM_ASSERT(!wire.contains("metadata_incarnation") && !wire.contains("metadataIncarnation"));
	UAM_ASSERT(ChatRepository::LoadLocalChat(temp.root, chat.id)->metadata_incarnation != incarnation);
}

namespace
{
	void CheckIconSnapshotMutation(const std::string& fixture, const std::function<void(ChatSession&)>& mutate,
	    const std::function<bool(const ChatSession&)>& has_latest)
	{
		TempDir temp(fixture);
		std::optional<bool> result;
		bool latest_at_ack = false;
		uam::AppState app; app.data_root = temp.root; app.chats.push_back(AsyncIconChat());
		app.chats.front().messages.front().content = std::string(8 * 1024 * 1024, 'x');
		Goal goal; goal.id = "private-goal"; goal.objective = "Original objective";
		app.chats.front().goals.push_back(goal); app.chats.front().active_goal_id = goal.id;
		app.chats.front().approval_mode = "default";
		app.chats.front().workspace_directory = (temp.root / "original-workspace").string();
		uam::AcpQueuedUserPromptState queued; queued.text = "Original queued input";
		app.chats.front().acp_queued_prompts.push_back(queued);
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
		QueueIcon(app, {"text", "A"}, []() { return true; }, [&](bool saved)
		{
			result = saved;
			if (!saved) return;
			const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(app.data_root, "async-icon");
			latest_at_ack = persisted && has_latest(*persisted);
		});
		const fs::path primary = AppPaths::UamChatFilePath(app.data_root, "async-icon");
		bool preparing = false;
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!preparing && std::chrono::steady_clock::now() < deadline)
		{
			// The actual writer creates this unique primary temp only after capturing metadata and bodies.
			for (const fs::directory_entry& entry : fs::directory_iterator(primary.parent_path()))
				if (entry.path().filename().string().starts_with(primary.filename().string() + ".tmp.")) preparing = true;
			if (!preparing) { uam::FlushPendingChatSaves(app); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
		}
		UAM_ASSERT(preparing); UAM_ASSERT(!result.has_value());
		mutate(app.chats.front());
		uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
		AwaitIconSaves(app, [&]() { return result.has_value(); });
		UAM_ASSERT(*result); UAM_ASSERT(latest_at_ack);
		UAM_ASSERT(has_latest(*ChatRepository::LoadLocalChat(app.data_root, "async-icon")));
	}
}

UAM_TEST(ChatIconPreparedSnapshotPreservesNewGoalCommands)
{
	CheckIconSnapshotMutation("uam-icon-fresh-goal", [](ChatSession& chat)
	{
		chat.goals.front().objective = "Latest objective"; chat.goals.front().status = GoalStatus::Paused;
		chat.goal_command_revision = "new-private-command";
	}, [](const ChatSession& chat)
	{
		return chat.goals.front().objective == "Latest objective" && chat.goals.front().status == GoalStatus::Paused &&
		    chat.goal_command_revision == "new-private-command";
	});
}

UAM_TEST(ChatIconPreparedSnapshotPreservesNewApprovalMode)
{
	CheckIconSnapshotMutation("uam-icon-fresh-approval", [](ChatSession& chat) { chat.approval_mode = "plan"; },
	    [](const ChatSession& chat) { return chat.approval_mode == "plan"; });
}

UAM_TEST(ChatIconPreparedSnapshotPreservesNewWorkspace)
{
	CheckIconSnapshotMutation("uam-icon-fresh-workspace", [](ChatSession& chat)
	{
		chat.workspace_directory += "-latest"; chat.workspace_base_ref = "private-latest-ref";
	}, [](const ChatSession& chat)
	{
		return chat.workspace_directory.ends_with("-latest") && chat.workspace_base_ref == "private-latest-ref";
	});
}

UAM_TEST(ChatIconPreparedSnapshotPreservesNewQueuedOutbox)
{
	CheckIconSnapshotMutation("uam-icon-fresh-outbox", [](ChatSession& chat)
	{
		chat.acp_queued_prompts.front().text = "Latest queued input"; chat.acp_queue_held = true;
	}, [](const ChatSession& chat)
	{
		return chat.acp_queued_prompts.size() == 1 && chat.acp_queued_prompts.front().text == "Latest queued input" && chat.acp_queue_held;
	});
}

UAM_TEST(ChatIconPreparedSnapshotPreservesNewMessageBodies)
{
	CheckIconSnapshotMutation("uam-icon-fresh-bodies", [](ChatSession& chat) { chat.messages.front().content = "Latest retained body"; },
	    [](const ChatSession& chat) { return chat.messages.front().content == "Latest retained body"; });
}
