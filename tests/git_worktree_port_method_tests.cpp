#include "test_harness.h"
#include "app/managed_subagent_control_service.h"

using namespace uam_test;

namespace
{
	std::string PortGitOutput(const fs::path& repository, const std::string& arguments)
	{
		const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
		    "git -C " + ShellQuoteForTest(repository.string()) + " " + arguments, 120000);
		UAM_ASSERT(result.ok && result.exit_code == 0);
		return uam::strings::Trim(result.output);
	}

	struct PortFixture
	{
		TempDir temporary{"uam-staged-port"};
		uam::AppState app;
		fs::path source = temporary.root / "source";
		fs::path worktree;
		std::string source_head;
		std::string chat_head;
		std::string chat_branch;

		explicit PortFixture(const std::string& method)
		{
			fs::create_directories(source);
			UAM_ASSERT(RunGitForTest(source, "init"));
			UAM_ASSERT(RunGitForTest(source, "config user.email fixture@example.test"));
			UAM_ASSERT(RunGitForTest(source, "config user.name Fixture"));
			UAM_ASSERT(uam::io::WriteTextFile(source / ".gitignore", "private.txt\n"));
			UAM_ASSERT(uam::io::WriteTextFile(source / "survivor.txt", "source survivor\n"));
			UAM_ASSERT(RunGitForTest(source, "add ."));
			UAM_ASSERT(RunGitForTest(source, "commit -m baseline"));
			UAM_ASSERT(RunGitForTest(source, "branch survivor"));
			app.data_root = temporary.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
			chat.workspace_directory = source.string();
			app.chats.push_back(std::move(chat));
			UAM_ASSERT(uam::GitWorktreeService().CreateForChat(app, app.chats.front()).ok);
			worktree = uam::paths::PathFromUtf8(app.chats.front().workspace_worktree_directory);
			chat_branch = app.chats.front().workspace_branch_name;
			UAM_ASSERT(uam::io::WriteTextFile(worktree / "chat.txt", "committed chat change\n"));
			UAM_ASSERT(RunGitForTest(worktree, "-c user.email=fixture@example.test -c user.name=Fixture add chat.txt"));
			UAM_ASSERT(RunGitForTest(worktree, "-c user.email=fixture@example.test -c user.name=Fixture commit -m chat"));
			UAM_ASSERT(uam::io::WriteTextFile(worktree / "private.txt", "worktree survivor\n"));
			UAM_ASSERT(uam::io::WriteTextFile(source / "private.txt", "source private survivor\n"));
			source_head = PortGitOutput(source, "rev-parse HEAD");
			chat_head = PortGitOutput(worktree, "rev-parse HEAD");
			const fs::path settings = temporary.root / "settings.txt";
			UAM_ASSERT(uam::io::WriteTextFile(settings, "git_enabled=true\ngit_port_method=" + method + "\n"));
			UAM_ASSERT(SettingsStore::Load(settings, app.settings).loaded);
		}

		void AssertRetained()
		{
			UAM_ASSERT_EQ(PortGitOutput(source, "rev-parse HEAD"), source_head);
			UAM_ASSERT_EQ(PortGitOutput(worktree, "rev-parse HEAD"), chat_head);
			UAM_ASSERT_EQ(PortGitOutput(source, "rev-parse refs/heads/" + chat_branch), chat_head);
			UAM_ASSERT_EQ(app.chats.front().workspace_worktree_directory, worktree.string());
			UAM_ASSERT_EQ(uam::io::ReadTextFile(source / "survivor.txt"), "source survivor\n");
			UAM_ASSERT_EQ(uam::io::ReadTextFile(source / "private.txt"), "source private survivor\n");
			UAM_ASSERT_EQ(uam::io::ReadTextFile(worktree / "private.txt"), "worktree survivor\n");
			UAM_ASSERT(RunGitForTest(source, "show-ref --verify refs/heads/survivor"));
		}
	};
}

UAM_TEST(GitWorktreePortMergeStagesOwnedCommitWithoutCommitting)
{
	if (!GitAvailableForTests()) return;
	PortFixture fixture("merge");
	UAM_ASSERT(uam::GitWorktreeService().PortChatChanges(fixture.app, fixture.app.chats.front()).ok);
	UAM_ASSERT_EQ(PortGitOutput(fixture.source, "rev-parse MERGE_HEAD"), fixture.chat_head);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.source / "chat.txt"), "committed chat change\n");
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortSquashStagesOwnedCommitWithoutCommitting)
{
	if (!GitAvailableForTests()) return;
	PortFixture fixture("squash");
	UAM_ASSERT(uam::GitWorktreeService().PortChatChanges(fixture.app, fixture.app.chats.front()).ok);
	const fs::path squash_message = uam::paths::PathFromUtf8(PortGitOutput(fixture.source, "rev-parse --path-format=absolute --git-path SQUASH_MSG"));
	UAM_ASSERT(fs::exists(squash_message));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.source / "chat.txt"), "committed chat change\n");
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortCancellationBeforeMutationPreservesSource)
{
	PortFixture fixture("merge");
	std::stop_source cancel;
	cancel.request_stop();
	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().PortCommittedChanges(fixture.app, fixture.app.chats.front(), cancel.get_token());
	UAM_ASSERT(!result.ok);
	UAM_ASSERT(result.operation_receipt.empty());
	UAM_ASSERT(!fs::exists(fixture.source / "chat.txt"));
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortRecreatedChatCancelsOwnedRequest)
{
	PortFixture fixture("squash");
	ChatSession& chat = fixture.app.chats.front();
	const std::shared_ptr<uam::AppState::GitPortRequest> request = std::make_shared<uam::AppState::GitPortRequest>();
	request->root = fixture.app.data_root;
	request->incarnation = chat.metadata_incarnation;
	request->workspace = chat.workspace_directory;
	request->source = chat.workspace_source_directory;
	request->worktree = chat.workspace_worktree_directory;
	request->branch = chat.workspace_branch_name;
	request->base = chat.workspace_base_ref;
	fixture.app.git_port_requests[chat.id] = request;
	chat.metadata_incarnation = std::make_shared<const uint8_t>(0);
	uam::PollGitPortRequests(fixture.app);
	UAM_ASSERT(request->cancellation->stop_requested());
	UAM_ASSERT(fixture.app.git_port_requests.empty());
	UAM_ASSERT(!fs::exists(fixture.source / "chat.txt"));
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortMergeRefusesDirtySourceBeforeReceipt)
{
	PortFixture fixture("merge");
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "survivor.txt", "new unrelated change\n"));
	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().PortChatChanges(fixture.app, fixture.app.chats.front());
	UAM_ASSERT(!result.ok && result.operation_receipt.empty());
	UAM_ASSERT(uam::io::ReadTextFile(fixture.source / "survivor.txt") == "new unrelated change\n");
	UAM_ASSERT(PortGitOutput(fixture.source, "rev-parse HEAD") == fixture.source_head);
}

UAM_TEST(GitWorktreePortMergeRetainsOwnedConflictRecovery)
{
	PortFixture fixture("merge");
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "chat.txt", "source conflict\n"));
	UAM_ASSERT(RunGitForTest(fixture.source, "add chat.txt"));
	UAM_ASSERT(RunGitForTest(fixture.source, "commit -m source-conflict"));
	fixture.source_head = PortGitOutput(fixture.source, "rev-parse HEAD");
	const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().PortChatChanges(fixture.app, fixture.app.chats.front());
	UAM_ASSERT(!result.ok && result.port_state == "conflicts");
	UAM_ASSERT(fs::exists(result.operation_receipt));
	UAM_ASSERT(PortGitOutput(fixture.source, "rev-parse MERGE_HEAD") == fixture.chat_head);
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortSkippedWorkerSettlesExpiredHandler)
{
	PortFixture fixture("merge");
	const std::shared_ptr<uam::AppState::GitPortRequest> request = std::make_shared<uam::AppState::GitPortRequest>();
	request->lifetime_bound = true;
	request->lease = fixture.app.worktree_retention_gate.TryAcquireMutation();
	UAM_ASSERT(request->lease);
	fixture.app.git_port_requests[fixture.app.chats.front().id] = request;
	fixture.app.worktree_operation_chat_ids.insert(fixture.app.chats.front().id);
	uam::PollGitPortRequests(fixture.app);
	UAM_ASSERT(request->cancellation->stop_requested());
	UAM_ASSERT(request->worker_state.load() == uam::AppState::GitPortRequest::WorkerState::Finished);
	UAM_ASSERT(fixture.app.git_port_requests.empty());
	UAM_ASSERT(fixture.app.worktree_operation_chat_ids.empty());
	UAM_ASSERT(fixture.app.worktree_retention_gate.TryAcquireCleanup());
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortDestroyedHandlerWaitsForRunningWorker)
{
	PortFixture fixture("squash");
	const std::shared_ptr<uam::AppState::GitPortRequest> request = std::make_shared<uam::AppState::GitPortRequest>();
	request->lifetime_bound = true;
	request->lease = fixture.app.worktree_retention_gate.TryAcquireMutation();
	UAM_ASSERT(request->lease);
	request->worker_state.store(uam::AppState::GitPortRequest::WorkerState::Running);
	fixture.app.git_port_requests[fixture.app.chats.front().id] = request;
	fixture.app.worktree_operation_chat_ids.insert(fixture.app.chats.front().id);
	uam::PollGitPortRequests(fixture.app);
	UAM_ASSERT(request->cancellation->stop_requested());
	UAM_ASSERT(fixture.app.git_port_requests.at(fixture.app.chats.front().id) == request);
	UAM_ASSERT(fixture.app.worktree_operation_chat_ids.contains(fixture.app.chats.front().id));
	UAM_ASSERT(!fixture.app.worktree_retention_gate.TryAcquireCleanup());
	request->worker_state.store(uam::AppState::GitPortRequest::WorkerState::Finished);
	uam::PollGitPortRequests(fixture.app);
	UAM_ASSERT(fixture.app.git_port_requests.empty() && fixture.app.worktree_operation_chat_ids.empty());
	UAM_ASSERT(fixture.app.worktree_retention_gate.TryAcquireCleanup());
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreePortSettlementPreservesReplacementRequest)
{
	PortFixture fixture("merge");
	const ChatSession& chat = fixture.app.chats.front();
	const std::shared_ptr<uam::AppState::GitPortRequest> old = std::make_shared<uam::AppState::GitPortRequest>();
	old->cancellation->request_stop();
	old->worker_state.store(uam::AppState::GitPortRequest::WorkerState::Finished);
	const std::shared_ptr<uam::AppState::GitPortRequest> replacement = std::make_shared<uam::AppState::GitPortRequest>();
	replacement->root = fixture.app.data_root;
	replacement->incarnation = chat.metadata_incarnation;
	replacement->workspace = chat.workspace_directory;
	replacement->source = chat.workspace_source_directory;
	replacement->worktree = chat.workspace_worktree_directory;
	replacement->branch = chat.workspace_branch_name;
	replacement->base = chat.workspace_base_ref;
	replacement->worker_state.store(uam::AppState::GitPortRequest::WorkerState::Running);
	fixture.app.git_port_requests[chat.id] = replacement;
	fixture.app.worktree_operation_chat_ids.insert(chat.id);
	uam::PollGitPortRequests(fixture.app);
	UAM_ASSERT(fixture.app.git_port_requests.at(chat.id) == replacement);
	UAM_ASSERT(!replacement->cancellation->stop_requested());
	UAM_ASSERT(fixture.app.worktree_operation_chat_ids.contains(chat.id));
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreeSharedChatRefusesDestructiveOperations)
{
	if (!GitAvailableForTests()) return;
	PortFixture fixture("patch");
	ChatSession alias = fixture.app.chats.front();
	alias.id += "-alias";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, alias));
	// Deliberately omit the persisted alias from the target-only async snapshot.
	UAM_ASSERT(uam::io::WriteTextFile(fixture.worktree / "chat.txt", "unsaved shared edits\n"));
	const uam::GitWorktreeOperationResult discard = uam::GitWorktreeService().DiscardChatChanges(fixture.app, fixture.app.chats.front());
	UAM_ASSERT(!discard.ok);
	UAM_ASSERT(discard.message.find("Another chat") != std::string::npos);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.worktree / "chat.txt"), "unsaved shared edits\n");
	const uam::GitWorktreeOperationResult port = uam::GitWorktreeService().PortChatChanges(fixture.app, fixture.app.chats.front());
	UAM_ASSERT(!port.ok);
	UAM_ASSERT(!fs::exists(fixture.source / "chat.txt"));
	std::string removal_error;
	UAM_ASSERT(!uam::GitWorktreeService().RemoveUnusedBranchWorktree(fixture.app, fixture.app.chats.front(), &removal_error));
	UAM_ASSERT(removal_error.find("Another chat") != std::string::npos);
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreeUnsavedSharedChatRefusesDiscard)
{
	if (!GitAvailableForTests()) return;
	PortFixture fixture("patch");
	ChatSession alias = fixture.app.chats.front();
	alias.id += "-unsaved-alias";
	fixture.app.chats.push_back(alias);
	UAM_ASSERT(!uam::GitWorktreeService().DiscardChatChanges(fixture.app, fixture.app.chats.front()).ok);
	fixture.AssertRetained();
}

namespace
{
	std::string AddFinishedWorktreeChild(PortFixture& fixture, const std::string& parent = {})
	{
		AgentRun run;
		run.id = uam::AgentRunLedger::NewRunId();
		run.root_chat_id = fixture.app.chats.front().id;
		run.parent_run_id = parent;
		run.depth = parent.empty() ? 1 : 2;
		run.transcript_chat_id = "child-" + run.id;
		run.agent_id = "worker";
		run.provider_id = fixture.app.chats.front().provider_id;
		run.model_id = fixture.app.chats.front().model_id;
		run.execution_capability = "uam-prompt-injected";
		run.effective_workspace_access = "write";
		run.status = "completed";
		run.created_at = fixture.app.chats.front().created_at;
		run.updated_at = run.created_at;
		run.finished_at = run.created_at;
		ChatSession child = ChatDomainService().CreateNewChat("", run.provider_id);
		child.id = run.transcript_chat_id;
		child.agent_run_id = run.id;
		child.model_id = run.model_id;
		child.execution_host_id = fixture.app.chats.front().execution_host_id;
		child.workspace_directory = fixture.worktree.string();
		child.workspace_source_directory = fixture.app.chats.front().workspace_source_directory;
		child.native_session_id = "retained-native-history";
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, child));
		fixture.app.hidden_agent_chat_ownership.data_root = fixture.app.data_root;
		fixture.app.hidden_agent_chat_ownership.chats.push_back(child);
		fixture.app.agent_runs.push_back(run);
		UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, run));
		return run.id;
	}

	void AdmitWorktreeFamily(PortFixture& fixture)
	{
		std::string error;
		fixture.app.worktree_cleanup_family = uam::ManagedSubagentControlService::CaptureWorktreeFamily(fixture.app, fixture.app.chats.front().id, &error);
		UAM_ASSERT(fixture.app.worktree_cleanup_family);
		const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.app.chats.front().id);
		UAM_ASSERT(revision);
		UAM_ASSERT(uam::ManagedSubagentControlService::BindWorktreeFamilyParentRevision(*fixture.app.worktree_cleanup_family, *revision, &error));
		for (const auto& id : fixture.app.worktree_cleanup_family->ChatIds()) fixture.app.worktree_operation_chat_ids.insert(id);
	}
}

UAM_TEST(GitWorktreeFinishedManagedFamilyPermitsPortAndDiscardPreservingHistory)
{
	if (!GitAvailableForTests()) return;
	for (const bool discard : {false, true})
	{
		PortFixture fixture("patch");
		const auto parent = AddFinishedWorktreeChild(fixture);
		AddFinishedWorktreeChild(fixture, parent);
		const auto child_id = fixture.app.hidden_agent_chat_ownership.chats.front().id;
		const auto before = ChatRepository::ChatStorageRevision(fixture.app.data_root, child_id);
		AdmitWorktreeFamily(fixture);
		const auto result = discard ? uam::GitWorktreeService().DiscardChatChanges(fixture.app, fixture.app.chats.front()) : uam::GitWorktreeService().PortChatChanges(fixture.app, fixture.app.chats.front());
		UAM_ASSERT(result.ok);
		UAM_ASSERT(!fs::exists(fixture.worktree));
		UAM_ASSERT(!RunGitForTest(fixture.source, "show-ref --verify refs/heads/" + fixture.chat_branch));
		UAM_ASSERT(before == ChatRepository::ChatStorageRevision(fixture.app.data_root, child_id));
		const auto history = ChatRepository::LoadLocalChat(fixture.app.data_root, child_id, true);
		UAM_ASSERT(history && history->native_session_id == "retained-native-history");
		UAM_ASSERT_EQ(history->workspace_directory, fixture.worktree.string());
	}
}

UAM_TEST(GitWorktreeManagedFamilyRefusesActiveQueuedStoppingAndHeldClaims)
{
	if (!GitAvailableForTests()) return;
	for (int claim = 0; claim < 8; ++claim)
	{
		PortFixture fixture("patch");
		const auto run_id = AddFinishedWorktreeChild(fixture);
		auto& child = fixture.app.hidden_agent_chat_ownership.chats.front();
		if (claim == 0) fixture.app.agent_runs.front().status = "running";
		if (claim == 1) fixture.app.queued_agent_run_ids.push_back(run_id);
		if (claim == 2) fixture.app.pending_agent_run_cancellation_ids.insert(run_id);
		if (claim == 3) child.acp_queue_held = true;
		if (claim == 4) child.remote_stop_cleanup_pending = true;
		if (claim == 5) fixture.app.pending_chat_save_at_by_chat_id[child.id] = 1;
		if (claim == 6) fixture.app.execution_routing_restored_unresolved_chat_ids.insert(child.id);
		if (claim == 7)
		{
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = child.id; // Even idle live runtimes must complete ordinary Stop.
			fixture.app.acp_sessions.push_back(std::move(session));
		}
		std::string error;
		UAM_ASSERT(!uam::ManagedSubagentControlService::CaptureWorktreeFamily(fixture.app, fixture.app.chats.front().id, &error));
		UAM_ASSERT(!error.empty());
		fixture.AssertRetained();
	}
}

UAM_TEST(GitWorktreeManagedFamilyRefusesReboundAndForgedLineage)
{
	if (!GitAvailableForTests()) return;
	for (int kind = 0; kind < 3; ++kind)
	{
		PortFixture fixture("patch");
		AddFinishedWorktreeChild(fixture);
		if (kind == 0) fixture.app.hidden_agent_chat_ownership.chats.front().agent_run_id = "forged";
		if (kind == 1) fixture.app.agent_runs.front().parent_run_id = uam::AgentRunLedger::NewRunId();
		if (kind == 2) fixture.app.hidden_agent_chat_ownership.chats.front().workspace_directory = fixture.source.string();
		std::string error;
		UAM_ASSERT(!uam::ManagedSubagentControlService::CaptureWorktreeFamily(fixture.app, fixture.app.chats.front().id, &error));
		fixture.AssertRetained();
	}
}

UAM_TEST(GitWorktreeManagedFamilyChangedSavedRevisionRetainsFilesAndBranch)
{
	if (!GitAvailableForTests()) return;
	PortFixture fixture("patch");
	AddFinishedWorktreeChild(fixture);
	AdmitWorktreeFamily(fixture);
	std::string error;
	UAM_ASSERT(uam::ManagedSubagentControlService::ValidateWorktreeFamily(*fixture.app.worktree_cleanup_family, &error));
	auto child = fixture.app.hidden_agent_chat_ownership.chats.front();
	child.title = "Changed after worker admission";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, child));
	UAM_ASSERT(!uam::GitWorktreeService().DiscardChatChanges(fixture.app, fixture.app.chats.front()).ok);
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreeManagedFamilyDoesNotExemptUnrelatedLiveReferencesOrBranchRemoval)
{
	if (!GitAvailableForTests()) return;
	PortFixture fixture("patch");
	AddFinishedWorktreeChild(fixture);
	ChatSession alias = fixture.app.chats.front();
	alias.id += "-unsaved-independent";
	fixture.app.chats.push_back(alias);
	AdmitWorktreeFamily(fixture);
	// Match the bridge worker: the unrelated reference exists only in the immutable captured admission.
	fixture.app.chats.pop_back();
	UAM_ASSERT(!uam::GitWorktreeService().DiscardChatChanges(fixture.app, fixture.app.chats.front()).ok);
	std::string error;
	UAM_ASSERT(!uam::GitWorktreeService().RemoveUnusedBranchWorktree(fixture.app, fixture.app.chats.front(), &error));
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreeManagedFamilyFencesRetireAfterExpiredWorkerSettles)
{
	PortFixture fixture("patch");
	AddFinishedWorktreeChild(fixture);
	AdmitWorktreeFamily(fixture);
	const auto ids = fixture.app.worktree_cleanup_family->ChatIds();
	auto request = std::make_shared<uam::AppState::GitPortRequest>();
	request->family_fenced_ids = ids;
	request->lifetime_bound = true;
	request->worker_state.store(uam::AppState::GitPortRequest::WorkerState::Running);
	request->lease = fixture.app.worktree_retention_gate.TryAcquireMutation();
	fixture.app.git_port_requests[fixture.app.chats.front().id] = request;
	uam::PollGitPortRequests(fixture.app);
	for (const auto& id : ids) UAM_ASSERT(fixture.app.worktree_operation_chat_ids.contains(id));
	request->worker_state.store(uam::AppState::GitPortRequest::WorkerState::Finished);
	uam::PollGitPortRequests(fixture.app);
	for (const auto& id : ids) UAM_ASSERT(!fixture.app.worktree_operation_chat_ids.contains(id));
	UAM_ASSERT(fixture.app.git_port_requests.empty());
	UAM_ASSERT(fixture.app.worktree_retention_gate.TryAcquireCleanup());
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreeCleanupRefusesReplacedParentContentBeforeFirstValidation)
{
	if (!GitAvailableForTests()) return;
	PortFixture fixture("patch");
	AddFinishedWorktreeChild(fixture);
	auto& parent = fixture.app.chats.front();
	parent.messages.push_back({MessageRole::User, "original parent input", "now"});
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, parent));
	AdmitWorktreeFamily(fixture); // Captures the physical revision before the worker candidate can be used.
	auto replacement = parent;
	replacement.messages.front().content = "newer parent input";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, replacement));
	const auto replacement_revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, parent.id);
	UAM_ASSERT(!uam::GitWorktreeService().DiscardChatChanges(fixture.app, parent).ok);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, parent.id), replacement_revision);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChatPrimaryReadOnly(fixture.app.data_root, parent.id, true)->messages.front().content, std::string("newer parent input"));
	fixture.AssertRetained();
}

UAM_TEST(GitWorktreeOrdinaryLargeParentCleanupPreservesHistory)
{
	if (!GitAvailableForTests()) return;
	for (const bool discard : {false, true})
	{
		PortFixture fixture("patch");
		auto& parent = fixture.app.chats.front();
		parent.messages.push_back({MessageRole::User, std::string(5U * 1024U * 1024U, 'x'), "now"});
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, parent));
		AdmitWorktreeFamily(fixture);
		const auto result = discard ? uam::GitWorktreeService().DiscardChatChanges(fixture.app, parent) : uam::GitWorktreeService().PortChatChanges(fixture.app, parent);
		UAM_ASSERT(result.ok);
		const auto saved = ChatRepository::LoadLocalChatPrimaryReadOnly(fixture.app.data_root, parent.id, true);
		UAM_ASSERT(saved && saved->messages.size() == 1);
		UAM_ASSERT_EQ(saved->messages.front().content, std::string(5U * 1024U * 1024U, 'x'));
	}
}

UAM_TEST(GitWorktreeCleanupScopesOutstandingClaimsToItsFamily)
{
	if (!GitAvailableForTests()) return;
	for (const bool related : {false, true})
	{
		PortFixture fixture("patch");
		AddFinishedWorktreeChild(fixture);
		auto stop = std::make_unique<uam::PendingAcpRemoteStop>();
		stop->chat_id = related ? fixture.app.hidden_agent_chat_ownership.chats.front().id : "unrelated-root";
		fixture.app.pending_acp_remote_stops.push_back(std::move(stop));
		fixture.app.managed_initial_mentions.emplace("unrelated-root", nullptr);
		fixture.app.managed_followups.emplace("unrelated-run", nullptr);
		fixture.app.managed_initial_runtime_owners.emplace("unrelated-run", nullptr);
		std::string error;
		const auto family = uam::ManagedSubagentControlService::CaptureWorktreeFamily(fixture.app, fixture.app.chats.front().id, &error);
		UAM_ASSERT(static_cast<bool>(family) == !related);
		fixture.AssertRetained();
	}
}
