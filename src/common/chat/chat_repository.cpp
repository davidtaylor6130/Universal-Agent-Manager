#include "common/chat/managed_parent_input_history.h"
#include "common/chat/acp_prompt_recovery.h"
#include "common/chat/chat_message_page_index.h"
#include "common/chat/chat_repository.h"
#include "common/chat/empty_held_queue_recovery.h"
#include "common/runtime/native_refresh_lifetime.h"

#include "common/chat/chat_ids.h"
#include "common/chat/passive_archive_json.h"
#include "common/chat/native_chat_identity.h"
#include "common/chat/message_attachment_json.h"
#include "common/chat/message_attachment_sources.h"
#include "common/config/approval_modes.h"
#include "common/config/custom_icon.h"
#include "common/config/execution_host_config.h"
#include "common/memory/memory_levels.h"
#include "common/security/command_safety.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_incremental_sha256.h"
#include "common/platform/published_file_change_witness.h"
#include "common/platform/platform_services.h"
#include <stdexcept>
#include "common/provider/provider_runtime.h"
#include "common/provider/native_session_storage_normalization.h"
#include "common/provider/provider_ids.h"
#include "common/provider/codex_thread_id.h"
#include "common/provider/native_agent_history.h"
#include "common/runtime/json_runtime.h"
#include "common/utils/io_utils.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/parse_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "common/utils/uuid.h"
#include "computer_use/computer_use_mcp_config.h"
#include "core/chat_import_utils.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <fstream>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace
{
	namespace fs = std::filesystem;
	namespace attachment_fields = uam::message_attachment_json;
	namespace attachment_persisted_fields = uam::message_attachment_json::persisted;
	// Refuse oversized saves before allocating the output or changing existing files.
	constexpr std::size_t kMaxChatJsonBytes = 256U * 1024U * 1024U;
	constexpr std::size_t kMaxQueueJournalBytes = 16U * 1024U * 1024U;
	constexpr std::size_t kMaxPassiveArchiveBytes = 16U * 1024U * 1024U;
	constexpr std::size_t kMaxPassiveNativeMetadataBytes = 1024U * 1024U;
	constexpr std::size_t kMaxPassiveMetadataReadBytes = 32U * 1024U * 1024U;
	constexpr std::string_view kQueueRevisionField = "acp_queue_storage_revision";
	constexpr std::string_view kQueueFullRevisionField = "acp_queue_full_save_revision";
	std::optional<std::string> StorageRevision(const fs::path& path);
	std::mutex& ChatSaveMutex();

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
	constexpr std::string_view kMessageContinuesTurnField = "continues_turn";
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
	constexpr std::string_view kToolCallSubAgentChildrenField = "sub_agent_children";
	constexpr std::string_view kToolCallSubAgentChildrenAuthoritativeField = "sub_agent_children_authoritative";

	constexpr std::string_view kMessageBlockTypeField = "type";
	constexpr std::string_view kMessageBlockTextField = "text";
	constexpr std::string_view kMessageBlockToolCallIdField = "tool_call_id";
	constexpr std::string_view kMessageBlockRequestIdField = "request_id";

	constexpr std::string_view kChatIdField = "id";
	constexpr std::string_view kChatExecutionHostIdField = "execution_host_id";
	constexpr std::string_view kChatProviderIdField = "provider_id";
	constexpr std::string_view kChatNativeSessionIdField = "native_session_id";
	constexpr std::string_view kChatOpenCodeV2SessionIdField = "opencode_v2_session_id";
	constexpr std::string_view kChatRemoteClaudeSessionUnstartedField = "remote_claude_session_unstarted";
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
	constexpr std::string_view kChatAcpQueueHeldField = "acp_queue_held";
	constexpr std::string_view kChatAcpDispatchedQueuedPromptCountField = "acp_dispatched_queued_prompt_count";
	constexpr std::size_t kMaxPersistedAcpQueuedPrompts = 32;
	constexpr std::size_t kMaxPersistedAcpQueuedPromptBytes = 2U * 1024U * 1024U;
	constexpr std::size_t kMaxPersistedRemoteInteractionResponseBytes = 1024U * 1024U;
	constexpr std::size_t kMaxPersistedRemoteInteractionResponses = 64;
	constexpr std::size_t kMaxPersistedRemoteInteractionResponsesBytes = 2U * 1024U * 1024U;
	constexpr std::string_view kChatRemotePendingRequestsField = "remote_pending_requests";
	constexpr std::string_view kChatRemoteNextRequestIdField = "remote_next_request_id";
	constexpr std::string_view kChatRemoteActiveTurnIdField = "remote_active_turn_id";
	constexpr std::string_view kChatRemoteTurnSerialField = "remote_turn_serial";
	constexpr std::string_view kChatRemoteTurnUserMessageIndexField = "remote_turn_user_message_index";
	constexpr std::size_t kMaxPersistedRemotePendingRequests = 64;
	constexpr std::size_t kMaxPersistedRemotePromptDeliveryBytes = 2U * 1024U * 1024U;
	constexpr std::string_view kChatParentChatIdField = "parent_chat_id";
	constexpr std::string_view kChatBranchRootChatIdField = "branch_root_chat_id";
	constexpr std::string_view kChatSubagentParentChatIdField = "subagent_parent_chat_id";
	constexpr std::string_view kChatSubagentParentCreatedAtField = "subagent_parent_created_at";
	constexpr std::string_view kChatBranchFromMessageIndexField = "branch_from_message_index";
	constexpr std::string_view kChatBranchMessageEditedField = "branch_message_edited";
	constexpr std::string_view kChatBranchOperationIdField = "branch_operation_id";
	constexpr std::string_view kChatBranchRetryErrorField = "branch_retry_error";
	constexpr std::string_view kChatFolderIdField = "folder_id";
	constexpr std::string_view kChatTitleField = "title";
	constexpr std::string_view kChatCustomIconField = "custom_icon";
	constexpr std::string_view kChatCreatedAtField = "created_at";
	constexpr std::string_view kChatUpdatedAtField = "updated_at";
	constexpr std::string_view kChatLastOpenedAtField = "last_opened_at";
	constexpr std::string_view kChatPinnedField = "pinned";
	constexpr std::string_view kChatLinkedFilesField = "linked_files";
	constexpr std::string_view kChatWorkspaceDirectoryField = "workspace_directory";
	constexpr std::string_view kChatWorkspaceIsolationKindField = "workspace_isolation_kind";
	constexpr std::string_view kChatWorkspaceSourceDirectoryField = "workspace_source_directory";
	constexpr std::string_view kChatWorkspaceBaseRefField = "workspace_base_ref";
	constexpr std::string_view kChatGitBaseBranchOverrideField = "git_base_branch_override";
	constexpr std::string_view kChatWorkspaceBranchNameField = "workspace_branch_name";
	constexpr std::string_view kChatWorkspaceWorktreeDirectoryField = "workspace_worktree_directory";
	constexpr std::string_view kChatSvnUpdatePendingField = "svn_update_pending";
	constexpr std::string_view kChatImportedReadOnlyField = "imported_read_only";
	constexpr std::string_view kChatApprovalModeField = "approval_mode";
	constexpr std::string_view kChatUamAgentIdField = "uam_agent_id";
	constexpr std::string_view kChatLastPromptAgentDefinitionHashField = "last_prompt_agent_definition_hash";
	constexpr std::string_view kChatAgentRunIdField = "agent_run_id";
	constexpr std::string_view kChatManagedFollowupBirthField = "managed_followup_incarnation";
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


	CustomIcon JsonCustomIconOrEmpty(const JsonValue* value)
	{
		if (value == nullptr || value->type != JsonValue::Type::Object) return {};
		const JsonValue* type = value->Find("type");
		const JsonValue* content = value->Find("value");
		if (type == nullptr || content == nullptr || type->type != JsonValue::Type::String || content->type != JsonValue::Type::String) return {};
		const CustomIcon icon{type->string_value, content->string_value};
		return uam::icons::IsValid(icon) ? icon : CustomIcon{};
	}

	JsonValue CustomIconToJson(const CustomIcon& icon)
	{
		if (!uam::icons::IsValid(icon) || icon.type.empty()) return {};
		JsonValue value = uam::json::Object();
		uam::json::SetString(value, "type", icon.type);
		uam::json::SetString(value, "value", icon.value);
		return value;
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

	std::optional<std::int64_t> NativeGoalBindingInteger(const JsonValue* value)
	{
		if (value == nullptr || value->type != JsonValue::Type::Number || !std::isfinite(value->number_value) ||
			value->number_value < 0 || value->number_value > 9'007'199'254'740'991.0 ||
			std::floor(value->number_value) != value->number_value) return std::nullopt;
		return static_cast<std::int64_t>(value->number_value);
	}

	std::optional<std::int64_t> ChildTimingInteger(const JsonValue* value)
	{
		const auto clock = NativeGoalBindingInteger(value);
		return clock && *clock <= 8'640'000'000'000'000LL ? clock : std::nullopt;
	}

	bool HasOwnedClaudeMaintenanceTerminalReceipt(const ChatSession& chat)
	{
		if (chat.provider_id != uam::provider_ids::kClaudeCli || !chat.remote_process_exists || chat.execution_host_id.empty() || chat.execution_host_id == "local" ||
		    !uam::uuid::IsCanonicalUuid(chat.native_session_id) || chat.execution_host_id.size() > 128 ||
		    chat.remote_process_control_token.empty() || chat.remote_process_control_token.size() > 256 ||
		    chat.remote_manual_compaction_boundary_json.empty() || chat.remote_manual_compaction_boundary_json.size() > 2048) return false;
		const auto receipt = ParseJson(chat.remote_manual_compaction_boundary_json);
		if (!receipt || receipt->type != JsonValue::Type::Object) return false;
		const auto request = NativeGoalBindingInteger(receipt->Find("requestId"));
		const std::string delivery = JsonStringOrEmpty(receipt->Find("deliveryId"));
		return JsonStringOrEmpty(receipt->Find("kind")) == "claude_manual_compaction_terminal" &&
		    JsonStringOrEmpty(receipt->Find("providerId")) == chat.provider_id &&
		    JsonStringOrEmpty(receipt->Find("sessionId")) == chat.native_session_id &&
		    JsonStringOrEmpty(receipt->Find("executionHostId")) == chat.execution_host_id &&
		    JsonStringOrEmpty(receipt->Find("processControlToken")) == chat.remote_process_control_token &&
		    uam::uuid::IsCanonicalUuid(JsonStringOrEmpty(receipt->Find("resultId"))) && request && *request > 0 &&
		    *request <= std::numeric_limits<int>::max() && !delivery.empty() && delivery.size() <= 128;
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
		if (!uam::attachment_sources::IsPersistable(attachment))
			throw std::length_error("Attachment source is invalid or original bytes are not prepared.");
		JsonValue obj = uam::json::Object();
		uam::json::SetString(obj, attachment_fields::kIdField, attachment.id);
		uam::json::SetString(obj, attachment_fields::kNameField, attachment.name);
		uam::json::SetString(obj, attachment_fields::kKindField, attachment.kind);
		uam::json::SetString(obj, attachment_persisted_fields::kMimeTypeField, attachment.mime_type);
		uam::json::SetString(obj, attachment_fields::kPathField, attachment.path);
		uam::json::SetNumber(obj, attachment_persisted_fields::kSizeBytesField, static_cast<double>(attachment.size_bytes));
		uam::json::SetBool(obj, attachment_fields::kCopiedField, attachment.copied);
		if (!attachment.preview_key.empty()) uam::json::SetString(obj, attachment_fields::kPreviewKeyField, attachment.preview_key);
		if (!attachment.preview_origin_path.empty()) uam::json::SetString(obj, attachment_fields::kPreviewOriginField, attachment.preview_origin_path);
		if (attachment.source != AttachmentSource::LegacyPath)
		{
			uam::json::SetString(obj, attachment_fields::kSourceField, std::string(uam::attachment_sources::Name(attachment.source)));
			for (const auto& [field, value] : {
			         std::pair{attachment_fields::kSourceUriField, &attachment.source_uri},
			         std::pair{attachment_fields::kOriginHostField, &attachment.origin_execution_host_id},
			         std::pair{attachment_fields::kNativePartKeyField, &attachment.native_part_key},
			         std::pair{attachment_fields::kNativeToolCallIdField, &attachment.native_tool_call_id},
			         std::pair{attachment_fields::kNativeSourceField, &attachment.native_source_field},
			         std::pair{attachment_fields::kNativeAssetKeyField, &attachment.native_asset_key},
			         std::pair{attachment_fields::kContentDigestField, &attachment.content_digest}})
				if (!value->empty()) uam::json::SetString(obj, field, *value);
		}
		return obj;
	}

	std::optional<MessageAttachment> AttachmentFromJson(const JsonValue& item)
	{
		MessageAttachment attachment;
		attachment.id = JsonStringOrEmpty(item.Find(attachment_fields::kIdField));
		attachment.name = JsonStringOrEmpty(item.Find(attachment_fields::kNameField));
		attachment.kind = JsonStringOrEmpty(item.Find(attachment_fields::kKindField));
		attachment.mime_type = JsonStringOrEmpty(item.Find(attachment_persisted_fields::kMimeTypeField));
		attachment.path = JsonStringOrEmpty(item.Find(attachment_fields::kPathField));
		attachment.size_bytes = NonNegativeUintmaxFieldOrZero(item.Find(attachment_persisted_fields::kSizeBytesField));
		attachment.copied = JsonBoolOrDefault(item.Find(attachment_fields::kCopiedField), false);
		attachment.preview_key = JsonStringOrEmpty(item.Find(attachment_fields::kPreviewKeyField));
		attachment.preview_origin_path = JsonStringOrEmpty(item.Find(attachment_fields::kPreviewOriginField));
		if (const auto* source = item.Find(attachment_fields::kSourceField))
		{
			if (source->type != JsonValue::Type::String) throw std::length_error("Saved native attachment source is invalid.");
			const auto parsed = uam::attachment_sources::Parse(source->string_value);
			if (!parsed) throw std::length_error("Saved native attachment source is invalid.");
			attachment.source = *parsed;
		}
		for (const auto& [field, value] : {
		         std::pair{attachment_fields::kSourceUriField, &attachment.source_uri},
		         std::pair{attachment_fields::kOriginHostField, &attachment.origin_execution_host_id},
		         std::pair{attachment_fields::kNativePartKeyField, &attachment.native_part_key},
		         std::pair{attachment_fields::kNativeToolCallIdField, &attachment.native_tool_call_id},
		         std::pair{attachment_fields::kNativeSourceField, &attachment.native_source_field},
		         std::pair{attachment_fields::kNativeAssetKeyField, &attachment.native_asset_key},
		         std::pair{attachment_fields::kContentDigestField, &attachment.content_digest}})
		{
			const auto* recorded = item.Find(field);
			if (recorded && recorded->type != JsonValue::Type::String) throw std::length_error("Saved native attachment source is invalid.");
			*value = JsonStringOrEmpty(recorded);
		}
		if (!uam::attachment_sources::IsPersistable(attachment)) throw std::length_error("Saved native attachment source is invalid.");
		return attachment;
	}

	bool AttachmentsPersistable(const std::vector<MessageAttachment>& attachments, std::string_view owner)
	{
		return std::all_of(attachments.begin(), attachments.end(), [&](const auto& attachment)
		{ return uam::attachment_sources::IsPersistable(attachment, owner); });
	}

	bool NativeQueueRepresentable(const ChatSession& chat);
	bool PassiveArchiveModelValid(const ChatSession& chat);

	bool ChatAttachmentsPersistable(const ChatSession& chat)
	{
		return PassiveArchiveModelValid(chat) && uam::prompt_recovery::Bounded(chat.acp_unsent_prompt_recoveries) && std::all_of(chat.messages.begin(), chat.messages.end(), [&](const auto& message)
		    { return AttachmentsPersistable(message.attachments, chat.id); }) &&
		    std::all_of(chat.acp_queued_prompts.begin(), chat.acp_queued_prompts.end(), [&](const auto& prompt)
		    { return AttachmentsPersistable(prompt.attachments, chat.id); }) && NativeQueueRepresentable(chat);
	}

	JsonValue UnsentRecoveryToJson(const uam::AcpUnsentPromptRecovery& value)
	{
		JsonValue obj = uam::json::Object();
		uam::json::SetNumber(obj, "version", value.version);
		uam::json::SetString(obj, "chat_id", value.chat_id);
		uam::json::SetString(obj, "provider_id", value.provider_id);
		uam::json::SetString(obj, "user_message_created_at", value.user_message_created_at);
		uam::json::SetString(obj, "row_witness", value.row_witness);
		uam::json::SetString(obj, "admission_id", value.admission_id);
		uam::json::SetString(obj, "uam_agent_id", value.uam_agent_id);
		uam::json::SetString(obj, "uam_agent_definition_hash", value.uam_agent_definition_hash);
		uam::json::SetString(obj, "uam_agent_definition_snapshot", value.uam_agent_definition_snapshot);
		uam::json::SetString(obj, "uam_agent_instructions", value.uam_agent_instructions);
		uam::json::SetString(obj, "uam_agent_workspace_access", value.uam_agent_workspace_access);
		uam::json::SetString(obj, "uam_agent_execution_capability", value.uam_agent_execution_capability);
		uam::json::SetNumber(obj, "user_message_index", value.user_message_index);
		uam::json::SetNumber(obj, "admission_revision", value.admission_revision);
		uam::json::SetValue(obj, "uam_agent_skills", StringArrayToJson(value.uam_agent_skills));
		uam::json::SetValue(obj, "uam_agent_delegates", StringArrayToJson(value.uam_agent_delegates));
		return obj;
	}
	JsonValue UnsentRecoveriesToJson(const ChatSession& chat)
	{
		JsonValue values = uam::json::Array();
		for (const auto& value : chat.acp_unsent_prompt_recoveries) uam::json::PushValue(values, UnsentRecoveryToJson(value));
		return values;
	}
	std::vector<uam::AcpUnsentPromptRecovery> UnsentRecoveriesFromJson(const JsonValue* values)
	{
		if (!values) return {};
		const auto invalid = [] { uam::AcpUnsentPromptRecovery value; value.version = 0; return std::vector{value}; };
		if (values->type != JsonValue::Type::Array || values->array_value.empty() ||
		    values->array_value.size() > uam::prompt_recovery::kMaxRecords) return invalid();
		std::vector<uam::AcpUnsentPromptRecovery> result;
		std::size_t total_bytes = 0;
		for (const auto& obj : values->array_value)
		{
			if (obj.type != JsonValue::Type::Object) return invalid();
			uam::AcpUnsentPromptRecovery value;
			const auto string = [&](const char* key, std::string& out, std::size_t limit)
			{
				const auto* field = obj.Find(key);
				if (!field || field->type != JsonValue::Type::String || field->string_value.size() > limit ||
				    field->string_value.size() > uam::prompt_recovery::kMaxBytes - total_bytes) return false;
				total_bytes += field->string_value.size();
				out = field->string_value; return true;
			};
			const auto integer = [&](const char* key, int& out, int minimum)
			{
				const auto* field = obj.Find(key);
				if (!field || field->type != JsonValue::Type::Number || !std::isfinite(field->number_value) ||
				    field->number_value < minimum || field->number_value > std::numeric_limits<int>::max() ||
				    std::floor(field->number_value) != field->number_value) return false;
				out = static_cast<int>(field->number_value); return true;
			};
			const auto ids = [&](const char* key, std::vector<std::string>& out)
			{
				const auto* field = obj.Find(key);
				if (!field || field->type != JsonValue::Type::Array || field->array_value.size() > 128) return false;
				for (const auto& id : field->array_value)
				{
					if (id.type != JsonValue::Type::String || id.string_value.empty() || id.string_value.size() > 64 ||
					    id.string_value.size() > uam::prompt_recovery::kMaxBytes - total_bytes) return false;
					total_bytes += id.string_value.size();
					out.push_back(id.string_value);
				}
				return true;
			};
			if (!integer("version", value.version, 0) || !integer("user_message_index", value.user_message_index, -1) ||
			    !integer("admission_revision", value.admission_revision, 1)) return invalid();
			if (!string("chat_id", value.chat_id, 256)) return invalid();
			if (!string("provider_id", value.provider_id, 256)) return invalid();
			if (!string("user_message_created_at", value.user_message_created_at, 256)) return invalid();
			if (!string("row_witness", value.row_witness, 256)) return invalid();
			if (!string("admission_id", value.admission_id, 256)) return invalid();
			if (!string("uam_agent_id", value.uam_agent_id, 64)) return invalid();
			if (!string("uam_agent_definition_hash", value.uam_agent_definition_hash, 256)) return invalid();
			if (!string("uam_agent_definition_snapshot", value.uam_agent_definition_snapshot, 256U * 1024U)) return invalid();
			if (!string("uam_agent_instructions", value.uam_agent_instructions, 192U * 1024U)) return invalid();
			if (!string("uam_agent_workspace_access", value.uam_agent_workspace_access, 256)) return invalid();
			if (!string("uam_agent_execution_capability", value.uam_agent_execution_capability, 256)) return invalid();
			if (!ids("uam_agent_skills", value.uam_agent_skills)) return invalid();
			if (!ids("uam_agent_delegates", value.uam_agent_delegates)) return invalid();
			result.push_back(std::move(value));
		}
		return uam::prompt_recovery::Bounded(result) ? result : invalid();
	}

	bool RawManagedQueueMarker(const JsonValue& root)
	{
		const auto* prompts = root.Find(kChatAcpQueuedPromptsField);
		if (!prompts) return false;
		if (prompts->type != JsonValue::Type::Array) return prompts->Find("managed_turn") != nullptr;
		// An uninspected tail cannot prove the absence of unsupported managed input.
		if (prompts->array_value.size() > kMaxPersistedAcpQueuedPrompts) return true;
		return std::ranges::any_of(prompts->array_value, [](const auto& prompt)
		    { return prompt.Find("managed_turn") != nullptr; });
	}

	void HoldManagedQueue(ChatSession& chat)
	{
		if (!chat.managed_followup_input_unavailable) return;
		chat.acp_queue_held = true;
		chat.acp_queue_storage_error = true;
	}

	bool ValidManagedPrompt(const uam::AcpQueuedUserPromptState& prompt)
	{
		if (!prompt.managed_turn) return true;
		const auto& identity = *prompt.managed_turn;
		return uam::uuid::IsCanonicalUuid(identity.run_id) && uam::uuid::IsCanonicalUuid(identity.turn_id) &&
		    identity.turn_number > 0 && identity.turn_id == prompt.id &&
		    identity.continuity == uam::ManagedRunContinuity::Native &&
		    !identity.expected_native_session_id.empty() && identity.expected_native_session_id.size() <= 256 &&
		    identity.expected_native_session_id.find('\0') == std::string::npos;
	}

	// Invalid optional provenance is dropped independently; authored content remains intact.
	std::optional<std::string> InputProvenanceFromJson(const JsonValue* value, bool native_id)
	{
		if (!value || value->type != JsonValue::Type::String) return std::nullopt;
		const bool valid = native_id ? uam::input_provenance::ValidNativeInputId(value->string_value)
		    : uam::input_provenance::ValidOriginRequestId(value->string_value);
		return valid ? std::optional<std::string>(value->string_value) : std::nullopt;
	}

	JsonValue AcpQueuedPromptToJson(const uam::AcpQueuedUserPromptState& prompt)
	{
		JsonValue obj = uam::json::Object();
		uam::json::SetString(obj, "id", prompt.id);
		uam::json::SetNumber(obj, "revision", prompt.revision);
		if (prompt.native_input_id && uam::input_provenance::ValidNativeInputId(*prompt.native_input_id)) uam::json::SetString(obj, "native_input_id", *prompt.native_input_id);
		if (prompt.origin_request_id && uam::input_provenance::ValidOriginRequestId(*prompt.origin_request_id)) uam::json::SetString(obj, "origin_request_id", *prompt.origin_request_id);
		if (prompt.managed_turn)
		{
			const auto& identity = *prompt.managed_turn;
			JsonValue managed = uam::json::Object();
			uam::json::SetString(managed, "run_id", identity.run_id);
			uam::json::SetString(managed, "turn_id", identity.turn_id);
			uam::json::SetNumber(managed, "turn_number", identity.turn_number);
			uam::json::SetString(managed, "expected_native_session_id", identity.expected_native_session_id);
			uam::json::SetString(managed, "continuity", identity.continuity == uam::ManagedRunContinuity::Native ? "native" : "");
			uam::json::SetValue(obj, "managed_turn", std::move(managed));
		}
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
		if (prompt.existing_user_message_index >= 0)
		{
			uam::json::SetNumber(obj, "existing_user_message_index", prompt.existing_user_message_index);
			uam::json::SetString(obj, "existing_user_message_created_at", prompt.existing_user_message_created_at);
		}
		if (prompt.include_prior_context)
			uam::json::SetBool(obj, "include_prior_context", true);
		if (prompt.direct_admission)
			uam::json::SetBool(obj, "direct_admission", true);
		if (prompt.repository_baseline_hold)
			uam::json::SetBool(obj, "repository_baseline_hold", true);
		if (prompt.prepared_for_delivery)
			uam::json::SetBool(obj, "prepared_for_delivery", true);
		if (!prompt.claude_quota_continuation_id.empty())
			uam::json::SetString(obj, "claude_quota_continuation_id", prompt.claude_quota_continuation_id);
		if (prompt.prepared_user_message_count > 0)
			uam::json::SetNumber(obj, "prepared_user_message_count", prompt.prepared_user_message_count);
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

	bool ValidRemotePendingRequest(const uam::AcpRemotePendingRequestState& request,
	                               std::size_t total_payload_bytes)
	{
		return request.request_id > 0 && request.request_id < std::numeric_limits<int>::max() &&
		    !request.method.empty() &&
		    request.method.size() <= 256 && request.delivery_id.size() <= 256 &&
		    request.provider_turn_id.size() <= 256 && request.user_message_index >= -1 &&
		    request.turn_serial >= 0 && request.payload.size() <=
		        kMaxPersistedRemotePromptDeliveryBytes -
		            std::min(total_payload_bytes, kMaxPersistedRemotePromptDeliveryBytes);
	}

	JsonValue RemotePendingRequestToJson(const uam::AcpRemotePendingRequestState& request)
	{
		JsonValue obj = uam::json::Object();
		uam::json::SetNumber(obj, "request_id", request.request_id);
		uam::json::SetString(obj, "method", request.method);
		uam::json::SetString(obj, "delivery_id", request.delivery_id);
		uam::json::SetString(obj, "payload", request.payload);
		uam::json::SetNumber(obj, "user_message_index", request.user_message_index);
		uam::json::SetNumber(obj, "turn_serial", request.turn_serial);
		uam::json::SetString(obj, "provider_turn_id", request.provider_turn_id);
		uam::json::SetBool(obj, "response_consumed", request.response_consumed);
		return obj;
	}

	std::optional<uam::AcpQueuedUserPromptState> AcpQueuedPromptFromJson(
	    const JsonValue& obj, std::size_t& total_text_bytes)
	{
		if (obj.type != JsonValue::Type::Object) return std::nullopt;
		uam::AcpQueuedUserPromptState prompt;
		prompt.id = JsonStringOrEmpty(obj.Find("id"));
		prompt.native_input_id = InputProvenanceFromJson(obj.Find("native_input_id"), true);
		prompt.origin_request_id = InputProvenanceFromJson(obj.Find("origin_request_id"), false);
		if (prompt.id.size() > 128) prompt.id.clear();
		prompt.revision = IntFieldAtLeastOrDefault(obj.Find("revision"), 1, 1);
		prompt.text = JsonStringOrEmpty(obj.Find("text"));
		if (prompt.text.size() > kMaxPersistedAcpQueuedPromptBytes -
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
				auto attachment = AttachmentFromJson(item);
				if (!attachment) return std::nullopt;
				if (attachment->source != AttachmentSource::LegacyPath || !attachment->path.empty())
					prompt.attachments.push_back(std::move(*attachment));
			}
		}
		if (prompt.text.empty() && prompt.attachments.empty()) return std::nullopt;
		prompt.append_user_message = JsonBoolOrDefault(obj.Find("append_user_message"), true);
		const JsonValue* existing_index = obj.Find("existing_user_message_index");
		const JsonValue* existing_created = obj.Find("existing_user_message_created_at");
		if (existing_index != nullptr || existing_created != nullptr)
		{
			if (existing_index == nullptr || existing_created == nullptr || prompt.append_user_message ||
			    existing_index->type != JsonValue::Type::Number || !std::isfinite(existing_index->number_value) ||
			    existing_index->number_value < 0 || existing_index->number_value > std::numeric_limits<int>::max() ||
			    std::floor(existing_index->number_value) != existing_index->number_value ||
			    existing_created->type != JsonValue::Type::String || existing_created->string_value.empty() ||
			    existing_created->string_value.size() > 256) return std::nullopt;
			prompt.existing_user_message_index = static_cast<int>(existing_index->number_value);
			prompt.existing_user_message_created_at = existing_created->string_value;
		}
		prompt.prepared_for_delivery = JsonBoolOrDefault(obj.Find("prepared_for_delivery"), false);
		if (const JsonValue* direct_admission = obj.Find("direct_admission"); direct_admission != nullptr)
		{
			if (direct_admission->type != JsonValue::Type::Bool) return std::nullopt;
			prompt.direct_admission = direct_admission->bool_value;
		}
		if (const JsonValue* baseline_hold = obj.Find("repository_baseline_hold"); baseline_hold != nullptr)
		{
			if (baseline_hold->type != JsonValue::Type::Bool) return std::nullopt;
			prompt.repository_baseline_hold = baseline_hold->bool_value;
		}
		if (const JsonValue* prior_context = obj.Find("include_prior_context"); prior_context != nullptr)
		{
			if (prior_context->type != JsonValue::Type::Bool) return std::nullopt;
			prompt.include_prior_context = prior_context->bool_value;
			if (prompt.include_prior_context && (prompt.append_user_message || prompt.existing_user_message_index < 0 || prompt.prepared_for_delivery)) return std::nullopt;
		}
		prompt.claude_quota_continuation_id = JsonStringOrEmpty(obj.Find("claude_quota_continuation_id"));
		prompt.prepared_user_message_count = std::min(NonNegativeIntFieldOrZero(
		    obj.Find("prepared_user_message_count")), 64);
		prompt.goal_mode = JsonBoolOrDefault(obj.Find("goal_mode"), false);
		prompt.goal_id = JsonStringOrEmpty(obj.Find("goal_id"));
		prompt.computer_use_mode = JsonBoolOrDefault(obj.Find("computer_use_mode"), false);
		prompt.priority_steer = JsonBoolOrDefault(obj.Find("priority_steer"), false);
		if (const auto* managed = obj.Find("managed_turn"))
		{
			if (managed->type != JsonValue::Type::Object || managed->object_value.size() != 5) return std::nullopt;
			for (const char* key : {"run_id", "turn_id", "expected_native_session_id", "continuity"})
			{
				const auto* field = managed->Find(key);
				if (!field || field->type != JsonValue::Type::String) return std::nullopt;
			}
			const auto* number = managed->Find("turn_number");
			if (!number || number->type != JsonValue::Type::Number || !std::isfinite(number->number_value) ||
			    number->number_value < 1 || number->number_value > std::numeric_limits<std::uint32_t>::max() ||
			    std::floor(number->number_value) != number->number_value ||
			    JsonStringOrEmpty(managed->Find("continuity")) != "native") return std::nullopt;
			prompt.managed_turn = uam::ManagedRunPromptIdentity{
			    JsonStringOrEmpty(managed->Find("run_id")), JsonStringOrEmpty(managed->Find("turn_id")),
			    static_cast<std::uint32_t>(number->number_value), JsonStringOrEmpty(managed->Find("expected_native_session_id")),
			    uam::ManagedRunContinuity::Native};
			if (!ValidManagedPrompt(prompt)) return std::nullopt;
		}
		return prompt;
	}


	bool NativeQueueRepresentable(const ChatSession& chat)
	{
		const bool has_native = std::any_of(chat.acp_queued_prompts.begin(), chat.acp_queued_prompts.end(), [](const auto& prompt)
		{
			return prompt.managed_turn.has_value() || std::any_of(prompt.attachments.begin(), prompt.attachments.end(), [](const auto& attachment)
			    { return attachment.source != AttachmentSource::LegacyPath; });
		});
		if (!has_native) return true;
		if (chat.acp_queued_prompts.size() > kMaxPersistedAcpQueuedPrompts) return false;
		std::size_t bytes = 0;
		for (const auto& prompt : chat.acp_queued_prompts)
		{
			if (prompt.attachments.size() > 64 || prompt.text.size() > kMaxPersistedAcpQueuedPromptBytes -
			    std::min(bytes, kMaxPersistedAcpQueuedPromptBytes)) return false;
			const auto decoded = AcpQueuedPromptFromJson(AcpQueuedPromptToJson(prompt), bytes);
			if (!decoded || decoded->text != prompt.text || decoded->attachments != prompt.attachments || decoded->managed_turn != prompt.managed_turn) return false;
		}
		return true;
	}

	bool HasNativeAttachmentFields(const JsonValue& value)
	{
		for (const auto field : {attachment_fields::kSourceField, attachment_fields::kSourceUriField,
		         attachment_fields::kOriginHostField, attachment_fields::kNativePartKeyField,
		         attachment_fields::kNativeToolCallIdField, attachment_fields::kNativeSourceField,
		         attachment_fields::kNativeAssetKeyField, attachment_fields::kContentDigestField})
			if (value.Find(field)) return true;
		return false;
	}

	bool NativeQueueDocumentRepresentable(const JsonValue& root)
	{
		const auto* prompts = root.Find(kChatAcpQueuedPromptsField);
		if (!prompts) return true;
		const auto has_native_prompt = [](const JsonValue& prompt)
		{
			const auto* attachments = prompt.Find("attachments");
			return attachments && (HasNativeAttachmentFields(*attachments) ||
			    std::any_of(attachments->array_value.begin(), attachments->array_value.end(), HasNativeAttachmentFields));
		};
		const bool has_native = has_native_prompt(*prompts) ||
		    std::any_of(prompts->array_value.begin(), prompts->array_value.end(), has_native_prompt);
		if (!has_native) return true;
		// The existing legacy queue decoder has retained-item and text ceilings.
		// New sources must be completely representable before it may adopt rows.
		if (prompts->type != JsonValue::Type::Array || prompts->array_value.size() > kMaxPersistedAcpQueuedPrompts) return false;
		std::size_t bytes = 0;
		for (const auto& prompt : prompts->array_value)
		{
			const auto* attachments = prompt.Find("attachments");
			if (attachments && (attachments->type != JsonValue::Type::Array || attachments->array_value.size() > 64 ||
			    std::any_of(attachments->array_value.begin(), attachments->array_value.end(), [](const auto& item)
			    { return item.type != JsonValue::Type::Object; }))) return false;
			JsonValue native_prompt = prompt;
			native_prompt.object_value.erase("managed_turn");
			const auto decoded = AcpQueuedPromptFromJson(native_prompt, bytes);
			if (!decoded || (attachments && decoded->attachments.size() != attachments->array_value.size())) return false;
		}
		return true;
	}

	bool DecodeArchiveDocument(const JsonValue& root, std::string_view owner,
	    std::vector<uam::AcpQueuedUserPromptState>& decoded)
	{
		if (root.type != JsonValue::Type::Object || owner.size() > 256 || !uam::chat_ids::IsSafeStorageChatId(owner) ||
		    JsonStringOrEmpty(root.Find(kChatIdField)) != owner || !NativeQueueDocumentRepresentable(root)) return false;
		const auto* prompts = root.Find(kChatAcpQueuedPromptsField);
		if (!prompts) return true;
		if (prompts->type != JsonValue::Type::Array || prompts->array_value.size() > kMaxPersistedAcpQueuedPrompts) return false;
		std::size_t text_bytes = 0;
		for (const auto& raw : prompts->array_value)
		{
			if (raw.type != JsonValue::Type::Object) return false;
			for (const auto field : {"id", "text"})
				if (const auto* value = raw.Find(field); value && value->type != JsonValue::Type::String) return false;
			const auto* attachments = raw.Find("attachments");
			if (attachments)
			{
				if (attachments->type != JsonValue::Type::Array || attachments->array_value.size() > 64) return false;
				for (const auto& part : attachments->array_value)
				{
					if (part.type != JsonValue::Type::Object) return false;
					if (!HasNativeAttachmentFields(part)) continue;
					for (const auto field : {"id", "name", "kind", "mime_type", "path", "source", "sourceUri", "originExecutionHostId",
					         "nativePartKey", "nativeToolCallId", "nativeSourceField", "nativeAssetKey", "contentDigest", "previewKey", "previewOriginPath"})
						if (const auto* value = part.Find(field); value && value->type != JsonValue::Type::String) return false;
					if (const auto* copied = part.Find("copied"); copied && copied->type != JsonValue::Type::Bool) return false;
					if (const auto* size = part.Find("size_bytes"); size && (size->type != JsonValue::Type::Number ||
					    !std::isfinite(size->number_value) || size->number_value < 0 || std::floor(size->number_value) != size->number_value ||
					    size->number_value > uam::native_assets::kMaxOriginalBytes)) return false;
				}
			}
			auto prompt = AcpQueuedPromptFromJson(raw, text_bytes);
			if (!prompt || prompt->id != JsonStringOrEmpty(raw.Find("id")) ||
			    (attachments && prompt->attachments.size() != attachments->array_value.size()) ||
			    !AttachmentsPersistable(prompt->attachments, owner)) return false;
			decoded.push_back(std::move(*prompt));
		}
		return true;
	}

	std::shared_ptr<const uam::PassiveQueuedPromptArchive> PassiveArchiveFromText(std::string_view text, std::string_view owner)
	{
		const auto raw = uam::passive_archive_json::Parse(text);
		if (!raw || raw->owner != owner) return {};
		// Typed validation checks supported queue/native references, but its double
		// representation is never the serialization authority for inert values.
		JsonValue document = uam::json::Object();
		uam::json::SetString(document, kChatIdField, owner);
		auto array = ParseJson(raw->canonical);
		if (!array) return {};
		uam::json::SetValue(document, kChatAcpQueuedPromptsField, std::move(*array));
		std::vector<uam::AcpQueuedUserPromptState> decoded;
		if (!DecodeArchiveDocument(document, owner, decoded)) return {};
		std::size_t native_bytes = 0;
		for (const auto& prompt : raw->prompts)
		{
			const auto attachments = prompt.find("attachments");
			if (attachments == prompt.end()) continue;
			for (const auto& part : *attachments)
			{
				const auto typed = ParseJson(part.dump());
				if (!typed || !HasNativeAttachmentFields(*typed)) continue;
				const auto bytes = part.dump().size();
				if (bytes > kMaxPassiveNativeMetadataBytes - native_bytes) return {};
				native_bytes += bytes;
			}
		}
		auto archive = std::make_shared<uam::PassiveQueuedPromptArchive>();
		archive->owner_chat_id = owner;
		archive->canonical_prompts_json = raw->canonical;
		return archive;
	}

	std::shared_ptr<const uam::PassiveQueuedPromptArchive> PassiveArchiveFromDocument(const JsonValue& root, std::string_view owner)
	{
		return PassiveArchiveFromText(SerializeJson(root, kMaxChatJsonBytes), owner);
	}

	std::shared_ptr<const uam::PassiveQueuedPromptArchive> ValidatePassiveArchive(
	    const uam::PassiveQueuedPromptArchive& archive, std::string_view owner)
	{
		if (archive.owner_chat_id != owner || archive.canonical_prompts_json.size() > kMaxPassiveArchiveBytes) return {};
		return PassiveArchiveFromText("{\"id\":" + nlohmann::json(std::string(owner)).dump() + ",\"acp_queued_prompts\":" + archive.canonical_prompts_json + "}", owner);
	}

	std::string SerializeArchiveDocument(const JsonValue& root,
	    const std::shared_ptr<const uam::PassiveQueuedPromptArchive>& archive, std::size_t limit = kMaxChatJsonBytes)
	{
		if (!archive) return SerializeJson(root, limit);
		if (root.type != JsonValue::Type::Object) throw std::length_error("Invalid archive document");
		std::string output = "{";
		bool first = true;
		const auto append = [&](std::string_view value)
		{
			if (value.size() > limit - std::min(output.size(), limit)) throw std::length_error("Archive document exceeds limit");
			output.append(value);
		};
		std::vector<std::string_view> keys;
		keys.reserve(root.object_value.size());
		for (const auto& [key, value] : root.object_value) keys.push_back(key);
		std::sort(keys.begin(), keys.end());
		for (const auto key : keys)
		{
			if (!first) append(",");
			first = false;
			append(nlohmann::json(std::string(key)).dump());
			append(":");
			if (key == kChatAcpQueuedPromptsField) append(archive->canonical_prompts_json);
			else append(SerializeJson(*root.Find(key), limit - output.size()));
		}
		append("}\n");
		return output;
	}

	bool SamePassiveArchive(const std::shared_ptr<const uam::PassiveQueuedPromptArchive>& lhs,
	    const std::shared_ptr<const uam::PassiveQueuedPromptArchive>& rhs)
	{
		return lhs && rhs && lhs->owner_chat_id == rhs->owner_chat_id && lhs->canonical_prompts_json == rhs->canonical_prompts_json;
	}

	bool PassiveArchiveModelValid(const ChatSession& chat)
	{
		if (!chat.imported_read_only) return !chat.passive_queued_prompt_archive;
		return (!chat.passive_queued_prompt_archive || ValidatePassiveArchive(*chat.passive_queued_prompt_archive, chat.id)) &&
		    chat.acp_queued_prompts.empty() && !chat.acp_queue_held && chat.acp_dispatched_queued_prompt_count == 0;
	}

	std::optional<ChatSession> DetachedPassiveSaveCandidate(const ChatSession& source,
	    std::shared_ptr<const uam::PassiveQueuedPromptArchive> authoritative = {})
	try
	{
		ChatSession detached = source;
		if (authoritative)
		{
			if (source.passive_queued_prompt_archive) return std::nullopt;
			detached.passive_queued_prompt_archive = std::move(authoritative);
		}
		if (!source.acp_queued_prompts.empty())
		{
			if (source.passive_queued_prompt_archive) return std::nullopt;
			if (!detached.passive_queued_prompt_archive)
			{
				// A new detached record has only typed input. Existing active records
				// instead supply the revision-bound raw primary archive above.
				JsonValue document = uam::json::Object();
				uam::json::SetString(document, kChatIdField, source.id);
				JsonValue prompts = uam::json::Array();
				for (const auto& prompt : source.acp_queued_prompts) uam::json::PushValue(prompts, AcpQueuedPromptToJson(prompt));
				uam::json::SetValue(document, kChatAcpQueuedPromptsField, std::move(prompts));
				detached.passive_queued_prompt_archive = PassiveArchiveFromDocument(document, source.id);
			}
			if (!detached.passive_queued_prompt_archive) return std::nullopt;
		}
		detached.acp_unsent_prompt_recoveries.clear();
		detached.acp_queued_prompts.clear();
		detached.acp_queue_held = false;
		detached.acp_dispatched_queued_prompt_count = 0;
		return detached;
	}
	catch (...)
	{
		return std::nullopt;
	}

	JsonValue MessageToJson(const Message& msg)
	{
		JsonValue obj = uam::json::Object();
		uam::json::SetString(obj, kMessageRoleField, RoleToString(msg.role));
		uam::json::SetString(obj, kMessageContentField, msg.content);
		uam::json::SetString(obj, kMessageCreatedAtField, msg.created_at);
		if (msg.native_input_id && uam::input_provenance::ValidNativeInputId(*msg.native_input_id)) uam::json::SetString(obj, "native_input_id", *msg.native_input_id);
		if (msg.role == MessageRole::User && msg.input_write_completed && msg.native_input_id && uam::input_provenance::ValidNativeInputId(*msg.native_input_id)) uam::json::SetBool(obj, "input_write_completed", true);
		if (msg.origin_request_id && uam::input_provenance::ValidOriginRequestId(*msg.origin_request_id)) uam::json::SetString(obj, "origin_request_id", *msg.origin_request_id);

		SetNonEmptyString(obj, kMessageProviderField, msg.provider);
		SetNonEmptyString(obj, "model_id", msg.model_id);
		SetPositiveNumber(obj, kMessageTokensInputField, static_cast<double>(msg.tokens_input));
		SetPositiveNumber(obj, kMessageTokensOutputField, static_cast<double>(msg.tokens_output));
		SetPositiveNumber(obj, kMessageEstimatedCostUsdField, msg.estimated_cost_usd);
		SetPositiveNumber(obj, kMessageTimeToFirstTokenMsField, static_cast<double>(msg.time_to_first_token_ms));
		SetPositiveNumber(obj, kMessageProcessingTimeMsField, static_cast<double>(msg.processing_time_ms));
		uam::json::SetString(obj, "stop_reason", msg.stop_reason);
		if (msg.interrupted)
		{
			uam::json::SetBool(obj, kMessageInterruptedField, true);
		}
		if (msg.acp_prompt_not_sent) uam::json::SetBool(obj, "acp_prompt_not_sent", true);
		if (msg.acp_prompt_discarded) uam::json::SetBool(obj, "acp_prompt_discarded", true);
		if (msg.continues_turn) uam::json::SetBool(obj, kMessageContinuesTurnField, true);
		if (msg.priority_steer)
		{
			uam::json::SetBool(obj, kMessagePrioritySteerField, true);
		}
		SetNonEmptyString(obj, kMessageCheckpointShaField, msg.checkpoint_sha);
		SetNonEmptyString(obj, kMessageCheckpointParentShaField, msg.checkpoint_parent_sha);
		SetNonEmptyString(obj, kMessageThoughtsField, msg.thoughts);
		SetNonEmptyString(obj, kMessagePlanSummaryField, msg.plan_summary);
		if (msg.managed_origin_run_id && uam::uuid::IsCanonicalUuid(*msg.managed_origin_run_id)) SetNonEmptyString(obj, "managed_origin_run_id", *msg.managed_origin_run_id);
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
				uam::json::SetString(tc_obj, "kind", tc.kind);
				uam::json::SetString(tc_obj, "approval_status", tc.approval_status);
				uam::json::SetString(tc_obj, "task_id", tc.task_id);
				uam::json::SetString(tc_obj, "child_activity", tc.child_activity);
				uam::json::SetString(tc_obj, kToolCallArgsJsonField, tc.args_json);
				uam::json::SetString(tc_obj, kToolCallResultTextField, tc.result_text);
				uam::json::SetString(tc_obj, kToolCallStatusField, tc.status);
				uam::json::SetBool(tc_obj, kToolCallIsSubAgentField, tc.is_sub_agent);
				uam::json::SetString(tc_obj, kToolCallSubAgentIdField, tc.sub_agent_id);
				uam::json::SetString(tc_obj, kToolCallSubAgentTitleField, tc.sub_agent_title);
				if (tc.sub_agent_children_authoritative) uam::json::SetBool(tc_obj, kToolCallSubAgentChildrenAuthoritativeField, true);
				if (tc.sub_agent_children_authoritative || !tc.sub_agent_children.empty())
				{
					JsonValue children = uam::json::Array();
					for (const SubAgentChild& child : tc.sub_agent_children)
					{
						JsonValue entry = uam::json::Object();
						uam::json::SetString(entry, "id", child.id);
						uam::json::SetString(entry, "title", child.title);
						uam::json::SetString(entry, "status", child.status);
						if (child.started_at_epoch_ms) uam::json::SetNumber(entry, "started_at_epoch_ms", static_cast<double>(*child.started_at_epoch_ms));
						if (child.finished_at_epoch_ms) uam::json::SetNumber(entry, "finished_at_epoch_ms", static_cast<double>(*child.finished_at_epoch_ms));
						if (child.duration_ms) uam::json::SetNumber(entry, "duration_ms", static_cast<double>(*child.duration_ms));
						uam::json::PushValue(children, std::move(entry));
					}
					uam::json::SetValue(tc_obj, kToolCallSubAgentChildrenField, std::move(children));
				}
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
				if (block.compaction_tokens_before >= 0) uam::json::SetNumber(block_obj, "compaction_tokens_before", static_cast<double>(block.compaction_tokens_before));
				if (block.compaction_tokens_after >= 0) uam::json::SetNumber(block_obj, "compaction_tokens_after", static_cast<double>(block.compaction_tokens_after));
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
		if (!msg.provider_event_ids.empty())
		{
			uam::json::SetValue(obj, "provider_event_ids", StringArrayToJson(msg.provider_event_ids));
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
				if (attachment.source == AttachmentSource::LegacyPath && attachment.path.empty())
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
		msg.model_id = JsonStringOrEmpty(obj.Find("model_id"));
		msg.native_input_id = InputProvenanceFromJson(obj.Find("native_input_id"), true);
		if (const auto* completed = obj.Find("input_write_completed"); completed && completed->type == JsonValue::Type::Bool && completed->bool_value && msg.role == MessageRole::User && msg.native_input_id) msg.input_write_completed = true;
		msg.origin_request_id = InputProvenanceFromJson(obj.Find("origin_request_id"), false);
		if (const auto* origin = obj.Find("managed_origin_run_id"); origin && origin->type == JsonValue::Type::String && uam::uuid::IsCanonicalUuid(origin->string_value)) msg.managed_origin_run_id = origin->string_value;
		msg.tokens_input = NonNegativeIntFieldOrZero(obj.Find(kMessageTokensInputField));
		msg.tokens_output = NonNegativeIntFieldOrZero(obj.Find(kMessageTokensOutputField));
		msg.estimated_cost_usd = NonNegativeNumberFieldOrZero(obj.Find(kMessageEstimatedCostUsdField));
		msg.time_to_first_token_ms = NonNegativeIntFieldOrZero(obj.Find(kMessageTimeToFirstTokenMsField));
		msg.processing_time_ms = NonNegativeIntFieldOrZero(obj.Find(kMessageProcessingTimeMsField));
		msg.interrupted = JsonBoolOrDefault(obj.Find(kMessageInterruptedField), false);
		msg.stop_reason = JsonStringOrEmpty(obj.Find("stop_reason"));
		msg.priority_steer = JsonBoolOrDefault(obj.Find(kMessagePrioritySteerField), false);
		msg.continues_turn = JsonBoolOrDefault(obj.Find(kMessageContinuesTurnField), false);
		msg.acp_prompt_not_sent = JsonBoolOrDefault(obj.Find("acp_prompt_not_sent"), false);
		msg.acp_prompt_discarded = JsonBoolOrDefault(obj.Find("acp_prompt_discarded"), false);
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
				tool_call.kind = JsonStringOrEmpty(tc.Find("kind"));
				tool_call.approval_status = JsonStringOrEmpty(tc.Find("approval_status"));
				tool_call.task_id = JsonStringOrEmpty(tc.Find("task_id"));
				tool_call.child_activity = JsonStringOrEmpty(tc.Find("child_activity"));
				tool_call.args_json = JsonStringOrEmpty(tc.Find(kToolCallArgsJsonField));
				tool_call.result_text = JsonStringOrEmpty(tc.Find(kToolCallResultTextField));
				tool_call.status = JsonStringOrEmpty(tc.Find(kToolCallStatusField));
				// Older saves recorded approval without an execution outcome; do not infer pending work.
				if (tool_call.status == "auto_approved")
				{
					tool_call.approval_status = "auto_approved";
					tool_call.status = "unknown";
				}
				tool_call.is_sub_agent = JsonBoolOrDefault(tc.Find(kToolCallIsSubAgentField), false);
				tool_call.sub_agent_id = JsonStringOrEmpty(tc.Find(kToolCallSubAgentIdField));
				tool_call.sub_agent_title = JsonStringOrEmpty(tc.Find(kToolCallSubAgentTitleField));
				tool_call.sub_agent_children_authoritative = JsonBoolOrDefault(tc.Find(kToolCallSubAgentChildrenAuthoritativeField), false);
				if (const JsonValue* children = uam::json::ArrayOrNull(tc.Find(kToolCallSubAgentChildrenField)))
				{
					std::unordered_set<std::string> seen;
					for (const JsonValue& entry : children->array_value)
					{
						if (entry.type != JsonValue::Type::Object) continue;
						SubAgentChild child;
						child.id = JsonStringOrEmpty(entry.Find("id"));
						if (uam::strings::Trim(child.id).empty() || !seen.insert(child.id).second) continue;
						child.title = JsonStringOrEmpty(entry.Find("title"));
						child.status = JsonStringOrEmpty(entry.Find("status"));
						child.started_at_epoch_ms = ChildTimingInteger(entry.Find("started_at_epoch_ms"));
						child.finished_at_epoch_ms = ChildTimingInteger(entry.Find("finished_at_epoch_ms"));
						child.duration_ms = ChildTimingInteger(entry.Find("duration_ms"));
						tool_call.sub_agent_children.push_back(std::move(child));
					}
				}
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
				for (const std::pair<const char*, int64_t*> field : {std::pair{"compaction_tokens_before", &message_block.compaction_tokens_before}, std::pair{"compaction_tokens_after", &message_block.compaction_tokens_after}})
				{
					const double count = JsonNumberOrDefault(block.Find(field.first), -1.0);
					if (std::isfinite(count) && count >= 0.0 && count <= 9'007'199'254'740'991.0 && std::floor(count) == count) *field.second = static_cast<int64_t>(count);
				}
				msg.blocks.push_back(std::move(message_block));
			}
		}

		msg.markdown_store_files = JsonStringArrayOrEmpty(obj.Find(kMessageMarkdownStoreFilesField));
		msg.provider_event_ids = JsonStringArrayOrEmpty(obj.Find("provider_event_ids"));
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

				auto attachment = AttachmentFromJson(item);
				if (!attachment) throw std::length_error("Saved attachment source is invalid.");
				if (attachment->source != AttachmentSource::LegacyPath || !attachment->path.empty())
					msg.attachments.push_back(std::move(*attachment));
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
		return lhs == rhs;
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
		return lhs.id == rhs.id && lhs.name == rhs.name && lhs.kind == rhs.kind && lhs.mime_type == rhs.mime_type && lhs.path == rhs.path && lhs.size_bytes == rhs.size_bytes && lhs.copied == rhs.copied && uam::attachment_sources::MetadataEqual(lhs, rhs);
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
		return lhs.input_write_completed == rhs.input_write_completed && lhs.native_input_id == rhs.native_input_id && lhs.origin_request_id == rhs.origin_request_id && lhs.managed_origin_run_id == rhs.managed_origin_run_id && lhs.role == rhs.role && lhs.content == rhs.content && lhs.created_at == rhs.created_at && lhs.provider == rhs.provider && lhs.model_id == rhs.model_id && lhs.provider_event_ids == rhs.provider_event_ids;
	}

	bool MessageUsageFieldsEquivalentForRecovery(const Message& lhs, const Message& rhs)
	{
		return lhs.tokens_input == rhs.tokens_input && lhs.tokens_output == rhs.tokens_output && lhs.estimated_cost_usd == rhs.estimated_cost_usd;
	}

	bool MessageTimingFieldsEquivalentForRecovery(const Message& lhs, const Message& rhs)
	{
		return lhs.time_to_first_token_ms == rhs.time_to_first_token_ms && lhs.processing_time_ms == rhs.processing_time_ms && lhs.interrupted == rhs.interrupted && lhs.stop_reason == rhs.stop_reason && lhs.priority_steer == rhs.priority_steer && lhs.continues_turn == rhs.continues_turn && lhs.checkpoint_sha == rhs.checkpoint_sha && lhs.checkpoint_parent_sha == rhs.checkpoint_parent_sha && lhs.acp_prompt_not_sent == rhs.acp_prompt_not_sent && lhs.acp_prompt_discarded == rhs.acp_prompt_discarded;
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
		return lhs.id == rhs.id && lhs.revision == rhs.revision && lhs.managed_turn == rhs.managed_turn &&
		       lhs.native_input_id == rhs.native_input_id && lhs.origin_request_id == rhs.origin_request_id &&
		       lhs.text == rhs.text && lhs.uam_agent_id == rhs.uam_agent_id &&
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
		       lhs.existing_user_message_index == rhs.existing_user_message_index &&
		       lhs.existing_user_message_created_at == rhs.existing_user_message_created_at &&
		       lhs.include_prior_context == rhs.include_prior_context &&
		       lhs.direct_admission == rhs.direct_admission &&
		       lhs.repository_baseline_hold == rhs.repository_baseline_hold &&
		       lhs.prepared_for_delivery == rhs.prepared_for_delivery &&
		       lhs.claude_quota_continuation_id == rhs.claude_quota_continuation_id &&
		       lhs.prepared_user_message_count == rhs.prepared_user_message_count &&
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
		       lhs.provider_id == rhs.provider_id && lhs.native_session_id == rhs.native_session_id &&
		       lhs.opencode_v2_session_id == rhs.opencode_v2_session_id &&
		       lhs.managed_parent_input_receipts == rhs.managed_parent_input_receipts && lhs.managed_parent_result == rhs.managed_parent_result &&
		       lhs.managed_followup_incarnation == rhs.managed_followup_incarnation &&
		       lhs.managed_followup_input_unavailable == rhs.managed_followup_input_unavailable;
	}

	bool ChatBranchFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		if (lhs.temporary_parent_chat_id != rhs.temporary_parent_chat_id || lhs.temporary_storage_owner_token != rhs.temporary_storage_owner_token || lhs.provider_handoff_context != rhs.provider_handoff_context || lhs.provider_handoff_session_id != rhs.provider_handoff_session_id ||
		    lhs.provider_handoff_cli_contexts != rhs.provider_handoff_cli_contexts || lhs.side_cleanup_requested != rhs.side_cleanup_requested || lhs.parent_chat_id != rhs.parent_chat_id || lhs.branch_root_chat_id != rhs.branch_root_chat_id)
		{
			return false;
		}

		return lhs.branch_from_message_index == rhs.branch_from_message_index && lhs.branch_message_edited == rhs.branch_message_edited;
	}

	bool ChatDisplayFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		if (lhs.subagent_parent_chat_id != rhs.subagent_parent_chat_id || lhs.subagent_parent_created_at != rhs.subagent_parent_created_at)
		{
			return false;
		}
		if (lhs.folder_id != rhs.folder_id || lhs.title != rhs.title || lhs.custom_icon != rhs.custom_icon || lhs.pinned != rhs.pinned)
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
		if (lhs.workspace_source_directory != rhs.workspace_source_directory || lhs.workspace_base_ref != rhs.workspace_base_ref || lhs.git_base_branch_override != rhs.git_base_branch_override)
		{
			return false;
		}

		return lhs.workspace_branch_name == rhs.workspace_branch_name &&
		       lhs.workspace_worktree_directory == rhs.workspace_worktree_directory &&
		       lhs.svn_update_pending == rhs.svn_update_pending &&
		       lhs.imported_read_only == rhs.imported_read_only;
	}

	bool ChatProviderFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		if (lhs.acp_unsent_prompt_recoveries != rhs.acp_unsent_prompt_recoveries ||
		    lhs.approval_mode != rhs.approval_mode || lhs.uam_agent_id != rhs.uam_agent_id ||
		    lhs.last_prompt_agent_definition_hash != rhs.last_prompt_agent_definition_hash ||
	    lhs.provider_handoff_context != rhs.provider_handoff_context ||
	    lhs.provider_handoff_session_id != rhs.provider_handoff_session_id ||
	    lhs.provider_handoff_cli_contexts != rhs.provider_handoff_cli_contexts ||
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
		       lhs.remote_prompt_image_supported == rhs.remote_prompt_image_supported &&
		       lhs.remote_structured_protocol == rhs.remote_structured_protocol &&
		       lhs.remote_opencode_native_port == rhs.remote_opencode_native_port &&
		       lhs.remote_opencode_native_token == rhs.remote_opencode_native_token &&
		       lhs.remote_manual_compaction_pending == rhs.remote_manual_compaction_pending &&
		       lhs.remote_manual_compaction_boundary_json == rhs.remote_manual_compaction_boundary_json &&
		       lhs.remote_stop_cleanup_pending == rhs.remote_stop_cleanup_pending &&
		       lhs.native_session_reset_pending == rhs.native_session_reset_pending &&
		       lhs.provider_handoff_context == rhs.provider_handoff_context &&
		       lhs.provider_handoff_session_id == rhs.provider_handoff_session_id &&
		       lhs.remote_restart_pending == rhs.remote_restart_pending &&
		       lhs.remote_recovery_enabled == rhs.remote_recovery_enabled &&
		       lhs.remote_recovery_state == rhs.remote_recovery_state &&
		       lhs.routing_mode == rhs.routing_mode &&
		       lhs.routing_inherited_default_fields == rhs.routing_inherited_default_fields &&
		       lhs.routing_source_folder_id == rhs.routing_source_folder_id &&
		       lhs.routing_source_directory == rhs.routing_source_directory &&
		       lhs.routing_assigned_connection_identity == rhs.routing_assigned_connection_identity &&
		       lhs.routing_assigned_provider_configuration_identity == rhs.routing_assigned_provider_configuration_identity &&
		       lhs.routing_reservation_pending == rhs.routing_reservation_pending &&
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
		       lhs.remote_pending_requests == rhs.remote_pending_requests &&
		       lhs.remote_next_request_id == rhs.remote_next_request_id &&
		       lhs.remote_active_turn_id == rhs.remote_active_turn_id &&
		       lhs.remote_turn_serial == rhs.remote_turn_serial &&
		       lhs.remote_turn_user_message_index == rhs.remote_turn_user_message_index &&
		       lhs.acp_queue_full_save_revision == rhs.acp_queue_full_save_revision &&
		       lhs.acp_queue_storage_revision == rhs.acp_queue_storage_revision &&
		       lhs.acp_queue_storage_error == rhs.acp_queue_storage_error &&
		       lhs.acp_queue_held == rhs.acp_queue_held &&
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
		return lhs.interaction_at == rhs.interaction_at && lhs.attention_revision == rhs.attention_revision && lhs.last_stop_reason == rhs.last_stop_reason &&
		       lhs.goal_command_revision == rhs.goal_command_revision &&
		       lhs.goal_pending_continuation_id == rhs.goal_pending_continuation_id &&
		       ChatIdentityFieldsEquivalentForRecovery(lhs, rhs) &&
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
		// Retain recognized primary ownership even when unrelated validation rejects the chat.
		bool managed_followup_input_unavailable = false;
	};

	void ApplyChatTimestampFallbacks(ChatSession& chat, bool legacy_interaction = true);

	std::string SummaryDigest(std::string_view updated_at, std::size_t message_count)
	{
		return std::string(updated_at) + ":" + std::to_string(message_count);
	}

	std::string SummaryDigest(const ChatSession& chat, std::size_t message_count)
	{
		return SummaryDigest(chat.updated_at, message_count);
	}

	bool ChatsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		// Goal binding changes must not be discarded by identical-message recovery.
		if (lhs.goals.size() != rhs.goals.size()) return false;
		for (std::size_t index = 0; index < lhs.goals.size(); ++index)
			if (lhs.goals[index].id != rhs.goals[index].id || lhs.goals[index].native_binding != rhs.goals[index].native_binding) return false;
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

	fs::path QueueJournalPath(const fs::path& data_root, std::string_view chat_id)
	{
		return data_root / "queue-outboxes" / (std::string(chat_id) + ".json");
	}

	std::string QueueOwner(const JsonValue& root)
	{
		JsonValue owner = uam::json::Object();
		for (const std::string_view key : {kChatIdField, kChatCreatedAtField, kChatProviderIdField,
		    kChatExecutionHostIdField, kChatNativeSessionIdField, kChatWorkspaceDirectoryField, kChatWorkspaceWorktreeDirectoryField})
			uam::json::SetString(owner, key, JsonStringOrEmpty(root.Find(key)));
		const std::string birth = JsonStringOrEmpty(root.Find(kChatManagedFollowupBirthField));
		if (!birth.empty()) uam::json::SetString(owner, kChatManagedFollowupBirthField, birth);
		if (const auto* recovery = root.Find("acp_unsent_prompt_recoveries"))
			uam::json::SetValue(owner, "acp_unsent_prompt_recoveries", *recovery);
		return SerializeJson(owner);
	}

	std::string QueueOwner(const ChatSession& chat)
	{
		JsonValue owner = uam::json::Object();
		uam::json::SetString(owner, kChatIdField, chat.id);
		uam::json::SetString(owner, kChatCreatedAtField, chat.created_at);
		uam::json::SetString(owner, kChatProviderIdField, chat.provider_id);
		uam::json::SetString(owner, kChatExecutionHostIdField, uam::strings::NonEmptyOrFallback(chat.execution_host_id, "local"));
		uam::json::SetString(owner, kChatNativeSessionIdField, chat.native_session_id);
		uam::json::SetString(owner, kChatWorkspaceDirectoryField, chat.workspace_directory);
		uam::json::SetString(owner, kChatWorkspaceWorktreeDirectoryField, chat.workspace_worktree_directory);
		if (!chat.managed_followup_incarnation.empty()) uam::json::SetString(owner, kChatManagedFollowupBirthField, chat.managed_followup_incarnation);
		if (!chat.acp_unsent_prompt_recoveries.empty())
			uam::json::SetValue(owner, "acp_unsent_prompt_recoveries", UnsentRecoveriesToJson(chat));
		return SerializeJson(owner);
	}

	struct QueueJournal
	{
		std::string revision;
		std::string base_revision;
		std::string base_queue_revision;
		std::string owner;
		std::vector<uam::AcpQueuedUserPromptState> prompts;
		bool held = false;
		std::size_t dispatched = 0;
		std::optional<std::pair<int, std::string>> memory_progress;
	};

	std::optional<QueueJournal> ReadQueueJournal(const fs::path& path, bool& exists,
	    bool* managed_marker = nullptr, std::string* raw_owner = nullptr)
	{
		std::error_code error;
		const fs::file_status parent = fs::symlink_status(path.parent_path(), error);
		exists = true;
		if (parent.type() == fs::file_type::not_found) { exists = false; return std::nullopt; }
		if (error || !fs::is_directory(parent) || uam::paths::IsLinkOrReparsePointNoThrow(path.parent_path())) return std::nullopt;
		const fs::file_status status = fs::symlink_status(path, error);
		if (status.type() == fs::file_type::not_found) { exists = false; return std::nullopt; }
		std::string text;
		if (error || !fs::is_regular_file(status) || uam::paths::IsLinkOrReparsePointNoThrow(path) ||
		    !uam::io::TryReadTextFile(path, text, kMaxQueueJournalBytes)) return std::nullopt;
		const std::optional<JsonValue> root = ParseJson(text);
		if (!root || root->type != JsonValue::Type::Object || JsonNumberOrDefault(root->Find("version"), 0) != 1) return std::nullopt;
		if (managed_marker) *managed_marker = RawManagedQueueMarker(*root);
		if (raw_owner) *raw_owner = JsonStringOrEmpty(root->Find("owner"));
		QueueJournal journal;
		journal.revision = JsonStringOrEmpty(root->Find("revision"));
		journal.base_revision = JsonStringOrEmpty(root->Find("base_revision"));
		journal.base_queue_revision = JsonStringOrEmpty(root->Find("base_queue_revision"));
		journal.owner = JsonStringOrEmpty(root->Find("owner"));
		const JsonValue* prompts = uam::json::ArrayOrNull(root->Find(kChatAcpQueuedPromptsField));
		const JsonValue* held = root->Find(kChatAcpQueueHeldField);
		const double dispatched = JsonNumberOrDefault(root->Find(kChatAcpDispatchedQueuedPromptCountField), -1);
		if (journal.revision.empty() || journal.revision.size() > 128 || journal.base_revision.empty() ||
		    journal.base_queue_revision.size() > 128 || journal.owner.empty() || prompts == nullptr ||
		    prompts->array_value.size() > kMaxPersistedAcpQueuedPrompts || held == nullptr || held->type != JsonValue::Type::Bool ||
		    !(dispatched >= 0 && dispatched <= prompts->array_value.size()) || dispatched != static_cast<std::size_t>(dispatched)) return std::nullopt;
		journal.held = held->bool_value;
		journal.dispatched = static_cast<std::size_t>(dispatched);
		if (root->Find(kChatMemoryLastProcessedMessageCountField) != nullptr || root->Find(kChatMemoryLastProcessedAtField) != nullptr)
		{
			const double count = JsonNumberOrDefault(root->Find(kChatMemoryLastProcessedMessageCountField), -1);
			const JsonValue* timestamp = root->Find(kChatMemoryLastProcessedAtField);
			if (!(count >= 0 && count <= std::numeric_limits<int>::max()) || std::floor(count) != count ||
			    timestamp == nullptr || timestamp->type != JsonValue::Type::String || timestamp->string_value.size() > 128) return std::nullopt;
			journal.memory_progress = std::make_pair(static_cast<int>(count), timestamp->string_value);
		}
		std::size_t bytes = 0;
		std::unordered_set<std::string> ids;
		for (const JsonValue& value : prompts->array_value)
		{
			std::optional<uam::AcpQueuedUserPromptState> prompt = AcpQueuedPromptFromJson(value, bytes);
			if (!prompt || prompt->id.empty() || !ids.insert(prompt->id).second ||
			    SerializeJson(AcpQueuedPromptToJson(*prompt)) != SerializeJson(value)) return std::nullopt;
			journal.prompts.push_back(std::move(*prompt));
		}
		return journal;
	}

	std::optional<std::string> QueueJournalRevision(const fs::path& path)
	{
		bool exists = false;
		const std::optional<QueueJournal> journal = ReadQueueJournal(path, exists);
		if (journal) return journal->revision;
		if (!exists) return std::string("missing");
		return std::nullopt;
	}

	std::string QueuePayload(const ChatSession& chat)
	{
		JsonValue payload = uam::json::Object();
		JsonValue prompts = uam::json::Array();
		for (const uam::AcpQueuedUserPromptState& prompt : chat.acp_queued_prompts)
			uam::json::PushValue(prompts, AcpQueuedPromptToJson(prompt));
		uam::json::SetValue(payload, kChatAcpQueuedPromptsField, std::move(prompts));
		uam::json::SetBool(payload, kChatAcpQueueHeldField, chat.acp_queue_held);
		uam::json::SetNumber(payload, kChatAcpDispatchedQueuedPromptCountField, static_cast<double>(chat.acp_dispatched_queued_prompt_count));
		uam::json::SetNumber(payload, kChatMemoryLastProcessedMessageCountField, chat.memory_last_processed_message_count);
		uam::json::SetString(payload, kChatMemoryLastProcessedAtField, chat.memory_last_processed_at);
		return SerializeJson(payload, kMaxChatJsonBytes);
	}

	std::string QueuePayload(const JsonValue& root)
	{
		JsonValue payload = uam::json::Object();
		const JsonValue* prompts = root.Find(kChatAcpQueuedPromptsField);
		uam::json::SetValue(payload, kChatAcpQueuedPromptsField, prompts != nullptr ? *prompts : uam::json::Array());
		uam::json::SetBool(payload, kChatAcpQueueHeldField, JsonBoolOrDefault(root.Find(kChatAcpQueueHeldField), false));
		uam::json::SetNumber(payload, kChatAcpDispatchedQueuedPromptCountField, JsonNumberOrDefault(root.Find(kChatAcpDispatchedQueuedPromptCountField), 0));
		uam::json::SetNumber(payload, kChatMemoryLastProcessedMessageCountField, JsonNumberOrDefault(root.Find(kChatMemoryLastProcessedMessageCountField), 0));
		uam::json::SetString(payload, kChatMemoryLastProcessedAtField, JsonStringOrEmpty(root.Find(kChatMemoryLastProcessedAtField)));
		return SerializeJson(payload, kMaxChatJsonBytes);
	}

	std::optional<JsonValue> SavedQueueMetadata(const fs::path& data_root, std::string_view chat_id)
	{
		const fs::path primary = AppPaths::UamChatFilePath(data_root, chat_id);
		const fs::path summary = AppPaths::UamChatSummaryFilePath(data_root, chat_id);
		std::error_code error;
		const std::uintmax_t size = fs::file_size(primary, error);
		if (error) return std::nullopt;
		std::string text;
		if (SummaryCacheIsCurrent(primary, summary) && uam::io::TryReadTextFile(summary, text, 4 * 1024 * 1024))
		{
			std::optional<JsonValue> root = ParseJson(text);
			if (root && root->type == JsonValue::Type::Object && JsonStringOrEmpty(root->Find(kChatIdField)) == chat_id &&
			    NonNegativeUintmaxFieldOrZero(root->Find(kChatSummarySourceSizeField)) == size) return root;
		}
		const std::optional<std::string> revision = StorageRevision(primary);
		const std::optional<uam::chat_pages::Index> index = revision ? uam::chat_pages::ReadIndex(uam::chat_pages::IndexPath(primary), *revision) : std::nullopt;
		if (index)
		{
			std::ifstream input(primary, std::ios::binary);
			if (uam::chat_pages::ReadMetadata(input, *index, text))
			{
				std::optional<JsonValue> root = ParseJson(text);
				if (root && root->type == JsonValue::Type::Object && JsonStringOrEmpty(root->Find(kChatIdField)) == chat_id)
				{
					root->object_value.erase(std::string(kChatMessagesField));
					return root;
				}
			}
		}
		if (!uam::io::TryReadTextFile(primary, text, kMaxChatJsonBytes)) return std::nullopt;
		std::optional<JsonValue> root = ParseJson(text);
		if (!root || root->type != JsonValue::Type::Object || JsonStringOrEmpty(root->Find(kChatIdField)) != chat_id) return std::nullopt;
		root->object_value.erase(std::string(kChatMessagesField));
		return root;
	}

	void ApplyQueueJournal(const fs::path& path, const JsonValue& root, ChatSession& chat)
	{
		chat.acp_queue_storage_revision = JsonStringOrEmpty(root.Find(kQueueRevisionField));
		chat.acp_queue_full_save_revision = JsonStringOrEmpty(root.Find(kQueueFullRevisionField));
		if (path.parent_path().filename() != "chats" && path.parent_path().filename() != "chat-summaries") return;
		const fs::path data_root = path.parent_path().parent_path();
		bool exists = false;
		bool managed_marker = false;
		std::string raw_owner;
		const std::optional<QueueJournal> journal = ReadQueueJournal(QueueJournalPath(data_root, chat.id), exists, &managed_marker, &raw_owner);
		if (!exists) return;
		// A validated consumed revision cannot reintroduce obsolete input or its blocker.
		if (journal && journal->revision == chat.acp_queue_storage_revision) return;
		const std::string owner = QueueOwner(root);
		// Preserve uncertainty for this owner, without importing a known foreign owner's marker.
		if (managed_marker && (raw_owner.empty() || raw_owner == owner))
			chat.managed_followup_input_unavailable = true;
		const bool matches = journal && journal->owner == owner &&
		    journal->base_queue_revision == chat.acp_queue_storage_revision &&
		    ChatRepository::ChatStorageRevision(data_root, chat.id) == journal->base_revision;
		if (matches)
		{
			chat.acp_queued_prompts = journal->prompts;
			chat.acp_queue_held = journal->held;
			chat.acp_dispatched_queued_prompt_count = journal->dispatched;
			chat.acp_queue_storage_revision = journal->revision;
			if (journal->memory_progress)
			{
				chat.memory_last_processed_message_count = journal->memory_progress->first;
				chat.memory_last_processed_at = journal->memory_progress->second;
			}
			return;
		}
		if (journal && journal->owner == owner)
		{
			chat.acp_queued_prompts = journal->prompts;
			chat.acp_dispatched_queued_prompt_count = journal->dispatched;
			chat.acp_queue_storage_revision = journal->revision;
		}
		chat.acp_queue_held = true;
		chat.acp_queue_storage_error = true;
		uam::diagnostics::Write("Queued inputs held: journal is unreadable or does not match saved chat " + chat.id);
	}

	LoadChatResult ParseLocalChatDocument(const fs::path& path, std::string file_text, bool include_messages, std::uintmax_t expected_source_size, bool publish_summary_cache, std::size_t max_read_bytes,
	    bool* recognized_managed_input = nullptr, bool apply_queue_journal = true)
	try
	{
		auto root_opt = ParseJson(file_text);
		if (!root_opt || root_opt->type != JsonValue::Type::Object)
		{
			return {std::nullopt, "contains invalid JSON"};
		}

		JsonValue& root = *root_opt;
		if (recognized_managed_input)
		{
			const std::string id = JsonStringOrEmpty(root.Find(kChatIdField));
			const fs::path identity_path = path.extension() == ".bak" ? path.stem() : path;
			*recognized_managed_input = uam::chat_ids::IsSafeStorageChatId(id) &&
			    id == identity_path.stem().string() && RawManagedQueueMarker(root) &&
			    !JsonBoolOrDefault(root.Find(kChatImportedReadOnlyField), false);
		}
		if (!NativeQueueDocumentRepresentable(root)) return {std::nullopt, "contains unrepresentable native queued attachments"};
		if (max_read_bytes != 0)
		{
			// Cleanup cannot treat malformed ownership as an unreferenced workspace.
			for (const std::string_view field : {kChatExecutionHostIdField, kChatWorkspaceDirectoryField, kChatWorkspaceWorktreeDirectoryField, kChatWorkspaceSourceDirectoryField})
			{
				const JsonValue* value = root.Find(field);
				if (value != nullptr && value->type != JsonValue::Type::String) return {std::nullopt, "contains invalid workspace ownership"};
			}
		}
		if (expected_source_size > 0 &&
		    NonNegativeUintmaxFieldOrZero(root.Find(kChatSummarySourceSizeField)) != expected_source_size)
		{
			return {std::nullopt, "is a stale summary cache"};
		}
		ChatSession chat;
		chat.managed_followup_input_unavailable = RawManagedQueueMarker(root) &&
		    !JsonBoolOrDefault(root.Find(kChatImportedReadOnlyField), false);

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
		if (root.Find(kChatOpenCodeV2SessionIdField) != nullptr)
		{
			const std::string origin = JsonStringOrEmpty(root.Find(kChatOpenCodeV2SessionIdField));
			chat.opencode_v2_session_id = origin.starts_with("ses_") && origin.size() <= 512 && uam::execution_hosts::IsPortableId(origin) ? origin : "";
		}
		if (const JsonValue* quota = root.Find("claude_quota_continuation"); quota != nullptr && quota->type == JsonValue::Type::Object)
		{
			ClaudeQuotaContinuation& owner = chat.claude_quota_continuation;
			owner.id = JsonStringOrEmpty(quota->Find("id"));
			owner.state = JsonStringOrEmpty(quota->Find("state"));
			owner.chat_id = JsonStringOrEmpty(quota->Find("chat_id"));
			owner.native_session_id = JsonStringOrEmpty(quota->Find("native_session_id"));
			owner.context = JsonStringOrEmpty(quota->Find("context"));
			owner.accepted_message_digest = JsonStringOrEmpty(quota->Find("accepted_message_digest"));
			owner.agent_id = JsonStringOrEmpty(quota->Find("agent_id"));
			owner.agent_definition_hash = JsonStringOrEmpty(quota->Find("agent_definition_hash"));
			owner.agent_workspace_access = JsonStringOrEmpty(quota->Find("agent_workspace_access"));
			owner.agent_execution_capability = JsonStringOrEmpty(quota->Find("agent_execution_capability"));
			const double observed = JsonNumberOrDefault(quota->Find("observed_at_epoch_s"), 0);
			const double reset = JsonNumberOrDefault(quota->Find("resets_at_epoch_s"), 0);
			const double index = JsonNumberOrDefault(quota->Find("user_message_index"), -1);
			const double count = JsonNumberOrDefault(quota->Find("message_count"), 0);
			if (observed > 0 && observed <= 253402282739 && std::floor(observed) == observed &&
			    reset > observed && reset <= observed + 18060 && std::floor(reset) == reset &&
			    index >= 0 && index <= std::numeric_limits<int>::max() && std::floor(index) == index &&
			    count > index && count <= std::numeric_limits<int>::max() && std::floor(count) == count &&
			    !owner.id.empty() && owner.id.size() <= 128 && owner.context.size() <= 32768 && owner.accepted_message_digest.size() == 16 &&
			    owner.chat_id == chat.id && owner.native_session_id == chat.native_session_id && chat.provider_id == uam::provider_ids::kClaudeCli)
			{
				owner.observed_at_epoch_s = static_cast<int64_t>(observed);
				owner.resets_at_epoch_s = static_cast<int64_t>(reset);
				owner.user_message_index = static_cast<int>(index);
				owner.message_count = static_cast<std::size_t>(count);
				// A crash between claiming and dispatch cannot prove whether input reached Claude.
				if (owner.state != "waiting" && owner.state != "cancelled" && owner.state != "completed") owner.state = "held";
			}
			else owner = {};
		}
		chat.remote_claude_session_unstarted = JsonBoolOrDefault(
		    root.Find(kChatRemoteClaudeSessionUnstartedField), false);
		chat.remote_turn_reconnect_pending = JsonBoolOrDefault(
		    root.Find(kChatRemoteTurnReconnectPendingField), false);
		chat.remote_process_exists = JsonBoolOrDefault(
		    root.Find(kChatRemoteProcessExistsField),
		    chat.remote_turn_reconnect_pending);
		chat.remote_prompt_image_supported = chat.remote_process_exists && JsonBoolOrDefault(root.Find("remote_prompt_image_supported"), false);
		chat.remote_structured_protocol = chat.remote_process_exists ? JsonStringOrEmpty(root.Find("remote_structured_protocol")) : "";
		if (chat.remote_structured_protocol.size() > 64) return {std::nullopt, "contains invalid remote protocol identity"};
		const double native_port = JsonNumberOrDefault(root.Find("remote_opencode_native_port"), 0);
		chat.remote_opencode_native_port = native_port >= 1 && native_port <= 65535 && std::floor(native_port) == native_port ? static_cast<int>(native_port) : 0;
		chat.remote_opencode_native_token = JsonStringOrEmpty(root.Find("remote_opencode_native_token"));
		if (!chat.remote_process_exists || chat.remote_opencode_native_port == 0 || chat.remote_opencode_native_token.size() < 16 ||
		    chat.remote_opencode_native_token.size() > 256 || std::ranges::any_of(chat.remote_opencode_native_token, [](unsigned char ch) { return ch < 33 || ch > 126; }))
		{
			chat.remote_opencode_native_port = 0;
			chat.remote_opencode_native_token.clear();
		}
		chat.remote_manual_compaction_pending = chat.remote_process_exists && JsonBoolOrDefault(root.Find("remote_manual_compaction_pending"), false);
		chat.remote_manual_compaction_boundary_json = JsonStringOrEmpty(root.Find("remote_manual_compaction_boundary_json"));
		if (chat.remote_manual_compaction_boundary_json.size() > 2 * 1024 * 1024) return {std::nullopt, "contains oversized remote compaction evidence"};
		chat.remote_stop_cleanup_pending = JsonBoolOrDefault(
		    root.Find(kChatRemoteStopCleanupPendingField), false);
		chat.native_session_reset_pending = JsonBoolOrDefault(root.Find("native_session_reset_pending"), false);
		chat.provider_handoff_context = JsonStringOrEmpty(root.Find("provider_handoff_context"));
		chat.provider_handoff_session_id = JsonStringOrEmpty(root.Find("provider_handoff_session_id"));
		chat.remote_recovery_enabled = JsonBoolOrDefault(root.Find("remote_recovery_enabled"), false);
		chat.remote_recovery_state = JsonStringOrEmpty(root.Find("remote_recovery_state"));
		chat.routing_mode = JsonStringOrEmpty(root.Find("routing_mode"));
		chat.routing_inherited_default_fields = JsonStringArrayOrEmpty(root.Find("routing_inherited_default_fields"));
		chat.routing_source_folder_id = JsonStringOrEmpty(root.Find("routing_source_folder_id"));
		chat.routing_source_directory = JsonStringOrEmpty(root.Find("routing_source_directory"));
		chat.routing_assigned_connection_identity = JsonStringOrEmpty(root.Find("routing_assigned_connection_identity"));
		chat.routing_assigned_provider_configuration_identity = JsonStringOrEmpty(root.Find("routing_assigned_provider_configuration_identity"));
		if (chat.routing_mode != "automatic") chat.routing_mode = "explicit";
		chat.routing_reservation_pending = JsonBoolOrDefault(root.Find("routing_reservation_pending"), false);
		chat.remote_restart_pending = JsonBoolOrDefault(
		    root.Find(kChatRemoteRestartPendingField), false);
		if (root.Find(kChatRemoteProcessExistsField) == nullptr)
			chat.remote_process_exists = chat.remote_turn_reconnect_pending ||
			    chat.remote_stop_cleanup_pending || chat.remote_restart_pending;
		chat.remote_process_control_token = JsonStringOrEmpty(
		    root.Find(kChatRemoteProcessControlTokenField));
		if (chat.remote_process_control_token.size() > 256)
			chat.remote_process_control_token.clear();
		if (!chat.remote_manual_compaction_pending && !HasOwnedClaudeMaintenanceTerminalReceipt(chat))
			chat.remote_manual_compaction_boundary_json.clear();
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
		chat.remote_active_turn_id = JsonStringOrEmpty(root.Find(kChatRemoteActiveTurnIdField));
		if (chat.remote_active_turn_id.size() > 256) chat.remote_active_turn_id.clear();
		const double turn_serial = JsonNumberOrDefault(root.Find(kChatRemoteTurnSerialField), 0);
		if (turn_serial >= 0 && turn_serial <= std::numeric_limits<int>::max() &&
		    turn_serial == static_cast<int>(turn_serial))
			chat.remote_turn_serial = static_cast<int>(turn_serial);
		const double turn_user_index = JsonNumberOrDefault(root.Find(kChatRemoteTurnUserMessageIndexField), -1);
		if (turn_user_index >= -1 && turn_user_index <= std::numeric_limits<int>::max() &&
		    turn_user_index == static_cast<int>(turn_user_index))
			chat.remote_turn_user_message_index = static_cast<int>(turn_user_index);
		const double next_request_id = JsonNumberOrDefault(root.Find(kChatRemoteNextRequestIdField), 1);
		if (next_request_id >= 1 && next_request_id < std::numeric_limits<int>::max() &&
		    next_request_id == static_cast<int>(next_request_id))
			chat.remote_next_request_id = static_cast<int>(next_request_id);
		if (const JsonValue* requests = uam::json::ArrayOrNull(root.Find(kChatRemotePendingRequestsField)))
		{
			std::size_t payload_bytes = 0;
			for (const JsonValue& item : requests->array_value)
			{
				if (chat.remote_pending_requests.size() >= kMaxPersistedRemotePendingRequests) break;
				if (item.type != JsonValue::Type::Object) continue;
				uam::AcpRemotePendingRequestState request;
				const double request_id = JsonNumberOrDefault(item.Find("request_id"), 0);
				if (!(request_id >= 1 && request_id < std::numeric_limits<int>::max()) ||
				    request_id != static_cast<int>(request_id)) continue;
				request.request_id = static_cast<int>(request_id);
				request.method = JsonStringOrEmpty(item.Find("method"));
				request.delivery_id = JsonStringOrEmpty(item.Find("delivery_id"));
				request.payload = JsonStringOrEmpty(item.Find("payload"));
				const double user_index = JsonNumberOrDefault(item.Find("user_message_index"), -1);
				const double turn_serial = JsonNumberOrDefault(item.Find("turn_serial"), 0);
				if (!(user_index >= -1 && user_index <= std::numeric_limits<int>::max()) ||
				    !(turn_serial >= 0 && turn_serial <= std::numeric_limits<int>::max()) ||
				    user_index != static_cast<int>(user_index) ||
				    turn_serial != static_cast<int>(turn_serial)) continue;
				request.user_message_index = static_cast<int>(user_index);
				request.turn_serial = static_cast<int>(turn_serial);
				request.provider_turn_id = JsonStringOrEmpty(item.Find("provider_turn_id"));
				request.response_consumed = JsonBoolOrDefault(item.Find("response_consumed"), false);
				if (!ValidRemotePendingRequest(request, payload_bytes) ||
				    std::any_of(chat.remote_pending_requests.begin(), chat.remote_pending_requests.end(),
				        [&request](const uam::AcpRemotePendingRequestState& saved)
				        { return saved.request_id == request.request_id; })) continue;
				payload_bytes += request.payload.size();
				chat.remote_pending_requests.push_back(std::move(request));
			}
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
		chat.acp_queue_held = JsonBoolOrDefault(root.Find(kChatAcpQueueHeldField), false);
		// Legacy identities are deterministic so repeated hydration and recovery agree.
		std::unordered_set<std::string> queued_ids;
		for (std::size_t index = 0; index < chat.acp_queued_prompts.size(); ++index)
		{
			uam::AcpQueuedUserPromptState& prompt = chat.acp_queued_prompts[index];
			if (!prompt.id.empty() && queued_ids.insert(prompt.id).second) continue;
			prompt.id = "legacy-queue-" + chat.id + "-" + std::to_string(index);
			while (!queued_ids.insert(prompt.id).second) prompt.id += "-legacy";
		}
		chat.acp_dispatched_queued_prompt_count = std::min<std::uintmax_t>(
		    NonNegativeUintmaxFieldOrZero(root.Find(kChatAcpDispatchedQueuedPromptCountField)),
		    chat.acp_queued_prompts.size());
		chat.temporary_parent_chat_id = JsonStringOrEmpty(root.Find("temporary_parent_chat_id"));
		chat.temporary_storage_owner_token = JsonStringOrEmpty(root.Find("temporary_storage_owner_token"));
		chat.side_cleanup_requested = JsonBoolOrDefault(root.Find("side_cleanup_requested"), false);
		chat.parent_chat_id = JsonStringOrEmpty(root.Find(kChatParentChatIdField));
		chat.branch_root_chat_id = JsonStringOrEmpty(root.Find(kChatBranchRootChatIdField));
		chat.subagent_parent_chat_id = JsonStringOrEmpty(root.Find(kChatSubagentParentChatIdField));
		chat.subagent_parent_created_at = JsonStringOrEmpty(root.Find(kChatSubagentParentCreatedAtField));
		chat.branch_from_message_index = IntFieldAtLeastOrDefault(root.Find(kChatBranchFromMessageIndexField), -1, -1);
		chat.branch_message_edited = JsonBoolOrDefault(root.Find(kChatBranchMessageEditedField), false);
		chat.branch_operation_id = JsonStringOrEmpty(root.Find(kChatBranchOperationIdField));
		chat.branch_retry_error = JsonStringOrEmpty(root.Find(kChatBranchRetryErrorField));
		chat.folder_id = JsonStringOrEmpty(root.Find(kChatFolderIdField));
		std::erase_if(chat.folder_id, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
		chat.title = JsonStringOrEmpty(root.Find(kChatTitleField));
		chat.custom_icon = JsonCustomIconOrEmpty(root.Find(kChatCustomIconField));
		chat.created_at = JsonStringOrEmpty(root.Find(kChatCreatedAtField));
		chat.updated_at = JsonStringOrEmpty(root.Find(kChatUpdatedAtField));
		chat.last_opened_at = JsonStringOrEmpty(root.Find(kChatLastOpenedAtField));
		chat.interaction_at = JsonStringOrEmpty(root.Find("interaction_at"));
		chat.attention_revision = JsonStringOrEmpty(root.Find("attention_revision"));
		chat.settled_at = JsonStringOrEmpty(root.Find("settled_at"));
		chat.snoozed_until_ms = NonNegativeInt64FieldOrZero(root.Find("snoozed_until_ms"));
		chat.last_stop_reason = JsonStringOrEmpty(root.Find("last_stop_reason"));
		chat.goal_command_revision = JsonStringOrEmpty(root.Find("goal_command_revision"));
		chat.goal_pending_continuation_id = JsonStringOrEmpty(root.Find("goal_pending_continuation_id"));
		chat.pinned = JsonBoolOrDefault(root.Find(kChatPinnedField), false);
		chat.linked_files = JsonStringArrayOrEmpty(root.Find(kChatLinkedFilesField));
		chat.workspace_directory = JsonStringOrEmpty(root.Find(kChatWorkspaceDirectoryField));
		chat.workspace_isolation_kind = JsonStringOrEmpty(root.Find(kChatWorkspaceIsolationKindField));
		chat.workspace_source_directory = JsonStringOrEmpty(root.Find(kChatWorkspaceSourceDirectoryField));
		chat.workspace_base_ref = JsonStringOrEmpty(root.Find(kChatWorkspaceBaseRefField));
		const JsonValue* base_override = root.Find(kChatGitBaseBranchOverrideField);
		if (base_override && base_override->type != JsonValue::Type::Null)
		{
			if (base_override->type != JsonValue::Type::String || base_override->string_value.size() > 255 ||
			    std::ranges::any_of(base_override->string_value, [](unsigned char c) { return c < 32 || c == 127; }))
				return {std::nullopt, "contains an invalid Git base branch override"};
			chat.git_base_branch_override = uam::strings::Trim(base_override->string_value);
		}
		chat.workspace_branch_name = JsonStringOrEmpty(root.Find(kChatWorkspaceBranchNameField));
		chat.workspace_worktree_directory = JsonStringOrEmpty(root.Find(kChatWorkspaceWorktreeDirectoryField));
		chat.svn_update_pending = JsonBoolOrDefault(root.Find(kChatSvnUpdatePendingField), false);
		chat.imported_read_only = JsonBoolOrDefault(root.Find(kChatImportedReadOnlyField), false);
		if (!chat.imported_read_only)
			chat.acp_unsent_prompt_recoveries = UnsentRecoveriesFromJson(root.Find("acp_unsent_prompt_recoveries"));
		if (chat.imported_read_only)
		{
			chat.passive_queued_prompt_archive = PassiveArchiveFromText(file_text, chat.id);
			if (!chat.passive_queued_prompt_archive) return {std::nullopt, "contains invalid passive queued archive"};
		}
		if (chat.imported_read_only ||
		    chat.execution_host_id == uam::execution_hosts::kLocalHostId)
		{
			chat.remote_turn_reconnect_pending = false;
			chat.remote_process_exists = false;
			chat.remote_structured_protocol.clear();
			chat.remote_prompt_image_supported = false;
			chat.remote_opencode_native_port = 0;
			chat.remote_opencode_native_token.clear();
			chat.remote_manual_compaction_pending = false;
			chat.remote_manual_compaction_boundary_json.clear();
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
			chat.remote_pending_requests.clear();
			chat.remote_next_request_id = 1;
			chat.remote_active_turn_id.clear();
			chat.remote_turn_serial = 0;
			chat.remote_turn_user_message_index = -1;
			if (chat.imported_read_only) chat.acp_queue_held = false;
			if (chat.imported_read_only) chat.acp_queued_prompts.clear();
			if (chat.imported_read_only) chat.acp_dispatched_queued_prompt_count = 0;
		}
		chat.approval_mode = JsonStringOrEmpty(root.Find(kChatApprovalModeField));
		chat.uam_agent_id = uam::strings::NonEmptyOrFallback(JsonStringOrEmpty(root.Find(kChatUamAgentIdField)), "build");
		chat.last_prompt_agent_definition_hash = JsonStringOrEmpty(root.Find(kChatLastPromptAgentDefinitionHashField));
	chat.provider_handoff_context = JsonStringOrEmpty(root.Find("provider_handoff_context"));
	chat.provider_handoff_session_id = JsonStringOrEmpty(root.Find("provider_handoff_session_id"));
	if (const JsonValue* contexts = uam::json::ArrayOrNull(root.Find("provider_handoff_cli_contexts")))
	{
		for (const JsonValue& context : contexts->array_value)
		{
			const std::string host = JsonStringOrEmpty(context.Find("host"));
			const std::string directory = JsonStringOrEmpty(context.Find("directory"));
			if (!host.empty() && !directory.empty()) chat.provider_handoff_cli_contexts.push_back({host, directory, JsonStringOrEmpty(context.Find("connection"))});
		}
	}
		chat.agent_run_id = JsonStringOrEmpty(root.Find(kChatAgentRunIdField));
		if (const auto* birth = root.Find(kChatManagedFollowupBirthField))
		{
			if (birth->type == JsonValue::Type::String &&
			    (birth->string_value.empty() || uam::uuid::IsCanonicalUuid(birth->string_value)))
				chat.managed_followup_incarnation = birth->string_value;
			else if (!chat.managed_followup_input_unavailable)
				return {std::nullopt, "contains invalid managed birth identity"};
		}
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
		chat.memory_level = uam::memory_levels::NormalizeSelection(JsonStringOrEmpty(root.Find(kChatMemoryLevelField)), chat.memory_enabled);
		chat.memory_enabled = uam::memory_levels::IsEnabled(chat.memory_level);
		chat.memory_last_processed_message_count = NonNegativeIntFieldOrZero(root.Find(kChatMemoryLastProcessedMessageCountField));
		chat.memory_last_processed_at = JsonStringOrEmpty(root.Find(kChatMemoryLastProcessedAtField));
		chat.small_model_mode = JsonBoolOrDefault(root.Find(kChatSmallModelModeField), false);
		chat.uam_control_enabled = JsonBoolOrDefault(root.Find("uamControlEnabled"), false);
		chat.uam_control_explicitly_disabled = JsonBoolOrDefault(root.Find("uamControlExplicitlyDisabled"), false);

		if (const JsonValue* values = root.Find("managedParentInputReceipts"))
		{
			chat.managed_parent_input_receipts_present = true;
			bool valid = values->type == JsonValue::Type::Array;
			if (valid)
				for (const auto& value : values->array_value)
				{
					if (value.type != JsonValue::Type::Object) { valid = false; break; }
					const auto is_string = [&](const char* name)
					{
						const auto* field = value.Find(name); return field && field->type == JsonValue::Type::String;
					};
					if (!std::ranges::all_of(std::array{"id", "originId", "rootId", "providerId", "hostId", "workspace", "nativeThread", "originTurn", "payload", "nativeRequestId", "acceptedTurnId"}, is_string))
					{ valid = false; break; }
					const auto* run_ids = value.Find("runIds");
					if (!run_ids || run_ids->type != JsonValue::Type::Array ||
					    !std::ranges::all_of(run_ids->array_value, [](const JsonValue& id) { return id.type == JsonValue::Type::String; }))
					{ valid = false; break; }
					ManagedParentInputReceipt receipt;
					receipt.version = NativeGoalBindingInteger(value.Find("version")) == 1 ? 1 : 0;
					receipt.id = JsonStringOrEmpty(value.Find("id"));
					receipt.origin_id = JsonStringOrEmpty(value.Find("originId"));
					receipt.root_id = JsonStringOrEmpty(value.Find("rootId"));
					receipt.provider_id = JsonStringOrEmpty(value.Find("providerId"));
					receipt.host_id = JsonStringOrEmpty(value.Find("hostId"));
					receipt.workspace = JsonStringOrEmpty(value.Find("workspace"));
					receipt.native_thread = JsonStringOrEmpty(value.Find("nativeThread"));
					receipt.origin_turn = JsonStringOrEmpty(value.Find("originTurn"));
					receipt.payload = JsonStringOrEmpty(value.Find("payload"));
					receipt.native_request_id = JsonStringOrEmpty(value.Find("nativeRequestId"));
					receipt.accepted_turn_id = JsonStringOrEmpty(value.Find("acceptedTurnId"));
					receipt.run_ids = JsonStringArrayOrEmpty(value.Find("runIds"));
					chat.managed_parent_input_receipts.push_back(std::move(receipt));
				}
			if (!valid || !uam::managed_parent_history::ValidCollection(chat.managed_parent_input_receipts))
			{
				// Invalid DATA is not silently downgraded to a missing proof or resaved as an empty list.
				ManagedParentInputReceipt unavailable;
				unavailable.version = 0;
				chat.managed_parent_input_receipts.push_back(std::move(unavailable));
			}
		}

		if (const JsonValue* value = root.Find("managedParentResult"))
		{
			ManagedParentResultRecord record; record.version = 0; record.stage = "held";
			const auto ids = JsonStringArrayOrEmpty(value->Find("runIds"));
			const auto delivered = JsonStringArrayOrEmpty(value->Find("deliveredIds"));
			const auto batch = JsonStringArrayOrEmpty(value->Find("batchIds"));
			const std::string payload = JsonStringOrEmpty(value->Find("payload"));
			if (value->type == JsonValue::Type::Object && NativeGoalBindingInteger(value->Find("version")) == 1 &&
			    ids.size() <= 4 && delivered.size() <= 4 && batch.size() <= 4 && payload.size() <= 72 * 1024)
			{
				record.root_id = JsonStringOrEmpty(value->Find("rootId"));
				record.provider_id = JsonStringOrEmpty(value->Find("providerId"));
				record.host_id = JsonStringOrEmpty(value->Find("hostId"));
				record.workspace = JsonStringOrEmpty(value->Find("workspace"));
				record.native_thread = JsonStringOrEmpty(value->Find("nativeThread"));
				record.policy_digest = JsonStringOrEmpty(value->Find("policyDigest"));
				record.version = 1; record.id = JsonStringOrEmpty(value->Find("id"));
				record.stage = JsonStringOrEmpty(value->Find("stage")); record.reason = JsonStringOrEmpty(value->Find("reason"));
				record.origin_turn = JsonStringOrEmpty(value->Find("originTurn"));
				record.origin_user_digest = JsonStringOrEmpty(value->Find("originUserDigest"));
				record.run_ids = ids; record.delivered_ids = delivered; record.batch_ids = batch; record.payload = payload;
				record.native_request_id = JsonStringOrEmpty(value->Find("nativeRequestId"));
				record.accepted_turn_id = JsonStringOrEmpty(value->Find("acceptedTurnId"));
			}
			const std::vector<std::string> stages{"collecting", "admitted", "write-unconfirmed", "native-accepted", "settled", "held"};
			const auto bounded_ids = [](const std::vector<std::string>& values)
			{
				std::unordered_set<std::string> unique;
				return std::ranges::all_of(values, [&unique](const auto& id) { return !id.empty() && id.size() <= 256 && unique.insert(id).second; });
			};
			if (record.id.empty() || record.id.size() > 256 || record.root_id != chat.id || record.provider_id != chat.provider_id ||
			    record.host_id != chat.execution_host_id || record.workspace.size() > 16384 || record.native_thread.size() > 256 ||
			    record.policy_digest.size() != 64 || record.origin_user_digest.size() != 64 || record.origin_turn.size() > 256 ||
			    record.reason.size() > 128 || record.native_request_id.size() > 32 || record.accepted_turn_id.size() > 256 ||
			    std::ranges::find(stages, record.stage) == stages.end() || !bounded_ids(record.run_ids) ||
			    !bounded_ids(record.delivered_ids) || !bounded_ids(record.batch_ids)) record.version = 0;
			chat.managed_parent_result = std::move(record);
			// Normalize ONLY an absent-key, authoritative FULL primary's strictly bound legacy DATA.
			// Summary caches, present empty/invalid collections and exports cannot mint this migration.
			const auto* primary_messages = uam::json::ArrayOrNull(root.Find("messages"));
			if (!chat.managed_parent_input_receipts_present && include_messages && expected_source_size == 0 && primary_messages &&
			    chat.managed_parent_result->version == 1)
			{
				const auto& current = *chat.managed_parent_result;
				ManagedParentInputReceipt legacy;
				legacy.origin_id = current.id; legacy.root_id = current.root_id;
				legacy.provider_id = current.provider_id; legacy.host_id = current.host_id;
				legacy.workspace = current.workspace; legacy.native_thread = current.native_thread;
				legacy.origin_turn = current.origin_turn; legacy.payload = current.payload;
				legacy.native_request_id = current.native_request_id; legacy.accepted_turn_id = current.accepted_turn_id;
				legacy.run_ids = current.batch_ids;
				uam::platform::IncrementalSha256 hash;
				bool hashed = true;
				for (const auto& field : std::vector<std::string>{"uam-parent-input-legacy-v1", legacy.root_id, legacy.native_thread,
				    legacy.accepted_turn_id, legacy.origin_id, legacy.native_request_id})
					hashed = hashed && hash.Add(std::to_string(field.size()) + ":") && hash.Add(field);
				for (const auto& id : legacy.run_ids)
					hashed = hashed && hash.Add(std::to_string(id.size()) + ":") && hash.Add(id);
				legacy.id = hashed ? "legacy-" + hash.Finish() : std::string{};
				const bool bound_stage = current.stage == "native-accepted" || current.stage == "settled" || current.stage == "held";
				const bool batch_owned = std::ranges::all_of(current.batch_ids, [&](const std::string& id)
				    { return std::ranges::find(current.run_ids, id) != current.run_ids.end(); });
				if (bound_stage && batch_owned && !legacy.native_request_id.empty() && !legacy.accepted_turn_id.empty() &&
				    legacy.accepted_turn_id != legacy.origin_turn && legacy.root_id == chat.id && legacy.provider_id == chat.provider_id &&
				    legacy.host_id == chat.execution_host_id && legacy.native_thread == chat.native_session_id &&
				    legacy.workspace == chat.workspace_directory && uam::managed_parent_history::Bound(legacy))
				{
					chat.managed_parent_input_receipts.push_back(std::move(legacy));
					chat.managed_parent_input_receipts_present = true;
				}
			}
		}

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
				if (const JsonValue* binding = goal_obj.Find("nativeBinding"); binding != nullptr)
				{
					NativeGoalBinding receipt;
					receipt.version = 0; // Invalid records remain a hold marker rather than legacy unbound intent.

					const std::optional<std::int64_t> version = NativeGoalBindingInteger(binding->Find("version"));
					const std::optional<std::int64_t> created = NativeGoalBindingInteger(binding->Find("createdAt"));
					const std::optional<std::int64_t> updated = NativeGoalBindingInteger(binding->Find("updatedAt"));
					const std::optional<std::int64_t> baseline = NativeGoalBindingInteger(binding->Find("uamTokensAtCreation"));
					const std::optional<std::int64_t> ceiling = NativeGoalBindingInteger(binding->Find("uamBudgetAtCreation"));
					const std::optional<std::int64_t> used = NativeGoalBindingInteger(binding->Find("nativeTokensUsed"));
					const JsonValue* native_budget = binding->Find("nativeTokenBudget");
					if (binding->type == JsonValue::Type::Object && version == 1 && created && updated && baseline && ceiling && used &&
						native_budget != nullptr && (native_budget->type == JsonValue::Type::Null || NativeGoalBindingInteger(native_budget)))
					{
						receipt.version = 1;
						receipt.provider_id = JsonStringOrEmpty(binding->Find("providerId"));
						receipt.execution_host_id = JsonStringOrEmpty(binding->Find("executionHostId"));
						receipt.thread_id = JsonStringOrEmpty(binding->Find("threadId"));
						receipt.uam_chat_id = JsonStringOrEmpty(binding->Find("uamChatId"));
						receipt.uam_goal_id = JsonStringOrEmpty(binding->Find("uamGoalId"));
						receipt.objective = JsonStringOrEmpty(binding->Find("objective"));
						receipt.created_at = *created;
						receipt.last_updated_at = *updated;
						receipt.uam_tokens_at_creation = *baseline;
						receipt.uam_budget_at_creation = *ceiling;
						receipt.last_native_tokens_used = *used;
						receipt.native_token_budget = NativeGoalBindingInteger(native_budget);
					}
					if (const JsonValue* delivery = binding->Find("initialDelivery"); delivery != nullptr)
					{
						const std::optional<std::int64_t> index = NativeGoalBindingInteger(delivery->Find("userMessageIndex"));
						const std::string timestamp = JsonStringOrEmpty(delivery->Find("userMessageCreatedAt"));
						const std::string state = JsonStringOrEmpty(delivery->Find("state"));
						const JsonValue* turn = delivery->Find("turnId");
						const std::string turn_id = JsonStringOrEmpty(turn);
						if (delivery->type != JsonValue::Type::Object || !index || *index < 0 || *index > std::numeric_limits<int>::max() ||
							timestamp.empty() || turn == nullptr || turn->type != JsonValue::Type::String ||
							(state != "pending" && state != "attempted" && state != "accepted") ||
							((state == "accepted") != !turn_id.empty())) receipt.version = 0;
						else receipt.initial_delivery = NativeGoalInitialDelivery{*index, timestamp,
							state == "pending" ? NativeGoalInitialDelivery::State::Pending :
							state == "attempted" ? NativeGoalInitialDelivery::State::Attempted : NativeGoalInitialDelivery::State::Accepted, turn_id};
					}
					goal.native_binding = std::move(receipt);
				}
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

		// Older bundle imports may still carry source-owned temporary cleanup metadata.
		if (chat.imported_read_only)
		{
			chat.temporary_parent_chat_id.clear();
			chat.temporary_storage_owner_token.clear();
			chat.side_cleanup_requested = false;
			chat.provider_handoff_context.clear();
			chat.provider_handoff_session_id.clear();
			chat.provider_handoff_cli_contexts.clear();
		}

		ApplyChatTimestampFallbacks(chat, root.Find("interaction_at") == nullptr);
		uam::provider_storage::NormalizeLoadedNativeSessionId(chat);
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
		if (expected_source_size > 0 &&
		    chat.persisted_messages_digest != SummaryDigest(chat, chat.persisted_message_count))
		{
			// Saves digest the stored timestamp; the load fallbacks above may fill an empty one.
			// Report the filled form a primary load derives instead of rejecting and rewriting the cache.
			if (chat.persisted_messages_digest != SummaryDigest(JsonStringOrEmpty(root.Find(kChatUpdatedAtField)), chat.persisted_message_count))
				return {std::nullopt, "contains a stale message digest"};
			chat.persisted_messages_digest = SummaryDigest(chat, chat.persisted_message_count);
		}

		if (publish_summary_cache && !include_messages && expected_source_size == 0 && path.extension() == ".json")
		{
			root.object_value.erase(std::string(kChatMessagesField));
			uam::json::SetNumber(root, kChatPersistedMessageCountField, static_cast<double>(chat.persisted_message_count));
			uam::json::SetString(root, kChatPersistedMessagesDigestField, chat.persisted_messages_digest);
			uam::json::SetNumber(root, kChatSummarySourceSizeField, static_cast<double>(file_text.size()));
			// This cache is rebuilt from the primary; metadata-only summary saves remain durable.
			(void)uam::io::WriteCacheFile(SummaryCachePathForChatFile(path), SerializeArchiveDocument(root, chat.passive_queued_prompt_archive));
		}

		if (!chat.imported_read_only && apply_queue_journal) ApplyQueueJournal(path, root, chat);
		else
		{
			// Canonical archive is inert; an outbox must not refill a passive runtime queue.
			chat.acp_queue_storage_revision = JsonStringOrEmpty(root.Find(kQueueRevisionField));
			chat.acp_queue_full_save_revision = JsonStringOrEmpty(root.Find(kQueueFullRevisionField));
		}
		HoldManagedQueue(chat);
		if (!ChatAttachmentsPersistable(chat)) return {std::nullopt, "contains invalid attachment ownership"};
		return {std::move(chat), ""};
	}
	catch (const std::length_error&)
	{
		return {std::nullopt, "contains invalid attachment metadata"};
	}

	LoadChatResult ParseLocalChatFile(const fs::path& path, bool include_messages = true, std::uintmax_t expected_source_size = 0, bool publish_summary_cache = true, std::size_t max_read_bytes = 0)
	{
		std::string file_text;
		if (max_read_bytes != 0)
		{
			// Refuse a known oversize source before allocating its body; the reader
			// also enforces the ceiling if the file grows after this observation.
			std::error_code size_error;
			const std::uintmax_t source_size = fs::file_size(path, size_error);
			if (size_error || source_size > max_read_bytes ||
			    !uam::io::TryReadTextFile(path, file_text, max_read_bytes))
				return {std::nullopt, "cannot be read within the requested size limit"};
		}
		else file_text = uam::io::ReadTextFile(path);
		if (file_text.empty())
		{
			return {std::nullopt, "is empty"};
		}

		bool recognized_managed_input = false;
		LoadChatResult result = ParseLocalChatDocument(path, std::move(file_text), include_messages,
		    expected_source_size, publish_summary_cache, max_read_bytes, &recognized_managed_input);
		result.managed_followup_input_unavailable = recognized_managed_input;
		return result;
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

	void ApplyChatTimestampFallbacks(ChatSession& chat, bool legacy_interaction)
	{
		if (chat.created_at.empty())
		{
			// Missing legacy dates must remain stable across summary and message loads.
			chat.created_at = !chat.updated_at.empty() ? chat.updated_at
			    : !chat.last_opened_at.empty() ? chat.last_opened_at
			    : !chat.interaction_at.empty() ? chat.interaction_at
			    : "1970-01-01T00:00:00.000Z";
		}
		if (chat.updated_at.empty())
		{
			chat.updated_at = chat.created_at;
		}
		// An explicitly empty timestamp means this import has no UAM interaction yet.
		if (legacy_interaction && chat.interaction_at.empty()) chat.interaction_at = chat.updated_at;
		if (chat.last_opened_at.empty())
		{
			chat.last_opened_at = chat.updated_at;
		}
	}

} // namespace

namespace
{
	std::mutex& ChatSaveMutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	std::optional<std::string> StorageRevision(const fs::path& path)
	{
		std::error_code error;
		const fs::file_status status = fs::symlink_status(path, error);
		if (status.type() == fs::file_type::not_found) return std::string("missing");
		if (error || !fs::is_regular_file(status)) return std::nullopt;
		const std::uintmax_t size = fs::file_size(path, error);
		if (error) return std::nullopt;
		const fs::file_time_type modified = fs::last_write_time(path, error);
		if (error) return std::nullopt;
		const std::chrono::seconds seconds = std::chrono::duration_cast<std::chrono::seconds>(modified.time_since_epoch());
		const std::chrono::nanoseconds fraction = std::chrono::duration_cast<std::chrono::nanoseconds>(modified.time_since_epoch() - seconds);
		return std::to_string(size) + ":" + std::to_string(seconds.count()) + ":" + std::to_string(fraction.count());
	}

	// Writer-only observation. A birth is storage data, never creation/admission authority.
	enum class CanonicalBirthKind { Missing, Unbound, Bound, Invalid, Corrupt, Unknown };
	struct CanonicalBirth
	{
		CanonicalBirthKind kind = CanonicalBirthKind::Unknown;
		std::string value;
	};

	class BirthEnvelopeReader
	{
	public:
		explicit BirthEnvelopeReader(std::ifstream& input) : m_input(input) {}
		int Peek()
		{
			if (m_position == m_count && !Refill()) return -1;
			return static_cast<unsigned char>(m_buffer[m_position]);
		}
		int Get()
		{
			const int value = Peek();
			if (value >= 0) ++m_position;
			return value;
		}
		void Drain() { while (Peek() >= 0) m_position = m_count; }
		bool Failed() const { return m_failed; }
		std::uintmax_t BytesRead() const { return m_bytes; }
	private:
		bool Refill()
		{
			if (m_eof || m_failed) return false;
			m_input.read(m_buffer.data(), static_cast<std::streamsize>(m_buffer.size()));
			const auto count = m_input.gcount();
			if (count < 0 || m_input.bad() || (!m_input && !m_input.eof()) ||
			    static_cast<std::uintmax_t>(count) > std::numeric_limits<std::uintmax_t>::max() - m_bytes)
			{
				m_failed = true;
				return false;
			}
			m_position = 0;
			m_count = static_cast<std::size_t>(count);
			m_bytes += static_cast<std::uintmax_t>(count);
			m_eof = m_input.eof();
			return m_count != 0;
		}
		std::ifstream& m_input;
		std::array<char, 65536> m_buffer{};
		std::size_t m_position = 0, m_count = 0;
		std::uintmax_t m_bytes = 0;
		bool m_eof = false, m_failed = false;
	};

	// Complete root-object grammar, discarding unrelated values without token/history allocation.
	// A resource limit is Unknown; readable syntax corruption keeps legacy recovery semantics.
	class BirthEnvelopeParser
	{
	public:
		explicit BirthEnvelopeParser(BirthEnvelopeReader& reader) : m_reader(reader) {}
		bool Parse()
		{
			if (m_reader.Peek() == 0xef)
			{
				m_reader.Get();
				if (m_reader.Get() != 0xbb || m_reader.Get() != 0xbf) return false;
			}
			Space();
			if (m_reader.Peek() != '{' || !Object(1, true)) return false;
			Space();
			return m_reader.Peek() < 0;
		}
		bool ResourceLimit() const { return m_resource_limit; }
		CanonicalBirth Result() const
		{
			if (m_invalid_birth) return {CanonicalBirthKind::Invalid, {}};
			return {m_birth.empty() ? CanonicalBirthKind::Unbound : CanonicalBirthKind::Bound, m_birth};
		}
	private:
		void Space()
		{
			while (m_reader.Peek() == ' ' || m_reader.Peek() == '\t' || m_reader.Peek() == '\r' || m_reader.Peek() == '\n')
				m_reader.Get();
		}
		bool Depth(unsigned int depth)
		{
			if (depth <= 256) return true;
			m_resource_limit = true;
			return false;
		}
		static void Append(std::string* output, std::size_t limit, bool* truncated, unsigned int code_point)
		{
			if (output == nullptr) return;
			const auto byte = [&](unsigned int value)
			{
				if (output->size() < limit) output->push_back(static_cast<char>(value));
				else if (truncated != nullptr) *truncated = true;
			};
			if (code_point < 0x80) byte(code_point);
			else if (code_point < 0x800)
			{
				byte(0xc0 | (code_point >> 6)); byte(0x80 | (code_point & 0x3f));
			}
			else if (code_point < 0x10000)
			{
				byte(0xe0 | (code_point >> 12)); byte(0x80 | ((code_point >> 6) & 0x3f)); byte(0x80 | (code_point & 0x3f));
			}
			else
			{
				byte(0xf0 | (code_point >> 18)); byte(0x80 | ((code_point >> 12) & 0x3f));
				byte(0x80 | ((code_point >> 6) & 0x3f)); byte(0x80 | (code_point & 0x3f));
			}
		}
		bool Hex(unsigned int& value)
		{
			value = 0;
			for (unsigned int index = 0; index != 4; ++index)
			{
				const int ch = m_reader.Get();
				const int digit = ch >= '0' && ch <= '9' ? ch - '0' :
				    ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
				if (digit < 0) return false;
				value = value * 16 + static_cast<unsigned int>(digit);
			}
			return true;
		}
		bool String(std::string* output = nullptr, std::size_t limit = 0, bool* truncated = nullptr)
		{
			if (output != nullptr) output->clear();
			if (truncated != nullptr) *truncated = false;
			if (m_reader.Get() != '"') return false;
			for (;;)
			{
				const int ch = m_reader.Get();
				if (ch == '"') return true;
				if (ch < 0x20) return false;
				unsigned int code_point = static_cast<unsigned int>(ch);
				if (ch == '\\')
				{
					switch (m_reader.Get())
					{
					case '"': code_point = '"'; break;
					case '\\': code_point = '\\'; break;
					case '/': code_point = '/'; break;
					case 'b': code_point = '\b'; break;
					case 'f': code_point = '\f'; break;
					case 'n': code_point = '\n'; break;
					case 'r': code_point = '\r'; break;
					case 't': code_point = '\t'; break;
					case 'u':
						if (!Hex(code_point)) return false;
						if (code_point >= 0xd800 && code_point <= 0xdbff)
						{
							unsigned int low = 0;
							if (m_reader.Get() != '\\' || m_reader.Get() != 'u' || !Hex(low) || low < 0xdc00 || low > 0xdfff) return false;
							code_point = 0x10000 + ((code_point - 0xd800) << 10) + low - 0xdc00;
						}
						else if (code_point >= 0xdc00 && code_point <= 0xdfff) return false;
						break;
					default: return false;
					}
				}
				else if (ch >= 0x80)
				{
					const unsigned int length = ch >= 0xc2 && ch <= 0xdf ? 2 : ch >= 0xe0 && ch <= 0xef ? 3 : ch >= 0xf0 && ch <= 0xf4 ? 4 : 0;
					if (length == 0) return false;
					code_point &= length == 2 ? 0x1f : length == 3 ? 0x0f : 0x07;
					for (unsigned int index = 1; index != length; ++index)
					{
						const int next = m_reader.Get();
						if (next < 0x80 || next > 0xbf) return false;
						code_point = (code_point << 6) | static_cast<unsigned int>(next & 0x3f);
					}
					if (code_point < (length == 2 ? 0x80U : length == 3 ? 0x800U : 0x10000U) ||
					    code_point > 0x10ffff || (code_point >= 0xd800 && code_point <= 0xdfff)) return false;
				}
				Append(output, limit, truncated, code_point);
			}
		}
		bool Literal(std::string_view text)
		{
			for (char ch : text) if (m_reader.Get() != ch) return false;
			return true;
		}
		bool Number()
		{
			if (m_reader.Peek() == '-') m_reader.Get();
			if (m_reader.Peek() == '0') m_reader.Get();
			else
			{
				if (m_reader.Peek() < '1' || m_reader.Peek() > '9') return false;
				do { m_reader.Get(); } while (m_reader.Peek() >= '0' && m_reader.Peek() <= '9');
			}
			if (m_reader.Peek() == '.')
			{
				m_reader.Get();
				if (m_reader.Peek() < '0' || m_reader.Peek() > '9') return false;
				do { m_reader.Get(); } while (m_reader.Peek() >= '0' && m_reader.Peek() <= '9');
			}
			if (m_reader.Peek() == 'e' || m_reader.Peek() == 'E')
			{
				m_reader.Get();
				if (m_reader.Peek() == '+' || m_reader.Peek() == '-') m_reader.Get();
				if (m_reader.Peek() < '0' || m_reader.Peek() > '9') return false;
				do { m_reader.Get(); } while (m_reader.Peek() >= '0' && m_reader.Peek() <= '9');
			}
			return true;
		}
		bool Value(unsigned int depth)
		{
			Space();
			switch (m_reader.Peek())
			{
			case '{': return Object(depth + 1, false);
			case '[': return Array(depth + 1);
			case '"': return String();
			case 't': return Literal("true");
			case 'f': return Literal("false");
			case 'n': return Literal("null");
			default: return Number();
			}
		}
		bool Object(unsigned int depth, bool root)
		{
			if (!Depth(depth) || m_reader.Get() != '{') return false;
			Space();
			if (m_reader.Peek() == '}') { m_reader.Get(); return true; }
			for (;;)
			{
				std::string key;
				bool long_key = false;
				if (!String(root ? &key : nullptr, kChatManagedFollowupBirthField.size(), &long_key)) return false;
				Space();
				if (m_reader.Get() != ':') return false;
				Space();
				if (root && !long_key && key == kChatManagedFollowupBirthField)
				{
					if (m_saw_birth) m_invalid_birth = true;
					m_saw_birth = true;
					if (m_reader.Peek() == '"')
					{
						bool long_birth = false;
						if (!String(&m_birth, 36, &long_birth)) return false;
						if (long_birth || (!m_birth.empty() && !uam::uuid::IsCanonicalUuid(m_birth))) m_invalid_birth = true;
					}
					else
					{
						m_invalid_birth = true;
						if (!Value(depth)) return false;
					}
				}
				else if (!Value(depth)) return false;
				Space();
				const int next = m_reader.Get();
				if (next == '}') return true;
				if (next != ',') return false;
				Space();
			}
		}
		bool Array(unsigned int depth)
		{
			if (!Depth(depth) || m_reader.Get() != '[') return false;
			Space();
			if (m_reader.Peek() == ']') { m_reader.Get(); return true; }
			for (;;)
			{
				if (!Value(depth)) return false;
				Space();
				const int next = m_reader.Get();
				if (next == ']') return true;
				if (next != ',') return false;
			}
		}
		BirthEnvelopeReader& m_reader;
		std::string m_birth;
		bool m_saw_birth = false, m_invalid_birth = false, m_resource_limit = false;
	};

	struct BirthFileStamp
	{
		std::uintmax_t size = 0;
		fs::file_time_type modified;
		bool operator==(const BirthFileStamp&) const = default;
	};
	std::optional<BirthFileStamp> ReadBirthFileStamp(const fs::path& native)
	{
		std::error_code error;
		const auto status = fs::symlink_status(native, error);
		if (error || !fs::is_regular_file(status) || uam::paths::IsLinkOrReparsePointNoThrow(native)) return {};
		const auto size = fs::file_size(native, error);
		if (error) return {};
		const auto modified = fs::last_write_time(native, error);
		if (error) return {};
		return BirthFileStamp{size, modified};
	}
	CanonicalBirth ReadCanonicalBirth(const fs::path& path)
	try
	{
		fs::path native;
		std::string path_error;
		if (!uam::io::NativeFilePath(path, native, path_error)) return {};
		std::error_code error;
		const auto status = fs::symlink_status(native, error);
		if (status.type() == fs::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
			return {CanonicalBirthKind::Missing, {}};
		if (error || !fs::is_regular_file(status) || uam::paths::IsLinkOrReparsePointNoThrow(native)) return {};
		const auto before = ReadBirthFileStamp(native);
		std::ifstream input;
		if (!before || !uam::io::OpenReadFile(native, input)) return {};
		BirthEnvelopeReader reader(input);
		BirthEnvelopeParser parser(reader);
		const bool complete = parser.Parse();
		if (parser.ResourceLimit()) return {};
		// Drain a syntax failure too: an IO failure/truncated observation is not legacy corruption permission.
		reader.Drain();
		if (reader.Failed() || reader.BytesRead() != before->size || ReadBirthFileStamp(native) != before) return {};
		return complete ? parser.Result() : CanonicalBirth{CanonicalBirthKind::Corrupt, {}};
	}
	catch (...)
	{
		return {};
	}

	bool CanonicalBirthAllowsValue(const fs::path& primary, std::string_view candidate)
	{
		const auto current = ReadCanonicalBirth(primary);
		if (current.kind == CanonicalBirthKind::Unknown || current.kind == CanonicalBirthKind::Invalid) return false;
		return current.kind != CanonicalBirthKind::Bound || current.value == candidate;
	}
	bool CanonicalBirthAllowsStage(const fs::path& primary, const fs::path& stage)
	{
		const auto current = ReadCanonicalBirth(primary);
		if (current.kind == CanonicalBirthKind::Unknown || current.kind == CanonicalBirthKind::Invalid) return false;
		if (current.kind != CanonicalBirthKind::Bound) return true;
		const auto candidate = ReadCanonicalBirth(stage);
		return candidate.kind == CanonicalBirthKind::Bound && candidate.value == current.value;
	}

	std::optional<ChatSession> ReadIndexedMetadata(const fs::path& primary, const uam::chat_pages::Index& index)
	{
		std::ifstream input(primary, std::ios::binary);
		std::string text;
		if (!uam::chat_pages::ReadMetadata(input, index, text)) return std::nullopt;
		std::optional<JsonValue> root = ParseJson(text);
		if (!root || root->type != JsonValue::Type::Object) return std::nullopt;
		root->object_value.erase(std::string(kChatMessagesField));
		uam::json::SetNumber(*root, kChatPersistedMessageCountField, static_cast<double>(index.message_count));
		// A primary contains metadata; its summary digest is recency/count, distinct from the frontend content hash.
		root->object_value.erase(std::string(kChatPersistedMessagesDigestField));
		auto archive = JsonBoolOrDefault(root->Find(kChatImportedReadOnlyField), false) ? PassiveArchiveFromText(text, index.attachment_owner) : nullptr;
		if (JsonBoolOrDefault(root->Find(kChatImportedReadOnlyField), false) && !archive) return std::nullopt;
		return ParseLocalChatDocument(primary, SerializeArchiveDocument(*root, archive), false, 0, false, kMaxChatJsonBytes).chat;
	}

	std::optional<std::string> ReadPrimaryArchiveMetadata(
	    const fs::path& primary, std::string_view owner, std::string_view revision)
	{
		// Ordinary synchronous passive saves only read bounded indexed metadata.
		if (uam::paths::IsLinkOrReparsePointNoThrow(primary) ||
		    uam::paths::IsLinkOrReparsePointNoThrow(uam::chat_pages::IndexPath(primary))) return {};
		const auto index = uam::chat_pages::ReadIndex(uam::chat_pages::IndexPath(primary), revision);
		if (!index || index->attachment_owner != owner || index->array_end >= index->source_size ||
		    index->array_begin > kMaxPassiveMetadataReadBytes - 2 ||
		    index->source_size - index->array_end - 1 > kMaxPassiveMetadataReadBytes - 2 - index->array_begin) return {};
		std::error_code error;
		if (fs::file_size(primary, error) != index->source_size || error) return {};
		std::ifstream input(primary, std::ios::binary);
		std::string text;
		if (!uam::chat_pages::ReadMetadata(input, *index, text) || StorageRevision(primary) != revision) return {};
		return text;
	}

	std::shared_ptr<const uam::PassiveQueuedPromptArchive> ReadPrimaryPassiveArchive(
	    const fs::path& primary, std::string_view owner, std::string_view revision)
	{
		const auto text = ReadPrimaryArchiveMetadata(primary, owner, revision);
		return text ? PassiveArchiveFromText(*text, owner) : nullptr;
	}

	// This probe establishes only permission to attempt active conversion. It
	// cannot supply an archive or publish anything; the complete owner/archive
	// validation and expected-primary revision fence remain mandatory afterward.
	class ActiveConversionPolicyProbe final : public nlohmann::json_sax<nlohmann::json>
	{
	public:
		std::string owner;
		std::optional<bool> read_only;
		bool has_owner = false;
		bool root_complete = false;
		bool Ready() const { return has_owner && read_only.has_value(); }
		bool null() override { return UnrelatedScalar(); }
		bool boolean(bool value) override
		{
			if (depth != 1 || field != "imported_read_only") return UnrelatedScalar();
			if (read_only.has_value()) return false;
			read_only = value;
			return !Ready();
		}
		bool number_integer(number_integer_t) override { return UnrelatedScalar(); }
		bool number_unsigned(number_unsigned_t) override { return UnrelatedScalar(); }
		bool number_float(number_float_t, const string_t&) override { return UnrelatedScalar(); }
		bool string(string_t& value) override
		{
			if (depth != 1 || field != "id") return UnrelatedScalar();
			if (has_owner || value.size() > 256) return false;
			owner = value;
			has_owner = true;
			return !Ready();
		}
		bool binary(binary_t&) override { return false; }
		bool start_object(std::size_t) override { return Start(false); }
		bool start_array(std::size_t) override { return Start(true); }
		bool key(string_t& value) override
		{
			if (depth == 1) field = value;
			return true;
		}
		bool end_object() override { return End(); }
		bool end_array() override { return End(); }
		bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
	private:
		std::size_t depth = 0;
		std::string field;
		bool UnrelatedScalar() const
		{
			return depth != 1 || (field != "id" && field != "imported_read_only");
		}
		bool Start(bool array)
		{
			if ((depth == 0 && array) || !UnrelatedScalar()) return false;
			++depth;
			return true;
		}
		bool End()
		{
			if (depth == 0) return false;
			if (--depth == 0) root_complete = true;
			return true;
		}
	};

	bool ProbePrimaryActiveConversionPolicy(const fs::path& primary, std::string_view owner, std::string_view revision)
	try
	{
		// A fixed prefix, never a transcript scan or full-primary fallback for
		// ordinary passive metadata. Unknown/order-beyond-cap policy fails closed.
		constexpr std::size_t kMaxPolicyProbeBytes = 64U * 1024U;
		if (uam::paths::IsLinkOrReparsePointNoThrow(primary)) return false;
		std::ifstream input(primary, std::ios::binary);
		if (!input) return false;
		std::string prefix(kMaxPolicyProbeBytes, '\0');
		input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
		if (input.bad() || input.gcount() <= 0) return false;
		prefix.resize(static_cast<std::size_t>(input.gcount()));
		ActiveConversionPolicyProbe policy;
		const bool complete = nlohmann::json::sax_parse(prefix.begin(), prefix.end(), &policy);
		// An explicit false marker can stop parsing early. A missing legacy
		// marker is active only if the complete bounded document was consumed.
		return policy.has_owner && policy.owner == owner && !policy.read_only.value_or(false) &&
		    (policy.Ready() || (complete && policy.root_complete)) && StorageRevision(primary) == revision;
	}
	catch (...)
	{
		return false;
	}

	std::shared_ptr<const uam::PassiveQueuedPromptArchive> ReadExplicitActiveConversionArchive(
	    const fs::path& primary, std::string_view owner, std::string_view revision, bool require_policy_probe)
	{
		auto text = ReadPrimaryArchiveMetadata(primary, owner, revision);
		if (!text)
		{
			// Preserve the existing bounded conversion route for an executable
			// queue; a new empty-candidate transition first needs policy proof.
			if (require_policy_probe && !ProbePrimaryActiveConversionPolicy(primary, owner, revision)) return {};
			// Compatibility for explicit active -> passive conversion, including
			// old zero-message primaries without an index. This small bounded probe
			// is never used for an ordinary already-passive metadata save.
			std::string bounded;
			if (uam::paths::IsLinkOrReparsePointNoThrow(primary) ||
			    !uam::io::TryReadTextFile(primary, bounded, kMaxPassiveMetadataReadBytes) ||
			    StorageRevision(primary) != revision) return {};
			text = std::move(bounded);
		}
		const auto document = uam::passive_archive_json::Parse(*text);
		if (!document || document->owner != owner || document->read_only) return {};
		return PassiveArchiveFromText(*text, owner);
	}

	std::optional<uam::chat_pages::Index> EnsureMessageIndex(const fs::path& primary, std::string_view revision, std::string_view owner,
	    const std::function<bool()>& cancelled = {}, bool rebuild = false)
	{
		if (!rebuild)
		{
			const std::optional<uam::chat_pages::Index> cached = uam::chat_pages::ReadIndex(uam::chat_pages::IndexPath(primary), revision);
			std::error_code error;
			if (cached && cached->attachment_owner == owner && fs::file_size(primary, error) == cached->source_size && !error)
			{
				const auto metadata = ReadIndexedMetadata(primary, *cached);
				if (metadata && metadata->id == owner) return cached;
			}
		}
		uam::chat_pages::Builder builder(primary, owner);
		std::uint64_t array_begin = 0, array_end = 0;
		const std::function<std::optional<Message>(std::string_view)> decode = [owner](std::string_view raw)
		{
			const std::optional<JsonValue> value = ParseJson(raw);
			if (!value || value->type != JsonValue::Type::Object) return std::optional<Message>{};
			Message message = JsonToMessage(*value);
			if (!AttachmentsPersistable(message.attachments, owner)) return std::optional<Message>{};
			return std::optional<Message>{std::move(message)};
		};
		if (!uam::chat_pages::Scan(primary, builder, decode, array_begin, array_end, cancelled)) return std::nullopt;
		std::error_code error;
		const std::uintmax_t size = fs::file_size(primary, error);
		if (error || (cancelled && cancelled()) || StorageRevision(primary) != revision) return std::nullopt;
		const std::shared_ptr<PreparedChatSave> staged = std::make_shared<PreparedChatSave>();
		staged->index_path = uam::chat_pages::IndexPath(primary);
		staged->index_temp = uam::io::MakeTempWritePath(staged->index_path);
		if (!builder.Finish(staged->index_temp, revision, size, array_begin, array_end)) return std::nullopt;
		const std::optional<uam::chat_pages::Index> index = uam::chat_pages::ReadIndex(staged->index_temp, revision);
		if (!index || index->attachment_owner != owner) return std::nullopt;
		const auto metadata = ReadIndexedMetadata(primary, *index);
		if (!metadata || metadata->id != owner) return std::nullopt;
		std::lock_guard<std::mutex> lock(ChatSaveMutex());
		if ((cancelled && cancelled()) || StorageRevision(primary) != revision ||
		    !uam::io::CommitSyncedAtomicTemp(staged->index_path, staged->index_temp, false).success) return std::nullopt;
		return index;
	}

	bool PrimaryTemporaryOwnerMatches(const fs::path& primary, std::string_view parent, std::string_view token)
	{
		if (parent.empty() || token.empty()) return false;
		const std::optional<std::string> revision = StorageRevision(primary);
		if (!revision || *revision == "missing") return false;
		std::string metadata;
		std::optional<uam::chat_pages::Index> index = uam::chat_pages::ReadIndex(uam::chat_pages::IndexPath(primary), *revision);
		// Ownership checks never repair or scan a large transcript on synchronous save callers.
		if (!index && !uam::io::TryReadTextFile(primary, metadata, 4 * 1024 * 1024)) return false;
		if (index)
		{
			std::ifstream input(primary, std::ios::binary);
			if (!uam::chat_pages::ReadMetadata(input, *index, metadata)) return false;
		}
		const std::optional<JsonValue> document = ParseJson(metadata);
		return document && document->type == JsonValue::Type::Object &&
			JsonStringOrEmpty(document->Find("temporary_parent_chat_id")) == parent &&
			JsonStringOrEmpty(document->Find("temporary_storage_owner_token")) == token && StorageRevision(primary) == revision;
	}

	bool PageOwnerMatches(const ChatSession& lhs, const ChatSession& rhs, bool require_same_recency = true)
	{
		return lhs.id == rhs.id && lhs.created_at == rhs.created_at && (!require_same_recency || lhs.updated_at == rhs.updated_at) && lhs.provider_id == rhs.provider_id &&
		    lhs.native_session_id == rhs.native_session_id && lhs.execution_host_id == rhs.execution_host_id &&
		    lhs.opencode_v2_session_id == rhs.opencode_v2_session_id &&
		    lhs.temporary_storage_owner_token == rhs.temporary_storage_owner_token &&
		    lhs.folder_id == rhs.folder_id && lhs.workspace_directory == rhs.workspace_directory &&
		    lhs.workspace_isolation_kind == rhs.workspace_isolation_kind && lhs.workspace_source_directory == rhs.workspace_source_directory &&
		    lhs.workspace_base_ref == rhs.workspace_base_ref && lhs.git_base_branch_override == rhs.git_base_branch_override && lhs.workspace_branch_name == rhs.workspace_branch_name &&
		    lhs.workspace_worktree_directory == rhs.workspace_worktree_directory && lhs.svn_update_pending == rhs.svn_update_pending && lhs.imported_read_only == rhs.imported_read_only &&
		    lhs.managed_followup_incarnation == rhs.managed_followup_incarnation &&
		    lhs.managed_followup_input_unavailable == rhs.managed_followup_input_unavailable &&
		    lhs.managed_parent_input_receipts == rhs.managed_parent_input_receipts && lhs.managed_parent_result == rhs.managed_parent_result &&
		    lhs.acp_unsent_prompt_recoveries == rhs.acp_unsent_prompt_recoveries &&
		    (!lhs.imported_read_only || !lhs.passive_queued_prompt_archive || SamePassiveArchive(lhs.passive_queued_prompt_archive, rhs.passive_queued_prompt_archive));
	}

}

std::optional<ChatMessagePage> ChatRepository::ReadMessagePage(const fs::path& data_root,
    const ChatSession& expected, std::size_t limit, std::optional<std::size_t> before,
    std::string* warning_out, const std::function<bool()>& cancelled)
try
{
	const std::function<std::optional<ChatMessagePage>()> failure = [&]()
	{
		if (warning_out) *warning_out = "Could not read chat history. Retry to refresh it.";
		return std::optional<ChatMessagePage>{};
	};
	if (!uam::chat_ids::IsSafeStorageChatId(expected.id) || expected.messages_loaded || limit > 200) return failure();
	const fs::path primary = AppPaths::UamChatFilePath(data_root, expected.id);
	const std::optional<std::string> revision = ChatStorageRevision(data_root, expected.id);
	if (!revision || *revision == "missing" || (cancelled && cancelled())) return failure();
	if (uam::paths::IsLinkOrReparsePointNoThrow(primary) || uam::paths::IsLinkOrReparsePointNoThrow(uam::chat_pages::IndexPath(primary))) return failure();
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		const std::optional<uam::chat_pages::Index> index = EnsureMessageIndex(primary, *revision, expected.id, cancelled, attempt != 0);
		if (!index) return failure();
		const std::optional<ChatSession> metadata = ReadIndexedMetadata(primary, *index);
		if (!metadata) continue;
		if (metadata->persisted_message_count != expected.persisted_message_count ||
		    metadata->persisted_messages_digest != expected.persisted_messages_digest || !PageOwnerMatches(expected, *metadata)) return failure();
		ChatMessagePage page;
		page.chat = *metadata;
		page.total_count = static_cast<std::size_t>(index->message_count);
		const std::size_t end = std::min(before.value_or(page.total_count), page.total_count);
		page.start_index = end > limit ? end - limit : 0;
		page.messages_digest = index->messages_digest;
		page.storage_revision = *revision;
		page.user_message_indices = index->user_indices;
		std::vector<uam::chat_pages::Span> spans;
		if (!uam::chat_pages::ReadMessageSpans(uam::chat_pages::IndexPath(primary), *index, page.start_index, end, spans)) continue;
		std::ifstream input(primary, std::ios::binary);
		bool valid = true;
		for (const uam::chat_pages::Span span : spans)
		{
			if (cancelled && cancelled()) return failure();
			std::string raw;
			if (!uam::chat_pages::ReadSpan(input, span, raw)) { valid = false; break; }
			const std::optional<JsonValue> parsed = ParseJson(raw);
			if (!parsed || parsed->type != JsonValue::Type::Object) { valid = false; break; }
			Message message = JsonToMessage(*parsed);
			if (!AttachmentsPersistable(message.attachments, page.chat.id)) { valid = false; break; }
			std::uint64_t hash = uam::hashing::kFnv1a64OffsetBasis;
			uam::chat_pages::AppendMessageDigest(hash, message);
			if (hash != span.content_hash) { valid = false; break; }
			page.chat.messages.push_back(std::move(message));
		}
		if (!valid) continue;
		if ((cancelled && cancelled()) || ChatStorageRevision(data_root, expected.id) != revision) return failure();
		return page;
	}
	return failure();
}
catch (const std::exception& error)
{
	uam::diagnostics::Write(std::string("History page read rejected: ") + error.what());
	if (warning_out) *warning_out = "Could not read chat history. Retry to refresh it.";
	return std::nullopt;
}

bool ChatRepository::MatchesMessagePage(const ChatSession& current, const ChatMessagePage& page)
{
	return !current.messages_loaded && PageOwnerMatches(current, page.chat) &&
	    current.persisted_message_count == page.total_count &&
	    current.persisted_messages_digest == page.chat.persisted_messages_digest;
}

std::optional<ChatMessagePage> ChatRepository::ReadSavedPrimaryMessagePage(const fs::path& data_root,
    std::string_view chat_id, std::size_t limit, std::optional<std::size_t> before,
    std::string* warning_out, const std::function<bool()>& cancelled)
{
	if (!uam::chat_ids::IsSafeStorageChatId(chat_id) || limit == 0 || limit > 200) return std::nullopt;
	const fs::path primary = AppPaths::UamChatFilePath(data_root, chat_id);
	if (uam::paths::IsLinkOrReparsePointNoThrow(primary) || uam::paths::IsLinkOrReparsePointNoThrow(uam::chat_pages::IndexPath(primary))) return std::nullopt;
	const auto revision = ChatStorageRevision(data_root, chat_id);
	if (!revision || *revision == "missing" || (cancelled && cancelled())) return std::nullopt;
	const auto index = EnsureMessageIndex(primary, *revision, chat_id, cancelled);
	const auto metadata = index ? ReadIndexedMetadata(primary, *index) : std::nullopt;
	if (!metadata || ChatStorageRevision(data_root, chat_id) != revision) return std::nullopt;
	// The strict existing page reader verifies this primary's own metadata/count,
	// each selected span and the physical revision. Only its derived index may change.
	auto page = ReadMessagePage(data_root, *metadata, limit, before, warning_out, cancelled);
	if (!page || page->storage_revision != *revision) return std::nullopt;
	return page;
}

bool ChatRepository::MatchesSavedPrimaryObservation(const ChatSession& current, const ChatMessagePage& page)
{
	return PageOwnerMatches(current, page.chat, false);
}

namespace
{
	bool PublicationDirectory(const fs::path& path)
	{
		fs::path native;
		std::string error;
		if (!uam::io::NativeFilePath(path, native, error, true)) return false;
		std::error_code ec;
		const auto status = fs::symlink_status(native, ec);
		return !ec && fs::is_directory(status) && !uam::paths::IsLinkOrReparsePointNoThrow(native);
	}

	bool PublicationAbsent(const fs::path& path)
	{
		if (!PublicationDirectory(path.parent_path())) return false;
		fs::path native;
		std::string error;
		if (!uam::io::NativeFilePath(path, native, error)) return false;
		std::error_code ec;
		const auto status = fs::symlink_status(native, ec);
		return status.type() == fs::file_type::not_found &&
		    (!ec || ec == std::errc::no_such_file_or_directory);
	}


	// Opt-in absence observation also admits a missing direct child directory.
	// Never interpret an unreadable/linked parent as absence.
	bool StoppedPublicationAbsent(const fs::path& path)
	{
		return PublicationAbsent(path) ||
		    (PublicationDirectory(path.parent_path().parent_path()) && PublicationAbsent(path.parent_path()));
	}

	fs::path PublicationRootSpelling(fs::path root)
	{
		// A terminal empty component is spelling only. Preserve the filesystem
		// root and internal components; do not resolve links, dot-dot or routing.
		while (root.has_relative_path() && root.filename().empty())
			root = root.parent_path();
		return root;
	}

	bool PublicationCreateDirectory(const fs::path& root, const fs::path& path)
	{
		if (!PublicationDirectory(root) || path.parent_path().lexically_normal() != root.lexically_normal()) return false;
		if (PublicationDirectory(path)) return true;
		if (!PublicationAbsent(path)) return false;
		std::error_code ec;
		fs::create_directory(path, ec);
		return !ec && PublicationDirectory(path);
	}

	struct PublicationWitness
	{
		std::uint64_t bytes = 0;
		fs::file_time_type modified;
		std::string digest;
		bool operator==(const PublicationWitness&) const = default;
	};

	std::optional<PublicationWitness> ReadPublicationWitness(const fs::path& path)
	{
		fs::path native;
		std::string path_error;
		if (!uam::io::NativeFilePath(path, native, path_error)) return {};
		const auto regular = [&]
		{
			return PublicationDirectory(path.parent_path()) &&
			    !uam::paths::IsLinkOrReparsePointNoThrow(native) && uam::paths::IsRegularFileNoThrow(native);
		};
		if (!regular()) return {};
		std::error_code ec;
		const auto size = fs::file_size(native, ec);
		if (ec || size > std::numeric_limits<std::uint64_t>::max()) return {};
		const auto modified = fs::last_write_time(native, ec);
		if (ec) return {};
		std::ifstream input;
		if (!uam::io::OpenReadFile(native, input)) return {};
		uam::platform::IncrementalSha256 hash;
		std::array<char, 65536> buffer{};
		std::uint64_t remaining = static_cast<std::uint64_t>(size);
		while (remaining != 0)
		{
			const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
			if (!input.read(buffer.data(), static_cast<std::streamsize>(count)) ||
			    !hash.Add(std::string_view(buffer.data(), count))) return {};
			remaining -= count;
		}
		char extra = 0;
		if (input.get(extra) || !input.eof() || input.bad()) return {};
		const auto after_size = fs::file_size(native, ec);
		if (ec || after_size != size) return {};
		const auto after_modified = fs::last_write_time(native, ec);
		if (ec || after_modified != modified || !regular()) return {};
		std::string digest = hash.Finish();
		if (digest.empty()) return {};
		return PublicationWitness{static_cast<std::uint64_t>(size), modified, std::move(digest)};
	}

	struct PublicationExpectedFile
	{
		bool absent = true;
		PublicationWitness witness;
		std::shared_ptr<const uam::platform::PublishedFileChangeWitness> physical;
	};

	bool CapturePublicationFile(const fs::path& path, PublicationExpectedFile& expected)
	{
		if (PublicationAbsent(path)) { expected.absent = true; return true; }
		const auto witness = ReadPublicationWitness(path);
		if (!witness) return false;
		expected.absent = false;
		expected.witness = *witness;
		return true;
	}

	bool PublicationFileMatches(const fs::path& path, const PublicationExpectedFile& expected)
	{
		if (expected.physical)
			return expected.physical->Current() && expected.physical->MatchesContent(expected.witness.bytes, expected.witness.digest);
		if (expected.absent) return PublicationAbsent(path);
		const auto current = ReadPublicationWitness(path);
		return current && *current == expected.witness;
	}

	fs::path PublicationPrivatePath(const fs::path& parent, std::string_view role)
	{
		const std::string uuid = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (!uam::uuid::IsCanonicalUuid(uuid)) throw std::runtime_error("Private stage UUID is unavailable.");
		// Never use the canonical .tmp.* / .bak.tmp.* namespaces scanned by the old janitor.
		return parent / (".uam-chat-publication-" + uuid + "-" + std::string(role));
	}

	struct PublicationStage
	{
		fs::path path;
		std::optional<PublicationWitness> witness;
		bool owned = false;
		bool retain = false;
		bool held_content = false;
		std::shared_ptr<const uam::platform::AtomicTempWriterOrigin> origin;
		std::shared_ptr<const uam::platform::PublishedFileChangeWitness> physical;

		bool Matches() const
		{
			if (held_content)
				return owned && witness && origin && physical && origin->Matches(*physical) && physical->Current() && physical->MatchesContent(witness->bytes, witness->digest);
			const auto current = witness ? ReadPublicationWitness(path) : std::nullopt;
			return owned && current && *current == *witness;
		}
		void Cleanup() const noexcept
		{
			if (!owned || retain || !witness) return;
			try { if (Matches()) uam::io::RemoveAtomicTempNoThrow(path); }
			catch (...) {} // Unknown/changed stages remain evidence; destruction must not terminate.
		}
	};

	class PublicationRecorder
	{
	public:
		explicit PublicationRecorder(PublicationStage& stage) : m_stage(stage) {}
		bool Created(uam::io::AtomicWriteStage stage)
		{
			if (stage == uam::io::AtomicWriteStage::AfterCreate) m_stage.owned = true;
			return true;
		}
		bool Emitted(std::string_view bytes)
		{
			if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - m_bytes || !m_hash.Add(bytes)) return false;
			m_bytes += bytes.size();
			return true;
		}
		bool Complete(bool success)
		{
			if (!m_stage.owned) return false;
			const std::string digest = m_hash.Finish();
			if (m_stage.held_content)
			{
				auto physical = uam::platform::PublishedFileChangeWitness::Capture(m_stage.path);
				std::error_code error;
				const auto modified = fs::last_write_time(m_stage.path, error);
				if (digest.empty() || error || !m_stage.origin || !physical || !m_stage.origin->Matches(*physical) ||
				    !physical->MatchesContent(m_bytes, digest) || !physical->Current()) return false;
				m_stage.witness = PublicationWitness{m_bytes, modified, digest};
				m_stage.physical = std::move(physical);
				return success;
			}
			const auto current = ReadPublicationWitness(m_stage.path);
			if (digest.empty() || !current || current->bytes != m_bytes || current->digest != digest) return false;
			m_stage.witness = *current;
			return success; // A failed write may still leave a verified unused emitted prefix for cleanup.
		}
	private:
		PublicationStage& m_stage;
		uam::platform::IncrementalSha256 m_hash;
		std::uint64_t m_bytes = 0;
	};

	bool PreparedHistoryOwnerMatches(const PreparedChatSave& prepared, const ChatSession& committed)
	{
		if (!prepared.history_metadata) return true;
		const auto& history = *prepared.history_metadata;
		return !committed.messages_loaded && committed.metadata_incarnation == history.metadata_incarnation &&
		    committed.updated_at == history.updated_at && committed.persisted_message_count == history.expected_count &&
		    committed.persisted_messages_digest == history.expected_digest;
	}

	void AcknowledgePreparedHistory(const PreparedChatSave& prepared, ChatSession* committed)
	{
		if (!committed || !prepared.history_metadata || !PreparedHistoryOwnerMatches(prepared, *committed)) return;
		// Only a confirmed own primary publication reaches this point; never reread a newer path.
		committed->persisted_message_count = prepared.history_metadata->emitted_count;
		committed->persisted_messages_digest = prepared.history_metadata->emitted_digest;
	}

	bool PreparedChatAcknowledgementMatches(const PreparedChatSave& prepared, const ChatSession* committed)
	{
		return committed == nullptr || prepared.chat_id.empty() ||
		    (committed->id == prepared.chat_id && committed->acp_queue_storage_revision == prepared.queue_storage_revision &&
		     PreparedHistoryOwnerMatches(prepared, *committed));
	}

	bool PreparedChatStorageMatches(const PreparedChatSave& prepared)
	{
		if (!prepared.temporary_storage_owner_token.empty() &&
		    !PrimaryTemporaryOwnerMatches(prepared.primary_path, prepared.temporary_parent_chat_id, prepared.temporary_storage_owner_token)) return false;
		if (!prepared.queue_path.empty() && QueueJournalRevision(prepared.queue_path) != prepared.expected_queue_revision) return false;
		if (!prepared.transcript_source_path.empty() && StorageRevision(prepared.transcript_source_path) != prepared.transcript_source_revision) return false;
		if (!prepared.expected_storage_revision.empty() && StorageRevision(prepared.primary_path) != prepared.expected_storage_revision) return false;
		if (!prepared.expected_summary_revision.empty() && StorageRevision(prepared.summary_path) != prepared.expected_summary_revision) return false;
		return true;
	}
}

// The legacy serializer adapter never escapes this opt-in preparation.
class PreparedChatPublication
{
public:
	~PreparedChatPublication()
	{
		m_legacy.primary_temp.clear();
		m_legacy.summary_temp.clear();
		m_legacy.index_temp.clear();
		m_primary.Cleanup();
		m_summary.Cleanup();
		m_index.Cleanup();
		m_source_index.Cleanup();
	}
private:
	friend class ChatRepository;
	PreparedChatPublication() = default;

	bool HasOwnPublicationPredecessor() const { return m_stopped_read || m_own_predecessor; }
	bool OwnPublicationPredecessorCurrent() const
	{
		return m_own_predecessor ? m_own_predecessor->Current() : m_stopped_read && m_stopped_read->Current();
	}

	bool Write(PublicationStage& stage, const uam::io::AtomicTempProducer& produce)
	{
		if (HasOwnPublicationPredecessor())
		{
			if (!PublicationAbsent(stage.path)) return false;
			stage.held_content = true;
			PublicationRecorder recorder(stage);
			uam::io::AtomicWriteResult result;
			const uam::io::AtomicTempProducer writer = [&](const std::function<bool(std::string_view)>& sink)
			{
				return produce([&](std::string_view bytes) { return sink(bytes) && recorder.Emitted(bytes); });
			};
			const uam::io::AtomicWriteStageHook hook = [&](uam::io::AtomicWriteStage point)
			{
				return recorder.Created(point) && (!m_stopped_creation_hook || m_stopped_creation_hook(
				    &stage == &m_primary ? ChatPublicationFile::Primary : &stage == &m_summary ? ChatPublicationFile::Summary : ChatPublicationFile::MessageIndex, point));
			};
			return recorder.Complete(uam::io::WriteAndSyncAtomicTempWithOrigin(stage.path, writer, hook, result, stage.origin));
		}
		if (!PublicationAbsent(stage.path)) return false;
		PublicationRecorder recorder(stage);
		uam::io::AtomicWriteResult result;
		const bool written = uam::io::WriteAndSyncAtomicTemp(stage.path,
		    [&](const std::function<bool(std::string_view)>& sink)
		    {
			    return produce([&](std::string_view bytes)
			    {
				    return sink(bytes) && recorder.Emitted(bytes);
			    });
		    },
		    [&](uam::io::AtomicWriteStage point) { return recorder.Created(point); }, result);
		return recorder.Complete(written);
	}
	bool WriteText(PublicationStage& stage, std::string_view bytes)
	{
		return Write(stage, [&](const std::function<bool(std::string_view)>& sink) { return sink(bytes); });
	}
	bool FinishIndex(uam::chat_pages::Builder& builder, PublicationStage& stage, std::string_view revision,
	    std::uint64_t size, std::uint64_t begin, std::uint64_t end)
	{
		if (HasOwnPublicationPredecessor())
		{
			if (!PublicationAbsent(stage.path)) return false;
			stage.held_content = true;
			PublicationRecorder recorder(stage);
			const std::function<bool(uam::io::AtomicWriteStage)> hook = [&](uam::io::AtomicWriteStage point)
			{
				return recorder.Created(point) && (!m_stopped_creation_hook || m_stopped_creation_hook(ChatPublicationFile::MessageIndex, point));
			};
			return recorder.Complete(builder.FinishWithOrigin(stage.path, revision, size, begin, end, hook,
			    [&](std::string_view bytes) { return recorder.Emitted(bytes); }, stage.origin));
		}
		if (!PublicationAbsent(stage.path)) return false;
		PublicationRecorder recorder(stage);
		return recorder.Complete(builder.Finish(stage.path, revision, size, begin, end,
		    [&](uam::io::AtomicWriteStage point) { return recorder.Created(point); },
		    [&](std::string_view bytes) { return recorder.Emitted(bytes); }));
	}
	std::optional<uam::chat_pages::Index> SourceIndex(const fs::path& primary, std::string_view revision,
	    std::string_view owner, fs::path& actual_index_path)
	{
		actual_index_path = uam::chat_pages::IndexPath(primary);
		const auto cached = uam::chat_pages::ReadIndex(actual_index_path, revision);
		std::error_code ec;
		if (cached && cached->attachment_owner == owner && fs::file_size(primary, ec) == cached->source_size && !ec)
		{
			const auto metadata = ReadIndexedMetadata(primary, *cached);
			if (metadata && metadata->id == owner) return cached;
		}
		uam::chat_pages::Builder builder(primary, owner, m_source_records);
		std::uint64_t begin = 0, end = 0;
		const auto decode = [owner](std::string_view raw) -> std::optional<Message>
		{
			const auto value = ParseJson(raw);
			if (!value || value->type != JsonValue::Type::Object) return {};
			Message message = JsonToMessage(*value);
			if (!AttachmentsPersistable(message.attachments, owner)) return {};
			return message;
		};
		if (!uam::chat_pages::Scan(primary, builder, decode, begin, end)) return {};
		const auto size = fs::file_size(primary, ec);
		if (ec || StorageRevision(primary) != revision ||
		    !FinishIndex(builder, m_source_index, revision, size, begin, end)) return {};
		actual_index_path = m_source_index.path;
		const auto index = uam::chat_pages::ReadIndex(actual_index_path, revision);
		const auto metadata = index ? ReadIndexedMetadata(primary, *index) : std::nullopt;
		if (!index || index->attachment_owner != owner || !metadata || metadata->id != owner) return {};
		return index;
	}
	bool MatchesBeforePrimary() const
	{
		if (HasOwnPublicationPredecessor())
		{
			// This route never reads a queue, canonical birth or source index by path.
			return OwnPublicationPredecessorCurrent() && m_primary_before.physical &&
			    PublicationFileMatches(m_legacy.primary_path, m_primary_before) &&
			    (m_summary_before.absent ? StoppedPublicationAbsent(m_legacy.summary_path) : PublicationFileMatches(m_legacy.summary_path, m_summary_before)) &&
			    (m_index_before.absent ? StoppedPublicationAbsent(uam::chat_pages::IndexPath(m_legacy.primary_path)) : PublicationFileMatches(uam::chat_pages::IndexPath(m_legacy.primary_path), m_index_before)) &&
			    m_primary.Matches() && m_summary.Matches() && (m_legacy.index_temp.empty() || m_index.Matches()) &&
			    m_legacy.transcript_source_path.empty() && m_legacy.temporary_storage_owner_token.empty();
		}
		if (!PublicationDirectory(m_root) || !PreparedChatStorageMatches(m_legacy)) return false;
		if (m_mode == ChatPublicationMode::CreateAbsent)
		{
			if (!PublicationAbsent(m_legacy.primary_path) || !PublicationAbsent(uam::io::MakeBackupPath(m_legacy.primary_path))) return false;
		}
		else if (!PublicationFileMatches(m_legacy.primary_path, m_primary_before)) return false;
		return PublicationFileMatches(m_legacy.summary_path, m_summary_before) &&
		    PublicationFileMatches(uam::chat_pages::IndexPath(m_legacy.primary_path), m_index_before) &&
		    m_primary.Matches() && m_summary.Matches() &&
		    (m_legacy.index_temp.empty() || m_index.Matches()) &&
		    (!m_source_index.owned || m_source_index.Matches());
	}

	PreparedChatSave m_legacy;
	fs::path m_root;
	ChatPublicationMode m_mode = ChatPublicationMode::CreateAbsent;
	PublicationExpectedFile m_primary_before, m_summary_before, m_index_before;
	PublicationStage m_primary, m_summary, m_index, m_source_index;
	fs::path m_records, m_source_records;
	fs::path m_primary_recovery, m_summary_recovery, m_index_recovery;
	bool m_consumed = false;
	std::shared_ptr<const StoppedChatCanonicalRead> m_stopped_read;
	std::shared_ptr<const StoppedChatLaunchStorageReceipt> m_own_predecessor;
	std::optional<ChatSession> m_stopped_candidate;
	bool m_inline_primary = false;
	bool m_stopped_birth_verified = false;
	std::function<bool(ChatPublicationFile, uam::io::AtomicWriteStage)> m_stopped_creation_hook;
};

bool ChatRepository::SaveChatImpl(const std::filesystem::path& data_root, const ChatSession& chat, bool fail_if_exists, bool skip_unchanged, PreparedChatSave* prepared, std::string* fingerprint, std::string* saved_revision, PreparedChatPublication* publication, SavedChatPrimaryWitness* primary_witness)
try
{
	std::unique_lock<std::mutex> lock(ChatSaveMutex(), std::defer_lock);
	if (prepared == nullptr && fingerprint == nullptr) lock.lock();

	if ((fingerprint == nullptr && chat.managed_followup_input_unavailable) ||
	    (!chat.managed_followup_incarnation.empty() && !uam::uuid::IsCanonicalUuid(chat.managed_followup_incarnation)) ||
	    !std::ranges::all_of(chat.acp_queued_prompts, ValidManagedPrompt) ||
	    !uam::chat_ids::IsSafeStorageChatId(chat.id) || !ChatAttachmentsPersistable(chat))
	{
		return false;
	}

	const fs::path file_path = AppPaths::UamChatFilePath(data_root, chat.id);
	if (fingerprint == nullptr && prepared == nullptr &&
	    !CanonicalBirthAllowsValue(file_path, chat.managed_followup_incarnation)) return false;
	if (fingerprint == nullptr && !fail_if_exists && !chat.temporary_storage_owner_token.empty() &&
		!PrimaryTemporaryOwnerMatches(file_path, chat.temporary_parent_chat_id, chat.temporary_storage_owner_token)) return false;
	const fs::path queue_path = QueueJournalPath(data_root, chat.id);
	const bool stopped_publication = publication && publication->HasOwnPublicationPredecessor();
	if (stopped_publication && !publication->OwnPublicationPredecessorCurrent()) return false;
	const std::optional<std::string> queue_revision = fingerprint == nullptr && !stopped_publication ? QueueJournalRevision(queue_path) : std::optional<std::string>("missing");
	if (!queue_revision || (*queue_revision != "missing" &&
	    (chat.acp_queue_storage_error || *queue_revision != chat.acp_queue_storage_revision))) return false;
	std::string full_queue_revision = chat.acp_queue_full_save_revision;
	if (fingerprint == nullptr && *queue_revision != "missing")
	{
		const std::optional<JsonValue> saved = SavedQueueMetadata(data_root, chat.id);
		if (!saved || (chat.acp_queue_full_save_revision != JsonStringOrEmpty(saved->Find(kQueueFullRevisionField)) &&
		    QueuePayload(chat) != QueuePayload(*saved))) return false;
		full_queue_revision = uam::io::MakeTempWritePath(fs::path("queue-full")).filename().string();
	}
	if (prepared != nullptr)
	{
		prepared->chat_id = chat.id;
		prepared->temporary_parent_chat_id = chat.temporary_parent_chat_id;
		prepared->temporary_storage_owner_token = chat.temporary_storage_owner_token;
		prepared->queue_storage_revision = chat.acp_queue_storage_revision;
		prepared->published_queue_full_revision = full_queue_revision;
		prepared->queue_path = queue_path;
		prepared->expected_queue_revision = *queue_revision;
	}
	if (fail_if_exists &&
	    (uam::paths::PathExistsNoThrow(file_path) ||
	     uam::paths::PathExistsNoThrow(uam::io::MakeBackupPath(file_path))))
	{
		return false;
	}

	auto passive_archive = chat.passive_queued_prompt_archive;
	if (chat.imported_read_only)
	{
		if (passive_archive)
		{
			passive_archive = ValidatePassiveArchive(*passive_archive, chat.id);
			if (!passive_archive) return false;
		}
		if (fingerprint == nullptr)
		{
			const auto revision = StorageRevision(file_path);
			if (!revision) return false;
			if (*revision != "missing")
			{
				auto current = ReadPrimaryPassiveArchive(file_path, chat.id, *revision);
				if (!current && prepared && prepared->passive_archive_worker_migration)
				{
					// Worker preparation enables bounded legacy migration. Synchronous
					// SaveChat enables it only after a revision-bound active transition
					// probe; ordinary passive metadata calls never enable this read.
					std::string canonical;
					if (!uam::paths::IsLinkOrReparsePointNoThrow(file_path) &&
					    uam::io::TryReadTextFile(file_path, canonical, kMaxChatJsonBytes) && StorageRevision(file_path) == revision)
						current = ChatRepository::ParsePassiveQueuedPromptArchive(canonical, chat.id);
				}
				if (!current || (passive_archive && !SamePassiveArchive(current, passive_archive))) return false;
				passive_archive = current; // Unknown never means permission to erase an existing archive.
			}
			else if (!passive_archive)
			{
				JsonValue empty = uam::json::Object();
				uam::json::SetString(empty, kChatIdField, chat.id);
				passive_archive = PassiveArchiveFromDocument(empty, chat.id);
				if (!passive_archive) return false;
			}
		}
	}

	std::error_code ec;
	if (fingerprint == nullptr && !uam::paths::CreateDirectoriesNoThrow(file_path.parent_path(), &ec))
	{
		return false;
	}

	JsonValue root = uam::json::Object();
	uam::json::SetString(root, kChatIdField, chat.id);
	uam::json::SetString(root, kChatExecutionHostIdField,
	    uam::strings::NonEmptyOrFallback(chat.execution_host_id, "local"));
	uam::json::SetString(root, kChatProviderIdField, chat.provider_id);
	uam::json::SetString(root, kChatNativeSessionIdField, chat.native_session_id);
	if (chat.opencode_v2_session_id) uam::json::SetString(root, kChatOpenCodeV2SessionIdField, *chat.opencode_v2_session_id);
	if (!chat.claude_quota_continuation.id.empty())
	{
		const ClaudeQuotaContinuation& owner = chat.claude_quota_continuation;
		JsonValue quota = uam::json::Object();
		uam::json::SetString(quota, "id", owner.id);
		uam::json::SetString(quota, "state", owner.state);
		uam::json::SetString(quota, "chat_id", owner.chat_id);
		uam::json::SetString(quota, "native_session_id", owner.native_session_id);
		uam::json::SetString(quota, "context", owner.context);
		uam::json::SetString(quota, "accepted_message_digest", owner.accepted_message_digest);
		uam::json::SetString(quota, "agent_id", owner.agent_id);
		uam::json::SetString(quota, "agent_definition_hash", owner.agent_definition_hash);
		uam::json::SetString(quota, "agent_workspace_access", owner.agent_workspace_access);
		uam::json::SetString(quota, "agent_execution_capability", owner.agent_execution_capability);
		uam::json::SetNumber(quota, "observed_at_epoch_s", static_cast<double>(owner.observed_at_epoch_s));
		uam::json::SetNumber(quota, "resets_at_epoch_s", static_cast<double>(owner.resets_at_epoch_s));
		uam::json::SetNumber(quota, "user_message_index", owner.user_message_index);
		uam::json::SetNumber(quota, "message_count", static_cast<double>(owner.message_count));
		uam::json::SetValue(root, "claude_quota_continuation", std::move(quota));
	}
	uam::json::SetBool(root, kChatRemoteClaudeSessionUnstartedField,
	                   chat.remote_claude_session_unstarted);
	uam::json::SetBool(root, kChatRemoteTurnReconnectPendingField,
	                   chat.remote_turn_reconnect_pending);
	uam::json::SetBool(root, kChatRemoteProcessExistsField,
	                   chat.remote_process_exists);
	uam::json::SetBool(root, "remote_prompt_image_supported", chat.remote_process_exists && chat.remote_prompt_image_supported);
	uam::json::SetString(root, "remote_structured_protocol", chat.remote_process_exists ? chat.remote_structured_protocol : "");
	uam::json::SetNumber(root, "remote_opencode_native_port", chat.remote_process_exists ? chat.remote_opencode_native_port : 0);
	uam::json::SetString(root, "remote_opencode_native_token", chat.remote_process_exists ? chat.remote_opencode_native_token : "");
	uam::json::SetBool(root, "remote_manual_compaction_pending", chat.remote_process_exists && chat.remote_manual_compaction_pending);
	uam::json::SetString(root, "remote_manual_compaction_boundary_json",
	    chat.remote_manual_compaction_pending || HasOwnedClaudeMaintenanceTerminalReceipt(chat) ? chat.remote_manual_compaction_boundary_json : "");
	uam::json::SetBool(root, kChatRemoteStopCleanupPendingField,
	                   chat.remote_stop_cleanup_pending);
	uam::json::SetBool(root, "native_session_reset_pending", chat.native_session_reset_pending);
	uam::json::SetString(root, "provider_handoff_context", chat.provider_handoff_context);
	uam::json::SetString(root, "provider_handoff_session_id", chat.provider_handoff_session_id);
	uam::json::SetBool(root, "remote_recovery_enabled", chat.remote_recovery_enabled);
	uam::json::SetString(root, "remote_recovery_state", chat.remote_recovery_state);
	uam::json::SetString(root, "routing_mode", chat.routing_mode);
	uam::json::SetValue(root, "routing_inherited_default_fields", StringArrayToJson(chat.routing_inherited_default_fields));
	uam::json::SetString(root, "routing_source_folder_id", chat.routing_source_folder_id);
	uam::json::SetString(root, "routing_source_directory", chat.routing_source_directory);
	uam::json::SetString(root, "routing_assigned_connection_identity", chat.routing_assigned_connection_identity);
	uam::json::SetString(root, "routing_assigned_provider_configuration_identity", chat.routing_assigned_provider_configuration_identity);
	uam::json::SetBool(root, "routing_reservation_pending", chat.routing_reservation_pending);
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
	if (chat.remote_active_turn_id.size() > 256 || chat.remote_turn_serial < 0 ||
	    chat.remote_turn_user_message_index < -1) return false;
	uam::json::SetString(root, kChatRemoteActiveTurnIdField, chat.remote_active_turn_id);
	uam::json::SetNumber(root, kChatRemoteTurnSerialField, chat.remote_turn_serial);
	uam::json::SetNumber(root, kChatRemoteTurnUserMessageIndexField, chat.remote_turn_user_message_index);
	if (chat.remote_next_request_id < 1 ||
	    chat.remote_next_request_id >= std::numeric_limits<int>::max()) return false;
	uam::json::SetNumber(root, kChatRemoteNextRequestIdField, chat.remote_next_request_id);
	JsonValue pending_requests = uam::json::Array();
	std::size_t pending_payload_bytes = 0;
	std::vector<int> pending_request_ids;
	for (const uam::AcpRemotePendingRequestState& request : chat.remote_pending_requests)
	{
		if (pending_requests.array_value.size() >= kMaxPersistedRemotePendingRequests) return false;
		if (!ValidRemotePendingRequest(request, pending_payload_bytes) ||
		    std::find(pending_request_ids.begin(), pending_request_ids.end(), request.request_id) !=
		        pending_request_ids.end()) return false;
		pending_payload_bytes += request.payload.size();
		pending_request_ids.push_back(request.request_id);
		uam::json::PushValue(pending_requests, RemotePendingRequestToJson(request));
	}
	uam::json::SetValue(root, kChatRemotePendingRequestsField, std::move(pending_requests));
	JsonValue queued_prompts = uam::json::Array();
	std::size_t queued_prompt_text_bytes = 0;
	if (chat.imported_read_only && passive_archive)
	{
		auto archive = ParseJson(passive_archive->canonical_prompts_json);
		if (!archive || archive->type != JsonValue::Type::Array) return false;
		queued_prompts = std::move(*archive);
	}
	else
	{
		for (const uam::AcpQueuedUserPromptState& prompt : chat.acp_queued_prompts)
		{
			if (queued_prompts.array_value.size() >= kMaxPersistedAcpQueuedPrompts ||
			    (prompt.text.empty() && prompt.attachments.empty()) || prompt.text.size() > kMaxPersistedAcpQueuedPromptBytes -
			    std::min(queued_prompt_text_bytes, kMaxPersistedAcpQueuedPromptBytes)) break;
			queued_prompt_text_bytes += prompt.text.size();
			uam::json::PushValue(queued_prompts, AcpQueuedPromptToJson(prompt));
		}
	}
	uam::json::SetValue(root, kChatAcpQueuedPromptsField, std::move(queued_prompts));
	uam::json::SetString(root, kQueueRevisionField, chat.acp_queue_storage_revision);
	uam::json::SetString(root, kQueueFullRevisionField, full_queue_revision);
	uam::json::SetBool(root, kChatAcpQueueHeldField, chat.acp_queue_held);
	uam::json::SetNumber(root, kChatAcpDispatchedQueuedPromptCountField,
	                     static_cast<double>(std::min(
	                         chat.acp_dispatched_queued_prompt_count,
	                         chat.acp_queued_prompts.size())));
	uam::json::SetString(root, "temporary_parent_chat_id", chat.temporary_parent_chat_id);
	SetNonEmptyString(root, "temporary_storage_owner_token", chat.temporary_storage_owner_token);
	uam::json::SetBool(root, "side_cleanup_requested", chat.side_cleanup_requested);
	uam::json::SetString(root, kChatParentChatIdField, chat.parent_chat_id);
	uam::json::SetString(root, kChatBranchRootChatIdField, chat.branch_root_chat_id);
	uam::json::SetString(root, kChatSubagentParentChatIdField, chat.subagent_parent_chat_id);
	uam::json::SetString(root, kChatSubagentParentCreatedAtField, chat.subagent_parent_created_at);
	uam::json::SetNumber(root, kChatBranchFromMessageIndexField, static_cast<double>(chat.branch_from_message_index));
	uam::json::SetBool(root, kChatBranchMessageEditedField, chat.branch_message_edited);
	uam::json::SetString(root, kChatBranchOperationIdField, chat.branch_operation_id);
	uam::json::SetString(root, kChatBranchRetryErrorField, chat.branch_retry_error);
	{
		std::string folder_id_sanitized = chat.folder_id;
		std::erase_if(folder_id_sanitized, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
		uam::json::SetString(root, kChatFolderIdField, std::move(folder_id_sanitized));
	}
	uam::json::SetString(root, kChatTitleField, chat.title);
	uam::json::SetValue(root, kChatCustomIconField, CustomIconToJson(chat.custom_icon));
	uam::json::SetString(root, kChatCreatedAtField, chat.created_at);
	uam::json::SetString(root, kChatUpdatedAtField, chat.updated_at);
	uam::json::SetString(root, kChatLastOpenedAtField, uam::strings::NonEmptyOrFallback(chat.last_opened_at, chat.updated_at));
	uam::json::SetString(root, "interaction_at", chat.interaction_at);
	uam::json::SetString(root, "attention_revision", chat.attention_revision);
	uam::json::SetString(root, "settled_at", chat.settled_at);
	uam::json::SetNumber(root, "snoozed_until_ms", static_cast<double>(chat.snoozed_until_ms));
	uam::json::SetString(root, "last_stop_reason", chat.last_stop_reason);
	uam::json::SetString(root, "goal_command_revision", chat.goal_command_revision);
	uam::json::SetString(root, "goal_pending_continuation_id", chat.goal_pending_continuation_id);
	uam::json::SetBool(root, kChatPinnedField, chat.pinned);
	uam::json::SetValue(root, kChatLinkedFilesField, StringArrayToJson(chat.linked_files));
	uam::json::SetString(root, kChatWorkspaceDirectoryField, chat.workspace_directory);
	uam::json::SetString(root, kChatWorkspaceIsolationKindField, chat.workspace_isolation_kind);
	uam::json::SetString(root, kChatWorkspaceSourceDirectoryField, chat.workspace_source_directory);
	uam::json::SetString(root, kChatWorkspaceBaseRefField, chat.workspace_base_ref);
	uam::json::SetValue(root, kChatGitBaseBranchOverrideField, chat.git_base_branch_override ? uam::json::String(*chat.git_base_branch_override) : JsonValue{});
	uam::json::SetString(root, kChatWorkspaceBranchNameField, chat.workspace_branch_name);
	uam::json::SetString(root, kChatWorkspaceWorktreeDirectoryField, chat.workspace_worktree_directory);
	uam::json::SetBool(root, kChatSvnUpdatePendingField, chat.svn_update_pending);
	uam::json::SetBool(root, kChatImportedReadOnlyField, chat.imported_read_only);
	if (!chat.imported_read_only && !chat.acp_unsent_prompt_recoveries.empty())
		uam::json::SetValue(root, "acp_unsent_prompt_recoveries", UnsentRecoveriesToJson(chat));
	uam::json::SetString(root, kChatApprovalModeField, chat.approval_mode);
	uam::json::SetString(root, kChatUamAgentIdField, uam::strings::NonEmptyOrFallback(chat.uam_agent_id, "build"));
	uam::json::SetString(root, kChatLastPromptAgentDefinitionHashField, chat.last_prompt_agent_definition_hash);
	uam::json::SetString(root, "provider_handoff_context", chat.provider_handoff_context);
	uam::json::SetString(root, "provider_handoff_session_id", chat.provider_handoff_session_id);
	JsonValue contexts = uam::json::Array();
	for (const ProviderHandoffCliContext& location : chat.provider_handoff_cli_contexts)
	{
		JsonValue context = uam::json::Object();
		uam::json::SetString(context, "host", location.execution_host_id);
		uam::json::SetString(context, "directory", location.directory);
		uam::json::SetString(context, "connection", location.connection_identity);
		uam::json::PushValue(contexts, std::move(context));
	}
	uam::json::SetValue(root, "provider_handoff_cli_contexts", std::move(contexts));
	uam::json::SetString(root, kChatAgentRunIdField, chat.agent_run_id);
	if (!chat.managed_followup_incarnation.empty()) uam::json::SetString(root, kChatManagedFollowupBirthField, chat.managed_followup_incarnation);
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
	const std::string memory_level = uam::memory_levels::NormalizeSelection(chat.memory_level, chat.memory_enabled);
	uam::json::SetString(root, kChatMemoryLevelField, memory_level);
	uam::json::SetBool(root, kChatMemoryEnabledField, uam::memory_levels::IsEnabled(memory_level));
	uam::json::SetNumber(root, kChatMemoryLastProcessedMessageCountField, static_cast<double>(chat.memory_last_processed_message_count));
	uam::json::SetString(root, kChatMemoryLastProcessedAtField, chat.memory_last_processed_at);
	uam::json::SetBool(root, kChatSmallModelModeField, chat.small_model_mode);
	uam::json::SetBool(root, "uamControlEnabled", chat.uam_control_enabled);
	uam::json::SetBool(root, "uamControlExplicitlyDisabled", chat.uam_control_explicitly_disabled);

	std::size_t persisted_message_count = chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count;
	std::string persisted_messages_digest = chat.messages_loaded && fingerprint == nullptr
	    ? SummaryDigest(chat, persisted_message_count)
	    : chat.persisted_messages_digest;
	bool preserve_existing_primary_as_backup = true;
	const bool stream_primary = prepared != nullptr && fingerprint == nullptr &&
	    !(publication && publication->m_inline_primary) && (chat.imported_read_only || (chat.messages_loaded && !chat.messages.empty()) || (!chat.messages_loaded && chat.persisted_message_count > 0));
	if (fingerprint == nullptr && chat.messages_loaded && !chat.messages.empty() && !stream_primary)
	{
		JsonValue msgs = uam::json::Array();
		for (const auto& m : chat.messages)
		{
			uam::json::PushValue(msgs, MessageToJson(m));
		}
		uam::json::SetValue(root, kChatMessagesField, std::move(msgs));
	}
	else if (fingerprint == nullptr && !chat.messages_loaded && !stream_primary)
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
			if (goal.native_binding.has_value())
			{
				const NativeGoalBinding& receipt = *goal.native_binding;
				JsonValue binding = uam::json::Object();
				uam::json::SetNumber(binding, "version", receipt.version);
				uam::json::SetString(binding, "providerId", receipt.provider_id);
				uam::json::SetString(binding, "executionHostId", receipt.execution_host_id);
				uam::json::SetString(binding, "threadId", receipt.thread_id);
			uam::json::SetString(binding, "uamChatId", receipt.uam_chat_id);
			uam::json::SetString(binding, "uamGoalId", receipt.uam_goal_id);
				uam::json::SetString(binding, "objective", receipt.objective);
				uam::json::SetNumber(binding, "createdAt", static_cast<double>(receipt.created_at));
				uam::json::SetNumber(binding, "updatedAt", static_cast<double>(receipt.last_updated_at));
				uam::json::SetNumber(binding, "uamTokensAtCreation", static_cast<double>(receipt.uam_tokens_at_creation));
				uam::json::SetNumber(binding, "uamBudgetAtCreation", static_cast<double>(receipt.uam_budget_at_creation));
				uam::json::SetNumber(binding, "nativeTokensUsed", static_cast<double>(receipt.last_native_tokens_used));
				uam::json::SetValue(binding, "nativeTokenBudget", receipt.native_token_budget.has_value()
					? uam::json::Number(static_cast<double>(*receipt.native_token_budget)) : JsonValue{});
				if (receipt.initial_delivery.has_value())
				{
					const NativeGoalInitialDelivery& witness = *receipt.initial_delivery;
					JsonValue delivery = uam::json::Object();
					uam::json::SetNumber(delivery, "userMessageIndex", static_cast<double>(witness.user_message_index));
					uam::json::SetString(delivery, "userMessageCreatedAt", witness.user_message_created_at);
					uam::json::SetString(delivery, "state", witness.state == NativeGoalInitialDelivery::State::Pending ? "pending" :
						witness.state == NativeGoalInitialDelivery::State::Attempted ? "attempted" :
						witness.state == NativeGoalInitialDelivery::State::Accepted ? "accepted" : "invalid");
					uam::json::SetString(delivery, "turnId", witness.turn_id);
					uam::json::SetValue(binding, "initialDelivery", std::move(delivery));
				}
				uam::json::SetValue(goal_obj, "nativeBinding", std::move(binding));
			}
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

	if (!uam::managed_parent_history::ValidCollection(chat.managed_parent_input_receipts)) return false;
	if (chat.managed_parent_input_receipts_present || !chat.managed_parent_input_receipts.empty() ||
	    !chat.managed_parent_result || chat.managed_parent_result->accepted_turn_id.empty())
	{
		JsonValue values = uam::json::Array();
		for (const auto& receipt : chat.managed_parent_input_receipts)
		{
			JsonValue value = uam::json::Object();
			uam::json::SetNumber(value, "version", receipt.version);
			uam::json::SetString(value, "id", receipt.id);
			uam::json::SetString(value, "originId", receipt.origin_id);
			uam::json::SetString(value, "rootId", receipt.root_id);
			uam::json::SetString(value, "providerId", receipt.provider_id);
			uam::json::SetString(value, "hostId", receipt.host_id);
			uam::json::SetString(value, "workspace", receipt.workspace);
			uam::json::SetString(value, "nativeThread", receipt.native_thread);
			uam::json::SetString(value, "originTurn", receipt.origin_turn);
			uam::json::SetString(value, "payload", receipt.payload);
			uam::json::SetString(value, "nativeRequestId", receipt.native_request_id);
			uam::json::SetString(value, "acceptedTurnId", receipt.accepted_turn_id);
			JsonValue ids = uam::json::Array();
			for (const auto& id : receipt.run_ids) uam::json::PushValue(ids, uam::json::String(id));
			uam::json::SetValue(value, "runIds", std::move(ids));
			// Request IDs occur in both the replaceable record and this slot; retain a constant pre-ACK byte budget.
			if (receipt.accepted_turn_id.empty())
				uam::json::SetString(value, "ackReservation", std::string(uam::managed_parent_history::kAckReservedBytes - 2 * receipt.native_request_id.size(), 'x'));
			uam::json::PushValue(values, std::move(value));
		}
		uam::json::SetValue(root, "managedParentInputReceipts", std::move(values));
	}

	if (chat.managed_parent_result)
	{
		const auto& record = *chat.managed_parent_result;
		if (record.version != 1 || record.run_ids.size() > 4 || record.delivered_ids.size() > 4 || record.batch_ids.size() > 4 || record.payload.size() > 72 * 1024) return false;
		JsonValue value = uam::json::Object();
		uam::json::SetNumber(value, "version", record.version);
		uam::json::SetString(value, "id", record.id);
		uam::json::SetString(value, "rootId", record.root_id);
		uam::json::SetString(value, "providerId", record.provider_id);
		uam::json::SetString(value, "hostId", record.host_id);
		uam::json::SetString(value, "workspace", record.workspace);
		uam::json::SetString(value, "nativeThread", record.native_thread);
		uam::json::SetString(value, "policyDigest", record.policy_digest);
		uam::json::SetString(value, "stage", record.stage);
		uam::json::SetString(value, "reason", record.reason);
		uam::json::SetString(value, "originTurn", record.origin_turn);
		uam::json::SetString(value, "originUserDigest", record.origin_user_digest);
		uam::json::SetString(value, "payload", record.payload);
		uam::json::SetString(value, "nativeRequestId", record.native_request_id);
		uam::json::SetString(value, "acceptedTurnId", record.accepted_turn_id);
		for (const auto& [key, ids] : std::vector<std::pair<std::string, std::vector<std::string>>>{{"runIds", record.run_ids}, {"deliveredIds", record.delivered_ids}, {"batchIds", record.batch_ids}})
		{
			JsonValue array = uam::json::Array();
			for (const std::string& id : ids) uam::json::PushValue(array, uam::json::String(id));
			uam::json::SetValue(value, key, std::move(array));
		}
		uam::json::SetValue(root, "managedParentResult", std::move(value));
	}

	// Serialize active_goal_id
	if (!chat.active_goal_id.empty())
	{
		uam::json::SetString(root, "activeGoalId", chat.active_goal_id);
	}

	if (fingerprint != nullptr)
	{
		if (chat.imported_read_only) uam::json::SetBool(root, "validationPassiveArchiveUnknown", !passive_archive);
		uam::json::SetBool(root, "validationQueueStorageError", chat.acp_queue_storage_error);
		uam::json::SetBool(root, "validationManagedFollowupInputUnavailable", chat.managed_followup_input_unavailable);
		uam::json::SetBool(root, "validationMessagesLoaded", chat.messages_loaded);
		uam::json::SetNumber(root, "validationPersistedCount", static_cast<double>(chat.persisted_message_count));
		uam::json::SetString(root, "validationPersistedDigest", chat.persisted_messages_digest);
		*fingerprint = SerializeArchiveDocument(root, passive_archive, kMaxChatJsonBytes);
		return true;
	}
	if (stream_primary)
	{
		std::optional<uam::chat_pages::Index> source_index;
		fs::path source_path;
		std::string source_revision;
		fs::path source_index_path;
		if (!chat.messages_loaded && chat.persisted_message_count > 0)
		{
			for (const fs::path& candidate : {file_path, uam::io::MakeBackupPath(file_path)})
			{
				if (publication != nullptr && candidate != file_path) break; // Explicit primary only; no backup adoption.
				const std::optional<std::string> revision = StorageRevision(candidate);
				if (!revision || *revision == "missing") continue;
				const std::optional<uam::chat_pages::Index> index = publication != nullptr ?
				    publication->SourceIndex(candidate, *revision, chat.id, source_index_path) : EnsureMessageIndex(candidate, *revision, chat.id);
				if (publication == nullptr) source_index_path = uam::chat_pages::IndexPath(candidate);
				const std::optional<ChatSession> metadata = index ? ReadIndexedMetadata(candidate, *index) : std::nullopt;
				if (!metadata || metadata->id != chat.id || index->message_count == 0) continue;
				source_index = index;
				source_path = candidate;
				source_revision = *revision;
				persisted_message_count = static_cast<std::size_t>(index->message_count);
				persisted_messages_digest = SummaryDigest(chat, persisted_message_count);
				preserve_existing_primary_as_backup = candidate == file_path;
				break;
			}
			if (!source_index) return false;
		}
		prepared->primary_path = file_path;
		prepared->primary_temp = publication != nullptr ? publication->m_primary.path : uam::io::MakeTempWritePath(file_path);
		prepared->summary_path = AppPaths::UamChatSummaryFilePath(data_root, chat.id);
		prepared->summary_temp = publication != nullptr ? publication->m_summary.path : uam::io::MakeTempWritePath(prepared->summary_path);
		prepared->index_path = uam::chat_pages::IndexPath(file_path);
		prepared->index_temp = publication != nullptr ? publication->m_index.path : uam::io::MakeTempWritePath(prepared->index_path);
		prepared->preserve_backup = preserve_existing_primary_as_backup;
		prepared->transcript_source_path = source_path;
		prepared->transcript_source_revision = source_revision;
		if (!uam::paths::CreateDirectoriesNoThrow(prepared->summary_path.parent_path())) return false;
		std::optional<uam::chat_pages::Builder> builder;
		if (publication != nullptr) builder.emplace(file_path, chat.id, publication->m_records);
		else builder.emplace(file_path, chat.id);
		std::uint64_t array_begin = 0;
		std::uint64_t written = 0, array_end = 0;
		std::unique_ptr<uam::platform::IncrementalSha256> own_primary_hash;
		if (primary_witness) own_primary_hash = std::make_unique<uam::platform::IncrementalSha256>();
		const std::string fields = SerializeArchiveDocument(root, passive_archive, kMaxChatJsonBytes);
		uam::io::AtomicWriteResult result;
		const uam::io::AtomicTempProducer writer = [&](const std::function<bool(std::string_view)>& sink)
		{
			const std::function<bool(std::string_view)> append = [&](std::string_view bytes)
			{
				// Loaded saves retain the full-loader ceiling; unloaded copies preserve complete existing transcripts.
				if (chat.messages_loaded && bytes.size() > kMaxChatJsonBytes - written) return false;
				if (own_primary_hash && !own_primary_hash->Add(bytes)) return false;
				if (!sink(bytes)) return false;
				written += bytes.size();
				return true;
			};
			if (!append("{\"messages\":[")) return false;
			array_begin = written - 1;
			if (source_index)
			{
				if (!builder->CopyRecords(source_index_path, *source_index, array_begin)) return false;
				std::ifstream input(source_path, std::ios::binary);
				input.seekg(static_cast<std::streamoff>(source_index->array_begin + 1));
				std::uint64_t remaining = source_index->array_end - source_index->array_begin - 1;
				std::array<char, 65536> buffer{};
				while (remaining != 0)
				{
					const std::size_t size = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
					if (!input.read(buffer.data(), static_cast<std::streamsize>(size)) || !append(std::string_view(buffer.data(), size))) return false;
					remaining -= size;
				}
			}
			else
			{
				for (const Message& message : chat.messages)
				{
					if (builder->Count() != 0 && !append(",")) return false;
					const JsonValue value = MessageToJson(message);
					const std::string text = SerializeJson(value, kMaxChatJsonBytes - static_cast<std::size_t>(written));
					const uam::chat_pages::Span span{written, text.size()};
					if (!append(text) || !builder->Add(span, JsonToMessage(value))) return false;
				}
			}
			array_end = written;
			return append("],") && !fields.empty() && append(std::string_view(fields).substr(1));
		};
		if (publication != nullptr ? !publication->Write(publication->m_primary, writer) :
		    !uam::io::WriteAndSyncAtomicTemp(prepared->primary_temp, writer, {}, result)) return false;
		const std::optional<std::string> revision = StorageRevision(prepared->primary_temp);
		if (!revision || (!source_path.empty() && StorageRevision(source_path) != source_revision)) return false;
		prepared->primary_revision = *revision;
		if (primary_witness)
		{
			const std::string digest = own_primary_hash->Finish();
			if (digest.empty()) return false;
			*primary_witness = {*revision, written, digest};
		}
		if (publication != nullptr ? !publication->FinishIndex(*builder, publication->m_index, *revision, written, array_begin, array_end) :
		    !builder->Finish(prepared->index_temp, *revision, written, array_begin, array_end)) return false;
		uam::json::SetNumber(root, kChatPersistedMessageCountField, static_cast<double>(persisted_message_count));
		uam::json::SetString(root, kChatPersistedMessagesDigestField, persisted_messages_digest);
		uam::json::SetNumber(root, kChatSummarySourceSizeField, static_cast<double>(written));
		const std::string summary = SerializeArchiveDocument(root, passive_archive, kMaxChatJsonBytes);
		const bool staged = publication != nullptr ? publication->WriteText(publication->m_summary, summary) :
		    uam::io::WriteAndSyncAtomicTemp(prepared->summary_temp, summary, {}, result);
		if (staged && !chat.messages_loaded)
		{
			// Count comes from the actual source index, not an assumed cold-summary count.
			prepared->history_metadata = PreparedChatHistoryMetadata{chat.metadata_incarnation, chat.updated_at,
			    chat.persisted_message_count, chat.persisted_messages_digest,
			    persisted_message_count, persisted_messages_digest};
		}
		return staged;
	}
	const std::string json = SerializeArchiveDocument(root, passive_archive, kMaxChatJsonBytes);
	const auto matches_file = [](const fs::path& path, const std::string& content)
	{
		std::error_code error;
		const fs::file_status status = fs::symlink_status(path, error);
		std::string existing;
		return !error && fs::is_regular_file(status) &&
		       uam::io::TryReadTextFile(path, existing, content.size()) && existing == content;
	};

	root.object_value.erase(std::string(kChatMessagesField));
	uam::json::SetNumber(root, kChatPersistedMessageCountField, static_cast<double>(persisted_message_count));
	uam::json::SetString(root, kChatPersistedMessagesDigestField,
	                     persisted_messages_digest.empty() ? SummaryDigest(chat, persisted_message_count) : persisted_messages_digest);
	uam::json::SetNumber(root, kChatSummarySourceSizeField, static_cast<double>(json.size()));
	const fs::path summary_path = AppPaths::UamChatSummaryFilePath(data_root, chat.id);
	const std::string summary_json = SerializeArchiveDocument(root, passive_archive, kMaxChatJsonBytes);
	if (prepared != nullptr)
	{
		if (!uam::paths::CreateDirectoriesNoThrow(summary_path.parent_path())) return false;
		prepared->primary_path = file_path;
		prepared->primary_temp = publication != nullptr ? publication->m_primary.path : uam::io::MakeTempWritePath(file_path);
		prepared->summary_path = summary_path;
		prepared->summary_temp = publication != nullptr ? publication->m_summary.path : uam::io::MakeTempWritePath(summary_path);
		prepared->preserve_backup = preserve_existing_primary_as_backup;
		uam::io::AtomicWriteResult result;
		if (publication != nullptr)
			return publication->WriteText(publication->m_primary, json) && publication->WriteText(publication->m_summary, summary_json);
		return uam::io::WriteAndSyncAtomicTemp(prepared->primary_temp, json, {}, result) &&
		       uam::io::WriteAndSyncAtomicTemp(prepared->summary_temp, summary_json, {}, result);
	}
	const bool primary_unchanged = skip_unchanged && matches_file(file_path, json);
	const bool chat_saved = primary_unchanged || (preserve_existing_primary_as_backup
	    ? uam::io::WriteTextFileWithBackup(file_path, json)
	    : uam::io::WriteTextFile(file_path, json));
	if (!chat_saved)
	{
		return false;
	}

	if (!primary_unchanged || !SummaryCacheIsCurrent(file_path, summary_path) || !matches_file(summary_path, summary_json))
	{
		(void)uam::io::WriteTextFile(summary_path, summary_json);
	}
	chat.acp_queue_full_save_revision = full_queue_revision;
	if (primary_witness != nullptr)
	{
		// This digest binds the JSON emitted by THIS save, even if a foreign file
		// wins before the subsequent revision sample. A worker must verify both.
		uam::platform::IncrementalSha256 hash;
		if (!hash.Add(json)) return false;
		const std::string digest = hash.Finish();
		const std::optional<std::string> revision = StorageRevision(file_path);
		if (digest.empty() || !revision || *revision == "missing") return false;
		*primary_witness = {*revision, static_cast<std::uint64_t>(json.size()), digest};
	}
	if (saved_revision != nullptr)
	{
		const std::optional<std::string> revision = StorageRevision(file_path);
		if (!revision) return false;
		// A small-child receipt binds exact bytes, including same-size/same-time replacement.
		*saved_revision = *revision + "\n" + json;
	}
	return true;
}

catch (const std::length_error& error)
{
	uam::diagnostics::Write(std::string("Chat save rejected: ") + error.what());
	return false;
}

PreparedChatSave::~PreparedChatSave()
{
	uam::io::RemoveAtomicTempNoThrow(primary_temp);
	uam::io::RemoveAtomicTempNoThrow(summary_temp);
	uam::io::RemoveAtomicTempNoThrow(index_temp);
}

std::optional<std::string> ChatRepository::ChatStorageRevision(const fs::path& data_root, std::string_view chat_id)
{
	if (!uam::chat_ids::IsSafeStorageChatId(chat_id)) return std::nullopt;
	return StorageRevision(AppPaths::UamChatFilePath(data_root, chat_id));
}

std::optional<std::string> ChatRepository::ChatSummaryStorageRevision(const fs::path& data_root, std::string_view chat_id)
{
	if (!uam::chat_ids::IsSafeStorageChatId(chat_id)) return std::nullopt;
	return StorageRevision(AppPaths::UamChatSummaryFilePath(data_root, chat_id));
}

std::shared_ptr<PreparedChatSave> ChatRepository::PrepareChatSave(const fs::path& data_root, const ChatSession& chat, std::string_view expected_storage_revision)
{
	if (!expected_storage_revision.empty() && ChatStorageRevision(data_root, chat.id) != expected_storage_revision) return nullptr;
	std::shared_ptr<PreparedChatSave> prepared = std::make_shared<PreparedChatSave>();
	prepared->passive_archive_worker_migration = chat.imported_read_only;
	prepared->expected_storage_revision = expected_storage_revision.empty() ? ChatStorageRevision(data_root, chat.id).value_or("unreadable") : std::string(expected_storage_revision);
	if (!SaveChatImpl(data_root, chat, false, false, prepared.get())) return nullptr;
	const std::optional<std::string> revision = StorageRevision(prepared->primary_temp);
	if (!revision) return nullptr;
	prepared->primary_revision = *revision;
	if (!expected_storage_revision.empty() && ChatStorageRevision(data_root, chat.id) != expected_storage_revision) return nullptr;
	return prepared;
}

std::shared_ptr<PreparedChatSave> ChatRepository::PrepareImportedChatSave(const std::filesystem::path& data_root,
    ChatSession& metadata, const ImportMessageProducer& produce)
try
{
	if (!uam::chat_ids::IsSafeStorageChatId(metadata.id) || !metadata.messages.empty()) return nullptr;
	std::shared_ptr<PreparedChatSave> prepared = std::make_shared<PreparedChatSave>();
	prepared->primary_path = AppPaths::UamChatFilePath(data_root, metadata.id);
	prepared->primary_temp = uam::io::MakeTempWritePath(prepared->primary_path);
	prepared->expected_storage_revision = ChatStorageRevision(data_root, metadata.id).value_or("unreadable");
	if (!uam::paths::CreateDirectoriesNoThrow(prepared->primary_path.parent_path())) return nullptr;
	std::shared_ptr<PreparedChatSave> base;
	std::size_t message_count = 0;
	std::uint64_t written_bytes = 0, array_end = 0;
	std::uint64_t array_begin = 0;
	uam::chat_pages::Builder index_builder(prepared->primary_path, metadata.id);
	const uam::io::AtomicTempProducer writer = [&](const std::function<bool(std::string_view)>& sink)
	{
		const std::function<bool(std::string_view)> append = [&](std::string_view bytes)
		{
			if (!sink(bytes)) return false;
			written_bytes += bytes.size();
			return true;
		};
		if (!append("{\"messages\":[")) return false;
		array_begin = written_bytes - 1;
		const std::function<bool(const Message&)> emit = [&](const Message& message)
		{
			if (!AttachmentsPersistable(message.attachments, metadata.id)) return false;
			if (message_count > 0 && !append(",")) return false;
			// A complete message is the serialization unit; aggregate transcript size is unrestricted.
			const JsonValue value = MessageToJson(message);
			const std::string serialized = SerializeJson(value);
			const uam::chat_pages::Span span{written_bytes, serialized.size()};
			if (!append(serialized) || !index_builder.Add(span, JsonToMessage(value))) return false;
			++message_count;
			return true;
		};
		const std::string approved_id = metadata.id;
		if (!produce(emit, metadata) || metadata.id != approved_id || !metadata.messages.empty()) return false;
		ApplyChatTimestampFallbacks(metadata, false);
		ChatSession empty = metadata;
		empty.messages_loaded = true;
		base = PrepareChatSave(data_root, empty, prepared->expected_storage_revision);
		if (!base) return false;
		std::string fields;
		if (!uam::io::TryReadTextFile(base->primary_temp, fields, 4 * 1024 * 1024)) return false;
		if (metadata.imported_read_only)
		{
			// Passive base preparation includes an empty indexed messages array.
			// Metadata appended to the streamed array must not declare it again.
			auto document = ParseJson(fields);
			const auto archive = PassiveArchiveFromText(fields, metadata.id);
			if (!document || document->type != JsonValue::Type::Object || !archive) return false;
			document->object_value.erase(std::string(kChatMessagesField));
			fields = SerializeArchiveDocument(*document, archive, 4 * 1024 * 1024);
		}
		array_end = written_bytes;
		const std::size_t opening = fields.find('{');
		if (opening == std::string::npos || !append("],")) return false;
		return append(std::string_view(fields).substr(opening + 1));
	};
	uam::io::AtomicWriteResult write_result;
	if (!uam::io::WriteAndSyncAtomicTemp(prepared->primary_temp, writer, {}, write_result) || !base) return nullptr;
	std::error_code size_error;
	const std::uintmax_t primary_size = fs::file_size(prepared->primary_temp, size_error);
	std::string summary_text;
	if (size_error || !uam::io::TryReadTextFile(base->summary_temp, summary_text, 4 * 1024 * 1024)) return nullptr;
	std::optional<JsonValue> summary = ParseJson(summary_text);
	if (!summary || summary->type != JsonValue::Type::Object) return nullptr;
	const auto summary_archive = metadata.imported_read_only ? PassiveArchiveFromText(summary_text, metadata.id) : nullptr;
	if (metadata.imported_read_only && !summary_archive) return nullptr;
	metadata.messages_loaded = false;
	metadata.persisted_message_count = message_count;
	metadata.persisted_messages_digest = SummaryDigest(metadata, message_count);
	uam::json::SetNumber(*summary, kChatPersistedMessageCountField, static_cast<double>(message_count));
	uam::json::SetString(*summary, kChatPersistedMessagesDigestField, metadata.persisted_messages_digest);
	uam::json::SetNumber(*summary, kChatSummarySourceSizeField, static_cast<double>(primary_size));
	prepared->summary_path = base->summary_path;
	prepared->summary_temp = uam::io::MakeTempWritePath(prepared->summary_path);
	prepared->preserve_backup = base->preserve_backup;
	prepared->chat_id = base->chat_id;
	prepared->temporary_parent_chat_id = base->temporary_parent_chat_id;
	prepared->temporary_storage_owner_token = base->temporary_storage_owner_token;
	prepared->queue_path = base->queue_path;
	prepared->expected_queue_revision = base->expected_queue_revision;
	prepared->queue_storage_revision = base->queue_storage_revision;
	prepared->published_queue_full_revision = base->published_queue_full_revision;
	if (!uam::io::WriteAndSyncAtomicTemp(prepared->summary_temp, SerializeArchiveDocument(*summary, summary_archive, 4 * 1024 * 1024), {}, write_result)) return nullptr;
	const std::optional<std::string> revision = StorageRevision(prepared->primary_temp);
	if (!revision) return nullptr;
	prepared->primary_revision = *revision;
	prepared->index_path = uam::chat_pages::IndexPath(prepared->primary_path);
	prepared->index_temp = uam::io::MakeTempWritePath(prepared->index_path);
	if (!index_builder.Finish(prepared->index_temp, *revision, primary_size, array_begin, array_end)) return nullptr;
	if (ChatStorageRevision(data_root, metadata.id) != prepared->expected_storage_revision) return nullptr;
	return prepared;
}
catch (const std::exception& error)
{
	uam::diagnostics::Write(std::string("Imported chat staging rejected: ") + error.what());
	return nullptr;
}

bool ChatRepository::PublishPreparedChatSave(const std::shared_ptr<PreparedChatSave>& prepared, bool sync_directory, ChatSession* committed_chat, const std::function<bool()>& still_owned, ChatPublicationState* primary_publication_state, const std::function<bool(uam::io::AtomicWriteStage)>& primary_stage_hook_for_tests)
{
	if (primary_publication_state) *primary_publication_state = ChatPublicationState::Rejected;
	if (!prepared) return false;
	if (!PreparedChatAcknowledgementMatches(*prepared, committed_chat)) return false;
	std::lock_guard<std::mutex> lock(ChatSaveMutex());
	if (still_owned && !still_owned()) return false;
	if (!PreparedChatStorageMatches(*prepared)) return false;
	if (prepared->binding_expected_primary &&
	    !MatchesSavedChatPrimaryWitness(prepared->binding_data_root, prepared->chat_id, *prepared->binding_expected_primary)) return false;
	if (prepared->primary_temp.empty())
		return uam::io::CommitSyncedAtomicTemp(prepared->summary_path, prepared->summary_temp, false, {}, sync_directory).success;
	if (!CanonicalBirthAllowsStage(prepared->primary_path, prepared->primary_temp)) return false;
	if (primary_publication_state) *primary_publication_state = ChatPublicationState::Uncertain;
	const uam::io::AtomicWriteResult primary = uam::io::CommitSyncedAtomicTemp(prepared->primary_path, prepared->primary_temp, prepared->preserve_backup, primary_stage_hook_for_tests, sync_directory);
	if (primary_publication_state && primary.primary_committed) *primary_publication_state = ChatPublicationState::Committed;
	if (!primary.success) return false;
	if (!prepared->index_temp.empty()) (void)uam::io::CommitSyncedAtomicTemp(prepared->index_path, prepared->index_temp, false, {}, sync_directory);
	(void)uam::io::CommitSyncedAtomicTemp(prepared->summary_path, prepared->summary_temp, false, {}, sync_directory);
	if (committed_chat != nullptr) committed_chat->acp_queue_full_save_revision = prepared->published_queue_full_revision;
	if (prepared->binding_expected_primary)
	{
		// Capture the actual canonical object BEFORE its physical SHA check; a
		// retained open handle prevents file-ID reuse before callback/consumption.
		prepared->binding_physical_primary = uam::platform::PublishedFileChangeWitness::Capture(prepared->primary_path);
		prepared->binding_primary_verified = prepared->binding_physical_primary && prepared->binding_emitted_primary &&
		    prepared->binding_physical_primary->MatchesContent(prepared->binding_emitted_primary->byte_count,
		        prepared->binding_emitted_primary->sha256) &&
		    ChatStorageRevision(prepared->binding_data_root, prepared->chat_id) ==
		        std::optional<std::string>(prepared->binding_emitted_primary->revision);
		if (!prepared->binding_primary_verified) return false;
	}
	AcknowledgePreparedHistory(*prepared, committed_chat);
	return true;
}

std::string ChatRepository::ChatMetadataFingerprint(const fs::path& data_root, const ChatSession& chat)
{
	std::string fingerprint;
	(void)SaveChatImpl(data_root, chat, false, false, nullptr, &fingerprint);
	return fingerprint;
}

bool ChatRepository::ApplyNativeChildDisplayLineage(const ChatSession& source, ChatSession& child, std::string_view native_child_id)
{
	// This is a display receipt of a validated open, not evidence that a native
	// session can be resumed or controlled. Titles and tool arguments are not IDs.
	const std::string native_id = uam::strings::Trim(native_child_id);
	const std::string provider = uam::provider_ids::NormalizeCliProviderAliasOrSelf(source.provider_id);
	if (source.imported_read_only || !source.messages_loaded || source.id.empty() || child.id.empty() || source.id == child.id ||
	    uam::strings::IsBlank(source.created_at) || uam::strings::IsBlank(source.native_session_id) || native_id.empty() ||
	    provider != uam::provider_ids::NormalizeCliProviderAliasOrSelf(child.provider_id)) return false;
	if (uam::strings::NonEmptyOrFallback(uam::strings::Trim(source.execution_host_id), "local") !=
	    uam::strings::NonEmptyOrFallback(uam::strings::Trim(child.execution_host_id), "local")) return false;
	const std::string workspace = uam::chat_identity::NativeWorkspaceForHistoryImport(source);
	if (workspace.empty() || workspace != uam::chat_identity::NativeWorkspaceForHistoryImport(child)) return false;

	const bool codex = provider == uam::provider_ids::kCodexCli;
	const bool opencode = provider == uam::provider_ids::kOpenCodeCli;
	const auto reference = uam::native_agents::ParseHistoryId(native_id);
	if (reference)
	{
		if (reference->provider_id != provider || reference->parent_session_id != source.native_session_id ||
		    reference->execution_host_id != uam::strings::NonEmptyOrFallback(source.execution_host_id, "local") ||
		    !child.imported_read_only || !child.native_session_id.empty() || child.id != native_id ||
		    child.parent_chat_id != reference->parent_session_id) return false;
	}
	else if (provider == uam::provider_ids::kAntigravityCli)
	{
		const std::string parent_native_id = uam::strings::Trim(source.native_session_id);
		if (child.imported_read_only || !uam::paths::IsControllerLocalWorkspace(source) || !uam::paths::IsControllerLocalWorkspace(child) ||
		    !uam::uuid::IsCanonicalUuid(parent_native_id) || !uam::uuid::IsCanonicalUuid(native_id) ||
		    native_id == parent_native_id || native_id != child.native_session_id) return false;
	}
	else if (codex)
	{
		const std::string key = uam::codex::ThreadIdKey(native_id);
		if (child.imported_read_only || key.empty() || key == uam::codex::ThreadIdKey(source.native_session_id) ||
		    key != uam::codex::ThreadIdKey(child.native_session_id)) return false;
	}
	else if (opencode)
	{
		if (child.imported_read_only || native_id == uam::strings::Trim(source.native_session_id) || native_id != child.native_session_id) return false;
	}
	else return false;

	const auto matches_native_id = [&](std::string_view id)
	{
		return codex ? uam::codex::ThreadIdKey(id) == uam::codex::ThreadIdKey(native_id) : id == native_id;
	};
	bool evidence = false;
	for (const Message& message : source.messages)
	{
		if (message.role != MessageRole::Assistant) continue;
		for (const ToolCall& tool : message.tool_calls)
		{
			if (!tool.is_sub_agent) continue;
			// Codex receiver vectors are normalized by its adapter. A rejected or
			// empty authoritative list must never fall back to a legacy scalar ID.
			if (tool.sub_agent_children_authoritative)
			{
				if (std::ranges::any_of(tool.sub_agent_children, [&](const SubAgentChild& candidate) { return matches_native_id(candidate.id); })) evidence = true;
			}
			else if (!codex && matches_native_id(tool.sub_agent_id)) evidence = true;
		}
	}
	if (!evidence) return false;
	// Never silently transfer a child to another UAM parent or to a recreated
	// parent with a reused ID. Existing receipts survive ordinary reopen/refresh.
	if ((!child.subagent_parent_chat_id.empty() || !child.subagent_parent_created_at.empty()) &&
	    (child.subagent_parent_chat_id != source.id || child.subagent_parent_created_at != source.created_at)) return false;
	child.subagent_parent_chat_id = source.id;
	child.subagent_parent_created_at = source.created_at;
	return true;
}

bool ChatRepository::SaveChat(const std::filesystem::path& data_root, const ChatSession& chat, bool skip_unchanged)
{
	if (chat.imported_read_only)
	{
		const auto revision = ChatStorageRevision(data_root, chat.id);
		if (!revision) return false;
		std::shared_ptr<const uam::PassiveQueuedPromptArchive> authoritative;
		if (!chat.passive_queued_prompt_archive && *revision != "missing")
		{
			authoritative = ReadExplicitActiveConversionArchive(AppPaths::UamChatFilePath(data_root, chat.id), chat.id, *revision,
			    chat.acp_queued_prompts.empty());
			if (!authoritative && !chat.acp_queued_prompts.empty()) return false;
		}
		// A validated active primary admits conversion even when its queue is
		// empty. Unknown passive metadata still uses the ordinary indexed guard.
		const bool explicit_conversion = static_cast<bool>(authoritative);
		const auto candidate = DetachedPassiveSaveCandidate(chat, std::move(authoritative));
		if (!candidate) return false;
		// Passive saves stage a current page index with the primary. Only an
		// admitted active transition may enable the bounded legacy fallback here.
		auto prepared = std::make_shared<PreparedChatSave>();
		prepared->passive_archive_worker_migration = explicit_conversion;
		prepared->expected_storage_revision = *revision;
		if (!SaveChatImpl(data_root, *candidate, false, false, prepared.get())) return false;
		if (!PublishPreparedChatSave(prepared)) return false;
		chat.acp_queue_full_save_revision = prepared->published_queue_full_revision;
		return true;
	}
	if (!chat.messages_loaded && chat.persisted_message_count > 0)
	{
		const std::shared_ptr<PreparedChatSave> prepared = PrepareChatSave(data_root, chat);
		if (!PublishPreparedChatSave(prepared)) return false;
		chat.acp_queue_full_save_revision = prepared->published_queue_full_revision;
		return true;
	}
	return SaveChatImpl(data_root, chat, false, skip_unchanged);
}

bool ChatRepository::SaveLoadedChatWithPrimaryWitness(const fs::path& data_root, const ChatSession& chat, SavedChatPrimaryWitness& witness)
{
	witness = {};
	if (!chat.messages_loaded || chat.imported_read_only) return false;
	return SaveChatImpl(data_root, chat, false, false, nullptr, nullptr, nullptr, nullptr, &witness);
}

bool ChatRepository::MatchesSavedChatPrimaryWitness(const fs::path& data_root, std::string_view chat_id, const SavedChatPrimaryWitness& witness)
{
	if (!uam::chat_ids::IsSafeStorageChatId(chat_id) || witness.revision.empty() || witness.revision == "missing" ||
	    witness.byte_count == 0 || witness.byte_count > kMaxChatJsonBytes || witness.sha256.size() != 64) return false;
	const fs::path path = AppPaths::UamChatFilePath(data_root, chat_id);
	if (StorageRevision(path) != std::optional<std::string>(witness.revision)) return false;
	const auto physical = uam::platform::PublishedFileChangeWitness::Capture(path);
	return physical && physical->MatchesContent(witness.byte_count, witness.sha256) &&
	    StorageRevision(path) == std::optional<std::string>(witness.revision);
}

std::shared_ptr<PreparedChatSave> ChatRepository::PrepareLoadedChatBindingSave(const fs::path& data_root, const ChatSession& chat, const SavedChatPrimaryWitness& expected)
{
	if (!chat.messages_loaded || chat.messages.empty() || chat.imported_read_only ||
	    !MatchesSavedChatPrimaryWitness(data_root, chat.id, expected)) return {};
	auto prepared = std::make_shared<PreparedChatSave>();
	prepared->expected_storage_revision = expected.revision;
	prepared->binding_expected_primary = expected;
	prepared->binding_data_root = data_root;
	SavedChatPrimaryWitness emitted;
	if (!SaveChatImpl(data_root, chat, false, false, prepared.get(), nullptr, nullptr, nullptr, &emitted) ||
	    !MatchesSavedChatPrimaryWitness(data_root, chat.id, expected)) return {};
	prepared->binding_emitted_primary = std::move(emitted);
	return prepared;
}

bool ChatRepository::BindingPublicationStillCurrent(const std::shared_ptr<PreparedChatSave>& prepared)
{
	return prepared && prepared->binding_primary_verified && prepared->binding_physical_primary &&
	    prepared->binding_physical_primary->Current();
}

std::string ChatRepository::QueueStorageOwner(const ChatSession& chat)
{
	return QueueOwner(chat);
}

std::string ChatRepository::QueueAdmissionFingerprint(const ChatSession& chat)
{
	return QueueOwner(chat) + "\n" + QueuePayload(chat);
}

bool ChatRepository::SaveQueuedPromptOutbox(const fs::path& data_root, ChatSession& chat, bool* full_save_required, const std::function<bool()>& still_owned)
try
{
	if (full_save_required != nullptr) *full_save_required = false;
	if (!uam::chat_ids::IsSafeStorageChatId(chat.id) || chat.imported_read_only || chat.acp_queue_storage_error ||
	    chat.managed_followup_input_unavailable ||
	    !ChatAttachmentsPersistable(chat)) return false;
	std::unique_lock<std::mutex> lock(ChatSaveMutex());
	if (still_owned && !still_owned()) return false;
	if (!chat.temporary_storage_owner_token.empty() &&
		!PrimaryTemporaryOwnerMatches(AppPaths::UamChatFilePath(data_root, chat.id), chat.temporary_parent_chat_id, chat.temporary_storage_owner_token)) return false;
	const fs::path queue_path = QueueJournalPath(data_root, chat.id);
	const std::optional<std::string> current = QueueJournalRevision(queue_path);
	if (!current || (*current != "missing" && *current != chat.acp_queue_storage_revision)) return false;
	const std::optional<std::string> primary_revision = ChatStorageRevision(data_root, chat.id);
	if (!primary_revision || (*primary_revision == "missing" && !chat.acp_queue_storage_revision.empty())) return false;
	const fs::path primary = AppPaths::UamChatFilePath(data_root, chat.id);
	const fs::path summary_path = AppPaths::UamChatSummaryFilePath(data_root, chat.id);
	std::string text;
	const bool summary_current = SummaryCacheIsCurrent(primary, summary_path) &&
	    uam::io::TryReadTextFile(summary_path, text, 4 * 1024 * 1024);
	const std::optional<JsonValue> summary = summary_current ? ParseJson(text) : std::nullopt;
	if (!summary || summary->type != JsonValue::Type::Object || QueueOwner(*summary) != QueueOwner(chat) ||
	    NonNegativeUintmaxFieldOrZero(summary->Find(kChatSummarySourceSizeField)) != fs::file_size(primary))
	{
		// A new chat or changed native identity needs the ordinary full durability barrier first.
		if (full_save_required != nullptr) *full_save_required = true;
		return false;
	}
	if (chat.acp_queue_full_save_revision != JsonStringOrEmpty(summary->Find(kQueueFullRevisionField))) return false;
	if (chat.memory_last_processed_message_count > 0 && static_cast<std::size_t>(chat.memory_last_processed_message_count) >
	    NonNegativeUintmaxFieldOrZero(summary->Find(kChatPersistedMessageCountField)))
	{
		if (full_save_required != nullptr) *full_save_required = true;
		return false;
	}
	JsonValue record = uam::json::Object();
	const std::string revision = uam::io::MakeTempWritePath(fs::path("queue")).filename().string();
	uam::json::SetNumber(record, "version", 1);
	uam::json::SetString(record, "revision", revision);
	uam::json::SetString(record, "base_revision", *primary_revision);
	uam::json::SetString(record, "base_queue_revision", JsonStringOrEmpty(summary->Find(kQueueRevisionField)));
	uam::json::SetString(record, "owner", QueueOwner(*summary));
	JsonValue prompts = uam::json::Array();
	std::size_t bytes = 0;
	std::unordered_set<std::string> ids;
	if (chat.acp_dispatched_queued_prompt_count > chat.acp_queued_prompts.size()) return false;
	for (const uam::AcpQueuedUserPromptState& prompt : chat.acp_queued_prompts)
	{
		if (prompt.id.empty() || (prompt.id.size() > 128 && prompt.id.starts_with("legacy-queue-")))
		{
			// Legacy queues retain the full save path until their IDs are normalized during loading.
			if (full_save_required != nullptr) *full_save_required = true;
			return false;
		}
		if (prompts.array_value.size() >= kMaxPersistedAcpQueuedPrompts || !ids.insert(prompt.id).second) return false;
		JsonValue value = AcpQueuedPromptToJson(prompt);
		const std::optional<uam::AcpQueuedUserPromptState> validated = AcpQueuedPromptFromJson(value, bytes);
		if (!validated || SerializeJson(AcpQueuedPromptToJson(*validated)) != SerializeJson(value)) return false;
		uam::json::PushValue(prompts, std::move(value));
	}
	uam::json::SetValue(record, kChatAcpQueuedPromptsField, std::move(prompts));
	uam::json::SetBool(record, kChatAcpQueueHeldField, chat.acp_queue_held);
	uam::json::SetNumber(record, kChatAcpDispatchedQueuedPromptCountField, static_cast<double>(chat.acp_dispatched_queued_prompt_count));
	if (chat.memory_last_processed_message_count < 0 || chat.memory_last_processed_at.size() > 128) return false;
	uam::json::SetNumber(record, kChatMemoryLastProcessedMessageCountField, chat.memory_last_processed_message_count);
	uam::json::SetString(record, kChatMemoryLastProcessedAtField, chat.memory_last_processed_at);
	const std::string encoded = SerializeJson(record, kMaxQueueJournalBytes);
	if (!uam::paths::CreateDirectoriesNoThrow(queue_path.parent_path()) ||
	    uam::paths::IsLinkOrReparsePointNoThrow(queue_path.parent_path()) ||
	    !uam::io::WriteTextFile(queue_path, encoded)) return false;
	chat.acp_queue_storage_revision = revision;
	return true;
}
catch (const std::length_error&)
{
	// Oversized captured agent metadata retains the existing full-save limit.
	if (full_save_required != nullptr) *full_save_required = true;
	return false;
}
catch (const std::exception& error)
{
	uam::diagnostics::Write(std::string("Queue save rejected: ") + error.what());
	return false;
}

std::shared_ptr<PreparedChatSave> ChatRepository::PrepareLastOpenedAt(const fs::path& data_root, const ChatSession& metadata)
try
{
	if (!metadata.temporary_storage_owner_token.empty() &&
		!PrimaryTemporaryOwnerMatches(AppPaths::UamChatFilePath(data_root, metadata.id), metadata.temporary_parent_chat_id, metadata.temporary_storage_owner_token)) return nullptr;
	const std::optional<std::string> revision = ChatStorageRevision(data_root, metadata.id);
	if (!revision) return nullptr;
	const fs::path primary = AppPaths::UamChatFilePath(data_root, metadata.id);
	const fs::path summary_path = AppPaths::UamChatSummaryFilePath(data_root, metadata.id);
	const std::optional<std::string> summary_revision = StorageRevision(summary_path);
	if (!summary_revision) return nullptr;
	std::error_code error;
	const std::uintmax_t source_size = fs::file_size(primary, error);
	std::optional<JsonValue> summary;
	std::string summary_text;
	if (!error && SummaryCacheIsCurrent(primary, summary_path) &&
	    uam::io::TryReadTextFile(summary_path, summary_text, kMaxPassiveMetadataReadBytes)) summary = ParseJson(summary_text);
	if (!summary || summary->type != JsonValue::Type::Object || JsonStringOrEmpty(summary->Find(kChatIdField)) != metadata.id ||
	    NonNegativeUintmaxFieldOrZero(summary->Find(kChatSummarySourceSizeField)) != source_size)
	{
		std::optional<ChatSession> persisted = LoadLocalChat(data_root, metadata.id, true, nullptr, false);
		if (!persisted) return nullptr;
		persisted->last_opened_at = metadata.last_opened_at;
		persisted->interaction_at = metadata.interaction_at;
		persisted->attention_revision = metadata.attention_revision;
		persisted->settled_at = metadata.settled_at;
		persisted->snoozed_until_ms = metadata.snoozed_until_ms;
		return PrepareChatSave(data_root, *persisted, *revision);
	}
	const bool passive_summary = JsonBoolOrDefault(summary->Find(kChatImportedReadOnlyField), false);
	const auto summary_archive = passive_summary ? PassiveArchiveFromText(summary_text, metadata.id) : nullptr;
	if (passive_summary && !summary_archive) return nullptr;
	uam::json::SetString(*summary, kChatLastOpenedAtField, metadata.last_opened_at);
	uam::json::SetString(*summary, "interaction_at", metadata.interaction_at);
	uam::json::SetString(*summary, "attention_revision", metadata.attention_revision);
	uam::json::SetString(*summary, "settled_at", metadata.settled_at);
	uam::json::SetNumber(*summary, "snoozed_until_ms", static_cast<double>(metadata.snoozed_until_ms));
	std::shared_ptr<PreparedChatSave> prepared = std::make_shared<PreparedChatSave>();
	prepared->primary_path = primary;
	prepared->temporary_parent_chat_id = metadata.temporary_parent_chat_id;
	prepared->temporary_storage_owner_token = metadata.temporary_storage_owner_token;
	prepared->summary_path = summary_path;
	prepared->summary_temp = uam::io::MakeTempWritePath(summary_path);
	prepared->expected_storage_revision = *revision;
	prepared->primary_revision = *revision;
	prepared->expected_summary_revision = *summary_revision;
	uam::io::AtomicWriteResult result;
	if (!uam::io::WriteAndSyncAtomicTemp(prepared->summary_temp, SerializeArchiveDocument(*summary, summary_archive), {}, result) ||
	    ChatStorageRevision(data_root, metadata.id) != revision) return nullptr;
	return prepared;
}
catch (const std::length_error&)
{
	return nullptr;
}

bool ChatRepository::SaveLastOpenedAt(const std::filesystem::path& data_root, const ChatSession& chat)
try
{
	if (!chat.temporary_storage_owner_token.empty()) return PublishPreparedChatSave(PrepareLastOpenedAt(data_root, chat));
	if (!uam::chat_ids::IsSafeStorageChatId(chat.id)) return false;
	const fs::path chat_path = AppPaths::UamChatFilePath(data_root, chat.id);
	const fs::path summary_path = AppPaths::UamChatSummaryFilePath(data_root, chat.id);
	std::error_code error;
	const std::uintmax_t source_size = fs::file_size(chat_path, error);
	if (error || !SummaryCacheIsCurrent(chat_path, summary_path)) return SaveChat(data_root, chat);
	std::string summary_text;
	if (!uam::io::TryReadTextFile(summary_path, summary_text, kMaxPassiveMetadataReadBytes)) return SaveChat(data_root, chat);
	auto summary = ParseJson(summary_text);
	if (!summary || summary->type != JsonValue::Type::Object ||
	    JsonStringOrEmpty(summary->Find(kChatIdField)) != chat.id ||
	    NonNegativeUintmaxFieldOrZero(summary->Find(kChatSummarySourceSizeField)) != source_size)
	{
		return SaveChat(data_root, chat);
	}
	const bool passive_summary = JsonBoolOrDefault(summary->Find(kChatImportedReadOnlyField), false);
	const auto summary_archive = passive_summary ? PassiveArchiveFromText(summary_text, chat.id) : nullptr;
	if (passive_summary && !summary_archive) return false;
	uam::json::SetString(*summary, kChatLastOpenedAtField,
	                     uam::strings::NonEmptyOrFallback(chat.last_opened_at, chat.updated_at));
	uam::json::SetString(*summary, "interaction_at", chat.interaction_at);
	uam::json::SetString(*summary, "attention_revision", chat.attention_revision);
	uam::json::SetString(*summary, "settled_at", chat.settled_at);
	uam::json::SetNumber(*summary, "snoozed_until_ms", static_cast<double>(chat.snoozed_until_ms));
	return uam::io::WriteTextFile(summary_path, SerializeArchiveDocument(*summary, summary_archive)) || SaveChat(data_root, chat);
}
catch (const std::length_error&)
{
	return false;
}

bool ChatRepository::SaveChatIfAbsent(const std::filesystem::path& data_root, const ChatSession& chat, std::string* saved_revision)
{
	if (saved_revision != nullptr) saved_revision->clear();
	if (chat.imported_read_only)
	{
		const auto candidate = DetachedPassiveSaveCandidate(chat);
		if (!candidate) return false;
		auto prepared = std::make_shared<PreparedChatSave>();
		prepared->expected_storage_revision = "missing";
		if (!SaveChatImpl(data_root, *candidate, true, false, prepared.get())) return false;
		const auto revision = StorageRevision(prepared->primary_temp);
		if (!revision) return false;
		std::string receipt_content;
		if (saved_revision && !uam::io::TryReadTextFile(prepared->primary_temp, receipt_content, kMaxChatJsonBytes)) return false;
		const fs::path primary = AppPaths::UamChatFilePath(data_root, chat.id);
		const auto absent = [&]()
		{
			return StorageRevision(primary) == "missing" && StorageRevision(uam::io::MakeBackupPath(primary)) == "missing";
		};
		if (!PublishPreparedChatSave(prepared, true, nullptr, absent)) return false;
		chat.acp_queue_full_save_revision = prepared->published_queue_full_revision;
		if (saved_revision) *saved_revision = *revision + "\n" + receipt_content;
		return true;
	}
	return SaveChatImpl(data_root, chat, true, false, nullptr, nullptr, saved_revision);
}

std::optional<std::vector<uam::AcpQueuedUserPromptState>> ChatRepository::ParseArchivalQueuedPrompts(
    std::string_view canonical_json, std::string_view expected_owner)
try
{
	if (canonical_json.size() > kMaxChatJsonBytes || !uam::chat_ids::IsSafeStorageChatId(expected_owner)) return std::nullopt;
	if (!PassiveArchiveFromText(canonical_json, expected_owner)) return std::nullopt;
	const auto root = ParseJson(canonical_json);
	if (!root) return std::nullopt;
	std::vector<uam::AcpQueuedUserPromptState> archive;
	if (!DecodeArchiveDocument(*root, expected_owner, archive)) return std::nullopt;
	return archive;
}
catch (...)
{
	return std::nullopt;
}

std::shared_ptr<const uam::PassiveQueuedPromptArchive> ChatRepository::ParsePassiveQueuedPromptArchive(
    std::string_view canonical_json, std::string_view expected_owner)
try
{
	if (canonical_json.size() > kMaxChatJsonBytes) return {};
	return PassiveArchiveFromText(canonical_json, expected_owner);
}
catch (...)
{
	return {};
}

bool ChatRepository::MatchesTemporaryChatSaveReceipt(const fs::path& data_root, std::string_view chat_id, std::string_view saved_revision)
{
	if (!uam::chat_ids::IsSafeStorageChatId(chat_id)) return false;
	const std::size_t boundary = saved_revision.find('\n');
	if (boundary == std::string_view::npos || boundary == 0) return false;
	const std::string_view revision = saved_revision.substr(0, boundary);
	const std::string_view expected_content = saved_revision.substr(boundary + 1);
	if (revision == "missing" || expected_content.empty()) return false;
	const fs::path primary = AppPaths::UamChatFilePath(data_root, chat_id);
	std::string actual_content;
	return StorageRevision(primary) == revision &&
		uam::io::TryReadTextFile(primary, actual_content, expected_content.size()) && actual_content == expected_content &&
		StorageRevision(primary) == revision;
}

namespace
{
	const char* RemovalOutcome(bool removed, const std::error_code& error)
	{
		return error ? "error" : (removed ? "removed" : "absent");
	}

	ChatStorageDeleteResult DeleteChatStorageFilesLocked(const fs::path& data_root, std::string_view chat_id, std::string_view reason)
	{
		ChatStorageDeleteResult result;
		if (!uam::chat_ids::IsSafeStorageChatId(chat_id))
		{
			result.unsafe_chat_id = true;
			uam::diagnostics::Write("Chat storage delete refused: reason=" + std::string(reason) + ", unsafe chat id");
			return result;
		}

		if (uam::paths::IsLinkOrReparsePointNoThrow(QueueJournalPath(data_root, chat_id).parent_path()))
		{
			result.queue_file_error = std::make_error_code(std::errc::operation_not_permitted);
			uam::diagnostics::Write("Chat storage delete refused: reason=" + std::string(reason) + ", chat_id=" + std::string(chat_id) + ", queue directory is a link");
			return result;
		}
		uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(AppPaths::ChatPath(data_root, chat_id), &result.legacy_directory_error);
		uam::paths::RemoveFileNoThrow(QueueJournalPath(data_root, chat_id), &result.queue_file_error);
		const fs::path metadata_path = AppPaths::UamChatFilePath(data_root, chat_id);
		const bool primary_removed = uam::paths::RemoveFileNoThrow(metadata_path, &result.metadata_file_error);
		uam::paths::RemoveFileNoThrow(uam::chat_pages::IndexPath(metadata_path), &result.message_index_file_error);
		uam::paths::RemoveFileNoThrow(uam::chat_pages::IndexPath(uam::io::MakeBackupPath(metadata_path)), &result.message_index_backup_file_error);
		const bool backup_removed = uam::paths::RemoveFileNoThrow(uam::io::MakeBackupPath(metadata_path), &result.metadata_backup_file_error);
		const fs::path summary_path = AppPaths::UamChatSummaryFilePath(data_root, chat_id);
		uam::paths::RemoveFileNoThrow(summary_path, &result.summary_file_error);
		uam::paths::RemoveFileNoThrow(uam::io::MakeBackupPath(summary_path), &result.summary_backup_file_error);
		uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(
		    data_root / "computer-use" / std::string(chat_id), &result.computer_use_directory_error);
		// Ids and outcomes only: the sole UAM path that removes a chat primary must leave an attributable record.
		uam::diagnostics::Write("Chat storage delete: reason=" + std::string(reason) + ", chat_id=" + std::string(chat_id) +
		    ", primary=" + RemovalOutcome(primary_removed, result.metadata_file_error) +
		    ", backup=" + RemovalOutcome(backup_removed, result.metadata_backup_file_error) +
		    ", result=" + (result.Failed() ? "failed" : "ok"));
		return result;
	}

}

bool ChatRepository::DeleteTemporaryChatIfRevision(const fs::path& data_root, std::string_view chat_id, std::string_view parent_id, std::string_view expected_revision)
{
	std::lock_guard<std::mutex> lock(ChatSaveMutex());
	if (!uam::chat_ids::IsSafeStorageChatId(chat_id) || parent_id.empty() || expected_revision.empty() || expected_revision == "missing" ||
		!MatchesTemporaryChatSaveReceipt(data_root, chat_id, expected_revision)) return false;
	const std::optional<ChatSession> child = LoadLocalChat(data_root, std::string(chat_id), false);
	if (!child || child->temporary_parent_chat_id != parent_id) return false;
	return !DeleteChatStorageFilesLocked(data_root, chat_id, "temporary_side_chat").Failed();
}

ChatStorageDeleteResult ChatRepository::DeleteChatStorageFiles(const fs::path& data_root, std::string_view chat_id, std::string_view reason)
{
	std::lock_guard<std::mutex> lock(ChatSaveMutex());
	return DeleteChatStorageFilesLocked(data_root, chat_id, reason);
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
	uam::provider_storage::NormalizeLoadedNativeSessionId(chat);

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
		// The primary's immutable archive stays authoritative; summary overlays do
		// not replace it with missing or stale archive state.
		if (!summary.managed_parent_input_receipts.empty() &&
		    summary.managed_parent_input_receipts != hydrated.managed_parent_input_receipts)
		{
			// Do not choose a newer-looking summary to replace primary-owned immutable input provenance.
			hydrated.managed_followup_input_unavailable = true;
		}
		hydrated.interaction_at = summary.interaction_at;
		if (summary.last_opened_at > hydrated.last_opened_at)
			hydrated.last_opened_at = summary.last_opened_at;
		hydrated.attention_revision = summary.attention_revision;
		hydrated.settled_at = summary.settled_at;
		hydrated.snoozed_until_ms = summary.snoozed_until_ms;
		hydrated.last_stop_reason = summary.last_stop_reason;
		hydrated.execution_host_id = summary.execution_host_id;
		hydrated.folder_id = summary.folder_id;
		hydrated.title = summary.title;
		hydrated.custom_icon = summary.custom_icon;
		hydrated.metadata_incarnation = summary.metadata_incarnation;
		hydrated.managed_subagent_revision = summary.managed_subagent_revision;
		hydrated.subagent_parent_chat_id = summary.subagent_parent_chat_id;
		hydrated.subagent_parent_created_at = summary.subagent_parent_created_at;
		hydrated.pinned = summary.pinned;
		hydrated.workspace_directory = summary.workspace_directory;
		hydrated.workspace_isolation_kind = summary.workspace_isolation_kind;
		hydrated.workspace_source_directory = summary.workspace_source_directory;
		hydrated.workspace_base_ref = summary.workspace_base_ref;
		hydrated.git_base_branch_override = summary.git_base_branch_override;
		hydrated.workspace_branch_name = summary.workspace_branch_name;
		hydrated.workspace_worktree_directory = summary.workspace_worktree_directory;
		hydrated.svn_update_pending = summary.svn_update_pending;
		hydrated.imported_read_only = summary.imported_read_only;
		hydrated.approval_mode = summary.approval_mode;
		hydrated.uam_agent_id = summary.uam_agent_id;
		hydrated.last_prompt_agent_definition_hash = summary.last_prompt_agent_definition_hash;
		hydrated.provider_handoff_context = summary.provider_handoff_context;
		hydrated.provider_handoff_session_id = summary.provider_handoff_session_id;
		hydrated.provider_handoff_cli_contexts = summary.provider_handoff_cli_contexts;
		hydrated.agent_run_id = summary.agent_run_id;
		hydrated.managed_followup_incarnation = summary.managed_followup_incarnation;
		hydrated.managed_followup_input_unavailable = hydrated.managed_followup_input_unavailable || summary.managed_followup_input_unavailable;
		hydrated.temporary_parent_chat_id = summary.temporary_parent_chat_id;
		hydrated.temporary_storage_owner_token = summary.temporary_storage_owner_token;
		hydrated.side_cleanup_requested = summary.side_cleanup_requested;
		hydrated.side_cleanup_retry_time_s = summary.side_cleanup_retry_time_s;
		hydrated.goal_owner_chat_id = summary.goal_owner_chat_id;
		hydrated.goal_iteration_goal_id = summary.goal_iteration_goal_id;
		hydrated.goal_iteration_turn_kind = summary.goal_iteration_turn_kind;
		hydrated.goal_iteration_repair_attempts = summary.goal_iteration_repair_attempts;
		hydrated.remote_turn_reconnect_pending = summary.remote_turn_reconnect_pending;
		hydrated.remote_process_exists = summary.remote_process_exists;
		hydrated.remote_prompt_image_supported = summary.remote_prompt_image_supported;
		hydrated.remote_structured_protocol = summary.remote_structured_protocol;
		hydrated.remote_opencode_native_port = summary.remote_opencode_native_port;
		hydrated.remote_opencode_native_token = summary.remote_opencode_native_token;
		hydrated.remote_manual_compaction_pending = summary.remote_manual_compaction_pending;
		hydrated.remote_manual_compaction_boundary_json = summary.remote_manual_compaction_boundary_json;
		hydrated.remote_stop_cleanup_pending = summary.remote_stop_cleanup_pending;
		hydrated.remote_restart_pending = summary.remote_restart_pending;
		hydrated.native_session_reset_pending = summary.native_session_reset_pending;
		hydrated.provider_handoff_context = summary.provider_handoff_context;
		hydrated.provider_handoff_session_id = summary.provider_handoff_session_id;
		hydrated.remote_recovery_enabled = summary.remote_recovery_enabled;
		hydrated.remote_recovery_state = summary.remote_recovery_state;
		hydrated.remote_process_control_token = summary.remote_process_control_token;
		hydrated.routing_mode = summary.routing_mode;
		hydrated.routing_inherited_default_fields = summary.routing_inherited_default_fields;
		hydrated.routing_source_folder_id = summary.routing_source_folder_id;
		hydrated.routing_source_directory = summary.routing_source_directory;
		hydrated.routing_assigned_connection_identity = summary.routing_assigned_connection_identity;
		hydrated.routing_assigned_provider_configuration_identity = summary.routing_assigned_provider_configuration_identity;
		hydrated.routing_reservation_pending = summary.routing_reservation_pending;
		hydrated.remote_delivered_stdout_cursor = summary.remote_delivered_stdout_cursor;
		hydrated.remote_delivered_stderr_cursor = summary.remote_delivered_stderr_cursor;
		hydrated.remote_source_exit_pending = summary.remote_source_exit_pending;
		hydrated.remote_source_exit_code = summary.remote_source_exit_code;
		hydrated.remote_uam_control_channel_id = summary.remote_uam_control_channel_id;
		hydrated.remote_interaction_responses = summary.remote_interaction_responses;
		hydrated.remote_prompt_delivery_session_id = summary.remote_prompt_delivery_session_id;
		hydrated.remote_prompt_delivery_id = summary.remote_prompt_delivery_id;
		hydrated.remote_prompt_delivery_payload = summary.remote_prompt_delivery_payload;
		hydrated.remote_pending_requests = summary.remote_pending_requests;
		hydrated.remote_next_request_id = summary.remote_next_request_id;
		hydrated.remote_active_turn_id = summary.remote_active_turn_id;
		hydrated.remote_turn_serial = summary.remote_turn_serial;
		hydrated.remote_turn_user_message_index = summary.remote_turn_user_message_index;
		hydrated.acp_queue_full_save_revision = summary.acp_queue_full_save_revision;
		hydrated.acp_queue_storage_revision = summary.acp_queue_storage_revision;
		hydrated.acp_queue_storage_error = summary.acp_queue_storage_error;
		hydrated.acp_queued_prompts = summary.acp_queued_prompts;
		hydrated.acp_unsent_prompt_recoveries = summary.acp_unsent_prompt_recoveries;
		hydrated.acp_queue_held = summary.acp_queue_held;
		hydrated.acp_dispatched_queued_prompt_count =
		    summary.acp_dispatched_queued_prompt_count;
		hydrated.active_goal_id = summary.active_goal_id;
		hydrated.goal_command_revision = summary.goal_command_revision;
		hydrated.goal_pending_continuation_id = summary.goal_pending_continuation_id;
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
		HoldManagedQueue(hydrated);
	}

	void ApplySummaryOpenedAtToFullChat(const fs::path& chat_path, ChatSession& chat, std::size_t max_read_bytes = 0)
	{
		const fs::path summary_path = SummaryCachePathForChatFile(chat_path);
		std::error_code error;
		const std::uintmax_t source_size = fs::file_size(chat_path, error);
		if (error || !SummaryCacheIsCurrent(chat_path, summary_path)) return;
		const LoadChatResult summary = ParseLocalChatFile(summary_path, false, source_size, false, max_read_bytes);
		if (summary.chat && summary.chat->id == chat.id)
		{
			if (summary.chat->last_opened_at > chat.last_opened_at) chat.last_opened_at = summary.chat->last_opened_at;
			chat.attention_revision = summary.chat->attention_revision;
			chat.interaction_at = summary.chat->interaction_at;
			chat.settled_at = summary.chat->settled_at;
			chat.snoozed_until_ms = summary.chat->snoozed_until_ms;
			chat.managed_followup_input_unavailable = chat.managed_followup_input_unavailable || summary.chat->managed_followup_input_unavailable;
			HoldManagedQueue(chat);
		}
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
		uam::provider_storage::NormalizeLoadedNativeSessionId(recovered);

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
				if (include_messages && primary_chat.chat)
					ApplySummaryOpenedAtToFullChat(entry.path(), *primary_chat.chat);
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

			if (primary_chat.managed_followup_input_unavailable)
			{
				AppendWarning(warning_out, "Skipped unsupported managed input without backup adoption: " + entry.path().string() + ".");
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

		std::optional<std::unordered_map<std::string, std::string>> codex_names;
		// Older imports used the first raw user event as their title. Read only
		// affected transcripts, and leave explicit names and source files intact.
		std::erase_if(chats, [&](ChatSession& chat)
		{
			if (!uam::provider_ids::IsCliProviderAliasOf(chat.provider_id, uam::provider_ids::kCodexCli) ||
			    !uam::IsInjectedChatTitle(chat.title)) return false;
			LoadChatResult loaded;
			if (!chat.messages_loaded) loaded = ParseLocalChatFile(AppPaths::UamChatFilePath(data_root, chat.id), true);
			const ChatSession* full = chat.messages_loaded ? &chat : (loaded.chat ? &*loaded.chat : nullptr);
			if (full == nullptr) return false;
			const std::vector<Message>::const_iterator first_user = std::ranges::find_if(full->messages, [](const Message& message)
			{
				return message.role == MessageRole::User;
			});
			if (first_user == full->messages.end() ||
			    chat.title != uam::strings::TrimAndElide(first_user->content, 48)) return false;
			const bool helper_only = std::ranges::none_of(full->messages, [](const Message& message)
			{
				return message.role == MessageRole::User && !uam::IsCodexSyntheticUserMessage(message.content);
			});
			if (helper_only) return true;
			chat.title = uam::BuildImportedChatTitle(full->messages, chat.created_at);
			if (uam::paths::IsControllerLocalWorkspace(chat))
			{
				if (!codex_names) codex_names = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCodexCli).ReadNativeSessionNames();
				const std::unordered_map<std::string, std::string>::const_iterator name = codex_names->find(chat.native_session_id);
				if (name != codex_names->end() && !uam::IsInjectedChatTitle(name->second)) chat.title = name->second;
			}
			return false;
		});
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

std::optional<ChatSession> ChatRepository::LoadLocalChat(const std::filesystem::path& data_root, std::string_view chat_id, bool include_messages, std::string* warning_out, bool publish_summary_cache)
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
		loaded = ParseLocalChatFile(chat_path, include_messages, 0, publish_summary_cache);
		if (include_messages && loaded.chat)
			ApplySummaryOpenedAtToFullChat(chat_path, *loaded.chat);
	}
	if (!loaded.chat)
	{
		SetWarning(warning_out, loaded.error);
	}
	return std::move(loaded.chat);
}

std::optional<ChatSession> ChatRepository::LoadLocalChatBounded(const fs::path& data_root,
    std::string_view chat_id, std::size_t max_bytes)
{
	if (!uam::chat_ids::IsSafeStorageChatId(chat_id) || max_bytes == 0) return std::nullopt;
	const fs::path primary = AppPaths::UamChatFilePath(data_root, chat_id);
	LoadChatResult loaded = ParseLocalChatFile(primary, true, 0, false, max_bytes);
	if (loaded.chat) ApplySummaryOpenedAtToFullChat(primary, *loaded.chat, max_bytes);
	return std::move(loaded.chat);
}

bool ChatRepository::HydrateChatMessages(const std::filesystem::path& data_root, ChatSession& chat, std::string* warning_out, std::size_t max_read_bytes)
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

	LoadChatResult loaded = ParseLocalChatFile(AppPaths::UamChatFilePath(data_root, chat.id), true, 0, true, max_read_bytes);
	if (!loaded.chat)
	{
		SetWarning(warning_out, loaded.error);
		return false;
	}

	// Move rather than copy: a second full transcript doubles peak hydration memory.
	ChatSession hydrated = std::move(*loaded.chat);
	if ((chat.imported_read_only || hydrated.imported_read_only) &&
	    (chat.imported_read_only != hydrated.imported_read_only ||
	     (chat.passive_queued_prompt_archive && !SamePassiveArchive(chat.passive_queued_prompt_archive, hydrated.passive_queued_prompt_archive))))
	{
		SetWarning(warning_out, "Passive attachment archive changed while history was loading. Reload before saving.");
		return false;
	}
	if (hydrated.managed_followup_incarnation != chat.managed_followup_incarnation ||
	    hydrated.acp_queue_full_save_revision != chat.acp_queue_full_save_revision ||
	    hydrated.acp_queue_storage_revision != chat.acp_queue_storage_revision ||
	    hydrated.acp_queue_storage_error != chat.acp_queue_storage_error)
	{
		SetWarning(warning_out, "Queued inputs changed while history was loading. Reload the chat before continuing.");
		return false;
	}
	CarrySummaryFieldsIntoHydratedChat(hydrated, chat);
	chat = std::move(hydrated);
	return true;
}

bool ChatRepository::CanAdoptHydratedMessages(const ChatSession& current, const ChatSession& loaded,
                                             std::size_t expected_count, std::string_view expected_digest)
{
	return (!(current.imported_read_only || loaded.imported_read_only) ||
	        (current.imported_read_only == loaded.imported_read_only &&
	         (!current.passive_queued_prompt_archive || SamePassiveArchive(current.passive_queued_prompt_archive, loaded.passive_queued_prompt_archive)))) &&
	    current.id == loaded.id && current.managed_followup_incarnation == loaded.managed_followup_incarnation &&
	    !current.messages_loaded && loaded.messages_loaded &&
	    current.persisted_message_count == expected_count && loaded.persisted_message_count == expected_count &&
	    current.persisted_messages_digest == expected_digest && loaded.persisted_messages_digest == expected_digest;
}

bool ChatRepository::AdoptHydratedMessagesIfUnchanged(ChatSession& current, ChatSession&& loaded,
                                                      std::size_t expected_count,
                                                      std::string_view expected_digest)
{
	if (!CanAdoptHydratedMessages(current, loaded, expected_count, expected_digest)) return false;
	current.messages = std::move(loaded.messages);
	current.managed_followup_input_unavailable = current.managed_followup_input_unavailable || loaded.managed_followup_input_unavailable;
	HoldManagedQueue(current);
	if (current.imported_read_only) current.passive_queued_prompt_archive = std::move(loaded.passive_queued_prompt_archive);
	current.messages_loaded = true;
	return true;
}

std::optional<ChatSession> ChatRepository::LoadLocalChatSummaryReadOnly(const fs::path& data_root, std::string_view chat_id)
{
	if (!uam::chat_ids::IsSafeStorageChatId(chat_id)) return std::nullopt;
	const fs::path primary = AppPaths::UamChatFilePath(data_root, chat_id);
	const fs::path summary = AppPaths::UamChatSummaryFilePath(data_root, chat_id);
	if (uam::paths::IsLinkOrReparsePointNoThrow(primary) || uam::paths::IsLinkOrReparsePointNoThrow(summary)) return std::nullopt;
	std::error_code error;
	const std::uintmax_t size = fs::file_size(primary, error);
	if (error) return std::nullopt;
	const fs::file_time_type modified = fs::last_write_time(primary, error);
	if (error) return std::nullopt;
	LoadChatResult loaded;
	if (SummaryCacheIsCurrent(primary, summary))
		loaded = ParseLocalChatFile(summary, false, size, false, 1024 * 1024);
	if (!loaded.chat || loaded.chat->id != chat_id)
		loaded = ParseLocalChatFile(primary, false, 0, false, 32 * 1024 * 1024);
	if (!loaded.chat || loaded.chat->id != chat_id) return std::nullopt;
	if (fs::file_size(primary, error) != size || error) return std::nullopt;
	if (fs::last_write_time(primary, error) != modified || error) return std::nullopt;
	return std::move(loaded.chat);
}

std::vector<ChatSession> ChatRepository::LoadLocalChatSummariesReadOnly(const fs::path& data_root, std::stop_token stop_token)
{
	std::vector<ChatSession> chats;
	std::error_code error;
	const fs::path root = AppPaths::UamChatsRootPath(data_root);
	for (fs::directory_iterator it(root, error), end; !error && it != end && !stop_token.stop_requested(); it.increment(error))
	{
		if (!uam::paths::IsRegularFileWithExtensionNoThrow(*it, ".json")) continue;
		std::optional<ChatSession> chat = LoadLocalChat(data_root, it->path().stem().string(), false, nullptr, false);
		if (chat) chats.push_back(std::move(*chat));
	}
	return chats;
}


bool StoppedChatCanonicalRead::Current() const
{
	return PublicationDirectory(m_root) && PublicationDirectory(m_primary_path.parent_path()) &&
	    m_primary && m_primary->Current() &&
	    (PublicationAbsent(m_queue_path.parent_path()) || PublicationDirectory(m_queue_path.parent_path())) &&
	    (PublicationAbsent(m_summary_path.parent_path()) || PublicationDirectory(m_summary_path.parent_path())) &&
	    StoppedPublicationAbsent(m_queue_path) &&
	    (m_summary ? m_summary->Current() : StoppedPublicationAbsent(m_summary_path));
}

std::shared_ptr<const StoppedChatCanonicalRead> ChatRepository::ReadStoppedChatCanonical(
    const fs::path& data_root, std::string_view chat_id, std::uint64_t max_bytes,
    std::string* error_out, std::stop_token cancel)
try
{
	if (error_out) error_out->clear();
	const auto fail = [error_out](const char* error) -> std::shared_ptr<const StoppedChatCanonicalRead>
	{
		if (error_out) *error_out = error;
		return {};
	};
	if (cancel.stop_requested() || !data_root.is_absolute() || max_bytes == 0 ||
	    max_bytes > kMaxChatJsonBytes || !uam::chat_ids::IsSafeStorageChatId(chat_id))
		return fail("Stopped canonical observation requires a bounded explicit storage owner.");
	auto read = std::shared_ptr<StoppedChatCanonicalRead>(new StoppedChatCanonicalRead());
	read->m_root = PublicationRootSpelling(data_root);
	read->m_primary_path = AppPaths::UamChatFilePath(read->m_root, chat_id);
	read->m_summary_path = AppPaths::UamChatSummaryFilePath(read->m_root, chat_id);
	read->m_queue_path = QueueJournalPath(read->m_root, chat_id);
	if (!PublicationDirectory(read->m_root) || !PublicationDirectory(read->m_primary_path.parent_path()) ||
	    (!PublicationAbsent(read->m_queue_path.parent_path()) && !PublicationDirectory(read->m_queue_path.parent_path())) ||
	    (!PublicationAbsent(read->m_summary_path.parent_path()) && !PublicationDirectory(read->m_summary_path.parent_path())) ||
	    !StoppedPublicationAbsent(read->m_queue_path))
		return fail("Stopped canonical observation refuses a present or unreadable queue journal.");
	read->m_primary = uam::platform::PublishedFileChangeWitness::Capture(read->m_primary_path);
	const auto primary = read->m_primary ? read->m_primary->ReadBounded(max_bytes, cancel) : std::nullopt;
	if (!primary) return fail("Stopped canonical primary could not be read from its held regular object.");
	read->m_primary_json = *primary;
	// Explicit pathless opt-in: no queue journal read and no summary-cache publication.
	auto decoded = ParseLocalChatDocument(read->m_primary_path, read->m_primary_json, true, 0, false,
	    static_cast<std::size_t>(max_bytes), nullptr, false);
	if (!decoded.chat || decoded.chat->id != chat_id || decoded.chat->imported_read_only ||
	    decoded.chat->messages.empty() || decoded.chat->acp_queue_storage_error || decoded.chat->acp_queue_held ||
	    decoded.chat->acp_dispatched_queued_prompt_count != 0 || !decoded.chat->acp_queued_prompts.empty() ||
	    decoded.chat->remote_turn_reconnect_pending || decoded.chat->remote_process_exists ||
	    decoded.chat->remote_restart_pending || decoded.chat->remote_stop_cleanup_pending ||
	    !decoded.chat->remote_active_turn_id.empty() || !decoded.chat->remote_pending_requests.empty() ||
	    !decoded.chat->remote_prompt_delivery_session_id.empty() || !decoded.chat->remote_prompt_delivery_id.empty() ||
	    !decoded.chat->remote_prompt_delivery_payload.empty() || !decoded.chat->remote_interaction_responses.empty() ||
	    !decoded.chat->agent_run_id.empty() || !decoded.chat->managed_followup_incarnation.empty() ||
	    !decoded.chat->temporary_storage_owner_token.empty())
		return fail("Canonical graph is outside this quiescent read-only observation slice.");
	read->m_chat = std::move(*decoded.chat);
	if (!StoppedPublicationAbsent(read->m_summary_path))
	{
		if (!PublicationDirectory(read->m_summary_path.parent_path()))
			return fail("Canonical summary directory is unavailable.");
		read->m_summary = uam::platform::PublishedFileChangeWitness::Capture(read->m_summary_path);
		const auto summary = read->m_summary ? read->m_summary->ReadBounded(max_bytes, cancel) : std::nullopt;
		if (!summary) return fail("Canonical summary could not be read from its held regular object.");
		read->m_summary_json = *summary;
		auto decoded_summary = ParseLocalChatDocument(read->m_summary_path, *summary, false, primary->size(), false,
		    static_cast<std::size_t>(max_bytes), nullptr, false);
		if (!decoded_summary.chat || decoded_summary.chat->id != chat_id)
			return fail("Canonical summary identity or graph is unsupported.");
		auto projected = read->m_chat;
		CarrySummaryFieldsIntoHydratedChat(projected, *decoded_summary.chat);
		// Compare modeled metadata plus every Message, but do not claim this is raw format preservation.
		// Raw buffers remain retained; full serializer baseline proof is a separate, still-closed gate.
		projected.persisted_message_count = read->m_chat.persisted_message_count;
		projected.persisted_messages_digest = read->m_chat.persisted_messages_digest;
		const auto baseline_metadata = ChatMetadataFingerprint(read->m_root, read->m_chat);
		if (baseline_metadata.empty() || projected.messages != read->m_chat.messages ||
		    ChatMetadataFingerprint(read->m_root, projected) != baseline_metadata)
			return fail("Effective summary overlays require a separate reconciled publication route.");
	}
	if (cancel.stop_requested() || !read->Current())
		return fail("Canonical storage changed while its held bytes were decoded.");
	return read;
}
catch (...)
{
	if (error_out) *error_out = "Stopped canonical observation failed without publishing storage.";
	return {};
}

std::shared_ptr<PreparedChatPublication> ChatRepository::PrepareChatPublication(
    const fs::path& data_root, const ChatSession& candidate, ChatPublicationMode mode,
    std::string_view expected_storage_revision, std::string* error_out)
{
	if (error_out) error_out->clear();
	const auto fail = [&](const char* error) -> std::shared_ptr<PreparedChatPublication>
	{
		if (error_out) *error_out = error;
		return {};
	};
	if (!uam::chat_ids::IsSafeStorageChatId(candidate.id) ||
	    (mode != ChatPublicationMode::CreateAbsent && mode != ChatPublicationMode::UpdateExisting) ||
	    (mode == ChatPublicationMode::CreateAbsent ? !expected_storage_revision.empty() :
	        (expected_storage_revision.empty() || expected_storage_revision == "missing" || expected_storage_revision == "unreadable")))
		return fail("Invalid conditional chat publication mode, identity or revision.");
	try
	{
		const fs::path root = PublicationRootSpelling(data_root);
		auto prepared = std::shared_ptr<PreparedChatPublication>(new PreparedChatPublication());
		prepared->m_root = root;
		prepared->m_mode = mode;
		auto& legacy = prepared->m_legacy;
		legacy.primary_path = AppPaths::UamChatFilePath(root, candidate.id);
		legacy.summary_path = AppPaths::UamChatSummaryFilePath(root, candidate.id);
		legacy.expected_storage_revision = mode == ChatPublicationMode::CreateAbsent ? "missing" : std::string(expected_storage_revision);
		{
			std::lock_guard<std::mutex> lock(ChatSaveMutex());
			if (!PublicationDirectory(root) ||
			    (mode == ChatPublicationMode::CreateAbsent ?
			        !PublicationCreateDirectory(root, legacy.primary_path.parent_path()) :
			        !PublicationDirectory(legacy.primary_path.parent_path())))
				return fail("Conditional chat storage directory is unavailable.");
			if (mode == ChatPublicationMode::CreateAbsent)
			{
				if (!PublicationAbsent(legacy.primary_path) || !PublicationAbsent(uam::io::MakeBackupPath(legacy.primary_path)))
					return fail("Initial chat primary or backup already has storage evidence.");
			}
			else if (StorageRevision(legacy.primary_path) != expected_storage_revision ||
			         !CapturePublicationFile(legacy.primary_path, prepared->m_primary_before) || prepared->m_primary_before.absent)
				return fail("Existing chat primary does not match the explicit revision.");
			if (!PublicationCreateDirectory(root, legacy.summary_path.parent_path()) ||
			    !CapturePublicationFile(legacy.summary_path, prepared->m_summary_before) ||
			    !CapturePublicationFile(uam::chat_pages::IndexPath(legacy.primary_path), prepared->m_index_before))
				return fail("Conditional secondary chat storage is unavailable.");
		}
		const auto& primary_parent = legacy.primary_path.parent_path();
		prepared->m_primary.path = PublicationPrivatePath(primary_parent, "primary");
		prepared->m_summary.path = PublicationPrivatePath(legacy.summary_path.parent_path(), "summary");
		prepared->m_index.path = PublicationPrivatePath(primary_parent, "index");
		prepared->m_source_index.path = PublicationPrivatePath(primary_parent, "source-index");
		prepared->m_records = PublicationPrivatePath(primary_parent, "records");
		prepared->m_source_records = PublicationPrivatePath(primary_parent, "source-records");
		prepared->m_primary_recovery = PublicationPrivatePath(primary_parent, "primary-predecessor");
		prepared->m_summary_recovery = PublicationPrivatePath(legacy.summary_path.parent_path(), "summary-predecessor");
		prepared->m_index_recovery = PublicationPrivatePath(primary_parent, "index-predecessor");
		// No passive-worker migration bit: this path never adopts legacy archive authority.
		if (!SaveChatImpl(root, candidate, false, false, &legacy, nullptr, nullptr, prepared.get()))
			return fail("Conditional chat candidate could not be staged and validated.");
		{
			std::lock_guard<std::mutex> lock(ChatSaveMutex());
			if (!prepared->MatchesBeforePrimary()) return fail("Chat storage changed during conditional preparation.");
		}
		return prepared;
	}
	catch (...)
	{
		return fail("Conditional chat preparation failed; unverifiable stages were preserved.");
	}
}

ChatPublicationResult ChatRepository::PublishPreparedChatPublication(
    const std::shared_ptr<PreparedChatPublication>& prepared,
    const std::function<bool()>& still_owned, ChatSession* committed_chat)
{
	return PublishPreparedChatPublication(prepared, still_owned, committed_chat, {});
}

ChatPublicationResult ChatRepository::PublishPreparedChatPublication(
    const std::shared_ptr<PreparedChatPublication>& prepared,
    const std::function<bool()>& still_owned, ChatSession* committed_chat,
    const std::function<bool(ChatPublicationFile, uam::io::AtomicWriteStage)>& stage_hook_for_tests)
{
	std::lock_guard<std::mutex> lock(ChatSaveMutex());
	if (!prepared || prepared->m_consumed)
		return {ChatPublicationState::Rejected, false, false, "Chat preparation is absent or already consumed.", {}};
	prepared->m_consumed = true;
	ChatPublicationResult result;
	bool primary_allowed = false, primary_confirmed = false;
	const auto add_evidence = [&](const fs::path& path)
	{
		if (!path.empty() && !PublicationAbsent(path) &&
		    std::find(result.evidence_paths.begin(), result.evidence_paths.end(), path) == result.evidence_paths.end())
			result.evidence_paths.push_back(path);
	};
	const auto collect_evidence = [&]
	{
		for (const auto* stage : {&prepared->m_primary, &prepared->m_summary, &prepared->m_index, &prepared->m_source_index})
			if (stage->owned && (stage->retain || !stage->Matches())) add_evidence(stage->path);
		if (prepared->m_primary.retain) add_evidence(prepared->m_primary_recovery);
		if (prepared->m_summary.retain) add_evidence(prepared->m_summary_recovery);
		if (prepared->m_index.retain) add_evidence(prepared->m_index_recovery);
	};
	try
	{
		auto& legacy = prepared->m_legacy;
		if (!still_owned || !PreparedChatAcknowledgementMatches(legacy, committed_chat) || !prepared->MatchesBeforePrimary())
		{
			result.error = "Chat publication ownership, storage or staged candidate changed.";
			collect_evidence();
			return result;
		}
		struct Attempt
		{
			uam::io::AtomicWriteResult written;
			bool allowed = false;
			bool confirmed = false;
		};
		const auto attempt = [&](ChatPublicationFile role, PublicationStage& stage, const fs::path& destination,
		                         const fs::path& recovery, bool preserve_backup, const PublicationExpectedFile* expected)
		{
			Attempt outcome;
			try
			{
				std::string rejection;
				const auto hook = [&](uam::io::AtomicWriteStage point)
				{
					// This checkpoint follows confirmed replacement under the helper's existing platform contract.
					if (point == uam::io::AtomicWriteStage::BeforeBackupCleanup)
					{
						outcome.confirmed = true;
						if (role == ChatPublicationFile::Primary) primary_confirmed = true;
					}
					if (stage_hook_for_tests && !stage_hook_for_tests(role, point)) return false;
					if (point == uam::io::AtomicWriteStage::BeforeReplace)
					{
						const bool matches = role == ChatPublicationFile::Primary ? prepared->MatchesBeforePrimary() :
						    (stage.Matches() && expected && PublicationFileMatches(destination, *expected));
						if (!matches || (role == ChatPublicationFile::Primary &&
						    ((prepared->HasOwnPublicationPredecessor() ? !prepared->m_stopped_birth_verified : !CanonicalBirthAllowsStage(destination, stage.path)) ||
						     !PreparedChatAcknowledgementMatches(legacy, committed_chat) || !still_owned())))
						{
							rejection = "Chat publication was rejected by its final owned storage gate.";
							return false;
						}
						outcome.allowed = true;
						stage.retain = true; // A failure from this point may have exchanged predecessor bytes.
						if (role == ChatPublicationFile::Primary) primary_allowed = true;
					}
					return true;
				};
				outcome.written = uam::io::CommitSyncedAtomicTemp(destination, stage.path, preserve_backup, hook, true, true, recovery);
				if (outcome.written.success && outcome.written.primary_committed)
				{
					outcome.confirmed = true;
					if (role == ChatPublicationFile::Primary) primary_confirmed = true;
				}
				if (!rejection.empty()) outcome.written.error = rejection;
			}
			catch (...)
			{
				outcome.written.error = "Conditional chat file publication threw; possible replacement evidence was retained.";
			}
			return outcome;
		};

		const auto primary = attempt(ChatPublicationFile::Primary, prepared->m_primary, legacy.primary_path,
		    prepared->m_primary_recovery, legacy.preserve_backup, nullptr);
		primary_confirmed = primary.confirmed;
		if (!primary_confirmed)
		{
			result.state = primary.allowed ? ChatPublicationState::Uncertain : ChatPublicationState::Rejected;
			result.error = primary.written.error.empty() ? "Chat primary publication was not confirmed." : std::string(uam::strings::Utf8Prefix(primary.written.error, 1024));
			collect_evidence();
			return result;
		}
		result.state = ChatPublicationState::Committed;
		result.backup_degraded = primary.written.backup_degraded ||
		    (!primary.written.success && prepared->m_mode == ChatPublicationMode::UpdateExisting);
		if (!primary.written.error.empty()) result.error = std::string(uam::strings::Utf8Prefix(primary.written.error, 1024));
		if (committed_chat != nullptr) committed_chat->acp_queue_full_save_revision = legacy.published_queue_full_revision;
		AcknowledgePreparedHistory(legacy, committed_chat);

		const auto secondary = [&](ChatPublicationFile role, PublicationStage& stage, const fs::path& destination,
		                           const fs::path& recovery, const PublicationExpectedFile& expected)
		{
			const auto outcome = attempt(role, stage, destination, recovery, false, &expected);
			if (!outcome.written.success || !outcome.confirmed || outcome.written.interrupted ||
			    outcome.written.stage != uam::io::AtomicWriteStage::Complete || !outcome.written.error.empty())
			{
				result.secondary_degraded = true;
				if (result.error.empty()) result.error = outcome.written.error.empty() ?
				    "Chat primary committed; secondary publication degraded." : std::string(uam::strings::Utf8Prefix(outcome.written.error, 1024));
			}
			return !outcome.allowed || outcome.confirmed; // Stop subsequent secondary writes after uncertainty.
		};
		bool continue_secondary = true;
		if (!legacy.index_temp.empty())
			continue_secondary = secondary(ChatPublicationFile::MessageIndex, prepared->m_index, legacy.index_path,
			    prepared->m_index_recovery, prepared->m_index_before);
		if (continue_secondary)
			secondary(ChatPublicationFile::Summary, prepared->m_summary, legacy.summary_path,
			    prepared->m_summary_recovery, prepared->m_summary_before);
		collect_evidence();
		return result;
	}
	catch (...)
	{
		result.state = primary_confirmed ? ChatPublicationState::Committed :
		    (primary_allowed ? ChatPublicationState::Uncertain : ChatPublicationState::Rejected);
		if (primary_confirmed) result.secondary_degraded = true;
		result.error = "Conditional publication failed; known commit facts and possible evidence were retained.";
		try { collect_evidence(); } catch (...) {}
		return result;
	}
}

namespace
{
	bool StoppedCompleteGraphMatches(const fs::path& root, ChatSession left, ChatSession right)
	{
		if (!left.messages_loaded || !right.messages_loaded || left.messages != right.messages) return false;
		// These two values are derived loader bookkeeping, not independently authored state.
		for (auto* graph : {&left, &right})
		{
			graph->persisted_message_count = graph->messages.size();
			graph->persisted_messages_digest = SummaryDigest(*graph, graph->messages.size());
		}
		const auto fingerprint = ChatRepository::ChatMetadataFingerprint(root, left);
		return !fingerprint.empty() && fingerprint == ChatRepository::ChatMetadataFingerprint(root, right);
	}

	std::optional<PublicationExpectedFile> StoppedExpectedFile(const fs::path& path,
	    std::shared_ptr<const uam::platform::PublishedFileChangeWitness> physical = {},
	    const std::string* held_bytes = nullptr)
	{
		if (!physical && StoppedPublicationAbsent(path)) return PublicationExpectedFile{};
		if (!physical) physical = uam::platform::PublishedFileChangeWitness::Capture(path);
		const auto own_bytes = !held_bytes && physical ? physical->ReadBounded(kMaxChatJsonBytes) : std::nullopt;
		if (!physical || (!held_bytes && !own_bytes)) return {};
		const std::string& bytes = held_bytes ? *held_bytes : *own_bytes;
		uam::platform::IncrementalSha256 hash;
		if (!hash.Add(bytes)) return {};
		const auto digest = hash.Finish();
		std::error_code error;
		const auto modified = fs::last_write_time(path, error);
		if (error || digest.empty() || !physical->MatchesContent(bytes.size(), digest) || !physical->Current()) return {};
		PublicationExpectedFile expected;
		expected.absent = false;
		expected.witness = {static_cast<std::uint64_t>(bytes.size()), modified, digest};
		expected.physical = std::move(physical);
		return expected;
	}
}

std::shared_ptr<PreparedChatPublication> ChatRepository::PrepareStoppedChatLaunchStorage(
    const std::shared_ptr<const StoppedChatCanonicalRead>& observed, const ChatSession& selected,
    const StoppedChatLaunchDelta& delta, std::string* error_out,
    const std::function<bool(ChatPublicationFile, uam::io::AtomicWriteStage)>& creation_hook_for_tests)
try
{
	if (error_out) error_out->clear();
	const auto fail = [error_out](const char* error) -> std::shared_ptr<PreparedChatPublication>
	{
		if (error_out) *error_out = error;
		return {};
	};
	if (!observed || !observed->Current() || observed->m_chat.execution_host_id == "local" ||
	    observed->m_chat.native_session_id.empty() || delta.process_control_token.empty() || delta.process_control_token.size() > 256 ||
	    std::ranges::any_of(delta.process_control_token, [](unsigned char value) { return value < 0x20 || value == 0x7f; }) ||
	    delta.process_control_token == observed->m_chat.remote_process_control_token ||
	    !StoppedCompleteGraphMatches(observed->m_root, observed->m_chat, selected))
		return fail("Stopped launch needs the exact complete selected quiescent graph and a new bounded token.");
	if (delta.transport_launch)
	{
		const auto& protocol = delta.transport_launch->structured_protocol;
		if (protocol.empty() || protocol.size() > 64 ||
		    std::ranges::any_of(protocol, [](unsigned char value) { return value <= 0x20 || value >= 0x7f; }))
			return fail("The saved chat's launch protocol is invalid.");
	}
	const auto primary_before = StoppedExpectedFile(observed->m_primary_path, observed->m_primary, &observed->m_primary_json);
	const auto summary_before = observed->m_summary ? StoppedExpectedFile(observed->m_summary_path, observed->m_summary, &*observed->m_summary_json) :
	    std::optional<PublicationExpectedFile>(PublicationExpectedFile{});
	const auto index_before = StoppedExpectedFile(uam::chat_pages::IndexPath(observed->m_primary_path));
	const auto revision = ChatStorageRevision(observed->m_root, observed->m_chat.id);
	if (!primary_before || !summary_before || !index_before || !revision || *revision == "missing" || !observed->Current())
		return fail("Stopped launch predecessor or companion observation changed.");
	const auto make = [&](bool inline_primary)
	{
		auto prepared = std::shared_ptr<PreparedChatPublication>(new PreparedChatPublication());
		prepared->m_root = observed->m_root;
		prepared->m_mode = ChatPublicationMode::UpdateExisting;
		prepared->m_stopped_read = observed;
		prepared->m_stopped_creation_hook = creation_hook_for_tests;
		prepared->m_inline_primary = inline_primary;
		prepared->m_primary_before = *primary_before;
		prepared->m_summary_before = *summary_before;
		prepared->m_index_before = *index_before;
		auto& legacy = prepared->m_legacy;
		legacy.primary_path = observed->m_primary_path;
		legacy.summary_path = observed->m_summary_path;
		legacy.expected_storage_revision = *revision;
		prepared->m_primary.path = PublicationPrivatePath(legacy.primary_path.parent_path(), "primary");
		prepared->m_summary.path = PublicationPrivatePath(legacy.summary_path.parent_path(), "summary");
		prepared->m_index.path = PublicationPrivatePath(legacy.primary_path.parent_path(), "index");
		prepared->m_source_index.path = PublicationPrivatePath(legacy.primary_path.parent_path(), "source-index");
		prepared->m_records = PublicationPrivatePath(legacy.primary_path.parent_path(), "records");
		prepared->m_source_records = PublicationPrivatePath(legacy.primary_path.parent_path(), "source-records");
		prepared->m_primary_recovery = PublicationPrivatePath(legacy.primary_path.parent_path(), "primary-predecessor");
		prepared->m_summary_recovery = PublicationPrivatePath(legacy.summary_path.parent_path(), "summary-predecessor");
		prepared->m_index_recovery = PublicationPrivatePath(legacy.primary_path.parent_path(), "index-predecessor");
		return prepared;
	};
	if (!PublicationCreateDirectory(observed->m_root, observed->m_summary_path.parent_path()))
		return fail("Stopped launch summary storage is unavailable.");
	std::optional<bool> admitted_inline;
	for (const bool inline_primary : {true, false})
	{
		auto baseline = make(inline_primary);
		if (!SaveChatImpl(observed->m_root, observed->m_chat, false, false, &baseline->m_legacy, nullptr, nullptr, baseline.get()) ||
		    !baseline->MatchesBeforePrimary()) return fail("Stopped baseline could not be emitted under the held predecessor.");
		const auto primary = baseline->m_primary.physical->ReadBounded(kMaxChatJsonBytes);
		const auto summary = observed->m_summary_json ? baseline->m_summary.physical->ReadBounded(kMaxChatJsonBytes) : std::nullopt;
		if (primary && *primary == observed->m_primary_json &&
		    (!observed->m_summary_json || (summary && *summary == *observed->m_summary_json)))
		{
			admitted_inline = inline_primary;
			break;
		}
	}
	if (!admitted_inline) return fail("Complete raw canonical baseline is unsupported; original fields and bytes are retained.");
	auto prepared = make(*admitted_inline);
	ChatSession candidate = observed->m_chat;
	candidate.remote_process_control_token = delta.process_control_token;
	if (delta.transport_launch)
	{
		candidate.remote_process_exists = true;
		candidate.remote_structured_protocol = delta.transport_launch->structured_protocol;
		candidate.remote_prompt_image_supported = false;
	}
	if (!SaveChatImpl(observed->m_root, candidate, false, false, &prepared->m_legacy, nullptr, nullptr, prepared.get()) ||
	    !prepared->MatchesBeforePrimary()) return fail("Stopped launch candidate lost its exact held predecessor.");
	// ReadStopped rejects bound/invalid managed births. Full baseline equality prevents erasing a raw marker.
	// Candidate changes only the typed token and explicitly requested transport marker, never a birth.
	prepared->m_stopped_birth_verified = observed->m_chat.managed_followup_incarnation.empty() && candidate.managed_followup_incarnation.empty();
	prepared->m_stopped_candidate = std::move(candidate);
	return prepared;
}
catch (...)
{
	if (error_out) *error_out = "Stopped launch preparation failed; no canonical publication was requested.";
	return {};
}

std::shared_ptr<PreparedChatPublication> ChatRepository::PrepareOwnChatPublication(
    const std::shared_ptr<const StoppedChatLaunchStorageReceipt>& predecessor, const ChatSession& candidate,
    std::string* error_out,
    const std::function<bool(ChatPublicationFile, uam::io::AtomicWriteStage)>& creation_hook_for_tests)
try
{
	if (error_out) error_out->clear();
	const auto fail = [error_out](const char* message) -> std::shared_ptr<PreparedChatPublication>
	{
		if (error_out) *error_out = message;
		return {};
	};
	if (!predecessor || !predecessor->Current()) return fail("The chat's own saved predecessor changed.");
	const ChatSession& previous = predecessor->m_graph;
	// Storage identity/configuration only. The runtime still owns operation, row and packet authorization.
	if (!candidate.messages_loaded || candidate.messages.empty() || candidate.imported_read_only ||
	    candidate.id != previous.id || candidate.created_at != previous.created_at ||
	    candidate.provider_id != previous.provider_id || candidate.execution_host_id != previous.execution_host_id ||
	    candidate.native_session_id != previous.native_session_id || candidate.workspace_directory != previous.workspace_directory ||
	    candidate.workspace_isolation_kind != previous.workspace_isolation_kind ||
	    candidate.workspace_source_directory != previous.workspace_source_directory ||
	    candidate.workspace_base_ref != previous.workspace_base_ref || candidate.git_base_branch_override != previous.git_base_branch_override ||
	    candidate.workspace_branch_name != previous.workspace_branch_name || candidate.svn_update_pending != previous.svn_update_pending ||
	    candidate.workspace_worktree_directory != previous.workspace_worktree_directory ||
	    candidate.parent_chat_id != previous.parent_chat_id || candidate.branch_root_chat_id != previous.branch_root_chat_id ||
	    candidate.subagent_parent_chat_id != previous.subagent_parent_chat_id || candidate.subagent_parent_created_at != previous.subagent_parent_created_at ||
	    candidate.agent_run_id != previous.agent_run_id || candidate.goal_owner_chat_id != previous.goal_owner_chat_id ||
	    !candidate.managed_followup_incarnation.empty() || candidate.managed_followup_input_unavailable ||
	    !candidate.temporary_storage_owner_token.empty() || candidate.temporary_parent_chat_id != previous.temporary_parent_chat_id ||
	    candidate.remote_process_control_token != previous.remote_process_control_token ||
	    candidate.remote_structured_protocol != previous.remote_structured_protocol ||
	    candidate.model_id != previous.model_id || candidate.reasoning_effort != previous.reasoning_effort ||
	    candidate.approval_mode != previous.approval_mode || candidate.command_safety_tier != previous.command_safety_tier ||
	    candidate.uam_agent_id != previous.uam_agent_id || candidate.extra_flags != previous.extra_flags ||
	    candidate.reviewer_model_id != previous.reviewer_model_id || candidate.small_model_mode != previous.small_model_mode ||
	    candidate.service_tier != previous.service_tier || candidate.service_tier_explicit != previous.service_tier_explicit ||
	    candidate.memory_level != previous.memory_level || candidate.memory_enabled != previous.memory_enabled ||
	    candidate.computer_use_backend != previous.computer_use_backend || candidate.uam_control_enabled != previous.uam_control_enabled ||
	    candidate.uam_control_explicitly_disabled != previous.uam_control_explicitly_disabled ||
	    candidate.acp_queued_prompts.size() > 1 || candidate.acp_queue_storage_error || candidate.acp_queue_held ||
	    std::ranges::any_of(candidate.acp_queued_prompts, [](const auto& head) { return head.managed_turn.has_value(); }))
		return fail("The complete saved candidate no longer matches its own chat and launch settings.");
	const auto primary_path = AppPaths::UamChatFilePath(predecessor->m_root, previous.id);
	const auto summary_path = AppPaths::UamChatSummaryFilePath(predecessor->m_root, previous.id);
	// Read the already-held own objects, never a newly opened canonical leaf as new authority.
	const auto primary_raw = predecessor->m_physical->ReadBounded(kMaxChatJsonBytes);
	const auto summary_raw = predecessor->m_summary->ReadBounded(kMaxChatJsonBytes);
	const auto primary_before = primary_raw ? StoppedExpectedFile(primary_path, predecessor->m_physical, &*primary_raw) : std::nullopt;
	const auto summary_before = summary_raw ? StoppedExpectedFile(summary_path, predecessor->m_summary, &*summary_raw) : std::nullopt;
	const auto index_before = predecessor->m_index ? StoppedExpectedFile(predecessor->m_index_path, predecessor->m_index) :
	    std::optional<PublicationExpectedFile>(PublicationExpectedFile{});
	const auto revision = ChatStorageRevision(predecessor->m_root, previous.id);
	if (!primary_before || !summary_before || !index_before || !revision ||
	    *revision != predecessor->m_primary.revision || !predecessor->Current())
		return fail("The chat's own saved primary or companion changed during preparation.");
	const auto make = [&](bool inline_primary)
	{
		auto prepared = std::shared_ptr<PreparedChatPublication>(new PreparedChatPublication());
		prepared->m_root = predecessor->m_root;
		prepared->m_mode = ChatPublicationMode::UpdateExisting;
		prepared->m_own_predecessor = predecessor;
		prepared->m_stopped_creation_hook = creation_hook_for_tests;
		prepared->m_inline_primary = inline_primary;
		prepared->m_primary_before = *primary_before;
		prepared->m_summary_before = *summary_before;
		prepared->m_index_before = *index_before;
		auto& legacy = prepared->m_legacy;
		legacy.primary_path = primary_path;
		legacy.summary_path = summary_path;
		legacy.expected_storage_revision = *revision;
		prepared->m_primary.path = PublicationPrivatePath(legacy.primary_path.parent_path(), "primary");
		prepared->m_summary.path = PublicationPrivatePath(legacy.summary_path.parent_path(), "summary");
		prepared->m_index.path = PublicationPrivatePath(legacy.primary_path.parent_path(), "index");
		prepared->m_source_index.path = PublicationPrivatePath(legacy.primary_path.parent_path(), "source-index");
		prepared->m_records = PublicationPrivatePath(legacy.primary_path.parent_path(), "records");
		prepared->m_source_records = PublicationPrivatePath(legacy.primary_path.parent_path(), "source-records");
		prepared->m_primary_recovery = PublicationPrivatePath(legacy.primary_path.parent_path(), "primary-predecessor");
		prepared->m_summary_recovery = PublicationPrivatePath(legacy.summary_path.parent_path(), "summary-predecessor");
		prepared->m_index_recovery = PublicationPrivatePath(legacy.primary_path.parent_path(), "index-predecessor");
		return prepared;
	};
	// Re-emit the complete own baseline using its recorded writer style. No decode/reconciliation/overlay adoption.
	auto baseline = make(predecessor->m_inline_primary);
	if (!SaveChatImpl(predecessor->m_root, previous, false, false, &baseline->m_legacy, nullptr, nullptr, baseline.get()) ||
	    !baseline->MatchesBeforePrimary()) return fail("The own saved baseline could not be verified.");
	const auto baseline_primary = baseline->m_primary.physical->ReadBounded(kMaxChatJsonBytes);
	const auto baseline_summary = baseline->m_summary.physical->ReadBounded(kMaxChatJsonBytes);
	if (!baseline_primary || !baseline_summary || *baseline_primary != *primary_raw || *baseline_summary != *summary_raw)
		return fail("The complete own saved baseline cannot be preserved by the current writer.");
	auto prepared = make(predecessor->m_inline_primary);
	// skip_unchanged=false: a same-graph binding is a real serialization/publication, not an observation.
	if (!SaveChatImpl(predecessor->m_root, candidate, false, false, &prepared->m_legacy, nullptr, nullptr, prepared.get()) ||
	    !prepared->MatchesBeforePrimary()) return fail("The complete candidate lost its own saved predecessor.");
	const auto emitted = prepared->m_primary.physical->ReadBounded(kMaxChatJsonBytes);
	if (!emitted) return fail("The complete candidate could not be read from its own stage.");
	const auto decoded = ParseLocalChatDocument(primary_path, *emitted, true, 0, false, kMaxChatJsonBytes, nullptr, false);
	if (!decoded.chat || !StoppedCompleteGraphMatches(predecessor->m_root, candidate, *decoded.chat) ||
	    !ChatsEquivalentForRecovery(candidate, *decoded.chat) || candidate.acp_queued_prompts != decoded.chat->acp_queued_prompts)
		return fail("The complete candidate cannot be preserved by the current writer and reader.");
	prepared->m_stopped_birth_verified = previous.managed_followup_incarnation.empty() && candidate.managed_followup_incarnation.empty();
	prepared->m_stopped_candidate = candidate;
	return prepared;
}
catch (...)
{
	if (error_out) *error_out = "Own chat continuation failed; no canonical publication was requested.";
	return {};
}

bool StoppedChatLaunchStorageReceipt::Current() const
{
	return PublicationDirectory(m_root) && m_physical && m_physical->Current() && m_summary && m_summary->Current() &&
	    StoppedPublicationAbsent(m_queue_path) && (m_index ? m_index->Current() : StoppedPublicationAbsent(m_index_path));
}

bool StoppedChatLaunchStorageReceipt::MatchesDataRoot(const fs::path& data_root) const
try
{
	return !data_root.empty() && PublicationRootSpelling(data_root) == m_root;
}
catch (...) { return false; }

StoppedChatLaunchPublicationResult ChatRepository::PublishStoppedChatLaunchStorage(
    const std::shared_ptr<PreparedChatPublication>& prepared, const std::function<bool()>& still_owned,
    const std::function<bool(ChatPublicationFile, uam::io::AtomicWriteStage)>& stage_hook_for_tests)
{
	StoppedChatLaunchPublicationResult result;
	if (!prepared || !prepared->HasOwnPublicationPredecessor() || !prepared->m_stopped_candidate || !prepared->m_stopped_birth_verified)
	{
		result.publication.error = "Stopped launch preparation is absent or not repository-admitted.";
		return result;
	}
	ChatSession committed = *prepared->m_stopped_candidate;
	result.publication = PublishPreparedChatPublication(prepared, still_owned, &committed, stage_hook_for_tests);
	if (result.publication.state != ChatPublicationState::Committed || result.publication.backup_degraded || result.publication.secondary_degraded)
		return result;
	try
	{
		const auto capture_own = [](const fs::path& path, const PublicationStage& stage)
		    -> std::shared_ptr<const uam::platform::PublishedFileChangeWitness>
		{
			auto physical = uam::platform::PublishedFileChangeWitness::Capture(path);
			return stage.witness && stage.origin && stage.physical && physical && stage.origin->Matches(*physical) &&
			    stage.physical->SameRetainedObject(*physical) &&
			    physical->MatchesContent(stage.witness->bytes, stage.witness->digest) && physical->Current() ? physical : nullptr;
		};
		auto receipt = std::shared_ptr<StoppedChatLaunchStorageReceipt>(new StoppedChatLaunchStorageReceipt());
		receipt->m_root = prepared->m_root;
		receipt->m_graph = std::move(committed);
		if (prepared->m_own_predecessor)
		{
			receipt->m_launch_origin = prepared->m_own_predecessor->m_launch_origin;
		}
		else if (prepared->m_stopped_read && receipt->m_graph.remote_process_exists && !receipt->m_graph.remote_structured_protocol.empty())
		{
			receipt->m_initial_transport_launch = true;
			receipt->m_launch_origin = std::make_shared<const std::uint8_t>(0);
		}
		receipt->m_queue_path = prepared->m_own_predecessor ? prepared->m_own_predecessor->m_queue_path : prepared->m_stopped_read->m_queue_path;
		receipt->m_inline_primary = prepared->m_inline_primary;
		receipt->m_index_path = uam::chat_pages::IndexPath(prepared->m_legacy.primary_path);
		receipt->m_physical = capture_own(prepared->m_legacy.primary_path, prepared->m_primary);
		receipt->m_summary = capture_own(prepared->m_legacy.summary_path, prepared->m_summary);
		receipt->m_index = prepared->m_legacy.index_temp.empty() ? prepared->m_index_before.physical : capture_own(prepared->m_legacy.index_path, prepared->m_index);
		const auto revision = ChatStorageRevision(prepared->m_root, receipt->m_graph.id);
		if (!prepared->m_primary.witness || !revision || !receipt->Current() || !still_owned || !still_owned() ||
		    (!prepared->m_legacy.index_temp.empty() && !receipt->m_index))
		{
			result.publication.error = "Storage committed; complete own publication witness is unavailable.";
			return result;
		}
		receipt->m_primary = {*revision, prepared->m_primary.witness->bytes, prepared->m_primary.witness->digest};
		result.receipt = std::move(receipt);
	}
	catch (...) { result.publication.error = "Storage committed; own publication observation failed."; }
	return result;
}

namespace uam::queue_recovery
{
	struct EmptyHeldQueueAckProof
	{
		fs::path root, primary_path, summary_path, queue_path;
		ChatSession candidate; // Detached canonical DATA, read and cloned only by workers.
		std::string metadata, queue_fingerprint;
		std::shared_ptr<const platform::PublishedFileChangeWitness> primary, summary, journal;
		bool PrimaryCurrent() const
		{
			return PublicationDirectory(root) && PublicationDirectory(primary_path.parent_path()) && primary && primary->Current() &&
			    (summary ? summary->Current() : StoppedPublicationAbsent(summary_path));
		}
		bool Current() const
		{
			return PrimaryCurrent() && (journal ? journal->Current() : StoppedPublicationAbsent(queue_path));
		}
		bool LiveMatches(const fs::path& data_root, const ChatSession& live) const
		{
			return data_root == root && live.messages_loaded && live.messages == candidate.messages &&
			    live.acp_queue_storage_revision == candidate.acp_queue_storage_revision &&
			    live.acp_queue_full_save_revision == candidate.acp_queue_full_save_revision &&
			    ChatRepository::ChatMetadataFingerprint(root, live) == metadata &&
			    ChatRepository::QueueAdmissionFingerprint(live) == queue_fingerprint;
		}
	};
}

std::shared_ptr<const uam::queue_recovery::EmptyHeldQueueAckProof> uam::queue_recovery::PrepareEmptyHeldQueueAck(
    const fs::path& data_root, ChatSession chat, const std::function<bool()>& owned)
try
{
	constexpr std::uint64_t limit = 4U * 1024U * 1024U;
	if (!owned || !owned() || !data_root.is_absolute() || !chat.messages_loaded ||
	    !chat.acp_queue_held || chat.native_session_reset_pending || chat.acp_queue_storage_error || chat.managed_followup_input_unavailable ||
	    chat.imported_read_only || !chat.acp_queued_prompts.empty() || chat.acp_dispatched_queued_prompt_count != 0 ||
	    !chat.acp_unsent_prompt_recoveries.empty() || !uam::chat_ids::IsSafeStorageChatId(chat.id)) return {};
	auto proof = uam::native_history::TryCreateWorkerOwnedNativeRefresh<EmptyHeldQueueAckProof>();
	if (!proof) return {}; // Arm the existing bounded worker destructor BEFORE any payload is read.
	const auto root = PublicationRootSpelling(data_root);
	const auto primary_path = AppPaths::UamChatFilePath(root, chat.id);
	const auto summary_path = AppPaths::UamChatSummaryFilePath(root, chat.id);
	const auto queue_path = QueueJournalPath(root, chat.id);
	if (!PublicationDirectory(root) || !PublicationDirectory(primary_path.parent_path()) ||
	    (!PublicationAbsent(summary_path.parent_path()) && !PublicationDirectory(summary_path.parent_path())) ||
	    (!PublicationAbsent(queue_path.parent_path()) && !PublicationDirectory(queue_path.parent_path()))) return {};
	const auto primary = uam::platform::PublishedFileChangeWitness::Capture(primary_path);
	const auto primary_bytes = primary ? primary->ReadBounded(limit) : std::nullopt;
	if (!primary_bytes) return {};
	const auto document = ParseJson(*primary_bytes);
	if (!document || document->type != JsonValue::Type::Object || RawManagedQueueMarker(*document)) return {};
	const auto empty_array = [](const JsonValue& object, std::string_view key, bool required)
	{
		const auto* value = object.Find(key);
		return value ? value->type == JsonValue::Type::Array && value->array_value.empty() : !required;
	};
	const auto false_bool = [](const JsonValue& object, std::string_view key)
	{
		const auto* value = object.Find(key);
		return !value || (value->type == JsonValue::Type::Bool && !value->bool_value);
	};
	const auto empty_string = [](const JsonValue& object, std::string_view key)
	{
		const auto* value = object.Find(key);
		return !value || (value->type == JsonValue::Type::String && value->string_value.empty());
	};
	const auto raw_empty_hold = [&](const JsonValue& raw) -> bool
	{
		const auto* held = raw.Find(kChatAcpQueueHeldField);
		const auto* dispatched = raw.Find(kChatAcpDispatchedQueuedPromptCountField);
		const auto* revision = raw.Find(kQueueRevisionField);
		const auto* full_revision = raw.Find(kQueueFullRevisionField);
		if ((revision && revision->type != JsonValue::Type::String) || !full_revision ||
		    full_revision->type != JsonValue::Type::String) return {}; // Existing writer permits a typechecked empty full-save revision.
		if (!held || held->type != JsonValue::Type::Bool || !held->bool_value ||
		    !dispatched || dispatched->type != JsonValue::Type::Number || dispatched->number_value != 0 ||
		    !empty_array(raw, kChatAcpQueuedPromptsField, true) ||
		    !empty_array(raw, "acp_unsent_prompt_recoveries", false) ||
		    !empty_array(raw, kChatRemotePendingRequestsField, false) ||
		    !empty_array(raw, kChatRemoteInteractionResponsesField, false)) return {};
		// A stable native identity makes this saved terminal bootstrap hint inert; it is not pending work.
		const auto* bootstrap = raw.Find(kChatRemoteClaudeSessionUnstartedField);
		if (bootstrap && (bootstrap->type != JsonValue::Type::Bool ||
		    (bootstrap->bool_value && chat.native_session_id.empty()))) return {};
		for (const auto key : {kChatRemoteTurnReconnectPendingField, kChatRemoteProcessExistsField,
		    kChatRemoteStopCleanupPendingField, kChatRemoteRestartPendingField, kChatRemoteSourceExitPendingField,
		    std::string_view("remote_manual_compaction_pending"),
		    std::string_view("native_session_reset_pending"), std::string_view("remote_reset_pending"), std::string_view("svn_update_pending"),
		    std::string_view("acp_queue_storage_error"), std::string_view("managed_followup_input_unavailable")})
			if (!false_bool(raw, key)) return {};
		for (const auto key : {kChatRemotePromptDeliverySessionIdField, kChatRemotePromptDeliveryIdField,
		    kChatRemotePromptDeliveryPayloadField, kChatRemoteActiveTurnIdField, kChatManagedFollowupBirthField,
		    std::string_view("agent_run_id"), std::string_view("active_goal_id"), std::string_view("goal_owner_chat_id"),
		    std::string_view("temporary_parent_chat_id"), std::string_view("temporary_storage_owner_token")})
			if (!empty_string(raw, key)) return {};
		return true;
	};
	if (!raw_empty_hold(*document)) return {};
	const auto* messages = document->Find(kChatMessagesField);
	if (!messages || messages->type != JsonValue::Type::Array) return {};
	for (const auto& row : messages->array_value)
		if (row.type != JsonValue::Type::Object || !false_bool(row, "acp_prompt_not_sent")) return {};
	const auto decoded = ParseLocalChatDocument(primary_path, *primary_bytes, true, 0, false, limit, nullptr, false);
	if (!decoded.chat || decoded.chat->id != chat.id || !decoded.chat->claude_quota_continuation.id.empty() ||
	    !decoded.chat->acp_unsent_prompt_recoveries.empty()) return {};
	ChatSession effective = *decoded.chat;
	std::shared_ptr<const uam::platform::PublishedFileChangeWitness> summary;
	if (!StoppedPublicationAbsent(summary_path))
	{
		summary = uam::platform::PublishedFileChangeWitness::Capture(summary_path);
		const auto bytes = summary ? summary->ReadBounded(limit) : std::nullopt;
		if (!bytes) return {};
		const auto raw_summary = ParseJson(*bytes);
		if (!raw_summary || raw_summary->type != JsonValue::Type::Object || RawManagedQueueMarker(*raw_summary) ||
		    !raw_empty_hold(*raw_summary) || QueueOwner(*raw_summary) != QueueOwner(*document)) return {};
		const auto decoded_summary = ParseLocalChatDocument(summary_path, *bytes, false, primary_bytes->size(), false, limit, nullptr, false);
		if (!decoded_summary.chat || decoded_summary.chat->id != chat.id) return {};
		CarrySummaryFieldsIntoHydratedChat(effective, *decoded_summary.chat);
	}
	effective.persisted_message_count = chat.persisted_message_count;
	effective.persisted_messages_digest = chat.persisted_messages_digest;
	const std::string metadata = ChatRepository::ChatMetadataFingerprint(root, chat);
	if (metadata.empty() || ChatRepository::ChatMetadataFingerprint(root, effective) != metadata ||
	    ChatRepository::QueueAdmissionFingerprint(effective) != ChatRepository::QueueAdmissionFingerprint(chat)) return {};
	std::shared_ptr<const uam::platform::PublishedFileChangeWitness> journal;
	if (!StoppedPublicationAbsent(queue_path))
	{
		journal = uam::platform::PublishedFileChangeWitness::Capture(queue_path);
		const auto bytes = journal ? journal->ReadBounded(kMaxQueueJournalBytes) : std::nullopt;
		if (!bytes) return {};
		bool exists = false, managed = false;
		const auto queue = ReadQueueJournal(queue_path, exists, &managed);
		// A matching revision is already consumed: its historical base is not replay authority.
		if (!exists || !queue || managed || !queue->prompts.empty() || queue->dispatched != 0 ||
		    queue->revision != chat.acp_queue_storage_revision || queue->owner != QueueOwner(*document) ||
		    queue->owner != QueueOwner(chat) || !journal->Current()) return {};
	}
	if (!owned() || !primary->Current() || (summary && !summary->Current()) || (journal && !journal->Current())) return {};
	proof->root = root; proof->primary_path = primary_path; proof->summary_path = summary_path; proof->queue_path = queue_path;
	proof->candidate = std::move(effective);
	proof->metadata = metadata;
	proof->queue_fingerprint = ChatRepository::QueueAdmissionFingerprint(chat);
	proof->primary = primary; proof->summary = std::move(summary); proof->journal = std::move(journal);
	return proof->Current() ? proof : std::shared_ptr<const EmptyHeldQueueAckProof>{};
}
catch (...) { return {}; }

bool uam::queue_recovery::PreparedEmptyHeldQueueAckMatches(const EmptyHeldQueueAckProof& proof,
    const fs::path& root, const ChatSession& chat)
try { return proof.LiveMatches(root, chat) && proof.Current(); }
catch (...) { return false; }

uam::queue_recovery::EmptyHeldQueueAckReceipt uam::queue_recovery::PublishPreparedEmptyHeldQueueAck(
    const std::shared_ptr<const EmptyHeldQueueAckProof>& proof, const std::function<bool()>& owned)
{
	EmptyHeldQueueAckReceipt result;
	bool writer_admitted = false;
	try
	{
		if (!proof || !owned || !owned() || !proof->Current()) return result;
		ChatSession candidate = proof->candidate; // Worker-only full graph clone.
		candidate.acp_queue_held = false;
		bool full_save_required = false;
		result.saved = ChatRepository::SaveQueuedPromptOutbox(proof->root, candidate, &full_save_required,
		    [&] { writer_admitted = owned() && proof->Current(); return writer_admitted; }); // Existing writer-lock boundary, never full-save fallback.
		result.uncertain = writer_admitted && !result.saved; // WriteTextFile can report failure after rename.
		if (!result.saved) return result;
		result.revision = std::move(candidate.acp_queue_storage_revision); // Real committed facts survive later revocation.
		result.journal = platform::PublishedFileChangeWitness::Capture(proof->queue_path);
		bool exists = false, managed = false;
		const auto current = ReadQueueJournal(proof->queue_path, exists, &managed);
		if (!result.journal || !exists || !current || managed || current->held || !current->prompts.empty() ||
		    current->dispatched != 0 || current->revision != result.revision || current->owner != QueueOwner(proof->candidate) ||
		    !result.journal->Current()) result.journal.reset();
	}
	catch (...) { result.uncertain = writer_admitted; result.journal.reset(); }
	return result;
}

bool uam::queue_recovery::PublishedEmptyHeldQueueAckMatches(const EmptyHeldQueueAckProof& proof,
    const EmptyHeldQueueAckReceipt& receipt, const fs::path& root, const ChatSession& chat)
try
{
	return receipt.saved && !receipt.revision.empty() && receipt.journal && receipt.journal->Current() &&
	    proof.PrimaryCurrent() && proof.LiveMatches(root, chat);
}
catch (...) { return false; }

bool uam::queue_recovery::AdoptEmptyHeldQueueAckRevision(const EmptyHeldQueueAckProof& proof,
    const EmptyHeldQueueAckReceipt& receipt, const fs::path& root, ChatSession& chat)
try
{
	if (!receipt.saved || receipt.revision.empty() || !receipt.journal || !receipt.journal->Current() ||
	    root != proof.root || QueueOwner(chat) != QueueOwner(proof.candidate) ||
	    chat.acp_queue_storage_revision != proof.candidate.acp_queue_storage_revision ||
	    chat.acp_queue_full_save_revision != proof.candidate.acp_queue_full_save_revision) return false;
	chat.acp_queue_storage_revision = receipt.revision;
	return true; // Revision-only adoption does not release a hold or authorize new input.
}
catch (...) { return false; }
