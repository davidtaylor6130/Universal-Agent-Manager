#include "test_harness.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_CODEX_CLI
namespace
{
	// Exercise the existing public Send queue path behind privately owned busy runtime state.
	// No process, initialized native session, delivery, completed turn, or provider reply is fabricated.
	struct AcpInputIdentityQueueFixture
	{
		TempDir temp{"uam-input-admission-identity"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;
		const std::string chat_id = "input-identity-private-chat";

		AcpInputIdentityQueueFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
			// An accidental provider launch is a failed prerequisite, never a real account call.
			app.provider_profiles.front().interactive_command.clear();
			app.settings.central_provider_configuration.enabled = false;
			app.settings.provider_chat_defaults["codex-cli"].feature_preference = "provider";
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.settings.git_enabled = false;
			app.settings.git_auto_worktree = false;
			app.settings.svn_enabled = false;
			const fs::path workspace = temp.root / "workspace";
			fs::create_directories(workspace);
			ChatSession chat;
			chat.id = chat_id;
			chat.created_at = "2026-10-21 12:00:00";
			chat.provider_id = "codex-cli";
			chat.execution_host_id = "local";
			chat.routing_mode = "explicit";
			chat.workspace_directory = workspace.string();
			chat.memory_enabled = false;
			chat.memory_level = "off";
			chat.uam_control_enabled = false;
			chat.uam_control_explicitly_disabled = true;
			ChatDomainService().AddMessage(chat, MessageRole::User, "Existing authored fixture row.");
			app.chats.push_back(chat);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, Chat()));
			auto owned = std::make_unique<uam::AcpSessionState>();
			owned->chat_id = chat_id;
			owned->provider_id = "codex-cli";
			owned->protocol_kind = "codex-app-server";
			owned->processing = true;
			session = owned.get();
			app.acp_sessions.push_back(std::move(owned));
			AssertNoLaunchOrWorker();
		}

		ChatSession& Chat()
		{
			// Reacquire on each use; managed mention may grow app.chats.
			ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
			UAM_ASSERT(chat != nullptr);
			return *chat;
		}

		bool Send(const std::string& text, const std::optional<std::string>& origin,
		    std::optional<uam::AcpInputAdmission>& admission, std::string& error)
		{
			error.clear();
			return uam::SendAcpPrompt(app, chat_id, text, {}, {}, false, &error, {}, false,
			    origin, &admission);
		}

		void AssertNoLaunchOrWorker()
		{
			UAM_ASSERT_EQ(app.acp_sessions.size(), std::size_t{1});
			UAM_ASSERT(session != nullptr && !session->running && !session->initialized && !session->session_ready);
			UAM_ASSERT(session->last_process_id.empty() && !session->stdin_writer);
			UAM_ASSERT(session->pending_request_methods.empty() && session->queued_prompt.empty());
			UAM_ASSERT_EQ(session->prompt_request_id, 0);
			UAM_ASSERT(app.local_acp_launch_tasks.empty() && app.agent_input_preparation_tasks.empty());
			UAM_ASSERT(app.cli_terminals.empty() && app.worktree_launch_preparations.empty());
			UAM_ASSERT(app.pending_acp_remote_stops.empty() && app.acp_process_stop_tasks.empty());
			UAM_ASSERT(app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty());
		}

		void AssertQueuedFact(const uam::AcpInputAdmission& fact, std::size_t index)
		{
			UAM_ASSERT(index < session->queued_user_prompts.size());
			const auto& queued = session->queued_user_prompts[index];
			UAM_ASSERT(uam::input_provenance::ValidNativeInputId(fact.native_input_id));
			UAM_ASSERT_EQ(fact.kind, uam::AcpInputAdmission::Kind::Queued);
			UAM_ASSERT(fact.queued.has_value() && !fact.recorded_row.has_value());
			UAM_ASSERT_EQ(fact.queued->id, queued.id);
			UAM_ASSERT_EQ(fact.queued->revision, queued.revision);
			UAM_ASSERT(!queued.id.empty() && queued.revision == 1);
			UAM_ASSERT_EQ(queued.native_input_id, std::optional<std::string>(fact.native_input_id));
			UAM_ASSERT_EQ(queued.origin_request_id, fact.origin_request_id);
			UAM_ASSERT(queued.id != fact.native_input_id);
			UAM_ASSERT(!queued.direct_admission && queued.append_user_message && !queued.prepared_for_delivery);
			UAM_ASSERT_EQ(Chat().acp_queued_prompts[index], queued);
		}
	};
}

UAM_TEST(AcpInputAdmissionIdenticalQueuedSendsHaveDistinctNativeIdsAndExactQueueFacts)
{
	AcpInputIdentityQueueFixture fixture;
	const auto original_rows = fixture.Chat().messages;
	std::optional<uam::AcpInputAdmission> first, second;
	std::string error;
	UAM_ASSERT(fixture.Send("Identical authored input.", std::string("same-origin"), first, error));
	UAM_ASSERT(error.empty() && first.has_value());
	UAM_ASSERT(fixture.Send("Identical authored input.", std::string("same-origin"), second, error));
	UAM_ASSERT(error.empty() && second.has_value());
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{2});
	fixture.AssertQueuedFact(*first, 0);
	fixture.AssertQueuedFact(*second, 1);
	UAM_ASSERT(first->native_input_id != second->native_input_id);
	UAM_ASSERT(first->queued->id != second->queued->id);
	UAM_ASSERT_EQ(first->origin_request_id, second->origin_request_id);
	UAM_ASSERT_EQ(fixture.Chat().messages, original_rows);
	fixture.AssertNoLaunchOrWorker();
}

UAM_TEST(AcpInputAdmissionQueuedOriginBoundsOmitMalformedOriginWithoutRejectingInput)
{
	AcpInputIdentityQueueFixture fixture;
	std::string boundary_origin(128, 'x');
	boundary_origin.front() = ' ';
	boundary_origin.back() = '~';
	const std::vector<std::optional<std::string>> origins = {
		std::nullopt, std::string(" "), boundary_origin, std::string{}, std::string(129, 'x'),
		std::string("control") + char(0x1f), std::string("delete") + char(0x7f),
		std::string("nul\0byte", 8), std::string("\xc2\xa0")
	};
	for (std::size_t index = 0; index < origins.size(); ++index)
	{
		std::optional<uam::AcpInputAdmission> admission;
		std::string error;
		UAM_ASSERT(fixture.Send("Bounded origin correlation.", origins[index], admission, error));
		UAM_ASSERT(error.empty() && admission.has_value());
		fixture.AssertQueuedFact(*admission, index);
		// Expected cases are explicit, independent of the production validator under test.
		if (index == 1 || index == 2)
			UAM_ASSERT_EQ(admission->origin_request_id, origins[index]);
		else
			UAM_ASSERT(!admission->origin_request_id.has_value());
	}
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), origins.size());
	fixture.AssertNoLaunchOrWorker();
}

UAM_TEST(AcpInputAdmissionReusedOutputClearsOnValidationAndOwnershipRefusal)
{
	AcpInputIdentityQueueFixture fixture;
	std::optional<uam::AcpInputAdmission> admission;
	std::string error;
	UAM_ASSERT(fixture.Send("First admitted input.", std::string("origin-first"), admission, error));
	UAM_ASSERT(admission.has_value());
	const auto valid_fact = *admission;
	const auto before_queue = fixture.session->queued_user_prompts;
	const auto before_outbox = fixture.Chat().acp_queued_prompts;
	const auto before_rows = fixture.Chat().messages;
	UAM_ASSERT(!fixture.Send("", std::string("origin-empty"), admission, error));
	UAM_ASSERT(!error.empty() && !admission.has_value());
	admission = valid_fact;
	fixture.Chat().imported_read_only = true;
	UAM_ASSERT(!fixture.Send("Read-only input.", std::string("origin-readonly"), admission, error));
	UAM_ASSERT(!error.empty() && !admission.has_value());
	fixture.Chat().imported_read_only = false;
	admission = valid_fact;
	UAM_ASSERT(!uam::SendAcpPrompt(fixture.app, "missing-private-chat", "Missing chat.", {}, {},
	    false, &error, {}, false, std::string("origin-missing"), &admission));
	UAM_ASSERT(!admission.has_value());
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts, before_queue);
	UAM_ASSERT_EQ(fixture.Chat().acp_queued_prompts, before_outbox);
	UAM_ASSERT_EQ(fixture.Chat().messages, before_rows);
	fixture.AssertNoLaunchOrWorker();
}

UAM_TEST(AcpInputAdmissionQueuedPersistenceFailureClearsOutputAndRollsBackNewEntry)
{
	AcpInputIdentityQueueFixture fixture;
	std::optional<uam::AcpInputAdmission> admission;
	std::string error;
	UAM_ASSERT(fixture.Send("Previously retained input.", std::string("origin-retained"), admission, error));
	UAM_ASSERT(admission.has_value());
	const auto before_queue = fixture.session->queued_user_prompts;
	const auto before_outbox = fixture.Chat().acp_queued_prompts;
	const auto before_rows = fixture.Chat().messages;
	const fs::path original_root = fixture.app.data_root;
	const fs::path blocked_root = fixture.temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocked_root, "Private failure fixture."));
	fixture.app.data_root = blocked_root;
	UAM_ASSERT(!fixture.Send("Must remain unadmitted.", std::string("origin-rejected"), admission, error));
	UAM_ASSERT(!error.empty() && !admission.has_value());
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts, before_queue);
	UAM_ASSERT_EQ(fixture.Chat().acp_queued_prompts, before_outbox);
	UAM_ASSERT_EQ(fixture.Chat().messages, before_rows);
	const auto durable = ChatRepository::LoadLocalChat(original_root, fixture.chat_id);
	UAM_ASSERT(durable.has_value());
	UAM_ASSERT_EQ(durable->acp_queued_prompts, before_outbox);
	fixture.AssertNoLaunchOrWorker();
}

UAM_TEST(AcpInputAdmissionQueuedProvenanceRoundTripsThroughRealRepository)
{
	AcpInputIdentityQueueFixture fixture;
	const auto before_rows = fixture.Chat().messages;
	std::optional<uam::AcpInputAdmission> first, second;
	std::string error;
	UAM_ASSERT(fixture.Send("First queued payload.", std::string("origin-durable"), first, error));
	UAM_ASSERT(fixture.Send("Second queued payload.", std::nullopt, second, error));
	UAM_ASSERT(first.has_value() && second.has_value());
	fixture.AssertQueuedFact(*first, 0);
	fixture.AssertQueuedFact(*second, 1);
	const auto loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.chat_id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->acp_queued_prompts, fixture.Chat().acp_queued_prompts);
	UAM_ASSERT_EQ(loaded->acp_queued_prompts.size(), std::size_t{2});
	UAM_ASSERT_EQ(loaded->acp_queued_prompts[0].native_input_id, std::optional<std::string>(first->native_input_id));
	UAM_ASSERT_EQ(loaded->acp_queued_prompts[1].native_input_id, std::optional<std::string>(second->native_input_id));
	UAM_ASSERT_EQ(loaded->acp_queued_prompts[0].origin_request_id, first->origin_request_id);
	UAM_ASSERT(!loaded->acp_queued_prompts[1].origin_request_id.has_value());
	UAM_ASSERT_EQ(loaded->messages, before_rows);
	fixture.AssertNoLaunchOrWorker();
}

UAM_TEST(AcpInputAdmissionManagedMentionKeepsSpecializedReceiptAndClearsOrdinaryOutput)
{
	AcpInputIdentityQueueFixture fixture;
	const fs::path agents = fixture.temp.root / "workspace" / ".uam" / "agents";
	fs::create_directories(agents);
	UAM_ASSERT(uam::io::WriteTextFile(agents / "worker.md",
	    "---\nversion: 1\nname: worker\ndescription: Worker\nmode: subagent\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nInspect the fixture.\n"));
	std::optional<uam::AcpInputAdmission> admission;
	std::string error;
	UAM_ASSERT(fixture.Send("Ordinary queue input.", std::string("origin-ordinary"), admission, error));
	UAM_ASSERT(admission.has_value());
	const auto before_queue = fixture.session->queued_user_prompts;
	const auto before_outbox = fixture.Chat().acp_queued_prompts;
	const auto before_count = fixture.Chat().messages.size();
	const std::string mention = "@worker Inspect this private fixture without running it.";
	UAM_ASSERT(fixture.Send(mention, std::string("origin-managed"), admission, error));
	UAM_ASSERT(error.empty() && !admission.has_value());
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts, before_queue);
	UAM_ASSERT_EQ(fixture.Chat().acp_queued_prompts, before_outbox);
	UAM_ASSERT_EQ(fixture.Chat().messages.size(), before_count + 1);
	UAM_ASSERT_EQ(fixture.Chat().messages.back().content, mention);
	UAM_ASSERT(!fixture.Chat().messages.back().native_input_id.has_value());
	UAM_ASSERT_EQ(fixture.app.agent_runs.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.app.queued_agent_run_ids.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("queued"));
	UAM_ASSERT(fixture.app.agent_runs.front().deliver_result_to_root_chat);
	fixture.AssertNoLaunchOrWorker();
}
#endif
