#include "test_harness.h"
#include "app/chat_title_service.h"
#include "app/provider_worker_command.h"
#include "common/state/app_state.h"
#include <nlohmann/json.hpp>

using namespace uam_test;

UAM_TEST(ChatTitlePromptBoundsConversationAndOmitsSystemMemoryToolsAndUndeliveredInputs)
{
	ChatSession chat; chat.title = "Previous title";
	Message system; system.role = MessageRole::System; system.content = "SECRET_SYSTEM_CONTEXT"; chat.messages.push_back(system);
	for (int index = 0; index < 100; ++index)
	{
		Message message; message.role = index % 2 == 0 ? MessageRole::User : MessageRole::Assistant;
		message.content = std::string(10000, 'x'); message.thoughts = "PRIVATE_REASONING"; chat.messages.push_back(message);
	}
	Message unsent; unsent.content = "UNSENT_INPUT"; unsent.acp_prompt_not_sent = true; chat.messages.push_back(unsent);
	const std::string prompt = uam::ChatTitleService::BuildPrompt(chat);
	UAM_ASSERT(prompt.size() < 10000); UAM_ASSERT(prompt.find("PRIVATE_REASONING") == std::string::npos);
	UAM_ASSERT(prompt.find("SECRET_SYSTEM_CONTEXT") == std::string::npos); UAM_ASSERT(prompt.find("UNSENT_INPUT") == std::string::npos);
	const nlohmann::json context = nlohmann::json::parse(prompt.substr(prompt.find('\n') + 1));
	UAM_ASSERT(context["messages"].size() <= 16); UAM_ASSERT_EQ(context["previousTitle"].get<std::string>(), "Previous title");
}

UAM_TEST(ChatTitlePromptSupportsAttachmentNamesAndDoesNotSplitUtf8)
{
	ChatSession chat; Message message; message.content = std::string(2047, 'x') + "é";
	chat.messages.push_back(message);
	UAM_ASSERT(!uam::ChatTitleService::BuildPrompt(chat).empty());
	chat.messages[0].content.clear(); MessageAttachment attachment; attachment.name = "diagram.png"; attachment.path = "PRIVATE_FILE_PATH"; chat.messages[0].attachments.push_back(attachment);
	const std::string prompt = uam::ChatTitleService::BuildPrompt(chat);
	UAM_ASSERT(prompt.find("diagram.png") != std::string::npos); UAM_ASSERT(prompt.find("PRIVATE_FILE_PATH") == std::string::npos);
	chat.messages.clear(); UAM_ASSERT(uam::ChatTitleService::BuildPrompt(chat).empty());
}

UAM_TEST(ChatTitleResultsValidateLengthControlCharactersAndProviderEnvelopes)
{
	for (const std::string& provider : {"gemini-cli", "claude-cli", "copilot-cli", "opencode-cli", "codex-cli"})
		UAM_ASSERT_EQ(uam::ChatTitleService::ParseResult(R"({"title":" Fix chat loading "})", provider).title, "Fix chat loading");
	UAM_ASSERT(uam::ChatTitleService::ParseResult(R"({"result":"{\"title\":\"Fix chat loading\"}","is_error":false})", "claude-cli").ok);
	UAM_ASSERT(uam::ChatTitleService::ParseResult(R"({"response":"{\"title\":\"Fix chat loading\"}"})", "gemini-cli").ok);
	for (const std::string& invalid : {std::string(R"({"title":""})"), std::string(R"({"title":4})"), std::string(R"({"title":"two\nlines"})"), std::string(R"({"title":"bad","is_error":true})"), std::string(R"({"title":"bad","is_error":"no"})"), nlohmann::json({{"title", std::string(241, 'a')}}).dump(), std::string(65537, 'x')})
		UAM_ASSERT(!uam::ChatTitleService::ParseResult(invalid, "claude-cli").ok);
	UAM_ASSERT(!uam::ChatTitleService::ParseResult("{not JSON}", "gemini-cli").ok);
}

UAM_TEST(ChatTitleResultsRejectUnfinishedCodexStreamsAndOpenCodeToolEvents)
{
	const std::string codex = R"({"type":"thread.started","thread_id":"thread"}
{"type":"turn.started"}
{"type":"item.completed","item":{"type":"agent_message","text":"{\"title\":\"Fix chat loading\"}"}}
)";
	UAM_ASSERT(!uam::ChatTitleService::ParseResult(codex, "codex-cli").ok);
	UAM_ASSERT(uam::ChatTitleService::ParseResult(codex + "{\"type\":\"turn.completed\"}\n", "codex-cli").ok);
	const std::string opencode = R"({"type":"step_start"}
{"type":"text","part":{"text":"{\"title\":\"Fix chat loading\"}"}}
{"type":"step_finish"}
)";
	UAM_ASSERT(uam::ChatTitleService::ParseResult(opencode, "opencode-cli").ok);
	UAM_ASSERT(!uam::ChatTitleService::ParseResult(opencode + "{\"type\":\"tool_use\"}\n", "opencode-cli").ok);
}

UAM_TEST(ChatTitleCancellationAndUnreadyRunnerDoNotLaunchWorkers)
{
	uam::AppState app; ChatSession chat;
	std::stop_source cancellation; cancellation.request_stop();
	UAM_ASSERT(!uam::ChatTitleService::Generate(app, chat, cancellation.get_token()).ok);
	ExecutionHost host; host.runner_status = "error";
	uam::ProviderWorkerInvocation invocation; invocation.direct_process = true; invocation.argv = {"never-launch"};
	UAM_ASSERT(!uam::ExecuteIsolatedProviderTextWorker(invocation, "codex-cli", &host).ok);
}

UAM_TEST(ChatTitlePreparedUpdatePreservesLatestTranscriptAndRejectsNewerChanges)
{
	TempDir temp("uam-title-update");
	ChatSession chat; chat.id = "title-chat"; chat.title = "Original"; chat.provider_id = "codex-cli"; chat.created_at = "2026-01-01"; chat.updated_at = "2026-01-01"; chat.last_opened_at = "2026-01-01";
	chat.messages.push_back(Message{MessageRole::User, "First message"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const uam::ChatTitleIdentity original = uam::ChatTitleIdentity::Capture(chat);
	chat.messages.push_back(Message{MessageRole::Assistant, "Newer reply"}); chat.updated_at = "2026-01-02";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	uam::PreparedChatTitleUpdate update = uam::ChatTitleService::PrepareUpdate(temp.root, original, "Generated title");
	UAM_ASSERT(update.save != nullptr); UAM_ASSERT(update.identity.Matches(chat));
	UAM_ASSERT(ChatRepository::PublishPreparedChatSave(update.save, true, &chat));
	const std::optional<ChatSession> renamed = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(renamed.has_value()); UAM_ASSERT_EQ(renamed->title, "Generated title");
	UAM_ASSERT_EQ(renamed->messages.size(), std::size_t{2}); UAM_ASSERT_EQ(renamed->messages[1].content, "Newer reply");
	UAM_ASSERT(!uam::ChatTitleService::PrepareUpdate(temp.root, original, "Stale title").save);
	const uam::ChatTitleIdentity latest = uam::ChatTitleIdentity::Capture(*renamed);
	update = uam::ChatTitleService::PrepareUpdate(temp.root, latest, "Another title");
	UAM_ASSERT(update.save != nullptr);
	ChatSession manually_renamed = *renamed; manually_renamed.title = "Manual rename";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, manually_renamed));
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(update.save));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(temp.root, chat.id)->title, "Manual rename");
	manually_renamed.provider_id = "opencode-cli"; UAM_ASSERT(!latest.Matches(manually_renamed, false));
	manually_renamed = *renamed; manually_renamed.execution_host_id = "remote"; UAM_ASSERT(!latest.Matches(manually_renamed, false));
	manually_renamed = *renamed; manually_renamed.last_opened_at = "new-open-time"; UAM_ASSERT(!latest.Matches(manually_renamed));
}

UAM_TEST(ChatTitlePreparedUpdateRejectsCancellationReadOnlyAndSummaryChanges)
{
	TempDir temp("uam-title-guards");
	ChatSession chat; chat.id = "title-guards"; chat.title = "Original"; chat.provider_id = "codex-cli";
	chat.messages.push_back(Message{MessageRole::User, "First message"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const uam::ChatTitleIdentity identity = uam::ChatTitleIdentity::Capture(chat);
	std::stop_source cancellation; cancellation.request_stop();
	UAM_ASSERT(!uam::ChatTitleService::PrepareUpdate(temp.root, identity, "Generated", cancellation.get_token()).save);
	UAM_ASSERT(!uam::ChatTitleService::PrepareUpdate(temp.root, identity, "bad\ntitle").save);
	uam::PreparedChatTitleUpdate update = uam::ChatTitleService::PrepareUpdate(temp.root, identity, "Generated");
	UAM_ASSERT(update.save != nullptr);
	chat.last_opened_at = "2026-10-10T10:00:00Z";
	UAM_ASSERT(ChatRepository::SaveLastOpenedAt(temp.root, chat));
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(update.save));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(temp.root, chat.id)->title, "Original");
	chat.imported_read_only = true; UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	UAM_ASSERT(!uam::ChatTitleService::PrepareUpdate(temp.root, identity, "Generated").save);
	UAM_ASSERT(!uam::ChatTitleService::PrepareUpdate(temp.root, uam::ChatTitleIdentity{"missing"}, "Generated").save);
}

#if !defined(_WIN32)
UAM_TEST(ChatTitleIsolatedWorkerExecutesPromptOnStdinAndCleansItsDirectory)
{
	TempDir temp("uam-title-worker");
	const fs::path shim = temp.root / "title-worker.sh";
	UAM_ASSERT(uam::io::WriteTextFile(shim, "#!/bin/sh\nset -eu\ntest ! -f AGENTS.md\ncat >/dev/null\nprintf '{\"title\":\"Improve chat loading\"}'\n"));
	fs::path isolated;
	{
		uam::AppState app;
		ChatSession chat; chat.messages.push_back(Message{MessageRole::User, "Review chat loading"});
		const ProviderProfile profile = ProviderProfileStore::DefaultClaudeProfile();
		std::string error;
		uam::ProviderWorkerInvocation invocation = uam::BuildProviderWorkerInvocation(app, profile, app.settings, uam::ChatTitleService::BuildPrompt(chat), "", uam::ProviderWorkerPathMode::BasePath, &error);
		UAM_ASSERT(!invocation.Empty()); UAM_ASSERT(error.empty()); UAM_ASSERT(invocation.isolated_working_directory != nullptr);
		isolated = *invocation.isolated_working_directory;
		UAM_ASSERT(isolated != temp.root); UAM_ASSERT(fs::exists(isolated));
		UAM_ASSERT(std::find(invocation.argv.begin(), invocation.argv.end(), "--tools") != invocation.argv.end());
		invocation.argv[0] = "/bin/sh"; invocation.argv.insert(invocation.argv.begin() + 1, shim.string());
		const ProcessExecutionResult result = uam::ExecuteIsolatedProviderTextWorker(invocation, profile.id, nullptr, 5000);
		UAM_ASSERT(result.ok); UAM_ASSERT_EQ(result.exit_code, 0);
		UAM_ASSERT_EQ(uam::ChatTitleService::ParseResult(result.output, profile.id).title, "Improve chat loading");
	}
	UAM_ASSERT(!fs::exists(isolated));
}
#endif
