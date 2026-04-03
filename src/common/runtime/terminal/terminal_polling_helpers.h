#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "common/platform/platform_services.h"
#include "common/state/app_state.h"

inline std::string CodepointToUtf8(const std::uint32_t cp)
{
	std::string out;

	if (cp <= 0x7F)
	{
		out.push_back(static_cast<char>(cp));
	}
	else if (cp <= 0x7FF)
	{
		out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
	else if (cp <= 0xFFFF)
	{
		out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
	else
	{
		out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}

	return out;
}

inline void ResizeCliTerminal(uam::CliTerminalState& terminal, const int rows, const int cols)
{
	const int safe_rows = std::max(8, rows);
	const int safe_cols = std::max(20, cols);

	if (terminal.rows == safe_rows && terminal.cols == safe_cols)
	{
		return;
	}

	terminal.rows = safe_rows;
	terminal.cols = safe_cols;

	if (terminal.vt != nullptr)
	{
		vterm_set_size(terminal.vt, terminal.rows, terminal.cols);
	}

	PlatformServicesFactory::Instance().terminal_runtime.ResizeCliTerminal(terminal);
}
