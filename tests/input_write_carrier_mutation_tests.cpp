#include "test_harness.h"
#include "common/chat/acp_prompt_recovery.h"
#include "common/chat/chat_message_page_index.h"
#include "common/runtime/acp/acp_session_runtime.h"

using namespace uam_test;

namespace
{
	// Historical persisted DATA only. No current writer, receipt, pipe, process, or provider
	// is fabricated or launched. A full historical pipe write and definite-unsent disposition
	// can coexist when the provider subsequently refuses that input.
	struct HistoricalWriteEditFixture
	{
		TempDir temp{"uam-historical-write-edit"};
		uam::AppState app;
		std::string chat_id;
		std::string attachment_file;
		std::string skill_file;

		explicit HistoricalWriteEditFixture(bool carriers = true, bool captured_policy = true)
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			ChatSession chat = ChatDomainService().CreateNewChat({}, "codex-cli", false);
			chat.workspace_directory = temp.root.string();
			chat.execution_host_id = "local";
			chat.native_session_id = "historical-native-session";
			ChatDomainService().AddMessage(chat, MessageRole::User, "Historical authored input.");
			Message& row = chat.messages.front();
			row.provider = chat.provider_id;
			row.acp_prompt_not_sent = true;
			row.interrupted = true;
			row.native_input_id = "5e260211-2bef-4d58-8df9-e372c5c2ba6a";
			row.origin_request_id = "historical-authored-input-request-1";
			row.input_write_completed = true;
			if (carriers)
			{
				attachment_file = (temp.root / "retained-original.txt").string();
				skill_file = (temp.root / "captured-skill.md").string();
				UAM_ASSERT(uam::io::WriteTextFile(attachment_file, "private historical attachment bytes"));
				UAM_ASSERT(uam::io::WriteTextFile(skill_file, "current disk skill text must not replace the captured block"));
				MessageAttachment attachment;
				attachment.id = "historical-attachment";
				attachment.name = "retained-original.txt";
				attachment.kind = "file";
				attachment.mime_type = "text/plain";
				attachment.path = attachment_file;
				row.attachments.push_back(attachment);
				row.markdown_store_files = {skill_file};
				row.markdown_store_prompt_blocks = {"Captured historical skill bytes."};
			}
			if (captured_policy)
				chat.acp_unsent_prompt_recoveries.push_back(uam::prompt_recovery::Capture(chat, 0, CapturedPrompt(chat)));
			UAM_ASSERT(uam::prompt_recovery::BindingsMatch(chat));
			chat_id = chat.id;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat_id, true);
			UAM_ASSERT(saved && saved->messages_loaded && saved->messages.size() == 1);
			UAM_ASSERT_EQ(saved->messages.front(), row);
			UAM_ASSERT(saved->messages.front().input_write_completed);
			app.chats.push_back(*saved);
			UAM_ASSERT_EQ(uam::KnownUnsentAcpMessageIndices(app, Chat()), std::vector<int>({0}));
			UAM_ASSERT(app.acp_sessions.empty());
		}

		ChatSession& Chat() { return app.chats.front(); }
		Message& Row() { return Chat().messages.front(); }
		std::string Digest()
		{
			std::uint64_t hash = uam::hashing::kFnv1a64OffsetBasis;
			for (const Message& row : Chat().messages) uam::chat_pages::AppendMessageDigest(hash, row);
			return uam::chat_pages::FinishMessageDigest(hash, Chat().messages.size());
		}
		static uam::AcpQueuedUserPromptState CapturedPrompt(const ChatSession& chat)
		{
			uam::AgentDefinition definition;
			std::string error;
			UAM_ASSERT(uam::AgentDefinitionService::DecodeCapturedPrimary("plan", "builtin:plan:v2", definition, &error));
			uam::AcpQueuedUserPromptState prompt;
			prompt.id = "historical-held-input";
			prompt.revision = 1;
			prompt.text = chat.messages.front().content;
			prompt.native_input_id = chat.messages.front().native_input_id;
			prompt.origin_request_id = chat.messages.front().origin_request_id;
			prompt.attachments = chat.messages.front().attachments;
			prompt.markdown_store_files = chat.messages.front().markdown_store_files;
			prompt.markdown_store_prompt_blocks = chat.messages.front().markdown_store_prompt_blocks;
			prompt.append_user_message = false;
			prompt.existing_user_message_index = 0;
			prompt.existing_user_message_created_at = chat.messages.front().created_at;
			prompt.uam_agent_id = "plan";
			prompt.uam_agent_definition_hash = definition.definition_hash;
			prompt.uam_agent_definition_snapshot = definition.markdown_snapshot;
			prompt.uam_agent_instructions = definition.instructions;
			prompt.uam_agent_skills = definition.skills;
			prompt.uam_agent_delegates = definition.delegates;
			prompt.uam_agent_workspace_access = definition.workspace_access;
			prompt.uam_agent_execution_capability = "uam-prompt-injected";
			return prompt;
		}
		void StageHeldReferencedQueue()
		{
			const auto prompt = CapturedPrompt(Chat());
			Chat().acp_queued_prompts = {prompt};
			Chat().acp_queue_held = true;
			Chat().acp_dispatched_queued_prompt_count = 0;
			UAM_ASSERT(uam::prompt_recovery::BindingsMatch(Chat()));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, Chat()));
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat_id, true);
			UAM_ASSERT(saved && saved->acp_queued_prompts == Chat().acp_queued_prompts && saved->acp_queue_held);
			Chat() = *saved;
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = chat_id;
			session->provider_id = Chat().provider_id;
			session->session_id = Chat().native_session_id;
			session->process_execution_host_id = Chat().execution_host_id;
			session->queue_held = true;
			session->queued_user_prompts.assign(Chat().acp_queued_prompts.begin(), Chat().acp_queued_prompts.end());
			// Deliberately stopped, no handles, pending requests, workers, or input preparation.
			UAM_ASSERT(!session->running && !session->processing && session->prompt_request_id == 0);
			app.acp_sessions.push_back(std::move(session));
		}
		void AssertAcceptedChangedRow(const Message& before)
		{
			const Message after = Row();
			UAM_ASSERT(after.native_input_id == before.native_input_id);
			UAM_ASSERT(after.origin_request_id == before.origin_request_id);
			UAM_ASSERT_EQ(after.created_at, before.created_at);
			UAM_ASSERT(after.acp_prompt_not_sent && !after.acp_prompt_discarded);
			UAM_ASSERT(uam::prompt_recovery::BindingsMatch(Chat()));
			const auto durable = ChatRepository::LoadLocalChat(app.data_root, chat_id, true);
			UAM_ASSERT(durable && durable->messages.size() == 1);
			UAM_ASSERT_EQ(durable->messages.front(), after);
			// Intended mutation oracle: the old physical write cannot confirm changed carriers.
			UAM_ASSERT(!after.input_write_completed);
			UAM_ASSERT(!durable->messages.front().input_write_completed);
		}
	};
}

UAM_TEST(UnsentTextEditInvalidatesHistoricalInputWriteFact)
{
	HistoricalWriteEditFixture fixture;
	const Message before = fixture.Row();
	std::string error;
	UAM_ASSERT(uam::EditUnsentAcpMessage(fixture.app, fixture.chat_id, 0, fixture.Digest(), "Changed authored input.", &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(fixture.Row().content, std::string("Changed authored input."));
	UAM_ASSERT_EQ(fixture.Row().attachments, before.attachments);
	UAM_ASSERT_EQ(fixture.Row().markdown_store_prompt_blocks, before.markdown_store_prompt_blocks);
	fixture.AssertAcceptedChangedRow(before);
}

UAM_TEST(UnsentAttachmentRemovalInvalidatesHistoricalInputWriteFact)
{
	HistoricalWriteEditFixture fixture;
	const Message before = fixture.Row();
	std::string error;
	UAM_ASSERT(uam::EditUnsentAcpMessage(fixture.app, fixture.chat_id, 0, fixture.Digest(), before.content, &error, std::vector<std::string>{}));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(fixture.Row().content, before.content);
	UAM_ASSERT(fixture.Row().attachments.empty());
	UAM_ASSERT_EQ(fixture.Row().markdown_store_files, before.markdown_store_files);
	fixture.AssertAcceptedChangedRow(before);
}

UAM_TEST(UnsentSkillRemovalInvalidatesHistoricalInputWriteFact)
{
	HistoricalWriteEditFixture fixture;
	const Message before = fixture.Row();
	std::string error;
	UAM_ASSERT(uam::EditUnsentAcpMessage(fixture.app, fixture.chat_id, 0, fixture.Digest(), before.content, &error, std::nullopt, std::vector<std::string>{}));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(fixture.Row().content, before.content);
	UAM_ASSERT_EQ(fixture.Row().attachments, before.attachments);
	UAM_ASSERT(fixture.Row().markdown_store_files.empty());
	UAM_ASSERT(fixture.Row().markdown_store_prompt_blocks.empty());
	fixture.AssertAcceptedChangedRow(before);
}

UAM_TEST(UnsentNoOpEditPreservesHistoricalInputWriteFact)
{
	HistoricalWriteEditFixture fixture;
	const Message before = fixture.Row();
	std::string error;
	UAM_ASSERT(uam::EditUnsentAcpMessage(fixture.app, fixture.chat_id, 0, fixture.Digest(), before.content, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(fixture.Row(), before);
	const auto durable = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.chat_id, true);
	UAM_ASSERT(durable && durable->messages.front() == before && durable->messages.front().input_write_completed);
}

UAM_TEST(UnsentStaleDigestRefusalPreservesHistoricalInputWriteFact)
{
	HistoricalWriteEditFixture fixture;
	const Message before = fixture.Row();
	std::string foreign_digest = fixture.Digest();
	foreign_digest[0] = foreign_digest[0] == '0' ? '1' : '0';
	std::string error;
	UAM_ASSERT(!uam::EditUnsentAcpMessage(fixture.app, fixture.chat_id, 0, foreign_digest, "Refused new bytes.", &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(fixture.Row(), before);
	const auto durable = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.chat_id, true);
	UAM_ASSERT(durable && durable->messages.front() == before && durable->messages.front().input_write_completed);
}

UAM_TEST(HeldReferencedQueueTextEditInvalidatesHistoricalInputWriteFact)
{
	HistoricalWriteEditFixture fixture;
	fixture.StageHeldReferencedQueue();
	const Message before = fixture.Row();
	const auto prompt = fixture.app.acp_sessions.front()->queued_user_prompts.front();
	std::string error;
	UAM_ASSERT(uam::EditQueuedAcpPrompt(fixture.app, fixture.chat_id, prompt.id, prompt.revision, "Changed held input.", &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(fixture.Row().content, std::string("Changed held input."));
	const auto& revised = fixture.app.acp_sessions.front()->queued_user_prompts.front();
	UAM_ASSERT_EQ(revised.revision, prompt.revision + 1);
	UAM_ASSERT_EQ(revised.text, fixture.Row().content);
	UAM_ASSERT_EQ(fixture.Chat().acp_queued_prompts.front(), revised);
	fixture.AssertAcceptedChangedRow(before);
}

UAM_TEST(HeldReferencedQueueAttachmentRemovalInvalidatesHistoricalInputWriteFact)
{
	HistoricalWriteEditFixture fixture;
	fixture.StageHeldReferencedQueue();
	const Message before = fixture.Row();
	const auto prompt = fixture.app.acp_sessions.front()->queued_user_prompts.front();
	std::string error;
	UAM_ASSERT(uam::EditQueuedAcpPrompt(fixture.app, fixture.chat_id, prompt.id, prompt.revision, before.content, &error, std::vector<std::string>{}));
	UAM_ASSERT(error.empty());
	UAM_ASSERT(fixture.Row().attachments.empty());
	UAM_ASSERT_EQ(fixture.Row().content, before.content);
	UAM_ASSERT_EQ(fixture.Chat().acp_queued_prompts.front().text, before.content);
	UAM_ASSERT(fixture.Chat().acp_queued_prompts.front().attachments.empty());
	fixture.AssertAcceptedChangedRow(before);
}

UAM_TEST(EditedBranchReplacementInvalidatesOnlyChangedHistoricalInputWriteFact)
{
	HistoricalWriteEditFixture fixture(false, false);
	fixture.Row().acp_prompt_not_sent = false;
	fixture.Row().interrupted = false;
	const Message before = fixture.Row();
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
	// Plain workspace, no native/terminal launch, no Git setup, no native originals/assets.
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(fixture.app, fixture.chat_id, 0, std::string("Replacement branch input."),
	    std::nullopt, nullptr, std::nullopt, nullptr, nullptr, false));
	const auto branch_it = std::ranges::find_if(fixture.app.chats, [&fixture](const ChatSession& chat)
	    { return chat.id != fixture.chat_id && chat.parent_chat_id == fixture.chat_id; });
	UAM_ASSERT(branch_it != fixture.app.chats.end());
	UAM_ASSERT(branch_it->branch_message_edited && branch_it->messages.size() == 1);
	UAM_ASSERT_EQ(branch_it->messages.front().content, std::string("Replacement branch input."));
	const ChatSession* original = ChatDomainService().FindChatById(fixture.app, fixture.chat_id);
	UAM_ASSERT(original && original->messages.front() == before && original->messages.front().input_write_completed);
	const auto saved_original = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.chat_id, true);
	const auto saved_branch = ChatRepository::LoadLocalChat(fixture.app.data_root, branch_it->id, true);
	UAM_ASSERT(saved_original && saved_original->messages.front() == before && saved_original->messages.front().input_write_completed);
	UAM_ASSERT(saved_branch && saved_branch->messages == branch_it->messages);
	UAM_ASSERT(!branch_it->messages.front().input_write_completed);
	UAM_ASSERT(!saved_branch->messages.front().input_write_completed);
	UAM_ASSERT(fixture.app.acp_sessions.empty());
}
