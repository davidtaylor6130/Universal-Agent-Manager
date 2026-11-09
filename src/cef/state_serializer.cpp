#include "common/utils/uuid.h"
#include "common/runtime/acp/compute_pool_gate.h"
#include "common/chat/chat_repository.h"
#include "common/chat/chat_message_page_index.h"
#include "app/startup_native_import_service.h"
#include "app/worktree_launch_preparation_service.h"
#include "app/execution_routing_integration.h"
#include "cef/state_serializer.h"
#include "common/config/custom_icon.h"

#include "app/agent_definition_service.h"
#include "app/chat_domain_service.h"
#include "app/managed_subagent_control_service.h"
#include "app/provider_model_catalog_service.h"
#include "app/provider_configuration_service.h"
#include "app/uam_control_service.h"
#include "common/memory/memory_levels.h"
#include "common/memory/memory_policy.h"

#include "common/chat/message_attachment_json.h"
#include "common/chat/message_attachment_sources.h"
#include "common/constants/app_constants.h"
#include "remote/runner_protocol.h"
#include "remote/runner_proxy.h"
#include "common/config/settings_frontend_json.h"
#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "app/chat_lifecycle_service.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_queue_projection.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/app_time.h"
#include "common/provider/excluded_provider_data.h"
#include "common/config/provider_chat_defaults.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_attention_kind.h"
#include "common/runtime/acp/acp_tool_items.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/utils/hash_utils.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"
#include "computer_use/computer_use_mcp_config.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <tuple>
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
			return !ChatDomainService().AttentionRevision(app, chat).empty();
		}

		std::string ResolvedAcpSessionIdForChat(const AppState& app, const ChatSession& chat)
		{
			const auto resolved = app.resolved_native_sessions_by_chat_id.find(chat.id);
			if (resolved != app.resolved_native_sessions_by_chat_id.end())
			{
				const std::string resolved_session_id = uam::strings::Trim(resolved->second);
				if (!resolved_session_id.empty())
				{
					return resolved_session_id;
				}
			}

			return uam::strings::Trim(chat.native_session_id);
		}

		struct CatalogSnapshotCache
		{
			using WorkspaceKey = std::tuple<bool, bool, std::string, std::string, std::string, std::string>;
			std::unordered_map<std::string, nlohmann::json> catalogs;
			std::unordered_map<std::string, nlohmann::json> fingerprints;
			bool compact_catalogs = false;
			std::map<WorkspaceKey, std::filesystem::path> workspaces;
		};

		/// <summary>Reuse identical workspace scopes only within this serialization.</summary>
		const std::filesystem::path& WorkspaceSnapshotForChat(const AppState& app, const ChatSession& chat,
		                                                       CatalogSnapshotCache& cache)
		{
			const CatalogSnapshotCache::WorkspaceKey key{chat.imported_read_only, uam::paths::IsGitWorktreeIsolated(chat),
			    chat.execution_host_id, chat.workspace_directory, chat.workspace_worktree_directory, chat.folder_id};
			if (const auto found = cache.workspaces.find(key); found != cache.workspaces.end()) return found->second;
			return cache.workspaces.emplace(key, uam::paths::ResolveWorkspaceRootPath(app, chat)).first->second;
		}

		std::string CatalogSnapshotKey(const std::string& provider_id,
		                              std::string_view workspace,
		                              std::string_view execution_host_id)
		{
			const std::string host = uam::strings::NonEmptyOrFallback(
			    uam::strings::Trim(std::string(execution_host_id)), std::string(uam::execution_hosts::kLocalHostId));
			return provider_id + "\n" + std::string(workspace) + "\n" + host;
		}

		std::string FallbackAcpCurrentModelForChat(const AppState& app, const ChatSession& chat)
		{
			if (!uam::strings::IsBlank(chat.execution_host_id) && chat.execution_host_id != uam::execution_hosts::kLocalHostId)
				return chat.model_id;
			if (app.provider_model_catalog != nullptr)
			{
				return app.provider_model_catalog->FallbackAcpCurrentModelForChat(chat.provider_id, chat.model_id);
			}
			return uam::strings::IsBlank(chat.model_id) ? std::string{} : chat.model_id;
		}

		nlohmann::json CatalogSnapshotForChat(const AppState& app, const ChatSession& chat,
		                                     CatalogSnapshotCache& cache)
		{
			const std::string workspace = WorkspaceSnapshotForChat(app, chat, cache).generic_string();
			const std::string key = CatalogSnapshotKey(chat.provider_id, workspace, chat.execution_host_id);
			if (const auto found = cache.catalogs.find(key); found != cache.catalogs.end()) return found->second;
			if (app.provider_model_catalog != nullptr)
			{
				auto snapshot = app.provider_model_catalog->GetCatalogSnapshot(chat.provider_id, workspace, chat.execution_host_id);
				const auto* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id.empty() ? "local" : chat.execution_host_id);
				ProviderModelCatalogService::ApplyLaunchOptionMetadata(snapshot["availableModels"], chat.provider_id, host != nullptr ? CheckedProviderConfigurationVersion(app, chat.provider_id, *host) : std::string{});
				return cache.catalogs.emplace(key, std::move(snapshot)).first->second;
			}
			return {{"availableModels", nlohmann::json::array()}, {"configOptions", nlohmann::json::array()},
			        {"modelsLoading", false}, {"modelRefreshError", ""}};
		}

		/// Fingerprint each inactive scope once per scan; changed patches hydrate its full catalog.
		const nlohmann::json& CatalogFingerprintForChat(const AppState& app, const ChatSession& chat,
		                                               CatalogSnapshotCache& cache)
		{
			const std::string workspace = WorkspaceSnapshotForChat(app, chat, cache).generic_string();
			const std::string key = CatalogSnapshotKey(chat.provider_id, workspace, chat.execution_host_id);
			if (const std::unordered_map<std::string, nlohmann::json>::const_iterator found = cache.fingerprints.find(key); found != cache.fingerprints.end()) return found->second;
			const nlohmann::json snapshot = CatalogSnapshotForChat(app, chat, cache);
			return cache.fingerprints.emplace(key, nlohmann::json{
			    {"catalogDigest", std::to_string(std::hash<std::string>{}(snapshot.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)))},
			    {"modelsLoading", snapshot.value("modelsLoading", false)},
			    {"modelRefreshError", snapshot.value("modelRefreshError", "")},
			}).first->second;
		}

		nlohmann::json SerializeProviderModelCatalogs(const AppState& app, CatalogSnapshotCache& cache)
		{
			nlohmann::json catalogs = nlohmann::json::array();
			if (app.provider_model_catalog == nullptr) return catalogs;
			std::unordered_set<std::string> seen;
			// Every provider revisits each folder; normalising a local path probes the filesystem, so do it once per scan.
			std::map<std::pair<std::string, std::string>, std::string> scope_keys;
			const auto append_catalog = [&](const std::string& provider_id,
			                                const std::string& workspace,
			                                const std::string& execution_host_id)
			{
				if (provider_id.empty() || workspace.empty()) return;
				const std::string host = uam::strings::NonEmptyOrFallback(
				    uam::strings::Trim(execution_host_id), std::string(uam::execution_hosts::kLocalHostId));
				const bool remote = host != uam::execution_hosts::kLocalHostId;
				auto scope_key_it = scope_keys.find({host, workspace});
				if (scope_key_it == scope_keys.end())
					scope_key_it = scope_keys.emplace(std::pair{host, workspace}, uam::paths::WorkspaceOwnershipKey(app, host, workspace)).first;
				const std::string& scope_key = scope_key_it->second;
				if (!seen.insert(provider_id + "\n" + scope_key).second) return;
				ChatSession scope;
				scope.provider_id = provider_id;
				scope.workspace_directory = workspace;
				scope.execution_host_id = host;
				nlohmann::json catalog = CatalogSnapshotForChat(app, scope, cache);
				catalog["providerId"] = provider_id;
				catalog["workspaceDirectory"] = workspace;
				catalog["executionHostId"] = host;
				catalog["currentModelId"] = remote ? std::string{} : app.provider_model_catalog->FallbackAcpCurrentModelForChat(provider_id, "");
				catalogs.push_back(std::move(catalog));
			};
			for (const ProviderProfile& provider : app.provider_profiles)
			{
				if (!provider.supports_structured) continue;
				for (const ChatFolder& folder : app.folders)
				{
					ChatSession scope;
					scope.workspace_directory = folder.directory;
					scope.execution_host_id = folder.execution_host_id;
					const std::string workspace = WorkspaceSnapshotForChat(app, scope, cache).generic_string();
					append_catalog(provider.id, workspace, folder.execution_host_id);
				}
			}
			for (const nlohmann::json& scope : app.provider_model_catalog->GetCatalogScopes())
			{
				if (!scope.is_object() || uam::excluded_provider_data::IsExcluded(scope.value("providerId", ""))) continue;
				const std::string host = scope.value("executionHostId", "local");
				append_catalog(scope.value("providerId", ""),
				    scope.value("workspaceDirectory", ""), host);
			}
			return catalogs;
		}

		struct SelectedHistoryDigestCache
		{
			std::string chat_id;
			std::vector<Message> messages;
			std::vector<std::uint64_t> prefix_hashes;
			std::size_t rehashed_message_count = 0;
		};

		// Retain only the selected transcript on this serialization thread; workers never share mutable cache state.
		thread_local SelectedHistoryDigestCache g_selected_history_digest;

		void SelectHistoryDigestCache(const std::string& chat_id)
		{
			if (g_selected_history_digest.chat_id == chat_id) return;
			g_selected_history_digest = {};
			g_selected_history_digest.chat_id = chat_id;
		}

		std::string MessageDigestForFingerprint(const ChatSession& session, bool cache_selected = false);
		std::size_t MessageCountForFrontend(const ChatSession& session);

		/// <summary>
		/// Chat summaries are written field by field, so one body yields either the frontend JSON or, for
		/// poll-time change detection, a 64-bit FNV-1a digest of exactly the same fields without building a
		/// JSON tree. A field added to a summary body is therefore always part of its digest.
		/// </summary>
		class SummaryJsonWriter
		{
		  public:
			static constexpr bool kDigest = false;
			explicit SummaryJsonWriter(nlohmann::json& target) : m_target(target) {}
			template <class T> void Set(const char* key, T&& value) { m_target[key] = std::forward<T>(value); }
			template <class Fill> void Object(const char* key, Fill&& fill)
			{
				SummaryJsonWriter child(m_target[key] = nlohmann::json::object());
				fill(child);
			}
			/// <summary>Adds every field of an already serialized object.</summary>
			void Merge(const nlohmann::json& fields)
			{
				for (nlohmann::json::const_iterator field = fields.begin(); field != fields.end(); ++field) m_target[field.key()] = field.value();
			}
			void Merge(nlohmann::json&& fields)
			{
				if (m_target.empty())
				{
					m_target = std::move(fields);
					return;
				}
				for (nlohmann::json::iterator field = fields.begin(); field != fields.end(); ++field) m_target[field.key()] = std::move(field.value());
			}

		  private:
			nlohmann::json& m_target;
		};

		class SummaryDigestWriter
		{
		  public:
			static constexpr bool kDigest = true;
			template <class T> void Set(const char* key, const T& value) { Text('k', key); Value(value); }
			template <class Fill> void Object(const char* key, Fill&& fill)
			{
				Text('k', key);
				Tag('{');
				fill(*this);
				Tag('}');
			}
			void Merge(const nlohmann::json& fields) { Json(fields); }
			std::uint64_t Digest() const { return m_hash; }

		  private:
			// Each word passes through the reversible MurmurHash3 fmix64 finaliser so high-bit changes diffuse into
			// every bit, then a word-wise FNV step. Both are bijections, so a change to any single short field always
			// changes the digest; long text contributes its library hash instead of every byte.
			static std::uint64_t Scramble(std::uint64_t word)
			{
				word ^= word >> 33;
				word *= 0xff51afd7ed558ccdULL;
				word ^= word >> 33;
				word *= 0xc4ceb9fe1a85ec53ULL;
				word ^= word >> 33;
				return word;
			}
			void Mix(std::uint64_t word) { m_hash = (m_hash ^ Scramble(word)) * uam::hashing::kFnv1a64Prime; }
			void Tag(unsigned char tag) { Mix(tag); }
			template <class N> void Number(N value)
			{
				std::uint64_t word = 0;
				static_assert(sizeof value <= sizeof word);
				std::memcpy(&word, &value, sizeof value);
				Mix(word);
			}
			void Text(unsigned char tag, std::string_view text)
			{
				Tag(tag);
				Mix(text.size());
				if (text.size() > 64)
				{
					Mix(std::hash<std::string_view>{}(text));
					return;
				}
				for (std::size_t offset = 0; offset < text.size(); offset += sizeof(std::uint64_t))
				{
					std::uint64_t word = 0;
					std::memcpy(&word, text.data() + offset, std::min(sizeof word, text.size() - offset));
					Mix(word);
				}
			}
			template <class T> void Value(const T& value)
			{
				using V = std::remove_cvref_t<T>;
				if constexpr (std::is_same_v<V, bool>) Tag(value ? 't' : 'f');
				else if constexpr (std::is_integral_v<V>) { Tag('i'); Number(static_cast<std::int64_t>(value)); }
				else if constexpr (std::is_floating_point_v<V>) { Tag('d'); Number(static_cast<double>(value)); }
				else if constexpr (std::is_same_v<V, std::nullptr_t>) Tag('n');
				else if constexpr (std::is_same_v<V, nlohmann::json>) Json(value);
				else if constexpr (std::is_same_v<V, std::vector<int>>)
				{
					Tag('[');
					for (const int item : value) Value(item);
					Tag(']');
				}
				else if constexpr (std::is_convertible_v<const V&, std::string_view>) Text('s', std::string_view(value));
				else Json(nlohmann::json(value));
			}
			void Json(const nlohmann::json& value)
			{
				switch (value.type())
				{
					case nlohmann::json::value_t::object:
						Tag('{');
						for (nlohmann::json::const_iterator field = value.begin(); field != value.end(); ++field)
						{
							Text('k', field.key());
							Json(field.value());
						}
						Tag('}');
						return;
					case nlohmann::json::value_t::array:
						Tag('[');
						for (const nlohmann::json& item : value) Json(item);
						Tag(']');
						return;
					case nlohmann::json::value_t::string: Text('s', value.get_ref<const std::string&>()); return;
					case nlohmann::json::value_t::boolean: Tag(value.get<bool>() ? 't' : 'f'); return;
					case nlohmann::json::value_t::number_integer: Tag('i'); Number(value.get<std::int64_t>()); return;
					case nlohmann::json::value_t::number_unsigned: Tag('i'); Number(static_cast<std::int64_t>(value.get<std::uint64_t>())); return;
					case nlohmann::json::value_t::number_float: Tag('d'); Number(value.get<double>()); return;
					case nlohmann::json::value_t::binary:
					{
						const nlohmann::json::binary_t& bytes = value.get_binary();
						Tag('b');
						Number(static_cast<std::uint64_t>(bytes.size()));
						uam::hashing::UpdateFnv1a64(m_hash, bytes.data(), bytes.size());
						return;
					}
					case nlohmann::json::value_t::discarded: Tag('x'); return;
					case nlohmann::json::value_t::null: Tag('n'); return;
				}
				Tag('?');
			}

			std::uint64_t m_hash = uam::hashing::kFnv1a64OffsetBasis;
		};

		template <class Writer>
		void WriteWorkspaceIsolationFields(Writer& w, const ChatSession& chat)
		{
			w.Set("workspaceIsolationKind", chat.workspace_isolation_kind);
			w.Set("workspaceSourceDirectory", chat.workspace_source_directory);
			w.Set("workspaceBaseRef", chat.workspace_base_ref);
			w.Set("gitBaseBranch", chat.git_base_branch_override ? nlohmann::json(*chat.git_base_branch_override) : nlohmann::json(nullptr));
			w.Set("workspaceBranchName", chat.workspace_branch_name);
			if (!chat.pending_git_branch_name.empty()) w.Set("pendingGitBranch", chat.pending_git_branch_name);
			w.Set("workspaceWorktreeDirectory", chat.workspace_worktree_directory);
		}

		template <class Writer>
		void WriteSessionSummaryFields(Writer& w, const ChatSession& session, std::string_view workspace_directory, bool cache_selected = false)
		{
			w.Set("id", session.id);
			w.Set("executionHostId", uam::strings::NonEmptyOrFallback(session.execution_host_id, "local"));
			w.Set("remoteRecoveryEnabled", session.remote_recovery_enabled);
			w.Set("remoteRecoveryState", session.remote_recovery_state);
			w.Set("routingMode", session.routing_mode);
			w.Set("routingSourceFolderId", session.routing_source_folder_id);
			w.Set("routingSourceDirectory", session.routing_source_directory);
			w.Set("routingReservationPending", session.routing_reservation_pending);
			w.Set("routingAssignedConnectionIdentity", session.routing_assigned_connection_identity);
			w.Set("routingAssignedProviderConfigurationIdentity", session.routing_assigned_provider_configuration_identity);
			w.Set("title", session.title);
			w.Set("customIcon", uam::icons::Serialize(session.custom_icon));
			w.Set("folderId", session.folder_id);
			w.Set("pinned", session.pinned);
			w.Set("providerId", session.provider_id);
			w.Set("temporaryParentChatId", session.temporary_parent_chat_id);
			w.Set("sideCleanupRequested", session.side_cleanup_requested);
			w.Set("parentChatId", session.parent_chat_id);
			w.Set("subagentParentChatId", session.subagent_parent_chat_id);
			w.Set("subagentParentCreatedAt", session.subagent_parent_created_at);
			if (!session.created_by_chat_id.empty()) w.Set("createdByChatId", session.created_by_chat_id);
			w.Set("branchRootChatId", uam::strings::NonEmptyOrFallback(session.branch_root_chat_id, session.id));
			w.Set("branchFromMessageIndex", session.branch_from_message_index);
			w.Set("branchMessageEdited", session.branch_message_edited);
			w.Set("modelId", session.model_id);
			w.Set("reviewerModelId", session.reviewer_model_id);
			w.Set("reasoningEffort", session.reasoning_effort);
			w.Set("serviceTier", session.service_tier);
			w.Set("serviceTierExplicit", session.service_tier_explicit);
			w.Set("approvalMode", session.approval_mode);
			w.Set("uamAgentId", uam::strings::NonEmptyOrFallback(session.uam_agent_id, "build"));
			w.Set("uamControlEnabled", session.uam_control_enabled);
			w.Set("commandSafetyTier", session.command_safety_tier);
			w.Set("computerUseEnabled", session.computer_use_enabled);
			w.Set("computerUseBackend", uam::computer_use::BackendPreference(session.computer_use_backend));
			w.Set("computerUseEffectiveBackend", uam::computer_use::EffectiveBackend(session));
			w.Set("computerUseProviderAvailable", uam::computer_use::ProviderBackendAvailable(session.provider_id));
			w.Set("computerUseTargetKind", session.computer_use_target_kind);
			w.Set("computerUseTargetId", session.computer_use_target_id);
			w.Set("computerUseTargetTitle", session.computer_use_target_title);
			w.Set("computerUseTargetInputMode", session.computer_use_target_input_mode);
			w.Set("memoryLevel", uam::memory_levels::NormalizeSelection(session.memory_level, session.memory_enabled));
			w.Set("memoryEnabled", uam::memory_levels::IsEnabled(session.memory_level, session.memory_enabled));
			w.Set("smallModelMode", session.small_model_mode);
			w.Set("memoryLastProcessedMessageCount", session.memory_last_processed_message_count);
			w.Set("memoryLastProcessedAt", session.memory_last_processed_at);
			// Passive native journals retain their attested workspace for display
			// lineage matching. Runtime workspace resolution remains read-only/empty.
			w.Set("workspaceDirectory", session.imported_read_only && !session.subagent_parent_chat_id.empty() && !session.subagent_parent_created_at.empty() ? session.workspace_directory : std::string(workspace_directory));
			w.Set("importedReadOnly", session.imported_read_only);
			WriteWorkspaceIsolationFields(w, session);
			w.Set("createdAt", session.created_at);
			w.Set("updatedAt", session.updated_at);
			w.Set("attentionRevision", session.attention_revision);
			w.Set("attentionSource", session.attention_source);
			w.Set("managedSubagentRevision", session.managed_subagent_revision);
			w.Set("interactionAt", session.interaction_at);
			w.Set("settledAt", session.settled_at);
			w.Set("snoozedUntilMs", session.snoozed_until_ms);
			w.Set("lastOpenedAt", uam::strings::NonEmptyOrFallback(session.last_opened_at, session.updated_at));
			w.Set("messageCount", MessageCountForFrontend(session));
			w.Set("messagesDigest", MessageDigestForFingerprint(session, cache_selected));
			w.Set("activeGoalId", session.active_goal_id.empty() ? nullptr : nlohmann::json(session.active_goal_id));
		}

		void AddWorkspaceIsolationFields(nlohmann::json& chat_json, const ChatSession& chat)
		{
			SummaryJsonWriter w(chat_json);
			WriteWorkspaceIsolationFields(w, chat);
		}

		void AddSessionSummaryFields(nlohmann::json& session_json, const ChatSession& session, std::string_view workspace_directory, bool cache_selected = false)
		{
			SummaryJsonWriter w(session_json);
			WriteSessionSummaryFields(w, session, workspace_directory, cache_selected);
		}

		template <class Writer>
		void WriteComputerUseState(Writer& w, const AppState& app, const ChatSession& chat)
		{
			w.Set("enabled", chat.computer_use_enabled);
			const auto found = uam::computer_use::UsesUamBackend(chat) ? app.computer_use_by_chat_id.find(chat.id) : app.computer_use_by_chat_id.end();
			if (found == app.computer_use_by_chat_id.end())
			{
				w.Set("state", chat.computer_use_enabled ? "running" : "stopped");
				w.Set("history", nlohmann::json::array());
				return;
			}
			w.Set("state", found->second.state);
			nlohmann::json history = nlohmann::json::array();
			for (const ComputerUseHistoryEntry& entry : found->second.history)
			{
				history.push_back({
				    {"time", entry.time}, {"action", entry.action},
				    {"status", entry.status}, {"detail", entry.detail},
				});
			}
			w.Set("history", std::move(history));
		}

		nlohmann::json SerializeComputerUseState(const AppState& app, const ChatSession& chat)
		{
			nlohmann::json result = nlohmann::json::object();
			SummaryJsonWriter w(result);
			WriteComputerUseState(w, app, chat);
			return result;
		}

	std::string LatestChildActivity(const std::string& activity)
	{
		if (!activity.empty())
		{
			const std::size_t end = activity.find_last_not_of("\r\n ");
			if (end != std::string::npos)
			{
				const std::size_t newline = activity.rfind('\n', end);
				const std::size_t start = newline == std::string::npos ? 0 : newline + 1;
				std::size_t length = std::min<std::size_t>(end - start + 1, 240);
				// Keep the preview bounded without cutting a UTF-8 code point.
				while (start + length < activity.size() && length > 0 &&
				       (static_cast<unsigned char>(activity[start + length]) & 0xC0) == 0x80) --length;
				return activity.substr(start, length);
			}
		}
		return {};
	}

	nlohmann::json SerializeSubAgentChildren(const std::vector<SubAgentChild>& children)
	{
		nlohmann::json result = JsonArrayWithCapacity(children.size());
		for (const SubAgentChild& child : children)
		{
			nlohmann::json entry = {{"id", child.id}};
			if (!child.title.empty())
				entry["title"] = child.title;
			if (!child.status.empty())
				entry["status"] = child.status;
			if (child.started_at_epoch_ms) entry["startedAtEpochMs"] = *child.started_at_epoch_ms;
			if (child.finished_at_epoch_ms) entry["finishedAtEpochMs"] = *child.finished_at_epoch_ms;
			if (child.duration_ms) entry["durationMs"] = *child.duration_ms;
			result.push_back(std::move(entry));
		}
		return result;
	}

	// Card metadata is independent of deferred details and grants no routing authority.
	void AddManagedResultCard(nlohmann::json& target, std::string_view name, bool is_sub_agent, const std::string& run_id, const std::string& result_text)
	{
		if (name != "uam_agent_result" || !is_sub_agent || run_id.empty() || run_id.size() > 160 || run_id.find('\0') != std::string::npos || result_text.empty() || result_text.size() > 65536)
			return;
		const auto result = nlohmann::json::parse(result_text, nullptr, false);
		if (!result.is_object() || !result.contains("runId") || !result["runId"].is_string() || result["runId"].get_ref<const std::string&>() != run_id)
			return;
		const auto bounded_text = [](const std::string& text, std::size_t limit)
		{
			std::size_t end = std::min(text.size(), limit);
			while (end > 0 && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
				--end;
			std::string value = text.substr(0, end);
			for (char& ch : value)
				if (static_cast<unsigned char>(ch) < 32 || ch == 127)
					ch = ' ';
			return value;
		};
		nlohmann::json card{{"runId", run_id}};
		for (const auto* key : {"providerId", "agentId", "status", "result", "startedAt", "completedAt"})
		{
			const auto it = result.find(key);
			if (it != result.end() && it->is_string())
				card[key] = bounded_text(it->get_ref<const std::string&>(), std::string_view(key) == "result" ? 1024 : 160);
		}
		for (const auto* key : {"startedAtEpochMs", "finishedAtEpochMs"})
		{
			const auto it = result.find(key);
			if (it == result.end() || !it->is_number_integer())
				continue;
			if (it->is_number_unsigned())
			{
				if (it->get<std::uint64_t>() <= 8640000000000000ULL)
					card[key] = *it;
			}
			else if (it->get<std::int64_t>() >= 0 && it->get<std::int64_t>() <= 8640000000000000LL)
				card[key] = *it;
		}
		std::string content = card.dump();
		if (content.size() <= 8192)
			target["subAgentResultContent"] = std::move(content);
	}

	// Derived output-only launch correlation; never a persisted run or routing authority.
	// Bare adapter tool names require an explicit UAM server envelope. Qualified MCP
	// names already identify the server, but a contradictory envelope still rejects.
	void AddManagedLaunchCorrelation(nlohmann::json& target, std::string_view name,
	    std::string_view kind, std::string_view status, std::string_view result_text,
	    std::size_t complete_content_size)
	{
		const bool qualified = name == "uam-control.agent_delegate" ||
		    name == "mcp__uam_control__agent_delegate" || name == "mcp__uam-control__agent_delegate";
		if ((!qualified && name != "agent_delegate") ||
		    (kind != "mcpToolCall" && kind != "other" && kind != name) ||
		    status != "completed" || result_text.empty() || complete_content_size > 65536 || result_text.size() > 65536)
			return;
		auto value = nlohmann::json::parse(result_text, nullptr, false);
		bool server_proven = qualified;
		for (int depth = 0; depth < 5; ++depth)
		{
			if (!value.is_object()) return;
			const auto is_text = [&](const char* key, std::string_view expected)
			{
				const auto field = value.find(key);
				return field != value.end() && field->is_string() && field->get_ref<const std::string&>() == expected;
			};
			const auto error = value.find("error");
			if (error != value.end())
			{
				const bool falseish = error->is_null() ||
				    (error->is_boolean() && !error->get<bool>()) ||
				    (error->is_string() && error->get_ref<const std::string&>().empty()) ||
				    (error->is_number() && error->get<double>() == 0.0);
				if (!falseish) return;
			}
			if (value.contains("isError") && value["isError"] == true) return;
			if (value.contains("server") || value.contains("tool") || is_text("type", "mcpToolCall"))
			{
				if (!is_text("server", "uam-control") || !is_text("tool", "agent_delegate") || !is_text("status", "completed")) return;
				server_proven = true;
			}
			const auto run = value.find("runId");
			if (run != value.end() && run->is_string() && is_text("status", "queued"))
			{
				const auto& id = run->get_ref<const std::string&>();
				if (server_proven && uam::uuid::IsCanonicalUuid(id)) target["managedLaunchRunId"] = id;
				return;
			}
			const auto result = value.find("result");
			if (result != value.end())
			{
				auto next = *result;
				value = std::move(next);
				continue;
			}
			const auto content = value.find("content");
			if (content == value.end() || !content->is_array() || content->size() != 1) return;
			const auto& block = content->at(0);
			if (!block.is_object() || !block.contains("type") || block["type"] != "text" ||
			    !block.contains("text") || !block["text"].is_string()) return;
			const auto& text = block["text"].get_ref<const std::string&>();
			if (text.size() > 8192) return;
			auto next = nlohmann::json::parse(text, nullptr, false);
			value = std::move(next);
		}
	}

	nlohmann::json SerializeToolCallForFrontend(const ToolCall& tool_call,
	                                             bool defer_tool_call_content = false)
	{
		constexpr std::size_t kInlineToolContentMaxBytes = 64 * 1024;
		nlohmann::json tool_json;
		tool_json["id"] = tool_call.id;
		tool_json["title"] = tool_call.name;
		tool_json["kind"] = uam::strings::NonEmptyOrFallback(tool_call.kind, uam::strings::NonEmptyOrFallback(tool_call.name, "tool"));
		tool_json["status"] = tool_call.status;
		tool_json["approvalStatus"] = tool_call.approval_status;
		const std::size_t content_size = tool_call.args_json.size() + tool_call.result_text.size() +
		                                 tool_call.child_activity.size() + (tool_call.child_activity.empty() ? 0 : std::string_view("\n\nChild activity:\n").size()) +
		                                 tool_call.approval_status.size() + (tool_call.approval_status.empty() ? 0 : std::string_view("Approval: \n\n").size()) +
		                                 (!tool_call.args_json.empty() && !tool_call.result_text.empty()
		                                      ? std::string_view("Arguments:\n\n\nResult:\n").size()
		                                      : 0);
		const bool content_deferred = defer_tool_call_content
		    ? content_size > 0
		    : content_size > kInlineToolContentMaxBytes;
		tool_json["contentDeferred"] = content_deferred;
		if (content_deferred)
			AddManagedLaunchCorrelation(tool_json, tool_call.name, tool_json["kind"].get_ref<const std::string&>(), tool_call.status, tool_call.result_text, content_size);
		if (content_deferred)
			tool_json["contentDigest"] = std::to_string(std::hash<std::string>{}(tool_call.args_json)) + ":" +
			                             std::to_string(std::hash<std::string>{}(tool_call.result_text)) + ":" +
		                             std::to_string(std::hash<std::string>{}(tool_call.approval_status)) + ":" +
		                             std::to_string(std::hash<std::string>{}(tool_call.child_activity));
		if (!content_deferred)
			tool_json["content"] = StateSerializer::ToolCallContentForFrontend(tool_call);
		if (tool_call.is_sub_agent) tool_json["currentActivity"] = LatestChildActivity(tool_call.child_activity);
		tool_json["isSubAgent"] = tool_call.is_sub_agent;
		tool_json["subAgentId"] = tool_call.sub_agent_id;
		tool_json["subAgentTitle"] = tool_call.sub_agent_title;
		AddManagedResultCard(tool_json, tool_call.name, tool_call.is_sub_agent, tool_call.sub_agent_id, tool_call.result_text);
		if (tool_call.sub_agent_children_authoritative || !tool_call.sub_agent_children.empty())
			tool_json["subAgentChildren"] = SerializeSubAgentChildren(tool_call.sub_agent_children);
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
			if (!block.title.empty()) block_json["title"] = block.title;
			if (block.compaction_tokens_before >= 0) block_json["tokensBefore"] = block.compaction_tokens_before;
			if (block.compaction_tokens_after >= 0) block_json["tokensAfter"] = block.compaction_tokens_after;
			return block_json;
		}

		nlohmann::json SerializeAttachmentForFrontend(const MessageAttachment& attachment)
		{
			nlohmann::json result = {
			    {std::string(attachment_fields::kIdField), attachment.id}, {std::string(attachment_fields::kNameField), attachment.name}, {std::string(attachment_fields::kKindField), attachment.kind}, {std::string(attachment_frontend_fields::kMimeTypeField), attachment.mime_type}, {std::string(attachment_fields::kPathField), attachment.path}, {std::string(attachment_frontend_fields::kSizeBytesField), attachment.size_bytes}, {std::string(attachment_fields::kCopiedField), attachment.copied}, {std::string(attachment_fields::kPreviewKeyField), attachment.preview_key}, {std::string(attachment_fields::kPreviewOriginField), attachment.preview_origin_path},
			};
			if (attachment.source != AttachmentSource::LegacyPath)
			{
				// Source metadata describes saved content; it never grants renderer
				// filesystem, provider URI, or original-payload authority.
				result[std::string(attachment_fields::kSourceField)] = uam::attachment_sources::Name(attachment.source);
				result[std::string(attachment_fields::kPathField)] = "";
				result[std::string(attachment_fields::kCopiedField)] = false;
				result[std::string(attachment_fields::kPreviewOriginField)] = "";
				if (attachment.source != AttachmentSource::OwnedNativeAsset)
					result[std::string(attachment_fields::kPreviewKeyField)] = "";
				for (const auto& [field, value] : {std::pair{attachment_fields::kSourceUriField, &attachment.source_uri}, std::pair{attachment_fields::kOriginHostField, &attachment.origin_execution_host_id}, std::pair{attachment_fields::kNativePartKeyField, &attachment.native_part_key}, std::pair{attachment_fields::kNativeToolCallIdField, &attachment.native_tool_call_id}, std::pair{attachment_fields::kNativeSourceField, &attachment.native_source_field}, std::pair{attachment_fields::kNativeAssetKeyField, &attachment.native_asset_key}, std::pair{attachment_fields::kContentDigestField, &attachment.content_digest}})
					if (!value->empty())
						result[std::string(field)] = *value;
			}
			return result;
		}

		nlohmann::json SerializeQueuedAcpUserPrompt(const AcpQueuedUserPromptState& prompt)
		{
			nlohmann::json attachments = JsonArrayWithCapacity(prompt.attachments.size());
			for (const MessageAttachment& attachment : prompt.attachments)
			{
				attachments.push_back(SerializeAttachmentForFrontend(attachment));
			}
			nlohmann::json result = {
			    {"id", prompt.id},
			    {"revision", prompt.revision},
			    {"prepared", prompt.prepared_for_delivery},
			    {"text", prompt.text},
			    {"uamAgentId", prompt.uam_agent_id},
			    {"markdownStoreFiles", prompt.markdown_store_files},
			    {"attachments", std::move(attachments)},
			    {"goalMode", prompt.goal_mode},
			    {"goalId", prompt.goal_id},
			    {"computerUseMode", prompt.computer_use_mode},
			    {"prioritySteer", prompt.priority_steer},
			};
			if (prompt.existing_user_message_index >= 0) result["existingUserMessageIndex"] = prompt.existing_user_message_index;
			if (prompt.native_input_id && uam::input_provenance::ValidNativeInputId(*prompt.native_input_id)) result["nativeInputId"] = *prompt.native_input_id;
			if (prompt.origin_request_id && uam::input_provenance::ValidOriginRequestId(*prompt.origin_request_id)) result["originRequestId"] = *prompt.origin_request_id;
			return result;
		}

		bool HasExactRawPromptReference(const ChatSession& chat, const AcpQueuedUserPromptState& prompt)
		{
			if (!chat.messages_loaded || prompt.id.empty() || prompt.revision <= 0 || prompt.existing_user_message_index < 0 ||
			    static_cast<std::size_t>(prompt.existing_user_message_index) >= chat.messages.size()) return false;
			const Message& message = chat.messages[static_cast<std::size_t>(prompt.existing_user_message_index)];
			return message.role == MessageRole::User && message.acp_prompt_not_sent && !message.acp_prompt_discarded &&
			    !prompt.existing_user_message_created_at.empty() && prompt.existing_user_message_created_at == message.created_at &&
			    prompt.text == message.content && prompt.attachments == message.attachments &&
			    prompt.markdown_store_files == message.markdown_store_files &&
			    prompt.markdown_store_prompt_blocks == message.markdown_store_prompt_blocks;
		}

		/// <summary>Separates direct admission receipts from future input queued behind a provider turn.</summary>
		void SerializePromptAdmissionQueues(nlohmann::json& result, const AppState& app, const ChatSession& chat,
		                                    const AcpSessionState* session, const std::vector<const AcpQueuedUserPromptState*>& prompts)
		{
			result["queuedPrompts"] = nlohmann::json::array();
			result["pendingDirectPrompts"] = nlohmann::json::array();
			result["unsentDirectPrompts"] = nlohmann::json::array();
			const std::vector<int> known = KnownUnsentAcpMessageIndices(app, chat);
			const bool uncertain = ChatHasUncertainRemoteInput(chat) || !chat.remote_interaction_responses.empty() || (session && (session->remote_stop_unconfirmed ||
			    !session->pending_request_methods.empty() || !session->pending_steer_requests.empty()));
			const bool pending = uncertain || (session && (session->processing || session->pending_prompt_delivery)) || (session && !session->queue_held &&
			    (session->processing || !session->queued_prompt.empty() || session->pending_agent_input ||
			     session->pending_local_launch || session->pending_prompt_delivery || session->pending_local_session_receipt ||
			     session->reconnect_pending || session->recovering_remote_turn || session->recovering_remote_process ||
			     session->worktree_launch_pending || ((session->remote_stop_pending || session->restart_marker_save_pending) &&
			     session->restart_after_remote_stop_cleanup)));
			const bool held = session ? session->queue_held : chat.acp_queue_held;
			const bool baseline_hold = std::ranges::any_of(prompts, [](const AcpQueuedUserPromptState* prompt) { return prompt->repository_baseline_hold; });
			for (const AcpQueuedUserPromptState* prompt : prompts)
			{
				if (prompt->prepared_for_delivery || !prompt->claude_quota_continuation_id.empty()) continue;
				// A held row-less direct receipt only advances through Resume (#682): presenting it as Sending
				// left no control while Send refused the held queue. Uncertain delivery, referenced rows and baseline holds keep
				// their receipts so Resume never replays input that may have reached the provider.
				if (prompt->direct_admission && held && !uncertain && !baseline_hold && prompt->existing_user_message_index < 0)
				{
					result["queuedPrompts"].push_back(SerializeQueuedAcpUserPrompt(*prompt));
					continue;
				}
				if (prompt->direct_admission)
				{
					if (pending)
					{
						result["pendingDirectPrompts"].push_back(SerializeQueuedAcpUserPrompt(*prompt));
						continue;
					}
					// A native raw row does not establish that the frontend has loaded it.
					// Keep the owned receipt visible until delivery consumes it; the exact
					// native reference lets the renderer present one row when history arrives.
					if (HasExactRawPromptReference(chat, *prompt))
					{
						auto receipt = SerializeQueuedAcpUserPrompt(*prompt);
						receipt["referencedMessageCreatedAt"] = prompt->existing_user_message_created_at;
						// Retry permission still comes exclusively from native recovery guards.
						receipt["retryAllowed"] = std::ranges::find(known, prompt->existing_user_message_index) != known.end();
						result["unsentDirectPrompts"].push_back(std::move(receipt));
						continue;
					}
					auto receipt = SerializeQueuedAcpUserPrompt(*prompt);
					// Only the exact receipt held by a failed repository baseline gains row-less Retry.
					if (prompt->repository_baseline_hold && AcpBaselineHoldRetryAllowed(app, chat, session))
						receipt["retryAllowed"] = true;
					result["unsentDirectPrompts"].push_back(std::move(receipt));
					continue;
				}
				if (IsVisibleAcpQueuedPrompt(*prompt))
					result["queuedPrompts"].push_back(SerializeQueuedAcpUserPrompt(*prompt));
			}
			result["queueHeld"] = held && !result["queuedPrompts"].empty();
			switch (ProviderSwitchRefusalReason(app, chat) != nullptr ? ProviderSwitchOverride::None : ProviderSwitchOverrideNeeded(app, chat))
			{
			case ProviderSwitchOverride::None: result["providerSwitchOverride"] = "none"; break;
			case ProviderSwitchOverride::Warn: result["providerSwitchOverride"] = "warn"; break;
			case ProviderSwitchOverride::Disconnected: result["providerSwitchOverride"] = "disconnected"; break;
			case ProviderSwitchOverride::StrandedInput: result["providerSwitchOverride"] = AcpRemoteRecoveryIsDisconnected(app, chat) ? "stranded-disconnected" : "stranded"; break;
			}
		}

		nlohmann::json SerializeMessageForFrontend(const Message& message,
		                                           bool defer_tool_call_content = false)
		{
			nlohmann::json message_json;
			message_json["role"] = RoleStr(message.role);
			message_json["content"] = message.content;
			message_json["createdAt"] = message.created_at;
			if (message.native_input_id && uam::input_provenance::ValidNativeInputId(*message.native_input_id)) message_json["nativeInputId"] = *message.native_input_id;
			if (message.role == MessageRole::User && message.input_write_completed && message.native_input_id && uam::input_provenance::ValidNativeInputId(*message.native_input_id)) message_json["inputWriteCompleted"] = true;
			if (message.origin_request_id && uam::input_provenance::ValidOriginRequestId(*message.origin_request_id)) message_json["originRequestId"] = *message.origin_request_id;
			if (message.managed_origin_run_id && uam::uuid::IsCanonicalUuid(*message.managed_origin_run_id)) message_json["managedOriginRunId"] = *message.managed_origin_run_id;
			if (!message.model_id.empty()) message_json["modelId"] = message.model_id;
			if (message.interrupted) message_json["interrupted"] = true;
			if (!message.stop_reason.empty()) message_json["stopReason"] = message.stop_reason;
			if (message.acp_prompt_not_sent) message_json["acpPromptNotSent"] = true;
			if (message.acp_prompt_discarded) message_json["acpPromptDiscarded"] = true;
			if (message.priority_steer) message_json["prioritySteer"] = true;
			if (message.continues_turn) message_json["continuesTurn"] = true;
			if (message.processing_time_ms > 0)
			{
				message_json["processingTimeMs"] = message.processing_time_ms;
			}
			if (!message.checkpoint_sha.empty()) message_json["checkpointSha"] = message.checkpoint_sha;
			if (!message.checkpoint_parent_sha.empty()) message_json["checkpointParentSha"] = message.checkpoint_parent_sha;
			// A turn an excluded provider answered before a handoff shows no provider rather than another provider's name.
			if (!message.provider.empty() && !uam::excluded_provider_data::IsExcluded(message.provider))
			{
				message_json["providerId"] = message.provider;
			}
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
					tool_calls.push_back(SerializeToolCallForFrontend(tool_call, defer_tool_call_content));
				}
				message_json["toolCalls"] = std::move(tool_calls);
			}
			if (!message.blocks.empty())
			{
				nlohmann::json blocks = JsonArrayWithCapacity(message.blocks.size());
				for (const MessageBlock& block : message.blocks)
				{
					if (!block.type.empty() && !block.type.starts_with("native_"))
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
					if (attachment.source == AttachmentSource::LegacyPath && attachment.path.empty())
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

		std::string MessageDigestForFingerprint(const ChatSession& session, bool cache_selected)
		{
			SelectedHistoryDigestCache* cache = cache_selected ? &g_selected_history_digest : nullptr;
			if (cache != nullptr)
			{
				SelectHistoryDigestCache(session.id);
				cache->rehashed_message_count = 0;
				if (!session.messages_loaded)
				{
					cache->messages.clear();
					cache->prefix_hashes.clear();
				}
			}

			if (!session.messages_loaded)
			{
				if (!session.persisted_messages_digest.empty())
				{
					return session.persisted_messages_digest;
				}
				return session.updated_at + ":" + std::to_string(session.persisted_message_count);
			}

			const auto started = std::chrono::steady_clock::now();
			std::uint64_t hash = uam::hashing::kFnv1a64OffsetBasis;

			std::size_t prefix = 0;
			if (cache != nullptr)
			{
				while (prefix < cache->messages.size() && prefix < session.messages.size() &&
				       cache->messages[prefix] == session.messages[prefix]) ++prefix;
				if (prefix != 0) hash = cache->prefix_hashes[prefix - 1];
				cache->messages.resize(prefix);
				cache->prefix_hashes.resize(prefix);
				cache->rehashed_message_count = session.messages.size() - prefix;
			}
			for (std::size_t index = prefix; index < session.messages.size(); ++index)
			{
				const Message& message = session.messages[index];
				chat_pages::AppendMessageDigest(hash, message);
				if (cache != nullptr)
				{
					cache->messages.push_back(message);
					cache->prefix_hashes.push_back(hash);
				}

			}

			// Count belongs after the cached body prefixes so appends do not invalidate settled history.

			const auto finished = std::chrono::steady_clock::now();
			const auto elapsed = finished - started;
			static auto last_slow_digest_report = std::chrono::steady_clock::time_point::min();
			if (elapsed >= std::chrono::milliseconds(100) &&
			    (last_slow_digest_report == std::chrono::steady_clock::time_point::min() ||
			     finished - last_slow_digest_report >= std::chrono::seconds(5)))
			{
				uam::diagnostics::Write("[performance] Loaded transcript digest took " +
					std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) +
					" ms across " + std::to_string(session.messages.size()) + " messages.");
				last_slow_digest_report = finished;
			}
			return chat_pages::FinishMessageDigest(hash, session.messages.size());
		}

		std::size_t MessageCountForFrontend(const ChatSession& session)
		{
			return session.messages_loaded ? session.messages.size() : session.persisted_message_count;
		}

		const uam::CliTerminalState* FindTerminalForChat(const uam::AppState& app, const ChatSession& chat)
		{
			return uam::FindCliTerminalForChat(app, chat);
		}

		void SerializeIdleDeadline(nlohmann::json& state, double started, int timeout)
		{
			if (started <= 0.0) return;
			static const std::int64_t epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			    std::chrono::system_clock::now().time_since_epoch()).count() - static_cast<std::int64_t>(GetAppTimeSeconds() * 1000.0);
			state["idleCountdownStartsAtMs"] = epoch_ms + static_cast<std::int64_t>((started + 60.0) * 1000.0);
			state["idleShutdownAtMs"] = epoch_ms + static_cast<std::int64_t>((started + 60.0 + timeout) * 1000.0);
			state["idleShutdownTimeoutSeconds"] = timeout;
		}

		template <class Writer>
		void WriteChatTerminalSummary(Writer& w, const AppState& app, const ChatSession& chat)
		{
			const bool ready_since_last_select = ChatHasUnseenUpdate(app, chat);

			if (const CliTerminalState* terminal = FindTerminalForChat(app, chat); terminal != nullptr)
			{
				// Live terminals are rare; their JSON keeps the existing idle-deadline helper.
				const bool terminal_processing = uam::CliTerminalLifecycleIsProcessing(*terminal);
				nlohmann::json terminal_json;
				terminal_json["terminalId"] = terminal->terminal_id;
				terminal_json["startupTime"] = terminal->startup_time_s;
				terminal_json["frontendChatId"] = terminal->frontend_chat_id;
				terminal_json["sourceChatId"] = uam::CliTerminalPrimaryChatId(*terminal);
				terminal_json["running"] = terminal->running;
				terminal_json["lifecycleState"] = uam::CliTerminalLifecycleStateLabel(*terminal);
				terminal_json["turnState"] = uam::CliTurnStateLabel(*terminal);
				terminal_json["processing"] = terminal_processing;
				terminal_json["readySinceLastSelect"] = ready_since_last_select;
				terminal_json["active"] = uam::CliTerminalLifecycleIsIdleLive(*terminal);
				terminal_json["pendingSteer"] = !terminal->pending_steer_prompt.empty();
				terminal_json["lastError"] = terminal->last_error;
				if (terminal->running && terminal->lifecycle_state == CliTerminalLifecycleState::Idle && terminal->pending_steer_prompt.empty())
					SerializeIdleDeadline(terminal_json, terminal->idle_interaction_started_time_s, app.settings.cli_idle_timeout_seconds);
				w.Merge(std::move(terminal_json));
				return;
			}

			w.Set("running", false);
			w.Set("lifecycleState", "stopped");
			w.Set("turnState", "idle");
			w.Set("processing", false);
			w.Set("readySinceLastSelect", ready_since_last_select);
			w.Set("active", false);
			w.Set("pendingSteer", false);
			w.Set("lastError", "");
		}

		nlohmann::json SerializeChatTerminalSummary(const AppState& app, const ChatSession& chat)
		{
			nlohmann::json terminal_json = nlohmann::json::object();
			SummaryJsonWriter w(terminal_json);
			WriteChatTerminalSummary(w, app, chat);
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
			constexpr std::size_t kInlineToolContentMaxBytes = 64 * 1024;
			nlohmann::json tool_calls_json = JsonArrayWithCapacity(tool_calls.size());
			for (const AcpToolCallState& tool_call : tool_calls)
			{
				const std::string content = StateSerializer::ToolCallContentForFrontend(tool_call);
				nlohmann::json tool_call_json = {
				    {"id", tool_call.id}, {"title", tool_call.title}, {"kind", tool_call.kind}, {"status", tool_call.status}, {"approvalStatus", tool_call.approval_status}, {"contentDeferred", content.size() > kInlineToolContentMaxBytes}, {"isSubAgent", tool_call.is_sub_agent}, {"subAgentId", tool_call.sub_agent_id}, {"subAgentTitle", tool_call.sub_agent_title},
				};
				AddManagedResultCard(tool_call_json, tool_call.title, tool_call.is_sub_agent, tool_call.sub_agent_id, tool_call.content);
				if (tool_call.sub_agent_children_authoritative || !tool_call.sub_agent_children.empty())
					tool_call_json["subAgentChildren"] = SerializeSubAgentChildren(tool_call.sub_agent_children);
				if (tool_call.is_sub_agent) tool_call_json["currentActivity"] = LatestChildActivity(tool_call.child_activity);
				if (content.size() <= kInlineToolContentMaxBytes)
					tool_call_json["content"] = content;
				else
					tool_call_json["contentDigest"] = std::to_string(std::hash<std::string>{}(content)) + ":" + std::to_string(std::hash<std::string>{}(tool_call.permission_review_decision)) + ":" + std::to_string(std::hash<std::string>{}(tool_call.permission_review_reason));
				tool_calls_json.push_back(std::move(tool_call_json));
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

		nlohmann::json SerializeAcpCommands(const std::vector<AcpCommandState>& commands)
		{
			nlohmann::json commands_json = JsonArrayWithCapacity(commands.size());
			for (const AcpCommandState& command : commands)
			{
				commands_json.push_back({
				    {"name", command.name},
				    {"description", command.description},
				    {"inputHint", command.input_hint},
				});
			}
			return commands_json;
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
				    {"contextWindowTokens", model.context_window_tokens ? nlohmann::json(*model.context_window_tokens) : nlohmann::json(nullptr)},
				    {"maxOutputTokens", model.max_output_tokens ? nlohmann::json(*model.max_output_tokens) : nlohmann::json(nullptr)},
				});
			}
			return models_json;
		}

		nlohmann::json SerializeAcpConfigOptions(const std::vector<AcpConfigOptionState>& options)
		{
			nlohmann::json result = nlohmann::json::array();
			for (const AcpConfigOptionState& option : options)
			{
				nlohmann::json choices = nlohmann::json::array();
				for (const AcpConfigOptionChoiceState& choice : option.choices)
				{
					choices.push_back({{"value", choice.value}, {"name", choice.name}, {"description", choice.description}});
				}
				result.push_back({{"id", option.id}, {"name", option.name}, {"description", option.description}, {"category", option.category}, {"currentValue", option.current_value}, {"options", std::move(choices)}});
			}
			return result;
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
				if (!event.title.empty()) event_json["title"] = event.title;
				if (event.compaction_tokens_before >= 0) event_json["tokensBefore"] = event.compaction_tokens_before;
				if (event.compaction_tokens_after >= 0) event_json["tokensAfter"] = event.compaction_tokens_after;
				turn_events_json.push_back(std::move(event_json));
			}
			return turn_events_json;
		}

		nlohmann::json SerializeAcpTokenUsageBreakdown(const AcpTokenUsageBreakdownState& usage)
		{
			return {
			    {"inputTokens", usage.input_tokens},
			    {"cachedInputTokens", usage.cached_input_tokens},
			    {"cacheWriteInputTokens", usage.cache_write_input_tokens},
			    {"outputTokens", usage.output_tokens},
			    {"reasoningOutputTokens", usage.reasoning_output_tokens},
			    {"totalTokens", usage.total_tokens},
			};
		}

		nlohmann::json SerializeAcpRateLimitWindow(const AcpRateLimitWindowState& window)
		{
			if (!window.available)
			{
				return nullptr;
			}
			nlohmann::json serialized;
			serialized["usedPercent"] = window.used_percent;
			serialized["resetsAt"] = window.resets_at > 0 ? nlohmann::json(window.resets_at) : nlohmann::json(nullptr);
			serialized["windowDurationMinutes"] = window.window_duration_minutes > 0 ? nlohmann::json(window.window_duration_minutes) : nlohmann::json(nullptr);
			return serialized;
		}

		nlohmann::json SerializeAcpProviderUsage(const AcpProviderUsageState& usage)
		{
			if (!usage.token_usage.available && !usage.rate_limits.available && !usage.context_usage.available)
			{
				return nullptr;
			}

			nlohmann::json serialized;
			serialized["contextUsage"] = usage.context_usage.available
			    ? nlohmann::json{{"updatedAt", usage.context_usage.updated_at_ms}, {"usedTokens", usage.context_usage.used_tokens}, {"windowTokens", usage.context_usage.window_tokens}}
			    : nlohmann::json(nullptr);
			serialized["tokenUsage"] = usage.token_usage.available
			    ? nlohmann::json{
			          {"updatedAt", usage.token_usage.updated_at_ms},
			          {"total", SerializeAcpTokenUsageBreakdown(usage.token_usage.total)},
			          {"last", usage.token_usage.last_available ? SerializeAcpTokenUsageBreakdown(usage.token_usage.last) : nlohmann::json(nullptr)},
			          {"modelContextWindow", usage.token_usage.model_context_window > 0 ? nlohmann::json(usage.token_usage.model_context_window) : nlohmann::json(nullptr)},
			      }
			    : nlohmann::json(nullptr);
			serialized["rateLimits"] = usage.rate_limits.available
			    ? nlohmann::json{
			          {"updatedAt", usage.rate_limits.updated_at_ms},
			          {"limitId", usage.rate_limits.limit_id},
			          {"limitName", usage.rate_limits.limit_name},
			          {"primary", SerializeAcpRateLimitWindow(usage.rate_limits.primary)},
			          {"secondary", SerializeAcpRateLimitWindow(usage.rate_limits.secondary)},
			          {"credits", usage.rate_limits.credits.available
			                          ? nlohmann::json{{"hasCredits", usage.rate_limits.credits.has_credits}, {"unlimited", usage.rate_limits.credits.unlimited}, {"balance", usage.rate_limits.credits.balance.empty() ? nlohmann::json(nullptr) : nlohmann::json(usage.rate_limits.credits.balance)}}
			                          : nlohmann::json(nullptr)},
			          {"individualLimit", usage.rate_limits.individual_limit.available
			                                   ? nlohmann::json{{"limit", usage.rate_limits.individual_limit.limit}, {"used", usage.rate_limits.individual_limit.used}, {"remainingPercent", usage.rate_limits.individual_limit.remaining_percent}, {"resetsAt", usage.rate_limits.individual_limit.resets_at}}
			                                   : nlohmann::json(nullptr)},
			          {"spendControlReached", usage.rate_limits.spend_control_reached_available ? nlohmann::json(usage.rate_limits.spend_control_reached) : nlohmann::json(nullptr)},
			          {"planType", usage.rate_limits.plan_type.empty() ? nlohmann::json(nullptr) : nlohmann::json(usage.rate_limits.plan_type)},
			          {"rateLimitReachedType", usage.rate_limits.rate_limit_reached_type.empty() ? nlohmann::json(nullptr) : nlohmann::json(usage.rate_limits.rate_limit_reached_type)},
			      }
			    : nlohmann::json(nullptr);
			return serialized;
		}

		nlohmann::json SerializeAcpPermissionOption(const AcpPermissionOptionState& option)
		{
			return {
			    {"id", option.id},
			    {"name", option.name},
			    {"kind", option.kind},
			};
		}

		nlohmann::json SerializePendingAcpPermission(const AcpSessionState& session)
		{
			const AcpPendingPermissionState& permission = session.pending_permission;
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
			permission_json["safetyRisk"] = permission.safety_risk;
			permission_json["safetyTier"] = permission.safety_tier;
			permission_json["safetyRequiresApproval"] = permission.safety_requires_approval;

			nlohmann::json options = JsonArrayWithCapacity(permission.options.size());
			for (const AcpPermissionOptionState& option : permission.options)
			{
				if (acp_detail::CapturedReadCeilingAllowsPermissionResponse(session, permission, option.id, false, nullptr))
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
			question_json["isMultiple"] = question.is_multiple;
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

		std::size_t ManagedChildRunCountForChat(const AppState& app, const ChatSession& chat)
		{
			return static_cast<std::size_t>(std::ranges::count_if(app.agent_runs, [&chat](const AgentRun& run)
			{
				return run.root_chat_id == chat.id && (run.status == "queued" || run.status == "running");
			}));
		}

		bool ManualCompactionAvailable(const AppState& app, const ChatSession& chat, const AcpSessionState* session)
		{
			// Advisory only: unknown/stopped sessions stay unavailable; the real request revalidates all permission.
			if (!session || !session->running || !session->initialized || !session->session_ready || session->lifecycle_state != acp_detail::kAcpLifecycleReady ||
			    session->chat_id != chat.id || session->provider_id != chat.provider_id || session->session_id.empty() || session->session_id != chat.native_session_id ||
			    session->process_execution_host_id != chat.execution_host_id || session->model_discovery_only || session->goal_internal_session ||
			    !session->transport_ownership_error.empty() || chat.imported_read_only || chat.side_cleanup_requested || chat.acp_queue_storage_error ||
			    AcpStopInProgress(app, chat.id) || session->local_stop_pending || session->remote_stop_pending || session->remote_stop_unconfirmed || AcpSessionHasPendingCancel(*session) ||
			    !chat.acp_queued_prompts.empty() || chat.acp_queue_held || AcpSessionHasActiveTurn(*session) || AcpSessionIsWaitingForInput(*session) ||
			    !session->queued_prompt.empty() || !session->queued_user_prompts.empty() || session->queue_held || session->pending_compaction ||
			    session->manual_compaction_pending || session->context_compaction_in_progress || session->pending_agent_input || session->pending_prompt_delivery ||
			    session->pending_queued_history || session->pending_local_launch || session->turn_checkpoint_preflight_pending || session->turn_checkpoint_commit_pending ||
			    AcpSessionHasPendingRuntimeRequest(*session) || !session->pending_request_methods.empty() || session->pending_local_session_receipt)
				return false;
			const auto workspace = app.worktree_launch_preparations.find(chat.id);
			const auto branch = app.branch_preparations.find(chat.id);
			if (app.worktree_operation_chat_ids.contains(chat.id) || (branch != app.branch_preparations.end() && !branch->second.expired()) ||
			    (workspace != app.worktree_launch_preparations.end() && !(workspace->second->saved && *workspace->second->saved))) return false;
			const bool remote = !chat.execution_host_id.empty() && chat.execution_host_id != "local";
			if (remote && (session->remote_runner_protocol_version < 3 || !chat.remote_process_exists || chat.remote_process_control_token.empty() ||
			    session->remote_output_delivery_token != chat.remote_process_control_token || !chat.remote_pending_requests.empty() ||
			    !session->pending_remote_output_ack_line.empty() || !session->pending_remote_source_exit_ack_line.empty())) return false;
			const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session->provider_id);
			if (!runtime.SupportsAcpCompaction() || session->protocol_kind != runtime.AcpProtocolKind()) return false;
			std::string method;
			const nlohmann::json request = runtime.OnAcpBuildCompact(*session, 0, method);
			return !request.is_null() && !request.empty() && !method.empty();
		}

		template <class Writer>
		void WriteManualCompactionAvailability(Writer& w, const AppState& app, const ChatSession& chat, const AcpSessionState* session)
		{
			w.Set("available", ManualCompactionAvailable(app, chat, session));
			w.Set("providerId", chat.provider_id);
			w.Set("modelId", chat.model_id);
			w.Set("nativeSessionId", session ? session->session_id : ResolvedAcpSessionIdForChat(app, chat));
		}

		nlohmann::json SerializeManualCompactionAvailability(const AppState& app, const ChatSession& chat, const AcpSessionState* session)
		{
			nlohmann::json result = nlohmann::json::object();
			SummaryJsonWriter w(result);
			WriteManualCompactionAvailability(w, app, chat, session);
			return result;
		}

		template <class Writer>
		void WriteStoppedAcpSessionSummary(Writer& w, const AppState& app, const ChatSession& chat,
		                                   bool ready_since_last_select, CatalogSnapshotCache& cache)
		{
			if (cache.compact_catalogs) w.Merge(CatalogFingerprintForChat(app, chat, cache));
			else w.Merge(CatalogSnapshotForChat(app, chat, cache));
			const std::string session_id = ResolvedAcpSessionIdForChat(app, chat);
			w.Set("sessionId", session_id);
			w.Set("providerId", chat.provider_id);
			w.Object("manualCompaction", [&](auto& compaction) { WriteManualCompactionAvailability(compaction, app, chat, nullptr); });
			w.Set("uamAgentExecutionCapability", AgentDefinitionService::ExecutionCapabilityForProvider(chat.provider_id));
			w.Set("protocolKind", "");
			w.Set("threadId", session_id);
			w.Set("running", false);
			w.Set("processing", (chat.claude_quota_continuation.state == "waiting" || chat.claude_quota_continuation.state == "held"));
			w.Set("backgroundTaskCount", ManagedChildRunCountForChat(app, chat));
			w.Set("readySinceLastSelect", ready_since_last_select);
			w.Set("lastStopReason", chat.last_stop_reason);
			w.Set("attentionKind", nullptr);
			w.Set("lifecycleState", chat.claude_quota_continuation.state == "waiting" ? "waiting_quota" : chat.claude_quota_continuation.state == "held" ? "quota_held" : "stopped");
			w.Set("lastError", "");
			w.Set("timeoutStop", nullptr);
			w.Set("recentStderr", "");
			w.Set("lastExitCode", nullptr);
			w.Set("diagnostics", nlohmann::json::array());
			w.Set("toolCalls", nlohmann::json::array());
			w.Set("planSummary", "");
			w.Set("planEntries", nlohmann::json::array());
			w.Set("availableCommands", nlohmann::json::array());
			w.Set("availableModes", nlohmann::json::array());
			w.Set("currentModeId", chat.approval_mode);
			w.Set("currentModelId", FallbackAcpCurrentModelForChat(app, chat));
			w.Set("turnEvents", nlohmann::json::array());
			w.Set("turnUserMessageIndex", -1);
			w.Set("turnAssistantMessageIndex", -1);
			w.Set("turnSerial", 0);
			std::vector<const AcpQueuedUserPromptState*> prompts;
			const std::size_t dispatched = std::min(chat.acp_dispatched_queued_prompt_count, chat.acp_queued_prompts.size());
			for (std::size_t index = dispatched; index < chat.acp_queued_prompts.size(); ++index)
				prompts.push_back(&chat.acp_queued_prompts[index]);
			nlohmann::json queues = nlohmann::json::object();
			SerializePromptAdmissionQueues(queues, app, chat, nullptr, prompts);
			w.Merge(std::move(queues));
			w.Set("unsentMessageIndices", KnownUnsentAcpMessageIndices(app, chat));
			w.Set("recoverableUnsentMessageIndices", RecoverableUnsentAcpMessageIndices(app, chat));
			w.Set("waitIsStale", false);
			w.Set("waitStaleReason", "");
			w.Set("waitSeconds", 0);
			w.Set("pendingPermission", nullptr);
			w.Set("pendingUserInput", nullptr);
			const auto approval = UamControlService::PendingApprovalForChat(app, chat.id);
			if (approval.has_value())
			{
				w.Set("pendingUserInput", SerializePendingAcpUserInput(*approval));
				w.Set("attentionKind", "question");
			}
			w.Set("providerUsage", nullptr);
		}

		nlohmann::json SerializeStoppedAcpSessionSummary(const AppState& app, const ChatSession& chat,
		                                                 bool ready_since_last_select, CatalogSnapshotCache& cache)
		{
			nlohmann::json acp_json = nlohmann::json::object();
			SummaryJsonWriter w(acp_json);
			WriteStoppedAcpSessionSummary(w, app, chat, ready_since_last_select, cache);
			return acp_json;
		}

		nlohmann::json SerializeAcpSessionSummary(const AppState& app, const ChatSession& chat,
		                                          CatalogSnapshotCache& cache)
		{
			const AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
			const bool ready_since_last_select = ChatHasUnseenUpdate(app, chat);
			if (session == nullptr)
			{
				return SerializeStoppedAcpSessionSummary(app, chat, ready_since_last_select, cache);
			}

			nlohmann::json acp_json = CatalogSnapshotForChat(app, chat, cache);
			acp_json["sessionId"] = session->session_id;
			acp_json["providerId"] = session->provider_id;
			acp_json["manualCompaction"] = SerializeManualCompactionAvailability(app, chat, session);
			acp_json["uamAgentExecutionCapability"] =
			    session->active_uam_agent_execution_capability;
			acp_json["protocolKind"] = session->protocol_kind;
			acp_json["threadId"] = session->codex_thread_id.empty() ? session->session_id : session->codex_thread_id;
			acp_json["running"] = session->running;
			acp_json["processing"] = session->processing || (chat.claude_quota_continuation.state == "waiting" || chat.claude_quota_continuation.state == "held");
			acp_json["compacting"] = session->pending_compaction != nullptr || (session->running && (session->manual_compaction_pending || session->context_compaction_in_progress));
			acp_json["backgroundTaskCount"] = ProviderRuntimeRegistry::ResolveById(session->provider_id).AcpBackgroundTaskCount(*session) + ManagedChildRunCountForChat(app, chat);
			if (AcpSessionCanIdleShutdown(*session, chat))
				SerializeIdleDeadline(acp_json, session->idle_interaction_started_time_s, app.settings.cli_idle_timeout_seconds);
			acp_json["readySinceLastSelect"] = ready_since_last_select;
			acp_json["lastStopReason"] = chat.last_stop_reason;
			const std::optional<AcpPendingUserInputState> uam_control_approval =
				UamControlService::PendingApprovalForChat(app, chat.id);
			const std::string attention_kind =
				uam_control_approval.has_value() && session->pending_user_input.request_id_json.empty() &&
					session->pending_permission.request_id_json.empty()
					? "question"
					: AcpAttentionKindForFrontend(*session);
			acp_json["attentionKind"] = uam::nlohmann_json::StringOrNull(attention_kind);
			acp_json["lifecycleState"] = chat.claude_quota_continuation.state == "waiting" ? "waiting_quota" : chat.claude_quota_continuation.state == "held" ? "quota_held" : session->lifecycle_state;
			if (uam::compute_pools::IsWaiting(*session, GetAppTimeSeconds()))
				acp_json["computePoolWait"] = {{"poolName", session->compute_pool_wait_name}, {"inUse", session->compute_pool_wait_in_use}, {"limit", session->compute_pool_wait_limit}};
			acp_json["lastError"] = session->last_error;
			acp_json["timeoutStop"] = !session->last_error.empty() && session->timeout_stop_error == session->last_error
			    ? (session->timeout_stop_kind.empty()
			           ? nlohmann::json{{"label", session->timeout_stop_label}, {"seconds", session->timeout_stop_seconds}}
			           : nlohmann::json{{"label", session->timeout_stop_label}, {"seconds", session->timeout_stop_seconds}, {"kind", session->timeout_stop_kind},
			                            {"channel", session->timeout_stop_channel}, {"count", session->timeout_stop_count}, {"example", session->timeout_stop_example}})
			    : nlohmann::json(nullptr);
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
			acp_json["availableCommands"] = SerializeAcpCommands(session->available_commands);
			acp_json["availableModes"] = SerializeAcpModes(session->available_modes);
			acp_json["currentModeId"] = uam::strings::NonEmptyOrFallback(session->current_mode_id, chat.approval_mode);
			if (session->session_ready && !session->available_models.empty())
				acp_json["availableModels"] = SerializeAcpModels(session->available_models);
			else
				acp_json["availableModels"] = ProviderModelCatalogService::MergeAcpModelArrays(std::move(acp_json["availableModels"]), SerializeAcpModels(session->available_models), true);
			const auto* model_host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id.empty() ? "local" : chat.execution_host_id);
			ProviderModelCatalogService::ApplyLaunchOptionMetadata(acp_json["availableModels"], chat.provider_id, model_host != nullptr ? CheckedProviderConfigurationVersion(app, chat.provider_id, *model_host) : std::string{});
			if (session->session_ready || !session->available_config_options.empty()) acp_json["configOptions"] = SerializeAcpConfigOptions(session->available_config_options);
			acp_json["nativeModelOptions"] = ProviderRuntimeRegistry::ResolveById(session->provider_id).OnAcpNativeModelOptionsForFrontend(*session);
			acp_json["currentModelId"] = uam::strings::NonEmptyOrFallback(session->current_model_id, FallbackAcpCurrentModelForChat(app, chat));
			acp_json["turnEvents"] = SerializeAcpTurnEvents(session->turn_events);
			acp_json["turnUserMessageIndex"] = session->turn_user_message_index;
			acp_json["turnAssistantMessageIndex"] = session->turn_assistant_message_index;
			acp_json["turnSerial"] = session->turn_serial;
			std::vector<const AcpQueuedUserPromptState*> prompts;
			for (const AcpQueuedUserPromptState& prompt : session->queued_user_prompts) prompts.push_back(&prompt);
			SerializePromptAdmissionQueues(acp_json, app, chat, session, prompts);
			acp_json["unsentMessageIndices"] = KnownUnsentAcpMessageIndices(app, chat);
			acp_json["recoverableUnsentMessageIndices"] = RecoverableUnsentAcpMessageIndices(app, chat);
			acp_json["goalTurnKind"] = session->goal_turn_kind;
			acp_json["waitIsStale"] = session->wait_is_stale;
			acp_json["waitStaleReason"] = session->wait_stale_reason;
			acp_json["waitSeconds"] = session->wait_started_time_s > 0.0 ? static_cast<int>(std::max(0.0, GetAppTimeSeconds() - session->wait_started_time_s)) : 0;
			acp_json["pendingPermission"] = SerializePendingAcpPermission(*session);
			if (uam_control_approval.has_value() && session->pending_user_input.request_id_json.empty() &&
				session->pending_permission.request_id_json.empty())
			{
				acp_json["pendingUserInput"] = SerializePendingAcpUserInput(*uam_control_approval);
			}
			else
			{
				acp_json["pendingUserInput"] = SerializePendingAcpUserInput(session->pending_user_input);
			}
			acp_json["providerUsage"] = SerializeAcpProviderUsage(session->provider_usage);

			return acp_json;
		}

		template <class Writer>
		void WriteFingerprintSession(Writer& w, const AppState& app, const ChatSession& chat,
		                             CatalogSnapshotCache& cache, bool cache_selected = false)
		{
			WriteSessionSummaryFields(w, chat, uam::paths::Utf8PathString(WorkspaceSnapshotForChat(app, chat, cache)), cache_selected);
			w.Set("memoryEnabled", uam::memory_levels::IsEnabled(uam::memory_policy::Resolve(app.settings, chat)));
			const std::unordered_map<std::string, std::string>::const_iterator routing_wait = app.execution_routing_waits.find(chat.id);
			w.Set("routingWaitReason", routing_wait == app.execution_routing_waits.end() ? "" : routing_wait->second);
			w.Set("routingUnresolvedReservation", app.execution_routing_restored_unresolved_chat_ids.contains(chat.id));
			w.Set("attentionRevision", ChatDomainService().AttentionRevision(app, chat));
			w.Set("attentionSource", chat.attention_source);
			w.Object("cliTerminal", [&](auto& terminal) { WriteChatTerminalSummary(terminal, app, chat); });
			if (FindAcpSessionForChat(app, chat.id) == nullptr)
			{
				const bool ready_since_last_select = ChatHasUnseenUpdate(app, chat);
				w.Object("acpSession", [&](auto& acp) { WriteStoppedAcpSessionSummary(acp, app, chat, ready_since_last_select, cache); });
			}
			else
			{
				nlohmann::json acp = SerializeAcpSessionSummary(app, chat, cache);
				// Mirrors cef_push's StripVolatileAcpWaitTelemetry: the wait counter alone never publishes a summary.
				if constexpr (Writer::kDigest) acp.erase("waitSeconds");
				w.Set("acpSession", std::move(acp));
			}
			w.Object("computerUse", [&](auto& computer_use) { WriteComputerUseState(computer_use, app, chat); });

			// Serialize goals for fingerprint
			if (!chat.goals.empty())
			{
				nlohmann::json goals_arr = JsonArrayWithCapacity(chat.goals.size());
				for (const auto& goal : chat.goals)
				{
					nlohmann::json goal_json;
					goal_json["id"] = goal.id;
					goal_json["objective"] = goal.objective;
					goal_json["status"] = GoalStatusToString(goal.status);
					goal_json["tokenBudget"] = goal.token_budget;
					goal_json["tokensUsed"] = goal.tokens_used;
					goal_json["blockedTurnCount"] = goal.blocked_turn_count;
					goal_json["lastBlocker"] = goal.last_blocker.empty() ? nullptr : nlohmann::json(goal.last_blocker);
					goal_json["lastBlockerKind"] = goal.last_blocker_kind.empty() ? nullptr : nlohmann::json(goal.last_blocker_kind);
					goal_json["lastDiagnostic"] = goal.last_diagnostic.empty() ? nullptr : nlohmann::json(goal.last_diagnostic);
					goal_json["lastDecisionSource"] = goal.last_decision_source.empty() ? nullptr : nlohmann::json(goal.last_decision_source);
					goal_json["pendingContinuation"] = chat.goal_pending_continuation_id == goal.id;
					goal_json["completedItems"] = goal.completed_items;
					goal_json["remainingItems"] = goal.remaining_items;
					goal_json["currentStep"] = goal.current_step;
					goal_json["lastVerification"] = goal.last_verification;
					goal_json["lastNextPrompt"] = goal.last_next_prompt;
					goal_json["sameNextPromptCount"] = goal.same_next_prompt_count;
					goal_json["loopCount"] = goal.loop_count;
					goal_json["createdAt"] = goal.created_at;
					goal_json["updatedAt"] = goal.updated_at;
					goal_json["executionOwner"] = goal.execution_owner == "provider" ? "provider" : "uam";
					goal_json["providerCommand"] = goal.execution_owner == "provider" ? goal.provider_command : "";
					goal_json["workerModelId"] = goal.worker_model_id;
					goal_json["reviewerModelId"] = goal.reviewer_model_id;
					goal_json["creator"] = goal.creator == "model" ? "model" : "user";
					goal_json["creatorProviderId"] = uam::excluded_provider_data::IsExcluded(goal.creator_provider_id) ? "" : goal.creator_provider_id;
					goal_json["creatorAgentId"] = goal.creator_agent_id;
					goal_json["creatorRunId"] = goal.creator_run_id;
					goals_arr.push_back(std::move(goal_json));
				}
				w.Set("goals", std::move(goals_arr));
			}
		}

		nlohmann::json SerializeFingerprintSession(const AppState& app, const ChatSession& chat,
		                                           CatalogSnapshotCache& cache, bool cache_selected = false)
		{
			nlohmann::json chat_json;
			SummaryJsonWriter w(chat_json);
			WriteFingerprintSession(w, app, chat, cache, cache_selected);
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
				terminal_json["startupTime"] = terminal.startup_time_s;
				terminal_json["frontendChatId"] = terminal.frontend_chat_id;
				terminal_json["sourceChatId"] = uam::CliTerminalPrimaryChatId(terminal);
				terminal_json["attachedSessionId"] = CliAttachedSessionIdForDiagnostics(app, terminal);
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
			return uam::provider_ids::ContainsProviderId(uam::provider_ids::kAllCliProviderIds, provider_id) && ProviderRuntimeRegistry::ResolveById(provider_id).CliVersionPolicy() != nullptr ? provider_id : std::string{};
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

		/** Retains local compatibility while binding remote observations to the current connection. */
		bool CliObservationMatchesHost(const ExecutionHost& observed, const ExecutionHost& current)
		{
			return current.id == execution_hosts::kLocalHostId
			    ? observed.id == execution_hosts::kLocalHostId
			    : execution_hosts::SameConnection(observed, current);
		}

		nlohmann::json SerializeCliVersionEntry(const AppState& app, const ProviderCliCompatibilityService& service, const std::string& provider_id, const ExecutionHost& execution_host = ExecutionHost{})
		{
			const bool check_running_for_provider = app.runtime_cli_version_check_task.running &&
			                                        NormalizedCliVersionManagedProviderId(app.runtime_cli_version_provider_id) == provider_id &&
			                                        CliObservationMatchesHost(app.runtime_cli_version_check_task.execution_host, execution_host);
			const auto install = app.runtime_cli_install_tasks.find(CliProviderVersionStateKey(provider_id, execution_host.id));
			const bool install_running_for_provider = install != app.runtime_cli_install_tasks.end() && install->second.running && CliObservationMatchesHost(install->second.execution_host, execution_host);
			const auto state_it = app.runtime_cli_versions_by_provider_id.find(CliProviderVersionStateKey(provider_id, execution_host.id));
			const bool has_provider_state = state_it != app.runtime_cli_versions_by_provider_id.end() &&
			    CliObservationMatchesHost(state_it->second.execution_host, execution_host) &&
			    (state_it->second.provider_id.empty() || NormalizedCliVersionManagedProviderId(state_it->second.provider_id) == provider_id);
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
				status = service.CompatibilityStatusForProvider(provider_id, provider_state.installed_version);
			}

			nlohmann::json provider_json;
			provider_json["providerId"] = provider_id;
			const ProviderCliPolicy* policy = ProviderRuntimeRegistry::ResolveById(provider_id).CliVersionPolicy();
			provider_json["canInstall"] = policy != nullptr && (!policy->npm_package.empty() || !policy->homebrew_package.empty() || !policy->winget_package.empty());
			provider_json["routingConnectionIdentity"] = ExecutionRoutingConnectionIdentity(execution_host);
			if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, provider_id); profile != nullptr)
				provider_json["routingProviderConfigurationIdentity"] = ExecutionRoutingProviderIdentity(app, *profile, execution_host.id);
			if (execution_host.id != execution_hosts::kLocalHostId)
			{
				provider_json["executionHostId"] = execution_host.id;
				provider_json["executionHostName"] = execution_host.label;
			}
			provider_json["installedVersion"] = provider_state.installed_version;
			provider_json["selectedVersion"] = selected_version;
			provider_json["availableVersions"] = std::move(versions);
			provider_json["preferredVersion"] = preferred_version;
			provider_json["verifiedVersion"] = service.VerifiedVersionForProvider(provider_id);
			provider_json["verifiedAt"] = service.VerifiedAtForProvider(provider_id);
			provider_json["status"] = status;
			provider_json["message"] = provider_state.message;
			provider_json["checkError"] = provider_state.check_error;
			const std::unordered_map<std::string, PendingCliUpdate>::const_iterator pending = app.pending_cli_updates.find(CliProviderVersionStateKey(provider_id, execution_host.id));
			const bool update_pending_for_provider = pending != app.pending_cli_updates.end() &&
			    CliObservationMatchesHost(pending->second.execution_host, execution_host) &&
			    (pending->second.provider_id.empty() || NormalizedCliVersionManagedProviderId(pending->second.provider_id) == provider_id);
			provider_json["running"] = check_running_for_provider || install_running_for_provider || update_pending_for_provider;
			provider_json["blockingChatIds"] = ProviderCliBlockingChatIds(app, provider_id, execution_host.id);
			provider_json["installMethod"] = provider_state.install_method;
			provider_json["lastInstallStatus"] = provider_state.last_install_status;
			provider_json["lastCommand"] = install_running_for_provider
			                                   ? install->second.command_preview
			                                   : (check_running_for_provider ? app.runtime_cli_version_check_task.command_preview : provider_state.install_command);
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

			nlohmann::json remote_providers = nlohmann::json::array();
			std::unordered_set<std::string> seen_host_ids;
			for (const ExecutionHost& host : app.settings.execution_hosts)
			{
				if (host.id == execution_hosts::kLocalHostId || host.transport != "ssh" ||
				    !execution_hosts::IsPortableId(host.id) || !execution_hosts::IsSafeSshAlias(host.ssh_alias) ||
				    !seen_host_ids.insert(host.id).second) continue;
				// Curated policy is available before probing; installed state remains explicitly unknown.
				for (const std::string& provider_id : seen_provider_ids)
					remote_providers.push_back(SerializeCliVersionEntry(app, service, provider_id, host));
			}
			std::sort(remote_providers.begin(), remote_providers.end(), [](const nlohmann::json& left, const nlohmann::json& right)
			{
				return std::pair{left["executionHostId"].get<std::string>(), left["providerId"].get<std::string>()} <
				       std::pair{right["executionHostId"].get<std::string>(), right["providerId"].get<std::string>()};
			});
			return {
			    {"providers", std::move(providers)},
			    {"remoteProviders", std::move(remote_providers)},
			};
		}

		nlohmann::json SerializeFoldersForFrontend(const std::vector<ChatFolder>& folders, const std::filesystem::path& data_root)
		{
			nlohmann::json folders_json = JsonArrayWithCapacity(folders.size());
			for (const ChatFolder& folder : folders)
			{
				nlohmann::json object = StateSerializer::SerializeFolder(folder, false);
				uam::icons::AddFrontendAsset(object, data_root, folder.custom_icon);
				folders_json.push_back(std::move(object));
			}
			return folders_json;
		}

		nlohmann::json SerializeResourceCollectionsForFrontend(const std::vector<ResourceCollection>& collections, const std::filesystem::path& data_root)
		{
			nlohmann::json collections_json = JsonArrayWithCapacity(collections.size());
			for (const ResourceCollection& collection : collections)
			{
				nlohmann::json object = StateSerializer::SerializeResourceCollection(collection);
				uam::icons::AddFrontendAsset(object, data_root, collection.custom_icon);
				for (std::size_t i = 0; i < collection.references.size(); ++i)
					uam::icons::AddFrontendAsset(object["references"][i], data_root, collection.references[i].custom_icon);
				collections_json.push_back(std::move(object));
			}
			return collections_json;
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

		nlohmann::json SerializeShellActionsForFrontend(const std::vector<ShellAction>& actions)
		{
			nlohmann::json result = JsonArrayWithCapacity(actions.size());
			for (const ShellAction& action : actions)
			{
				// An excluded provider's actions stay saved but do not exist in this build's interface.
				if (uam::excluded_provider_data::IsExcluded(action.provider_id)) continue;
				result.push_back({
				    {"id", action.id},
				    {"label", action.label},
				    {"skillPath", action.skill_path},
				    {"providerId", action.provider_id},
				    {"modelId", action.model_id},
				    {"groupPath", action.group_path},
				    {"acceptsFiles", action.accepts_files},
				    {"acceptsFolders", action.accepts_folders},
				    {"enabled", action.enabled},
				    {"openWorkspace", action.open_workspace},
				});
			}
			return result;
		}

		bool IsInternalChat(const ChatSession& chat)
		{
			return !chat.agent_run_id.empty() || !chat.goal_owner_chat_id.empty();
		}

		std::unordered_map<std::string, std::size_t> ManagedSubagentCounts(const AppState& app)
		{
			std::unordered_map<std::string, std::size_t> counts;
			for (const AgentRun& run : app.agent_runs)
				++counts[run.root_chat_id];
			ManagedSubagentControlService::AddRestartInspectionCounts(app, counts);
			return counts;
		}

		std::string SelectedVisibleChatId(const AppState& app)
		{
			const ChatSession* selected = ChatDomainService().SelectedChat(app);
			if (selected != nullptr && !IsInternalChat(*selected)) return selected->id;
			if (selected != nullptr && !selected->goal_owner_chat_id.empty())
			{
				if (const ChatSession* owner = ChatDomainService().FindChatById(app, selected->goal_owner_chat_id);
				    owner != nullptr && !IsInternalChat(*owner))
				{
					return owner->id;
				}
			}
			for (const ChatSession& chat : app.chats)
			{
				if (!IsInternalChat(chat)) return chat.id;
			}
			return "";
		}

		/// <summary>One fingerprint chat entry: the compact session summary plus its managed subagent count.</summary>
		template <class Writer>
		void WriteFingerprintChat(Writer& w, const AppState& app, const ChatSession& chat, CatalogSnapshotCache& cache,
		                          bool selected, std::size_t managed_count)
		{
			WriteFingerprintSession(w, app, chat, cache, selected);
			w.Set("managedSubagentCount", managed_count);
		}

		/// <summary>Visits visible chats in catalog order with their selection and managed counts, reporting slow scans.</summary>
		template <class Visit>
		void ScanFingerprintChats(const AppState& app, Visit&& visit)
		{
			const auto managed_counts = ManagedSubagentCounts(app);
			const std::string selected_chat_id = SelectedVisibleChatId(app);
			SelectHistoryDigestCache(selected_chat_id);
			const auto chats_started = std::chrono::steady_clock::now();
			std::chrono::microseconds slowest_chat{0};
			std::size_t loaded_chats = 0;
			for (const auto& chat : app.chats)
			{
				if (IsInternalChat(chat)) continue;
				const auto chat_started = std::chrono::steady_clock::now();
				const auto managed_count = managed_counts.find(chat.id);
				visit(chat, chat.id == selected_chat_id, managed_count == managed_counts.end() ? std::size_t{0} : managed_count->second);
				slowest_chat = std::max(slowest_chat, std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - chat_started));
				loaded_chats += chat.messages_loaded ? 1 : 0;
			}
			const auto chats_finished = std::chrono::steady_clock::now();
			static auto last_slow_chat_scan_report = std::chrono::steady_clock::time_point::min();
			if (chats_finished - chats_started >= std::chrono::milliseconds(250) &&
			    (last_slow_chat_scan_report == std::chrono::steady_clock::time_point::min() ||
			     chats_finished - last_slow_chat_scan_report >= std::chrono::seconds(5)))
			{
				uam::diagnostics::Write("[performance] Fingerprint chat scan took " +
					std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(chats_finished - chats_started).count()) +
					" ms; loaded=" + std::to_string(loaded_chats) +
					", slowestChatUs=" + std::to_string(slowest_chat.count()) +
					", acpSessions=" + std::to_string(app.acp_sessions.size()) +
					", terminals=" + std::to_string(app.cli_terminals.size()) + ".");
				last_slow_chat_scan_report = chats_finished;
			}
		}

	} // anonymous namespace

	// ---------------------------------------------------------------------------
	// StateSerializer implementation
	// ---------------------------------------------------------------------------

	nlohmann::json StateSerializer::SerializeStartupNativeImports(const StartupNativeImportSnapshot& snapshot)
	{
		nlohmann::json candidates = nlohmann::json::array();
		for (const StartupNativeImportCandidate& candidate : snapshot.candidates)
		{
			candidates.push_back({{"candidateId", candidate.candidate_id}, {"providerId", candidate.provider_id},
			    {"nativeSessionId", candidate.native_session_id}, {"title", candidate.title}, {"folderId", candidate.folder_id},
			    {"workspaceDirectory", candidate.workspace_directory}, {"executionHostId", candidate.execution_host_id},
			    {"updatedAt", candidate.updated_at}, {"workspaceTitle", candidate.workspace_title}, {"newWorkspace", candidate.new_workspace}});
		}
		return {{"phase", snapshot.phase}, {"generation", snapshot.generation}, {"candidates", candidates},
		    {"totalCount", snapshot.total_count}, {"nextOffset", snapshot.next_offset},
		    {"importedCount", snapshot.imported_count}, {"errors", snapshot.errors}};
	}

	nlohmann::json StateSerializer::Serialize(const AppState& app, bool summary_only)
	{
		CatalogSnapshotCache catalog_cache;
		nlohmann::json j;
		j["stateRevision"] = app.state_revision;
		j["appVersion"] = uam::constants::kAppVersion;
		j["runnerProtocolVersion"] = uam::remote::kRunnerProtocolVersion;

		j["folders"] = SerializeFoldersForFrontend(app.folders, app.data_root);
		j["resourceCollections"] = SerializeResourceCollectionsForFrontend(app.resource_collections, app.data_root);
		j["shellActions"] = SerializeShellActionsForFrontend(app.shell_actions);
		j["computePoolUsage"] = uam::compute_pools::UsageForFrontend(app);
		j["shellActionNotification"] = app.shell_action_notification;
		j["statusLine"] = app.status_line;
		if (app.deletion_progress)
		{
			j["deletionProgress"] = {{"completed", app.deletion_progress->completed},
			    {"total", app.deletion_progress->total}, {"currentLabel", app.deletion_progress->current_label}};
		}
		const StartupNativeImportSnapshot import_snapshot = app.startup_native_import_service
		    ? app.startup_native_import_service->Snapshot(0, 0) : StartupNativeImportSnapshot{};
		j["startupNativeImports"] = {{"phase", import_snapshot.phase}, {"generation", import_snapshot.generation},
		    {"totalCount", import_snapshot.total_count}, {"importedCount", import_snapshot.imported_count}};

		// Chat sessions
		const auto managed_counts = ManagedSubagentCounts(app);
		nlohmann::json chats_arr = JsonArrayWithCapacity(app.chats.size());
		const std::string selected_chat_id = SelectedVisibleChatId(app);
		SelectHistoryDigestCache(selected_chat_id);
		const bool has_selected_chat = !selected_chat_id.empty();
		for (const auto& chat : app.chats)
		{
			if (IsInternalChat(chat)) continue;
			const bool selected_chat = !has_selected_chat || selected_chat_id == chat.id;
			nlohmann::json chat_json;
			if (selected_chat && chat.messages_loaded && !summary_only)
			{
				chat_json = SerializeSession(chat, chat.id == selected_chat_id);
				chat_json["memoryEnabled"] = uam::memory_levels::IsEnabled(uam::memory_policy::Resolve(app.settings, chat));
				const std::unordered_map<std::string, std::string>::const_iterator routing_wait = app.execution_routing_waits.find(chat.id);
				chat_json["routingWaitReason"] = routing_wait == app.execution_routing_waits.end() ? "" : routing_wait->second;
			chat_json["routingUnresolvedReservation"] = app.execution_routing_restored_unresolved_chat_ids.contains(chat.id);
			if (!chat.imported_read_only || chat.subagent_parent_chat_id.empty() || chat.subagent_parent_created_at.empty())
				chat_json["workspaceDirectory"] = uam::paths::Utf8PathString(WorkspaceSnapshotForChat(app, chat, catalog_cache));
			chat_json["cliTerminal"] = SerializeChatTerminalSummary(app, chat);
			chat_json["acpSession"] = SerializeAcpSessionSummary(app, chat, catalog_cache);
			chat_json["computerUse"] = SerializeComputerUseState(app, chat);
			}
			else
			{
				chat_json = SerializeFingerprintSession(app, chat, catalog_cache, chat.id == selected_chat_id);
			}
			chat_json["attentionRevision"] = ChatDomainService().AttentionRevision(app, chat);
			chat_json["attentionSource"] = chat.attention_source;
			const auto managed_count = managed_counts.find(chat.id);
			chat_json["managedSubagentCount"] = managed_count == managed_counts.end() ? 0 : managed_count->second;
			uam::icons::AddFrontendAsset(chat_json, app.data_root, chat.custom_icon);
			chats_arr.push_back(std::move(chat_json));
		}
		j["chats"] = std::move(chats_arr);
		j["cliDebug"] = SerializeCliDebugState(app);
		j["memoryActivity"] = SerializeMemoryActivity(app);
		j["cliVersionManager"] = SerializeCliVersionManager(app);

		j["selectedChatId"] = uam::nlohmann_json::StringOrNull(selected_chat_id);

		j["providers"] = SerializeProvidersForFrontend(app.provider_profiles);
		j["providerModelCatalogs"] = SerializeProviderModelCatalogs(app, catalog_cache);

		// Settings slice that the UI cares about
		{
			j["settings"] = uam::settings_frontend_json::SerializeLiveSettingsFields(app.settings, app.memory_last_status);
			j["settings"]["computerUseRevocationPending"] = !app.computer_use_revocation_pending_chat_ids.empty();
			j["settings"]["branchSwitching"] = true;
			for (nlohmann::json& host : j["settings"]["executionHosts"])
			{
				const ExecutionHost* configured = uam::execution_hosts::Find(app.settings.execution_hosts, host.value("id", ""));
				if (configured != nullptr) uam::icons::AddFrontendAsset(host, app.data_root, configured->custom_icon);
				// The UI compares this with the host's installed runnerSha256 to offer rebuilt helpers.
				if (configured != nullptr && configured->transport == "ssh")
					host["bundledRunnerSha256"] = uam::remote::PackagedRunnerSha256(configured->platform, configured->architecture);
			}
		}

		return j;
	}

	nlohmann::json StateSerializer::SerializeChatSummary(const AppState& app, const ChatSession& chat)
	{
		CatalogSnapshotCache catalog_cache;
		return SerializeFingerprintSession(app, chat, catalog_cache);
	}

	std::vector<StateSerializer::ChatSummaryDigest> StateSerializer::SerializeChatFingerprintDigests(const AppState& app)
	{
		CatalogSnapshotCache catalog_cache;
		catalog_cache.compact_catalogs = true;
		std::vector<ChatSummaryDigest> digests;
		digests.reserve(app.chats.size());
		ScanFingerprintChats(app, [&](const ChatSession& chat, bool selected, std::size_t managed_count)
		{
			SummaryDigestWriter w;
			if (FindAcpSessionForChat(app, chat.id) == nullptr)
			{
				WriteFingerprintChat(w, app, chat, catalog_cache, selected, managed_count);
				digests.push_back({&chat, w.Digest(), nullptr});
				return;
			}
			// Live sessions change on most ticks and are built anyway when they do, so keep the JSON for the patch.
			nlohmann::json summary;
			SummaryJsonWriter json_writer(summary);
			WriteFingerprintChat(json_writer, app, chat, catalog_cache, selected, managed_count);
			// Mirrors cef_push's StripVolatileAcpWaitTelemetry: the wait counter alone never publishes a summary.
			if (const nlohmann::json::iterator acp = summary.find("acpSession"); acp != summary.end() && acp->is_object()) acp->erase("waitSeconds");
			w.Merge(summary);
			digests.push_back({&chat, w.Digest(), std::move(summary)});
		});
		return digests;
	}

	std::vector<nlohmann::json> StateSerializer::SerializeChatFingerprints(const AppState& app, const std::vector<const ChatSession*>& chats)
	{
		CatalogSnapshotCache catalog_cache;
		catalog_cache.compact_catalogs = true;
		const auto managed_counts = ManagedSubagentCounts(app);
		const std::string selected_chat_id = SelectedVisibleChatId(app);
		std::vector<nlohmann::json> summaries;
		summaries.reserve(chats.size());
		for (const ChatSession* chat : chats)
		{
			const auto managed_count = managed_counts.find(chat->id);
			SummaryJsonWriter w(summaries.emplace_back());
			WriteFingerprintChat(w, app, *chat, catalog_cache, chat->id == selected_chat_id,
			    managed_count == managed_counts.end() ? std::size_t{0} : managed_count->second);
		}
		return summaries;
	}

	nlohmann::json StateSerializer::SerializeFingerprint(const AppState& app, bool compact_catalogs, bool include_chats)
	{
		CatalogSnapshotCache catalog_cache;
		catalog_cache.compact_catalogs = compact_catalogs;
		nlohmann::json j;

		j["folders"] = SerializeFoldersForFrontend(app.folders, app.data_root);
		j["resourceCollections"] = SerializeResourceCollectionsForFrontend(app.resource_collections, app.data_root);
		j["shellActions"] = SerializeShellActionsForFrontend(app.shell_actions);
		j["computePoolUsage"] = uam::compute_pools::UsageForFrontend(app);
		j["shellActionNotification"] = app.shell_action_notification;
		j["statusLine"] = app.status_line;
		if (app.deletion_progress)
		{
			j["deletionProgress"] = {{"completed", app.deletion_progress->completed},
			    {"total", app.deletion_progress->total}, {"currentLabel", app.deletion_progress->current_label}};
		}
		const StartupNativeImportSnapshot import_snapshot = app.startup_native_import_service
		    ? app.startup_native_import_service->Snapshot(0, 0) : StartupNativeImportSnapshot{};
		j["startupNativeImports"] = {{"phase", import_snapshot.phase}, {"generation", import_snapshot.generation},
		    {"totalCount", import_snapshot.total_count}, {"importedCount", import_snapshot.imported_count}};

		const std::string selected_chat_id = SelectedVisibleChatId(app);
		if (include_chats)
		{
			nlohmann::json chats_arr = JsonArrayWithCapacity(app.chats.size());
			ScanFingerprintChats(app, [&](const ChatSession& chat, bool selected, std::size_t managed_count)
			{
				nlohmann::json& chat_json = chats_arr.emplace_back();
				SummaryJsonWriter w(chat_json);
				WriteFingerprintChat(w, app, chat, catalog_cache, selected, managed_count);
			});
			j["chats"] = std::move(chats_arr);
		}

		j["selectedChatId"] = uam::nlohmann_json::StringOrNull(selected_chat_id);

		j["providers"] = SerializeProvidersForFrontend(app.provider_profiles);
		j["providerModelCatalogs"] = SerializeProviderModelCatalogs(app, catalog_cache);
		j["memoryActivity"] = SerializeMemoryActivity(app);
		j["cliVersionManager"] = SerializeCliVersionManager(app);

		{
			j["settings"] = uam::settings_frontend_json::SerializeFingerprintSettingsFields(app.settings);
			j["settings"]["computerUseRevocationPending"] = !app.computer_use_revocation_pending_chat_ids.empty();
			j["settings"]["branchSwitching"] = true;
			for (nlohmann::json& host : j["settings"]["executionHosts"])
			{
				const ExecutionHost* configured = uam::execution_hosts::Find(app.settings.execution_hosts, host.value("id", ""));
				if (configured != nullptr) uam::icons::AddFrontendAsset(host, app.data_root, configured->custom_icon);
			}
		}

		return j;
	}

	nlohmann::json StateSerializer::SerializeSession(const ChatSession& session, bool reuse_selected_history)
	{
		nlohmann::json j;
		AddSessionSummaryFields(j, session, session.workspace_directory, reuse_selected_history);

		nlohmann::json msgs = JsonArrayWithCapacity(session.messages.size());
		for (const Message& message : session.messages)
		{
			msgs.push_back(SerializeMessageForFrontend(message));
		}
		j["messages"] = std::move(msgs);

		// Serialize goals
		if (!session.goals.empty())
		{
			nlohmann::json goals_arr = JsonArrayWithCapacity(session.goals.size());
			for (const auto& goal : session.goals)
			{
				nlohmann::json goal_json;
				goal_json["id"] = goal.id;
				goal_json["objective"] = goal.objective;
				goal_json["status"] = GoalStatusToString(goal.status);
				goal_json["tokenBudget"] = goal.token_budget;
				goal_json["tokensUsed"] = goal.tokens_used;
				goal_json["blockedTurnCount"] = goal.blocked_turn_count;
				goal_json["lastBlocker"] = goal.last_blocker.empty() ? nullptr : nlohmann::json(goal.last_blocker);
				goal_json["lastBlockerKind"] = goal.last_blocker_kind.empty() ? nullptr : nlohmann::json(goal.last_blocker_kind);
				goal_json["lastDiagnostic"] = goal.last_diagnostic.empty() ? nullptr : nlohmann::json(goal.last_diagnostic);
				goal_json["lastDecisionSource"] = goal.last_decision_source.empty() ? nullptr : nlohmann::json(goal.last_decision_source);
					goal_json["pendingContinuation"] = session.goal_pending_continuation_id == goal.id;
				goal_json["completedItems"] = goal.completed_items;
				goal_json["remainingItems"] = goal.remaining_items;
				goal_json["currentStep"] = goal.current_step;
				goal_json["lastVerification"] = goal.last_verification;
				goal_json["lastNextPrompt"] = goal.last_next_prompt;
				goal_json["sameNextPromptCount"] = goal.same_next_prompt_count;
				goal_json["loopCount"] = goal.loop_count;
				goal_json["createdAt"] = goal.created_at;
				goal_json["updatedAt"] = goal.updated_at;
				goal_json["executionOwner"] = goal.execution_owner == "provider" ? "provider" : "uam";
				goal_json["providerCommand"] = goal.execution_owner == "provider" ? goal.provider_command : "";
				goal_json["workerModelId"] = goal.worker_model_id;
				goal_json["reviewerModelId"] = goal.reviewer_model_id;
				goal_json["creator"] = goal.creator == "model" ? "model" : "user";
				goal_json["creatorProviderId"] = uam::excluded_provider_data::IsExcluded(goal.creator_provider_id) ? "" : goal.creator_provider_id;
				goal_json["creatorAgentId"] = goal.creator_agent_id;
				goal_json["creatorRunId"] = goal.creator_run_id;
				goals_arr.push_back(std::move(goal_json));
			}
			j["goals"] = std::move(goals_arr);
		}

		return j;
	}

	std::string StateSerializer::MessageDigest(const ChatSession& session, bool reuse_selected_history)
	{
		return MessageDigestForFingerprint(session, reuse_selected_history);
	}

	std::size_t StateSerializer::SelectedHistoryRehashedMessageCount()
	{
		return g_selected_history_digest.rehashed_message_count;
	}

	nlohmann::json StateSerializer::SerializeMessagePage(const ChatSession& session,
	                                                     std::size_t limit,
	                                                     std::optional<std::size_t> before,
	                                                     bool defer_tool_call_content,
	                                                     std::string_view known_digest, bool reuse_selected_history)
	{
		const std::size_t total_count = session.messages.size();
		const std::size_t end = std::min(before.value_or(total_count), total_count);
		const std::size_t start = end > limit ? end - limit : 0;
		const std::string digest = MessageDigestForFingerprint(session, reuse_selected_history);
		if (!before.has_value() && !known_digest.empty() && known_digest == digest)
		{
			return {{"startIndex", start}, {"totalCount", total_count},
			        {"messagesDigest", digest}, {"unchanged", true}};
		}
		nlohmann::json user_message_indices = nlohmann::json::array();
		for (std::size_t index = 0; index < total_count; ++index)
		{
			if (session.messages[index].role != MessageRole::User || session.messages[index].acp_prompt_discarded) continue;
			user_message_indices.push_back(index);
			// Keep the navigation detail within the existing bounded tail-cache budget.
			if (user_message_indices.size() > 32768) { user_message_indices = nullptr; break; }
		}
		nlohmann::json messages = JsonArrayWithCapacity(end - start);
		for (std::size_t index = start; index < end; ++index)
		{
			messages.push_back(SerializeMessageForFrontend(session.messages[index], defer_tool_call_content));
		}

		return {
		    {"messages", std::move(messages)},
		    {"userMessageIndices", std::move(user_message_indices)},
		    {"startIndex", start},
		    {"totalCount", total_count},
		    {"messagesDigest", digest},
		};
	}

	nlohmann::json StateSerializer::SerializeDetachedMessagePage(const ChatMessagePage& page,
	    bool defer_tool_call_content, std::string_view known_digest, bool allow_unchanged)
	{
		nlohmann::json result{{"chatId", page.chat.id}, {"startIndex", page.start_index},
		    {"totalCount", page.total_count}, {"messagesDigest", page.messages_digest},
		    {"unchanged", allow_unchanged && !known_digest.empty() && known_digest == page.messages_digest}};
		if (result["unchanged"].get<bool>()) return result;
		result["userMessageIndices"] = page.user_message_indices ? nlohmann::json(*page.user_message_indices) : nlohmann::json(nullptr);
		nlohmann::json messages = JsonArrayWithCapacity(page.chat.messages.size());
		for (const Message& message : page.chat.messages) messages.push_back(SerializeMessageForFrontend(message, defer_tool_call_content));
		result["messages"] = std::move(messages);
		return result;
	}

	std::string StateSerializer::ToolCallContentForFrontend(const ToolCall& tool_call)
	{
		std::string content;
		if (!tool_call.approval_status.empty()) content = "Approval: " + tool_call.approval_status + "\n\n";
		if (!tool_call.args_json.empty() && !tool_call.result_text.empty())
			content += "Arguments:\n" + tool_call.args_json + "\n\nResult:\n" + tool_call.result_text;
		else content += tool_call.result_text.empty() ? tool_call.args_json : tool_call.result_text;
		if (!tool_call.child_activity.empty()) content += "\n\nChild activity:\n" + tool_call.child_activity;
		return content;
	}

	std::string StateSerializer::ToolCallContentForFrontend(const AcpToolCallState& tool_call)
	{
		return ToolCallContentForFrontend(acp_detail::PersistedToolCallFromAcpToolCall(tool_call));
	}

	static nlohmann::json ToolContentPage(
	    const std::vector<std::string_view>& parts, std::size_t requested_offset)
	{
		std::size_t total_bytes = 0;
		for (std::string_view part : parts) total_bytes += part.size();
		const auto byte_at = [&](std::size_t index)
		{
			for (std::string_view part : parts)
			{
				if (index < part.size()) return static_cast<unsigned char>(part[index]);
				index -= part.size();
			}
			return static_cast<unsigned char>(0);
		};
		constexpr std::size_t kPageMaxBytes = 128 * 1024;
		const auto is_continuation = [&](std::size_t index)
		{
			return index < total_bytes &&
			       (byte_at(index) & 0xC0U) == 0x80U;
		};
		const auto boundary_at_or_after = [&](std::size_t offset)
		{
			offset = std::min(offset, total_bytes);
			while (is_continuation(offset)) ++offset;
			return offset;
		};
		const auto boundary_at_or_before = [&](std::size_t offset)
		{
			offset = std::min(offset, total_bytes);
			while (offset > 0 && is_continuation(offset)) --offset;
			return offset;
		};

		const std::size_t last_offset = total_bytes > kPageMaxBytes
		                                    ? boundary_at_or_after(total_bytes - kPageMaxBytes)
		                                    : 0;
		const std::size_t offset = requested_offset > total_bytes
		                               ? last_offset
		                               : boundary_at_or_before(requested_offset);
		std::size_t next_offset = std::min(total_bytes, offset + kPageMaxBytes);
		while (next_offset < total_bytes && is_continuation(next_offset)) --next_offset;
		const std::size_t previous_offset = offset > kPageMaxBytes
		                                        ? boundary_at_or_after(offset - kPageMaxBytes)
		                                        : 0;

		std::string content;
		content.reserve(next_offset - offset);
		std::size_t position = 0;
		for (std::string_view part : parts)
		{
			const std::size_t begin = std::max(offset, position);
			const std::size_t end = std::min(next_offset, position + part.size());
			if (begin < end) content.append(part.substr(begin - position, end - begin));
			position += part.size();
		}
		return {
		    {"content", std::move(content)},
		    {"offset", offset},
		    {"nextOffset", next_offset},
		    {"previousOffset", previous_offset},
		    {"lastOffset", last_offset},
		    {"totalBytes", total_bytes},
		    {"hasPrevious", offset > 0},
		    {"hasMore", next_offset < total_bytes},
		};
	}


	static nlohmann::json ToolFieldsPage(std::string_view arguments, std::string_view result, std::string_view approval,
	    std::string_view child, std::size_t offset)
	{
		std::vector<std::string_view> parts;
		if (!approval.empty()) parts.insert(parts.end(), {"Approval: ", approval, "\n\n"});
		if (!arguments.empty() && !result.empty()) parts.insert(parts.end(), {"Arguments:\n", arguments, "\n\nResult:\n", result});
		else parts.push_back(result.empty() ? arguments : result);
		if (!child.empty()) parts.insert(parts.end(), {"\n\nChild activity:\n", child});
		return ToolContentPage(parts, offset);
	}

	nlohmann::json StateSerializer::ToolCallContentPageForFrontend(std::string_view content, std::size_t offset)
	{
		return ToolContentPage({content}, offset);
	}

	nlohmann::json StateSerializer::ToolCallContentPageForFrontend(const ToolCall& tool, std::size_t offset)
	{
		return ToolFieldsPage(tool.args_json, tool.result_text, tool.approval_status, tool.child_activity, offset);
	}

	nlohmann::json StateSerializer::ToolCallContentPageForFrontend(const AcpToolCallState& tool, std::size_t offset)
	{
		return ToolFieldsPage(tool.args_json, tool.content, tool.approval_status, tool.child_activity, offset);
	}

	nlohmann::json StateSerializer::SerializeFolder(const ChatFolder& folder, bool probe_directory)
	{
		nlohmann::json j;
		j["id"] = folder.id;
		j["title"] = folder.title;
		j["directory"] = folder.directory;
		j["collapsed"] = folder.collapsed;
		j["customIcon"] = uam::icons::Serialize(folder.custom_icon);
		j["executionHostId"] = uam::strings::NonEmptyOrFallback(
		    uam::strings::Trim(folder.execution_host_id), "local");
		if (!probe_directory)
		{
			j["missing"] = folder.availability_directory == folder.directory &&
			    folder.availability_host_id == folder.execution_host_id && folder.directory_missing;
		}
		else
		{
		const std::filesystem::path directory = uam::paths::PathFromUtf8(folder.directory);
		const bool can_probe = folder.execution_host_id.empty() ||
		                       folder.execution_host_id == uam::execution_hosts::kLocalHostId
		    ? PlatformServicesFactory::Instance().path_service.CanProbeDirectoryWithoutPrompt(directory)
		    : false;
		j["missing"] = !folder.directory.empty() && can_probe && !uam::paths::IsDirectoryNoThrow(directory);
		}

		return j;
	}

	nlohmann::json StateSerializer::SerializeResourceReference(const ResourceReference& reference)
	{
		return {
		    {"id", reference.id},
		    {"type", reference.type},
		    {"target", reference.target},
		    {"label", reference.label},
		    {"customIcon", uam::icons::Serialize(reference.custom_icon)},
		};
	}

	nlohmann::json StateSerializer::SerializeResourceCollection(const ResourceCollection& collection)
	{
		nlohmann::json references = JsonArrayWithCapacity(collection.references.size());
		for (const ResourceReference& reference : collection.references)
		{
			references.push_back(SerializeResourceReference(reference));
		}
		nlohmann::json object = {
		    {"id", collection.id},
		    {"name", collection.name},
		    {"collapsed", collection.collapsed},
		    {"customIcon", uam::icons::Serialize(collection.custom_icon)},
		    {"references", std::move(references)},
		};
		if (!collection.parent_id.empty())
		{
			object["parentId"] = collection.parent_id;
		}
		return object;
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
		j["structuredPermissionControl"] = profile.supports_structured && !uam::provider_chat_defaults::ProviderManagesOwnPermissions(profile.id) ? "uam" : "provider";
		j["terminalPermissionControl"] = "provider";
		j["nativeGoalCommand"] = profile.native_goal_command;
		// Without native steering, Steer stops the running turn and sends the steer first.
		j["supportsNativeSteering"] = ProviderRuntimeRegistry::ResolveById(profile.id).SupportsAcpSteering();
		
		const std::string npm_package = GetNpmPackageNameForProvider(profile.id);
		if (!npm_package.empty())
		{
			j["npmPackageName"] = npm_package;
		}
		
		return j;
	}

} // namespace uam
