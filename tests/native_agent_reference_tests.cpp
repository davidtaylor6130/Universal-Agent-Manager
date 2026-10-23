#include "test_harness.h"
#include "app/runtime_orchestration_services.h"
#include "common/provider/native_agent_history.h"

using namespace uam_test;

UAM_TEST(NativeReadOnlyAgentReferencesSupportFullLengthRemoteHosts)
{
	TempDir root("uam-native-agent-long-host");
	for (const std::string provider : {uam::provider_ids::kCopilotCli, uam::provider_ids::kClaudeCli, uam::provider_ids::kGeminiCli})
	{
		const std::string parent = "550e8400-e29b-41d4-a716-446655440000";
		const std::string agent = "660e8400-e29b-41d4-a716-446655440001";
		const std::string host(64, 'h');
		UAM_ASSERT_EQ(uam::native_agents::BuildHistoryId(provider, "p", "a", "local"), std::string(uam::native_agents::ReferencePrefix(provider)) + "70-61-6c6f63616c");
		UAM_ASSERT(!uam::native_agents::ParseHistoryId(std::string(uam::native_agents::ReferencePrefix(provider, true)) + "oa-me-nrxwgylm")); // Compact aliases for an existing short hex identity are noncanonical.
		const std::string reference = uam::native_agents::BuildHistoryId(provider, parent, agent, host);
		UAM_ASSERT(!reference.empty() && reference.size() <= 240);
		UAM_ASSERT(std::ranges::all_of(reference, [](unsigned char ch) { return uam::strings::IsAsciiAlnum(ch) || ch == '-'; }));
		const auto parsed = uam::native_agents::ParseHistoryId(reference);
		UAM_ASSERT(parsed && parsed->provider_id == provider && parsed->parent_session_id == parent && parsed->agent_id == agent && parsed->execution_host_id == host);
		UAM_ASSERT_EQ(uam::native_agents::BuildHistoryId(provider, parsed->parent_session_id, parsed->agent_id, parsed->execution_host_id), reference);
		UAM_ASSERT(!uam::native_agents::ParseHistoryId(reference + "-00"));
		UAM_ASSERT(!uam::native_agents::ParseHistoryId(reference.substr(0, reference.size() - 1)));
		std::string invalid_padding = reference;
		invalid_padding.back() = 'b'; // A 64-byte component has three zero padding bits in its final base32 digit.
		UAM_ASSERT(!uam::native_agents::ParseHistoryId(invalid_padding));
		std::string invalid_case = reference;
		invalid_case[reference.find('-', reference.find('-') + 1) + 1] = 'Z';
		UAM_ASSERT(!uam::native_agents::ParseHistoryId(invalid_case));

		uam::AppState app;
		app.data_root = root.root / provider;
		ExecutionHost remote;
		remote.id = host;
		remote.label = "Long host fixture";
		remote.transport = "ssh";
		remote.ssh_alias = "isolated-test";
		remote.platform = "linux";
		app.settings.execution_hosts.push_back(remote);
		uam::execution_hosts::Normalize(app.settings.execution_hosts);
		UAM_ASSERT(uam::execution_hosts::Find(app.settings.execution_hosts, host) != nullptr);
		ChatSession source;
		source.id = "parent-chat";
		source.provider_id = provider;
		source.native_session_id = parent;
		source.execution_host_id = host;
		source.workspace_directory = "/workspace/native-agent-fixture";
		app.chats.push_back(source);
		ChatSession child;
		child.id = reference;
		child.provider_id = provider;
		child.parent_chat_id = parent;
		child.execution_host_id = host;
		child.workspace_directory = source.workspace_directory;
		child.imported_read_only = true;
		child.messages.push_back({MessageRole::Assistant, "Actual remote child message"});
		const ProviderProfile profile = provider == uam::provider_ids::kCopilotCli ? ProviderProfileStore::DefaultCopilotProfile() :
		    provider == uam::provider_ids::kClaudeCli ? ProviderProfileStore::DefaultClaudeProfile() : ProviderProfileStore::DefaultGeminiProfile();
		UAM_ASSERT(ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, &child, true));
		for (int mismatch = 0; mismatch < 3; ++mismatch)
		{
			ChatSession forged = child;
			if (mismatch == 0) forged.provider_id = provider == uam::provider_ids::kClaudeCli ? uam::provider_ids::kGeminiCli : uam::provider_ids::kClaudeCli;
			if (mismatch == 1) forged.parent_chat_id = "foreign-parent";
			if (mismatch == 2) forged.execution_host_id = std::string(64, 'x');
			UAM_ASSERT(!ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, &forged, true));
		}
		const std::string other_host = uam::native_agents::BuildHistoryId(provider, parent, agent, std::string(64, 'x'));
		UAM_ASSERT(!ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, source, profile, other_host, false, &child, true));
	}
}

UAM_TEST(NativeReadOnlyAgentReferencesCannotCrossProviderParentOrHost)
{
	TempDir root("uam-native-agent-reference");
	for (const std::string provider : {uam::provider_ids::kClaudeCli, uam::provider_ids::kGeminiCli})
	{
		const std::string parent = "550e8400-e29b-41d4-a716-446655440000";
		const std::string reference = uam::native_agents::BuildHistoryId(provider, parent, "actual-agent", "local");
		const auto parsed = uam::native_agents::ParseHistoryId(reference);
		UAM_ASSERT(parsed && parsed->provider_id == provider && parsed->parent_session_id == parent && parsed->agent_id == "actual-agent" && parsed->execution_host_id == "local");
		UAM_ASSERT(!uam::native_agents::ParseHistoryId(reference + "-00"));
		UAM_ASSERT(uam::native_agents::BuildHistoryId(provider, "../parent", "actual-agent", "local").empty());
		UAM_ASSERT(uam::native_agents::BuildHistoryId(provider, parent, std::string(128, 'a'), "local").empty());
		uam::AppState app;
		app.data_root = root.root / provider;
		ChatSession source;
		source.id = "parent-chat";
		source.provider_id = provider;
		source.native_session_id = parent;
		source.execution_host_id = "local";
		source.workspace_directory = root.root.string();
		app.chats.push_back(source);
		ChatSession child;
		child.id = reference;
		child.provider_id = provider;
		child.parent_chat_id = parent;
		child.execution_host_id = "local";
		child.workspace_directory = source.workspace_directory;
		child.imported_read_only = true;
		Message message;
		message.role = MessageRole::Assistant;
		message.content = "Actual child journal content";
		child.messages.push_back(message);
		const ProviderProfile profile = provider == uam::provider_ids::kClaudeCli ? ProviderProfileStore::DefaultClaudeProfile() : ProviderProfileStore::DefaultGeminiProfile();
		ChatSession* imported = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, &child, true);
		UAM_ASSERT(imported && imported->imported_read_only && imported->native_session_id.empty());
		UAM_ASSERT(!app.resolved_native_sessions_by_chat_id.contains(reference));
		child.messages.front().content = "Actual refreshed journal content";
		UAM_ASSERT(ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, &child, true));
		UAM_ASSERT_EQ(app.chats.back().messages.front().content, child.messages.front().content);
		for (int mismatch = 0; mismatch < 3; ++mismatch)
		{
			ChatSession forged = child;
			if (mismatch == 0) forged.provider_id = provider == uam::provider_ids::kClaudeCli ? uam::provider_ids::kGeminiCli : uam::provider_ids::kClaudeCli;
			if (mismatch == 1) forged.parent_chat_id = "foreign-parent";
			if (mismatch == 2) forged.execution_host_id = "foreign-host";
			UAM_ASSERT(!ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, &forged, true));
		}
		app.chats.back().imported_read_only = false;
		UAM_ASSERT(!ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, &child, true));
	}
}
