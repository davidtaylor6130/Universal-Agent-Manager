#include "app/agent_run_ledger.h"

#include "common/chat/chat_ids.h"
#include "common/provider/excluded_provider_data.h"
#include "common/config/approval_modes.h"
#include "common/security/command_safety.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/uuid.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace uam
{
	namespace
	{
		constexpr std::uintmax_t kMaxRunFileBytes = 512U * 1024U;
		constexpr std::uintmax_t kMaxLedgerLoadBytes = 64U * 1024U * 1024U;
		constexpr std::size_t kMaxRunFiles = 10000;
		constexpr std::size_t kMaxLoadErrors = 128;
		constexpr std::size_t kMaxSnapshotBytes = 256U * 1024U;
		constexpr std::size_t kMaxTaskBytes = 128U * 1024U;
		constexpr std::size_t kMaxResultBytes = 16U * 1024U;
		constexpr std::size_t kMaxDiagnosticBytes = 4096;
		constexpr std::size_t kMaxIdBytes = 160;

		const std::unordered_set<std::string> kStatuses = {
		    "queued", "running", "completed", "failed", "cancelled", "interrupted"};

		std::mutex& LedgerWriteMutex()
		{
			static std::mutex mutex;
			return mutex;
		}

		void AddLoadError(AgentRunLoadResult& result, std::string error)
		{
			if (result.errors.size() < kMaxLoadErrors) result.errors.push_back(std::move(error));
			else if (result.errors.size() == kMaxLoadErrors) result.errors.push_back("Additional agent-run errors were omitted.");
		}

		std::filesystem::path RunsRoot(const std::filesystem::path& data_root)
		{
			return data_root / "agent-runs";
		}

		std::filesystem::path RunPath(const std::filesystem::path& data_root, std::string_view id)
		{
			return RunsRoot(data_root) / (std::string(id) + ".json");
		}

		bool SafeText(std::string_view value, std::size_t max_bytes)
		{
			return value.size() <= max_bytes && value.find('\0') == std::string_view::npos;
		}

		bool SafeReference(std::string_view value, bool allow_empty = true)
		{
			return (allow_empty && value.empty()) ||
			       (value.size() <= kMaxIdBytes && uam::chat_ids::IsSafeStorageChatId(value));
		}

		bool ExactFields(const nlohmann::json& value, std::initializer_list<const char*> keys)
		{
			return value.is_object() && value.size() == keys.size() &&
			    std::ranges::all_of(keys, [&](const char* key) { return value.contains(key); });
		}

		std::string ReadManagedString(const nlohmann::json& value)
		{
			if (!value.is_string()) throw std::invalid_argument("managed string type");
			return value.get<std::string>();
		}

		bool ReadManagedBool(const nlohmann::json& value)
		{
			if (!value.is_boolean()) throw std::invalid_argument("managed bool type");
			return value.get<bool>();
		}

		template <typename T> T ReadManagedInteger(const nlohmann::json& value, T minimum, T maximum)
		{
			if (!value.is_number_integer()) throw std::invalid_argument("managed integer type");
			if (value.is_number_unsigned())
			{
				const auto number = value.get<std::uint64_t>();
				if (number > static_cast<std::uint64_t>(maximum) ||
				    (minimum > 0 && number < static_cast<std::uint64_t>(minimum)))
					throw std::invalid_argument("managed integer range");
				return static_cast<T>(number);
			}
			const auto number = value.get<std::int64_t>();
			if (number < static_cast<std::int64_t>(minimum) ||
			    (number >= 0 && static_cast<std::uint64_t>(number) > static_cast<std::uint64_t>(maximum)))
				throw std::invalid_argument("managed integer range");
			return static_cast<T>(number);
		}

		std::string ManagedRunTurnStatusName(ManagedRunTurnStatus value)
		{
			switch (value)
			{
				case ManagedRunTurnStatus::Queued: return "queued";
				case ManagedRunTurnStatus::Running: return "running";
				case ManagedRunTurnStatus::Completed: return "completed";
				case ManagedRunTurnStatus::Failed: return "failed";
				case ManagedRunTurnStatus::Cancelled: return "cancelled";
				case ManagedRunTurnStatus::Interrupted: return "interrupted";
			}
			return {};
		}

		ManagedRunTurnStatus ReadManagedRunTurnStatus(const nlohmann::json& value)
		{
			const std::string text = ReadManagedString(value);
			if (text == "queued") return ManagedRunTurnStatus::Queued;
			if (text == "running") return ManagedRunTurnStatus::Running;
			if (text == "completed") return ManagedRunTurnStatus::Completed;
			if (text == "failed") return ManagedRunTurnStatus::Failed;
			if (text == "cancelled") return ManagedRunTurnStatus::Cancelled;
			if (text == "interrupted") return ManagedRunTurnStatus::Interrupted;
			throw std::invalid_argument("managed enum value");
		}

		std::string ManagedRunPrincipalKindName(ManagedRunPrincipalKind value)
		{
			switch (value)
			{
				case ManagedRunPrincipalKind::UserRoot: return "user-root";
				case ManagedRunPrincipalKind::AgentRoot: return "agent-root";
				case ManagedRunPrincipalKind::AgentRun: return "agent-run";
			}
			return {};
		}

		ManagedRunPrincipalKind ReadManagedRunPrincipalKind(const nlohmann::json& value)
		{
			const std::string text = ReadManagedString(value);
			if (text == "user-root") return ManagedRunPrincipalKind::UserRoot;
			if (text == "agent-root") return ManagedRunPrincipalKind::AgentRoot;
			if (text == "agent-run") return ManagedRunPrincipalKind::AgentRun;
			throw std::invalid_argument("managed enum value");
		}

		std::string ManagedRunContinuityName(ManagedRunContinuity value)
		{
			switch (value)
			{
				case ManagedRunContinuity::Native: return "native";
			}
			return {};
		}

		ManagedRunContinuity ReadManagedRunContinuity(const nlohmann::json& value)
		{
			const std::string text = ReadManagedString(value);
			if (text == "native") return ManagedRunContinuity::Native;
			throw std::invalid_argument("managed enum value");
		}

		std::string ManagedRunRecoveryDecisionName(ManagedRunRecoveryDecision value)
		{
			switch (value)
			{
				case ManagedRunRecoveryDecision::None: return "none";
				case ManagedRunRecoveryDecision::ContinueWithoutReplay: return "continue-without-replay";
			}
			return {};
		}

		ManagedRunRecoveryDecision ReadManagedRunRecoveryDecision(const nlohmann::json& value)
		{
			const std::string text = ReadManagedString(value);
			if (text == "none") return ManagedRunRecoveryDecision::None;
			if (text == "continue-without-replay") return ManagedRunRecoveryDecision::ContinueWithoutReplay;
			throw std::invalid_argument("managed enum value");
		}

		nlohmann::json ManagedRunPrincipalToJson(const ManagedRunPrincipal& value)
		{
			nlohmann::json result = nlohmann::json::object();
			result["kind"] = ManagedRunPrincipalKindName(value.kind);
			result["rootChatId"] = value.root_chat_id;
			result["rootBirthId"] = value.root_birth_id;
			result["parentRunId"] = value.parent_run_id;
			result["parentTranscriptBirthId"] = value.parent_transcript_birth_id;
			return result;
		}

		ManagedRunPrincipal ReadManagedRunPrincipal(const nlohmann::json& value)
		{
			if (!ExactFields(value, {"kind", "rootChatId", "rootBirthId", "parentRunId", "parentTranscriptBirthId"}))
				throw std::invalid_argument("managed object fields");
			ManagedRunPrincipal result;
			result.kind = ReadManagedRunPrincipalKind(value.at("kind"));
			result.root_chat_id = ReadManagedString(value.at("rootChatId"));
			result.root_birth_id = ReadManagedString(value.at("rootBirthId"));
			result.parent_run_id = ReadManagedString(value.at("parentRunId"));
			result.parent_transcript_birth_id = ReadManagedString(value.at("parentTranscriptBirthId"));
			return result;
		}

		nlohmann::json ManagedRunOwnerBindingToJson(const ManagedRunOwnerBinding& value)
		{
			nlohmann::json result = nlohmann::json::object();
			result["rootBirthId"] = value.root_birth_id;
			result["transcriptBirthId"] = value.transcript_birth_id;
			result["delegationGrant"] = value.delegation_grant ? ManagedRunPrincipalToJson(*value.delegation_grant) : nlohmann::json(nullptr);
			return result;
		}

		ManagedRunOwnerBinding ReadManagedRunOwnerBinding(const nlohmann::json& value)
		{
			if (!ExactFields(value, {"rootBirthId", "transcriptBirthId", "delegationGrant"}))
				throw std::invalid_argument("managed object fields");
			ManagedRunOwnerBinding result;
			result.root_birth_id = ReadManagedString(value.at("rootBirthId"));
			result.transcript_birth_id = ReadManagedString(value.at("transcriptBirthId"));
			if (!value.at("delegationGrant").is_null()) result.delegation_grant = ReadManagedRunPrincipal(value.at("delegationGrant"));
			return result;
		}

		nlohmann::json ManagedRunExecutionBindingToJson(const ManagedRunExecutionBinding& value)
		{
			nlohmann::json result = nlohmann::json::object();
			result["connectionConfigurationJson"] = value.connection_configuration_json;
			result["providerConfigurationJson"] = value.provider_configuration_json;
			result["executionHostId"] = value.execution_host_id;
			result["workspaceDirectory"] = value.workspace_directory;
			result["workspaceWorktreeDirectory"] = value.workspace_worktree_directory;
			result["workspaceSourceDirectory"] = value.workspace_source_directory;
			result["routingMode"] = value.routing_mode;
			result["routingSourceFolderId"] = value.routing_source_folder_id;
			result["routingSourceDirectory"] = value.routing_source_directory;
			result["effectiveModelId"] = value.effective_model_id;
			result["modelIdentitySource"] = value.model_identity_source;
			result["reasoningEffort"] = value.reasoning_effort;
			result["serviceTier"] = value.service_tier;
			result["serviceTierExplicit"] = value.service_tier_explicit;
			result["approvalMode"] = value.approval_mode;
			result["commandSafetyTier"] = value.command_safety_tier;
			result["nativeSessionId"] = value.native_session_id;
			return result;
		}

		ManagedRunExecutionBinding ReadManagedRunExecutionBinding(const nlohmann::json& value)
		{
			if (!ExactFields(value, {"connectionConfigurationJson", "providerConfigurationJson", "executionHostId", "workspaceDirectory", "workspaceWorktreeDirectory", "workspaceSourceDirectory", "routingMode", "routingSourceFolderId", "routingSourceDirectory", "effectiveModelId", "modelIdentitySource", "reasoningEffort", "serviceTier", "serviceTierExplicit", "approvalMode", "commandSafetyTier", "nativeSessionId"}))
				throw std::invalid_argument("managed object fields");
			ManagedRunExecutionBinding result;
			result.connection_configuration_json = ReadManagedString(value.at("connectionConfigurationJson"));
			result.provider_configuration_json = ReadManagedString(value.at("providerConfigurationJson"));
			result.execution_host_id = ReadManagedString(value.at("executionHostId"));
			result.workspace_directory = ReadManagedString(value.at("workspaceDirectory"));
			result.workspace_worktree_directory = ReadManagedString(value.at("workspaceWorktreeDirectory"));
			result.workspace_source_directory = ReadManagedString(value.at("workspaceSourceDirectory"));
			result.routing_mode = ReadManagedString(value.at("routingMode"));
			result.routing_source_folder_id = ReadManagedString(value.at("routingSourceFolderId"));
			result.routing_source_directory = ReadManagedString(value.at("routingSourceDirectory"));
			result.effective_model_id = ReadManagedString(value.at("effectiveModelId"));
			result.model_identity_source = ReadManagedString(value.at("modelIdentitySource"));
			result.reasoning_effort = ReadManagedString(value.at("reasoningEffort"));
			result.service_tier = ReadManagedString(value.at("serviceTier"));
			result.service_tier_explicit = ReadManagedBool(value.at("serviceTierExplicit"));
			result.approval_mode = ReadManagedString(value.at("approvalMode"));
			result.command_safety_tier = ReadManagedString(value.at("commandSafetyTier"));
			result.native_session_id = ReadManagedString(value.at("nativeSessionId"));
			return result;
		}

		nlohmann::json ManagedRunTurnReceiptToJson(const ManagedRunTurnReceipt& value)
		{
			nlohmann::json result = nlohmann::json::object();
			result["turnId"] = value.turn_id;
			result["turnNumber"] = value.turn_number;
			result["clientRequestId"] = value.client_request_id;
			result["afterTurnId"] = value.after_turn_id;
			result["principal"] = ManagedRunPrincipalToJson(value.principal);
			result["text"] = value.text;
			result["continuity"] = ManagedRunContinuityName(value.continuity);
			result["recoveryDecision"] = ManagedRunRecoveryDecisionName(value.recovery_decision);
			result["retryOfTurnId"] = value.retry_of_turn_id;
			result["status"] = ManagedRunTurnStatusName(value.status);
			result["deliveryId"] = value.delivery_id;
			result["dispatchAttempted"] = value.dispatch_attempted;
			result["expectedTurnSerial"] = value.expected_turn_serial;
			result["firstUserMessageIndex"] = value.first_user_message_index;
			result["userMessageCreatedAt"] = value.user_message_created_at;
			result["startedAtEpochMs"] = value.started_at_epoch_ms;
			result["finishedAtEpochMs"] = value.finished_at_epoch_ms;
			result["resultRequired"] = value.result_required;
			result["resultDelivered"] = value.result_delivered;
			result["resultDeliveryAttempts"] = value.result_delivery_attempts;
			result["auditPending"] = value.audit_pending;
			result["diagnosticCode"] = value.diagnostic_code;
			return result;
		}

		ManagedRunTurnReceipt ReadManagedRunTurnReceipt(const nlohmann::json& value)
		{
			if (!ExactFields(value, {"turnId", "turnNumber", "clientRequestId", "afterTurnId", "principal", "text", "continuity", "recoveryDecision", "retryOfTurnId", "status", "deliveryId", "dispatchAttempted", "expectedTurnSerial", "firstUserMessageIndex", "userMessageCreatedAt", "startedAtEpochMs", "finishedAtEpochMs", "resultRequired", "resultDelivered", "resultDeliveryAttempts", "auditPending", "diagnosticCode"}))
				throw std::invalid_argument("managed object fields");
			ManagedRunTurnReceipt result;
			result.turn_id = ReadManagedString(value.at("turnId"));
			result.turn_number = ReadManagedInteger<std::uint32_t>(value.at("turnNumber"), 1, std::numeric_limits<std::uint32_t>::max());
			result.client_request_id = ReadManagedString(value.at("clientRequestId"));
			result.after_turn_id = ReadManagedString(value.at("afterTurnId"));
			result.principal = ReadManagedRunPrincipal(value.at("principal"));
			result.text = ReadManagedString(value.at("text"));
			result.continuity = ReadManagedRunContinuity(value.at("continuity"));
			result.recovery_decision = ReadManagedRunRecoveryDecision(value.at("recoveryDecision"));
			result.retry_of_turn_id = ReadManagedString(value.at("retryOfTurnId"));
			result.status = ReadManagedRunTurnStatus(value.at("status"));
			result.delivery_id = ReadManagedString(value.at("deliveryId"));
			result.dispatch_attempted = ReadManagedBool(value.at("dispatchAttempted"));
			result.expected_turn_serial = ReadManagedInteger<int>(value.at("expectedTurnSerial"), 0, std::numeric_limits<int>::max());
			result.first_user_message_index = ReadManagedInteger<int>(value.at("firstUserMessageIndex"), -1, std::numeric_limits<int>::max());
			result.user_message_created_at = ReadManagedString(value.at("userMessageCreatedAt"));
			result.started_at_epoch_ms = ReadManagedInteger<int64_t>(value.at("startedAtEpochMs"), 0, std::numeric_limits<int64_t>::max());
			result.finished_at_epoch_ms = ReadManagedInteger<int64_t>(value.at("finishedAtEpochMs"), 0, std::numeric_limits<int64_t>::max());
			result.result_required = ReadManagedBool(value.at("resultRequired"));
			result.result_delivered = ReadManagedBool(value.at("resultDelivered"));
			result.result_delivery_attempts = ReadManagedInteger<int>(value.at("resultDeliveryAttempts"), 0, 3);
			result.audit_pending = ReadManagedBool(value.at("auditPending"));
			result.diagnostic_code = ReadManagedString(value.at("diagnosticCode"));
			return result;
		}

		nlohmann::json ManagedRunContinuationToJson(const ManagedRunContinuation& value)
		{
			nlohmann::json result = nlohmann::json::object();
			result["owner"] = ManagedRunOwnerBindingToJson(value.owner);
			result["execution"] = value.execution ? ManagedRunExecutionBindingToJson(*value.execution) : nlohmann::json(nullptr);
			result["currentTurnId"] = value.current_turn_id;
			result["currentTurnNumber"] = value.current_turn_number;
			result["stateRevision"] = value.state_revision;
			result["turns"] = nlohmann::json::array();
			for (const auto& turn : value.turns) result["turns"].push_back(ManagedRunTurnReceiptToJson(turn));
			return result;
		}

		ManagedRunContinuation ReadManagedRunContinuation(const nlohmann::json& value)
		{
			if (!ExactFields(value, {"owner", "execution", "currentTurnId", "currentTurnNumber", "stateRevision", "turns"}))
				throw std::invalid_argument("managed object fields");
			ManagedRunContinuation result;
			result.owner = ReadManagedRunOwnerBinding(value.at("owner"));
			if (!value.at("execution").is_null()) result.execution = ReadManagedRunExecutionBinding(value.at("execution"));
			result.current_turn_id = ReadManagedString(value.at("currentTurnId"));
			result.current_turn_number = ReadManagedInteger<std::uint32_t>(value.at("currentTurnNumber"), 1, std::numeric_limits<std::uint32_t>::max());
			result.state_revision = ReadManagedInteger<std::uint64_t>(value.at("stateRevision"), 1, std::numeric_limits<std::uint64_t>::max() - 1);
			if (!value.at("turns").is_array() || value.at("turns").empty() || value.at("turns").size() > 33) throw std::invalid_argument("managed turn count");
			for (const auto& turn : value.at("turns")) result.turns.push_back(ReadManagedRunTurnReceipt(turn));
			return result;
		}

		bool CanonicalManagedConfiguration(const std::string& text)
		{
			if (!SafeText(text, 64U * 1024U) || text.empty()) return false;
			const auto value = nlohmann::json::parse(text, nullptr, false);
			return (value.is_object() || value.is_array()) && value.dump() == text;
		}

		bool ValidManagedPrincipal(const ManagedRunPrincipal& value, const AgentRun& run,
		    const ManagedRunOwnerBinding& owner)
		{
			if (value.root_chat_id != run.root_chat_id || value.root_birth_id != owner.root_birth_id ||
			    ManagedRunPrincipalKindName(value.kind).empty()) return false;
			if (value.kind == ManagedRunPrincipalKind::AgentRun)
				return uuid::IsCanonicalUuid(value.parent_run_id) && value.parent_run_id == run.parent_run_id &&
				    uuid::IsCanonicalUuid(value.parent_transcript_birth_id);
			return value.parent_run_id.empty() && value.parent_transcript_birth_id.empty();
		}

		bool ValidateManagedContinuation(const AgentRun& run)
		{
			if (!run.managed_continuation) return true;
			const auto& value = *run.managed_continuation;
			if (!uuid::IsCanonicalUuid(value.owner.root_birth_id) || !uuid::IsCanonicalUuid(value.owner.transcript_birth_id) ||
			    run.transcript_chat_id.empty() || run.transcript_chat_id == run.root_chat_id ||
			    value.turns.empty() || value.turns.size() > 33 || value.current_turn_number != value.turns.size() ||
			    value.current_turn_id != value.turns.back().turn_id || value.state_revision < value.current_turn_number ||
			    value.state_revision == std::numeric_limits<std::uint64_t>::max()) return false;
			if (value.owner.delegation_grant &&
			    (value.owner.delegation_grant->kind == ManagedRunPrincipalKind::UserRoot ||
			     !ValidManagedPrincipal(*value.owner.delegation_grant, run, value.owner))) return false;
			if (value.turns.size() > 1 && (!value.execution || value.execution->native_session_id.empty() ||
			    value.execution->effective_model_id.empty() || value.execution->model_identity_source.empty())) return false;
			if (value.execution)
			{
				const auto& execution = *value.execution;
				if (!CanonicalManagedConfiguration(execution.connection_configuration_json) ||
				    !CanonicalManagedConfiguration(execution.provider_configuration_json) ||
				    !SafeReference(execution.execution_host_id, false) ||
				    !SafeReference(execution.routing_source_folder_id) ||
				    (execution.routing_mode != "explicit" && execution.routing_mode != "automatic") ||
				    !SafeText(execution.effective_model_id, 160) ||
				    (execution.model_identity_source != "" && execution.model_identity_source != "exact-requested" &&
				     execution.model_identity_source != "advertised-resolution") ||
				    !SafeText(execution.native_session_id, 256) || !SafeText(execution.reasoning_effort, 256) ||
				    !SafeText(execution.service_tier, 256) ||
				    (!execution.approval_mode.empty() && !approval_modes::IsAppApprovalMode(execution.approval_mode)) ||
				    command_safety::NormalizeTier(execution.command_safety_tier) != execution.command_safety_tier) return false;
				for (const auto* path : {&execution.workspace_directory, &execution.workspace_worktree_directory,
				    &execution.workspace_source_directory, &execution.routing_source_directory})
					if (!SafeText(*path, 16U * 1024U)) return false;
			}
			std::unordered_set<std::string> turns, requests, deliveries;
			std::size_t text_bytes = 0;
			for (std::size_t index = 0; index < value.turns.size(); ++index)
			{
				const auto& turn = value.turns[index];
				if (!uuid::IsCanonicalUuid(turn.turn_id) || !uuid::IsCanonicalUuid(turn.delivery_id) ||
				    !turns.insert(turn.turn_id).second || !deliveries.insert(turn.delivery_id).second ||
				    turn.turn_number != index + 1 || !ValidManagedPrincipal(turn.principal, run, value.owner) ||
				    ManagedRunTurnStatusName(turn.status).empty() || turn.continuity != ManagedRunContinuity::Native ||
				    ManagedRunRecoveryDecisionName(turn.recovery_decision).empty() || turn.expected_turn_serial < 0 ||
				    turn.first_user_message_index < -1 || !SafeText(turn.user_message_created_at, 256) ||
				    ((turn.first_user_message_index == -1) != turn.user_message_created_at.empty()) ||
				    turn.started_at_epoch_ms < 0 || turn.finished_at_epoch_ms < 0 ||
				    (turn.finished_at_epoch_ms != 0 && turn.finished_at_epoch_ms < turn.started_at_epoch_ms) ||
				    turn.result_delivery_attempts < 0 || turn.result_delivery_attempts > 3 ||
				    (turn.result_delivered && !turn.result_required) || !SafeText(turn.diagnostic_code, 80)) return false;
				if ((turn.status == ManagedRunTurnStatus::Queued || turn.status == ManagedRunTurnStatus::Running) &&
				    index + 1 != value.turns.size()) return false;
				if (turn.status == ManagedRunTurnStatus::Running && !turn.dispatch_attempted) return false;
				if (index == 0)
				{
					if (turn.turn_id != run.id || turn.delivery_id != run.id || !turn.client_request_id.empty() ||
					    !turn.after_turn_id.empty() || !turn.text.empty() || !turn.retry_of_turn_id.empty() ||
					    turn.recovery_decision != ManagedRunRecoveryDecision::None ||
					    turn.result_required != run.deliver_result_to_root_chat ||
					    turn.result_delivered != run.root_result_delivered ||
					    turn.result_delivery_attempts != run.root_result_delivery_attempts) return false;
				}
				else
				{
					if (!uuid::IsCanonicalUuid(turn.client_request_id) || !requests.insert(turn.client_request_id).second ||
					    turn.after_turn_id != value.turns[index - 1].turn_id || !SafeText(turn.text, 32U * 1024U) ||
					    strings::IsBlank(turn.text) || !turn.result_required || turn.delivery_id != turn.turn_id ||
					    (!turn.retry_of_turn_id.empty() && (turn.retry_of_turn_id == turn.turn_id || !turns.contains(turn.retry_of_turn_id)))) return false;
					text_bytes += turn.text.size();
					if (text_bytes > 128U * 1024U) return false;
				}
			}
			const auto& current = value.turns.back();
			return run.status == ManagedRunTurnStatusName(current.status) && run.expected_turn_serial == current.expected_turn_serial &&
			    run.started_at_epoch_ms == current.started_at_epoch_ms && run.finished_at_epoch_ms == current.finished_at_epoch_ms;
		}

		bool RecognizableManagedRecord(std::string_view text)
		{
			const auto value = nlohmann::json::parse(text, nullptr, false);
			return value.is_object() && (value.contains("managedContinuation") ||
			    (value.contains("version") && value["version"] == 2));
		}

		bool Validate(const AgentRun& run, std::string* error_out)
		{
			const bool valid = uam::uuid::IsCanonicalUuid(run.id) &&
			                   SafeReference(run.root_chat_id, false) && SafeReference(run.transcript_chat_id) &&
			                   SafeReference(run.goal_id) &&
			                   (run.parent_run_id.empty() || uam::uuid::IsCanonicalUuid(run.parent_run_id)) &&
			                   (run.resumed_from_run_id.empty() || uam::uuid::IsCanonicalUuid(run.resumed_from_run_id)) &&
			                   SafeReference(run.agent_id, false) && SafeReference(run.provider_id, false) &&
			                   SafeText(run.model_id, kMaxIdBytes) && SafeText(run.definition_hash, kMaxIdBytes) &&
			                   SafeText(run.definition_snapshot, kMaxSnapshotBytes) && !run.definition_snapshot.empty() &&
			                   SafeText(run.definition_instructions, kMaxSnapshotBytes) &&
			                   run.skills_snapshot.size() <= 16 && run.delegates_snapshot.size() <= 16 &&
			                   std::ranges::all_of(run.skills_snapshot, [](const std::string& id) { return SafeReference(id, false); }) &&
			                   std::ranges::all_of(run.delegates_snapshot, [](const std::string& id) { return SafeReference(id, false); }) &&
			                   SafeText(run.execution_capability, 80) &&
			                   SafeText(run.task, kMaxTaskBytes) && !run.task.empty() &&
			                   (run.effective_workspace_access == "read" || run.effective_workspace_access == "write") &&
			                   kStatuses.contains(run.status) && run.depth >= 1 && run.depth <= 2 &&
			                   run.expected_turn_serial >= 0 && run.started_at_epoch_ms >= 0 && run.finished_at_epoch_ms >= 0 &&
			                   run.deadline_at_epoch_ms >= run.started_at_epoch_ms &&
			                   (run.result_delivery_id.empty() || run.result_delivery_id == run.id) &&
			                   run.root_result_delivery_attempts >= 0 &&
			                   run.root_result_delivery_attempts <= 3 &&
			                   (!run.root_result_delivered || run.deliver_result_to_root_chat) &&
			                   !run.created_at.empty() && !run.updated_at.empty() &&
			                   SafeText(run.created_at, 80) && SafeText(run.started_at, 80) &&
			                   SafeText(run.finished_at, 80) && SafeText(run.updated_at, 80) &&
			                   SafeText(run.result_excerpt, kMaxResultBytes) &&
			                   SafeText(run.diagnostic_code, 80) && SafeText(run.diagnostic, kMaxDiagnosticBytes) &&
		                   ValidateManagedContinuation(run);
			if (!valid && error_out != nullptr) *error_out = "Agent run contains invalid or oversized fields.";
			return valid;
		}

		std::string JsonString(const nlohmann::json& object, std::string_view key)
		{
			const auto found = object.find(key);
			return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
		}

		std::optional<AgentRun> Parse(std::string_view text, std::string_view expected_id,
		                              std::string* error_out)
		try
		{
			const nlohmann::json value = nlohmann::json::parse(text, nullptr, false);
			const auto deliver_result = value.find("deliverResultToRootChat");
			const auto result_delivered = value.find("rootResultDelivered");
			const auto delivery_attempts = value.find("rootResultDeliveryAttempts");
			if (!value.is_object() || !value.contains("version") || !value["version"].is_number_integer() ||
			    (value["version"] != 1 && value["version"] != 2) || !value.contains("depth") || !value["depth"].is_number_integer() ||
			    !value.contains("expectedTurnSerial") || !value["expectedTurnSerial"].is_number_integer() ||
			    (deliver_result != value.end() && !deliver_result->is_boolean()) ||
			    (result_delivered != value.end() && !result_delivered->is_boolean()) ||
			    (delivery_attempts != value.end() && (!delivery_attempts->is_number_integer() ||
			                                           *delivery_attempts < 0 || *delivery_attempts > 3)))
			{
				if (error_out != nullptr) *error_out = "Agent run JSON has an invalid schema.";
				return std::nullopt;
			}
			if (value["version"] == 1 && value.contains("managedContinuation"))
				throw std::invalid_argument("v1 continuation is not legacy evidence");
			if (value["version"] == 2)
			{
				for (const char* key : {"id", "rootChatId", "parentRunId", "resumedFromRunId", "transcriptChatId", "goalId", "agentId", "definitionHash", "definitionSnapshot", "definitionInstructions", "providerId", "modelId", "executionCapability", "task", "effectiveWorkspaceAccess", "status", "createdAt", "startedAt", "finishedAt", "updatedAt", "resultDeliveryId", "resultExcerpt", "diagnosticCode", "diagnostic"})
					(void)ReadManagedString(value.at(key));
				for (const char* key : {"deliverResultToRootChat", "rootResultDelivered"}) (void)ReadManagedBool(value.at(key));
				for (const char* key : {"skillsSnapshot", "delegatesSnapshot"})
				{
					if (!value.at(key).is_array() || value.at(key).size() > 16) throw std::invalid_argument("managed saved policy list");
					for (const auto& item : value.at(key)) (void)ReadManagedString(item);
				}
				(void)ReadManagedInteger<int>(value.at("depth"), 1, 2);
				(void)ReadManagedInteger<int>(value.at("expectedTurnSerial"), 0, std::numeric_limits<int>::max());
				(void)ReadManagedInteger<int>(value.at("rootResultDeliveryAttempts"), 0, 3);
				for (const char* key : {"startedAtEpochMs", "deadlineAtEpochMs", "finishedAtEpochMs"})
					(void)ReadManagedInteger<int64_t>(value.at(key), 0, std::numeric_limits<int64_t>::max());
			}
			AgentRun run;
			if (value["version"] == 2) run.managed_continuation = ReadManagedRunContinuation(value.at("managedContinuation"));
			run.id = JsonString(value, "id");
			run.root_chat_id = JsonString(value, "rootChatId");
			run.parent_run_id = JsonString(value, "parentRunId");
			run.resumed_from_run_id = JsonString(value, "resumedFromRunId");
			run.transcript_chat_id = JsonString(value, "transcriptChatId");
			run.goal_id = JsonString(value, "goalId");
			run.agent_id = JsonString(value, "agentId");
			run.definition_hash = JsonString(value, "definitionHash");
			run.definition_snapshot = JsonString(value, "definitionSnapshot");
			run.definition_instructions = JsonString(value, "definitionInstructions");
			if (const auto skills = value.find("skillsSnapshot"); skills != value.end() && skills->is_array())
			{
				for (const auto& skill : *skills) if (skill.is_string()) run.skills_snapshot.push_back(skill.get<std::string>());
			}
			if (const auto delegates = value.find("delegatesSnapshot"); delegates != value.end() && delegates->is_array())
			{
				for (const auto& delegate : *delegates)
				{
					if (delegate.is_string()) run.delegates_snapshot.push_back(delegate.get<std::string>());
				}
			}
			run.provider_id = JsonString(value, "providerId");
			run.model_id = JsonString(value, "modelId");
			run.execution_capability = JsonString(value, "executionCapability");
			run.task = JsonString(value, "task");
			run.effective_workspace_access = JsonString(value, "effectiveWorkspaceAccess");
			run.status = JsonString(value, "status");
			run.depth = value["depth"].get<int>();
			run.expected_turn_serial = value["expectedTurnSerial"].get<int>();
			if (const auto started = value.find("startedAtEpochMs"); started != value.end() && started->is_number_integer())
				run.started_at_epoch_ms = started->get<int64_t>();
			if (const auto deadline = value.find("deadlineAtEpochMs"); deadline != value.end() && deadline->is_number_integer())
				run.deadline_at_epoch_ms = deadline->get<int64_t>();
			if (const auto finished = value.find("finishedAtEpochMs"); finished != value.end())
			{
				if (!finished->is_number_integer() || *finished < 0 || *finished > std::numeric_limits<int64_t>::max())
				{
					if (error_out) *error_out = "Agent run has an invalid completion epoch.";
					return std::nullopt;
				}
				run.finished_at_epoch_ms = finished->get<int64_t>();
			}
			run.created_at = JsonString(value, "createdAt");
			run.started_at = JsonString(value, "startedAt");
			run.finished_at = JsonString(value, "finishedAt");
			run.updated_at = JsonString(value, "updatedAt");
			run.result_delivery_id = JsonString(value, "resultDeliveryId");
			if (run.result_delivery_id.empty()) run.result_delivery_id = run.id;
			if (deliver_result != value.end()) run.deliver_result_to_root_chat = deliver_result->get<bool>();
			if (result_delivered != value.end()) run.root_result_delivered = result_delivered->get<bool>();
			if (delivery_attempts != value.end()) run.root_result_delivery_attempts = delivery_attempts->get<int>();
			run.result_excerpt = JsonString(value, "resultExcerpt");
			run.diagnostic_code = JsonString(value, "diagnosticCode");
			run.diagnostic = JsonString(value, "diagnostic");
			if (run.id != expected_id || !Validate(run, error_out)) return std::nullopt;
			return run;
		}
		catch (const std::exception&)
		{
			if (error_out) *error_out = "Agent run JSON has an invalid schema.";
			return std::nullopt;
		}

		nlohmann::json ToJson(const AgentRun& run)
		{
			nlohmann::json value = {{"version", run.managed_continuation ? 2 : 1}, {"id", run.id}, {"rootChatId", run.root_chat_id},
			        {"parentRunId", run.parent_run_id}, {"resumedFromRunId", run.resumed_from_run_id},
			        {"transcriptChatId", run.transcript_chat_id},
			        {"goalId", run.goal_id}, {"agentId", run.agent_id},
			        {"definitionHash", run.definition_hash}, {"definitionSnapshot", run.definition_snapshot},
			        {"definitionInstructions", run.definition_instructions}, {"skillsSnapshot", run.skills_snapshot},
			        {"delegatesSnapshot", run.delegates_snapshot},
			        {"providerId", run.provider_id}, {"modelId", run.model_id},
			        {"executionCapability", run.execution_capability}, {"task", run.task},
			        {"effectiveWorkspaceAccess", run.effective_workspace_access}, {"status", run.status},
			        {"depth", run.depth}, {"expectedTurnSerial", run.expected_turn_serial},
			        {"startedAtEpochMs", run.started_at_epoch_ms}, {"finishedAtEpochMs", run.finished_at_epoch_ms}, {"deadlineAtEpochMs", run.deadline_at_epoch_ms},
			        {"createdAt", run.created_at}, {"startedAt", run.started_at},
			        {"finishedAt", run.finished_at}, {"updatedAt", run.updated_at},
			        {"resultDeliveryId", run.result_delivery_id.empty() ? run.id : run.result_delivery_id},
			        {"deliverResultToRootChat", run.deliver_result_to_root_chat},
			        {"rootResultDelivered", run.root_result_delivered},
			        {"rootResultDeliveryAttempts", run.root_result_delivery_attempts},
			        {"resultExcerpt", run.result_excerpt}, {"diagnosticCode", run.diagnostic_code},
			        {"diagnostic", run.diagnostic}};
			if (run.managed_continuation) value["managedContinuation"] = ManagedRunContinuationToJson(*run.managed_continuation);
			return value;
		}

		struct LedgerFileSnapshot
		{
			std::string bytes;
			std::uintmax_t size = 0;
			std::filesystem::file_time_type modified;
		};

		bool ReadLedgerSnapshot(const std::filesystem::path& path, LedgerFileSnapshot& snapshot)
		{
			const auto regular = [&]
			{
				return !paths::IsLinkOrReparsePointNoThrow(path.parent_path()) &&
				    paths::IsDirectoryNoThrow(path.parent_path()) &&
				    !paths::IsLinkOrReparsePointNoThrow(path) && paths::IsRegularFileNoThrow(path);
			};
			if (!regular()) return false;
			std::error_code error;
			const auto size = paths::FileSizeNoThrow(path);
			const auto modified = std::filesystem::last_write_time(path, error);
			if (!size || *size > kMaxRunFileBytes || error) return false;
			std::string bytes;
			if (!io::TryReadTextFile(path, bytes, kMaxRunFileBytes)) return false;
			const auto after_size = paths::FileSizeNoThrow(path);
			const auto after_modified = std::filesystem::last_write_time(path, error);
			if (error || !regular() || !after_size || *after_size != *size || bytes.size() != *size ||
			    after_modified != modified) return false;
			snapshot = {std::move(bytes), *size, modified};
			return true;
		}

		bool SameLedgerSnapshot(const LedgerFileSnapshot& left, const LedgerFileSnapshot& right)
		{
			return left.size == right.size && left.modified == right.modified && left.bytes == right.bytes;
		}

		bool SameRunValue(const AgentRun& left, const AgentRun& right)
		{
			// ToJson covers the full ledger schema; delivery ID has an explicit encoding normalization.
			return left.result_delivery_id == right.result_delivery_id && ToJson(left) == ToJson(right);
		}

		bool SameRunImmutableFields(const AgentRun& before, const AgentRun& after)
		{
			auto left = ToJson(before), right = ToJson(after);
			// An explicit mutable allowlist keeps other/future encoded fields immutable by default.
			for (const char* field : {"managedContinuation", "status", "expectedTurnSerial", "startedAtEpochMs",
			    "finishedAtEpochMs", "deadlineAtEpochMs", "startedAt", "finishedAt", "updatedAt",
			    "rootResultDelivered", "rootResultDeliveryAttempts", "resultExcerpt", "diagnosticCode", "diagnostic"})
			{
				left.erase(field);
				right.erase(field);
			}
			return before.result_delivery_id == after.result_delivery_id && left == right;
		}

		bool TerminalTurn(ManagedRunTurnStatus status)
		{
			return status != ManagedRunTurnStatus::Queued && status != ManagedRunTurnStatus::Running;
		}

		bool SameTurnAuthoredFields(const ManagedRunTurnReceipt& before, const ManagedRunTurnReceipt& after)
		{
			auto permitted = before;
			permitted.status = after.status;
			permitted.dispatch_attempted = after.dispatch_attempted;
			permitted.expected_turn_serial = after.expected_turn_serial;
			permitted.first_user_message_index = after.first_user_message_index;
			permitted.user_message_created_at = after.user_message_created_at;
			permitted.started_at_epoch_ms = after.started_at_epoch_ms;
			permitted.finished_at_epoch_ms = after.finished_at_epoch_ms;
			permitted.result_delivered = after.result_delivered;
			permitted.result_delivery_attempts = after.result_delivery_attempts;
			permitted.audit_pending = after.audit_pending;
			permitted.diagnostic_code = after.diagnostic_code;
			return permitted == after;
		}

		template <typename T> bool StableCheckpoint(const T& before, const T& after, const T& empty)
		{
			return before == empty || before == after;
		}

		bool CurrentTurnAdvances(const ManagedRunTurnReceipt& before, const ManagedRunTurnReceipt& after)
		{
			const bool status_advances = before.status == after.status || before.status == ManagedRunTurnStatus::Queued ||
			    (before.status == ManagedRunTurnStatus::Running && TerminalTurn(after.status));
			return SameTurnAuthoredFields(before, after) && status_advances &&
			    (!before.dispatch_attempted || after.dispatch_attempted) &&
			    (!before.result_delivered || after.result_delivered) &&
			    (!after.result_delivered || TerminalTurn(after.status)) &&
			    after.result_delivery_attempts >= before.result_delivery_attempts &&
			    StableCheckpoint(before.expected_turn_serial, after.expected_turn_serial, 0) &&
			    StableCheckpoint(before.first_user_message_index, after.first_user_message_index, -1) &&
			    StableCheckpoint(before.user_message_created_at, after.user_message_created_at, std::string{}) &&
			    StableCheckpoint(before.started_at_epoch_ms, after.started_at_epoch_ms, int64_t{0}) &&
			    StableCheckpoint(before.finished_at_epoch_ms, after.finished_at_epoch_ms, int64_t{0});
		}

		bool CompleteExecutionShape(const ManagedRunExecutionBinding& value)
		{
			return !value.native_session_id.empty() && !value.effective_model_id.empty() && !value.model_identity_source.empty();
		}

		bool ValidManagedTransition(const AgentRun& expected, const AgentRun& candidate)
		{
			if (!expected.managed_continuation || !candidate.managed_continuation ||
			    !SameRunImmutableFields(expected, candidate)) return false;
			const auto& before = *expected.managed_continuation;
			const auto& after = *candidate.managed_continuation;
			if (before.owner != after.owner || after.state_revision != before.state_revision + 1) return false;
			if (before.execution != after.execution)
			{
				// Structural initial capture only: the future owner must supply actual native attestation.
				if (before.execution || !after.execution || before.turns.size() != 1 || after.turns.size() != 1 ||
				    !CompleteExecutionShape(*after.execution)) return false;
			}
			if (after.turns.size() == before.turns.size())
			{
				if (before.current_turn_id != after.current_turn_id || before.current_turn_number != after.current_turn_number ||
				    !std::equal(before.turns.begin(), before.turns.end() - 1, after.turns.begin()) ||
				    !CurrentTurnAdvances(before.turns.back(), after.turns.back())) return false;
				if (before.turns.size() > 1 && (expected.root_result_delivered != candidate.root_result_delivered ||
				    expected.root_result_delivery_attempts != candidate.root_result_delivery_attempts)) return false;
				return StableCheckpoint(expected.started_at, candidate.started_at, std::string{}) &&
				    StableCheckpoint(expected.finished_at, candidate.finished_at, std::string{}) &&
				    StableCheckpoint(expected.deadline_at_epoch_ms, candidate.deadline_at_epoch_ms, int64_t{0}) &&
				    candidate.diagnostic_code == after.turns.back().diagnostic_code;
			}
			if (after.turns.size() != before.turns.size() + 1 || before.execution != after.execution ||
			    !before.execution || !CompleteExecutionShape(*before.execution) ||
			    !TerminalTurn(before.turns.back().status) ||
			    (before.turns.back().result_required && !before.turns.back().result_delivered) ||
			    !std::equal(before.turns.begin(), before.turns.end(), after.turns.begin()) ||
			    expected.root_result_delivered != candidate.root_result_delivered ||
			    expected.root_result_delivery_attempts != candidate.root_result_delivery_attempts) return false;
			const auto& next = after.turns.back();
			return next.status == ManagedRunTurnStatus::Queued && !next.dispatch_attempted && next.expected_turn_serial == 0 &&
			    next.first_user_message_index == -1 && next.user_message_created_at.empty() &&
			    next.started_at_epoch_ms == 0 && next.finished_at_epoch_ms == 0 && next.result_required &&
			    !next.result_delivered && next.result_delivery_attempts == 0 && !next.audit_pending && next.diagnostic_code.empty() &&
			    candidate.started_at.empty() && candidate.finished_at.empty() && candidate.deadline_at_epoch_ms == 0 &&
			    candidate.result_excerpt.empty() && candidate.diagnostic_code.empty() && candidate.diagnostic.empty();
		}

		bool RealLedgerDirectory(const std::filesystem::path& path)
		{
			std::error_code error;
			const auto status = std::filesystem::symlink_status(path, error);
			return !error && std::filesystem::is_directory(status) && !paths::IsLinkOrReparsePointNoThrow(path);
		}

		bool LedgerPathAbsent(const std::filesystem::path& path)
		{
			std::error_code error;
			const auto status = std::filesystem::symlink_status(path, error);
			return status.type() == std::filesystem::file_type::not_found &&
			    (!error || error == std::errc::no_such_file_or_directory) && !paths::IsLinkOrReparsePointNoThrow(path);
		}

		bool InitialLedgerPathsAbsent(const std::filesystem::path& primary)
		{
			return RealLedgerDirectory(primary.parent_path().parent_path()) && RealLedgerDirectory(primary.parent_path()) &&
			    LedgerPathAbsent(primary) && LedgerPathAbsent(io::MakeBackupPath(primary));
		}

		bool ValidManagedInitialShape(const AgentRun& run)
		{
			if (!run.managed_continuation) return false;
			const auto& initial = *run.managed_continuation;
			if (initial.turns.size() != 1 || initial.current_turn_number != 1 || initial.state_revision != 1 ||
			    initial.current_turn_id != run.id || initial.execution ||
			    initial.owner.root_birth_id == initial.owner.transcript_birth_id) return false;
			const auto& turn = initial.turns.front();
			return run.status == "queued" && run.result_delivery_id == run.id && run.expected_turn_serial == 0 &&
			    run.started_at_epoch_ms == 0 && run.finished_at_epoch_ms == 0 && run.deadline_at_epoch_ms == 0 &&
			    run.started_at.empty() && run.finished_at.empty() && !run.root_result_delivered &&
			    run.root_result_delivery_attempts == 0 && run.result_excerpt.empty() && run.diagnostic_code.empty() && run.diagnostic.empty() &&
			    turn.status == ManagedRunTurnStatus::Queued && !turn.dispatch_attempted && turn.expected_turn_serial == 0 &&
			    turn.first_user_message_index == -1 && turn.user_message_created_at.empty() &&
			    turn.started_at_epoch_ms == 0 && turn.finished_at_epoch_ms == 0 && !turn.result_delivered &&
			    turn.result_delivery_attempts == 0 && !turn.audit_pending && turn.diagnostic_code.empty();
		}

		std::optional<AgentRun> ReadPath(const std::filesystem::path& path, std::string_view expected_id,
		                                 std::string* error_out, std::string* text_out = nullptr,
		                                 std::uintmax_t* remaining_bytes = nullptr,
		                                 const std::function<void(const std::filesystem::path&)>& after_size_for_tests = {},
		                                 bool* legacy_recovery_allowed_out = nullptr)
		{
			if (legacy_recovery_allowed_out != nullptr) *legacy_recovery_allowed_out = false;
			if (uam::paths::IsLinkOrReparsePointNoThrow(path))
			{
				if (error_out != nullptr) *error_out = "Agent run file is a link.";
				return std::nullopt;
			}
			if (legacy_recovery_allowed_out != nullptr)
			{
				if (LedgerPathAbsent(path))
				{
					*legacy_recovery_allowed_out = true;
					if (error_out != nullptr) *error_out = "Agent run file is absent.";
					return std::nullopt;
				}
				if (!uam::paths::IsRegularFileNoThrow(path))
				{
					if (error_out != nullptr) *error_out = "Agent run file is not an available regular file.";
					return std::nullopt;
				}
			}
			const auto size = uam::paths::FileSizeNoThrow(path);
			if (!size.has_value() || *size == 0 || *size > kMaxRunFileBytes)
			{
				if (legacy_recovery_allowed_out != nullptr && size.has_value() && *size == 0)
					*legacy_recovery_allowed_out = true;
				if (error_out != nullptr) *error_out = "Agent run file is empty or exceeds 512 KiB.";
				return std::nullopt;
			}
			if (remaining_bytes != nullptr && *size > *remaining_bytes)
			{
				if (error_out != nullptr) *error_out = "The bounded 64 MiB agent-run load budget was exhausted.";
				*remaining_bytes = 0;
				return std::nullopt;
			}
			if (remaining_bytes != nullptr) *remaining_bytes -= *size;
			if (after_size_for_tests) after_size_for_tests(path);
			// Read only the admitted size even if a concurrent atomic save grows the primary.
			std::ifstream input(path, std::ios::binary);
			std::string text(static_cast<std::size_t>(*size), '\0');
			if (!input || !input.read(text.data(), static_cast<std::streamsize>(text.size())))
			{
				if (error_out != nullptr) *error_out = "Agent run file could not be read.";
				return std::nullopt;
			}
			const auto size_after = uam::paths::FileSizeNoThrow(path);
			if (!size_after.has_value() || *size_after != *size || text.size() != *size)
			{
				if (error_out != nullptr) *error_out = "Agent run file changed while it was being read.";
				return std::nullopt;
			}
			if (text_out != nullptr) *text_out = text;
			// Only completely inspected bytes may authorize legacy recovery after a parse failure.
			if (legacy_recovery_allowed_out != nullptr) *legacy_recovery_allowed_out = true;
			return Parse(text, expected_id, error_out);
		}
	} // namespace

	struct PreparedAgentRunSave
	{
		std::filesystem::path primary_path;
		std::filesystem::path temp_path;
		AgentRun expected;
		AgentRun candidate;
		LedgerFileSnapshot primary_snapshot;
		std::optional<LedgerFileSnapshot> temp_snapshot;
		std::string encoded;
		bool initial_creation = false;
		bool owns_temp = false;
		bool consumed = false;
		bool cleanup_unused = true;

		~PreparedAgentRunSave()
		{
			// Never remove a path that might now contain the exchanged old primary or foreign bytes.
			if (!owns_temp || !cleanup_unused) return;
			try
			{
				LedgerFileSnapshot current;
				if (ReadLedgerSnapshot(temp_path, current) &&
				    (temp_snapshot ? SameLedgerSnapshot(*temp_snapshot, current) : encoded.starts_with(current.bytes)))
					io::RemoveAtomicTempNoThrow(temp_path);
			}
			catch (...) {} // Cleanup failure must not terminate an owner or discard other evidence.
		}
	};

	namespace
	{
		std::shared_ptr<PreparedAgentRunSave> StageAgentRunSave(
		    const std::shared_ptr<PreparedAgentRunSave>& prepared, std::string* error_out)
		{
			prepared->temp_path = io::MakeTempWritePath(prepared->primary_path);
			io::AtomicWriteResult write_result;
			const auto created = [&](io::AtomicWriteStage stage)
			{
				if (stage == io::AtomicWriteStage::AfterCreate) prepared->owns_temp = true;
				return true;
			};
			LedgerFileSnapshot staged;
			if (!io::WriteAndSyncAtomicTemp(prepared->temp_path, prepared->encoded, created, write_result) ||
			    !ReadLedgerSnapshot(prepared->temp_path, staged) || staged.bytes != prepared->encoded)
			{
				if (error_out) *error_out = write_result.error.empty() ? "Agent-run candidate could not be staged and synced." : write_result.error;
				return {};
			}
			prepared->temp_snapshot = std::move(staged);
			if (error_out) error_out->clear();
			return prepared;
		}
	} // namespace

	std::string AgentRunLedger::NewRunId()
	{
		return PlatformServicesFactory::Instance().process_service.GenerateUuid();
	}

	bool AgentRunLedger::Save(const std::filesystem::path& data_root, const AgentRun& run,
	                          std::string* error_out)
	{
		if (!Validate(run, error_out)) return false;
		std::string encoded;
		try { encoded = ToJson(run).dump(2); }
		catch (const std::exception&)
		{
			if (error_out) *error_out = "Agent run cannot be encoded.";
			return false;
		}
		if (encoded.size() > kMaxRunFileBytes)
		{
			if (error_out) *error_out = "Encoded agent run exceeds 512 KiB.";
			return false;
		}
		std::lock_guard<std::mutex> lock(LedgerWriteMutex());
		const auto primary = RunPath(data_root, run.id);
		if (uam::paths::PathExistsNoThrow(primary))
		{
			std::string primary_text;
			if (uam::paths::IsLinkOrReparsePointNoThrow(primary) ||
			    !uam::paths::IsRegularFileNoThrow(primary) ||
			    !uam::io::TryReadTextFile(primary, primary_text, kMaxRunFileBytes))
			{
				if (error_out) *error_out = "Existing agent run cannot be inspected safely before saving.";
				return false;
			}
			if (RecognizableManagedRecord(primary_text))
			{
				const auto existing = Parse(primary_text, run.id, nullptr);
				// Preserve an unchanged inert roundtrip without rewriting its primary or backup.
				if (existing && run.managed_continuation && ToJson(*existing) == ToJson(run)) return true;
				if (error_out) *error_out = "Managed continuation changes require conditional publication.";
				return false;
			}
		}
		if (!uam::paths::CreateDirectoriesNoThrow(RunsRoot(data_root)))
		{
			if (error_out != nullptr) *error_out = "Agent run directory could not be created.";
			return false;
		}
		if (!uam::io::WriteTextFileWithBackup(RunPath(data_root, run.id), encoded))
		{
			if (error_out != nullptr) *error_out = "Agent run could not be persisted.";
			return false;
		}
		return true;
	}

	AgentRunLoadResult AgentRunLedger::LoadAll(const std::filesystem::path& data_root)
	{
		AgentRunLoadResult result = LoadAllImpl(data_root, true, {});
		uam::excluded_provider_data::RemoveRuns(result.runs);
		return result;
	}

	AgentRunLoadResult AgentRunLedger::LoadAllReadOnly(const std::filesystem::path& data_root,
	    const std::function<void(const std::filesystem::path&)>& after_primary_size_for_tests)
	{
		AgentRunLoadResult result = LoadAllImpl(data_root, false, after_primary_size_for_tests);
		uam::excluded_provider_data::RemoveRuns(result.runs);
		return result;
	}

	AgentRunLoadResult AgentRunLedger::LoadAllImpl(const std::filesystem::path& data_root, bool recover,
	    const std::function<void(const std::filesystem::path&)>& after_primary_size_for_tests)
	{
		AgentRunLoadResult result;
		const std::filesystem::path root = RunsRoot(data_root);
		if (!uam::paths::PathExistsNoThrow(root)) return result;
		if (!uam::paths::IsDirectoryNoThrow(root) || uam::paths::IsLinkOrReparsePointNoThrow(root))
		{
			AddLoadError(result, "Agent run root is not a real directory.");
			return result;
		}
		std::vector<std::filesystem::path> files;
		std::size_t discovered_files = 0;
		std::size_t directory_entries = 0;
		std::error_code list_error;
		for (std::filesystem::directory_iterator it(root, list_error), end; !list_error && it != end; it.increment(list_error))
		{
			if (!recover && ++directory_entries > kMaxRunFiles * 2)
			{
				AddLoadError(result, "Read-only agent-run directory exceeds the bounded 20,000-entry scan limit.");
				return result;
			}
			if (it->path().extension() != ".json") continue;
			++discovered_files;
			if (!recover && discovered_files > kMaxRunFiles)
			{
				AddLoadError(result, "Read-only agent-run directory exceeds the bounded 10,000-file limit.");
				return result;
			}
			if (files.size() < kMaxRunFiles) files.push_back(it->path());
		}
		if (list_error)
		{
			AddLoadError(result, "Agent run directory could not be listed.");
			return result;
		}
		if (discovered_files > kMaxRunFiles)
			AddLoadError(result, "Agent run directory exceeds the bounded 10,000-file load limit; excess records were not executed.");
		std::ranges::sort(files, {}, [](const auto& path) { return uam::paths::PortablePathString(path.filename()); });
		std::map<std::string, AgentRun> runs;
		std::uintmax_t remaining_bytes = kMaxLedgerLoadBytes;
		for (const auto& path : files)
		{
			// Mutable recovery validates its primary and backup inside the same writer boundary.
			// Read-only observers stay unlocked and may themselves call Save.
			std::unique_lock<std::mutex> recovery_lock(LedgerWriteMutex(), std::defer_lock);
			if (recover) recovery_lock.lock();
			const std::string id = uam::paths::Utf8PathString(path.stem());
			std::string primary_error;
			std::string primary_text;
			bool legacy_recovery_allowed = false;
			std::optional<AgentRun> run = ReadPath(path, id, &primary_error, &primary_text, &remaining_bytes, after_primary_size_for_tests,
			    recover ? &legacy_recovery_allowed : nullptr);
			// Startup must not adopt even a legacy backup over a recognizable v2 primary.
			if (recover && RecognizableManagedRecord(primary_text))
			{
				AddLoadError(result, "Managed continuation record is unsupported for execution and remains inert: " + id + ".");
				continue;
			}
			if (!run.has_value())
			{
				if (!recover)
				{
					AddLoadError(result, "Read-only agent-run primary is unavailable, invalid or changed during the read.");
					if (remaining_bytes == 0) break;
					continue;
				}
				if (!legacy_recovery_allowed)
				{
					AddLoadError(result, uam::paths::Utf8PathString(path) + ": primary could not be inspected safely (" + primary_error + "); backup was not restored.");
					if (remaining_bytes == 0) break;
					continue;
				}
				std::string backup_error;
				std::string backup_text;
				run = ReadPath(uam::io::MakeBackupPath(path), id, &backup_error, &backup_text, &remaining_bytes);
				if (RecognizableManagedRecord(backup_text))
				{
					AddLoadError(result, "Managed continuation backup is unsupported for execution and was not restored: " + id + ".");
					continue;
				}
				if (!run.has_value())
				{
					AddLoadError(result, uam::paths::Utf8PathString(path) + ": primary invalid (" + primary_error + "); backup invalid (" + backup_error + ").");
					if (remaining_bytes == 0) break;
					continue;
				}
				if (!uam::io::WriteTextFile(path, backup_text))
				{
					AddLoadError(result, uam::paths::Utf8PathString(path) + ": loaded backup but could not restore primary.");
				}
			}
			if (!runs.emplace(run->id, *run).second)
				AddLoadError(result, "Duplicate agent run identity: " + run->id + ".");
		}

		std::unordered_set<std::string> invalid;
		for (const auto& [id, run] : runs)
		{
			std::unordered_set<std::string> seen{id};
			std::string parent = run.parent_run_id;
			while (!parent.empty())
			{
				const auto found = runs.find(parent);
				if (found == runs.end() || !seen.insert(parent).second)
				{
					invalid.insert(id);
					AddLoadError(result, "Agent run " + id + " has a missing or cyclic parent.");
					break;
				}
				parent = found->second.parent_run_id;
			}
		}
		for (auto& [id, run] : runs)
		{
			if (!invalid.contains(id)) result.runs.push_back(std::move(run));
		}
		return result;
	}

	bool AgentRunLedger::MarkNonterminalInterrupted(const std::filesystem::path& data_root,
	                                                std::vector<AgentRun>* runs,
	                                                std::string_view diagnostic_code,
	                                                std::vector<std::string>* errors)
	{
		if (runs == nullptr || diagnostic_code.empty() || diagnostic_code.size() > 80) return false;
		bool success = true;
		const int64_t now_epoch_ms = uam::time::SystemEpochMillisecondsNow();
		const std::string now = uam::time::detail::FormatTimestamp(
		    uam::time::detail::LocalTimeSnapshot(static_cast<std::time_t>(now_epoch_ms / 1000)), uam::time::kLocalDisplayTimestampFormat);
		for (AgentRun& run : *runs)
		{
			if (run.managed_continuation)
			{
				success = false;
				if (errors) errors->push_back("Managed continuation record is inert; legacy interruption was refused: " + run.id + ".");
				continue;
			}
			if (run.status != "queued" && run.status != "running") continue;
			AgentRun interrupted = run;
			interrupted.status = "interrupted";
			interrupted.finished_at = now;
			interrupted.finished_at_epoch_ms = now_epoch_ms;
			interrupted.updated_at = now;
			interrupted.diagnostic_code = std::string(diagnostic_code);
			interrupted.diagnostic = "UAM interrupted this run instead of relaunching it.";
			std::string error;
			if (!Save(data_root, interrupted, &error))
			{
				success = false;
				if (errors != nullptr) errors->push_back("Agent run " + run.id + ": " + error);
				continue;
			}
			run = std::move(interrupted);
		}
		return success;
	}

	std::shared_ptr<PreparedAgentRunSave> AgentRunLedger::PrepareInitialSave(
	    const std::filesystem::path& data_root, const AgentRun& candidate, std::string* error_out)
	try
	{
		if (!Validate(candidate, error_out)) return {};
		if (!ValidManagedInitialShape(candidate))
		{
			if (error_out) *error_out = "Agent run is not an unstarted managed initial record.";
			return {};
		}
		auto prepared = std::make_shared<PreparedAgentRunSave>();
		prepared->initial_creation = true;
		prepared->candidate = candidate;
		prepared->encoded = ToJson(candidate).dump(2);
		if (prepared->encoded.size() > kMaxRunFileBytes)
		{
			if (error_out) *error_out = "Encoded agent run exceeds 512 KiB.";
			return {};
		}
		std::error_code path_error;
		const auto absolute_root = std::filesystem::absolute(data_root, path_error);
		if (path_error || data_root.empty())
		{
			if (error_out) *error_out = "Agent run root could not be resolved.";
			return {};
		}
		const auto root = absolute_root.lexically_normal();
		prepared->primary_path = RunPath(root, candidate.id);
		{
			std::lock_guard<std::mutex> lock(LedgerWriteMutex());
			const auto directory = RunsRoot(root);
			if (!RealLedgerDirectory(root) ||
			    (!RealLedgerDirectory(directory) && (!LedgerPathAbsent(directory) || !paths::CreateDirectoriesNoThrow(directory))) ||
			    !InitialLedgerPathsAbsent(prepared->primary_path))
			{
				if (error_out) *error_out = "Initial agent-run primary and backup must both be absent under a real data root.";
				return {};
			}
		}
		return StageAgentRunSave(prepared, error_out);
	}
	catch (const std::exception&)
	{
		if (error_out) *error_out = "Initial agent-run preparation failed.";
		return {};
	}

	std::shared_ptr<PreparedAgentRunSave> AgentRunLedger::PrepareSave(
	    const std::filesystem::path& data_root, const AgentRun& expected,
	    const AgentRun& candidate, std::string* error_out)
	try
	{
		if (!Validate(expected, error_out) || !Validate(candidate, error_out)) return {};
		if (!ValidManagedTransition(expected, candidate))
		{
			if (error_out) *error_out = "Invalid managed continuation transition.";
			return {};
		}
		auto prepared = std::make_shared<PreparedAgentRunSave>();
		prepared->expected = expected;
		prepared->candidate = candidate;
		prepared->encoded = ToJson(candidate).dump(2);
		if (prepared->encoded.size() > kMaxRunFileBytes)
		{
			if (error_out) *error_out = "Encoded agent run exceeds 512 KiB.";
			return {};
		}
		std::error_code path_error;
		const auto absolute_root = std::filesystem::absolute(data_root, path_error);
		if (path_error)
		{
			if (error_out) *error_out = "Agent run root could not be resolved.";
			return {};
		}
		prepared->primary_path = RunPath(absolute_root.lexically_normal(), expected.id);
		{
			std::lock_guard<std::mutex> lock(LedgerWriteMutex());
			if (!ReadLedgerSnapshot(prepared->primary_path, prepared->primary_snapshot))
			{
				if (error_out) *error_out = "Expected agent-run primary is unavailable.";
				return {};
			}
			const auto parsed = Parse(prepared->primary_snapshot.bytes, expected.id, nullptr);
			const auto raw = nlohmann::json::parse(prepared->primary_snapshot.bytes, nullptr, false);
			if (!parsed || !SameRunValue(*parsed, expected) || raw != ToJson(expected))
			{
				if (error_out) *error_out = "Agent-run primary differs from the exact modeled predecessor.";
				return {};
			}
		}
		return StageAgentRunSave(prepared, error_out);
	}
	catch (const std::exception&)
	{
		if (error_out) *error_out = "Agent-run conditional preparation failed.";
		return {};
	}

	AgentRunPublishResult AgentRunLedger::PublishPreparedSave(
	    const std::shared_ptr<PreparedAgentRunSave>& prepared,
	    const std::function<bool()>& still_owned)
	{
		return PublishPreparedSave(prepared, still_owned, {});
	}

	AgentRunPublishResult AgentRunLedger::PublishPreparedSave(
	    const std::shared_ptr<PreparedAgentRunSave>& prepared,
	    const std::function<bool()>& still_owned,
	    const std::function<bool(io::AtomicWriteStage)>& stage_hook_for_tests)
	{
		std::lock_guard<std::mutex> lock(LedgerWriteMutex());
		if (!prepared || prepared->consumed) return {AgentRunPublishState::Rejected, "Agent-run preparation is absent or already consumed."};
		prepared->consumed = true;
		const auto matches = [&]
		{
			LedgerFileSnapshot primary, staged;
			const bool primary_matches = prepared->initial_creation ? InitialLedgerPathsAbsent(prepared->primary_path) :
			    (ReadLedgerSnapshot(prepared->primary_path, primary) && SameLedgerSnapshot(prepared->primary_snapshot, primary));
			return primary_matches && prepared->temp_snapshot && ReadLedgerSnapshot(prepared->temp_path, staged) &&
			    SameLedgerSnapshot(*prepared->temp_snapshot, staged) && staged.bytes == prepared->encoded;
		};
		bool replacement_allowed = false;
		std::string rejection_reason;
		try
		{
			if (!matches()) return {AgentRunPublishState::Rejected, "Agent-run primary or staged candidate changed."};
			const auto hook = [&](io::AtomicWriteStage stage)
			{
				// Hooks and still_owned are read-only, bounded and non-reentrant under the writer mutex.
				if (stage_hook_for_tests && !stage_hook_for_tests(stage)) return false;
				if (stage == io::AtomicWriteStage::BeforeReplace)
				{
					if (!matches())
					{
						rejection_reason = "Agent-run primary or staged candidate changed before replacement.";
						return false;
					}
					if (!still_owned || !still_owned())
					{
						rejection_reason = "Agent-run publication no longer has live ownership.";
						return false;
					}
					replacement_allowed = true;
					// From this point a failure may have exchanged the primary; preserve all evidence.
					prepared->cleanup_unused = false;
				}
				return true;
			};
			const auto written = io::CommitSyncedAtomicTemp(prepared->primary_path, prepared->temp_path, true, hook);
			if (written.success && written.primary_committed)
				return {AgentRunPublishState::Committed, written.error};
			if (!replacement_allowed)
				return {AgentRunPublishState::Rejected, !rejection_reason.empty() ? rejection_reason :
				    (written.error.empty() ? "Agent-run publication was rejected before replacement." : written.error)};
			// POSIX can exchange before primary_committed becomes true. Do not infer rollback from a false flag.
			return {AgentRunPublishState::Uncertain, written.error.empty() ? "Agent-run replacement durability is uncertain." : written.error};
		}
		catch (...)
		{
			return {replacement_allowed ? AgentRunPublishState::Uncertain : AgentRunPublishState::Rejected,
			    "Agent-run publication failed; replacement evidence was retained if replacement was possible."};
		}
	}
} // namespace uam
