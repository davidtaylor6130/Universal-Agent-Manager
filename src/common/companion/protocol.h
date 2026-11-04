#pragma once

#include "common/companion/request_transfer.h"
#include "common/companion/settings_protocol.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <mutex>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace uam::companion
{
	struct Action
	{
		std::string_view name;
		bool chat_scoped;
		bool read_only;
	};

	// clang-format off
	inline constexpr Action kActions[] = {
	    {"getCompanionCapabilities", false, true},
	    {"getCompanionSettings", false, true},
	    {"getMemorySettings", false, true},
	    {"getProviderMcpStatus", false, true},
	    {"setProviderChatDefaults", false, false},
	    {"setMemorySettings", false, false},
	    {"setUamAgentPreferences", false, false},
	    {"setStorageSettings", false, false},
	    {"setVcsSettings", false, false},
	    {"setProviderMcpConfiguration", false, false},
	    {"setProviderAvailability", false, false},
	    {"getInitialState", false, true},
	    {"getChatMessages", true, true},
	    {"getVcsCommitStatus", true, true},
	    {"getVcsFileDiff", true, true},
	    {"getChatRuntimeMetrics", true, true},
	    {"getChatFileChangeReceipts", true, true},
	    {"getToolCallContent", true, true},
	    {"getAttachmentImagePreview", true, true},
	    {"listManagedSubagents", true, true},
	    {"getManagedAgentTranscript", true, true},
	    {"getUsageHistory", false, true},
	    {"getProviderAllowances", false, true},
	    {"listUamAgents", true, true},
	    {"listRemoteDirectories", false, true},
	    {"discoverProviderModels", true, true},
	    {"getCompanionResponseChunk", false, true},
	    {"recordRuntimeActivity", true, false},
	    {"companionCliTerminal", true, false},
	    {"acknowledgeChatAttention", true, false},
	    {"openNativeSessionChat", true, false},
	    {"createSession", false, false},
	    {"renameSession", true, false},
	    {"deleteSession", true, false},
	    {"branchFromMessage", true, false},
	    {"setChatPinned", true, false},
	    {"setChatSnoozedUntil", true, false},
	    {"setChatSettled", true, false},
	    {"setChatModel", true, false},
	    {"setChatProvider", true, false},
	    {"setChatCommandSafetyTier", true, false},
	    {"setChatUamControlEnabled", true, false},
	    {"setChatUamAgent", true, false},
	    {"setChatMemoryEnabled", true, false},
	    {"setChatSmallModelMode", true, false},
	    {"setChatCodexOptions", true, false},
	    {"setChatApprovalMode", true, false},
	    {"setAcpConfigOption", true, false},
	    {"setNativeModelOption", true, false},
	    {"refreshNativeModelOptions", true, false},
	    {"cancelManagedSubagent", true, false},
	    {"submitManagedSubagentFollowup", true, false},
	    {"compactAcpSession", true, false},
	    {"manageQueuedAcpPrompt", true, false},
	    {"stageChatAttachments", true, false},
	    {"sendAcpPrompt", true, false},
	    {"resolveAcpPermission", true, false},
	    {"resolveAcpUserInput", true, false},
	    {"cancelAcpTurn", true, false},
	    {"stopAcpSession", true, false},
	};
	// clang-format on

	inline const Action* FindAction(std::string_view name)
	{
		const auto found = std::find_if(std::begin(kActions), std::end(kActions), [name](const Action& action) { return action.name == name; });
		return found == std::end(kActions) ? nullptr : found;
	}

	// The desktop-owned Phone setting, never the phone, decides whether an allowance refresh may show the macOS Keychain dialog.
	inline void ApplyKeychainPromptPolicy(std::string_view action, nlohmann::json& payload, bool allowed)
	{
		if (action == "getProviderAllowances" && payload.is_object()) payload["allowKeychainPrompt"] = allowed;
	}

	inline bool IsRequestTransferAction(std::string_view action)
	{
		return action == "beginCompanionRequestTransfer" || action == "appendCompanionRequestChunk" || action == "commitCompanionRequestTransfer";
	}

	inline bool NonBlankString(const nlohmann::json& object, std::string_view key, std::size_t maximum = 256)
	{
		const auto value = object.find(std::string(key));
		return value != object.end() && value->is_string() && !value->get_ref<const std::string&>().empty() && value->get_ref<const std::string&>().size() <= maximum && value->get_ref<const std::string&>().find_first_not_of(" \t\r\n") != std::string::npos;
	}

	inline nlohmann::json Pick(const nlohmann::json& source, std::initializer_list<std::string_view> keys)
	{
		nlohmann::json result = nlohmann::json::object();
		for (const std::string_view key : keys)
		{
			const auto field = source.find(std::string(key));
			if (field != source.end())
				result[std::string(key)] = *field;
		}
		return result;
	}

	inline nlohmann::json MobileSettings(const nlohmann::json& settings)
	{
		return SettingsProjection(settings);
	}

	inline nlohmann::json MobileState(nlohmann::json state)
	{
		if (state.contains("settings"))
			state["settings"] = MobileSettings(state["settings"]);
		if (state.contains("providers") && state["providers"].is_array())
			for (auto& provider : state["providers"])
				provider = Pick(provider, {"id", "name", "shortName", "outputMode", "supportsStructured", "supportsCli", "structuredProtocol", "structuredPermissionControl", "terminalPermissionControl", "nativeGoalCommand", "npmPackageName"});
		if (state.contains("memoryActivity"))
		{
			const auto& activity = state["memoryActivity"];
			auto safe = Pick(activity, {"entryCount", "lastCreatedAt", "lastCreatedCount", "runningCount", "lastWorkerChatId", "lastWorkerProviderId", "lastWorkerUpdatedAt", "lastWorkerTimedOut", "lastWorkerCanceled", "lastWorkerHasExitCode", "lastWorkerExitCode"});
			safe["status"] = activity.value("runningCount", 0) > 0 ? "running" : activity.value("lastWorkerTimedOut", false) ? "timedOut" :
			                 activity.value("lastWorkerCanceled", false) ? "cancelled" : activity.value("lastWorkerHasExitCode", false) ?
			                 (activity.value("lastWorkerExitCode", 0) == 0 ? "completed" : "failed") : "unknown";
			state["memoryActivity"] = std::move(safe);
		}
		state.erase("cliDebug");
		state.erase("cliVersionManager");
		if (state.contains("shellActions"))
			state["shellActions"] = nlohmann::json::array();
		state.erase("shellActionNotification");
		return state;
	}

	inline nlohmann::json Capabilities(const nlohmann::json& state)
	{
		nlohmann::json host = {{"id", "local"}, {"label", "Main UAM device"}, {"transport", "local"}};
		const auto settings = state.find("settings");
		if (settings != state.end() && settings->contains("executionHosts") && (*settings)["executionHosts"].is_array())
			for (const auto& candidate : (*settings)["executionHosts"])
				if (candidate.value("id", std::string{}) == "local")
					host.update(Pick(candidate, {"id", "label", "transport", "platform", "architecture"}));
		host["instanceId"] = state.value("bootId", std::string{});
		nlohmann::json actions = nlohmann::json::array();
		for (const Action& action : kActions)
			actions.push_back({{"action", action.name}, {"chatScoped", action.chat_scoped}, {"readOnly", action.read_only}});
		for (const char* action : {"beginCompanionRequestTransfer", "appendCompanionRequestChunk", "commitCompanionRequestTransfer"})
			actions.push_back({{"action", action}, {"chatScoped", false}, {"readOnly", false}});
		nlohmann::json result = {{"protocolVersion", 1}, {"mainHost", host}, {"readOnly", false}, {"actions", actions}};
		result["features"] = {{"revisionPolling", true}, {"responseChunks", true}, {"requestChunks", true}, {"byteAttachmentStaging", true}, {"chatRename", true}, {"chatDelete", true}, {"chatBranch", true}, {"settingsReadOnly", false}};
		result["restrictions"] = {{"settingsReadOnly", false}, {"importedChatsReadOnly", true}, {"desktopSelectionReadOnly", true}, {"desktopTokensUnavailable", true}, {"globalConfigurationReadOnly", false}, {"configurationDefinitionsReadOnly", true}, {"connectionAdministrationReadOnly", true}, {"attachmentPathsRequireStaging", true}, {"automaticReplay", false}};
		result["writableSettingsActions"] = WritableSettingsActions();
		result["unavailableActions"] = {{"deleteSessions", "Bulk deletion requires desktop ownership."}};
		result["limits"] = {{"wireRequestBytes", 65536}, {"directRequestBytes", 60 * 1024}, {"maxRequestBytes", kMaxRequestBytes}, {"requestChunkBytes", kRequestChunkBytes}, {"maxRequestTransfers", kMaxRequestTransfers}, {"requestTransferTotalBytes", kMaxRequestBytes}, {"requestTransferTtlSeconds", kRequestTransferTtl.count()}, {"maxInFlightRequests", kMaxInFlightRequests}, {"inFlightRequestTotalBytes", kMaxRequestBytes}, {"maxResponseBytes", kMaxResponseBytes}, {"maxResponseTransfers", kMaxResponseTransfers}, {"responseTransferTotalBytes", kMaxResponseBytes}, {"responseTransferTtlSeconds", kResponseTransferTtl.count()}};
		return result;
	}

	struct PolicyResult
	{
		int status = 200;
		std::string error;
		bool Ok() const
		{
			return status == 200;
		}
	};

	inline bool QueueInteger(const nlohmann::json& value, int minimum)
	{
		if (!value.is_number_integer()) return false;
		return value.is_number_unsigned() ? value.get<std::uint64_t>() >= static_cast<std::uint64_t>(minimum) && value.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()) :
		    value.get<std::int64_t>() >= minimum && value.get<std::int64_t>() <= std::numeric_limits<int>::max();
	}

	inline bool QueueIdentity(const nlohmann::json& identity)
	{
		return identity.is_object() && NonBlankString(identity, "id") && identity.contains("revision") && QueueInteger(identity["revision"], 1);
	}

	inline PolicyResult ValidateQueueMutation(const nlohmann::json& payload)
	{
		if (!payload.contains("operation") || !payload["operation"].is_string()) return {400, "Invalid queued prompt operation."};
		const auto& operation = payload["operation"].get_ref<const std::string&>();
		if (operation == "remove" || operation == "steer")
		{
			if (!payload.contains("index") || !QueueInteger(payload["index"], 0) || !payload.contains("expected") || !QueueIdentity(payload["expected"]))
				return {400, "Queued prompt requires index and expected id/revision."};
		}
		else if (operation == "edit")
		{
			if (!NonBlankString(payload, "promptId") || !payload.contains("expectedRevision") || !QueueInteger(payload["expectedRevision"], 1))
				return {400, "Queued prompt edit requires promptId and expectedRevision."};
		}
		else if (operation == "reorder")
		{
			if (!payload.contains("expected") || !payload["expected"].is_array() || payload["expected"].size() > 32)
				return {400, "Queued prompt order requires expected identities."};
			for (const auto& identity : payload["expected"])
				if (!QueueIdentity(identity)) return {400, "Invalid queued prompt identity."};
		}
		else if (operation != "discardUnsent") return {400, "Unknown queued prompt operation."};
		return {};
	}

	// Called with the current UI-thread summary immediately before native dispatch. A stable ID
	// may move within the visible queue, but cannot cross into native-owned delivery receipts.
	inline PolicyResult AuthorizeQueueMutation(const nlohmann::json& payload, const nlohmann::json& chat)
	{
		const auto validation = ValidateQueueMutation(payload);
		if (!validation.Ok()) return validation;
		const auto& operation = payload["operation"].get_ref<const std::string&>();
		if (operation == "discardUnsent") return {}; // Existing transcript index/digest guard owns this operation.
		const auto session = chat.find("acpSession");
		if (session == chat.end() || !session->is_object() || !session->contains("queuedPrompts") || !(*session)["queuedPrompts"].is_array())
			return {409, "Queued prompt is no longer visible. Refresh the queue."};
		if (operation == "steer" && session->value("queueHeld", false)) return {409, "Resume the restored queue before steering."};
		const auto& queue = (*session)["queuedPrompts"];
		const auto current_identity = [&](const nlohmann::json& identity)
		{
			std::size_t matches = 0;
			for (const auto& row : queue)
			{
				if (!row.is_object() || !row.contains("id") || row["id"] != identity["id"]) continue;
				if (!QueueIdentity(row) || row["revision"] != identity["revision"] || row.value("prepared", false)) return false;
				++matches;
			}
			return matches == 1;
		};
		if (operation == "reorder")
		{
			if (payload["expected"].size() != queue.size()) return {409, "Queue changed. Refresh before reordering."};
			for (std::size_t index = 0; index < queue.size(); ++index)
				if (!current_identity(payload["expected"][index]) || queue[index]["id"] != payload["expected"][index]["id"])
					return {409, "Queue changed. Refresh before reordering."};
			return {};
		}
		const auto identity = operation == "edit" ? nlohmann::json{{"id", payload["promptId"]}, {"revision", payload["expectedRevision"]}} : payload["expected"];
		return current_identity(identity) ? PolicyResult{} : PolicyResult{409, "Queued prompt changed or left the visible queue. Refresh the queue."};
	}

	inline PolicyResult ValidateTransferPayload(std::string_view action, const nlohmann::json& payload, bool transferred = false)
	{
		if (transferred)
			return {403, "Nested request transfers are unavailable."};
		if (!payload.is_object())
			return {400, "Transfer payload must be an object."};
		if (action == "beginCompanionRequestTransfer")
		{
			if (payload.size() != 1 || !payload.contains("totalBytes") || !payload["totalBytes"].is_number_unsigned())
				return {400, "Transfer requires unsigned totalBytes."};
			const std::uint64_t total = payload["totalBytes"].get<std::uint64_t>();
			return total == 0 || total > kMaxRequestBytes ? PolicyResult{413, "Request transfer exceeds its size bounds."} : PolicyResult{};
		}
		if (!NonBlankString(payload, "id", 128))
			return {400, "Transfer requires a server-issued id."};
		if (action == "appendCompanionRequestChunk")
		{
			if (payload.size() != 3 || !payload.contains("offset") || !payload["offset"].is_number_unsigned() || payload["offset"].get<std::uint64_t>() > kMaxRequestBytes || !payload.contains("base64") || !payload["base64"].is_string())
				return {400, "Chunk requires unsigned offset and base64."};
			return {};
		}
		if (action != "commitCompanionRequestTransfer")
			return {403, "Unknown request transfer action."};
		return payload.size() == 1 ? PolicyResult{} : PolicyResult{400, "Commit requires only id."};
	}

	inline PolicyResult PrepareRequest(std::string_view action, nlohmann::json& payload)
	{
		const Action* policy = FindAction(action);
		if (!policy)
			return {403, "Action unavailable to the companion."};
		if (!payload.is_object())
			return {400, "Companion payload must be an object."};
		if (IsSettingsAction(action))
		{
			const auto validation = ValidateSettingsRequest(action, payload);
			return {validation.status, validation.error};
		}
		if (policy->chat_scoped && !NonBlankString(payload, "chatId"))
			return {400, "An explicit chatId is required."};
		if (action == "getVcsCommitStatus" || action == "getVcsFileDiff")
		{
			if (payload.value("comparisonRef", nlohmann::json{}) != "chat" ||
			    payload.value("vcsType", nlohmann::json("git")) != "git")
				return {403, "Only the saved chat Git comparison is available to the companion."};
			if (action == "getVcsFileDiff")
			{
				if (!NonBlankString(payload, "path", 32768)) return {400, "A relative review file is required."};
				const std::string& path = payload["path"].get_ref<const std::string&>();
				if (path.front() == '/' || path.front() == '\\' || path.find(':') != std::string::npos || path.find('\0') != std::string::npos) return {403, "Review paths must stay within the chat workspace."};
				for (std::size_t start = 0; start < path.size();)
				{
					const std::size_t end = path.find_first_of("/\\", start);
					if (path.substr(start, end == std::string::npos ? end : end - start) == "..") return {403, "Review paths must stay within the chat workspace."};
					if (end == std::string::npos) break;
					start = end + 1;
				}
			}
		}
		if (action == "listManagedSubagents" || action == "getManagedAgentTranscript" ||
		    action == "cancelManagedSubagent" || action == "submitManagedSubagentFollowup" ||
		    action == "setNativeModelOption" || action == "refreshNativeModelOptions")
		{
			const bool listing = action == "listManagedSubagents";
			const bool transcript = action == "getManagedAgentTranscript";
			const bool cancel = action == "cancelManagedSubagent";
			const bool followup = action == "submitManagedSubagentFollowup";
			const bool native_set = action == "setNativeModelOption";
			const bool native_options = native_set || action == "refreshNativeModelOptions";
			for (const auto& [key, value] : payload.items())
			{
				const bool allowed = key == "chatId" ||
				    (listing && (key == "runId" || key == "offset" || key == "limit")) ||
				    (transcript && key == "transcriptChatId") ||
				    ((cancel || followup) && (key == "runId" || key == "controlScopeId")) ||
				    (followup && (key == "afterTurnId" || key == "clientRequestId" || key == "text")) ||
				    (native_options && (key == "nativeSessionId" || key == "executionHostId" || key == "lifetime")) ||
				    (native_set && (key == "optionId" || key == "value"));
				if (!allowed) return {400, "Unknown managed-agent or native-model field."};
				if (value.is_string() && value.get_ref<const std::string&>().find('\0') != std::string::npos)
					return {400, "Managed-agent and native-model fields cannot contain NUL."};
			}
			if (listing)
			{
				if (payload.contains("runId") && (!payload["runId"].is_string() || payload["runId"].get_ref<const std::string&>().size() > 160))
					return {400, "Invalid managed run identity."};
				if ((payload.contains("offset") && (!QueueInteger(payload["offset"], 0) || payload["offset"].get<std::uint64_t>() > 10000)) ||
				    (payload.contains("limit") && (!QueueInteger(payload["limit"], 1) || payload["limit"].get<std::uint64_t>() > 100)))
					return {400, "Invalid bounded managed-agent page."};
			}
			if (transcript && !NonBlankString(payload, "transcriptChatId", 160))
				return {400, "A managed transcript identity is required."};
			if ((cancel || followup) && (!NonBlankString(payload, "runId", 160) || !NonBlankString(payload, "controlScopeId", 160)))
				return {400, "Managed control requires its current run and listing scope."};
			if (followup && (!NonBlankString(payload, "afterTurnId", 160) || !NonBlankString(payload, "clientRequestId", 160) || !NonBlankString(payload, "text", 32 * 1024)))
				return {400, "Managed follow-up requires its current turn, request identity and bounded text."};
			if (native_options)
				for (const char* key : {"nativeSessionId", "executionHostId", "lifetime"})
					if (!NonBlankString(payload, key, 512)) return {400, "Native model options require exact current session ownership."};
			if (native_set && (!NonBlankString(payload, "optionId", 128) || !payload.contains("value") ||
			    (!payload["value"].is_null() && !NonBlankString(payload, "value", 512))))
				return {400, "A native model option requires an exact string choice or Default."};
			// The native handlers retain canonical IDs, current listing capabilities,
			// process/lifetime ownership, policy and save-before-delivery authority.
		}
		if (action == "manageQueuedAcpPrompt")
			return ValidateQueueMutation(payload);
		if (action == "getInitialState")
			payload["summaryOnly"] = true;
		if (action == "createSession" && (payload.contains("sourceChatId") || payload.contains("expectedWorkspaceDirectory") || payload.contains("expectedExecutionHostId")))
			return {403, "Shared-worktree creation requires desktop selection ownership."};
		if (action == "openNativeSessionChat" || action == "createSession" || action == "branchFromMessage")
			payload["selectChat"] = false;
		if (action == "deleteSession" && payload.size() != 1)
			return {400, "Delete requires only chatId."};
		if (action == "branchFromMessage")
		{
			for (const auto& [key, value] : payload.items())
				if (key != "chatId" && key != "messageIndex" && key != "content" && key != "operationId" && key != "retainedAttachmentIds" && key != "retainedMarkdownStoreFiles" && key != "selectChat" && key != "regenerate")
					return {400, "Unknown branch field."};
			if (!payload.contains("messageIndex") || !payload["messageIndex"].is_number_integer() ||
			    (payload["messageIndex"].is_number_unsigned() ? payload["messageIndex"].get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) :
			     payload["messageIndex"].get<std::int64_t>() < 0 || payload["messageIndex"].get<std::int64_t>() > std::numeric_limits<int>::max()))
				return {400, "A valid messageIndex is required."};
			if ((payload.contains("content") && !payload["content"].is_string()) ||
			    (payload.contains("operationId") && !NonBlankString(payload, "operationId", 128)) ||
			    (payload.contains("regenerate") && !payload["regenerate"].is_boolean()))
				return {400, "Invalid branch content, operationId or regenerate."};
			if (payload.contains("retainedAttachmentIds"))
			{
				if (!payload["retainedAttachmentIds"].is_array() || payload["retainedAttachmentIds"].size() > 128)
					return {400, "Invalid retainedAttachmentIds."};
				for (const auto& id : payload["retainedAttachmentIds"])
					if (!id.is_string() || id.get_ref<const std::string&>().empty() || id.get_ref<const std::string&>().size() > 256)
						return {400, "Invalid retained attachment ID."};
			}
			if (payload.contains("retainedMarkdownStoreFiles"))
			{
				const auto& files = payload["retainedMarkdownStoreFiles"];
				if (!files.is_array() || files.size() > 128)
					return {400, "Invalid retained skill selection."};
				for (const auto& file : files)
					if (!file.is_string() || file.get_ref<const std::string&>().empty() || file.get_ref<const std::string&>().size() > 16384 || file.get_ref<const std::string&>().find('\0') != std::string::npos)
						return {400, "Invalid retained skill identity."};
				// These are source-message selections, never new host attachments. The native
				// branch service owns exact captured membership and prompt-snapshot validation.
			}
		}
		if (action == "stageChatAttachments")
		{
			const std::string& chat_id = payload["chatId"].get_ref<const std::string&>();
			if (chat_id.size() > 64 || chat_id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
				return {400, "Attachment staging requires a portable chatId."};
			if (!payload.contains("items") || !payload["items"].is_array() || payload["items"].empty() || payload["items"].size() > 16)
				return {400, "Stage 1 to 16 byte attachments."};
			for (const auto& item : payload["items"])
			{
				if (!item.is_object())
					return {400, "Attachment must be an object."};
				for (const auto& [key, value] : item.items())
					if (key != "name" && key != "kind" && key != "mimeType" && key != "dataBase64")
						return {403, "Companion attachments accept client bytes only, without filesystem paths or ids."};
				if (!NonBlankString(item, "name") || !item.contains("kind") || !item["kind"].is_string() || (item["kind"] != "file" && item["kind"] != "image") || !item.contains("dataBase64") || !item["dataBase64"].is_string() || (item.contains("mimeType") && (!item["mimeType"].is_string() || item["mimeType"].get_ref<const std::string&>().size() > 256)))
					return {400, "Attachments require name, file/image kind and dataBase64."};
				const auto size = Base64DecodedSize(item["dataBase64"].get_ref<const std::string&>());
				if (!size)
					return {400, "Attachment data must be canonical base64."};
				if (*size > 25 * 1024 * 1024)
					return {413, "Attachment exceeds 25 MiB."};
			}
		}
		if (action == "sendAcpPrompt")
		{
			if (payload.contains("markdownStoreFiles") && (!payload["markdownStoreFiles"].is_array() || !payload["markdownStoreFiles"].empty()))
				return {403, "Companion prompts cannot attach host markdown-store paths."};
			if (payload.contains("attachments") && (!payload["attachments"].is_array() || payload["attachments"].size() > 16))
				return {400, "Invalid companion attachments."};
		}
		return {};
	}

	inline PolicyResult AuthorizeChat(std::string_view action, const nlohmann::json& payload, const nlohmann::json& state)
	{
		const Action* policy = FindAction(action);
		if (!policy || !policy->chat_scoped || policy->read_only)
			return {};
		if (!state.contains("chats") || !state["chats"].is_array())
			return {503, "Chat state is unavailable."};
		for (const auto& chat : state["chats"])
			if (chat.value("id", std::string{}) == payload.at("chatId").get<std::string>())
			{
				if (chat.value("importedReadOnly", false)) return {403, "Imported chat is read-only."};
				return action == "manageQueuedAcpPrompt" ? AuthorizeQueueMutation(payload, chat) : PolicyResult{};
			}
		return {404, "Chat not found."};
	}

	inline nlohmann::json ChatAttachmentScope(const nlohmann::json& state, const std::string& chat_id)
	{
		if (state.contains("chats") && state["chats"].is_array())
			for (const auto& chat : state["chats"])
				if (chat.value("id", std::string{}) == chat_id)
					return Pick(chat, {"id", "executionHostId", "workspaceDirectory"});
		return nullptr;
	}

	// Retain only server-issued descriptors, never uploaded bytes, credentials or prompts.
	class StagedAttachments
	{
	  public:
		using Clock = std::chrono::steady_clock;
		bool Remember(const std::string& chat_id, const nlohmann::json& attachments, const nlohmann::json& scope, Clock::time_point now = Clock::now())
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Prune(now);
			if (!scope.is_object() || !attachments.is_array() || attachments.size() > 16 || m_entries.size() + attachments.size() > 256)
				return false;
			std::size_t bytes = 0;
			for (const auto& attachment : attachments)
				bytes += attachment.dump().size() + scope.dump().size() + chat_id.size();
			if (bytes > 64 * 1024 || m_bytes + bytes > 1024 * 1024)
				return false;
			for (const auto& attachment : attachments)
				if (!attachment.is_object() || !NonBlankString(attachment, "id") || !NonBlankString(attachment, "path", 8192) || !attachment.value("copied", false))
					return false;
			for (const auto& attachment : attachments)
				m_entries.push_back({chat_id, attachment, scope, now});
			m_bytes += bytes;
			return true;
		}

		bool Allows(const std::string& chat_id, const nlohmann::json& attachments, const nlohmann::json& scope, Clock::time_point now = Clock::now())
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Prune(now);
			if (!attachments.is_array() || attachments.size() > 16)
				return false;
			for (const auto& attachment : attachments)
				if (std::none_of(m_entries.begin(), m_entries.end(), [&](const Entry& entry) { return entry.chat_id == chat_id && entry.scope == scope && entry.attachment == attachment; }))
					return false;
			return true;
		}

	  private:
		struct Entry
		{
			std::string chat_id;
			nlohmann::json attachment;
			nlohmann::json scope;
			Clock::time_point created;
		};
		void Prune(Clock::time_point now)
		{
			std::erase_if(m_entries, [now](const Entry& entry) { return now - entry.created >= std::chrono::hours(1); });
			m_bytes = 0;
			for (const Entry& entry : m_entries)
				m_bytes += entry.attachment.dump().size() + entry.scope.dump().size() + entry.chat_id.size();
		}
		std::mutex m_mutex;
		std::vector<Entry> m_entries;
		std::size_t m_bytes = 0;
	};
} // namespace uam::companion
