#include "test_harness.h"
#include "app/opencode_compaction_sync_service.h"
#include "app/opencode_compaction_history.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"

#include <atomic>
#include <thread>

using namespace uam_test;

namespace
{
	struct CompactionHistoryFixture
	{
		TempDir temp{"uam-compaction-history"};
		uam::AppState app;
		nlohmann::json rows = nlohmann::json::array();
		std::atomic<int> queries{0}, finished{0};
		std::atomic<bool> hold{false};
		bool fail = false;
		std::string foreign_session, foreign_directory;
		std::unique_ptr<uam::OpenCodeCompactionSyncService> service;

		explicit CompactionHistoryFixture(int summaries = 2)
		{
			app.data_root = temp.root / "data";
			app.provider_profiles.push_back(ProviderProfileStore::DefaultOpenCodeProfile());
			app.settings.git_enabled = app.settings.git_auto_worktree = false;
			ChatSession chat;
			chat.id = "history-owner"; chat.provider_id = "opencode-cli";
			chat.native_session_id = "ses_history"; chat.execution_host_id = "local";
			chat.workspace_directory = uam::paths::Utf8PathString(temp.root);
			chat.created_at = "2026-10-15T00:00:00Z";
			chat.updated_at = "2026-10-15T00:00:01Z";
			chat.title = "Preserve local title";
			Message user;
			user.role = MessageRole::User; user.content = "Private synthetic request";
			chat.messages.push_back(std::move(user));
			Message assistant;
			assistant.role = MessageRole::Assistant;
			assistant.created_at = chat.updated_at;
			const auto tool = [&](int index)
			{
				const std::string id = "tool_" + std::to_string(index);
				assistant.blocks.push_back({"tool_call", "", id, ""});
				ToolCall call; call.id = id; call.name = "Synthetic tool"; call.result_text = "Keep result";
				assistant.tool_calls.push_back(std::move(call));
				rows.push_back({{"messageId", "msg_tool_" + std::to_string(index)}, {"partId", "part_tool_" + std::to_string(index)},
				    {"type", "tool"}, {"toolId", id}, {"summary", 0}, {"parentProof", 0}, {"finish", "stop"}, {"errorType", nullptr}});
			};
			tool(0);
			for (int index = 0; index < summaries; ++index)
			{
				// Identical ordinary replies and summaries must be distinguished by native position and tool anchors.
				const std::string summary = "Repeated synthetic summary";
				assistant.blocks.push_back({"assistant_text", summary, "", ""});
				assistant.blocks.push_back({"thought", "Keep private synthetic reasoning", "", ""});
				assistant.blocks.push_back({"assistant_text", summary, "", ""});
				assistant.content += summary + summary;
				rows.push_back({{"messageId", "msg_summary_" + std::to_string(index)}, {"partId", "part_summary_" + std::to_string(index)},
				    {"type", "text"}, {"text", summary}, {"summary", 1}, {"parentProof", 1}, {"finish", "stop"}, {"errorType", nullptr}});
				rows.push_back({{"messageId", "msg_reply_" + std::to_string(index)}, {"partId", "part_reply_" + std::to_string(index)},
				    {"type", "text"}, {"text", "\n" + summary + "\n"}, {"summary", 0}, {"parentProof", 0}, {"finish", "stop"}, {"errorType", nullptr}});
				tool(index + 1);
			}
			chat.messages.push_back(std::move(assistant));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			app.chats.push_back(*ChatRepository::LoadLocalChat(app.data_root, chat.id));
			app.selected_chat_index = 0;
			service = std::make_unique<uam::OpenCodeCompactionSyncService>([this](const uam::OpenCodeCompactionSyncService::Query& query, std::stop_token stop)
			{
				++queries;
				UAM_ASSERT(std::ranges::find(query.argv, "db") != query.argv.end());
				UAM_ASSERT(std::ranges::find(query.argv, query.sql) != query.argv.end());
				while (hold.load() && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
				ProcessExecutionResult result;
				result.ok = !fail && !stop.stop_requested(); result.exit_code = result.ok ? 0 : 1;
				result.output = nlohmann::json::array({{{"sessionId", foreign_session.empty() ? "ses_history" : foreign_session},
				    {"directory", foreign_directory.empty() ? uam::paths::Utf8PathString(temp.root) : foreign_directory},
				    {"rows", rows.dump()}}}).dump();
				++finished;
				return result;
			});
		}
		~CompactionHistoryFixture() { hold = false; service->Stop(); }
		ChatSession& Chat() { return app.chats.front(); }
		bool Wait(bool expect_change = true)
		{
			bool changed = false;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (std::chrono::steady_clock::now() < deadline)
			{
				changed = service->Poll(app) || changed;
				if (changed && expect_change) return true;
				if (!expect_change && finished.load() > 0)
				{
					for (int poll = 0; poll < 100; ++poll)
					{
						changed = service->Poll(app) || changed;
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					}
					return changed;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return changed;
		}
		std::size_t Boundaries() const
		{
			std::size_t count = 0;
			for (const Message& message : app.chats.front().messages)
				for (const MessageBlock& block : message.blocks) if (block.type == "context_compaction") ++count;
			return count;
		}
		void WaitStarted()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (queries.load() == 0 && std::chrono::steady_clock::now() < deadline)
			{
				(void)service->Poll(app);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT_EQ(queries.load(), 1);
		}
	};
}

UAM_TEST(OpenCodeCompactionHistoryRepairsAggregatedOldChatAndCachesSelection)
{
	CompactionHistoryFixture fixture(24);
	const ChatSession before = fixture.Chat();
	UAM_ASSERT(fixture.Wait());
	UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{24});
	UAM_ASSERT_EQ(fixture.Chat().messages.size(), before.messages.size());
	UAM_ASSERT_EQ(fixture.Chat().messages[1].content, before.messages[1].content);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].tool_calls, before.messages[1].tool_calls);
	UAM_ASSERT_EQ(fixture.Chat().updated_at, before.updated_at);
	UAM_ASSERT_EQ(fixture.Chat().title, before.title);
	for (std::size_t index = 0; index < 24; ++index)
	{
		const std::size_t block = 1 + index * 4;
		UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[block].type, std::string("context_compaction"));
		UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[block].request_id_json, "msg_summary_" + std::to_string(index));
		UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[block + 1], before.messages[1].blocks[block + 1]);
		UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[block + 2].type, std::string("assistant_text"));
	}
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(saved && saved->messages == fixture.Chat().messages);
	UAM_ASSERT_EQ(saved->messages[1].blocks[1].type, std::string("context_compaction"));
	fixture.Chat().last_opened_at = "2026-10-15T00:10:00Z";
	for (int poll = 0; poll < 30; ++poll) (void)fixture.service->Poll(fixture.app);
	UAM_ASSERT_EQ(fixture.queries.load(), 1);
}

UAM_TEST(OpenCodeCompactionHistoryLoadsUnhydratedSelectedChat)
{
	CompactionHistoryFixture fixture;
	fixture.Chat() = *ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id, false);
	UAM_ASSERT(!fixture.Chat().messages_loaded);
	UAM_ASSERT(fixture.Wait());
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(saved && saved->messages[1].blocks[1].type == "context_compaction");
}

UAM_TEST(OpenCodeCompactionHistoryRejectsUnprovedAndForeignMetadata)
{
	for (int mode = 0; mode < 7; ++mode)
	{
		CompactionHistoryFixture fixture;
		const auto before = fixture.Chat().messages;
		if (mode == 0) fixture.rows[1]["parentProof"] = 0;
		if (mode == 1) fixture.rows[1]["finish"] = nullptr;
		if (mode == 2) fixture.rows[1]["errorType"] = "object";
		if (mode == 3) fixture.rows[1]["text"] = 42;
		if (mode == 4) fixture.foreign_session = "ses_foreign";
		if (mode == 5) fixture.foreign_directory = "/foreign/workspace";
		if (mode == 6) fixture.rows[1]["text"] = "Repeated synthetic summary ";
		UAM_ASSERT(!fixture.Wait(false));
		UAM_ASSERT_EQ(fixture.Chat().messages, before);
		UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{0});
	}
}

UAM_TEST(OpenCodeCompactionHistoryRejectsDuplicateAndChangedToolAnchors)
{
	for (const bool duplicate : {false, true})
	{
		CompactionHistoryFixture fixture;
		fixture.rows[3]["toolId"] = duplicate ? "tool_0" : "tool_foreign";
		UAM_ASSERT(!fixture.Wait(false));
		UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{0});
	}
}

UAM_TEST(OpenCodeCompactionHistoryDoesNotPublishAfterNewTurnOrIdentityChange)
{
	for (const bool new_turn : {false, true})
	{
		CompactionHistoryFixture fixture;
		fixture.hold = true;
		fixture.WaitStarted();
		if (new_turn)
		{
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = fixture.Chat().id; session->provider_id = "opencode-cli";
			session->processing = true; session->turn_serial = 2;
			fixture.app.acp_sessions.push_back(std::move(session));
		}
		else fixture.Chat().native_session_id = "ses_changed";
		fixture.hold = false;
		UAM_ASSERT(!fixture.Wait(false));
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
		UAM_ASSERT(saved && saved->messages[1].blocks[1].type == "assistant_text");
	}
}

UAM_TEST(OpenCodeCompactionHistoryQueryFailureKeepsOriginalHistory)
{
	CompactionHistoryFixture fixture;
	fixture.fail = true;
	const ChatSession before = fixture.Chat();
	UAM_ASSERT(!fixture.Wait(false));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(saved && saved->messages == before.messages);
	UAM_ASSERT_EQ(fixture.Chat().messages, before.messages);
}

UAM_TEST(OpenCodeCompactionHistoryUpdatesRetainedEventsWithoutLosingToolOwners)
{
	CompactionHistoryFixture fixture;
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = fixture.Chat().id; session->provider_id = "opencode-cli";
	session->session_id = fixture.Chat().native_session_id;
	session->turn_serial = session->last_settled_turn_serial = 1;
	session->last_turn_outcome = "completed";
	session->turn_assistant_message_index = session->current_assistant_message_index = 1;
	uam::acp_detail::RestoreTurnEventsFromMessageBlocks(*session, fixture.Chat().messages[1]);
	session->tool_call_message_indices["earlier_tool"] = 0;
	fixture.app.acp_sessions.push_back(std::move(session));
	const auto owners = fixture.app.acp_sessions.front()->tool_call_message_indices;
	UAM_ASSERT(fixture.Wait());
	UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->turn_events[1].type, std::string("context_compaction"));
	UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->tool_call_message_indices, owners);
	UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->turn_events[3].type, std::string("assistant_text"));
}

UAM_TEST(OpenCodeCompactionHistoryBoundedReadPreservesSummaryAndSkipsOversizedPrimary)
{
	CompactionHistoryFixture fixture;
	ChatSession selected = fixture.Chat();
	selected.last_opened_at = "2026-10-15T00:20:00Z";
	UAM_ASSERT(ChatRepository::SaveLastOpenedAt(fixture.app.data_root, selected));
	const fs::path primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.Chat().id);
	const fs::path summary = AppPaths::UamChatSummaryFilePath(fixture.app.data_root, fixture.Chat().id);
	const std::string primary_before = uam::io::ReadTextFile(primary);
	const std::string summary_before = uam::io::ReadTextFile(summary);
	const auto loaded = ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.Chat().id, 8 * 1024 * 1024);
	UAM_ASSERT(loaded && loaded->last_opened_at == selected.last_opened_at);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(primary), primary_before);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(summary), summary_before);
	UAM_ASSERT(!ChatRepository::LoadLocalChatBounded(fixture.app.data_root, fixture.Chat().id, primary_before.size() - 1));
	fs::resize_file(primary, 8 * 1024 * 1024 + 1);
	for (int poll = 0; poll < 100; ++poll)
	{
		UAM_ASSERT(!fixture.service->Poll(fixture.app));
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT_EQ(fixture.queries.load(), 0);
	UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{0});
}

UAM_TEST(OpenCodeCompactionHistoryNeverOverwritesConcurrentSelectionMetadata)
{
	CompactionHistoryFixture fixture;
	fixture.hold = true;
	fixture.WaitStarted();
	fixture.Chat().last_opened_at = "2026-10-15T00:30:00Z";
	UAM_ASSERT(ChatRepository::SaveLastOpenedAt(fixture.app.data_root, fixture.Chat()));
	fixture.app.pending_chat_save_at_by_chat_id[fixture.Chat().id] = 1.0;
	fixture.hold = false;
	UAM_ASSERT(!fixture.Wait(false));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(saved && saved->last_opened_at == fixture.Chat().last_opened_at);
	UAM_ASSERT_EQ(saved->messages[1].blocks[1].type, std::string("assistant_text"));
}

UAM_TEST(OpenCodeCompactionHistoryReplacesOnlyMatchingEmptyNativeMarkers)
{
	for (const bool namespaced : {false, true})
	{
		CompactionHistoryFixture fixture;
		fixture.Chat().messages[1].blocks.push_back({"context_compaction", "", "", (namespaced ? "acp-compaction:" : "") + std::string("msg_summary_0")});
		fixture.Chat().messages[1].blocks.push_back({"context_compaction", "", "", "unrelated_boundary"});
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
		auto session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = fixture.Chat().id; session->provider_id = "opencode-cli";
		session->session_id = fixture.Chat().native_session_id;
		session->turn_assistant_message_index = session->current_assistant_message_index = 1;
		uam::acp_detail::RestoreTurnEventsFromMessageBlocks(*session, fixture.Chat().messages[1]);
		fixture.app.acp_sessions.push_back(std::move(session));
		UAM_ASSERT(fixture.Wait());
		UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{3});
		UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().request_id_json, std::string("unrelated_boundary"));
		UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->turn_events.size(), fixture.Chat().messages[1].blocks.size());
		UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->turn_events[1].type, std::string("context_compaction"));
		UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->turn_events.back().request_id_json, std::string("unrelated_boundary"));
	}
}

UAM_TEST(OpenCodeCompactionHistoryRetriesAStaleSaveWithoutLosingNewMetadata)
{
	for (const bool summary_only : {false, true})
	{
		CompactionHistoryFixture fixture;
		fixture.hold = true;
		fixture.WaitStarted();
		if (summary_only)
		{
			fixture.Chat().last_opened_at = "2026-10-15T00:40:00Z";
			UAM_ASSERT(ChatRepository::SaveLastOpenedAt(fixture.app.data_root, fixture.Chat()));
		}
		else
		{
			fixture.Chat().title = "New title during native lookup";
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
		}
		fixture.hold = false;
		UAM_ASSERT(fixture.Wait());
		UAM_ASSERT_EQ(fixture.queries.load(), 2);
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
		UAM_ASSERT(saved && saved->title == fixture.Chat().title && saved->last_opened_at == fixture.Chat().last_opened_at);
		UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{2});
	}
}

UAM_TEST(OpenCodeCompactionHistoryResetsRetryBudgetForNewMetadata)
{
	CompactionHistoryFixture fixture;
	fixture.hold = true;
	fixture.WaitStarted();
	for (int attempt = 1; attempt <= 4; ++attempt)
	{
		fixture.Chat().last_opened_at = "2026-10-15T00:50:0" + std::to_string(attempt) + "Z";
		UAM_ASSERT(ChatRepository::SaveLastOpenedAt(fixture.app.data_root, fixture.Chat()));
		fixture.hold = false;
		const auto finished_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (fixture.finished.load() < attempt && std::chrono::steady_clock::now() < finished_deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		UAM_ASSERT_EQ(fixture.finished.load(), attempt);
		fixture.hold = true;
		const auto query_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (fixture.queries.load() < attempt + 1 && std::chrono::steady_clock::now() < query_deadline)
		{
			UAM_ASSERT(!fixture.service->Poll(fixture.app));
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT_EQ(fixture.queries.load(), attempt + 1);
	}
	fixture.hold = false;
	UAM_ASSERT(fixture.Wait());
	UAM_ASSERT_EQ(fixture.queries.load(), 5);
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(saved && saved->last_opened_at == fixture.Chat().last_opened_at);
	UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{2});
}

UAM_TEST(OpenCodeCompactionHistoryRejectsMismatchedRetainedEventsWithoutRetrying)
{
	for (int mode = 0; mode < 3; ++mode)
	{
		CompactionHistoryFixture fixture;
		auto session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = fixture.Chat().id; session->provider_id = "opencode-cli";
		session->session_id = fixture.Chat().native_session_id;
		session->turn_assistant_message_index = session->current_assistant_message_index = 1;
		uam::acp_detail::RestoreTurnEventsFromMessageBlocks(*session, fixture.Chat().messages[1]);
		if (mode == 0) session->turn_assistant_message_index = 99;
		if (mode == 1) session->turn_events.pop_back();
		if (mode == 2) session->turn_events[1].text = "Different retained text";
		fixture.app.acp_sessions.push_back(std::move(session));
		UAM_ASSERT(!fixture.Wait(false));
		UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{0});
		UAM_ASSERT_EQ(fixture.queries.load(), 1);
	}
}

UAM_TEST(OpenCodeCompactionHistoryPreservesStaleUnsentFlagsAndAdmissionAuthority)
{
	CompactionHistoryFixture fixture(24);
	fixture.Chat().messages.front().acp_prompt_not_sent = true;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
	const auto before = fixture.Chat().messages;
	const auto known_before = uam::KnownUnsentAcpMessageIndices(fixture.app, fixture.Chat());
	const auto recoverable_before = uam::RecoverableUnsentAcpMessageIndices(fixture.app, fixture.Chat());
	UAM_ASSERT(known_before.empty() && recoverable_before.empty());
	UAM_ASSERT(fixture.Wait());
	UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{24});
	UAM_ASSERT(fixture.Chat().messages.front().acp_prompt_not_sent);
	UAM_ASSERT_EQ(fixture.Chat().messages.front(), before.front());
	UAM_ASSERT_EQ(uam::KnownUnsentAcpMessageIndices(fixture.app, fixture.Chat()), known_before);
	UAM_ASSERT_EQ(uam::RecoverableUnsentAcpMessageIndices(fixture.app, fixture.Chat()), recoverable_before);
	auto restored = fixture.Chat().messages;
	for (std::size_t message = 0; message < restored.size(); ++message)
		for (std::size_t block = 0; block < restored[message].blocks.size(); ++block)
			if (restored[message].blocks[block].type == "context_compaction")
			{
				restored[message].blocks[block].type = before[message].blocks[block].type;
				restored[message].blocks[block].request_id_json = before[message].blocks[block].request_id_json;
			}
	UAM_ASSERT_EQ(restored, before);
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(saved && saved->messages.front() == before.front());
}

UAM_TEST(OpenCodeCompactionHistoryWaitsForActualQueuedWork)
{
	for (const bool runtime_queue : {false, true})
	{
		CompactionHistoryFixture fixture;
		if (runtime_queue)
		{
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = fixture.Chat().id; session->provider_id = "opencode-cli";
			session->queued_prompt = "Real queued synthetic input";
			fixture.app.acp_sessions.push_back(std::move(session));
		}
		else
		{
			uam::AcpQueuedUserPromptState queued;
			queued.text = "Real queued synthetic input";
			fixture.Chat().acp_queued_prompts.push_back(std::move(queued));
		}
		for (int poll = 0; poll < 50; ++poll)
		{
			UAM_ASSERT(!fixture.service->Poll(fixture.app));
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT_EQ(fixture.queries.load(), 0);
		UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{0});
	}
}

namespace
{
	struct CompactionPageFixture
	{
		uam::OpenCodeCompactionHistoryService::Query query;
		std::vector<std::string> chunks{"[\"" + std::string(2045, 'x') + "\xc3\xa9", "\xf0\x9f\xa7\xaa" + std::string(2040, 'y') + "\"]"};
		std::size_t calls = 0;
		int mode = 0;
		std::stop_source cancellation;

		CompactionPageFixture()
		{
			query.sql = "SELECT fixture";
			query.argv = {"fixture-opencode", "db", query.sql, "--format", "json", "--pure"};
			query.workspace = "/fixture";
		}
		std::string Rows() const { return chunks[0] + chunks[1]; }
		ProcessExecutionResult Execute(const uam::OpenCodeCompactionHistoryService::Query& page, std::stop_token)
		{
			ProcessExecutionResult result;
			result.ok = true; result.exit_code = 0;
			++calls;
			if (calls == 1)
			{
				result.output = mode == 1 ? nlohmann::json::array({{{"sessionId", "ses_page"}, {"directory", "/fixture"}, {"rows", Rows().substr(0, 20)}}}).dump() : "[{\"sessionId\":\"ses_page\"";
				if (mode == 2) { result.ok = false; result.exit_code = 7; }
				if (mode == 3) result.output_truncated = true;
				return result;
			}
			UAM_ASSERT(page.sql.find("max(time_updated)") != std::string::npos);
			UAM_ASSERT(page.sql.find("sum(time_updated)") != std::string::npos);
			UAM_ASSERT(page.sql.find("count(*)") != std::string::npos);
			UAM_ASSERT(std::ranges::find(page.argv, page.sql) != page.argv.end());
			const std::size_t index = calls - 2;
			UAM_ASSERT(index < chunks.size());
			const std::size_t offset = index * 2048 + 1;
			UAM_ASSERT(page.sql.find("substr(h.rows," + std::to_string(offset) + ",2048)") != std::string::npos);
			nlohmann::json envelope = {{"sessionId", "ses_page"}, {"directory", "/fixture"}, {"total", 4091},
			    {"offset", offset}, {"chunk", chunks[index]}, {"revision", "[[9,100,900,\"msg_last\"],[12,100,1200,\"part_last\"]]"}};
			if (mode == 4 && index == 1) envelope["revision"] = "changed";
			if (mode == 5 && index == 1) envelope["sessionId"] = "ses_foreign";
			if (mode == 6 && index == 1) envelope["directory"] = "/foreign";
			if (mode == 7) envelope["total"] = 4 * 1024 * 1024 + 1;
			if (mode == 8) envelope["chunk"] = "partial";
			if (mode == 9) cancellation.request_stop();
			if (mode == 10) envelope["offset"] = 99;
			result.output = nlohmann::json::array({envelope}).dump();
			return result;
		}
		ProcessExecutionResult Collect()
		{
			return uam::OpenCodeCompactionHistoryService::CollectBoundedPages(query,
			    [&](const uam::OpenCodeCompactionHistoryService::Query& page, std::stop_token stop) { return Execute(page, stop); }, cancellation.get_token());
		}
	};
}

UAM_TEST(OpenCodeCompactionHistoryCollectsTruncatedSuccessfulUnicodeOutput)
{
	for (const bool truncated_inner : {false, true})
	{
		CompactionPageFixture fixture;
		fixture.mode = truncated_inner ? 1 : 0;
		const auto result = fixture.Collect();
		UAM_ASSERT(result.ok && result.exit_code == 0 && !result.output_truncated);
		UAM_ASSERT_EQ(fixture.calls, std::size_t{3});
		const auto envelope = nlohmann::json::parse(result.output);
		UAM_ASSERT_EQ(envelope.front().at("rows").get<std::string>(), fixture.Rows());
		UAM_ASSERT(nlohmann::json::parse(envelope.front().at("rows").get<std::string>()).is_array());
	}
}

UAM_TEST(OpenCodeCompactionHistoryCollectorPreservesCompleteDirectOutput)
{
	CompactionPageFixture fixture;
	const std::string direct = nlohmann::json::array({{{"sessionId", "ses_page"}, {"directory", "/fixture"}, {"rows", fixture.Rows()}}}).dump();
	std::size_t calls = 0;
	const auto result = uam::OpenCodeCompactionHistoryService::CollectBoundedPages(fixture.query,
	    [&](const uam::OpenCodeCompactionHistoryService::Query&, std::stop_token)
	{
		++calls;
		ProcessExecutionResult output; output.ok = true; output.exit_code = 0; output.output = direct;
		return output;
	});
	UAM_ASSERT(result.ok);
	UAM_ASSERT_EQ(result.output, direct);
	UAM_ASSERT_EQ(calls, std::size_t{1});
}

UAM_TEST(OpenCodeCompactionHistoryCollectorRejectsChangedPagesAndLimits)
{
	for (const int mode : {4, 5, 6, 7, 8, 10})
	{
		CompactionPageFixture fixture;
		fixture.mode = mode;
		const auto result = fixture.Collect();
		UAM_ASSERT(!result.ok && !result.error.empty());
		UAM_ASSERT(result.output.empty());
		UAM_ASSERT(fixture.calls <= 3);
	}
}

UAM_TEST(OpenCodeCompactionHistoryCollectorPreservesFailuresAndCancellation)
{
	for (const int mode : {2, 3, 9})
	{
		CompactionPageFixture fixture;
		fixture.mode = mode;
		const auto result = fixture.Collect();
		if (mode == 2) UAM_ASSERT(!result.ok && result.exit_code == 7 && fixture.calls == 1);
		if (mode == 3) UAM_ASSERT(result.output_truncated && fixture.calls == 1);
		if (mode == 9) UAM_ASSERT(!result.ok && result.canceled && fixture.calls == 2);
	}
}

UAM_TEST(OpenCodeCompactionHistoryCollectorUsesSmallWindowsPipePages)
{
	CompactionPageFixture fixture;
	fixture.query.host = ExecutionHost{};
	fixture.query.host->platform = "windows";
	int calls = 0;
	const auto result = uam::OpenCodeCompactionHistoryService::CollectBoundedPages(fixture.query,
	    [&](const uam::OpenCodeCompactionHistoryService::Query& page, std::stop_token)
	{
		ProcessExecutionResult output; output.ok = true; output.exit_code = 0;
		if (++calls == 1) { output.output = "["; return output; }
		UAM_ASSERT(page.sql.find("substr(h.rows,1,256)") != std::string::npos);
		output.output = nlohmann::json::array({{{"sessionId", "ses_page"}, {"directory", "/fixture"}, {"total", 2},
		    {"offset", 1}, {"chunk", "[]"}, {"revision", "stable"}}}).dump();
		return output;
	});
	UAM_ASSERT(result.ok && calls == 2);
}

UAM_TEST(OpenCodeCompactionHistoryCollectorUsesBoundedLinuxPages)
{
	CompactionPageFixture fixture;
	fixture.query.host = ExecutionHost{};
	fixture.query.host->platform = "linux";
	const std::vector<std::string> chunks{"[\"" + std::string(8189, 'x') + "\xc3\xa9", "\xf0\x9f\xa7\xaa" + std::string(10, 'y') + "\"]"};
	std::size_t calls = 0;
	const auto result = uam::OpenCodeCompactionHistoryService::CollectBoundedPages(fixture.query,
	    [&](const uam::OpenCodeCompactionHistoryService::Query& page, std::stop_token)
	{
		ProcessExecutionResult output; output.ok = true; output.exit_code = 0;
		if (++calls == 1) { output.output = "["; return output; }
		const std::size_t index = calls - 2;
		UAM_ASSERT(index < chunks.size());
		const std::size_t offset = index * 8192 + 1;
		UAM_ASSERT(page.sql.find("substr(h.rows," + std::to_string(offset) + ",8192)") != std::string::npos);
		output.output = nlohmann::json::array({{{"sessionId", "ses_page"}, {"directory", "/fixture"}, {"total", 8205},
		    {"offset", offset}, {"chunk", chunks[index]}, {"revision", "stable"}}}).dump();
		return output;
	});
	UAM_ASSERT(result.ok && calls == 3);
	UAM_ASSERT_EQ(nlohmann::json::parse(result.output).front().at("rows").get<std::string>(), chunks[0] + chunks[1]);
}

UAM_TEST(OpenCodeCompactionHistoryRepairsCompleteTextOnlyTimeline)
{
	CompactionHistoryFixture fixture;
	nlohmann::json text_rows = nlohmann::json::array();
	for (const nlohmann::json& row : fixture.rows)
		if (row.at("type") == "text") text_rows.push_back(row);
	fixture.rows = std::move(text_rows);
	Message& assistant = fixture.Chat().messages[1];
	std::erase_if(assistant.blocks, [](const MessageBlock& block) { return block.type == "tool_call"; });
	assistant.tool_calls.clear();
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
	const Message before = assistant;
	UAM_ASSERT(fixture.Wait());
	UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{2});
	Message restored = fixture.Chat().messages[1];
	for (MessageBlock& block : restored.blocks)
		if (block.type == "context_compaction") { block.type = "assistant_text"; block.request_id_json.clear(); }
	UAM_ASSERT_EQ(restored, before);
}
