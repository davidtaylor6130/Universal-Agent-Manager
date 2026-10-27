#pragma once

#include <filesystem>
#include <optional>

namespace uam::platform
{
	/// Creates a fresh owner-private directory atomically in the OS temporary root.
	/// The caller owns bounded contents and scoped removal. Never follows an
	/// existing candidate directory or publishes bytes before protection succeeds.
	std::optional<std::filesystem::path> CreatePrivateHistoryTempDirectory();
} // namespace uam::platform
