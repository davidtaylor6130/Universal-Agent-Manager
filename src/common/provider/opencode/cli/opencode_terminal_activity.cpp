#include "common/provider/opencode_terminal_activity.h"
#include "common/provider/opencode/opencode_native_identity.h"
#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/state/app_state.h"
#include "common/utils/hash_utils.h"
#include "remote/runner_client.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <future>
#include <thread>

namespace uam
{
	namespace
	{
		using Json = nlohmann::json;
		using Clock = std::chrono::steady_clock;
		struct Snapshot
		{
			bool ok = false, busy = false;
			std::string latest_completed;
			std::uint64_t generation = 0;
		};
		std::string String(const Json& object, const char* key)
		{
			const auto found = object.find(key);
			return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
		}
	}

	struct OpenCodeTerminalActivityObserver::State
	{
		opencode_native::Endpoint endpoint;
		ExecutionHost host;
		std::string terminal_id, native, workspace;
		opencode_terminal::Query query;
		opencode_terminal::ClockNow clock;
		std::future<Snapshot> pending;
		std::stop_source cancel;
		Clock::time_point next{};
		Clock::time_point receipt_deadline = Clock::now() + std::chrono::seconds(10);
		double startup = 0.0;
		std::uint64_t generation = 0;
		bool started = false, awaiting = false, saw_busy = false, idle_baseline = false;
		std::string last_completed;
		Clock::time_point Now() const { return clock ? clock() : Clock::now(); }
		~State() { cancel.request_stop(); }
	};

	namespace opencode_terminal
	{
		void PrepareLaunch(CliTerminalState& terminal, const ChatSession& chat, const ExecutionHost& host,
		    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment, Query query, ClockNow clock)
		{
			const bool custom = argv.empty() || !opencode_native::IsNativeExecutable(argv.front(), host.platform) ||
			    std::ranges::any_of(argv, [](const std::string& value)
			    {
				    return value == "attach" || value == "--mini" || value.starts_with("--mini=") || value == "--port" || value.starts_with("--port=") ||
				        value == "--hostname" || value.starts_with("--hostname=") || value == "--mdns" || value.starts_with("--mdns=") ||
				        value == "--no-mdns" || value == "--cors" || value.starts_with("--cors=");
			    });
			if (custom || !chat.native_session_id.starts_with("ses_") || !opencode_native::ValidNativeId(chat.native_session_id))
			{
				terminal.opencode_activity_observer.reset();
				if (terminal.native_activity_provider_id == "opencode-cli") terminal.native_activity_provider_id.clear();
				return;
			}
			const std::string workspace = chat.workspace_directory;
			std::shared_ptr<OpenCodeTerminalActivityObserver::State> state;
			if (terminal.opencode_activity_observer) state = terminal.opencode_activity_observer->state;
			if (!state || state->started || state->terminal_id != terminal.terminal_id || state->native != chat.native_session_id ||
			    state->workspace != workspace || !execution_hosts::SameConnection(state->host, host))
			{
				state = std::make_shared<OpenCodeTerminalActivityObserver::State>();
				state->endpoint.token = PlatformServicesFactory::Instance().process_service.GenerateUuid();
				state->endpoint.port = 49152 + static_cast<int>(hashing::Fnv1a64(state->endpoint.token) % 16384);
				if (!opencode_native::ValidEndpoint(state->endpoint)) { terminal.opencode_activity_observer.reset(); return; }
				state->host = host; state->native = chat.native_session_id; state->workspace = workspace;
				state->terminal_id = terminal.terminal_id; state->query = std::move(query);
				state->clock = std::move(clock); state->receipt_deadline = state->Now() + std::chrono::seconds(10);
				terminal.opencode_activity_observer = std::make_shared<OpenCodeTerminalActivityObserver>();
				terminal.opencode_activity_observer->state = state;
			}
			argv.insert(argv.end(), {"--hostname", "127.0.0.1", "--port", std::to_string(state->endpoint.port), "--no-mdns"});
			environment.emplace_back("OPENCODE_SERVER_USERNAME", "opencode");
			environment.emplace_back("OPENCODE_SERVER_PASSWORD", state->endpoint.token);
			if (host.id == execution_hosts::kLocalHostId && opencode_native::RemoveConflictingTerminalObserver(argv, environment, host.platform))
			{
				terminal.opencode_activity_observer.reset();
				terminal.native_activity_provider_id.clear();
				return;
			}
			// Bind the checkpoint before the newly started PTY can accept its first input.
			terminal.native_activity_provider_id = "opencode-cli";
		}

		bool MatchesHost(const CliTerminalState& terminal, const ExecutionHost& host)
		{
			return terminal.opencode_activity_observer && terminal.opencode_activity_observer->state &&
			    execution_hosts::SameConnection(terminal.opencode_activity_observer->state->host, host);
		}
		void Checkpoint(CliTerminalState& terminal)
		{
			if (!terminal.opencode_activity_observer || !terminal.opencode_activity_observer->state) return;
			auto& state = *terminal.opencode_activity_observer->state;
			++state.generation; state.awaiting = true; state.saw_busy = false; state.idle_baseline = false; state.next = {};
			state.receipt_deadline = state.Now() + std::chrono::seconds(10);
		}
		ProviderTerminalActivity Poll(CliTerminalState& terminal, std::string_view native_session, const std::filesystem::path& workspace, bool ambiguous)
		{
			if (!terminal.opencode_activity_observer || !terminal.opencode_activity_observer->state) return ProviderTerminalActivity::Unavailable;
			auto& state = *terminal.opencode_activity_observer->state;
			if (!terminal.running || terminal.lifecycle_state == CliTerminalLifecycleState::ShuttingDown || ambiguous || state.terminal_id != terminal.terminal_id || state.native != native_session ||
			    state.workspace != paths::Utf8PathString(workspace) || (state.started && state.startup != terminal.startup_time_s))
			{ state.cancel.request_stop(); return ProviderTerminalActivity::Unavailable; }
			if (!state.started)
			{
				state.started = true; state.startup = terminal.startup_time_s;
				state.receipt_deadline = state.Now() + std::chrono::seconds(10);
			}
			ProviderTerminalActivity activity = ProviderTerminalActivity::Unavailable;
			const auto quiet_unknown = [&]
			{
				state.idle_baseline = true;
				terminal.uses_prompt_activity_tracking = false;
				terminal.lifecycle_state = CliTerminalLifecycleState::Unknown;
				terminal.turn_state = CliTerminalTurnState::Unknown;
				terminal.generation_in_progress = false;
			};
			if (state.pending.valid() && state.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			{
				Snapshot snapshot;
				try { snapshot = state.pending.get(); } catch (...) { snapshot.generation = state.generation; }
				if (!snapshot.ok && snapshot.generation == state.generation)
				{
					state.next = state.Now() + std::chrono::seconds(1);
					if (state.Now() >= state.receipt_deadline)
					{
						// Missing/old helper or a failed authenticated observer must not spawn forever.
						quiet_unknown();
					}
				}
				if (snapshot.ok && snapshot.generation == state.generation)
				{
					terminal.uses_prompt_activity_tracking = true;
					if (snapshot.busy)
					{
						state.receipt_deadline = state.Now() + std::chrono::seconds(10);
						state.saw_busy = true; state.idle_baseline = false; activity = ProviderTerminalActivity::Busy;
					}
					else if (!state.awaiting || state.saw_busy || (!snapshot.latest_completed.empty() && !state.last_completed.empty() && snapshot.latest_completed != state.last_completed))
					{ state.awaiting = false; state.saw_busy = false; state.idle_baseline = true; activity = ProviderTerminalActivity::Complete; }
					else if (state.Now() >= state.receipt_deadline) quiet_unknown();
					if (!snapshot.latest_completed.empty()) state.last_completed = snapshot.latest_completed;
				}
			}
			// A proven idle TUI needs no background curl/SSH process until deliberate input.
			if (!state.pending.valid() && !state.idle_baseline && state.Now() >= state.next && !state.cancel.stop_requested())
			{
				const auto endpoint = state.endpoint;
				const std::optional<ExecutionHost> host = state.host.id == execution_hosts::kLocalHostId ? std::nullopt : std::optional<ExecutionHost>(state.host);
				const auto query = state.query;
				const std::string expected_workspace = state.workspace, native = state.native;
				const auto generation = state.generation;
				const auto stop = state.cancel.get_token();
				std::packaged_task<Snapshot()> task([endpoint, host, query, expected_workspace, native, generation, stop]
				{
					Snapshot result; result.generation = generation;
					// One bridge serves the complete snapshot and is destroyed on this worker.
					std::unique_ptr<remote::RunnerClient> bridge;
					if (!query && host)
						bridge = std::make_unique<remote::RunnerClient>(PlatformServicesFactory::Instance().process_service,
						    *host);
					const Query perform = query ? query : Query([&](const opencode_native::Endpoint& target, const std::optional<ExecutionHost>&,
					    const std::string& directory, const Json& operation, Json& value, std::string& error, std::stop_token cancel)
					{
						return bridge ? bridge->ObserveOpenCodeTerminal(target.port, target.token, directory, operation, value, &error, cancel) :
						    opencode_native::Query(PlatformServicesFactory::Instance().process_service, target, directory, operation, value, error, cancel);
					});
					Json root, statuses, messages;
					std::string error;
					if (!perform(endpoint, host, expected_workspace, {{"operation", "session"}, {"nativeSessionId", native}}, root, error, stop) || String(root, "id") != native) return result;
					bool matching = opencode::WorkspaceDirectoriesMatch(expected_workspace, String(root, "directory"), host ? std::string_view(host->platform) : std::string_view{});
					if (!host)
					{
						std::error_code a, b;
						const auto expected = std::filesystem::canonical(paths::PathFromUtf8(expected_workspace), a);
						const auto actual = std::filesystem::canonical(paths::PathFromUtf8(String(root, "directory")), b);
						matching = !a && !b && expected == actual;
					}
					if (!matching || !perform(endpoint, host, expected_workspace, {{"operation", "status"}}, statuses, error, stop) || !statuses.is_object()) return result;
					const Json status = statuses.value(native, Json{{"type", "idle"}});
					const std::string type = String(status, "type");
					if (type != "idle" && type != "busy" && type != "retry") return result;
					result.busy = type != "idle";
					if (!result.busy && perform(endpoint, host, expected_workspace, {{"operation", "messages"}, {"nativeSessionId", native}}, messages, error, stop) && messages.is_array() && messages.size() <= 2)
						for (const Json& message : messages)
						{
							const Json info = message.value("info", Json::object());
							if (String(info, "role") != "assistant" || !info.contains("time") || !info["time"].is_object()) continue;
							const auto complete = info["time"].find("completed");
							if (complete != info["time"].end() && complete->is_number() && complete->get<double>() > 0) result.latest_completed = String(info, "id");
						}
					result.ok = !stop.stop_requested();
					return result;
				});
				state.pending = task.get_future();
				state.next = state.Now() + std::chrono::milliseconds(host ? 1000 : 250);
				std::thread(std::move(task)).detach();
			}
			return activity;
		}
	}
}
