#include "test_harness.h"

using namespace uam_test;

namespace
{
	struct AgentProviderFixture
	{
		TempDir temp{"uam-agent-provider-inheritance"};
		uam::AppState app;

		AgentProviderFixture()
		{
			const fs::path workspace = temp.root / "workspace";
			const fs::path agents = workspace / ".uam" / "agents";
			fs::create_directories(agents);
			UAM_ASSERT(uam::io::WriteTextFile(agents / "worker.md",
			    "---\nversion: 1\nname: worker\ndescription: Worker\nmode: subagent\nworkspaceAccess: write\nskills: []\ndelegates: []\n---\nDo work.\n"));
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile(), ProviderProfileStore::DefaultClaudeProfile()};
			ChatSession root;
			root.id = "root";
			root.provider_id = "codex-cli";
			root.workspace_directory = workspace.string();
			root.model_id = "parent-codex-model";
			root.reasoning_effort = "high";
			root.service_tier = "fast";
			root.service_tier_explicit = true;
			root.command_safety_tier = "off";
			root.approval_mode = "default";
			app.chats.push_back(root);
		}

		const ChatSession& Enqueue(const std::string& provider, const std::string& model = {})
		{
			std::string run_id;
			std::string error;
			UAM_ASSERT(uam::AgentRunScheduler::Enqueue(app, "root", {}, "worker", "Bounded task.", &run_id, &error, provider, model));
			UAM_ASSERT_EQ(app.agent_runs.back().id, run_id);
			UAM_ASSERT_EQ(app.agent_runs.back().model_id, app.chats.back().model_id);
			return app.chats.back();
		}
	};
}

UAM_TEST(AgentChildSameProviderAliasesInheritModelOptionsAndExplicitOverride)
{
	for (const std::string& provider : {"", "codex", "codex-cli"})
	{
		AgentProviderFixture fixture;
		fixture.app.chats.front().provider_id = "codex";
		const ChatSession& child = fixture.Enqueue(provider);
		UAM_ASSERT_EQ(child.provider_id, std::string("codex-cli"));
		UAM_ASSERT_EQ(child.model_id, std::string("parent-codex-model"));
		UAM_ASSERT_EQ(child.reasoning_effort, std::string("high"));
		UAM_ASSERT_EQ(child.service_tier, std::string("fast"));
		UAM_ASSERT(child.service_tier_explicit);
	}
	AgentProviderFixture fixture;
	UAM_ASSERT_EQ(fixture.Enqueue("codex", "explicit-child-model").model_id, std::string("explicit-child-model"));
}

UAM_TEST(AgentCrossProviderDefaultsDoNotInheritParentModelOptionsOrWeakenSafety)
{
	for (const std::string& model : {"", "explicit-claude-model"})
	{
		AgentProviderFixture fixture;
		fixture.app.chats.front().command_safety_tier = "yolo";
		fixture.app.chats.front().approval_mode = "plan";
		const ChatSession& child = fixture.Enqueue("claude-cli", model);
		UAM_ASSERT_EQ(child.provider_id, std::string("claude-cli"));
		UAM_ASSERT_EQ(child.model_id, model);
		UAM_ASSERT(child.reasoning_effort.empty());
		UAM_ASSERT(child.service_tier.empty());
		UAM_ASSERT(!child.service_tier_explicit);
		UAM_ASSERT_EQ(child.command_safety_tier, std::string("yolo"));
		UAM_ASSERT_EQ(child.approval_mode, std::string("plan"));
		UAM_ASSERT_EQ(fixture.app.agent_runs.back().effective_workspace_access, std::string("read"));
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, child.id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->model_id, model);
		UAM_ASSERT(saved->reasoning_effort.empty() && saved->service_tier.empty() && !saved->service_tier_explicit);
	}
}

UAM_TEST(AgentUnavailableProviderRejectsWithoutCreatingRunOrTranscript)
{
	AgentProviderFixture fixture;
	std::string run_id;
	std::string error;
	UAM_ASSERT(!uam::AgentRunScheduler::Enqueue(fixture.app, "root", {}, "worker", "Task.", &run_id, &error, "missing-provider"));
	UAM_ASSERT_EQ(error, std::string("Requested provider is unavailable."));
	UAM_ASSERT(fixture.app.agent_runs.empty() && fixture.app.queued_agent_run_ids.empty());
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	UAM_ASSERT(run_id.empty());
}
