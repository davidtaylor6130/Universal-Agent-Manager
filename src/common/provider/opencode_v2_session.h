#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <functional>
#include <deque>
#include <cstdint>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class IPlatformProcessService;

namespace uam::opencode_v2
{

// Native V2 transport is deliberately provider-private. ACP-shaped messages below
// are UAM's internal projection, never messages sent to `opencode acp`.
using Json = nlohmann::json;
using Query = std::function<Json(const std::string&, const std::string&, const Json&)>;
using PublishPolicy = std::function<void(const std::string&, const std::string&, bool)>;
using Emit = std::function<void(const Json&)>;

std::string Encode(std::string_view value);
std::string ToolKey(std::string_view message_id, std::string_view tool_id);
std::string PermissionPluginSource();
std::string WorkerVersionError(bool checked, bool same_host, std::string_view version, std::string_view check_error);
inline int HistoryCommandTimeoutMilliseconds(const std::vector<std::string>& argv)
{
	// 10s version probe + the original complete 30s native command + 5s
	// dispatch/pipe retirement. Direct/custom V1 retains its original budget.
	return argv.size() >= 4 && argv[1] == "--uam-opencode-history" ? 45000 : 30000;
}
/// <summary>Returns UAM's own sanitized history/bridge refusal from a worker's stderr tail; native stderr is never surfaced.</summary>
inline std::string AdapterError(std::string_view diagnostic_tail)
{
	constexpr std::string_view prefix = "UAM OpenCode adapter: ";
	const std::size_t start = diagnostic_tail.rfind(prefix);
	if (start == std::string_view::npos) return {};
	const std::string_view line = diagnostic_tail.substr(start + prefix.size());
	return std::string(line.substr(0, std::min<std::size_t>(line.find_first_of("\r\n"), 512)));
}
Json NormalizeExport(const Json& native);
std::vector<std::string> ServeArgv(std::vector<std::string> command);
bool IsCheckedHistoryV2(std::string_view version);
std::vector<std::string> HistoryArgv(std::string_view version, std::vector<std::string> command,
    const std::string& operation, const std::string& id);
/// <summary>POSIX only: OpenCode 1.x export can exit 0 before a piped stdout drains (1.19.4 stops at 128/192 KiB),
/// so its stdout goes to a private regular file that cat then streams completely.</summary>
std::vector<std::string> FileBackedStdoutArgv(std::vector<std::string> command);

class Session
{
public:
	Session(Query query, PublishPolicy publish_policy, Emit emit);
	void Handle(const Json& message);
	void Event(const Json& event);
	void CheckDeadlines(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
	void Disconnected(std::string reason = "OpenCode V2 connection closed; delivery may have occurred. Input is held and is never replayed automatically.");
	bool Broken() const { return disconnected_; }
	bool HasTurn() const { return !prompt_id_.is_null(); }
	const std::string& NativeId() const { return session_id_; }

private:
	void Reply(const Json& id, const Json& result);
	void Update(Json update);
	void Policy(const Json& params);
	Json Setup(const Json& params, bool load);
	Json Options();
	void SelectModel(const std::string& id);
	void Finish(const std::string& reason);
	std::string Route(const std::string& suffix = {}) const;
	void RequireEmptyInbox();
	void InterruptCancelledTurn();
	Query query_;
	PublishPolicy publish_policy_;
	Emit emit_;
	std::string cwd_, session_id_, mode_ = "build", model_, prompt_inbox_;
	bool read_only_ = false, prompt_started_ = false, disconnected_ = false, ready_ = false;
	bool usage_invalid_ = false;
	bool cancel_requested_ = false, cancel_interrupt_sent_ = false;
	std::chrono::steady_clock::time_point cancel_deadline_{};
	std::string finish_;
	Json prompt_id_;
	Json policy_capture_, usage_ = Json::object();
	std::map<std::string, Json> tools_, permissions_;
	std::map<std::string, Json> forms_;
	std::map<std::string, Json> model_refs_;
	std::map<std::string, std::int64_t> model_context_;
	std::int64_t last_step_used_ = 0;
	std::unordered_set<std::string> session_grants_;
	std::unordered_set<std::string> seen_events_, seen_steps_;
	std::deque<std::string> seen_event_order_;
};

// Early headless command, before CEF/app startup. No installed native accounts
// are modified; the process owns its private service, config and policy lease.
bool IsCommand(const std::vector<std::string>& arguments);
std::optional<int> RunCommand(const std::vector<std::string>& arguments, IPlatformProcessService& process_service);
}
