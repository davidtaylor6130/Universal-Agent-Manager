#pragma once

#include <utility>

#include "common/runtime/terminal/terminal_lifecycle.h"

inline void WriteBytesToPty(const char* bytes, const std::size_t len, void* user)
{
	if (user == nullptr || bytes == nullptr || len == 0)
	{
		return;
	}

	auto* terminal = static_cast<uam::CliTerminalState*>(user);

	if (!WriteToCliTerminal(*terminal, bytes, len))
	{
		terminal->last_error = "Failed to write to provider terminal.";
	}
}

inline int OnVTermDamage(VTermRect, void* user)
{
	if (user != nullptr)
	{
		static_cast<uam::CliTerminalState*>(user)->needs_full_refresh = true;
	}

	return 1;
}

inline int OnVTermMoveRect(VTermRect, VTermRect, void* user)
{
	if (user != nullptr)
	{
		static_cast<uam::CliTerminalState*>(user)->needs_full_refresh = true;
	}

	return 1;
}

inline int OnVTermMoveCursor(VTermPos, VTermPos, int, void* user)
{
	if (user != nullptr)
	{
		static_cast<uam::CliTerminalState*>(user)->needs_full_refresh = true;
	}

	return 1;
}

inline int OnVTermResize(int rows, int cols, void* user)
{
	if (user != nullptr)
	{
		auto* terminal = static_cast<uam::CliTerminalState*>(user);
		terminal->rows = rows;
		terminal->cols = cols;
		terminal->scrollback_view_offset = std::clamp(terminal->scrollback_view_offset, 0, static_cast<int>(terminal->scrollback_lines.size()));
		terminal->needs_full_refresh = true;
	}

	return 1;
}

inline VTermScreenCell BlankTerminalCell()
{
	VTermScreenCell cell{};
	cell.width = 1;
	return cell;
}

inline int OnVTermScrollbackPushLine(int cols, const VTermScreenCell* cells, void* user)
{
	if (user == nullptr || cells == nullptr || cols <= 0)
	{
		return 1;
	}

	auto* terminal = static_cast<uam::CliTerminalState*>(user);
	uam::TerminalScrollbackLine line;
	line.cells.assign(cells, cells + cols);
	terminal->scrollback_lines.push_back(std::move(line));

	if (terminal->scrollback_lines.size() > uam::kTerminalScrollbackMaxLines)
	{
		terminal->scrollback_lines.pop_front();
	}

	if (terminal->scrollback_view_offset > 0)
	{
		terminal->scrollback_view_offset = std::min(terminal->scrollback_view_offset + 1, static_cast<int>(terminal->scrollback_lines.size()));
	}

	terminal->needs_full_refresh = true;
	return 1;
}

inline int OnVTermScrollbackPopLine(int cols, VTermScreenCell* cells, void* user)
{
	if (user == nullptr || cells == nullptr || cols <= 0)
	{
		return 0;
	}

	auto* terminal = static_cast<uam::CliTerminalState*>(user);

	if (terminal->scrollback_lines.empty())
	{
		return 0;
	}

	uam::TerminalScrollbackLine line = std::move(terminal->scrollback_lines.back());
	terminal->scrollback_lines.pop_back();

	const int copy_cols = std::min(cols, static_cast<int>(line.cells.size()));

	for (int i = 0; i < copy_cols; ++i)
	{
		cells[i] = line.cells[static_cast<std::size_t>(i)];
	}

	for (int i = copy_cols; i < cols; ++i)
	{
		cells[i] = BlankTerminalCell();
	}

	terminal->scrollback_view_offset = std::clamp(terminal->scrollback_view_offset, 0, static_cast<int>(terminal->scrollback_lines.size()));
	terminal->needs_full_refresh = true;
	return 1;
}

inline int OnVTermScrollbackClear(void* user)
{
	if (user == nullptr)
	{
		return 1;
	}

	auto* terminal = static_cast<uam::CliTerminalState*>(user);
	terminal->scrollback_lines.clear();
	terminal->scrollback_view_offset = 0;
	terminal->needs_full_refresh = true;
	return 1;
}

inline const VTermScreenCallbacks kVTermScreenCallbacks = {OnVTermDamage, OnVTermMoveRect, OnVTermMoveCursor, nullptr, nullptr, OnVTermResize, OnVTermScrollbackPushLine, OnVTermScrollbackPopLine, OnVTermScrollbackClear, nullptr};
