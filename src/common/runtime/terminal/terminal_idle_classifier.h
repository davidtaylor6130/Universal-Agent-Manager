#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{

inline constexpr std::size_t kTerminalPromptScanLimit = 8192;

std::size_t CountTerminalLineBreaks(std::string_view value);
std::vector<std::string> SplitTerminalLines(std::string_view value);
bool SkipTerminalEscapeSequence(std::string_view input, std::size_t& index);
std::string StripTerminalControlSequencesForLifecycle(std::string_view input);
std::string RecentTerminalPromptScanText(std::string_view recent_output);

} // namespace uam
