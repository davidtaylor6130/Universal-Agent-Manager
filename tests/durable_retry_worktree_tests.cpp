#include "test_harness.h"
#include "cef/state_serializer.h"
#include <cerrno>
#include <cstring>
#include <future>
#include "app/worktree_launch_preparation_service.h"
#include "app/opencode_compaction_sync_service.h"
#include "common/chat/native_attachment_assets.h"
#include "common/chat/native_media_chat_save.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

namespace
{
	// DATA only: publish a retained original, but never create a provider process,
	// poll a launch intent, acquire a native binding or write provider input.
	struct ColdRemoteImageRetryFixture
	{
		TempDir temp{"uam-cold-image-retry-admission"};
		uam::AppState app;
		std::string original_bytes;

		explicit ColdRemoteImageRetryFixture(const std::string& provider)
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			app.settings.central_provider_configuration.enabled = false;
			app.settings.git_enabled = app.settings.git_auto_worktree = false;
			ExecutionHost host;
			host.id = "data-only-image-host";
			host.transport = "ssh";
			host.ssh_alias = "never-connect-image-admission";
			host.platform = "linux";
			host.runner_protocol_version = 3;
			app.settings.execution_hosts.push_back(host);
			ChatSession chat;
			chat.id = "data-only-image-retry";
			chat.created_at = "2026-10-23T12:00:00Z";
			chat.provider_id = provider;
			chat.execution_host_id = host.id;
			chat.workspace_directory = "/data-only/remote-workspace";
			chat.native_session_id = "data-only-saved-native-session";
			chat.routing_mode = "automatic";
			chat.model_id = "data-only-image-model";
			chat.memory_enabled = false;
			chat.small_model_mode = true;
			chat.messages = {{.role = MessageRole::User, .content = "Retry the retained image.", .created_at = "2026-10-23T12:00:01Z", .interrupted = true, .acp_prompt_not_sent = true}};
			chat.messages.back().markdown_store_files = {"captured-skill.md"};
			chat.messages.back().markdown_store_prompt_blocks = {"Keep the original captured instructions."};
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			const auto revision = ChatRepository::ChatStorageRevision(app.data_root, chat.id);
			UAM_ASSERT(revision);
			const std::string png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
			UAM_ASSERT(uam::base64::Decode(png, original_bytes));
			MessageAttachment image;
			image.id = "retained-image"; image.name = "original.png"; image.kind = "image";
			image.source = AttachmentSource::NativeInlinePending;
			image.native_part_key = "v1:data-only/user/image/0";
			image.native_source_field = "data-only/user/attachments/0";
			image.origin_execution_host_id = host.id;
			std::string error;
			image.native_inline_payload = uam::native_assets::MakeInlinePayload(std::string(png), "image/png", std::make_shared<uam::native_assets::PayloadBudget>(), error);
			UAM_ASSERT(image.native_inline_payload && error.empty());
			image.mime_type = image.native_inline_payload->MimeType();
			image.content_digest = image.native_inline_payload->Digest();
			image.size_bytes = image.native_inline_payload->DecodedSize();
			chat.messages.back().attachments.push_back(std::move(image));
			auto published = std::async(std::launch::async, [root = app.data_root, chat = std::move(chat), revision = *revision]() mutable
			{
				std::string error;
				auto prepared = uam::PreparedNativeMediaChatSave::Prepare(root, std::move(chat), revision, error);
				UAM_ASSERT(prepared && error.empty());
				UAM_ASSERT(prepared->Publish(error) && error.empty());
				UAM_ASSERT(prepared->AcknowledgeOriginals(error) && error.empty());
				return prepared->TakeCommittedChat();
			}).get();
			UAM_ASSERT(published);
			app.chats.push_back(std::move(*published));
		}

		void HoldOriginal()
		{
			ChatSession& chat = app.chats.front();
			const Message& row = chat.messages.front();
			uam::AcpQueuedUserPromptState prompt;
			prompt.id = "held-original-image";
			prompt.text = row.content;
			prompt.attachments = row.attachments;
			prompt.markdown_store_files = row.markdown_store_files;
			prompt.markdown_store_prompt_blocks = row.markdown_store_prompt_blocks;
			prompt.direct_admission = true;
			prompt.append_user_message = false;
			prompt.existing_user_message_index = 0;
			prompt.existing_user_message_created_at = row.created_at;
			chat.acp_queued_prompts = {prompt};
			chat.acp_queue_held = true;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		}

		void CheckRefusal(bool indexed)
		{
			CheckActionRefusal([&](std::string& error)
			{
				const std::string& id = app.chats.front().id;
				return indexed ? uam::RetryFailedAcpMessage(app, id, 0, &error) : uam::RetryLastAcpPrompt(app, id, &error);
			}, "Saved image Retry needs this chat's existing ready SSH session. Reopen it or attach the files again.");
		}

		void CheckActionRefusal(const std::function<bool(std::string&)>& action, const std::string& expected_error)
		{
			ChatSession& chat = app.chats.front();
			const ChatSession before = chat;
			const auto primary = AppPaths::UamChatFilePath(app.data_root, chat.id);
			const std::string primary_before = ReadFile(primary);
			const auto outbox = app.data_root / "queue-outboxes" / (chat.id + ".json");
			const bool outbox_existed = fs::exists(outbox);
			const std::string outbox_before = outbox_existed ? ReadFile(outbox) : std::string{};
			const std::size_t sessions_before = app.acp_sessions.size();
			std::string error;
			UAM_ASSERT(!action(error));
			UAM_ASSERT_EQ(error, expected_error);
			UAM_ASSERT_EQ(ReadFile(primary), primary_before);
			UAM_ASSERT_EQ(fs::exists(outbox), outbox_existed);
			if (outbox_existed) UAM_ASSERT_EQ(ReadFile(outbox), outbox_before);
			UAM_ASSERT_EQ(chat.messages, before.messages);
			UAM_ASSERT(chat.goals.empty() && chat.active_goal_id.empty());
			UAM_ASSERT_EQ(chat.acp_queued_prompts, before.acp_queued_prompts);
			UAM_ASSERT_EQ(chat.acp_queue_held, before.acp_queue_held);
			UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, before.acp_dispatched_queued_prompt_count);
			UAM_ASSERT_EQ(chat.native_session_id, before.native_session_id);
			UAM_ASSERT_EQ(chat.execution_host_id, before.execution_host_id);
			UAM_ASSERT_EQ(chat.workspace_directory, before.workspace_directory);
			UAM_ASSERT_EQ(chat.svn_update_pending, before.svn_update_pending);
			UAM_ASSERT(!chat.remote_process_exists && chat.remote_process_control_token.empty());
			UAM_ASSERT_EQ(chat.metadata_incarnation, before.metadata_incarnation);
			UAM_ASSERT_EQ(app.acp_sessions.size(), sessions_before);
			UAM_ASSERT(app.cli_terminals.empty() && app.worktree_launch_preparations.empty() && app.execution_routing_waits.empty());
			UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty() && app.claude_input_tasks.empty());
			for (const auto& session : app.acp_sessions)
			{
				UAM_ASSERT(!session->running && !session->processing && !session->session_ready && !session->reconnect_pending);
				UAM_ASSERT(!session->saved_image_retry && !session->pending_prompt_delivery && !session->pending_local_launch && !session->stdin_writer);
				UAM_ASSERT_EQ(session->session_id, chat.native_session_id);
				UAM_ASSERT_EQ(std::vector<uam::AcpQueuedUserPromptState>(session->queued_user_prompts.begin(), session->queued_user_prompts.end()), chat.acp_queued_prompts);
			}
			const auto& image = chat.messages.front().attachments.front();
			UAM_ASSERT(image.source == AttachmentSource::OwnedNativeAsset && image.path.empty());
			const auto original = uam::native_assets::ReadOwned(app.data_root, chat.id, image.native_asset_key);
			UAM_ASSERT(original.ok && original.bytes == original_bytes);
		}
	};

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

UAM_TEST(AcpColdRemoteImageRetryRefusesBeforeGoalOrSessionMutation)
{
	for (const std::string provider : uam::provider_ids::kAllCliProviderIds)
	{
		for (const bool stopped_session : {false, true})
		{
			ColdRemoteImageRetryFixture fixture(provider);
			if (stopped_session)
			{
				auto session = std::make_unique<uam::AcpSessionState>();
				session->chat_id = fixture.app.chats.front().id;
				session->provider_id = provider;
				session->session_id = fixture.app.chats.front().native_session_id;
				const auto& runtime = ProviderRuntimeRegistry::ResolveById(provider);
				session->protocol_kind = runtime.AcpProtocolKind();
				// A recognized protocol must still refuse a stopped holder. These
				// capability fields are DATA, with no process or native binding.
				session->initialized = session->prompt_image_supported = true;
				UAM_ASSERT(!session->protocol_kind.empty());
				UAM_ASSERT_EQ(runtime.SupportsSavedImageRetry(*session), provider != uam::provider_ids::kAntigravityCli);
				fixture.app.acp_sessions.push_back(std::move(session));
			}
			fixture.CheckRefusal(false);
		}
	}
}

UAM_TEST(AcpColdRemoteIndexedImageRetryPreservesPlaceholderAndHeldQueue)
{
	for (const std::string provider : uam::provider_ids::kAllCliProviderIds)
	{
		for (const bool held_queue : {false, true})
		{
			ColdRemoteImageRetryFixture fixture(provider);
			ChatSession& chat = fixture.app.chats.front();
			chat.svn_update_pending = true;
			chat.messages.push_back({.role = MessageRole::Assistant, .created_at = "2026-10-23T12:00:02Z"});
			if (held_queue)
			{
				uam::AcpQueuedUserPromptState future;
				future.id = "held-unrelated-future";
				future.text = "Keep this held input independent of Retry.";
				future.append_user_message = true;
				chat.acp_queued_prompts = {future};
				chat.acp_queue_held = true;
				auto session = std::make_unique<uam::AcpSessionState>();
				session->chat_id = chat.id;
				session->provider_id = chat.provider_id;
				session->session_id = chat.native_session_id;
				session->queue_held = true;
				session->queued_user_prompts.push_back(future);
				fixture.app.acp_sessions.push_back(std::move(session));
			}
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
			fixture.CheckRefusal(true);
		}
	}
}

UAM_TEST(AcpRemoteOwnedImageReceiptRetryDoesNotAcquireColdRuntime)
{
	for (const std::string provider : uam::provider_ids::kAllCliProviderIds)
	{
		ColdRemoteImageRetryFixture fixture(provider);
		fixture.HoldOriginal();
		const ChatSession& chat = fixture.app.chats.front();
		const auto receipt = chat.acp_queued_prompts.front();
		fixture.CheckActionRefusal([&](std::string& error) { return uam::RetryQueuedAcpPrompt(fixture.app, chat.id, {receipt.id, receipt.revision}, &error); },
		    "Saved image Retry needs this chat's existing ready SSH session. Reopen it or attach the files again.");
	}
}

UAM_TEST(AcpRemoteOwnedImageQueueResumeKeepsRevokedInvocationHeld)
{
	for (const std::string provider : uam::provider_ids::kAllCliProviderIds)
	{
		ColdRemoteImageRetryFixture fixture(provider);
		fixture.HoldOriginal();
		ChatSession& chat = fixture.app.chats.front();
		auto session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = chat.id;
		session->provider_id = provider;
		session->session_id = chat.native_session_id;
		session->queue_held = true;
		session->queued_user_prompts.assign(chat.acp_queued_prompts.begin(), chat.acp_queued_prompts.end());
		// Retaining copied DATA after revocation cannot reconstruct input authority.
		auto retired = std::make_shared<uam::SavedImageRetryPermission>();
		retired->root = fixture.app.data_root;
		retired->incarnation = chat.metadata_incarnation;
		retired->chat_id = chat.id;
		retired->provider_id = chat.provider_id;
		retired->execution_host_id = chat.execution_host_id;
		retired->row_index = 0;
		retired->content = chat.messages.front().content;
		retired->attachments = chat.messages.front().attachments;
		session->saved_image_retry = retired;
		uam::acp_detail::RevokeSavedImageRetry(*session);
		UAM_ASSERT(retired && !session->saved_image_retry);
		fixture.app.acp_sessions.push_back(std::move(session));
		const auto receipt = chat.acp_queued_prompts.front();
		fixture.CheckActionRefusal([&](std::string& error) { return uam::ResumeQueuedAcpPrompts(fixture.app, chat.id, {{receipt.id, receipt.revision}}, &error); },
		    "Saved image Retry needs fresh saved-message verification before this queue can resume. Your input remains held.");
	}
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
	UAM_ASSERT(fixture.session->queued_prompt.find("</UAM History>\n\nRetry this exact input") != std::string::npos);
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

UAM_TEST(AcpWorkspaceHeldSingletonResendUsesExactQueueIdentity)
{
	for (const bool failed_message : {false, true})
	{
		RetryWorkspaceFixture fixture;
		fixture.Admit();
		fixture.task->error = "Private held retry preparation failure";
		UAM_ASSERT_EQ(uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, *fixture.session, fixture.app.chats.front()), uam::WorktreeLaunchPreparationState::Failed);
		fixture.AwaitSave();
		ChatSession& chat = fixture.app.chats.front();
		const auto original_queue = fixture.session->queued_user_prompts;
		const auto original_messages = chat.messages;
		const int next_request = fixture.session->next_request_id;
		const auto resend = [&](std::string& error)
		{
			return failed_message ? uam::RetryFailedAcpMessage(fixture.app, chat.id, 3, &error)
			    : uam::RetryLastAcpPrompt(fixture.app, chat.id, &error);
		};
		for (const bool prepared : {false, true})
		{
			auto guarded = original_queue;
			if (prepared) guarded.front().prepared_for_delivery = true;
			else
			{
				uam::AcpQueuedUserPromptState unrelated;
				unrelated.id = "unrelated-future-input";
				unrelated.text = "Do not release another queued input";
				guarded.push_back(unrelated);
			}
			fixture.session->queued_user_prompts = guarded;
			chat.acp_queued_prompts.assign(guarded.begin(), guarded.end());
			std::string error;
			UAM_ASSERT(!resend(error));
			UAM_ASSERT_EQ(error, std::string("Resume the held queue to retry this message."));
			UAM_ASSERT(fixture.session->queue_held && chat.acp_queue_held);
			UAM_ASSERT_EQ(fixture.session->queued_user_prompts, guarded);
			UAM_ASSERT_EQ(chat.messages, original_messages);
		}
		fixture.session->queued_user_prompts = original_queue;
		chat.acp_queued_prompts.assign(original_queue.begin(), original_queue.end());
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			std::string error;
			UAM_ASSERT(resend(error));
			UAM_ASSERT(error.empty());
			UAM_ASSERT(!fixture.session->queue_held && !chat.acp_queue_held);
			UAM_ASSERT_EQ(fixture.session->queued_user_prompts, original_queue);
			UAM_ASSERT_EQ(chat.messages, original_messages);
			UAM_ASSERT(!fixture.session->processing && fixture.session->prompt_request_id == 0);
			UAM_ASSERT_EQ(fixture.session->next_request_id, next_request);
			UAM_ASSERT(fixture.session->reconnect_pending && !fixture.session->running);
		}
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


UAM_TEST(AcpRemoteQueuedPrewriteFailureHoldsRawBatchWithoutReplay)
{
	RetryWorkspaceFixture fixture;
	fixture.RemoteReady();
	ChatSession& chat = fixture.app.chats.front();
	uam::AcpQueuedUserPromptState raw;
	raw.id = "private-held-raw-head"; raw.revision = 4; raw.text = "Original authored queued input";
	raw.markdown_store_files = {"head-skill.md"}; raw.markdown_store_prompt_blocks = {"ORIGINAL_HEAD_SKILL"};
	raw.attachments = {{.id = "head-attachment", .kind = "file", .path = "private-head.txt"}};
	uam::AcpQueuedUserPromptState future;
	future.id = "private-held-raw-future"; future.revision = 9; future.text = "Future captured input";
	fixture.session->queued_user_prompts = {raw, future};
	chat.acp_queued_prompts = {raw, future};
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	const std::vector<Message> original = chat.messages;
	const std::vector<uam::AcpQueuedUserPromptState> original_outbox = chat.acp_queued_prompts;
	std::string transport_error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.WriteToStdioProcess(*fixture.session, "\n", 1, &transport_error));
	const auto transport_writer = fixture.session->stdin_writer;
	UAM_ASSERT(transport_writer != nullptr);
	const auto wire = std::make_shared<std::string>();
	const auto observed_writer = std::make_shared<uam::platform::AsyncByteWriter>(
	    [wire, transport_writer](const char* bytes, std::size_t count, std::string& failure) -> std::ptrdiff_t
	    {
		    wire->append(bytes, count);
		    return transport_writer->Enqueue(bytes, count, &failure) ? static_cast<std::ptrdiff_t>(count) : -1;
	    }, [] {});
	fixture.session->stdin_writer = observed_writer;
	int gate_calls = 0;
	fixture.app.opencode_compaction_before_dispatch = [&gate_calls](uam::AcpSessionState&, ChatSession&, std::string& error)
	{
		++gate_calls;
		error = "Private metadata prewrite rejection";
		return uam::OpenCodeCompactionGateState::Failed;
	};
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
	fixture.AwaitSave();
	for (int poll = 0; poll < 4; ++poll)
	{
		UAM_ASSERT(fixture.session->queue_held && chat.acp_queue_held);
		UAM_ASSERT(chat.messages == original);
		UAM_ASSERT(chat.acp_queued_prompts == original_outbox);
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{2});
		UAM_ASSERT(fixture.session->queued_user_prompts[0] == raw && fixture.session->queued_user_prompts[1] == future);
		UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, std::size_t{0});
		UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
		UAM_ASSERT_EQ(fixture.session->next_request_id, 17);
		UAM_ASSERT(!fixture.session->processing && !fixture.session->reconnect_pending && fixture.session->queued_prompt.empty());
		UAM_ASSERT(!fixture.session->pending_prompt_delivery && fixture.session->pending_request_methods.empty());
		UAM_ASSERT(!chat.remote_turn_reconnect_pending && chat.remote_pending_requests.empty());
		UAM_ASSERT(chat.remote_prompt_delivery_session_id.empty() && chat.remote_prompt_delivery_id.empty() && chat.remote_prompt_delivery_payload.empty());
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
		UAM_ASSERT(saved && saved->acp_queue_held && saved->messages == original && saved->acp_queued_prompts == original_outbox);
		UAM_ASSERT_EQ(saved->acp_dispatched_queued_prompt_count, std::size_t{0});
		UAM_ASSERT(!saved->remote_turn_reconnect_pending && saved->remote_pending_requests.empty());
		UAM_ASSERT(saved->remote_prompt_delivery_session_id.empty() && saved->remote_prompt_delivery_id.empty() && saved->remote_prompt_delivery_payload.empty());
		UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
		(void)uam::PollAllAcpSessions(fixture.app);
		fixture.AwaitSave();
		UAM_ASSERT(observed_writer->Flush(std::chrono::seconds(1)));
		UAM_ASSERT(transport_writer->Flush(std::chrono::seconds(1)));
		UAM_ASSERT(wire->empty());
		UAM_ASSERT_EQ(gate_calls, 1);
	}
	UAM_ASSERT(chat.messages == original && chat.acp_queued_prompts == original_outbox);
	UAM_ASSERT(fixture.session->queue_held && chat.acp_queue_held);
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{2});
	UAM_ASSERT(fixture.session->queued_user_prompts[0] == raw && fixture.session->queued_user_prompts[1] == future);
}


UAM_TEST(AcpWorkspaceReconnectFailurePreservesHeldSettlement)
{
	for (const bool canceled : {false, true})
	{
		RetryWorkspaceFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		std::string error;
		UAM_ASSERT(uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
		fixture.AwaitSave();
		UAM_ASSERT(fixture.session->reconnect_pending && !fixture.session->worktree_launch_pending);
		const auto receipt = fixture.session->queued_user_prompts.front();
		const auto messages = chat.messages;
		if (canceled)
		{
			// Cancellation belongs to the preparation already captured by this session.
			// A canceled task not yet observed by a launch may legitimately be replaced.
			(void)uam::PollAllAcpSessions(fixture.app);
			UAM_ASSERT(fixture.session->worktree_launch_pending);
			UAM_ASSERT(fixture.session->worktree_launch_cancellation == fixture.task->cancellation);
			UAM_ASSERT(!fixture.session->running && !fixture.session->processing);
			fixture.task->cancellation->request_stop();
		}
		else fixture.task->error = "Private preparation failure before provider launch";
		(void)uam::PollAllAcpSessions(fixture.app);
		UAM_ASSERT(fixture.session->queue_held && chat.acp_queue_held);
		UAM_ASSERT(!fixture.session->worktree_launch_pending);
		UAM_ASSERT(!fixture.session->last_error.empty());
		UAM_ASSERT(!fixture.session->reconnect_pending);
		UAM_ASSERT(!fixture.session->running && !fixture.session->processing && fixture.session->last_process_id.empty());
		UAM_ASSERT_EQ(chat.messages, messages);
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts, (std::deque<uam::AcpQueuedUserPromptState>{receipt}));
		const int attempts = fixture.session->reconnect_attempts;
		(void)uam::PollAllAcpSessions(fixture.app);
		UAM_ASSERT(!fixture.session->reconnect_pending && fixture.session->reconnect_attempts == attempts);
		UAM_ASSERT_EQ(chat.messages, messages);
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts, (std::deque<uam::AcpQueuedUserPromptState>{receipt}));
	}
}



UAM_TEST(AcpReferencedRetryExecFailureRetainsNativeHistoryAndRecoveryProjection)
{
#if defined(__APPLE__) && UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-rtry");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
	// Codex deliberately uses literal argv[0]="codex", ignoring profile command.
	// First executable PATH match is private, but its absolute interpreter cannot
	// exist: resolution succeeds, actual kernel exec fails, no provider can run.
	const auto private_bin = temp.root / "bin";
	fs::create_directories(private_bin);
	const auto private_codex = private_bin / "codex";
	const auto missing_interpreter = temp.root / "never-created-interpreter";
	const auto forbidden_marker = temp.root / "unexpected-wrapper-ran";
	const std::string script = "#!" + uam::paths::Utf8PathString(missing_interpreter) + "\nexit 99\n";
	UAM_ASSERT(uam::io::WriteTextFile(private_codex, script));
	fs::permissions(private_codex, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
	UAM_ASSERT(fs::is_regular_file(private_codex) && !fs::is_symlink(private_codex));
	UAM_ASSERT((fs::status(private_codex).permissions() & fs::perms::owner_exec) != fs::perms::none);
	UAM_ASSERT_EQ(ReadFile(private_codex), script);
	UAM_ASSERT(!fs::exists(missing_interpreter));
	const std::string private_path = private_bin.string() + ":/usr/bin:/bin";
	ScopedEnvVar path("PATH", private_path);
	UAM_ASSERT_EQ(uam::env::GetNonEmptyString("PATH").value_or(""), private_path);
	app.settings.git_enabled = app.settings.git_auto_worktree = false;
	app.settings.svn_enabled = false;
	app.settings.central_provider_configuration.enabled = true;
	app.settings.central_provider_configuration.uam_control_enabled = false;
	app.settings.provider_chat_defaults["codex-cli"].feature_preference = "uam";
	ChatSession value;
	value.id = "retained-codex-retry-prespawn";
	value.created_at = "2026-10-22T00:00:00Z";
	value.provider_id = "codex-cli";
	value.execution_host_id = "local";
	value.workspace_directory = temp.root.string();
	value.native_session_id = "0194c9a0-1111-7111-8111-111111111111";
	value.memory_enabled = false;
	// A real update-only preparation/save boundary; disabled VCS performs no commands.
	value.svn_update_pending = true;
	value.messages = {{.role = MessageRole::User, .content = std::string(251, 'r'),
	    .created_at = "2026-10-22T00:00:01Z", .interrupted = true, .acp_prompt_not_sent = true}};
	value.messages.front().markdown_store_files = {"captured-private-skill.md"};
	value.messages.front().markdown_store_prompt_blocks = {"CAPTURED_RETAINED_SKILL"};
	app.chats.push_back(std::move(value));
	auto& chat = app.chats.front();
	const Message original = chat.messages.front();
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	auto& session = uam::acp_detail::EnsureAcpSessionForChat(app, chat);
	struct StopPrivateLaunch
	{
		uam::AcpSessionState& session;
		~StopPrivateLaunch()
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
		}
	} stop_private_launch{session};
	const auto flush = [&]
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while ((app.pending_chat_save_at_by_chat_id.contains(chat.id) || app.pending_chat_save_tasks.contains(chat.id)) && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::yield();
		}
		UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(chat.id) && !app.pending_chat_save_tasks.contains(chat.id));
	};
	const auto assert_history = [&](const uam::AcpQueuedUserPromptState& expected)
	{
		UAM_ASSERT(chat.messages_loaded && chat.messages.size() == 1);
		const Message& row = chat.messages.front();
		UAM_ASSERT_EQ(row.content, original.content);
		UAM_ASSERT_EQ(row.created_at, original.created_at);
		UAM_ASSERT(row.attachments == original.attachments && row.markdown_store_files == original.markdown_store_files && row.markdown_store_prompt_blocks == original.markdown_store_prompt_blocks);
		UAM_ASSERT(row.acp_prompt_not_sent && !row.acp_prompt_discarded && !row.input_write_completed);
		const auto page = uam::StateSerializer::SerializeMessagePage(chat, 60, std::nullopt);
		UAM_ASSERT_EQ(page.at("totalCount").get<std::size_t>(), std::size_t{1});
		UAM_ASSERT_EQ(page.at("messages").size(), std::size_t{1});
		UAM_ASSERT_EQ(page.at("messages").at(0).at("content").get<std::string>(), original.content);
		UAM_ASSERT(page.at("messages").at(0).value("acpPromptNotSent", false));
		UAM_ASSERT(!page.at("messages").at(0).value("inputWriteCompleted", false));
		const auto pushed = uam::StateSerializer::Serialize(app);
		const auto& projected = pushed.at("chats").at(0).at("acpSession");
		UAM_ASSERT(projected.at("unsentMessageIndices") == nlohmann::json::array({0}));
		UAM_ASSERT(projected.at("recoverableUnsentMessageIndices") == nlohmann::json::array({0}));
		// Native authority is already established above; this carrier keeps cold
		// frontend history reachable, not a text/time-derived delivery authority.
		UAM_ASSERT_EQ(projected.at("unsentDirectPrompts").size(), std::size_t{1});
		const auto& retained = projected.at("unsentDirectPrompts").at(0);
		UAM_ASSERT_EQ(retained.at("id").get<std::string>(), expected.id);
		UAM_ASSERT_EQ(retained.at("revision").get<int>(), expected.revision);
		UAM_ASSERT_EQ(retained.at("text").get<std::string>(), original.content);
		UAM_ASSERT(retained.at("markdownStoreFiles") == nlohmann::json(original.markdown_store_files));
		UAM_ASSERT_EQ(retained.at("existingUserMessageIndex").get<int>(), 0);
		UAM_ASSERT_EQ(retained.at("referencedMessageCreatedAt").get<std::string>(), original.created_at);
		UAM_ASSERT(retained.at("retryAllowed").get<bool>());
		UAM_ASSERT(projected.at("pendingDirectPrompts").empty());
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
		UAM_ASSERT(saved && saved->messages.size() == 1);
		UAM_ASSERT_EQ(saved->messages.front().content, original.content);
		UAM_ASSERT(saved->messages.front().acp_prompt_not_sent && !saved->messages.front().acp_prompt_discarded && !saved->messages.front().input_write_completed);
	};
	// Occupy the version task with a private completed-result fixture only after
	// launch capture, as in the public deferred-configuration health regression.
	ExecutionHost foreign{"retention-foreign-health", "Private", "ssh", "private-fixture", "ready", "development", "linux", "x86_64", "", "runner", 3};
	app.settings.execution_hosts.push_back(foreign);
	auto& health = app.runtime_cli_version_check_task;
	health.running = true;
	health.execution_host = foreign;
	health.state = std::make_shared<AsyncProcessTaskState>();
	app.runtime_cli_version_provider_id = "codex-cli";
	std::string error;
	UAM_ASSERT(uam::RetryLastAcpPrompt(app, chat.id, &error));
	UAM_ASSERT(error.empty());
	flush();
	UAM_ASSERT_EQ(chat.acp_queued_prompts.size(), std::size_t{1});
	const auto receipt = chat.acp_queued_prompts.front();
	UAM_ASSERT(receipt.direct_admission && receipt.include_prior_context && !receipt.append_user_message && !receipt.prepared_for_delivery);
	UAM_ASSERT(!receipt.id.empty() && receipt.revision > 0 && receipt.existing_user_message_index == 0);
	UAM_ASSERT_EQ(receipt.existing_user_message_created_at, original.created_at);
	for (int attempt = 0; attempt != 2; ++attempt)
	{
		if (attempt != 0)
		{
			error.clear();
			UAM_ASSERT(uam::RetryQueuedAcpPrompt(app, chat.id, {receipt.id, receipt.revision}, &error));
			UAM_ASSERT(error.empty());
			flush();
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		bool saw_configuration = false;
		while (std::chrono::steady_clock::now() < deadline)
		{
			UAM_ASSERT(!session.running);
			(void)uam::PollAllAcpSessions(app);
			// Stop immediately on unexpected acceptance, before any further poll.
			UAM_ASSERT(!session.running && !session.stdin_writer);
			flush();
			if (session.pending_local_launch)
			{
				saw_configuration = true;
				if (attempt == 0 && health.state)
				{
					health.state->result.ok = true;
					health.state->result.exit_code = 0;
					health.state->result.output = "0.0.1\n";
					health.state->remote_helper_connected = true;
					health.state->completed.store(true, std::memory_order_release);
				}
			}
			if (saw_configuration && !session.pending_local_launch && session.queue_held && !session.reconnect_pending) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT(saw_configuration && !chat.svn_update_pending && !session.worktree_launch_pending);
		UAM_ASSERT_EQ(session.last_error, "posix_spawn failed: " + std::string(std::strerror(ENOENT)) + ".");
		UAM_ASSERT(session.last_error.find("ownership changed") == std::string::npos);
		UAM_ASSERT(session.last_error.find("not found on PATH") == std::string::npos);
		UAM_ASSERT_EQ(ReadFile(private_codex), script);
		UAM_ASSERT(!fs::exists(missing_interpreter) && !fs::exists(forbidden_marker));
		UAM_ASSERT_EQ(uam::env::GetNonEmptyString("PATH").value_or(""), private_path);
		UAM_ASSERT(!session.pending_local_launch && session.queue_held && chat.acp_queue_held && !session.reconnect_pending);
		UAM_ASSERT(!session.running && !session.processing && !session.stdin_writer && session.last_process_id.empty());
		UAM_ASSERT(session.child_pid == -1 && session.watchdog_pid == -1 && session.prompt_request_id == 0);
		UAM_ASSERT(session.stdin_write_fd == -1 && session.stdout_read_fd == -1 && session.stderr_read_fd == -1);
		UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, std::size_t{0});
		UAM_ASSERT_EQ(chat.acp_queued_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(chat.acp_queued_prompts.front().id, receipt.id);
		UAM_ASSERT_EQ(chat.acp_queued_prompts.front().revision, receipt.revision);
		assert_history(receipt);
	}
	UAM_ASSERT(app.remote_host_health_changed && !health.running && !health.state && !health.worker);
	// Actual history-request settlement is allowed to retire redundant receipts,
	// but must retain the authored row and native-known-unsent recovery projection.
	(void)uam::SettleStoppedDirectAcpAdmissions(app, chat);
	flush();
	assert_history(receipt);
	UAM_ASSERT(!session.running && !session.processing && !fs::exists(missing_interpreter));
#endif
}
