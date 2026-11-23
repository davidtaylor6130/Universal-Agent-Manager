#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace uam::diagnostics
{

inline constexpr std::size_t kMaxFileBytes = 5 * 1024 * 1024;
inline constexpr std::size_t kMaxEntryBytes = 16 * 1024;
/// Storage audit lines outlive the busy diagnostic log: kept for this many days, within this byte cap.
inline constexpr int kAuditRetentionDays = 90;
inline constexpr std::size_t kMaxAuditFileBytes = 1024 * 1024;

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

/// <summary>Opens the storage audit log beside an active file log. The worker drops lines older than
/// retention_days at start, daily and before the file would exceed kMaxAuditFileBytes (then oldest first).
/// Writes an "Audit log opened" marker; StopFileLog writes the matching "closed" marker.</summary>
bool StartAuditLog(const std::filesystem::path& path, std::string& error, int retention_days = kAuditRetentionDays);
/// <summary>Queues one storage or deletion audit entry to both the audit log and the ordinary log.
/// Audit entries are never dropped for queue pressure. Without an audit log it behaves like Write.</summary>
void WriteAudit(std::string_view message);
/// <summary>Returns retained audit text: lines whose leading UTC timestamp is older than retention_days
/// before now are dropped, then the oldest lines until the text fits within max_bytes.</summary>
std::string CompactAuditText(std::string_view text, std::chrono::system_clock::time_point now, int retention_days, std::size_t max_bytes);

} // namespace uam::diagnostics
