#pragma once

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace uam::cef
{
	/// Paths of the files in the most recent drag into the window. The renderer's File objects carry no path, so the
	/// composer asks for these on drop and stages each file from disk instead of reading it into memory.
	inline std::vector<std::string> ExchangeDroppedAttachmentPaths(std::vector<std::string> paths = {})
	{
		static std::mutex mutex;
		static std::vector<std::string> remembered;
		const std::scoped_lock lock(mutex);
		return std::exchange(remembered, std::move(paths));
	}
} // namespace uam::cef
