#include "app/chat_domain_service.h"
#include "app/chat_lifecycle_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/chat_repository.h"
#include "common/config/settings_store.h"
#include "common/constants/app_constants.h"
#include "common/paths/app_paths.h"
#include "common/platform/platform_services.h"
#include "common/provider/codex/cli/codex_session_index.h"
#include "common/provider/gemini/base/gemini_history_loader.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/terminal/terminal_idle_classifier.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/utils/io_utils.h"
#include "cef/uam_bridge_request.h"
#include "cef/state_serializer.h"
#include "core/gemini_cli_compat.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <cerrno>
#include <csignal>
#endif

namespace fs = std::filesystem;

namespace
{
	struct TestCase
	{
		std::string name;
		std::function<void()> fn;
	};

	std::vector<TestCase>& Registry()
	{
		static std::vector<TestCase> tests;
		return tests;
	}

	struct RegisterTest
	{
		RegisterTest(std::string name, std::function<void()> fn)
		{
			Registry().push_back({std::move(name), std::move(fn)});
		}
	};

	struct TestFailure : std::runtime_error
	{
		using std::runtime_error::runtime_error;
	};

	void Fail(const char* expr, const char* file, int line)
	{
		std::ostringstream out;
		out << file << ':' << line << ": assertion failed: " << expr;
		throw TestFailure(out.str());
	}

	template <typename T, typename U> void AssertEq(const T& actual, const U& expected, const char* actual_expr, const char* expected_expr, const char* file, int line)
	{
		if (!(actual == expected))
		{
			std::ostringstream out;
			out << file << ':' << line << ": expected " << actual_expr << " == " << expected_expr;
			throw TestFailure(out.str());
		}
	}

	struct TempDir
	{
		fs::path root;

		explicit TempDir(const std::string& prefix)
		{
			const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
			root = fs::temp_directory_path() / (prefix + "-" + std::to_string(ticks));
			fs::create_directories(root);
		}

		~TempDir()
		{
			std::error_code ec;
			fs::remove_all(root, ec);
		}
	};

	std::string ReadFile(const fs::path& path)
	{
		return uam::io::ReadTextFile(path);
	}

	struct ScopedEnvVar
	{
		std::string name;
		std::string previous;
		bool had_previous = false;

		ScopedEnvVar(std::string key, const std::string& value) : name(std::move(key))
		{
			if (const char* existing = std::getenv(name.c_str()); existing != nullptr)
			{
				had_previous = true;
				previous = existing;
			}

#if defined(_WIN32)
			_putenv_s(name.c_str(), value.c_str());
#else
			setenv(name.c_str(), value.c_str(), 1);
#endif
		}

		~ScopedEnvVar()
		{
#if defined(_WIN32)
			_putenv_s(name.c_str(), had_previous ? previous.c_str() : "");
#else
			if (had_previous)
			{
				setenv(name.c_str(), previous.c_str(), 1);
			}
			else
			{
				unsetenv(name.c_str());
			}
#endif
		}
	};

#if !defined(_WIN32)
	struct ScopedDirectoryNoWrite
	{
		fs::path path;

		explicit ScopedDirectoryNoWrite(fs::path target) : path(std::move(target))
		{
			std::error_code ec;
			fs::permissions(path, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace, ec);
		}

		~ScopedDirectoryNoWrite()
		{
			std::error_code ec;
			fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, ec);
		}
	};
#endif
} // namespace

#define UAM_TEST(name)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         \
	static void name();                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        \
	static RegisterTest register_##name(#name, name);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          \
	static void name()

#define UAM_ASSERT(expr)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       \
	do                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         \
	{                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          \
		if (!(expr))                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           \
			Fail(#expr, __FILE__, __LINE__);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   \
	} while (false)

#define UAM_ASSERT_EQ(actual, expected)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        \
	do                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         \
	{                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          \
		AssertEq((actual), (expected), #actual, #expected, __FILE__, __LINE__);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                \
	} while (false)

UAM_TEST(SettingsStoreLoadsLegacyButWritesReleaseSliceOnly)
{
	TempDir temp("uam-settings");
	const fs::path settings_file = temp.root / "settings.txt";
	UAM_ASSERT(uam::io::WriteTextFile(settings_file, "active_provider_id=codex-cli\n"
	                                                 "gemini_command_template=gemini -p {prompt}\n"
	                                                 "gemini_yolo_mode=1\n"
	                                                 "gemini_extra_flags=--approval-mode yolo\n"
	                                                 "selected_model_id=legacy-model.gguf\n"
	                                                 "vector_db_backend=ollama-engine\n"
	                                                 "prompt_profile_root_path=/tmp/templates\n"
	                                                 "rag_enabled=1\n"
	                                                 "cli_idle_timeout_seconds=9999\n"
	                                                 "ui_theme=system\n"
	                                                 "last_selected_chat_id=chat-1\n"));

	AppSettings settings;
	CenterViewMode mode = CenterViewMode::CliConsole;
	SettingsStore::Load(settings_file, settings, mode);

	UAM_ASSERT_EQ(settings.active_provider_id, std::string("codex-cli"));
	UAM_ASSERT_EQ(settings.provider_command_template, std::string("gemini -p {prompt}"));
	UAM_ASSERT_EQ(settings.provider_yolo_mode, true);
	UAM_ASSERT_EQ(settings.provider_extra_flags, std::string("--approval-mode yolo"));
	UAM_ASSERT_EQ(settings.cli_idle_timeout_seconds, 3600);
	UAM_ASSERT_EQ(settings.ui_theme, std::string("system"));
	UAM_ASSERT_EQ(mode, CenterViewMode::CliConsole);

	UAM_ASSERT(SettingsStore::Save(settings_file, settings, mode));
	const std::string saved = ReadFile(settings_file);
	UAM_ASSERT(saved.find("active_provider_id=codex-cli") != std::string::npos);
	UAM_ASSERT(saved.find("provider_command_template=gemini -p {prompt}") != std::string::npos);
	UAM_ASSERT(saved.find("rag_") == std::string::npos);
	UAM_ASSERT(saved.find("selected_model_id") == std::string::npos);
	UAM_ASSERT(saved.find("vector_db_backend") == std::string::npos);
	UAM_ASSERT(saved.find("prompt_profile") == std::string::npos);
}

UAM_TEST(ChatRepositoryToleratesLegacyFieldsAndDropsThemOnWrite)
{
	TempDir temp("uam-chats");
	const fs::path chats_dir = temp.root / "chats";
	fs::create_directories(chats_dir);
	const fs::path legacy_file = chats_dir / "legacy-chat.json";

	UAM_ASSERT(uam::io::WriteTextFile(legacy_file, R"({
  "id": "legacy-chat",
  "provider_id": "codex-cli",
  "model_id": "flash",
  "native_session_id": "6a6f0f3b-1a0b-4a9c-8a01-111111111111",
  "folder_id": "folder-a",
  "template_override_id": "removed-template.md",
  "prompt_profile_bootstrapped": true,
  "rag_enabled": false,
  "rag_source_directories": ["/tmp/a"],
  "title": "Legacy",
  "created_at": "2026-01-01 00:00:00",
  "updated_at": "2026-01-01 00:00:01",
  "messages": [
    {"role": "user", "content": "hello", "created_at": "2026-01-01 00:00:00"}
  ]
})"));

	std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().id, std::string("legacy-chat"));
	UAM_ASSERT_EQ(loaded.front().native_session_id, std::string("6a6f0f3b-1a0b-4a9c-8a01-111111111111"));
	UAM_ASSERT_EQ(loaded.front().model_id, std::string("flash"));
	UAM_ASSERT_EQ(loaded.front().messages.size(), static_cast<std::size_t>(1));

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, loaded.front()));
	const std::string rewritten = ReadFile(legacy_file);
	const nlohmann::json rewritten_json = nlohmann::json::parse(rewritten);
	UAM_ASSERT_EQ(rewritten_json.value("model_id", ""), std::string("flash"));
	UAM_ASSERT(rewritten.find("template_override_id") == std::string::npos);
	UAM_ASSERT(rewritten.find("prompt_profile_bootstrapped") == std::string::npos);
	UAM_ASSERT(rewritten.find("rag_enabled") == std::string::npos);
	UAM_ASSERT(rewritten.find("rag_source_directories") == std::string::npos);
}

UAM_TEST(ChatRepositoryDoesNotSynthesizeInvalidCodexNativeIds)
{
	TempDir temp("uam-codex-native-normalize");
	const fs::path chats_dir = temp.root / "chats";
	fs::create_directories(chats_dir);

	UAM_ASSERT(uam::io::WriteTextFile(chats_dir / "codex-missing.json", R"({
  "id": "chat-codex-missing",
  "provider_id": "codex-cli",
  "title": "Codex Missing",
  "created_at": "2026-01-01T00:00:00.000Z",
  "updated_at": "2026-01-01T00:00:00.000Z",
  "messages": []
})"));
	UAM_ASSERT(uam::io::WriteTextFile(chats_dir / "codex-invalid.json", R"({
  "id": "chat-codex-invalid",
  "provider_id": "codex-cli",
  "native_session_id": "chat-codex-invalid",
  "title": "Codex Invalid",
  "created_at": "2026-01-01T00:00:00.000Z",
  "updated_at": "2026-01-01T00:00:00.000Z",
  "messages": []
})"));
	UAM_ASSERT(uam::io::WriteTextFile(chats_dir / "gemini-missing.json", R"({
  "id": "gemini-missing",
  "provider_id": "gemini-cli",
  "title": "Gemini Missing",
  "created_at": "2026-01-01T00:00:00.000Z",
  "updated_at": "2026-01-01T00:00:00.000Z",
  "messages": []
})"));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	auto find_chat = [&](const std::string& id) -> const ChatSession*
	{
		for (const ChatSession& chat : loaded)
		{
			if (chat.id == id)
			{
				return &chat;
			}
		}
		return nullptr;
	};

	const ChatSession* codex_missing = find_chat("chat-codex-missing");
	const ChatSession* codex_invalid = find_chat("chat-codex-invalid");
	const ChatSession* gemini_missing = find_chat("gemini-missing");
	UAM_ASSERT(codex_missing != nullptr);
	UAM_ASSERT(codex_invalid != nullptr);
	UAM_ASSERT(gemini_missing != nullptr);
	UAM_ASSERT_EQ(codex_missing->native_session_id, std::string(""));
	UAM_ASSERT_EQ(codex_invalid->native_session_id, std::string(""));
	UAM_ASSERT_EQ(gemini_missing->native_session_id, std::string("gemini-missing"));
}

UAM_TEST(StateSerializerIncludesChatModelId)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-1";
	chat.title = "Model chat";
	chat.provider_id = "gemini-cli";
	chat.model_id = "auto-gemini-3";
	chat.approval_mode = "plan";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	app.chats.push_back(std::move(chat));
	app.selected_chat_index = 0;

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(serialized["chats"][0].value("modelId", ""), std::string("auto-gemini-3"));
	UAM_ASSERT_EQ(serialized["chats"][0].value("approvalMode", ""), std::string("plan"));
	UAM_ASSERT_EQ(serialized["chats"][0]["acpSession"].value("currentModeId", ""), std::string("plan"));
	UAM_ASSERT_EQ(serialized["chats"][0]["acpSession"].value("currentModelId", ""), std::string("auto-gemini-3"));

	const nlohmann::json fingerprint = uam::StateSerializer::SerializeFingerprint(app);
	UAM_ASSERT_EQ(fingerprint["chats"][0].value("modelId", ""), std::string("auto-gemini-3"));
	UAM_ASSERT_EQ(fingerprint["chats"][0].value("approvalMode", ""), std::string("plan"));
}

UAM_TEST(StateSerializerIncludesMessageToolCalls)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-1";
	chat.title = "Tool chat";
	chat.provider_id = "gemini-cli";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	Message assistant;
	assistant.role = MessageRole::Assistant;
	assistant.content = "Done.";
	assistant.created_at = "2026-01-01T00:00:01.000Z";
	ToolCall tool_call;
	tool_call.id = "tool-1";
	tool_call.name = "Read file";
	tool_call.args_json = R"({"path":"file.txt"})";
	tool_call.result_text = "file contents";
	tool_call.status = "completed";
	assistant.tool_calls.push_back(std::move(tool_call));
	chat.messages.push_back(std::move(assistant));
	app.chats.push_back(std::move(chat));
	app.selected_chat_index = 0;

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json tool_json = serialized["chats"][0]["messages"][0]["toolCalls"][0];
	UAM_ASSERT_EQ(tool_json.value("id", ""), std::string("tool-1"));
	UAM_ASSERT_EQ(tool_json.value("title", ""), std::string("Read file"));
	UAM_ASSERT_EQ(tool_json.value("status", ""), std::string("completed"));
	UAM_ASSERT(tool_json.value("content", "").find("file contents") != std::string::npos);
}

UAM_TEST(ProviderRegistryResolvesGeminiCodexAndUnknownExactly)
{
	const IProviderRuntime& gemini = ProviderRuntimeRegistry::ResolveById("gemini-cli");
	UAM_ASSERT_EQ(std::string(gemini.RuntimeId()), std::string("gemini-cli"));
	UAM_ASSERT(ProviderRuntimeRegistry::IsKnownRuntimeId("gemini-cli"));
	UAM_ASSERT(ProviderRuntime::IsRuntimeEnabled("gemini-cli"));

	const IProviderRuntime& codex = ProviderRuntimeRegistry::ResolveById("codex-cli");
	UAM_ASSERT_EQ(std::string(codex.RuntimeId()), std::string("codex-cli"));
	UAM_ASSERT(ProviderRuntimeRegistry::IsKnownRuntimeId("codex-cli"));
	UAM_ASSERT(ProviderRuntime::IsRuntimeEnabled("codex-cli"));

	const IProviderRuntime& unknown = ProviderRuntimeRegistry::ResolveById("unknown");
	UAM_ASSERT_EQ(std::string(unknown.RuntimeId()), std::string("unsupported"));
	UAM_ASSERT(!ProviderRuntimeRegistry::IsKnownRuntimeId("unknown"));
	UAM_ASSERT(!ProviderRuntime::IsRuntimeEnabled("unknown"));
	UAM_ASSERT(!ProviderRuntimeRegistry::IsKnownRuntimeId("gemini"));
	UAM_ASSERT(!ProviderRuntimeRegistry::IsKnownRuntimeId("codex"));
}

UAM_TEST(CodexThreadIdValidatorAcceptsOnlyUuidThreadIds)
{
	UAM_ASSERT(uam::IsValidCodexThreadIdForTests("6a6f0f3b-1a0b-4a9c-8a01-111111111111"));
	UAM_ASSERT(uam::IsValidCodexThreadIdForTests("urn:uuid:6a6f0f3b-1a0b-4a9c-8a01-111111111111"));
	UAM_ASSERT(!uam::IsValidCodexThreadIdForTests(""));
	UAM_ASSERT(!uam::IsValidCodexThreadIdForTests("chat-1"));
	UAM_ASSERT(!uam::IsValidCodexThreadIdForTests("native-abc"));
	UAM_ASSERT(!uam::IsValidCodexThreadIdForTests("thread-1"));
	UAM_ASSERT(!uam::IsValidCodexThreadIdForTests("6a6f0f3b-1a0b-4a9c-8a01-zzzzzzzzzzzz"));
}

UAM_TEST(GeminiCliInteractiveArgvUsesResumeAndFlags)
{
	ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	AppSettings settings;
	settings.provider_yolo_mode = true;
	settings.provider_extra_flags = "--checkpointing";

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "native-abc";

	const std::vector<std::string> argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	UAM_ASSERT_EQ(argv.size(), static_cast<std::size_t>(5));
	UAM_ASSERT_EQ(argv[0], std::string("gemini"));
	UAM_ASSERT_EQ(argv[1], std::string("--yolo"));
	UAM_ASSERT_EQ(argv[2], std::string("--checkpointing"));
	UAM_ASSERT_EQ(argv[3], std::string("-r"));
	UAM_ASSERT_EQ(argv[4], std::string("native-abc"));
}

UAM_TEST(CodexCliInteractiveArgvUsesResumeModelAndFlags)
{
	ProviderProfile profile = ProviderProfileStore::DefaultCodexProfile();
	AppSettings settings;
	settings.provider_yolo_mode = true;
	settings.provider_extra_flags = "--sandbox danger-full-access --ask-for-approval never";

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	chat.model_id = "gpt-5.4";

	const std::vector<std::string> fresh = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	UAM_ASSERT_EQ(fresh.size(), static_cast<std::size_t>(9));
	UAM_ASSERT_EQ(fresh[0], std::string("codex"));
	UAM_ASSERT_EQ(fresh[1], std::string("--no-alt-screen"));
	UAM_ASSERT_EQ(fresh[2], std::string("-m"));
	UAM_ASSERT_EQ(fresh[3], std::string("gpt-5.4"));
	UAM_ASSERT_EQ(fresh[4], std::string("--full-auto"));
	UAM_ASSERT_EQ(fresh[5], std::string("--sandbox"));
	UAM_ASSERT_EQ(fresh[6], std::string("danger-full-access"));
	UAM_ASSERT_EQ(fresh[7], std::string("--ask-for-approval"));
	UAM_ASSERT_EQ(fresh[8], std::string("never"));

	chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	const std::vector<std::string> resumed = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	UAM_ASSERT_EQ(resumed.size(), static_cast<std::size_t>(6));
	UAM_ASSERT_EQ(resumed[0], std::string("codex"));
	UAM_ASSERT_EQ(resumed[1], std::string("resume"));
	UAM_ASSERT_EQ(resumed[2], std::string("--no-alt-screen"));
	UAM_ASSERT_EQ(resumed[3], chat.native_session_id);
	UAM_ASSERT_EQ(resumed[4], std::string("-m"));
	UAM_ASSERT_EQ(resumed[5], std::string("gpt-5.4"));

	chat.native_session_id = "chat-1";
	const std::vector<std::string> invalid_resume = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	UAM_ASSERT_EQ(invalid_resume.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(invalid_resume[0], std::string("codex"));
	UAM_ASSERT_EQ(invalid_resume[1], std::string("--no-alt-screen"));
	UAM_ASSERT_EQ(invalid_resume[2], std::string("-m"));
	UAM_ASSERT_EQ(invalid_resume[3], std::string("gpt-5.4"));
}

UAM_TEST(CodexSessionIndexPicksNewestNewSessionForMatchingCwd)
{
	TempDir temp("uam-codex-index");
	const fs::path cwd = temp.root / "workspace";
	fs::create_directories(cwd);
	fs::create_directories(temp.root / "sessions" / "2026");

	const std::string old_id = "11111111-1111-4111-8111-111111111111";
	const std::string wrong_id = "22222222-2222-4222-8222-222222222222";
	const std::string match_id = "33333333-3333-4333-8333-333333333333";
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "session_index.jsonl",
	                                  nlohmann::json({{"id", old_id}, {"updated_at", "2026-04-18T10:00:00Z"}}).dump() + "\n" +
	                                      nlohmann::json({{"id", wrong_id}, {"updated_at", "2026-04-18T10:01:00Z"}}).dump() + "\n" +
	                                      nlohmann::json({{"id", match_id}, {"updated_at", "2026-04-18T10:02:00Z"}}).dump() + "\n"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "sessions" / "2026" / ("rollout-" + wrong_id + ".jsonl"),
	                                  nlohmann::json({{"type", "session_meta"}, {"payload", {{"id", wrong_id}, {"cwd", (temp.root / "other").string()}}}}).dump() + "\n"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "sessions" / "2026" / ("rollout-" + match_id + ".jsonl"),
	                                  nlohmann::json({{"type", "session_meta"}, {"payload", {{"id", match_id}, {"cwd", cwd.string()}}}}).dump() + "\n"));

	const std::vector<std::string> before = {old_id};
	UAM_ASSERT_EQ(uam::codex::PickNewSessionId(before, cwd, temp.root), match_id);
	UAM_ASSERT_EQ(uam::codex::PickNewSessionId({old_id, match_id}, cwd, temp.root), std::string(""));
}

UAM_TEST(AcpJsonRpcBuildersUseProtocolMethods)
{
	const nlohmann::json initialize = nlohmann::json::parse(uam::BuildAcpInitializeRequestForTests(7));
	UAM_ASSERT_EQ(initialize.value("jsonrpc", ""), std::string("2.0"));
	UAM_ASSERT_EQ(initialize.value("id", 0), 7);
	UAM_ASSERT_EQ(initialize.value("method", ""), std::string("initialize"));
	UAM_ASSERT_EQ(initialize["params"].value("protocolVersion", 0), 1);

	const nlohmann::json session_new = nlohmann::json::parse(uam::BuildAcpNewSessionRequestForTests(8, "/tmp/project"));
	UAM_ASSERT_EQ(session_new.value("method", ""), std::string("session/new"));
	UAM_ASSERT_EQ(session_new["params"].value("cwd", ""), std::string("/tmp/project"));
	UAM_ASSERT(session_new["params"]["mcpServers"].is_array());

	const nlohmann::json prompt = nlohmann::json::parse(uam::BuildAcpPromptRequestForTests(9, "sess-1", "hello"));
	UAM_ASSERT_EQ(prompt.value("method", ""), std::string("session/prompt"));
	UAM_ASSERT_EQ(prompt["params"].value("sessionId", ""), std::string("sess-1"));
	UAM_ASSERT_EQ(prompt["params"]["prompt"][0].value("type", ""), std::string("text"));
	UAM_ASSERT_EQ(prompt["params"]["prompt"][0].value("text", ""), std::string("hello"));

	const nlohmann::json set_mode = nlohmann::json::parse(uam::BuildAcpSetModeRequestForTests(10, "sess-1", "plan"));
	UAM_ASSERT_EQ(set_mode.value("method", ""), std::string("session/set_mode"));
	UAM_ASSERT_EQ(set_mode["params"].value("sessionId", ""), std::string("sess-1"));
	UAM_ASSERT_EQ(set_mode["params"].value("modeId", ""), std::string("plan"));

	const nlohmann::json set_model = nlohmann::json::parse(uam::BuildAcpSetModelRequestForTests(11, "sess-1", "auto-gemini-3"));
	UAM_ASSERT_EQ(set_model.value("method", ""), std::string("session/set_model"));
	UAM_ASSERT_EQ(set_model["params"].value("sessionId", ""), std::string("sess-1"));
	UAM_ASSERT_EQ(set_model["params"].value("modelId", ""), std::string("auto-gemini-3"));
}

UAM_TEST(CodexAppServerRequestBuildersUseCodexProtocolMethods)
{
	ChatSession chat;
	chat.provider_id = "codex-cli";
	chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	chat.model_id = "gpt-5.4";
	chat.approval_mode = "plan";

	const nlohmann::json initialize = nlohmann::json::parse(uam::BuildCodexInitializeRequestForTests(21));
	UAM_ASSERT_EQ(initialize.value("method", ""), std::string("initialize"));
	UAM_ASSERT(initialize["params"].contains("clientInfo"));
	UAM_ASSERT(initialize["params"]["capabilities"].is_object());
	UAM_ASSERT(initialize["params"]["capabilities"].value("experimentalApi", false));

	const nlohmann::json initialized = nlohmann::json::parse(uam::BuildCodexInitializedNotificationForTests());
	UAM_ASSERT_EQ(initialized.value("method", ""), std::string("initialized"));
	UAM_ASSERT(!initialized.contains("id"));

	const nlohmann::json model_list = nlohmann::json::parse(uam::BuildCodexModelListRequestForTests(22));
	UAM_ASSERT_EQ(model_list.value("method", ""), std::string("model/list"));

	ChatSession invalid_resume_chat = chat;
	invalid_resume_chat.native_session_id = "chat-1";
	const nlohmann::json invalid_setup = nlohmann::json::parse(uam::BuildCodexSessionSetupRequestForTests(20, invalid_resume_chat, "/tmp/project"));
	UAM_ASSERT_EQ(invalid_setup.value("method", ""), std::string("thread/start"));

	const nlohmann::json valid_setup = nlohmann::json::parse(uam::BuildCodexSessionSetupRequestForTests(20, chat, "/tmp/project"));
	UAM_ASSERT_EQ(valid_setup.value("method", ""), std::string("thread/resume"));
	UAM_ASSERT_EQ(valid_setup["params"].value("threadId", ""), chat.native_session_id);

	const nlohmann::json thread_start = nlohmann::json::parse(uam::BuildCodexThreadStartRequestForTests(23, chat, "/tmp/project"));
	UAM_ASSERT_EQ(thread_start.value("method", ""), std::string("thread/start"));
	UAM_ASSERT_EQ(thread_start["params"].value("cwd", ""), std::string("/tmp/project"));
	UAM_ASSERT_EQ(thread_start["params"].value("model", ""), std::string("gpt-5.4"));
	UAM_ASSERT(thread_start["params"].value("persistExtendedHistory", false));

	const nlohmann::json thread_resume = nlohmann::json::parse(uam::BuildCodexThreadResumeRequestForTests(24, chat, "/tmp/project"));
	UAM_ASSERT_EQ(thread_resume.value("method", ""), std::string("thread/resume"));
	UAM_ASSERT_EQ(thread_resume["params"].value("threadId", ""), chat.native_session_id);
	UAM_ASSERT(thread_resume["params"].value("persistExtendedHistory", false));

	const nlohmann::json turn_start = nlohmann::json::parse(uam::BuildCodexTurnStartRequestForTests(25, chat.native_session_id, "hello", chat));
	UAM_ASSERT_EQ(turn_start.value("method", ""), std::string("turn/start"));
	UAM_ASSERT_EQ(turn_start["params"].value("threadId", ""), chat.native_session_id);
	UAM_ASSERT_EQ(turn_start["params"]["input"][0].value("text", ""), std::string("hello"));
	UAM_ASSERT_EQ(turn_start["params"]["collaborationMode"].value("mode", ""), std::string("plan"));

	const nlohmann::json interrupt = nlohmann::json::parse(uam::BuildCodexTurnInterruptRequestForTests(26, chat.native_session_id, "turn-1"));
	UAM_ASSERT_EQ(interrupt.value("method", ""), std::string("turn/interrupt"));
	UAM_ASSERT_EQ(interrupt["params"].value("turnId", ""), std::string("turn-1"));
}

UAM_TEST(AcpLaunchArgsIncludeSelectedModel)
{
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "native-1";

	const std::vector<std::string> default_argv = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT_EQ(default_argv.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(default_argv[0], std::string("gemini"));
	UAM_ASSERT_EQ(default_argv[1], std::string("--acp"));

	chat.model_id = " flash ";
	const std::vector<std::string> selected_argv = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT_EQ(selected_argv.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(selected_argv[0], std::string("gemini"));
	UAM_ASSERT_EQ(selected_argv[1], std::string("--acp"));
	UAM_ASSERT_EQ(selected_argv[2], std::string("--model"));
	UAM_ASSERT_EQ(selected_argv[3], std::string("flash"));

	chat.approval_mode = " plan ";
	const std::vector<std::string> plan_argv = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT_EQ(plan_argv.size(), static_cast<std::size_t>(6));
	UAM_ASSERT_EQ(plan_argv[0], std::string("gemini"));
	UAM_ASSERT_EQ(plan_argv[1], std::string("--acp"));
	UAM_ASSERT_EQ(plan_argv[2], std::string("--approval-mode"));
	UAM_ASSERT_EQ(plan_argv[3], std::string("plan"));
	UAM_ASSERT_EQ(plan_argv[4], std::string("--model"));
	UAM_ASSERT_EQ(plan_argv[5], std::string("flash"));

	const std::string detail = uam::BuildAcpLaunchDetailForTests("/tmp/project", chat);
	UAM_ASSERT(detail.find("cwd=/tmp/project") != std::string::npos);
	UAM_ASSERT(detail.find("argv=gemini --acp --approval-mode plan --model flash") != std::string::npos);
	UAM_ASSERT(detail.find("nativeSessionId=native-1") != std::string::npos);

	ChatSession codex_chat;
	codex_chat.id = "codex-chat";
	codex_chat.provider_id = "codex-cli";
	codex_chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	codex_chat.model_id = "gpt-5.4";
	const std::vector<std::string> codex_argv = uam::BuildAcpLaunchArgvForTests(codex_chat);
	UAM_ASSERT_EQ(codex_argv.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(codex_argv[0], std::string("codex"));
	UAM_ASSERT_EQ(codex_argv[1], std::string("app-server"));
	UAM_ASSERT_EQ(codex_argv[2], std::string("--listen"));
	UAM_ASSERT_EQ(codex_argv[3], std::string("stdio://"));
	const std::string codex_detail = uam::BuildAcpLaunchDetailForTests("/tmp/project", codex_chat);
	UAM_ASSERT(codex_detail.find("argv=codex app-server --listen stdio://") != std::string::npos);
	UAM_ASSERT(codex_detail.find("nativeSessionId=6a6f0f3b-1a0b-4a9c-8a01-111111111111") != std::string::npos);
}

UAM_TEST(CefBridgeRequestValidationRejectsMalformedEnvelopes)
{
	const auto non_object = uam::cef::ParseBridgeRequest(R"(["getInitialState"])");
	UAM_ASSERT(!non_object.ok);
	UAM_ASSERT_EQ(non_object.status, 400);

	const auto missing_action = uam::cef::ParseBridgeRequest(R"({"payload":{}})");
	UAM_ASSERT(!missing_action.ok);
	UAM_ASSERT_EQ(missing_action.status, 400);

	const auto non_string_action = uam::cef::ParseBridgeRequest(R"({"action":7,"payload":{}})");
	UAM_ASSERT(!non_string_action.ok);
	UAM_ASSERT_EQ(non_string_action.status, 400);

	const auto blank_action = uam::cef::ParseBridgeRequest(R"({"action":"  ","payload":{}})");
	UAM_ASSERT(!blank_action.ok);
	UAM_ASSERT_EQ(blank_action.status, 400);

	const auto string_payload = uam::cef::ParseBridgeRequest(R"({"action":"getInitialState","payload":"bad"})");
	UAM_ASSERT(!string_payload.ok);
	UAM_ASSERT_EQ(string_payload.status, 400);

	const auto array_payload = uam::cef::ParseBridgeRequest(R"({"action":"getInitialState","payload":[]})");
	UAM_ASSERT(!array_payload.ok);
	UAM_ASSERT_EQ(array_payload.status, 400);
}

UAM_TEST(CefBridgeRequestValidationDefaultsMissingAndNullPayload)
{
	const auto missing_payload = uam::cef::ParseBridgeRequest(R"({"action":"getInitialState"})");
	UAM_ASSERT(missing_payload.ok);
	UAM_ASSERT_EQ(missing_payload.request.action, std::string("getInitialState"));
	UAM_ASSERT(missing_payload.request.payload.is_object());
	UAM_ASSERT(missing_payload.request.payload.empty());

	const auto null_payload = uam::cef::ParseBridgeRequest(R"({"action":"getInitialState","payload":null})");
	UAM_ASSERT(null_payload.ok);
	UAM_ASSERT_EQ(null_payload.request.action, std::string("getInitialState"));
	UAM_ASSERT(null_payload.request.payload.is_object());
	UAM_ASSERT(null_payload.request.payload.empty());

	const auto valid_payload = uam::cef::ParseBridgeRequest(R"({"action":"selectSession","payload":{"chatId":"chat-1"}})");
	UAM_ASSERT(valid_payload.ok);
	UAM_ASSERT_EQ(valid_payload.request.action, std::string("selectSession"));
	UAM_ASSERT_EQ(valid_payload.request.payload.value("chatId", ""), std::string("chat-1"));
}

UAM_TEST(AcpTurnTimelinePreservesStreamOrder)
{
	TempDir temp("uam-acp-turn-events");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message user;
	user.role = MessageRole::User;
	user.content = "Please inspect this.";
	user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 0;
	session->turn_serial = 4;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));
	app.chats_with_unseen_updates.insert("chat-1");

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Before "}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_thought_chunk","content":{"type":"text","text":"Need to inspect the file first."}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call","toolCallId":"tool-1","title":"Read file","kind":"read","status":"in_progress","content":{"type":"text","text":"Reading"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call","toolCallId":"tool-1","title":"Read file","kind":"read","status":"completed","content":{"type":"text","text":"Read complete"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":5,"method":"session/request_permission","params":{"toolCall":{"toolCallId":"tool-1","title":"Read file","kind":"read","status":"pending","content":{"type":"text","text":"Read /tmp/file.txt"}},"options":[{"optionId":"allow-once","name":"Allow once","kind":"allow_once"}]}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"After"}}}})"));

	UAM_ASSERT_EQ(raw_session->turn_events.size(), static_cast<std::size_t>(5));
	UAM_ASSERT_EQ(raw_session->turn_events[0].type, std::string("assistant_text"));
	UAM_ASSERT_EQ(raw_session->turn_events[0].text, std::string("Before "));
	UAM_ASSERT_EQ(raw_session->turn_events[1].type, std::string("thought"));
	UAM_ASSERT_EQ(raw_session->turn_events[1].text, std::string("Need to inspect the file first."));
	UAM_ASSERT_EQ(raw_session->turn_events[2].type, std::string("tool_call"));
	UAM_ASSERT_EQ(raw_session->turn_events[2].tool_call_id, std::string("tool-1"));
	UAM_ASSERT_EQ(raw_session->turn_events[3].type, std::string("permission_request"));
	UAM_ASSERT_EQ(raw_session->turn_events[3].request_id_json, std::string("5"));
	UAM_ASSERT_EQ(raw_session->turn_events[4].type, std::string("assistant_text"));
	UAM_ASSERT_EQ(raw_session->turn_events[4].text, std::string("After"));
	UAM_ASSERT_EQ(app.chats.front().messages[1].content, std::string("Before After"));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json acp = serialized["chats"][0]["acpSession"];
	UAM_ASSERT_EQ(acp["turnEvents"].size(), static_cast<std::size_t>(5));
	UAM_ASSERT_EQ(acp.value("turnUserMessageIndex", -2), 0);
	UAM_ASSERT_EQ(acp.value("turnAssistantMessageIndex", -2), 1);
	UAM_ASSERT_EQ(acp.value("turnSerial", -2), 4);
	UAM_ASSERT(acp.value("readySinceLastSelect", false));
	UAM_ASSERT_EQ(acp["turnEvents"][1].value("type", ""), std::string("thought"));
	UAM_ASSERT_EQ(acp["turnEvents"][2].value("toolCallId", ""), std::string("tool-1"));
	UAM_ASSERT_EQ(acp["turnEvents"][3].value("requestId", ""), std::string("5"));
}

UAM_TEST(AcpPromptCompletionClearsProcessingByMethodAndPromptId)
{
	TempDir temp("uam-acp-completion");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->processing = true;
	session->waiting_for_permission = true;
	session->prompt_request_id = 42;
	session->queued_prompt = "hello";
	session->current_assistant_message_index = 0;
	session->pending_permission.request_id_json = "7";
	session->pending_request_methods[42] = "session/prompt";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":42,"result":{"stopReason":"end_turn"}})"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(!raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string(""));
	UAM_ASSERT_EQ(raw_session->pending_permission.request_id_json, std::string(""));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("ready"));
	UAM_ASSERT(app.chats_with_unseen_updates.find("chat-1") != app.chats_with_unseen_updates.end());

	raw_session->processing = true;
	raw_session->waiting_for_permission = true;
	raw_session->prompt_request_id = 99;
	raw_session->queued_prompt = "again";
	raw_session->current_assistant_message_index = 0;
	raw_session->pending_permission.request_id_json = "8";

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":99,"result":{"stopReason":"end_turn"}})"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(!raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string(""));
	UAM_ASSERT_EQ(raw_session->pending_permission.request_id_json, std::string(""));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("ready"));

	raw_session->processing = true;
	raw_session->waiting_for_permission = true;
	raw_session->prompt_request_id = 100;
	raw_session->queued_prompt = "bad json";
	raw_session->pending_permission.request_id_json = "9";

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":)"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(!raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string(""));
	UAM_ASSERT_EQ(raw_session->pending_permission.request_id_json, std::string(""));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT(raw_session->last_error.find("Invalid JSON from Gemini ACP") != std::string::npos);
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("invalid_json"));
	UAM_ASSERT(raw_session->diagnostics.back().detail.find(R"({"jsonrpc":)") != std::string::npos);
}

UAM_TEST(AcpJsonRpcErrorsIncludeRequestDiagnostics)
{
	TempDir temp("uam-acp-jsonrpc-diagnostics");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->prompt_request_id = 42;
	session->queued_prompt = "hello";
	session->recent_stderr = "Gemini stderr stack trace";
	session->pending_request_methods[42] = "session/prompt";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":42,"error":{"code":-32603,"message":"Internal error","data":{"cause":"boom","trace":"hidden detail"}}})"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(raw_session->last_error.find("Gemini ACP session/prompt failed (id=42, code=-32603): Internal error") != std::string::npos);
	UAM_ASSERT(raw_session->last_error.find("See diagnostics/stderr details.") != std::string::npos);
	UAM_ASSERT(!raw_session->diagnostics.empty());

	const uam::AcpDiagnosticEntryState& diagnostic = raw_session->diagnostics.back();
	UAM_ASSERT_EQ(diagnostic.event, std::string("response"));
	UAM_ASSERT_EQ(diagnostic.reason, std::string("jsonrpc_error"));
	UAM_ASSERT_EQ(diagnostic.method, std::string("session/prompt"));
	UAM_ASSERT_EQ(diagnostic.request_id, std::string("42"));
	UAM_ASSERT(diagnostic.has_code);
	UAM_ASSERT_EQ(diagnostic.code, -32603);
	UAM_ASSERT_EQ(diagnostic.message, std::string("Internal error"));
	UAM_ASSERT(diagnostic.detail.find("error.data=") != std::string::npos);
	UAM_ASSERT(diagnostic.detail.find("Gemini stderr stack trace") != std::string::npos);

	raw_session->has_last_exit_code = true;
	raw_session->last_exit_code = 137;
	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json acp = serialized["chats"][0]["acpSession"];
	UAM_ASSERT_EQ(acp.value("lastExitCode", 0), 137);
	UAM_ASSERT(!acp["diagnostics"].empty());
	UAM_ASSERT_EQ(acp["diagnostics"].back().value("reason", ""), std::string("jsonrpc_error"));
	UAM_ASSERT_EQ(acp["diagnostics"].back().value("method", ""), std::string("session/prompt"));
	UAM_ASSERT_EQ(acp["diagnostics"].back().value("code", 0), -32603);
}

UAM_TEST(CodexAppServerErrorsUseCodexRuntimeName)
{
	TempDir temp("uam-codex-app-server-error-name");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->initialized = true;
	session->session_setup_request_id = 3;
	session->recent_stderr = "Codex app-server stderr";
	session->pending_request_methods[3] = "thread/start";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":3,"error":{"code":-32600,"message":"thread/start.persistFullHistory requires experimentalApi capability"}})"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT(raw_session->last_error.find("Codex app-server thread/start failed (id=3, code=-32600): thread/start.persistFullHistory requires experimentalApi capability") != std::string::npos);
	UAM_ASSERT(raw_session->last_error.find("Gemini") == std::string::npos);
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().method, std::string("thread/start"));
	UAM_ASSERT(raw_session->diagnostics.back().detail.find("Codex app-server stderr") != std::string::npos);
}

UAM_TEST(AcpMissingSessionIdRecordsDiagnostics)
{
	TempDir temp("uam-acp-missing-session-id");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->initialized = true;
	session->session_setup_request_id = 8;
	session->pending_request_methods[8] = "session/new";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":8,"result":{}})"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT(raw_session->last_error.find("Gemini ACP session/new failed (id=8): Gemini ACP did not return a session id.") != std::string::npos);
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("missing_session_id"));
	UAM_ASSERT(raw_session->diagnostics.back().detail.find("result={}") != std::string::npos);
}

UAM_TEST(AcpSessionNewParsesModesModelsAndModeUpdates)
{
	TempDir temp("uam-acp-modes-models");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->initialized = true;
	session->session_setup_request_id = 8;
	session->pending_request_methods[8] = "session/new";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const nlohmann::json session_new =
	    {
	        {"jsonrpc", "2.0"},
	        {"id", 8},
	        {"result",
	         {
	             {"sessionId", "sess-1"},
	             {"modes",
	              {
	                  {"availableModes",
	                   nlohmann::json::array({
	                       {{"id", "default"}, {"name", "Default"}, {"description", "Run normally"}},
	                       {{"id", "plan"}, {"name", "Plan"}, {"description", "Plan before editing"}},
	                   })},
	                  {"currentModeId", "default"},
	              }},
	             {"models",
	              {
	                  {"availableModels",
	                   nlohmann::json::array({
	                       {{"modelId", "auto-gemini-3"}, {"name", "Auto 3"}, {"description", "Gemini 3 routing"}},
	                       {{"modelId", "gemini-3-flash-preview"}, {"name", "Gemini 3 Flash"}, {"description", "Preview model"}},
	                   })},
	                  {"currentModelId", "auto-gemini-3"},
	              }},
	         }},
	    };

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), session_new.dump()));
	UAM_ASSERT_EQ(raw_session->session_id, std::string("sess-1"));
	UAM_ASSERT_EQ(raw_session->available_modes.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->available_modes[1].id, std::string("plan"));
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string("default"));
	UAM_ASSERT_EQ(raw_session->available_models.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->available_models[0].id, std::string("auto-gemini-3"));
	UAM_ASSERT_EQ(raw_session->available_models[1].name, std::string("Gemini 3 Flash"));
	UAM_ASSERT_EQ(raw_session->current_model_id, std::string("auto-gemini-3"));

	const nlohmann::json mode_update =
	    {
	        {"jsonrpc", "2.0"},
	        {"method", "session/update"},
	        {"params", {{"update", {{"sessionUpdate", "current_mode_update"}, {"currentModeId", "plan"}}}}},
	    };
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), mode_update.dump()));
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string("plan"));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json acp = serialized["chats"][0]["acpSession"];
	UAM_ASSERT_EQ(acp["availableModes"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(acp.value("currentModeId", ""), std::string("plan"));
	UAM_ASSERT_EQ(acp["availableModels"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(acp.value("currentModelId", ""), std::string("auto-gemini-3"));
}

UAM_TEST(CodexAppServerStateTransitionsMapModelsTurnsToolsAndApprovals)
{
	TempDir temp("uam-codex-app-server-state");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	chat.approval_mode = "plan";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->next_request_id = 100;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	raw_session->initialize_request_id = 1;
	raw_session->pending_request_methods[1] = "initialize";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":1,"result":{"userAgent":"codex-cli/1.2.3"}})"));
	UAM_ASSERT(raw_session->initialized);
	UAM_ASSERT_EQ(raw_session->agent_title, std::string("Codex"));
	UAM_ASSERT_EQ(raw_session->agent_version, std::string("codex-cli/1.2.3"));

	raw_session->pending_request_methods[2] = "model/list";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":2,"result":{"data":[{"id":"gpt-5.4","displayName":"GPT 5.4","description":"Frontier","isDefault":true},{"id":"hidden","displayName":"Hidden","hidden":true}]}})"));
	UAM_ASSERT_EQ(raw_session->available_models.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->available_models[0].id, std::string("gpt-5.4"));
	UAM_ASSERT_EQ(raw_session->current_model_id, std::string("gpt-5.4"));

	raw_session->session_setup_request_id = 3;
	raw_session->pending_request_methods[3] = "thread/start";
	const std::string codex_thread_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), nlohmann::json({{"jsonrpc", "2.0"}, {"id", 3}, {"result", {{"thread", {{"id", codex_thread_id}}}, {"model", "gpt-5.4"}}}}).dump()));
	UAM_ASSERT_EQ(raw_session->session_id, codex_thread_id);
	UAM_ASSERT_EQ(raw_session->codex_thread_id, codex_thread_id);
	UAM_ASSERT_EQ(app.chats.front().native_session_id, codex_thread_id);
	UAM_ASSERT_EQ(raw_session->available_modes.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string("plan"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("ready"));

	raw_session->processing = true;
	raw_session->queued_prompt = "hello";
	raw_session->prompt_request_id = 4;
	raw_session->pending_request_methods[4] = "turn/start";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":4,"result":{"turn":{"id":"turn-1"}}})"));
	UAM_ASSERT_EQ(raw_session->codex_turn_id, std::string("turn-1"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("processing"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"item/agentMessage/delta","params":{"delta":"Hello from Codex."}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages[0].provider, std::string("codex-cli"));
	UAM_ASSERT_EQ(app.chats.front().messages[0].content, std::string("Hello from Codex."));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"turn/plan/updated","params":{"plan":[{"step":"Inspect files","status":"completed"},{"step":"Patch code","status":"pending"}]}})"));
	UAM_ASSERT_EQ(raw_session->plan_entries.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->plan_entries[1].content, std::string("Patch code"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"item/started","params":{"item":{"id":"cmd-1","type":"commandExecution","command":"ls","status":"running"}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"item/commandExecution/outputDelta","params":{"itemId":"cmd-1","delta":"file.txt\n"}})"));
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->tool_calls[0].id, std::string("cmd-1"));
	UAM_ASSERT(raw_session->tool_calls[0].content.find("file.txt") != std::string::npos);

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":7,"method":"item/commandExecution/requestApproval","params":{"itemId":"cmd-1","command":"rm -rf build","availableDecisions":["accept","decline"]}})"));
	UAM_ASSERT(raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->pending_permission.request_id_json, std::string("7"));
	UAM_ASSERT_EQ(raw_session->pending_permission.provider_request_kind, std::string("codex-command"));
	UAM_ASSERT_EQ(raw_session->pending_permission.options[0].id, std::string("accept"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("waitingPermission"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"turn/completed","params":{"turnId":"turn-1"}})"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(!raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->codex_turn_id, std::string(""));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("ready"));
}

UAM_TEST(AcpDiagnosticRingCapsEntriesAndLongDetails)
{
	TempDir temp("uam-acp-diagnostic-ring");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const std::string long_invalid_line = std::string("{\"jsonrpc\":") + std::string(10000, 'x');
	for (int i = 0; i < 90; ++i)
	{
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), long_invalid_line));
	}

	UAM_ASSERT_EQ(raw_session->diagnostics.size(), static_cast<std::size_t>(80));
	UAM_ASSERT(raw_session->diagnostics.back().detail.size() < long_invalid_line.size());
	UAM_ASSERT(raw_session->diagnostics.back().detail.find("[truncated ") != std::string::npos);
}

UAM_TEST(AcpAssistantReplayIsStrippedFromNewTurn)
{
	TempDir temp("uam-acp-replay-strip");
	uam::AppState app;
	app.data_root = temp.root;

	const std::string previous_response = "Previous Gemini response with enough content to identify a replayed assistant message.";
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message first_user;
	first_user.role = MessageRole::User;
	first_user.content = "First prompt";
	first_user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(first_user));
	Message first_assistant;
	first_assistant.role = MessageRole::Assistant;
	first_assistant.content = previous_response;
	first_assistant.created_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(std::move(first_assistant));
	Message second_user;
	second_user.role = MessageRole::User;
	second_user.content = "Second prompt";
	second_user.created_at = "2026-01-01T00:00:02.000Z";
	chat.messages.push_back(std::move(second_user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 2;
	session->assistant_replay_prefixes.push_back(previous_response);
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"method", "session/update"},
	                                                          {"params",
	                                                           {
	                                                               {"update",
	                                                                {
	                                                                    {"sessionUpdate", "agent_message_chunk"},
	                                                                    {"content", {{"type", "text"}, {"text", previous_response}}},
	                                                                }},
	                                                           }},
	                                                      })
	                                           .dump()));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(3));
	UAM_ASSERT(raw_session->turn_events.empty());

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"method", "session/update"},
	                                                          {"params",
	                                                           {
	                                                               {"update",
	                                                                {
	                                                                    {"sessionUpdate", "agent_message_chunk"},
	                                                                    {"content", {{"type", "text"}, {"text", previous_response + "\n\nSecond answer"}}},
	                                                                }},
	                                                           }},
	                                                      })
	                                           .dump()));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(app.chats.front().messages[3].content, std::string("Second answer"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"method", "session/update"},
	                                                          {"params",
	                                                           {
	                                                               {"update",
	                                                                {
	                                                                    {"sessionUpdate", "agent_message_chunk"},
	                                                                    {"content", {{"type", "text"}, {"text", previous_response + "\n\nSecond answer with suffix"}}},
	                                                                }},
	                                                           }},
	                                                      })
	                                           .dump()));
	UAM_ASSERT_EQ(app.chats.front().messages[3].content, std::string("Second answer with suffix"));
	UAM_ASSERT_EQ(raw_session->turn_events.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->turn_events[0].text, std::string("Second answer with suffix"));
}

UAM_TEST(AcpLoadHistoryReplaySuppressesShortAssistantResponse)
{
	TempDir temp("uam-acp-short-replay");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message first_user;
	first_user.role = MessageRole::User;
	first_user.content = "First prompt";
	first_user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(first_user));
	Message first_assistant;
	first_assistant.role = MessageRole::Assistant;
	first_assistant.content = "OK";
	first_assistant.created_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(std::move(first_assistant));
	Message second_user;
	second_user.role = MessageRole::User;
	second_user.content = "Second prompt";
	second_user.created_at = "2026-01-01T00:00:02.000Z";
	chat.messages.push_back(std::move(second_user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 2;
	uam::AcpReplayUpdateState user_replay;
	user_replay.session_update = "user_message_chunk";
	user_replay.text = "First prompt";
	session->load_history_replay_updates.push_back(std::move(user_replay));
	uam::AcpReplayUpdateState assistant_replay;
	assistant_replay.session_update = "agent_message_chunk";
	assistant_replay.text = "OK";
	session->load_history_replay_updates.push_back(std::move(assistant_replay));
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"user_message_chunk","content":{"type":"text","text":"First prompt"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"OK"}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(3));
	UAM_ASSERT(raw_session->turn_events.empty());

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"New answer"}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(app.chats.front().messages[3].content, std::string("New answer"));
}

UAM_TEST(AcpLoadHistoryReplayStripsPrefixAndKeepsNewSuffix)
{
	TempDir temp("uam-acp-replay-suffix");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message user;
	user.role = MessageRole::User;
	user.content = "First prompt";
	user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(user));
	Message assistant;
	assistant.role = MessageRole::Assistant;
	assistant.content = "OK";
	assistant.created_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(std::move(assistant));
	Message next_user;
	next_user.role = MessageRole::User;
	next_user.content = "Second prompt";
	next_user.created_at = "2026-01-01T00:00:02.000Z";
	chat.messages.push_back(std::move(next_user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 2;
	uam::AcpReplayUpdateState replay;
	replay.session_update = "agent_message_chunk";
	replay.text = "OK";
	session->load_history_replay_updates.push_back(std::move(replay));
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"OK\n\nSecond answer"}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(app.chats.front().messages[3].content, std::string("Second answer"));
}

UAM_TEST(AcpThoughtsPersistOnAssistantMessage)
{
	TempDir temp("uam-acp-thought-persist");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message user;
	user.role = MessageRole::User;
	user.content = "Please inspect this.";
	user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 0;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_thought_chunk","content":{"type":"text","text":"Need to inspect the file first."}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Done."}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.chats.front().messages[1].thoughts, std::string("Need to inspect the file first."));
	UAM_ASSERT_EQ(app.chats.front().messages[1].content, std::string("Done."));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][1].value("thoughts", ""), std::string("Need to inspect the file first."));
}

UAM_TEST(AcpToolCallsPersistOnAssistantMessage)
{
	TempDir temp("uam-acp-tool-persist");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message user;
	user.role = MessageRole::User;
	user.content = "Please read this file.";
	user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 0;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = "session/prompt";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call","toolCallId":"tool-1","title":"Read file","kind":"read","status":"completed","content":{"type":"text","text":"file contents"}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(1));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(2));
	const Message& assistant = app.chats.front().messages[1];
	UAM_ASSERT_EQ(assistant.role, MessageRole::Assistant);
	UAM_ASSERT_EQ(assistant.content, std::string(""));
	UAM_ASSERT_EQ(assistant.tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(assistant.tool_calls[0].id, std::string("tool-1"));
	UAM_ASSERT_EQ(assistant.tool_calls[0].name, std::string("Read file"));
	UAM_ASSERT_EQ(assistant.tool_calls[0].status, std::string("completed"));
	UAM_ASSERT_EQ(assistant.tool_calls[0].result_text, std::string("file contents"));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][1]["toolCalls"][0].value("title", ""), std::string("Read file"));
}

UAM_TEST(AcpLoadHistoryReplaySuppressesHistoricalThoughts)
{
	TempDir temp("uam-acp-thought-replay");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message first_user;
	first_user.role = MessageRole::User;
	first_user.content = "First prompt";
	first_user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(first_user));
	Message first_assistant;
	first_assistant.role = MessageRole::Assistant;
	first_assistant.content = "Previous answer";
	first_assistant.thoughts = "Old thought";
	first_assistant.created_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(std::move(first_assistant));
	Message second_user;
	second_user.role = MessageRole::User;
	second_user.content = "Second prompt";
	second_user.created_at = "2026-01-01T00:00:02.000Z";
	chat.messages.push_back(std::move(second_user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 2;
	uam::AcpReplayUpdateState user_replay;
	user_replay.session_update = "user_message_chunk";
	user_replay.text = "First prompt";
	session->load_history_replay_updates.push_back(std::move(user_replay));
	uam::AcpReplayUpdateState thought_replay;
	thought_replay.session_update = "agent_thought_chunk";
	thought_replay.text = "Old thought";
	session->load_history_replay_updates.push_back(std::move(thought_replay));
	uam::AcpReplayUpdateState assistant_replay;
	assistant_replay.session_update = "agent_message_chunk";
	assistant_replay.text = "Previous answer";
	session->load_history_replay_updates.push_back(std::move(assistant_replay));
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"user_message_chunk","content":{"type":"text","text":"First prompt"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_thought_chunk","content":{"type":"text","text":"Old thought"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Previous answer"}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(3));
	UAM_ASSERT(raw_session->turn_events.empty());

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_thought_chunk","content":{"type":"text","text":"New thought"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"New answer"}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(app.chats.front().messages[3].thoughts, std::string("New thought"));
	UAM_ASSERT_EQ(app.chats.front().messages[3].content, std::string("New answer"));
}

UAM_TEST(GeminiCliCompatibilityAcceptsCurrentStableVersions)
{
	UAM_ASSERT_EQ(std::string(uam::PreferredGeminiCliVersion()), std::string("0.38.1"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.38.1"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.36.0"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.39.0"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.30.0"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("not-a-version"));
}

UAM_TEST(GeminiPromptClassifierStripsAnsiAndDetectsPrompt)
{
	const std::string output = "\x1b[33mThinking...\x1b[0m\r\n\xe2\x94\x82 > Type your message or @path\r\n";
	UAM_ASSERT(GeminiCliRecentOutputIndicatesInputPrompt(output));

	const std::string stripped = StripTerminalControlSequencesForLifecycle("\x1b[31mhello\x1b[0m\b!");
	UAM_ASSERT_EQ(stripped, std::string("hell!"));
	UAM_ASSERT(!GeminiCliRecentOutputIndicatesInputPrompt("tool output is still streaming\nno prompt yet"));
}

UAM_TEST(CliLifecycleTransitionsDriveBackgroundShutdownEligibility)
{
	uam::AppState app;
	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.frontend_chat_id = "chat-1";
	terminal.attached_chat_id = "chat-1";
	terminal.attached_session_id = "native-1";

	MarkCliTerminalTurnBusy(terminal);
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Busy);
	UAM_ASSERT_EQ(terminal.turn_state, uam::CliTerminalTurnState::Busy);
	UAM_ASSERT(terminal.generation_in_progress);
	terminal.last_idle_confirmed_time_s = 1.0;
	UAM_ASSERT(!IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 121.0));

	MarkCliTerminalTurnIdle(terminal);
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Idle);
	UAM_ASSERT_EQ(terminal.turn_state, uam::CliTerminalTurnState::Idle);
	UAM_ASSERT(!terminal.generation_in_progress);
	terminal.last_idle_confirmed_time_s = 59.0;
	UAM_ASSERT(IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
	UAM_ASSERT(!IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-1", 120.0));
	UAM_ASSERT(!IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "native-1", 120.0));

	terminal.ui_attached = true;
	UAM_ASSERT(!IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
	terminal.ui_attached = false;

	PendingRuntimeCall pending;
	pending.chat_id = "chat-1";
	app.pending_calls.push_back(std::move(pending));
	UAM_ASSERT(!IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
}

UAM_TEST(CliTerminalIdentitySeparatesFrontendChatAndNativeSession)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-local";
	chat.native_session_id = "native-session";
	app.chats.push_back(chat);

	uam::CliTerminalState terminal;
	terminal.frontend_chat_id = "chat-local";
	terminal.attached_chat_id = "chat-local";
	terminal.attached_session_id = "native-session";

	UAM_ASSERT(CliTerminalMatchesChatId(terminal, "chat-local"));
	UAM_ASSERT(CliTerminalMatchesChatId(terminal, "native-session"));
	UAM_ASSERT(!CliTerminalMatchesChatId(terminal, "other-chat"));
	UAM_ASSERT_EQ(CliTerminalPrimaryChatId(terminal), std::string("chat-local"));
	UAM_ASSERT_EQ(CliTerminalSyncTargetId(terminal), std::string("native-session"));
	UAM_ASSERT_EQ(FindChatIndexForCliTerminal(app, terminal), 0);
}

UAM_TEST(FolderLifecycleKeepsWorkspaceRootsMinimal)
{
	TempDir temp("uam-folders");
	uam::AppState app;
	app.data_root = temp.root;
	ChatDomainService().EnsureDefaultFolder(app);

	std::string created_id;
	UAM_ASSERT(CreateFolder(app, "Project", temp.root.string(), &created_id));
	UAM_ASSERT(!created_id.empty());
	UAM_ASSERT(ChatDomainService().FindFolderById(app, created_id) != nullptr);

	const fs::path renamed_root = temp.root / "renamed";
	fs::create_directories(renamed_root);
	UAM_ASSERT(RenameFolderById(app, created_id, "Renamed", renamed_root.string()));
	const ChatFolder* renamed = ChatDomainService().FindFolderById(app, created_id);
	UAM_ASSERT(renamed != nullptr);
	UAM_ASSERT_EQ(renamed->title, std::string("Renamed"));
	UAM_ASSERT_EQ(renamed->directory, renamed_root.string());

	ChatSession folder_chat;
	folder_chat.id = "chat-in-folder";
	folder_chat.provider_id = "gemini-cli";
	folder_chat.folder_id = created_id;
	folder_chat.title = "Folder chat";
	folder_chat.created_at = "2026-01-01T00:00:00.000Z";
	folder_chat.updated_at = "2026-01-01T00:00:00.000Z";

	ChatSession general_chat;
	general_chat.id = "chat-in-general";
	general_chat.provider_id = "gemini-cli";
	general_chat.folder_id = uam::constants::kDefaultFolderId;
	general_chat.title = "General chat";
	general_chat.created_at = "2026-01-01T00:00:00.000Z";
	general_chat.updated_at = "2026-01-01T00:00:00.000Z";

	app.chats.push_back(folder_chat);
	app.chats.push_back(general_chat);
	app.selected_chat_index = 0;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, folder_chat));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, general_chat));

	const fs::path folder_chat_file = AppPaths::UamChatFilePath(temp.root, folder_chat.id);
	const fs::path general_chat_file = AppPaths::UamChatFilePath(temp.root, general_chat.id);
	UAM_ASSERT(fs::exists(folder_chat_file));
	UAM_ASSERT(fs::exists(general_chat_file));

	UAM_ASSERT(DeleteFolderById(app, created_id));
	UAM_ASSERT(ChatDomainService().FindFolderById(app, created_id) == nullptr);
	UAM_ASSERT(ChatDomainService().FindFolderById(app, uam::constants::kDefaultFolderId) != nullptr);
	UAM_ASSERT(ChatDomainService().FindChatIndexById(app, folder_chat.id) < 0);
	UAM_ASSERT(ChatDomainService().FindChatIndexById(app, general_chat.id) >= 0);
	UAM_ASSERT_EQ(app.chats[ChatDomainService().FindChatIndexById(app, general_chat.id)].folder_id, std::string(uam::constants::kDefaultFolderId));
	UAM_ASSERT(fs::exists(renamed_root));
	UAM_ASSERT(!fs::exists(folder_chat_file));
	UAM_ASSERT(fs::exists(general_chat_file));
}

UAM_TEST(DeleteFolderRefreshesRememberedSelectionToFallbackChat)
{
	TempDir temp("uam-folder-delete-selection");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.remember_last_chat = true;
	ChatDomainService().EnsureDefaultFolder(app);

	std::string created_id;
	UAM_ASSERT(CreateFolder(app, "Project", temp.root.string(), &created_id));

	ChatSession folder_chat;
	folder_chat.id = "chat-in-folder";
	folder_chat.provider_id = "gemini-cli";
	folder_chat.folder_id = created_id;
	folder_chat.title = "Folder chat";
	folder_chat.created_at = "2026-01-01T00:00:00.000Z";
	folder_chat.updated_at = "2026-01-01T00:00:00.000Z";

	ChatSession fallback_chat;
	fallback_chat.id = "chat-fallback";
	fallback_chat.provider_id = "gemini-cli";
	fallback_chat.folder_id = uam::constants::kDefaultFolderId;
	fallback_chat.title = "Fallback chat";
	fallback_chat.created_at = "2026-01-01T00:00:00.000Z";
	fallback_chat.updated_at = "2026-01-01T00:00:00.000Z";

	app.chats.push_back(folder_chat);
	app.chats.push_back(fallback_chat);
	app.selected_chat_index = 0;
	ChatDomainService().RefreshRememberedSelection(app);
	UAM_ASSERT_EQ(app.settings.last_selected_chat_id, std::string("chat-in-folder"));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, folder_chat));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, fallback_chat));

	UAM_ASSERT(DeleteFolderById(app, created_id));
	UAM_ASSERT_EQ(app.selected_chat_index, 0);
	UAM_ASSERT_EQ(app.chats[static_cast<std::size_t>(app.selected_chat_index)].id, std::string("chat-fallback"));
	UAM_ASSERT_EQ(app.settings.last_selected_chat_id, std::string("chat-fallback"));
}

UAM_TEST(DeleteFolderBlocksWhenContainedChatIsRunning)
{
	TempDir temp("uam-folder-pending-delete");
	uam::AppState app;
	app.data_root = temp.root;
	ChatDomainService().EnsureDefaultFolder(app);

	std::string created_id;
	UAM_ASSERT(CreateFolder(app, "Project", temp.root.string(), &created_id));

	ChatSession folder_chat;
	folder_chat.id = "chat-running";
	folder_chat.provider_id = "gemini-cli";
	folder_chat.folder_id = created_id;
	folder_chat.title = "Running chat";
	app.chats.push_back(folder_chat);

	PendingRuntimeCall call;
	call.chat_id = folder_chat.id;
	app.pending_calls.push_back(std::move(call));

	UAM_ASSERT(!DeleteFolderById(app, created_id));
	UAM_ASSERT(ChatDomainService().FindFolderById(app, created_id) != nullptr);
	UAM_ASSERT(ChatDomainService().FindChatIndexById(app, folder_chat.id) >= 0);
}

UAM_TEST(DataRootLockRejectsSecondWriter)
{
	TempDir temp("uam-data-root-lock");
	std::string first_error;
	auto first_lock = PlatformServicesFactory::Instance().process_service.TryAcquireDataRootLock(temp.root, &first_error);
	UAM_ASSERT(first_lock != nullptr);
	UAM_ASSERT(first_error.empty());

	std::string second_error;
	auto second_lock = PlatformServicesFactory::Instance().process_service.TryAcquireDataRootLock(temp.root, &second_error);
	UAM_ASSERT(second_lock == nullptr);
	UAM_ASSERT(!second_error.empty());

	first_lock.reset();
	second_error.clear();
	second_lock = PlatformServicesFactory::Instance().process_service.TryAcquireDataRootLock(temp.root, &second_error);
	UAM_ASSERT(second_lock != nullptr);
}

UAM_TEST(CreateFolderGeneratesUniqueIds)
{
	TempDir temp("uam-folder-ids");
	uam::AppState app;
	app.data_root = temp.root;
	ChatDomainService().EnsureDefaultFolder(app);

	std::unordered_set<std::string> ids;
	for (int i = 0; i < 128; ++i)
	{
		std::string created_id;
		UAM_ASSERT(CreateFolder(app, "Project " + std::to_string(i), temp.root.string(), &created_id));
		UAM_ASSERT(!created_id.empty());
		UAM_ASSERT(ids.insert(created_id).second);
	}
}

UAM_TEST(MoveChatToFolderHandlesMissingWorkspacePaths)
{
	TempDir temp("uam-move-missing-workspace");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatDomainService().EnsureDefaultFolder(app);

	std::string target_folder_id;
	const fs::path missing_target = temp.root / "missing-target";
	UAM_ASSERT(CreateFolder(app, "Missing Target", missing_target.string(), &target_folder_id));

	ChatSession chat;
	chat.id = "chat-missing-workspace";
	chat.provider_id = "gemini-cli";
	chat.folder_id = uam::constants::kDefaultFolderId;
	chat.title = "Missing workspace";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:00.000Z";
	chat.workspace_directory = (temp.root / "missing-source").string();
	app.chats.push_back(chat);

	UAM_ASSERT(ChatHistorySyncService().MoveChatToFolder(app, app.chats.back(), target_folder_id));
	UAM_ASSERT_EQ(app.chats.back().folder_id, target_folder_id);
	UAM_ASSERT_EQ(app.chats.back().workspace_directory, missing_target.string());
}

UAM_TEST(ImportDiscoveryDoesNotRecreateFolderForEmptyNativeSource)
{
	TempDir temp("uam-import-empty-source");
	const fs::path gemini_home = temp.root / "gemini-home";
	const fs::path data_root = temp.root / "data";
	const fs::path workspace_root = temp.root / "workspace";
	const fs::path source_root = gemini_home / "tmp" / "workspace-source";
	const fs::path source_chats = source_root / "chats";
	fs::create_directories(workspace_root);
	fs::create_directories(source_chats);
	UAM_ASSERT(uam::io::WriteTextFile(source_root / ".project_root", workspace_root.string()));

	ScopedEnvVar gemini_home_env("GEMINI_CLI_HOME", gemini_home.string());
	uam::AppState app;
	app.data_root = data_root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatDomainService().EnsureDefaultFolder(app);
	UAM_ASSERT(ChatFolderStore::Save(data_root, app.folders));

	const ChatHistorySyncService::ImportResult result = ChatHistorySyncService().ImportAllNativeChatsByDiscovery(app, false);
	UAM_ASSERT_EQ(result.total_count, 0);
	UAM_ASSERT_EQ(result.imported_count, 0);
	UAM_ASSERT(ChatRepository::LoadLocalChats(data_root).empty());
	for (const ChatFolder& folder : app.folders)
	{
		UAM_ASSERT(!FolderDirectoryMatches(folder.directory, workspace_root));
	}
	for (const ChatFolder& folder : ChatFolderStore::Load(data_root))
	{
		UAM_ASSERT(!FolderDirectoryMatches(folder.directory, workspace_root));
	}
}

#if !defined(_WIN32)
UAM_TEST(ImportFallsBackToDefaultFolderWhenNewFolderMetadataSaveFails)
{
	TempDir temp("uam-import-folder-save");
	const fs::path gemini_home = temp.root / "gemini-home";
	const fs::path data_root = temp.root / "data";
	const fs::path chats_root = data_root / "chats";
	const fs::path workspace_root = temp.root / "workspace";
	const fs::path source_root = gemini_home / "tmp" / "workspace-source";
	const fs::path source_chats = source_root / "chats";
	fs::create_directories(chats_root);
	fs::create_directories(workspace_root);
	fs::create_directories(source_chats);
	UAM_ASSERT(uam::io::WriteTextFile(source_root / ".project_root", workspace_root.string()));
	UAM_ASSERT(uam::io::WriteTextFile(source_chats / "native-1.json", R"({
  "sessionId": "native-1",
  "startTime": "2026-01-01T00:00:00.000Z",
  "lastUpdated": "2026-01-01T00:00:02.000Z",
  "messages": [
    {"type": "user", "timestamp": "2026-01-01T00:00:01.000Z", "content": "hello"}
  ]
})"));

	ScopedEnvVar gemini_home_env("GEMINI_CLI_HOME", gemini_home.string());
	uam::AppState app;
	app.data_root = data_root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatDomainService().EnsureDefaultFolder(app);

	ScopedDirectoryNoWrite block_folder_write(data_root);
	const ChatHistorySyncService::ImportResult result = ChatHistorySyncService().ImportAllNativeChatsByDiscovery(app, false);
	UAM_ASSERT_EQ(result.total_count, 1);
	UAM_ASSERT_EQ(result.imported_count, 1);

	const std::vector<ChatSession> imported = ChatRepository::LoadLocalChats(data_root);
	UAM_ASSERT_EQ(imported.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(imported.front().id, std::string("native-1"));
	UAM_ASSERT_EQ(imported.front().folder_id, std::string(uam::constants::kDefaultFolderId));
	UAM_ASSERT_EQ(imported.front().workspace_directory, workspace_root.string());
	for (const ChatFolder& folder : app.folders)
	{
		UAM_ASSERT(!FolderDirectoryMatches(folder.directory, workspace_root));
	}
}
#endif

UAM_TEST(NativeGeminiHistoryLoadCapsAreBounded)
{
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxFileBytes() > 0);
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxMessages() > 0);
}

UAM_TEST(GeminiHistoryParseFileHonorsCaps)
{
	TempDir temp("uam-gemini-history-caps");
	const fs::path history_file = temp.root / "session.json";
	UAM_ASSERT(uam::io::WriteTextFile(history_file, R"({
  "sessionId": "native-capped",
  "startTime": "2026-01-01T00:00:00.000Z",
  "lastUpdated": "2026-01-01T00:00:02.000Z",
  "messages": [
    {"type": "user", "timestamp": "2026-01-01T00:00:01.000Z", "content": "one"},
    {"type": "model", "timestamp": "2026-01-01T00:00:02.000Z", "content": "two"}
  ]
})"));

	GeminiJsonHistoryStoreOptions file_cap;
	file_cap.max_file_bytes = 1;
	UAM_ASSERT(!GeminiJsonHistoryStore::ParseFile(history_file, ProviderProfileStore::DefaultGeminiProfile(), file_cap).has_value());

	GeminiJsonHistoryStoreOptions message_cap;
	message_cap.max_messages = 1;
	const auto parsed = GeminiJsonHistoryStore::ParseFile(history_file, ProviderProfileStore::DefaultGeminiProfile(), message_cap);
	UAM_ASSERT(parsed.has_value());
	UAM_ASSERT_EQ(parsed->messages.size(), static_cast<std::size_t>(1));
}

UAM_TEST(GeminiHistoryPreservesThoughtOnlyAndToolOnlyMessages)
{
	TempDir temp("uam-gemini-history-empty-content");
	const fs::path history_file = temp.root / "session.json";
	UAM_ASSERT(uam::io::WriteTextFile(history_file, R"({
  "sessionId": "native-rich",
  "startTime": "2026-01-01T00:00:00.000Z",
  "lastUpdated": "2026-01-01T00:00:03.000Z",
  "messages": [
    {"type": "model", "timestamp": "2026-01-01T00:00:01.000Z", "content": "", "thoughts": [{"text": "Only thought"}]},
    {"type": "model", "timestamp": "2026-01-01T00:00:02.000Z", "content": "", "toolCalls": [{"id": "tool-1", "name": "Read file", "status": "completed", "args": {"path": "file.txt"}, "result": {"text": "file contents"}}]},
    {"type": "model", "timestamp": "2026-01-01T00:00:03.000Z", "content": ""}
  ]
})"));

	const auto parsed = GeminiJsonHistoryStore::ParseFile(history_file, ProviderProfileStore::DefaultGeminiProfile());
	UAM_ASSERT(parsed.has_value());
	UAM_ASSERT_EQ(parsed->messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(parsed->messages[0].content, std::string(""));
	UAM_ASSERT_EQ(parsed->messages[0].thoughts, std::string("Only thought"));
	UAM_ASSERT_EQ(parsed->messages[1].content, std::string(""));
	UAM_ASSERT_EQ(parsed->messages[1].tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(parsed->messages[1].tool_calls[0].id, std::string("tool-1"));
	UAM_ASSERT_EQ(parsed->messages[1].tool_calls[0].name, std::string("Read file"));
	UAM_ASSERT_EQ(parsed->messages[1].tool_calls[0].result_text, std::string("file contents"));
}

#if defined(__APPLE__)
UAM_TEST(MacTerminalRejectsInvalidWorkingDirectory)
{
	TempDir temp("uam-mac-terminal-workdir");
	const fs::path file_path = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(file_path, "not a directory"));

	uam::CliTerminalState terminal;
	terminal.rows = 24;
	terminal.cols = 80;
	std::string error;
	const bool started = PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, file_path, {"/bin/echo", "hello"}, &error);
	if (started)
	{
		PlatformServicesFactory::Instance().terminal_runtime.StopCliTerminalProcess(terminal, true);
		PlatformServicesFactory::Instance().terminal_runtime.CloseCliTerminalHandles(terminal);
	}
	UAM_ASSERT(!started);
	UAM_ASSERT(!error.empty());
}

UAM_TEST(MacTerminalFastStopTerminatesProcessGroupChildren)
{
	TempDir temp("uam-mac-terminal-process-group");
	uam::CliTerminalState terminal;
	terminal.rows = 24;
	terminal.cols = 80;
	std::string error;
	const bool started = PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, temp.root, {"/bin/sh", "-c", "sleep 30"}, &error);
	UAM_ASSERT(started);
	UAM_ASSERT(terminal.child_pid > 0);
	const pid_t process_group_id = terminal.child_pid;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	PlatformServicesFactory::Instance().terminal_runtime.StopCliTerminalProcess(terminal, true);
	PlatformServicesFactory::Instance().terminal_runtime.CloseCliTerminalHandles(terminal);
	UAM_ASSERT_EQ(terminal.child_pid, static_cast<pid_t>(-1));

	bool process_group_gone = false;
	for (int attempt = 0; attempt < 20; ++attempt)
	{
		errno = 0;
		if (kill(-process_group_id, 0) != 0 && errno == ESRCH)
		{
			process_group_gone = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}

	UAM_ASSERT(process_group_gone);
}
#endif

int main()
{
	int failures = 0;
	for (const TestCase& test : Registry())
	{
		try
		{
			test.fn();
			std::cout << "[PASS] " << test.name << '\n';
		}
		catch (const std::exception& ex)
		{
			++failures;
			std::cerr << "[FAIL] " << test.name << ": " << ex.what() << '\n';
		}
	}

	if (failures != 0)
	{
		std::cerr << failures << " test(s) failed.\n";
		return 1;
	}

	return 0;
}
