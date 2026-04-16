#pragma once

// ---------------------------------------------------------------------------
// Legacy terminal callback shim for the CEF build.
// ---------------------------------------------------------------------------
// PTY bytes are forwarded raw to the React frontend via uam::PushCliOutput(),
// where xterm.js handles terminal decoding and rendering.
// ---------------------------------------------------------------------------

#include "common/runtime/terminal/terminal_lifecycle.h"

/// Stub: PTY writeback now uses WriteToCliTerminal directly.
inline void WriteBytesToPty(const char* bytes, const std::size_t len, void* user)
{
	if (user == nullptr || bytes == nullptr || len == 0)
		return;

	auto* terminal = static_cast<uam::CliTerminalState*>(user);
	if (!WriteToCliTerminal(*terminal, bytes, len))
		terminal->last_error = "Failed to write to provider terminal.";
}
