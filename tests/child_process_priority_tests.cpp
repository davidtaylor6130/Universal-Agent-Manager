#include "test_harness.h"
#include "common/platform/child_process_priority.h"
#include "common/platform/platform_services.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/utils/base64.h"
#include "remote/runner_proxy.h"
#include "remote/runner_state.h"

#include <chrono>
#include <fstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace uam_test;
using uam::platform::ChildProcessClass;

namespace
{
#if !defined(_WIN32)
	/// The niceness a started shell reports for itself, written to a file so no output protocol is involved.
	int ReportedNiceness(const fs::path& report)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		std::string text;
		while (std::chrono::steady_clock::now() < deadline)
		{
			std::ifstream input(report);
			if (input && std::getline(input, text) && text.find_first_not_of(" \t") != std::string::npos) break;
			text.clear();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		UAM_ASSERT(!text.empty());
		return std::stoi(text);
	}

	std::vector<std::string> NicenessReporter(const fs::path& report)
	{
		// Linux reads its own /proc entry (field 19 is the niceness), so a host without ps still runs the check.
		return {"/bin/sh", "-c", "if [ -r /proc/$$/stat ]; then cut -d' ' -f19 /proc/$$/stat; else ps -o ni= -p $$; fi > \"$0.tmp\" && mv \"$0.tmp\" \"$0\"", uam::paths::Utf8PathString(report)};
	}
#endif
}

UAM_TEST(ChildProcessClassesMapToTheirSpawnOptions)
{
	using uam::platform::SpawnPriorityFor;
	// A user turn keeps every platform default.
	UAM_ASSERT(!SpawnPriorityFor(ChildProcessClass::UserTurn).Lowered());
	UAM_ASSERT_EQ(SpawnPriorityFor(ChildProcessClass::UserTurn), uam::platform::ChildProcessSpawnPriority{});
	// Agent and background work: utility QoS on macOS, nice 10 on Linux and macOS, below normal on Windows.
	for (const ChildProcessClass lowered : {ChildProcessClass::Agent, ChildProcessClass::Background})
	{
		const uam::platform::ChildProcessSpawnPriority priority = SpawnPriorityFor(lowered);
		UAM_ASSERT(priority.Lowered());
		UAM_ASSERT(priority.macos_utility_qos);
		UAM_ASSERT_EQ(priority.unix_nice, 10);
		UAM_ASSERT(priority.windows_below_normal);
	}
	for (const ChildProcessClass value : {ChildProcessClass::UserTurn, ChildProcessClass::Agent, ChildProcessClass::Background})
		UAM_ASSERT_EQ(uam::platform::ParseChildProcessClass(uam::platform::ChildProcessClassName(value)), value);
	// A name a helper does not know never fails or raises a start; it keeps normal priority.
	UAM_ASSERT_EQ(uam::platform::ParseChildProcessClass("realtime"), ChildProcessClass::UserTurn);
	UAM_ASSERT_EQ(uam::platform::ParseChildProcessClass(""), ChildProcessClass::UserTurn);
}

UAM_TEST(ChildProcessClassScopesOnlyLowerAndBelongToTheirThread)
{
	using uam::platform::CurrentChildProcessClass;
	using uam::platform::ScopedChildProcessClass;
	UAM_ASSERT_EQ(CurrentChildProcessClass(), ChildProcessClass::UserTurn);
	{
		const ScopedChildProcessClass agent(ChildProcessClass::Agent);
		UAM_ASSERT_EQ(CurrentChildProcessClass(), ChildProcessClass::Agent);
		{
			// A nested user-turn scope cannot raise what an agent starts.
			const ScopedChildProcessClass user(ChildProcessClass::UserTurn);
			UAM_ASSERT_EQ(CurrentChildProcessClass(), ChildProcessClass::Agent);
		}
		{
			const ScopedChildProcessClass background(ChildProcessClass::Background);
			UAM_ASSERT_EQ(CurrentChildProcessClass(), ChildProcessClass::Background);
		}
		UAM_ASSERT_EQ(CurrentChildProcessClass(), ChildProcessClass::Agent);
		// Another thread, such as the poll thread launching a user's turn, is not lowered by this scope.
		ChildProcessClass other_thread = ChildProcessClass::Agent;
		std::thread([&other_thread] { other_thread = CurrentChildProcessClass(); }).join();
		UAM_ASSERT_EQ(other_thread, ChildProcessClass::UserTurn);
	}
	UAM_ASSERT_EQ(CurrentChildProcessClass(), ChildProcessClass::UserTurn);
}

UAM_TEST(ProviderProcessesAreClassedByTheWorkTheyServe)
{
	const auto classify = [](const auto& change)
	{
		uam::AcpSessionState session;
		ChatSession chat;
		change(session, chat);
		return uam::acp_detail::ProviderProcessClass(session, chat);
	};
	UAM_ASSERT_EQ(classify([](uam::AcpSessionState&, ChatSession&) {}), ChildProcessClass::UserTurn);
	UAM_ASSERT_EQ(classify([](uam::AcpSessionState& session, ChatSession&) { session.model_discovery_only = true; }), ChildProcessClass::Background);
	UAM_ASSERT_EQ(classify([](uam::AcpSessionState& session, ChatSession&) { session.ephemeral_model_discovery = true; }), ChildProcessClass::Background);
	UAM_ASSERT_EQ(classify([](uam::AcpSessionState& session, ChatSession&) { session.managed_agent_run_id = "run"; }), ChildProcessClass::Agent);
	UAM_ASSERT_EQ(classify([](uam::AcpSessionState&, ChatSession& chat) { chat.agent_run_id = "run"; }), ChildProcessClass::Agent);
	UAM_ASSERT_EQ(classify([](uam::AcpSessionState&, ChatSession& chat) { chat.goal_owner_chat_id = "goal-parent"; }), ChildProcessClass::Agent);
	UAM_ASSERT_EQ(classify([](uam::AcpSessionState&, ChatSession& chat) { chat.automation_token_id = "token"; }), ChildProcessClass::Agent);
	// A chat that merely has a goal is still the user's own chat.
	UAM_ASSERT_EQ(classify([](uam::AcpSessionState&, ChatSession& chat) { chat.active_goal_id = "goal"; }), ChildProcessClass::UserTurn);
}

UAM_TEST(RemoteProxySpecCarriesOnlyALoweredProcessClass)
{
	const auto decoded = []
	{
		std::string json;
		UAM_ASSERT(uam::base64::Decode(uam::remote::BuildProcessProxySpec("acp-chat", fs::path("/workspace"), {"codex", "app-server"}, {}), json));
		return nlohmann::json::parse(json);
	};
	// A user turn sends nothing, so an older helper sees an unchanged request.
	UAM_ASSERT(!decoded().contains("processClass"));
	const uam::platform::ScopedChildProcessClass agent(ChildProcessClass::Agent);
	UAM_ASSERT_EQ(decoded().value("processClass", ""), std::string("agent"));
}

UAM_TEST(SpawnedChildrenStartAtTheirClassPriority)
{
	TempDir temp("uam-child-process-priority");
	const IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
#if defined(_WIN32)
	const auto priority_class = [&](ChildProcessClass process_class)
	{
		uam::platform::StdioProcessPlatformFields process;
		std::string error;
		{
			const uam::platform::ScopedChildProcessClass scope(process_class);
			UAM_ASSERT(service.StartStdioProcess(process, {}, {"ping.exe", "-n", "5", "127.0.0.1"}, &error));
		}
		const DWORD value = GetPriorityClass(process.process_info.hProcess);
		service.StopStdioProcess(process, true);
		return value;
	};
	UAM_ASSERT_EQ(uam::platform::CurrentChildProcessClass(), ChildProcessClass::UserTurn);
	// A user turn passes no class flag, so it gets Windows' default: normal, or this process's own class when that
	// is below normal or idle (a lowered CI runner), exactly as any child of this process would.
	const DWORD own = GetPriorityClass(GetCurrentProcess());
	const DWORD user_default = own == BELOW_NORMAL_PRIORITY_CLASS || own == IDLE_PRIORITY_CLASS ? own : static_cast<DWORD>(NORMAL_PRIORITY_CLASS);
	UAM_ASSERT_EQ(priority_class(ChildProcessClass::UserTurn), user_default);
	UAM_ASSERT_EQ(priority_class(ChildProcessClass::Agent), static_cast<DWORD>(BELOW_NORMAL_PRIORITY_CLASS));
	UAM_ASSERT_EQ(priority_class(ChildProcessClass::Background), static_cast<DWORD>(BELOW_NORMAL_PRIORITY_CLASS));
#else
	const auto niceness = [&](ChildProcessClass process_class, const std::string& name)
	{
		const fs::path report = temp.root / name;
		uam::platform::StdioProcessPlatformFields process;
		std::string error;
		{
			const uam::platform::ScopedChildProcessClass scope(process_class);
			UAM_ASSERT(service.StartStdioProcess(process, temp.root, NicenessReporter(report), &error));
		}
		const int value = ReportedNiceness(report);
		service.StopStdioProcess(process, true);
		return value;
	};
	const int own = niceness(ChildProcessClass::UserTurn, "user");
	// A lowered child never runs above nice 10, and never above this process either.
	UAM_ASSERT_EQ(niceness(ChildProcessClass::Agent, "agent"), std::max(own, 10));
	UAM_ASSERT_EQ(niceness(ChildProcessClass::Background, "background"), std::max(own, 10));
#endif
}

#if !defined(_WIN32)
UAM_TEST(RunnerStartsARequestedProcessClassAndIgnoresAnUnknownOne)
{
	TempDir temp("uam-runner-process-class");
	uam::remote::RunnerState state;
	const auto start = [&](const std::string& session, const nlohmann::json& process_class)
	{
		const fs::path report = temp.root / session;
		nlohmann::json request{{"id", session}, {"type", "process.start"}, {"sessionId", session}, {"controlToken", "owner-" + session},
		    {"cwd", uam::paths::Utf8PathString(temp.root)}, {"argv", NicenessReporter(report)}, {"environment", nlohmann::json::object()}};
		if (!process_class.is_null()) request["processClass"] = process_class;
		UAM_ASSERT(state.HandleProcessRequest(request).value("ok", false));
		return ReportedNiceness(report);
	};
	const int own = start("user-turn", nullptr);
	UAM_ASSERT_EQ(start("agent-work", "agent"), std::max(own, 10));
	UAM_ASSERT_EQ(start("background-work", "background"), std::max(own, 10));
	// A class this helper does not know, or a malformed one, starts normally rather than failing.
	UAM_ASSERT_EQ(start("unknown-class", "realtime"), own);
	UAM_ASSERT_EQ(start("malformed-class", 7), own);
}
#endif
