#pragma once

#include <string>
#include <unordered_set>
#include <utility>

namespace uam::query_handler_async
{
	/// Runs on the UI thread; the dispatcher owns its failure response and existing request lease.
	template <typename Dispatch>
	bool DispatchBranchOperation(std::unordered_set<std::string>& operations, const std::string& chat_id, Dispatch&& dispatch)
	{
		if (std::forward<Dispatch>(dispatch)()) return true;
		// RunAsyncCefQuery already sent its one failure response; its completion will not run.
		operations.erase(chat_id);
		return false;
	}
}
