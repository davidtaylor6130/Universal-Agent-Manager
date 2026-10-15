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
		uam::AcpQueuedUserPromptState captured_agent;
		std::unique_ptr<uam::OpenCodeCompactionSyncService> service;

		NativeMetadataFixture()
		{
			UAM_ASSERT_EQ(sqlite3_open(":memory:", &database), SQLITE_OK);
			const char* schema = "CREATE TABLE session(id TEXT PRIMARY KEY,directory TEXT);"
			    "CREATE TABLE message(id TEXT PRIMARY KEY,session_id TEXT,time_created INTEGER,data TEXT);"
			    "CREATE INDEX message_session_time_created_id_idx ON message(session_id,time_created,id);"
			    "CREATE TABLE part(id TEXT PRIMARY KEY,message_id TEXT,session_id TEXT,data TEXT);"
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
				UAM_ASSERT(query.sql.find("$.text") == std::string::npos);
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
			UAM_ASSERT_EQ(sqlite3_prepare_v2(database, "INSERT OR REPLACE INTO message VALUES (?,?,?,?)", -1, &statement, nullptr), SQLITE_OK);
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
			UAM_ASSERT_EQ(sqlite3_prepare_v2(database, "INSERT OR REPLACE INTO part VALUES (?,?,?,?)", -1, &statement, nullptr), SQLITE_OK);
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
		fixture.service = std::make_unique<uam::OpenCodeCompactionSyncService>([oversized](const uam::OpenCodeCompactionSyncService::Query&, std::stop_token)
		{
			if (oversized) return ProcessExecutionResult{.ok = true, .output = std::string(512 * 1024 + 1, 'x')};
			return ProcessExecutionResult{.timed_out = true};
		});
		bool failed = false;
		try { fixture.WaitGate(); }
		catch (const std::runtime_error& failure)
		{ failed = std::string(failure.what()).find(oversized ? "512 KiB" : "timed out") != std::string::npos; }
		UAM_ASSERT(failed);
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
