#include "app/host_load_integration.h"

#include "app/chat_domain_service.h"
#include "app/execution_routing_integration.h"
#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/compute_pool_gate.h"
#include "common/runtime/app_time.h"
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

		std::unordered_map<std::string, ExecutionHostUsage> CountUsage(const AppState& app, std::size_t& unassigned_waiting)
		{
			std::unordered_map<std::string, ExecutionHostUsage> usage;
			std::vector<ExecutionRoutingOccupancy> occupancy;
			for (const ChatSession& chat : app.chats)
				if (ExecutionRoutingOwnsWork(app, chat) || chat.routing_reservation_pending)
					occupancy.push_back({chat.id, HostIdOf(chat.execution_host_id)});
			if (app.execution_routing_service)
				for (const auto& [host_id, count] : app.execution_routing_service->OccupiedCounts(occupancy)) usage[host_id].in_use = count;
			else
				for (const ExecutionRoutingOccupancy& occupied : occupancy) ++usage[occupied.host_id].in_use;

			unassigned_waiting = 0;
			std::unordered_map<std::string, std::unordered_set<std::string>> waiting;
			for (const auto& [chat_id, text] : app.execution_routing_waits)
			{
				if (text.empty()) continue;
				const ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
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
				const ChatSession* chat = chat_id.empty() ? nullptr : ChatDomainService().FindChatById(app, chat_id);
				if (chat == nullptr || HostIdOf(chat->execution_host_id) == execution_hosts::kLocalHostId) ++usage[std::string(execution_hosts::kLocalHostId)].local_processes;
			}
			for (const auto& [host_id, chats] : waiting) usage[host_id].waiting = chats.size();
			for (const ExecutionRoutingHostPolicy& policy : app.settings.execution_routing.hosts)
				usage[policy.host_id].limit = policy.max_active_turns;
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
		const double now = GetAppTimeSeconds();
		if (now < app.host_load_next_poll_s) return false;
		app.host_load_next_poll_s = now + kUsageRecountSeconds;

		std::size_t unassigned_waiting = 0;
		std::unordered_map<std::string, ExecutionHostUsage> usage = CountUsage(app, unassigned_waiting);
		bool changed = usage != app.execution_host_usage || unassigned_waiting != app.execution_routing_unassigned_waiting;
		app.execution_host_usage = std::move(usage);
		app.execution_routing_unassigned_waiting = unassigned_waiting;

		const bool interest = now < app.host_load_interest_until_s;
		std::vector<ExecutionHost> wanted;
		const ExecutionHostUsage* local = nullptr;
		if (const auto found = app.execution_host_usage.find(std::string(execution_hosts::kLocalHostId)); found != app.execution_host_usage.end()) local = &found->second;
		if (interest || (local != nullptr && (local->in_use > 0 || local->waiting > 0 || local->local_processes > 0)))
			wanted.push_back(execution_hosts::LocalHost());
		for (const ExecutionHost& host : app.settings.execution_hosts)
		{
			if (host.id == execution_hosts::kLocalHostId || !remote::CanAttemptRunnerConnection(host)) continue;
			const std::unordered_map<std::string, ExecutionHostUsage>::const_iterator figures = app.execution_host_usage.find(host.id);
			const bool busy = figures != app.execution_host_usage.end() && (figures->second.in_use > 0 || figures->second.waiting > 0);
			const bool routed = app.settings.execution_routing.enabled && std::ranges::any_of(app.settings.execution_routing.hosts,
			    [&host](const ExecutionRoutingHostPolicy& policy) { return policy.host_id == host.id && policy.enabled; });
			// A computer whose last health check failed is left to the recovery contact; busy work there may still try it.
			if ((interest || routed || busy) && (host.runner_status == "ready" || busy)) wanted.push_back(host);
		}
		if (!wanted.empty() && !app.host_load_sampler) app.host_load_sampler = std::make_shared<host_load::Sampler>();
		if (app.host_load_sampler)
		{
			for (const ExecutionHost& host : wanted)
			{
				host_load::ReaderFactory reader = app.host_load_reader ? app.host_load_reader(host)
				    : host.id == execution_hosts::kLocalHostId ? LocalReader() : RemoteReader(host);
				app.host_load_sampler->Want(host.id, SampleIdentity(host), std::move(reader));
			}
			changed = app.host_load_sampler->Poll() || changed;
		}
		return changed;
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
