#include "common/platform/background_thread_priority.h"
#include "app/startup_native_import_service.h"

#include "app/chat_domain_service.h"
#include "common/chat/chat_repository.h"
#include "common/chat/native_chat_identity.h"
#include "common/chat/native_import_filter.h"
#include "common/chat/chat_folder_store.h"
#include "core/chat_import_utils.h"
#include "common/config/execution_host_config.h"
#include "common/config/provider_availability.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_ids.h"
#include "common/utils/string_utils.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
			result.title = chat.title; // Import filters recheck title rules before publication.
			result.folder_id = chat.folder_id;
			result.execution_host_id = chat.execution_host_id;
			result.workspace_directory = chat.workspace_directory;
			result.workspace_worktree_directory = chat.workspace_worktree_directory;
			result.messages_loaded = false;
			return result;
		}

		/// <summary>Scans one host's folders in order on one shared client. A connect failure gets one bounded reconnect;
		/// if that fails too, the host's remaining folders stay pending for a later automatic retry and are named once.</summary>
		ChatHistorySyncService::LocalHistoryDiscovery ScanRemoteHost(const std::vector<const StartupNativeImportRemoteScope*>& scopes,
		    const StartupNativeImportService::RemoteScanner& remote, const ProviderProfile* profile, const StartupNativeImportRemoteRetry& retry, std::stop_token stop)
		try
		{
			ChatHistorySyncService::LocalHistoryDiscovery discovery;
			const ExecutionHost& host = scopes.front()->host;
			const std::shared_ptr<StartupNativeImportRemoteScanGate>& gate = scopes.front()->gate;
			ChatHistorySyncService::RemoteHistoryScanContext host_scan(host);
			bool unreachable = false;
			bool reconnected = false;
			bool reached = false;
			std::string reason;
			std::vector<std::string> waiting;
			std::set<std::string> reported;
			for (const StartupNativeImportRemoteScope* entry : scopes)
			{
				const StartupNativeImportRemoteScope& scope = *entry;
				if (stop.stop_requested()) break;
				std::stop_token cancelled;
				{
					std::lock_guard lock(scope.gate->mutex);
					if (scope.gate->pauses != 0 || scope.admission_generation != scope.gate->generation || scope.complete->load()) continue;
					if (unreachable)
					{
						waiting.push_back(scope.folder.title);
						continue;
					}
					++scope.gate->active;
					cancelled = scope.gate->cancel.get_token();
				}
				struct Completion
				{
					const StartupNativeImportRemoteScope& scope;
					bool pending = false;
					~Completion()
					{
						std::lock_guard lock(scope.gate->mutex);
						// A host pause or an unreachable host leaves the scope pending so a later admission rescans it from the start.
						if (!pending) scope.complete->store(true);
						--scope.gate->active;
					}
				};
				Completion completion{scope};
				std::stop_source scope_stop;
				const std::stop_callback forward_stop(stop, [&scope_stop]() { scope_stop.request_stop(); });
				const std::stop_callback forward_pause(cancelled, [&scope_stop]() { scope_stop.request_stop(); });
				ChatHistorySyncService::LocalHistoryDiscovery result = remote(scope, profile, host_scan, scope_stop.get_token());
				if (result.remote_host_unreachable && !reconnected && !scope_stop.stop_requested())
				{
					// One bounded reconnect for a host that is just waking or still joining its VPN.
					reconnected = true;
					std::mutex delay_mutex;
					std::condition_variable_any delay;
					std::unique_lock delay_lock(delay_mutex);
					(void)delay.wait_for(delay_lock, scope_stop.get_token(), retry.reconnect_delay, [] { return false; });
					if (!scope_stop.stop_requested())
					{
						result.CancelPending();
						host_scan.connect_failed = false;
						host_scan.connect_error.clear();
						result = remote(scope, profile, host_scan, scope_stop.get_token());
					}
				}
				if (cancelled.stop_requested())
				{
					completion.pending = true;
					diagnostics::Write("Startup native history: remote scan on host " + strings::SafeLine(scope.host.id, 128, true) + " cancelled for a helper install; it will rescan after the install.");
					result.CancelPending();
					continue;
				}
				if (result.remote_host_unreachable)
				{
					unreachable = completion.pending = true;
					reason = !host_scan.connect_error.empty() ? host_scan.connect_error
					    : !result.result.errors.empty() ? result.result.errors.back() : std::string("the connection failed");
					waiting.push_back(scope.folder.title);
					result.CancelPending();
					continue;
				}
				reached = true;
				// One host reports a repeated refusal (for example an unverified OpenCode version) once, not once per folder.
				ChatHistorySyncService::ImportResult merged = std::move(result.result);
				std::erase_if(merged.errors, [&reported](const std::string& error) { return !reported.insert(error).second; });
				discovery.result.Merge(merged);
				for (ChatSession& chat : result.chats) discovery.chats.push_back(std::move(chat));
			}
			bool report = false;
			{
				std::lock_guard lock(gate->mutex);
				if (unreachable)
				{
					gate->retry_delay = gate->retry_delay.count() == 0 ? retry.first_retry : std::min(gate->retry_delay * 2, retry.max_retry);
					gate->retry_at = std::chrono::steady_clock::now() + gate->retry_delay;
					report = !std::exchange(gate->unreachable_reported, true);
				}
				else if (reached)
				{
					gate->retry_delay = std::chrono::milliseconds{0};
					gate->unreachable_reported = false;
				}
			}
			if (report)
			{
				std::string titles;
				for (const std::string& title : waiting) titles += (titles.empty() ? "" : ", ") + title;
				discovery.result.Fail("Remote history on " + host.id + " could not be reached (" + strings::SafeLine(reason, 240, true) +
				    "). UAM will retry automatically for these workspaces: " + titles + ".");
			}
			return discovery;
		}
		catch (const std::exception& error)
		{
			// A jthread body must not throw; the failure stays on this host's report.
			ChatHistorySyncService::LocalHistoryDiscovery failed;
			failed.result.Fail(std::string("Remote history discovery failed: ") + error.what());
			return failed;
		}
	}

	StartupNativeImportService::StartupNativeImportService(Scanner scanner, RemoteScanner remote_scanner, StartupNativeImportRemoteRetry retry)
	    : m_scanner(std::move(scanner))
	{
		m_scoped_scanner = !m_scanner || static_cast<bool>(remote_scanner);
		if (!m_scoped_scanner) return;
		const Scanner local = m_scanner ? std::move(m_scanner) : Scanner([](const StartupNativeImportScanRequest& request, std::stop_token stop)
		{
			return ChatHistorySyncService().DiscoverProviderChatsForFolder({}, request.open_code_profile && request.open_code_local_enabled ? &*request.open_code_profile : nullptr,
			    stop, request.open_code_offset, request.open_code_only, request.open_code_remaining, request.open_code_list_fingerprint);
		});
		if (!remote_scanner) remote_scanner = [](const StartupNativeImportRemoteScope& scope, const ProviderProfile* profile, ChatHistorySyncService::RemoteHistoryScanContext& host_scan, std::stop_token stop)
		{
			return ChatHistorySyncService().DiscoverRemoteProviderChatsForFolder(scope.folder, scope.host, profile, stop, &host_scan);
		};
		m_scanner = [local, remote = std::move(remote_scanner), retry](const StartupNativeImportScanRequest& request, std::stop_token stop)
		{
			ChatHistorySyncService::LocalHistoryDiscovery discovery = request.remote_only ? ChatHistorySyncService::LocalHistoryDiscovery{} : local(request, stop);
			std::vector<std::vector<const StartupNativeImportRemoteScope*>> hosts;
			for (const StartupNativeImportRemoteScope& scope : request.remote_scopes)
			{
				const auto host = std::ranges::find_if(hosts, [&](const auto& scopes) { return scopes.front()->host.id == scope.host.id; });
				if (host != hosts.end()) host->push_back(&scope);
				else hosts.push_back({&scope});
			}
			const ProviderProfile* profile = request.open_code_profile ? &*request.open_code_profile : nullptr;
			std::vector<ChatHistorySyncService::LocalHistoryDiscovery> results(hosts.size());
			std::atomic<std::size_t> next{0};
			const auto drain = [&]()
			{
				for (std::size_t index = next++; index < hosts.size(); index = next++) results[index] = ScanRemoteHost(hosts[index], remote, hosts[index].front()->open_code_enabled ? profile : nullptr, retry, stop);
			};
			{
				std::vector<std::jthread> workers;
				// ponytail: fixed at four hosts at once; a slot count from settings waits until someone runs more dead hosts than that.
				for (std::size_t worker = 1; worker < std::min<std::size_t>(hosts.size(), 4); ++worker)
				{
					try { workers.emplace_back([&drain]() { uam::platform::LowerCurrentThreadToBackgroundPriority(); drain(); }); }
					catch (const std::system_error&) { break; } // This thread still drains every host.
				}
				drain();
			}
			for (ChatHistorySyncService::LocalHistoryDiscovery& result : results)
			{
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
		std::erase_if(m_children, [](const Child& child) { return paths::IsControllerLocalWorkspace(child.chat); });
		// Remote sessions startup left out join this review; the local scan below finds local ones without the startup rules.
		for (Candidate& candidate : m_startup_deferred) m_candidates.push_back(std::move(candidate));
		m_startup_deferred.clear();
		HoldBackIneligibleRemoteCandidates(app);
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
		if (include_remote) m_unavailable_hosts_reported.clear();
		m_include_unregistered_local = !include_remote;
		m_backfill_due = !include_remote;
		m_generation = time::SteadyEpochNanosecondsTokenNow();
		m_phase = "scanning";
		const ProviderProfile* configured = ProviderProfileStore::FindById(app.provider_profiles, provider_ids::kOpenCodeCli);
		StartupNativeImportScanRequest request;
		request.open_code_profile = configured != nullptr ? std::optional<ProviderProfile>(*configured) : std::nullopt;
		request.open_code_local_enabled = provider_availability::IsEnabled(app.settings, provider_ids::kOpenCodeCli);
		request.open_code_offset = app.open_code_history_scan_offset;
		for (const ChatFolder& folder : app.folders)
		{
			if (!include_remote || paths::IsControllerLocalWorkspace(folder)) continue;
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, folder.execution_host_id);
			if (host == nullptr || host->transport != "ssh" || host->runner_status != "ready")
			{
				AddHostUnavailableError(folder.execution_host_id, folder.title);
				continue;
			}
			if (!execution_hosts::IsAbsoluteRemotePath(host->platform, folder.directory))
			{
				AddError("Remote history was not scanned because its workspace path is not valid on its host: " + folder.title);
				continue;
			}
			m_remote_hosts[host->id] = *host;
			m_remote_folders[folder.id] = folder;
			const auto existing = std::ranges::find_if(request.remote_scopes, [&](const StartupNativeImportRemoteScope& scope) { return scope.host.id == host->id; });
			const std::shared_ptr<StartupNativeImportRemoteScanGate> gate = existing == request.remote_scopes.end()
			    ? std::make_shared<StartupNativeImportRemoteScanGate>() : existing->gate;
			request.remote_scopes.push_back({folder, *host, gate, std::make_shared<std::atomic<bool>>(false), 0, provider_availability::IsEnabled(app.settings, provider_ids::kOpenCodeCli, host->id)});
		}
		if (include_remote) m_remote_scopes = request.remote_scopes;
		m_scan_request = request;
		try
		{
			m_scan = std::async(std::launch::async, [scanner = m_scanner, request = std::move(request), stop = m_stop.get_token()]()
			{
				uam::platform::LowerCurrentThreadToBackgroundPriority();
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

	void StartupNativeImportService::AddHostUnavailableError(const std::string& host_id, const std::string& fallback_name)
	{
		if (m_unavailable_hosts_reported.insert(host_id).second)
			AddError("Remote history was not scanned because its workspace host was unavailable: " + strings::NonEmptyOrFallback(host_id, fallback_name));
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
				std::stop_source running;
				{
					std::lock_guard lock(gate->mutex);
					++gate->pauses;
					++gate->generation;
					running = std::exchange(gate->cancel, std::stop_source());
				}
				// Outside the gate lock: stop callbacks may terminate the in-flight remote export.
				running.request_stop();
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
			if (scope.gate->pauses != 0 || host->runner_status == "installing" || scope.gate->retry_at > std::chrono::steady_clock::now()) continue;
			if (host->runner_status != "ready")
			{
				scope.complete->store(true);
				AddHostUnavailableError(scope.host.id, scope.folder.title);
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
		request.open_code_local_enabled = m_scan_request.open_code_local_enabled;
		request.remote_only = true;
		request.remote_scopes = PendingRemoteScopes(app);
		if (request.remote_scopes.empty()) return false;
		m_scan_request = request;
		try
		{
			m_scan = std::async(std::launch::async, [scanner = m_scanner, request = std::move(request), stop = m_stop.get_token()]()
			{
				uam::platform::LowerCurrentThreadToBackgroundPriority();
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

	bool StartupNativeImportService::RemoteScansOutstanding() const
	{
		const auto now = std::chrono::steady_clock::now();
		return std::ranges::any_of(m_remote_scopes, [now](const StartupNativeImportRemoteScope& scope)
		{
			if (scope.complete->load()) return false;
			std::lock_guard lock(scope.gate->mutex);
			return scope.gate->retry_at <= now;
		});
	}

	bool StartupNativeImportService::RemoteRetryDue() const
	{
		// Only scopes that waited for an unreachable host; a fresh scan's pending scopes are already running.
		const auto now = std::chrono::steady_clock::now();
		return std::ranges::any_of(m_remote_scopes, [now](const StartupNativeImportRemoteScope& scope)
		{
			if (scope.complete->load()) return false;
			std::lock_guard lock(scope.gate->mutex);
			return scope.gate->retry_delay.count() != 0 && scope.gate->retry_at <= now;
		});
	}

	bool StartupNativeImportService::SelectedImportPendingOnHost(const std::string& host_id) const
	{
		return m_phase == "importing" && std::ranges::any_of(m_selected,
		    [&](const Candidate& candidate) { return candidate.summary.execution_host_id == host_id; });
	}

	bool StartupNativeImportService::Poll(AppState& app)
	{
		const bool settled = m_phase == "ready" || m_phase == "complete";
		// An unreachable host's workspaces are retried in the background while the review stays open.
		if (settled && m_scoped_scanner && !m_scan.valid() && !m_stop.stop_requested() && RemoteRetryDue()) (void)StartPendingRemoteScans(app);
		if (m_phase == "scanning" || (settled && m_scan.valid()))
		{
			if (!m_scan.valid())
			{
				if (m_scoped_scanner && StartPendingRemoteScans(app)) return true;
				if (m_scoped_scanner && RemoteScansOutstanding()) return false;
				m_phase = m_candidates.empty() ? "complete" : "ready";
				(void)StartSavedParentBackfill(app);
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
				m_phase = m_phase == "scanning" && m_scoped_scanner && RemoteScansOutstanding() ? "scanning" : m_candidates.empty() ? "complete" : "ready";
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
			// Sub-chats leave the review list here: each waits for its parent's decision, and a fresh scan replaces an earlier sighting.
			const auto take_child = [&](ChatSession& chat, std::filesystem::path* rollout)
			{
				if (chat.native_parent_session_id.empty()) return false;
				Child child{std::move(chat), rollout != nullptr ? std::move(*rollout) : std::filesystem::path{}, {}, {}};
				child.key = chat_identity::NativeChildIdentityKeyForHistoryImport(child.chat);
				child.parent_key = chat_identity::NativeParentIdentityKeyForHistoryImport(child.chat);
				std::erase_if(m_children, [&](const Child& known) { return known.key == child.key; });
				m_children.push_back(std::move(child));
				return true;
			};
			std::erase_if(discovery.chats, [&](ChatSession& chat) { return take_child(chat, nullptr); });
			std::erase_if(discovery.codex_rollouts, [&](std::pair<ChatSession, std::filesystem::path>& record) { return take_child(record.first, &record.second); });
			std::string filter_error;
			if (!ChatHistorySyncService().FilterStartupImportCandidates(app, discovery, &filter_error, true))
			{
				AddError(filter_error);
				discovery.CancelPending();
				m_phase = "complete";
				return true;
			}
			LinkOwnedChildren(app);
			// Identity keys canonicalize workspace paths (a file-system query each); derive each once per pass.
			std::set<std::pair<std::string, std::string>> candidate_keys;
			for (const std::vector<Candidate>* staged : {&m_candidates, &m_startup_deferred})
				for (const Candidate& existing : *staged)
					candidate_keys.emplace(chat_identity::NativeIdentityKeyForHistoryImport(existing.chat), existing.chat.execution_host_id);
			const std::time_t now = std::time(nullptr);
			// Unmatched local histories share one planned workspace per directory; it is saved only with a selected import.
			AppState plan;
			plan.folders = app.folders;
			for (const Candidate& existing : m_candidates)
				if (existing.new_workspace && ChatDomainService().FindFolderById(plan, existing.owner.id) == nullptr) plan.folders.push_back(existing.owner);
			const auto add = [&](ChatSession chat, std::filesystem::path rollout)
			{
				const ChatFolder* owner = ChatDomainService().FindFolderById(app, chat.folder_id);
				bool new_workspace = false;
				if (owner == nullptr && paths::IsControllerLocalWorkspace(chat))
				{
					if (const ChatFolder* planned = ChatHistorySyncService().PlanImportWorkspace(plan, chat))
					{
						new_workspace = ChatDomainService().FindFolderById(app, planned->id) == nullptr;
						owner = new_workspace ? planned : ChatDomainService().FindFolderById(app, planned->id);
						chat.folder_id = owner->id;
					}
				}
				if (owner == nullptr && (!m_include_unregistered_local || !paths::IsControllerLocalWorkspace(chat))) return;
				const std::string key = chat_identity::NativeIdentityKeyForHistoryImport(chat);
				if (candidate_keys.contains({key, chat.execution_host_id})) return;
				// Startup leaves old and default-named sessions to explicit discovery: local ones are rescanned there, remote ones wait below.
				const bool deferred = !m_include_unregistered_local &&
				    (chat_import::NativeImportOlderThanStartupLimit(app.settings, chat, now) || chat_import::NativeImportHasProviderDefaultTitle(chat));
				if (deferred && paths::IsControllerLocalWorkspace(chat)) return;
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
				candidate.summary = {m_generation + ":" + std::to_string(m_candidates.size() + m_startup_deferred.size()), chat.provider_id,
				    chat.native_session_id, strings::SafeLine(chat.title, 512, true), chat.folder_id, chat.workspace_directory,
				    strings::NonEmptyOrFallback(chat.execution_host_id, "local"), chat.updated_at};
				const auto exported = discovery.open_code_exports.find(key);
				if (exported != discovery.open_code_exports.end()) candidate.open_code_export = std::move(exported->second);
				candidate.chat = std::move(chat);
				candidate.rollout = std::move(rollout);
				if (owner != nullptr) candidate.owner = *owner;
				candidate.new_workspace = candidate.summary.new_workspace = new_workspace;
				candidate.summary.workspace_title = strings::SafeLine(candidate.owner.title, 512, true);
				candidate_keys.emplace(key, candidate.chat.execution_host_id);
				(deferred ? m_startup_deferred : m_candidates).push_back(std::move(candidate));
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
						uam::platform::LowerCurrentThreadToBackgroundPriority();
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
			if (m_scoped_scanner && RemoteScansOutstanding())
			{
				(void)StartPendingRemoteScans(app);
				return true;
			}
			m_phase = m_candidates.empty() ? "complete" : "ready";
			(void)StartSavedParentBackfill(app);
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
		if (m_omitted_error_count)
		{
			const bool singular = m_omitted_error_count == 1;
			snapshot.errors.push_back(std::to_string(m_omitted_error_count) + " additional native history " + (singular ? "issue was" : "issues were") + " omitted; review discovery before importing.");
		}
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

	void StartupNativeImportService::HoldBackIneligibleRemoteCandidates(const AppState& app)
	{
		// Local discovery never rescans remote hosts, so a session excluded by a rule or missing its workspace is set aside rather than destroyed: it returns at the next discovery once that changes.
		const auto kept = std::ranges::stable_partition(m_candidates, [&app](const Candidate& candidate)
		{
			if (paths::IsControllerLocalWorkspace(candidate.chat)) return true;
			const ChatFolder* owner = ChatDomainService().FindFolderById(app, candidate.owner.id);
			return owner != nullptr && owner->directory == candidate.owner.directory && owner->execution_host_id == candidate.owner.execution_host_id &&
			    !chat_import::NativeImportExcludedBySettings(app.settings, candidate.chat);
		});
		for (Candidate& candidate : kept) m_startup_deferred.push_back(std::move(candidate));
		m_candidates.erase(kept.begin(), kept.end());
	}

	bool StartupNativeImportService::Revalidate(AppState& app, std::string* error_out)
	{
		HoldBackIneligibleRemoteCandidates(app);
		ChatHistorySyncService::LocalHistoryDiscovery metadata;
		for (const Candidate& candidate : m_candidates) metadata.chats.push_back(OwnershipMetadata(candidate.chat));
		if (!ChatHistorySyncService().FilterStartupImportCandidates(app, metadata, error_out, true)) return false;
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
			const bool staged_or_unassigned = paths::IsControllerLocalWorkspace(candidate.chat) &&
			    (candidate.new_workspace || (m_include_unregistered_local && candidate.owner.id.empty()));
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
		if (ChatDomainService().FindFolderById(app, assignment.id) == nullptr) assignment.title = name;
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
		// Sub-chats follow their parent's decision. Collect them nearest level first, each in its parent's reviewed workspace.
		std::unordered_map<std::string, std::string> rider_folders;
		for (const Candidate& candidate : m_candidates)
			if (selected.contains(candidate.summary.candidate_id)) rider_folders.emplace(chat_identity::NativeIdentityKeyForHistoryImport(candidate.chat), candidate.owner.id);
		std::vector<Child> riders = CollectRiders(rider_folders);
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
		QueueRiders(std::move(riders));
		m_phase = !m_selected.empty() ? "importing" : m_candidates.empty() ? "complete" : "ready";
		return true;
	}

	std::vector<StartupNativeImportService::Child> StartupNativeImportService::CollectRiders(std::unordered_map<std::string, std::string>& folders, std::unordered_map<std::string, std::size_t> per_parent) const
	{
		// Nearest level first, each child in its parent's workspace. A rider stays known afterwards, so one already saved
		// as an ordinary chat can still be linked once its parent is saved.
		std::vector<Child> riders;
		for (bool grew = !m_children.empty(); grew;)
		{
			grew = false;
			for (const Child& child : m_children)
			{
				const auto parent = folders.find(child.parent_key);
				// At most 64 sub-chats per parent, counting those already saved.
				if (parent == folders.end() || folders.contains(child.key) || per_parent[child.parent_key] >= 64) continue;
				++per_parent[child.parent_key];
				riders.push_back(child);
				riders.back().chat.folder_id = parent->second;
				folders.emplace(child.key, parent->second);
				grew = true;
			}
		}
		return riders;
	}

	void StartupNativeImportService::QueueRiders(std::vector<Child> riders)
	{
		// The import takes histories from the back, so a parent placed after its children is admitted before them.
		for (Child& rider : riders)
		{
			if (rider.rollout.empty()) m_import.chats.insert(m_import.chats.begin(), std::move(rider.chat));
			else m_import.codex_rollouts.emplace(m_import.codex_rollouts.begin(), std::move(rider.chat), std::move(rider.rollout));
		}
	}

	std::unordered_map<std::string, ChatSession*> StartupNativeImportService::SavedChatsNamedByChildren(AppState& app) const
	{
		// This runs on the app thread. An import key resolves the chat's workspace on disk, so only chats whose native
		// ID a found child carries or names are keyed, each distinct workspace once.
		std::unordered_set<std::string> named;
		for (const Child& child : m_children)
		{
			named.insert(strings::Trim(child.chat.native_parent_session_id));
			named.insert(child.chat.imported_read_only && strings::IsBlank(child.chat.native_session_id) ? child.chat.id : strings::Trim(child.chat.native_session_id));
		}
		chat_identity::NativeWorkspaceCache workspaces;
		std::unordered_map<std::string, ChatSession*> saved;
		for (ChatSession& chat : app.chats)
		{
			const bool journal = strings::IsBlank(chat.native_session_id);
			if (journal ? !chat.imported_read_only || !named.contains(chat.id) : !named.contains(strings::Trim(chat.native_session_id))) continue;
			saved.insert_or_assign(journal ? chat_identity::NativeChildIdentityKeyForHistoryImport(chat) : chat_identity::NativeIdentityKeyForHistoryImport(chat, workspaces), &chat);
		}
		return saved;
	}

	bool StartupNativeImportService::StartSavedParentBackfill(AppState& app)
	{
		// Only a scan the user started brings in sub-chats of chats already in UAM, once, when that scan settles. Startup
		// never adds chats nobody asked for, and neither does a background remote retry that follows a manual scan.
		if (!std::exchange(m_backfill_due, false) || m_children.empty() || !m_selected.empty() || m_import.Pending()) return false;
		const std::unordered_map<std::string, ChatSession*> saved = SavedChatsNamedByChildren(app);
		std::unordered_map<std::string, std::string> folders;
		std::unordered_map<std::string, std::size_t> saved_children;
		std::unordered_map<std::string, const ChatFolder*> owners;
		for (const Child& child : m_children)
		{
			if (saved.contains(child.key)) { ++saved_children[child.parent_key]; continue; }
			const auto parent = saved.find(child.parent_key);
			if (parent == saved.end() || folders.contains(child.parent_key)) continue;
			const ChatSession& chat = *parent->second;
			if (chat.imported_read_only || !paths::IsControllerLocalWorkspace(chat)) continue;
			// The staged import files a history under the workspace that owns its directory; a parent the user moved elsewhere is left alone.
			const auto [owner, fresh] = owners.try_emplace(chat.workspace_worktree_directory + '\n' + chat.workspace_directory, nullptr);
			if (fresh) owner->second = ChatHistorySyncService().StartupImportWorkspaceOwner(app, chat);
			if (owner->second != nullptr && owner->second->id == chat.folder_id) folders.emplace(child.parent_key, chat.folder_id);
		}
		if (folders.empty()) return false;
		std::vector<Child> riders = CollectRiders(folders, std::move(saved_children));
		std::erase_if(riders, [&](const Child& rider) { return saved.contains(rider.key) || !paths::IsControllerLocalWorkspace(rider.chat); });
		if (riders.empty()) return false;
		m_import.preparation_stop = std::stop_source{};
		QueueRiders(std::move(riders));
		m_phase = "importing";
		return true;
	}

	void StartupNativeImportService::LinkOwnedChildren(AppState& app)
	{
		// Workspace deletion owns its chats until it finishes; the next scan links them.
		if (m_children.empty() || !app.worktree_operation_chat_ids.empty()) return;
		const std::unordered_map<std::string, ChatSession*> saved = SavedChatsNamedByChildren(app);
		for (const Child& found : m_children)
		{
			const auto child = saved.find(found.key);
			const auto parent = saved.find(found.parent_key);
			if (child == saved.end() || parent == saved.end() || !child->second->subagent_parent_chat_id.empty() || !child->second->subagent_parent_created_at.empty()) continue;
			ChatSession& chat = *child->second;
			chat.native_parent_session_id = found.chat.native_parent_session_id;
			const std::string status = app.status_line;
			if (ChatRepository::ApplyImportedNativeChildLineage(*parent->second, chat) && !ChatHistorySyncService().SaveChatWithStatus(app, chat, status, status))
			{
				// An unsaved link is not shown; the next scan tries again.
				chat.subagent_parent_chat_id.clear();
				chat.subagent_parent_created_at.clear();
			}
			app.status_line = status;
		}
	}

	void StartupNativeImportService::FinishImport(AppState& app)
	{
		// Assignment plans belong to this selection, including failed attempts.
		m_import.selected_new_folders.clear();
		m_import.selected_folder_snapshots.clear();
		m_import.expected_hosts.clear();
		for (Candidate& candidate : m_selected) m_candidates.push_back(std::move(candidate));
		m_selected.clear();
		LinkOwnedChildren(app);
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
		m_children.clear();
		m_startup_deferred.clear();
		m_selected.clear();
		if (m_initialized) m_phase = "cancelled";
	}
}
