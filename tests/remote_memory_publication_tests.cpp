#include "test_harness.h"
#include "app/memory_service.h"
#include <condition_variable>
using namespace uam_test;
UAM_TEST(RemoteMemoryWorkerCancelsChangedOwnersBeforeCompletion)
{
	for (int variant = 0; variant < 16; ++variant)
	{
		TempDir temp("uam-remote-memory-owner");
		uam::AppState app;
		app.data_root = temp.root;
		ExecutionHost host;
		host.id = "owned-remote"; host.platform = "linux"; host.ssh_alias = "owned-no-network";
		app.settings.execution_hosts.push_back(host);
		ChatSession chat;
		chat.id = "owned-memory"; chat.provider_id = "claude-cli"; chat.execution_host_id = host.id;
		chat.workspace_directory = "/owned-workspace"; chat.memory_level = "strict";
		chat.messages.push_back({MessageRole::User, "Fixture preference."});
		app.chats.push_back(chat);
		uam::AsyncMemoryExtractionTask task;
		task.owner_provider_id = chat.provider_id; task.memory_level = "strict";
		task.running = true; task.data_root = app.data_root; task.chat_id = chat.id;
		task.remote_host = host; task.remote_workspace = chat.workspace_directory;
		task.state = std::make_shared<AsyncProcessTaskState>();
		task.extraction_state = std::make_shared<uam::AsyncMemoryExtractionState>();
		task.worker = std::make_unique<std::jthread>([](std::stop_token stop)
		{
			std::mutex mutex; std::condition_variable_any condition; std::unique_lock lock(mutex);
			(void)condition.wait(lock, stop, []() { return false; });
		});
		const std::stop_token token = task.worker->get_stop_token();
		app.memory_extraction_tasks.push_back(std::move(task));
		(void)MemoryService::ProcessDueMemoryWork(app);
		UAM_ASSERT(!token.stop_requested());
		if (variant >= 8) app.memory_extraction_tasks.front().extraction_state->remote_publication_ready.store(true);
		const int change = variant % 8;
		if (change == 0) app.data_root /= "changed";
		else if (change == 1) app.chats.front().workspace_directory = "/changed";
		else if (change == 2) app.settings.execution_hosts.back().ssh_alias = "changed-no-network";
		else if (change == 3) app.chats.clear();
		else if (change == 4) app.chats.front().memory_level = "off";
		else if (change == 5) app.chats.front().provider_id = "codex-cli";
		else if (change == 6) app.settings.execution_hosts.clear();
		else app.chats.front().imported_read_only = true;
		(void)MemoryService::ProcessDueMemoryWork(app);
		UAM_ASSERT(token.stop_requested());
	}
}

UAM_TEST(RemoteMemoryPublicationRequiresCurrentUncancelledOwner)
{
	for (bool canceled : {false, true})
	{
		TempDir temp("uam-memory-publication");
		uam::AppState app;
		app.data_root = temp.root;
		ExecutionHost host;
		host.id = "owned-remote";
		host.platform = "linux";
		host.ssh_alias = "owned-no-network";
		app.settings.execution_hosts.push_back(host);
		ChatSession chat;
		chat.id = "owned-memory";
		chat.provider_id = "claude-cli";
		chat.execution_host_id = host.id;
		chat.workspace_directory = "/owned-workspace";
		chat.memory_level = "strict";
		chat.messages.push_back({MessageRole::User, "Fixture preference."});
		app.chats.push_back(chat);
		uam::AsyncMemoryExtractionTask task;
		task.running = true;
		task.data_root = app.data_root;
		task.chat_id = chat.id;
		task.owner_provider_id = chat.provider_id;
		task.memory_level = "strict";
		task.remote_host = host;
		task.remote_workspace = chat.workspace_directory;
		task.state = std::make_shared<AsyncProcessTaskState>();
		task.extraction_state = std::make_shared<uam::AsyncMemoryExtractionState>();
		const std::shared_ptr<uam::AsyncMemoryExtractionState> state = task.extraction_state;
		std::atomic<bool> published{false};
		task.worker = std::make_unique<std::jthread>([state, &published](std::stop_token stop)
		{
			std::unique_lock lock(state->remote_publication_mutex);
			if (state->remote_publication_condition.wait(lock, stop, [state]() { return state->remote_publication_permitted; }) && !stop.stop_requested())
				published.store(true);
		});
		app.memory_extraction_tasks.push_back(std::move(task));
		(void)MemoryService::ProcessDueMemoryWork(app);
		UAM_ASSERT(!state->remote_publication_permitted);
		if (canceled) app.memory_extraction_tasks.front().worker->request_stop();
		state->remote_publication_ready.store(true, std::memory_order_release);
		(void)MemoryService::ProcessDueMemoryWork(app);
		app.memory_extraction_tasks.front().worker->join();
		UAM_ASSERT_EQ(state->remote_publication_permitted, !canceled);
		UAM_ASSERT_EQ(published.load(), !canceled);
	}
}

UAM_TEST(RemoteMemoryCompletionCannotAdvanceReplacedPolicy)
{
	for (bool provider_changed : {false, true})
	{
		TempDir temp("uam-remote-memory-completion");
		uam::AppState app;
		app.data_root = temp.root;
		ExecutionHost host;
		host.id = "owned-remote";
		host.platform = "linux";
		host.ssh_alias = "owned-no-network";
		app.settings.execution_hosts.push_back(host);
		ChatSession chat;
		chat.id = "owned-memory";
		chat.provider_id = "claude-cli";
		chat.execution_host_id = host.id;
		chat.workspace_directory = "/owned-workspace";
		chat.memory_level = "strict";
		chat.messages.push_back({MessageRole::User, "Fixture preference."});
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		app.chats.push_back(chat);
		uam::AsyncMemoryExtractionTask task;
		task.data_root = app.data_root;
		task.chat_id = chat.id;
		task.owner_provider_id = chat.provider_id;
		task.memory_level = "strict";
		task.remote_host = host;
		task.remote_workspace = chat.workspace_directory;
		task.message_count = 1;
		app.memory_retry_not_before_by_chat_id[chat.id] = uam::GetAppTimeSeconds() + 60.0;
		task.state = std::make_shared<AsyncProcessTaskState>();
		task.state->result.ok = true;
		task.state->completed.store(true);
		task.extraction_state = std::make_shared<uam::AsyncMemoryExtractionState>();
		task.extraction_state->remote_applied = true;
		app.memory_extraction_tasks.push_back(std::move(task));
		if (provider_changed) app.chats.front().provider_id = "codex-cli";
		else app.chats.front().memory_level = "balanced";
		(void)MemoryService::ProcessDueMemoryWork(app);
		UAM_ASSERT_EQ(app.chats.front().memory_last_processed_message_count, 0);
		UAM_ASSERT(app.memory_extraction_tasks.empty());
	}
}
