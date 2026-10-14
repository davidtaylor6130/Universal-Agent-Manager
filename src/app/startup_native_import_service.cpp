#include "app/startup_native_import_service.h"

#include "app/chat_domain_service.h"
#include "common/chat/native_chat_identity.h"
#include "common/config/execution_host_config.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_ids.h"
#include "common/utils/string_utils.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <chrono>
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
			return result;
		}
	}

	StartupNativeImportService::StartupNativeImportService(Scanner scanner) : m_scanner(std::move(scanner))
	{
		if (!m_scanner)
		{
			m_scanner = [](const StartupNativeImportScanRequest& request, std::stop_token stop)
			{
				const ChatHistorySyncService history;
				ChatHistorySyncService::LocalHistoryDiscovery discovery = history.DiscoverProviderChatsForFolder({},
				    request.open_code_profile ? &*request.open_code_profile : nullptr, stop, request.open_code_offset);
				for (const StartupNativeImportRemoteScope& scope : request.remote_scopes)
				{
					if (stop.stop_requested()) break;
					ChatHistorySyncService::LocalHistoryDiscovery remote = history.DiscoverRemoteProviderChatsForFolder(scope.folder, scope.host,
					    request.open_code_profile ? &*request.open_code_profile : nullptr, stop);
					discovery.result.Merge(remote.result);
					for (ChatSession& chat : remote.chats) discovery.chats.push_back(std::move(chat));
				}
				return discovery;
			};
		}
	}

	StartupNativeImportService::~StartupNativeImportService()
	{
		Cancel();
	}

	void StartupNativeImportService::Initialize(const AppState& app)
	{
		if (m_initialized) return;
		m_initialized = true;
		m_generation = time::SteadyEpochNanosecondsTokenNow();
		m_phase = "scanning";
		const ProviderProfile* configured = ProviderProfileStore::FindById(app.provider_profiles, provider_ids::kOpenCodeCli);
		StartupNativeImportScanRequest request;
		request.open_code_profile = configured != nullptr ? std::optional<ProviderProfile>(*configured) : std::nullopt;
		request.open_code_offset = app.open_code_history_scan_offset;
		for (const ChatFolder& folder : app.folders)
		{
			if (paths::IsControllerLocalWorkspace(folder)) continue;
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, folder.execution_host_id);
			if (host == nullptr || host->transport != "ssh" || host->runner_status != "ready" ||
			    !execution_hosts::IsAbsoluteRemotePath(host->platform, folder.directory))
			{
				AddError("Remote history was not scanned because its workspace host was unavailable: " + folder.title);
				continue;
			}
			m_remote_hosts[host->id] = *host;
			m_remote_folders[folder.id] = folder;
			request.remote_scopes.push_back({folder, *host});
		}
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
	}

	bool StartupNativeImportService::Poll(AppState& app)
	{
		if (m_phase == "scanning")
		{
			if (!m_scan.valid() || m_scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
			ChatHistorySyncService::LocalHistoryDiscovery discovery;
			try { discovery = m_scan.get(); }
			catch (const std::exception& error) { AddError(std::string("History discovery failed: ") + error.what()); }
			for (const std::string& error : discovery.result.errors) AddError(error);
			std::string filter_error;
			if (!ChatHistorySyncService().FilterStartupImportCandidates(app, discovery, &filter_error))
			{
				AddError(filter_error);
				discovery.CancelPending();
				m_phase = "complete";
				return true;
			}
			const auto add = [&](ChatSession chat, std::filesystem::path rollout)
			{
				const ChatFolder* owner = ChatDomainService().FindFolderById(app, chat.folder_id);
				if (owner == nullptr) return;
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
				const auto exported = discovery.open_code_exports.find(chat_identity::NativeIdentityKeyForHistoryImport(chat));
				if (exported != discovery.open_code_exports.end()) candidate.open_code_export = std::move(exported->second);
				candidate.chat = std::move(chat);
				candidate.rollout = std::move(rollout);
				candidate.owner = *owner;
				m_candidates.push_back(std::move(candidate));
			};
			for (ChatSession& chat : discovery.chats) add(std::move(chat), {});
			for (std::pair<ChatSession, std::filesystem::path>& record : discovery.codex_rollouts) add(std::move(record.first), std::move(record.second));
			m_phase = m_candidates.empty() ? "complete" : "ready";
			return true;
		}
		if (m_phase != "importing") return false;
		const ChatHistorySyncService::ImportResult result = ChatHistorySyncService().ImportDiscoveredProviderChatsBatch(
		    app, {}, m_import, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly);
		m_imported_count += result.imported_count;
		for (const std::string& error : result.errors) AddError(error);
		if (!m_import.Pending()) FinishImport(app);
		return result.imported_count > 0 || !result.errors.empty() || m_phase != "importing";
	}

	StartupNativeImportSnapshot StartupNativeImportService::Snapshot(std::size_t offset, std::size_t limit,
	    const std::string& folder_id) const
	{
		StartupNativeImportSnapshot snapshot;
		snapshot.phase = m_phase;
		snapshot.generation = m_generation;
		snapshot.errors = m_errors;
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
		if (!ChatHistorySyncService().FilterStartupImportCandidates(app, metadata, error_out)) return false;
		std::unordered_set<std::string> eligible;
		for (const ChatSession& chat : metadata.chats) eligible.insert(chat_identity::NativeIdentityKeyForHistoryImport(chat));
		std::erase_if(m_candidates, [&](const Candidate& candidate)
		{
			const ChatFolder* owner = ChatDomainService().FindFolderById(app, candidate.owner.id);
			const ExecutionHost* host = candidate.host ? execution_hosts::Find(app.settings.execution_hosts, candidate.host->id) : nullptr;
			return (candidate.host && (host == nullptr || host->runner_status != "ready" || !execution_hosts::SameConnection(*candidate.host, *host))) ||
			    owner == nullptr || owner->directory != candidate.owner.directory || owner->execution_host_id != candidate.owner.execution_host_id ||
			    !eligible.contains(chat_identity::NativeIdentityKeyForHistoryImport(candidate.chat));
		});
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
		}
		const std::unordered_set<std::string> declined(declined_ids.begin(), declined_ids.end());
		std::vector<ChatSession> declined_chats;
		for (const Candidate& candidate : m_candidates)
			if (declined.contains(candidate.summary.candidate_id)) declined_chats.push_back(OwnershipMetadata(candidate.chat));
		std::vector<std::string> added_keys;
		if (!ChatHistorySyncService().AddNativeImportTombstones(app.data_root, declined_chats, added_keys))
			return reject("Declined history could not be saved. No decisions were applied.");
		const std::unordered_set<std::string> selected(selected_candidates.begin(), selected_candidates.end());
		for (Candidate& candidate : m_candidates)
		{
			if (!selected.contains(candidate.summary.candidate_id)) continue;
			m_import.selected_folder_snapshots[candidate.owner.id] = candidate.owner;
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
