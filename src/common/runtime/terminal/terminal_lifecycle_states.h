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

/// <summary>How long a submission made while no native turn is tracked waits for the provider to record a turn start.</summary>
inline constexpr double kCliTerminalTurnReceiptSeconds = 10.0;

inline bool CliTerminalLifecycleStateIsProcessing(const CliTerminalLifecycleState state)
{
	return uam::ranges::Contains(kCliTerminalProcessingLifecycleStates, state);
}

} // namespace uam
