#include "app/startup_native_import_service.h"

#include "app/chat_domain_service.h"
#include "common/chat/native_chat_identity.h"
#include "common/chat/chat_folder_store.h"
#include "core/chat_import_utils.h"
#include "common/config/execution_host_config.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_ids.h"
#include "common/utils/string_utils.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace uam
{
	namespace
	{

		ChatSession OwnershipMetadata(const ChatSession& chat)
		{
			ChatSession result;
			result.id = chat.id;
			result.native_session_id = chat.native_session_id;
			result.provider_id = chat.provider_id;
			result.folder_id = chat.folder_id;
			result.execution_host_id = chat.execution_host_id;
			result.workspace_directory = chat.workspace_directory;
			result.workspace_worktree_directory = chat.workspace_worktree_directory;
			result.messages_loaded = false;
			return result;
		}
	}

	StartupNativeImportService::StartupNativeImportService(Scanner scanner, RemoteScanner remote_scanner) : m_scanner(std::move(scanner))
	{
		m_scoped_scanner = !m_scanner || static_cast<bool>(remote_scanner);
		if (!m_scoped_scanner) return;
		const Scanner local = m_scanner ? std::move(m_scanner) : Scanner([](const StartupNativeImportScanRequest& request, std::stop_token stop)
		{
			return ChatHistorySyncService().DiscoverProviderChatsForFolder({}, request.open_code_profile ? &*request.open_code_profile : nullptr,
			    stop, request.open_code_offset, request.open_code_only, request.open_code_remaining, request.open_code_list_fingerprint);
		});
		if (!remote_scanner) remote_scanner = [](const StartupNativeImportRemoteScope& scope, const ProviderProfile* profile, std::stop_token stop)
		{
			return ChatHistorySyncService().DiscoverRemoteProviderChatsForFolder(scope.folder, scope.host, profile, stop);
		};
		m_scanner = [local, remote = std::move(remote_scanner)](const StartupNativeImportScanRequest& request, std::stop_token stop)
		{
			ChatHistorySyncService::LocalHistoryDiscovery discovery = request.remote_only ? ChatHistorySyncService::LocalHistoryDiscovery{} : local(request, stop);
			for (const StartupNativeImportRemoteScope& scope : request.remote_scopes)
			{
				if (stop.stop_requested()) break;
				{
					std::lock_guard lock(scope.gate->mutex);
					if (scope.gate->pauses != 0 || scope.admission_generation != scope.gate->generation || scope.complete->load()) continue;
					++scope.gate->active;
				}
				struct Completion
				{
					const StartupNativeImportRemoteScope& scope;
					~Completion()
					{
						std::lock_guard lock(scope.gate->mutex);
						scope.complete->store(true);
						--scope.gate->active;
					}
				};
				const Completion completion{scope};
				ChatHistorySyncService::LocalHistoryDiscovery result = remote(scope, request.open_code_profile ? &*request.open_code_profile : nullptr, stop);
				discovery.result.Merge(result.result);
				for (ChatSession& chat : result.chats) discovery.chats.push_back(std::move(chat));
			}
			return discovery;
		};
	}

	StartupNativeImportService::~StartupNativeImportService()
	{
		Cancel();
	}

	void StartupNativeImportService::Initialize(const AppState& app)
	{
		if (m_initialized) return;
		StartScan(app, true);
	}

	bool StartupNativeImportService::DiscoverLocalHistory(const AppState& app, std::string* error_out)
	{
		if (m_phase == "scanning" || m_phase == "importing" || m_scan.valid() || m_stop.stop_requested())
		{
			if (error_out != nullptr) *error_out = "Wait for the current history operation to finish before discovering again.";
			return false;
		}
		// Only replace settled staging metadata; saved chats, declines and native histories remain authoritative.
		std::erase_if(m_candidates, [](const Candidate& candidate) { return paths::IsControllerLocalWorkspace(candidate.chat); });
		m_selected.clear();
		m_errors.clear();
		m_omitted_error_count = 0;
		m_imported_count = 0;
		m_import.CancelPending();
		m_import = {};
		StartScan(app, false);
		return true;
	}

	void StartupNativeImportService::StartScan(const AppState& app, bool include_remote)
	{
		m_initialized = true;
		m_include_unregistered_local = !include_remote;
		m_generation = time::SteadyEpochNanosecondsTokenNow();
		m_phase = "scanning";
		const ProviderProfile* configured = ProviderProfileStore::FindById(app.provider_profiles, provider_ids::kOpenCodeCli);
		StartupNativeImportScanRequest request;
		request.open_code_profile = configured != nullptr ? std::optional<ProviderProfile>(*configured) : std::nullopt;
		request.open_code_offset = app.open_code_history_scan_offset;
		for (const ChatFolder& folder : app.folders)
		{
			if (!include_remote || paths::IsControllerLocalWorkspace(folder)) continue;
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, folder.execution_host_id);
			if (host == nullptr || host->transport != "ssh" || host->runner_status != "ready" ||
			    !execution_hosts::IsAbsoluteRemotePath(host->platform, folder.directory))
			{
				AddError("Remote history was not scanned because its workspace host was unavailable: " + folder.title);
				continue;
			}
			m_remote_hosts[host->id] = *host;
			m_remote_folders[folder.id] = folder;
			const auto existing = std::ranges::find_if(request.remote_scopes, [&](const StartupNativeImportRemoteScope& scope) { return scope.host.id == host->id; });
			const std::shared_ptr<StartupNativeImportRemoteScanGate> gate = existing == request.remote_scopes.end()
			    ? std::make_shared<StartupNativeImportRemoteScanGate>() : existing->gate;
			request.remote_scopes.push_back({folder, *host, gate, std::make_shared<std::atomic<bool>>(false)});
		}
		if (include_remote) m_remote_scopes = request.remote_scopes;
		m_scan_request = request;
		try
		{
			m_scan = std::async(std::launch::async, [scanner = m_scanner, request = std::move(request), stop = m_stop.get_token()]()
			{
				return scanner(request, stop);
			});
		}
		catch (const std::exception& error)
		{
			AddError(std::string("History discovery could not start: ") + error.what());
			m_phase = "complete";
		}
	}

	void StartupNativeImportService::AddError(const std::string& error)
	{
		if (error.empty()) return;
		const std::string message = strings::SafeLine(error, 512, true);
		diagnostics::Write("Startup native history: " + message);
		if (m_errors.size() < 20) m_errors.push_back(message);
		else ++m_omitted_error_count;
	}

	bool StartupNativeImportService::BackgroundScanPendingOnHost(const std::string& host_id) const
	{
		if (!m_scoped_scanner)
			return m_phase == "scanning" && m_scan.valid() && m_scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready &&
			    std::ranges::any_of(m_scan_request.remote_scopes, [&](const StartupNativeImportRemoteScope& scope) { return scope.host.id == host_id; });
		const auto scope = std::ranges::find_if(m_remote_scopes, [&](const StartupNativeImportRemoteScope& value) { return value.host.id == host_id; });
		if (scope == m_remote_scopes.end()) return false;
		std::lock_guard lock(scope->gate->mutex);
		return scope->gate->active != 0;
	}

	std::shared_ptr<void> StartupNativeImportService::PauseBackgroundScanOnHost(const std::string& host_id)
	{
		const auto scope = std::ranges::find_if(m_remote_scopes, [&](const StartupNativeImportRemoteScope& value) { return value.host.id == host_id; });
		if (scope == m_remote_scopes.end()) return {};
		struct Pause
		{
			explicit Pause(std::shared_ptr<StartupNativeImportRemoteScanGate> value) : gate(std::move(value))
			{
				std::lock_guard lock(gate->mutex);
				++gate->pauses;
				++gate->generation;
			}
			~Pause()
			{
				std::lock_guard lock(gate->mutex);
				--gate->pauses;
			}
			std::shared_ptr<StartupNativeImportRemoteScanGate> gate;
		};
		return std::make_shared<Pause>(scope->gate);
	}

	std::vector<StartupNativeImportRemoteScope> StartupNativeImportService::PendingRemoteScopes(const AppState& app)
	{
		std::vector<StartupNativeImportRemoteScope> pending;
		for (StartupNativeImportRemoteScope& scope : m_remote_scopes)
		{
			if (scope.complete->load()) continue;
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, scope.host.id);
			const ChatFolder* folder = ChatDomainService().FindFolderById(app, scope.folder.id);
			if (host == nullptr || folder == nullptr || folder->execution_host_id != scope.folder.execution_host_id || folder->directory != scope.folder.directory ||
			    host->transport != scope.host.transport || host->platform != scope.host.platform ||
			    host->ssh_alias != scope.host.ssh_alias || host->runner_directory != scope.host.runner_directory)
			{
				scope.complete->store(true);
				AddError("Remote history was not scanned because its workspace host changed: " + scope.folder.title);
				continue;
			}
			std::lock_guard lock(scope.gate->mutex);
			if (scope.gate->pauses != 0 || host->runner_status == "installing") continue;
			if (host->runner_status != "ready")
			{
				scope.complete->store(true);
				AddError("Remote history was not scanned because its workspace host was unavailable: " + scope.folder.title);
				continue;
			}
			scope.host = *host;
			scope.admission_generation = scope.gate->generation;
			m_remote_hosts[host->id] = *host;
			pending.push_back(scope);
		}
		return pending;
	}

	bool StartupNativeImportService::StartPendingRemoteScans(const AppState& app)
	{
		StartupNativeImportScanRequest request;
		request.open_code_profile = m_scan_request.open_code_profile;
		request.remote_only = true;
		request.remote_scopes = PendingRemoteScopes(app);
		if (request.remote_scopes.empty()) return false;
		m_scan_request = request;
		try
		{
			m_scan = std::async(std::launch::async, [scanner = m_scanner, request = std::move(request), stop = m_stop.get_token()]()
			{
				return scanner(request, stop);
			});
		}
		catch (const std::exception& error)
		{
			for (const StartupNativeImportRemoteScope& scope : m_scan_request.remote_scopes) scope.complete->store(true);
			AddError(std::string("Remote history discovery could not start: ") + error.what());
		}
		return true;
	}

	bool StartupNativeImportService::SelectedImportPendingOnHost(const std::string& host_id) const
	{
		return m_phase == "importing" && std::ranges::any_of(m_selected,
		    [&](const Candidate& candidate) { return candidate.summary.execution_host_id == host_id; });
	}

	bool StartupNativeImportService::Poll(AppState& app)
	{
		if (m_phase == "scanning")
		{
			if (!m_scan.valid())
			{
				if (m_scoped_scanner && StartPendingRemoteScans(app)) return true;
				if (m_scoped_scanner && std::ranges::any_of(m_remote_scopes, [](const StartupNativeImportRemoteScope& scope) { return !scope.complete->load(); })) return false;
				m_phase = m_candidates.empty() ? "complete" : "ready";
				return true;
			}
			if (m_scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
			// Keep completed scan results until the ready-only ownership filter can validate them.
			if (std::ranges::any_of(m_scan_request.remote_scopes, [&](const StartupNativeImportRemoteScope& scope)
			{
				const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, scope.host.id);
				return host != nullptr && host->runner_status == "installing";
			})) return false;
			ChatHistorySyncService::LocalHistoryDiscovery discovery;
			try { discovery = m_scan.get(); }
			catch (const std::exception& error) { AddError(std::string("History discovery failed: ") + error.what()); }
			if (m_stop.stop_requested()) return false;
			if (discovery.open_code_next_offset) app.open_code_history_scan_offset = *discovery.open_code_next_offset;
			const bool changed_list = m_scan_request.open_code_remaining &&
			    (discovery.open_code_list_fingerprint != m_scan_request.open_code_list_fingerprint ||
			     discovery.open_code_session_count < *m_scan_request.open_code_remaining);
			if (changed_list)
			{
				AddError("OpenCode session list changed during discovery; retry the scan.");
				discovery.CancelPending();
				m_phase = m_scoped_scanner && std::ranges::any_of(m_remote_scopes, [](const StartupNativeImportRemoteScope& scope) { return !scope.complete->load(); })
				    ? "scanning" : m_candidates.empty() ? "complete" : "ready";
				return true;
			}
			const std::size_t remaining = m_scan_request.open_code_remaining.value_or(discovery.open_code_session_count);
			const bool continue_scan = discovery.open_code_scanned_count > 0 && discovery.open_code_scanned_count < remaining &&
			    discovery.open_code_next_offset && !discovery.open_code_list_fingerprint.empty();
			m_omitted_error_count += discovery.result.omitted_error_count;
			for (const std::string& error : discovery.result.errors)
			{
				if (continue_scan && error == "OpenCode history scan stopped before all sessions were read; retry to continue.") continue;
				AddError(error);
			}
			std::string filter_error;
			if (!ChatHistorySyncService().FilterStartupImportCandidates(app, discovery, &filter_error, m_include_unregistered_local))
			{
				AddError(filter_error);
				discovery.CancelPending();
				m_phase = "complete";
				return true;
			}
			// Identity keys canonicalize workspace paths (a file-system query each); derive each once per pass.
			std::set<std::pair<std::string, std::string>> candidate_keys;
			for (const Candidate& existing : m_candidates)
				candidate_keys.emplace(chat_identity::NativeIdentityKeyForHistoryImport(existing.chat), existing.chat.execution_host_id);
			const auto add = [&](ChatSession chat, std::filesystem::path rollout)
			{
				const ChatFolder* owner = ChatDomainService().FindFolderById(app, chat.folder_id);
				if (owner == nullptr && (!m_include_unregistered_local || !paths::IsControllerLocalWorkspace(chat))) return;
				const std::string key = chat_identity::NativeIdentityKeyForHistoryImport(chat);
				if (candidate_keys.contains({key, chat.execution_host_id})) return;
				Candidate candidate;
				if (!paths::IsControllerLocalWorkspace(chat))
				{
					const auto captured = m_remote_hosts.find(chat.execution_host_id);
					const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
					const auto captured_folder = m_remote_folders.find(owner->id);
					if (captured == m_remote_hosts.end() || captured_folder == m_remote_folders.end() || host == nullptr ||
					    !execution_hosts::SameConnection(captured->second, *host) ||
					    captured_folder->second.directory != owner->directory || captured_folder->second.execution_host_id != owner->execution_host_id) return;
					candidate.host = captured->second;
				}
				candidate.summary = {m_generation + ":" + std::to_string(m_candidates.size()), chat.provider_id,
				    chat.native_session_id, strings::SafeLine(chat.title, 512, true), chat.folder_id, chat.workspace_directory,
				    strings::NonEmptyOrFallback(chat.execution_host_id, "local"), chat.updated_at};
				const auto exported = discovery.open_code_exports.find(key);
				if (exported != discovery.open_code_exports.end()) candidate.open_code_export = std::move(exported->second);
				candidate.chat = std::move(chat);
				candidate.rollout = std::move(rollout);
				if (owner != nullptr) candidate.owner = *owner;
				candidate.summary.workspace_title = strings::SafeLine(candidate.owner.title, 512, true);
				candidate_keys.emplace(key, candidate.chat.execution_host_id);
				m_candidates.push_back(std::move(candidate));
			};
			for (ChatSession& chat : discovery.chats) add(std::move(chat), {});
			for (std::pair<ChatSession, std::filesystem::path>& record : discovery.codex_rollouts) add(std::move(record.first), std::move(record.second));
			if (discovery.open_code_scanned_count > remaining)
				AddError("OpenCode history scan exceeded its remaining session budget.");
			else if (continue_scan)
			{
				m_scan_request.open_code_only = true;
				m_scan_request.remote_scopes = m_scoped_scanner ? PendingRemoteScopes(app) : std::vector<StartupNativeImportRemoteScope>{};
				m_scan_request.open_code_offset = *discovery.open_code_next_offset;
				m_scan_request.open_code_remaining = remaining - discovery.open_code_scanned_count;
				m_scan_request.open_code_list_fingerprint = discovery.open_code_list_fingerprint;
				try
				{
					m_scan = std::async(std::launch::async, [scanner = m_scanner, request = m_scan_request, stop = m_stop.get_token()]
					{
						return scanner(request, stop);
					});
					return true;
				}
				catch (const std::exception&)
				{
					AddError("OpenCode history continuation could not start; retry the scan.");
				}
			}
			if (remaining > 0 && discovery.open_code_scanned_count == 0 && !discovery.open_code_list_fingerprint.empty())
				AddError("OpenCode history scan made no progress; retry the scan.");
			if (m_scoped_scanner && std::ranges::any_of(m_remote_scopes, [](const StartupNativeImportRemoteScope& scope) { return !scope.complete->load(); }))
			{
				(void)StartPendingRemoteScans(app);
				return true;
			}
			m_phase = m_candidates.empty() ? "complete" : "ready";
			return true;
		}
		if (m_phase != "importing") return false;
		const ChatHistorySyncService::ImportResult result = ChatHistorySyncService().ImportDiscoveredProviderChatsBatch(
		    app, {}, m_import, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly);
		m_imported_count += result.imported_count;
		if (!result.confirmed_empty_native_keys.empty()) std::erase_if(m_selected, [&](const Candidate& candidate)
		{
			return std::ranges::find(result.confirmed_empty_native_keys, chat_identity::NativeIdentityKeyForHistoryImport(candidate.chat)) != result.confirmed_empty_native_keys.end();
		});
		m_omitted_error_count += result.omitted_error_count;
		for (const std::string& error : result.errors) AddError(error);
		if (!m_import.Pending()) FinishImport(app);
		return result.imported_count > 0 || !result.errors.empty() || result.omitted_error_count > 0 || m_phase != "importing";
	}

	StartupNativeImportSnapshot StartupNativeImportService::Snapshot(std::size_t offset, std::size_t limit,
	    const std::string& folder_id) const
	{
		StartupNativeImportSnapshot snapshot;
		snapshot.phase = m_phase;
		snapshot.generation = m_generation;
		snapshot.errors = m_errors;
		if (m_omitted_error_count) snapshot.errors.push_back(std::to_string(m_omitted_error_count) + " additional native history issues were omitted; review discovery before importing.");
		snapshot.imported_count = m_imported_count;
		std::vector<const Candidate*> candidates;
		for (const Candidate& candidate : m_candidates)
		{
			if (folder_id.empty() || candidate.owner.id == folder_id) candidates.push_back(&candidate);
		}
		snapshot.total_count = candidates.size();
		offset = std::min(offset, candidates.size());
		const std::size_t count = std::min(std::clamp<std::size_t>(limit, 1, 200), candidates.size() - offset);
		for (std::size_t index = offset; index < offset + count; ++index) snapshot.candidates.push_back(candidates[index]->summary);
		snapshot.next_offset = offset + count;
		return snapshot;
	}

	bool StartupNativeImportService::Revalidate(AppState& app, std::string* error_out)
	{
		ChatHistorySyncService::LocalHistoryDiscovery metadata;
		for (const Candidate& candidate : m_candidates) metadata.chats.push_back(OwnershipMetadata(candidate.chat));
		if (!ChatHistorySyncService().FilterStartupImportCandidates(app, metadata, error_out, m_include_unregistered_local)) return false;
		AppState plan;
		plan.folders = app.folders;
		plan.settings.execution_hosts = app.settings.execution_hosts;
		for (const Candidate& candidate : m_candidates)
			if (candidate.new_workspace && ChatDomainService().FindFolderById(plan, candidate.owner.id) == nullptr) plan.folders.push_back(candidate.owner);
		std::unordered_set<std::string> eligible;
		for (const ChatSession& chat : metadata.chats) eligible.insert(chat_identity::NativeIdentityKeyForHistoryImport(chat));
		std::erase_if(m_candidates, [&](const Candidate& candidate)
		{
			const ChatFolder* owner = ChatDomainService().FindFolderById(app, candidate.owner.id);
			const ExecutionHost* host = candidate.host ? execution_hosts::Find(app.settings.execution_hosts, candidate.host->id) : nullptr;
			const bool installing = host != nullptr && host->runner_status == "installing";
			const bool staged_or_unassigned = m_include_unregistered_local && paths::IsControllerLocalWorkspace(candidate.chat) &&
			    (candidate.new_workspace || candidate.owner.id.empty());
			const ChatFolder* resolved = ChatHistorySyncService().StartupImportWorkspaceOwner(plan, candidate.chat);
			const bool owner_changed = owner == nullptr ? !staged_or_unassigned
			    : owner->directory != candidate.owner.directory || owner->execution_host_id != candidate.owner.execution_host_id;
			const bool staged_conflict = candidate.new_workspace && resolved != nullptr && resolved->id != candidate.owner.id;
			return (candidate.host && (host == nullptr || (host->runner_status != "ready" && !installing) || !execution_hosts::SameConnection(*candidate.host, *host))) ||
			    owner_changed || staged_conflict ||
			    (!installing && !eligible.contains(chat_identity::NativeIdentityKeyForHistoryImport(candidate.chat)));
		});
		for (Candidate& candidate : m_candidates)
		{
			if (candidate.new_workspace && ChatDomainService().FindFolderById(app, candidate.owner.id) != nullptr)
			{
				candidate.new_workspace = false;
				candidate.summary.new_workspace = false;
			}
		}
		return true;
	}

	bool StartupNativeImportService::AssignWorkspace(AppState& app, const std::string& generation, const std::string& candidate_id,
	    const std::string& title, std::string* error_out)
	{
		const auto reject = [&](const std::string& error)
		{
			if (error_out != nullptr) *error_out = error;
			return false;
		};
		if (!m_include_unregistered_local || m_phase != "ready" || generation != m_generation)
			return reject("History candidates changed. Review the current list.");
		const std::string name = strings::Trim(title);
		if (name.empty() || name.size() > 256 || std::ranges::any_of(name, [](unsigned char character) { return character < 0x20 || character == 0x7f; }))
			return reject("Enter a workspace name of at most 256 bytes on one line.");
		if (!Revalidate(app, error_out)) return false;
		const auto found = std::ranges::find_if(m_candidates, [&](const Candidate& candidate) { return candidate.summary.candidate_id == candidate_id; });
		if (found == m_candidates.end() || !paths::IsControllerLocalWorkspace(found->chat) || !paths::PathFromUtf8(found->chat.workspace_directory).is_absolute())
			return reject("The history is no longer available for local workspace assignment.");
		AppState plan;
		plan.folders = app.folders;
		plan.settings.execution_hosts = app.settings.execution_hosts;
		for (const Candidate& candidate : m_candidates)
			if (candidate.new_workspace && ChatDomainService().FindFolderById(plan, candidate.owner.id) == nullptr) plan.folders.push_back(candidate.owner);
		ChatFolder assignment;
		if (const ChatFolder* current = ChatHistorySyncService().StartupImportWorkspaceOwner(plan, found->chat)) assignment = *current;
		else
		{
			std::vector<ChatFolder> reserved = app.folders;
			for (const Candidate& candidate : m_candidates)
			{
				if (!candidate.new_workspace) continue;
				reserved.push_back(candidate.owner);
				if (candidate.chat.workspace_directory == found->chat.workspace_directory) assignment = candidate.owner;
			}
			if (assignment.id.empty()) assignment.id = ChatFolderStore::AllocateId(reserved);
			if (assignment.id.empty()) return reject("A workspace identity could not be allocated. Try again.");
			assignment.title = name;
			assignment.directory = found->chat.workspace_directory;
			assignment.execution_host_id = "local";
		}
		if (ChatDomainService().FindFolderById(app, assignment.id) == nullptr && assignment.directory == found->chat.workspace_directory) assignment.title = name;
		if (ChatDomainService().FindFolderById(plan, assignment.id) == nullptr) plan.folders.push_back(assignment);
		for (const Candidate& candidate : m_candidates)
		{
			if (candidate.owner.id.empty()) continue;
			const ChatFolder* resolved = ChatHistorySyncService().StartupImportWorkspaceOwner(plan, candidate.chat);
			if (resolved == nullptr || resolved->id != candidate.owner.id)
				return reject("This workspace conflicts with another reviewed assignment. Import that selection first, then discover again.");
		}
		// Group only the exact source path; no caller-supplied path can relocate native history.
		const std::string source = found->chat.workspace_directory;
		for (Candidate& candidate : m_candidates)
		{
			if (!paths::IsControllerLocalWorkspace(candidate.chat) || (candidate.chat.workspace_directory != source && candidate.owner.id != assignment.id)) continue;
			candidate.owner = assignment;
			candidate.new_workspace = ChatDomainService().FindFolderById(app, assignment.id) == nullptr;
			candidate.chat.folder_id = assignment.id;
			candidate.summary.folder_id = assignment.id;
			candidate.summary.workspace_title = strings::SafeLine(assignment.title, 512, true);
			candidate.summary.new_workspace = candidate.new_workspace;
		}
		return true;
	}

	bool StartupNativeImportService::Decide(AppState& app, const std::string& generation, const std::vector<std::string>& selected_ids,
	    const std::vector<std::string>& declined_ids, std::string* error_out, const std::string& folder_id, bool import_all)
	{
		const auto reject = [&](const std::string& error)
		{
			if (error_out != nullptr) *error_out = error;
			return false;
		};
		if (m_phase != "ready" || generation != m_generation) return reject("History candidates changed. Review the current list.");
		if (import_all && (!selected_ids.empty() || !declined_ids.empty())) return reject("Import all cannot be combined with selected decisions.");
		std::vector<std::string> selected_candidates = selected_ids;
		if (import_all)
		{
			for (const Candidate& candidate : m_candidates)
				if (folder_id.empty() || candidate.owner.id == folder_id) selected_candidates.push_back(candidate.summary.candidate_id);
		}
		std::unordered_set<std::string> requested;
		for (const std::vector<std::string>* group : std::initializer_list<const std::vector<std::string>*>{&selected_candidates, &declined_ids})
			for (const std::string& id : *group)
				if (id.empty() || !requested.insert(id).second) return reject("Each history candidate must have one decision.");
		if (requested.empty()) return reject("Select chats to import or decline.");
		for (const Candidate& candidate : m_candidates)
		{
			if (!requested.contains(candidate.summary.candidate_id) || !candidate.host) continue;
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, candidate.host->id);
			if (host != nullptr && host->runner_status == "installing")
				return reject("Wait for this host's helper installation to finish before changing its history selections.");
		}
		std::string filter_error;
		if (!Revalidate(app, &filter_error)) return reject(filter_error);
		std::unordered_map<std::string, const Candidate*> candidates_by_id;
		for (const Candidate& candidate : m_candidates) candidates_by_id.emplace(candidate.summary.candidate_id, &candidate);
		for (const std::string& id : requested)
		{
			const std::unordered_map<std::string, const Candidate*>::const_iterator candidate = candidates_by_id.find(id);
			if (candidate == candidates_by_id.end())
				return reject("A history candidate is no longer available or its workspace changed. Review the current list.");
			if (!folder_id.empty() && candidate->second->owner.id != folder_id)
				return reject("A history candidate belongs to another workspace. Review the current list.");
			if (std::ranges::find(selected_candidates, id) != selected_candidates.end() && candidate->second->owner.id.empty())
				return reject("Assign a workspace to every selected history before importing.");
		}
		const std::unordered_set<std::string> declined(declined_ids.begin(), declined_ids.end());
		std::vector<ChatSession> declined_chats;
		for (const Candidate& candidate : m_candidates)
			if (declined.contains(candidate.summary.candidate_id)) declined_chats.push_back(OwnershipMetadata(candidate.chat));
		std::vector<std::string> added_keys;
		if (!ChatHistorySyncService().AddNativeImportTombstones(app.data_root, declined_chats, added_keys))
			return reject("Declined history could not be saved. No decisions were applied.");
		const std::unordered_set<std::string> selected(selected_candidates.begin(), selected_candidates.end());
		m_import.preparation_stop = std::stop_source{};
		for (Candidate& candidate : m_candidates)
		{
			if (!selected.contains(candidate.summary.candidate_id)) continue;
			m_import.selected_folder_snapshots[candidate.owner.id] = candidate.owner;
			if (candidate.new_workspace && std::ranges::none_of(m_import.selected_new_folders, [&](const ChatFolder& folder) { return folder.id == candidate.owner.id; }))
				m_import.selected_new_folders.push_back(candidate.owner);
			if (candidate.host) m_import.expected_hosts[candidate.host->id] = *candidate.host;
			if (candidate.rollout.empty())
			{
				m_import.chats.push_back(candidate.chat);
				if (candidate.open_code_export) m_import.open_code_exports.emplace(chat_identity::NativeIdentityKeyForHistoryImport(candidate.chat), candidate.open_code_export);
			}
			else m_import.codex_rollouts.emplace_back(candidate.chat, candidate.rollout);
			const std::string candidate_id = candidate.summary.candidate_id;
			m_selected.push_back(std::move(candidate));
			candidate.summary.candidate_id = candidate_id;
		}
		std::erase_if(m_candidates, [&](const Candidate& candidate)
		{
			return requested.contains(candidate.summary.candidate_id);
		});
		m_phase = !m_selected.empty() ? "importing" : m_candidates.empty() ? "complete" : "ready";
		return true;
	}

	void StartupNativeImportService::FinishImport(AppState& app)
	{
		// Assignment plans belong to this selection, including failed attempts.
		m_import.selected_new_folders.clear();
		m_import.selected_folder_snapshots.clear();
		m_import.expected_hosts.clear();
		for (Candidate& candidate : m_selected) m_candidates.push_back(std::move(candidate));
		m_selected.clear();
		std::string error;
		if (!Revalidate(app, &error)) AddError(error);
		m_phase = m_candidates.empty() ? "complete" : "ready";
	}

	void StartupNativeImportService::Cancel()
	{
		m_stop.request_stop();
		if (m_scan.valid())
		{
			std::thread([scan = std::move(m_scan)]() mutable { scan.wait(); }).detach();
		}
		m_import.CancelPending();
		m_candidates.clear();
		m_selected.clear();
		if (m_initialized) m_phase = "cancelled";
	}
}
