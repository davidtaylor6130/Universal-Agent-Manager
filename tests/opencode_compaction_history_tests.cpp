#include "test_harness.h"
#include "app/opencode_compaction_sync_service.h"
#include "app/opencode_compaction_history.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"

#include <atomic>
#include <thread>
#include <sqlite3.h>

using namespace uam_test;

UAM_TEST(OpenCodeCompactionProofAcceptsNativeCompactionAgentWithTextOnlyParent)
{
	for (int variant = 0; variant < 8; ++variant)
	{
		sqlite3* opened = nullptr;
		UAM_ASSERT_EQ(sqlite3_open(":memory:", &opened), SQLITE_OK);
		std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(opened, sqlite3_close);
		UAM_ASSERT_EQ(sqlite3_exec(database.get(),
		    "CREATE TABLE message(id TEXT PRIMARY KEY,session_id TEXT,data TEXT);"
		    "CREATE TABLE part(message_id TEXT,session_id TEXT,data TEXT);"
		    "INSERT INTO message VALUES('parent','owned','{\"role\":\"user\"}');"
		    "INSERT INTO part VALUES('parent','owned','{\"type\":\"text\",\"text\":\"Continue\"}');",
		    nullptr, nullptr, nullptr), SQLITE_OK);
		nlohmann::json info = {{"role", "assistant"}, {"summary", true}, {"agent", "compaction"},
		    {"mode", "compaction"}, {"parentID", "parent"}, {"finish", "stop"}};
		if (variant == 1) info["agent"] = "build";
		if (variant == 2) info["mode"] = "build";
		if (variant == 3) info["summary"] = false;
		if (variant == 4) UAM_ASSERT_EQ(sqlite3_exec(database.get(), "UPDATE message SET session_id='foreign' WHERE id='parent'", nullptr, nullptr, nullptr), SQLITE_OK);
		if (variant == 5) UAM_ASSERT_EQ(sqlite3_exec(database.get(), "UPDATE message SET data='{\"role\":\"assistant\"}' WHERE id='parent'", nullptr, nullptr, nullptr), SQLITE_OK);
		if (variant == 6)
		{
			info.erase("agent"); info.erase("mode");
			UAM_ASSERT_EQ(sqlite3_exec(database.get(), "INSERT INTO part VALUES('parent','owned','{\"type\":\"compaction\"}')", nullptr, nullptr, nullptr), SQLITE_OK);
		}
		if (variant == 7) info["parentID"] = "missing";
		sqlite3_stmt* opened_statement = nullptr;
		UAM_ASSERT_EQ(sqlite3_prepare_v2(database.get(), "INSERT INTO message VALUES('summary','owned',?)", -1, &opened_statement, nullptr), SQLITE_OK);
		std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> insert(opened_statement, sqlite3_finalize);
		const std::string metadata = info.dump();
		UAM_ASSERT_EQ(sqlite3_bind_text(insert.get(), 1, metadata.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
		UAM_ASSERT_EQ(sqlite3_step(insert.get()), SQLITE_DONE);
		const std::string query = "SELECT CASE WHEN " + uam::OpenCodeCompactionProofSql() + " THEN 1 ELSE 0 END FROM message m WHERE m.id='summary'";
		opened_statement = nullptr;
		UAM_ASSERT_EQ(sqlite3_prepare_v2(database.get(), query.c_str(), -1, &opened_statement, nullptr), SQLITE_OK);
		std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(opened_statement, sqlite3_finalize);
		UAM_ASSERT_EQ(sqlite3_step(statement.get()), SQLITE_ROW);
		UAM_ASSERT_EQ(sqlite3_column_int(statement.get(), 0), variant == 0 || variant == 6 ? 1 : 0);
	}
}

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

UAM_TEST(OpenCodeCompactionHistoryIsolatesUnprovedSegmentsAndRejectsForeignMetadata)
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
		const bool independent_segment = mode == 0 || mode == 1 || mode == 2 || mode == 6;
		if (independent_segment)
		{
			UAM_ASSERT(fixture.Wait());
			UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[1], before[1].blocks[1]);
			UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[5].type, std::string("context_compaction"));
			UAM_ASSERT_EQ(fixture.Chat().messages[1].content, before[1].content);
			UAM_ASSERT_EQ(fixture.Chat().messages[1].tool_calls, before[1].tool_calls);
		}
		else
		{
			UAM_ASSERT(!fixture.Wait(false));
			UAM_ASSERT_EQ(fixture.Chat().messages, before);
		}
		UAM_ASSERT_EQ(fixture.Boundaries(), independent_segment ? std::size_t{1} : std::size_t{0});
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

UAM_TEST(OpenCodeCompactionHistoryRepairsEmptyHeldQueueWithoutResumingIt)
{
	for (const bool retained_runtime : {false, true})
	{
		CompactionHistoryFixture fixture;
		fixture.Chat().acp_queue_held = true;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
		if (retained_runtime)
		{
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = fixture.Chat().id; session->provider_id = "opencode-cli";
			session->session_id = fixture.Chat().native_session_id;
			session->queue_held = true;
			session->turn_assistant_message_index = session->current_assistant_message_index = 1;
			uam::acp_detail::RestoreTurnEventsFromMessageBlocks(*session, fixture.Chat().messages[1]);
			fixture.app.acp_sessions.push_back(std::move(session));
		}
		const ChatSession before = fixture.Chat();
		UAM_ASSERT(fixture.Wait());
		UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{2});
		UAM_ASSERT(fixture.Chat().acp_queue_held);
		UAM_ASSERT(fixture.Chat().acp_queued_prompts.empty());
		UAM_ASSERT_EQ(fixture.Chat().acp_dispatched_queued_prompt_count, std::size_t{0});
		UAM_ASSERT_EQ(fixture.Chat().messages[1].content, before.messages[1].content);
		UAM_ASSERT_EQ(fixture.Chat().messages[1].tool_calls, before.messages[1].tool_calls);
		if (retained_runtime)
		{
			UAM_ASSERT(fixture.app.acp_sessions.front()->queue_held);
			UAM_ASSERT(fixture.app.acp_sessions.front()->queued_user_prompts.empty());
			UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->turn_events[1].type, std::string("context_compaction"));
		}
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
		UAM_ASSERT(saved && saved->acp_queue_held && saved->acp_queued_prompts.empty() && saved->messages == fixture.Chat().messages);
		for (int poll = 0; poll < 50; ++poll) UAM_ASSERT(!fixture.service->Poll(fixture.app));
		UAM_ASSERT_EQ(fixture.queries.load(), 1);
	}
}

UAM_TEST(OpenCodeCompactionHistoryWaitsForActualQueuedWork)
{
	for (const bool runtime_queue : {false, true})
	{
		CompactionHistoryFixture fixture;
		fixture.Chat().acp_queue_held = true;
		if (runtime_queue)
		{
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = fixture.Chat().id; session->provider_id = "opencode-cli";
			session->queued_prompt = "Real queued synthetic input";
			session->queue_held = true;
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

UAM_TEST(OpenCodeCompactionHistoryKeepsPartialTailWhileRepairingProvenSegments)
{
	CompactionHistoryFixture fixture(32);
	Message& assistant = fixture.Chat().messages[1];
	for (std::size_t index = 0; index < 24; ++index)
	{
		MessageBlock& block = assistant.blocks[1 + index * 4];
		block.type = "context_compaction";
		block.request_id_json = "msg_summary_" + std::to_string(index);
	}
	// The final native compaction failed, and the interrupted ACP transcript retained only its prefix.
	fixture.rows[1 + 31 * 3]["text"] = std::string(16800, 'x');
	fixture.rows[1 + 31 * 3]["finish"] = nullptr;
	fixture.rows[1 + 31 * 3]["errorType"] = "object";
	fixture.rows[1 + 31 * 3]["nativeTail"] = 1;
	fixture.rows[1 + 31 * 3]["turnEnded"] = 1;
	assistant.blocks[1 + 31 * 4].text = std::string(16638, 'x');
	assistant.blocks.erase(assistant.blocks.begin() + (2 + 31 * 4), assistant.blocks.end());
	assistant.tool_calls.pop_back();
	fixture.rows.erase(fixture.rows.begin() + (2 + 31 * 3), fixture.rows.end());
	assistant.interrupted = true;
	fixture.Chat().last_stop_reason = "timeout";
	assistant.content.clear();
	for (const MessageBlock& block : assistant.blocks)
		if (block.type == "assistant_text" || block.type == "context_compaction") assistant.content += block.text;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
	const Message before = assistant;
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = fixture.Chat().id; session->provider_id = "opencode-cli";
	session->session_id = fixture.Chat().native_session_id;
	session->turn_assistant_message_index = session->current_assistant_message_index = 1;
	uam::acp_detail::RestoreTurnEventsFromMessageBlocks(*session, assistant);
	session->tool_call_message_indices["earlier_tool"] = 0;
	fixture.app.acp_sessions.push_back(std::move(session));
	const auto owners = fixture.app.acp_sessions.front()->tool_call_message_indices;
	UAM_ASSERT(fixture.Wait());
	UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{31});
	for (std::size_t index = 0; index < 24; ++index)
		UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[1 + index * 4], before.blocks[1 + index * 4]);
	for (std::size_t index = 24; index < 31; ++index)
	{
		const MessageBlock& block = fixture.Chat().messages[1].blocks[1 + index * 4];
		UAM_ASSERT_EQ(block.type, std::string("context_compaction"));
		UAM_ASSERT_EQ(block.text, before.blocks[1 + index * 4].text);
		UAM_ASSERT_EQ(block.request_id_json, "msg_summary_" + std::to_string(index));
	}
	MessageBlock interrupted = before.blocks[1 + 31 * 4];
	interrupted.type = "context_compaction_interrupted";
	interrupted.request_id_json = "msg_summary_31";
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[1 + 31 * 4], interrupted);
	UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->turn_events[1 + 31 * 4].type, interrupted.type);
	UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->turn_events[1 + 31 * 4].text, interrupted.text);
	UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->turn_events[1 + 31 * 4].request_id_json, interrupted.request_id_json);
	UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->tool_call_message_indices, owners);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].content, before.content);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].tool_calls, before.tool_calls);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(saved && saved->messages == fixture.Chat().messages);
	for (int poll = 0; poll < 100; ++poll) (void)fixture.service->Poll(fixture.app);
	UAM_ASSERT_EQ(fixture.queries.load(), 1);
}

UAM_TEST(OpenCodeCompactionHistoryRejectsUnprovedInterruptedPrefixes)
{
	for (int invalid = 0; invalid < 9; ++invalid)
	{
		CompactionHistoryFixture fixture;
		Message& assistant = fixture.Chat().messages[1];
		fixture.rows[4]["text"] = "Exact native failed summary with undelivered suffix.";
		fixture.rows[4]["finish"] = nullptr;
		fixture.rows[4]["errorType"] = "object";
		fixture.rows[4]["nativeTail"] = 1;
		fixture.rows[4]["turnEnded"] = 1;
		assistant.blocks[5].text = "Exact native failed summary";
		assistant.blocks.resize(6);
		assistant.tool_calls.pop_back();
		fixture.rows.erase(fixture.rows.begin() + 5, fixture.rows.end());
		assistant.interrupted = true;
		fixture.Chat().last_stop_reason = "timeout";
		if (invalid == 0) assistant.blocks[5].text = "Different saved prefix";
		if (invalid == 1) fixture.rows[3]["toolId"] = "foreign_tool_anchor";
		if (invalid == 2)
		{
			assistant.blocks.push_back({"assistant_text", "Following output", "", ""});
			fixture.rows.push_back({{"messageId", "msg_after_failed"}, {"partId", "part_after_failed"},
			    {"type", "text"}, {"text", "Following output"}, {"summary", 0}, {"parentProof", 0}, {"finish", "stop"}, {"errorType", nullptr}});
		}
		if (invalid == 3) fixture.rows[4]["errorType"] = nullptr;
		if (invalid == 4) fixture.rows[4]["parentProof"] = 0;
		if (invalid == 5) assistant.interrupted = false;
		if (invalid == 6) fixture.rows[4]["nativeTail"] = 0;
		if (invalid == 7) fixture.rows[4]["turnEnded"] = 0;
		if (invalid == 8) { fixture.rows[4].erase("nativeTail"); fixture.rows[4].erase("turnEnded"); }
		assistant.content.clear();
		for (const MessageBlock& block : assistant.blocks)
			if (block.type == "assistant_text") assistant.content += block.text;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
		const Message before = assistant;
		if (invalid == 1) UAM_ASSERT(!fixture.Wait(false));
		else UAM_ASSERT(fixture.Wait());
		UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[5], before.blocks[5]);
		UAM_ASSERT_EQ(fixture.Chat().messages[1].content, before.content);
		UAM_ASSERT_EQ(fixture.Chat().messages[1].tool_calls, before.tool_calls);
		UAM_ASSERT_EQ(fixture.Boundaries(), invalid == 1 ? std::size_t{0} : std::size_t{1});
		UAM_ASSERT(std::ranges::none_of(fixture.Chat().messages[1].blocks,
		    [](const MessageBlock& block) { return block.type == "context_compaction_interrupted"; }));
	}
}

UAM_TEST(OpenCodeCompactionHistoryCommitsOnlyFullyMatchedToolSegments)
{
	CompactionHistoryFixture fixture;
	fixture.rows[2]["text"] = "Unmatched ordinary text after the first summary.";
	const Message before = fixture.Chat().messages[1];
	UAM_ASSERT(fixture.Wait());
	UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[1], before.blocks[1]);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[3], before.blocks[3]);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[5].type, std::string("context_compaction"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].content, before.content);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].tool_calls, before.tool_calls);
}

UAM_TEST(OpenCodeCompactionHistoryRepairsInterruptedTurnBeforeLaterUserTurn)
{
	for (const bool already_typed : {false, true})
	{
		CompactionHistoryFixture fixture;
		Message& assistant = fixture.Chat().messages[1];
		fixture.rows[4]["text"] = "Exact failed summary with an undelivered suffix.";
		fixture.rows[4]["finish"] = nullptr;
		fixture.rows[4]["errorType"] = "object";
		fixture.rows[4]["nativeTail"] = 1;
		fixture.rows[4]["turnEnded"] = 1;
		fixture.rows.erase(fixture.rows.begin() + 5, fixture.rows.end());
		assistant.blocks[5].text = "Exact failed summary";
		assistant.blocks.resize(6);
		assistant.tool_calls.pop_back();
		assistant.interrupted = true;
		if (already_typed)
		{
			assistant.blocks[5].type = "context_compaction_interrupted";
			assistant.blocks[5].request_id_json = "acp-compaction:msg_summary_1";
		}
		assistant.content.clear();
		for (const MessageBlock& block : assistant.blocks)
			if (block.type == "assistant_text" || block.type == "context_compaction_interrupted") assistant.content += block.text;
		const Message before = assistant;
		Message user;
		user.role = MessageRole::User; user.content = "A later independent request";
		fixture.Chat().messages.push_back(std::move(user));
		Message later;
		later.role = MessageRole::Assistant; later.content = "A later ordinary answer";
		later.blocks.push_back({"assistant_text", later.content, "", ""});
		fixture.Chat().messages.push_back(later);
		fixture.rows.push_back({{"messageId", "msg_later_reply"}, {"partId", "part_later_reply"},
		    {"type", "text"}, {"text", later.content}, {"summary", 0}, {"parentProof", 0}, {"finish", "stop"}, {"errorType", nullptr}});
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
		UAM_ASSERT(fixture.Wait());
		UAM_ASSERT_EQ(fixture.Boundaries(), std::size_t{1});
		const MessageBlock& repaired = fixture.Chat().messages[1].blocks[5];
		UAM_ASSERT_EQ(repaired.type, std::string("context_compaction_interrupted"));
		UAM_ASSERT_EQ(repaired.text, before.blocks[5].text);
		UAM_ASSERT_EQ(repaired.request_id_json, already_typed ? before.blocks[5].request_id_json : std::string("msg_summary_1"));
		UAM_ASSERT_EQ(fixture.Chat().messages[1].content, before.content);
		UAM_ASSERT_EQ(fixture.Chat().messages[1].tool_calls, before.tool_calls);
		UAM_ASSERT_EQ(fixture.Chat().messages[3], later);
	}
}

UAM_TEST(OpenCodeCompactionHistoryUsesVerifiedCheckpointWithRealNativeSqliteData)
{
	for (int variant = 0; variant < 9; ++variant)
	{
		CompactionHistoryFixture fixture(32);
		fixture.service->Stop();
		Message& assistant = fixture.Chat().messages[1];
		for (int index = 0; index < 32; ++index)
		{
			MessageBlock& block = assistant.blocks[1 + index * 4];
			block.text = std::string(index == 31 ? 7107 : 14000, static_cast<char>('a' + index % 26));
			fixture.rows[1 + index * 3]["text"] = block.text;
			if (index < 31)
			{
				block.type = "context_compaction";
				block.request_id_json = "msg_summary_" + std::to_string(index);
			}
		}
		if (variant == 1) fixture.rows[91]["text"] = "Changed native checkpoint body";
		if (variant == 2 || variant == 4)
		{
			const int source = variant == 2 ? 91 : 1;
			nlohmann::json duplicate = fixture.rows[source];
			duplicate["partId"] = duplicate["partId"].get<std::string>() + "_duplicate";
			fixture.rows.insert(fixture.rows.begin() + source + 1, duplicate);
		}
		if (variant == 3)
		{
			assistant.blocks[1].type = "assistant_text";
			assistant.blocks[1].request_id_json.clear();
		}
		if (variant == 5) fixture.rows[90]["toolId"] = "foreign_previous_tool";
		if (variant == 6) assistant.blocks[121].type = "context_compaction_interrupted";
		if (variant == 8)
		{
			// The checkpoint's independently verified preceding tool still anchors a following interrupted prefix.
			assistant.blocks.erase(assistant.blocks.begin() + 122, assistant.blocks.begin() + 125);
			assistant.blocks.resize(123);
			assistant.blocks.back().text.resize(7000);
			assistant.interrupted = true;
			fixture.rows.erase(fixture.rows.begin() + 92, fixture.rows.begin() + 94);
			fixture.rows.erase(fixture.rows.begin() + 93, fixture.rows.end());
			fixture.rows.back()["finish"] = nullptr;
			fixture.rows.back()["errorType"] = "object";
		}
		assistant.content.clear();
		for (const MessageBlock& block : assistant.blocks)
			if (block.type == "assistant_text" || block.type == "context_compaction" || block.type == "context_compaction_interrupted")
				assistant.content += block.text;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
		const std::vector<Message> before = fixture.Chat().messages;
		sqlite3* opened = nullptr;
		UAM_ASSERT_EQ(sqlite3_open(":memory:", &opened), SQLITE_OK);
		std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(opened, sqlite3_close);
		UAM_ASSERT_EQ(sqlite3_exec(database.get(),
		    "CREATE TABLE session(id TEXT PRIMARY KEY,directory TEXT);"
		    "CREATE TABLE message(id TEXT PRIMARY KEY,session_id TEXT,data TEXT,time_created INTEGER,time_updated INTEGER);"
		    "CREATE TABLE part(id TEXT PRIMARY KEY,message_id TEXT,session_id TEXT,data TEXT,time_updated INTEGER);",
		    nullptr, nullptr, nullptr), SQLITE_OK);
		const auto insert = [&](const std::string& sql, const std::vector<std::string>& values)
		{
			sqlite3_stmt* raw = nullptr;
			UAM_ASSERT_EQ(sqlite3_prepare_v2(database.get(), sql.c_str(), -1, &raw, nullptr), SQLITE_OK);
			std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
			for (std::size_t index = 0; index < values.size(); ++index)
				UAM_ASSERT_EQ(sqlite3_bind_text(statement.get(), static_cast<int>(index + 1), values[index].c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
			UAM_ASSERT_EQ(sqlite3_step(statement.get()), SQLITE_DONE);
		};
		insert("INSERT INTO session VALUES(?,?)", {"ses_history", uam::paths::Utf8PathString(fixture.temp.root)});
		for (std::size_t index = 0; index < fixture.rows.size(); ++index)
		{
			const nlohmann::json& row = fixture.rows[index];
			const std::string message = row.at("messageId").get<std::string>();
			nlohmann::json info = {{"role", "assistant"}, {"summary", row.at("summary") == 1}, {"finish", "stop"}};
			if (row.at("summary") == 1)
			{
				const std::string parent = "parent_" + message;
				info["parentID"] = parent; info["agent"] = "compaction"; info["mode"] = "compaction";
				if (variant == 7 && message == "msg_summary_30") info["agent"] = "build";
				if (row.at("finish").is_null()) { info["finish"] = nullptr; info["error"] = nlohmann::json::object(); }
				insert("INSERT OR IGNORE INTO message VALUES(?,'ses_history',?,0,1)", {parent, nlohmann::json({{"role", "user"}}).dump()});
				insert("INSERT OR IGNORE INTO part VALUES(?,?,'ses_history',?,1)",
				    {"part_" + parent, parent, nlohmann::json({{"type", "text"}, {"text", "Continue"}}).dump()});
			}
			insert("INSERT OR IGNORE INTO message VALUES(?,'ses_history',?,?,1)", {message, info.dump(), std::to_string(index + 1)});
			nlohmann::json part = {{"type", row.at("type")}};
			if (row.at("type") == "text") part["text"] = row.at("text");
			else part["callID"] = row.at("toolId");
			insert("INSERT INTO part VALUES(?,?,'ses_history',?,1)", {row.at("partId").get<std::string>(), message, part.dump()});
		}
		std::atomic<int> completed{0}, proof{-1}, returned_rows{0};
		std::atomic<std::size_t> returned_bytes{0};
		uam::OpenCodeCompactionHistoryService service([&](const uam::OpenCodeCompactionHistoryService::Query& query, std::stop_token)
		{
			sqlite3_stmt* raw = nullptr;
			UAM_ASSERT_EQ(sqlite3_prepare_v2(database.get(), query.sql.c_str(), -1, &raw, nullptr), SQLITE_OK);
			std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
			nlohmann::json output = nlohmann::json::array();
			int status = SQLITE_OK;
			while ((status = sqlite3_step(statement.get())) == SQLITE_ROW)
			{
				nlohmann::json envelope = nlohmann::json::object();
				for (int column = 0; column < sqlite3_column_count(statement.get()); ++column)
				{
					const std::string key = sqlite3_column_name(statement.get(), column);
					if (sqlite3_column_type(statement.get(), column) == SQLITE_INTEGER) envelope[key] = sqlite3_column_int64(statement.get(), column);
					else envelope[key] = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), column));
				}
				proof = envelope.at("checkpointProof").get<int>();
				returned_rows = static_cast<int>(nlohmann::json::parse(envelope.at("rows").get<std::string>()).size());
				output.push_back(std::move(envelope));
			}
			UAM_ASSERT_EQ(status, SQLITE_DONE);
			ProcessExecutionResult result;
			result.ok = true; result.exit_code = 0; result.output = output.dump();
			returned_bytes = result.output.size(); ++completed;
			return result;
		});
		bool changed = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline && !changed)
		{
			changed = service.Poll(fixture.app);
			if (completed.load() > 0 && variant == 5) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		service.Stop();
		UAM_ASSERT_EQ(completed.load(), 1);
		const bool verified = variant == 0 || variant == 6 || variant == 8;
		UAM_ASSERT_EQ(proof.load(), verified ? 1 : 0);
		if (verified)
		{
			UAM_ASSERT(returned_rows.load() < 10);
			UAM_ASSERT(returned_bytes.load() < 25000);
		}
		else UAM_ASSERT(returned_bytes.load() > 400000);
		if (variant == 5) UAM_ASSERT(!changed);
		else UAM_ASSERT(changed);
		const Message& after = fixture.Chat().messages[1];
		UAM_ASSERT_EQ(after.content, before[1].content);
		UAM_ASSERT_EQ(after.tool_calls, before[1].tool_calls);
		UAM_ASSERT_EQ(fixture.Chat().messages.front(), before.front());
		const std::size_t newest = variant == 8 ? 122 : 125;
		UAM_ASSERT_EQ(after.blocks[newest].type, variant == 5 ? std::string("assistant_text") :
		    variant == 8 ? std::string("context_compaction_interrupted") : std::string("context_compaction"));
		for (std::size_t block = 0; block < before[1].blocks.size(); ++block)
			if (block != newest && !(variant == 3 && block == 1)) UAM_ASSERT_EQ(after.blocks[block], before[1].blocks[block]);
		if (variant == 3) UAM_ASSERT_EQ(after.blocks[1].type, std::string("context_compaction"));
	}
}
