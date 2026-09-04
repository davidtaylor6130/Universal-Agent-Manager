#pragma once

#include "cef/cef_includes.h"
#include "common/state/app_state.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace uam::acp_detail
{

inline constexpr std::size_t kMaxAcpStdoutLineBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kAcpStdoutBytesPerPoll = 256 * 1024;
inline constexpr std::size_t kAcpStdoutLinesPerPoll = 256;
inline constexpr std::size_t kAcpStderrBytesPerPoll = 64 * 1024;
inline constexpr std::uint64_t kAcpOutputFloodBytesPerMinute = 256ull * 1024 * 1024;

bool ProcessAcpLine(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line, CefRefPtr<CefBrowser> browser);
bool AppendAcpStdoutChunk(AcpSessionState& session, std::string_view chunk);
std::size_t ProcessBufferedAcpStdoutForTests(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser, std::size_t max_lines);
bool DrainStdout(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser);
bool DrainStderr(AppState& app, AcpSessionState& session, ChatSession& chat);
void MarkAcpProcessExited(AcpSessionState& session, ChatSession* chat = nullptr,
	                      bool has_exit_code = false, int exit_code = 0,
	                      bool preserve_active_turn = false);

} // namespace uam::acp_detail
