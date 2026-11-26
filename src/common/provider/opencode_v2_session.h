#pragma once

#include "common/config/build_features.h"
#include "common/runtime/cli_version_parser.h"
#include "common/utils/string_utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
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
/// Publishes the captured guard: native session, workspace, read-only ceiling and imported agent deny actions.
using PublishPolicy = std::function<void(const std::string&, const std::string&, bool, const std::vector<std::string>&)>;
enum class PermissionProbe { Deny, Ask };
using VerifyPermission = std::function<void(const std::string&, const std::string&, PermissionProbe)>;
using StartupClock = std::function<std::chrono::steady_clock::time_point()>;
using Emit = std::function<void(const Json&)>;
inline constexpr int kRuntimeVerificationError = -32041;
inline constexpr std::string_view kStartupFailureMethod = "uam/opencode/startupFailure";

// The one OpenCode V2 native-protocol contract UAM has verified, for chat, model
// refresh, text workers and history. See docs/reference/opencode-v2-protocol-assessment.md
// for how each bound was verified before widening it.
inline constexpr std::string_view kVerifiedV2Minimum = "2.1.2";
inline constexpr std::string_view kVerifiedV2Maximum = "2.2.7";
inline auto ParseExactVersion(std::string_view version) -> std::optional<std::array<unsigned long long, 3>>
{
	std::array<unsigned long long, 3> parts{};
	for (std::size_t index = 0; index < parts.size(); ++index)
	{
		const auto dot = version.find('.');
		const auto component = version.substr(0, dot);
		if (component.empty() || (component.size() > 1 && component.front() == '0') ||
		    (dot == std::string_view::npos) != (index == parts.size() - 1)) return std::nullopt;
		const auto end = component.data() + component.size();
		const auto parsed = std::from_chars(component.data(), end, parts[index]);
		if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
		if (dot != std::string_view::npos) version.remove_prefix(dot + 1);
	}
	return parts;
}
/// <summary>True only for an exact x.y.z inside the verified V2 range.</summary>
inline bool IsVerifiedV2(std::string_view version)
{
	const auto value = ParseExactVersion(version);
	return value && *value >= *ParseExactVersion(kVerifiedV2Minimum) && *value <= *ParseExactVersion(kVerifiedV2Maximum);
}
/// <summary>These releases can attempt local Chat startup with a process-specific permission proof.</summary>
inline bool NeedsLocalChatRuntimeVerification(std::string_view version)
{
	const auto value = ParseExactVersion(version);
	const auto maximum = ParseExactVersion(kVerifiedV2Maximum);
	return value && value->front() == maximum->front() && *value > *maximum;
}
/// <summary>Names the installed and supported versions for a refused V2 installation.</summary>
inline std::string UnverifiedV2Message(std::string_view installed, std::string_view use)
{
	const std::string shown = strings::SafeLine(installed, 32, true);
	return "OpenCode " + (shown.empty() ? std::string("(unreported version)") : shown) + " is installed, but UAM's " + std::string(use) + " is verified only for OpenCode V2 " +
	       std::string(kVerifiedV2Minimum) + " through " + std::string(kVerifiedV2Maximum) + ".";
}

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
std::string Encode(std::string_view value);
std::string ToolKey(std::string_view message_id, std::string_view tool_id);
std::string PermissionPluginSource();
std::string WorkerVersionError(bool checked, bool same_host, std::string_view version, std::string_view check_error);
#endif
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
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
Json NormalizeExport(const Json& native);
std::vector<std::string> ServeArgv(std::vector<std::string> command);
bool IsCheckedHistoryV2(std::string_view version);
std::vector<std::string> HistoryArgv(std::string_view version, std::vector<std::string> command,
    const std::string& operation, const std::string& id);
/// <summary>HistoryArgv for a host's own CLI over the runner; the list keeps the 200-session bound of the remote reader.</summary>
std::vector<std::string> RemoteHistoryArgv(std::string_view version, std::vector<std::string> command,
    const std::string& operation, const std::string& id);
/// <summary>Maps V2 list entries to the fields the history importer reads; throws on a malformed entry.</summary>
void NormalizeHistoryList(Json& list);
/// <summary>POSIX only: OpenCode 1.x export can exit 0 before a piped stdout drains (1.19.4 stops at 128/192 KiB),
/// so its stdout goes to a private regular file that cat then streams completely.</summary>
std::vector<std::string> FileBackedStdoutArgv(std::vector<std::string> command);
#endif

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
/// <summary>V2 deny actions for an imported agent's canonical V1 rules (tool -> ask|deny). Pinned V2 renames `bash` to
/// `shell` and `task` to `subagent`, and edits are one `edit` action, so both spellings are denied. Throws on invalid rules.</summary>
std::vector<std::string> ImportedDenyActions(const std::string& permission_json);
#endif

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
class Session
{
public:
	Session(Query query, PublishPolicy publish_policy, Emit emit, std::string native_version = std::string(kVerifiedV2Minimum), VerifyPermission verify_permission = {});
	void Handle(const Json& message);
	void Event(const Json& event);
	void CheckDeadlines(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
	void Disconnected(std::string reason = "The OpenCode V2 connection closed before delivery was confirmed; input is held and never replayed.");
	bool Ready() const { return ready_; }
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
	VerifyPermission verify_permission_;
	Emit emit_;
	std::string native_version_, cwd_, session_id_, mode_ = "build", model_, prompt_inbox_;
	bool read_only_ = false, prompt_started_ = false, disconnected_ = false, ready_ = false;
	std::vector<std::string> deny_;
	bool usage_invalid_ = false, startup_created_ = false;
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
std::optional<int> RunCommand(const std::vector<std::string>& arguments, IPlatformProcessService& process_service, StartupClock startup_now = std::chrono::steady_clock::now);
#endif
}
