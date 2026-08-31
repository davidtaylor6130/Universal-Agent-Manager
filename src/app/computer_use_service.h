#pragma once

#include "common/state/app_state.h"

#include <string>
#include <string_view>

namespace uam
{
	class ComputerUseService
	{
	  public:
		static bool ResetControlsForStartup(AppState& app);
		static bool Poll(AppState& app);
		static bool SetControlState(AppState& app, const std::string& chat_id, std::string_view state,
		    std::string* error = nullptr);
	};
} // namespace uam
