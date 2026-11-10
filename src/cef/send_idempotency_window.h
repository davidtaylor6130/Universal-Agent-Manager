#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <string_view>

namespace uam::cef
{

	/// Composer submissions seen by this app run, so a client retrying one submission whose reply it lost cannot admit it twice.
	/// The ID names one submission, never its text: a new submission with identical words carries a new ID and is admitted.
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
		struct Result
		{
			Claim claim = Claim::New;
			/// The request ID of the attempt that claimed the submission first; empty when it carried none.
			std::string origin_request_id;
		};

		Result Begin(std::string_view chat_id, std::string_view id, std::string_view origin_request_id)
		{
			if (const auto entry = Find(chat_id, id); entry != m_entries.end())
				return {entry->admitted ? Claim::Admitted : Claim::InProgress, entry->origin_request_id};
			if (m_entries.size() >= kCapacity)
				m_entries.pop_front();
			m_entries.push_back(Entry{std::string(chat_id), std::string(id), std::string(origin_request_id), false});
			return {};
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
			std::string origin_request_id;
			bool admitted = false;
		};
		static constexpr std::size_t kCapacity = 256;

		std::deque<Entry>::iterator Find(std::string_view chat_id, std::string_view id)
		{
			return std::ranges::find_if(m_entries, [&](const Entry& entry) { return entry.chat_id == chat_id && entry.id == id; });
		}

		std::deque<Entry> m_entries;
	};

	/// One claim's outcome. Every exit settles it: Settle reports the outcome, and a settlement destroyed unsettled
	/// (an exception, or an asynchronous completion that is dropped without running) releases the claim, so a retry
	/// is a fresh admission rather than an indefinite 409. The window is touched only while its owner is alive.
	class SendIdempotencySettlement
	{
	  public:
		SendIdempotencySettlement(SendIdempotencyWindow& window, std::weak_ptr<void> owner_lifetime, std::string chat_id, std::string id)
		    : m_window(window), m_owner_lifetime(std::move(owner_lifetime)), m_chat_id(std::move(chat_id)), m_id(std::move(id))
		{
		}
		SendIdempotencySettlement(const SendIdempotencySettlement&) = delete;
		SendIdempotencySettlement& operator=(const SendIdempotencySettlement&) = delete;
		~SendIdempotencySettlement()
		{
			Settle(false);
		}

		void Settle(bool admitted)
		{
			if (m_settled)
				return;
			m_settled = true;
			if (!m_id.empty() && !m_owner_lifetime.expired())
				m_window.Settle(m_chat_id, m_id, admitted);
		}

	  private:
		SendIdempotencyWindow& m_window;
		std::weak_ptr<void> m_owner_lifetime;
		std::string m_chat_id;
		std::string m_id;
		bool m_settled = false;
	};

} // namespace uam::cef
