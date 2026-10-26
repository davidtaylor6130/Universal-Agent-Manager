#include "app/managed_subagent_control_service.h"

#include "app/agent_run_ledger.h"
#include "app/agent_run_scheduler.h"
#include "app/transcript_retention_service.h"
#include "common/platform/platform_incremental_sha256.h"
#include "common/chat/chat_ids.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/runtime/acp/acp_model_json.h"
#include <fstream>
#include "common/provider/provider_ids.h"
#include "common/utils/uuid.h"

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace uam
{
	struct ManagedInitialHistoryCatalogue
	{
		struct Entry
		{
			AgentRun metadata;      // Only typed initial/lineage/status metadata and a bounded summary.
			ChatSession transcript; // Existing value-owned metadata projection; no messages.
			std::string private_body_digest;
			bool binding_present = false;
			std::string error;
		};
		std::filesystem::path data_root;
		std::vector<Entry> entries;
		std::string error;
	};
	namespace
	{
		bool Fail(std::string* error_out, const char* message)
		{
			if (error_out != nullptr)
				*error_out = message;
			return false;
		}

		bool Terminal(const AgentRun& run)
		{
			return run.status == "completed" || run.status == "failed" || run.status == "cancelled" || run.status == "interrupted";
		}

		bool SafeChatId(const std::string& id)
		{
			return id.size() <= 160 && chat_ids::IsSafeStorageChatId(id);
		}

		std::string CardSummary(const AgentRun& run)
		{
			const std::string& text = run.result_excerpt;
			std::size_t end = std::min<std::size_t>(text.size(), 1024);
			while (end > 0 && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
				--end;
			std::string summary = text.substr(0, end);
			for (char& ch : summary)
				if (static_cast<unsigned char>(ch) < 32 || ch == 127)
					ch = ' ';
			while (!summary.empty() && summary.back() == ' ')
				summary.pop_back();
			const auto start = summary.find_first_not_of(' ');
			return start == std::string::npos ? std::string{} : summary.substr(start);
		}

		/// Byte length of the strictly valid UTF-8 scalar at `text[at]`, or 0 for a stray/truncated/overlong/surrogate/out-of-range sequence.
		std::size_t Utf8Scalar(std::string_view text, std::size_t at)
		{
			const auto byte = [&](std::size_t index) { return static_cast<unsigned char>(text[index]); };
			const unsigned char lead = byte(at);
			if (lead < 0x80)
				return 1;
			const std::size_t size = lead >= 0xC2 && lead <= 0xDF ? 2 : lead >= 0xE0 && lead <= 0xEF ? 3 : lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
			if (size == 0 || at + size > text.size())
				return 0;
			for (std::size_t i = 1; i < size; ++i)
				if ((byte(at + i) & 0xC0) != 0x80)
					return 0;
			const unsigned char second = byte(at + 1);
			if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second > 0x9F) || (lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second > 0x8F))
				return 0;
			return size;
		}

		/// First meaningful line/sentence of the owned task: at most 100 code points and 256 bytes of strictly valid UTF-8 (the valid prefix when the task is corrupt), so ToJson never sees invalid text.
		/// Empty for a missing or credential-looking line; the frontend then shows its generic label. Never the result, definition or diagnostics.
		std::string TaskTitle(const AgentRun& run)
		{
			const std::string& text = run.task;
			std::size_t begin = 0;
			while (begin < text.size() && (static_cast<unsigned char>(text[begin]) <= ' ' || text[begin] == 127))
				++begin;
			std::string title;
			std::size_t points = 0;
			for (std::size_t i = begin; i < text.size() && points < 100;)
			{
				if (text[i] == '\n' || text[i] == '\r')
					break;
				const std::size_t size = Utf8Scalar(text, i);
				if (size == 0 || title.size() + size > 256)
					break;
				const char first = text[i];
				const bool control = static_cast<unsigned char>(first) < 32 || first == 127;
				const bool sentence_end = (first == '.' || first == '!' || first == '?') && (i + 1 == text.size() || text[i + 1] == ' ');
				if (control)
					title.push_back(' ');
				else
					title.append(text, i, size);
				++points;
				i += size;
				if (sentence_end && points > 12)
					break;
			}
			while (!title.empty() && title.back() == ' ')
				title.pop_back();
			return title;
		}

		bool BoundedMetadata(const AgentRun& run)
		{
			const auto text = [](const std::string& value, std::size_t maximum) { return value.size() <= maximum && value.find('\0') == std::string::npos; };
			return text(run.id, 160) && text(run.root_chat_id, 160) && text(run.parent_run_id, 160) && text(run.transcript_chat_id, 160) && text(run.agent_id, 160) && text(run.provider_id, 160) && text(run.model_id, 160) && text(run.execution_capability, 80) && text(run.definition_hash, 160) && text(run.goal_id, 160) && text(run.resumed_from_run_id, 160) && text(run.created_at, 80) && text(run.started_at, 80) && text(run.finished_at, 80) && text(run.updated_at, 80) && text(run.status, 20);
		}

		// Immutable lineage, rather than mutable status, binds a listing to the selected run.
		std::string OwnershipKey(const AgentRun& run)
		{
			return nlohmann::json::array({run.id, run.root_chat_id, run.parent_run_id, run.transcript_chat_id, run.agent_id, run.provider_id, run.model_id, run.execution_capability, run.definition_hash, run.goal_id, run.depth, run.created_at, run.resumed_from_run_id}).dump();
		}

		bool SameLedgerRecord(const AgentRun& live, const AgentRun& saved)
		{
			return OwnershipKey(live) == OwnershipKey(saved) && live.status == saved.status && live.started_at == saved.started_at && live.finished_at == saved.finished_at && live.updated_at == saved.updated_at && live.expected_turn_serial == saved.expected_turn_serial && live.started_at_epoch_ms == saved.started_at_epoch_ms && live.finished_at_epoch_ms == saved.finished_at_epoch_ms && live.deadline_at_epoch_ms == saved.deadline_at_epoch_ms;
		}

		bool InitialHistoryShape(const AgentRun& run)
		{
			if (!run.managed_continuation || !run.parent_run_id.empty() || !run.resumed_from_run_id.empty() || run.depth != 1)
				return false;
			const auto& value = *run.managed_continuation;
			const bool complete_binding = !value.execution || (!value.execution->native_session_id.empty() && !value.execution->effective_model_id.empty() && !value.execution->model_identity_source.empty());
			if (!complete_binding || value.owner.delegation_grant || value.turns.empty() || value.turns.size() > 33 || value.current_turn_number != value.turns.size() || value.turns.front().principal.kind != ManagedRunPrincipalKind::UserRoot || !value.turns.front().text.empty())
				return false;
			if (value.turns.size() == 1)
				return true; // Preserve the existing unbound initial inspection route.
			// Later receipts are inspectable only with the genuine saved binding and the same UserRoot scope.
			return value.execution && value.current_turn_id == value.turns.back().turn_id && std::ranges::all_of(value.turns, [&](const ManagedRunTurnReceipt& turn)
			    { return turn.principal.kind == ManagedRunPrincipalKind::UserRoot && turn.principal.root_chat_id == run.root_chat_id && turn.principal.root_birth_id == value.owner.root_birth_id && turn.principal.parent_run_id.empty() && turn.principal.parent_transcript_birth_id.empty(); });
		}

		std::optional<ManagedRunContinuation> HistoryContinuation(const AgentRun& run)
		{
			if (!run.managed_continuation)
				return {};
			const auto& original = *run.managed_continuation;
			ManagedRunContinuation copy;
			// Construct only the inspection fields; never copy execution/configuration and then reset it.
			copy.owner = original.owner;
			copy.current_turn_id = original.current_turn_id;
			copy.current_turn_number = original.current_turn_number;
			copy.state_revision = original.state_revision;
			copy.turns.reserve(original.turns.size());
			for (const auto& turn : original.turns)
			{
				auto& metadata = copy.turns.emplace_back();
				// Never copy private receipt text into the catalogue or a per-query snapshot.
				metadata.turn_id = turn.turn_id;
				metadata.turn_number = turn.turn_number;
				metadata.client_request_id = turn.client_request_id;
				metadata.after_turn_id = turn.after_turn_id;
				metadata.principal = turn.principal;
				metadata.continuity = turn.continuity;
				metadata.recovery_decision = turn.recovery_decision;
				metadata.retry_of_turn_id = turn.retry_of_turn_id;
				metadata.status = turn.status;
				metadata.delivery_id = turn.delivery_id;
				metadata.dispatch_attempted = turn.dispatch_attempted;
				metadata.expected_turn_serial = turn.expected_turn_serial;
				metadata.first_user_message_index = turn.first_user_message_index;
				metadata.user_message_created_at = turn.user_message_created_at;
				metadata.started_at_epoch_ms = turn.started_at_epoch_ms;
				metadata.finished_at_epoch_ms = turn.finished_at_epoch_ms;
				metadata.result_required = turn.result_required;
				metadata.result_delivered = turn.result_delivered;
				metadata.result_delivery_attempts = turn.result_delivery_attempts;
				metadata.audit_pending = turn.audit_pending;
				metadata.diagnostic_code = turn.diagnostic_code;
			}
			return copy;
		}

		std::string PrivateHistoryDigest(const AgentRun& run)
		{
			platform::IncrementalSha256 hash;
			const auto add = [&](std::string_view value) { return hash.Add(std::to_string(value.size())) && hash.Add(":") && hash.Add(value); };
			for (const auto* value : {&run.task, &run.definition_snapshot, &run.definition_instructions, &run.diagnostic, &run.result_excerpt})
				if (!add(*value))
					return {};
			for (const auto* values : {&run.skills_snapshot, &run.delegates_snapshot})
			{
				if (!add(std::to_string(values->size())))
					return {};
				for (const auto& value : *values)
					if (!add(value))
						return {};
			}
			if (!add(run.managed_continuation ? "1" : "0"))
				return {};
			if (run.managed_continuation)
			{
				const auto& turns = run.managed_continuation->turns;
				if (!add(std::to_string(turns.size())))
					return {};
				for (const auto& turn : turns)
				{
					// Original private text/authored fields are hashed at initialization and detached read, never Capture.
					for (const auto* value : {&turn.turn_id, &turn.client_request_id, &turn.after_turn_id, &turn.principal.root_chat_id, &turn.principal.root_birth_id, &turn.principal.parent_run_id, &turn.principal.parent_transcript_birth_id, &turn.text, &turn.retry_of_turn_id, &turn.delivery_id})
						if (!add(*value))
							return {};
					if (!add(std::to_string(turn.turn_number)) || !add(std::to_string(static_cast<int>(turn.principal.kind))) || !add(std::to_string(static_cast<int>(turn.continuity))) || !add(std::to_string(static_cast<int>(turn.recovery_decision))))
						return {};
				}
			}
			const bool binding_present = run.managed_continuation && run.managed_continuation->execution.has_value();
			if (!add(binding_present ? "1" : "0"))
				return {};
			if (binding_present)
			{
				const auto& binding = *run.managed_continuation->execution;
				for (const auto* value : {&binding.connection_configuration_json, &binding.provider_configuration_json, &binding.execution_host_id, &binding.workspace_directory, &binding.workspace_worktree_directory, &binding.workspace_source_directory, &binding.routing_mode, &binding.routing_source_folder_id, &binding.routing_source_directory, &binding.effective_model_id, &binding.model_identity_source, &binding.reasoning_effort, &binding.service_tier, &binding.approval_mode, &binding.command_safety_tier, &binding.native_session_id})
					if (!add(*value))
						return {};
				if (!add(binding.service_tier_explicit ? "1" : "0"))
					return {};
			}
			return hash.Finish();
		}

		bool SameHistoryChat(const ChatSession& saved, const ChatSession& expected)
		{
			return saved.id == expected.id && saved.agent_run_id == expected.agent_run_id && saved.managed_followup_incarnation == expected.managed_followup_incarnation && saved.provider_id == expected.provider_id && saved.model_id == expected.model_id && saved.native_session_id == expected.native_session_id && saved.execution_host_id == expected.execution_host_id && saved.workspace_directory == expected.workspace_directory && saved.workspace_worktree_directory == expected.workspace_worktree_directory && saved.folder_id == expected.folder_id && saved.created_at == expected.created_at && saved.updated_at == expected.updated_at && (saved.messages_loaded ? saved.messages.size() : saved.persisted_message_count) == expected.persisted_message_count && saved.persisted_messages_digest == expected.persisted_messages_digest && !saved.imported_read_only && !saved.side_cleanup_requested;
		}

		bool SameHistoryMetadata(const AgentRun& cached, bool binding_present, const AgentRun& current)
		{
			return InitialHistoryShape(current) && SameLedgerRecord(cached, current) && cached.managed_continuation == HistoryContinuation(current) && binding_present == current.managed_continuation->execution.has_value() && cached.effective_workspace_access == current.effective_workspace_access && cached.result_delivery_id == current.result_delivery_id && cached.deliver_result_to_root_chat == current.deliver_result_to_root_chat && cached.root_result_delivered == current.root_result_delivered && cached.root_result_delivery_attempts == current.root_result_delivery_attempts && cached.diagnostic_code == current.diagnostic_code;
		}

		bool HistoryBindingMatchesTranscript(const AgentRun& run, const ChatSession& transcript)
		{
			if (!run.managed_continuation || !run.managed_continuation->execution)
				return true; // Existing unbound/v1 inspection semantics are unchanged.
			const auto& binding = *run.managed_continuation->execution;
			// Worker-only comparison against the ORIGINAL canonical binding, never the sanitized cache.
			return transcript.messages_loaded && transcript.id == run.transcript_chat_id && transcript.agent_run_id == run.id && transcript.provider_id == run.provider_id && transcript.managed_followup_incarnation == run.managed_continuation->owner.transcript_birth_id && transcript.native_session_id == binding.native_session_id && transcript.execution_host_id == binding.execution_host_id && transcript.workspace_directory == binding.workspace_directory && transcript.workspace_worktree_directory == binding.workspace_worktree_directory && transcript.workspace_source_directory == binding.workspace_source_directory && transcript.routing_mode == binding.routing_mode && transcript.routing_source_folder_id == binding.routing_source_folder_id && transcript.routing_source_directory == binding.routing_source_directory && transcript.model_id == binding.effective_model_id && transcript.reasoning_effort == binding.reasoning_effort && transcript.service_tier == binding.service_tier && transcript.service_tier_explicit == binding.service_tier_explicit && transcript.approval_mode == binding.approval_mode && transcript.command_safety_tier == binding.command_safety_tier;
		}

		std::optional<ChatSession> ReadHistoryPrimary(const std::filesystem::path& root, const std::string& id)
		{
			if (!SafeChatId(id))
				return {};
			const auto path = AppPaths::UamChatFilePath(root, id);
			if (paths::IsLinkOrReparsePointNoThrow(path) || paths::IsLinkOrReparsePointNoThrow(path.parent_path()))
				return {};
			std::error_code error;
			if (!std::filesystem::is_regular_file(path, error) || error)
				return {};
			const auto size = std::filesystem::file_size(path, error);
			if (error || size > 4U * 1024U * 1024U)
				return {};
			const auto modified = std::filesystem::last_write_time(path, error);
			if (error)
				return {};
			auto chat = ChatRepository::LoadLocalChatBounded(root, id, 4U * 1024U * 1024U);
			if (!chat || chat->id != id || std::filesystem::file_size(path, error) != size || error || std::filesystem::last_write_time(path, error) != modified || error)
				return {};
			return chat;
		}

		struct Index
		{
			const ChatSession* root = nullptr;
			std::unordered_set<std::string> inspection;
			std::unordered_map<std::string, const AgentRun*> runs;
			std::unordered_map<std::string, const ChatSession*> chats;
			std::unordered_map<std::string, const ChatSession*> archived;
			std::unordered_map<std::string, const AcpSessionState*> sessions;
			std::unordered_map<std::string, std::vector<const AgentRun*>> children;
			std::unordered_map<std::string, std::size_t> transcript_uses;
		};

		bool BuildIndex(const AppState& app, const std::string& root_id, Index& index, std::string* error_out)
		{
			if (!SafeChatId(root_id) || app.data_root.empty() || app.agent_runs.size() > ManagedSubagentControlService::MaxLedgerRuns || app.chats.size() > ManagedSubagentControlService::MaxChatMetadata || app.hidden_agent_chat_ownership.chats.size() > ManagedSubagentControlService::MaxChatMetadata || app.acp_sessions.size() > ManagedSubagentControlService::MaxLedgerRuns)
				return Fail(error_out, "Managed subagent scope is invalid or exceeds its bounded inventory.");
			for (const ChatSession& chat : app.chats)
				if (!index.chats.emplace(chat.id, &chat).second)
					return Fail(error_out, "Managed chat identity is ambiguous.");
			const auto root = index.chats.find(root_id);
			if (root == index.chats.end() || !root->second->agent_run_id.empty() || !root->second->temporary_parent_chat_id.empty() || !root->second->goal_owner_chat_id.empty() || root->second->metadata_incarnation == nullptr || root->second->created_at.size() > 80)
				return Fail(error_out, "Managed subagents require an available visible root chat.");
			index.root = root->second;
			for (const AgentRun& run : app.agent_runs)
			{
				if (!BoundedMetadata(run) || !uuid::IsCanonicalUuid(run.id) || !index.runs.emplace(run.id, &run).second)
					return Fail(error_out, "Managed run identity is invalid or ambiguous.");
				++index.transcript_uses[run.transcript_chat_id];
				if (!run.parent_run_id.empty())
					index.children[run.parent_run_id].push_back(&run);
			}
			for (const auto& session : app.acp_sessions)
				if (session && !index.sessions.emplace(session->chat_id, session.get()).second)
					return Fail(error_out, "Managed session identity is ambiguous.");
			if (app.hidden_agent_chat_ownership.data_root == app.data_root)
				for (const ChatSession& chat : app.hidden_agent_chat_ownership.chats)
					if (!index.archived.emplace(chat.id, &chat).second)
						return Fail(error_out, "Archived managed chat identity is ambiguous.");

			const auto history = app.managed_initial_history;
			if (history && history->data_root == app.data_root)
			{
				if (!history->error.empty())
					return Fail(error_out, "Saved managed initial history is unavailable.");
				std::unordered_set<std::string> transcript_ids;
				for (const auto& [id, run] : index.runs)
					transcript_ids.insert(run->transcript_chat_id);
				for (const auto& entry : history->entries)
				{
					const auto& run = entry.metadata;
					const auto live = index.runs.find(run.id);
					if (live != index.runs.end())
					{
						const auto child = index.chats.find(run.transcript_chat_id);
						if (!SameHistoryMetadata(run, entry.binding_present, *live->second) || child == index.chats.end() || !SameHistoryChat(*child->second, entry.transcript))
							return Fail(error_out, "Live and saved managed identities conflict.");
						continue;
					}
					if (!transcript_ids.insert(run.transcript_chat_id).second)
						return Fail(error_out, "Saved managed transcript identity is ambiguous.");
					if (transcript_ids.size() > ManagedSubagentControlService::MaxLedgerRuns)
						return Fail(error_out, "Managed history exceeds its bounded inventory.");
					if (run.root_chat_id != root_id)
						continue;
					if (index.root->side_cleanup_requested || app.worktree_operation_chat_ids.contains(root_id) || app.worktree_operation_chat_ids.contains(run.transcript_chat_id))
						return Fail(error_out, "Saved managed history is being deleted.");
					if (index.root->managed_followup_incarnation != run.managed_continuation->owner.root_birth_id)
						return Fail(error_out, "Saved managed parent identity changed.");
					if (!entry.error.empty())
						return Fail(error_out, "Saved managed conversation metadata is unavailable.");
					const auto child = index.chats.find(run.transcript_chat_id);
					const auto archived = index.archived.find(run.transcript_chat_id);
					const ChatSession* current = child != index.chats.end() ? child->second : archived != index.archived.end() ? archived->second : nullptr;
					if (!current || !SameHistoryChat(*current, entry.transcript))
						return Fail(error_out, "Saved managed conversation identity changed.");
					index.runs.emplace(run.id, &run);
					index.inspection.insert(run.id);
					if (index.runs.size() > ManagedSubagentControlService::MaxLedgerRuns)
						return Fail(error_out, "Managed history exceeds its bounded inventory.");
				}
			}
			return true;
		}

		bool Owned(const Index& index, const AgentRun& run)
		{
			if (run.root_chat_id != index.root->id)
				return false;
			const AgentRun* cursor = &run;
			for (int depth = 0; depth < 2; ++depth)
			{
				if (cursor->root_chat_id != index.root->id)
					return false;
				// Ledger v1 has no persisted root incarnation. Reject provably older local-time runs;
				// the server-held scope additionally rejects root replacement during this app lifetime.
				if (index.root->created_at.size() == 19 && cursor->created_at.size() == 19 && index.root->created_at[10] == ' ' && cursor->created_at[10] == ' ' && cursor->created_at < index.root->created_at)
					return false;
				if (cursor->parent_run_id.empty())
					return cursor->depth == 1 && run.depth == depth + 1;
				const auto parent = index.runs.find(cursor->parent_run_id);
				if (parent == index.runs.end())
					return false;
				cursor = parent->second;
			}
			return false;
		}

		bool ModelMatches(const Index& index, const ChatSession& chat, const AgentRun& run)
		{
			if (chat.model_id == run.model_id)
				return true;
			const auto session = index.sessions.find(chat.id);
			if (provider_ids::NormalizeCliProviderAliasOrSelf(run.provider_id) != provider_ids::kOpenCodeCli || session == index.sessions.end() || session->second->managed_agent_run_id != run.id || session->second->provider_id != provider_ids::kOpenCodeCli)
				return false;
			std::string error;
			const auto resolved = acp_models::ResolveAdvertisedModelId(*session->second, run.model_id, error);
			return resolved && *resolved == chat.model_id;
		}

		bool TranscriptMatches(const Index& index, const ChatSession& chat, const AgentRun& run)
		{
			return chat.id == run.transcript_chat_id && chat.agent_run_id == run.id && provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id) == provider_ids::NormalizeCliProviderAliasOrSelf(run.provider_id) && ModelMatches(index, chat, run) && !chat.imported_read_only;
		}

		std::string TranscriptKey(const ChatSession& chat)
		{
			// Model ownership is the immutable requested ID plus the actual runtime resolver above.
			return nlohmann::json::array({chat.id, chat.agent_run_id, provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id), chat.execution_host_id, chat.created_at, chat.workspace_directory, chat.folder_id}).dump();
		}

		// Traverse the same raw parent edges as CancelTree, including malformed cross-root edges.
		std::vector<const AgentRun*> Tree(const Index& index, const AgentRun& selected)
		{
			std::vector<const AgentRun*> tree;
			std::deque<const AgentRun*> pending{&selected};
			std::unordered_set<std::string> seen;
			while (!pending.empty())
			{
				const AgentRun* run = pending.front();
				pending.pop_front();
				if (!seen.insert(run->id).second)
					continue;
				tree.push_back(run);
				const auto children = index.children.find(run->id);
				if (children != index.children.end())
					for (const AgentRun* child : children->second)
						pending.push_back(child);
			}
			return tree;
		}

		bool CanStopTree(const Index& index, const std::vector<const AgentRun*>& tree)
		{
			for (const AgentRun* run : tree)
			{
				if (index.inspection.contains(run->id) || !Owned(index, *run))
					return false;
				if (Terminal(*run))
					continue;
				const auto chat = index.chats.find(run->transcript_chat_id);
				const auto uses = index.transcript_uses.find(run->transcript_chat_id);
				if (!SafeChatId(run->transcript_chat_id) || run->transcript_chat_id == index.root->id || uses == index.transcript_uses.end() || uses->second != 1 || chat == index.chats.end() || !TranscriptMatches(index, *chat->second, *run))
					return false;
				const auto session = index.sessions.find(run->transcript_chat_id);
				if (session != index.sessions.end() && (session->second->managed_agent_run_id != run->id || (provider_ids::NormalizeCliProviderAliasOrSelf(session->second->provider_id) != provider_ids::NormalizeCliProviderAliasOrSelf(run->provider_id)) || (!session->second->process_execution_host_id.empty() && session->second->process_execution_host_id != chat->second->execution_host_id) || (run->expected_turn_serial > 0 && session->second->turn_serial != run->expected_turn_serial)))
					return false;
			}
			return true;
		}

		std::optional<std::string> Host(const Index& index, const AgentRun& run)
		{
			const auto live = index.chats.find(run.transcript_chat_id);
			if (live != index.chats.end())
				return TranscriptMatches(index, *live->second, run) && SafeChatId(live->second->execution_host_id) ? std::optional<std::string>{live->second->execution_host_id} : std::nullopt;
			const auto archived = index.archived.find(run.transcript_chat_id);
			return archived != index.archived.end() && TranscriptMatches(index, *archived->second, run) && SafeChatId(archived->second->execution_host_id) ? std::optional<std::string>{archived->second->execution_host_id} : std::nullopt;
		}

		AgentRun Metadata(const AgentRun& run, bool history_entry = false)
		{
			AgentRun copy;
			copy.id = run.id;
			copy.task = TaskTitle(run); // Retain only the bounded display label for detached title validation.
			copy.root_chat_id = run.root_chat_id;
			copy.parent_run_id = run.parent_run_id;
			copy.resumed_from_run_id = run.resumed_from_run_id;
			copy.transcript_chat_id = run.transcript_chat_id;
			copy.agent_id = run.agent_id;
			copy.provider_id = run.provider_id;
			copy.model_id = run.model_id;
			copy.execution_capability = run.execution_capability;
			copy.definition_hash = run.definition_hash;
			copy.goal_id = run.goal_id;
			copy.depth = run.depth;
			copy.created_at = run.created_at;
			copy.started_at = run.started_at;
			copy.finished_at = run.finished_at;
			copy.updated_at = run.updated_at;
			copy.status = run.status;
			copy.expected_turn_serial = run.expected_turn_serial;
			copy.started_at_epoch_ms = run.started_at_epoch_ms;
			copy.finished_at_epoch_ms = run.finished_at_epoch_ms;
			copy.deadline_at_epoch_ms = run.deadline_at_epoch_ms;
			// A catalogue entry is already a sanitized projection, not a full executable codec value.
			if (history_entry || InitialHistoryShape(run))
				copy.managed_continuation = HistoryContinuation(run);
			return copy;
		}

		std::optional<std::string> ReadArchivedHost(const std::filesystem::path& data_root, const ManagedSubagentRun& run, std::size_t& bytes_read)
		{
			using Service = ManagedSubagentControlService;
			if (!SafeChatId(run.transcript_chat_id))
				return std::nullopt;
			const auto path = AppPaths::UamChatFilePath(data_root, run.transcript_chat_id);
			if (paths::IsLinkOrReparsePointNoThrow(path.parent_path()) || paths::IsLinkOrReparsePointNoThrow(path))
				return std::nullopt;
			std::error_code error;
			if (!std::filesystem::is_regular_file(path, error) || error)
				return std::nullopt;
			const auto size = std::filesystem::file_size(path, error);
			if (error || size == 0 || size >= Service::MaxArchivedMetadataBytes || size + 1 > Service::MaxArchiveTotalBytes - bytes_read)
				return std::nullopt;
			const auto modified = std::filesystem::last_write_time(path, error);
			if (error)
				return std::nullopt;
			std::ifstream input(path, std::ios::binary);
			if (!input)
				return std::nullopt;
			std::string text(static_cast<std::size_t>(size) + 1, '\0');
			input.read(text.data(), static_cast<std::streamsize>(text.size()));
			const auto actual = static_cast<std::size_t>(input.gcount());
			bytes_read += actual;
			if (actual != size || std::filesystem::file_size(path, error) != size || error || std::filesystem::last_write_time(path, error) != modified || error)
				return std::nullopt;
			text.resize(actual);
			// Decode only ownership metadata. No ChatRepository loader, message objects, summary,
			// backup, queue journal, cache publication or recovery side effect is reachable here.
			const std::unordered_set<std::string> allowed = {"id", "agent_run_id", "provider_id", "model_id", "execution_host_id", "imported_read_only"};
			std::unordered_set<std::string> seen;
			bool duplicate = false;
			const auto value = nlohmann::json::parse(
			    text,
			    [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& item)
			    {
				    if (event != nlohmann::json::parse_event_t::key)
					    return true;
				    const auto key = item.get<std::string>();
				    if (depth != 1 || !allowed.contains(key))
					    return false;
				    if (!seen.insert(key).second)
					    duplicate = true;
				    return true;
			    },
			    false);
			if (duplicate || !value.is_object())
				return std::nullopt;
			const auto string_is = [&](const char* key, const std::string& expected)
			{
				const auto item = value.find(key);
				return item != value.end() && item->is_string() && item->get_ref<const std::string&>() == expected;
			};
			if (!string_is("id", run.transcript_chat_id) || !string_is("agent_run_id", run.run_id) || !string_is("provider_id", run.provider_id) || !string_is("model_id", run.model_id))
				return std::nullopt;
			if (const auto readonly = value.find("imported_read_only"); readonly != value.end() && (!readonly->is_boolean() || readonly->get<bool>()))
				return std::nullopt;
			const auto host = value.find("execution_host_id");
			if (host == value.end() || !host->is_string() || !SafeChatId(host->get_ref<const std::string&>()))
				return std::nullopt;
			return host->get<std::string>();
		}

	} // namespace

	void ManagedSubagentControlService::LoadRestartInspection(AppState& app)
	{
		if (app.managed_initial_history && app.managed_initial_history->data_root == app.data_root)
			return;
		auto history = std::make_shared<ManagedInitialHistoryCatalogue>();
		history->data_root = app.data_root;
		try
		{
			if (app.chats.size() > MaxChatMetadata || app.hidden_agent_chat_ownership.chats.size() > MaxChatMetadata)
			{
				history->error = "Saved managed metadata exceeds its bounded inventory.";
				app.managed_initial_history = std::move(history);
				return;
			}
			std::unordered_map<std::string, const ChatSession*> captured_chats;
			if (app.hidden_agent_chat_ownership.data_root == app.data_root)
				for (const auto& chat : app.hidden_agent_chat_ownership.chats)
					if (!captured_chats.emplace(chat.id, &chat).second)
						captured_chats[chat.id] = nullptr;
			const auto ledger = AgentRunLedger::LoadAllReadOnly(app.data_root);
			if (!ledger.errors.empty())
				history->error = "Saved managed initial ledger is unavailable or invalid.";
			else
				for (const auto& run : ledger.runs)
				{
					if (!InitialHistoryShape(run))
						continue;
					ManagedInitialHistoryCatalogue::Entry entry;
					entry.metadata = Metadata(run);
					entry.binding_present = run.managed_continuation->execution.has_value();
					entry.metadata.effective_workspace_access = run.effective_workspace_access;
					entry.metadata.result_delivery_id = run.result_delivery_id;
					entry.metadata.deliver_result_to_root_chat = run.deliver_result_to_root_chat;
					entry.metadata.root_result_delivered = run.root_result_delivered;
					entry.metadata.root_result_delivery_attempts = run.root_result_delivery_attempts;
					entry.metadata.result_excerpt = CardSummary(run);
					entry.metadata.diagnostic_code = run.diagnostic_code;
					entry.private_body_digest = PrivateHistoryDigest(run);
					const auto found = captured_chats.find(run.transcript_chat_id);
					const ChatSession* captured = found != captured_chats.end() ? found->second : nullptr;
					if (!captured || !entry.error.empty() || entry.private_body_digest.empty())
						entry.error = "Saved transcript metadata is unavailable.";
					else
					{
						const auto& chat = *captured;
						entry.transcript.id = chat.id;
						entry.transcript.agent_run_id = chat.agent_run_id;
						entry.transcript.managed_followup_incarnation = chat.managed_followup_incarnation;
						entry.transcript.provider_id = chat.provider_id;
						entry.transcript.model_id = chat.model_id;
						entry.transcript.native_session_id = chat.native_session_id;
						entry.transcript.execution_host_id = chat.execution_host_id;
						entry.transcript.workspace_directory = chat.workspace_directory;
						entry.transcript.workspace_worktree_directory = chat.workspace_worktree_directory;
						entry.transcript.folder_id = chat.folder_id;
						entry.transcript.created_at = chat.created_at;
						entry.transcript.updated_at = chat.updated_at;
						entry.transcript.messages_loaded = false;
						entry.transcript.persisted_message_count = chat.persisted_message_count;
						entry.transcript.persisted_messages_digest = chat.persisted_messages_digest;
						if (chat.agent_run_id != run.id || chat.provider_id != run.provider_id || chat.managed_followup_incarnation != run.managed_continuation->owner.transcript_birth_id)
							entry.error = "Saved transcript birth or provider changed.";
					}
					history->entries.push_back(std::move(entry));
				}
		}
		catch (...)
		{
			history->entries.clear();
			history->error = "Saved managed initial history could not be read.";
		}
		app.managed_initial_history = std::move(history); // Empty/error results are latched too.
	}

	void ManagedSubagentControlService::AddRestartInspectionCounts(const AppState& app, std::unordered_map<std::string, std::size_t>& counts)
	{
		const auto history = app.managed_initial_history;
		if (!history || history->data_root != app.data_root || !history->error.empty() || app.agent_runs.size() > MaxLedgerRuns || history->entries.size() > MaxLedgerRuns || app.chats.size() > MaxChatMetadata || app.hidden_agent_chat_ownership.chats.size() > MaxChatMetadata)
			return;
		std::unordered_map<std::string, const ChatSession*> roots, children;
		std::unordered_map<std::string, const AgentRun*> runs, transcripts;
		std::unordered_set<std::string> conflicts;
		for (const auto& chat : app.chats)
		{
			if (!roots.emplace(chat.id, &chat).second)
				roots[chat.id] = nullptr;
			if (!children.emplace(chat.id, &chat).second)
				children[chat.id] = nullptr;
		}
		for (const auto& run : app.agent_runs)
		{
			if (!runs.emplace(run.id, &run).second)
				runs[run.id] = nullptr;
			if (!transcripts.emplace(run.transcript_chat_id, &run).second)
				transcripts[run.transcript_chat_id] = nullptr;
		}
		if (app.hidden_agent_chat_ownership.data_root == app.data_root)
			for (const auto& chat : app.hidden_agent_chat_ownership.chats)
			{
				const auto prior = children.find(chat.id);
				if (prior != children.end())
				{
					if (!prior->second || !SameHistoryChat(*prior->second, chat))
						children[chat.id] = nullptr;
				}
				else
					children.emplace(chat.id, &chat);
			}
		std::unordered_set<std::string> all_ids;
		for (const auto& [id, run] : runs)
			all_ids.insert(id);
		for (const auto& entry : history->entries)
			all_ids.insert(entry.metadata.id);
		if (all_ids.size() > MaxLedgerRuns)
			return;
		std::unordered_map<std::string, std::size_t> extras;
		std::unordered_set<std::string> saved_transcripts;
		for (const auto& entry : history->entries)
		{
			const auto& run = entry.metadata;
			const auto root = roots.find(run.root_chat_id);
			if (root == roots.end() || !root->second || !root->second->agent_run_id.empty() || root->second->managed_followup_incarnation != run.managed_continuation->owner.root_birth_id)
				continue;
			const auto current_child = children.find(run.transcript_chat_id);
			if (current_child != children.end() && (!current_child->second || !SameHistoryChat(*current_child->second, entry.transcript)))
			{
				conflicts.insert(run.root_chat_id);
				continue;
			}
			const auto live = runs.find(run.id);
			if (live != runs.end())
			{
				const auto child = children.find(run.transcript_chat_id);
				if (!live->second || !SameHistoryMetadata(run, entry.binding_present, *live->second) || child == children.end() || !child->second || !SameHistoryChat(*child->second, entry.transcript))
					conflicts.insert(run.root_chat_id);
				continue;
			}
			if (transcripts.contains(run.transcript_chat_id) || !saved_transcripts.insert(run.transcript_chat_id).second)
			{
				conflicts.insert(run.root_chat_id);
				continue;
			}
			++extras[run.root_chat_id]; // Pure hint, including valid ledger rows whose body is unavailable.
		}
		for (const auto& [root, extra] : extras)
			if (!conflicts.contains(root) && extra <= MaxLedgerRuns && counts[root] <= MaxLedgerRuns - extra)
				counts[root] += extra;
	}

	bool ManagedSubagentControlService::CaptureRestartTranscript(const AppState& app, std::string_view root_id, std::string_view transcript_id, ManagedAgentTranscriptReadIdentity& identity)
	{
		const auto history = app.managed_initial_history;
		if (!history || history->data_root != app.data_root)
			return false;
		const auto candidate = std::ranges::find_if(history->entries, [&](const auto& value) { return value.metadata.root_chat_id == root_id && value.metadata.transcript_chat_id == transcript_id; });
		if (candidate == history->entries.end())
			return false;
		const auto unavailable = [&]()
		{
			identity.history_error = "Saved managed conversation metadata or ownership is unavailable.";
			return true;
		};
		Index index;
		if (!BuildIndex(app, std::string(root_id), index, nullptr))
			return unavailable();
		const AgentRun* selected = nullptr;
		for (const auto& [id, run] : index.runs)
			if (index.inspection.contains(id) && run->root_chat_id == root_id && run->transcript_chat_id == transcript_id)
			{
				if (selected)
					return false;
				selected = run;
			}
		if (!selected)
			return false; // An exact live runtime identity retains its original read path.
		if (app.worktree_operation_chat_ids.contains(selected->root_chat_id) || app.worktree_operation_chat_ids.contains(selected->transcript_chat_id))
			return unavailable();
		auto snapshot = std::make_shared<ManagedSubagentSnapshot>();
		if (!Capture(app, std::string(root_id), snapshot.get(), nullptr, selected->id, 0, 1))
			return unavailable();
		snapshot->wants_history_transcript = true;
		identity.initial_history = std::move(snapshot);
		identity.history_data_root = app.data_root;
		identity.root_chat_id = std::string(root_id);
		identity.transcript_chat_id = std::string(transcript_id);
		identity.run_id = selected->id;
		identity.run_agent_id = selected->agent_id;
		identity.run_provider_id = selected->provider_id;
		identity.run_status = selected->status;
		identity.run_execution_capability = selected->execution_capability;
		identity.history_transcript_birth = selected->managed_continuation->owner.transcript_birth_id;
		return true;
	}

	bool ManagedSubagentControlService::Capture(const AppState& app, const std::string& visible_root_chat_id, ManagedSubagentSnapshot* snapshot_out, std::string* error_out, const std::string& run_id, std::size_t offset, std::size_t maximum_count)
	{
		if (snapshot_out != nullptr)
			*snapshot_out = {};
		if (error_out != nullptr)
			error_out->clear();
		if (!snapshot_out || maximum_count == 0 || maximum_count > MaxPageSize || offset > MaxLedgerRuns || (!run_id.empty() && !uuid::IsCanonicalUuid(run_id)))
			return Fail(error_out, "Managed subagent page or run selection is invalid.");
		Index index;
		if (!BuildIndex(app, visible_root_chat_id, index, error_out))
			return false;
		ManagedSubagentSnapshot snapshot;
		snapshot.selected_run_id = run_id;
		snapshot.offset = offset;
		snapshot.limit = maximum_count;
		auto& page = snapshot.page;
		page.scope.app_incarnation = app.managed_subagent_control_incarnation;
		page.scope.data_root = app.data_root;
		page.scope.root_chat_id = visible_root_chat_id;
		page.scope.root_created_at = index.root->created_at;
		page.scope.root_incarnation = index.root->metadata_incarnation;
		if (!index.inspection.empty())
		{
			page.scope.history_catalogue = app.managed_initial_history;
			page.scope.root_birth = index.root->managed_followup_incarnation;
		}
		std::vector<const AgentRun*> owned;
		nlohmann::json signature = {{"readOnly", index.root->imported_read_only}, {"runs", nlohmann::json::object()}};
		for (const auto& [view_id, view_run] : index.runs)
		{
			const AgentRun& run = *view_run;
			if (run.root_chat_id != visible_root_chat_id)
				continue;
			if (!Owned(index, run))
				return Fail(error_out, "Managed run lineage changed.");
			owned.push_back(&run);
			snapshot.records.push_back(Metadata(run, index.inspection.contains(run.id)));
			if (index.inspection.contains(run.id))
				snapshot.history_ids.insert(run.id);
			const auto transcript = index.chats.find(run.transcript_chat_id);
			const auto archived = index.archived.find(run.transcript_chat_id);
			const ChatSession* chat = transcript != index.chats.end() ? transcript->second : archived != index.archived.end() ? archived->second : nullptr;
			if (chat && (chat->execution_host_id.size() > 160 || chat->workspace_directory.size() > 4096 || chat->folder_id.size() > 160 || chat->created_at.size() > 80 || chat->provider_id.size() > 160 || chat->agent_run_id.size() > 160 || chat->model_id.size() > 160))
				return Fail(error_out, "Managed transcript metadata exceeds its bounded scope.");
			page.scope.runs.emplace(run.id, ManagedSubagentControlScope::RunOwnership{OwnershipKey(run), chat ? TranscriptKey(*chat) : std::string{}, chat ? chat->metadata_incarnation : nullptr, run.managed_continuation ? run.managed_continuation->owner.root_birth_id : std::string{}, run.managed_continuation ? run.managed_continuation->owner.transcript_birth_id : std::string{}});
			signature["runs"][run.id] = nlohmann::json::array({run.status, run.updated_at, run.started_at, run.finished_at, run.started_at_epoch_ms, run.finished_at_epoch_ms, run.deadline_at_epoch_ms, run.expected_turn_serial, chat ? TranscriptMatches(index, *chat, run) : false, CanStopTree(index, Tree(index, run))});
		}
		if (!run_id.empty())
		{
			const auto selected = index.runs.find(run_id);
			if (selected == index.runs.end() || !Owned(index, *selected->second))
				return Fail(error_out, "Managed run is outside the visible root chat.");
			std::unordered_set<std::string> selected_ids;
			for (const AgentRun* run : Tree(index, *selected->second))
				selected_ids.insert(run->id);
			std::erase_if(owned, [&](const AgentRun* run) { return !selected_ids.contains(run->id); });
		}
		std::ranges::sort(owned, [](const AgentRun* left, const AgentRun* right) { return left->created_at != right->created_at ? left->created_at < right->created_at : left->id < right->id; });
		page.total_count = owned.size();
		page.offset = offset;
		const std::size_t end = std::min(owned.size(), offset + maximum_count);
		for (std::size_t i = offset; i < end; ++i)
		{
			const AgentRun& run = *owned[i];
			page.runs.push_back({run.id, run.parent_run_id, run.agent_id, run.transcript_chat_id, run.status, run.provider_id, run.model_id, Host(index, run), run.created_at, run.started_at, run.finished_at, run.started_at_epoch_ms, run.finished_at_epoch_ms, run.execution_capability, !index.root->imported_read_only && !Terminal(run) && CanStopTree(index, Tree(index, run)), CardSummary(run), index.inspection.contains(run.id), TaskTitle(run)});
			// Only the bounded visible page carries output; the whole ledger snapshot
			// copies only the bounded task title, never full task or result bodies.
			signature["summaries"][run.id] = page.runs.back().summary;
		}
		if (end < owned.size())
			page.next_offset = end;
		snapshot.signature = signature.dump();
		*snapshot_out = std::move(snapshot);
		return true;
	}

	ManagedSubagentDetachedRead ManagedSubagentControlService::ReadDetached(const ManagedSubagentSnapshot& snapshot, bool read_archives, const std::function<void(const std::filesystem::path&)>& after_primary_size_for_tests)
	{
		ManagedSubagentDetachedRead result;
		if (!snapshot.page.scope.app_incarnation || snapshot.signature.empty())
		{
			result.error = "Managed subagent snapshot is unavailable.";
			return result;
		}
		result.page = snapshot.page;
		const auto ledger = AgentRunLedger::LoadAllReadOnly(snapshot.page.scope.data_root, after_primary_size_for_tests);
		if (!ledger.errors.empty() || ledger.runs.size() > MaxLedgerRuns)
		{
			result.error = "Managed run ledger is unavailable or invalid.";
			return result;
		}
		std::unordered_map<std::string, const AgentRun*> saved;
		for (const AgentRun& run : ledger.runs)
			saved.emplace(run.id, &run);
		for (const AgentRun& run : snapshot.records)
		{
			const auto stored = saved.find(run.id);
			if (stored == saved.end() || !SameLedgerRecord(run, *stored->second))
			{
				result.error = "Managed run ledger ownership changed.";
				return result;
			}
		}
		for (const ManagedSubagentRun& row : result.page.runs)
		{
			const auto stored = saved.find(row.run_id);
			if (stored == saved.end() || row.summary != CardSummary(*stored->second) || row.task_title != TaskTitle(*stored->second))
			{
				result.error = "Managed run ownership changed while reading.";
				return result;
			}
		}
		std::optional<ChatSession> history_root;
		for (auto& row : result.page.runs)
		{
			if (!row.last_reported)
				continue;
			const auto catalogue = snapshot.page.scope.history_catalogue;
			if (!catalogue || catalogue->data_root != snapshot.page.scope.data_root)
			{
				result.error = "Saved managed history is unavailable.";
				return result;
			}
			const auto entry = std::ranges::find_if(catalogue->entries, [&](const auto& value) { return value.metadata.id == row.run_id; });
			const auto primary = saved.find(row.run_id);
			if (entry == catalogue->entries.end() || primary == saved.end() || !SameHistoryMetadata(entry->metadata, entry->binding_present, *primary->second) || entry->private_body_digest.empty() || entry->private_body_digest != PrivateHistoryDigest(*primary->second))
			{
				result.error = "Saved managed ledger identity changed.";
				return result;
			}
			if (!history_root)
				history_root = ReadHistoryPrimary(catalogue->data_root, snapshot.page.scope.root_chat_id);
			auto transcript = ReadHistoryPrimary(catalogue->data_root, row.transcript_chat_id);
			if (!history_root || !transcript || !history_root->agent_run_id.empty() || history_root->managed_followup_incarnation != entry->metadata.managed_continuation->owner.root_birth_id || history_root->managed_followup_incarnation != snapshot.page.scope.root_birth || !SameHistoryChat(*transcript, entry->transcript) || !HistoryBindingMatchesTranscript(*primary->second, *transcript))
			{
				result.error = "Saved managed conversation is unavailable, exceeds 4 MiB, or its birth/metadata changed.";
				return result;
			}
			row.can_cancel = false;
			row.host = SafeChatId(transcript->execution_host_id) ? std::optional<std::string>{transcript->execution_host_id} : std::nullopt;
			if (snapshot.wants_history_transcript && snapshot.selected_run_id == row.run_id)
				result.history_transcript = std::move(*transcript);
		} // Child bodies die here; ROOT was read once, never stored in a snapshot/catalogue/page.
		history_root.reset();
		if (read_archives)
			for (ManagedSubagentRun& run : result.page.runs)
			{
				const auto owner = result.page.scope.runs.find(run.run_id);
				if (!run.host && owner != result.page.scope.runs.end() && !owner->second.transcript_incarnation)
					run.host = ReadArchivedHost(snapshot.page.scope.data_root, run, result.archive_bytes_read);
			}
		result.ready = true;
		return result;
	}

	bool ManagedSubagentControlService::MatchesSnapshot(const AppState& app, const ManagedSubagentSnapshot& snapshot)
	{
		ManagedSubagentSnapshot current;
		return Capture(app, snapshot.page.scope.root_chat_id, &current, nullptr, snapshot.selected_run_id, snapshot.offset, snapshot.limit) && current.page.scope == snapshot.page.scope && current.signature == snapshot.signature;
	}

	bool ManagedSubagentControlService::List(const AppState& app, const std::string& visible_root_chat_id, ManagedSubagentPage* page_out, std::string* error_out, const std::string& run_id, std::size_t offset, std::size_t maximum_count)
	{
		if (!page_out)
			return Fail(error_out, "Managed subagent output is missing.");
		*page_out = {};
		ManagedSubagentSnapshot snapshot;
		if (!Capture(app, visible_root_chat_id, &snapshot, error_out, run_id, offset, maximum_count))
			return false;
		auto result = ReadDetached(snapshot);
		if (!result.ready)
		{
			if (error_out)
				*error_out = result.error;
			return false;
		}
		if (!MatchesSnapshot(app, snapshot))
			return Fail(error_out, "Managed subagent ownership changed during the read.");
		*page_out = std::move(result.page);
		return true;
	}

	bool ManagedSubagentControlService::ValidateFollowupScopeImpl(const AppState& app, const std::string& root_id, const std::string& run_id, const ManagedSubagentControlScope& scope, bool completion, std::string* error_out)
	{
		if (error_out) error_out->clear();
		if (scope.app_incarnation != app.managed_subagent_control_incarnation || scope.data_root != app.data_root || scope.root_chat_id != root_id || !uuid::IsCanonicalUuid(run_id))
			return Fail(error_out, "Managed follow-up data owner changed; refresh the listing.");
		Index index;
		if (!BuildIndex(app, root_id, index, error_out)) return false;
		if (index.root->metadata_incarnation != scope.root_incarnation || index.root->created_at != scope.root_created_at || index.root->imported_read_only || index.root->side_cleanup_requested || app.worktree_operation_chat_ids.contains(root_id))
			return Fail(error_out, "Managed follow-up root was replaced, is read-only or is closing.");
		const auto selected = index.runs.find(run_id);
		const auto original = scope.runs.find(run_id);
		if (selected == index.runs.end() || original == scope.runs.end() || index.inspection.contains(run_id) || !Owned(index, *selected->second))
			return Fail(error_out, "Managed follow-up requires the original live owned run.");
		const auto& run = *selected->second;
		if (!run.managed_continuation || run.depth != 1 || !run.parent_run_id.empty() || index.transcript_uses[run.transcript_chat_id] != 1 || original->second.identity != OwnershipKey(run) || app.worktree_operation_chat_ids.contains(run.transcript_chat_id))
			return Fail(error_out, "Managed follow-up lineage changed or is unsupported.");
		const auto& owner = run.managed_continuation->owner;
		if (owner.delegation_grant || !uuid::IsCanonicalUuid(owner.root_birth_id) || !uuid::IsCanonicalUuid(owner.transcript_birth_id) || original->second.root_birth != owner.root_birth_id || original->second.transcript_birth != owner.transcript_birth_id || index.root->managed_followup_incarnation != owner.root_birth_id)
			return Fail(error_out, "Managed follow-up birth ownership changed.");
		const auto live = index.chats.find(run.transcript_chat_id);
		const auto retired = index.archived.find(run.transcript_chat_id);
		const auto* chat = live != index.chats.end() ? live->second : retired != index.archived.end() ? retired->second : nullptr;
		if (!chat || !chat->metadata_incarnation || !original->second.transcript_incarnation || !TranscriptMatches(index, *chat, run) || chat->side_cleanup_requested || chat->managed_followup_incarnation != owner.transcript_birth_id || original->second.transcript_identity != TranscriptKey(*chat) || (!completion && original->second.transcript_incarnation != chat->metadata_incarnation))
			return Fail(error_out, "Managed follow-up transcript was replaced; refresh the listing.");
		// Completion deliberately checks immutable lineage, not the old retired incarnation.
		// Only the scheduler's original hydrated runtime owner may grant subsequent input.
		return true;
	}

	bool ManagedSubagentControlService::ValidateFollowupScope(const AppState& app, const std::string& root_id, const std::string& run_id, const ManagedSubagentControlScope& scope, std::string* error_out)
	{
		return ValidateFollowupScopeImpl(app, root_id, run_id, scope, false, error_out);
	}

	bool ManagedSubagentControlService::ValidateFollowupCompletionScope(const AppState& app, const std::string& root_id, const std::string& run_id, const ManagedSubagentControlScope& scope, std::string* error_out)
	{
		return ValidateFollowupScopeImpl(app, root_id, run_id, scope, true, error_out);
	}

	bool ManagedSubagentControlService::CancelPrepared(AppState& app, const std::string& visible_root_chat_id, const std::string& run_id, const ManagedSubagentControlScope& scope, const ManagedSubagentSnapshot& snapshot, const ManagedSubagentDetachedRead& read, std::string* error_out)
	{
		if (error_out)
			error_out->clear();
		if (snapshot.history_ids.contains(run_id))
			return Fail(error_out, "Saved managed history is read-only; execution ownership was not restored.");
		if (!read.ready || read.page.scope != snapshot.page.scope || snapshot.selected_run_id != run_id || !MatchesSnapshot(app, snapshot) || scope.app_incarnation != app.managed_subagent_control_incarnation || scope.data_root != app.data_root || scope.root_chat_id != visible_root_chat_id)
			return Fail(error_out, "Managed subagent admission changed; refresh the listing.");
		Index index;
		if (!BuildIndex(app, visible_root_chat_id, index, error_out))
			return false;
		if (index.root->metadata_incarnation != scope.root_incarnation || index.root->created_at != scope.root_created_at || index.root->imported_read_only)
			return Fail(error_out, "Managed subagent root changed or is read-only.");
		const auto selected = index.runs.find(run_id);
		if (selected == index.runs.end() || !Owned(index, *selected->second))
			return Fail(error_out, "Managed run is outside the visible root chat.");
		const auto tree = Tree(index, *selected->second);
		if (!CanStopTree(index, tree))
			return Fail(error_out, "Managed subtree ownership changed.");
		auto receipt = std::make_shared<ManagedSubagentCancellationReceipt>();
		receipt->scope = scope;
		receipt->scope.runs.clear();
		receipt->selected_run_id = run_id;
		bool active = false;
		for (const AgentRun* run : tree)
		{
			const auto original = scope.runs.find(run->id);
			if (original == scope.runs.end() || original->second.identity != OwnershipKey(*run))
				return Fail(error_out, "Managed subtree lineage changed; refresh the listing.");
			receipt->scope.runs.emplace(*original);
			if (Terminal(*run))
				continue;
			active = true;
			const auto transcript = index.chats.find(run->transcript_chat_id);
			if (transcript == index.chats.end() || original->second.transcript_incarnation != transcript->second->metadata_incarnation || original->second.transcript_identity != TranscriptKey(*transcript->second))
				return Fail(error_out, "Managed subtree transcript was replaced; refresh the listing.");
		}
		if (!active)
			return true;
		// This is a NEW explicit Stop, independently checked against its current server-held scope
		// and durable snapshot above. Supersede old retry authority only after the entire new tree
		// passes admission; old scopes/retries never acquire authority over changed owners.
		for (const AgentRun* run : tree)
		{
			if (!Terminal(*run))
				app.managed_subagent_cancellation_receipts.insert_or_assign(run->id, receipt);
			else
				app.managed_subagent_cancellation_receipts.erase(run->id);
		}
		std::string scheduler_error;
		if (!AgentRunScheduler::CancelTree(app, run_id, &scheduler_error))
			return Fail(error_out, "Managed cancellation is pending; UAM will retry its stop intent.");
		return true;
	}

	bool ManagedSubagentControlService::Cancel(AppState& app, const std::string& visible_root_chat_id, const std::string& run_id, const ManagedSubagentControlScope& scope, std::string* error_out)
	{
		ManagedSubagentSnapshot snapshot;
		if (!Capture(app, visible_root_chat_id, &snapshot, error_out, run_id, 0, 1))
			return false;
		const auto read = ReadDetached(snapshot, false);
		if (!read.ready)
		{
			if (error_out)
				*error_out = read.error;
			return false;
		}
		return CancelPrepared(app, visible_root_chat_id, run_id, scope, snapshot, read, error_out);
	}

	bool ManagedSubagentControlService::ValidateCancellationOwnership(const AppState& app, const std::string& run_id, std::string* error_out)
	{
		const auto admitted = app.managed_subagent_cancellation_receipts.find(run_id);
		// Do not impose visible-root, inventory, model or GUI rules on existing internal cancellations.
		if (admitted == app.managed_subagent_cancellation_receipts.end())
			return true;
		if (!admitted->second)
			return Fail(error_out, "Managed cancellation receipt is unavailable.");
		const auto& receipt = *admitted->second;
		const auto& scope = receipt.scope;
		if (scope.app_incarnation != app.managed_subagent_control_incarnation || scope.data_root != app.data_root)
			return Fail(error_out, "Managed cancellation data owner changed.");
		Index index;
		if (!BuildIndex(app, scope.root_chat_id, index, error_out))
			return false;
		if (index.root->metadata_incarnation != scope.root_incarnation || index.root->created_at != scope.root_created_at || index.root->imported_read_only)
			return Fail(error_out, "Managed cancellation root was replaced.");
		const auto selected = index.runs.find(receipt.selected_run_id);
		if (selected == index.runs.end())
			return Fail(error_out, "Managed cancellation tree changed.");
		const auto tree = Tree(index, *selected->second);
		if (tree.size() != scope.runs.size() || !CanStopTree(index, tree))
			return Fail(error_out, "Managed cancellation descendant ownership changed.");
		for (const AgentRun* run : tree)
		{
			const auto original = scope.runs.find(run->id);
			if (original == scope.runs.end() || original->second.identity != OwnershipKey(*run))
				return Fail(error_out, "Managed cancellation run was rebound.");
			if (Terminal(*run))
				continue;
			const auto chat = index.chats.find(run->transcript_chat_id);
			if (chat == index.chats.end() || chat->second->metadata_incarnation != original->second.transcript_incarnation || TranscriptKey(*chat->second) != original->second.transcript_identity)
				return Fail(error_out, "Managed cancellation transcript was replaced.");
		}
		return true;
	}
	nlohmann::json ManagedSubagentControlService::ToJson(const ManagedSubagentPage& page)
	{
		nlohmann::json runs = nlohmann::json::array();
		for (const ManagedSubagentRun& run : page.runs)
		{
			runs.push_back({{"runId", run.run_id}, {"parentRunId", run.parent_run_id}, {"agentId", run.agent_id}, {"transcriptChatId", run.transcript_chat_id}, {"status", run.status}, {"providerId", run.provider_id}, {"modelId", run.model_id}, {"host", run.host ? nlohmann::json(*run.host) : nlohmann::json(nullptr)}, {"createdAt", run.created_at}, {"startedAt", run.started_at}, {"completedAt", run.completed_at}, {"startedAtEpochMs", run.started_at_epoch_ms}, {"finishedAtEpochMs", run.finished_at_epoch_ms}, {"executionCapability", run.execution_capability}, {"canCancel", run.can_cancel}, {"summary", run.summary}, {"lastReported", run.last_reported}, {"taskTitle", run.task_title}});
		}
		return {{"runs", std::move(runs)}, {"totalCount", page.total_count}, {"offset", page.offset}, {"nextOffset", page.next_offset ? nlohmann::json(*page.next_offset) : nlohmann::json(nullptr)}};
	}
} // namespace uam
