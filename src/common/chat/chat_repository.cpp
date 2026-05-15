#include "common/chat/chat_repository.h"

#include "common/chat/chat_ids.h"
#include "common/chat/message_attachment_json.h"
#include "common/config/approval_modes.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/json_runtime.h"
#include "common/utils/io_utils.h"
#include "common/utils/parse_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

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
	constexpr std::string_view kMessageInterruptedField = "interrupted";
	constexpr std::string_view kMessageThoughtsField = "thoughts";
	constexpr std::string_view kMessagePlanSummaryField = "plan_summary";
	constexpr std::string_view kMessagePlanEntriesField = "plan_entries";
	constexpr std::string_view kMessageToolCallsField = "tool_calls";
	constexpr std::string_view kMessageBlocksField = "blocks";
	constexpr std::string_view kMessageMarkdownStoreFilesField = "markdown_store_files";
	constexpr std::string_view kMessageAttachmentsField = "attachments";

	constexpr std::string_view kPlanEntryPriorityField = "priority";
	constexpr std::string_view kPlanEntryStatusField = "status";

	constexpr std::string_view kToolCallIdField = "id";
	constexpr std::string_view kToolCallNameField = "name";
	constexpr std::string_view kToolCallArgsJsonField = "args_json";
	constexpr std::string_view kToolCallResultTextField = "result_text";
	constexpr std::string_view kToolCallStatusField = "status";

	constexpr std::string_view kMessageBlockTypeField = "type";
	constexpr std::string_view kMessageBlockTextField = "text";
	constexpr std::string_view kMessageBlockToolCallIdField = "tool_call_id";
	constexpr std::string_view kMessageBlockRequestIdField = "request_id";

	constexpr std::string_view kChatIdField = "id";
	constexpr std::string_view kChatProviderIdField = "provider_id";
	constexpr std::string_view kChatNativeSessionIdField = "native_session_id";
	constexpr std::string_view kChatParentChatIdField = "parent_chat_id";
	constexpr std::string_view kChatBranchRootChatIdField = "branch_root_chat_id";
	constexpr std::string_view kChatBranchFromMessageIndexField = "branch_from_message_index";
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
	constexpr std::string_view kChatApprovalModeField = "approval_mode";
	constexpr std::string_view kChatAutoApproveCommandsField = "auto_approve_commands";
	constexpr std::string_view kChatModelIdField = "model_id";
	constexpr std::string_view kChatReasoningEffortField = "reasoning_effort";
	constexpr std::string_view kChatServiceTierField = "service_tier";
	constexpr std::string_view kChatExtraFlagsField = "extra_flags";
	constexpr std::string_view kChatMemoryEnabledField = "memory_enabled";
	constexpr std::string_view kChatMemoryLastProcessedMessageCountField = "memory_last_processed_message_count";
	constexpr std::string_view kChatMemoryLastProcessedAtField = "memory_last_processed_at";
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
		return lhs.id == rhs.id && lhs.name == rhs.name && lhs.args_json == rhs.args_json && lhs.result_text == rhs.result_text && lhs.status == rhs.status;
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
		return lhs.markdown_store_files == rhs.markdown_store_files;
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
		return lhs.time_to_first_token_ms == rhs.time_to_first_token_ms && lhs.processing_time_ms == rhs.processing_time_ms && lhs.interrupted == rhs.interrupted;
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

	bool ChatIdentityFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		return lhs.id == rhs.id && lhs.provider_id == rhs.provider_id && lhs.native_session_id == rhs.native_session_id;
	}

	bool ChatBranchFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		if (lhs.parent_chat_id != rhs.parent_chat_id || lhs.branch_root_chat_id != rhs.branch_root_chat_id)
		{
			return false;
		}

		return lhs.branch_from_message_index == rhs.branch_from_message_index;
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

		return lhs.workspace_branch_name == rhs.workspace_branch_name && lhs.workspace_worktree_directory == rhs.workspace_worktree_directory;
	}

	bool ChatProviderFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		if (lhs.approval_mode != rhs.approval_mode || lhs.auto_approve_commands != rhs.auto_approve_commands || lhs.model_id != rhs.model_id)
		{
			return false;
		}
		if (lhs.reasoning_effort != rhs.reasoning_effort || lhs.service_tier != rhs.service_tier)
		{
			return false;
		}

		return lhs.extra_flags == rhs.extra_flags;
	}

	bool ChatMemoryFieldsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		if (lhs.memory_enabled != rhs.memory_enabled || lhs.memory_last_processed_message_count != rhs.memory_last_processed_message_count)
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

	LoadChatResult ParseLocalChatFile(const fs::path& path, bool include_messages = true)
	{
		const std::string file_text = uam::io::ReadTextFile(path);
		if (file_text.empty())
		{
			return {std::nullopt, "is empty"};
		}

		const auto root_opt = ParseJson(file_text);
		if (!root_opt || root_opt->type != JsonValue::Type::Object)
		{
			return {std::nullopt, "contains invalid JSON"};
		}

		const JsonValue& root = *root_opt;
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

		chat.provider_id = JsonStringOrEmpty(root.Find(kChatProviderIdField));
		chat.native_session_id = JsonStringOrEmpty(root.Find(kChatNativeSessionIdField));
		chat.parent_chat_id = JsonStringOrEmpty(root.Find(kChatParentChatIdField));
		chat.branch_root_chat_id = JsonStringOrEmpty(root.Find(kChatBranchRootChatIdField));
		chat.branch_from_message_index = IntFieldAtLeastOrDefault(root.Find(kChatBranchFromMessageIndexField), -1, -1);
		chat.folder_id = JsonStringOrEmpty(root.Find(kChatFolderIdField));
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
		chat.approval_mode = JsonStringOrEmpty(root.Find(kChatApprovalModeField));
		chat.auto_approve_commands = JsonBoolOrDefault(root.Find(kChatAutoApproveCommandsField), false);
		if (chat.approval_mode == uam::approval_modes::kLegacyYoloApprovalMode)
		{
			chat.approval_mode = uam::approval_modes::kDefaultApprovalMode;
			chat.auto_approve_commands = true;
		}
		chat.model_id = JsonStringOrEmpty(root.Find(kChatModelIdField));
		chat.reasoning_effort = JsonStringOrEmpty(root.Find(kChatReasoningEffortField));
		chat.service_tier = JsonStringOrEmpty(root.Find(kChatServiceTierField));
		chat.extra_flags = JsonStringOrEmpty(root.Find(kChatExtraFlagsField));
		chat.memory_enabled = JsonBoolOrDefault(root.Find(kChatMemoryEnabledField), true);
		chat.memory_last_processed_message_count = NonNegativeIntFieldOrZero(root.Find(kChatMemoryLastProcessedMessageCountField));
		chat.memory_last_processed_at = JsonStringOrEmpty(root.Find(kChatMemoryLastProcessedAtField));

		ApplyChatTimestampFallbacks(chat);
		NormalizeLoadedNativeSessionId(chat);
		if (chat.branch_root_chat_id.empty())
		{
			chat.branch_root_chat_id = chat.id;
		}

		const JsonValue* msgs = uam::json::ArrayOrNull(root.Find(kChatMessagesField));
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
			chat.persisted_message_count = 0;
		}

		chat.messages_loaded = include_messages;
		if (include_messages)
		{
			chat.persisted_message_count = chat.messages.size();
		}
		chat.persisted_messages_digest = SummaryDigest(chat, chat.persisted_message_count);

		return {std::move(chat), ""};
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

bool ChatRepository::SaveChat(const std::filesystem::path& data_root, const ChatSession& chat)
{
	static std::mutex save_mutex;
	std::lock_guard<std::mutex> lock(save_mutex);

	if (!uam::chat_ids::IsSafeStorageChatId(chat.id))
	{
		return false;
	}

	const fs::path file_path = AppPaths::UamChatFilePath(data_root, chat.id);

	std::error_code ec;
	if (!uam::paths::CreateDirectoriesNoThrow(file_path.parent_path(), &ec))
	{
		return false;
	}

	JsonValue root = uam::json::Object();
	uam::json::SetString(root, kChatIdField, chat.id);
	uam::json::SetString(root, kChatProviderIdField, chat.provider_id);
	uam::json::SetString(root, kChatNativeSessionIdField, chat.native_session_id);
	uam::json::SetString(root, kChatParentChatIdField, chat.parent_chat_id);
	uam::json::SetString(root, kChatBranchRootChatIdField, chat.branch_root_chat_id);
	uam::json::SetNumber(root, kChatBranchFromMessageIndexField, static_cast<double>(chat.branch_from_message_index));
	uam::json::SetString(root, kChatFolderIdField, chat.folder_id);
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
	uam::json::SetString(root, kChatApprovalModeField, chat.approval_mode);
	uam::json::SetBool(root, kChatAutoApproveCommandsField, chat.auto_approve_commands);
	uam::json::SetString(root, kChatModelIdField, chat.model_id);
	uam::json::SetString(root, kChatReasoningEffortField, chat.reasoning_effort);
	uam::json::SetString(root, kChatServiceTierField, chat.service_tier);
	uam::json::SetString(root, kChatExtraFlagsField, chat.extra_flags);
	uam::json::SetBool(root, kChatMemoryEnabledField, chat.memory_enabled);
	uam::json::SetNumber(root, kChatMemoryLastProcessedMessageCountField, static_cast<double>(chat.memory_last_processed_message_count));
	uam::json::SetString(root, kChatMemoryLastProcessedAtField, chat.memory_last_processed_at);

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
		const std::string existing_text = uam::io::ReadTextFile(file_path);
		const auto existing_root = existing_text.empty() ? std::optional<JsonValue>{} : ParseJson(existing_text);
		if (existing_root && existing_root->type == JsonValue::Type::Object)
		{
			if (const JsonValue* existing_messages = existing_root->Find(kChatMessagesField); existing_messages != nullptr && existing_messages->type == JsonValue::Type::Array)
			{
				uam::json::SetValue(root, kChatMessagesField, *existing_messages);
			}
		}
	}

	const std::string json = SerializeJson(root);
	return uam::io::WriteTextFile(file_path, json);
}

ChatStorageDeleteResult ChatRepository::DeleteChatStorageFiles(const std::filesystem::path& data_root, std::string_view chat_id)
{
	ChatStorageDeleteResult result;
	if (!uam::chat_ids::IsSafeStorageChatId(chat_id))
	{
		result.unsafe_chat_id = true;
		return result;
	}

	uam::paths::RemoveAllNoThrow(AppPaths::ChatPath(data_root, chat_id), &result.legacy_directory_error);
	uam::paths::RemoveFileNoThrow(AppPaths::UamChatFilePath(data_root, chat_id), &result.metadata_file_error);
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
		hydrated.folder_id = summary.folder_id;
		hydrated.title = summary.title;
		hydrated.pinned = summary.pinned;
		hydrated.workspace_directory = summary.workspace_directory;
		hydrated.workspace_isolation_kind = summary.workspace_isolation_kind;
		hydrated.workspace_source_directory = summary.workspace_source_directory;
		hydrated.workspace_base_ref = summary.workspace_base_ref;
		hydrated.workspace_branch_name = summary.workspace_branch_name;
		hydrated.workspace_worktree_directory = summary.workspace_worktree_directory;
		hydrated.approval_mode = summary.approval_mode;
		hydrated.auto_approve_commands = summary.auto_approve_commands;
		hydrated.model_id = summary.model_id;
		hydrated.reasoning_effort = summary.reasoning_effort;
		hydrated.service_tier = summary.service_tier;
		hydrated.extra_flags = summary.extra_flags;
		hydrated.memory_enabled = summary.memory_enabled;
		hydrated.memory_last_processed_message_count = summary.memory_last_processed_message_count;
		hydrated.memory_last_processed_at = summary.memory_last_processed_at;
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

			const LoadChatResult primary_chat = ParseLocalChatFile(entry.path(), include_messages);
			if (primary_chat.chat)
			{
				const ChatSession& primary = *primary_chat.chat;
				if (ChatIdWasMigrated(migrated_chat_ids, primary.id))
				{
					continue;
				}

				const fs::path backup_path = uam::io::MakeBackupPath(entry.path());
				if (uam::paths::PathExistsNoThrow(backup_path))
				{
					const LoadChatResult backup_chat = ParseLocalChatFile(backup_path, include_messages);
					if (backup_chat.chat && !ChatsEquivalentForRecovery(primary, *backup_chat.chat))
					{
						AppendWarning(warning_out, "Recovered chat file " + entry.path().string() + " differs from backup " + backup_path.string() + ".");
					}
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
