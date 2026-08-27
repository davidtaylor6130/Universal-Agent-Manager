#include "app/agent_run_ledger.h"

#include "common/chat/chat_ids.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <optional>
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

		bool Validate(const AgentRun& run, std::string* error_out)
		{
			const bool valid = uam::codex::IsCanonicalUuid(run.id) &&
			                   SafeReference(run.root_chat_id, false) && SafeReference(run.transcript_chat_id) &&
			                   SafeReference(run.goal_id) &&
			                   (run.parent_run_id.empty() || uam::codex::IsCanonicalUuid(run.parent_run_id)) &&
			                   (run.resumed_from_run_id.empty() || uam::codex::IsCanonicalUuid(run.resumed_from_run_id)) &&
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
			                   run.expected_turn_serial >= 0 && run.started_at_epoch_ms >= 0 &&
			                   run.deadline_at_epoch_ms >= run.started_at_epoch_ms &&
			                   (run.result_delivery_id.empty() || run.result_delivery_id == run.id) &&
			                   run.root_result_delivery_attempts >= 0 &&
			                   run.root_result_delivery_attempts <= 3 &&
			                   (!run.root_result_delivered || run.deliver_result_to_root_chat) &&
			                   !run.created_at.empty() && !run.updated_at.empty() &&
			                   SafeText(run.created_at, 80) && SafeText(run.started_at, 80) &&
			                   SafeText(run.finished_at, 80) && SafeText(run.updated_at, 80) &&
			                   SafeText(run.result_excerpt, kMaxResultBytes) &&
			                   SafeText(run.diagnostic_code, 80) && SafeText(run.diagnostic, kMaxDiagnosticBytes);
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
		{
			const nlohmann::json value = nlohmann::json::parse(text, nullptr, false);
			const auto deliver_result = value.find("deliverResultToRootChat");
			const auto result_delivered = value.find("rootResultDelivered");
			const auto delivery_attempts = value.find("rootResultDeliveryAttempts");
			if (!value.is_object() || !value.contains("version") || !value["version"].is_number_integer() ||
			    value["version"].get<int>() != 1 || !value.contains("depth") || !value["depth"].is_number_integer() ||
			    !value.contains("expectedTurnSerial") || !value["expectedTurnSerial"].is_number_integer() ||
			    (deliver_result != value.end() && !deliver_result->is_boolean()) ||
			    (result_delivered != value.end() && !result_delivered->is_boolean()) ||
			    (delivery_attempts != value.end() && (!delivery_attempts->is_number_integer() ||
			                                           *delivery_attempts < 0 || *delivery_attempts > 3)))
			{
				if (error_out != nullptr) *error_out = "Agent run JSON has an invalid schema.";
				return std::nullopt;
			}
			AgentRun run;
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

		nlohmann::json ToJson(const AgentRun& run)
		{
			return {{"version", 1}, {"id", run.id}, {"rootChatId", run.root_chat_id},
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
			        {"startedAtEpochMs", run.started_at_epoch_ms}, {"deadlineAtEpochMs", run.deadline_at_epoch_ms},
			        {"createdAt", run.created_at}, {"startedAt", run.started_at},
			        {"finishedAt", run.finished_at}, {"updatedAt", run.updated_at},
			        {"resultDeliveryId", run.result_delivery_id.empty() ? run.id : run.result_delivery_id},
			        {"deliverResultToRootChat", run.deliver_result_to_root_chat},
			        {"rootResultDelivered", run.root_result_delivered},
			        {"rootResultDeliveryAttempts", run.root_result_delivery_attempts},
			        {"resultExcerpt", run.result_excerpt}, {"diagnosticCode", run.diagnostic_code},
			        {"diagnostic", run.diagnostic}};
		}

		std::optional<AgentRun> ReadPath(const std::filesystem::path& path, std::string_view expected_id,
		                                 std::string* error_out, std::string* text_out = nullptr,
		                                 std::uintmax_t* remaining_bytes = nullptr)
		{
			if (uam::paths::IsLinkOrReparsePointNoThrow(path))
			{
				if (error_out != nullptr) *error_out = "Agent run file is a link.";
				return std::nullopt;
			}
			const auto size = uam::paths::FileSizeNoThrow(path);
			if (!size.has_value() || *size == 0 || *size > kMaxRunFileBytes)
			{
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
			std::string text;
			if (!uam::io::TryReadTextFile(path, text))
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
			return Parse(text, expected_id, error_out);
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
		if (!uam::paths::CreateDirectoriesNoThrow(RunsRoot(data_root)))
		{
			if (error_out != nullptr) *error_out = "Agent run directory could not be created.";
			return false;
		}
		if (!uam::io::WriteTextFileWithBackup(RunPath(data_root, run.id), ToJson(run).dump(2)))
		{
			if (error_out != nullptr) *error_out = "Agent run could not be persisted.";
			return false;
		}
		return true;
	}

	AgentRunLoadResult AgentRunLedger::LoadAll(const std::filesystem::path& data_root)
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
		std::error_code list_error;
		for (std::filesystem::directory_iterator it(root, list_error), end; !list_error && it != end; it.increment(list_error))
		{
			if (it->path().extension() != ".json") continue;
			++discovered_files;
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
			const std::string id = uam::paths::Utf8PathString(path.stem());
			std::string primary_error;
			std::optional<AgentRun> run = ReadPath(path, id, &primary_error, nullptr, &remaining_bytes);
			if (!run.has_value())
			{
				std::string backup_error;
				std::string backup_text;
				run = ReadPath(uam::io::MakeBackupPath(path), id, &backup_error, &backup_text, &remaining_bytes);
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
		const std::string now = uam::time::TimestampNow();
		for (AgentRun& run : *runs)
		{
			if (run.status != "queued" && run.status != "running") continue;
			AgentRun interrupted = run;
			interrupted.status = "interrupted";
			interrupted.finished_at = now;
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
} // namespace uam
