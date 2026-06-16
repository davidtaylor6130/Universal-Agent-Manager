#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{

inline constexpr std::size_t kTerminalPromptScanLimit = 8192;
inline constexpr auto kGeminiExactPromptLines = std::to_array<std::string_view>({">", ">_"});
inline constexpr auto kGeminiPromptCueTexts = std::to_array<std::string_view>({"Type your message", "? for shortcuts"});
inline constexpr auto kCodexPromptMarkers = std::to_array<std::string_view>({"\xE2\x80\xBA", "> "});
inline constexpr auto kCodexPromptCueTexts = std::to_array<std::string_view>({"Send", "message", "for shortcuts"});
inline constexpr int kGeminiPromptRecentLineLimit = 6;

std::size_t CountTerminalLineBreaks(std::string_view value);
std::vector<std::string> SplitTerminalLines(std::string_view value);
bool SkipTerminalEscapeSequence(std::string_view input, std::size_t& index);
std::string StripTerminalControlSequencesForLifecycle(std::string_view input);
std::string NormalizeGeminiPromptLine(std::string_view line);
std::string RecentTerminalPromptScanText(std::string_view recent_output);
bool GeminiCliRecentOutputIndicatesInputPrompt(std::string_view recent_output);
bool CodexCliRecentOutputIndicatesInputPrompt(std::string_view recent_output);
bool FallbackCliRecentOutputIndicatesInputPrompt(std::string_view recent_output);

} // namespace uam
