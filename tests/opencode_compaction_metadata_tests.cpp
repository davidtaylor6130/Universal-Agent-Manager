#include "common/config/build_features.h"
// These fixtures drive the OpenCode runtime by its real id, so they need OpenCode compiled in.
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
#include "test_harness.h"
#include "app/opencode_compaction_sync_service.h"
#include "app/agent_definition_service.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/chat/chat_message_page_index.h"
#include "common/utils/hash_utils.h"

#include <sqlite3.h>
#include <algorithm>
#include <mutex>

using namespace uam_test;

namespace
{
	struct NativeMetadataFixture
	{
		TempDir temp{"uam-opencode-native-metadata"};
		uam::AppState app;
		sqlite3* database = nullptr;
		std::mutex database_mutex;
		std::atomic<int> queries{0};
		std::atomic<int> completed_queries{0};
		std::atomic<int> fail_queries{0};
		bool ready_observed = false;
		std::atomic<bool> allow_summary_text{false};
		uam::AcpQueuedUserPromptState captured_agent;
		std::unique_ptr<uam::OpenCodeCompactionSyncService> service;

		NativeMetadataFixture()
		{
			UAM_ASSERT_EQ(sqlite3_open(":memory:", &database), SQLITE_OK);
			const char* schema = "CREATE TABLE session(id TEXT PRIMARY KEY,directory TEXT);"
			    "CREATE TABLE message(id TEXT PRIMARY KEY,session_id TEXT,time_created INTEGER,data TEXT,time_updated INTEGER DEFAULT 0);"
			    "CREATE INDEX message_session_time_created_id_idx ON message(session_id,time_created,id);"
			    "CREATE TABLE part(id TEXT PRIMARY KEY,message_id TEXT,session_id TEXT,data TEXT,time_updated INTEGER DEFAULT 0);"
			    "CREATE INDEX part_message_id_id_idx ON part(message_id,id);";
			UAM_ASSERT_EQ(sqlite3_exec(database, schema, nullptr, nullptr, nullptr), SQLITE_OK);
			app.data_root = temp.root;
			app.settings.git_enabled = app.settings.git_auto_worktree = false;
			app.settings.central_provider_configuration.enabled = false;
			app.provider_profiles.push_back(ProviderProfileStore::DefaultOpenCodeProfile());
			ChatSession chat;
			chat.id = "native-metadata-owner"; chat.created_at = "2026-10-15T00:00:00Z";
			chat.provider_id = "opencode-cli"; chat.execution_host_id = "local";
			chat.native_session_id = "ses_owned";
			chat.approval_mode.clear(); chat.memory_enabled = false;
			chat.workspace_directory = uam::paths::Utf8PathString(temp.root);
			Message user;
			user.role = MessageRole::User; user.content = "Private fixture input";
			user.created_at = "2026-10-15T00:00:00Z";
			chat.messages.push_back(std::move(user));
			Message assistant;
			assistant.role = MessageRole::Assistant; assistant.content = "Preserve authored answer";
			assistant.created_at = "2026-10-15T00:00:01Z";
			assistant.blocks.push_back({"assistant_text", assistant.content, "", ""});
			chat.messages.push_back(std::move(assistant));
			app.chats.push_back(std::move(chat));
			std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = app.chats.front().id; session->provider_id = "opencode-cli";
			session->session_id = "ses_owned"; session->agent_version = "1.18.35";
			session->last_process_id = "private-unlaunched-process";
			session->process_execution_host_id = "local";
			session->running = session->session_ready = session->processing = true;
			session->turn_serial = 1; session->queued_prompt = "Private fixture input";
			session->turn_assistant_message_index = 1;
			app.acp_sessions.push_back(std::move(session));
			sqlite3_stmt* insert = nullptr;
			UAM_ASSERT_EQ(sqlite3_prepare_v2(database, "INSERT INTO session VALUES ('ses_owned',?)", -1, &insert, nullptr), SQLITE_OK);
			const std::string workspace = app.chats.front().workspace_directory;
			UAM_ASSERT_EQ(sqlite3_bind_text(insert, 1, workspace.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
			UAM_ASSERT_EQ(sqlite3_step(insert), SQLITE_DONE);
			sqlite3_finalize(insert);
			service = std::make_unique<uam::OpenCodeCompactionSyncService>([this](const uam::OpenCodeCompactionSyncService::Query& query, std::stop_token stop)
			{
				UAM_ASSERT(!stop.stop_requested());
				UAM_ASSERT(allow_summary_text.load() || query.sql.find("$.text") == std::string::npos);
				UAM_ASSERT(query.sql.find("SELECT *") == std::string::npos);
				UAM_ASSERT(std::ranges::find(query.argv, query.sql) != query.argv.end());
				UAM_ASSERT(std::ranges::find(query.argv, "db") != query.argv.end());
				++queries;
				ProcessExecutionResult result;
				if (fail_queries.load() > 0) { --fail_queries; result.error = "Private transient metadata failure"; }
				else result = Execute(query.sql);
				completed_queries.fetch_add(1, std::memory_order_release);
				return result;
			});
		}

		~NativeMetadataFixture()
		{
			service.reset();
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(Session(), true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(Session());
			sqlite3_close(database);
		}
		uam::AcpSessionState& Session() { return *app.acp_sessions.front(); }
		ChatSession& Chat() { return app.chats.front(); }

		ProcessExecutionResult Execute(const std::string& sql)
		{
			std::lock_guard<std::mutex> lock(database_mutex);
			ProcessExecutionResult result;
			sqlite3_stmt* raw = nullptr;
			if (sqlite3_prepare_v2(database, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
			{ result.error = sqlite3_errmsg(database); return result; }
			std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
			nlohmann::json rows = nlohmann::json::array();
			int status = SQLITE_OK;
			while ((status = sqlite3_step(raw)) == SQLITE_ROW)
			{
				nlohmann::json row = nlohmann::json::object();
				for (int column = 0; column < sqlite3_column_count(raw); ++column)
				{
					const char* name = sqlite3_column_name(raw, column);
					if (sqlite3_column_type(raw, column) == SQLITE_NULL) row[name] = nullptr;
					else if (sqlite3_column_type(raw, column) == SQLITE_INTEGER) row[name] = sqlite3_column_int64(raw, column);
					else row[name] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(raw, column)));
				}
				rows.push_back(std::move(row));
			}
			result.ok = status == SQLITE_DONE; result.exit_code = result.ok ? 0 : 1;
			result.output = rows.dump();
			if (!result.ok) result.error = sqlite3_errmsg(database);
			return result;
		}

		void NativeMessage(std::string id, std::int64_t time, nlohmann::json metadata, std::string session = "ses_owned")
		{
			std::lock_guard<std::mutex> lock(database_mutex);
			sqlite3_stmt* statement = nullptr;
			UAM_ASSERT_EQ(sqlite3_prepare_v2(database, "INSERT OR REPLACE INTO message(id,session_id,time_created,data) VALUES (?,?,?,?)", -1, &statement, nullptr), SQLITE_OK);
			const std::string json = metadata.dump();
			sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(statement, 2, session.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_int64(statement, 3, time);
			sqlite3_bind_text(statement, 4, json.c_str(), -1, SQLITE_TRANSIENT);
			UAM_ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
			sqlite3_finalize(statement);
		}

		void Parent(std::string id = "usr_parent", std::int64_t time = 1, std::string session = "ses_owned")
		{
			NativeMessage(id, time, {{"role", "user"}, {"summary", {{"title", "Native user metadata"}}}}, session);
			std::lock_guard<std::mutex> lock(database_mutex);
			sqlite3_stmt* statement = nullptr;
			UAM_ASSERT_EQ(sqlite3_prepare_v2(database, "INSERT OR REPLACE INTO part(id,message_id,session_id,data) VALUES (?,?,?,?)", -1, &statement, nullptr), SQLITE_OK);
			const std::string part_id = "part_" + id;
			const std::string data = nlohmann::json{{"type", "compaction"}, {"text", std::string(1024 * 1024, 'x')}}.dump();
			sqlite3_bind_text(statement, 1, part_id.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(statement, 2, id.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(statement, 3, session.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(statement, 4, data.c_str(), -1, SQLITE_TRANSIENT);
			UAM_ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
			sqlite3_finalize(statement);
		}

		void Summary(std::string id, std::int64_t time, std::string finish = "stop", bool error = false, std::string parent = "usr_parent")
		{
			nlohmann::json metadata = {{"role", "assistant"}, {"summary", true}, {"parentID", parent}};
			if (!finish.empty()) metadata["finish"] = finish;
			if (error) metadata["error"] = {{"name", "PrivateFixtureError"}};
			NativeMessage(std::move(id), time, std::move(metadata));
		}

		void Tool(const std::string& id, std::int64_t time, const std::string& call)
		{
			NativeMessage(id, time, {{"role", "assistant"}});
			std::lock_guard<std::mutex> lock(database_mutex);
			sqlite3_stmt* statement = nullptr;
			UAM_ASSERT_EQ(sqlite3_prepare_v2(database, "INSERT OR REPLACE INTO part(id,message_id,session_id,data) VALUES (?,?,?,?)", -1, &statement, nullptr), SQLITE_OK);
			const std::string part = "tool_" + id;
			const std::string data = nlohmann::json{{"type", "tool"}, {"callID", call}}.dump();
			sqlite3_bind_text(statement, 1, part.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(statement, 2, id.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(statement, 3, "ses_owned", -1, SQLITE_STATIC);
			sqlite3_bind_text(statement, 4, data.c_str(), -1, SQLITE_TRANSIENT);
			UAM_ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
			sqlite3_finalize(statement);
		}

		void SummaryText(const std::string& id, const std::string& text, const std::string& part_id = {})
		{
			allow_summary_text = true;
			std::lock_guard<std::mutex> lock(database_mutex);
			sqlite3_stmt* statement = nullptr;
			UAM_ASSERT_EQ(sqlite3_prepare_v2(database, "INSERT OR REPLACE INTO part(id,message_id,session_id,data) VALUES (?,?,?,?)", -1, &statement, nullptr), SQLITE_OK);
			const std::string part = part_id.empty() ? "text_" + id : part_id;
			const std::string data = nlohmann::json{{"type", "text"}, {"text", text}}.dump();
			sqlite3_bind_text(statement, 1, part.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(statement, 2, id.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(statement, 3, "ses_owned", -1, SQLITE_STATIC);
			sqlite3_bind_text(statement, 4, data.c_str(), -1, SQLITE_TRANSIENT);
			UAM_ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
			sqlite3_finalize(statement);
		}

		void StartStreaming()
		{
			// The preloaded ACP answer belongs to this turn and must have an ordinary native projection.
			NativeMessage("zz_stream_prefix", 1, {{"role", "assistant"}});
			SummaryText("zz_stream_prefix", Chat().messages[1].content);
			Session().queued_prompt.clear();
			Session().prompt_request_id = 99;
			Session().current_assistant_message_index = Session().turn_assistant_message_index = 1;
			uam::acp_detail::RestoreTurnEventsFromMessageBlocks(Session(), Chat().messages[1]);
		}

		void RestoreActiveRemotePrompt()
		{
			ExecutionHost host;
			host.id = "compaction-recovery-host"; host.transport = "ssh"; host.ssh_alias = "fixture";
#if defined(_WIN32)
			host.platform = "windows";
#else
			host.platform = "linux";
#endif
			host.runner_status = "ready"; host.runner_version = "development"; host.runner_protocol_version = 3;
			app.settings.execution_hosts.push_back(host);
			Chat().execution_host_id = Session().process_execution_host_id = host.id;
			Chat().remote_turn_reconnect_pending = true;
			Chat().remote_turn_serial = 7;
			Chat().remote_turn_user_message_index = 0;
			Chat().remote_pending_requests.push_back({.request_id = 73, .method = "session/prompt", .turn_serial = 7});
			Session().turn_serial = 0; Session().prompt_request_id = 0; Session().queued_prompt.clear();
			Session().recovering_remote_turn = true; Session().remote_runner_protocol_version = 3;
			Session().agent_version = "1.17.20";
			uam::acp_detail::RestoreRemoteAcpRequests(Session(), Chat());
			uam::acp_detail::RestoreRemoteAcpTranscript(Session(), Chat());
		}

		bool AwaitBoundary(const std::string& id, std::chrono::milliseconds timeout = std::chrono::seconds(4))
		{
			const auto deadline = std::chrono::steady_clock::now() + timeout;
			while (std::chrono::steady_clock::now() < deadline)
			{
				(void)service->Poll(app);
				const auto boundaries = Boundaries();
				if (std::ranges::find(boundaries, "acp-compaction:" + id) != boundaries.end()) return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return false;
		}

		uam::OpenCodeCompactionGateState WaitGate()
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline)
			{
				std::string error;
				const uam::OpenCodeCompactionGateState state = service->BeforeDispatch(app, Session(), Chat(), error);
				if (state != uam::OpenCodeCompactionGateState::Pending)
				{
					if (state == uam::OpenCodeCompactionGateState::Failed) throw std::runtime_error(error);
					return state;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			throw std::runtime_error("Private metadata worker did not settle.");
		}

		void Settle(bool handoff = false)
		{
			Session().queued_prompt.clear();
			Session().processing = false; Session().prompt_request_id = 0;
			Session().last_settled_turn_serial = Session().turn_serial;
			Session().last_turn_outcome = "completed";
			if (handoff)
			{
				service->ObserveBeforeTurnReset(app, Session(), Chat());
				uam::acp_detail::ResetAcpTurnStreamState(Session());
				++Session().turn_serial;
				Session().processing = true;
				Session().queued_prompt = "Next staged input";
				Session().turn_assistant_message_index = Session().current_assistant_message_index = -1;
				Session().turn_first_user_message_index = Session().turn_user_message_index = static_cast<int>(Chat().messages.size());
				Message next;
				next.role = MessageRole::User; next.content = "Next staged input"; next.acp_prompt_not_sent = true;
				next.created_at = "2026-10-15T00:00:02Z";
				Chat().messages.push_back(std::move(next));
			}
		}

		void BindPublicJourney()
		{
#if defined(_WIN32)
			const std::vector<std::string> sink = {"cmd", "/C", "more > NUL"};
#else
			const std::vector<std::string> sink = {"/bin/cat"};
#endif
			std::string error;
			Session().running = Session().session_ready = false;
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(Session(), temp.root, sink, &error));
			Session().next_request_id = 17;
			Session().protocol_kind = "opencode-acp";
			Session().initialized = true;
			uam::SelectedPrimaryAgentSnapshot snapshot;
			UAM_ASSERT(uam::AgentDefinitionService::CaptureSelectedPrimary(app.data_root, temp.root, "build", snapshot, error));
			const uam::AgentDefinition& agent = snapshot.definition;
			captured_agent.uam_agent_id = Session().active_uam_agent_id = agent.id;
			captured_agent.uam_agent_definition_hash = Session().active_uam_agent_definition_hash = agent.definition_hash;
			captured_agent.uam_agent_definition_snapshot = Session().active_uam_agent_definition_snapshot = agent.markdown_snapshot;
			captured_agent.uam_agent_instructions = Session().active_uam_agent_instructions = agent.instructions;
			captured_agent.uam_agent_skills = Session().active_uam_agent_skills = agent.skills;
			captured_agent.uam_agent_delegates = Session().active_uam_agent_delegates = agent.delegates;
			captured_agent.uam_agent_workspace_access = Session().active_uam_agent_workspace_access = agent.workspace_access;
			captured_agent.uam_agent_execution_capability = Session().active_uam_agent_execution_capability =
			    uam::AgentDefinitionService::ExecutionCapabilityForProvider(Chat().provider_id);
			Session().running = Session().session_ready = true;
			app.opencode_compaction_before_turn_reset = [this](uam::AcpSessionState& session, const ChatSession& chat)
			{ service->ObserveBeforeTurnReset(app, session, chat); };
			app.opencode_compaction_before_dispatch = [this](uam::AcpSessionState& session, ChatSession& chat, std::string& error)
			{
				const uam::OpenCodeCompactionGateState state = service->BeforeDispatch(app, session, chat, error);
				if (state == uam::OpenCodeCompactionGateState::Ready)
				{
					ready_observed = true;
					return uam::OpenCodeCompactionGateState::Pending; // Stop at the real pre-write boundary, never send a provider prompt.
				}
				return state;
			};
		}

		void AwaitSave()
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (app.pending_chat_save_at_by_chat_id.contains(Chat().id) && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::FlushPendingChatSaves(app);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(Chat().id));
		}

		void AwaitPublicFailure(const std::string& cause = "metadata")
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while ((Session().last_turn_outcome != "failed" || Session().last_settled_turn_serial != Session().turn_serial) && std::chrono::steady_clock::now() < deadline)
			{
				AwaitSave();
				(void)uam::acp_detail::SendQueuedPromptIfReady(app, Session(), Chat());
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT_EQ(Session().last_turn_outcome, std::string("failed"));
			UAM_ASSERT_EQ(Session().last_settled_turn_serial, Session().turn_serial);
			UAM_ASSERT_EQ(Session().next_request_id, 17);
			UAM_ASSERT_EQ(Session().prompt_request_id, 0);
			if (Session().last_error.find(cause) == std::string::npos)
				throw std::runtime_error("Unexpected public compaction failure: " + Session().last_error);
			AwaitSave();
		}

		void AwaitPublicReady()
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!ready_observed && std::chrono::steady_clock::now() < deadline)
			{
				AwaitSave();
				(void)uam::acp_detail::ResumeQueuedUserPromptsAfterSessionSetup(app, Session(), Chat());
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (!ready_observed) throw std::runtime_error("Public compaction recovery did not become ready: " + Session().last_error +
			    " lifecycle=" + Session().lifecycle_state + " held=" + std::to_string(Session().queue_held) + " queries=" + std::to_string(queries.load()));
			UAM_ASSERT_EQ(Session().prompt_request_id, 0);
			UAM_ASSERT_EQ(Session().next_request_id, 17);
		}

		void RetryFailed(int index)
		{
			std::string error;
			if (!uam::RetryFailedAcpMessage(app, Chat().id, index, &error))
				throw std::runtime_error("Public RetryFailedAcpMessage rejected: " + error);
		}

		std::vector<std::string> Boundaries() const
		{
			std::vector<std::string> ids;
			for (const ::Message& message : app.chats.front().messages)
				for (const MessageBlock& block : message.blocks)
					if (block.type == "context_compaction") ids.push_back(block.request_id_json);
			return ids;
		}
	};
}

UAM_TEST(OpenCodeMetadataPublishesOnlyNewCompletionDuringUnsentTurnHandoff)
{
	NativeMetadataFixture fixture;
	fixture.Parent();
	fixture.Summary("cmp_historical", 2);
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	fixture.Summary("cmp_new", 3);
	fixture.Settle(true);
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_new"});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].content, std::string("Preserve authored answer"));
	UAM_ASSERT_EQ(fixture.Chat().messages.back().content, std::string("Next staged input"));
	UAM_ASSERT(fixture.Chat().messages.back().acp_prompt_not_sent);
	UAM_ASSERT(fixture.Session().turn_events.empty());
	UAM_ASSERT(fixture.Session().processing);
	UAM_ASSERT(!fixture.Session().context_compaction_in_progress);
}

UAM_TEST(OpenCodeMetadataPagesBeyond128WithoutLoadingTranscriptParts)
{
	NativeMetadataFixture fixture;
	fixture.Parent();
	fixture.WaitGate();
	for (int index = 0; index < 260; ++index)
		fixture.NativeMessage("msg_" + std::to_string(index), index + 2, {{"role", "assistant"}});
	fixture.Summary("cmp_last", 300);
	fixture.Settle(true);
	fixture.WaitGate();
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_last"});
	UAM_ASSERT_EQ(fixture.queries.load(), 4);
}

UAM_TEST(OpenCodeMetadataDoesNotReturnUserSummaryBodies)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate();
	fixture.NativeMessage("usr_summary", 2, {{"role", "user"}, {"summary", {{"body", std::string(1024 * 1024, 'x')}}}});
	fixture.Summary("cmp_new", 3);
	fixture.Settle(true); fixture.WaitGate();
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_new"});
}

UAM_TEST(OpenCodeMetadataIdlePollDoesNotScheduleMoreQueries)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate();
	const int baseline_queries = fixture.queries.load();
	for (int tick = 0; tick < 200; ++tick) UAM_ASSERT(!fixture.service->Poll(fixture.app));
	UAM_ASSERT_EQ(fixture.queries.load(), baseline_queries);
	fixture.Summary("cmp_done", 2); fixture.Settle(true); fixture.WaitGate();
	const int settled_queries = fixture.queries.load();
	for (int tick = 0; tick < 200; ++tick) UAM_ASSERT(!fixture.service->Poll(fixture.app));
	UAM_ASSERT_EQ(fixture.queries.load(), settled_queries);
}

UAM_TEST(OpenCodeMetadataNeverAttributesHistoricalPendingToTheNewTurn)
{
	NativeMetadataFixture fixture;
	fixture.Parent();
	fixture.Parent("usr_foreign", 1, "ses_child");
	fixture.Summary("cmp_pending", 2, "");
	fixture.WaitGate();
	fixture.Summary("cmp_pending", 2);
	fixture.Summary("cmp_failed", 3, "error", true);
	fixture.Summary("cmp_foreign", 4, "stop", false, "usr_foreign");
	fixture.Summary("cmp_foreign_pending", 5, "", false, "usr_foreign");
	fixture.Summary("cmp_new", 6);
	fixture.Settle(true);
	fixture.WaitGate();
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_new"});
	UAM_ASSERT_EQ(fixture.queries.load(), 2);
}

UAM_TEST(OpenCodeMetadataPreservesImportedBoundarySummaryAndNativeIdentity)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate();
	fixture.Summary("cmp_imported", 2);
	fixture.Chat().messages[1].blocks.push_back({"context_compaction", "Original imported summary", "", "cmp_imported"});
	fixture.Settle(true); fixture.WaitGate();
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"cmp_imported"});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().text, std::string("Original imported summary"));
}

UAM_TEST(OpenCodeMetadataDiscardsChangedCapturedAssistantOrProcess)
{
	for (int change = 0; change < 4; ++change)
	{
		NativeMetadataFixture fixture;
		fixture.Parent(); fixture.WaitGate(); fixture.Summary("cmp_stale", 2);
		fixture.Settle(true);
		if (change == 0) fixture.Chat().messages[1].content = "Changed content only";
		if (change == 1) fixture.Chat().messages[1].blocks.front().text = "Changed block only";
		if (change == 2) fixture.Session().last_process_id = "replacement-process";
		if (change == 3) fixture.Chat().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		fixture.WaitGate();
		UAM_ASSERT(fixture.Boundaries().empty());
	}
}

UAM_TEST(OpenCodeMetadataUnsupportedVersionPreservesNormalChatCompatibility)
{
	NativeMetadataFixture fixture;
	std::string error;
	for (const std::string& version : {std::string("1.18.34"), std::string(), std::string("unknown")})
	{
		fixture.Session().agent_version = version;
		UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.Chat(), error) == uam::OpenCodeCompactionGateState::NotApplicable);
		UAM_ASSERT(error.empty());
	}
	UAM_ASSERT_EQ(fixture.queries.load(), 0);
	UAM_ASSERT(fixture.Session().processing);
	UAM_ASSERT_EQ(fixture.Session().queued_prompt, std::string("Private fixture input"));
}

UAM_TEST(OpenCodeMetadataRequiresVerifiedSchemaForSupportedVersion)
{
	NativeMetadataFixture fixture;
	UAM_ASSERT_EQ(sqlite3_exec(fixture.database, "DROP INDEX part_message_id_id_idx", nullptr, nullptr, nullptr), SQLITE_OK);
	bool failed = false;
	try { fixture.WaitGate(); }
	catch (const std::runtime_error& failure) { failed = std::string(failure.what()).find("schema") != std::string::npos; }
	UAM_ASSERT(failed);
}

UAM_TEST(OpenCodeMetadataProtocol2IsExplicitlyUnavailableWithoutBlockingChat)
{
	NativeMetadataFixture fixture;
	ExecutionHost host;
	host.id = "private-host"; host.transport = "ssh"; host.platform = "linux";
	host.runner_protocol_version = 2;
	fixture.app.settings.execution_hosts.push_back(host);
	fixture.Chat().execution_host_id = host.id;
	fixture.Session().process_execution_host_id = host.id;
	std::string error;
	UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.Chat(), error) == uam::OpenCodeCompactionGateState::NotApplicable);
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(fixture.queries.load(), 0);
	UAM_ASSERT(fixture.Session().processing);
}

UAM_TEST(OpenCodeMetadataCapsOwnedPendingRowsWithoutAttributingThemToNextTurn)
{
	NativeMetadataFixture fixture;
	fixture.Parent();
	fixture.WaitGate();
	for (int index = 0; index < 65; ++index) fixture.Summary("pending_" + std::to_string(index), index + 2, "");
	fixture.Settle(true);
	bool failed = false;
	try { fixture.WaitGate(); }
	catch (const std::runtime_error& failure) { failed = std::string(failure.what()).find("limit") != std::string::npos; }
	UAM_ASSERT(failed);
	UAM_ASSERT(fixture.Boundaries().empty());
	UAM_ASSERT_EQ(fixture.queries.load(), 2);
}

UAM_TEST(OpenCodeMetadataKeepsLateNativeCompletionWithItsOriginalUnsentHandoff)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate();
	fixture.Summary("cmp_late", 2, "");
	fixture.Settle(true);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (fixture.completed_queries.load(std::memory_order_acquire) < 2 && std::chrono::steady_clock::now() < deadline)
	{
		fixture.service->Poll(fixture.app);
		std::this_thread::yield();
	}
	UAM_ASSERT_EQ(fixture.completed_queries.load(std::memory_order_acquire), 2);
	// The private query has captured its pending row before the native completion is written.
	fixture.service->Poll(fixture.app);
	UAM_ASSERT(fixture.Boundaries().empty());
	std::string error;
	UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.Chat(), error) == uam::OpenCodeCompactionGateState::Pending);
	fixture.Summary("cmp_late", 2);
	fixture.WaitGate();
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_late"});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().request_id_json, std::string("acp-compaction:cmp_late"));
	UAM_ASSERT(fixture.Chat().messages.back().blocks.empty());
	UAM_ASSERT(fixture.Session().turn_events.empty());
}

UAM_TEST(OpenCodeMetadataUnresolvedPendingHasBoundedQueriesAndClearDispatchFailure)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.Summary("cmp_unresolved", 2, "");
	fixture.Settle(true);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
	uam::OpenCodeCompactionGateState state = uam::OpenCodeCompactionGateState::Pending;
	std::string error;
	while (state == uam::OpenCodeCompactionGateState::Pending && std::chrono::steady_clock::now() < deadline)
	{
		state = fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.Chat(), error);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(state == uam::OpenCodeCompactionGateState::Failed);
	UAM_ASSERT(error.find("remained pending") != std::string::npos);
	UAM_ASSERT(fixture.queries.load() <= 12);
	UAM_ASSERT(fixture.Boundaries().empty());
	UAM_ASSERT(fixture.Chat().messages.back().acp_prompt_not_sent);
}

UAM_TEST(OpenCodeMetadataStopCancelsAndJoinsItsSingleWorker)
{
	NativeMetadataFixture fixture;
	std::atomic<bool> entered{false}, exited{false};
	fixture.service = std::make_unique<uam::OpenCodeCompactionSyncService>([&](const uam::OpenCodeCompactionSyncService::Query&, std::stop_token stop)
	{
		entered.store(true, std::memory_order_release);
		while (!stop.stop_requested()) std::this_thread::yield();
		exited.store(true, std::memory_order_release);
		return ProcessExecutionResult{.canceled = true, .error = "Private fixture cancellation."};
	});
	std::string error;
	UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.Chat(), error) == uam::OpenCodeCompactionGateState::Pending);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	UAM_ASSERT(entered.load(std::memory_order_acquire));
	fixture.service->Stop();
	UAM_ASSERT(exited.load(std::memory_order_acquire));
	UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.Chat(), error) == uam::OpenCodeCompactionGateState::Failed);
	UAM_ASSERT(fixture.Boundaries().empty());
}

UAM_TEST(OpenCodeMetadataOutputAndTimeoutFailuresHaveExplicitCauses)
{
	for (const bool oversized : {false, true})
	{
		NativeMetadataFixture fixture;
		std::atomic<int> attempts{0};
		fixture.service = std::make_unique<uam::OpenCodeCompactionSyncService>([oversized, &attempts](const uam::OpenCodeCompactionSyncService::Query&, std::stop_token)
		{
			++attempts;
			if (oversized) return ProcessExecutionResult{.ok = true, .output = std::string(512 * 1024 + 1, 'x')};
			return ProcessExecutionResult{.timed_out = true};
		});
		bool failed = false;
		try { fixture.WaitGate(); }
		catch (const std::runtime_error& failure)
		{ failed = std::string(failure.what()).find(oversized ? "512 KiB" : "timed out") != std::string::npos; }
		UAM_ASSERT(failed);
		UAM_ASSERT_EQ(attempts.load(), oversized ? 1 : 3);
		UAM_ASSERT(fixture.Boundaries().empty());
	}
}

UAM_TEST(OpenCodeMetadataPublicRetryRecoversBaselineFailureWithoutAutomaticReplay)
{
	NativeMetadataFixture fixture;
	fixture.Chat().messages.resize(1);
	fixture.Chat().messages.front().acp_prompt_not_sent = true;
	fixture.Session().turn_first_user_message_index = fixture.Session().turn_user_message_index = 0;
	fixture.Session().turn_assistant_message_index = -1;
	fixture.BindPublicJourney();
	fixture.fail_queries = 1;
	fixture.AwaitPublicFailure();
	UAM_ASSERT(!fixture.Session().queue_held && !fixture.Chat().acp_queue_held);
	UAM_ASSERT(fixture.Chat().messages.front().acp_prompt_not_sent);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(saved && saved->messages.front().acp_prompt_not_sent);
	for (int index = 0; index < 100; ++index) (void)fixture.service->Poll(fixture.app);
	UAM_ASSERT_EQ(fixture.queries.load(), 1);
	fixture.RetryFailed(0);
	fixture.AwaitPublicReady();
	UAM_ASSERT_EQ(fixture.queries.load(), 2);
	UAM_ASSERT_EQ(fixture.Chat().messages.size(), std::size_t{1});
	UAM_ASSERT(fixture.Chat().messages.front().acp_prompt_not_sent);
	UAM_ASSERT(fixture.Boundaries().empty());
}

UAM_TEST(OpenCodeMetadataPublicRetryRetainsOriginalSettlementOwnerAcrossFailedAttempts)
{
	NativeMetadataFixture fixture;
	fixture.BindPublicJourney();
	fixture.Parent();
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	fixture.Summary("cmp_owned_before_retry", 2);
	fixture.fail_queries = 2;
	fixture.Settle(true);
	fixture.AwaitPublicFailure();
	const Message original = fixture.Chat().messages[1];
	fixture.RetryFailed(2);
	fixture.AwaitPublicFailure();
	UAM_ASSERT(fixture.Boundaries().empty());
	fixture.RetryFailed(2);
	fixture.AwaitPublicReady();
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_owned_before_retry"});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].content, original.content);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.front().text, original.blocks.front().text);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("context_compaction"));
	UAM_ASSERT(fixture.Chat().messages.back().blocks.empty());
	UAM_ASSERT(fixture.Chat().messages.back().acp_prompt_not_sent);
	UAM_ASSERT(fixture.Session().turn_events.empty());
}

UAM_TEST(OpenCodeMetadataPublicRetryDoesNotTrustMalformedSchema)
{
	NativeMetadataFixture fixture;
	fixture.Chat().messages.resize(1);
	fixture.Chat().messages.front().acp_prompt_not_sent = true;
	fixture.Session().turn_first_user_message_index = fixture.Session().turn_user_message_index = 0;
	fixture.Session().turn_assistant_message_index = -1;
	fixture.BindPublicJourney();
	UAM_ASSERT_EQ(sqlite3_exec(fixture.database, "DROP INDEX part_message_id_id_idx", nullptr, nullptr, nullptr), SQLITE_OK);
	fixture.AwaitPublicFailure();
	std::string error;
	fixture.RetryFailed(0);
	fixture.AwaitPublicFailure();
	UAM_ASSERT_EQ(fixture.queries.load(), 2);
	UAM_ASSERT(!fixture.ready_observed && fixture.Chat().messages.front().acp_prompt_not_sent);
	fixture.Session().agent_version.clear();
	error.clear();
	UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.Chat(), error) == uam::OpenCodeCompactionGateState::NotApplicable);
	UAM_ASSERT(error.empty());
}

UAM_TEST(OpenCodeMetadataFailureLeavesUnsentDeleteUsable)
{
	NativeMetadataFixture fixture;
	fixture.Chat().messages.resize(1);
	fixture.Chat().messages.front().acp_prompt_not_sent = true;
	fixture.Session().turn_first_user_message_index = fixture.Session().turn_user_message_index = 0;
	fixture.Session().turn_assistant_message_index = -1;
	fixture.BindPublicJourney();
	fixture.fail_queries = 1;
	fixture.AwaitPublicFailure();
	std::uint64_t digest = uam::hashing::kFnv1a64OffsetBasis;
	for (const Message& message : fixture.Chat().messages) uam::chat_pages::AppendMessageDigest(digest, message);
	std::string error;
	UAM_ASSERT(uam::DiscardUnsentAcpMessage(fixture.app, fixture.Chat().id, 0,
	    uam::chat_pages::FinishMessageDigest(digest, fixture.Chat().messages.size()), &error));
	UAM_ASSERT(fixture.Chat().messages.front().acp_prompt_discarded);
	UAM_ASSERT_EQ(fixture.queries.load(), 1);
}

UAM_TEST(OpenCodeMetadataFailurePreservesFutureQueueResumeAndDelete)
{
	for (const bool resume : {false, true})
	{
		NativeMetadataFixture fixture;
		fixture.Chat().messages.resize(1);
		fixture.Chat().messages.front().acp_prompt_not_sent = true;
		fixture.Session().turn_first_user_message_index = fixture.Session().turn_user_message_index = 0;
		fixture.Session().turn_assistant_message_index = -1;
		fixture.BindPublicJourney();
		uam::AcpQueuedUserPromptState future = fixture.captured_agent;
		future.id = "owned-future-input"; future.revision = 7;
		future.text = "Future captured input";
		future.markdown_store_files = {"private-future-skill.md"};
		future.markdown_store_prompt_blocks = {"Captured future skill"};
		fixture.Session().queued_user_prompts.push_back(future);
		fixture.Chat().acp_queued_prompts = {future};
		fixture.fail_queries = 1;
		fixture.AwaitPublicFailure();
		UAM_ASSERT(fixture.Session().queue_held && fixture.Chat().acp_queue_held);
		UAM_ASSERT(fixture.Session().queued_user_prompts.front() == future);
		UAM_ASSERT(fixture.Chat().acp_queued_prompts.front() == future);
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
		UAM_ASSERT(saved && saved->acp_queue_held && saved->acp_queued_prompts.front() == future);
		std::string error;
		if (resume)
		{
			UAM_ASSERT(uam::ResumeQueuedAcpPrompts(fixture.app, fixture.Chat().id, {{future.id, future.revision}}, &error));
			fixture.AwaitPublicReady();
			UAM_ASSERT(fixture.Chat().messages.front().acp_prompt_not_sent);
			UAM_ASSERT_EQ(fixture.Chat().messages.back().content, future.text);
			UAM_ASSERT_EQ(fixture.Chat().messages.back().markdown_store_prompt_blocks, future.markdown_store_prompt_blocks);
		}
		else
		{
			UAM_ASSERT(uam::RemoveQueuedAcpPrompt(fixture.app, fixture.Chat().id, 0, &error, std::make_pair(future.id, future.revision)));
			UAM_ASSERT(fixture.Session().queued_user_prompts.empty() && fixture.Chat().acp_queued_prompts.empty());
			UAM_ASSERT(!fixture.Session().queue_held && !fixture.Chat().acp_queue_held);
			UAM_ASSERT(fixture.Chat().messages.front().acp_prompt_not_sent);
			UAM_ASSERT_EQ(fixture.queries.load(), 1);
		}
	}
}


UAM_TEST(OpenCodeMetadataFailedNativeTurnCompletionCannotMoveToNextAssistant)
{
	NativeMetadataFixture fixture;
	fixture.Parent();
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	fixture.Summary("cmp_failed_turn", 2);
	uam::acp_detail::CompletePromptTurn(fixture.Session(), uam::acp_detail::kAcpLifecycleError);
	fixture.service->ObserveBeforeTurnReset(fixture.app, fixture.Session(), fixture.Chat());
	uam::acp_detail::ResetAcpTurnStreamState(fixture.Session());
	++fixture.Session().turn_serial;
	fixture.Session().processing = true;
	fixture.Session().queued_prompt = "Next native turn";
	fixture.Chat().messages.push_back({.role = MessageRole::User, .content = "Next native turn"});
	fixture.Chat().messages.push_back({.role = MessageRole::Assistant, .content = "New successful answer"});
	fixture.Session().turn_assistant_message_index = 3;
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	fixture.Summary("cmp_successful_turn", 3);
	fixture.Settle();
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_successful_turn"});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Chat().messages[3].blocks.back().request_id_json, std::string("acp-compaction:cmp_successful_turn"));
}

UAM_TEST(OpenCodeMetadataUnfinishedFailedNativeTurnDoesNotHoldLaterDispatch)
{
	NativeMetadataFixture fixture;
	fixture.Parent();
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	fixture.Summary("cmp_pending_failed_turn", 2, "");
	uam::acp_detail::CompletePromptTurn(fixture.Session(), uam::acp_detail::kAcpLifecycleError);
	fixture.service->ObserveBeforeTurnReset(fixture.app, fixture.Session(), fixture.Chat());
	uam::acp_detail::ResetAcpTurnStreamState(fixture.Session());
	++fixture.Session().turn_serial;
	fixture.Session().processing = true;
	fixture.Session().queued_prompt = "Next native turn";
	fixture.Chat().messages.push_back({.role = MessageRole::User, .content = "Next native turn"});
	fixture.Chat().messages.push_back({.role = MessageRole::Assistant, .content = "New successful answer"});
	fixture.Session().turn_assistant_message_index = 3;
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	fixture.Settle();
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	UAM_ASSERT(fixture.Boundaries().empty());
	UAM_ASSERT_EQ(fixture.queries.load(), 3); // Original baseline, replacement baseline, successful settlement.
}

UAM_TEST(OpenCodeMetadataMissingAssistantOwnerRetryTakesFreshBaseline)
{
	NativeMetadataFixture fixture;
	fixture.BindPublicJourney();
	fixture.Parent();
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	fixture.Summary("cmp_unowned_completed_turn", 2);
	fixture.Session().turn_assistant_message_index = -1;
	fixture.Settle(true);
	fixture.AwaitPublicFailure("no unchanged completed assistant owner");
	fixture.RetryFailed(2);
	fixture.AwaitPublicReady();
	UAM_ASSERT(fixture.Boundaries().empty());
	UAM_ASSERT_EQ(fixture.queries.load(), 2);
	UAM_ASSERT(fixture.Chat().messages.back().acp_prompt_not_sent);
}

UAM_TEST(OpenCodeMetadataAcceptsVerified11720Schema)
{
	NativeMetadataFixture fixture;
	fixture.Session().agent_version = "1.17.20";
	fixture.Parent();
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	UAM_ASSERT_EQ(fixture.queries.load(), 1);
}

UAM_TEST(OpenCodeMetadataClassifiesActiveSummaryInlineWithoutResettingOwners)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	const std::string summary = "Native completed summary.";
	fixture.NativeMessage("msg_inline_prefix", 2, {{"role", "assistant"}});
	fixture.SummaryText("msg_inline_prefix", " Before ");
	fixture.Summary("cmp_live_inline", 3);
	fixture.SummaryText("cmp_live_inline", summary);
	fixture.Session().tool_call_message_indices["retained-tool-owner"] = 1;
	const std::unordered_map<std::string, int> owners = fixture.Session().tool_call_message_indices;
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), " Before " + summary + " After");
	const std::string content = fixture.Chat().messages[1].content;
	UAM_ASSERT(fixture.AwaitBoundary("cmp_live_inline"));
	const std::vector<MessageBlock>& blocks = fixture.Chat().messages[1].blocks;
	UAM_ASSERT_EQ(blocks.size(), std::size_t{3});
	UAM_ASSERT_EQ(blocks[0].type, std::string("assistant_text"));
	UAM_ASSERT_EQ(blocks[0].text, std::string("Preserve authored answer Before "));
	UAM_ASSERT_EQ(blocks[1].type, std::string("context_compaction"));
	UAM_ASSERT_EQ(blocks[1].text, summary);
	UAM_ASSERT_EQ(blocks[2].text, std::string(" After"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].content, content);
	UAM_ASSERT_EQ(fixture.Session().tool_call_message_indices, owners);
	UAM_ASSERT(uam::acp_detail::MessageBlocksEqual(blocks, uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session())));
	UAM_ASSERT(fixture.Session().processing && fixture.Session().prompt_request_id == 99);
	const int queries = fixture.queries.load();
	for (int tick = 0; tick < 100; ++tick) (void)fixture.service->Poll(fixture.app);
	UAM_ASSERT(fixture.queries.load() <= queries + 1);
}

UAM_TEST(OpenCodeMetadataWaitsForBothNativeProofAndStreamedSummary)
{
	for (const bool native_first : {false, true})
	{
		NativeMetadataFixture fixture;
		fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
		const std::string summary = "A summary arriving on separate transports.";
		if (native_first)
		{
			fixture.Summary("cmp_arrival_order", 2);
			fixture.SummaryText("cmp_arrival_order", summary);
			UAM_ASSERT(!fixture.AwaitBoundary("cmp_arrival_order", std::chrono::milliseconds(2300)));
		}
		(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), summary);
		if (!native_first)
		{
			fixture.Summary("cmp_arrival_order", 2, "");
			fixture.SummaryText("cmp_arrival_order", summary);
			UAM_ASSERT(!fixture.AwaitBoundary("cmp_arrival_order", std::chrono::milliseconds(2300)));
			fixture.Summary("cmp_arrival_order", 2);
		}
		UAM_ASSERT(fixture.AwaitBoundary("cmp_arrival_order"));
		UAM_ASSERT_EQ(fixture.Boundaries().size(), std::size_t{1});
		UAM_ASSERT(uam::acp_detail::MessageBlocksEqual(fixture.Chat().messages[1].blocks,
		    uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session())));
	}
}

UAM_TEST(OpenCodeMetadataAdoptsAlreadyRunningCurrentTurnWithoutHistoricalWrites)
{
	NativeMetadataFixture fixture;
	fixture.Session().agent_version = "1.17.20";
	fixture.Chat().messages[0].created_at = "2026-10-15 19:06:39"; // Naive user timestamps are not UTC anchors.
	fixture.Chat().messages[1].created_at = "1970-01-01T00:00:00.001Z";
	fixture.Session().turn_first_user_message_index = fixture.Session().turn_user_message_index = 0;
	fixture.Parent();
	fixture.Summary("cmp_prior_native_turn", 0);
	fixture.SummaryText("cmp_prior_native_turn", "Prior native summary.");
	fixture.Tool("msg_adoption_anchor", 2, "chatcmpl-tool-adoption");
	fixture.Summary("cmp_adopted_live", 3);
	fixture.SummaryText("cmp_adopted_live", "Current owned summary.");
	fixture.StartStreaming();
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Prior native summary.");
	uam::acp_detail::AppendToolTurnEventIfNeeded(fixture.Session(), "chatcmpl-tool-adoption");
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Current owned summary.");
	const Message user = fixture.Chat().messages[0];
	UAM_ASSERT(fixture.AwaitBoundary("cmp_adopted_live"));
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_adopted_live"});
	UAM_ASSERT_EQ(fixture.Chat().messages[0], user);
	UAM_ASSERT(fixture.Session().processing && fixture.Session().prompt_request_id == 99);
}

UAM_TEST(OpenCodeMetadataRejectsAmbiguousLiveSummaryText)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	fixture.NativeMessage("msg_unproven_prefix", 2, {{"role", "assistant"}});
	fixture.SummaryText("msg_unproven_prefix", "Native prefix absent from the ACP projection.");
	fixture.Summary("cmp_ambiguous", 3);
	fixture.SummaryText("cmp_ambiguous", "Repeated exact summary.");
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Repeated exact summary.Repeated exact summary.");
	const Message before = fixture.Chat().messages[1];
	UAM_ASSERT(!fixture.AwaitBoundary("cmp_ambiguous", std::chrono::milliseconds(2300)));
	UAM_ASSERT_EQ(fixture.Chat().messages[1], before);
}

UAM_TEST(OpenCodeMetadataSeparatesRepeatedNativeSummariesByToolAnchor)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	const std::string summary = "Identical native compaction summary.";
	for (int index = 0; index < 2; ++index)
	{
		const std::string suffix = std::to_string(index);
		const std::string native = "cmp_anchored_" + suffix;
		const std::string tool = "chatcmpl-tool-anchor-" + suffix;
		fixture.NativeMessage("msg_tool_" + suffix, 2 + index * 2, {{"role", "assistant"}});
		{
			std::lock_guard<std::mutex> lock(fixture.database_mutex);
			const std::string sql = "INSERT INTO part(id,message_id,session_id,data) VALUES ('part_tool_" + suffix + "','msg_tool_" + suffix +
			    "','ses_owned','{\"type\":\"tool\",\"callID\":\"" + tool + "\"}')";
			UAM_ASSERT_EQ(sqlite3_exec(fixture.database, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
		}
		fixture.Summary(native, 3 + index * 2);
		fixture.SummaryText(native, summary);
		uam::acp_detail::AppendToolTurnEventIfNeeded(fixture.Session(), tool);
		(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), summary);
	}
	UAM_ASSERT(fixture.AwaitBoundary("cmp_anchored_0"));
	UAM_ASSERT(fixture.AwaitBoundary("cmp_anchored_1"));
	UAM_ASSERT_EQ(fixture.Boundaries().size(), std::size_t{2});
	UAM_ASSERT(uam::acp_detail::MessageBlocksEqual(fixture.Chat().messages[1].blocks,
	    uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session())));
}

UAM_TEST(OpenCodeMetadataPaginatesUnicodeSummaryAndReplacesMatchingEmptyMarker)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	std::string summary;
	for (int index = 0; index < 8300; ++index) summary += "é😀";
	fixture.Summary("cmp_unicode_live", 2);
	fixture.SummaryText("cmp_unicode_live", summary);
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), summary);
	fixture.Session().turn_events.push_back({.type = "context_compaction", .request_id_json = "cmp_unicode_live"});
	fixture.Session().turn_events.push_back({.type = "context_compaction", .request_id_json = "unrelated-boundary"});
	(void)uam::acp_detail::SyncCurrentAssistantMessageBlocksFromTurnEvents(fixture.Chat(), fixture.Session());
	UAM_ASSERT(fixture.AwaitBoundary("cmp_unicode_live"));
	UAM_ASSERT_EQ(fixture.Boundaries(), (std::vector<std::string>{"acp-compaction:cmp_unicode_live", "unrelated-boundary"}));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[1].text, summary);
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Ordinary following answer.");
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().text, std::string("Ordinary following answer."));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[1].text, summary);
}

UAM_TEST(OpenCodeMetadataDoesNotAdoptNaiveAssistantTimestamp)
{
	NativeMetadataFixture fixture;
	fixture.Chat().messages[1].created_at = "2026-10-15 19:06:39";
	fixture.Parent();
	fixture.Summary("cmp_without_utc_anchor", 2);
	fixture.SummaryText("cmp_without_utc_anchor", "Unscoped native summary.");
	fixture.StartStreaming();
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Unscoped native summary.");
	for (int tick = 0; tick < 100; ++tick) (void)fixture.service->Poll(fixture.app);
	UAM_ASSERT_EQ(fixture.queries.load(), 0);
	UAM_ASSERT(fixture.Boundaries().empty());
}

UAM_TEST(OpenCodeMetadataRetiresPublishedSummariesDuringLongActiveTurn)
{
	NativeMetadataFixture fixture;
	fixture.Chat().messages[1].created_at = "1970-01-01T00:00:00.001Z";
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	fixture.Session().tool_call_message_indices["retained-owner"] = 1;
	const std::unordered_map<std::string, int> owners = fixture.Session().tool_call_message_indices;
	std::vector<std::string> summaries;
	for (int index = 0; index < 66; ++index)
	{
		summaries.push_back("Exact native summary [" + std::to_string(index) + "].");
		(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), summaries.back());
	}
	const std::string content = fixture.Chat().messages[1].content;
	for (int batch = 0; batch < 3; ++batch)
	{
		for (int index = batch * 22; index < (batch + 1) * 22; ++index)
		{
			const std::string id = "cmp_long_active_" + std::to_string(index);
			fixture.Summary(id, index + 2);
			fixture.SummaryText(id, summaries[index]);
		}
		UAM_ASSERT(fixture.AwaitBoundary("cmp_long_active_" + std::to_string((batch + 1) * 22 - 1)));
		UAM_ASSERT_EQ(fixture.Boundaries().size(), static_cast<std::size_t>((batch + 1) * 22));
	}
	UAM_ASSERT_EQ(fixture.Chat().messages[1].content, content);
	UAM_ASSERT_EQ(fixture.Session().tool_call_message_indices, owners);
	UAM_ASSERT(uam::acp_detail::MessageBlocksEqual(fixture.Chat().messages[1].blocks,
	    uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session())));
	// A restarted reconciler must also skip the 66 persisted typed identities before its unresolved-work cap.
	fixture.service = std::make_unique<uam::OpenCodeCompactionSyncService>([&fixture](const uam::OpenCodeCompactionSyncService::Query& query, std::stop_token stop)
	{
		UAM_ASSERT(!stop.stop_requested());
		++fixture.queries;
		return fixture.Execute(query.sql);
	});
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), summaries.front());
	const int queries = fixture.queries.load();
	for (int tick = 0; tick < 100; ++tick) (void)fixture.service->Poll(fixture.app);
	UAM_ASSERT_EQ(fixture.Boundaries().size(), std::size_t{66});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("assistant_text"));
	UAM_ASSERT(fixture.queries.load() <= queries + 1);
	fixture.Settle();
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	UAM_ASSERT(fixture.queries.load() <= 80); // Three bounded metadata batches, 66 body fetches and settlement.
}

UAM_TEST(OpenCodeMetadataDoesNotRetypeEarlierOrdinaryOccurrenceBeforeSummaryArrives)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	const std::string summary = "Identical ordinary answer and native summary.";
	fixture.NativeMessage("msg_ordinary_before_summary", 2, {{"role", "assistant"}});
	fixture.SummaryText("msg_ordinary_before_summary", summary);
	fixture.Summary("cmp_after_ordinary_duplicate", 3);
	fixture.SummaryText("cmp_after_ordinary_duplicate", summary);
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), summary);
	UAM_ASSERT(!fixture.AwaitBoundary("cmp_after_ordinary_duplicate", std::chrono::milliseconds(2300)));
	UAM_ASSERT(fixture.Boundaries().empty());
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), summary);
	UAM_ASSERT(fixture.AwaitBoundary("cmp_after_ordinary_duplicate"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.front().text, std::string("Preserve authored answer") + summary);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("context_compaction"));
}

UAM_TEST(OpenCodeMetadataDefersOwnershipUntilFirstAssistantChunk)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate();
	fixture.Chat().messages.resize(1);
	fixture.Session().queued_prompt.clear();
	fixture.Session().prompt_request_id = 99;
	fixture.Session().turn_assistant_message_index = fixture.Session().current_assistant_message_index = -1;
	fixture.Session().turn_events.clear();
	for (int tick = 0; tick < 100; ++tick) (void)fixture.service->Poll(fixture.app);
	fixture.Summary("cmp_first_assistant_chunk", 2);
	fixture.SummaryText("cmp_first_assistant_chunk", "First native summary.");
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "First native summary.");
	UAM_ASSERT(fixture.AwaitBoundary("cmp_first_assistant_chunk"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.front().type, std::string("context_compaction"));
	UAM_ASSERT(fixture.Session().processing && fixture.Session().prompt_request_id == 99);
}

UAM_TEST(OpenCodeMetadataUsesNativeToolPartBoundaryWithinOneMessage)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	fixture.Tool("msg_part_order", 2, "chatcmpl-tool-part-order");
	fixture.SummaryText("msg_part_order", "Text before native tool.", "aa_before_tool");
	fixture.SummaryText("msg_part_order", "é😀Text after native tool.", "zz_after_tool");
	fixture.Summary("cmp_part_order", 3);
	fixture.SummaryText("cmp_part_order", "Part-anchored summary.");
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Text before native tool.");
	uam::acp_detail::AppendToolTurnEventIfNeeded(fixture.Session(), "chatcmpl-tool-part-order");
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "é😀Text after native tool.Part-anchored summary.");
	UAM_ASSERT(fixture.AwaitBoundary("cmp_part_order"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[2].text, std::string("é😀Text after native tool."));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[3].type, std::string("context_compaction"));
}

UAM_TEST(OpenCodeMetadataExcludesBaselineTupleFromLivePrefix)
{
	NativeMetadataFixture fixture;
	fixture.Parent();
	fixture.Tool("msg_old_tool", 1, "chatcmpl-tool-prior-turn");
	fixture.NativeMessage("msg_old_text", 1, {{"role", "assistant"}});
	fixture.SummaryText("msg_old_text", "Ordinary text before the baseline tuple.");
	fixture.WaitGate(); fixture.StartStreaming();
	fixture.Summary("cmp_after_same_time_baseline", 2);
	fixture.SummaryText("cmp_after_same_time_baseline", "Current turn summary.");
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Current turn summary.");
	UAM_ASSERT(fixture.AwaitBoundary("cmp_after_same_time_baseline"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.front().text, std::string("Preserve authored answer"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("context_compaction"));
}

UAM_TEST(OpenCodeMetadataStartsPendingGraceWhenActiveTurnSettles)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	fixture.Summary("cmp_settlement_grace", 2, "");
	fixture.SummaryText("cmp_settlement_grace", "Summary finishing just after the ACP turn.");
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Summary finishing just after the ACP turn.");
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (fixture.completed_queries.load(std::memory_order_acquire) < 2 && std::chrono::steady_clock::now() < deadline)
	{
		(void)fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT_EQ(fixture.completed_queries.load(), 2); // Baseline plus active page, before its pending probe.
	(void)fixture.service->Poll(fixture.app);
	fixture.Settle();
	std::string error;
	UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.Chat(), error) == uam::OpenCodeCompactionGateState::Pending);
	fixture.Summary("cmp_settlement_grace", 2);
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_settlement_grace"});
	UAM_ASSERT(fixture.queries.load() <= 8);
}

UAM_TEST(OpenCodeMetadataAdoptsDurablyRecoveredRemotePromptWithoutPromptId)
{
	NativeMetadataFixture fixture;
	fixture.Chat().messages[1].created_at = "1970-01-01T00:00:00.001Z";
	fixture.Parent();
	fixture.Tool("msg_recovery_tool", 2, "chatcmpl-tool-recovery");
	fixture.Summary("cmp_recovered_prompt", 3);
	fixture.SummaryText("cmp_recovered_prompt", "Recovered native summary.");
	fixture.Chat().messages[1].content += "Recovered native summary.";
	fixture.Chat().messages[1].blocks.push_back({"tool_call", "", "chatcmpl-tool-recovery", ""});
	fixture.Chat().messages[1].blocks.push_back({"assistant_text", "Recovered native summary.", "", ""});
	fixture.RestoreActiveRemotePrompt();
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 0);
	UAM_ASSERT_EQ(fixture.Session().pending_request_methods.at(73), std::string("session/prompt"));
	const std::vector<uam::AcpRemotePendingRequestState> requests = fixture.Chat().remote_pending_requests;
	const std::string content = fixture.Chat().messages[1].content;
	UAM_ASSERT(fixture.AwaitBoundary("cmp_recovered_prompt"));
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 0);
	UAM_ASSERT_EQ(fixture.Chat().remote_pending_requests, requests);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].content, content);
	UAM_ASSERT(fixture.Session().processing && fixture.Session().recovering_remote_turn);
	UAM_ASSERT(uam::acp_detail::MessageBlocksEqual(fixture.Chat().messages[1].blocks,
	    uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session())));
}

UAM_TEST(OpenCodeMetadataRejectsUnownedRecoveredPromptProof)
{
	for (int invalid = 0; invalid < 7; ++invalid)
	{
		NativeMetadataFixture fixture;
		fixture.Chat().messages[1].created_at = "1970-01-01T00:00:00.001Z";
		fixture.Parent();
		fixture.RestoreActiveRemotePrompt();
		if (invalid == 0) fixture.Chat().remote_pending_requests.front().turn_serial = 6;
		if (invalid == 1) fixture.Chat().remote_pending_requests.front().response_consumed = true;
		if (invalid == 2) fixture.Session().pending_request_methods[73] = "session/cancel";
		if (invalid == 3) fixture.Session().recovering_remote_turn = false;
		if (invalid == 4) fixture.Chat().remote_turn_reconnect_pending = false;
		if (invalid == 5) fixture.Chat().remote_pending_requests.front().method = "session/cancel";
		if (invalid == 6) fixture.Chat().remote_pending_requests.front().request_id = 0;
		const Message before = fixture.Chat().messages[1];
		for (int tick = 0; tick < 100; ++tick) (void)fixture.service->Poll(fixture.app);
		UAM_ASSERT_EQ(fixture.queries.load(), 0);
		UAM_ASSERT_EQ(fixture.Chat().messages[1], before);
		UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 0);
	}
}


UAM_TEST(OpenCodeMetadataRecognizes11720TextOnlyCompactionParent)
{
	NativeMetadataFixture fixture;
	fixture.Session().agent_version = "1.17.20";
	fixture.NativeMessage("usr_text_parent", 1, {{"role", "user"}});
	fixture.SummaryText("usr_text_parent", "Private fixture user input.");
	fixture.WaitGate(); fixture.StartStreaming();
	fixture.NativeMessage("cmp_automatic", 2, {{"role", "assistant"}, {"summary", true},
	    {"parentID", "usr_text_parent"}, {"agent", "compaction"}, {"mode", "compaction"}, {"finish", "stop"}});
	fixture.SummaryText("cmp_automatic", "Verified automatic summary.");
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Verified automatic summary.");
	UAM_ASSERT(fixture.AwaitBoundary("cmp_automatic"));
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_automatic"});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().text, std::string("Verified automatic summary."));
}

UAM_TEST(OpenCodeMetadataPendingNativeSummaryDrivesExistingCompactionState)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	fixture.Summary("cmp_pending_live", 2, "");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (!fixture.Session().context_compaction_in_progress && std::chrono::steady_clock::now() < deadline)
	{
		(void)fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(fixture.Session().context_compaction_in_progress);
	UAM_ASSERT(fixture.Boundaries().empty());
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 99);
	fixture.Summary("cmp_pending_live", 2);
	fixture.SummaryText("cmp_pending_live", "Owned pending summary.");
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Owned pending summary.");
	UAM_ASSERT(fixture.AwaitBoundary("cmp_pending_live"));
	UAM_ASSERT(!fixture.Session().context_compaction_in_progress);
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_pending_live"});
	UAM_ASSERT(std::ranges::none_of(fixture.Session().turn_events, [](const uam::AcpTurnEventState& event)
	{ return event.type == "context_compaction_pending"; }));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("context_compaction"));
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 99);
}

UAM_TEST(OpenCodeMetadataCompletedWithoutTextDoesNotClaimActiveCompaction)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	fixture.Summary("cmp_text_pending_only", 2);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (fixture.completed_queries.load() < 2 && std::chrono::steady_clock::now() < deadline)
	{
		(void)fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	(void)fixture.service->Poll(fixture.app);
	UAM_ASSERT_EQ(fixture.completed_queries.load(), 2);
	UAM_ASSERT(!fixture.Session().context_compaction_in_progress);
	UAM_ASSERT(fixture.Boundaries().empty());
}

UAM_TEST(OpenCodeMetadataNativePendingCleanupPreservesOtherAcpCompaction)
{
	for (const int outcome : {0, 1, 2, 3})
	{
		NativeMetadataFixture fixture;
		fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
		fixture.Summary("cmp_native_cleanup", 2, "");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
		while (!fixture.Session().context_compaction_in_progress && std::chrono::steady_clock::now() < deadline)
		{
			(void)fixture.service->Poll(fixture.app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT(fixture.Session().context_compaction_in_progress);
		fixture.Session().turn_events.push_back({.type = "context_compaction_pending", .request_id_json = "acp-compaction:independent-acp"});
		if (outcome == 0) fixture.Summary("cmp_native_cleanup", 2, "error", true);
		if (outcome == 1) fixture.Session().cancel_requested = true;
		if (outcome == 2) fixture.Settle();
		if (outcome == 3) fixture.Chat().native_session_id = "ses_replaced";
		const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
		while (std::ranges::any_of(fixture.Session().turn_events, [](const uam::AcpTurnEventState& event)
		{ return event.request_id_json == "acp-compaction:cmp_native_cleanup"; }) && std::chrono::steady_clock::now() < cleanup_deadline)
		{
			(void)fixture.service->Poll(fixture.app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT(std::ranges::none_of(fixture.Session().turn_events, [](const uam::AcpTurnEventState& event)
		{ return event.request_id_json == "acp-compaction:cmp_native_cleanup"; }));
		UAM_ASSERT(std::ranges::any_of(fixture.Session().turn_events, [](const uam::AcpTurnEventState& event)
		{ return event.request_id_json == "acp-compaction:independent-acp"; }));
		UAM_ASSERT(fixture.Session().context_compaction_in_progress == (outcome == 0 || outcome == 3));
		UAM_ASSERT(fixture.Boundaries().empty());
	}
}

UAM_TEST(OpenCodeMetadataTimeoutRetriesSameBaselineWithoutPromptDelivery)
{
	NativeMetadataFixture fixture;
	std::atomic<int> attempts{0};
	std::vector<std::string> sql;
	fixture.service = std::make_unique<uam::OpenCodeCompactionSyncService>([&](const uam::OpenCodeCompactionSyncService::Query& query, std::stop_token)
	{
		sql.push_back(query.sql);
		if (++attempts == 1) return ProcessExecutionResult{.timed_out = true, .error = "Remote command timed out."};
		return fixture.Execute(query.sql);
	});
	const int request = fixture.Session().prompt_request_id;
	const std::string staged = fixture.Session().queued_prompt;
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	UAM_ASSERT_EQ(attempts.load(), 2);
	UAM_ASSERT_EQ(sql[0], sql[1]);
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, request);
	UAM_ASSERT_EQ(fixture.Session().queued_prompt, staged);
	UAM_ASSERT_EQ(fixture.Chat().messages.size(), std::size_t{2});
	UAM_ASSERT(fixture.Boundaries().empty());
}

UAM_TEST(OpenCodeMetadataAutomaticShapeRejectsForeignParentAndPartialAgentProof)
{
	for (const int invalid : {0, 1, 2})
	{
		NativeMetadataFixture fixture;
		fixture.NativeMessage("usr_unproved_parent", 1, {{"role", "user"}}, invalid == 0 ? "ses_foreign" : "ses_owned");
		fixture.WaitGate(); fixture.StartStreaming();
		fixture.NativeMessage("cmp_unproved", 2, {{"role", "assistant"}, {"summary", true}, {"parentID", "usr_unproved_parent"},
		    {"agent", invalid == 1 ? "build" : "compaction"}, {"mode", invalid == 2 ? "build" : "compaction"}, {"finish", "stop"}});
		fixture.Settle();
		UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
		UAM_ASSERT(fixture.Boundaries().empty());
		UAM_ASSERT(!fixture.Session().context_compaction_in_progress);
	}
}


UAM_TEST(OpenCodeMetadataExactNativeIdentityClassifiesFirstAdoptedOutputBeforeLocalTimestamp)
{
	NativeMetadataFixture fixture;
	fixture.Session().agent_version = "1.17.20";
	fixture.NativeMessage("usr_automatic_parent", 1, {{"role", "user"}});
	fixture.NativeMessage("cmp_before_adoption", 2, {{"role", "assistant"}, {"summary", true}, {"parentID", "usr_automatic_parent"},
	    {"agent", "compaction"}, {"mode", "compaction"}, {"finish", "stop"}});
	fixture.SummaryText("cmp_before_adoption", "Native first summary.");
	fixture.Chat().messages[1].content.clear(); fixture.Chat().messages[1].blocks.clear();
	fixture.Session().queued_prompt.clear(); fixture.Session().prompt_request_id = 99;
	fixture.Session().current_assistant_message_index = fixture.Session().turn_assistant_message_index = 1;
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Native first ", "opencode-message:cmp_before_adoption");
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "summary.", "opencode-message:cmp_before_adoption");
	UAM_ASSERT(fixture.AwaitBoundary("cmp_before_adoption"));
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_before_adoption"});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[0].text, std::string("Native first summary."));
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 99);
}

UAM_TEST(OpenCodeMetadataExactIdentityRequiresCompleteBodyAndRejectsDifferentIdentity)
{
	for (const bool foreign : {false, true})
	{
		NativeMetadataFixture fixture;
		fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
		fixture.Summary("cmp_identified_complete", 2);
		fixture.SummaryText("cmp_identified_complete", "Complete native summary.");
		(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), foreign ? "Complete native summary." : "Complete native ",
		    foreign ? "opencode-message:other_native_message" : "opencode-message:cmp_identified_complete");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (fixture.completed_queries.load() < 3 && std::chrono::steady_clock::now() < deadline)
		{
			(void)fixture.service->Poll(fixture.app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		(void)fixture.service->Poll(fixture.app);
		UAM_ASSERT(fixture.Boundaries().empty());
		if (!foreign)
		{
			(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "summary.", "opencode-message:cmp_identified_complete");
			UAM_ASSERT(fixture.AwaitBoundary("cmp_identified_complete"));
			UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_identified_complete"});
		}
	}
}


UAM_TEST(OpenCodeMetadataPendingBeforeFirstChunkDoesNotManufactureAssistant)
{
	NativeMetadataFixture fixture;
	fixture.Chat().messages.resize(1);
	fixture.Session().turn_assistant_message_index = fixture.Session().current_assistant_message_index = -1;
	fixture.Parent(); fixture.WaitGate();
	fixture.Session().queued_prompt.clear(); fixture.Session().prompt_request_id = 99;
	fixture.Summary("cmp_before_first_chunk", 2, "");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (!fixture.Session().context_compaction_in_progress && std::chrono::steady_clock::now() < deadline)
	{
		(void)fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(fixture.Session().context_compaction_in_progress);
	UAM_ASSERT_EQ(fixture.Chat().messages.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Session().current_assistant_message_index, -1);
	UAM_ASSERT(fixture.Boundaries().empty());
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "First native summary.", "opencode-message:cmp_before_first_chunk");
	fixture.Summary("cmp_before_first_chunk", 2);
	fixture.SummaryText("cmp_before_first_chunk", "First native summary.");
	UAM_ASSERT(fixture.AwaitBoundary("cmp_before_first_chunk"));
	UAM_ASSERT(!fixture.Session().context_compaction_in_progress);
	UAM_ASSERT_EQ(fixture.Chat().messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_before_first_chunk"});
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks[0].text, std::string("First native summary."));
	UAM_ASSERT(std::ranges::none_of(fixture.Session().turn_events, [](const uam::AcpTurnEventState& event)
	{ return event.type == "context_compaction_pending"; }));
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 99);
}

UAM_TEST(OpenCodeMetadataInspectsRetainedSummaryAfterMoreThan128OrdinaryIdentities)
{
	NativeMetadataFixture fixture;
	fixture.Parent();
	fixture.Chat().messages[1].content.clear(); fixture.Chat().messages[1].blocks.clear();
	fixture.Session().queued_prompt.clear(); fixture.Session().prompt_request_id = 99;
	fixture.Session().current_assistant_message_index = fixture.Session().turn_assistant_message_index = 1;
	for (int index = 0; index < 130; ++index)
	{
		const std::string id = "msg_retained_" + std::to_string(index);
		fixture.NativeMessage(id, index + 2, {{"role", "assistant"}});
		(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Ordinary output.", "opencode-message:" + id);
	}
	fixture.Summary("cmp_retained_later", 200);
	fixture.SummaryText("cmp_retained_later", "Later retained summary.");
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Later retained summary.", "opencode-message:cmp_retained_later");
	UAM_ASSERT(fixture.AwaitBoundary("cmp_retained_later", std::chrono::seconds(12)));
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_retained_later"});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.size(), std::size_t{131});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.front().type, std::string("assistant_text"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("context_compaction"));
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 99);
}


UAM_TEST(OpenCodeMetadataQuarantineSurvivesNativeFinishBeforeFinalStreamChunk)
{
	NativeMetadataFixture fixture;
	fixture.Chat().messages.resize(1);
	fixture.Session().turn_assistant_message_index = fixture.Session().current_assistant_message_index = -1;
	fixture.Parent(); fixture.WaitGate();
	fixture.Session().queued_prompt.clear(); fixture.Session().prompt_request_id = 99;
	fixture.Summary("cmp_quarantined", 2, "");
	const auto started_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (!fixture.Session().context_compaction_in_progress && std::chrono::steady_clock::now() < started_deadline)
	{
		(void)fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(fixture.Session().context_compaction_in_progress);
	UAM_ASSERT_EQ(fixture.Chat().messages.size(), std::size_t{1});
	UAM_ASSERT(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Native ", "opencode-message:cmp_quarantined").empty());
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks.front().type, std::string("context_compaction_summary"));
	fixture.Summary("cmp_quarantined", 2);
	fixture.SummaryText("cmp_quarantined", "Native summary.");
	const auto finished_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (fixture.Session().context_compaction_in_progress && std::chrono::steady_clock::now() < finished_deadline)
	{
		(void)fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(!fixture.Session().context_compaction_in_progress);
	UAM_ASSERT(fixture.Boundaries().empty());
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks.front().type, std::string("context_compaction_summary"));
	UAM_ASSERT(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "summary.", "opencode-message:cmp_quarantined").empty());
	UAM_ASSERT(fixture.AwaitBoundary("cmp_quarantined"));
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_quarantined"});
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks.front().text, std::string("Native summary."));
	UAM_ASSERT_EQ(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Normal next output.", "opencode-message:msg_normal_after_summary"), std::string("Normal next output."));
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks.back().type, std::string("assistant_text"));
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 99);
}

UAM_TEST(OpenCodeMetadataQuarantineSurvivesMetadataErrorWhileProviderStillStreams)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	fixture.Summary("cmp_query_error", 2, "");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (!fixture.Session().context_compaction_in_progress && std::chrono::steady_clock::now() < deadline)
	{
		(void)fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(fixture.Session().context_compaction_in_progress);
	UAM_ASSERT(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Partial ", "opencode-message:cmp_query_error").empty());
	fixture.fail_queries = 1;
	const int before = fixture.completed_queries.load();
	const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (fixture.completed_queries.load() == before && std::chrono::steady_clock::now() < failure_deadline)
	{
		(void)fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	(void)fixture.service->Poll(fixture.app);
	(void)fixture.service->Poll(fixture.app);
	UAM_ASSERT(fixture.Session().processing);
	UAM_ASSERT(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "summary tail.", "opencode-message:cmp_query_error").empty());
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks.back().type, std::string("context_compaction_summary"));
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks.back().text, std::string("Partial summary tail."));
	UAM_ASSERT(fixture.Boundaries().empty());
}

UAM_TEST(OpenCodeMetadataFailedTurnCollapsesOnlyVerifiedSummaryPartial)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	fixture.Summary("cmp_interrupted_hidden", 2, "");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (!fixture.Session().context_compaction_in_progress && std::chrono::steady_clock::now() < deadline)
	{
		(void)fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(fixture.Session().context_compaction_in_progress);
	UAM_ASSERT(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Retained partial summary.", "opencode-message:cmp_interrupted_hidden").empty());
	fixture.Session().processing = false; fixture.Session().prompt_request_id = 0;
	fixture.Session().last_settled_turn_serial = fixture.Session().turn_serial;
	fixture.Session().last_turn_outcome = "failed";
	(void)fixture.service->Poll(fixture.app);
	UAM_ASSERT(!fixture.Session().context_compaction_in_progress);
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks.front().type, std::string("assistant_text"));
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks.front().text, std::string("Preserve authored answer"));
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks.back().type, std::string("context_compaction_interrupted"));
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks.back().text, std::string("Retained partial summary."));
	UAM_ASSERT_EQ(fixture.Chat().messages.back().content, std::string("Preserve authored answerRetained partial summary."));
}

UAM_TEST(OpenCodeMetadataFailedLifecycleRetainsInterruptedBoundaryBeforeReset)
{
	for (const bool first_chunk : {false, true})
	for (const bool reset_before_poll : {false, true})
	{
		NativeMetadataFixture fixture;
		if (first_chunk)
		{
			fixture.Chat().messages.resize(1);
			fixture.Session().turn_assistant_message_index = fixture.Session().current_assistant_message_index = -1;
		}
		fixture.Parent(); fixture.WaitGate();
		if (first_chunk) { fixture.Session().queued_prompt.clear(); fixture.Session().prompt_request_id = 99; }
		else fixture.StartStreaming();
		fixture.Summary("cmp_failed_lifecycle", 2, "");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
		while (!fixture.Session().context_compaction_in_progress && std::chrono::steady_clock::now() < deadline)
		{
			(void)fixture.service->Poll(fixture.app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT(fixture.Session().context_compaction_in_progress);
		UAM_ASSERT(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Retained partial summary.", "opencode-message:cmp_failed_lifecycle").empty());
		uam::acp_detail::CompletePromptTurn(fixture.Session(), uam::acp_detail::kAcpLifecycleError);
		UAM_ASSERT_EQ(fixture.Session().current_assistant_message_index, -1);
		if (reset_before_poll) fixture.service->ObserveBeforeTurnReset(fixture.app, fixture.Session(), fixture.Chat());
		else (void)fixture.service->Poll(fixture.app);
		UAM_ASSERT(std::ranges::none_of(fixture.Session().turn_events,
		    [](const uam::AcpTurnEventState& event) { return event.type == "context_compaction_pending"; }));
		const Message original = fixture.Chat().messages[1];
		UAM_ASSERT_EQ(original.blocks.back().type, std::string("context_compaction_interrupted"));
		UAM_ASSERT_EQ(original.blocks.back().request_id_json, std::string("acp-compaction:cmp_failed_lifecycle"));
		UAM_ASSERT_EQ(original.blocks.back().text, std::string("Retained partial summary."));
		UAM_ASSERT_EQ(original.content, first_chunk ? std::string("Retained partial summary.") : std::string("Preserve authored answerRetained partial summary."));
		UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.Chat()));
		const auto saved = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.Chat().id);
		UAM_ASSERT(saved && saved->messages[1] == original);
		if (!reset_before_poll) fixture.service->ObserveBeforeTurnReset(fixture.app, fixture.Session(), fixture.Chat());
		uam::acp_detail::ResetAcpTurnStreamState(fixture.Session());
		++fixture.Session().turn_serial;
		fixture.Session().processing = true;
		fixture.Session().queued_prompt = "Exact next continuation";
		fixture.Session().turn_assistant_message_index = -1;
		UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
		UAM_ASSERT_EQ(fixture.Chat().messages[1], original);
		UAM_ASSERT_EQ(fixture.Chat().native_session_id, std::string("ses_owned"));
		UAM_ASSERT_EQ(fixture.Session().session_id, std::string("ses_owned"));
		UAM_ASSERT_EQ(fixture.Session().queued_prompt, std::string("Exact next continuation"));
		UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 0);
	}
}

UAM_TEST(OpenCodeMetadataExplicitStopRetainsInterruptedBoundaryWithoutProcess)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	fixture.Summary("cmp_explicit_stop", 2, "");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (!fixture.Session().context_compaction_in_progress && std::chrono::steady_clock::now() < deadline)
	{
		(void)fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(fixture.Session().context_compaction_in_progress);
	UAM_ASSERT(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Explicitly stopped summary.", "opencode-message:cmp_explicit_stop").empty());
	fixture.app.opencode_compaction_before_turn_reset = [&](uam::AcpSessionState& session, const ChatSession& chat)
	{ fixture.service->ObserveBeforeTurnReset(fixture.app, session, chat); };
	// Exercise public Stop's retained-history retirement without owning or launching a process.
	fixture.Session().running = false;
	UAM_ASSERT(uam::StopAcpSession(fixture.app, fixture.Chat().id));
	UAM_ASSERT(fixture.Session().turn_events.empty());
	UAM_ASSERT(fixture.app.acp_process_stop_tasks.empty());
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("context_compaction_interrupted"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().text, std::string("Explicitly stopped summary."));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().request_id_json, std::string("acp-compaction:cmp_explicit_stop"));
	UAM_ASSERT(fixture.Chat().messages[1].interrupted);
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.Chat()));
	const auto saved = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.Chat().id);
	UAM_ASSERT(saved && saved->messages[1] == fixture.Chat().messages[1]);
	UAM_ASSERT_EQ(saved->native_session_id, std::string("ses_owned"));
	fixture.Session().running = fixture.Session().session_ready = fixture.Session().processing = true;
	fixture.Session().turn_serial = 2;
	fixture.Session().queued_prompt = "Continuation after explicit Stop";
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	UAM_ASSERT_EQ(fixture.Session().session_id, saved->native_session_id);
	UAM_ASSERT_EQ(fixture.Session().queued_prompt, std::string("Continuation after explicit Stop"));
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 0);
}

UAM_TEST(OpenCodeMetadataPendingCancelRetainsFirstChunkWithoutReplayingInput)
{
	NativeMetadataFixture fixture;
	fixture.Chat().messages.resize(1);
	fixture.Session().turn_assistant_message_index = fixture.Session().current_assistant_message_index = -1;
	fixture.Parent(); fixture.WaitGate();
	fixture.Session().queued_prompt.clear(); fixture.Session().prompt_request_id = 99;
	fixture.Summary("cmp_pending_cancel", 2, "");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (!fixture.Session().context_compaction_in_progress && std::chrono::steady_clock::now() < deadline)
	{
		(void)fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(fixture.Session().context_compaction_in_progress);
	UAM_ASSERT(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "First cancelled summary chunk.", "opencode-message:cmp_pending_cancel").empty());
	// Inject CancelAcpTurn's pre-ack state, retaining its native request without writing to a process.
	fixture.Session().processing = false;
	fixture.Session().cancel_requested = true;
	fixture.Session().current_assistant_message_index = -1;
	(void)fixture.service->Poll(fixture.app);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("context_compaction_interrupted"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().text, std::string("First cancelled summary chunk."));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().request_id_json, std::string("acp-compaction:cmp_pending_cancel"));
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 99);
	UAM_ASSERT(fixture.Session().cancel_requested);
	UAM_ASSERT(fixture.Session().queued_prompt.empty());
	UAM_ASSERT_EQ(fixture.Chat().native_session_id, std::string("ses_owned"));
}


UAM_TEST(OpenCodeMetadataSummaryCompletionPreservesInterleavedPendingMarker)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	fixture.Summary("cmp_marker_gap", 2, "");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (!fixture.Session().context_compaction_in_progress && std::chrono::steady_clock::now() < deadline)
	{
		(void)fixture.service->Poll(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(fixture.Session().context_compaction_in_progress);
	UAM_ASSERT(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Split ", "opencode-message:cmp_marker_gap").empty());
	fixture.Session().turn_events.push_back({.type = "context_compaction_pending", .request_id_json = "acp-compaction:independent-retained"});
	UAM_ASSERT(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "summary.", "opencode-message:cmp_marker_gap").empty());
	fixture.Summary("cmp_marker_gap", 2);
	fixture.SummaryText("cmp_marker_gap", "Split summary.");
	UAM_ASSERT(fixture.AwaitBoundary("cmp_marker_gap"));
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:cmp_marker_gap"});
	UAM_ASSERT(std::ranges::any_of(fixture.Session().turn_events, [](const uam::AcpTurnEventState& event)
	{ return event.type == "context_compaction_pending" && event.request_id_json == "acp-compaction:independent-retained"; }));
	UAM_ASSERT_EQ(fixture.Chat().messages.back().blocks.back().text, std::string("Split summary."));
	UAM_ASSERT(fixture.Session().context_compaction_in_progress);
}


namespace
{
	void RestoreWarmMetadataOwnerWithoutVersion(NativeMetadataFixture& fixture)
	{
		fixture.RestoreActiveRemotePrompt();
		fixture.Session().agent_version.clear();
		fixture.Session().initialized = true;
		fixture.Chat().remote_process_exists = true;
		fixture.Chat().remote_process_control_token = "private-fixture-owned-process-capability";
		fixture.Session().remote_output_delivery_token = fixture.Chat().remote_process_control_token;
	}
}

UAM_TEST(OpenCodeMetadataWarmEmptyVersionRetainsLiveSettlementAndNextDispatchOwnership)
{
	NativeMetadataFixture fixture;
	fixture.Parent();
	fixture.Summary("cmp_warm_without_initialize", 2);
	fixture.SummaryText("cmp_warm_without_initialize", "Recovered native summary.");
	fixture.Chat().messages[1].content = "Recovered native summary.";
	fixture.Chat().messages[1].blocks = {{"assistant_text", "Recovered native summary.", "", "opencode-message:cmp_warm_without_initialize"}};
	RestoreWarmMetadataOwnerWithoutVersion(fixture);
	const std::vector<uam::AcpRemotePendingRequestState> requests = fixture.Chat().remote_pending_requests;
	UAM_ASSERT(fixture.AwaitBoundary("cmp_warm_without_initialize"));
	UAM_ASSERT(fixture.Session().agent_version.empty());
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 0);
	UAM_ASSERT_EQ(fixture.Chat().remote_pending_requests, requests);
	fixture.Settle();
	fixture.Session().recovering_remote_turn = false;
	fixture.Chat().remote_turn_reconnect_pending = false;
	fixture.Chat().remote_pending_requests.clear();
	fixture.Session().pending_request_methods.clear();
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	++fixture.Session().turn_serial;
	fixture.Session().processing = true;
	fixture.Session().queued_prompt = "Next private staged input";
	fixture.Session().turn_assistant_message_index = fixture.Session().current_assistant_message_index = -1;
	UAM_ASSERT(fixture.WaitGate() == uam::OpenCodeCompactionGateState::Ready);
	UAM_ASSERT(fixture.Session().agent_version.empty());
	UAM_ASSERT_EQ(fixture.Session().queued_prompt, std::string("Next private staged input"));
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 0);
}

UAM_TEST(OpenCodeMetadataWarmEmptyVersionRejectsMissingOwnershipAndUnsupportedVersion)
{
	for (const int invalid : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11})
	{
		NativeMetadataFixture fixture;
		RestoreWarmMetadataOwnerWithoutVersion(fixture);
		if (invalid == 0) fixture.Session().agent_version = "9.99.0";
		if (invalid == 1) fixture.Chat().remote_process_control_token.clear();
		if (invalid == 2) fixture.Session().remote_output_delivery_token = "different-private-capability";
		if (invalid == 3) fixture.Chat().remote_process_exists = false;
		if (invalid == 4) fixture.Session().process_execution_host_id = "foreign-host";
		if (invalid == 5) fixture.Session().remote_runner_protocol_version = 2;
		if (invalid == 6) fixture.Session().initialized = false;
		if (invalid == 7) fixture.Chat().native_session_id = "ses_foreign";
		if (invalid == 8) fixture.Chat().remote_restart_pending = true;
		if (invalid == 9) fixture.Chat().remote_stop_cleanup_pending = true;
		if (invalid == 10) fixture.Session().cancel_requested = true;
		if (invalid == 11) fixture.Session().last_process_id.clear();
		std::string error;
		UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.Chat(), error) == uam::OpenCodeCompactionGateState::NotApplicable);
		UAM_ASSERT_EQ(fixture.queries.load(), 0);
		UAM_ASSERT(fixture.Boundaries().empty());
	}
}


UAM_TEST(OpenCodeMetadataEmptyQuarantineReceiptAllowsProvenLegacySummary)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	const std::string id = "cmp_empty_receipt_legacy";
	const std::string summary = "Exact legacy summary after its native prefix.";
	fixture.Session().turn_events.push_back({.type = "context_compaction_summary", .request_id_json = "opencode-message:" + id});
	(void)uam::acp_detail::SyncCurrentAssistantMessageBlocksFromTurnEvents(fixture.Chat(), fixture.Session());
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.size(), std::size_t{1});
	fixture.Summary(id, 2);
	fixture.SummaryText(id, summary);
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), summary);
	const std::string content = fixture.Chat().messages[1].content;
	const Message user = fixture.Chat().messages.front();
	UAM_ASSERT(fixture.AwaitBoundary(id));
	UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:" + id});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.size(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.front().type, std::string("assistant_text"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.front().text, std::string("Preserve authored answer"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().text, summary);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].content, content);
	UAM_ASSERT_EQ(fixture.Chat().messages.front(), user);
	UAM_ASSERT(uam::acp_detail::MessageBlocksEqual(fixture.Chat().messages[1].blocks,
	    uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session())));
	UAM_ASSERT_EQ(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Ordinary next output.", "opencode-message:msg_after_legacy"),
	    std::string("Ordinary next output."));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("assistant_text"));
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 99);
}

UAM_TEST(OpenCodeMetadataNonemptyQuarantineReceiptRejectsLegacyFallback)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	const std::string id = "cmp_nonempty_receipt_legacy";
	const std::string summary = "Exact native body also present as ordinary output.";
	fixture.Session().turn_events.push_back({.type = "context_compaction_summary", .text = "Different native fragment.",
	    .request_id_json = "opencode-message:" + id});
	(void)uam::acp_detail::SyncCurrentAssistantMessageBlocksFromTurnEvents(fixture.Chat(), fixture.Session());
	fixture.Summary(id, 2);
	fixture.SummaryText(id, summary);
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), summary);
	const Message before = fixture.Chat().messages[1];
	UAM_ASSERT(!fixture.AwaitBoundary(id, std::chrono::milliseconds(2300)));
	UAM_ASSERT(fixture.Boundaries().empty());
	UAM_ASSERT_EQ(fixture.Chat().messages[1], before);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks[1].type, std::string("context_compaction_summary"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("assistant_text"));
	UAM_ASSERT(uam::acp_detail::MessageBlocksEqual(before.blocks, uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session())));
}

UAM_TEST(OpenCodeMetadataEmptyReceiptDoesNotRetypeForeignNativeOutput)
{
	NativeMetadataFixture fixture;
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	const std::string id = "cmp_empty_receipt_foreign_output";
	const std::string summary = "Identical text carried by a different native message.";
	fixture.Session().turn_events.push_back({.type = "context_compaction_summary", .request_id_json = "opencode-message:" + id});
	fixture.Summary(id, 2);
	fixture.SummaryText(id, summary);
	UAM_ASSERT_EQ(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), summary, "opencode-message:msg_foreign_ordinary"), summary);
	const Message before = fixture.Chat().messages[1];
	UAM_ASSERT(!fixture.AwaitBoundary(id, std::chrono::milliseconds(2300)));
	UAM_ASSERT(fixture.Boundaries().empty());
	UAM_ASSERT_EQ(fixture.Chat().messages[1], before);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("assistant_text"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().request_id_json, std::string("opencode-message:msg_foreign_ordinary"));
	UAM_ASSERT(uam::acp_detail::MessageBlocksEqual(before.blocks, uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session())));
}

namespace
{
	void CheckOpenCodeMetadataNativeIdBoundary(std::size_t length, bool accepted)
	{
		NativeMetadataFixture fixture;
		fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
		const std::string id(length, 'x');
		const std::string summary = "Verified summary with a bounded native identity.";
		fixture.Summary(id, 2);
		fixture.SummaryText(id, summary, "part_bounded_metadata_body");
		(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), summary, "opencode-message:" + id);
		const std::string content = fixture.Chat().messages[1].content;
		if (accepted)
		{
			UAM_ASSERT(fixture.AwaitBoundary(id));
			UAM_ASSERT_EQ(fixture.Boundaries(), std::vector<std::string>{"acp-compaction:" + id});
			UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().text, summary);
		}
		else
		{
			UAM_ASSERT(!fixture.AwaitBoundary(id, std::chrono::milliseconds(2300)));
			std::string error;
			UAM_ASSERT(fixture.service->BeforeDispatch(fixture.app, fixture.Session(), fixture.Chat(), error) == uam::OpenCodeCompactionGateState::Failed);
			UAM_ASSERT(error.find("invalid metadata identity") != std::string::npos);
			UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("assistant_text"));
			UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().request_id_json, "opencode-message:" + id);
		}
		UAM_ASSERT_EQ(fixture.Chat().messages[1].content, content);
		UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 99);
		UAM_ASSERT(uam::acp_detail::MessageBlocksEqual(fixture.Chat().messages[1].blocks,
		    uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session())));
	}

	void CheckOpenCodeHistoryNativeIdBoundary(std::size_t length, bool accepted)
	{
		NativeMetadataFixture fixture;
		const std::string id(length, 'x');
		const std::string summary = "Saved quarantined summary with exact native proof.";
		fixture.Parent();
		fixture.Summary(id, 2);
		fixture.SummaryText(id, summary, "part_bounded_history_body");
		fixture.Chat().messages[1].content = summary;
		fixture.Chat().messages[1].blocks = {{"context_compaction_summary", summary, "", "opencode-message:" + id}};
		fixture.Session().running = fixture.Session().session_ready = fixture.Session().processing = false;
		fixture.Session().queued_prompt.clear();
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
		UAM_ASSERT(saved.has_value());
		fixture.Chat() = *saved;
		fixture.app.selected_chat_index = 0;
		const std::vector<Message> before = fixture.Chat().messages;
		bool changed = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (std::chrono::steady_clock::now() < deadline)
		{
			changed = fixture.service->Poll(fixture.app) || changed;
			if (changed) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT_EQ(fixture.queries.load(), 1);
		UAM_ASSERT_EQ(fixture.completed_queries.load(), 1);
		UAM_ASSERT(changed == accepted);
		const MessageBlock& block = fixture.Chat().messages[1].blocks.front();
		UAM_ASSERT_EQ(block.type, accepted ? std::string("context_compaction") : std::string("context_compaction_summary"));
		UAM_ASSERT_EQ(block.request_id_json, accepted ? id : "opencode-message:" + id);
		UAM_ASSERT_EQ(block.text, summary);
		UAM_ASSERT_EQ(fixture.Chat().messages[1].content, before[1].content);
		UAM_ASSERT_EQ(fixture.Chat().messages.front(), before.front());
		const auto repaired = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
		UAM_ASSERT(repaired.has_value());
		UAM_ASSERT_EQ(repaired->messages, fixture.Chat().messages);
		if (!accepted) UAM_ASSERT_EQ(fixture.Chat().messages, before);
	}
}

UAM_TEST(OpenCodeMetadataNativeIdAccepts64Characters)
{
	CheckOpenCodeMetadataNativeIdBoundary(64, true);
}

UAM_TEST(OpenCodeMetadataNativeIdAccepts65Characters)
{
	CheckOpenCodeMetadataNativeIdBoundary(65, true);
}

UAM_TEST(OpenCodeMetadataNativeIdAccepts512Characters)
{
	CheckOpenCodeMetadataNativeIdBoundary(512, true);
}

UAM_TEST(OpenCodeMetadataNativeIdRejects513Characters)
{
	CheckOpenCodeMetadataNativeIdBoundary(513, false);
}

UAM_TEST(OpenCodeHistoryNativeIdAccepts64Characters)
{
	CheckOpenCodeHistoryNativeIdBoundary(64, true);
}

UAM_TEST(OpenCodeHistoryNativeIdAccepts65Characters)
{
	CheckOpenCodeHistoryNativeIdBoundary(65, true);
}

UAM_TEST(OpenCodeHistoryNativeIdAccepts512Characters)
{
	CheckOpenCodeHistoryNativeIdBoundary(512, true);
}

UAM_TEST(OpenCodeHistoryNativeIdRejects513Characters)
{
	CheckOpenCodeHistoryNativeIdBoundary(513, false);
}


UAM_TEST(OpenCodeHistoryRejectsForeignOrdinaryNativeIdentityWithoutPublication)
{
	const std::string id = "cmp_history_identity_proof";
	for (const std::string identity : {"opencode-message:cmp_history_identity_proof", "", "opencode-message:msg_foreign_ordinary"})
	{
		NativeMetadataFixture fixture;
		const bool accepted = identity.empty() || identity == "opencode-message:" + id;
		const std::string summary = "Exact historical summary bytes with authoritative native position.";
		fixture.Parent();
		fixture.Summary(id, 2);
		fixture.SummaryText(id, summary, "part_history_identity_body");
		fixture.Chat().messages[1].content = summary;
		fixture.Chat().messages[1].blocks = {{"assistant_text", summary, "", identity}};
		fixture.Session().running = fixture.Session().session_ready = fixture.Session().processing = false;
		fixture.Session().queued_prompt.clear();
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
		UAM_ASSERT(saved.has_value());
		fixture.Chat() = *saved;
		fixture.app.selected_chat_index = 0;
		fixture.Session().current_assistant_message_index = fixture.Session().turn_assistant_message_index = 1;
		uam::acp_detail::RestoreTurnEventsFromMessageBlocks(fixture.Session(), fixture.Chat().messages[1]);
		const std::vector<Message> before = fixture.Chat().messages;
		const std::vector<MessageBlock> runtime_before = uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session());
		const fs::path primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.Chat().id);
		const fs::path summary_file = AppPaths::UamChatSummaryFilePath(fixture.app.data_root, fixture.Chat().id);
		const std::string primary_before = uam::io::ReadTextFile(primary);
		const std::string summary_before = uam::io::ReadTextFile(summary_file);
		bool changed = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (std::chrono::steady_clock::now() < deadline)
		{
			changed = fixture.service->Poll(fixture.app) || changed;
			if (changed) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT_EQ(fixture.queries.load(), 1);
		UAM_ASSERT_EQ(fixture.completed_queries.load(), 1);
		UAM_ASSERT(changed == accepted);
		UAM_ASSERT_EQ(fixture.Chat().messages.front(), before.front());
		UAM_ASSERT_EQ(fixture.Chat().messages[1].content, before[1].content);
		UAM_ASSERT(uam::acp_detail::MessageBlocksEqual(fixture.Chat().messages[1].blocks,
		    uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session())));
		if (accepted)
		{
			UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.front().type, std::string("context_compaction"));
			UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.front().request_id_json, id);
			const auto repaired = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
			UAM_ASSERT(repaired.has_value());
			UAM_ASSERT_EQ(repaired->messages, fixture.Chat().messages);
		}
		else
		{
			UAM_ASSERT_EQ(fixture.Chat().messages, before);
			UAM_ASSERT(uam::acp_detail::MessageBlocksEqual(runtime_before, uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session())));
			UAM_ASSERT_EQ(uam::io::ReadTextFile(primary), primary_before);
			UAM_ASSERT_EQ(uam::io::ReadTextFile(summary_file), summary_before);
		}
	}
}

UAM_TEST(OpenCodeMetadataPublishedReceiptDoesNotRediscoverOrDelaySecondBatch)
{
	const std::string first_id = "cmp_trace_first_batch";
	const std::string second_id = "cmp_trace_second_batch";
	const std::string first_summary = "First published summary in the query trace.";
	const std::string second_summary = "Second published summary in the query trace.";
	std::atomic<bool> first_published{false};
	std::atomic<int> published_identity_queries{0};
	std::mutex trace_mutex;
	std::vector<std::string> published_identity_sql;
	NativeMetadataFixture fixture;
	fixture.service = std::make_unique<uam::OpenCodeCompactionSyncService>([&](const uam::OpenCodeCompactionSyncService::Query& query, std::stop_token stop)
	{
		UAM_ASSERT(!stop.stop_requested());
		UAM_ASSERT(std::ranges::find(query.argv, query.sql) != query.argv.end());
		UAM_ASSERT(std::ranges::find(query.argv, "db") != query.argv.end());
		if (first_published.load(std::memory_order_acquire) && query.sql.find(" AND m.id IN (") != std::string::npos &&
		    query.sql.find("'" + first_id + "'") != std::string::npos)
		{
			std::lock_guard<std::mutex> lock(trace_mutex);
			if (published_identity_sql.size() < 2) published_identity_sql.push_back(query.sql);
			published_identity_queries.fetch_add(1, std::memory_order_release);
		}
		++fixture.queries;
		const ProcessExecutionResult result = fixture.Execute(query.sql);
		fixture.completed_queries.fetch_add(1, std::memory_order_release);
		return result;
	});
	fixture.Parent(); fixture.WaitGate(); fixture.StartStreaming();
	fixture.Summary(first_id, 2);
	fixture.SummaryText(first_id, first_summary);
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), first_summary);
	UAM_ASSERT(fixture.AwaitBoundary(first_id));
	UAM_ASSERT(std::ranges::any_of(fixture.Session().turn_events, [&](const uam::AcpTurnEventState& event)
	{ return event.type == "context_compaction_summary" && event.text.empty() && event.request_id_json == "opencode-message:" + first_id; }));
	first_published.store(true, std::memory_order_release);
	fixture.Summary(second_id, 3);
	fixture.SummaryText(second_id, second_summary);
	(void)uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), second_summary);
	// Keep AwaitBoundary's existing four-second budget: a retired-ID probe must not add another two-second interval.
	const bool second_published = fixture.AwaitBoundary(second_id);
	const int rediscovered = published_identity_queries.load(std::memory_order_acquire);
	if (rediscovered != 0 || !second_published)
	{
		std::lock_guard<std::mutex> lock(trace_mutex);
		throw std::runtime_error("Published native identity query trace: rediscovered=" + std::to_string(rediscovered) +
		    " second_boundary=" + std::to_string(second_published) + " queries=" + std::to_string(fixture.queries.load()) +
		    " SQL=" + (published_identity_sql.empty() ? std::string("none") : published_identity_sql.front()));
	}
	UAM_ASSERT_EQ(published_identity_queries.load(), 0);
	UAM_ASSERT(second_published);
	UAM_ASSERT_EQ(fixture.Boundaries(), (std::vector<std::string>{"acp-compaction:" + first_id, "acp-compaction:" + second_id}));
	UAM_ASSERT(fixture.queries.load() <= 6);
	UAM_ASSERT_EQ(fixture.Chat().messages[1].content, std::string("Preserve authored answer") + first_summary + second_summary);
	UAM_ASSERT(std::ranges::any_of(fixture.Session().turn_events, [&](const uam::AcpTurnEventState& event)
	{ return event.type == "context_compaction_summary" && event.text.empty() && event.request_id_json == "opencode-message:" + first_id; }));
	UAM_ASSERT(uam::acp_detail::MessageBlocksEqual(fixture.Chat().messages[1].blocks,
	    uam::acp_detail::MessageBlocksFromTurnEvents(fixture.Session())));
	UAM_ASSERT(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Late summary chunk.", "opencode-message:" + first_id).empty());
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("context_compaction_summary"));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().request_id_json, "opencode-message:" + first_id);
	UAM_ASSERT_EQ(uam::acp_detail::AppendAssistantChunk(fixture.Chat(), fixture.Session(), "Ordinary next output.", "opencode-message:msg_after_trace"),
	    std::string("Ordinary next output."));
	UAM_ASSERT_EQ(fixture.Chat().messages[1].blocks.back().type, std::string("assistant_text"));
	UAM_ASSERT_EQ(fixture.Session().prompt_request_id, 99);
}
#endif
