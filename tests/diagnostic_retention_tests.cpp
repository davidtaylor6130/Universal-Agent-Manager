#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/stage_timing_summary.h"
#include "common/utils/time_utils.h"

using namespace uam_test;

namespace
{
	std::string StampDaysAgo(std::chrono::system_clock::time_point now, int days)
	{
		const std::time_t when = std::chrono::system_clock::to_time_t(now - std::chrono::hours(24) * days);
		return uam::time::detail::FormatTimestamp(uam::time::detail::UtcTimeSnapshot(when), uam::time::kIsoUtcTimestampFormat);
	}

	std::vector<std::chrono::milliseconds> Stages(int first, int second)
	{
		return {std::chrono::milliseconds(first), std::chrono::milliseconds(second)};
	}
}

UAM_TEST(DiagnosticAuditLogExpiresByAgeAndKeepsDeletionLines)
{
	TempDir temp("uam-audit-age");
	const auto now = std::chrono::system_clock::now();
	const fs::path audit = temp.root / "uam-audit.log";
	UAM_ASSERT(uam::io::WriteTextFile(audit,
	    StampDaysAgo(now, 200) + " Chat storage delete: reason=user_deletion, chat_id=expired-chat, primary=removed, backup=removed, result=ok\n" +
	    StampDaysAgo(now, 10) + " Chat storage delete: reason=user_deletion, chat_id=recent-chat, primary=removed, backup=absent, result=ok\n" +
	    "torn fragment without a stamp"));
	std::string error;
	UAM_ASSERT(uam::diagnostics::StartFileLog(temp.root / "uam.log", error, 0));
	struct StopLog { ~StopLog() { uam::diagnostics::StopFileLog(); } } stop_log; // A failed assertion must not leak the global log.
	UAM_ASSERT(uam::diagnostics::StartAuditLog(audit, error, 90));
	UAM_ASSERT(!uam::diagnostics::StartAuditLog(audit, error, 90)); // One audit owner per file log.
	uam::diagnostics::WriteAudit("Chat storage delete: reason=create_rollback, chat_id=new-chat, primary=removed, backup=absent, result=ok");
	for (int line = 0; line < 50; ++line) uam::diagnostics::Write("[performance] ordinary noise " + std::to_string(line));
	uam::diagnostics::FlushFileLog();
	const std::string open_text = uam::io::ReadTextFile(audit);
	UAM_ASSERT(!uam::strings::Contains(open_text, "expired-chat"));
	UAM_ASSERT(!uam::strings::Contains(open_text, "torn fragment"));
	UAM_ASSERT(uam::strings::Contains(open_text, "recent-chat"));
	UAM_ASSERT(uam::strings::Contains(open_text, "Audit log opened: retention_days=90"));
	UAM_ASSERT(uam::strings::Contains(open_text, "chat_id=new-chat"));
	UAM_ASSERT(!uam::strings::Contains(open_text, "ordinary noise"));
	uam::diagnostics::StopFileLog();
	const std::string closed_text = uam::io::ReadTextFile(audit);
	UAM_ASSERT(closed_text.find("chat_id=new-chat") < closed_text.find("Audit log closed"));
	// The ordinary log keeps the same line beside its surrounding context.
	UAM_ASSERT(uam::strings::Contains(uam::io::ReadTextFile(temp.root / "uam.log"), "chat_id=new-chat"));
	UAM_ASSERT(!fs::exists(temp.root / "uam-audit.log.tmp"));
}

UAM_TEST(DiagnosticAuditLogRefusesUnsafePathsAndNeedsFileLog)
{
	TempDir temp("uam-audit-path");
	std::string error;
	UAM_ASSERT(!uam::diagnostics::StartAuditLog(temp.root / "uam-audit.log", error));
	UAM_ASSERT(!error.empty());
	uam::diagnostics::WriteAudit("Chat storage delete: reason=user_deletion, chat_id=no-audit-file"); // stderr only, no crash.
	UAM_ASSERT(uam::diagnostics::StartFileLog(temp.root / "uam.log", error, 0));
	struct StopLog { ~StopLog() { uam::diagnostics::StopFileLog(); } } stop_log;
	fs::create_directories(temp.root / "audit-dir");
	UAM_ASSERT(!uam::diagnostics::StartAuditLog(temp.root / "audit-dir", error));
	UAM_ASSERT(!uam::diagnostics::StartAuditLog(temp.root / "uam-audit.log", error, 0));
	uam::diagnostics::WriteAudit("Chat storage delete: reason=user_deletion, chat_id=diagnostic-only");
	uam::diagnostics::StopFileLog();
	UAM_ASSERT(!fs::exists(temp.root / "uam-audit.log"));
	UAM_ASSERT(uam::strings::Contains(uam::io::ReadTextFile(temp.root / "uam.log"), "chat_id=diagnostic-only"));
}

UAM_TEST(DiagnosticAuditTextCompactionDropsOldestToFitCap)
{
	const auto now = std::chrono::system_clock::now();
	std::string text;
	for (int day = 120; day >= 0; day -= 10) text += StampDaysAgo(now, day) + " audit line " + std::to_string(day) + "\n";
	const std::string aged = uam::diagnostics::CompactAuditText(text, now, 30, 1024 * 1024);
	UAM_ASSERT(!uam::strings::Contains(aged, "audit line 40\n"));
	UAM_ASSERT(uam::strings::Contains(aged, "audit line 30\n") || uam::strings::Contains(aged, "audit line 20\n"));
	UAM_ASSERT(uam::strings::Contains(aged, "audit line 0\n"));
	const std::size_t line_bytes = (StampDaysAgo(now, 0) + " audit line 0\n").size();
	const std::string capped = uam::diagnostics::CompactAuditText(text, now, 365, line_bytes * 2 + 4);
	UAM_ASSERT(capped.size() <= line_bytes * 2 + 4);
	UAM_ASSERT(uam::strings::Contains(capped, "audit line 0\n") && uam::strings::Contains(capped, "audit line 10\n"));
	UAM_ASSERT(!uam::strings::Contains(capped, "audit line 20\n"));
}

UAM_TEST(PollTimingSummaryReplacesPerTickLinesWithWindowPercentiles)
{
	using Clock = uam::diagnostics::StageTimingSummary::Clock;
	uam::diagnostics::StageTimingSummaryOptions options;
	options.window = std::chrono::seconds(300);
	uam::diagnostics::StageTimingSummary summary("PollTick", {"runtimes", "push"}, options);
	Clock::time_point now = Clock::now();
	std::vector<std::string> lines;
	const auto record = [&](int runtimes, int push)
	{
		for (std::string& line : summary.Record(now, std::chrono::milliseconds(runtimes + push), Stages(runtimes, push))) lines.push_back(std::move(line));
		now += std::chrono::seconds(1);
	};
	// The old writer logged a line for any tick of 50 ms or more every 5 s; these stay silent until the window ends.
	for (int tick = 0; tick < 100; ++tick) record(tick % 5 == 0 ? 70 : 3, 1);
	UAM_ASSERT(lines.empty());
	record(290, 20); // A tick past the individual threshold is still written on its own.
	UAM_ASSERT_EQ(lines.size(), std::size_t{1});
	UAM_ASSERT(uam::strings::Contains(lines[0], "[performance] PollTick took 310 ms; stages=runtimes:290,push:20 ms."));
	record(400, 0); // ...at most once per interval.
	UAM_ASSERT_EQ(lines.size(), std::size_t{1});
	for (int tick = 0; tick < 200; ++tick) record(3, 1);
	UAM_ASSERT_EQ(lines.size(), std::size_t{2});
	UAM_ASSERT(uam::strings::Contains(lines[1], "[performance] PollTick summary over 300 s: samples=300 slow(>=50ms)=22;"));
	UAM_ASSERT(uam::strings::Contains(lines[1], "total=4/80/400"));
	UAM_ASSERT(uam::strings::Contains(lines[1], "runtimes:3/80/400"));
	UAM_ASSERT(uam::strings::Contains(lines[1], "push:1/1/20"));
	// A window without one slow tick writes nothing at all.
	for (int tick = 0; tick < 400; ++tick) record(3, 0);
	UAM_ASSERT_EQ(lines.size(), std::size_t{2});
	UAM_ASSERT(!summary.Flush(now).has_value());
}

UAM_TEST(ModelDiscoveryAcpDiagnosticsKeepFailuresOnly)
{
	uam::AcpSessionState session;
	uam::AcpDiagnosticEntryState sent;
	sent.event = "write";
	sent.reason = "sent";
	uam::AcpDiagnosticEntryState result;
	result.event = "response";
	result.reason = "jsonrpc_result";
	uam::AcpDiagnosticEntryState failure;
	failure.event = "response";
	failure.reason = "jsonrpc_error";
	uam::AcpDiagnosticEntryState crashed;
	crashed.event = "process_exit";
	crashed.reason = "idle";
	crashed.has_code = true;
	crashed.code = 1;
	UAM_ASSERT(uam::acp_detail::ShouldWriteAcpDiagnosticLogLine(session, sent));
	UAM_ASSERT(uam::acp_detail::ShouldWriteAcpDiagnosticLogLine(session, result));
	for (const bool ephemeral : {false, true})
	{
		session.model_discovery_only = !ephemeral;
		session.ephemeral_model_discovery = ephemeral;
		UAM_ASSERT(!uam::acp_detail::ShouldWriteAcpDiagnosticLogLine(session, sent));
		UAM_ASSERT(!uam::acp_detail::ShouldWriteAcpDiagnosticLogLine(session, result));
		UAM_ASSERT(uam::acp_detail::ShouldWriteAcpDiagnosticLogLine(session, failure));
		UAM_ASSERT(uam::acp_detail::ShouldWriteAcpDiagnosticLogLine(session, crashed));
	}
}
