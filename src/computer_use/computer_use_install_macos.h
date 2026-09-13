#pragma once

#include <filesystem>
#include <string>

namespace uam::computer_use
{
	std::filesystem::path PrepareStandaloneComputerUse(const std::filesystem::path& embedded_app, std::string* error);
}
