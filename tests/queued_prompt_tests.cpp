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
			chat.created_at = "2026-10-17T00:00:00.000Z";
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
	const nlohmann::json visible = uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"]["queuedPrompts"];
	UAM_ASSERT_EQ(visible.size(), std::size_t{1});
	UAM_ASSERT_EQ(visible[0].at("id"), "two");
	UAM_ASSERT(!visible[0].value("prepared", false));
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
	// This synthetic non-draft ID must carry the native identity retained by legacy loads.
	chat.native_session_id = "queued-fingerprint-native";
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
		// Index zero projects the future row; the hidden prepared row is still protected.
		const nlohmann::json visible = uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"]["queuedPrompts"];
		UAM_ASSERT_EQ(visible.size(), std::size_t{1});
		UAM_ASSERT_EQ(visible[0].at("id"), "two");
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

namespace
{
	std::size_t ConfigureVisibleQueueControlFixture(QueuedPromptFixture& fixture, int hidden_kind)
	{
		ChatSession& chat = fixture.app.chats.front();
		fixture.app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		chat.provider_id = fixture.session->provider_id = "opencode-cli";
		chat.created_at = chat.updated_at = "2026-10-17T00:00:00.000Z";
		fixture.session->processing = false;
		fixture.session->reconnect_pending = true;
		fixture.session->lifecycle_state = "starting";
		std::deque<uam::AcpQueuedUserPromptState> queue;
		if (hidden_kind != 1)
		{
			uam::AcpQueuedUserPromptState direct;
			direct.id = "hidden-direct";
			direct.text = "Current direct admission.";
			direct.direct_admission = true;
			queue.push_back(direct);
		}
		if (hidden_kind != 0)
		{
			uam::AcpQueuedUserPromptState prepared;
			prepared.id = "hidden-prepared";
			prepared.text = "Prepared delivery remains immutable.";
			prepared.prepared_for_delivery = true;
			queue.push_back(prepared);
		}
		const std::size_t hidden_count = queue.size();
		queue.insert(queue.end(), fixture.session->queued_user_prompts.begin(), fixture.session->queued_user_prompts.end());
		fixture.session->queued_user_prompts = std::move(queue);
		chat.acp_queued_prompts.assign(fixture.session->queued_user_prompts.begin(), fixture.session->queued_user_prompts.end());
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
		return hidden_count;
	}

	std::vector<std::string> ProjectedVisibleQueueIds(const QueuedPromptFixture& fixture)
	{
		const nlohmann::json state = uam::StateSerializer::Serialize(fixture.app);
		std::vector<std::string> ids;
		for (const nlohmann::json& prompt : state["chats"][0]["acpSession"]["queuedPrompts"])
			ids.push_back(prompt["id"].get<std::string>());
		return ids;
	}
}

UAM_TEST(QueuedPromptVisibleIndexRemoveSkipsHiddenAdmissionPrefixes)
{
	for (const int hidden_kind : {0, 1, 2})
	{
		QueuedPromptFixture fixture;
		const std::size_t hidden_count = ConfigureVisibleQueueControlFixture(fixture, hidden_kind);
		UAM_ASSERT_EQ(ProjectedVisibleQueueIds(fixture), (std::vector<std::string>{"one", "two"}));
		auto expected = fixture.session->queued_user_prompts;
		expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(hidden_count));
		std::string error;
		UAM_ASSERT(uam::RemoveQueuedAcpPrompt(fixture.app, "queued-edits", 0, &error));
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts, expected);
		UAM_ASSERT_EQ(ProjectedVisibleQueueIds(fixture), (std::vector<std::string>{"two"}));
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, "queued-edits");
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->acp_queued_prompts, (std::vector<uam::AcpQueuedUserPromptState>(expected.begin(), expected.end())));
	}
}

UAM_TEST(QueuedPromptVisibleIndexSteerSkipsHiddenAdmissionPrefixes)
{
	for (const int hidden_kind : {0, 1, 2})
	{
		QueuedPromptFixture fixture;
		const std::size_t hidden_count = ConfigureVisibleQueueControlFixture(fixture, hidden_kind);
		UAM_ASSERT_EQ(ProjectedVisibleQueueIds(fixture), (std::vector<std::string>{"one", "two"}));
		const auto before = fixture.session->queued_user_prompts;
		std::string error;
		UAM_ASSERT(uam::SteerQueuedAcpPrompt(fixture.app, "queued-edits", 1, &error));
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), before.size());
		for (std::size_t index = 0; index < before.size(); ++index)
		{
			const auto item = std::find_if(fixture.session->queued_user_prompts.begin(), fixture.session->queued_user_prompts.end(),
			    [&](const uam::AcpQueuedUserPromptState& prompt) { return prompt.id == before[index].id; });
			UAM_ASSERT(item != fixture.session->queued_user_prompts.end());
			uam::AcpQueuedUserPromptState expected = before[index];
			if (index == hidden_count + 1) expected.priority_steer = true;
			UAM_ASSERT_EQ(*item, expected);
		}
		UAM_ASSERT_EQ(ProjectedVisibleQueueIds(fixture), (std::vector<std::string>{"two", "one"}));
		UAM_ASSERT(!fixture.session->running && !fixture.session->processing && fixture.session->prompt_request_id == 0);
		UAM_ASSERT(fixture.session->pending_request_methods.empty());
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, "queued-edits");
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->acp_queued_prompts,
		    (std::vector<uam::AcpQueuedUserPromptState>(fixture.session->queued_user_prompts.begin(), fixture.session->queued_user_prompts.end())));
	}
}

UAM_TEST(QueuedPromptVisibleIndexIdentityRejectsAnotherVisibleRow)
{
	for (const bool steer : {false, true})
	{
		QueuedPromptFixture fixture;
		(void)ConfigureVisibleQueueControlFixture(fixture, 2);
		UAM_ASSERT_EQ(ProjectedVisibleQueueIds(fixture), (std::vector<std::string>{"one", "two"}));
		const auto before = fixture.session->queued_user_prompts;
		const std::string primary = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, "queued-edits"));
		std::string error;
		for (const std::pair<std::string, int>& expected : {std::pair{std::string("two"), 1}, std::pair{std::string("one"), 2}})
		{
			UAM_ASSERT(!(steer ? uam::SteerQueuedAcpPrompt(fixture.app, "queued-edits", 0, &error, expected) :
			    uam::RemoveQueuedAcpPrompt(fixture.app, "queued-edits", 0, &error, expected)));
			UAM_ASSERT_EQ(fixture.session->queued_user_prompts, before);
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, "queued-edits")), primary);
		}
	}
}

UAM_TEST(QueuedPromptIdentifiedUnsentDirectRemovalKeepsVisibleQueue)
{
	QueuedPromptFixture fixture;
	const std::size_t hidden_count = ConfigureVisibleQueueControlFixture(fixture, 0);
	UAM_ASSERT_EQ(hidden_count, std::size_t{1});
	fixture.session->reconnect_pending = false;
	fixture.session->lifecycle_state = "stopped";
	const auto before = fixture.session->queued_user_prompts;
	const nlohmann::json projection = uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"];
	UAM_ASSERT_EQ(projection["unsentDirectPrompts"].size(), std::size_t{1});
	UAM_ASSERT_EQ(projection["unsentDirectPrompts"][0]["id"].get<std::string>(), before.front().id);
	std::string error;
	UAM_ASSERT(!uam::RemoveQueuedAcpPrompt(fixture.app, "queued-edits", 0, &error, std::pair{before.front().id, before.front().revision + 1}));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts, before);
	UAM_ASSERT(uam::RemoveQueuedAcpPrompt(fixture.app, "queued-edits", 0, &error, std::pair{before.front().id, before.front().revision}));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts, (std::deque<uam::AcpQueuedUserPromptState>{before[1], before[2]}));
	UAM_ASSERT_EQ(ProjectedVisibleQueueIds(fixture), (std::vector<std::string>{"one", "two"}));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, "queued-edits");
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->acp_queued_prompts, (std::vector<uam::AcpQueuedUserPromptState>{before[1], before[2]}));
}

#include "common/chat/acp_prompt_recovery.h"
namespace
{
	struct ExactRetainedReceiptRetryFixture
	{
		TempDir temp{"uam-explicit-retained-receipt-retry"};
		uam::AppState app;
		ExactRetainedReceiptRetryFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
			app.provider_profiles.front().interactive_command.clear();
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			ChatSession chat;
			chat.id = "retained-receipt-retry"; chat.provider_id = "codex-cli"; chat.execution_host_id = "local";
			chat.created_at = chat.updated_at = "2026-10-22T12:00:00Z";
			chat.workspace_directory = temp.root.string();
			chat.workspace_isolation_kind = uam::paths::kPendingGitWorktreeIsolationKind;
			chat.memory_enabled = false;
			Message row; row.role = MessageRole::User; row.content = "One retained input, no new authored row.";
			row.created_at = chat.created_at; row.provider = chat.provider_id; row.interrupted = row.acp_prompt_not_sent = true;
			chat.messages.push_back(row);
			uam::SelectedPrimaryAgentSnapshot capture; std::string error;
			UAM_ASSERT(uam::AgentDefinitionService::CaptureSelectedPrimary(app.data_root, temp.root, "plan", capture, error));
			uam::AcpQueuedUserPromptState prompt;
			prompt.id = "exact-retained-receipt"; prompt.text = chat.messages.front().content;
			prompt.append_user_message = false; prompt.direct_admission = true;
			prompt.existing_user_message_index = 0; prompt.existing_user_message_created_at = chat.created_at;
			prompt.uam_agent_id = capture.definition.id;
			prompt.uam_agent_definition_hash = capture.definition.definition_hash;
			prompt.uam_agent_definition_snapshot = capture.definition.markdown_snapshot;
			prompt.uam_agent_instructions = capture.definition.instructions;
			prompt.uam_agent_skills = capture.definition.skills; prompt.uam_agent_delegates = capture.definition.delegates;
			prompt.uam_agent_workspace_access = capture.definition.workspace_access;
			prompt.uam_agent_execution_capability = uam::AgentDefinitionService::ExecutionCapabilityForProvider(chat.provider_id);
			chat.acp_queued_prompts = {prompt}; chat.acp_queue_held = true;
			chat.acp_unsent_prompt_recoveries = {uam::prompt_recovery::Capture(chat, 0, prompt)};
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat)); app.chats.push_back(chat);
		}
	};
}

UAM_TEST(ExactRetainedDirectReceiptRetrySchedulesOnlyItsOriginalInput)
{
	ExactRetainedReceiptRetryFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	const auto rows = chat.messages; const auto receipt = chat.acp_queued_prompts.front();
	const auto captures = chat.acp_unsent_prompt_recoveries;
	std::string error;
	UAM_ASSERT(uam::RetryQueuedAcpPrompt(fixture.app, chat.id, {receipt.id, receipt.revision}, &error));
	const auto* session = uam::FindAcpSessionForChat(fixture.app, chat.id);
	UAM_ASSERT(session && session->reconnect_pending && !session->queue_held && !session->running && !session->processing);
	UAM_ASSERT(session->prompt_request_id == 0 && session->last_process_id.empty());
	UAM_ASSERT_EQ(chat.messages, rows); UAM_ASSERT_EQ(chat.acp_unsent_prompt_recoveries, captures);
	UAM_ASSERT_EQ(chat.acp_queued_prompts, std::vector<uam::AcpQueuedUserPromptState>{receipt});
	UAM_ASSERT(!chat.acp_queue_held && chat.acp_dispatched_queued_prompt_count == 0);
	UAM_ASSERT(!uam::RetryQueuedAcpPrompt(fixture.app, chat.id, {receipt.id, receipt.revision}, &error));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->messages == rows && saved->acp_queued_prompts == chat.acp_queued_prompts && !saved->acp_queue_held);
}

UAM_TEST(ExactRetainedDirectReceiptRetryPreservesRefusedOrUnknownInput)
{
	for (int restriction = 0; restriction < 14; ++restriction)
	{
		ExactRetainedReceiptRetryFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		const auto receipt = chat.acp_queued_prompts.front();
		std::pair<std::string, int> expected{receipt.id, receipt.revision};
		if (restriction == 0) ++expected.second;
		if (restriction == 1) expected.first = "another-receipt";
		if (restriction == 2) chat.acp_queued_prompts.front().prepared_for_delivery = true;
		if (restriction == 3) chat.remote_prompt_delivery_id = "unresolved-original-delivery";
		if (restriction == 4) chat.imported_read_only = true;
		if (restriction == 5) chat.acp_unsent_prompt_recoveries.front().uam_agent_instructions += "changed";
		if (restriction == 6) chat.acp_queued_prompts.front().existing_user_message_index = -1;
		if (restriction == 7) fixture.app.pending_chat_save_at_by_chat_id[chat.id] = 0.0;
		if (restriction == 8) { chat.messages_loaded = false; chat.messages.clear(); }
		if (restriction == 9) chat.acp_queued_prompts.push_back(chat.acp_queued_prompts.front());
		if (restriction == 10) chat.acp_dispatched_queued_prompt_count = 1;
		if (restriction == 11) chat.acp_queue_storage_error = true;
		if (restriction == 12) chat.acp_queue_held = false;
		if (restriction == 13) chat.acp_queued_prompts.front().claude_quota_continuation_id = "not-an-ordinary-input";
		const ChatSession before = chat;
		const std::string primary = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, chat.id));
		std::string error;
		UAM_ASSERT(!uam::RetryQueuedAcpPrompt(fixture.app, chat.id, expected, &error));
		UAM_ASSERT_EQ(chat.messages, before.messages); UAM_ASSERT_EQ(chat.acp_queued_prompts, before.acp_queued_prompts);
		UAM_ASSERT_EQ(chat.acp_unsent_prompt_recoveries, before.acp_unsent_prompt_recoveries);
		UAM_ASSERT_EQ(chat.acp_queue_held, before.acp_queue_held);
		UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, before.acp_dispatched_queued_prompt_count);
		UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, chat.id)), primary);
		for (const auto& session : fixture.app.acp_sessions)
			UAM_ASSERT(!session->running && !session->processing && !session->reconnect_pending && session->last_process_id.empty());
	}
}


UAM_TEST(StoppedRetainedDirectReceiptHydrationDoesNotInventPendingDelivery)
{
	ExactRetainedReceiptRetryFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	auto& session = uam::acp_detail::EnsureAcpSessionForChat(fixture.app, chat);
	session.running = session.processing = session.reconnect_pending = false;
	session.queue_held = true;
	// An unresolved old request excludes the row from native known-unsent authority;
	// its absence is not evidence of a current send.
	session.turn_user_message_index = 0;
	session.prompt_request_id = 71;
	const ChatSession authored = chat;
	const auto projection = [&]() { return uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"]; };
	chat.messages_loaded = false;
	const auto cold = projection();
	UAM_ASSERT(cold["pendingDirectPrompts"].empty());
	UAM_ASSERT_EQ(cold["unsentDirectPrompts"].size(), std::size_t{1});
	chat.messages_loaded = true;
	UAM_ASSERT(uam::KnownUnsentAcpMessageIndices(fixture.app, chat).empty());
	const auto loaded = projection();
	UAM_ASSERT(loaded["pendingDirectPrompts"].empty());
	UAM_ASSERT_EQ(loaded["unsentDirectPrompts"][0]["id"], chat.acp_queued_prompts.front().id);
	UAM_ASSERT_EQ(loaded["unsentDirectPrompts"][0]["referencedMessageCreatedAt"], chat.messages.front().created_at);
	UAM_ASSERT_EQ(loaded["unsentDirectPrompts"][0]["retryAllowed"], false);
	UAM_ASSERT(!cold["unsentDirectPrompts"][0].contains("referencedMessageCreatedAt"));
	UAM_ASSERT(loaded["recoverableUnsentMessageIndices"].empty());
	UAM_ASSERT_EQ(chat.messages, authored.messages);
	UAM_ASSERT_EQ(chat.acp_queued_prompts, authored.acp_queued_prompts);
	UAM_ASSERT_EQ(chat.acp_unsent_prompt_recoveries, authored.acp_unsent_prompt_recoveries);
	// Genuine active admission continues to project pending rather than granting recovery.
	session.queue_held = false; session.reconnect_pending = true;
	const auto active = projection();
	UAM_ASSERT_EQ(active["pendingDirectPrompts"].size(), std::size_t{1});
	UAM_ASSERT(active["unsentDirectPrompts"].empty());
	session.reconnect_pending = false; session.queue_held = true; session.processing = true;
	UAM_ASSERT_EQ(projection()["pendingDirectPrompts"].size(), std::size_t{1});
	session.processing = false;
	UAM_ASSERT(!session.running && !session.processing && session.last_process_id.empty());
}
