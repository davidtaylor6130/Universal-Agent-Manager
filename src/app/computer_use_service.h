#pragma once

#include "common/state/app_state.h"

#include <future>
#include <memory>
#include <string>
#include <string_view>

namespace uam
{
	class ComputerUseService
	{
	  public:
		static bool ResetControlsForStartup(AppState& app);
		static bool Poll(AppState& app, std::string_view chat_id = {});
		/// <summary>Scans control files off-thread and adopts only unchanged chat/control identities.</summary>
		bool PollInBackground(AppState& app);
		static bool SetControlState(AppState& app, const std::string& chat_id, std::string_view state,
		    std::string* error = nullptr);
		static bool PersistTrustedTask(AppState& app, const std::string& chat_id,
		    std::string_view prompt, std::string* error = nullptr);
	  private:
		struct Scan;
		std::future<std::shared_ptr<Scan>> m_scan;
		double m_nextScanTime = 0.0;
	};
} // namespace uam
