#pragma once

#include "common/platform/terminal_mac.h"
#include "common/platform/terminal_windows.h"

/// <summary>
/// Platform startup dispatch for embedded provider terminal processes.
/// </summary>
static bool StartCliTerminalPlatform(AppState& app, CliTerminalState& terminal, const ChatSession& chat)
{
#if defined(_WIN32)
	return StartCliTerminalWindows(app, terminal, chat);
#elif defined(__APPLE__)
	return StartCliTerminalMac(app, terminal, chat);
#else
#error "StartCliTerminalPlatform is only supported on Windows and macOS."
#endif
}
