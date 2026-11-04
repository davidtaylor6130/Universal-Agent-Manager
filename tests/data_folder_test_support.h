#pragma once
#include "common/platform/data_root_lock.h"
#if !defined(UAM_DATA_FOLDER_STANDALONE)
#include "test_harness.h"
inline const IPlatformProcessService& DataFolderTestProcess()
{
	return PlatformServicesFactory::Instance().process_service;
}
#else
#include "common/platform/platform_services.h"
#include "common/config/line_value_codec.h"
#include "common/config/settings_store.h"
#include "common/platform/retained_directory_identity.h"
#include "common/utils/io_utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <sstream>
#if !defined(_WIN32)
#include <sys/file.h>
#endif
namespace fs = std::filesystem;
namespace uam_test
{
	struct TestCase
	{
		std::string name;
		std::function<void()> fn;
	};

	inline std::vector<TestCase>& Registry()
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

	inline void Fail(const char* expr, const char* file, int line)
	{
		std::ostringstream out;
		out << file << ':' << line << ": assertion failed: " << expr;
		throw TestFailure(out.str());
	}

	template <typename T, typename U> inline void AssertEq(const T& actual, const U& expected, const char* actual_expr, const char* expected_expr, const char* file, int line)
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
			root = fs::temp_directory_path() / (prefix + "-" + uam::time::SteadyEpochNanosecondsTokenNow());
			fs::create_directories(root);
		}

		~TempDir()
		{
			std::error_code ec;
			fs::remove_all(root, ec);
		}
	};

	inline std::string ReadFile(const fs::path& path)
	{
		return uam::io::ReadTextFile(path);
	}

} // namespace uam_test
// clang-format off
#define UAM_TEST(name) \
	static void name(); \
	static uam_test::RegisterTest register_##name(#name, name); \
	static void name()

#define UAM_ASSERT(expr) \
	do \
	{ \
		if (!(expr)) \
			uam_test::Fail(#expr, __FILE__, __LINE__); \
	} while (false)

#define UAM_ASSERT_EQ(actual, expected) \
	do \
	{ \
		uam_test::AssertEq((actual), (expected), #actual, #expected, __FILE__, __LINE__); \
	} while (false)

// clang-format on

// Test-only adapter: no command, provider, terminal, shell or device activity is possible.
class DataFolderFixtureProcess final : public IPlatformProcessService
{
  public:
	bool SupportsDetachedProcesses() const override
	{
		return false;
	}
	bool PopulateLocalTime(std::time_t, std::tm*) const override
	{
		return false;
	}
	std::string BuildShellCommandWithWorkingDirectory(const fs::path&, const std::string&) const override
	{
		return {};
	}
	bool CaptureCommandOutput(const std::string&, std::string*, int*, std::string*) const override
	{
		return false;
	}
	int NormalizeCapturedCommandExitCode(int status) const override
	{
		return status;
	}
	ProcessExecutionResult ExecuteCommand(const std::string&, int, std::stop_token) const override
	{
		return {};
	}
	bool StartStdioProcess(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>&, std::string*, const std::vector<std::pair<std::string, std::string>>&) const override
	{
		return false;
	}
	bool StartStdioProcessWithInput(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>&, std::string_view, std::string*, const std::vector<std::pair<std::string, std::string>>&) const override
	{
		return false;
	}
	void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields&) const override
	{
	}
	bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields&, const char*, std::size_t, std::string*) const override
	{
		return false;
	}
	void CloseStdioProcessInput(uam::platform::StdioProcessPlatformFields&) const override
	{
	}
	void TerminateStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override
	{
	}
	void StopStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override
	{
	}
	std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string*) const override
	{
		return -1;
	}
	std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string*) const override
	{
		return -1;
	}
	bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields&, int*) const override
	{
		return false;
	}
	fs::path ResolveCurrentExecutablePath() const override
	{
		return {};
	}
	uintmax_t NativeGeminiSessionMaxFileBytes() const override
	{
		return 0;
	}
	std::size_t NativeGeminiSessionMaxMessages() const override
	{
		return 0;
	}
	std::string GenerateUuid() const override
	{
		static std::atomic<unsigned> next{0};
		return "fixture-" + std::to_string(++next);
	}
	bool LaunchShellAt(const fs::path&, std::string*) const override
	{
		return false;
	}
	void SetKeepSystemAwake(bool) const override
	{
	}
	bool EmbeddedBrowserUsesMockKeychain() const override
	{
		return false;
	}
	std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const fs::path& root, std::string* error = nullptr) const override
	{
		return uam::platform::AnchoredDataRootLock::TryAcquire(root, error);
	}

};
inline const IPlatformProcessService& DataFolderTestProcess()
{
	static DataFolderFixtureProcess service;
	return service;
}
#endif
