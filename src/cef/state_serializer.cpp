#include "cef/state_serializer.h"

#include "app/chat_domain_service.h"

#include "common/chat/message_attachment_json.h"
#include "common/config/settings_frontend_json.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/app_time.h"
#include "common/provider/codex/cli/codex_session_index.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_attention_kind.h"
#include "common/runtime/acp/acp_model_json.h"
#include "common/runtime/acp/acp_tool_items.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/utils/env_utils.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_set>
#include <vector>

namespace uam
{

	// ---------------------------------------------------------------------------
	// Internal helpers
	// ---------------------------------------------------------------------------

	namespace
	{
		namespace attachment_fields = uam::message_attachment_json;
		namespace attachment_frontend_fields = uam::message_attachment_json::frontend;

		std::string RoleStr(MessageRole role)
		{
			switch (role)
			{
			case MessageRole::User:
				return "user";
			case MessageRole::Assistant:
				return "assistant";
			case MessageRole::System:
				return "system";
			}
			return "user";
		}

		std::string ToolCallContentForFrontend(const ToolCall& tool_call)
		{
			if (!tool_call.args_json.empty() && !tool_call.result_text.empty())
			{
				return "Arguments:\n" + tool_call.args_json + "\n\nResult:\n" + tool_call.result_text;
			}
			if (!tool_call.result_text.empty())
			{
				return tool_call.result_text;
			}
			return tool_call.args_json;
		}

		nlohmann::json JsonArrayWithCapacity(std::size_t capacity)
		{
			nlohmann::json array = nlohmann::json::array();
			array.get_ref<nlohmann::json::array_t&>().reserve(capacity);
			return array;
		}

		std::string AcpAttentionKindForFrontend(const AcpSessionState& session)
		{
			if (!session.pending_user_input.request_id_json.empty())
			{
				return NormalizeAcpAttentionKind(session.pending_user_input.attention_kind, "question");
			}
			if (!session.pending_permission.request_id_json.empty())
			{
				if (session.pending_permission.kind == uam::acp_tool_items::kCommandExecution)
				{
					return "command";
				}
				if (session.pending_permission.kind == uam::acp_tool_items::kFileChange)
				{
					return "file";
				}
				return "permission";
			}
			if (session.lifecycle_state == "error" && !session.last_error.empty())
			{
				return "error";
			}
			return "";
		}

		bool ChatHasUnseenUpdate(const AppState& app, const ChatSession& chat)
		{
			return app.chats_with_unseen_updates.contains(chat.id);
		}

		nlohmann::json ReadJsonFile(const std::filesystem::path& path)
		{
			const std::string text = uam::io::ReadTextFile(path);
			if (text.empty())
			{
				return nullptr;
			}

			try
			{
				return nlohmann::json::parse(text);
			}
			catch (const nlohmann::json::exception&)
			{
				return nullptr;
			}
		}

		nlohmann::json ReadCachedCodexModelsForFrontend()
		{
			auto models_json = nlohmann::json::array();
			const nlohmann::json cache = ReadJsonFile(uam::codex::CodexHomePath() / "models_cache.json");
			if (!cache.is_object())
			{
				return models_json;
			}

			const nlohmann::json* models = uam::nlohmann_json::FindArrayField(cache, "models");
			if (models == nullptr)
			{
				return models_json;
			}

			std::vector<std::string> seen_model_ids;
			uam::acp_models::CodexModelParseOptions parse_options;
			parse_options.skip_hidden_field = false;
			parse_options.allow_default_non_list_visibility = false;
			for (const nlohmann::json& model : *models)
			{
				const auto parsed = uam::acp_models::ParseCodexModelEntry(model, parse_options);
				if (!parsed || !uam::ranges::PushUniqueNonEmptyString(seen_model_ids, parsed->model.id))
				{
					continue;
				}

				models_json.push_back({
				    {"id", parsed->model.id},
				    {"name", parsed->model.name},
				    {"description", parsed->model.description},
				    {"defaultReasoningEffort", parsed->model.default_reasoning_effort},
				    {"supportedReasoningEfforts", parsed->model.supported_reasoning_efforts},
				    {"additionalSpeedTiers", parsed->model.additional_speed_tiers},
				});
			}

			return models_json;
		}

		std::filesystem::path OpenCodeConfigPath()
		{
			if (const std::optional<std::filesystem::path> config_home = uam::env::GetTrimmedPath("XDG_CONFIG_HOME"))
			{
				return *config_home / "opencode" / "opencode.json";
			}

#if defined(_WIN32)
			if (const std::optional<std::filesystem::path> app_data = uam::env::GetTrimmedPath("APPDATA"))
			{
				return *app_data / "opencode" / "opencode.json";
			}
#endif

			if (const std::optional<std::filesystem::path> home = uam::env::GetTrimmedPath("HOME"))
			{
				return *home / ".config" / "opencode" / "opencode.json";
			}

			return uam::paths::CurrentPathOrDot() / ".config" / "opencode" / "opencode.json";
		}

		void PushModelIfNew(nlohmann::json& models_json, std::vector<std::string>& seen_model_ids, const std::string& id, const std::string& name, const std::string& description)
		{
			if (!uam::ranges::PushUniqueNonEmptyString(seen_model_ids, id))
			{
				return;
			}

			models_json.push_back({
			    {"id", id},
			    {"name", uam::strings::NonEmptyOrFallback(name, id)},
			    {"description", description},
			});
		}

		nlohmann::json ReadConfiguredOpenCodeModelsForFrontend()
		{
			auto models_json = nlohmann::json::array();
			const nlohmann::json config = ReadJsonFile(OpenCodeConfigPath());
			if (!config.is_object())
			{
				return models_json;
			}

			const nlohmann::json* providers = uam::nlohmann_json::FindObjectField(config, "provider");
			if (providers == nullptr)
			{
				return models_json;
			}

			std::vector<std::string> seen_model_ids;
			for (const auto& provider_entry : providers->items())
			{
				const std::string provider_id = uam::strings::Trim(provider_entry.key());
				if (provider_id.empty() || !provider_entry.value().is_object())
				{
					continue;
				}

				const nlohmann::json* models = uam::nlohmann_json::FindObjectField(provider_entry.value(), "models");
				if (models == nullptr)
				{
					continue;
				}

				for (const auto& model_entry : models->items())
				{
					const std::string model_id = uam::strings::Trim(model_entry.key());
					if (model_id.empty())
					{
						continue;
					}
					const std::string full_id = provider_id + "/" + model_id;
					std::string name;
					std::string description;
					if (model_entry.value().is_object())
					{
						name = uam::nlohmann_json::TrimmedStringValue(model_entry.value(), {"name", "displayName", "display_name"});
						description = uam::nlohmann_json::TrimmedStringValue(model_entry.value(), {"description"});
					}
					PushModelIfNew(models_json, seen_model_ids, full_id, uam::strings::NonEmptyOrFallback(name, model_id), description);
				}
			}

			return models_json;
		}

		std::string ReadConfiguredOpenCodeDefaultModelForFrontend()
		{
			const nlohmann::json config = ReadJsonFile(OpenCodeConfigPath());
			if (!config.is_object())
			{
				return "";
			}

			return uam::nlohmann_json::TrimmedStringValue(config, {"model"});
		}

		nlohmann::json FallbackAcpModelsForChat(const ChatSession& chat)
		{
			if (uam::provider_ids::IsCliProviderAliasOf(chat.provider_id, uam::provider_ids::kCodexCli))
			{
				return ReadCachedCodexModelsForFrontend();
			}
			if (uam::provider_ids::IsCliProviderAliasOf(chat.provider_id, uam::provider_ids::kOpenCodeCli))
			{
				return ReadConfiguredOpenCodeModelsForFrontend();
			}
			return nlohmann::json::array();
		}

		std::string FallbackAcpCurrentModelForChat(const ChatSession& chat)
		{
			if (!uam::strings::IsBlank(chat.model_id))
			{
				return chat.model_id;
			}
			return uam::provider_ids::IsCliProviderAliasOf(chat.provider_id, uam::provider_ids::kOpenCodeCli) ? ReadConfiguredOpenCodeDefaultModelForFrontend() : std::string{};
		}

		nlohmann::json MergeAcpModelArrays(nlohmann::json fallback_models, nlohmann::json runtime_models)
		{
			if (!fallback_models.is_array())
			{
				fallback_models = nlohmann::json::array();
			}
			if (!runtime_models.is_array())
			{
				return fallback_models;
			}

			std::vector<std::string> seen_model_ids;
			for (const nlohmann::json& model : fallback_models)
			{
				if (model.is_object())
				{
					uam::ranges::PushUniqueNonEmptyString(seen_model_ids, uam::nlohmann_json::TrimmedStringValue(model, {"id"}));
				}
			}
			for (const nlohmann::json& model : runtime_models)
			{
				if (!model.is_object())
				{
					continue;
				}
				const std::string id = uam::nlohmann_json::TrimmedStringValue(model, {"id"});
				if (!uam::ranges::PushUniqueNonEmptyString(seen_model_ids, id))
				{
					continue;
				}
				nlohmann::json normalized_model = model;
				normalized_model["id"] = id;
				fallback_models.push_back(std::move(normalized_model));
			}
			return fallback_models;
		}

		std::string MessageDigestForFingerprint(const ChatSession& session);
		std::size_t MessageCountForFrontend(const ChatSession& session);

		void AddWorkspaceIsolationFields(nlohmann::json& chat_json, const ChatSession& chat)
		{
			chat_json["workspaceIsolationKind"] = chat.workspace_isolation_kind;
			chat_json["workspaceSourceDirectory"] = chat.workspace_source_directory;
			chat_json["workspaceBaseRef"] = chat.workspace_base_ref;
			chat_json["workspaceBranchName"] = chat.workspace_branch_name;
			chat_json["workspaceWorktreeDirectory"] = chat.workspace_worktree_directory;
		}

		void AddSessionSummaryFields(nlohmann::json& session_json, const ChatSession& session, std::string_view workspace_directory)
		{
			session_json["id"] = session.id;
			session_json["title"] = session.title;
			session_json["folderId"] = session.folder_id;
			session_json["pinned"] = session.pinned;
			session_json["providerId"] = session.provider_id;
			session_json["modelId"] = session.model_id;
			session_json["reasoningEffort"] = session.reasoning_effort;
			session_json["serviceTier"] = session.service_tier;
			session_json["approvalMode"] = session.approval_mode;
			session_json["autoApproveCommands"] = session.auto_approve_commands;
			session_json["memoryEnabled"] = session.memory_enabled;
			session_json["memoryLastProcessedMessageCount"] = session.memory_last_processed_message_count;
			session_json["memoryLastProcessedAt"] = session.memory_last_processed_at;
			session_json["workspaceDirectory"] = std::string(workspace_directory);
			AddWorkspaceIsolationFields(session_json, session);
			session_json["createdAt"] = session.created_at;
			session_json["updatedAt"] = session.updated_at;
			session_json["lastOpenedAt"] = uam::strings::NonEmptyOrFallback(session.last_opened_at, session.updated_at);
			session_json["messageCount"] = MessageCountForFrontend(session);
			session_json["messagesDigest"] = MessageDigestForFingerprint(session);
		}

		nlohmann::json SerializeToolCallForFrontend(const ToolCall& tool_call)
		{
			nlohmann::json tool_json;
			tool_json["id"] = tool_call.id;
			tool_json["title"] = tool_call.name;
			tool_json["kind"] = uam::strings::NonEmptyOrFallback(tool_call.name, "tool");
			tool_json["status"] = tool_call.status;
			tool_json["content"] = ToolCallContentForFrontend(tool_call);
			return tool_json;
		}

		template <typename PlanEntry>
		nlohmann::json SerializePlanEntryForFrontend(const PlanEntry& entry)
		{
			return {
			    {"content", entry.content},
			    {"priority", entry.priority},
			    {"status", entry.status},
			};
		}

		nlohmann::json SerializeMessageBlockForFrontend(const MessageBlock& block)
		{
			nlohmann::json block_json;
			block_json["type"] = block.type;
			if (!block.text.empty())
			{
				block_json["text"] = block.text;
			}
			if (!block.tool_call_id.empty())
			{
				block_json["toolCallId"] = block.tool_call_id;
			}
			if (!block.request_id_json.empty())
			{
				block_json["requestId"] = block.request_id_json;
			}
			return block_json;
		}

		nlohmann::json SerializeAttachmentForFrontend(const MessageAttachment& attachment)
		{
			return {
			    {std::string(attachment_fields::kIdField), attachment.id},
			    {std::string(attachment_fields::kNameField), attachment.name},
			    {std::string(attachment_fields::kKindField), attachment.kind},
			    {std::string(attachment_frontend_fields::kMimeTypeField), attachment.mime_type},
			    {std::string(attachment_fields::kPathField), attachment.path},
			    {std::string(attachment_frontend_fields::kSizeBytesField), attachment.size_bytes},
			    {std::string(attachment_fields::kCopiedField), attachment.copied},
			};
		}

		nlohmann::json SerializeMessageForFrontend(const Message& message)
		{
			nlohmann::json message_json;
			message_json["role"] = RoleStr(message.role);
			message_json["content"] = message.content;
			message_json["createdAt"] = message.created_at;
			if (!message.thoughts.empty())
			{
				message_json["thoughts"] = message.thoughts;
			}
			if (!message.plan_summary.empty())
			{
				message_json["planSummary"] = message.plan_summary;
			}
			if (!message.plan_entries.empty())
			{
				nlohmann::json plan_entries = JsonArrayWithCapacity(message.plan_entries.size());
				for (const MessagePlanEntry& entry : message.plan_entries)
				{
					plan_entries.push_back(SerializePlanEntryForFrontend(entry));
				}
				message_json["planEntries"] = std::move(plan_entries);
			}
			if (!message.tool_calls.empty())
			{
				nlohmann::json tool_calls = JsonArrayWithCapacity(message.tool_calls.size());
				for (const ToolCall& tool_call : message.tool_calls)
				{
					tool_calls.push_back(SerializeToolCallForFrontend(tool_call));
				}
				message_json["toolCalls"] = std::move(tool_calls);
			}
			if (!message.blocks.empty())
			{
				nlohmann::json blocks = JsonArrayWithCapacity(message.blocks.size());
				for (const MessageBlock& block : message.blocks)
				{
					if (!block.type.empty())
					{
						blocks.push_back(SerializeMessageBlockForFrontend(block));
					}
				}
				if (!blocks.empty())
				{
					message_json["blocks"] = std::move(blocks);
				}
			}
			if (!message.markdown_store_files.empty())
			{
				nlohmann::json markdown_store_files = JsonArrayWithCapacity(message.markdown_store_files.size());
				for (const std::string& file : message.markdown_store_files)
				{
					markdown_store_files.push_back(file);
				}
				message_json["markdownStoreFiles"] = std::move(markdown_store_files);
			}
			if (!message.attachments.empty())
			{
				nlohmann::json attachments = JsonArrayWithCapacity(message.attachments.size());
				for (const MessageAttachment& attachment : message.attachments)
				{
					if (attachment.path.empty())
					{
						continue;
					}
					attachments.push_back(SerializeAttachmentForFrontend(attachment));
				}
				if (!attachments.empty())
				{
					message_json["attachments"] = std::move(attachments);
				}
			}
			return message_json;
		}

		void FingerprintHashBytes(std::uint64_t& hash, const unsigned char* data, std::size_t len)
		{
			uam::hashing::UpdateFnv1a64(hash, data, len);
		}

		void FingerprintHashString(std::uint64_t& hash, const std::string& value)
		{
			uam::hashing::UpdateFnv1a64WithSeparator(hash, value);
		}

		void FingerprintHashBool(std::uint64_t& hash, bool value)
		{
			const unsigned char byte = value ? 1u : 0u;
			FingerprintHashBytes(hash, &byte, 1);
		}

		std::string FingerprintHashHex(std::uint64_t hash)
		{
			return uam::hashing::Hex64Padded(hash);
		}

		std::string MessageDigestForFingerprint(const ChatSession& session)
		{
			if (!session.messages_loaded)
			{
				if (!session.persisted_messages_digest.empty())
				{
					return session.persisted_messages_digest;
				}
				return session.updated_at + ":" + std::to_string(session.persisted_message_count);
			}

			std::uint64_t hash = uam::hashing::kFnv1a64OffsetBasis;

			FingerprintHashString(hash, session.updated_at);
			FingerprintHashString(hash, std::to_string(session.messages.size()));

			if (!session.messages.empty())
			{
				const Message& last_message = session.messages.back();
				FingerprintHashString(hash, RoleStr(last_message.role));
				FingerprintHashString(hash, last_message.created_at);
				FingerprintHashString(hash, last_message.provider);
				FingerprintHashString(hash, std::to_string(last_message.content.size()));
				FingerprintHashString(hash, std::to_string(last_message.tool_calls.size()));
				for (const ToolCall& tool_call : last_message.tool_calls)
				{
					FingerprintHashString(hash, tool_call.id);
					FingerprintHashString(hash, tool_call.name);
					FingerprintHashString(hash, tool_call.status);
					FingerprintHashString(hash, std::to_string(tool_call.args_json.size()));
					FingerprintHashString(hash, std::to_string(tool_call.result_text.size()));
				}
				FingerprintHashString(hash, std::to_string(last_message.thoughts.size()));
				FingerprintHashString(hash, std::to_string(last_message.plan_summary.size()));
				FingerprintHashString(hash, std::to_string(last_message.plan_entries.size()));
				for (const MessagePlanEntry& entry : last_message.plan_entries)
				{
					FingerprintHashString(hash, entry.content);
					FingerprintHashString(hash, entry.priority);
					FingerprintHashString(hash, entry.status);
				}
				FingerprintHashString(hash, std::to_string(last_message.blocks.size()));
				for (const MessageBlock& block : last_message.blocks)
				{
					FingerprintHashString(hash, block.type);
					FingerprintHashString(hash, block.text);
					FingerprintHashString(hash, block.tool_call_id);
					FingerprintHashString(hash, block.request_id_json);
				}
				FingerprintHashString(hash, std::to_string(last_message.attachments.size()));
				for (const MessageAttachment& attachment : last_message.attachments)
				{
					FingerprintHashString(hash, attachment.id);
					FingerprintHashString(hash, attachment.kind);
					FingerprintHashString(hash, attachment.path);
					FingerprintHashString(hash, std::to_string(attachment.size_bytes));
					FingerprintHashBool(hash, attachment.copied);
				}
				FingerprintHashBool(hash, last_message.interrupted);
			}

			return FingerprintHashHex(hash);
		}

		std::size_t MessageCountForFrontend(const ChatSession& session)
		{
			return session.messages_loaded ? session.messages.size() : session.persisted_message_count;
		}

		const uam::CliTerminalState* FindTerminalForChat(const uam::AppState& app, const ChatSession& chat)
		{
			for (const auto& terminal : app.cli_terminals)
			{
				if (terminal == nullptr)
				{
					continue;
				}

				if (uam::CliTerminalMatchesChat(*terminal, chat))
				{
					return terminal.get();
				}
			}

			return nullptr;
		}

		nlohmann::json SerializeChatTerminalSummary(const AppState& app, const ChatSession& chat)
		{
			const bool ready_since_last_select = ChatHasUnseenUpdate(app, chat);
			const bool has_pending_call = uam::HasPendingCallForChat(app, chat.id);

			if (const CliTerminalState* terminal = FindTerminalForChat(app, chat); terminal != nullptr)
			{
				const bool terminal_processing = uam::CliTerminalLifecycleIsProcessing(*terminal);
				const bool processing = has_pending_call || terminal_processing;
				nlohmann::json terminal_json;
				terminal_json["terminalId"] = terminal->terminal_id;
				terminal_json["frontendChatId"] = terminal->frontend_chat_id;
				terminal_json["sourceChatId"] = uam::CliTerminalPrimaryChatId(*terminal);
				terminal_json["running"] = terminal->running;
				terminal_json["lifecycleState"] = uam::CliTerminalLifecycleStateLabel(*terminal);
				terminal_json["turnState"] = terminal_processing ? "busy" : "idle";
				terminal_json["processing"] = processing;
				terminal_json["readySinceLastSelect"] = ready_since_last_select;
				terminal_json["active"] = uam::CliTerminalLifecycleIsIdleLive(*terminal);
				terminal_json["lastError"] = terminal->last_error;
				return terminal_json;
			}

			const bool processing = has_pending_call;
			nlohmann::json terminal_json;
			terminal_json["running"] = false;
			terminal_json["lifecycleState"] = "stopped";
			terminal_json["turnState"] = "idle";
			terminal_json["processing"] = processing;
			terminal_json["readySinceLastSelect"] = ready_since_last_select;
			terminal_json["active"] = false;
			terminal_json["lastError"] = "";
			return terminal_json;
		}

		nlohmann::json SerializeAcpDiagnostics(const std::vector<AcpDiagnosticEntryState>& diagnostics)
		{
			nlohmann::json diagnostics_json = JsonArrayWithCapacity(diagnostics.size());
			for (const AcpDiagnosticEntryState& diagnostic : diagnostics)
			{
				nlohmann::json diagnostic_json;
				diagnostic_json["time"] = diagnostic.time;
				diagnostic_json["event"] = diagnostic.event;
				diagnostic_json["reason"] = diagnostic.reason;
				diagnostic_json["method"] = diagnostic.method;
				diagnostic_json["requestId"] = diagnostic.request_id;
				diagnostic_json["code"] = uam::nlohmann_json::IntOrNull(diagnostic.has_code, diagnostic.code);
				diagnostic_json["message"] = diagnostic.message;
				diagnostic_json["detail"] = diagnostic.detail;
				diagnostic_json["lifecycleState"] = diagnostic.lifecycle_state;
				diagnostics_json.push_back(std::move(diagnostic_json));
			}
			return diagnostics_json;
		}

		nlohmann::json SerializeAcpToolCalls(const std::vector<AcpToolCallState>& tool_calls)
		{
			nlohmann::json tool_calls_json = JsonArrayWithCapacity(tool_calls.size());
			for (const AcpToolCallState& tool_call : tool_calls)
			{
				tool_calls_json.push_back({
				    {"id", tool_call.id},
				    {"title", tool_call.title},
				    {"kind", tool_call.kind},
				    {"status", tool_call.status},
				    {"content", tool_call.content},
				});
			}
			return tool_calls_json;
		}

		nlohmann::json SerializeAcpPlanEntries(const std::vector<AcpPlanEntryState>& plan_entries)
		{
			nlohmann::json plan_entries_json = JsonArrayWithCapacity(plan_entries.size());
			for (const AcpPlanEntryState& entry : plan_entries)
			{
				plan_entries_json.push_back(SerializePlanEntryForFrontend(entry));
			}
			return plan_entries_json;
		}

		nlohmann::json SerializeAcpModes(const std::vector<AcpModeState>& modes)
		{
			nlohmann::json modes_json = JsonArrayWithCapacity(modes.size());
			for (const AcpModeState& mode : modes)
			{
				modes_json.push_back({
				    {"id", mode.id},
				    {"name", mode.name},
				    {"description", mode.description},
				});
			}
			return modes_json;
		}

		nlohmann::json SerializeAcpModels(const std::vector<AcpModelState>& models)
		{
			nlohmann::json models_json = JsonArrayWithCapacity(models.size());
			for (const AcpModelState& model : models)
			{
				const std::string id = uam::strings::Trim(model.id);
				if (id.empty())
				{
					continue;
				}
				const std::string name = uam::strings::TrimOrFallback(model.name, id);
				models_json.push_back({
				    {"id", id},
				    {"name", name},
				    {"description", uam::strings::Trim(model.description)},
				    {"defaultReasoningEffort", uam::strings::Trim(model.default_reasoning_effort)},
				    {"supportedReasoningEfforts", model.supported_reasoning_efforts},
				    {"additionalSpeedTiers", model.additional_speed_tiers},
				});
			}
			return models_json;
		}

		nlohmann::json SerializeAcpTurnEvents(const std::vector<AcpTurnEventState>& turn_events)
		{
			nlohmann::json turn_events_json = JsonArrayWithCapacity(turn_events.size());
			for (const AcpTurnEventState& event : turn_events)
			{
				nlohmann::json event_json;
				event_json["type"] = event.type;
				if (!event.text.empty())
				{
					event_json["text"] = event.text;
				}
				if (!event.tool_call_id.empty())
				{
					event_json["toolCallId"] = event.tool_call_id;
				}
				if (!event.request_id_json.empty())
				{
					event_json["requestId"] = event.request_id_json;
				}
				turn_events_json.push_back(std::move(event_json));
			}
			return turn_events_json;
		}

		nlohmann::json SerializeAcpPermissionOption(const AcpPermissionOptionState& option)
		{
			return {
			    {"id", option.id},
			    {"name", option.name},
			    {"kind", option.kind},
			};
		}

		nlohmann::json SerializePendingAcpPermission(const AcpPendingPermissionState& permission)
		{
			if (permission.request_id_json.empty())
			{
				return nullptr;
			}

			nlohmann::json permission_json;
			permission_json["requestId"] = permission.request_id_json;
			permission_json["toolCallId"] = permission.tool_call_id;
			permission_json["title"] = permission.title;
			permission_json["kind"] = permission.kind;
			permission_json["status"] = permission.status;
			permission_json["content"] = permission.content;

			nlohmann::json options = JsonArrayWithCapacity(permission.options.size());
			for (const AcpPermissionOptionState& option : permission.options)
			{
				options.push_back(SerializeAcpPermissionOption(option));
			}
			permission_json["options"] = std::move(options);
			return permission_json;
		}

		nlohmann::json SerializeAcpUserInputOption(const AcpUserInputOptionState& option)
		{
			return {
			    {"label", option.label},
			    {"description", option.description},
			};
		}

		nlohmann::json SerializeAcpUserInputQuestion(const AcpUserInputQuestionState& question)
		{
			nlohmann::json question_json;
			question_json["id"] = question.id;
			question_json["header"] = question.header;
			question_json["question"] = question.question;
			question_json["isOther"] = question.is_other;
			question_json["isSecret"] = question.is_secret;

			nlohmann::json options = JsonArrayWithCapacity(question.options.size());
			for (const AcpUserInputOptionState& option : question.options)
			{
				options.push_back(SerializeAcpUserInputOption(option));
			}
			question_json["options"] = std::move(options);
			return question_json;
		}

		nlohmann::json SerializePendingAcpUserInput(const AcpPendingUserInputState& input)
		{
			if (input.request_id_json.empty())
			{
				return nullptr;
			}

			nlohmann::json input_json;
			input_json["requestId"] = input.request_id_json;
			input_json["itemId"] = input.item_id;
			input_json["status"] = input.status;
			input_json["attentionKind"] = NormalizeAcpAttentionKind(input.attention_kind, "question");

			nlohmann::json questions = JsonArrayWithCapacity(input.questions.size());
			for (const AcpUserInputQuestionState& question : input.questions)
			{
				questions.push_back(SerializeAcpUserInputQuestion(question));
			}
			input_json["questions"] = std::move(questions);
			return input_json;
		}

		nlohmann::json SerializeStoppedAcpSessionSummary(const ChatSession& chat, bool ready_since_last_select)
		{
			nlohmann::json acp_json;
			acp_json["sessionId"] = chat.native_session_id;
			acp_json["providerId"] = chat.provider_id;
			acp_json["protocolKind"] = "";
			acp_json["threadId"] = chat.native_session_id;
			acp_json["running"] = false;
			acp_json["processing"] = false;
			acp_json["readySinceLastSelect"] = ready_since_last_select;
			acp_json["attentionKind"] = nullptr;
			acp_json["lifecycleState"] = "stopped";
			acp_json["lastError"] = "";
			acp_json["recentStderr"] = "";
			acp_json["lastExitCode"] = nullptr;
			acp_json["diagnostics"] = nlohmann::json::array();
			acp_json["toolCalls"] = nlohmann::json::array();
			acp_json["planSummary"] = "";
			acp_json["planEntries"] = nlohmann::json::array();
			acp_json["availableModes"] = nlohmann::json::array();
			acp_json["currentModeId"] = chat.approval_mode;
			acp_json["availableModels"] = FallbackAcpModelsForChat(chat);
			acp_json["currentModelId"] = FallbackAcpCurrentModelForChat(chat);
			acp_json["turnEvents"] = nlohmann::json::array();
			acp_json["turnUserMessageIndex"] = -1;
			acp_json["turnAssistantMessageIndex"] = -1;
			acp_json["turnSerial"] = 0;
			acp_json["waitIsStale"] = false;
			acp_json["waitStaleReason"] = "";
			acp_json["waitSeconds"] = 0;
			acp_json["pendingPermission"] = nullptr;
			acp_json["pendingUserInput"] = nullptr;
			return acp_json;
		}

		nlohmann::json SerializeAcpSessionSummary(const AppState& app, const ChatSession& chat)
		{
			nlohmann::json acp_json;
			const AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
			const bool ready_since_last_select = ChatHasUnseenUpdate(app, chat);
			if (session == nullptr)
			{
				return SerializeStoppedAcpSessionSummary(chat, ready_since_last_select);
			}

			acp_json["sessionId"] = session->session_id;
			acp_json["providerId"] = session->provider_id;
			acp_json["protocolKind"] = session->protocol_kind;
			acp_json["threadId"] = session->codex_thread_id.empty() ? session->session_id : session->codex_thread_id;
			acp_json["running"] = session->running;
			acp_json["processing"] = session->processing;
			acp_json["readySinceLastSelect"] = ready_since_last_select;
			const std::string attention_kind = AcpAttentionKindForFrontend(*session);
			acp_json["attentionKind"] = uam::nlohmann_json::StringOrNull(attention_kind);
			acp_json["lifecycleState"] = session->lifecycle_state;
			acp_json["lastError"] = session->last_error;
			acp_json["recentStderr"] = session->recent_stderr;
			acp_json["lastExitCode"] = uam::nlohmann_json::IntOrNull(session->has_last_exit_code, session->last_exit_code);
			acp_json["agentInfo"] = {
			    {"name", session->agent_name},
			    {"title", session->agent_title},
			    {"version", session->agent_version},
			};

			acp_json["diagnostics"] = SerializeAcpDiagnostics(session->diagnostics);
			acp_json["toolCalls"] = SerializeAcpToolCalls(session->tool_calls);
			acp_json["planSummary"] = session->plan_summary;
			acp_json["planEntries"] = SerializeAcpPlanEntries(session->plan_entries);
			acp_json["availableModes"] = SerializeAcpModes(session->available_modes);
			acp_json["currentModeId"] = uam::strings::NonEmptyOrFallback(session->current_mode_id, chat.approval_mode);
			acp_json["availableModels"] = MergeAcpModelArrays(FallbackAcpModelsForChat(chat), SerializeAcpModels(session->available_models));
			acp_json["currentModelId"] = uam::strings::NonEmptyOrFallback(session->current_model_id, FallbackAcpCurrentModelForChat(chat));
			acp_json["turnEvents"] = SerializeAcpTurnEvents(session->turn_events);
			acp_json["turnUserMessageIndex"] = session->turn_user_message_index;
			acp_json["turnAssistantMessageIndex"] = session->turn_assistant_message_index;
			acp_json["turnSerial"] = session->turn_serial;
			acp_json["waitIsStale"] = session->wait_is_stale;
			acp_json["waitStaleReason"] = session->wait_stale_reason;
			acp_json["waitSeconds"] = session->wait_started_time_s > 0.0 ? static_cast<int>(std::max(0.0, GetAppTimeSeconds() - session->wait_started_time_s)) : 0;
			acp_json["pendingPermission"] = SerializePendingAcpPermission(session->pending_permission);
			acp_json["pendingUserInput"] = SerializePendingAcpUserInput(session->pending_user_input);

			return acp_json;
		}

		nlohmann::json SerializeFingerprintSession(const AppState& app, const ChatSession& chat)
		{
			nlohmann::json chat_json;
			AddSessionSummaryFields(chat_json, chat, uam::paths::ResolveWorkspaceRootPath(app, chat).string());
			chat_json["cliTerminal"] = SerializeChatTerminalSummary(app, chat);
			chat_json["acpSession"] = SerializeAcpSessionSummary(app, chat);
			return chat_json;
		}

		nlohmann::json SerializeCliDebugState(const AppState& app)
		{
			nlohmann::json cli_debug;
			cli_debug["selectedChatId"] = CliSelectedChatId(app);
			cli_debug["terminalCount"] = app.cli_terminals.size();

			std::size_t running_count = 0;
			std::size_t busy_count = 0;
			nlohmann::json terminals = JsonArrayWithCapacity(app.cli_terminals.size());

			for (const auto& terminal_ptr : app.cli_terminals)
			{
				if (terminal_ptr == nullptr)
				{
					continue;
				}

				const CliTerminalState& terminal = *terminal_ptr;
				if (terminal.running)
				{
					++running_count;
				}

				if (uam::CliTerminalLifecycleIsProcessing(terminal))
				{
					++busy_count;
				}

				nlohmann::json terminal_json;
				terminal_json["terminalId"] = terminal.terminal_id;
				terminal_json["frontendChatId"] = terminal.frontend_chat_id;
				terminal_json["sourceChatId"] = uam::CliTerminalPrimaryChatId(terminal);
				terminal_json["attachedSessionId"] = terminal.attached_session_id;
				terminal_json["providerId"] = CliProviderIdForDiagnostics(app, terminal);
				terminal_json["nativeSessionId"] = CliNativeSessionIdForDiagnostics(app, terminal);
				terminal_json["processId"] = CliProcessHandleLabel(terminal);
				terminal_json["running"] = terminal.running;
				terminal_json["uiAttached"] = terminal.ui_attached;
				terminal_json["lifecycleState"] = uam::CliTerminalLifecycleStateLabel(terminal);
				terminal_json["turnState"] = CliTurnStateLabel(terminal);
				terminal_json["inputReady"] = terminal.input_ready;
				terminal_json["generationInProgress"] = terminal.generation_in_progress;
				terminal_json["lastUserInputAt"] = terminal.last_user_input_time_s;
				terminal_json["lastAiOutputAt"] = terminal.last_ai_output_time_s;
				terminal_json["lastPolledAt"] = terminal.last_polled_time_s;
				terminal_json["lastError"] = terminal.last_error;
				terminals.push_back(std::move(terminal_json));
			}

			cli_debug["runningTerminalCount"] = running_count;
			cli_debug["busyTerminalCount"] = busy_count;
			cli_debug["terminals"] = std::move(terminals);
			return cli_debug;
		}

		nlohmann::json SerializeMemoryActivity(const AppState& app)
		{
			uam::MemoryActivityState activity = app.memory_activity;
			activity.running_count = static_cast<int>(app.memory_extraction_tasks.size() + app.memory_extraction_queue.size());
			if (!app.memory_last_status.empty())
			{
				activity.last_status = app.memory_last_status;
			}
			nlohmann::json activity_json;
			activity_json["entryCount"] = activity.entry_count;
			activity_json["lastCreatedAt"] = activity.last_created_at;
			activity_json["lastCreatedCount"] = activity.last_created_count;
			activity_json["runningCount"] = activity.running_count;
			activity_json["lastStatus"] = activity.last_status;
			activity_json["lastWorkerChatId"] = activity.last_worker_chat_id;
			activity_json["lastWorkerProviderId"] = activity.last_worker_provider_id;
			activity_json["lastWorkerUpdatedAt"] = activity.last_worker_updated_at;
			activity_json["lastWorkerStatus"] = activity.last_worker_status;
			activity_json["lastWorkerOutput"] = activity.last_worker_output;
			activity_json["lastWorkerError"] = activity.last_worker_error;
			activity_json["lastWorkerTimedOut"] = activity.last_worker_timed_out;
			activity_json["lastWorkerCanceled"] = activity.last_worker_canceled;
			activity_json["lastWorkerHasExitCode"] = activity.last_worker_has_exit_code;
			activity_json["lastWorkerExitCode"] = activity.last_worker_exit_code;
			return activity_json;
		}

		std::string NormalizedCliVersionManagedProviderId(std::string_view raw_provider_id)
		{
			const std::string provider_id = uam::provider_ids::NormalizeCliProviderAlias(raw_provider_id);
			return uam::provider_ids::IsVersionManagedCliProviderId(provider_id) ? provider_id : std::string{};
		}

		std::string NormalizedCliVersionManagedProviderId(const ProviderProfile& profile)
		{
			return profile.supports_cli ? NormalizedCliVersionManagedProviderId(profile.id) : std::string{};
		}

		std::string SelectedCliVersionForFrontend(const CliProviderVersionState& provider_state, const std::string& preferred_version)
		{
			if (!provider_state.selected_version.empty())
			{
				return provider_state.selected_version;
			}
			if (!provider_state.installed_version.empty())
			{
				return provider_state.installed_version;
			}

			return preferred_version;
		}

		nlohmann::json SerializeCliVersionEntry(const AppState& app, const ProviderCliCompatibilityService& service, const std::string& provider_id)
		{
			const bool check_running_for_provider = app.runtime_cli_version_check_task.running &&
			                                        NormalizedCliVersionManagedProviderId(app.runtime_cli_version_provider_id) == provider_id;
			const bool install_running_for_provider = app.runtime_cli_pin_task.running &&
			                                          NormalizedCliVersionManagedProviderId(app.runtime_cli_pin_provider_id) == provider_id;
			const auto state_it = app.runtime_cli_versions_by_provider_id.find(provider_id);
			const bool has_provider_state = state_it != app.runtime_cli_versions_by_provider_id.end();
			const CliProviderVersionState provider_state = has_provider_state ? state_it->second : CliProviderVersionState{};
			const std::string preferred_version = service.PreferredVersionForProvider(provider_id);
			const std::string selected_version = SelectedCliVersionForFrontend(provider_state, preferred_version);

			const std::vector<CliProviderVersionOption> supported_versions = service.SupportedVersionsForProvider(provider_id);
			nlohmann::json versions = JsonArrayWithCapacity(supported_versions.size());
			for (const CliProviderVersionOption& option : supported_versions)
			{
				versions.push_back({
				    {"version", option.version},
				    {"preferred", option.preferred},
				});
			}

			std::string status = "unknown";
			if (check_running_for_provider)
			{
				status = "checking";
			}
			else if (install_running_for_provider)
			{
				status = "installing";
			}
			else if (provider_state.checked)
			{
				status = provider_state.supported ? "supported" : "unsupported";
			}

			nlohmann::json provider_json;
			provider_json["providerId"] = provider_id;
			provider_json["installedVersion"] = provider_state.installed_version;
			provider_json["selectedVersion"] = selected_version;
			provider_json["availableVersions"] = std::move(versions);
			provider_json["preferredVersion"] = preferred_version;
			provider_json["status"] = status;
			provider_json["message"] = provider_state.message;
			provider_json["running"] = check_running_for_provider || install_running_for_provider;
			provider_json["lastCommand"] = install_running_for_provider ? app.runtime_cli_pin_task.command_preview : app.runtime_cli_version_check_task.command_preview;
			provider_json["lastOutput"] = uam::strings::NonEmptyOrFallback(provider_state.install_output, provider_state.raw_output);
			return provider_json;
		}

		nlohmann::json SerializeCliVersionManager(const AppState& app)
		{
			const ProviderCliCompatibilityService service;
			nlohmann::json providers = JsonArrayWithCapacity(app.provider_profiles.size());
			std::unordered_set<std::string> seen_provider_ids;
			for (const ProviderProfile& profile : app.provider_profiles)
			{
				const std::string provider_id = NormalizedCliVersionManagedProviderId(profile);
				if (provider_id.empty() || !seen_provider_ids.insert(provider_id).second)
				{
					continue;
				}
				providers.push_back(SerializeCliVersionEntry(app, service, provider_id));
			}

			return {
			    {"providers", std::move(providers)},
			};
		}

		nlohmann::json SerializeFoldersForFrontend(const std::vector<ChatFolder>& folders)
		{
			nlohmann::json folders_json = JsonArrayWithCapacity(folders.size());
			for (const ChatFolder& folder : folders)
			{
				folders_json.push_back(StateSerializer::SerializeFolder(folder));
			}
			return folders_json;
		}

		nlohmann::json SerializeProvidersForFrontend(const std::vector<ProviderProfile>& profiles)
		{
			nlohmann::json providers_json = JsonArrayWithCapacity(profiles.size());
			for (const ProviderProfile& profile : profiles)
			{
				if (ProviderRuntime::IsRuntimeEnabled(profile))
				{
					providers_json.push_back(StateSerializer::SerializeProvider(profile));
				}
			}
			return providers_json;
		}

	} // anonymous namespace

	// ---------------------------------------------------------------------------
	// StateSerializer implementation
	// ---------------------------------------------------------------------------

	nlohmann::json StateSerializer::Serialize(const AppState& app)
	{
		nlohmann::json j;
		j["stateRevision"] = app.state_revision;

		j["folders"] = SerializeFoldersForFrontend(app.folders);

		// Chat sessions
		nlohmann::json chats_arr = JsonArrayWithCapacity(app.chats.size());
		const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
		const bool has_selected_chat = !selected_chat_id.empty();
		for (const auto& chat : app.chats)
		{
			const bool selected_chat = !has_selected_chat || selected_chat_id == chat.id;
			nlohmann::json chat_json = (selected_chat && chat.messages_loaded) ? SerializeSession(chat) : SerializeFingerprintSession(app, chat);
			chat_json["workspaceDirectory"] = uam::paths::ResolveWorkspaceRootPath(app, chat).string();

			chat_json["cliTerminal"] = SerializeChatTerminalSummary(app, chat);
			chat_json["acpSession"] = SerializeAcpSessionSummary(app, chat);
			chats_arr.push_back(std::move(chat_json));
		}
		j["chats"] = chats_arr;
		j["cliDebug"] = SerializeCliDebugState(app);
		j["memoryActivity"] = SerializeMemoryActivity(app);
		j["cliVersionManager"] = SerializeCliVersionManager(app);

		j["selectedChatId"] = uam::nlohmann_json::StringOrNull(selected_chat_id);

		j["providers"] = SerializeProvidersForFrontend(app.provider_profiles);

		// Settings slice that the UI cares about
		{
			j["settings"] = uam::settings_frontend_json::SerializeLiveSettingsFields(app.settings, app.memory_last_status);
		}

		return j;
	}

	nlohmann::json StateSerializer::SerializeFingerprint(const AppState& app)
	{
		nlohmann::json j;

		j["folders"] = SerializeFoldersForFrontend(app.folders);

		nlohmann::json chats_arr = JsonArrayWithCapacity(app.chats.size());
		for (const auto& chat : app.chats)
		{
			chats_arr.push_back(SerializeFingerprintSession(app, chat));
		}
		j["chats"] = chats_arr;

		j["selectedChatId"] = uam::nlohmann_json::StringOrNull(ChatDomainService().SelectedChatId(app));

		j["providers"] = SerializeProvidersForFrontend(app.provider_profiles);
		j["memoryActivity"] = SerializeMemoryActivity(app);
		j["cliVersionManager"] = SerializeCliVersionManager(app);

		{
			j["settings"] = uam::settings_frontend_json::SerializeFingerprintSettingsFields(app.settings);
		}

		return j;
	}

	nlohmann::json StateSerializer::SerializeSession(const ChatSession& session)
	{
		nlohmann::json j;
		AddSessionSummaryFields(j, session, session.workspace_directory);

		nlohmann::json msgs = JsonArrayWithCapacity(session.messages.size());
		for (const Message& message : session.messages)
		{
			msgs.push_back(SerializeMessageForFrontend(message));
		}
		j["messages"] = msgs;

		return j;
	}

	nlohmann::json StateSerializer::SerializeFolder(const ChatFolder& folder)
	{
		nlohmann::json j;
		j["id"] = folder.id;
		j["title"] = folder.title;
		j["directory"] = folder.directory;
		j["collapsed"] = folder.collapsed;
		return j;
	}

	nlohmann::json StateSerializer::SerializeProvider(const ProviderProfile& profile)
	{
		nlohmann::json j;
		j["id"] = profile.id;
		j["name"] = profile.title;
		j["shortName"] = profile.title; // React derives a short name from this
		j["outputMode"] = profile.output_mode;
		j["supportsCli"] = profile.supports_cli;
		j["supportsStructured"] = profile.supports_structured;
		j["structuredProtocol"] = profile.structured_protocol;
		return j;
	}

} // namespace uam
