#pragma once

#include <string>
#include <vector>

/// <summary>
/// Splits a user-provided command line into argv-style tokens.
/// On Windows this follows CommandLineToArgvW semantics so quoted paths stay intact.
/// </summary>
std::vector<std::string> SplitCommandLineWords(const std::string& value);
