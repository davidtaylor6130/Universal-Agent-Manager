#include "common/platform/platform_services.h"

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <string_view>
#include <vector>

// Any accidental fallback to the real platform factory fails closed in this binary.
PlatformServices& PlatformServicesFactory::Instance()
{
	throw std::runtime_error("Real platform services are forbidden in bootstrap probe tests.");
}

// Finalize logging is outside the probe scope; the isolated binary does not build the log writer.
namespace uam::diagnostics
{
void Write(std::string_view) {}
}

namespace uam_test
{
	struct TestCase
	{
		std::string name;
		std::function<void()> run;
	};
	std::vector<TestCase>& Registry()
	{
		static std::vector<TestCase> tests;
		return tests;
	}
	struct RegisterTest
	{
		RegisterTest(std::string name, std::function<void()> run)
		{
			Registry().push_back({std::move(name), std::move(run)});
		}
	};
	void Require(bool value, const char* expression, int line)
	{
		if (!value)
			throw std::runtime_error("Line " + std::to_string(line) + ": " + expression);
	}
}

#define UAM_TEST(name) static void name(); static uam_test::RegisterTest register_##name(#name, name); static void name()
#define UAM_ASSERT(expression) uam_test::Require(static_cast<bool>(expression), #expression, __LINE__)
#define UAM_ASSERT_EQ(actual, expected) UAM_ASSERT((actual) == (expected))
#include "runner_bootstrap_probe_cases.h"

int main(int argc, char** argv)
{
	if (argc == 2 && std::string_view(argv[1]) == "--list-tests")
	{
		for (const auto& test : uam_test::Registry())
			std::cout << test.name << '\n';
		return 0;
	}
	std::unordered_set<std::string> requested;
	for (int index = 1; index < argc; ++index) requested.insert(argv[index]);
	std::size_t passed = 0;
	bool failed = false;
	for (const auto& test : uam_test::Registry())
	{
		if (argc > 1 && requested.erase(test.name) == 0) continue;
		try
		{
			test.run();
			std::cout << "[PASS] " << test.name << '\n';
			++passed;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
			failed = true;
		}
	}
	if (!requested.empty())
	{
		std::cerr << "Unknown test name requested.\n";
		return 1;
	}
	std::cout << passed << " bootstrap probe cases passed.\n";
	return failed ? 1 : 0;
}
