#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <string>
#include <string_view>

namespace uam::cef
{

	/// Composer submissions seen by this app run, so a client retrying a send whose reply it lost cannot admit the input twice.
	/// ponytail: one in-memory FIFO across all chats; 256 entries far exceeds the sends of any retry window. A retry spanning an
	/// app restart is not deduplicated, because the restarted app shows the first attempt in the transcript before any resend.
	class SendIdempotencyWindow
	{
	  public:
		enum class Claim
		{
			New,
			InProgress,
			Admitted
		};

		Claim Begin(std::string_view chat_id, std::string_view id)
		{
			if (const auto entry = Find(chat_id, id); entry != m_entries.end())
				return entry->admitted ? Claim::Admitted : Claim::InProgress;
			if (m_entries.size() >= kCapacity)
				m_entries.pop_front();
			m_entries.push_back(Entry{std::string(chat_id), std::string(id), false});
			return Claim::New;
		}

		/// A refused or unsaved send forgets the id, so its retry is a fresh admission.
		void Settle(std::string_view chat_id, std::string_view id, bool admitted)
		{
			const auto entry = Find(chat_id, id);
			if (entry == m_entries.end())
				return;
			if (admitted)
				entry->admitted = true;
			else
				m_entries.erase(entry);
		}

	  private:
		struct Entry
		{
			std::string chat_id;
			std::string id;
			bool admitted = false;
		};
		static constexpr std::size_t kCapacity = 256;

		std::deque<Entry>::iterator Find(std::string_view chat_id, std::string_view id)
		{
			return std::ranges::find_if(m_entries, [&](const Entry& entry) { return entry.chat_id == chat_id && entry.id == id; });
		}

		std::deque<Entry> m_entries;
	};

} // namespace uam::cef
