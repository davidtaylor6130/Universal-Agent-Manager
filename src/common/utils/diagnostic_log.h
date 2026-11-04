#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace uam::diagnostics
{

inline constexpr std::size_t kMaxFileBytes = 5 * 1024 * 1024;
inline constexpr std::size_t kMaxEntryBytes = 16 * 1024;

/// <summary>Enables desktop logging after acquiring the data-root lock. Rotates oversized existing logs into path.1; the worker expires regular backups using the configured age, with zero disabling expiry.</summary>
bool StartFileLog(const std::filesystem::path& path, std::string& error, int backup_retention_days = 14);
/// <summary>Changes only regular backup age expiry; returns false while an expiry removal owns its lease.</summary>
bool ConfigureBackupRetention(int days);
/// <summary>Waits for queued writes and their flush; use only for explicit durability barriers.</summary>
void FlushFileLog();
/// <summary>Drains and closes the file after desktop workers stop; subsequent diagnostics use stderr.</summary>
void StopFileLog();
/// <summary>Queues one bounded entry without disk IO. A full queue drops oldest pending entries with a logged count.</summary>
void Write(std::string_view message);

} // namespace uam::diagnostics
