#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

namespace
{
	struct TicketFixture
	{
		TempDir temporary{"uam-workspace-save-ticket"};
		std::optional<bool> result;
		uam::AppState app;
		std::shared_ptr<uam::WorkspaceChatSaveTicket> ticket;
		TicketFixture()
		{
			app.data_root = temporary.root;
			ChatSession chat; chat.id = "workspace-ticket"; chat.provider_id = "codex-cli";
			chat.created_at = chat.updated_at = "2026-10-13T00:00:00Z";
			chat.messages.push_back({.role = MessageRole::User, .content = std::string(8 * 1024 * 1024, 'x')});
			app.chats.push_back(std::move(chat)); UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
		}
		fs::path Primary() const { return AppPaths::UamChatFilePath(app.data_root, "workspace-ticket"); }
		void Begin(std::function<void(bool)> completion = {})
		{
			ticket = uam::ScheduleWorkspaceChatSave(app, app.chats.front(), completion ? std::move(completion) :
			    std::function<void(bool)>([this](bool saved) { result = saved; }));
			UAM_ASSERT(ticket);
		}
		void Capture()
		{
			const fs::path primary = Primary();
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline)
			{
				for (const fs::directory_entry& entry : fs::directory_iterator(primary.parent_path()))
					if (entry.path().filename().string().starts_with(primary.filename().string() + ".tmp.")) return;
				uam::FlushPendingChatSaves(app); std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(false);
		}
		void Drain()
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!app.pending_chat_save_at_by_chat_id.empty() && std::chrono::steady_clock::now() < deadline)
			{
				uam::FlushPendingChatSaves(app); std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
		}
	};
}

UAM_TEST(WorkspaceSaveTicketRevokesItsFailedPublicationBackoff)
{
	TicketFixture fixture;
	std::optional<uam::WorkspaceChatSaveRevocation> revoked;
	fixture.Begin([&](bool saved)
	{
		fixture.result = saved;
		if (!saved) revoked = uam::RevokeWorkspaceChatSave(fixture.app, fixture.ticket);
	});
	fixture.Capture();
	fs::rename(fixture.Primary(), fixture.temporary.root / "preserved.json"); fs::create_directory(fixture.Primary());
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!fixture.result && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(fixture.app); std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(fixture.result && !*fixture.result); UAM_ASSERT(revoked == uam::WorkspaceChatSaveRevocation::OwnedRevoked);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
}

UAM_TEST(WorkspaceSaveTicketRetainsFailedSnapshotAfterTaskMapRemoval)
{
	TicketFixture fixture;
	std::optional<uam::WorkspaceChatSaveRevocation> revoked;
	fixture.Begin([&](bool saved) { fixture.result = saved; if (!saved) revoked = uam::RevokeWorkspaceChatSave(fixture.app, fixture.ticket); });
	fixture.Capture(); fs::rename(fixture.Primary(), fixture.temporary.root / "preserved.json"); fs::create_directory(fixture.Primary());
	uam::FlushPendingChatSaves(fixture.app, true);
	UAM_ASSERT(fixture.result && !*fixture.result); UAM_ASSERT(revoked == uam::WorkspaceChatSaveRevocation::OwnedRevoked);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
}

UAM_TEST(WorkspaceSaveTicketCannotRevokeSameValueNewSchedule)
{
	TicketFixture fixture; fixture.Begin(); fixture.Capture();
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.app.chats.front(), 0.0);
	UAM_ASSERT_EQ(uam::RevokeWorkspaceChatSave(fixture.app, fixture.ticket), uam::WorkspaceChatSaveRevocation::ForeignNewer);
	UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.empty()); fixture.Drain();
}

UAM_TEST(WorkspaceSaveTicketCannotRevokeUnscheduledBodyChange)
{
	TicketFixture fixture; fixture.Begin(); fixture.Capture();
	fixture.app.chats.front().messages.front().content[0] = 'z';
	UAM_ASSERT_EQ(uam::RevokeWorkspaceChatSave(fixture.app, fixture.ticket), uam::WorkspaceChatSaveRevocation::ForeignNewer);
	fixture.Drain(); UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(fixture.app.data_root, "workspace-ticket")->messages.front().content[0], 'z');
}

UAM_TEST(WorkspaceSaveTicketCannotRevokeUnscheduledGoalMetadataChange)
{
	TicketFixture fixture; fixture.Begin(); fixture.Capture();
	Goal goal; goal.id = "new-goal"; goal.objective = "Preserve newer goal"; fixture.app.chats.front().goals.push_back(goal);
	UAM_ASSERT_EQ(uam::RevokeWorkspaceChatSave(fixture.app, fixture.ticket), uam::WorkspaceChatSaveRevocation::ForeignNewer);
	fixture.Drain(); UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(fixture.app.data_root, "workspace-ticket")->goals.front().objective, goal.objective);
}

UAM_TEST(WorkspaceSaveTicketCannotRevokeDifferentRootOrIncarnation)
{
	for (const bool root_switch : {false, true})
	{
		TicketFixture fixture; fixture.Begin(); fixture.Capture();
		if (root_switch) { fixture.app.data_root = fixture.temporary.root / "other"; UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.app.chats.front())); }
		else fixture.app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		UAM_ASSERT_EQ(uam::RevokeWorkspaceChatSave(fixture.app, fixture.ticket), uam::WorkspaceChatSaveRevocation::NotOwned);
		UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.empty()); fixture.Drain();
	}
}

UAM_TEST(WorkspaceSaveTicketPreservesNewIconReceipt)
{
	std::optional<bool> icon_saved; TicketFixture fixture; fixture.Begin(); fixture.Capture();
	fixture.app.chats.front().custom_icon = {"text", "A"};
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.app.chats.front(), 0.0);
	UAM_ASSERT(uam::DeferChatIconSaveResult(fixture.app, "workspace-ticket", {"text", "A"}, []() { return true; }, [&](bool saved) { icon_saved = saved; }));
	UAM_ASSERT_EQ(uam::RevokeWorkspaceChatSave(fixture.app, fixture.ticket), uam::WorkspaceChatSaveRevocation::ForeignNewer);
	fixture.Drain(); UAM_ASSERT(icon_saved && *icon_saved);
}

UAM_TEST(WorkspaceSaveTicketPreservesNewAdmissionObserver)
{
	std::optional<bool> observer_saved; TicketFixture fixture; fixture.Begin(); fixture.Capture();
	UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, "workspace-ticket", [&](bool saved) { observer_saved = saved; }, true));
	UAM_ASSERT_EQ(uam::RevokeWorkspaceChatSave(fixture.app, fixture.ticket), uam::WorkspaceChatSaveRevocation::ForeignNewer);
	fixture.Drain(); UAM_ASSERT(observer_saved && *observer_saved);
}

UAM_TEST(WorkspaceSaveTicketInflightRevocationDoesNotJoinWorker)
{
	TicketFixture fixture; fixture.Begin(); fixture.Capture();
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	UAM_ASSERT_EQ(uam::RevokeWorkspaceChatSave(fixture.app, fixture.ticket), uam::WorkspaceChatSaveRevocation::OwnedRevoked);
	UAM_ASSERT(std::chrono::steady_clock::now() - start <= std::chrono::milliseconds(100));
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty()); UAM_ASSERT(fixture.result && !*fixture.result);
	uam::FlushPendingChatSaves(fixture.app);
}

UAM_TEST(WorkspaceSaveTicketGenerationKeepsNonzeroDebounce)
{
	TicketFixture fixture; std::optional<bool> saved;
	fixture.app.chats.front().title = "Delayed newer title";
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.app.chats.front(), 60.0);
	UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, "workspace-ticket", [&](bool value) { saved = value; }));
	for (int index = 0; index < 10; ++index)
	{
		uam::FlushPendingChatSaves(fixture.app); std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	UAM_ASSERT(!saved.has_value()); UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.empty());
	UAM_ASSERT(ChatRepository::LoadLocalChat(fixture.app.data_root, "workspace-ticket")->title != "Delayed newer title");
	for (const fs::directory_entry& entry : fs::directory_iterator(fixture.Primary().parent_path()))
		UAM_ASSERT(!entry.path().filename().string().starts_with(fixture.Primary().filename().string() + ".tmp."));
	uam::FlushPendingChatSaves(fixture.app, true); UAM_ASSERT(saved && *saved);
}

UAM_TEST(WorkspaceSaveTicketPreservesEarlierForeignDirtySchedule)
{
	TicketFixture fixture;
	fixture.app.chats.front().title = "Earlier foreign edit";
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.app.chats.front(), 0.0);
	fixture.Begin(); fixture.Capture();
	UAM_ASSERT_EQ(uam::RevokeWorkspaceChatSave(fixture.app, fixture.ticket), uam::WorkspaceChatSaveRevocation::ForeignNewer);
	fixture.Drain(); UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(fixture.app.data_root, "workspace-ticket")->title, std::string("Earlier foreign edit"));
}

UAM_TEST(WorkspaceSaveTicketKeepsFirstBodyOwnershipAcrossAutomaticRecapture)
{
	TicketFixture fixture; fixture.Begin(); fixture.Capture();
	fixture.app.chats.front().messages.front().content[0] = 'z';
	bool recaptured = false;
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!recaptured && std::chrono::steady_clock::now() < deadline)
	{
		for (const fs::directory_entry& entry : fs::directory_iterator(fixture.Primary().parent_path()))
			if (entry.path().filename().string().starts_with(fixture.Primary().filename().string() + ".tmp."))
			{
				const nlohmann::json snapshot = nlohmann::json::parse(ReadFile(entry.path()), nullptr, false);
				if (!snapshot.is_discarded() && snapshot.contains("messages") && !snapshot["messages"].empty())
					recaptured = snapshot["messages"][0].value("content", std::string{}).starts_with("z");
			}
		if (!recaptured) { uam::FlushPendingChatSaves(fixture.app); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
	}
	UAM_ASSERT(recaptured);
	UAM_ASSERT_EQ(uam::RevokeWorkspaceChatSave(fixture.app, fixture.ticket), uam::WorkspaceChatSaveRevocation::ForeignNewer);
	fixture.Drain(); UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(fixture.app.data_root, "workspace-ticket")->messages.front().content[0], 'z');
}

UAM_TEST(WorkspaceSaveTicketCannotRevokeDifferentFailedOwnerWithEqualDeadline)
{
	TicketFixture fixture; fixture.Begin(); fixture.Capture();
	const fs::path backup = fixture.temporary.root / "preserved.json";
	fs::rename(fixture.Primary(), backup); fs::create_directory(fixture.Primary());
	uam::FlushPendingChatSaves(fixture.app, true);
	UAM_ASSERT(fixture.result && !*fixture.result);
	const double old_deadline = fixture.app.pending_chat_save_at_by_chat_id.at("workspace-ticket");
	const std::shared_ptr<uam::WorkspaceChatSaveTicket> old_ticket = fixture.ticket;
	fs::remove(fixture.Primary()); fs::copy_file(backup, fixture.Primary());
	fixture.result.reset(); fixture.Begin(); fixture.Capture();
	fs::remove(fixture.Primary()); fs::create_directory(fixture.Primary());
	uam::FlushPendingChatSaves(fixture.app, true);
	UAM_ASSERT(fixture.result && !*fixture.result);
	// Deadlines are scheduling values, never ownership tokens, even when equal.
	fixture.app.pending_chat_save_at_by_chat_id.at("workspace-ticket") = old_deadline;
	UAM_ASSERT_EQ(uam::RevokeWorkspaceChatSave(fixture.app, old_ticket), uam::WorkspaceChatSaveRevocation::ForeignNewer);
	UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.empty());
	fs::remove(fixture.Primary()); fs::copy_file(backup, fixture.Primary());
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.app.chats.front(), 0.0); fixture.Drain();
}

UAM_TEST(WorkspaceSaveTicketGenerationKeepsDelayedQueueAdmissionUnstarted)
{
	std::optional<bool> saved;
	TicketFixture fixture;
	uam::AcpQueuedUserPromptState queued; queued.text = "Delayed private queued input"; queued.id = "private-delayed-input";
	fixture.app.chats.front().acp_queued_prompts.push_back(queued);
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.app.chats.front(), 60.0);
	UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, "workspace-ticket", [&](bool value) { saved = value; }, true));
	for (int index = 0; index < 10; ++index)
	{
		uam::FlushPendingChatSaves(fixture.app); std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	UAM_ASSERT(!saved.has_value()); UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.empty());
	UAM_ASSERT(ChatRepository::LoadLocalChat(fixture.app.data_root, "workspace-ticket")->acp_queued_prompts.empty());
	uam::FlushPendingChatSaves(fixture.app, true);
	UAM_ASSERT(saved && *saved);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(fixture.app.data_root, "workspace-ticket")->acp_queued_prompts.front().text, queued.text);
}

UAM_TEST(WorkspaceSaveTicketNeverAcknowledgesMetadataFromQueueOnlyPublication)
{
	std::optional<bool> queue_saved;
	bool matching_full_snapshot_at_ack = false;
	TicketFixture fixture;
	fixture.app.chats.front().workspace_base_ref = "private-prepared-base";
	fixture.Begin([&](bool saved)
	{
		fixture.result = saved;
		if (!saved) return;
		const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChatSummaryReadOnly(fixture.app.data_root, "workspace-ticket");
		matching_full_snapshot_at_ack = persisted && persisted->workspace_base_ref == "private-prepared-base";
	});
	uam::AcpQueuedUserPromptState queued; queued.id = "private-concurrent-input"; queued.text = "Independent queued input";
	fixture.app.chats.front().acp_queued_prompts.push_back(queued);
	UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, "workspace-ticket", [&](bool saved) { queue_saved = saved; }, true));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!queue_saved && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(fixture.app); std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(queue_saved && *queue_saved);
	UAM_ASSERT(!fixture.result.has_value());
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummaryReadOnly(fixture.app.data_root, "workspace-ticket")->workspace_base_ref, std::string{});
	fixture.Drain();
	UAM_ASSERT(fixture.result && *fixture.result); UAM_ASSERT(matching_full_snapshot_at_ack);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(fixture.app.data_root, "workspace-ticket")->acp_queued_prompts.front().text, queued.text);
}

UAM_TEST(WorkspaceSaveTicketKeepsFullReceiptAfterIndependentQueueFailure)
{
	std::optional<bool> queue_saved;
	int queue_callbacks = 0;
	bool matching_full_snapshot_at_ack = false;
	TicketFixture fixture;
	fixture.app.chats.front().workspace_base_ref = "private-prepared-base";
	fixture.Begin([&](bool saved)
	{
		fixture.result = saved;
		if (!saved) return;
		const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChatSummaryReadOnly(fixture.app.data_root, "workspace-ticket");
		matching_full_snapshot_at_ack = persisted && persisted->workspace_base_ref == "private-prepared-base";
	});
	const fs::path queue_path = fixture.app.data_root / "queue-outboxes" / "workspace-ticket.json";
	fs::create_directories(queue_path);
	uam::AcpQueuedUserPromptState queued; queued.id = "private-failed-queue"; queued.text = "Independent queued input";
	fixture.app.chats.front().acp_queued_prompts.push_back(queued);
	UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, "workspace-ticket", [&](bool saved)
	{
		queue_saved = saved; ++queue_callbacks;
		if (!saved) fs::remove(queue_path);
	}, true));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!queue_saved && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(fixture.app); std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(queue_saved && !*queue_saved); UAM_ASSERT_EQ(queue_callbacks, 1);
	UAM_ASSERT(!fixture.result.has_value());
	fixture.Drain();
	UAM_ASSERT(fixture.result && *fixture.result); UAM_ASSERT(matching_full_snapshot_at_ack); UAM_ASSERT_EQ(queue_callbacks, 1);
}

UAM_TEST(WorkspaceSaveSynchronousIconCallbackRecreatesChatWithoutStealingNewObservers)
{
	std::optional<bool> icon_saved, old_observer_saved, new_observer_saved;
	int workspace_callbacks = 0, old_callbacks = 0, new_callbacks = 0;
	bool deleted = false, recreated = false, new_save_registered = false;
	std::shared_ptr<uam::PendingChatSaveTask> new_task;
	TicketFixture fixture;
	const std::string private_workspace = (fixture.temporary.root / "private-workspace").string();
	fs::create_directory(private_workspace);
	fixture.app.chats.front().workspace_directory = private_workspace;
	UAM_ASSERT(fixture.app.chats.front().native_session_id.empty());
	fixture.app.chats.front().workspace_base_ref = "private-original-base";
	fixture.app.chats.front().custom_icon = {"text", "A"};
	const std::shared_ptr<const std::uint8_t> original_incarnation = fixture.app.chats.front().metadata_incarnation;
	fixture.Begin([&](bool saved) { fixture.result = saved; ++workspace_callbacks; });
	UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, "workspace-ticket", [&](bool saved) { old_observer_saved = saved; ++old_callbacks; }));
	const std::shared_ptr<uam::PendingChatSaveTask> original_task = fixture.app.pending_chat_save_tasks.at("workspace-ticket");
	UAM_ASSERT(uam::DeferChatIconSaveResult(fixture.app, "workspace-ticket", {"text", "A"}, [&]()
	{
		const ChatSession* current = ChatDomainService().FindChatById(fixture.app, "workspace-ticket");
		return current && current->metadata_incarnation == original_incarnation && current->custom_icon == CustomIcon{"text", "A"};
	}, [&](bool saved)
	{
		icon_saved = saved;
		if (!saved) return;
		// Production deletion uses only this unique private workspace and has no native session.
		deleted = RemoveChatById(fixture.app, "workspace-ticket");
		if (!deleted) return;
		uam::FlushPendingChatSaves(fixture.app);
		ChatSession replacement;
		replacement.id = "workspace-ticket"; replacement.provider_id = "codex-cli"; replacement.title = "Replacement baseline";
		replacement.created_at = replacement.updated_at = "2026-10-13T00:00:01Z";
		replacement.workspace_directory = private_workspace; replacement.workspace_base_ref = "private-original-base"; replacement.custom_icon = {"text", "A"};
		replacement.messages.push_back({.role = MessageRole::User, .content = "Replacement body"});
		recreated = replacement.metadata_incarnation != original_incarnation;
		fixture.app.chats.push_back(std::move(replacement));
		if (!ChatRepository::SaveChat(fixture.app.data_root, fixture.app.chats.front())) return;
		fixture.app.chats.front().title = "Replacement pending edit";
		uam::acp_detail::ScheduleChatSave(fixture.app, fixture.app.chats.front(), 0.0);
		new_save_registered = uam::DeferChatSaveResult(fixture.app, "workspace-ticket", [&](bool written) { new_observer_saved = written; ++new_callbacks; });
		new_task = fixture.app.pending_chat_save_tasks.at("workspace-ticket");
		fixture.app.pending_chat_save_tasks.rehash(fixture.app.pending_chat_save_tasks.bucket_count() * 4 + 17);
	}));
	UAM_ASSERT(uam::acp_detail::SaveChatQuietly(fixture.app, fixture.app.chats.front()));
	UAM_ASSERT(icon_saved && *icon_saved); UAM_ASSERT(deleted && recreated && new_save_registered);
	UAM_ASSERT(fixture.result && !*fixture.result); UAM_ASSERT_EQ(workspace_callbacks, 1);
	UAM_ASSERT(old_observer_saved && *old_observer_saved); UAM_ASSERT_EQ(old_callbacks, 1);
	UAM_ASSERT(new_task && new_task != original_task);
	UAM_ASSERT(fixture.app.pending_chat_save_tasks.at("workspace-ticket") == new_task);
	UAM_ASSERT(!new_observer_saved.has_value()); UAM_ASSERT_EQ(new_callbacks, 0);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains("workspace-ticket"));
	fixture.Drain();
	UAM_ASSERT(new_observer_saved && *new_observer_saved); UAM_ASSERT_EQ(new_callbacks, 1);
	UAM_ASSERT_EQ(workspace_callbacks, 1); UAM_ASSERT_EQ(old_callbacks, 1);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(fixture.app.data_root, "workspace-ticket")->title, std::string("Replacement pending edit"));
}
