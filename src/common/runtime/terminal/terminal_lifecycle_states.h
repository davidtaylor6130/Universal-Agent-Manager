#pragma once

#include "common/state/app_state.h"
#include "common/utils/range_utils.h"

#include <array>

namespace uam
{

inline constexpr auto kCliTerminalProcessingLifecycleStates = std::to_array<CliTerminalLifecycleState>({
	CliTerminalLifecycleState::Busy,
	CliTerminalLifecycleState::ShuttingDown,
});

inline bool CliTerminalLifecycleStateIsProcessing(const CliTerminalLifecycleState state)
{
	return uam::ranges::Contains(kCliTerminalProcessingLifecycleStates, state);
}

} // namespace uam
