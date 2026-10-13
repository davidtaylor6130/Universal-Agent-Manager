#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <stdexcept>

namespace uam::usage
{
	/// <summary>Owned immutable JSON snapshots; only the serialized usage worker accesses this cache.</summary>
	class HistoryTransport
	{
	public:
		static constexpr std::size_t kResponseBytes = 2 * 1024 * 1024;
		static constexpr std::size_t kChunkBytes = 256 * 1024;
		using Clock = std::chrono::steady_clock;

		nlohmann::json Start(int browser, std::string owner, std::string refresh, std::string body, Clock::time_point now = Clock::now())
		{
			Prune(now);
			m_snapshots.erase(browser);
			if (body.size() <= kResponseBytes) return nlohmann::json::parse(body);
			while (m_snapshots.size() >= 8 || (!m_snapshots.empty() && RetainedBytes() + body.size() > 64 * 1024 * 1024)) m_snapshots.erase(m_snapshots.begin());
			Snapshot snapshot{std::move(owner), std::move(refresh), std::to_string(++m_generation), std::move(body), 0, now + std::chrono::seconds(120)};
			m_snapshots.emplace(browser, std::move(snapshot));
			return Page(browser, m_snapshots.at(browser).owner, m_snapshots.at(browser).id, 0, now);
		}

		nlohmann::json Page(int browser, const std::string& owner, const std::string& id, std::size_t offset, Clock::time_point now = Clock::now())
		{
			Prune(now);
			const std::map<int, Snapshot>::iterator found = m_snapshots.find(browser);
			if (found == m_snapshots.end() || found->second.owner != owner || found->second.id != id || found->second.next != offset)
				throw std::runtime_error("Usage snapshot expired or changed. Refresh to try again.");
			Snapshot& snapshot = found->second;
			std::size_t end = std::min(offset + kChunkBytes, snapshot.body.size());
			while (end < snapshot.body.size() && (static_cast<unsigned char>(snapshot.body[end]) & 0xc0) == 0x80) --end;
			const bool complete = end == snapshot.body.size();
			nlohmann::json page{{"usagePage", 1}, {"snapshotId", snapshot.id}, {"refreshId", snapshot.refresh}, {"offset", offset},
				{"nextOffset", complete ? nlohmann::json(nullptr) : nlohmann::json(end)}, {"totalBytes", snapshot.body.size()}, {"chunk", snapshot.body.substr(offset, end - offset)}};
			if (page.dump().size() > kResponseBytes) throw std::runtime_error("Usage page exceeds the bridge byte limit.");
			snapshot.next = end;
			if (complete) m_snapshots.erase(found);
			return page;
		}

	private:
		struct Snapshot
		{
			std::string owner, refresh, id, body;
			std::size_t next;
			Clock::time_point expires;
		};
		void Prune(Clock::time_point now)
		{
			for (std::map<int, Snapshot>::iterator it = m_snapshots.begin(); it != m_snapshots.end();)
			{
				if (it->second.expires <= now) it = m_snapshots.erase(it); else ++it;
			}
		}
		std::size_t RetainedBytes() const
		{
			std::size_t bytes = 0;
			for (const std::pair<const int, Snapshot>& entry : m_snapshots) bytes += entry.second.body.size();
			return bytes;
		}
		std::map<int, Snapshot> m_snapshots;
		std::uint64_t m_generation = 0;
	};
}
