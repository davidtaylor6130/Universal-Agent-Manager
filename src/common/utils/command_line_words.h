#pragma once

#include <string>
#include <string_view>
#include <vector>

/// <summary>
/// Splits a user-provided command line into argv-style tokens.
/// On Windows this follows CommandLineToArgvW semantics so quoted paths stay intact.
/// </summary>
namespace uam::command_line
{
	std::vector<std::string> SplitWords(std::string_view value);
}
