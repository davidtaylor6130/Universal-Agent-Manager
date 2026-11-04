#include "../antigravity_native_import_test_support.h"

int main()
{
	std::size_t failures = 0;
	for (const auto& fixture : uam_test::Registry())
	{
		try
		{
			fixture.run();
			std::cout << "PASS " << fixture.name << '\n';
		}
		catch (const std::exception& error)
		{
			++failures;
			std::cerr << "FAIL " << fixture.name << ": " << error.what() << '\n';
		}
	}
	std::cout << uam_test::Registry().size() - failures << '/' << uam_test::Registry().size() << " parser fixtures passed\n";
	return failures ? 1 : 0;
}
