#pragma once

#include "common/platform/terminal_unix.h"
#include "common/platform/terminal_windows.h"

/// <summary>
/// Platform startup dispatch for embedded provider terminal processes.
/// </summary>
static bool StartCliTerminalPlatform(AppState& app, CliTerminalState& terminal, const ChatSession& chat)
{
#if defined(_WIN32)
	return StartCliTerminalWindows(app, terminal, chat);
#else
	return StartCliTerminalUnix(app, terminal, chat);
#endif
}
