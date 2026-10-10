#pragma once

#include "common/usage/transcript_usage.h"

#include <filesystem>
#include <functional>
#include <vector>

namespace uam::usage
{
	struct ScanOptions
	{
		std::size_t byte_budget = 32 * 1024 * 1024;
		std::size_t max_line_bytes = 8 * 1024 * 1024;
		std::size_t max_records = 100000;
	};

	struct ScanResult
	{
		std::string error;
		std::size_t bytes_read = 0;
		bool reset = false;
		bool complete = false;
		bool cancelled = false;
	};

	/// <summary>Worker-owned incremental JSONL cursor. Only newline-terminated events enter the usage cache.</summary>
	class TranscriptScan
	{
	public:
		explicit TranscriptScan(std::string provider_id);
		ScanResult Refresh(const std::filesystem::path& path, const ScanOptions& options = {}, const std::function<bool()>& cancelled = {});
		const std::vector<Record>& Records() const { return m_records; }
		std::size_t InvalidLines() const { return m_invalidLines; }
		std::size_t OversizedLines() const { return m_oversizedLines; }
		std::size_t SuppressedForkRecords() const { return m_codexState.suppressed_fork_records; }
		bool PendingTail() const { return !m_pendingLine.empty() || m_pendingOversized; }

	private:
		void Reset();
		bool CommitLine(const ScanOptions& options);
		std::string m_providerId;
		std::filesystem::path m_path;
		std::filesystem::file_time_type m_lastWriteTime{};
		std::uintmax_t m_seenSize = 0;
		std::uintmax_t m_readOffset = 0;
		std::string m_prefixGuard;
		std::string m_tailGuard;
		std::string m_pendingLine;
		bool m_pendingOversized = false;
		bool m_recordLimitReached = false;
		std::size_t m_invalidLines = 0;
		std::size_t m_oversizedLines = 0;
		CodexScanState m_codexState;
		std::vector<Record> m_records;
	};
}
