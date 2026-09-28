#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace uam::diagnostics
{

inline constexpr std::size_t kMaxFileBytes = 5 * 1024 * 1024;
inline constexpr std::size_t kMaxEntryBytes = 16 * 1024;

/// <summary>Enables desktop logging after acquiring the data-root lock. Keeps path and path.1.</summary>
bool StartFileLog(const std::filesystem::path& path, std::string& error);
/// <summary>Closes the file after desktop workers stop; subsequent diagnostics use stderr.</summary>
void StopFileLog();
/// <summary>Writes one bounded entry. File failures fall back to stderr until restart.</summary>
void Write(std::string_view message);

} // namespace uam::diagnostics
