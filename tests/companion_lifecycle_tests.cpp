#include "test_harness.h"

using namespace uam_test;

namespace
{
	struct CompanionLifecycleFixture
	{
		TempDir temporary{"uam-companion-lifecycle"};
		uam::AppState app;
		CompanionLifecycleFixture()
		{
			app.data_root = temporary.root;
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			for (const char* id : {"doomed", "desktop", "other"})
			{
				ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli", false);
				chat.id = id;
				chat.branch_root_chat_id = id;
				chat.updated_at = "2020-01-01T00:00:00Z";
				chat.workspace_directory = temporary.root.string();
				chat.messages.push_back({MessageRole::User, std::string("Prompt for ") + id});
				chat.messages.push_back({MessageRole::Assistant, "Original answer"});
				app.chats.push_back(chat);
				UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			}
			ChatDomainService().SelectChatById(app, "desktop");
			app.composer_text = "Unsaved desktop draft";
		}
		void Delete(const std::vector<std::string>& ids, const std::function<void()>& after_stage = {})
		{
			uam::WorkspaceDeletionTask task;
			UAM_ASSERT(uam::PrepareChatDeletion(app, ids, task));
			bool staged = false;
			std::thread worker([&]() { staged = uam::StageWorkspaceDeletion(task); });
			worker.join();
			UAM_ASSERT(staged);
			if (after_stage) after_stage();
			uam::CommitWorkspaceDeletion(app, task);
			UAM_ASSERT(!task.commit_rejected && task.metadata_saved && task.settings_saved);
			UAM_ASSERT(uam::CleanupWorkspaceDeletion(task));
		}
	};
}

UAM_TEST(CompanionDeletionPreservesUnrelatedDesktopDraftAcrossIndexShift)
{
	CompanionLifecycleFixture fixture;
	fixture.Delete({"doomed"});
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), "desktop");
	UAM_ASSERT_EQ(fixture.app.composer_text, "Unsaved desktop draft");
	UAM_ASSERT_EQ(fixture.app.settings.last_selected_chat_id, "desktop");
	UAM_ASSERT(!ChatRepository::LoadLocalChat(fixture.app.data_root, "doomed", true));
	UAM_ASSERT_EQ(ChatDomainService().FindChatById(fixture.app, "desktop")->messages.size(), std::size_t(2));
}

UAM_TEST(CompanionDeletionUsesCurrentSelectionAfterAsyncStaging)
{
	CompanionLifecycleFixture fixture;
	ChatDomainService().SelectChatById(fixture.app, "doomed");
	fixture.Delete({"doomed"}, [&]()
	{
		ChatDomainService().SelectChatById(fixture.app, "other");
		fixture.app.composer_text = "Typed while deletion awaited";
	});
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), "other");
	UAM_ASSERT_EQ(fixture.app.composer_text, "Typed while deletion awaited");
	UAM_ASSERT_EQ(fixture.app.settings.last_selected_chat_id, "other");
}

UAM_TEST(CompanionDeletionOfCurrentDesktopChatKeepsOrdinarySelectionSemantics)
{
	CompanionLifecycleFixture fixture;
	fixture.Delete({"desktop"});
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), "other");
	UAM_ASSERT(fixture.app.composer_text.empty());
	fixture.app.composer_text = "Another draft";
	fixture.Delete({"doomed", "other"});
	UAM_ASSERT(ChatDomainService().SelectedChatId(fixture.app).empty());
	UAM_ASSERT(fixture.app.composer_text.empty());
}

UAM_TEST(CompanionBranchReturnsChildWithoutSelectingOrLosingMedia)
{
	CompanionLifecycleFixture fixture;
	ChatSession* source = ChatDomainService().FindChatById(fixture.app, "doomed");
	source->native_session_id = "source-native-identity";
	MessageAttachment media;
	media.id = "original-image";
	media.name = "image.png";
	media.path = "/original/media/image.png";
	source->messages.front().attachments.push_back(media);
	const auto original_messages = source->messages;
	std::string child_id;
	std::string error;
	UAM_ASSERT(uam::BranchFromMessageAndRetry(fixture.app, "doomed", 0, std::nullopt, &child_id, &error,
	    std::string("mobile-branch"), nullptr, std::nullopt, nullptr, nullptr, false, false));
	UAM_ASSERT(!child_id.empty() && child_id != "desktop" && child_id != "doomed");
	const ChatSession* child = ChatDomainService().FindChatById(fixture.app, child_id);
	UAM_ASSERT(child && child->parent_chat_id == "doomed" && child->native_session_id.empty());
	UAM_ASSERT_EQ(child->messages.size(), std::size_t(1));
	UAM_ASSERT(child->messages.front() == original_messages.front());
	UAM_ASSERT(ChatDomainService().FindChatById(fixture.app, "doomed")->messages == original_messages);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), "desktop");
	UAM_ASSERT_EQ(fixture.app.composer_text, "Unsaved desktop draft");
	UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, child_id, true);
	UAM_ASSERT(saved && saved->messages.front() == original_messages.front());
	std::string replay_id;
	UAM_ASSERT(uam::BranchFromMessageAndRetry(fixture.app, "doomed", 0, std::nullopt, &replay_id, &error,
	    std::string("mobile-branch"), nullptr, std::nullopt, nullptr, nullptr, false, false));
	UAM_ASSERT_EQ(replay_id, child_id);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t(4));
	UAM_ASSERT_EQ(fixture.app.composer_text, "Unsaved desktop draft");
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), "desktop");
}

UAM_TEST(CompanionBranchRegenerationFailurePreservesDesktopSelectionAndDraft)
{
	for (const bool idempotent : {false, true})
	{
		CompanionLifecycleFixture fixture;
		const fs::path invalid_workspace = fixture.temporary.root / "not-directory";
		UAM_ASSERT(uam::io::WriteTextFile(invalid_workspace, "file"));
		ChatDomainService().FindChatById(fixture.app, "doomed")->workspace_directory = invalid_workspace.string();
		std::string branch_id, error, warning;
		UAM_ASSERT(!uam::BranchFromMessageAndRetry(fixture.app, "doomed", 0, std::nullopt, &branch_id, &error,
		    idempotent ? std::optional<std::string>("failure-operation") : std::nullopt, &warning,
		    std::nullopt, nullptr, nullptr, false));
		UAM_ASSERT(!branch_id.empty() && !error.empty());
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), "desktop");
		UAM_ASSERT_EQ(fixture.app.composer_text, "Unsaved desktop draft");
		UAM_ASSERT_EQ(fixture.app.chats.size(), idempotent ? std::size_t(4) : std::size_t(3));
		if (idempotent)
		{
			UAM_ASSERT(!warning.empty());
			UAM_ASSERT(!ChatDomainService().FindChatById(fixture.app, branch_id)->branch_retry_error.empty());
		}
	}
}

UAM_TEST(CompanionBranchDesktopDefaultStillSelectsChild)
{
	CompanionLifecycleFixture fixture;
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(fixture.app, "doomed", 0));
	const ChatSession* selected = ChatDomainService().SelectedChat(fixture.app);
	UAM_ASSERT(selected && selected->parent_chat_id == "doomed");
	UAM_ASSERT(fixture.app.composer_text.empty());
	UAM_ASSERT_EQ(fixture.app.settings.last_selected_chat_id, selected->id);
}

UAM_TEST(CompanionColdBranchCompletionPreservesNewDesktopSelection)
{
	CompanionLifecycleFixture fixture;
	const ChatDomainService domain;
	const auto summary = ChatRepository::LoadLocalChat(fixture.app.data_root, "doomed", false);
	UAM_ASSERT(summary && !summary->messages_loaded);
	*domain.FindChatById(fixture.app, "doomed") = *summary;
	const auto identity = domain.CaptureBranchHistoryIdentity(*summary);
	const auto ownership = domain.BeginBranchPreparation(fixture.app, "doomed", "");
	UAM_ASSERT(ownership);
	std::optional<ChatSession> prepared;
	std::thread worker([&]() { prepared = domain.LoadBranchHistoryDetached(fixture.app.data_root, identity); });
	worker.join();
	UAM_ASSERT(prepared);
	// The phone's cold read must not restore the desktop selection from request time.
	domain.SelectChatById(fixture.app, "other");
	fixture.app.composer_text = "Draft written during phone history load";
	UAM_ASSERT(domain.ConsumeBranchPreparation(fixture.app, "doomed", ownership));
	UAM_ASSERT(domain.MatchesPreparedBranchHistory(*domain.FindChatById(fixture.app, "doomed"), identity, false, &*prepared));
	std::string child_id, error;
	UAM_ASSERT(uam::BranchFromMessageAndRetry(fixture.app, "doomed", 0, std::nullopt, &child_id, &error,
	    std::string("cold-mobile-branch"), nullptr, std::nullopt, &*prepared, nullptr, false, false));
	UAM_ASSERT_EQ(domain.SelectedChatId(fixture.app), "other");
	UAM_ASSERT_EQ(fixture.app.composer_text, "Draft written during phone history load");
	UAM_ASSERT(!domain.FindChatById(fixture.app, "doomed")->messages_loaded);
	UAM_ASSERT(domain.FindChatById(fixture.app, child_id)->messages.front() == prepared->messages.front());
}

UAM_TEST(CompanionBranchSaveFailurePreservesDesktopDraft)
{
	CompanionLifecycleFixture fixture;
	const fs::path settings_path = AppPaths::SettingsFilePath(fixture.app.data_root);
	fs::create_directories(settings_path);
	UAM_ASSERT(uam::io::WriteTextFile(settings_path / "survivor", "block settings replacement"));
	std::string child_id, error;
	UAM_ASSERT(!uam::BranchFromMessageAndRetry(fixture.app, "doomed", 0, std::nullopt, &child_id, &error,
	    std::string("save-failure-mobile-branch"), nullptr, std::nullopt, nullptr, nullptr, false, false));
	UAM_ASSERT(child_id.empty() && !error.empty());
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t(3));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), "desktop");
	UAM_ASSERT_EQ(fixture.app.composer_text, "Unsaved desktop draft");
	UAM_ASSERT_EQ(ReadFile(settings_path / "survivor"), "block settings replacement");
}
