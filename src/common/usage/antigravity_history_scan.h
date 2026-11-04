#pragma once

#include "common/usage/antigravity_accounting.h"
#include "common/usage/transcript_scan.h"
#include <memory>

namespace uam::usage
{
	/// <summary>Owns a read-only SQLite snapshot across bounded worker batches; streams receipts or publishes completed legacy caches.</summary>
	class AntigravityHistoryScan
	{
	public:
		AntigravityHistoryScan();
		~AntigravityHistoryScan();
		ScanResult Refresh(const std::filesystem::path& path, const std::function<bool()>& cancelled = {}, const std::function<bool(const std::vector<Record>&, std::string&)>& sink = {});
		const std::vector<AntigravityUsageCandidate>& Candidates() const { return m_candidates; }
		std::size_t InvalidRows() const { return m_invalidRows; }
	private:
		struct State;
		std::unique_ptr<State> m_state;
		std::filesystem::path m_path;
		std::string m_revision;
		std::vector<AntigravityUsageCandidate> m_candidates;
		std::size_t m_invalidRows = 0;
	};
#if !UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
	// Builds without Antigravity list no Antigravity history source, so this scan never reads anything.
	struct AntigravityHistoryScan::State {};
	inline AntigravityHistoryScan::AntigravityHistoryScan() = default;
	inline AntigravityHistoryScan::~AntigravityHistoryScan() = default;
	inline ScanResult AntigravityHistoryScan::Refresh(const std::filesystem::path&, const std::function<bool()>&, const std::function<bool(const std::vector<Record>&, std::string&)>&)
	{
		ScanResult result;
		result.complete = true;
		return result;
	}
#endif
}
