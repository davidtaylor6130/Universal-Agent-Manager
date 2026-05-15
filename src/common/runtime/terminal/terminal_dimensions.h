#pragma once

#include <algorithm>

namespace uam
{
	inline constexpr int kCliTerminalDefaultRows = 24;
	inline constexpr int kCliTerminalDefaultCols = 80;
	inline constexpr int kCliTerminalMinimumResizeRows = 1;
	inline constexpr int kCliTerminalMinimumResizeCols = 1;
	inline constexpr int kCliTerminalMinimumLaunchRows = 8;
	inline constexpr int kCliTerminalMinimumLaunchCols = 20;

	inline int ClampCliTerminalDimension(int minimum, int value)
	{
		return std::max(minimum, value);
	}

	inline int ClampCliTerminalResizeRows(int rows)
	{
		return ClampCliTerminalDimension(kCliTerminalMinimumResizeRows, rows);
	}

	inline int ClampCliTerminalResizeCols(int cols)
	{
		return ClampCliTerminalDimension(kCliTerminalMinimumResizeCols, cols);
	}

	inline int ClampCliTerminalLaunchRows(int rows)
	{
		return ClampCliTerminalDimension(kCliTerminalMinimumLaunchRows, rows);
	}

	inline int ClampCliTerminalLaunchCols(int cols)
	{
		return ClampCliTerminalDimension(kCliTerminalMinimumLaunchCols, cols);
	}
} // namespace uam
