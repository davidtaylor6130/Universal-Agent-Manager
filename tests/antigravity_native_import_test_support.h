#pragma once

// Small non-process runner for the exact same parser fixtures used by core tests.
// It deliberately has no provider/runtime/platform-service test doubles.
#include "common/utils/time_utils.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
namespace uam_test
{
	struct FixtureCase
	{
		std::string name;
		std::function<void()> run;
	};
	inline std::vector<FixtureCase>& Registry()
	{
		static std::vector<FixtureCase> cases;
		return cases;
	}
	struct RegisterTest
	{
		RegisterTest(const char* name, std::function<void()> run)
		{
			Registry().push_back({name, std::move(run)});
		}
	};
	inline void Assert(bool result, const char* expression, int line)
	{
		if (!result)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
	struct TempDir
	{
		fs::path root;
		explicit TempDir(const std::string& prefix)
		{
			root = fs::temp_directory_path() / (prefix + '-' + uam::time::SteadyEpochNanosecondsTokenNow());
			fs::create_directories(root);
		}
		~TempDir()
		{
			std::error_code ignored;
			fs::remove_all(root, ignored);
		}
	};
	inline std::string ReadFile(const fs::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		std::ostringstream bytes;
		bytes << input.rdbuf();
		return bytes.str();
	}
} // namespace uam_test
#define UAM_ASSERT(value) uam_test::Assert(static_cast<bool>(value), #value, __LINE__)
#define UAM_ASSERT_EQ(value, expected) uam_test::Assert((value) == (expected), #value " == " #expected, __LINE__)
#define UAM_TEST(name) \
	static void name(); \
	static uam_test::RegisterTest register_##name(#name, name); \
	static void name()
