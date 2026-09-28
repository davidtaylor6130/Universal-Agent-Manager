#pragma once

#include <string>
#include <vector>

namespace uam::computer_use
{
	int RunIndependentComputerUse(const std::vector<std::string>& arguments);
	bool AttachIndependentComputerUseSocket(const std::vector<std::string>& arguments, std::string* error);
} // namespace uam::computer_use
