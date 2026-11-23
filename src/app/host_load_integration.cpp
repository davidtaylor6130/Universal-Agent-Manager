#include "app/host_load_integration.h"

#include "app/agent_run_scheduler.h"
#include "app/execution_routing_integration.h"
#include "common/config/build_features.h"
#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/compute_pool_gate.h"
#include "common/runtime/app_time.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/string_utils.h"
#include "remote/runner_client.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>

namespace uam
{
	namespace
	{
		constexpr double kUsageRecountSeconds = 1.0;
		constexpr double kInterestSeconds = 30.0;

		std::string HostIdOf(const std::string& host_id)
		{
			return host_id.empty() ? std::string(execution_hosts::kLocalHostId) : host_id;
		}

		host_load::ReaderFactory LocalReader()
		{
			return []
			{
				return host_load::Reader([](std::stop_token)
				{
					return host_load::Reading{host_load::SampleStatus::Known, platform::ReadCurrentHostLoad()};
				});
			};
		}

		/// The connection is opened, reused and closed on the sampling thread only.
		host_load::ReaderFactory RemoteReader(const ExecutionHost& host)
		{
			return [host]
			{
				const std::shared_ptr<remote::RunnerClient> client = std::make_shared<remote::RunnerClient>(PlatformServicesFactory::Instance().process_service, host);
				client->SetDeadlines(std::chrono::seconds(20), std::chrono::seconds(5));
				return host_load::Reader([client](std::stop_token stop)
				{
					client->SetStopToken(stop);
					platform::HostLoad load;
					bool unsupported = false;
					if (client->ReadHostLoad(load, &unsupported, nullptr, stop)) return host_load::Reading{host_load::SampleStatus::Known, load};
					return host_load::Reading{unsupported ? host_load::SampleStatus::Unsupported : host_load::SampleStatus::Unreachable, {}};
				});
			};
		}

		std::string SampleIdentity(const ExecutionHost& host)
		{
			if (host.id == execution_hosts::kLocalHostId) return std::string(execution_hosts::kLocalHostId);
			// A helper update can add or remove host.load, so a new version is read afresh.
			return ExecutionRoutingConnectionIdentity(host) + "|" + host.runner_version + "|" + std::to_string(host.runner_protocol_version);
		}

		/// Chats that can own work by state held outside the chat record. Any other chat owns work only through its own
		/// remote fields, so ExecutionRoutingOwnsWork, which scans sessions, terminals and runs, is asked only about these.
		std::unordered_set<std::string> ChatsWithRuntimeState(const AppState& app)
		{
			std::unordered_set<std::string> ids(app.execution_routing_reserved_chat_ids.begin(), app.execution_routing_reserved_chat_ids.end());
			for (const std::unique_ptr<AcpSessionState>& session : app.acp_sessions)
				if (session) ids.insert(session->chat_id);
			for (const std::unique_ptr<CliTerminalState>& terminal : app.cli_terminals)
				if (terminal) { ids.insert(terminal->attached_chat_id); ids.insert(terminal->frontend_chat_id); }
			for (const AgentRun& run : app.agent_runs)
				if (run.status == "running") ids.insert(run.transcript_chat_id);
			for (const auto& [id, mention] : app.managed_initial_mentions) ids.insert(id);
			return ids;
		}

		bool RemoteFieldsOwnWork(const ChatSession& chat)
		{
			return chat.remote_turn_reconnect_pending || !chat.remote_prompt_delivery_id.empty() || !chat.remote_pending_requests.empty() ||
			    !chat.remote_interaction_responses.empty() || !chat.remote_active_turn_id.empty() || chat.remote_stop_cleanup_pending || chat.remote_restart_pending;
		}

		std::unordered_map<std::string, ExecutionHostUsage> CountUsage(const AppState& app, std::size_t& unassigned_waiting)
		{
			std::unordered_map<std::string, ExecutionHostUsage> usage;
			std::vector<ExecutionRoutingOccupancy> occupancy;
			const std::unordered_set<std::string> runtime_chats = ChatsWithRuntimeState(app);
			const bool every_chat = !app.managed_initial_runtime_owners.empty();
			std::unordered_map<std::string_view, const ChatSession*> chats_by_id;
			chats_by_id.reserve(app.chats.size());
			for (const ChatSession& chat : app.chats)
			{
				chats_by_id.emplace(chat.id, &chat);
				// The same answer as admission's occupancy, without scanning every runtime list for every idle chat.
				// A managed initial runtime owner names its root chat privately, so while any exists every chat is asked.
				if (chat.routing_reservation_pending || ((every_chat || RemoteFieldsOwnWork(chat) || runtime_chats.contains(chat.id)) && ExecutionRoutingOwnsWork(app, chat)))
					occupancy.push_back({chat.id, HostIdOf(chat.execution_host_id)});
			}
			const auto find_chat = [&chats_by_id](const std::string& id) -> const ChatSession*
			{
				const auto found = chats_by_id.find(id);
				return found == chats_by_id.end() ? nullptr : found->second;
			};
			if (app.execution_routing_service)
				for (const auto& [host_id, count] : app.execution_routing_service->OccupiedCounts(occupancy)) usage[host_id].in_use = count;
			else
				for (const ExecutionRoutingOccupancy& occupied : occupancy) ++usage[occupied.host_id].in_use;

			unassigned_waiting = 0;
			std::unordered_map<std::string, std::unordered_set<std::string>> waiting;
			for (const auto& [chat_id, text] : app.execution_routing_waits)
			{
				if (text.empty()) continue;
				const ChatSession* chat = find_chat(chat_id);
				if (chat == nullptr) continue;
				// An automatic chat has no computer until it is admitted; it waits for all of them.
				if (chat->routing_mode == "automatic" && chat->routing_assigned_connection_identity.empty()) ++unassigned_waiting;
				else waiting[HostIdOf(chat->execution_host_id)].insert(chat_id);
			}
			const double now = GetAppTimeSeconds();
			for (const std::unique_ptr<AcpSessionState>& session : app.acp_sessions)
			{
				if (!session) continue;
				if (session->running && HostIdOf(session->process_execution_host_id) == execution_hosts::kLocalHostId) ++usage[std::string(execution_hosts::kLocalHostId)].local_processes;
				if (compute_pools::IsWaiting(*session, now)) waiting[HostIdOf(session->process_execution_host_id)].insert(session->chat_id);
			}
			for (const std::unique_ptr<CliTerminalState>& terminal : app.cli_terminals)
			{
				if (!terminal || !terminal->running) continue;
				const std::string& chat_id = terminal->attached_chat_id.empty() ? terminal->frontend_chat_id : terminal->attached_chat_id;
				const ChatSession* chat = chat_id.empty() ? nullptr : find_chat(chat_id);
				if (chat == nullptr || HostIdOf(chat->execution_host_id) == execution_hosts::kLocalHostId) ++usage[std::string(execution_hosts::kLocalHostId)].local_processes;
			}
			for (const auto& [host_id, chats] : waiting) usage[host_id].waiting = chats.size();
			// A limit is shown only where admission applies it.
			if (app.settings.execution_routing.enabled)
				for (const ExecutionRoutingHostPolicy& policy : app.settings.execution_routing.hosts)
					if (policy.enabled) usage[policy.host_id].limit = policy.max_active_turns;
			return usage;
		}

		const char* StatusName(host_load::SampleStatus status)
		{
			switch (status)
			{
			case host_load::SampleStatus::Known: return "known";
			case host_load::SampleStatus::Unsupported: return "unsupported";
			case host_load::SampleStatus::Unreachable: return "unreachable";
			case host_load::SampleStatus::Stale: return "stale";
			case host_load::SampleStatus::Pending: break;
			}
			return "pending";
		}

		const char* SkipName(ExecutionRoutingHostSkip skip)
		{
			switch (skip)
			{
			case ExecutionRoutingHostSkip::Off: return "off";
			case ExecutionRoutingHostSkip::Unreachable: return "unreachable";
			case ExecutionRoutingHostSkip::ProviderUnavailable: return "providerUnavailable";
			case ExecutionRoutingHostSkip::Unconfirmed: return "unconfirmed";
			case ExecutionRoutingHostSkip::NoMapping: return "noMapping";
			case ExecutionRoutingHostSkip::Full: return "full";
			}
			return "unreachable";
		}
	}

	void RequestHostLoad(AppState& app)
	{
		app.host_load_interest_until_s = GetAppTimeSeconds() + kInterestSeconds;
		// The next poll starts any reading that is not running yet.
		app.host_load_next_poll_s = 0.0;
	}

	bool PollHostLoad(AppState& app)
	{
		// Nothing shows these figures without SSH, so a build without it reads and recounts nothing.
		if constexpr (!UAM_ENABLE_SSH) return false;
		const double now = GetAppTimeSeconds();
		if (now < app.host_load_next_poll_s) return false;
		app.host_load_next_poll_s = now + kUsageRecountSeconds;

		const bool interest = now < app.host_load_interest_until_s;
		const bool has_ssh_host = std::ranges::any_of(app.settings.execution_hosts, [](const ExecutionHost& host) { return host.id != execution_hosts::kLocalHostId; });
		// Counts are shown only for SSH computers and in routing settings; without either, nothing is recounted.
		if (has_ssh_host || interest || app.settings.execution_routing.enabled)
			app.execution_host_usage = CountUsage(app, app.execution_routing_unassigned_waiting);
		else
		{
			app.execution_host_usage.clear();
			app.execution_routing_unassigned_waiting = 0;
		}

		// Load is read for a computer with live or waiting work and, while settings show load, for every computer.
		// Admission does not read load yet, so an idle computer is not sampled just because routing may use it.
		std::vector<ExecutionHost> wanted;
		if (interest) wanted.push_back(execution_hosts::LocalHost());
		for (const ExecutionHost& host : app.settings.execution_hosts)
		{
			if (host.id == execution_hosts::kLocalHostId) continue;
			// An install fence or a missing helper stops a running reader at once, so it never reconnects mid-install.
			if (!remote::CanAttemptRunnerConnection(host))
			{
				if (app.host_load_sampler) app.host_load_sampler->Drop(host.id);
				continue;
			}
			const std::unordered_map<std::string, ExecutionHostUsage>::const_iterator figures = app.execution_host_usage.find(host.id);
			const bool busy = figures != app.execution_host_usage.end() && (figures->second.in_use > 0 || figures->second.waiting > 0);
			// A computer whose last health check failed is left to the recovery contact; busy work there may still try it.
			if ((interest || busy) && (host.runner_status == "ready" || busy)) wanted.push_back(host);
		}
		if (!wanted.empty() && !app.host_load_sampler) app.host_load_sampler = std::make_shared<host_load::Sampler>();
		if (app.host_load_sampler)
		{
			for (const std::string& key : app.host_load_sampler->Keys())
				if (key != execution_hosts::kLocalHostId && execution_hosts::Find(app.settings.execution_hosts, key) == nullptr) app.host_load_sampler->Drop(key);
			std::erase_if(app.host_load_reported_status, [&app](const std::pair<const std::string, std::string>& entry)
			{ return entry.first != execution_hosts::kLocalHostId && execution_hosts::Find(app.settings.execution_hosts, entry.first) == nullptr; });
			for (const ExecutionHost& host : wanted)
			{
				host_load::ReaderFactory reader = app.host_load_reader ? app.host_load_reader(host)
				    : host.id == execution_hosts::kLocalHostId ? LocalReader() : RemoteReader(host);
				app.host_load_sampler->Want(host.id, SampleIdentity(host), std::move(reader));
			}
			(void)app.host_load_sampler->Poll();
			// One line per computer whose reading changed status, never per reading.
			for (const std::string& key : app.host_load_sampler->Keys())
			{
				const std::optional<host_load::Sample> sample = app.host_load_sampler->Latest(key);
				if (!sample || sample->status == host_load::SampleStatus::Pending) continue;
				std::string& reported = app.host_load_reported_status[key];
				const std::string status = StatusName(sample->status);
				if (reported == status) continue;
				if (!reported.empty() || sample->status != host_load::SampleStatus::Known)
					diagnostics::Write("[host-load] computer " + strings::SafeLine(key, 128, true) + " load " + (reported.empty() ? std::string("first read") : reported) + " -> " + status + ".");
				reported = status;
			}
		}
		// Publish only what the interface would show: rounded figures, so a byte of memory never pushes state.
		std::string shown = HostLoadForFrontend(app).dump();
		if (shown == app.host_load_published) return false;
		app.host_load_published = std::move(shown);
		return true;
	}

	nlohmann::json HostLoadForFrontend(const AppState& app)
	{
		nlohmann::json hosts = nlohmann::json::object();
		std::vector<std::string> ids{std::string(execution_hosts::kLocalHostId)};
		for (const ExecutionHost& host : app.settings.execution_hosts)
			if (host.id != execution_hosts::kLocalHostId) ids.push_back(host.id);
		for (const std::string& id : ids)
		{
			const std::unordered_map<std::string, ExecutionHostUsage>::const_iterator found = app.execution_host_usage.find(id);
			const ExecutionHostUsage usage = found != app.execution_host_usage.end() ? found->second : ExecutionHostUsage{};
			nlohmann::json entry = {{"inUse", usage.in_use}, {"waiting", usage.waiting}, {"limit", usage.limit ? nlohmann::json(*usage.limit) : nlohmann::json(nullptr)}};
			const std::optional<host_load::Sample> sample = app.host_load_sampler ? app.host_load_sampler->Latest(id) : std::nullopt;
			entry["status"] = sample ? StatusName(sample->status) : "notSampled";
			const platform::HostLoad load = sample && sample->status == host_load::SampleStatus::Known ? sample->load : platform::HostLoad{};
			// Rounded so a push follows a visible change, not every byte of memory.
			entry["cores"] = load.cores ? nlohmann::json(*load.cores) : nlohmann::json(nullptr);
			entry["loadAverage1m"] = load.load_average_1m ? nlohmann::json(std::round(*load.load_average_1m * 10.0) / 10.0) : nlohmann::json(nullptr);
			entry["cpuPercent"] = load.cpu_percent ? nlohmann::json(std::lround(*load.cpu_percent)) : nlohmann::json(nullptr);
			constexpr std::uint64_t kMemoryStep = 64ull * 1024ull * 1024ull;
			entry["memoryAvailableBytes"] = load.memory_available_bytes ? nlohmann::json(*load.memory_available_bytes / kMemoryStep * kMemoryStep) : nlohmann::json(nullptr);
			entry["memoryTotalBytes"] = load.memory_total_bytes ? nlohmann::json(*load.memory_total_bytes) : nlohmann::json(nullptr);
			// This computer's own count is always known; a remote count comes only from its helper.
			entry["processes"] = id == execution_hosts::kLocalHostId ? nlohmann::json(usage.local_processes)
			    : load.runner_processes ? nlohmann::json(*load.runner_processes) : nlohmann::json(nullptr);
			hosts[id] = std::move(entry);
		}
		return {{"hosts", std::move(hosts)}, {"unassignedWaiting", app.execution_routing_unassigned_waiting}};
	}

	nlohmann::json RoutingWaitHostsForFrontend(const AppState& app, const std::string& chat_id)
	{
		nlohmann::json hosts = nlohmann::json::array();
		const std::unordered_map<std::string, std::string>::const_iterator wait = app.execution_routing_waits.find(chat_id);
		const std::unordered_map<std::string, ExecutionRoutingWaitHosts>::const_iterator reasons = app.execution_routing_wait_hosts.find(chat_id);
		if (wait == app.execution_routing_waits.end() || reasons == app.execution_routing_wait_hosts.end() || reasons->second.text != wait->second) return hosts;
		for (const ExecutionRoutingSkippedHost& skipped : reasons->second.hosts)
			hosts.push_back({{"hostId", skipped.host_id}, {"reason", SkipName(skipped.reason)}});
		return hosts;
	}
}
