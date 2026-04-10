#pragma once

// ---------------------------------------------------------------------------
// VTerm callbacks — replaced by xterm.js (CEF build)
// ---------------------------------------------------------------------------
// libvterm is no longer used.  PTY bytes are forwarded raw to the React
// frontend via uam::PushCliOutput(), where xterm.js handles all VT100
// decoding and rendering.  This file is kept so that translation units that
// once included it continue to compile without modification.
// ---------------------------------------------------------------------------

#include "common/runtime/terminal/terminal_lifecycle.h"

/// Stub: PTY writeback now uses WriteToCliTerminal directly (no vterm routing).
inline void WriteBytesToPty(const char* bytes, const std::size_t len, void* user)
{
	if (user == nullptr || bytes == nullptr || len == 0)
		return;

	auto* terminal = static_cast<uam::CliTerminalState*>(user);
	if (!WriteToCliTerminal(*terminal, bytes, len))
		terminal->last_error = "Failed to write to provider terminal.";
}
