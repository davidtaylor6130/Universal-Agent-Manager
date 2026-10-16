#include "cef/state_serializer.h"
#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"

#include <functional>

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
	MessageAttachment image = attachment;
	image.id = "image-id";
	image.name = "image.png";
	image.path = "workspace/.UAM/attachments/image.png";
	image.kind = "image";
	fixture.session->queued_user_prompts.front().attachments.push_back(attachment);
	fixture.session->queued_user_prompts.front().attachments.push_back(image);
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
	UAM_ASSERT_EQ(edited.attachments.size(), std::size_t{2});
	UAM_ASSERT_EQ(edited.attachments.front(), attachment);
	UAM_ASSERT_EQ(edited.attachments.back(), image);
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "one", 1, "Stale text", &error));
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "gone", 1, "Missing", &error));
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "two", 1, "  ", &error));
	UAM_ASSERT(uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "one", 2, "", &error));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().attachments.size(), std::size_t{2});
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "one", 3, "Invalid IDs", &error, std::vector<std::string>{"missing"}));
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "one", 3, "Duplicate IDs", &error, std::vector<std::string>{"image-id", "image-id"}));
	UAM_ASSERT(uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "one", 3, "Keep image", &error, std::vector<std::string>{"image-id"}));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().attachments, std::vector<MessageAttachment>{image});
	UAM_ASSERT(uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "one", 4, "Remove all images", &error, std::vector<std::string>{}));
	UAM_ASSERT(fixture.session->queued_user_prompts.front().attachments.empty());
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "one", 5, "", &error, std::vector<std::string>{}));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().text, std::string("Remove all images"));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(fixture.temp.root, "queued-edits");
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().id, std::string("one"));
	UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().revision, 5);
	UAM_ASSERT(loaded->acp_queued_prompts.front().attachments.empty());
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
	MessageAttachment attachment;
	attachment.id = "prepared-image";
	attachment.path = "workspace/image.png";
	attachment.kind = "image";
	fixture.session->queued_user_prompts.front().attachments.push_back(attachment);
	UAM_ASSERT(uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"]["queuedPrompts"][0].value("prepared", false));
	std::string error;
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "one", 1, "Too late", &error));
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, "queued-edits", "one", 1, "Remove image", &error, std::vector<std::string>{}));
	UAM_ASSERT(!uam::ReorderQueuedAcpPrompts(fixture.app, "queued-edits", {{"one", 1}, {"two", 1}}, {"two", "one"}, &error));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().text, std::string("one"));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().attachments.front(), attachment);
}

UAM_TEST(QueuedPromptSaveFailurePreservesPendingRemoteOutboxPrefix)
{
	QueuedPromptFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.execution_host_id = "remote-host";
	fixture.session->queued_prompt = "Dispatched remote prompt";
	MessageAttachment attachment;
	attachment.id = "protected-image";
	attachment.path = "remote/workspace/image.png";
	attachment.kind = "image";
	fixture.session->queued_user_prompts.front().attachments.push_back(attachment);
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
	UAM_ASSERT(!uam::EditQueuedAcpPrompt(fixture.app, chat.id, "one", 1, "Cannot save", &error, std::vector<std::string>{}));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().text, std::string("one"));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().attachments.front(), attachment);
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

UAM_TEST(QueuedPromptDirectAdmissionPersistsAndChangesQueueOwnershipFingerprint)
{
	QueuedPromptFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	const std::string queued_fingerprint = ChatRepository::QueueAdmissionFingerprint(chat);
	const uam::AcpQueuedUserPromptState queued = chat.acp_queued_prompts.front();
	chat.acp_queued_prompts.front().direct_admission = true;
	UAM_ASSERT(!(queued == chat.acp_queued_prompts.front()));
	UAM_ASSERT(queued_fingerprint != ChatRepository::QueueAdmissionFingerprint(chat));
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, chat));
	const nlohmann::json saved = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(fixture.temp.root, chat.id)));
	UAM_ASSERT(saved["acp_queued_prompts"][0]["direct_admission"].get<bool>());
	UAM_ASSERT(!saved["acp_queued_prompts"][1].contains("direct_admission"));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->acp_queued_prompts, chat.acp_queued_prompts);
	UAM_ASSERT(loaded->acp_queued_prompts.front().direct_admission);
	UAM_ASSERT(!loaded->acp_queued_prompts.back().direct_admission);
	UAM_ASSERT_EQ(ChatRepository::QueueAdmissionFingerprint(*loaded), ChatRepository::QueueAdmissionFingerprint(chat));
}

UAM_TEST(QueuedPromptDirectAdmissionRejectsNonBooleanAndKeepsLegacyQueued)
{
	QueuedPromptFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, chat));
	const fs::path path = AppPaths::UamChatFilePath(fixture.temp.root, chat.id);
	const nlohmann::json original = nlohmann::json::parse(ReadFile(path));
	for (const nlohmann::json& value : {nlohmann::json("true"), nlohmann::json(1), nlohmann::json(nullptr)})
	{
		nlohmann::json malformed = original;
		malformed["acp_queued_prompts"][0]["direct_admission"] = value;
		UAM_ASSERT(uam::io::WriteTextFile(path, malformed.dump()));
		const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id);
		UAM_ASSERT(loaded.has_value());
		UAM_ASSERT_EQ(loaded->acp_queued_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().id, std::string("two"));
		UAM_ASSERT(!loaded->acp_queued_prompts.front().direct_admission);
	}
}

UAM_TEST(QueuedPromptRemoveAndSteerRejectStaleIdentityAndPreparedDelivery)
{
	for (const bool steer : {false, true})
	{
		QueuedPromptFixture fixture;
		fixture.session->processing = false;
		fixture.session->reconnect_pending = true;
		std::string error;
		const std::function<bool(const std::optional<std::pair<std::string, int>>&)> act = [&](const std::optional<std::pair<std::string, int>>& expected)
		{
			return steer
			    ? uam::SteerQueuedAcpPrompt(fixture.app, "queued-edits", 0, &error, expected)
			    : uam::RemoveQueuedAcpPrompt(fixture.app, "queued-edits", 0, &error, expected);
		};
		// An observed row index can now refer to another prompt or a newer revision.
		UAM_ASSERT(!act(std::make_pair(std::string("two"), 1)));
		UAM_ASSERT(!act(std::make_pair(std::string("one"), 2)));
		UAM_ASSERT(!act(std::make_pair(std::string(), 1)));
		UAM_ASSERT(!act(std::make_pair(std::string("one"), 0)));
		fixture.session->queued_user_prompts.front().prepared_for_delivery = true;
		UAM_ASSERT(!act(std::make_pair(std::string("one"), 1)));
		UAM_ASSERT(!act(std::nullopt));
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{2});
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().id, std::string("one"));
		UAM_ASSERT_EQ(fixture.app.chats.front().acp_queued_prompts.size(), std::size_t{2});
		UAM_ASSERT(!fixture.session->processing);
		UAM_ASSERT(fixture.session->reconnect_pending);
		fixture.session->queued_user_prompts.front().prepared_for_delivery = false;
		UAM_ASSERT(act(std::make_pair(std::string("one"), 1)));
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), steer ? std::size_t{2} : std::size_t{1});
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().id, steer ? std::string("one") : std::string("two"));
		if (steer) UAM_ASSERT(fixture.session->queued_user_prompts.front().priority_steer);
		// Legacy index-only clients retain access to future, undispatched queue entries.
		UAM_ASSERT(act(std::nullopt));
	}
}

UAM_TEST(ObserverOnlySaveFailuresPreserveForeignRemotePromptPrefix)
{
	for (const std::string kind : {"metadata", "icon", "workspace"})
	{
		std::optional<bool> observer_saved;
		int callbacks = 0;
		QueuedPromptFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		chat.execution_host_id = "remote-host";
		fixture.session->queued_prompt = "Dispatched remote prompt";
		uam::AcpQueuedUserPromptState pending;
		pending.id = "dispatched"; pending.text = fixture.session->queued_prompt; pending.prepared_for_delivery = true;
		chat.acp_queued_prompts.insert(chat.acp_queued_prompts.begin(), pending);
		chat.acp_dispatched_queued_prompt_count = 1;
		const fs::path blocked = fixture.temp.root / "blocked-root";
		UAM_ASSERT(uam::io::WriteTextFile(blocked, "Not a directory")); fixture.app.data_root = blocked;
		std::shared_ptr<uam::WorkspaceChatSaveTicket> ticket;
		if (kind == "workspace")
			ticket = uam::ScheduleWorkspaceChatSave(fixture.app, chat, [&](bool saved) { observer_saved = saved; ++callbacks; });
		else
		{
			uam::acp_detail::ScheduleChatSave(fixture.app, chat, 0.0);
			if (kind == "metadata")
				UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, chat.id, [&](bool saved) { observer_saved = saved; ++callbacks; }));
			if (kind == "icon")
			{
				chat.custom_icon = {"text", "A"};
				UAM_ASSERT(uam::DeferChatIconSaveResult(fixture.app, chat.id, chat.custom_icon, []() { return true; },
				    [&](bool saved) { observer_saved = saved; ++callbacks; }));
			}
		}
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!observer_saved && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(fixture.app); std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT(observer_saved && !*observer_saved); UAM_ASSERT_EQ(callbacks, 1);
		UAM_ASSERT_EQ(fixture.session->queued_prompt, std::string("Dispatched remote prompt")); UAM_ASSERT(fixture.session->processing);
		UAM_ASSERT(!uam::acp_detail::SaveChatQuietly(fixture.app, chat));
		UAM_ASSERT_EQ(fixture.session->queued_prompt, std::string("Dispatched remote prompt")); UAM_ASSERT(fixture.session->processing);
		UAM_ASSERT_EQ(chat.acp_queued_prompts.front().id, std::string("dispatched")); UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, std::size_t{1});
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().id, std::string("one"));
		UAM_ASSERT(observer_saved && !*observer_saved); UAM_ASSERT_EQ(callbacks, 1);
		uam::FlushPendingChatSaves(fixture.app, true);
		UAM_ASSERT_EQ(fixture.session->queued_prompt, std::string("Dispatched remote prompt")); UAM_ASSERT(fixture.session->processing);
		UAM_ASSERT_EQ(chat.acp_queued_prompts.front().id, std::string("dispatched")); UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, std::size_t{1});
		UAM_ASSERT_EQ(callbacks, 1);
	}
}
