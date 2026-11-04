#pragma once
#include <cstddef>
#include <filesystem>

namespace uam_test
{
	void SetNativeImportFixtureRoot(const std::filesystem::path& root);
	std::size_t NativeImportFixtureUnexpectedCalls();
} // namespace uam_test
