#include "test_harness.h"

int main(int argc, char** argv)
{
	using namespace uam_test;
	if (argc == 2 && std::string_view(argv[1]) == "--list-tests")
	{
		for (const auto& test : Registry()) std::cout << test.name << '\n';
		return 0;
	}
	if (argc < 2)
	{
		std::cerr << "Pass exact test names or --list-tests; no implicit full inventory run.\n";
		return 2;
	}
	std::unordered_set<std::string> requested;
	for (int i = 1; i < argc; ++i)
	{
		if (!requested.insert(argv[i]).second || std::ranges::none_of(Registry(), [&](const auto& test) { return test.name == argv[i]; }))
		{
			std::cerr << "Unknown or duplicate test: " << argv[i] << '\n';
			return 2;
		}
	}
	std::size_t passed = 0;
	for (const auto& test : Registry())
	{
		if (!requested.contains(test.name)) continue;
		try
		{
			test.fn();
			++passed;
			std::cout << "[PASS] " << test.name << '\n';
		}
		catch (const std::exception& error) { std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n'; }
	}
	return passed == requested.size() ? 0 : 1;
}
