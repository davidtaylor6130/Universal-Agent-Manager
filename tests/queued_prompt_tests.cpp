#include "test_harness.h"

using namespace uam_test;

namespace
{
	struct QueuedPromptFixture
	{
		TempDir temp{"uam-queued-edits"};
		uam::AppState app;
		uam::AcpSessionState* session;

		QueuedPromptFixture()
		{
			app.data_root = temp.root;
			ChatSession chat;
			chat.id = "queued-edits";
			chat.provider_id = "gemini-cli";
			app.chats.push_back(chat);
			std::unique_ptr<uam::AcpSessionState> runtime = std::make_unique<uam::AcpSessionState>();
			runtime->chat_id = chat.id;
			runtime->provider_id = chat.provider_id;
			runtime->processing = true;
			for (const std::string& id : {"one", "two"})
			{
				uam::AcpQueuedUserPromptState prompt;
				prompt.id = id;
				prompt.text = id;
				prompt.markdown_store_files = {"skill.md"};
				prompt.markdown_store_prompt_blocks = {"SNAPSHOT"};
				prompt.uam_agent_instructions = "AGENT_INSTRUCTIONS";
				prompt.goal_mode = true;
				prompt.goal_id = "goal";
				prompt.computer_use_mode = true;
				runtime->queued_user_prompts.push_back(prompt);
			}
			session = runtime.get();
			app.acp_sessions.push_back(std::move(runtime));
			app.chats.front().acp_queued_prompts.assign(session->queued_user_prompts.begin(), session->queued_user_prompts.end());
		}
	};
}

UAM_TEST(QueuedPromptEditRetainsSnapshotsAndRejectsStaleRevision)
{
	QueuedPromptFixture fixture;
	MessageAttachment attachment;
	attachment.id = "file-id";
	attachment.name = "evidence.txt";
	attachment.path = "evidence.txt";
	attachment.kind = "file";
	fixture.session->queued_user_prompts.front().attachments.push_back(attachment);
	std::string error;
	UAM_ASSERT(uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "one", 1, "Edited text", &error));
	const uam::AcpQueuedUserPromptState& edited = fixture.session->queued_user_prompts.front();
	UAM_ASSERT_EQ(edited.text, std::string("Edited text"));
	UAM_ASSERT_EQ(edited.revision, 2);
	UAM_ASSERT_EQ(edited.id, std::string("one"));
	UAM_ASSERT_EQ(edited.markdown_store_prompt_blocks.front(), std::string("SNAPSHOT"));
	UAM_ASSERT_EQ(edited.uam_agent_instructions, std::string("AGENT_INSTRUCTIONS"));
	UAM_ASSERT(edited.goal_mode && edited.computer_use_mode);
	UAM_ASSERT_EQ(edited.goal_id, std::string("goal"));
	UAM_ASSERT_EQ(edited.attachments.front(), attachment);
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "one", 1, "Stale text", &error));
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "gone", 1, "Missing", &error));
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "two", 1, "  ", &error));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().text, std::string("Edited text"));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(fixture.temp.root, "queued-edits");
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().id, std::string("one"));
	UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().revision, 2);
}

UAM_TEST(QueuedPromptReorderRejectsChangedQueueAndKeepsEveryEntry)
{
	QueuedPromptFixture fixture;
	std::string error;
	const std::vector<std::pair<std::string, int>> expected = {{"one", 1}, {"two", 1}};
	UAM_ASSERT(!uam::ReorderQueuedAcpPrompts(fixture.app, "queued-edits", expected, {"one", "one"}, &error));
	UAM_ASSERT(!uam::ReorderQueuedAcpPrompts(fixture.app, "queued-edits", {{"one", 2}, {"two", 1}}, {"two", "one"}, &error));
	UAM_ASSERT(uam::ReorderQueuedAcpPrompts(fixture.app, "queued-edits", expected, {"two", "one"}, &error));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().id, std::string("two"));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.back().markdown_store_prompt_blocks.front(), std::string("SNAPSHOT"));
	UAM_ASSERT(!uam::ReorderQueuedAcpPrompts(fixture.app, "queued-edits", expected, {"one", "two"}, &error));
	uam::AcpQueuedUserPromptState added;
	added.id = "three";
	added.text = "Concurrent append";
	fixture.session->queued_user_prompts.push_back(added);
	UAM_ASSERT(!uam::ReorderQueuedAcpPrompts(fixture.app, "queued-edits", {{"two", 1}, {"one", 1}}, {"one", "two"}, &error));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{3});
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.back().text, std::string("Concurrent append"));
}

UAM_TEST(QueuedPromptPreparedEntriesCannotBeEditedOrMoved)
{
	QueuedPromptFixture fixture;
	fixture.session->queued_user_prompts.front().prepared_for_delivery = true;
	std::string error;
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "one", 1, "Too late", &error));
	UAM_ASSERT(!uam::ReorderQueuedAcpPrompts(fixture.app, "queued-edits", {{"one", 1}, {"two", 1}}, {"two", "one"}, &error));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().text, std::string("one"));
}

UAM_TEST(QueuedPromptSaveFailurePreservesPendingRemoteOutboxPrefix)
{
	QueuedPromptFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.execution_host_id = "remote-host";
	fixture.session->queued_prompt = "Dispatched remote prompt";
	uam::AcpQueuedUserPromptState pending;
	pending.id = "dispatched";
	pending.text = fixture.session->queued_prompt;
	pending.prepared_for_delivery = true;
	chat.acp_queued_prompts.insert(chat.acp_queued_prompts.begin(), pending);
	chat.acp_dispatched_queued_prompt_count = 1;
	const fs::path blocked = fixture.temp.root / "blocked-root";
	UAM_ASSERT(uam::io::WriteTextFile(blocked, "Not a directory"));
	fixture.app.data_root = blocked;
	std::string error;
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, chat.id, "one", 1, "Cannot save", &error));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().text, std::string("one"));
	UAM_ASSERT_EQ(chat.acp_queued_prompts.size(), std::size_t{3});
	UAM_ASSERT_EQ(chat.acp_queued_prompts.front().id, std::string("dispatched"));
	UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, std::size_t{1});
	UAM_ASSERT(!uam::ReorderQueuedAcpPrompts(fixture.app, chat.id, {{"one", 1}, {"two", 1}}, {"two", "one"}, &error));
	UAM_ASSERT_EQ(chat.acp_queued_prompts.front().id, std::string("dispatched"));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().id, std::string("one"));
	fixture.app.data_root = fixture.temp.root;
	UAM_ASSERT(uam::EditQueuedAcpPrompt(fixture.app, chat.id, "one", 1, "Saved", &error));
	UAM_ASSERT_EQ(chat.acp_queued_prompts.front().id, std::string("dispatched"));
	UAM_ASSERT_EQ(chat.acp_queued_prompts[1].text, std::string("Saved"));
}

UAM_TEST(QueuedPromptLegacyIdsAreUniqueAndStableAcrossHydration)
{
	QueuedPromptFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.acp_queued_prompts.front().id.clear();
	chat.acp_queued_prompts.back().id.clear();
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, chat));
	const std::optional<ChatSession> first = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id);
	const std::optional<ChatSession> second = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id);
	UAM_ASSERT(first.has_value() && second.has_value());
	UAM_ASSERT(!first->acp_queued_prompts.front().id.empty());
	UAM_ASSERT(first->acp_queued_prompts.front().id != first->acp_queued_prompts.back().id);
	UAM_ASSERT_EQ(first->acp_queued_prompts.front().id, second->acp_queued_prompts.front().id);
	ChatSession duplicate = *first;
	duplicate.acp_queued_prompts.back().id = duplicate.acp_queued_prompts.front().id;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, duplicate));
	const std::optional<ChatSession> repaired = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id);
	UAM_ASSERT(repaired.has_value());
	UAM_ASSERT(repaired->acp_queued_prompts.front().id != repaired->acp_queued_prompts.back().id);
}
