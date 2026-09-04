#include "common/chat/chat_repository.h"

#include "common/chat/chat_ids.h"
#include "common/chat/message_attachment_json.h"
#include "common/config/approval_modes.h"
#include "common/config/execution_host_config.h"
#include "common/memory/memory_levels.h"
#include "common/security/command_safety.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/json_runtime.h"
#include "common/utils/io_utils.h"
#include "common/utils/parse_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "computer_use/computer_use_mcp_config.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace
{
	namespace fs = std::filesystem;
	namespace attachment_fields = uam::message_attachment_json;
	namespace attachment_persisted_fields = uam::message_attachment_json::persisted;

	constexpr std::string_view kLegacyProviderIdKey = "provider_id";
	constexpr std::string_view kLegacyNativeSessionIdKey = "native_session_id";
	constexpr std::string_view kLegacyParentChatKey = "parent_chat";
	constexpr std::string_view kLegacyBranchRootKey = "branch_root";
	constexpr std::string_view kLegacyBranchFromIndexKey = "branch_from_index";
	constexpr std::string_view kLegacyFolderKey = "folder";
	constexpr std::string_view kLegacyTitleKey = "title";
	constexpr std::string_view kLegacyCreatedAtKey = "created_at";
	constexpr std::string_view kLegacyUpdatedAtKey = "updated_at";
	constexpr std::string_view kLegacyLastOpenedAtKey = "last_opened_at";
	constexpr std::string_view kLegacyLinkedFileKey = "file";

	constexpr std::string_view kMessageRoleField = "role";
	constexpr std::string_view kMessageContentField = "content";
	constexpr std::string_view kMessageCreatedAtField = "created_at";
	constexpr std::string_view kMessageProviderField = "provider";
	constexpr std::string_view kMessageTokensInputField = "tokens_input";
	constexpr std::string_view kMessageTokensOutputField = "tokens_output";
	constexpr std::string_view kMessageEstimatedCostUsdField = "estimated_cost_usd";
	constexpr std::string_view kMessageTimeToFirstTokenMsField = "time_to_first_token_ms";
	constexpr std::string_view kMessageProcessingTimeMsField = "processing_time_ms";
	constexpr std::string_view kMessageCheckpointShaField = "checkpoint_sha";
	constexpr std::string_view kMessageCheckpointParentShaField = "checkpoint_parent_sha";
	constexpr std::string_view kMessageInterruptedField = "interrupted";
	constexpr std::string_view kMessagePrioritySteerField = "priority_steer";
	constexpr std::string_view kMessageThoughtsField = "thoughts";
	constexpr std::string_view kMessagePlanSummaryField = "plan_summary";
	constexpr std::string_view kMessagePlanEntriesField = "plan_entries";
	constexpr std::string_view kMessageToolCallsField = "tool_calls";
	constexpr std::string_view kMessageBlocksField = "blocks";
	constexpr std::string_view kMessageMarkdownStoreFilesField = "markdown_store_files";
	constexpr std::string_view kMessageMarkdownStorePromptBlocksField = "markdown_store_prompt_blocks";
	constexpr std::string_view kMessageAttachmentsField = "attachments";

	constexpr std::string_view kPlanEntryPriorityField = "priority";
	constexpr std::string_view kPlanEntryStatusField = "status";

	constexpr std::string_view kToolCallIdField = "id";
	constexpr std::string_view kToolCallNameField = "name";
	constexpr std::string_view kToolCallArgsJsonField = "args_json";
	constexpr std::string_view kToolCallResultTextField = "result_text";
	constexpr std::string_view kToolCallStatusField = "status";
	constexpr std::string_view kToolCallIsSubAgentField = "is_sub_agent";
	constexpr std::string_view kToolCallSubAgentIdField = "sub_agent_id";
	constexpr std::string_view kToolCallSubAgentTitleField = "sub_agent_title";

	constexpr std::string_view kMessageBlockTypeField = "type";
	constexpr std::string_view kMessageBlockTextField = "text";
	constexpr std::string_view kMessageBlockToolCallIdField = "tool_call_id";
	constexpr std::string_view kMessageBlockRequestIdField = "request_id";

	constexpr std::string_view kChatIdField = "id";
	constexpr std::string_view kChatExecutionHostIdField = "execution_host_id";
	constexpr std::string_view kChatProviderIdField = "provider_id";
	constexpr std::string_view kChatNativeSessionIdField = "native_session_id";
	constexpr std::string_view kChatRemoteTurnReconnectPendingField = "remote_turn_reconnect_pending";
	constexpr std::string_view kChatRemoteProcessExistsField = "remote_process_exists";
	constexpr std::string_view kChatRemoteStopCleanupPendingField = "remote_stop_cleanup_pending";
	constexpr std::string_view kChatRemoteRestartPendingField = "remote_restart_pending";
	constexpr std::string_view kChatRemoteProcessControlTokenField = "remote_process_control_token";
	constexpr std::string_view kChatRemoteDeliveredStdoutCursorField = "remote_delivered_stdout_cursor";
	constexpr std::string_view kChatRemoteDeliveredStderrCursorField = "remote_delivered_stderr_cursor";
	constexpr std::string_view kChatRemoteSourceExitPendingField = "remote_source_exit_pending";
	constexpr std::string_view kChatRemoteSourceExitCodeField = "remote_source_exit_code";
	constexpr std::string_view kChatRemoteUamControlChannelIdField = "remote_uam_control_channel_id";
	constexpr std::string_view kChatRemoteInteractionResponseRequestIdField = "remote_interaction_response_request_id";
	constexpr std::string_view kChatRemoteInteractionResponseJsonField = "remote_interaction_response_json";
	constexpr std::string_view kChatRemoteInteractionResponsesField = "remote_interaction_responses";
	constexpr std::string_view kChatRemotePromptDeliverySessionIdField = "remote_prompt_delivery_session_id";
	constexpr std::string_view kChatRemotePromptDeliveryIdField = "remote_prompt_delivery_id";
	constexpr std::string_view kChatRemotePromptDeliveryPayloadField = "remote_prompt_delivery_payload";
	constexpr std::string_view kChatAcpQueuedPromptsField = "acp_queued_prompts";
	constexpr std::string_view kChatAcpDispatchedQueuedPromptCountField = "acp_dispatched_queued_prompt_count";
	constexpr std::size_t kMaxPersistedAcpQueuedPrompts = 32;
	constexpr std::size_t kMaxPersistedAcpQueuedPromptBytes = 2U * 1024U * 1024U;
	constexpr std::size_t kMaxPersistedRemoteInteractionResponseBytes = 1024U * 1024U;
	constexpr std::size_t kMaxPersistedRemoteInteractionResponses = 64;
	constexpr std::size_t kMaxPersistedRemoteInteractionResponsesBytes = 2U * 1024U * 1024U;
	constexpr std::size_t kMaxPersistedRemotePromptDeliveryBytes = 2U * 1024U * 1024U;
	constexpr std::string_view kChatParentChatIdField = "parent_chat_id";
	constexpr std::string_view kChatBranchRootChatIdField = "branch_root_chat_id";
	constexpr std::string_view kChatBranchFromMessageIndexField = "branch_from_message_index";
	constexpr std::string_view kChatBranchMessageEditedField = "branch_message_edited";
	constexpr std::string_view kChatFolderIdField = "folder_id";
	constexpr std::string_view kChatTitleField = "title";
	constexpr std::string_view kChatCreatedAtField = "created_at";
	constexpr std::string_view kChatUpdatedAtField = "updated_at";
	constexpr std::string_view kChatLastOpenedAtField = "last_opened_at";
	constexpr std::string_view kChatPinnedField = "pinned";
	constexpr std::string_view kChatLinkedFilesField = "linked_files";
	constexpr std::string_view kChatWorkspaceDirectoryField = "workspace_directory";
	constexpr std::string_view kChatWorkspaceIsolationKindField = "workspace_isolation_kind";
	constexpr std::string_view kChatWorkspaceSourceDirectoryField = "workspace_source_directory";
	constexpr std::string_view kChatWorkspaceBaseRefField = "workspace_base_ref";
	constexpr std::string_view kChatWorkspaceBranchNameField = "workspace_branch_name";
	constexpr std::string_view kChatWorkspaceWorktreeDirectoryField = "workspace_worktree_directory";
	constexpr std::string_view kChatImportedReadOnlyField = "imported_read_only";
	constexpr std::string_view kChatApprovalModeField = "approval_mode";
	constexpr std::string_view kChatUamAgentIdField = "uam_agent_id";
	constexpr std::string_view kChatAgentRunIdField = "agent_run_id";
	constexpr std::string_view kChatGoalOwnerChatIdField = "goal_owner_chat_id";
	constexpr std::string_view kChatGoalIterationGoalIdField = "goal_iteration_goal_id";
	constexpr std::string_view kChatGoalIterationTurnKindField = "goal_iteration_turn_kind";
	constexpr std::string_view kChatGoalIterationRepairAttemptsField = "goal_iteration_repair_attempts";
	constexpr std::string_view kChatAutoApproveCommandsField = "auto_approve_commands";
	constexpr std::string_view kChatCommandSafetyTierField = "commandSafetyTier";
	constexpr std::string_view kChatComputerUseBackendField = "computer_use_backend";
	constexpr std::string_view kChatModelIdField = "model_id";
	constexpr std::string_view kChatReviewerModelIdField = "reviewer_model_id";
	constexpr std::string_view kChatReasoningEffortField = "reasoning_effort";
	constexpr std::string_view kChatServiceTierField = "service_tier";
	constexpr std::string_view kChatServiceTierExplicitField = "service_tier_explicit";
	constexpr std::string_view kChatExtraFlagsField = "extra_flags";
	constexpr std::string_view kChatMemoryLevelField = "memory_level";
	constexpr std::string_view kChatMemoryEnabledField = "memory_enabled";
	constexpr std::string_view kChatMemoryLastProcessedMessageCountField = "memory_last_processed_message_count";
	constexpr std::string_view kChatMemoryLastProcessedAtField = "memory_last_processed_at";
	constexpr std::string_view kChatPersistedMessageCountField = "persisted_message_count";
	constexpr std::string_view kChatPersistedMessagesDigestField = "persisted_messages_digest";
	constexpr std::string_view kChatSummarySourceSizeField = "summary_source_size";
	constexpr std::string_view kChatSmallModelModeField = "small_model_mode";
	constexpr std::string_view kChatMessagesField = "messages";

	MessageRole ParseLegacyMessageRoleFromFilename(const fs::path& message_file)
	{
		const std::string file_name = message_file.filename().string();
		const auto underscore = file_name.find('_');
		const auto dot = file_name.find_last_of('.');
		if (underscore == std::string::npos || dot == std::string::npos || dot <= underscore)
		{
			return MessageRole::User;
		}

		return RoleFromString(file_name.substr(underscore + 1, dot - underscore - 1));
	}

	std::string_view StripCarriageReturn(std::string_view line)
	{
		if (!line.empty() && line.back() == '\r')
		{
			return line.substr(0, line.size() - 1);
		}
		return line;
	}

	bool ChatUpdatedNewestFirst(const ChatSession& lhs, const ChatSession& rhs)
	{
		return lhs.updated_at > rhs.updated_at;
	}

	void NormalizeLoadedNativeSessionId(ChatSession& chat)
	{
		if (uam::provider_ids::IsCliProviderAliasOf(chat.provider_id, uam::provider_ids::kCodexCli))
		{
			chat.native_session_id = uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
			return;
		}

		if (chat.native_session_id.empty() && !chat.id.empty() && !uam::chat_ids::IsLocalDraftChatId(chat.id))
		{
			chat.native_session_id = chat.id;
		}
	}

	JsonValue StringArrayToJson(const std::vector<std::string>& values);
	std::vector<std::string> JsonStringArrayOrEmpty(const JsonValue* value);

	void SetNonEmptyString(JsonValue& obj, std::string_view key, std::string_view value)
	{
		if (!value.empty())
		{
			uam::json::SetString(obj, key, value);
		}
	}

	void SetPositiveNumber(JsonValue& obj, std::string_view key, double value)
	{
		if (value > 0.0)
		{
			uam::json::SetNumber(obj, key, value);
		}
	}

	int NonNegativeIntFieldOrZero(const JsonValue* value)
	{
		const double parsed = JsonNumberOrDefault(value, 0.0);
		const double bounded = std::clamp(parsed, 0.0, static_cast<double>(std::numeric_limits<int>::max()));
		return static_cast<int>(bounded);
	}

	int64_t NonNegativeInt64FieldOrZero(const JsonValue* value)
	{
		// JsonValue stores numbers as double, so 2^53-1 is the largest exact integer contract.
		constexpr double kMaxExactJsonInteger = 9'007'199'254'740'991.0;
		const double parsed = JsonNumberOrDefault(value, 0.0);
		const double bounded = std::clamp(parsed, 0.0, kMaxExactJsonInteger);
		return static_cast<int64_t>(bounded);
	}

	int IntFieldAtLeastOrDefault(const JsonValue* value, int minimum, int fallback)
	{
		const double parsed = JsonNumberOrDefault(value, static_cast<double>(fallback));
		const double bounded = std::clamp(parsed, static_cast<double>(minimum), static_cast<double>(std::numeric_limits<int>::max()));
		return static_cast<int>(bounded);
	}

	double NonNegativeNumberFieldOrZero(const JsonValue* value)
	{
		return std::max(0.0, JsonNumberOrDefault(value, 0.0));
	}

	std::uintmax_t NonNegativeUintmaxFieldOrZero(const JsonValue* value)
	{
		const double parsed = JsonNumberOrDefault(value, 0.0);
		if (parsed <= 0.0)
		{
			return 0;
		}

		const double max_value = static_cast<double>(std::numeric_limits<std::uintmax_t>::max());
		if (parsed >= max_value)
		{
			return std::numeric_limits<std::uintmax_t>::max();
		}

		return static_cast<std::uintmax_t>(parsed);
	}

	JsonValue AttachmentToJson(const MessageAttachment& attachment)
	{
		JsonValue obj = uam::json::Object();
		uam::json::SetString(obj, attachment_fields::kIdField, attachment.id);
		uam::json::SetString(obj, attachment_fields::kNameField, attachment.name);
		uam::json::SetString(obj, attachment_fields::kKindField, attachment.kind);
		uam::json::SetString(obj, attachment_persisted_fields::kMimeTypeField, attachment.mime_type);
		uam::json::SetString(obj, attachment_fields::kPathField, attachment.path);
		uam::json::SetNumber(obj, attachment_persisted_fields::kSizeBytesField, static_cast<double>(attachment.size_bytes));
		uam::json::SetBool(obj, attachment_fields::kCopiedField, attachment.copied);
		return obj;
	}

	JsonValue AcpQueuedPromptToJson(const uam::AcpQueuedUserPromptState& prompt)
	{
		JsonValue obj = uam::json::Object();
		uam::json::SetString(obj, "text", prompt.text);
		uam::json::SetString(obj, "uam_agent_id", prompt.uam_agent_id);
		uam::json::SetString(obj, "uam_agent_definition_hash", prompt.uam_agent_definition_hash);
		uam::json::SetString(obj, "uam_agent_definition_snapshot", prompt.uam_agent_definition_snapshot);
		uam::json::SetString(obj, "uam_agent_instructions", prompt.uam_agent_instructions);
		uam::json::SetValue(obj, "uam_agent_skills", StringArrayToJson(prompt.uam_agent_skills));
		uam::json::SetValue(obj, "uam_agent_delegates", StringArrayToJson(prompt.uam_agent_delegates));
		uam::json::SetString(obj, "uam_agent_workspace_access", prompt.uam_agent_workspace_access);
		uam::json::SetString(obj, "uam_agent_execution_capability", prompt.uam_agent_execution_capability);
		uam::json::SetValue(obj, "markdown_store_files", StringArrayToJson(prompt.markdown_store_files));
		JsonValue prompt_blocks = uam::json::Array();
		for (const std::string& block : prompt.markdown_store_prompt_blocks)
			uam::json::PushValue(prompt_blocks, uam::json::String(block));
		uam::json::SetValue(obj, "markdown_store_prompt_blocks", std::move(prompt_blocks));
		JsonValue attachments = uam::json::Array();
		for (const MessageAttachment& attachment : prompt.attachments)
			uam::json::PushValue(attachments, AttachmentToJson(attachment));
		uam::json::SetValue(obj, "attachments", std::move(attachments));
		uam::json::SetBool(obj, "append_user_message", prompt.append_user_message);
		uam::json::SetBool(obj, "goal_mode", prompt.goal_mode);
		uam::json::SetString(obj, "goal_id", prompt.goal_id);
		uam::json::SetBool(obj, "computer_use_mode", prompt.computer_use_mode);
		uam::json::SetBool(obj, "priority_steer", prompt.priority_steer);
		return obj;
	}

	JsonValue AcpRemoteInteractionResponseToJson(
	    const uam::AcpRemoteInteractionResponseState& response)
	{
		JsonValue obj = uam::json::Object();
		uam::json::SetString(obj, "request_id_json", response.request_id_json);
		uam::json::SetString(obj, "response_json", response.response_json);
		return obj;
	}

	std::optional<uam::AcpRemoteInteractionResponseState>
	AcpRemoteInteractionResponseFromJson(const JsonValue& obj,
	                                     std::size_t& total_response_bytes)
	{
		if (obj.type != JsonValue::Type::Object) return std::nullopt;
		uam::AcpRemoteInteractionResponseState response;
		response.request_id_json = JsonStringOrEmpty(obj.Find("request_id_json"));
		response.response_json = JsonStringOrEmpty(obj.Find("response_json"));
		if (response.request_id_json.empty() || response.request_id_json.size() > 1024 ||
		    response.response_json.empty() ||
		    response.response_json.size() > kMaxPersistedRemoteInteractionResponseBytes ||
		    response.response_json.size() > kMaxPersistedRemoteInteractionResponsesBytes -
		        std::min(total_response_bytes,
		                 kMaxPersistedRemoteInteractionResponsesBytes) ||
		    !ParseJson(response.response_json).has_value())
			return std::nullopt;
		total_response_bytes += response.response_json.size();
		return response;
	}

	std::optional<uam::AcpQueuedUserPromptState> AcpQueuedPromptFromJson(
	    const JsonValue& obj, std::size_t& total_text_bytes)
	{
		if (obj.type != JsonValue::Type::Object) return std::nullopt;
		uam::AcpQueuedUserPromptState prompt;
		prompt.text = JsonStringOrEmpty(obj.Find("text"));
		if (prompt.text.empty() || prompt.text.size() > kMaxPersistedAcpQueuedPromptBytes -
		    std::min(total_text_bytes, kMaxPersistedAcpQueuedPromptBytes)) return std::nullopt;
		total_text_bytes += prompt.text.size();
		prompt.uam_agent_id = uam::strings::NonEmptyOrFallback(
		    JsonStringOrEmpty(obj.Find("uam_agent_id")), "build");
		prompt.uam_agent_definition_hash = JsonStringOrEmpty(obj.Find("uam_agent_definition_hash"));
		prompt.uam_agent_definition_snapshot = JsonStringOrEmpty(obj.Find("uam_agent_definition_snapshot"));
		prompt.uam_agent_instructions = JsonStringOrEmpty(obj.Find("uam_agent_instructions"));
		prompt.uam_agent_skills = JsonStringArrayOrEmpty(obj.Find("uam_agent_skills"));
		prompt.uam_agent_delegates = JsonStringArrayOrEmpty(obj.Find("uam_agent_delegates"));
		prompt.uam_agent_workspace_access = uam::strings::NonEmptyOrFallback(
		    JsonStringOrEmpty(obj.Find("uam_agent_workspace_access")), "write");
		prompt.uam_agent_execution_capability = uam::strings::NonEmptyOrFallback(
		    JsonStringOrEmpty(obj.Find("uam_agent_execution_capability")), "uam-prompt-injected");
		prompt.markdown_store_files = JsonStringArrayOrEmpty(obj.Find("markdown_store_files"));
		if (const JsonValue* blocks = uam::json::ArrayOrNull(obj.Find("markdown_store_prompt_blocks"));
		    blocks != nullptr)
		{
			for (const JsonValue& block : blocks->array_value)
				if (block.type == JsonValue::Type::String)
					prompt.markdown_store_prompt_blocks.push_back(block.string_value);
		}
		if (const JsonValue* attachments = uam::json::ArrayOrNull(obj.Find("attachments"));
		    attachments != nullptr)
		{
			for (const JsonValue& item : attachments->array_value)
			{
				if (item.type != JsonValue::Type::Object || prompt.attachments.size() >= 64) break;
				MessageAttachment attachment;
				attachment.id = JsonStringOrEmpty(item.Find(attachment_fields::kIdField));
				attachment.name = JsonStringOrEmpty(item.Find(attachment_fields::kNameField));
				attachment.kind = JsonStringOrEmpty(item.Find(attachment_fields::kKindField));
				attachment.mime_type = JsonStringOrEmpty(item.Find(attachment_persisted_fields::kMimeTypeField));
				attachment.path = JsonStringOrEmpty(item.Find(attachment_fields::kPathField));
				attachment.size_bytes = NonNegativeUintmaxFieldOrZero(item.Find(attachment_persisted_fields::kSizeBytesField));
				attachment.copied = JsonBoolOrDefault(item.Find(attachment_fields::kCopiedField), false);
				if (!attachment.path.empty()) prompt.attachments.push_back(std::move(attachment));
			}
		}
		prompt.append_user_message = JsonBoolOrDefault(obj.Find("append_user_message"), true);
		prompt.goal_mode = JsonBoolOrDefault(obj.Find("goal_mode"), false);
		prompt.goal_id = JsonStringOrEmpty(obj.Find("goal_id"));
		prompt.computer_use_mode = JsonBoolOrDefault(obj.Find("computer_use_mode"), false);
		prompt.priority_steer = JsonBoolOrDefault(obj.Find("priority_steer"), false);
		return prompt;
	}

	JsonValue MessageToJson(const Message& msg)
	{
		JsonValue obj = uam::json::Object();
		uam::json::SetString(obj, kMessageRoleField, RoleToString(msg.role));
		uam::json::SetString(obj, kMessageContentField, msg.content);
		uam::json::SetString(obj, kMessageCreatedAtField, msg.created_at);

		SetNonEmptyString(obj, kMessageProviderField, msg.provider);
		SetPositiveNumber(obj, kMessageTokensInputField, static_cast<double>(msg.tokens_input));
		SetPositiveNumber(obj, kMessageTokensOutputField, static_cast<double>(msg.tokens_output));
		SetPositiveNumber(obj, kMessageEstimatedCostUsdField, msg.estimated_cost_usd);
		SetPositiveNumber(obj, kMessageTimeToFirstTokenMsField, static_cast<double>(msg.time_to_first_token_ms));
		SetPositiveNumber(obj, kMessageProcessingTimeMsField, static_cast<double>(msg.processing_time_ms));
		if (msg.interrupted)
		{
			uam::json::SetBool(obj, kMessageInterruptedField, true);
		}
		if (msg.priority_steer)
		{
			uam::json::SetBool(obj, kMessagePrioritySteerField, true);
		}
		SetNonEmptyString(obj, kMessageCheckpointShaField, msg.checkpoint_sha);
		SetNonEmptyString(obj, kMessageCheckpointParentShaField, msg.checkpoint_parent_sha);
		SetNonEmptyString(obj, kMessageThoughtsField, msg.thoughts);
		SetNonEmptyString(obj, kMessagePlanSummaryField, msg.plan_summary);
		if (!msg.plan_entries.empty())
		{
			JsonValue plan_arr = uam::json::Array();
			plan_arr.array_value.reserve(msg.plan_entries.size());
			for (const MessagePlanEntry& entry : msg.plan_entries)
			{
				JsonValue entry_obj = uam::json::Object();
				uam::json::SetString(entry_obj, kMessageContentField, entry.content);
				uam::json::SetString(entry_obj, kPlanEntryPriorityField, entry.priority);
				uam::json::SetString(entry_obj, kPlanEntryStatusField, entry.status);
				uam::json::PushValue(plan_arr, std::move(entry_obj));
			}
			uam::json::SetValue(obj, kMessagePlanEntriesField, std::move(plan_arr));
		}
		if (!msg.tool_calls.empty())
		{
			JsonValue tc_arr = uam::json::Array();
			tc_arr.array_value.reserve(msg.tool_calls.size());
			for (const auto& tc : msg.tool_calls)
			{
				JsonValue tc_obj = uam::json::Object();
				uam::json::SetString(tc_obj, kToolCallIdField, tc.id);
				uam::json::SetString(tc_obj, kToolCallNameField, tc.name);
				uam::json::SetString(tc_obj, kToolCallArgsJsonField, tc.args_json);
				uam::json::SetString(tc_obj, kToolCallResultTextField, tc.result_text);
				uam::json::SetString(tc_obj, kToolCallStatusField, tc.status);
				uam::json::SetBool(tc_obj, kToolCallIsSubAgentField, tc.is_sub_agent);
				uam::json::SetString(tc_obj, kToolCallSubAgentIdField, tc.sub_agent_id);
				uam::json::SetString(tc_obj, kToolCallSubAgentTitleField, tc.sub_agent_title);
				uam::json::PushValue(tc_arr, std::move(tc_obj));
			}
			uam::json::SetValue(obj, kMessageToolCallsField, std::move(tc_arr));
		}
		if (!msg.blocks.empty())
		{
			JsonValue block_arr = uam::json::Array();
			block_arr.array_value.reserve(msg.blocks.size());
			for (const MessageBlock& block : msg.blocks)
			{
				if (block.type.empty())
				{
					continue;
				}
				JsonValue block_obj = uam::json::Object();
				uam::json::SetString(block_obj, kMessageBlockTypeField, block.type);
				SetNonEmptyString(block_obj, kMessageBlockTextField, block.text);
				SetNonEmptyString(block_obj, kMessageBlockToolCallIdField, block.tool_call_id);
				SetNonEmptyString(block_obj, kMessageBlockRequestIdField, block.request_id_json);
				uam::json::PushValue(block_arr, std::move(block_obj));
			}
			if (!block_arr.array_value.empty())
			{
				uam::json::SetValue(obj, kMessageBlocksField, std::move(block_arr));
			}
		}
		if (!msg.markdown_store_files.empty())
		{
			uam::json::SetValue(obj, kMessageMarkdownStoreFilesField, StringArrayToJson(msg.markdown_store_files));
		}
		if (!msg.markdown_store_prompt_blocks.empty())
		{
			JsonValue prompt_blocks = uam::json::Array();
			for (const std::string& prompt_block : msg.markdown_store_prompt_blocks)
			{
				if (!prompt_block.empty())
				{
					uam::json::PushValue(prompt_blocks, uam::json::String(prompt_block));
				}
			}
			if (!prompt_blocks.array_value.empty())
			{
				uam::json::SetValue(obj, kMessageMarkdownStorePromptBlocksField, std::move(prompt_blocks));
			}
		}
		if (!msg.attachments.empty())
		{
			JsonValue attachments = uam::json::Array();
			attachments.array_value.reserve(msg.attachments.size());
			for (const MessageAttachment& attachment : msg.attachments)
			{
				if (attachment.path.empty())
				{
					continue;
				}
				uam::json::PushValue(attachments, AttachmentToJson(attachment));
			}
			if (!attachments.array_value.empty())
			{
				uam::json::SetValue(obj, kMessageAttachmentsField, std::move(attachments));
			}
		}
		return obj;
	}

	Message JsonToMessage(const JsonValue& obj)
	{
		Message msg;
		msg.role = RoleFromString(JsonStringOrEmpty(obj.Find(kMessageRoleField)));
		msg.content = JsonStringOrEmpty(obj.Find(kMessageContentField));
		msg.created_at = JsonStringOrEmpty(obj.Find(kMessageCreatedAtField));
		msg.provider = JsonStringOrEmpty(obj.Find(kMessageProviderField));
		msg.tokens_input = NonNegativeIntFieldOrZero(obj.Find(kMessageTokensInputField));
		msg.tokens_output = NonNegativeIntFieldOrZero(obj.Find(kMessageTokensOutputField));
		msg.estimated_cost_usd = NonNegativeNumberFieldOrZero(obj.Find(kMessageEstimatedCostUsdField));
		msg.time_to_first_token_ms = NonNegativeIntFieldOrZero(obj.Find(kMessageTimeToFirstTokenMsField));
		msg.processing_time_ms = NonNegativeIntFieldOrZero(obj.Find(kMessageProcessingTimeMsField));
		msg.interrupted = JsonBoolOrDefault(obj.Find(kMessageInterruptedField), false);
		msg.priority_steer = JsonBoolOrDefault(obj.Find(kMessagePrioritySteerField), false);
		msg.checkpoint_sha = JsonStringOrEmpty(obj.Find(kMessageCheckpointShaField));
		msg.checkpoint_parent_sha = JsonStringOrEmpty(obj.Find(kMessageCheckpointParentShaField));
		msg.thoughts = JsonStringOrEmpty(obj.Find(kMessageThoughtsField));
		msg.plan_summary = JsonStringOrEmpty(obj.Find(kMessagePlanSummaryField));

		if (const JsonValue* plan_arr = uam::json::ArrayOrNull(obj.Find(kMessagePlanEntriesField)); plan_arr != nullptr)
		{
			msg.plan_entries.reserve(plan_arr->array_value.size());
			for (const JsonValue& entry : plan_arr->array_value)
			{
				if (entry.type != JsonValue::Type::Object)
				{
					continue;
				}

				MessagePlanEntry plan_entry;
				plan_entry.content = JsonStringOrEmpty(entry.Find(kMessageContentField));
				plan_entry.priority = JsonStringOrEmpty(entry.Find(kPlanEntryPriorityField));
				plan_entry.status = JsonStringOrEmpty(entry.Find(kPlanEntryStatusField));
				msg.plan_entries.push_back(std::move(plan_entry));
			}
		}

		if (const JsonValue* tc_arr = uam::json::ArrayOrNull(obj.Find(kMessageToolCallsField)); tc_arr != nullptr)
		{
			msg.tool_calls.reserve(tc_arr->array_value.size());
			for (const JsonValue& tc : tc_arr->array_value)
			{
				if (tc.type != JsonValue::Type::Object)
				{
					continue;
				}

				ToolCall tool_call;
				tool_call.id = JsonStringOrEmpty(tc.Find(kToolCallIdField));
				tool_call.name = JsonStringOrEmpty(tc.Find(kToolCallNameField));
				tool_call.args_json = JsonStringOrEmpty(tc.Find(kToolCallArgsJsonField));
				tool_call.result_text = JsonStringOrEmpty(tc.Find(kToolCallResultTextField));
				tool_call.status = JsonStringOrEmpty(tc.Find(kToolCallStatusField));
				tool_call.is_sub_agent = JsonBoolOrDefault(tc.Find(kToolCallIsSubAgentField), false);
				tool_call.sub_agent_id = JsonStringOrEmpty(tc.Find(kToolCallSubAgentIdField));
				tool_call.sub_agent_title = JsonStringOrEmpty(tc.Find(kToolCallSubAgentTitleField));
				msg.tool_calls.push_back(std::move(tool_call));
			}
		}

		if (const JsonValue* block_arr = uam::json::ArrayOrNull(obj.Find(kMessageBlocksField)); block_arr != nullptr)
		{
			msg.blocks.reserve(block_arr->array_value.size());
			for (const JsonValue& block : block_arr->array_value)
			{
				if (block.type != JsonValue::Type::Object)
				{
					continue;
				}

				MessageBlock message_block;
				message_block.type = JsonStringOrEmpty(block.Find(kMessageBlockTypeField));
				if (message_block.type.empty())
				{
					continue;
				}

				message_block.text = JsonStringOrEmpty(block.Find(kMessageBlockTextField));
				message_block.tool_call_id = JsonStringOrEmpty(block.Find(kMessageBlockToolCallIdField));
				message_block.request_id_json = JsonStringOrEmpty(block.Find(kMessageBlockRequestIdField));
				msg.blocks.push_back(std::move(message_block));
			}
		}

		msg.markdown_store_files = JsonStringArrayOrEmpty(obj.Find(kMessageMarkdownStoreFilesField));
		if (const JsonValue* prompt_blocks = uam::json::ArrayOrNull(obj.Find(kMessageMarkdownStorePromptBlocksField)); prompt_blocks != nullptr)
		{
			for (const JsonValue& prompt_block : prompt_blocks->array_value)
			{
				if (prompt_block.type == JsonValue::Type::String && !prompt_block.string_value.empty())
				{
					msg.markdown_store_prompt_blocks.push_back(prompt_block.string_value);
				}
			}
		}

		if (const JsonValue* attachments = uam::json::ArrayOrNull(obj.Find(kMessageAttachmentsField)); attachments != nullptr)
		{
			msg.attachments.reserve(attachments->array_value.size());
			for (const JsonValue& item : attachments->array_value)
			{
				if (item.type != JsonValue::Type::Object)
				{
					continue;
				}

				MessageAttachment attachment;
				attachment.id = JsonStringOrEmpty(item.Find(attachment_fields::kIdField));
				attachment.name = JsonStringOrEmpty(item.Find(attachment_fields::kNameField));
				attachment.kind = JsonStringOrEmpty(item.Find(attachment_fields::kKindField));
				attachment.mime_type = JsonStringOrEmpty(item.Find(attachment_persisted_fields::kMimeTypeField));
				attachment.path = JsonStringOrEmpty(item.Find(attachment_fields::kPathField));
				attachment.size_bytes = NonNegativeUintmaxFieldOrZero(item.Find(attachment_persisted_fields::kSizeBytesField));
				attachment.copied = JsonBoolOrDefault(item.Find(attachment_fields::kCopiedField), false);
				if (!attachment.path.empty())
				{
					msg.attachments.push_back(std::move(attachment));
				}
			}
		}
		return msg;
	}

	JsonValue StringArrayToJson(const std::vector<std::string>& values)
	{
		JsonValue arr = uam::json::Array();
		arr.array_value.reserve(values.size());

		for (const std::string& value : values)
		{
			const std::string trimmed_value = uam::strings::Trim(value);
			if (!trimmed_value.empty())
			{
				uam::json::PushValue(arr, uam::json::String(trimmed_value));
			}
		}

		return arr;
	}

	std::vector<std::string> JsonStringArrayOrEmpty(const JsonValue* value)
	{
		std::vector<std::string> out;

		if (value == nullptr || value->type != JsonValue::Type::Array)
		{
			return out;
		}

		out.reserve(value->array_value.size());
		for (const JsonValue& item : value->array_value)
		{
			if (item.type == JsonValue::Type::String)
			{
				const std::string trimmed_value = uam::strings::Trim(item.string_value);
				if (!trimmed_value.empty())
				{
					out.push_back(trimmed_value);
				}
			}
		}

		return out;
	}

	template <typename T, typename Predicate> bool EquivalentVectors(const std::vector<T>& lhs, const std::vector<T>& rhs, Predicate equivalent)
	{
		return std::ranges::equal(lhs, rhs, equivalent);
	}

	bool ToolCallEquivalentForRecovery(const ToolCall& lhs, const ToolCall& rhs)
	{
		return lhs.id == rhs.id && lhs.name == rhs.name && lhs.args_json == rhs.args_json && lhs.result_text == rhs.result_text && lhs.status == rhs.status && lhs.is_sub_agent == rhs.is_sub_agent && lhs.sub_agent_id == rhs.sub_agent_id && lhs.sub_agent_title == rhs.sub_agent_title;
	}

	bool ToolCallsEquivalentForRecovery(const std::vector<ToolCall>& lhs, const std::vector<ToolCall>& rhs)
	{
		return EquivalentVectors(lhs, rhs, ToolCallEquivalentForRecovery);
	}

	bool PlanEntryEquivalentForRecovery(const MessagePlanEntry& lhs, const MessagePlanEntry& rhs)
	{
		return lhs.content == rhs.content && lhs.priority == rhs.priority && lhs.status == rhs.status;
	}

	bool PlanEntriesEquivalentForRecovery(const std::vector<MessagePlanEntry>& lhs, const std::vector<MessagePlanEntry>& rhs)
	{
		return EquivalentVectors(lhs, rhs, PlanEntryEquivalentForRecovery);
	}

	bool MessageBlockEquivalentForRecovery(const MessageBlock& lhs, const MessageBlock& rhs)
	{
		return lhs.type == rhs.type && lhs.text == rhs.text && lhs.tool_call_id == rhs.tool_call_id && lhs.request_id_json == rhs.request_id_json;
	}

	bool MessageBlocksEquivalentForRecovery(const std::vector<MessageBlock>& lhs, const std::vector<MessageBlock>& rhs)
	{
		return EquivalentVectors(lhs, rhs, MessageBlockEquivalentForRecovery);
	}

	bool MessageAttachmentEquivalentForRecovery(const MessageAttachment& lhs, const MessageAttachment& rhs)
	{
		return lhs.id == rhs.id && lhs.name == rhs.name && lhs.kind == rhs.kind && lhs.mime_type == rhs.mime_type && lhs.path == rhs.path && lhs.size_bytes == rhs.size_bytes && lhs.copied == rhs.copied;
	}

	bool MessageAttachmentsEquivalentForRecovery(const std::vector<MessageAttachment>& lhs, const std::vector<MessageAttachment>& rhs)
	{
		return EquivalentVectors(lhs, rhs, MessageAttachmentEquivalentForRecovery);
	}

	bool MessageMarkdownStoreFilesEquivalentForRecovery(const Message& lhs, const Message& rhs)
	{
		return lhs.markdown_store_files == rhs.markdown_store_files &&
		       lhs.markdown_store_prompt_blocks == rhs.markdown_store_prompt_blocks;
	}

	bool MessageIdentityFieldsEquivalentForRecovery(const Message& lhs, const Message& rhs)
	{
		return lhs.role == rhs.role && lhs.content == rhs.content && lhs.created_at == rhs.created_at && lhs.provider == rhs.provider;
	}

	bool MessageUsageFieldsEquivalentForRecovery(const Message& lhs, const Message& rhs)
	{
		return lhs.tokens_input == rhs.tokens_input && lhs.tokens_output == rhs.tokens_output && lhs.estimated_cost_usd == rhs.estimated_cost_usd;
	}

	bool MessageTimingFieldsEquivalentForRecovery(const Message& lhs, const Message& rhs)
	{
		return lhs.time_to_first_token_ms == rhs.time_to_first_token_ms && lhs.processing_time_ms == rhs.processing_time_ms && lhs.interrupted == rhs.interrupted && lhs.priority_steer == rhs.priority_steer && lhs.checkpoint_sha == rhs.checkpoint_sha && lhs.checkpoint_parent_sha == rhs.checkpoint_parent_sha;
	}

	bool MessageNarrativeFieldsEquivalentForRecovery(const Message& lhs, const Message& rhs)
	{
		return lhs.thoughts == rhs.thoughts && lhs.plan_summary == rhs.plan_summary;
	}

	bool MessageScalarFieldsEquivalentForRecovery(const Message& lhs, const Message& rhs)
	{
		return MessageIdentityFieldsEquivalentForRecovery(lhs, rhs) &&
		       MessageUsageFieldsEquivalentForRecovery(lhs, rhs) &&
		       MessageTimingFieldsEquivalentForRecovery(lhs, rhs) &&
		       MessageNarrativeFieldsEquivalentForRecovery(lhs, rhs);
	}

	bool MessageEquivalentForRecovery(const Message& lhs, const Message& rhs)
	{
		if (!MessageScalarFieldsEquivalentForRecovery(lhs, rhs))
		{
			return false;
		}
		if (!PlanEntriesEquivalentForRecovery(lhs.plan_entries, rhs.plan_entries))
		{
			return false;
		}
		if (!ToolCallsEquivalentForRecovery(lhs.tool_calls, rhs.tool_calls))
		{
			return false;
		}
		if (!MessageBlocksEquivalentForRecovery(lhs.blocks, rhs.blocks))
		{
			return false;
		}
		if (!MessageMarkdownStoreFilesEquivalentForRecovery(lhs, rhs))
		{
			return false;
		}

		return MessageAttachmentsEquivalentForRecovery(lhs.attachments, rhs.attachments);
	}

	bool MessagesEquivalentForRecovery(const std::vector<Message>& lhs, const std::vector<Message>& rhs)
	{
		return EquivalentVectors(lhs, rhs, MessageEquivalentForRecovery);
	}

	bool AcpQueuedPromptEquivalentForRecovery(
	    const uam::AcpQueuedUserPromptState& lhs,
	    const uam::AcpQueuedUserPromptState& rhs)
	{
		return lhs.text == rhs.text && lhs.uam_agent_id == rhs.uam_agent_id &&
		       lhs.uam_agent_definition_hash == rhs.uam_agent_definition_hash &&
		       lhs.uam_agent_definition_snapshot == rhs.uam_agent_definition_snapshot &&
		       lhs.uam_agent_instructions == rhs.uam_agent_instructions &&
		       lhs.uam_agent_skills == rhs.uam_agent_skills &&
		       lhs.uam_agent_delegates == rhs.uam_agent_delegates &&
		       lhs.uam_agent_workspace_access == rhs.uam_agent_workspace_access &&
		       lhs.uam_agent_execution_capability == rhs.uam_agent_execution_capability &&
		       lhs.markdown_store_files == rhs.markdown_store_files &&
		       lhs.markdown_store_prompt_blocks == rhs.markdown_store_prompt_blocks &&
		       MessageAttachmentsEquivalentForRecovery(lhs.attachments, rhs.attachments) &&
		       lhs.append_user_message == rhs.append_user_message &&
		       lhs.goal_mode == rhs.goal_mode && lhs.goal_id == rhs.goal_id &&
		       lhs.computer_use_mode == rhs.computer_use_mode &&
		       lhs.priority_steer == rhs.priority_steer;
	}

	bool AcpQueuedPromptsEquivalentForRecovery(
	    const std::vector<uam::AcpQueuedUserPromptState>& lhs,
	    const std::vector<uam::AcpQueuedUserPromptState>& rhs)
	{
		return EquivalentVectors(lhs, rhs, AcpQueuedPromptEquivalentForRecovery);
	}

	bool AcpRemoteInteractionResponsesEquivalentForRecovery(
	    const std::vector<uam::AcpRemoteInteractionResponseState>& lhs,
	    const std::vector<uam::AcpRemoteInteractionResponseState>& rhs)
	{
		return EquivalentVectors(lhs, rhs,
		    [](const uam::AcpRemoteInteractionResponseState& left,
	       const uam::AcpRemoteInteractionResponseState& right)
		    {
			    return left.request_id_json == right.request_id_json &&
			           left.response_json == right.response_json;
		    });
	}

	bool ChatIdentityFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		return lhs.id == rhs.id && lhs.execution_host_id == rhs.execution_host_id &&
		       lhs.provider_id == rhs.provider_id && lhs.native_session_id == rhs.native_session_id;
	}

	bool ChatBranchFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		if (lhs.parent_chat_id != rhs.parent_chat_id || lhs.branch_root_chat_id != rhs.branch_root_chat_id)
		{
			return false;
		}

		return lhs.branch_from_message_index == rhs.branch_from_message_index && lhs.branch_message_edited == rhs.branch_message_edited;
	}

	bool ChatDisplayFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		if (lhs.folder_id != rhs.folder_id || lhs.title != rhs.title || lhs.pinned != rhs.pinned)
		{
			return false;
		}
		if (lhs.created_at != rhs.created_at || lhs.updated_at != rhs.updated_at || lhs.last_opened_at != rhs.last_opened_at)
		{
			return false;
		}

		return lhs.linked_files == rhs.linked_files;
	}

	bool ChatWorkspaceFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		if (lhs.workspace_directory != rhs.workspace_directory || lhs.workspace_isolation_kind != rhs.workspace_isolation_kind)
		{
			return false;
		}
		if (lhs.workspace_source_directory != rhs.workspace_source_directory || lhs.workspace_base_ref != rhs.workspace_base_ref)
		{
			return false;
		}

		return lhs.workspace_branch_name == rhs.workspace_branch_name &&
		       lhs.workspace_worktree_directory == rhs.workspace_worktree_directory &&
		       lhs.imported_read_only == rhs.imported_read_only;
	}

	bool ChatProviderFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		if (lhs.approval_mode != rhs.approval_mode || lhs.uam_agent_id != rhs.uam_agent_id ||
		    lhs.agent_run_id != rhs.agent_run_id || lhs.command_safety_tier != rhs.command_safety_tier ||
		    lhs.computer_use_backend != rhs.computer_use_backend ||
		    lhs.goal_owner_chat_id != rhs.goal_owner_chat_id ||
		    lhs.goal_iteration_goal_id != rhs.goal_iteration_goal_id ||
		    lhs.goal_iteration_turn_kind != rhs.goal_iteration_turn_kind ||
		    lhs.goal_iteration_repair_attempts != rhs.goal_iteration_repair_attempts ||
		    lhs.model_id != rhs.model_id || lhs.reviewer_model_id != rhs.reviewer_model_id || lhs.small_model_mode != rhs.small_model_mode)
		{
			return false;
		}
		if (lhs.reasoning_effort != rhs.reasoning_effort || lhs.service_tier != rhs.service_tier || lhs.service_tier_explicit != rhs.service_tier_explicit)
		{
			return false;
		}

		return lhs.extra_flags == rhs.extra_flags &&
		       lhs.remote_turn_reconnect_pending == rhs.remote_turn_reconnect_pending &&
		       lhs.remote_process_exists == rhs.remote_process_exists &&
		       lhs.remote_stop_cleanup_pending == rhs.remote_stop_cleanup_pending &&
		       lhs.remote_restart_pending == rhs.remote_restart_pending &&
		       lhs.remote_process_control_token == rhs.remote_process_control_token &&
		       lhs.remote_delivered_stdout_cursor == rhs.remote_delivered_stdout_cursor &&
		       lhs.remote_delivered_stderr_cursor == rhs.remote_delivered_stderr_cursor &&
		       lhs.remote_source_exit_pending == rhs.remote_source_exit_pending &&
		       lhs.remote_source_exit_code == rhs.remote_source_exit_code &&
		       lhs.remote_uam_control_channel_id == rhs.remote_uam_control_channel_id &&
		       AcpRemoteInteractionResponsesEquivalentForRecovery(
		           lhs.remote_interaction_responses, rhs.remote_interaction_responses) &&
		       lhs.remote_prompt_delivery_session_id == rhs.remote_prompt_delivery_session_id &&
		       lhs.remote_prompt_delivery_id == rhs.remote_prompt_delivery_id &&
		       lhs.remote_prompt_delivery_payload == rhs.remote_prompt_delivery_payload &&
		       lhs.acp_dispatched_queued_prompt_count == rhs.acp_dispatched_queued_prompt_count &&
		       AcpQueuedPromptsEquivalentForRecovery(lhs.acp_queued_prompts, rhs.acp_queued_prompts);
	}

	bool ChatMemoryFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		if (lhs.memory_level != rhs.memory_level || lhs.memory_enabled != rhs.memory_enabled || lhs.memory_last_processed_message_count != rhs.memory_last_processed_message_count)
		{
			return false;
		}

		return lhs.memory_last_processed_at == rhs.memory_last_processed_at;
	}

	bool ChatScalarFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		return ChatIdentityFieldsEquivalentForRecovery(lhs, rhs) &&
		       ChatBranchFieldsEquivalentForRecovery(lhs, rhs) &&
		       ChatDisplayFieldsEquivalentForRecovery(lhs, rhs) &&
		       ChatWorkspaceFieldsEquivalentForRecovery(lhs, rhs) &&
		       ChatProviderFieldsEquivalentForRecovery(lhs, rhs) &&
		       ChatMemoryFieldsEquivalentForRecovery(lhs, rhs);
	}

	struct LoadChatResult
	{
		std::optional<ChatSession> chat;
		std::string error;
	};

	void ApplyChatTimestampFallbacks(ChatSession& chat);

	std::string SummaryDigest(const ChatSession& chat, std::size_t message_count)
	{
		return chat.updated_at + ":" + std::to_string(message_count);
	}

	bool ChatsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		return ChatScalarFieldsEquivalentForRecovery(lhs, rhs) && MessagesEquivalentForRecovery(lhs.messages, rhs.messages);
	}

	fs::path SummaryCachePathForChatFile(const fs::path& path)
	{
		return path.parent_path().parent_path() / "chat-summaries" / path.filename();
	}

	bool SummaryCacheIsCurrent(const fs::path& chat_path, const fs::path& summary_path)
	{
		std::error_code ec;
		const auto chat_time = fs::last_write_time(chat_path, ec);
		if (ec)
		{
			return false;
		}
		const auto summary_time = fs::last_write_time(summary_path, ec);
		return !ec && summary_time >= chat_time;
	}

	LoadChatResult ParseLocalChatFile(const fs::path& path, bool include_messages = true, std::uintmax_t expected_source_size = 0)
	{
		const std::string file_text = uam::io::ReadTextFile(path);
		if (file_text.empty())
		{
			return {std::nullopt, "is empty"};
		}

		auto root_opt = ParseJson(file_text);
		if (!root_opt || root_opt->type != JsonValue::Type::Object)
		{
			return {std::nullopt, "contains invalid JSON"};
		}

		JsonValue& root = *root_opt;
		if (expected_source_size > 0 &&
		    NonNegativeUintmaxFieldOrZero(root.Find(kChatSummarySourceSizeField)) != expected_source_size)
		{
			return {std::nullopt, "is a stale summary cache"};
		}
		ChatSession chat;

		chat.id = JsonStringOrEmpty(root.Find(kChatIdField));
		if (chat.id.empty())
		{
			return {std::nullopt, "is missing a chat id"};
		}

		if (!uam::chat_ids::IsSafeStorageChatId(chat.id))
		{
			return {std::nullopt, "contains an unsafe chat id"};
		}

		chat.execution_host_id = uam::strings::NonEmptyOrFallback(
		    JsonStringOrEmpty(root.Find(kChatExecutionHostIdField)), "local");
		chat.provider_id = JsonStringOrEmpty(root.Find(kChatProviderIdField));
		chat.native_session_id = JsonStringOrEmpty(root.Find(kChatNativeSessionIdField));
		chat.remote_turn_reconnect_pending = JsonBoolOrDefault(
		    root.Find(kChatRemoteTurnReconnectPendingField), false);
		chat.remote_process_exists = JsonBoolOrDefault(
		    root.Find(kChatRemoteProcessExistsField),
		    chat.remote_turn_reconnect_pending);
		chat.remote_stop_cleanup_pending = JsonBoolOrDefault(
		    root.Find(kChatRemoteStopCleanupPendingField), false);
		chat.remote_restart_pending = JsonBoolOrDefault(
		    root.Find(kChatRemoteRestartPendingField), false);
		if (root.Find(kChatRemoteProcessExistsField) == nullptr)
			chat.remote_process_exists = chat.remote_turn_reconnect_pending ||
			    chat.remote_stop_cleanup_pending || chat.remote_restart_pending;
		chat.remote_process_control_token = JsonStringOrEmpty(
		    root.Find(kChatRemoteProcessControlTokenField));
		if (chat.remote_process_control_token.size() > 256)
			chat.remote_process_control_token.clear();
		chat.remote_delivered_stdout_cursor = NonNegativeUintmaxFieldOrZero(
		    root.Find(kChatRemoteDeliveredStdoutCursorField));
		chat.remote_delivered_stderr_cursor = NonNegativeUintmaxFieldOrZero(
		    root.Find(kChatRemoteDeliveredStderrCursorField));
		chat.remote_source_exit_pending = JsonBoolOrDefault(
		    root.Find(kChatRemoteSourceExitPendingField), false);
		chat.remote_source_exit_code = chat.remote_source_exit_pending
		                                   ? IntFieldAtLeastOrDefault(
		                                         root.Find(kChatRemoteSourceExitCodeField), -1, -1)
		                                   : -1;
		chat.remote_uam_control_channel_id = JsonStringOrEmpty(
		    root.Find(kChatRemoteUamControlChannelIdField));
		if (chat.remote_uam_control_channel_id.size() > 256)
			chat.remote_uam_control_channel_id.clear();
		if (const JsonValue* responses = uam::json::ArrayOrNull(
		        root.Find(kChatRemoteInteractionResponsesField)); responses != nullptr)
		{
			std::size_t total_response_bytes = 0;
			for (const JsonValue& item : responses->array_value)
			{
				if (chat.remote_interaction_responses.size() >=
				    kMaxPersistedRemoteInteractionResponses) break;
				std::optional<uam::AcpRemoteInteractionResponseState> response =
				    AcpRemoteInteractionResponseFromJson(item, total_response_bytes);
				if (response.has_value())
					chat.remote_interaction_responses.push_back(std::move(*response));
			}
		}
		else
		{
			const std::string legacy_request_id = JsonStringOrEmpty(
			    root.Find(kChatRemoteInteractionResponseRequestIdField));
			const std::string legacy_response = JsonStringOrEmpty(
			    root.Find(kChatRemoteInteractionResponseJsonField));
			if (!legacy_request_id.empty() && legacy_request_id.size() <= 1024 &&
			    !legacy_response.empty() &&
			    legacy_response.size() <= kMaxPersistedRemoteInteractionResponseBytes &&
			    ParseJson(legacy_response).has_value())
				chat.remote_interaction_responses.push_back(
				    {legacy_request_id, legacy_response});
		}
		chat.remote_prompt_delivery_id = JsonStringOrEmpty(
		    root.Find(kChatRemotePromptDeliveryIdField));
		chat.remote_prompt_delivery_payload = JsonStringOrEmpty(
		    root.Find(kChatRemotePromptDeliveryPayloadField));
		chat.remote_prompt_delivery_session_id = JsonStringOrEmpty(
		    root.Find(kChatRemotePromptDeliverySessionIdField));
		if (chat.remote_prompt_delivery_session_id.size() > 256 ||
		    chat.remote_prompt_delivery_id.size() > 256 ||
		    chat.remote_prompt_delivery_payload.size() >
		        kMaxPersistedRemotePromptDeliveryBytes)
		{
			chat.remote_prompt_delivery_session_id.clear();
			chat.remote_prompt_delivery_id.clear();
			chat.remote_prompt_delivery_payload.clear();
		}
		if (const JsonValue* queued_prompts = uam::json::ArrayOrNull(
		        root.Find(kChatAcpQueuedPromptsField)); queued_prompts != nullptr)
		{
			std::size_t total_text_bytes = 0;
			for (const JsonValue& item : queued_prompts->array_value)
			{
				if (chat.acp_queued_prompts.size() >= kMaxPersistedAcpQueuedPrompts) break;
				std::optional<uam::AcpQueuedUserPromptState> prompt =
				    AcpQueuedPromptFromJson(item, total_text_bytes);
				if (prompt.has_value()) chat.acp_queued_prompts.push_back(std::move(*prompt));
			}
		}
		chat.acp_dispatched_queued_prompt_count = std::min<std::uintmax_t>(
		    NonNegativeUintmaxFieldOrZero(root.Find(kChatAcpDispatchedQueuedPromptCountField)),
		    chat.acp_queued_prompts.size());
		chat.parent_chat_id = JsonStringOrEmpty(root.Find(kChatParentChatIdField));
		chat.branch_root_chat_id = JsonStringOrEmpty(root.Find(kChatBranchRootChatIdField));
		chat.branch_from_message_index = IntFieldAtLeastOrDefault(root.Find(kChatBranchFromMessageIndexField), -1, -1);
		chat.branch_message_edited = JsonBoolOrDefault(root.Find(kChatBranchMessageEditedField), false);
		chat.folder_id = JsonStringOrEmpty(root.Find(kChatFolderIdField));
		std::erase_if(chat.folder_id, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
		chat.title = JsonStringOrEmpty(root.Find(kChatTitleField));
		chat.created_at = JsonStringOrEmpty(root.Find(kChatCreatedAtField));
		chat.updated_at = JsonStringOrEmpty(root.Find(kChatUpdatedAtField));
		chat.last_opened_at = JsonStringOrEmpty(root.Find(kChatLastOpenedAtField));
		chat.pinned = JsonBoolOrDefault(root.Find(kChatPinnedField), false);
		chat.linked_files = JsonStringArrayOrEmpty(root.Find(kChatLinkedFilesField));
		chat.workspace_directory = JsonStringOrEmpty(root.Find(kChatWorkspaceDirectoryField));
		chat.workspace_isolation_kind = JsonStringOrEmpty(root.Find(kChatWorkspaceIsolationKindField));
		chat.workspace_source_directory = JsonStringOrEmpty(root.Find(kChatWorkspaceSourceDirectoryField));
		chat.workspace_base_ref = JsonStringOrEmpty(root.Find(kChatWorkspaceBaseRefField));
		chat.workspace_branch_name = JsonStringOrEmpty(root.Find(kChatWorkspaceBranchNameField));
		chat.workspace_worktree_directory = JsonStringOrEmpty(root.Find(kChatWorkspaceWorktreeDirectoryField));
		chat.imported_read_only = JsonBoolOrDefault(root.Find(kChatImportedReadOnlyField), false);
		if (chat.imported_read_only ||
		    chat.execution_host_id == uam::execution_hosts::kLocalHostId)
		{
			chat.remote_turn_reconnect_pending = false;
			chat.remote_process_exists = false;
			chat.remote_stop_cleanup_pending = false;
			chat.remote_restart_pending = false;
			chat.remote_process_control_token.clear();
			chat.remote_delivered_stdout_cursor = 0;
			chat.remote_delivered_stderr_cursor = 0;
			chat.remote_source_exit_pending = false;
			chat.remote_source_exit_code = -1;
			chat.remote_uam_control_channel_id.clear();
			chat.remote_interaction_responses.clear();
			chat.remote_prompt_delivery_session_id.clear();
			chat.remote_prompt_delivery_id.clear();
			chat.remote_prompt_delivery_payload.clear();
			if (chat.imported_read_only) chat.acp_queued_prompts.clear();
			if (chat.imported_read_only) chat.acp_dispatched_queued_prompt_count = 0;
		}
		chat.approval_mode = JsonStringOrEmpty(root.Find(kChatApprovalModeField));
		chat.uam_agent_id = uam::strings::NonEmptyOrFallback(JsonStringOrEmpty(root.Find(kChatUamAgentIdField)), "build");
		chat.agent_run_id = JsonStringOrEmpty(root.Find(kChatAgentRunIdField));
		chat.goal_owner_chat_id = uam::strings::Trim(
		    JsonStringOrEmpty(root.Find(kChatGoalOwnerChatIdField)));
		chat.goal_iteration_goal_id = uam::strings::Trim(
		    JsonStringOrEmpty(root.Find(kChatGoalIterationGoalIdField)));
		chat.goal_iteration_turn_kind = JsonStringOrEmpty(root.Find(kChatGoalIterationTurnKindField));
		chat.goal_iteration_repair_attempts = IntFieldAtLeastOrDefault(root.Find(kChatGoalIterationRepairAttemptsField), 0, 0);
		const bool legacy_auto_approve_commands = JsonBoolOrDefault(root.Find(kChatAutoApproveCommandsField), false);
		const std::string persisted_command_safety_tier = JsonStringOrEmpty(root.Find(kChatCommandSafetyTierField));
		chat.command_safety_tier = uam::command_safety::NormalizeTier(
		    persisted_command_safety_tier.empty() && legacy_auto_approve_commands ? "yolo" : persisted_command_safety_tier);
		chat.computer_use_backend = uam::computer_use::BackendPreference(
		    JsonStringOrEmpty(root.Find(kChatComputerUseBackendField)));
		if (chat.approval_mode == uam::approval_modes::kLegacyYoloApprovalMode)
		{
			chat.approval_mode = uam::approval_modes::kDefaultApprovalMode;
			chat.command_safety_tier = "yolo";
		}
		else if (chat.approval_mode == uam::approval_modes::kAcceptEditsApprovalMode)
		{
			chat.approval_mode = uam::approval_modes::kDefaultApprovalMode;
			chat.command_safety_tier = uam::approval_modes::kAcceptEditsApprovalMode;
		}
		chat.model_id = JsonStringOrEmpty(root.Find(kChatModelIdField));
		chat.reviewer_model_id = JsonStringOrEmpty(root.Find(kChatReviewerModelIdField));
		chat.reasoning_effort = JsonStringOrEmpty(root.Find(kChatReasoningEffortField));
		chat.service_tier = JsonStringOrEmpty(root.Find(kChatServiceTierField));
		chat.service_tier_explicit = JsonBoolOrDefault(root.Find(kChatServiceTierExplicitField), !chat.service_tier.empty());
		chat.extra_flags = JsonStringOrEmpty(root.Find(kChatExtraFlagsField));
		chat.memory_enabled = JsonBoolOrDefault(root.Find(kChatMemoryEnabledField), true);
		chat.memory_level = uam::memory_levels::Normalize(JsonStringOrEmpty(root.Find(kChatMemoryLevelField)), chat.memory_enabled);
		chat.memory_enabled = uam::memory_levels::IsEnabled(chat.memory_level);
		chat.memory_last_processed_message_count = NonNegativeIntFieldOrZero(root.Find(kChatMemoryLastProcessedMessageCountField));
		chat.memory_last_processed_at = JsonStringOrEmpty(root.Find(kChatMemoryLastProcessedAtField));
		chat.small_model_mode = JsonBoolOrDefault(root.Find(kChatSmallModelModeField), false);
		chat.uam_control_enabled = JsonBoolOrDefault(root.Find("uamControlEnabled"), false);

		// Load goals array
		if (const JsonValue* goals_arr = uam::json::ArrayOrNull(root.Find("goals")); goals_arr != nullptr)
		{
			for (const auto& goal_obj : goals_arr->array_value)
			{
				if (goal_obj.type != JsonValue::Type::Object)
					continue;

				Goal goal;
				goal.id = JsonStringOrEmpty(goal_obj.Find("id"));
				goal.objective = JsonStringOrEmpty(goal_obj.Find("objective"));
				goal.status = GoalStatusFromString(JsonStringOrEmpty(goal_obj.Find("status")));
				goal.token_budget = NonNegativeInt64FieldOrZero(goal_obj.Find("tokenBudget"));
				goal.tokens_used = NonNegativeInt64FieldOrZero(goal_obj.Find("tokensUsed"));
				goal.blocked_turn_count = NonNegativeIntFieldOrZero(goal_obj.Find("blockedTurnCount"));
				goal.last_blocker = JsonStringOrEmpty(goal_obj.Find("lastBlocker"));
				goal.last_blocker_kind = JsonStringOrEmpty(goal_obj.Find("lastBlockerKind"));
				goal.last_diagnostic = JsonStringOrEmpty(goal_obj.Find("lastDiagnostic"));
				goal.completed_items = JsonStringArrayOrEmpty(goal_obj.Find("completedItems"));
				goal.remaining_items = JsonStringArrayOrEmpty(goal_obj.Find("remainingItems"));
				goal.current_step = JsonStringOrEmpty(goal_obj.Find("currentStep"));
				if (goal.status == GoalStatus::Complete)
				{
					goal.remaining_items.clear();
					goal.current_step.clear();
				}
				goal.last_verification = JsonStringOrEmpty(goal_obj.Find("lastVerification"));
				goal.last_next_prompt = JsonStringOrEmpty(goal_obj.Find("lastNextPrompt"));
				goal.same_next_prompt_count = NonNegativeIntFieldOrZero(goal_obj.Find("sameNextPromptCount"));
				goal.last_assistant_text = JsonStringOrEmpty(goal_obj.Find("lastAssistantText"));
				goal.same_assistant_text_count = NonNegativeIntFieldOrZero(goal_obj.Find("sameAssistantTextCount"));
				goal.loop_count = NonNegativeIntFieldOrZero(goal_obj.Find("loopCount"));
				goal.created_at = JsonStringOrEmpty(goal_obj.Find("createdAt"));
				goal.updated_at = JsonStringOrEmpty(goal_obj.Find("updatedAt"));
				goal.execution_owner = JsonStringOrEmpty(goal_obj.Find("executionOwner")) == "provider" ? "provider" : "uam";
				goal.provider_command = goal.execution_owner == "provider" ? JsonStringOrEmpty(goal_obj.Find("providerCommand")) : "";
				goal.worker_model_id = JsonStringOrEmpty(goal_obj.Find("workerModelId"));
				goal.reviewer_model_id = JsonStringOrEmpty(goal_obj.Find("reviewerModelId"));
				goal.creator = JsonStringOrEmpty(goal_obj.Find("creator")) == "model" ? "model" : "user";
				goal.creator_provider_id = goal.creator == "model" ? JsonStringOrEmpty(goal_obj.Find("creatorProviderId")) : "";
				goal.creator_agent_id = goal.creator == "model" ? JsonStringOrEmpty(goal_obj.Find("creatorAgentId")) : "";
				goal.creator_run_id = goal.creator == "model" ? JsonStringOrEmpty(goal_obj.Find("creatorRunId")) : "";
				goal.creator_request_key_hash = goal.creator == "model" ? JsonStringOrEmpty(goal_obj.Find("creatorRequestKeyHash")) : "";
				chat.goals.push_back(std::move(goal));
			}
		}
		if (const JsonValue* audit = uam::json::ArrayOrNull(root.Find("uamControlAudit")); audit != nullptr)
		{
			const std::size_t start = audit->array_value.size() > 64 ? audit->array_value.size() - 64 : 0;
			for (std::size_t index = start; index < audit->array_value.size(); ++index)
			{
				const JsonValue& value = audit->array_value[index];
				if (value.type != JsonValue::Type::Object) continue;
				chat.uam_control_audit.push_back({
				    .request_id = JsonStringOrEmpty(value.Find("requestId")),
				    .method = JsonStringOrEmpty(value.Find("method")),
				    .result = JsonStringOrEmpty(value.Find("result")),
				    .reason = JsonStringOrEmpty(value.Find("reason")),
				    .provider_id = JsonStringOrEmpty(value.Find("providerId")),
				    .agent_id = JsonStringOrEmpty(value.Find("agentId")),
				    .run_id = JsonStringOrEmpty(value.Find("runId")),
				    .created_at = JsonStringOrEmpty(value.Find("createdAt")),
				});
			}
		}

		// Load active_goal_id
		chat.active_goal_id = JsonStringOrEmpty(root.Find("activeGoalId"));

		ApplyChatTimestampFallbacks(chat);
		NormalizeLoadedNativeSessionId(chat);
		if (chat.branch_root_chat_id.empty())
		{
			chat.branch_root_chat_id = chat.id;
		}

		const JsonValue* messages_value = root.Find(kChatMessagesField);
		if (messages_value != nullptr && messages_value->type != JsonValue::Type::Array)
		{
			return {std::nullopt, "contains a non-array messages field"};
		}
		const JsonValue* msgs = uam::json::ArrayOrNull(messages_value);
		if (msgs != nullptr)
		{
			chat.persisted_message_count = msgs->array_value.size();
			if (include_messages)
			{
				for (const auto& m : msgs->array_value)
				{
					if (m.type == JsonValue::Type::Object)
					{
						chat.messages.push_back(JsonToMessage(m));
					}
				}
			}
		}
		else
		{
			chat.persisted_message_count = static_cast<std::size_t>(
			    NonNegativeUintmaxFieldOrZero(root.Find(kChatPersistedMessageCountField)));
		}

		chat.messages_loaded = include_messages;
		if (include_messages)
		{
			chat.persisted_message_count = chat.messages.size();
		}
		chat.persisted_messages_digest = JsonStringOrEmpty(root.Find(kChatPersistedMessagesDigestField));
		if (chat.persisted_messages_digest.empty())
		{
			chat.persisted_messages_digest = SummaryDigest(chat, chat.persisted_message_count);
		}

		if (!include_messages && expected_source_size == 0 && path.extension() == ".json")
		{
			root.object_value.erase(std::string(kChatMessagesField));
			uam::json::SetNumber(root, kChatPersistedMessageCountField, static_cast<double>(chat.persisted_message_count));
			uam::json::SetString(root, kChatPersistedMessagesDigestField, chat.persisted_messages_digest);
			uam::json::SetNumber(root, kChatSummarySourceSizeField, static_cast<double>(file_text.size()));
			(void)uam::io::WriteTextFile(SummaryCachePathForChatFile(path), SerializeJson(root));
		}

		return {std::move(chat), ""};
	}

	enum class UnloadedTranscriptSource
	{
		None,
		Primary,
		Backup,
		Unrecoverable,
	};

	struct UnloadedTranscriptSelection
	{
		UnloadedTranscriptSource source = UnloadedTranscriptSource::None;
		std::optional<ChatSession> chat;
	};

	bool HasTranscriptForUnloadedSave(const ChatSession& chat, std::size_t expected_message_count)
	{
		return expected_message_count == 0 || !chat.messages.empty();
	}

	UnloadedTranscriptSelection SelectTranscriptForUnloadedSave(
	    const fs::path& primary_path,
	    std::string_view expected_chat_id,
	    std::size_t expected_message_count)
	{
		const bool primary_exists = uam::paths::PathExistsNoThrow(primary_path);
		if (primary_exists)
		{
			LoadChatResult primary = ParseLocalChatFile(primary_path, true);
			if (primary.chat && primary.chat->id == expected_chat_id &&
			    HasTranscriptForUnloadedSave(*primary.chat, expected_message_count))
			{
				return {UnloadedTranscriptSource::Primary, std::move(primary.chat)};
			}
		}

		const fs::path backup_path = uam::io::MakeBackupPath(primary_path);
		const bool backup_exists = uam::paths::PathExistsNoThrow(backup_path);
		if (backup_exists)
		{
			LoadChatResult backup = ParseLocalChatFile(backup_path, true);
			if (backup.chat && backup.chat->id == expected_chat_id &&
			    HasTranscriptForUnloadedSave(*backup.chat, expected_message_count))
			{
				return {UnloadedTranscriptSource::Backup, std::move(backup.chat)};
			}
		}

		if (primary_exists || backup_exists || expected_message_count > 0)
		{
			return {UnloadedTranscriptSource::Unrecoverable, std::nullopt};
		}
		return {};
	}

	void AssignStringView(std::string& target, std::string_view value)
	{
		target.assign(value);
	}

	void ApplyLegacyChatMeta(ChatSession& chat, std::string_view key, std::string_view value)
	{
		const std::string_view normalized_key = uam::strings::TrimAsciiView(key);

		if (normalized_key == kLegacyProviderIdKey)
		{
			AssignStringView(chat.provider_id, value);
		}
		else if (normalized_key == kLegacyNativeSessionIdKey)
		{
			AssignStringView(chat.native_session_id, value);
		}
		else if (normalized_key == kLegacyParentChatKey)
		{
			AssignStringView(chat.parent_chat_id, value);
		}
		else if (normalized_key == kLegacyBranchRootKey)
		{
			AssignStringView(chat.branch_root_chat_id, value);
		}
		else if (normalized_key == kLegacyBranchFromIndexKey)
		{
			chat.branch_from_message_index = std::max(-1, uam::parse::IntOr(value, -1));
		}
		else if (normalized_key == kLegacyFolderKey)
		{
			AssignStringView(chat.folder_id, value);
		}
		else if (normalized_key == kLegacyTitleKey)
		{
			AssignStringView(chat.title, value);
		}
		else if (normalized_key == kLegacyCreatedAtKey)
		{
			AssignStringView(chat.created_at, value);
		}
		else if (normalized_key == kLegacyUpdatedAtKey)
		{
			AssignStringView(chat.updated_at, value);
		}
		else if (normalized_key == kLegacyLastOpenedAtKey)
		{
			AssignStringView(chat.last_opened_at, value);
		}
		else if (normalized_key == kLegacyLinkedFileKey)
		{
			const std::string linked_file = uam::strings::Trim(value);
			if (!linked_file.empty())
			{
				chat.linked_files.push_back(linked_file);
			}
		}
	}

	void ApplyChatTimestampFallbacks(ChatSession& chat)
	{
		if (chat.created_at.empty())
		{
			chat.created_at = uam::time::TimestampNow();
		}
		if (chat.updated_at.empty())
		{
			chat.updated_at = chat.created_at;
		}
		if (chat.last_opened_at.empty())
		{
			chat.last_opened_at = chat.updated_at;
		}
	}

} // namespace

bool ChatRepository::SaveChatImpl(const std::filesystem::path& data_root, const ChatSession& chat, bool fail_if_exists)
{
	static std::mutex save_mutex;
	std::lock_guard<std::mutex> lock(save_mutex);

	if (!uam::chat_ids::IsSafeStorageChatId(chat.id))
	{
		return false;
	}

	const fs::path file_path = AppPaths::UamChatFilePath(data_root, chat.id);
	if (fail_if_exists &&
	    (uam::paths::PathExistsNoThrow(file_path) ||
	     uam::paths::PathExistsNoThrow(uam::io::MakeBackupPath(file_path))))
	{
		return false;
	}

	std::error_code ec;
	if (!uam::paths::CreateDirectoriesNoThrow(file_path.parent_path(), &ec))
	{
		return false;
	}

	JsonValue root = uam::json::Object();
	uam::json::SetString(root, kChatIdField, chat.id);
	uam::json::SetString(root, kChatExecutionHostIdField,
	    uam::strings::NonEmptyOrFallback(chat.execution_host_id, "local"));
	uam::json::SetString(root, kChatProviderIdField, chat.provider_id);
	uam::json::SetString(root, kChatNativeSessionIdField, chat.native_session_id);
	uam::json::SetBool(root, kChatRemoteTurnReconnectPendingField,
	                   chat.remote_turn_reconnect_pending);
	uam::json::SetBool(root, kChatRemoteProcessExistsField,
	                   chat.remote_process_exists);
	uam::json::SetBool(root, kChatRemoteStopCleanupPendingField,
	                   chat.remote_stop_cleanup_pending);
	uam::json::SetBool(root, kChatRemoteRestartPendingField,
	                   chat.remote_restart_pending);
	uam::json::SetString(root, kChatRemoteProcessControlTokenField,
	                   chat.remote_process_control_token);
	uam::json::SetNumber(root, kChatRemoteDeliveredStdoutCursorField,
	                     static_cast<double>(chat.remote_delivered_stdout_cursor));
	uam::json::SetNumber(root, kChatRemoteDeliveredStderrCursorField,
	                     static_cast<double>(chat.remote_delivered_stderr_cursor));
	uam::json::SetBool(root, kChatRemoteSourceExitPendingField,
	                   chat.remote_source_exit_pending);
	uam::json::SetNumber(root, kChatRemoteSourceExitCodeField,
	                     static_cast<double>(chat.remote_source_exit_code));
	uam::json::SetString(root, kChatRemoteUamControlChannelIdField,
	                     chat.remote_uam_control_channel_id);
	JsonValue interaction_responses = uam::json::Array();
	std::size_t interaction_response_bytes = 0;
	for (const uam::AcpRemoteInteractionResponseState& response :
	     chat.remote_interaction_responses)
	{
		if (interaction_responses.array_value.size() >=
		        kMaxPersistedRemoteInteractionResponses ||
		    response.request_id_json.empty() ||
		    response.request_id_json.size() > 1024 || response.response_json.empty() ||
		    response.response_json.size() > kMaxPersistedRemoteInteractionResponseBytes ||
		    response.response_json.size() > kMaxPersistedRemoteInteractionResponsesBytes -
		        std::min(interaction_response_bytes,
		                 kMaxPersistedRemoteInteractionResponsesBytes))
			break;
		interaction_response_bytes += response.response_json.size();
		uam::json::PushValue(interaction_responses,
		                     AcpRemoteInteractionResponseToJson(response));
	}
	uam::json::SetValue(root, kChatRemoteInteractionResponsesField,
	                    std::move(interaction_responses));
	uam::json::SetString(root, kChatRemotePromptDeliveryIdField,
	                     chat.remote_prompt_delivery_id);
	uam::json::SetString(root, kChatRemotePromptDeliveryPayloadField,
	                     chat.remote_prompt_delivery_payload);
	uam::json::SetString(root, kChatRemotePromptDeliverySessionIdField,
	                     chat.remote_prompt_delivery_session_id);
	JsonValue queued_prompts = uam::json::Array();
	std::size_t queued_prompt_text_bytes = 0;
	for (const uam::AcpQueuedUserPromptState& prompt : chat.acp_queued_prompts)
	{
		if (queued_prompts.array_value.size() >= kMaxPersistedAcpQueuedPrompts ||
		    prompt.text.empty() || prompt.text.size() > kMaxPersistedAcpQueuedPromptBytes -
		    std::min(queued_prompt_text_bytes, kMaxPersistedAcpQueuedPromptBytes)) break;
		queued_prompt_text_bytes += prompt.text.size();
		uam::json::PushValue(queued_prompts, AcpQueuedPromptToJson(prompt));
	}
	uam::json::SetValue(root, kChatAcpQueuedPromptsField, std::move(queued_prompts));
	uam::json::SetNumber(root, kChatAcpDispatchedQueuedPromptCountField,
	                     static_cast<double>(std::min(
	                         chat.acp_dispatched_queued_prompt_count,
	                         chat.acp_queued_prompts.size())));
	uam::json::SetString(root, kChatParentChatIdField, chat.parent_chat_id);
	uam::json::SetString(root, kChatBranchRootChatIdField, chat.branch_root_chat_id);
	uam::json::SetNumber(root, kChatBranchFromMessageIndexField, static_cast<double>(chat.branch_from_message_index));
	uam::json::SetBool(root, kChatBranchMessageEditedField, chat.branch_message_edited);
	{
		std::string folder_id_sanitized = chat.folder_id;
		std::erase_if(folder_id_sanitized, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
		uam::json::SetString(root, kChatFolderIdField, std::move(folder_id_sanitized));
	}
	uam::json::SetString(root, kChatTitleField, chat.title);
	uam::json::SetString(root, kChatCreatedAtField, chat.created_at);
	uam::json::SetString(root, kChatUpdatedAtField, chat.updated_at);
	uam::json::SetString(root, kChatLastOpenedAtField, uam::strings::NonEmptyOrFallback(chat.last_opened_at, chat.updated_at));
	uam::json::SetBool(root, kChatPinnedField, chat.pinned);
	uam::json::SetValue(root, kChatLinkedFilesField, StringArrayToJson(chat.linked_files));
	uam::json::SetString(root, kChatWorkspaceDirectoryField, chat.workspace_directory);
	uam::json::SetString(root, kChatWorkspaceIsolationKindField, chat.workspace_isolation_kind);
	uam::json::SetString(root, kChatWorkspaceSourceDirectoryField, chat.workspace_source_directory);
	uam::json::SetString(root, kChatWorkspaceBaseRefField, chat.workspace_base_ref);
	uam::json::SetString(root, kChatWorkspaceBranchNameField, chat.workspace_branch_name);
	uam::json::SetString(root, kChatWorkspaceWorktreeDirectoryField, chat.workspace_worktree_directory);
	uam::json::SetBool(root, kChatImportedReadOnlyField, chat.imported_read_only);
	uam::json::SetString(root, kChatApprovalModeField, chat.approval_mode);
	uam::json::SetString(root, kChatUamAgentIdField, uam::strings::NonEmptyOrFallback(chat.uam_agent_id, "build"));
	uam::json::SetString(root, kChatAgentRunIdField, chat.agent_run_id);
	uam::json::SetString(root, kChatGoalOwnerChatIdField, chat.goal_owner_chat_id);
	uam::json::SetString(root, kChatGoalIterationGoalIdField, chat.goal_iteration_goal_id);
	uam::json::SetString(root, kChatGoalIterationTurnKindField, chat.goal_iteration_turn_kind);
	uam::json::SetNumber(root, kChatGoalIterationRepairAttemptsField, static_cast<double>(chat.goal_iteration_repair_attempts));
	uam::json::SetString(root, kChatCommandSafetyTierField, uam::command_safety::NormalizeTier(chat.command_safety_tier));
	uam::json::SetString(root, kChatComputerUseBackendField,
	    uam::computer_use::BackendPreference(chat.computer_use_backend));
	uam::json::SetString(root, kChatModelIdField, chat.model_id);
	uam::json::SetString(root, kChatReviewerModelIdField, chat.reviewer_model_id);
	uam::json::SetString(root, kChatReasoningEffortField, chat.reasoning_effort);
	uam::json::SetString(root, kChatServiceTierField, chat.service_tier);
	uam::json::SetBool(root, kChatServiceTierExplicitField, chat.service_tier_explicit);
	uam::json::SetString(root, kChatExtraFlagsField, chat.extra_flags);
	const std::string memory_level = uam::memory_levels::Normalize(chat.memory_level, chat.memory_enabled);
	uam::json::SetString(root, kChatMemoryLevelField, memory_level);
	uam::json::SetBool(root, kChatMemoryEnabledField, uam::memory_levels::IsEnabled(memory_level));
	uam::json::SetNumber(root, kChatMemoryLastProcessedMessageCountField, static_cast<double>(chat.memory_last_processed_message_count));
	uam::json::SetString(root, kChatMemoryLastProcessedAtField, chat.memory_last_processed_at);
	uam::json::SetBool(root, kChatSmallModelModeField, chat.small_model_mode);
	uam::json::SetBool(root, "uamControlEnabled", chat.uam_control_enabled);

	std::size_t persisted_message_count = chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count;
	std::string persisted_messages_digest = chat.persisted_messages_digest;
	bool preserve_existing_primary_as_backup = true;
	if (chat.messages_loaded && !chat.messages.empty())
	{
		JsonValue msgs = uam::json::Array();
		for (const auto& m : chat.messages)
		{
			uam::json::PushValue(msgs, MessageToJson(m));
		}
		uam::json::SetValue(root, kChatMessagesField, std::move(msgs));
	}
	else if (!chat.messages_loaded)
	{
		UnloadedTranscriptSelection transcript =
		    SelectTranscriptForUnloadedSave(file_path, chat.id, chat.persisted_message_count);
		if (transcript.source == UnloadedTranscriptSource::Unrecoverable)
		{
			return false;
		}
		if (transcript.chat)
		{
			JsonValue msgs = uam::json::Array();
			for (const Message& message : transcript.chat->messages)
			{
				uam::json::PushValue(msgs, MessageToJson(message));
			}
			uam::json::SetValue(root, kChatMessagesField, std::move(msgs));
			persisted_message_count = transcript.chat->messages.size();
			persisted_messages_digest = SummaryDigest(chat, persisted_message_count);
			preserve_existing_primary_as_backup = transcript.source != UnloadedTranscriptSource::Backup;
		}
	}

	// Serialize goals array
	if (!chat.goals.empty())
	{
		JsonValue goals_arr = uam::json::Array();
		for (const auto& goal : chat.goals)
		{
			JsonValue goal_obj = uam::json::Object();
			uam::json::SetString(goal_obj, "id", goal.id);
			uam::json::SetString(goal_obj, "objective", goal.objective);
			uam::json::SetString(goal_obj, "status", GoalStatusToString(goal.status));
			uam::json::SetNumber(goal_obj, "tokenBudget", static_cast<double>(goal.token_budget));
			uam::json::SetNumber(goal_obj, "tokensUsed", static_cast<double>(goal.tokens_used));
			uam::json::SetNumber(goal_obj, "blockedTurnCount", static_cast<double>(goal.blocked_turn_count));
			uam::json::SetString(goal_obj, "lastBlocker", goal.last_blocker);
			uam::json::SetString(goal_obj, "lastBlockerKind", goal.last_blocker_kind);
			uam::json::SetString(goal_obj, "lastDiagnostic", goal.last_diagnostic);
			uam::json::SetValue(goal_obj, "completedItems", StringArrayToJson(goal.completed_items));
			uam::json::SetValue(goal_obj, "remainingItems", StringArrayToJson(goal.remaining_items));
			uam::json::SetString(goal_obj, "currentStep", goal.current_step);
			uam::json::SetString(goal_obj, "lastVerification", goal.last_verification);
			uam::json::SetString(goal_obj, "lastNextPrompt", goal.last_next_prompt);
			uam::json::SetNumber(goal_obj, "sameNextPromptCount", static_cast<double>(goal.same_next_prompt_count));
			uam::json::SetString(goal_obj, "lastAssistantText", goal.last_assistant_text);
			uam::json::SetNumber(goal_obj, "sameAssistantTextCount", static_cast<double>(goal.same_assistant_text_count));
			uam::json::SetNumber(goal_obj, "loopCount", static_cast<double>(goal.loop_count));
			uam::json::SetString(goal_obj, "createdAt", goal.created_at);
			uam::json::SetString(goal_obj, "updatedAt", goal.updated_at);
			uam::json::SetString(goal_obj, "executionOwner", goal.execution_owner == "provider" ? "provider" : "uam");
			uam::json::SetString(goal_obj, "providerCommand", goal.execution_owner == "provider" ? goal.provider_command : "");
			uam::json::SetString(goal_obj, "workerModelId", goal.worker_model_id);
			uam::json::SetString(goal_obj, "reviewerModelId", goal.reviewer_model_id);
			uam::json::SetString(goal_obj, "creator", goal.creator == "model" ? "model" : "user");
			uam::json::SetString(goal_obj, "creatorProviderId", goal.creator == "model" ? goal.creator_provider_id : "");
			uam::json::SetString(goal_obj, "creatorAgentId", goal.creator == "model" ? goal.creator_agent_id : "");
			uam::json::SetString(goal_obj, "creatorRunId", goal.creator == "model" ? goal.creator_run_id : "");
			uam::json::SetString(goal_obj, "creatorRequestKeyHash", goal.creator == "model" ? goal.creator_request_key_hash : "");
			uam::json::PushValue(goals_arr, std::move(goal_obj));
		}
		uam::json::SetValue(root, "goals", std::move(goals_arr));
	}
	if (!chat.uam_control_audit.empty())
	{
		JsonValue audit = uam::json::Array();
		const std::size_t start = chat.uam_control_audit.size() > 64 ? chat.uam_control_audit.size() - 64 : 0;
		for (std::size_t index = start; index < chat.uam_control_audit.size(); ++index)
		{
			const UamControlAuditRecord& record = chat.uam_control_audit[index];
			JsonValue value = uam::json::Object();
			uam::json::SetString(value, "requestId", record.request_id);
			uam::json::SetString(value, "method", record.method);
			uam::json::SetString(value, "result", record.result);
			uam::json::SetString(value, "reason", record.reason);
			uam::json::SetString(value, "providerId", record.provider_id);
			uam::json::SetString(value, "agentId", record.agent_id);
			uam::json::SetString(value, "runId", record.run_id);
			uam::json::SetString(value, "createdAt", record.created_at);
			uam::json::PushValue(audit, std::move(value));
		}
		uam::json::SetValue(root, "uamControlAudit", std::move(audit));
	}

	// Serialize active_goal_id
	if (!chat.active_goal_id.empty())
	{
		uam::json::SetString(root, "activeGoalId", chat.active_goal_id);
	}

	const std::string json = SerializeJson(root);
	const bool chat_saved = preserve_existing_primary_as_backup
	    ? uam::io::WriteTextFileWithBackup(file_path, json)
	    : uam::io::WriteTextFile(file_path, json);
	if (!chat_saved)
	{
		return false;
	}

	root.object_value.erase(std::string(kChatMessagesField));
	uam::json::SetNumber(root, kChatPersistedMessageCountField, static_cast<double>(persisted_message_count));
	uam::json::SetString(root, kChatPersistedMessagesDigestField,
	                     persisted_messages_digest.empty() ? SummaryDigest(chat, persisted_message_count) : persisted_messages_digest);
	uam::json::SetNumber(root, kChatSummarySourceSizeField, static_cast<double>(json.size()));
	(void)uam::io::WriteTextFile(AppPaths::UamChatSummaryFilePath(data_root, chat.id), SerializeJson(root));
	return true;
}

bool ChatRepository::SaveChat(const std::filesystem::path& data_root, const ChatSession& chat)
{
	return SaveChatImpl(data_root, chat, false);
}

bool ChatRepository::SaveChatIfAbsent(const std::filesystem::path& data_root, const ChatSession& chat)
{
	return SaveChatImpl(data_root, chat, true);
}

ChatStorageDeleteResult ChatRepository::DeleteChatStorageFiles(const std::filesystem::path& data_root, std::string_view chat_id)
{
	ChatStorageDeleteResult result;
	if (!uam::chat_ids::IsSafeStorageChatId(chat_id))
	{
		result.unsafe_chat_id = true;
		return result;
	}

	uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(AppPaths::ChatPath(data_root, chat_id), &result.legacy_directory_error);
	const fs::path metadata_path = AppPaths::UamChatFilePath(data_root, chat_id);
	uam::paths::RemoveFileNoThrow(metadata_path, &result.metadata_file_error);
	uam::paths::RemoveFileNoThrow(uam::io::MakeBackupPath(metadata_path), &result.metadata_backup_file_error);
	const fs::path summary_path = AppPaths::UamChatSummaryFilePath(data_root, chat_id);
	uam::paths::RemoveFileNoThrow(summary_path, &result.summary_file_error);
	uam::paths::RemoveFileNoThrow(uam::io::MakeBackupPath(summary_path), &result.summary_backup_file_error);
	uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(
	    data_root / "computer-use" / std::string(chat_id), &result.computer_use_directory_error);
	return result;
}

ChatSession LoadLegacyChatFromDirectory(const fs::path& chat_root)
{
	ChatSession chat;
	chat.id = chat_root.filename().string();
	chat.title = "Imported Chat";

	const fs::path meta_file = chat_root / "meta.txt";

	if (uam::paths::PathExistsNoThrow(meta_file))
	{
		std::istringstream lines(uam::io::ReadTextFile(meta_file));
		std::string line;

		while (std::getline(lines, line))
		{
			const std::string_view trimmed_line = StripCarriageReturn(line);

			const auto equals_at = trimmed_line.find('=');
			if (equals_at == std::string_view::npos)
			{
				continue;
			}

			const std::string_view key = trimmed_line.substr(0, equals_at);
			const std::string_view value = trimmed_line.substr(equals_at + 1);
			ApplyLegacyChatMeta(chat, key, value);
		}
	}

	ApplyChatTimestampFallbacks(chat);
	NormalizeLoadedNativeSessionId(chat);

	if (chat.branch_root_chat_id.empty())
	{
		chat.branch_root_chat_id = chat.id;
	}

	const fs::path messages_dir = chat_root / "messages";
	if (uam::paths::IsDirectoryNoThrow(messages_dir))
	{
		std::vector<fs::path> message_files;
		std::error_code ec;
		for (fs::directory_iterator it(messages_dir, ec), end; !ec && it != end; it.increment(ec))
		{
			const fs::directory_entry& file = *it;
			if (uam::paths::IsRegularFileWithExtensionNoThrow(file, ".txt"))
			{
				message_files.push_back(file.path());
			}
		}

		std::ranges::sort(message_files);

		for (const auto& message_file : message_files)
		{
			Message message;
			message.role = ParseLegacyMessageRoleFromFilename(message_file);
			message.content = uam::io::ReadTextFile(message_file);
			message.created_at = chat.updated_at;
			chat.messages.push_back(std::move(message));
		}
	}

	chat.messages_loaded = true;
	chat.persisted_message_count = chat.messages.size();
	chat.persisted_messages_digest = SummaryDigest(chat, chat.persisted_message_count);
	return chat;
}

namespace
{
	void SetWarning(std::string* warning_out, std::string warning)
	{
		if (warning_out != nullptr)
		{
			*warning_out = std::move(warning);
		}
	}

	void AppendWarning(std::string* warning_out, const std::string& warning)
	{
		if (warning_out == nullptr || warning.empty())
		{
			return;
		}

		if (!warning_out->empty())
		{
			warning_out->push_back('\n');
		}

		warning_out->append(warning);
	}

	bool ChatIdWasMigrated(const std::unordered_set<std::string>& migrated_chat_ids, const std::string& chat_id)
	{
		return migrated_chat_ids.contains(chat_id);
	}

	bool TryMigrateLegacyChatDirectory(const fs::path& data_root, const fs::directory_entry& folder, std::vector<ChatSession>& chats, std::unordered_set<std::string>& migrated_chat_ids, std::string* warning_out)
	{
		const std::string chat_id = folder.path().filename().string();
		const fs::path migrated_chat_file = AppPaths::UamChatFilePath(data_root, chat_id);

		if (uam::paths::PathExistsNoThrow(migrated_chat_file))
		{
			return false;
		}

		ChatSession chat = LoadLegacyChatFromDirectory(folder.path());
		if (chat.id.empty())
		{
			return false;
		}

		if (!ChatRepository::SaveChat(data_root, chat))
		{
			AppendWarning(warning_out, "Failed to migrate legacy chat folder: " + folder.path().string());
			return true;
		}

		chats.push_back(chat);
		migrated_chat_ids.insert(chat.id);
		return true;
	}

	void CarrySummaryFieldsIntoHydratedChat(ChatSession& hydrated, const ChatSession& summary)
	{
		hydrated.execution_host_id = summary.execution_host_id;
		hydrated.folder_id = summary.folder_id;
		hydrated.title = summary.title;
		hydrated.pinned = summary.pinned;
		hydrated.workspace_directory = summary.workspace_directory;
		hydrated.workspace_isolation_kind = summary.workspace_isolation_kind;
		hydrated.workspace_source_directory = summary.workspace_source_directory;
		hydrated.workspace_base_ref = summary.workspace_base_ref;
		hydrated.workspace_branch_name = summary.workspace_branch_name;
		hydrated.workspace_worktree_directory = summary.workspace_worktree_directory;
		hydrated.imported_read_only = summary.imported_read_only;
		hydrated.approval_mode = summary.approval_mode;
		hydrated.uam_agent_id = summary.uam_agent_id;
		hydrated.agent_run_id = summary.agent_run_id;
		hydrated.goal_owner_chat_id = summary.goal_owner_chat_id;
		hydrated.goal_iteration_goal_id = summary.goal_iteration_goal_id;
		hydrated.goal_iteration_turn_kind = summary.goal_iteration_turn_kind;
		hydrated.goal_iteration_repair_attempts = summary.goal_iteration_repair_attempts;
		hydrated.remote_turn_reconnect_pending = summary.remote_turn_reconnect_pending;
		hydrated.remote_process_exists = summary.remote_process_exists;
		hydrated.remote_stop_cleanup_pending = summary.remote_stop_cleanup_pending;
		hydrated.remote_restart_pending = summary.remote_restart_pending;
		hydrated.remote_process_control_token = summary.remote_process_control_token;
		hydrated.remote_delivered_stdout_cursor = summary.remote_delivered_stdout_cursor;
		hydrated.remote_delivered_stderr_cursor = summary.remote_delivered_stderr_cursor;
		hydrated.remote_source_exit_pending = summary.remote_source_exit_pending;
		hydrated.remote_source_exit_code = summary.remote_source_exit_code;
		hydrated.remote_uam_control_channel_id = summary.remote_uam_control_channel_id;
		hydrated.remote_interaction_responses = summary.remote_interaction_responses;
		hydrated.remote_prompt_delivery_session_id = summary.remote_prompt_delivery_session_id;
		hydrated.remote_prompt_delivery_id = summary.remote_prompt_delivery_id;
		hydrated.remote_prompt_delivery_payload = summary.remote_prompt_delivery_payload;
		hydrated.acp_queued_prompts = summary.acp_queued_prompts;
		hydrated.acp_dispatched_queued_prompt_count =
		    summary.acp_dispatched_queued_prompt_count;
		hydrated.active_goal_id = summary.active_goal_id;
		hydrated.goals = summary.goals;
		hydrated.command_safety_tier = summary.command_safety_tier;
		hydrated.computer_use_enabled = summary.computer_use_enabled;
		hydrated.computer_use_backend = summary.computer_use_backend;
		hydrated.computer_use_target_kind = summary.computer_use_target_kind;
		hydrated.computer_use_target_id = summary.computer_use_target_id;
		hydrated.computer_use_target_process_id = summary.computer_use_target_process_id;
		hydrated.computer_use_target_title = summary.computer_use_target_title;
		hydrated.computer_use_target_input_mode = summary.computer_use_target_input_mode;
		hydrated.model_id = summary.model_id;
		hydrated.reviewer_model_id = summary.reviewer_model_id;
		hydrated.reasoning_effort = summary.reasoning_effort;
		hydrated.service_tier = summary.service_tier;
		hydrated.service_tier_explicit = summary.service_tier_explicit;
		hydrated.extra_flags = summary.extra_flags;
		hydrated.memory_level = summary.memory_level;
		hydrated.memory_enabled = summary.memory_enabled;
		hydrated.memory_last_processed_message_count = summary.memory_last_processed_message_count;
		hydrated.memory_last_processed_at = summary.memory_last_processed_at;
		hydrated.small_model_mode = summary.small_model_mode;
	}

	ChatSession BuildRecoveredChatFromBackup(const fs::path& backup_path, const LoadChatResult& backup_chat, bool include_messages, const std::string& recovered_id)
	{
		ChatSession recovered = *backup_chat.chat;

		if (!include_messages)
		{
			recovered = ParseLocalChatFile(backup_path, true).chat.value_or(recovered);
		}

		const std::string previous_id = backup_chat.chat->id;
		recovered.id = recovered_id;
		NormalizeLoadedNativeSessionId(recovered);

		if (recovered.branch_root_chat_id.empty() || recovered.branch_root_chat_id == previous_id)
		{
			recovered.branch_root_chat_id = recovered.id;
		}

		return recovered;
	}

	void RecoverChatFromBackup(const fs::path& data_root,
	                              const fs::path& backup_path,
	                              const fs::path& restored_primary_path,
	                              bool include_messages,
	                              const std::unordered_set<std::string>& migrated_chat_ids,
	                              std::vector<ChatSession>& chats,
	                              std::string* warning_out)
	{
		const LoadChatResult backup_chat = ParseLocalChatFile(backup_path, include_messages);
		if (!backup_chat.chat)
		{
			AppendWarning(warning_out, "Skipped corrupted backup file " + backup_path.string() + ": " + backup_chat.error + ".");
			return;
		}

		if (ChatIdWasMigrated(migrated_chat_ids, backup_chat.chat->id))
		{
			return;
		}

		ChatSession recovered = BuildRecoveredChatFromBackup(backup_path, backup_chat, include_messages, restored_primary_path.stem().string());

		if (ChatIdWasMigrated(migrated_chat_ids, recovered.id))
		{
			return;
		}

		if (!ChatRepository::SaveChat(data_root, recovered))
		{
			AppendWarning(warning_out, "Recovered backup file " + backup_path.string() + ", but failed to save " + restored_primary_path.string() + ".");
			return;
		}

		chats.push_back(recovered);
	}

	std::vector<ChatSession> LoadLocalChatsImpl(const std::filesystem::path& data_root, bool include_messages, std::string* warning_out)
	{
		std::vector<ChatSession> chats;
		std::unordered_set<std::string> migrated_chat_ids;
		if (warning_out != nullptr)
		{
			warning_out->clear();
		}

		const fs::path old_chats_root = AppPaths::ChatsRootPath(data_root);
		if (uam::paths::IsDirectoryNoThrow(old_chats_root))
		{
			std::error_code ec;
			for (fs::directory_iterator it(old_chats_root, ec), end; !ec && it != end; it.increment(ec))
			{
				const fs::directory_entry& folder = *it;
				if (!uam::paths::IsDirectoryEntryNoThrow(folder))
				{
					continue;
				}

				if (TryMigrateLegacyChatDirectory(data_root, folder, chats, migrated_chat_ids, warning_out))
				{
					continue;
				}
			}
		}

		const fs::path chats_root = AppPaths::UamChatsRootPath(data_root);

		if (!uam::paths::IsDirectoryNoThrow(chats_root))
		{
			return chats;
		}

		std::error_code ec;

		for (fs::directory_iterator it(chats_root, ec), end; !ec && it != end; it.increment(ec))
		{
			const fs::directory_entry& entry = *it;
			if (!uam::paths::IsRegularFileWithExtensionNoThrow(entry, ".json"))
			{
				continue;
			}

			LoadChatResult primary_chat;
			if (!include_messages)
			{
				const fs::path summary_path = AppPaths::UamChatSummaryFilePath(data_root, entry.path().stem().string());
				std::error_code size_error;
				const std::uintmax_t source_size = fs::file_size(entry.path(), size_error);
				if (!size_error && SummaryCacheIsCurrent(entry.path(), summary_path))
				{
					primary_chat = ParseLocalChatFile(summary_path, false, source_size);
				}
			}
			if (!primary_chat.chat)
			{
				primary_chat = ParseLocalChatFile(entry.path(), include_messages);
			}
			if (primary_chat.chat)
			{
				const ChatSession& primary = *primary_chat.chat;
				if (ChatIdWasMigrated(migrated_chat_ids, primary.id))
				{
					continue;
				}

				chats.push_back(primary);
				continue;
			}

			const fs::path backup_path = uam::io::MakeBackupPath(entry.path());
			if (uam::paths::PathExistsNoThrow(backup_path))
			{
				RecoverChatFromBackup(data_root, backup_path, entry.path(), include_messages, migrated_chat_ids, chats, warning_out);
			}
			else if (!primary_chat.error.empty())
			{
				AppendWarning(warning_out, "Skipped malformed chat file " + entry.path().string() + ": " + primary_chat.error + ".");
			}
		}

		ec.clear();
		for (fs::directory_iterator it(chats_root, ec), end; !ec && it != end; it.increment(ec))
		{
			const fs::directory_entry& entry = *it;
			if (!uam::paths::IsRegularFileWithExtensionNoThrow(entry, ".bak"))
			{
				continue;
			}

			const fs::path primary_path = entry.path().parent_path() / entry.path().stem();
			if (uam::paths::PathExistsNoThrow(primary_path))
			{
				continue;
			}

			RecoverChatFromBackup(data_root, entry.path(), primary_path, include_messages, migrated_chat_ids, chats, warning_out);
		}

		std::ranges::sort(chats, ChatUpdatedNewestFirst);
		return chats;
	}
} // namespace

std::vector<ChatSession> ChatRepository::LoadLocalChats(const std::filesystem::path& data_root, std::string* warning_out)
{
	return LoadLocalChatsImpl(data_root, true, warning_out);
}

std::vector<ChatSession> ChatRepository::LoadLocalChatSummaries(const std::filesystem::path& data_root, std::string* warning_out)
{
	return LoadLocalChatsImpl(data_root, false, warning_out);
}

std::optional<ChatSession> ChatRepository::LoadLocalChat(const std::filesystem::path& data_root, std::string_view chat_id, bool include_messages, std::string* warning_out)
{
	SetWarning(warning_out, "");
	if (!uam::chat_ids::IsSafeStorageChatId(chat_id))
	{
		SetWarning(warning_out, "contains an unsafe chat id");
		return std::nullopt;
	}

	const fs::path chat_path = AppPaths::UamChatFilePath(data_root, chat_id);
	LoadChatResult loaded;
	if (!include_messages)
	{
		const fs::path summary_path = AppPaths::UamChatSummaryFilePath(data_root, chat_id);
		std::error_code size_error;
		const std::uintmax_t source_size = fs::file_size(chat_path, size_error);
		if (!size_error && SummaryCacheIsCurrent(chat_path, summary_path))
		{
			loaded = ParseLocalChatFile(summary_path, false, source_size);
		}
	}
	if (!loaded.chat)
	{
		loaded = ParseLocalChatFile(chat_path, include_messages);
	}
	if (!loaded.chat)
	{
		SetWarning(warning_out, loaded.error);
	}
	return std::move(loaded.chat);
}

bool ChatRepository::HydrateChatMessages(const std::filesystem::path& data_root, ChatSession& chat, std::string* warning_out)
{
	SetWarning(warning_out, "");

	if (chat.messages_loaded)
	{
		return true;
	}

	if (!uam::chat_ids::IsSafeStorageChatId(chat.id))
	{
		SetWarning(warning_out, "contains an unsafe chat id");
		return false;
	}

	LoadChatResult loaded = ParseLocalChatFile(AppPaths::UamChatFilePath(data_root, chat.id), true);
	if (!loaded.chat)
	{
		SetWarning(warning_out, loaded.error);
		return false;
	}

	ChatSession hydrated = *loaded.chat;
	CarrySummaryFieldsIntoHydratedChat(hydrated, chat);
	chat = std::move(hydrated);
	return true;
}
