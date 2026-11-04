#include "test_harness.h"
#include "cef/state_serializer.h"
#include "app/managed_subagent_control_service.h"
#include "common/runtime/acp/acp_session_internal.h"
#include <future>

using namespace uam_test;

namespace
{
	struct ManagedControlFixture
	{
		TempDir temp{"uam-managed-control"};
		uam::AppState app;

		ManagedControlFixture()
		{
			app.data_root = temp.root / "data";
			ChatSession root;
			root.id = "root";
			root.created_at = "2026-10-16 10:00:00";
			app.chats.push_back(root);
			root.id = "other-root";
			root.metadata_incarnation = std::make_shared<const std::uint8_t>(0);
			app.chats.push_back(root);
		}

		std::string Add(std::string root = "root", std::string parent = {}, std::string status = "queued")
		{
			AgentRun run;
			run.id = uam::AgentRunLedger::NewRunId();
			run.root_chat_id = std::move(root);
			run.parent_run_id = std::move(parent);
			run.transcript_chat_id = "transcript-" + run.id;
			run.agent_id = "worker";
			run.provider_id = "codex-cli";
			run.model_id = "actual-child-model";
			run.execution_capability = "uam-prompt-injected";
			run.definition_snapshot = "PRIVATE-DEFINITION";
			run.task = "Review the sidebar diff.\nPRIVATE-PROMPT";
			run.effective_workspace_access = "read";
			run.status = std::move(status);
			run.depth = run.parent_run_id.empty() ? 1 : 2;
			run.created_at = "2026-10-16 10:01:00";
			run.updated_at = run.created_at;
			if (run.status != "queued")
			{
				run.started_at = "2026-10-16 10:02:00";
				run.started_at_epoch_ms = 1792141320000LL;
				run.deadline_at_epoch_ms = run.started_at_epoch_ms + 1800000;
			}
			if (run.status == "completed" || run.status == "failed" || run.status == "interrupted" || run.status == "cancelled")
				run.finished_at = "2026-10-16 10:03:00";
			ChatSession transcript;
			transcript.id = run.transcript_chat_id;
			transcript.agent_run_id = run.id;
			transcript.created_at = run.created_at;
			transcript.provider_id = run.provider_id;
			transcript.model_id = run.model_id;
			transcript.execution_host_id = "actual-child-host";
			app.chats.push_back(transcript);
			app.agent_runs.push_back(run);
			if (run.status == "queued") app.queued_agent_run_ids.push_back(run.id);
			UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
			return run.id;
		}

		uam::ManagedSubagentPage List(std::string run = {}, std::size_t offset = 0, std::size_t limit = 50)
		{
			uam::ManagedSubagentPage page;
			std::string error;
			UAM_ASSERT(uam::ManagedSubagentControlService::List(app, "root", &page, &error, run, offset, limit));
			return page;
		}

		bool Cancel(const std::string& run, const uam::ManagedSubagentControlScope& scope)
		{
			std::string error;
			return uam::ManagedSubagentControlService::Cancel(app, "root", run, scope, &error);
		}
	};
}

UAM_TEST(ManagedSubagentListProjectsExactLineageAndTimingWithoutPrivatePayloads)
{
	ManagedControlFixture fixture;
	const auto parent = fixture.Add("root", {}, "running");
	const auto child = fixture.Add("root", parent, "completed");
	fixture.Add("other-root");
	const auto page = fixture.List();
	UAM_ASSERT_EQ(page.total_count, std::size_t{2});
	const auto json = uam::ManagedSubagentControlService::ToJson(page);
	const auto row = std::ranges::find_if(json.at("runs"), [&](const auto& item) { return item.at("runId") == child; });
	UAM_ASSERT(row != json.at("runs").end());
	UAM_ASSERT_EQ(row->at("parentRunId"), nlohmann::json(parent));
	UAM_ASSERT_EQ(row->at("agentId"), nlohmann::json("worker"));
	UAM_ASSERT_EQ(row->at("providerId"), nlohmann::json("codex-cli"));
	UAM_ASSERT_EQ(row->at("modelId"), nlohmann::json("actual-child-model"));
	UAM_ASSERT_EQ(row->at("host"), nlohmann::json("actual-child-host"));
	UAM_ASSERT_EQ(row->at("createdAt"), nlohmann::json("2026-10-16 10:01:00"));
	UAM_ASSERT_EQ(row->at("startedAt"), nlohmann::json("2026-10-16 10:02:00"));
	UAM_ASSERT_EQ(row->at("completedAt"), nlohmann::json("2026-10-16 10:03:00"));
	UAM_ASSERT_EQ(row->at("startedAtEpochMs"), nlohmann::json(1792141320000LL));
	UAM_ASSERT(!row->at("canCancel").get<bool>());
	UAM_ASSERT(json.dump().find("PRIVATE-") == std::string::npos);
	UAM_ASSERT(json.dump().find("scope") == std::string::npos);
	UAM_ASSERT_EQ(fixture.List(parent).total_count, std::size_t{2});
}

UAM_TEST(ManagedSubagentListBoundsPagesAndRejectsForeignOrHiddenRoots)
{
	ManagedControlFixture fixture;
	for (int i = 0; i < 4; ++i) fixture.Add();
	const auto foreign = fixture.Add("other-root");
	const auto first = fixture.List({}, 0, 2);
	const auto second = fixture.List({}, 2, 2);
	UAM_ASSERT_EQ(first.runs.size(), std::size_t{2});
	UAM_ASSERT_EQ(first.next_offset, std::optional<std::size_t>{2});
	UAM_ASSERT(!second.next_offset.has_value());
	UAM_ASSERT(first.runs.front().run_id != second.runs.front().run_id);
	const auto empty = fixture.List({}, 4, 2);
	UAM_ASSERT(empty.runs.empty());
	uam::ManagedSubagentPage page;
	std::string error;
	UAM_ASSERT(!uam::ManagedSubagentControlService::List(fixture.app, "root", &page, &error, {}, 0, 0));
	UAM_ASSERT(!uam::ManagedSubagentControlService::List(fixture.app, "root", &page, &error, {}, 0, 101));
	UAM_ASSERT(!uam::ManagedSubagentControlService::List(fixture.app, "root", &page, &error, foreign));
	UAM_ASSERT(!uam::ManagedSubagentControlService::List(fixture.app, fixture.app.agent_runs.front().transcript_chat_id, &page, &error));
	fixture.app.agent_runs.resize(uam::ManagedSubagentControlService::MaxLedgerRuns + 1);
	UAM_ASSERT(!uam::ManagedSubagentControlService::List(fixture.app, "root", &page, &error));
}

UAM_TEST(ManagedSubagentCancelStopsOnlySelectedTreeAndIsTerminalIdempotent)
{
	ManagedControlFixture fixture;
	const auto parent = fixture.Add();
	const auto child = fixture.Add("root", parent);
	const auto sibling = fixture.Add();
	const auto foreign = fixture.Add("other-root");
	const auto page = fixture.List();
	UAM_ASSERT(!fixture.Cancel(foreign, page.scope));
	UAM_ASSERT(fixture.Cancel(parent, page.scope));
	UAM_ASSERT(fixture.Cancel(parent, page.scope));
	UAM_ASSERT_EQ(fixture.app.agent_runs.size(), std::size_t{4});
	UAM_ASSERT_EQ(fixture.app.agent_runs[0].status, std::string("cancelled"));
	UAM_ASSERT_EQ(fixture.app.agent_runs[1].status, std::string("cancelled"));
	UAM_ASSERT_EQ(fixture.app.agent_runs[2].status, std::string("queued"));
	UAM_ASSERT_EQ(fixture.app.agent_runs[3].status, std::string("queued"));
	UAM_ASSERT_EQ(fixture.app.queued_agent_run_ids.size(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.app.queued_agent_run_ids[0], sibling);
	UAM_ASSERT_EQ(fixture.app.queued_agent_run_ids[1], foreign);
	UAM_ASSERT_EQ(uam::AgentRunLedger::LoadAll(fixture.app.data_root).runs.size(), std::size_t{4});
	UAM_ASSERT(fixture.app.acp_sessions.empty());
	(void)child;
}

UAM_TEST(ManagedSubagentCancelRejectsRootRecreationAndLedgerScopeChanges)
{
	ManagedControlFixture fixture;
	const auto run = fixture.Add();
	const auto page = fixture.List();
	const auto incarnation = fixture.app.chats.front().metadata_incarnation;
	fixture.app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
	UAM_ASSERT(!fixture.Cancel(run, page.scope));
	fixture.app.chats.front().metadata_incarnation = incarnation;
	const auto data_root = fixture.app.data_root;
	fixture.app.data_root = fixture.temp.root / "different-ledger";
	UAM_ASSERT(!fixture.Cancel(run, page.scope));
	uam::ManagedSubagentPage stale;
	std::string error;
	UAM_ASSERT(!uam::ManagedSubagentControlService::List(fixture.app, "root", &stale, &error));
	fixture.app.data_root = data_root;
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("queued"));
	UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.empty());
}

UAM_TEST(ManagedSubagentCancelRejectsCrossRootDescendantsBeforeAnyMutation)
{
	ManagedControlFixture fixture;
	const auto run = fixture.Add();
	const auto foreign = fixture.Add("other-root", run);
	const auto page = fixture.List();
	UAM_ASSERT_EQ(page.total_count, std::size_t{1});
	UAM_ASSERT(!page.runs.front().can_cancel);
	UAM_ASSERT(!fixture.Cancel(run, page.scope));
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("queued"));
	UAM_ASSERT_EQ(fixture.app.agent_runs.back().status, std::string("queued"));
	UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.empty());
	(void)foreign;
}

UAM_TEST(ManagedSubagentCancelRejectsReboundTranscriptAndForeignSessionOwnership)
{
	for (int mode = 0; mode < 5; ++mode)
	{
		ManagedControlFixture fixture;
		const auto run = fixture.Add();
		const auto page = fixture.List();
		if (mode == 0) fixture.app.chats.back().agent_run_id = "foreign-run";
		if (mode == 1) fixture.app.agent_runs.back().transcript_chat_id = "other-root";
		if (mode == 2) fixture.app.chats.back().provider_id = "claude-cli";
		if (mode == 3) fixture.app.chats.back().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		if (mode == 4)
		{
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = fixture.app.agent_runs.back().transcript_chat_id;
			session->managed_agent_run_id = "foreign-run";
			session->processing = true;
			fixture.app.acp_sessions.push_back(std::move(session));
		}
		UAM_ASSERT(!fixture.Cancel(run, page.scope));
		UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("queued"));
		UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.empty());
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{3});
		if (mode == 4) UAM_ASSERT(fixture.app.acp_sessions.front()->processing);
	}
}

UAM_TEST(ManagedSubagentArchivedHostIsExactOrExplicitlyUnknown)
{
	ManagedControlFixture fixture;
	const auto run = fixture.Add("root", {}, "completed");
	const ChatSession transcript = fixture.app.chats.back();
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, transcript));
	fixture.app.chats.pop_back();
	UAM_ASSERT_EQ(fixture.List().runs.front().host, std::optional<std::string>{"actual-child-host"});
	ChatSession rebound = transcript;
	rebound.agent_run_id = "foreign-run";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, rebound));
	UAM_ASSERT(!fixture.List().runs.front().host.has_value());
	UAM_ASSERT(fixture.Cancel(run, fixture.List().scope));
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("completed"));
}

UAM_TEST(ManagedSubagentCancellationRetainsSchedulerPersistenceBarrierAndRetryIntent)
{
	ManagedControlFixture fixture;
	const auto run = fixture.Add("root", {}, "running");
	const auto transcript = fixture.app.agent_runs.front().transcript_chat_id;
	const auto page = fixture.List();
	fixture.app.pending_chat_save_at_by_chat_id[transcript] = 1.0;
	UAM_ASSERT(!fixture.Cancel(run, page.scope));
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("running"));
	UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.contains(run));
	UAM_ASSERT_EQ(uam::AgentRunLedger::LoadAll(fixture.app.data_root).runs.front().status, std::string("running"));
	fixture.app.pending_chat_save_at_by_chat_id.clear();
	UAM_ASSERT(fixture.Cancel(run, page.scope));
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("cancelled"));
	UAM_ASSERT(!fixture.app.pending_agent_run_cancellation_ids.contains(run));
}

UAM_TEST(ManagedSubagentRetryGuardRejectsReboundOwnershipAfterPendingStop)
{
	ManagedControlFixture fixture;
	const auto run = fixture.Add("root", {}, "running");
	const auto transcript = fixture.app.agent_runs.front().transcript_chat_id;
	const auto page = fixture.List();
	fixture.app.pending_chat_save_at_by_chat_id[transcript] = 1.0;
	UAM_ASSERT(!fixture.Cancel(run, page.scope));
	fixture.app.pending_chat_save_at_by_chat_id.clear();
	fixture.app.chats.back().agent_run_id = "unrelated-owner";
	std::string error;
	UAM_ASSERT(!uam::ManagedSubagentControlService::ValidateCancellationOwnership(fixture.app, run, &error));
	UAM_ASSERT(!fixture.Cancel(run, page.scope));
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1000);
	UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.contains(run));
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("running"));
	UAM_ASSERT_EQ(fixture.app.chats.back().agent_run_id, std::string("unrelated-owner"));
}

UAM_TEST(ManagedSubagentListRejectsMissingCyclicAndStaleLedgerIdentity)
{
	for (int mode = 0; mode < 4; ++mode)
	{
		ManagedControlFixture fixture;
		const auto run = fixture.Add();
		if (mode == 0) fixture.app.agent_runs.front().parent_run_id = uam::AgentRunLedger::NewRunId();
		if (mode == 1) fixture.app.agent_runs.front().parent_run_id = run;
		if (mode == 2) fixture.app.agent_runs.front().model_id = "different-model";
		if (mode == 3) fixture.app.chats.front().created_at = "2026-10-16 11:00:00";
		uam::ManagedSubagentPage page;
		std::string error;
		UAM_ASSERT(!uam::ManagedSubagentControlService::List(fixture.app, "root", &page, &error));
		UAM_ASSERT(page.runs.empty());
		UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.empty());
	}
}

UAM_TEST(ManagedSubagentDeferredCancellationRetainsOriginalAdmissionAcrossAllRebindings)
{
	for (int mode = 0; mode < 7; ++mode)
	{
		ManagedControlFixture fixture;
		const auto parent = fixture.Add();
		const auto child = fixture.Add("root", parent);
		const auto scope = fixture.List().scope;
		const auto child_transcript = fixture.app.agent_runs[1].transcript_chat_id;
		const auto parent_transcript = fixture.app.agent_runs[0].transcript_chat_id;
		fixture.app.pending_chat_save_at_by_chat_id[parent_transcript] = 1.0;
		fixture.app.pending_chat_save_at_by_chat_id[child_transcript] = 1.0;
		UAM_ASSERT(!fixture.Cancel(parent, scope));
		UAM_ASSERT(fixture.app.managed_subagent_cancellation_receipts.contains(parent));
		UAM_ASSERT(fixture.app.managed_subagent_cancellation_receipts.contains(child));
		fixture.app.pending_chat_save_at_by_chat_id.clear();
		if (mode == 0) fixture.app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		if (mode == 1) fixture.app.chats.back().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		if (mode == 2) fixture.app.data_root = fixture.temp.root / "replacement-data-root";
		if (mode == 3)
		{
			fixture.app.chats.back().id = "replacement-transcript";
			fixture.app.agent_runs[1].transcript_chat_id = fixture.app.chats.back().id;
			UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, fixture.app.agent_runs[1]));
		}
		if (mode == 4)
		{
			fixture.app.agent_runs[1].agent_id = "different-agent";
			UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, fixture.app.agent_runs[1]));
		}
		if (mode == 5) fixture.Add("root", parent, "completed");
		if (mode == 6) fixture.app.managed_subagent_control_incarnation = std::make_shared<const std::uint8_t>(0);
		auto replacement = std::make_unique<uam::AcpSessionState>();
		replacement->chat_id = fixture.app.agent_runs[1].transcript_chat_id;
		replacement->provider_id = "codex-cli";
		replacement->managed_agent_run_id = child;
		replacement->processing = true;
		fixture.app.acp_sessions.push_back(std::move(replacement));
		(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1000);
		UAM_ASSERT_EQ(fixture.app.agent_runs[0].status, std::string("queued"));
		UAM_ASSERT_EQ(fixture.app.agent_runs[1].status, std::string("queued"));
		UAM_ASSERT(fixture.app.acp_sessions.front()->processing);
		UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.contains(parent));
		UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.contains(child));
		if (mode == 2) UAM_ASSERT(!fs::exists(fixture.app.data_root / "agent-runs"));
	}
}

UAM_TEST(ManagedSubagentInternalCancellationRetriesBypassGuiAdmissionAndUnrelatedInventory)
{
	for (int mode = 0; mode < 3; ++mode)
	{
		ManagedControlFixture fixture;
		const auto run = fixture.Add("root", {}, mode == 2 ? "running" : "queued");
		fixture.app.chats.back().execution_host_id = "local";
		if (mode == 0) fixture.app.chats.front().goal_owner_chat_id = "visible-owner";
		if (mode == 1) fixture.app.chats.front().temporary_parent_chat_id = "visible-owner";
		if (mode == 2)
		{
			fixture.app.chats.front().goal_owner_chat_id = "visible-owner";
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = fixture.app.agent_runs.front().transcript_chat_id;
			session->managed_agent_run_id = run;
			session->managed_cancellation_pending = true;
			fixture.app.acp_sessions.push_back(std::move(session));
		}
		const auto transcript = fixture.app.agent_runs.front().transcript_chat_id;
		fixture.app.pending_chat_save_at_by_chat_id[transcript] = 1.0;
		std::string error;
		UAM_ASSERT(!uam::AgentRunScheduler::CancelTree(fixture.app, run, &error));
		UAM_ASSERT(fixture.app.managed_subagent_cancellation_receipts.empty());
		// Synthetic, unrelated records must not trigger global GUI inventory/identity admission.
		fixture.app.agent_runs.emplace_back();
		fixture.app.agent_runs.back().status = "interrupted";
		fixture.app.pending_chat_save_at_by_chat_id.clear();
		UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(fixture.app, 1001));
		UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("cancelled"));
		UAM_ASSERT(!fixture.app.pending_agent_run_cancellation_ids.contains(run));
		UAM_ASSERT(fixture.app.managed_subagent_cancellation_receipts.empty());
	}
}

UAM_TEST(ManagedSubagentOpenCodeStartupCanonicalModelRetainsStopAndRejectsRealRebind)
{
	for (bool rebind : {false, true})
	{
		ManagedControlFixture fixture;
		const auto run = fixture.Add("root", {}, "running");
		auto& record = fixture.app.agent_runs.front();
		record.provider_id = "opencode-cli";
		record.model_id = "vendor/MODEL";
		auto& chat = fixture.app.chats.back();
		chat.provider_id = record.provider_id;
		chat.model_id = record.model_id;
		chat.execution_host_id = "local";
		UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, record));
		auto session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = chat.id;
		session->provider_id = "opencode-cli";
		session->managed_agent_run_id = run;
		session->running = session->session_ready = true;
		session->session_id = "actual-owned-native-session";
		session->current_model_id = "vendor/model";
		uam::AcpModelState advertised;
		advertised.id = "vendor/model";
		session->available_models.push_back(advertised);
		// Execute the actual startup resolver path: it canonicalises chat.model_id before a write.
		UAM_ASSERT(!uam::acp_detail::SendStartupModelIfNeeded(*session, chat));
		UAM_ASSERT_EQ(chat.model_id, std::string("vendor/model"));
		session->running = false;
		fixture.app.acp_sessions.push_back(std::move(session));
		const auto page = fixture.List();
		UAM_ASSERT(page.runs.front().can_cancel);
		UAM_ASSERT(page.runs.front().host.has_value());
		fixture.app.pending_chat_save_at_by_chat_id[chat.id] = 1.0;
		UAM_ASSERT(!fixture.Cancel(run, page.scope));
		fixture.app.pending_chat_save_at_by_chat_id.clear();
		if (rebind) fixture.app.chats.back().model_id = "vendor/unrelated-model";
		(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1000);
		UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string(rebind ? "running" : "cancelled"));
		if (rebind) UAM_ASSERT_EQ(fixture.app.chats.back().model_id, std::string("vendor/unrelated-model"));
	}
}

UAM_TEST(ManagedSubagentDetachedMetadataReadNeverMergesOversizedPrivateSidecars)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "completed");
	const ChatSession transcript = fixture.app.chats.back();
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, transcript));
	fixture.app.chats.pop_back();
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, transcript.id);
	const std::string primary_before = ReadFile(primary);
	const std::string private_bytes(2U * 1024U * 1024U, 'X');
	UAM_ASSERT(uam::io::WriteTextFile(AppPaths::UamChatSummaryFilePath(fixture.app.data_root, transcript.id), private_bytes));
	// This is QueueJournalPath's real path; the reader must never open this prompt sidecar.
	fs::create_directories(fixture.app.data_root / "queue-outboxes");
	UAM_ASSERT(uam::io::WriteTextFile(fixture.app.data_root / "queue-outboxes" / (transcript.id + ".json"), private_bytes));
	uam::ManagedSubagentSnapshot snapshot;
	std::string error;
	UAM_ASSERT(uam::ManagedSubagentControlService::Capture(fixture.app, "root", &snapshot, &error));
	const auto read = uam::ManagedSubagentControlService::ReadDetached(snapshot);
	UAM_ASSERT(read.ready);
	UAM_ASSERT_EQ(read.page.runs.front().host, std::optional<std::string>{"actual-child-host"});
	UAM_ASSERT_EQ(read.archive_bytes_read, primary_before.size());
	UAM_ASSERT(read.archive_bytes_read < uam::ManagedSubagentControlService::MaxArchivedMetadataBytes);
	UAM_ASSERT_EQ(ReadFile(primary), primary_before);
}

UAM_TEST(ManagedSubagentDetachedReadRevalidatesOriginalOwnerBeforePublishOrCancel)
{
	for (int mode = 0; mode < 4; ++mode)
	{
		ManagedControlFixture fixture;
		const auto run = fixture.Add();
		const auto scope = fixture.List().scope;
		uam::ManagedSubagentSnapshot snapshot;
		UAM_ASSERT(uam::ManagedSubagentControlService::Capture(fixture.app, "root", &snapshot, nullptr, run));
		const auto read = uam::ManagedSubagentControlService::ReadDetached(snapshot, false);
		UAM_ASSERT(read.ready);
		if (mode == 0) fixture.app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		if (mode == 1) fixture.app.chats.back().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		if (mode == 2) fixture.app.data_root = fixture.temp.root / "new-data-root";
		if (mode == 3) fixture.app.agent_runs.front().agent_id = "different-agent";
		UAM_ASSERT(!uam::ManagedSubagentControlService::MatchesSnapshot(fixture.app, snapshot));
		UAM_ASSERT(!uam::ManagedSubagentControlService::CancelPrepared(fixture.app, "root", run, scope, snapshot, read));
		UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.empty());
		UAM_ASSERT(fixture.app.managed_subagent_cancellation_receipts.empty());
		UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("queued"));
	}
}

UAM_TEST(ManagedSubagentArchivedHostReadsShareOneAggregateMetadataBudget)
{
	ManagedControlFixture fixture;
	for (int i = 0; i < 24; ++i)
	{
		fixture.Add("root", {}, "completed");
		const auto& run = fixture.app.agent_runs.back();
		const auto path = AppPaths::UamChatFilePath(fixture.app.data_root, run.transcript_chat_id);
		fs::create_directories(path.parent_path());
		const auto primary = nlohmann::json{{"id", run.transcript_chat_id}, {"agent_run_id", run.id},
		    {"provider_id", run.provider_id}, {"model_id", run.model_id}, {"execution_host_id", "archive-host"},
		    {"messages", nlohmann::json::array({{{"content", std::string(60000, 'P')}}})}};
		UAM_ASSERT(uam::io::WriteTextFile(path, primary.dump()));
	}
	fixture.app.chats.resize(2);
	uam::ManagedSubagentSnapshot snapshot;
	UAM_ASSERT(uam::ManagedSubagentControlService::Capture(fixture.app, "root", &snapshot));
	const auto read = uam::ManagedSubagentControlService::ReadDetached(snapshot);
	if (!read.ready) throw TestFailure("Archived metadata read not ready: " + read.error);
	UAM_ASSERT(read.ready);
	UAM_ASSERT(read.archive_bytes_read <= uam::ManagedSubagentControlService::MaxArchiveTotalBytes);
	UAM_ASSERT(read.archive_bytes_read > 60000);
	UAM_ASSERT(std::ranges::any_of(read.page.runs, [](const auto& row) { return !row.host.has_value(); }));
	UAM_ASSERT(std::ranges::any_of(read.page.runs, [](const auto& row) { return row.host == std::optional<std::string>{"archive-host"}; }));
	UAM_ASSERT(uam::ManagedSubagentControlService::ToJson(read.page).dump().find(std::string(100, 'P')) == std::string::npos);
}

UAM_TEST(ManagedSubagentRestartRequiredEffortInheritanceIsScopedToEffectiveModel)
{
	for (const std::string provider : {"gemini-cli", "codex-cli"})
	{
		for (bool same_model : {false, true})
		{
			ManagedControlFixture fixture;
			const auto workspace = fixture.temp.root / "workspace";
			const auto agents = workspace / ".uam" / "agents";
			fs::create_directories(agents);
			UAM_ASSERT(uam::io::WriteTextFile(agents / "worker.md",
			    "---\nversion: 1\nname: worker\ndescription: Worker\nmode: subagent\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nInspect.\n"));
			fixture.app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			auto& root = fixture.app.chats.front();
			root.provider_id = provider;
			root.model_id = provider == "gemini-cli" ? "gemini-3-pro-preview" : "parent-model";
			root.reasoning_effort = "high";
			root.workspace_directory = workspace.string();
			const auto requested = same_model ? root.model_id : "different-model";
			const auto parent_model = root.model_id;
			std::string run_id, error;
			UAM_ASSERT(uam::AgentRunScheduler::Enqueue(fixture.app, "root", {}, "worker", "Inspect.", &run_id, &error, provider, requested));
			const auto& transcript = fixture.app.chats.back();
			UAM_ASSERT_EQ(transcript.model_id, requested);
			UAM_ASSERT_EQ(transcript.reasoning_effort, provider == "gemini-cli" && !same_model ? std::string{} : std::string("high"));
			const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, transcript.id);
			UAM_ASSERT(saved.has_value());
			UAM_ASSERT_EQ(saved->reasoning_effort, transcript.reasoning_effort);
			UAM_ASSERT_EQ(fixture.app.chats.front().model_id, parent_model);
			UAM_ASSERT_EQ(fixture.app.chats.front().reasoning_effort, std::string("high"));
		}
	}
}

UAM_TEST(ManagedSubagentFinishedEpochPersistsAndLegacyRecordsRemainUnknown)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "completed");
	auto& run = fixture.app.agent_runs.front();
	run.finished_at_epoch_ms = 1792141380123LL;
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	UAM_ASSERT_EQ(uam::AgentRunLedger::LoadAll(fixture.app.data_root).runs.front().finished_at_epoch_ms, run.finished_at_epoch_ms);
	UAM_ASSERT_EQ(uam::ManagedSubagentControlService::ToJson(fixture.List())["runs"][0]["finishedAtEpochMs"], nlohmann::json(run.finished_at_epoch_ms));
	const auto path = fixture.app.data_root / "agent-runs" / (run.id + ".json");
	auto json = nlohmann::json::parse(ReadFile(path));
	json.erase("finishedAtEpochMs");
	UAM_ASSERT(uam::io::WriteTextFile(path, json.dump()));
	UAM_ASSERT_EQ(uam::AgentRunLedger::LoadAll(fixture.app.data_root).runs.front().finished_at_epoch_ms, int64_t{0});
	run.finished_at_epoch_ms = -1;
	UAM_ASSERT(!uam::AgentRunLedger::Save(fixture.app.data_root, run));
	fs::remove(uam::io::MakeBackupPath(path));
	json["finishedAtEpochMs"] = -1;
	UAM_ASSERT(uam::io::WriteTextFile(path, json.dump()));
	const auto invalid = uam::AgentRunLedger::LoadAll(fixture.app.data_root);
	UAM_ASSERT(invalid.runs.empty());
	UAM_ASSERT(!invalid.errors.empty());
}

UAM_TEST(ManagedSubagentTerminalWritersCaptureEpochWithoutParsingLocalStrings)
{
	for (int mode = 0; mode < 4; ++mode)
	{
		ManagedControlFixture fixture;
		const auto run = fixture.Add("root", {}, mode == 0 ? "queued" : "running");
		fixture.app.chats.back().execution_host_id = "local";
		const auto before = uam::time::SystemEpochMillisecondsNow();
		if (mode < 2)
		{
			UAM_ASSERT(uam::AgentRunScheduler::CancelTree(fixture.app, run));
			UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("cancelled"));
		}
		if (mode == 2)
		{
			UAM_ASSERT(uam::AgentRunLedger::MarkNonterminalInterrupted(fixture.app.data_root, &fixture.app.agent_runs, "test_restart"));
			UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("interrupted"));
		}
		if (mode == 3)
		{
			fixture.app.agent_runs.front().expected_turn_serial = 1;
			UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, fixture.app.agent_runs.front()));
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = fixture.app.agent_runs.front().transcript_chat_id;
			session->managed_agent_run_id = run;
			session->provider_id = "codex-cli";
			session->last_settled_turn_serial = 1;
			session->last_turn_outcome = "completed";
			fixture.app.acp_sessions.push_back(std::move(session));
			UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(fixture.app, 1000));
			UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("completed"));
		}
		const auto after = uam::time::SystemEpochMillisecondsNow();
		const auto finished = fixture.app.agent_runs.front().finished_at_epoch_ms;
		UAM_ASSERT(finished >= before && finished <= after);
		UAM_ASSERT_EQ(uam::AgentRunLedger::LoadAll(fixture.app.data_root).runs.front().finished_at_epoch_ms, finished);
		const auto expected_local = uam::time::detail::FormatTimestamp(
		    uam::time::detail::LocalTimeSnapshot(static_cast<std::time_t>(finished / 1000)), uam::time::kLocalDisplayTimestampFormat);
		UAM_ASSERT_EQ(fixture.app.agent_runs.front().finished_at, expected_local);
	}
}

UAM_TEST(ManagedSubagentResumeResetsFinishedEpochOnFreshQueuedRun)
{
	ManagedControlFixture fixture;
	const auto old = fixture.Add("root", {}, "interrupted");
	fixture.app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	fixture.app.agent_runs.front().finished_at_epoch_ms = 1792141380123LL;
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, fixture.app.agent_runs.front()));
	std::string resumed, error;
	UAM_ASSERT(uam::AgentRunScheduler::ResumeInterrupted(fixture.app, old, &resumed, &error));
	UAM_ASSERT_EQ(fixture.app.agent_runs.back().status, std::string("queued"));
	UAM_ASSERT(fixture.app.agent_runs.back().finished_at.empty());
	UAM_ASSERT_EQ(fixture.app.agent_runs.back().finished_at_epoch_ms, int64_t{0});
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().finished_at_epoch_ms, int64_t{1792141380123LL});
}

UAM_TEST(ManagedSubagentStaleGuiReceiptCannotWriteShutdownStateIntoReplacementRoot)
{
	ManagedControlFixture fixture;
	const auto run = fixture.Add();
	const auto scope = fixture.List().scope;
	fixture.app.pending_chat_save_at_by_chat_id[fixture.app.agent_runs.front().transcript_chat_id] = 1.0;
	UAM_ASSERT(!fixture.Cancel(run, scope));
	fixture.app.pending_chat_save_at_by_chat_id.clear();
	fixture.app.data_root = fixture.temp.root / "replacement-data";
	(void)uam::AgentRunScheduler::InterruptForShutdown(fixture.app);
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("queued"));
	UAM_ASSERT(!fs::exists(fixture.app.data_root / "agent-runs"));
}

UAM_TEST(ManagedSubagentInternalRemoteCancellationRetainsExistingSaveAndStopRetry)
{
	ManagedControlFixture fixture;
	const auto run = fixture.Add("root", {}, "running");
	fixture.app.chats.front().goal_owner_chat_id = "visible-owner";
	auto& transcript = fixture.app.chats.back();
	transcript.execution_host_id = "ssh-test";
	transcript.remote_turn_reconnect_pending = true;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, transcript));
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = transcript.id;
	session->managed_agent_run_id = run;
	session->managed_launch_attempted = true;
	session->running = session->processing = true;
	session->prompt_request_id = 7;
	std::string process_error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {"cmd.exe", "/d", "/s", "/c", "set /p line= & exit /b 0"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "IFS= read -r line; exit 0"};
#endif
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*session, fixture.temp.root, argv, &process_error));
	fixture.app.acp_sessions.push_back(std::move(session));
	const auto original_root = fixture.app.data_root;
	const auto blocked = fixture.temp.root / "blocked-data-root";
	UAM_ASSERT(uam::io::WriteTextFile(blocked, "blocked"));
	fixture.app.data_root = blocked;
	UAM_ASSERT(!uam::AgentRunScheduler::CancelTree(fixture.app, run));
	UAM_ASSERT(fixture.app.acp_sessions.front()->managed_cancellation_pending);
	UAM_ASSERT(fixture.app.managed_subagent_cancellation_receipts.empty());
	for (int attempt = 0; attempt < 100 && !fixture.app.pending_acp_remote_stops.empty(); ++attempt)
	{
		(void)uam::PollAllAcpSessions(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains(transcript.id));
	UAM_ASSERT(!uam::AgentRunScheduler::PollAtForTests(fixture.app, 1000));
	fixture.app.data_root = original_root;
	uam::FlushPendingChatSaves(fixture.app, true);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1001);
	for (int attempt = 0; attempt < 100 && !fixture.app.pending_acp_remote_stops.empty(); ++attempt)
	{
		(void)uam::PollAllAcpSessions(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(fixture.app, 1002));
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("cancelled"));
	UAM_ASSERT(fixture.app.managed_subagent_cancellation_receipts.empty());
	UAM_ASSERT(fixture.app.agent_runs.front().finished_at_epoch_ms > 0);
}

UAM_TEST(ManagedSubagentReadOnlyLedgerCannotRollBackConcurrentTerminalSave)
{
	for (bool unrelated_root : {false, true})
	{
		ManagedControlFixture fixture;
		const auto owned = fixture.Add("root", {}, "running");
		const auto foreign = fixture.Add("other-root", {}, "running");
		const auto target = unrelated_root ? foreign : owned;
		const auto path = fixture.app.data_root / "agent-runs" / (target + ".json");
		const auto initial = ReadFile(path);
		uam::ManagedSubagentSnapshot snapshot;
		UAM_ASSERT(uam::ManagedSubagentControlService::Capture(fixture.app, "root", &snapshot));
		std::promise<void> sampled;
		auto sample_ready = sampled.get_future();
		std::promise<void> release;
		auto released = release.get_future().share();
		auto reader = std::async(std::launch::async, [snapshot, path, &sampled, released]()
		{
			return uam::ManagedSubagentControlService::ReadDetached(snapshot, false, [&](const fs::path& primary)
			{
				if (primary != path) return;
				sampled.set_value();
				released.wait();
			});
		});
		struct ReleaseOnExit
		{
			std::promise<void>& promise;
			bool done = false;
			void Release() { if (!done) { promise.set_value(); done = true; } }
			~ReleaseOnExit() { Release(); }
		} unblock{release};
		UAM_ASSERT(sample_ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		// Execute the real scheduler terminal writer while the actual listing read is held
		// between its size admission and primary read. The backup is now the old running run.
		const bool cancelled = uam::AgentRunScheduler::CancelTree(fixture.app, target);
		const auto newest = ReadFile(path);
		const auto backup = ReadFile(uam::io::MakeBackupPath(path));
		unblock.Release();
		const auto read = reader.get();
		UAM_ASSERT(cancelled);
		UAM_ASSERT(!read.ready);
		UAM_ASSERT(newest.size() != initial.size());
		UAM_ASSERT_EQ(backup, initial);
		UAM_ASSERT_EQ(ReadFile(path), newest);
		UAM_ASSERT_EQ(ReadFile(uam::io::MakeBackupPath(path)), backup);
		const auto durable = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
		UAM_ASSERT(durable.errors.empty());
		const auto saved = std::ranges::find(durable.runs, target, &AgentRun::id);
		UAM_ASSERT(saved != durable.runs.end());
		UAM_ASSERT_EQ(saved->status, std::string("cancelled"));
		UAM_ASSERT_EQ(saved->root_chat_id, std::string(unrelated_root ? "other-root" : "root"));
		UAM_ASSERT(saved->finished_at_epoch_ms > 0);
		const auto live = std::ranges::find(fixture.app.agent_runs, target, &AgentRun::id);
		UAM_ASSERT_EQ(saved->finished_at_epoch_ms, live->finished_at_epoch_ms);
	}
}

UAM_TEST(ManagedSubagentReadOnlyLedgerNeverRepairsCorruptPrimaryButStartupStillRecovers)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "running");
	const auto run = fixture.app.agent_runs.front();
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	const auto path = fixture.app.data_root / "agent-runs" / (run.id + ".json");
	const auto backup = ReadFile(uam::io::MakeBackupPath(path));
	UAM_ASSERT(uam::io::WriteTextFile(path, "broken primary"));
	const auto readonly = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
	UAM_ASSERT(readonly.runs.empty());
	UAM_ASSERT(!readonly.errors.empty());
	UAM_ASSERT_EQ(ReadFile(path), std::string("broken primary"));
	UAM_ASSERT_EQ(ReadFile(uam::io::MakeBackupPath(path)), backup);
	const auto recovered = uam::AgentRunLedger::LoadAll(fixture.app.data_root);
	UAM_ASSERT_EQ(recovered.runs.size(), std::size_t{1});
	UAM_ASSERT_EQ(ReadFile(path), backup);
}

UAM_TEST(ManagedSubagentFreshExplicitStopReplacesStaleReceiptAndRejectsPendingDelegation)
{
	for (bool legacy_tree_growth : {false, true})
	{
		ManagedControlFixture fixture;
		const auto workspace = fixture.temp.root / "workspace";
		const auto agents = workspace / ".uam" / "agents";
		fs::create_directories(agents);
		UAM_ASSERT(uam::io::WriteTextFile(agents / "child.md",
		    "---\nversion: 1\nname: child\ndescription: Child\nmode: subagent\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nInspect.\n"));
		fixture.app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		fixture.app.chats.front().provider_id = "codex-cli";
		fixture.app.chats.front().workspace_directory = workspace.string();
		fixture.app.chats.front().created_at = "1970-01-01 00:00:00";
		const auto parent = fixture.Add("root", {}, "running");
		fixture.app.agent_runs.front().delegates_snapshot = {"child"};
		UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, fixture.app.agent_runs.front()));
		// Establish that the real Enqueue route admits this saved delegate before Stop.
		std::string child, error;
		UAM_ASSERT(uam::AgentRunScheduler::Enqueue(fixture.app, "root", parent, "child", "Inspect.", &child, &error, "codex-cli"));
		const auto original_scope = fixture.List().scope;
		for (const auto& run : fixture.app.agent_runs)
			fixture.app.pending_chat_save_at_by_chat_id[run.transcript_chat_id] = 1.0;
		UAM_ASSERT(!fixture.Cancel(parent, original_scope));
		const auto count = fixture.app.agent_runs.size();
		std::string denied;
		UAM_ASSERT(!uam::AgentRunScheduler::Enqueue(fixture.app, "root", parent, "child", "Must not launch.", &denied, &error, "codex-cli"));
		UAM_ASSERT(denied.empty());
		UAM_ASSERT_EQ(fixture.app.agent_runs.size(), count);
		UAM_ASSERT(error.find("stopping") != std::string::npos);
		fixture.app.pending_chat_save_at_by_chat_id.clear();
		if (legacy_tree_growth)
			fixture.Add("root", parent, "completed"); // already-existing stale tree from an older admission
		else
			fixture.app.chats.back().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		UAM_ASSERT(!uam::ManagedSubagentControlService::ValidateCancellationOwnership(fixture.app, parent));
		(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1000);
		UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("running"));
		UAM_ASSERT(!fixture.Cancel(parent, original_scope)); // old explicit scope remains stale too
		const auto fresh = fixture.List();
		const auto row = std::ranges::find(fresh.runs, parent, &uam::ManagedSubagentRun::run_id);
		UAM_ASSERT(row != fresh.runs.end() && row->can_cancel);
		UAM_ASSERT(fixture.Cancel(parent, fresh.scope));
		UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("cancelled"));
		UAM_ASSERT_EQ(fixture.app.agent_runs[1].status, std::string("cancelled"));
		UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.empty());
		UAM_ASSERT(fixture.app.managed_subagent_cancellation_receipts.empty());
		UAM_ASSERT(fixture.app.queued_agent_run_ids.empty());
		UAM_ASSERT(fixture.app.acp_sessions.empty());
		const auto durable = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
		const auto saved = std::ranges::find(durable.runs, parent, &AgentRun::id);
		UAM_ASSERT(saved != durable.runs.end());
		UAM_ASSERT_EQ(saved->status, std::string("cancelled"));
		UAM_ASSERT(saved->finished_at_epoch_ms > 0);
	}
}

UAM_TEST(ManagedSubagentRevisionSignalsOnlyCommittedOwnedRunChanges)
{
	ManagedControlFixture fixture;
	const auto workspace = fixture.temp.root / "workspace";
	const auto agents = workspace / ".uam" / "agents";
	fs::create_directories(agents);
	UAM_ASSERT(uam::io::WriteTextFile(agents / "worker.md",
	    "---\nversion: 1\nname: worker\ndescription: Worker\nmode: subagent\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nInspect.\n"));
	fixture.app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	for (auto& chat : fixture.app.chats)
	{
		chat.created_at = "1970-01-01 00:00:00";
		chat.provider_id = "codex-cli";
		chat.workspace_directory = workspace.string();
	}
	fixture.app.chats.shrink_to_fit(); // Enqueue must reacquire the root after possible vector growth.
	const auto revision = [&](const std::string& id)
	{
		return ChatDomainService().FindChatById(fixture.app, id)->managed_subagent_revision;
	};
	std::string run, foreign, error;
	UAM_ASSERT(!uam::AgentRunScheduler::Enqueue(fixture.app, "root", {}, "missing", "Inspect.", &run, &error));
	UAM_ASSERT_EQ(revision("root"), std::uint64_t{0});
	UAM_ASSERT(uam::AgentRunScheduler::Enqueue(fixture.app, "root", {}, "worker", "Inspect.", &run, &error));
	UAM_ASSERT_EQ(revision("root"), std::uint64_t{1});
	UAM_ASSERT_EQ(revision("other-root"), std::uint64_t{0});
	UAM_ASSERT(uam::AgentRunScheduler::Enqueue(fixture.app, "other-root", {}, "worker", "Inspect.", &foreign, &error));
	UAM_ASSERT_EQ(revision("root"), std::uint64_t{1});
	UAM_ASSERT_EQ(revision("other-root"), std::uint64_t{1});
	UAM_ASSERT(fixture.Cancel(run, fixture.List().scope));
	UAM_ASSERT_EQ(revision("root"), std::uint64_t{2});
	UAM_ASSERT_EQ(revision("other-root"), std::uint64_t{1});
	UAM_ASSERT(uam::AgentRunScheduler::CancelTree(fixture.app, run));
	UAM_ASSERT_EQ(revision("root"), std::uint64_t{2});
	const auto data_root = fixture.app.data_root;
	const auto blocked = fixture.temp.root / "blocked-root";
	UAM_ASSERT(uam::io::WriteTextFile(blocked, "blocked"));
	fixture.app.data_root = blocked;
	UAM_ASSERT(!uam::AgentRunScheduler::CancelTree(fixture.app, foreign));
	UAM_ASSERT_EQ(revision("other-root"), std::uint64_t{1});
	fixture.app.data_root = data_root;
	UAM_ASSERT(uam::AgentRunScheduler::CancelTree(fixture.app, foreign));
	UAM_ASSERT_EQ(revision("other-root"), std::uint64_t{2});
	UAM_ASSERT_EQ(revision("root"), std::uint64_t{2});
	const auto root = *ChatDomainService().FindChatById(fixture.app, "root");
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, root));
	const auto reloaded = ChatRepository::LoadLocalChat(fixture.app.data_root, "root");
	UAM_ASSERT(reloaded.has_value());
	UAM_ASSERT_EQ(reloaded->managed_subagent_revision, std::uint64_t{0});
}

UAM_TEST(ManagedSubagentRevisionIsFrontendScalarAndFingerprintInput)
{
	ManagedControlFixture fixture;
	fixture.app.chats.front().managed_subagent_revision = 7;
	const auto before = uam::StateSerializer::SerializeFingerprint(fixture.app);
	const auto root = uam::StateSerializer::SerializeSession(fixture.app.chats.front());
	const auto foreign = uam::StateSerializer::SerializeSession(fixture.app.chats[1]);
	UAM_ASSERT_EQ(root.at("managedSubagentRevision"), nlohmann::json(7));
	UAM_ASSERT_EQ(foreign.at("managedSubagentRevision"), nlohmann::json(0));
	fixture.app.chats.front().managed_subagent_revision = 8;
	UAM_ASSERT(before != uam::StateSerializer::SerializeFingerprint(fixture.app));
}

UAM_TEST(ManagedSubagentLazyRootHydrationPreservesEphemeralRevisionAndIncarnation)
{
	ManagedControlFixture fixture;
	auto& root = fixture.app.chats.front();
	root.managed_subagent_revision = 7;
	ChatDomainService().AddMessage(root, MessageRole::User, "Durable lazy-root history.");
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, root));
	const auto lazy = ChatRepository::LoadLocalChat(fixture.app.data_root, "root", false);
	UAM_ASSERT(lazy.has_value());
	UAM_ASSERT(!lazy->messages_loaded && lazy->messages.empty());
	UAM_ASSERT_EQ(lazy->managed_subagent_revision, std::uint64_t{0});
	root = *lazy;
	root.managed_subagent_revision = 7; // current manager's lifecycle signal on this lazy incarnation
	const auto incarnation = root.metadata_incarnation;
	fixture.app.chats[1].managed_subagent_revision = 3;
	std::string error;
	UAM_ASSERT(ChatRepository::HydrateChatMessages(fixture.app.data_root, root, &error));
	UAM_ASSERT(root.messages_loaded);
	UAM_ASSERT_EQ(root.messages.size(), std::size_t{1});
	UAM_ASSERT_EQ(root.messages.front().content, std::string("Durable lazy-root history."));
	UAM_ASSERT_EQ(root.metadata_incarnation, incarnation);
	UAM_ASSERT_EQ(root.managed_subagent_revision, std::uint64_t{7});
	UAM_ASSERT_EQ(fixture.app.chats[1].managed_subagent_revision, std::uint64_t{3});
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeSession(root).at("managedSubagentRevision"), nlohmann::json(7));
	const auto reloaded = ChatRepository::LoadLocalChat(fixture.app.data_root, "root");
	UAM_ASSERT(reloaded.has_value());
	UAM_ASSERT_EQ(reloaded->managed_subagent_revision, std::uint64_t{0});
	UAM_ASSERT(reloaded->metadata_incarnation != incarnation);
}

UAM_TEST(StateSerializerCountsManagedChildrenWithoutExposingHiddenChats)
{
	ManagedControlFixture fixture;
	const auto parent = fixture.Add("root", {}, "completed");
	fixture.Add("root", parent, "completed");
	fixture.Add("other-root", {}, "completed");
	for (const bool summary : {false, true})
	{
		const auto state = uam::StateSerializer::Serialize(fixture.app, summary);
		UAM_ASSERT_EQ(state.at("chats").size(), std::size_t{2});
		for (const auto& row : state.at("chats"))
		{
			const auto id = row.at("id").get<std::string>();
			UAM_ASSERT_EQ(row.at("managedSubagentCount"), nlohmann::json(id == "root" ? 2 : 1));
		}
		UAM_ASSERT(state.dump().find("PRIVATE-PROMPT") == std::string::npos);
		UAM_ASSERT(state.dump().find("PRIVATE-DEFINITION") == std::string::npos);
	}
	const auto before = uam::StateSerializer::SerializeFingerprint(fixture.app);
	UAM_ASSERT_EQ(before.at("chats").size(), std::size_t{2});
	for (const auto& row : before.at("chats"))
		UAM_ASSERT_EQ(row.at("managedSubagentCount"), nlohmann::json(row.at("id") == "root" ? 2 : 1));
	fixture.app.agent_runs.pop_back();
	const auto after = uam::StateSerializer::SerializeFingerprint(fixture.app);
	UAM_ASSERT(before != after);
	for (const auto& row : after.at("chats"))
		UAM_ASSERT_EQ(row.at("managedSubagentCount"), nlohmann::json(row.at("id") == "root" ? 2 : 0));
}

UAM_TEST(ManagedSubagentCardSummaryIsOwnedOutputWithoutPromptOrDiagnostic)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "completed");
	fixture.Add("other-root", {}, "completed");
	auto& visible = fixture.app.agent_runs.front();
	visible.result_excerpt = "Reviewed the changes.\nNo blocking findings.";
	visible.diagnostic = "PRIVATE-DIAGNOSTIC";
	fixture.app.agent_runs.back().result_excerpt = "PRIVATE-FOREIGN-RESULT";
	for (const auto& run : fixture.app.agent_runs) UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	const auto json = uam::ManagedSubagentControlService::ToJson(fixture.List());
	UAM_ASSERT_EQ(json.at("runs").size(), std::size_t(1));
	const auto& row = json.at("runs").front();
	UAM_ASSERT(row.contains("summary"));
	UAM_ASSERT_EQ(row.at("summary"), nlohmann::json("Reviewed the changes. No blocking findings."));
	UAM_ASSERT(json.dump().find("PRIVATE-") == std::string::npos);
}

UAM_TEST(ManagedSubagentCardSummaryCapsUtf8AtWholeCharacters)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "completed");
	auto& run = fixture.app.agent_runs.front();
	run.result_excerpt = std::string(1023, 'a') + "\xC3\xA9" + " trailing output";
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	const auto json = uam::ManagedSubagentControlService::ToJson(fixture.List());
	UAM_ASSERT(json.at("runs").front().contains("summary"));
	UAM_ASSERT_EQ(json.at("runs").front().at("summary"), nlohmann::json(std::string(1023, 'a')));
	UAM_ASSERT_EQ(nlohmann::json::parse(json.dump()), json);
}

UAM_TEST(ManagedSubagentCardSummaryRejectsChangedDetachedOutput)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "completed");
	auto& run = fixture.app.agent_runs.front();
	run.result_excerpt = "First completed result";
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	uam::ManagedSubagentSnapshot snapshot;
	std::string error;
	UAM_ASSERT(uam::ManagedSubagentControlService::Capture(fixture.app, "root", &snapshot, &error));
	run.result_excerpt = "Different completed result, unchanged timestamps";
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	UAM_ASSERT(!uam::ManagedSubagentControlService::ReadDetached(snapshot).ready);
	UAM_ASSERT(!uam::ManagedSubagentControlService::MatchesSnapshot(fixture.app, snapshot));
}

UAM_TEST(ManagedSubagentCardSummaryDoesNotUseTheTaskAsFallback)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "running");
	const auto json = uam::ManagedSubagentControlService::ToJson(fixture.List());
	UAM_ASSERT(json.at("runs").front().contains("summary"));
	UAM_ASSERT_EQ(json.at("runs").front().at("summary"), nlohmann::json(""));
	UAM_ASSERT(json.dump().find("PRIVATE-") == std::string::npos);
}

UAM_TEST(ManagedSubagentResultCardCarriesItsOwnRecordedTiming)
{
	ManagedControlFixture fixture;
	{
		// Use the real domain constructor before any child captures this parent.
		const std::string root_id = fixture.app.chats.front().id;
		ChatSession root_parent = ChatDomainService().CreateNewChat({}, "codex-cli", false);
		root_parent.id = root_id;
		root_parent.branch_root_chat_id = root_id;
		fixture.app.chats.front() = std::move(root_parent);
	}
	const auto run_id = fixture.Add("root", {}, "running");
	auto& run = fixture.app.agent_runs.front();
	run.expected_turn_serial = 1;
	run.deliver_result_to_root_chat = true;
	run.started_at_epoch_ms = uam::time::SystemEpochMillisecondsNow() - 5000;
	run.deadline_at_epoch_ms = run.started_at_epoch_ms + 1800000;
	fixture.app.chats.back().execution_host_id = "local";
	fixture.app.chats.back().messages.push_back(Message{MessageRole::Assistant, "Recorded child result"});
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = run.transcript_chat_id;
	session->managed_agent_run_id = run_id;
	session->provider_id = run.provider_id;
	session->last_settled_turn_serial = 1;
	session->last_turn_outcome = "completed";
	fixture.app.acp_sessions.push_back(std::move(session));
	{
		// Result publication requires the same persisted parent that the real domain creates.
		auto& root_parent = fixture.app.chats.front();
		root_parent.branch_root_chat_id = root_parent.id;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, root_parent));
		const auto root_revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, root_parent.id);
		UAM_ASSERT(root_revision.has_value());
		const auto reloaded_parent = ChatRepository::LoadLocalChat(fixture.app.data_root, root_parent.id, true, nullptr, false);
		UAM_ASSERT(reloaded_parent && reloaded_parent->messages_loaded);
		UAM_ASSERT_EQ(reloaded_parent->messages, root_parent.messages);
		UAM_ASSERT_EQ(reloaded_parent->branch_root_chat_id, root_parent.branch_root_chat_id);
		// Match the consumer's three load-cache projections without changing authored metadata.
		ChatSession persisted_parent = *reloaded_parent;
		persisted_parent.persisted_message_count = root_parent.persisted_message_count;
		persisted_parent.persisted_messages_digest = root_parent.persisted_messages_digest;
		persisted_parent.acp_queue_full_save_revision = root_parent.acp_queue_full_save_revision;
		UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, persisted_parent),
		    ChatRepository::ChatMetadataFingerprint(fixture.app.data_root, root_parent));
		UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, root_parent.id), root_revision);
	}
	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(fixture.app, 1000));
	UAM_ASSERT_EQ(run.status, std::string("completed"));
	UAM_ASSERT(run.root_result_delivered);
	const auto& root = fixture.app.chats.front();
	UAM_ASSERT(!root.messages.empty() && !root.messages.back().tool_calls.empty());
	const auto& tool = root.messages.back().tool_calls.front();
	UAM_ASSERT_EQ(tool.name, std::string("uam_agent_result"));
	const auto result = nlohmann::json::parse(tool.result_text);
	UAM_ASSERT(result.contains("startedAtEpochMs") && result.contains("finishedAtEpochMs"));
	UAM_ASSERT_EQ(result.at("runId"), nlohmann::json(run_id));
	UAM_ASSERT_EQ(result.at("providerId"), nlohmann::json("codex-cli"));
	UAM_ASSERT_EQ(result.at("result"), nlohmann::json("Recorded child result"));
	UAM_ASSERT_EQ(result.at("startedAtEpochMs"), nlohmann::json(run.started_at_epoch_ms));
	UAM_ASSERT_EQ(result.at("finishedAtEpochMs"), nlohmann::json(run.finished_at_epoch_ms));
	UAM_ASSERT_EQ(result.at("startedAt"), nlohmann::json(run.started_at));
	UAM_ASSERT_EQ(result.at("completedAt"), nlohmann::json(run.finished_at));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, root.id);
	UAM_ASSERT(saved && !saved->messages.empty() && !saved->messages.back().tool_calls.empty());
	UAM_ASSERT_EQ(nlohmann::json::parse(saved->messages.back().tool_calls.front().result_text), result);
}


namespace
{
	ToolCall RecordedManagedCardTool()
	{
		ToolCall tool;
		tool.id = "recorded-managed-card";
		tool.name = "uam_agent_result";
		tool.is_sub_agent = true;
		tool.sub_agent_id = "managed-card-run";
		tool.args_json = R"({"runId":"managed-card-run","task":"PRIVATE_TASK"})";
		tool.result_text = nlohmann::json{
			{"runId", tool.sub_agent_id}, {"providerId", "codex-cli"},
			{"agentId", "reviewer"}, {"status", "completed"},
			{"result", "Recorded output"}, {"diagnostic", "PRIVATE_DIAGNOSTIC"},
			{"transcriptChatId", "PRIVATE_TRANSCRIPT"},
			{"startedAtEpochMs", 1792173600000LL}, {"finishedAtEpochMs", 1792173690000LL},
			{"startedAt", "2026-10-16T18:00:00Z"}, {"completedAt", "2026-10-16T18:01:30Z"}}.dump();
		tool.status = "completed";
		return tool;
	}
}

UAM_TEST(ManagedCardMetadataSurvivesDeferredSavedToolContent)
{
	ChatSession chat;
	chat.id = "card-parent";
	Message message{MessageRole::Assistant, "Done"};
	message.tool_calls.push_back(RecordedManagedCardTool());
	chat.messages.push_back(message);
	const auto page = uam::StateSerializer::SerializeMessagePage(chat, 10, std::nullopt, true);
	const auto& tool = page.at("messages").at(0).at("toolCalls").at(0);
	UAM_ASSERT(tool.value("contentDeferred", false));
	UAM_ASSERT(!tool.contains("content"));
	UAM_ASSERT(tool.contains("subAgentResultContent"));
	const auto card = nlohmann::json::parse(tool.at("subAgentResultContent").get<std::string>());
	UAM_ASSERT_EQ(card.at("result"), nlohmann::json("Recorded output"));
	UAM_ASSERT_EQ(card.at("runId"), nlohmann::json("managed-card-run"));
	UAM_ASSERT_EQ(card.at("finishedAtEpochMs"), nlohmann::json(1792173690000LL));
	UAM_ASSERT(card.dump().find("PRIVATE_") == std::string::npos);
	const auto ordinary = uam::StateSerializer::SerializeSession(chat);
	UAM_ASSERT_EQ(ordinary.at("messages").at(0).at("toolCalls").at(0).at("subAgentResultContent"), tool.at("subAgentResultContent"));
	UAM_ASSERT(ordinary.at("messages").at(0).at("toolCalls").at(0).value("content", "").starts_with("Arguments:"));
}

UAM_TEST(ManagedCardMetadataCapsOutputAndRejectsUnrelatedResults)
{
	ChatSession chat;
	Message message{MessageRole::Assistant, "Done"};
	ToolCall tool = RecordedManagedCardTool();
	auto result = nlohmann::json::parse(tool.result_text);
	result["result"] = std::string(1023, 'x') + "\xF0\x9F\x98\x80";
	tool.result_text = result.dump();
	message.tool_calls.push_back(tool);
	ToolCall unrelated = tool;
	unrelated.sub_agent_id = "other-run";
	message.tool_calls.push_back(unrelated);
	unrelated = tool; unrelated.is_sub_agent = false;
	message.tool_calls.push_back(unrelated);
	unrelated = tool; unrelated.name = "ordinary-tool";
	message.tool_calls.push_back(unrelated);
	unrelated = tool; unrelated.result_text = "{";
	message.tool_calls.push_back(unrelated);
	unrelated = tool; unrelated.result_text.assign(65537, 'x');
	message.tool_calls.push_back(unrelated);
	chat.messages.push_back(message);
	const auto serialized = uam::StateSerializer::SerializeSession(chat);
	const auto& tools = serialized.at("messages").at(0).at("toolCalls");
	UAM_ASSERT(tools.at(0).contains("subAgentResultContent"));
	const auto card = nlohmann::json::parse(tools.at(0).at("subAgentResultContent").get<std::string>());
	UAM_ASSERT_EQ(card.at("result").get<std::string>(), std::string(1023, 'x'));
	UAM_ASSERT(tools.at(0).at("subAgentResultContent").get<std::string>().size() <= 8192);
	for (std::size_t i = 1; i < tools.size(); ++i) UAM_ASSERT(!tools.at(i).contains("subAgentResultContent"));
}

UAM_TEST(ManagedCardMetadataProjectsLiveResultWithoutPrivateFields)
{
	uam::AppState app;
	ChatSession chat; chat.id = "live-card-parent";
	app.chats.push_back(chat); app.selected_chat_index = 0;
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	const auto saved = RecordedManagedCardTool();
	uam::AcpToolCallState tool;
	tool.id = saved.id; tool.title = saved.name; tool.status = saved.status;
	tool.is_sub_agent = true; tool.sub_agent_id = saved.sub_agent_id;
	tool.content = saved.result_text;
	session->tool_calls.push_back(tool); app.acp_sessions.push_back(std::move(session));
	const auto state = uam::StateSerializer::Serialize(app);
	const auto& projected = state.at("chats").at(0).at("acpSession").at("toolCalls").at(0);
	UAM_ASSERT(projected.contains("subAgentResultContent"));
	const auto card = nlohmann::json::parse(projected.at("subAgentResultContent").get<std::string>());
	UAM_ASSERT_EQ(card.at("providerId"), nlohmann::json("codex-cli"));
	UAM_ASSERT(card.dump().find("PRIVATE_") == std::string::npos);
}

UAM_TEST(ManagedSubagentListEmitsFirstTaskLineAsTitleWithoutTheRestOfThePromptOrResult)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "completed");
	fixture.Add("other-root", {}, "completed");
	auto& visible = fixture.app.agent_runs.front();
	visible.result_excerpt = "Reviewed. PRIVATE-RESULT stays only in the summary.";
	visible.diagnostic = "PRIVATE-DIAGNOSTIC";
	for (const auto& run : fixture.app.agent_runs) UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	const auto json = uam::ManagedSubagentControlService::ToJson(fixture.List());
	UAM_ASSERT_EQ(json.at("runs").size(), std::size_t(1));
	const auto& row = json.at("runs").front();
	UAM_ASSERT_EQ(row.at("taskTitle"), nlohmann::json("Review the sidebar diff."));
	UAM_ASSERT_EQ(row.at("agentId"), nlohmann::json("worker"));
	UAM_ASSERT_EQ(row.at("summary"), nlohmann::json("Reviewed. PRIVATE-RESULT stays only in the summary."));
	UAM_ASSERT(row.at("taskTitle").get<std::string>().find("PRIVATE-") == std::string::npos);
	UAM_ASSERT(row.at("summary").get<std::string>().find("Review the sidebar diff") == std::string::npos);
	UAM_ASSERT(json.dump().find("PRIVATE-PROMPT") == std::string::npos);
	UAM_ASSERT(json.dump().find("PRIVATE-DIAGNOSTIC") == std::string::npos);
	UAM_ASSERT_EQ(nlohmann::json::parse(json.dump()), json);
}

UAM_TEST(ManagedSubagentTaskTitleKeepsBlankFallbackAndOrdinaryPasswordPurpose)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "running");
	auto& run = fixture.app.agent_runs.front();
	for (const auto& [task, expected] : std::vector<std::pair<std::string, std::string>>{{"   \n\t \n", ""}, {"Implement password reset\nPRIVATE-PROMPT", "Implement password reset"}})
	{
		run.task = task;
		UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
		const auto json = uam::ManagedSubagentControlService::ToJson(fixture.List());
		UAM_ASSERT_EQ(json.at("runs").front().at("taskTitle"), nlohmann::json(expected));
		UAM_ASSERT(json.dump().find("PRIVATE-PROMPT") == std::string::npos);
	}
}

UAM_TEST(ManagedSubagentTaskTitleStopsAtFirstLineAndSentenceAndNormalizesControls)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "running");
	auto& run = fixture.app.agent_runs.front();
	const auto title = [&](const std::string& task)
	{
		run.task = task;
		UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
		return uam::ManagedSubagentControlService::ToJson(fixture.List()).at("runs").front().at("taskTitle");
	};
	UAM_ASSERT_EQ(title("\r\n  Audit the parser\tcarefully\nSecond line"), nlohmann::json("Audit the parser carefully"));
	UAM_ASSERT_EQ(title("Audit the whole parser module now. Then fix every bug."), nlohmann::json("Audit the whole parser module now."));
	UAM_ASSERT_EQ(title("Fix e.g. the parser"), nlohmann::json("Fix e.g. the parser"));
}

UAM_TEST(ManagedSubagentTaskTitleCapsAtCodePointsAndBytesOnWholeCharacters)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "running");
	auto& run = fixture.app.agent_runs.front();
	const auto title = [&](const std::string& task)
	{
		run.task = task;
		UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
		return uam::ManagedSubagentControlService::ToJson(fixture.List()).at("runs").front().at("taskTitle").get<std::string>();
	};
	UAM_ASSERT_EQ(title(std::string(500, 'a')), std::string(100, 'a'));
	// 100 four-byte characters would be 400 bytes: the 256-byte cap keeps 64 whole characters.
	std::string emoji;
	for (int i = 0; i < 100; ++i) emoji += "\xF0\x9F\x98\x80";
	UAM_ASSERT_EQ(title(emoji).size(), std::size_t(256));
	std::string two_byte;
	for (int i = 0; i < 200; ++i) two_byte += "\xC3\xA9";
	UAM_ASSERT_EQ(title(two_byte).size(), std::size_t(200));
}

UAM_TEST(ManagedSubagentTaskTitleKeepsOnlyStrictlyValidUtf8SoTheDtoIsAlwaysValidJson)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "running");
	auto& run = fixture.app.agent_runs.front();
	const std::vector<std::pair<std::string, std::string>> cases{
	    {"ok \x80 tail", "ok"}, {"ok \xC0\xAF tail", "ok"}, {"ok \xE0\x80\x80 tail", "ok"},
	    {"ok \xED\xA0\x80 tail", "ok"}, {"ok \xF4\x90\x80\x80 tail", "ok"},
	    {"ok \xF8\x88\x80\x80\x80 tail", "ok"}, {"ok \xE2\x82", "ok"}};
	for (const auto& [task, expected] : cases)
	{
		run.task = task; // Corrupt UTF-8 must not be written through the strict ledger serializer.
		uam::ManagedSubagentSnapshot snapshot;
		UAM_ASSERT(uam::ManagedSubagentControlService::Capture(fixture.app, "root", &snapshot));
		const auto detached = uam::ManagedSubagentControlService::ReadDetached(snapshot);
		UAM_ASSERT(!detached.ready); // Saved task differs: formatting-only, never an accepted owned read.
		UAM_ASSERT_EQ(detached.page.runs.size(), std::size_t{1});
		UAM_ASSERT_EQ(detached.page.runs.front().task_title, expected);
		const auto json = uam::ManagedSubagentControlService::ToJson(detached.page);
		const auto text = json.dump();
		UAM_ASSERT_EQ(nlohmann::json::parse(text), json);
		UAM_ASSERT_EQ(json.at("runs").front().at("taskTitle").get<std::string>(), expected);
	}
	run.task = "caf\xC3\xA9 \xF0\x9F\x98\x80";
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	const auto valid = uam::ManagedSubagentControlService::ToJson(fixture.List());
	UAM_ASSERT_EQ(valid.at("runs").front().at("taskTitle").get<std::string>(), run.task);
	UAM_ASSERT_EQ(nlohmann::json::parse(valid.dump()), valid);
}

UAM_TEST(ManagedSubagentDetachedReadRequiresTheSavedTaskToProduceTheSameTitle)
{
	ManagedControlFixture fixture;
	fixture.Add("root", {}, "running");
	auto& run = fixture.app.agent_runs.front();
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	uam::ManagedSubagentSnapshot snapshot;
	std::string error;
	UAM_ASSERT(uam::ManagedSubagentControlService::Capture(fixture.app, "root", &snapshot, &error));
	const auto ready = uam::ManagedSubagentControlService::ReadDetached(snapshot);
	UAM_ASSERT(ready.ready);
	UAM_ASSERT_EQ(ready.page.runs.front().task_title, std::string("Review the sidebar diff."));
	run.task = "A different task rewritten on disk only";
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	UAM_ASSERT(!uam::ManagedSubagentControlService::ReadDetached(snapshot).ready);
}

// DATA-only ordering control. Disconnected metadata is not live process ownership evidence.
UAM_TEST(ManagedSubagentDeadlineRefusalKeepsDisconnectedRunNonterminal)
{
	ManagedControlFixture fixture;
	fixture.app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
	const std::string run_id = fixture.Add("root", {}, "running");
	AgentRun& run = fixture.app.agent_runs.front();
	run.expected_turn_serial = 1;
	run.deliver_result_to_root_chat = true;
	UAM_ASSERT(!run.managed_continuation && run.finished_at.empty() && !run.root_result_delivered);
	const int64_t deadline = run.deadline_at_epoch_ms;
	UAM_ASSERT(deadline > run.started_at_epoch_ms);
	fixture.app.agent_run_deadline_steady_ms[run_id] = deadline;
	ChatSession& root = fixture.app.chats.front();
	ChatSession& transcript = fixture.app.chats.back();
	root.messages_loaded = true;
	transcript.messages_loaded = true;
	ChatDomainService().AddMessage(root, MessageRole::User, "Keep the managed result pending until retirement is confirmed.");
	ChatDomainService().AddMessage(transcript, MessageRole::Assistant, "Private retained DATA excerpt.");
	transcript.remote_turn_reconnect_pending = true;
	UAM_ASSERT(transcript.execution_host_id != uam::execution_hosts::kLocalHostId);
	UAM_ASSERT(root.goals.empty() && transcript.goals.empty());
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, root));
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, transcript));
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
	const auto saved_before = uam::AgentRunLedger::LoadAll(fixture.app.data_root).runs;
	UAM_ASSERT_EQ(saved_before.size(), std::size_t{1});
	UAM_ASSERT_EQ(saved_before.front().status, std::string("running"));
	const auto root_before = ChatRepository::LoadLocalChat(fixture.app.data_root, root.id, true, nullptr, false);
	const auto transcript_before = ChatRepository::LoadLocalChat(fixture.app.data_root, transcript.id, true, nullptr, false);
	UAM_ASSERT(root_before && transcript_before);
	UAM_ASSERT_EQ(root_before->messages, root.messages);
	UAM_ASSERT_EQ(transcript_before->messages, transcript.messages);
	UAM_ASSERT(transcript_before->remote_turn_reconnect_pending);
	const auto root_messages = root.messages;
	const std::string root_id = root.id;
	const std::string transcript_id = transcript.id;
	const std::size_t chat_count = fixture.app.chats.size();

	auto inert = std::make_unique<uam::AcpSessionState>();
	inert->chat_id = transcript_id;
	inert->provider_id = run.provider_id;
	inert->managed_agent_run_id = run_id;
	inert->processing = true;
	inert->reconnect_pending = true;
	inert->recovering_remote_turn = true;
	// No managed_launch_attempted, process identity, transport, Stop task or native RPC is invented.
	uam::AcpSessionState* const captured = inert.get();
	fixture.app.acp_sessions.push_back(std::move(inert));
	const auto require_inert = [&]()
	{
		UAM_ASSERT_EQ(fixture.app.acp_sessions.size(), std::size_t{1});
		const auto& session = *fixture.app.acp_sessions.front();
		UAM_ASSERT(&session == captured);
		UAM_ASSERT(!session.running && session.processing && !session.managed_launch_attempted);
		UAM_ASSERT(session.session_id.empty() && session.codex_thread_id.empty() && session.codex_turn_id.empty());
		UAM_ASSERT(session.last_process_id.empty() && session.process_execution_host_id.empty());
		UAM_ASSERT(!session.stdin_writer && !session.uam_control_relay && !session.opencode_native_companion);
		UAM_ASSERT(!session.local_stop_pending && !session.remote_stop_pending && !session.remote_stop_unconfirmed);
		UAM_ASSERT(!session.managed_initial_owner && !session.managed_parent_continuation);
		UAM_ASSERT(!session.pending_local_launch && !session.pending_agent_input && !session.worktree_launch_pending);
		UAM_ASSERT(session.queued_user_prompts.empty() && session.queued_prompt.empty() && session.prompt_request_id == 0);
#if defined(_WIN32)
		UAM_ASSERT(session.stdin_write == INVALID_HANDLE_VALUE && session.stdout_read == INVALID_HANDLE_VALUE && session.stderr_read == INVALID_HANDLE_VALUE);
		UAM_ASSERT(session.process_info.hProcess == INVALID_HANDLE_VALUE && session.process_info.hThread == INVALID_HANDLE_VALUE);
		UAM_ASSERT(session.process_info.dwProcessId == 0 && session.process_info.dwThreadId == 0 && session.job_object == nullptr);
#else
		UAM_ASSERT(session.stdin_write_fd == -1 && session.stdout_read_fd == -1 && session.stderr_read_fd == -1);
		UAM_ASSERT(session.child_pid == -1 && session.watchdog_pid == -1);
#endif
		UAM_ASSERT(fixture.app.queued_agent_run_ids.empty() && fixture.app.pending_agent_run_cancellation_ids.empty());
		UAM_ASSERT(fixture.app.managed_initial_mentions.empty() && fixture.app.managed_initial_runtime_owners.empty() && fixture.app.managed_followups.empty());
		UAM_ASSERT(fixture.app.branch_preparations.empty() && fixture.app.local_acp_launch_tasks.empty() && fixture.app.agent_input_preparation_tasks.empty());
		UAM_ASSERT(fixture.app.pending_acp_remote_stops.empty() && fixture.app.acp_process_stop_tasks.empty());
		UAM_ASSERT(fixture.app.remote_context_tasks.empty() && fixture.app.memory_recall_tasks.empty() && fixture.app.claude_input_tasks.empty());
	};
	require_inert();
	UAM_ASSERT_EQ(run.status, std::string("running"));
	UAM_ASSERT(!run.root_result_delivered && run.root_result_delivery_attempts == 0);
	// Exactly one ordinary scheduler pass; no runtime polling, transport or cleanup helpers.
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, deadline);
	// These are the intended baseline RED assertions, after genuine saved-data prerequisites above.
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("running"));
	UAM_ASSERT(fixture.app.agent_runs.front().finished_at.empty());
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().finished_at_epoch_ms, int64_t{0});
	UAM_ASSERT(!fixture.app.agent_runs.front().root_result_delivered);
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().root_result_delivery_attempts, 0);
	UAM_ASSERT(fixture.app.agent_run_deadline_steady_ms.contains(run_id));
	UAM_ASSERT_EQ(fixture.app.chats.size(), chat_count);
	UAM_ASSERT_EQ(fixture.app.chats.front().id, root_id);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages, root_messages);
	UAM_ASSERT_EQ(fixture.app.chats.back().id, transcript_id);
	UAM_ASSERT(fixture.app.chats.back().remote_turn_reconnect_pending);
	require_inert();
	const auto saved_after = uam::AgentRunLedger::LoadAll(fixture.app.data_root).runs;
	UAM_ASSERT_EQ(saved_after.size(), std::size_t{1});
	UAM_ASSERT_EQ(saved_after.front().status, std::string("running"));
	UAM_ASSERT(!saved_after.front().root_result_delivered && saved_after.front().root_result_delivery_attempts == 0);
	UAM_ASSERT(saved_after.front().finished_at.empty() && saved_after.front().finished_at_epoch_ms == 0);
	const auto root_after = ChatRepository::LoadLocalChat(fixture.app.data_root, root_id, true, nullptr, false);
	const auto transcript_after = ChatRepository::LoadLocalChat(fixture.app.data_root, transcript_id, true, nullptr, false);
	UAM_ASSERT(root_after && transcript_after);
	UAM_ASSERT_EQ(root_after->messages, root_messages);
	UAM_ASSERT_EQ(transcript_after->messages, transcript_before->messages);
	UAM_ASSERT(transcript_after->remote_turn_reconnect_pending);
}

namespace
{
	struct DeadlinePublicationDataFixture : ManagedControlFixture
	{
		std::string run_id, transcript_id;
		const std::string excerpt = "Retained deadline output.\nExact second line.";
		int64_t deadline = 0;
		explicit DeadlinePublicationDataFixture(bool disconnected = false)
		{
			ChatSession parent = ChatDomainService().CreateNewChat({}, "codex-cli", false);
			parent.id = "root";
			parent.branch_root_chat_id = parent.id;
			app.chats.front() = std::move(parent);
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			run_id = Add("root", {}, "running");
			auto& run = app.agent_runs.front();
			run.expected_turn_serial = 1;
			run.deliver_result_to_root_chat = true;
			deadline = run.deadline_at_epoch_ms;
			app.agent_run_deadline_steady_ms[run_id] = deadline;
			auto& transcript = app.chats.back();
			transcript_id = transcript.id;
			transcript.execution_host_id = disconnected ? "inert-disconnected-host" : "local";
			transcript.remote_turn_reconnect_pending = disconnected;
			ChatDomainService().AddMessage(transcript, MessageRole::Assistant, excerpt);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, transcript));
			UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
			const auto loaded = ChatRepository::LoadLocalChat(app.data_root, "root", true, nullptr, false);
			UAM_ASSERT(loaded && loaded->messages_loaded);
			ChatSession comparable = *loaded;
			comparable.persisted_message_count = app.chats.front().persisted_message_count;
			comparable.persisted_messages_digest = app.chats.front().persisted_messages_digest;
			comparable.acp_queue_full_save_revision = app.chats.front().acp_queue_full_save_revision;
			UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, comparable), ChatRepository::ChatMetadataFingerprint(app.data_root, app.chats.front()));
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = transcript_id;
			session->provider_id = run.provider_id;
			session->managed_agent_run_id = run_id;
			session->processing = disconnected;
			session->reconnect_pending = disconnected;
			session->recovering_remote_turn = disconnected;
			app.acp_sessions.push_back(std::move(session));
			RequireInert();
		}
		void RequireInert() const
		{
			UAM_ASSERT(app.pending_acp_remote_stops.empty() && app.acp_process_stop_tasks.empty());
			UAM_ASSERT(app.queued_agent_run_ids.empty() && app.managed_initial_mentions.empty() && app.managed_initial_runtime_owners.empty() && app.managed_followups.empty());
			UAM_ASSERT(app.local_acp_launch_tasks.empty() && app.agent_input_preparation_tasks.empty());
			for (const auto& session : app.acp_sessions)
			{
				UAM_ASSERT(session && !session->running && !session->managed_launch_attempted && !session->stdin_writer);
				UAM_ASSERT(session->session_id.empty() && session->last_process_id.empty() && session->process_execution_host_id.empty());
				UAM_ASSERT(!session->uam_control_relay && !session->managed_initial_owner && !session->managed_parent_continuation);
#if defined(_WIN32)
				UAM_ASSERT(session->stdin_write == INVALID_HANDLE_VALUE && session->stdout_read == INVALID_HANDLE_VALUE && session->stderr_read == INVALID_HANDLE_VALUE);
				UAM_ASSERT(session->process_info.hProcess == INVALID_HANDLE_VALUE && session->process_info.hThread == INVALID_HANDLE_VALUE && session->job_object == nullptr);
#else
				UAM_ASSERT(session->stdin_write_fd == -1 && session->stdout_read_fd == -1 && session->stderr_read_fd == -1 && session->child_pid == -1 && session->watchdog_pid == -1);
#endif
			}
		}
		void RequireDelivery() const
		{
			const auto& run = app.agent_runs.front();
			UAM_ASSERT_EQ(run.status, std::string("failed"));
			UAM_ASSERT_EQ(run.diagnostic_code, std::string("wall_clock_deadline_exceeded"));
			UAM_ASSERT_EQ(run.result_excerpt, excerpt);
			UAM_ASSERT(run.root_result_delivered && run.root_result_delivery_attempts == 1);
			UAM_ASSERT(!run.finished_at.empty() && run.finished_at_epoch_ms > 0);
			UAM_ASSERT(!app.agent_run_deadline_steady_ms.contains(run_id));
			UAM_ASSERT(app.acp_sessions.empty());
			UAM_ASSERT(!std::ranges::any_of(app.chats, [&](const ChatSession& chat) { return chat.id == transcript_id; }));
			const auto saved = uam::AgentRunLedger::LoadAllReadOnly(app.data_root);
			UAM_ASSERT(saved.errors.empty() && saved.runs.size() == 1);
			UAM_ASSERT_EQ(saved.runs.front().status, run.status);
			UAM_ASSERT_EQ(saved.runs.front().diagnostic_code, run.diagnostic_code);
			UAM_ASSERT_EQ(saved.runs.front().result_excerpt, excerpt);
			UAM_ASSERT(saved.runs.front().root_result_delivered);
			const auto root = ChatRepository::LoadLocalChat(app.data_root, "root", true, nullptr, false);
			UAM_ASSERT(root);
			int deliveries = 0;
			for (const auto& message : root->messages) for (const auto& tool : message.tool_calls)
			{
				if (tool.id != "uam-agent-result-" + run_id) continue;
				++deliveries;
				const auto result = nlohmann::json::parse(tool.result_text);
				UAM_ASSERT_EQ(result.at("result"), nlohmann::json(excerpt));
				UAM_ASSERT_EQ(result.at("diagnosticCode"), nlohmann::json("wall_clock_deadline_exceeded"));
			}
			UAM_ASSERT_EQ(deliveries, 1);
			RequireInert();
		}
	};
}

UAM_TEST(ManagedSubagentStoppedDeadlinePublishesExactExcerptOnceBeforeRetirement)
{
	DeadlinePublicationDataFixture fixture;
	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(fixture.app, fixture.deadline));
	fixture.RequireDelivery();
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, fixture.deadline + 1);
	fixture.RequireDelivery();
}

UAM_TEST(ManagedSubagentDeadlineRetryFlagNeverBecomesUserCancellation)
{
	DeadlinePublicationDataFixture fixture(true);
	fixture.app.acp_sessions.front()->managed_cancellation_pending = true;
	fixture.app.pending_chat_save_at_by_chat_id[fixture.transcript_id] = 1.0;
	UAM_ASSERT(!uam::AgentRunScheduler::PollAtForTests(fixture.app, fixture.deadline));
	UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.empty());
	fixture.app.pending_chat_save_at_by_chat_id.erase(fixture.transcript_id);
	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(fixture.app, fixture.deadline + 1));
	UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->stop_purpose, uam::AcpStopPurpose::Timeout);
	UAM_ASSERT(!uam::AgentRunScheduler::PollAtForTests(fixture.app, fixture.deadline + 2));
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("running"));
	UAM_ASSERT(fixture.app.agent_runs.front().diagnostic_code.empty());
	UAM_ASSERT(!fixture.app.agent_runs.front().root_result_delivered && fixture.app.agent_runs.front().root_result_delivery_attempts == 0);
	UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.empty());
	UAM_ASSERT(fixture.app.agent_run_deadline_steady_ms.contains(fixture.run_id));
	const auto saved = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
	UAM_ASSERT(saved.errors.empty() && saved.runs.size() == 1);
	UAM_ASSERT_EQ(saved.runs.front().status, std::string("running"));
	UAM_ASSERT(saved.runs.front().diagnostic_code.empty() && !saved.runs.front().root_result_delivered);
	fixture.RequireInert();
}

UAM_TEST(ManagedSubagentExpiredDeadlinePrecedesSubsequentlyObservedSettlement)
{
	DeadlinePublicationDataFixture fixture;
	fixture.app.acp_sessions.front()->last_settled_turn_serial = 1;
	fixture.app.acp_sessions.front()->last_turn_outcome = "completed";
	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(fixture.app, fixture.deadline));
	fixture.RequireDelivery();
}

UAM_TEST(ManagedSubagentDeadlineTerminalSaveRefusalRetainsExcerptAndRuntimeForRetry)
{
	DeadlinePublicationDataFixture fixture;
	const auto primary = fixture.app.data_root / "agent-runs" / (fixture.run_id + ".json");
	const auto held = std::filesystem::path(primary.string() + ".private-held");
	struct RestorePrivateLedger
	{
		std::filesystem::path primary, held;
		bool active = false;
		bool Restore() noexcept
		{
			if (!active) return true;
			std::error_code error;
			std::filesystem::remove_all(primary, error);
			if (error) return false;
			std::filesystem::rename(held, primary, error);
			if (error) return false;
			active = false;
			return true;
		}
		~RestorePrivateLedger() { (void)Restore(); }
	} restore{primary, held};
	std::filesystem::rename(primary, held);
	restore.active = true;
	UAM_ASSERT(std::filesystem::create_directory(primary));
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, fixture.deadline);
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("running"));
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().result_excerpt, fixture.excerpt);
	UAM_ASSERT(!fixture.app.agent_runs.front().root_result_delivered && fixture.app.agent_runs.front().root_result_delivery_attempts == 0);
	UAM_ASSERT(fixture.app.agent_run_deadline_steady_ms.contains(fixture.run_id));
	UAM_ASSERT_EQ(fixture.app.acp_sessions.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.app.chats.back().id, fixture.transcript_id);
	UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->lifecycle_state, std::string("stopped"));
	fixture.RequireInert();
	UAM_ASSERT(!uam::AgentRunScheduler::PollAtForTests(fixture.app, fixture.deadline + 1));
	UAM_ASSERT(restore.Restore());
	const auto before_retry = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
	UAM_ASSERT(before_retry.errors.empty() && before_retry.runs.size() == 1);
	UAM_ASSERT_EQ(before_retry.runs.front().status, std::string("running"));
	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(fixture.app, fixture.deadline + 2));
	fixture.RequireDelivery();
}

// DATA pending-task evidence only; there is deliberately no worker or owned OS process.
UAM_TEST(ManagedSubagentDeadlineDataPendingStopTaskPreventsTerminalPublication)
{
	DeadlinePublicationDataFixture fixture;
	fixture.RequireInert();
	const auto root_messages = fixture.app.chats.front().messages;
	auto unfinished = std::make_shared<std::atomic<bool>>(false);
	uam::AsyncAcpProcessStopTask pending;
	pending.chat_id = fixture.transcript_id;
	pending.finished = unfinished;
	// Default purpose models an earlier ordinary Stop; no completion/result is fabricated.
	UAM_ASSERT(!pending.worker && !pending.owned_process && !pending.result);
	fixture.app.acp_process_stop_tasks.push_back(std::move(pending));
	struct RetireOnlyInertFixtureTask
	{
		uam::AppState& app;
		std::shared_ptr<std::atomic<bool>> marker;
		void Retire()
		{
			std::erase_if(app.acp_process_stop_tasks, [&](const auto& task)
			{ return task.finished == marker && !task.worker && !task.owned_process && !task.result; });
		}
		~RetireOnlyInertFixtureTask() { Retire(); }
	} cleanup{fixture.app, unfinished};
	UAM_ASSERT(!fixture.app.acp_sessions.front()->running);
	UAM_ASSERT(!fixture.app.acp_sessions.front()->local_stop_pending);
	UAM_ASSERT(uam::AcpStopInProgress(fixture.app, fixture.transcript_id));
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, fixture.deadline);
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("running"));
	UAM_ASSERT(!fixture.app.agent_runs.front().root_result_delivered && fixture.app.agent_runs.front().root_result_delivery_attempts == 0);
	UAM_ASSERT(fixture.app.agent_run_deadline_steady_ms.contains(fixture.run_id));
	UAM_ASSERT_EQ(fixture.app.acp_sessions.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->stop_purpose, uam::AcpStopPurpose::Timeout);
	UAM_ASSERT_EQ(fixture.app.chats.back().id, fixture.transcript_id);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages, root_messages);
	UAM_ASSERT(uam::AcpStopInProgress(fixture.app, fixture.transcript_id));
	UAM_ASSERT_EQ(fixture.app.acp_process_stop_tasks.size(), std::size_t{1});
	UAM_ASSERT(fixture.app.acp_process_stop_tasks.front().finished == unfinished);
	UAM_ASSERT(!unfinished->load());
	UAM_ASSERT(!fixture.app.acp_process_stop_tasks.front().worker && !fixture.app.acp_process_stop_tasks.front().owned_process);
	UAM_ASSERT(fixture.app.pending_acp_remote_stops.empty());
	const auto saved = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
	UAM_ASSERT(saved.errors.empty() && saved.runs.size() == 1);
	UAM_ASSERT_EQ(saved.runs.front().status, std::string("running"));
	UAM_ASSERT(!saved.runs.front().root_result_delivered && saved.runs.front().root_result_delivery_attempts == 0);
	const auto root = ChatRepository::LoadLocalChat(fixture.app.data_root, "root", true, nullptr, false);
	UAM_ASSERT(root);
	UAM_ASSERT_EQ(root->messages, root_messages);
	cleanup.Retire(); // Remove only the synthetic inert record; this is not wait/reap evidence.
	fixture.RequireInert();
}
