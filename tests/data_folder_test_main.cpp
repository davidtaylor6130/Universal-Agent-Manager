#include "data_folder_test_support.h"
int main(int argc, char** argv)
{
	unsigned failed = 0, executed = 0;
	for (const auto& test : uam_test::Registry())
	{
		if (argc > 1 && test.name != argv[1])
			continue;
		++executed;
		try
		{
			test.fn();
			std::cout << "PASS " << test.name << '\n';
		}
		catch (const std::exception& error)
		{
			++failed;
			std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
		}
	}
	std::cout << executed << " disposable cases, " << failed << " failed\n";
	return failed || !executed ? 1 : 0;
}
