#include "common/provider/opencode_v2_session.h"
#include "common/runtime/cli_version_parser.h"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace uam::opencode_v2;
void RunOpenCodeV2TransportFixtures();
namespace
{
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Native
{
	Json info{{"id", "ses_owned"}, {"agent", "build"}, {"location", {{"directory", "/workspace"}}}, {"permissions", Json::array({{{"action", "*"}, {"resource", "*"}, {"effect", "allow"}}})}};
	std::vector<Json> calls, output, policies;
	bool mismatch = false;
	bool queued_native_input = false, lose_prompt_receipt = false, lose_cancel_receipt = false, lose_delete_receipt = false, interrupt_idle = false, fail_event_control = false;
	Session session;
	Native() : session([&](const auto& method, const auto& route, const auto& body) { return Query(method, route, body); },
		[&](const auto& id, const auto& cwd, bool read) { policies.push_back({{"id", id}, {"cwd", cwd}, {"read", read}}); },
		[&](const auto& message) { output.push_back(message); }) {}
	Json Query(const std::string& method, const std::string& route, const Json& body)
	{
		calls.push_back({{"method", method}, {"route", route}, {"body", body}});
		if (route.starts_with("/api/location")) return {{"directory", "/workspace"}};
		if (route.starts_with("/api/plugin?")) return {{"data", Json::array()}};
		if (route.starts_with("/api/model/default?")) return {{"data", {{"providerID", "mock"}, {"id", "model/with/slash"}}}};
		if (route.starts_with("/api/model?")) return {{"data", Json::array({{{"providerID", "mock"}, {"id", "model/with/slash"}, {"name", "Mock"}, {"limit", {{"context", 200000}, {"output", 32000}}}, {"variants", Json::array({{{"id", "high"}}})}}})}};
		if (method == "POST" && route == "/api/session") { info["permissions"] = body.at("permissions"); return {{"data", info}}; }
		if (method == "PATCH" && route == "/api/session/ses_owned") { if (!mismatch) info["permissions"] = body.at("permissions"); return Json::object(); }
		if (method == "POST" && route.ends_with("/model")) { info["model"] = body.at("model"); return Json::object(); }
		if (method == "POST" && route.ends_with("/agent")) { info["agent"] = body.at("agent"); return Json::object(); }
		if (method == "GET" && route == "/api/session/ses_owned") return {{"data", info}};
		if (method == "GET" && route.ends_with("/inbox")) return {{"data", queued_native_input ? Json::array({{{"id", "msg_preexisting"}}}) : Json::array()}};
		if (method == "POST" && route.ends_with("/prompt"))
		{
			if (lose_prompt_receipt) throw std::runtime_error("Injected lost native admission reply");
			return {{"data", {{"id", "msg_admitted"}, {"sessionID", "ses_owned"}}}};
		}
		if (route.ends_with("/interrupt") && lose_cancel_receipt) throw std::runtime_error("Injected lost native cancellation reply");
		if (method == "DELETE" && route.find("/inbox/") != std::string::npos && lose_delete_receipt) throw std::runtime_error("Injected unconfirmed inbox deletion");
		if (route.ends_with("/interrupt")) return {{"interrupted", !interrupt_idle}};
		if (fail_event_control && (route.ends_with("/reply") || method == "DELETE")) throw std::runtime_error("Injected event control failure");
		if (route.ends_with("/interrupt") || route.ends_with("/reply") || route.starts_with("/api/experimental/mcp/") || (method == "DELETE" && (route.find("/form/") != std::string::npos || route.find("/inbox/") != std::string::npos))) return Json::object();
		throw std::runtime_error("Unmocked route: " + route);
	}
	void Send(int id, const std::string& method, Json params) { session.Handle({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}); }
	void Setup(bool load = false, bool read = false)
	{
		Send(1, load ? "session/load" : "session/new", {{"cwd", "/workspace"}, {"sessionId", "ses_owned"}, {"_meta", {{"uamPolicy", {{"readOnly", read}}}}}});
	}
	void Prompt(int id = 2, bool read = false)
	{
		Send(id, "session/prompt", {{"sessionId", "ses_owned"}, {"prompt", Json::array({{{"type", "text"}, {"text", "captured input"}}})}, {"_meta", {{"uamPolicy", {{"readOnly", read}}}}}});
	}
	void Event(const std::string& type, Json data, std::string id = {})
	{
		data["sessionID"] = "ses_owned";
		if (type.starts_with("session.tool.") && !data.contains("assistantMessageID")) data["assistantMessageID"] = "msg_assistant";
		if (type == "permission.asked" && data.contains("source") && !data["source"].contains("messageID")) data["source"]["messageID"] = "msg_assistant";
		session.Event({{"type", type}, {"data", data}, {"id", id}});
	}
	void Started() { Event("session.inbox.delivered", {{"inboxID", "msg_admitted"}}); }
};
void Run(const char* name, const std::function<void()>& test) { test(); std::cout << "PASS " << name << '\n'; }
}

int main(int argc, char** argv)
{
	if (argc == 2 && std::string(argv[1]) == "--plugin-source") { std::cout << PermissionPluginSource(); return 0; }
	try
	{
		Run("HeadlessTransportUsesOnlyInjectedNativeProcesses", RunOpenCodeV2TransportFixtures);
		if (argc == 2 && std::string(argv[1]) == "--transport-only") return 0;
		Run("TextWorkerVersionIsHeldUntilExecutingHostIdentityIsChecked", []
		{
			Check(!WorkerVersionError(false, true, "2.1.2", "").empty(), "unchecked worker fell back to V1");
			Check(!WorkerVersionError(true, false, "2.1.2", "").empty(), "foreign host authorized worker");
			Check(!WorkerVersionError(true, true, "2.1.2", "probe failed").empty(), "failed check authorized worker");
			Check(!WorkerVersionError(true, true, "", "").empty(), "empty checked version authorized worker");
			Check(!WorkerVersionError(true, true, "1.invalid", "").empty(), "malformed V1 identity authorized worker");
			Check(WorkerVersionError(true, true, "1.19.4", "").empty(), "confirmed V1 worker lost its route");
			Check(WorkerVersionError(true, true, "2.1.2", "").empty(), "checked V2 worker lost its route");
			Check(!WorkerVersionError(true, true, "2.2.0", "").empty(), "unknown V2 worker authorized");
		});
		Run("DecoratedCliVersionsAndCompleteHistoryBudgetsRemainCompatible", []
		{
			for (const std::string raw : {"1.19.4\n", "opencode v1.19.4 (Homebrew)", "CLI version: 1.19.4+build"})
				Check(uam::cli_versions::ExtractSemverVersion(raw) == std::optional<std::string>{"1.19.4"}, "version manager and history dispatch disagree");
			Check(!uam::cli_versions::ExtractSemverVersion("DESCRIPTION"), "unversioned output was accepted");
			Check(HistoryCommandTimeoutMilliseconds({"uam", "--uam-opencode-history", "export", "ses_owned", "--", "opencode"}) >= 10000 + 30000 + 5000, "history probe consumed the V1 export budget");
			Check(HistoryCommandTimeoutMilliseconds({"custom-opencode", "export", "ses_owned", "--pure"}) == 30000, "custom/native history budget changed");
		});
		Run("ExactGenerationSpecificLaunchAndHistoryArguments", []
		{
			Check(ServeArgv({"opencode"}) == std::vector<std::string>({"opencode", "serve", "--stdio", "--hostname", "127.0.0.1", "--port", "0"}), "V2 serve arguments changed");
			Check(HistoryArgv("1.19.4", {"opencode"}, "list", {}) == std::vector<std::string>({"opencode", "session", "list", "--format", "json", "--pure"}), "V1 list changed");
			Check(HistoryArgv("1.19.4", {"opencode"}, "export", "ses_exact") == std::vector<std::string>({"opencode", "export", "ses_exact", "--pure"}), "V1 export changed");
			Check(HistoryArgv("2.1.2", {"opencode"}, "export", "ses_exact") == std::vector<std::string>({"opencode", "session", "export", "ses_exact", "--standalone"}), "V2 export changed");
			std::string refusal; try { (void)HistoryArgv("2.2.0", {"opencode"}, "export", "ses_exact"); } catch (const std::exception& error) { refusal = error.what(); }
			Check(refusal.find("OpenCode 2.2.0 history has not been verified") != std::string::npos, "unknown V2 history admitted or unnamed");
			Check(AdapterError("native noise\nUAM OpenCode adapter: " + refusal + "\n") == refusal && AdapterError("native only\n").empty(), "adapter refusal not isolated from native stderr");
		});
		Run("CreatePublishesAndReadsBackExactPolicy", []
		{
			Native native; native.Setup(); Check(native.output.back().contains("result"), "setup failed");
			Check(native.output.back()["result"]["models"]["availableModels"][0]["contextWindowTokens"] == 200000 && native.output.back()["result"]["models"]["availableModels"][0]["maxOutputTokens"] == 32000, "observed model limits were dropped");
			Check(native.policies.size() == 2 && native.policies.front()["id"] == "" && native.policies.back()["id"] == "ses_owned", "policy root not bound");
			for (const auto& rule : native.info["permissions"]) Check(rule["effect"] == "deny", "session permissions weakened native agent denials");
		});
		Run("ResumeReplacesNativeSessionGrantsWithoutCreating", []
		{
			Native native; native.Setup(true, true); Check(native.output.back().contains("result"), "resume failed");
			for (const auto& call : native.calls) Check(!(call["route"] == "/api/session" && call["method"] == "POST"), "resume created replacement");
			Check(native.info["permissions"].size() == 3, "read ceiling absent");
		});
		Run("MismatchedReadbackAndWorkspaceHoldInput", []
		{
			Native native; native.mismatch = true; native.Setup(true); Check(native.output.back().contains("error"), "bad policy accepted");
			Native foreign; foreign.info["location"]["directory"] = "/foreign"; foreign.Setup(true); Check(foreign.output.back().contains("error"), "foreign resume accepted");
		});
		Run("UnownedNativeQueueCannotBeWokenByResumeOrNewInput", []
		{
			Native resumed; resumed.queued_native_input = true; resumed.Setup(true);
			Check(resumed.output.back().contains("error") && resumed.session.NativeId() == "ses_owned", "queued native resume silently replaced or started");
			for (const auto& call : resumed.calls) Check(call["method"] != "PATCH", "resume rewrote policy for unowned queued work");
			Native native; native.Setup(); native.calls.clear(); native.queued_native_input = true; native.Prompt();
			for (const auto& call : native.calls) Check(call["method"] != "PATCH", "new input rewrote policy for unowned queued work");
			for (const auto& call : native.calls) Check(call["route"] != "/api/session/ses_owned/prompt", "pre-existing queue was woken");
		});
		Run("LostNativeAdmissionNeverAutomaticallySubmitsAgain", []
		{
			Native native; native.Setup(); native.lose_prompt_receipt = true; native.Prompt(); const auto calls = native.calls.size();
			Check(native.output.back().at("error").at("data").value("uamInputHeld", false) &&
			    native.output.back().at("error").at("data").value("noReplay", false), "ambiguous admission did not mark the owned input held");
			native.Prompt(3); Check(native.calls.size() == calls && native.output.back().contains("error"), "ambiguous input was replayed");
		});
		Run("PromptAdmissionRequiresOwnedInboxAndPreservesUsage", []
		{
			Native native; native.Setup(); native.Prompt(); const auto before = native.output.size();
			native.Event("session.execution.succeeded", {}); Check(native.output.size() == before, "HTTP admission or stale execution completed prompt");
			native.Started(); native.Event("session.text.delta", {{"delta", "hello"}}, "evt_text");
			native.Event("session.text.delta", {{"delta", "hello"}}, "evt_text");
			native.Event("session.step.ended", {{"tokens", {{"input", 10}, {"output", 3}, {"reasoning", 2}, {"cache", {{"read", 4}, {"write", 1}}}}}}, "evt_usage");
			native.Event("session.execution.succeeded", {});
			Check(native.output.back()["result"]["usage"]["totalTokens"] == 20, "usage lost/doubled");
			Check(native.output.size() == before + 3, "duplicate delta was projected");
			Check(native.output[native.output.size() - 2]["params"]["update"]["used"] == 20, "last-step context was confused with billed turn usage");
		});
		Run("ApprovalOnceAndSessionMemoryNeverWriteNativeAlways", []
		{
			Native native; native.Setup(); native.Prompt(); native.Started();
			native.Event("session.tool.input.started", {{"id", "tool_owned"}, {"name", "shell"}});
			native.Event("session.tool.called", {{"id", "tool_owned"}, {"input", {{"command", "git status"}}}});
			const Json ask{{"id", "per_first"}, {"action", "shell"}, {"resources", Json::array({"git status"})}, {"source", {{"type", "tool"}, {"id", "tool_owned"}}}};
			native.Event("permission.asked", ask);
			native.session.Handle({{"id", "v2:per_first"}, {"result", {{"outcome", {{"outcome", "selected"}, {"optionId", "session"}}}}}});
			Check(native.calls.back()["body"]["decision"] == "once", "native saved grant written");
			Json again = ask; again["id"] = "per_second"; const auto before = native.output.size(); native.Event("permission.asked", again);
			Check(native.output.size() == before && native.calls.back()["body"]["decision"] == "once", "UAM session grant not reused");
			native.Event("session.execution.succeeded", {}); native.Prompt(3, true); native.Started();
			native.Event("session.tool.called", {{"id", "tool_owned"}, {"name", "shell"}, {"input", {{"command", "git status"}}}});
			again["id"] = "per_read"; native.Event("permission.asked", again);
			Check(native.output.back().value("method", "") == "session/request_permission", "changed read policy reused broad grant");
		});
		Run("ReadOnlyRejectsEditEvenWithSyntheticNativeAllow", []
		{
			Native native; native.Setup(false, true); native.Prompt(2, true); native.Started();
			native.Event("permission.asked", {{"id", "per_edit"}, {"action", "edit"}, {"resources", Json::array({"/workspace/file"})}});
			Check(native.calls.back()["body"]["decision"] == "reject", "read policy widened");
		});
		Run("RetiredApprovalCannotPublishAGrantIntoTheNextTurn", []
		{
			Native native; native.Setup(); native.Prompt(); native.Started();
			native.Event("session.tool.called", {{"id", "read"}, {"name", "shell"}, {"input", {{"command", "git status"}}}});
			Json ask{{"id", "per_retired"}, {"action", "shell"}, {"resources", Json::array({"git status"})}, {"source", {{"type", "tool"}, {"id", "read"}}}};
			native.Event("permission.asked", ask);
			native.Event("session.execution.succeeded", {});
			native.Prompt(3, true); native.Started();
			const auto calls = native.calls.size();
			native.session.Handle({{"id", "v2:per_retired"}, {"result", {{"outcome", {{"outcome", "selected"}, {"optionId", "session"}}}}}});
			Check(native.calls.size() == calls, "retired approval reached the native service");
			native.Event("session.tool.called", {{"id", "read"}, {"name", "shell"}, {"input", {{"command", "git status"}}}});
			ask["id"] = "per_current"; native.Event("permission.asked", ask);
			Check(native.output.back().value("method", "") == "session/request_permission", "retired approval populated the current policy's remembered grant");
		});
		Run("AmbiguousCancelNotificationRetiresTheInputLease", []
		{
			Native native; native.Setup(); native.Prompt(); native.Started(); native.lose_cancel_receipt = true;
			native.session.Handle({{"method", "session/cancel"}, {"params", {{"sessionId", "ses_owned"}}}});
			Check(!native.session.Broken(), "lost cancellation reply discarded a still-observable native outcome");
			native.session.CheckDeadlines(std::chrono::steady_clock::now() + std::chrono::seconds(6));
			Check(native.session.Broken() && native.output.back()["id"] == 2 && native.output.back().contains("error"), "ambiguous notification silently retained a writable lease");
			Check(native.output.back()["error"]["data"] == Json{{"uamInputHeld", true}, {"noReplay", true}}, "shared consumer lost the machine-readable recovery hold");
			const auto calls = native.calls.size(); native.Prompt(3);
			Check(native.calls.size() == calls, "input was replayed after cancellation lost confirmation");
		});
		Run("ReusedToolIdsRetainExactNativeMessageInput", []
		{
			Native native; native.Setup(); native.Prompt(); native.Started();
			for (const std::string message : {"msg_read", "msg_write"})
			{
				native.Event("session.tool.input.started", {{"assistantMessageID", message}, {"id", "reused"}, {"name", "shell"}});
				native.Event("session.tool.called", {{"assistantMessageID", message}, {"id", "reused"}, {"input", {{"command", message == "msg_read" ? "git status" : "touch file"}}}});
			}
			for (const std::string message : {"msg_read", "msg_write"})
			{
				native.Event("permission.asked", {{"id", "per_" + message}, {"action", "shell"}, {"resources", Json::array({"*"})}, {"source", {{"type", "tool"}, {"messageID", message}, {"id", "reused"}}}});
				const auto& tool = native.output.back()["params"]["toolCall"];
				Check(tool["toolCallId"] == ToolKey(message, "reused"), "native tool identity collided");
				Check(tool["rawInput"]["command"] == (message == "msg_read" ? "git status" : "touch file"), "approval borrowed another message's input");
			}
		});
		Run("ExternalDirectoryApprovalDoesNotInheritAutomaticEditKind", []
		{
			Native native; native.Setup(); native.Prompt(); native.Started();
			native.Event("session.tool.input.started", {{"id", "edit_outside"}, {"name", "edit"}});
			native.Event("session.tool.called", {{"id", "edit_outside"}, {"input", {{"filePath", "/outside/file"}}}});
			native.Event("permission.asked", {{"id", "per_outside"}, {"action", "external_directory"}, {"resources", Json::array({"/outside/*"})}, {"source", {{"type", "tool"}, {"id", "edit_outside"}}}});
			Check(native.output.back()["params"]["toolCall"]["kind"] == "other", "workspace edit mode would automatically approve external access");
		});
		Run("InvalidTokenCountsRemainUnknownWithoutBreakingCompletion", []
		{
			Native native; native.Setup(); native.Prompt(); native.Started();
			native.Event("session.step.ended", {{"assistantMessageID", "msg_bad_usage"}, {"tokens", {{"input", -1}, {"output", 3}, {"reasoning", 0}, {"cache", {{"read", 0}, {"write", 0}}}}}}, "evt_bad_usage");
			native.Event("session.execution.succeeded", {});
			Check(native.output.back()["result"]["stopReason"] == "end_turn" && !native.output.back()["result"].contains("usage"), "invalid usage became billed counts or broke completion");
		});
		Run("CancelWaitsForNativeInterruptionAndDisconnectNeverReplays", []
		{
			Native native; native.Setup(); native.Prompt(); native.Started(); const auto before = native.output.size();
			native.Send(3, "session/cancel", {{"sessionId", "ses_owned"}}); Check(native.output.size() == before, "interrupt acceptance completed turn");
			native.Event("session.execution.interrupted", {}); Check(native.output.back()["result"]["stopReason"] == "cancelled", "interruption not settled");
			native.Prompt(4); native.session.Disconnected(); const auto calls = native.calls.size(); native.Prompt(5); Check(native.calls.size() == calls, "disconnected input replayed");
		});
		Run("ModelsPreserveNativeSlashesAndVariantIdentity", []
		{
			Native native; native.Setup(); native.Send(2, "session/set_model", {{"sessionId", "ses_owned"}, {"modelId", "mock/model/with/slash#high"}});
			Check(native.info["model"] == Json({{"providerID", "mock"}, {"id", "model/with/slash"}, {"variant", "high"}}), "model parsed ambiguously");
			Check(native.output.back()["result"].contains("configOptions"), "model options were published before the model acknowledgement");
		});
		Run("QueuedCancelRequiresExactNativeInboxCancellation", []
		{
			Native native; native.Setup(); native.Prompt(); const auto before = native.output.size();
			native.Send(3, "session/cancel", {{"sessionId", "ses_owned"}});
			for (const auto& call : native.calls) Check(!call["route"].get<std::string>().ends_with("/interrupt"), "queued cancellation interrupted unrelated execution");
			Check(native.output.size() == before, "cancel ACK inferred queued settlement");
			native.Event("session.inbox.cancelled", {{"inboxID", "msg_foreign"}}); Check(native.output.size() == before, "foreign inbox cancelled owned prompt");
			native.Event("session.inbox.cancelled", {{"inboxID", "msg_admitted"}}); Check(native.output.back()["result"]["stopReason"] == "cancelled", "owned queued input not settled");
			Check(!native.session.Broken(), "normal queued cancellation lost the process");
		});
		Run("DeliveredCancelRaceRequiresOwnedDeliveryEvenAfterFailedDelete", []
		{
			for (const bool lost_delete : {false, true})
			{
				Native native; native.Setup(); native.Prompt(); native.lose_delete_receipt = lost_delete;
				native.Send(3, "session/cancel", {{"sessionId", "ses_owned"}});
				Check(!native.session.Broken(), "early cancel prematurely lost the process");
				for (const auto& call : native.calls) Check(!call["route"].get<std::string>().ends_with("/interrupt"), "DELETE outcome was treated as delivery proof");
				native.Event("session.inbox.delivered", {{"inboxID", "msg_foreign"}});
				Check(native.calls.back()["method"] == "DELETE", "foreign delivery triggered interrupt");
				native.Started(); Check(native.calls.back()["route"] == "/api/session/ses_owned/interrupt", "owned delivery failed to interrupt");
				native.Event("session.execution.interrupted", {});
				Check(native.output.back()["result"]["stopReason"] == "cancelled" && !native.session.Broken(), "normal early cancellation became process loss");
				native.Prompt(4); Check(native.session.HasTurn(), "cancelled native session could not accept the next explicit turn");
			}
		});
		Run("AmbiguousDeleteNeverInfersDeliveryOrReplaysInput", []
		{
			Native native; native.Setup(); native.Prompt(); native.lose_delete_receipt = true;
			native.Send(3, "session/cancel", {{"sessionId", "ses_owned"}});
			native.session.CheckDeadlines(std::chrono::steady_clock::now() + std::chrono::seconds(6));
			Check(native.session.Broken() && native.output.back()["error"]["message"].get<std::string>().find("never replayed") != std::string::npos, "ambiguous deletion did not hold input explicitly");
			for (const auto& call : native.calls) Check(!call["route"].get<std::string>().ends_with("/interrupt"), "failed DELETE blindly became delivered");
			const auto calls = native.calls.size(); native.Prompt(4); Check(native.calls.size() == calls, "ambiguous cancelled input was replayed");
		});
		Run("IdleOrLostInterruptReplyCanSettleThroughOwnedTerminalEvent", []
		{
			for (const bool lost_reply : {false, true})
			{
				Native native; native.Setup(); native.Prompt(); native.Started(); native.interrupt_idle = true; native.lose_cancel_receipt = lost_reply;
				native.Send(3, "session/cancel", {{"sessionId", "ses_owned"}});
				Check(!native.session.Broken(), "idle/lost interrupt acknowledgement killed the process");
				native.Event(lost_reply ? "session.execution.interrupted" : "session.execution.succeeded", {});
				Check(!native.session.Broken() && !native.session.HasTurn(), "owned terminal event did not settle cancellation race");
			}
		});
		Run("MalformedEventsAndFailedEventControlsExplicitlyHoldWithoutReplay", []
		{
			for (int scenario = 0; scenario < 4; ++scenario)
			{
				Native native; native.Setup(false, true); native.Prompt(2, true); native.Started();
				if (scenario == 0) native.Event("permission.asked", {{"id", "per_bad"}, {"action", "shell"}, {"source", {{"messageID", "msg_assistant"}, {"id", "tool"}}}});
				if (scenario == 1) native.session.Event({{"type", "session.tool.called"}, {"data", {{"sessionID", "ses_owned"}, {"id", "tool"}}}});
				if (scenario == 2) { native.fail_event_control = true; native.Event("permission.asked", {{"id", "per_edit"}, {"action", "edit"}, {"resources", Json::array({"file"})}}); }
				if (scenario == 3) { native.fail_event_control = true; native.Event("form.created", {{"form", {{"id", "frm_bad"}, {"sessionID", "ses_owned"}, {"fields", Json::array({{{"key", "external"}, {"type", "external"}}})}}}}); }
				Check(native.session.Broken() && native.output.back()["id"] == 2 && native.output.back().contains("error"), "event failure escaped without the owned prompt error");
				Check(native.output.back()["error"]["data"] == Json{{"uamInputHeld", true}, {"noReplay", true}}, "event failure did not retain explicit no-replay semantics");
				const auto calls = native.calls.size(); native.Prompt(3); Check(native.calls.size() == calls, "event failure replayed input");
			}
		});
		Run("CancellationRejectsLateApprovalsWithoutLearningSessionGrants", []
		{
			Native native; native.Setup(); native.Prompt(); native.Started();
			native.Event("session.tool.called", {{"id", "tool"}, {"name", "shell"}, {"input", {{"command", "git status"}}}});
			const Json permission{{"id", "per_late"}, {"action", "shell"}, {"resources", Json::array({"git status"})}, {"source", {{"id", "tool"}}}};
			native.Event("permission.asked", permission);
			native.Send(3, "session/cancel", {{"sessionId", "ses_owned"}});
			native.session.Handle({{"id", "v2:per_late"}, {"result", {{"outcome", {{"outcome", "selected"}, {"optionId", "session"}}}}}});
			Check(native.calls.back()["body"]["decision"] == "reject", "late approval widened a cancelling turn");
			auto second = permission; second["id"] = "per_after_cancel";
			native.Event("permission.asked", second);
			Check(native.calls.back()["body"]["decision"] == "reject", "new native permission escaped cancellation denial");
			native.Event("session.execution.interrupted", {});
			native.Prompt(4); native.Started();
			native.Event("session.tool.called", {{"id", "tool"}, {"name", "shell"}, {"input", {{"command", "git status"}}}});
			native.Event("permission.asked", permission);
			Check(native.output.back().value("method", "") == "session/request_permission", "cancelling approval became a remembered session grant");
		});
		Run("PlanAndEffortChangesAreConfirmedBeforeNextInput", []
		{
			Native native; native.Setup(); native.Send(2, "session/set_config_option", {{"sessionId", "ses_owned"}, {"configId", "effort"}, {"value", "high"}});
			Check(native.info["model"]["variant"] == "high" && native.output.back().contains("result"), "effort not confirmed");
			native.Send(3, "session/set_mode", {{"sessionId", "ses_owned"}, {"modeId", "plan"}});
			Check(native.policies.back()["read"] == true, "Plan policy not published");
			native.Prompt(4); Check(native.policies.back()["read"] == true, "prompt widened Plan");
		});
		Run("NativeQuestionsRetainFieldAndOptionIdentity", []
		{
			Native native; native.Setup(); native.Prompt(); native.Started();
			Json form{{"id", "frm_owned"}, {"sessionID", "ses_owned"}, {"title", "Choose"}, {"fields", Json::array({{{"key", "choice"}, {"type", "string"}, {"options", Json::array({{{"label", "Human label"}, {"value", "native-value"}}})}}})}};
			native.Event("form.created", {{"form", form}}, "evt_form");
			Check(native.output.back().value("method", "") == "uam/opencode/question", "question was not projected");
			native.session.Handle({{"id", "v2form:frm_owned"}, {"result", {{"answers", {{"choice", {{"answers", Json::array({"Human label"})}}}}}}}});
			Check(native.calls.back()["body"]["answer"]["choice"] == "native-value", "question option value lost");
			form["id"] = "frm_unsupported"; form["fields"][0]["type"] = "external";
			native.Event("form.created", {{"form", form}}, "evt_external");
			Check(native.calls.back()["method"] == "DELETE", "unsupported form left hanging");
		});
		Run("LocationMcpUsesDefinitionsNotPreferenceKeys", []
		{
			Native native; native.Send(1, "session/new", {{"cwd", "/workspace"}, {"_meta", {{"uamPolicy", {{"readOnly", false}}}}}, {"mcpServers", Json::array({{{"name", "uam-control"}, {"command", "private-helper"}, {"args", Json::array({"--stdio"})}, {"env", Json::array({{{"name", "PRIVATE"}, {"value", "value"}}})}}})}});
			bool registered = false;
			for (const auto& call : native.calls) if (call["route"] == "/api/experimental/mcp/uam-control?location[directory]=%2Fworkspace") registered = call["body"]["config"]["environment"]["PRIVATE"] == "value";
			Check(registered, "MCP registration lost captured config");
		});
		Run("V2HistoryNormalizesWithoutInventingCompactionCoverage", []
		{
			const Json result = NormalizeExport({{"data", {{"info", {{"id", "ses_owned"}, {"location", {{"directory", "/workspace"}}}}}, {"messages", Json::array({{{"id", "msg_u"}, {"type", "user"}, {"text", "hello"}}, {{"id", "msg_a"}, {"type", "assistant"}, {"model", {{"providerID", "mock"}, {"id", "model"}}}, {"tokens", {{"input", 4}, {"output", 2}}}, {"content", Json::array({{{"type", "text"}, {"text", "reply"}}})}}, {{"id", "msg_c"}, {"type", "compaction"}, {"status", "completed"}, {"summary", "visible data"}}})}}}});
			Check(result["info"]["directory"] == "/workspace" && result["messages"][1]["info"]["providerID"] == "mock", "V2 history identity lost");
			Check(result["messages"][2]["info"]["role"] == "system" && !result["messages"][2]["info"].contains("summary"), "V2 compaction claimed V1 coverage");
		});
		Run("ImportedNativeToolTextRendersAsTextAndRetainsNonTextFallback", []
		{
			Json message{{"id", "msg_tool"}, {"type", "assistant"}, {"model", {{"providerID", "mock"}, {"id", "model"}}}, {"content", Json::array({{{"type", "tool"}, {"id", "read"}, {"name", "read"}, {"state", {{"status", "completed"}, {"content", Json::array({{{"type", "text"}, {"text", "first"}}, {{"type", "text"}, {"text", "second"}}})}}}}})}};
			const auto normalize = [&] { return NormalizeExport({{"info", {{"id", "ses_owned"}, {"location", {{"directory", "/workspace"}}}}}, {"messages", Json::array({message})}}); };
			Check(normalize()["messages"][0]["parts"][0]["state"]["output"] == "first\nsecond", "tool content was displayed as raw text-part JSON");
			message["content"][0]["state"]["content"].push_back({{"type", "image"}, {"data", "synthetic"}});
			const auto fallback = normalize()["messages"][0]["parts"][0]["state"]["output"].get<std::string>();
			Check(Json::parse(fallback).at(2).at("type") == "image", "non-text native content was discarded");
		});
		return 0;
	}
	catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
