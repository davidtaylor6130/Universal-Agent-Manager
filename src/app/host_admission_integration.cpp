#include "app/startup_native_import_service.h"
#include "app/host_admission_integration.h"

#include "app/chat_domain_service.h"
#include "common/config/execution_host_config.h"
#include "common/config/host_admission_config.h"
#include "common/runtime/acp/compute_pool_gate.h"
#include "common/runtime/app_time.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <thread>

namespace uam
{
	namespace
	{
		std::string HostOf(const std::string& host_id)
		{
			return strings::NonEmptyOrFallback(strings::Trim(host_id), std::string(execution_hosts::kLocalHostId));
		}

		host_admission::LoadSample LoadFor(const AppState& app, const std::string& host_id)
		{
			if (!app.host_load_sample) return {};
			try { return app.host_load_sample(host_id); }
			catch (...) { return {}; }
		}

		host_admission::Thresholds ThresholdsFor(const HostAdmissionSettings& settings)
		{
			host_admission::Thresholds thresholds;
			thresholds.soft_cpu_percent = settings.soft_cpu_percent;
			thresholds.soft_free_memory_percent = settings.soft_free_memory_percent;
			thresholds.hard_cpu_percent = settings.hard_cpu_percent;
			thresholds.hard_cpu_seconds = settings.hard_cpu_seconds;
			thresholds.hard_free_memory_percent = settings.hard_free_memory_percent;
			return thresholds;
		}
	}

	std::string HostAdmissionLabel(const AppState& app, const std::string& host_id)
	{
		const std::string id = HostOf(host_id);
		if (id == execution_hosts::kLocalHostId) return "this computer";
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, id);
		return host != nullptr ? strings::NonEmptyOrFallback(strings::Trim(host->label), id) : id;
	}

	host_admission::WorkClass HostAdmissionClassForChat(const AcpSessionState* session, const ChatSession& chat)
	{
		if (session != nullptr && (session->model_discovery_only || session->ephemeral_model_discovery)) return host_admission::WorkClass::Background;
		if ((session != nullptr && (!session->managed_agent_run_id.empty() || !session->goal_turn_kind.empty())) || !chat.agent_run_id.empty() ||
		    !chat.goal_owner_chat_id.empty() || !chat.automation_token_id.empty())
			return host_admission::WorkClass::Agent;
		return host_admission::WorkClass::Interactive;
	}

	std::vector<host_admission::HostLimits> HostAdmissionLimits(const AppState& app)
	{
		const HostAdmissionSettings& settings = app.settings.host_admission;
		std::vector<host_admission::HostLimits> limits;
		host_admission::HostLimits local;
		local.host_id = std::string(execution_hosts::kLocalHostId);
		local.label = "this computer";
		local.slot_cap = static_cast<std::size_t>(host_admission_config::LocalSlotLimit(settings, std::thread::hardware_concurrency()));
		local.background_budget = static_cast<std::size_t>(settings.background_budget);
		local.load = LoadFor(app, local.host_id);
		limits.push_back(std::move(local));
		for (const ExecutionHost& host : app.settings.execution_hosts)
		{
			if (host.id == execution_hosts::kLocalHostId) continue;
			host_admission::HostLimits remote;
			remote.host_id = host.id;
			remote.label = HostAdmissionLabel(app, host.id);
			remote.slot_cap = static_cast<std::size_t>(settings.remote_slot_limit);
			for (const ExecutionRoutingHostPolicy& policy : app.settings.execution_routing.hosts)
				if (policy.host_id == host.id && policy.max_active_turns > 0) remote.slot_cap = policy.max_active_turns;
			remote.background_budget = static_cast<std::size_t>(settings.background_budget);
			remote.reachable = host.runner_status != "error";
			remote.load = LoadFor(app, host.id);
			limits.push_back(std::move(remote));
		}
		return limits;
	}

	std::vector<host_admission::LiveWork> HostAdmissionLiveWork(const AppState& app)
	{
		std::vector<host_admission::LiveWork> live;
		for (const std::unique_ptr<AcpSessionState>& session : app.acp_sessions)
		{
			if (!session) continue;
			if (session->model_discovery_only && session->running)
			{
				const ChatSession* chat = ChatDomainService().FindChatById(app, session->chat_id);
				const std::string host = HostOf(!session->process_execution_host_id.empty() ? session->process_execution_host_id : chat != nullptr ? chat->execution_host_id : std::string());
				live.push_back({"discovery-session:" + session->chat_id, host, host_admission::WorkClass::Background});
				continue;
			}
			if (!compute_pools::OccupiesTurnSlot(app, *session)) continue;
			const ChatSession* chat = ChatDomainService().FindChatById(app, session->chat_id);
			if (chat == nullptr) continue;
			live.push_back({"turn:" + chat->id, HostOf(chat->execution_host_id), HostAdmissionClassForChat(session.get(), *chat)});
		}
		for (const std::unique_ptr<CliTerminalState>& terminal : app.cli_terminals)
		{
			if (!terminal || !terminal->running) continue;
			const std::string chat_id = strings::NonEmptyOrFallback(terminal->attached_chat_id, terminal->frontend_chat_id);
			const ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
			if (chat == nullptr) continue;
			live.push_back({"cli:" + chat->id, HostOf(chat->execution_host_id), HostAdmissionClassForChat(nullptr, *chat)});
		}
		// Background work tracked by its own task state: the running version check, the repository inventory read
		// and the history scan. Workers that hold a lease (memory, native discovery) are counted by the lease.
		if (app.runtime_cli_version_check_task.running)
			live.push_back({"version-check", HostOf(app.runtime_cli_version_check_task.execution_host.id), host_admission::WorkClass::Background});
		if (app.repository_route_refresh && !app.repository_route_refresh->finished.load(std::memory_order_acquire))
			live.push_back({"repository-check", std::string(execution_hosts::kLocalHostId), host_admission::WorkClass::Background});
		if (app.startup_native_import_service && app.startup_native_import_service->ScanRunning())
			live.push_back({"history-import", std::string(execution_hosts::kLocalHostId), host_admission::WorkClass::Background});
		return live;
	}

	void RefreshHostAdmission(AppState& app, double now_s)
	{
		if (!app.host_admission) return;
		app.host_admission->SetThresholds(ThresholdsFor(app.settings.host_admission));
		app.host_admission->UpdateHosts(HostAdmissionLimits(app), now_s);
		app.host_admission->SetLive(HostAdmissionLiveWork(app));
	}

	BackgroundAdmission AdmitBackgroundStart(AppState& app, const std::string& key, const std::string& host_id, bool hold)
	{
		BackgroundAdmission result;
		if (!app.host_admission)
		{
			result.admitted = true;
			return result;
		}
		const double now = GetAppTimeSeconds();
		RefreshHostAdmission(app, now);
		host_admission::Request request;
		request.key = key;
		request.host_id = HostOf(host_id);
		request.work_class = host_admission::WorkClass::Background;
		request.hold = hold;
		request.now_s = now;
		const host_admission::Decision decision = app.host_admission->Admit(request);
		result.admitted = decision.admitted;
		result.message = decision.message;
		if (decision.admitted)
		{
			app.host_admission_waits.erase(key);
			if (hold) result.lease = host_admission::HeldLease(app.host_admission, key);
			return result;
		}
		// One diagnostic line when a wait starts or its reason changes, not one per retry.
		std::string& previous = app.host_admission_waits[key];
		if (previous != decision.message) diagnostics::Write("Host admission: background start waits (" + key.substr(0, key.find(':')) + "): " + decision.message);
		previous = decision.message;
		return result;
	}

	nlohmann::json HostAdmissionUsageForFrontend(const AppState& app)
	{
		nlohmann::json hosts = nlohmann::json::object();
		if (!app.host_admission) return hosts;
		for (const host_admission::HostUsage& usage : app.host_admission->Usage(GetAppTimeSeconds()))
			hosts[usage.host_id] = {{"interactive", usage.interactive}, {"agent", usage.agent}, {"background", usage.background}, {"waiting", usage.waiting},
			    {"slotLimit", usage.slot_cap}, {"backgroundBudget", usage.background_budget}, {"health", host_admission::HealthName(usage.health)},
			    {"label", HostAdmissionLabel(app, usage.host_id)}};
		return hosts;
	}
}
