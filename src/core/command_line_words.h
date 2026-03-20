#pragma once

#include <string>
#include <vector>

// Splits a user-provided command line into argv-style tokens.
// On Windows this follows CommandLineToArgvW semantics so quoted
// paths like "C:\Program Files\tool.exe" stay intact.
std::vector<std::string> SplitCommandLineWords(const std::string& value);
