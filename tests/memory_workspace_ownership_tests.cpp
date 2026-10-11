#include "test_harness.h"
#include "common/paths/workspace_root.h"

using namespace uam_test;

UAM_TEST(MemoryWorkerCompletionRetainsWorkspaceOwnershipAcrossMovesAndWorktreeChanges)
{
	for (const bool has_entry : {false, true})
	for (const std::string change : {"move", "directory", "worktree", "unchanged"})
	{
		TempDir temp("uam-memory-workspace-owner");
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.settings.memory_idle_delay_seconds = -1;
		app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
		app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
		const fs::path old_workspace = temp.root / "old-workspace";
		const fs::path new_workspace = temp.root / "new-workspace";
		fs::create_directories(old_workspace);
		fs::create_directories(new_workspace);
		app.folders = {{"old", "Old", old_workspace.string(), false}, {"new", "New", new_workspace.string(), false}};
		ChatSession chat;
		chat.id = "workspace-memory";
		chat.provider_id = "codex-cli";
		chat.folder_id = "old";
		chat.workspace_directory = old_workspace.string();
		chat.memory_enabled = true;
		chat.memory_level = "open";
		chat.messages.push_back({MessageRole::User, "Remember our project coding preference.", "now"});
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		app.chats.push_back(chat);
		uam::AsyncMemoryExtractionTask task;
		task.data_root = app.data_root;
		task.running = true;
		task.chat_id = chat.id;
		task.message_count = 1;
		task.workspace_root = uam::paths::ResolveControllerWorkspaceRootPath(app, chat);
		task.state = std::make_shared<AsyncProcessTaskState>();
		task.state->result.ok = true;
		task.state->result.output = has_entry
		    ? R"({"memories":[{"scope":"local","category":"Lessons/User_Lessons","title":"Brace preference","memory":"Prefer Allman brace style in this project.","evidence":"User said: Please remember that this project uses Allman braces.","confidence":"high"}]})"
		    : R"({"memories":[]})";
		app.memory_extraction_tasks.push_back(std::move(task));
		if (change == "move")
		{
			UAM_ASSERT(ChatHistorySyncService().MoveChatToFolder(app, app.chats.front(), "new"));
			UAM_ASSERT_EQ(app.chats.front().workspace_directory, new_workspace.string());
		}
		else if (change == "directory") app.chats.front().workspace_directory = new_workspace.string();
		else if (change == "worktree")
		{
			app.chats.front().workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
			app.chats.front().workspace_worktree_directory = new_workspace.string();
		}
		app.memory_global_retry_not_before = uam::GetAppTimeSeconds() + 3600.0;
		app.memory_extraction_tasks.front().state->completed.store(true);
		UAM_ASSERT(MemoryService::ProcessDueMemoryWork(app));
		UAM_ASSERT(app.memory_extraction_tasks.empty());
		UAM_ASSERT_EQ(app.memory_extraction_queue.size(), change == "unchanged" ? std::size_t{0} : std::size_t{1});
		if (change != "unchanged") UAM_ASSERT_EQ(app.memory_extraction_queue.front().chat_id, chat.id);
		UAM_ASSERT_EQ(app.chats.front().memory_last_processed_message_count, change == "unchanged" ? 1 : 0);
		UAM_ASSERT_EQ(fs::exists(MemoryService::LocalMemoryRoot(old_workspace)), change == "unchanged" && has_entry);
		UAM_ASSERT(!fs::exists(MemoryService::LocalMemoryRoot(new_workspace)));
	}
}

UAM_TEST(ColdMemoryPreparationCannotMarkANewWorkspaceProcessedFromAnOldLowSignalSnapshot)
{
	TempDir temp("uam-memory-cold-workspace-owner");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.memory_idle_delay_seconds = -1;
	const fs::path old_workspace = temp.root / "old";
	const fs::path new_workspace = temp.root / "new";
	fs::create_directories(old_workspace);
	fs::create_directories(new_workspace);
	app.folders = {{"old", "Old", old_workspace.string(), false}, {"new", "New", new_workspace.string(), false}};
	ChatSession chat;
	chat.id = "cold-workspace-memory";
	chat.provider_id = "codex-cli";
	chat.folder_id = "old";
	chat.workspace_directory = old_workspace.string();
	chat.memory_enabled = true;
	chat.messages.push_back({MessageRole::User, "Hello", "now"});
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	app.chats = ChatRepository::LoadLocalChatSummaries(app.data_root);
	UAM_ASSERT(!app.chats.front().messages_loaded);
	uam::AsyncMemoryExtractionTask task;
	task.data_root = app.data_root;
	task.running = true;
	task.chat_id = chat.id;
	task.prepared_from_cold = true;
	task.source_persisted_message_count = app.chats.front().persisted_message_count;
	task.source_persisted_messages_digest = app.chats.front().persisted_messages_digest;
	task.workspace_root = uam::paths::ResolveControllerWorkspaceRootPath(app, app.chats.front());
	task.state = std::make_shared<AsyncProcessTaskState>();
	task.extraction_state = std::make_shared<uam::AsyncMemoryExtractionState>();
	task.extraction_state->hydrated_message_count = 1;
	task.extraction_state->hydrated_messages_digest = task.source_persisted_messages_digest;
	task.extraction_state->low_signal_skip = true;
	app.memory_extraction_tasks.push_back(std::move(task));
	UAM_ASSERT(ChatHistorySyncService().MoveChatToFolder(app, app.chats.front(), "new"));
	app.memory_extraction_tasks.front().state->completed.store(true);
	UAM_ASSERT(MemoryService::ProcessDueMemoryWork(app));
	UAM_ASSERT_EQ(app.chats.front().memory_last_processed_message_count, 0);
	UAM_ASSERT(!fs::exists(MemoryService::LocalMemoryRoot(old_workspace)));
	UAM_ASSERT(!fs::exists(MemoryService::LocalMemoryRoot(new_workspace)));
	MemoryService::StopMemoryTasks(app);
	for (const bool connection_changed : {false, true})
	{
		TempDir remote_temp("uam-memory-cold-host-owner");
		uam::AppState remote_app;
		remote_app.data_root = remote_temp.root / "data";
		remote_app.settings.memory_idle_delay_seconds = -1;
		ExecutionHost host;
		host.id = "remote";
		host.transport = "ssh";
		host.platform = "linux";
		host.ssh_alias = "fixture-host";
		remote_app.settings.execution_hosts.push_back(host);
		ChatSession remote_chat;
		remote_chat.id = "cold-remote-memory";
		remote_chat.provider_id = "codex-cli";
		remote_chat.execution_host_id = host.id;
		remote_chat.workspace_directory = "/srv/fixture";
		remote_chat.memory_enabled = true;
		remote_chat.messages.push_back({MessageRole::User, "Hello", "now"});
		UAM_ASSERT(ChatRepository::SaveChat(remote_app.data_root, remote_chat));
		remote_app.chats = ChatRepository::LoadLocalChatSummaries(remote_app.data_root);
		uam::AsyncMemoryExtractionTask remote_task;
		remote_task.data_root = remote_app.data_root;
		remote_task.running = true;
		remote_task.chat_id = remote_chat.id;
		remote_task.owner_provider_id = remote_chat.provider_id;
		remote_task.memory_level = "strict";
		remote_task.prepared_from_cold = true;
		remote_task.source_persisted_message_count = remote_app.chats.front().persisted_message_count;
		remote_task.source_persisted_messages_digest = remote_app.chats.front().persisted_messages_digest;
		remote_task.remote_host = host;
		remote_task.remote_workspace = remote_chat.workspace_directory;
		remote_task.state = std::make_shared<AsyncProcessTaskState>();
		remote_task.extraction_state = std::make_shared<uam::AsyncMemoryExtractionState>();
		remote_task.extraction_state->hydrated_message_count = 1;
		remote_task.extraction_state->hydrated_messages_digest = remote_task.source_persisted_messages_digest;
		remote_task.extraction_state->low_signal_skip = true;
		remote_task.state->completed.store(true);
		remote_app.memory_extraction_tasks.push_back(std::move(remote_task));
		if (connection_changed) remote_app.settings.execution_hosts.front().runner_directory = "changed-runner";
		UAM_ASSERT(MemoryService::ProcessDueMemoryWork(remote_app));
		UAM_ASSERT_EQ(remote_app.chats.front().memory_last_processed_message_count, connection_changed ? 0 : 1);
		MemoryService::StopMemoryTasks(remote_app);
	}

}
