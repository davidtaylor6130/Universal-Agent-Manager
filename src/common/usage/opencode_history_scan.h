#pragma once

#include "common/usage/transcript_scan.h"

namespace uam::usage
{
	/// <summary>Worker-owned bounded read-only scan of OpenCode's legacy and current message tables.</summary>
	class OpenCodeHistoryScan
	{
	public:
		void Reset();
		ScanResult Refresh(const std::filesystem::path& path, std::int64_t start_ms, std::int64_t end_ms, const std::function<bool()>& cancelled = {});
		void ConsumeRecords() { m_recordBase += m_records.size(); m_records.clear(); }
		std::size_t RecordBase() const { return m_recordBase; }
		const std::vector<Record>& Records() const { return m_records; }
		std::size_t InvalidRows() const { return m_invalidRows; }
		std::size_t SuppressedForkRecords() const { return m_suppressedCopies; }

	private:
		std::filesystem::path m_path;
		std::int64_t m_startMs = -1, m_endMs = -1;
		std::size_t m_table = 0;
		bool m_sawTable = false;
		std::int64_t m_cursor = 0;
		std::size_t m_invalidRows = 0;
		std::size_t m_suppressedCopies = 0;
		std::vector<Record> m_records;
		std::size_t m_recordBase = 0;
	};
}
