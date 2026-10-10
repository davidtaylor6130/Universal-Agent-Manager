#pragma once

#include "common/state/app_state.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uam
{
	struct ManagedAgentTranscriptReadIdentity
	{
		std::string root_chat_id;
		std::string root_provider_id;
		std::string root_execution_host_id;
		std::string root_native_session_id;
		std::string root_workspace_directory;
		std::string root_folder_id;
		std::string root_native_identity_key;
		std::optional<ExecutionHost> root_host;
		std::string run_id;
		std::string run_agent_id;
		std::string run_provider_id;
		std::string run_execution_capability;
		std::string run_resumed_from_run_id;
		std::string run_status;
		std::string run_updated_at;
		bool run_delivers_result = false;
		bool run_result_delivered = false;
		std::string transcript_chat_id;
		std::string transcript_provider_id;
		std::string transcript_execution_host_id;
		std::string transcript_native_session_id;
		std::string transcript_workspace_directory;
		std::string transcript_folder_id;
		std::string transcript_updated_at;
		std::string transcript_native_identity_key;
		std::size_t transcript_message_count = 0;
		std::string transcript_messages_digest;
		std::optional<ExecutionHost> transcript_host;
	};

	enum class ManagedAgentTranscriptReadStatus
	{
		Ready,
		Unavailable,
		Changed
	};

	struct ManagedAgentTranscriptReadResult
	{
		ManagedAgentTranscriptReadStatus status = ManagedAgentTranscriptReadStatus::Unavailable;
		ChatSession transcript;
		std::string error;
	};

	/// <summary>Capture authorization and cold transcript identity without copying message bodies.</summary>
	std::optional<ManagedAgentTranscriptReadIdentity> CaptureManagedAgentTranscriptRead(
	    const AppState& app, std::string_view root_chat_id, std::string_view transcript_chat_id);
	/// <summary>Read a cold managed transcript on a worker without adopting it into live state.</summary>
	ManagedAgentTranscriptReadResult LoadManagedAgentTranscriptDetached(
	    const std::filesystem::path& data_root, const ManagedAgentTranscriptReadIdentity& expected);
	/// <summary>Revalidate authorization and identity before returning a detached read result.</summary>
	bool MatchesManagedAgentTranscriptRead(const AppState& app,
	                                      const ManagedAgentTranscriptReadIdentity& expected,
	                                      const ChatSession& loaded_transcript);

	struct ToolCallContentIdentity
	{
		std::string chat_id;
		std::string provider_id;
		std::string execution_host_id;
		std::string native_session_id;
		std::string workspace_directory;
		std::string workspace_isolation_kind;
		std::string workspace_source_directory;
		std::string workspace_base_ref;
		std::string workspace_branch_name;
		std::string workspace_worktree_directory;
		std::string updated_at;
		std::string messages_digest;
		std::size_t message_count = 0;
		std::optional<std::size_t> message_index;
	};

	struct ToolCallContentSnapshot
	{
		ToolCallContentIdentity identity;
		std::size_t message_index = 0;
		ToolCall tool_call;
	};

	ToolCallContentIdentity CaptureToolCallContentIdentity(
	    const ChatSession& chat, std::optional<std::size_t> message_index);
	/// <summary>Resolve persisted tool content from a detached transcript without hydrating the live chat.</summary>
	std::optional<ToolCallContentSnapshot> LoadToolCallContentSnapshot(
	    const std::filesystem::path& data_root, const ToolCallContentIdentity& expected, std::string_view tool_call_id);
	/// <summary>Reject a detached result if the live transcript or native session changed during lookup.</summary>
	bool MatchesToolCallContentSnapshot(const ChatSession& current, const ToolCallContentSnapshot& snapshot);

	/// <summary>Release only durable, inactive transcripts which the desktop no longer displays.</summary>
	class TranscriptRetentionService
	{
	public:
		/// <summary>Validate a background history load; page requests must leave cold transcripts detached.</summary>
		static bool CompleteHistoryLoad(ChatSession& current, ChatSession&& loaded, std::size_t expected_count, std::string_view expected_digest, bool retain_history);
		void SetVisibleChatIds(const std::vector<std::string>& chat_ids);
		void Invalidate(const std::string& chat_id);
		std::optional<std::uint64_t> BeginRelease(const AppState& app, const std::string& chat_id);
		bool CompleteRelease(AppState& app, const std::string& chat_id, std::uint64_t generation, const ChatSession& persisted);

	private:
		bool CanRelease(const AppState& app, const ChatSession& chat) const;
		bool m_visibilityKnown = false;
		std::unordered_set<std::string> m_visibleChatIds;
		std::unordered_map<std::string, std::uint64_t> m_generations;
	};
}
