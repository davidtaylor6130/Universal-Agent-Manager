#include "test_harness.h"
#include "common/platform/published_file_change_witness.h"

using namespace uam_test;

namespace
{
	struct AgentResultDeliveryFixture
	{
		TempDir temp{"uam-agent-result-delivery"};
		uam::AppState app;
		AgentRun run;

		AgentResultDeliveryFixture()
		{
			app.data_root = temp.root / "data";
			ChatSession parent;
			parent.id = "result-parent";
			parent.branch_root_chat_id = parent.id;
			parent.provider_id = "codex-cli";
			parent.created_at = "2026-10-21 10:00:00";
			parent.updated_at = parent.created_at;
			parent.managed_followup_incarnation = uam::AgentRunLedger::NewRunId();
			Message first;
			first.role = MessageRole::User;
			first.content = "Authored parent message A.";
			first.created_at = parent.created_at;
			parent.messages.push_back(first);
			UAM_ASSERT(parent.messages_loaded);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, parent));
			app.chats.push_back(parent);

			run.id = uam::AgentRunLedger::NewRunId();
			run.root_chat_id = parent.id;
			run.transcript_chat_id = "result-transcript-" + run.id;
			run.agent_id = "reviewer";
			run.definition_hash = "fixture-definition-identity";
			run.definition_snapshot = "---\nversion: 1\n---\nReview the requested change.";
			run.provider_id = parent.provider_id;
			run.execution_capability = "uam-prompt-injected";
			run.task = "Review the requested change.";
			run.effective_workspace_access = "read";
			run.status = "completed";
			run.depth = 1;
			run.expected_turn_serial = 1;
			run.started_at_epoch_ms = 1000;
			run.finished_at_epoch_ms = 1100;
			run.deadline_at_epoch_ms = 1801000;
			run.created_at = parent.created_at;
			run.started_at = parent.created_at;
			run.finished_at = "2026-10-21 10:00:01";
			run.updated_at = run.finished_at;
			run.result_delivery_id = run.id;
			run.deliver_result_to_root_chat = true;
			run.result_excerpt = "Original child result.\nRetain these exact bytes.";
			UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
			app.agent_runs.push_back(run);
		}

		ChatSession LoadParent() const
		{
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, run.root_chat_id, true, nullptr, false);
			UAM_ASSERT(saved.has_value());
			return *saved;
		}

		AgentRun LoadRun() const
		{
			const auto saved = uam::AgentRunLedger::LoadAllReadOnly(app.data_root);
			UAM_ASSERT(saved.errors.empty());
			UAM_ASSERT_EQ(saved.runs.size(), std::size_t{1});
			UAM_ASSERT_EQ(saved.runs.front().id, run.id);
			UAM_ASSERT_EQ(saved.runs.front().result_excerpt, run.result_excerpt);
			return saved.runs.front();
		}

		std::size_t ResultCount(const ChatSession& parent) const
		{
			std::size_t count = 0;
			for (const auto& message : parent.messages)
			{
				for (const auto& tool : message.tool_calls)
				{
					if (tool.name != "uam_agent_result") continue;
					const auto result = nlohmann::json::parse(tool.result_text);
					UAM_ASSERT_EQ(result.at("runId").get<std::string>(), run.id);
					UAM_ASSERT_EQ(result.at("result").get<std::string>(), run.result_excerpt);
					UAM_ASSERT_EQ(result.at("status").get<std::string>(), run.status);
					++count;
				}
			}
			return count;
		}

		void DiagnoseMissingResult() const
		{
			const auto& current = app.chats.front();
			auto saved = LoadParent();
			saved.persisted_message_count = current.persisted_message_count;
			saved.persisted_messages_digest = current.persisted_messages_digest;
			saved.acp_queue_full_save_revision = current.acp_queue_full_save_revision;
			const auto before = nlohmann::json::parse(ChatRepository::ChatMetadataFingerprint(app.data_root, saved));
			const auto after = nlohmann::json::parse(ChatRepository::ChatMetadataFingerprint(app.data_root, current));
			nlohmann::json paths = nlohmann::json::array();
			for (const auto& difference : nlohmann::json::diff(before, after))
				paths.push_back(difference.at("path"));
			std::cerr << "RESULT_DELIVERY_DIAGNOSTIC attempts=" << app.agent_runs.front().root_result_delivery_attempts
			    << " messages_equal=" << (saved.messages == current.messages)
			    << " metadata_changed_paths=" << paths.dump() << '\n';
		}
	};
}

UAM_TEST(AgentResultDeliveryAppendsOnceToCurrentLoadedParent)
{
	AgentResultDeliveryFixture fixture;
	const auto authored = fixture.app.chats.front().messages.front();
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1200);
	const auto delivered = fixture.LoadParent();
	UAM_ASSERT_EQ(delivered.messages.front(), authored);
	if (fixture.ResultCount(delivered) == 0) fixture.DiagnoseMissingResult();
	UAM_ASSERT_EQ(fixture.ResultCount(delivered), std::size_t{1});
	UAM_ASSERT(fixture.app.agent_runs.front().root_result_delivered);
	UAM_ASSERT(fixture.LoadRun().root_result_delivered);

	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1300);
	UAM_ASSERT_EQ(fixture.LoadParent().messages, delivered.messages);
	UAM_ASSERT_EQ(fixture.ResultCount(fixture.LoadParent()), std::size_t{1});
}

UAM_TEST(AgentResultDeliveryPreservesNewerSavedParentBeforeAcknowledging)
{
	AgentResultDeliveryFixture fixture;
	const auto stale_messages = fixture.app.chats.front().messages;
	ChatSession newer = fixture.app.chats.front();
	Message second;
	second.role = MessageRole::User;
	second.content = "Newer authored parent message B must survive.";
	second.created_at = "2026-10-21 10:00:02";
	newer.messages.push_back(second);
	newer.updated_at = second.created_at;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, newer));
	UAM_ASSERT_EQ(newer.managed_followup_incarnation, fixture.app.chats.front().managed_followup_incarnation);
	UAM_ASSERT_EQ(newer.metadata_incarnation, fixture.app.chats.front().metadata_incarnation);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages, stale_messages);

	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1200);
	const auto after_stale_poll = fixture.LoadParent();
	UAM_ASSERT(after_stale_poll.messages.size() >= newer.messages.size());
	UAM_ASSERT_EQ(after_stale_poll.messages.at(0), newer.messages.at(0));
	UAM_ASSERT_EQ(after_stale_poll.messages.at(1), second);
	const auto result_count = fixture.ResultCount(after_stale_poll);
	UAM_ASSERT(result_count <= 1);
	// Reconciliation may publish safely or refuse; refusal must retain the result without a false ACK.
	if (result_count == 0)
	{
		UAM_ASSERT_EQ(after_stale_poll.messages, newer.messages);
		UAM_ASSERT_EQ(fixture.app.chats.front().messages, stale_messages);
		UAM_ASSERT(!fixture.app.agent_runs.front().root_result_delivered);
		UAM_ASSERT(!fixture.LoadRun().root_result_delivered);
	}
	else
	{
		UAM_ASSERT(fixture.app.agent_runs.front().root_result_delivered);
		UAM_ASSERT(fixture.LoadRun().root_result_delivered);
	}
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().result_excerpt, fixture.run.result_excerpt);

	// Explicitly adopt authoritative history before retrying; this read grants no provider execution.
	const auto incarnation = fixture.app.chats.front().metadata_incarnation;
	fixture.app.chats.front() = fixture.LoadParent();
	fixture.app.chats.front().metadata_incarnation = incarnation;
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1300);
	const auto delivered = fixture.LoadParent();
	UAM_ASSERT_EQ(delivered.messages.at(0), newer.messages.at(0));
	UAM_ASSERT_EQ(delivered.messages.at(1), second);
	UAM_ASSERT_EQ(fixture.ResultCount(delivered), std::size_t{1});
	UAM_ASSERT(fixture.app.agent_runs.front().root_result_delivered);
	UAM_ASSERT(fixture.LoadRun().root_result_delivered);

	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1400);
	UAM_ASSERT_EQ(fixture.LoadParent().messages, delivered.messages);
	UAM_ASSERT_EQ(fixture.ResultCount(fixture.LoadParent()), std::size_t{1});
}

UAM_TEST(AgentResultDeliverySurvivesPendingAuthoredParentSave)
{
	AgentResultDeliveryFixture fixture;
	Message second;
	second.role = MessageRole::User;
	second.content = "Pending authored parent message B must survive.";
	second.created_at = "2026-10-21 10:00:02";
	fixture.app.chats.front().messages.push_back(second);
	fixture.app.chats.front().updated_at = second.created_at;
	// A pending parent save may reconcile safely or defer, without exhausting delivery.
	for (int attempt = 0; attempt < 4; ++attempt)
		(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1200 + attempt);
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.app.chats.front()));
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1300);
	const auto delivered = fixture.LoadParent();
	UAM_ASSERT_EQ(delivered.messages.at(1), second);
	if (fixture.ResultCount(delivered) == 0) fixture.DiagnoseMissingResult();
	UAM_ASSERT_EQ(fixture.ResultCount(delivered), std::size_t{1});
	UAM_ASSERT(fixture.app.agent_runs.front().root_result_delivered);
	UAM_ASSERT(fixture.LoadRun().root_result_delivered);
	(void)uam::AgentRunScheduler::PollAtForTests(fixture.app, 1400);
	UAM_ASSERT_EQ(fixture.LoadParent().messages, delivered.messages);
	UAM_ASSERT_EQ(fixture.ResultCount(fixture.LoadParent()), std::size_t{1});
}

UAM_TEST(AgentResultDeliveryRepeatedStaleRefusalIsReadOnly)
{
	AgentResultDeliveryFixture fixture;
	const auto live_before = fixture.app.chats.front().messages;
	ChatSession newer = fixture.app.chats.front();
	ChatDomainService().AddMessage(newer, MessageRole::User, "Newer saved parent message B.");
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, newer));
	const auto saved_before = fixture.LoadParent().messages;
	const auto ledger = fixture.app.data_root / "agent-runs" / (fixture.run.id + ".json");
	const auto bytes = ReadFile(ledger);
	const auto attempts = fixture.app.agent_runs.front().root_result_delivery_attempts;
	const auto physical = uam::platform::PublishedFileChangeWitness::Capture(ledger);
	UAM_ASSERT(physical);
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		const bool changed = uam::AgentRunScheduler::PollAtForTests(fixture.app, 1200 + attempt);
		// A retained physical object detects an equal-byte rewrite without clocks or sleeps.
		UAM_ASSERT(physical->Current());
		UAM_ASSERT(!changed);
		UAM_ASSERT_EQ(ReadFile(ledger), bytes);
		UAM_ASSERT_EQ(fixture.app.agent_runs.front().root_result_delivery_attempts, attempts);
		UAM_ASSERT(!fixture.app.agent_runs.front().root_result_delivered);
		UAM_ASSERT(!fixture.LoadRun().root_result_delivered);
		UAM_ASSERT_EQ(fixture.app.chats.front().messages, live_before);
		UAM_ASSERT_EQ(fixture.LoadParent().messages, saved_before);
		UAM_ASSERT_EQ(fixture.ResultCount(fixture.LoadParent()), std::size_t{0});
		UAM_ASSERT(fixture.app.acp_sessions.empty());
		UAM_ASSERT(fixture.app.queued_agent_run_ids.empty());
	}
}
