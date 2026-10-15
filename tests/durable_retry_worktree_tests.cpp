#include "test_harness.h"
#include "app/worktree_launch_preparation_service.h"
#include "app/opencode_compaction_sync_service.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

namespace
{
	struct RetryWorkspaceFixture
	{
		TempDir temp{"uam-durable-workspace-retry"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;
		std::shared_ptr<uam::WorktreeLaunchPreparationTask> task;
		std::unique_ptr<ScopedEnvVar> private_path;

		RetryWorkspaceFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
			app.settings.central_provider_configuration.enabled = false;
			ChatSession chat;
			chat.id = "private-durable-retry";
			chat.created_at = "2026-10-15T00:00:00Z";
			chat.provider_id = "gemini-cli";
			chat.workspace_directory = temp.root.string();
			chat.branch_root_chat_id = "private-source";
			chat.approval_mode.clear();
			chat.memory_enabled = false;
			chat.svn_update_pending = true;
			chat.messages = {
			    {.role = MessageRole::User, .content = "Earlier delivered input", .created_at = "2026-10-15T00:00:01Z"},
			    {.role = MessageRole::Assistant, .content = "Earlier delivered answer", .created_at = "2026-10-15T00:00:02Z"},
			    {.role = MessageRole::User, .content = "Older unsent attempt", .created_at = "2026-10-15T00:00:03Z", .interrupted = true, .acp_prompt_not_sent = true},
			    {.role = MessageRole::User, .content = "Retry this exact input", .created_at = "2026-10-15T00:00:04Z", .interrupted = true, .acp_prompt_not_sent = true}};
			chat.messages.back().markdown_store_files = {"private-skill.md"};
			chat.messages.back().markdown_store_prompt_blocks = {"CAPTURED_PRIVATE_SKILL"};
			chat.messages.back().attachments = {{.id = "private-attachment", .name = "diagram.png", .kind = "image", .path = "private/diagram.png"}};
			app.chats.push_back(std::move(chat));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			session = &uam::acp_detail::EnsureAcpSessionForChat(app, app.chats.front());
			task = std::make_shared<uam::WorktreeLaunchPreparationTask>();
			task->data_root = app.data_root;
			task->original = app.chats.front();
			task->resolved_source = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, app.chats.front()));
			task->base_branch = app.settings.git_default_base_branch;
			task->pull_before_chat = app.settings.git_pull_before_chat;
			task->git_enabled = app.settings.git_enabled;
			task->automatic_worktree = app.settings.git_auto_worktree;
			task->svn_enabled = app.settings.svn_enabled;
			task->svn_update_before_chat = app.settings.svn_update_before_chat;
			task->update_only = true;
			app.worktree_launch_preparations.emplace(app.chats.front().id, task);
		}

		~RetryWorkspaceFixture()
		{
			for (const std::pair<const std::string, std::shared_ptr<uam::WorktreeLaunchPreparationTask>>& entry : app.worktree_launch_preparations)
			{
				entry.second->cancellation->request_stop();
				if (entry.second->worker.valid()) entry.second->worker.wait();
			}
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(*session, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*session);
		}

		void AwaitSave()
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id) && std::chrono::steady_clock::now() < deadline)
			{
				uam::FlushPendingChatSaves(app);
				std::this_thread::yield();
			}
			UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id));
		}

		void Admit()
		{
			std::string error;
			UAM_ASSERT(uam::RetryLastAcpPrompt(app, app.chats.front().id, &error));
			UAM_ASSERT(error.empty());
			AwaitSave();
			UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(app, *session, app.chats.front()));
			UAM_ASSERT(session->worktree_launch_pending);
		}

		void RemoteReady()
		{
			app.worktree_launch_preparations.clear();
			app.settings.git_enabled = app.settings.git_auto_worktree = false;
			ChatSession& chat = app.chats.front();
			chat.svn_update_pending = false;
			chat.execution_host_id = "private-remote-retry";
			chat.workspace_directory = "/private/remote-retry-source";
			chat.native_session_id = "private-existing-native-session";
			chat.remote_process_exists = true;
			ExecutionHost host;
			host.id = chat.execution_host_id; host.transport = "ssh"; host.platform = "linux";
			host.ssh_alias = "private-never-connect"; host.runner_status = "ready";
			host.runner_version = std::string(uam::constants::kAppVersion).substr(1);
			host.runner_protocol_version = 3;
			app.settings.execution_hosts.push_back(host);
			session->session_id = chat.native_session_id;
			session->process_execution_host_id = chat.execution_host_id;
			StartPrivateSink();
			session->initialized = session->session_ready = true;
			session->next_request_id = 17;
		}

		void StartPrivateSink()
		{
#if defined(_WIN32)
			const std::vector<std::string> argv = {"cmd", "/C", "more > NUL"};
#else
			const std::vector<std::string> argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
			std::string error;
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*session, temp.root, argv, &error));
			session->running = true;
			session->session_ready = false;
		}
	};
}

UAM_TEST(AcpWorkspaceRetryPersistsOneRawReferencedIntent)
{
	RetryWorkspaceFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	const std::vector<Message> original = chat.messages;
	std::string error;
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	UAM_ASSERT(uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
	UAM_ASSERT(std::chrono::steady_clock::now() - started <= std::chrono::milliseconds(100));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT(!fixture.session->running && !fixture.session->processing && fixture.session->prompt_request_id == 0);
	UAM_ASSERT(!fixture.session->worktree_launch_pending);
	fixture.AwaitSave();
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->acp_queued_prompts.size() == 1);
	const uam::AcpQueuedUserPromptState& prompt = saved->acp_queued_prompts.front();
	UAM_ASSERT(!prompt.id.empty() && !prompt.append_user_message && prompt.include_prior_context);
	UAM_ASSERT_EQ(prompt.text, original.back().content);
	UAM_ASSERT_EQ(prompt.existing_user_message_index, 3);
	UAM_ASSERT_EQ(prompt.existing_user_message_created_at, original.back().created_at);
	UAM_ASSERT(prompt.markdown_store_files == original.back().markdown_store_files);
	UAM_ASSERT(prompt.markdown_store_prompt_blocks == original.back().markdown_store_prompt_blocks);
	UAM_ASSERT(prompt.attachments == original.back().attachments);
	UAM_ASSERT(!prompt.uam_agent_definition_hash.empty() && !prompt.uam_agent_definition_snapshot.empty());
	UAM_ASSERT_EQ(chat.messages.size(), original.size());
	UAM_ASSERT_EQ(chat.messages.back().created_at, original.back().created_at);
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
	UAM_ASSERT(fixture.session->worktree_launch_pending);
}

UAM_TEST(AcpWorkspaceFailedMessageRetryRetainsBranchIntent)
{
	RetryWorkspaceFixture fixture;
	std::string error;
	UAM_ASSERT(uam::RetryFailedAcpMessage(fixture.app, fixture.app.chats.front().id, 3, &error));
	UAM_ASSERT(error.empty());
	fixture.AwaitSave();
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().existing_user_message_index, 3);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{4});
}

UAM_TEST(AcpWorkspaceRetryDuplicateDoesNotResnapshotOrEnqueue)
{
	RetryWorkspaceFixture fixture;
	std::string error;
	UAM_ASSERT(uam::RetryLastAcpPrompt(fixture.app, fixture.app.chats.front().id, &error));
	const uam::AcpQueuedUserPromptState original = fixture.session->queued_user_prompts.front();
	fixture.app.chats.front().uam_agent_id = "plan";
	UAM_ASSERT(uam::RetryLastAcpPrompt(fixture.app, fixture.app.chats.front().id, &error));
	fixture.AwaitSave();
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, fixture.app.chats.front()));
	UAM_ASSERT(uam::RetryLastAcpPrompt(fixture.app, fixture.app.chats.front().id, &error));
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT(fixture.session->queued_user_prompts.front() == original);
}

UAM_TEST(AcpWorkspaceRetryReloadRestoresHeldOriginalIdentity)
{
	RetryWorkspaceFixture fixture;
	fixture.Admit();
	const uam::AcpQueuedUserPromptState original = fixture.session->queued_user_prompts.front();
	uam::AppState reloaded;
	reloaded.data_root = fixture.app.data_root;
	reloaded.provider_profiles = fixture.app.provider_profiles;
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(reloaded.data_root, fixture.app.chats.front().id);
	UAM_ASSERT(saved.has_value());
	reloaded.chats.push_back(*saved);
	UAM_ASSERT_EQ(uam::RestoreRemoteAcpSessionsAfterRestart(reloaded, true), std::size_t{1});
	const uam::AcpSessionState* restored = uam::FindAcpSessionForChat(reloaded, saved->id);
	UAM_ASSERT(restored && restored->queue_held && reloaded.chats.front().acp_queue_held);
	UAM_ASSERT_EQ(restored->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT(restored->queued_user_prompts.front() == original);
	UAM_ASSERT(!restored->running && restored->prompt_request_id == 0);
}

UAM_TEST(AcpWorkspaceRetryDispatchUsesCapturedSkillsAndBranchHistory)
{
	RetryWorkspaceFixture fixture;
	fixture.Admit();
	ChatSession& chat = fixture.app.chats.front();
	fixture.task->saved = true;
	UAM_ASSERT_EQ(uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, *fixture.session, chat), uam::WorktreeLaunchPreparationState::Ready);
	chat.svn_update_pending = false;
	chat.uam_agent_id = "plan";
	fixture.StartPrivateSink();
	UAM_ASSERT(uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
	UAM_ASSERT(fixture.session->processing && fixture.session->queued_user_prompts.empty());
	UAM_ASSERT_EQ(fixture.session->active_uam_agent_id, std::string("build"));
	UAM_ASSERT(fixture.session->queued_prompt.find("Earlier delivered input") != std::string::npos);
	UAM_ASSERT(fixture.session->queued_prompt.find("Earlier delivered answer") != std::string::npos);
	UAM_ASSERT(fixture.session->queued_prompt.find("Older unsent attempt") == std::string::npos);
	UAM_ASSERT(fixture.session->queued_prompt.find("CAPTURED_PRIVATE_SKILL") != std::string::npos);
	UAM_ASSERT(fixture.session->queued_prompt.find("private/diagram.png") != std::string::npos);
	UAM_ASSERT(fixture.session->queued_prompt.find("Current user message:\nRetry this exact input") != std::string::npos);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{4});
	UAM_ASSERT_EQ(chat.messages.back().content, std::string("Retry this exact input"));
}

UAM_TEST(AcpWorkspaceRetryFailureAndCancellationHoldOriginalIntent)
{
	for (const bool canceled : {false, true})
	{
		RetryWorkspaceFixture fixture;
		fixture.Admit();
		const uam::AcpQueuedUserPromptState original = fixture.session->queued_user_prompts.front();
		if (canceled) fixture.task->cancellation->request_stop();
		else fixture.task->error = "Private preparation failure";
		UAM_ASSERT_EQ(uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, *fixture.session, fixture.app.chats.front()), uam::WorktreeLaunchPreparationState::Failed);
		fixture.AwaitSave();
		UAM_ASSERT(fixture.session->queue_held && fixture.app.chats.front().acp_queue_held);
		UAM_ASSERT(!fixture.session->last_error.empty());
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT(fixture.session->queued_user_prompts.front() == original);
		UAM_ASSERT(!fixture.session->running && !fixture.session->processing && fixture.session->prompt_request_id == 0);
	}
}

UAM_TEST(AcpWorkspaceRetryChangedOrDeletedRowNeverDispatches)
{
	for (const bool deleted : {false, true})
	{
		RetryWorkspaceFixture fixture;
		fixture.Admit();
		ChatSession& chat = fixture.app.chats.front();
		fixture.task->saved = true;
		UAM_ASSERT_EQ(uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, *fixture.session, chat), uam::WorktreeLaunchPreparationState::Ready);
		chat.svn_update_pending = false;
		if (deleted) chat.messages.pop_back();
		else chat.messages.back().content = "Changed after retry admission";
		fixture.StartPrivateSink();
		UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
		UAM_ASSERT(fixture.session->queue_held && chat.acp_queue_held);
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT(!fixture.session->processing && fixture.session->prompt_request_id == 0);
		UAM_ASSERT(!fixture.session->last_error.empty());
	}
}

UAM_TEST(AcpWorkspaceRetrySaveFailureNeverPreparesOrLaunches)
{
	RetryWorkspaceFixture fixture;
	std::string error;
	UAM_ASSERT(uam::RetryLastAcpPrompt(fixture.app, fixture.app.chats.front().id, &error));
	std::optional<bool> saved;
	UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, fixture.app.chats.front().id, [&saved](bool value) { saved = value; }));
	fs::remove_all(fixture.app.data_root);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.app.data_root, "private unavailable storage"));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!saved.has_value() && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(fixture.app);
		std::this_thread::yield();
	}
	UAM_ASSERT(saved.has_value() && !*saved);
	UAM_ASSERT(fixture.session->queue_held && fixture.app.chats.front().acp_queue_held);
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{4});
	UAM_ASSERT(!fixture.session->worktree_launch_pending && !fixture.session->running && !fixture.session->processing);
	UAM_ASSERT(fixture.app.worktree_operation_chat_ids.empty());
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
}

UAM_TEST(AcpWorkspaceRetryStaleHostOrWorkspaceNeverDispatches)
{
	for (const bool host_changed : {false, true})
	{
		RetryWorkspaceFixture fixture;
		fixture.Admit();
		if (host_changed) fixture.app.chats.front().execution_host_id = "private-replacement-host";
		else fixture.app.chats.front().workspace_directory = (fixture.temp.root / "changed-workspace").string();
		UAM_ASSERT_EQ(uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, *fixture.session, fixture.app.chats.front()), uam::WorktreeLaunchPreparationState::Failed);
		UAM_ASSERT(fixture.session->queue_held && fixture.app.chats.front().acp_queue_held);
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT(!fixture.session->running && !fixture.session->processing && fixture.session->prompt_request_id == 0);
	}
}

UAM_TEST(AcpWorkspaceRetryRefusesAdmissionDuringMutation)
{
	RetryWorkspaceFixture fixture;
	fixture.app.worktree_operation_chat_ids.insert(fixture.app.chats.front().id);
	std::string error;
	UAM_ASSERT(!uam::RetryLastAcpPrompt(fixture.app, fixture.app.chats.front().id, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(fixture.session->queued_user_prompts.empty());
	UAM_ASSERT(fixture.app.chats.front().acp_queued_prompts.empty());
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{4});
}

UAM_TEST(AcpWorkspaceNativeRetryKeepsJournalWhileStoppingIdleProvider)
{
	RetryWorkspaceFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.native_session_id = fixture.session->session_id = "private-existing-native-session";
	fixture.task->original.native_session_id = chat.native_session_id;
	fixture.StartPrivateSink();
	fixture.session->session_ready = true;
	fixture.session->initialized = true;
	fixture.session->process_execution_host_id = chat.execution_host_id;
	std::string error;
	UAM_ASSERT(uam::RetryFailedAcpMessage(fixture.app, chat.id, 3, &error));
	fixture.AwaitSave();
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	const uam::AcpQueuedUserPromptState original = fixture.session->queued_user_prompts.front();
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
	UAM_ASSERT(fixture.session->worktree_launch_pending && !fixture.session->running);
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT(fixture.session->queued_user_prompts.front() == original);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->acp_queued_prompts.size() == 1);
	UAM_ASSERT(saved->acp_queued_prompts.front() == original);
	UAM_ASSERT_EQ(saved->messages.size(), std::size_t{4});
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
}

UAM_TEST(AcpWorkspaceRetryContextFlagRejectsMalformedPersistence)
{
	RetryWorkspaceFixture fixture;
	fixture.Admit();
	const fs::path path = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.app.chats.front().id);
	const nlohmann::json original = nlohmann::json::parse(ReadFile(path));
	for (int variant = 0; variant < 4; ++variant)
	{
		nlohmann::json changed = original;
		nlohmann::json& prompt = changed["acp_queued_prompts"][0];
		if (variant == 0) prompt["include_prior_context"] = "true";
		if (variant == 1) prompt["append_user_message"] = true;
		if (variant == 2) prompt["prepared_for_delivery"] = true;
		if (variant == 3) prompt.erase("existing_user_message_index");
		UAM_ASSERT(uam::io::WriteTextFile(path, changed.dump()));
		const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
		UAM_ASSERT(loaded.has_value() && loaded->acp_queued_prompts.empty());
	}
}

UAM_TEST(AcpWorkspaceRetryActiveTurnRejectsDuplicateWithoutNewInput)
{
	RetryWorkspaceFixture fixture;
	fixture.Admit();
	ChatSession& chat = fixture.app.chats.front();
	fixture.task->saved = true;
	UAM_ASSERT_EQ(uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, *fixture.session, chat), uam::WorktreeLaunchPreparationState::Ready);
	chat.svn_update_pending = false;
	fixture.StartPrivateSink();
	UAM_ASSERT(uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
	const int serial = fixture.session->turn_serial;
	const std::string staged = fixture.session->queued_prompt;
	chat.uam_agent_id = "private-missing-agent";
	std::string error;
	UAM_ASSERT(!uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
	UAM_ASSERT(error.find("pending runtime work") != std::string::npos);
	UAM_ASSERT_EQ(fixture.session->turn_serial, serial);
	UAM_ASSERT_EQ(fixture.session->queued_prompt, staged);
	UAM_ASSERT(fixture.session->queued_user_prompts.empty());
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{4});
}

UAM_TEST(AcpDirectRetryRejectsInvalidPriorSkillBeforeAgentOrGoalMutation)
{
	for (const bool missing_agent : {false, true})
	{
		RetryWorkspaceFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		fixture.app.worktree_launch_preparations.clear();
		chat.svn_update_pending = false;
		chat.small_model_mode = true;
		chat.messages.front().markdown_store_files = {"missing-original-snapshot.md"};
		if (missing_agent) chat.uam_agent_id = "private-unavailable-agent";
		std::string error;
		UAM_ASSERT(!uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
		UAM_ASSERT_EQ(error, std::string("The original attached skill snapshots are unavailable."));
		UAM_ASSERT(chat.goals.empty() && chat.active_goal_id.empty());
		UAM_ASSERT(chat.acp_queued_prompts.empty() && fixture.session->queued_user_prompts.empty());
		UAM_ASSERT(!fixture.session->processing && !fixture.session->running && fixture.session->prompt_request_id == 0);
		UAM_ASSERT_EQ(chat.messages.size(), std::size_t{4});
	}
}

UAM_TEST(AcpDirectRetryPreservesIdleDiscoveryAndReconnectTakeover)
{
	for (const bool discovery : {false, true})
	{
		RetryWorkspaceFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		fixture.app.worktree_launch_preparations.clear();
		chat.svn_update_pending = false;
		fixture.StartPrivateSink();
		fixture.session->model_discovery_only = discovery;
		fixture.session->processing = discovery;
		fixture.session->reconnect_pending = true;
		std::string error;
		UAM_ASSERT(uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
		UAM_ASSERT(error.empty());
		UAM_ASSERT(fixture.session->processing && fixture.session->turn_serial == 1);
		UAM_ASSERT(fixture.session->queued_prompt.find("Retry this exact input") != std::string::npos);
		UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
		UAM_ASSERT_EQ(chat.messages.size(), std::size_t{4});
	}
}

UAM_TEST(AcpWorkspaceHeldRetryRequiresExplicitQueueResume)
{
	RetryWorkspaceFixture fixture;
	fixture.Admit();
	fixture.task->error = "Private held retry preparation failure";
	UAM_ASSERT_EQ(uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, *fixture.session, fixture.app.chats.front()), uam::WorktreeLaunchPreparationState::Failed);
	fixture.AwaitSave();
	const uam::AcpQueuedUserPromptState original = fixture.session->queued_user_prompts.front();
	for (const bool failed_message : {false, true})
	{
		std::string error;
		const bool accepted = failed_message
		    ? uam::RetryFailedAcpMessage(fixture.app, fixture.app.chats.front().id, 3, &error)
		    : uam::RetryLastAcpPrompt(fixture.app, fixture.app.chats.front().id, &error);
		UAM_ASSERT(!accepted);
		UAM_ASSERT_EQ(error, std::string("Resume the held queue to retry this message."));
		UAM_ASSERT(fixture.session->queue_held && fixture.app.chats.front().acp_queue_held);
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT(fixture.session->queued_user_prompts.front() == original);
		UAM_ASSERT(!fixture.session->processing && fixture.session->prompt_request_id == 0);
	}
}

UAM_TEST(AcpWorkspaceRetryStopFailurePersistsHoldAndReason)
{
	RetryWorkspaceFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	std::string error;
	UAM_ASSERT(uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
	fixture.AwaitSave();
	const uam::AcpQueuedUserPromptState original = fixture.session->queued_user_prompts.front();
	fixture.StartPrivateSink();
	fixture.session->local_stop_pending = true;
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
	UAM_ASSERT(fixture.session->queue_held && chat.acp_queue_held);
	UAM_ASSERT_EQ(fixture.session->last_error, std::string("Could not stop the idle provider before preparing the retry workspace."));
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains(chat.id));
	fixture.AwaitSave();
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->acp_queue_held && saved->acp_queued_prompts.size() == 1);
	UAM_ASSERT(saved->acp_queued_prompts.front() == original);
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{4});
}


UAM_TEST(AcpRemoteQueuedPrewriteFailureRestoresRawSnapshotsAndCount)
{
	RetryWorkspaceFixture fixture;
	fixture.RemoteReady();
	ChatSession& chat = fixture.app.chats.front();
	uam::AcpQueuedUserPromptState raw;
	raw.id = "private-raw-head"; raw.revision = 4; raw.text = "Original authored queued input";
	raw.markdown_store_files = {"head-skill.md"}; raw.markdown_store_prompt_blocks = {"ORIGINAL_HEAD_SKILL"};
	raw.attachments = {{.id = "head-attachment", .kind = "file", .path = "private-head.txt"}};
	uam::AcpQueuedUserPromptState future;
	future.id = "private-raw-future"; future.revision = 9; future.text = "Future captured input";
	fixture.session->queued_user_prompts = {raw, future};
	chat.acp_queued_prompts = {raw, future};
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	const std::vector<Message> original = chat.messages;
	fixture.app.opencode_compaction_before_dispatch = [](uam::AcpSessionState&, ChatSession&, std::string& error)
	{
		error = "Private metadata prewrite rejection";
		return uam::OpenCodeCompactionGateState::Failed;
	};
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
	fixture.AwaitSave();
	UAM_ASSERT(chat.messages == original);
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
	UAM_ASSERT_EQ(fixture.session->next_request_id, 17);
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{2});
	UAM_ASSERT(fixture.session->queued_user_prompts.front() == raw);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->acp_queued_prompts == std::vector<uam::AcpQueuedUserPromptState>({raw, future}));
	UAM_ASSERT_EQ(saved->acp_dispatched_queued_prompt_count, std::size_t{0});
	UAM_ASSERT(saved->messages == original);
	UAM_ASSERT(!saved->remote_turn_reconnect_pending && saved->remote_pending_requests.empty());
}

UAM_TEST(AcpRemoteQueuedUncertainErrorRetainsTranscriptAndDeliveryAuthority)
{
	RetryWorkspaceFixture fixture;
	fixture.RemoteReady();
	ChatSession& chat = fixture.app.chats.front();
	uam::AcpQueuedUserPromptState raw;
	raw.id = "private-uncertain-head"; raw.text = "Possibly delivered authored input";
	uam::AcpQueuedUserPromptState future;
	future.id = "private-uncertain-future"; future.text = "Future captured input";
	fixture.session->queued_user_prompts = {raw, future};
	chat.acp_queued_prompts = {raw, future};
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	const std::size_t original_count = chat.messages.size();
	fixture.app.opencode_compaction_before_dispatch = [](uam::AcpSessionState& session, ChatSession& owner, std::string& error)
	{
		// Model the transport's committed uncertainty before its error reaches StartBatch.
		owner.remote_turn_reconnect_pending = true;
		owner.remote_prompt_delivery_session_id = "private-remote-session";
		owner.remote_prompt_delivery_id = "private-delivery-identity";
		owner.remote_prompt_delivery_payload = "private-immutable-payload";
		owner.remote_turn_user_message_index = session.turn_user_message_index;
		owner.remote_pending_requests.push_back({.request_id = 31, .method = "session/prompt", .turn_serial = session.turn_serial});
		error = "Private delivery acknowledgement was lost";
		return uam::OpenCodeCompactionGateState::Failed;
	};
	UAM_ASSERT(uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
	fixture.AwaitSave();
	UAM_ASSERT_EQ(chat.messages.size(), original_count + 1);
	UAM_ASSERT_EQ(chat.messages.back().content, raw.text);
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT(fixture.session->queued_user_prompts.front() == future);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->remote_turn_reconnect_pending && saved->remote_pending_requests.size() == 1);
	UAM_ASSERT_EQ(saved->remote_prompt_delivery_payload, std::string("private-immutable-payload"));
	UAM_ASSERT_EQ(saved->messages.back().content, raw.text);
	UAM_ASSERT(saved->acp_queued_prompts == std::vector<uam::AcpQueuedUserPromptState>({future}));
}

UAM_TEST(AcpRemoteQueuedRecoveryAuthorityRejectsDrainWithoutMutatingRawInput)
{
	for (int variant = 0; variant < 4; ++variant)
	{
		RetryWorkspaceFixture fixture;
		fixture.RemoteReady();
		ChatSession& chat = fixture.app.chats.front();
		uam::AcpQueuedUserPromptState raw;
		raw.id = "private-pending-recovery"; raw.text = "Keep exact captured future input";
		fixture.session->queued_user_prompts = {raw};
		chat.acp_queued_prompts = {raw};
		if (variant == 0) chat.remote_turn_reconnect_pending = true;
		if (variant == 1) chat.remote_pending_requests.push_back({.request_id = 22, .method = "session/prompt", .turn_serial = 1});
		if (variant == 2) chat.remote_prompt_delivery_id = "private-unconfirmed-delivery";
		if (variant == 3) fixture.session->recovering_remote_process = true;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
		const std::vector<Message> original = chat.messages;
		UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
		UAM_ASSERT(chat.messages == original);
		UAM_ASSERT(chat.acp_queued_prompts == std::vector<uam::AcpQueuedUserPromptState>({raw}));
		UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, std::size_t{0});
		UAM_ASSERT_EQ(fixture.session->turn_serial, 0);
		UAM_ASSERT_EQ(fixture.session->next_request_id, 17);
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT(fixture.session->queued_user_prompts.front() == raw);
	}
}

UAM_TEST(AcpRemoteIsolatedRetryLastRetainsFirstAttemptBeforeWorkspaceRevalidation)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	RetryWorkspaceFixture fixture;
	fixture.RemoteReady();
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "ssh", "#!/bin/sh\nexit 1\n"));
	fs::permissions(fixture.temp.root / "ssh", fs::perms::owner_all);
	fixture.private_path = std::make_unique<ScopedEnvVar>("PATH", fixture.temp.root.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin"));
	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*fixture.session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*fixture.session);
	fixture.session->running = fixture.session->session_ready = false;
	ChatSession& chat = fixture.app.chats.front();
	chat.remote_process_exists = false;
	chat.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
	chat.workspace_source_directory = chat.workspace_directory;
	chat.workspace_worktree_directory = "/private/remote-retry-isolated";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	std::string error;
	UAM_ASSERT(uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	const uam::AcpQueuedUserPromptState& retry = fixture.session->queued_user_prompts.front();
	UAM_ASSERT_EQ(retry.text, chat.messages[3].content);
	UAM_ASSERT_EQ(retry.existing_user_message_index, 3);
	UAM_ASSERT_EQ(retry.existing_user_message_created_at, chat.messages[3].created_at);
	UAM_ASSERT(!retry.append_user_message && !retry.prepared_for_delivery && retry.include_prior_context);
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
	UAM_ASSERT(!fixture.session->processing && fixture.session->queued_prompt.empty());
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->acp_queued_prompts.size() == 1 && saved->acp_queued_prompts.front() == retry);
	UAM_ASSERT_EQ(saved->acp_dispatched_queued_prompt_count, std::size_t{0});
#endif
}
