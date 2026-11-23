#pragma once

#include "common/models/app_models.h"

#include <chrono>
#include <cstdint>
#include <utility>

namespace uam::chat_snapshot
{
	/// <summary>
	/// Copies every chat field except the transcript, for a rollback that never needs the messages back. The live chat
	/// keeps its transcript and the transcript's revision stamp (a move keeps the stamp), so the caller can prove later,
	/// with messages.revision(), that nothing reached the messages for writing while the copy was held.
	/// </summary>
	inline ChatSession CopyWithoutMessages(ChatSession& chat)
	{
		uam::RevisionedVector<Message> messages = std::move(chat.messages);
		try
		{
			ChatSession copy = chat;
			chat.messages = std::move(messages);
			return copy;
		}
		catch (...)
		{
			chat.messages = std::move(messages);
			throw;
		}
	}

	/// <summary>Puts back every field from a CopyWithoutMessages snapshot and keeps the live transcript and its stamp.</summary>
	inline void RestoreWithoutMessages(ChatSession& chat, ChatSession snapshot)
	{
		snapshot.messages = std::move(chat.messages);
		chat = std::move(snapshot);
	}

	/// <summary>
	/// Remembers the last revision-stamp pair a full comparison found equal. RevisionedVector gives every write a new
	/// stamp, so while both stamps are unchanged the content is unchanged; a full comparison still runs at least every
	/// kFullComparisonInterval, as the type asks of stamp readers, in case a writer kept an element reference.
	/// </summary>
	struct TranscriptMatchMemo
	{
		static constexpr std::chrono::milliseconds kFullComparisonInterval{2000};
		std::uint64_t live_revision = 0, captured_revision = 0;
		std::chrono::steady_clock::time_point compared_at{};
	};

	/// <summary>
	/// Equality of a live transcript with a captured one for a check repeated on every poll. Decisions that publish or
	/// commit compare in full instead (operator==), never through the memo.
	/// </summary>
	inline bool SameMessagesPolled(const uam::RevisionedVector<Message>& live, const uam::RevisionedVector<Message>& captured,
	    TranscriptMatchMemo& memo, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
	{
		const std::uint64_t live_revision = live.revision(), captured_revision = captured.revision();
		if (live_revision != 0 && captured_revision != 0 && live_revision == memo.live_revision && captured_revision == memo.captured_revision &&
		    now - memo.compared_at < TranscriptMatchMemo::kFullComparisonInterval)
			return true;
		const bool same = live == captured;
		memo = same ? TranscriptMatchMemo{live_revision, captured_revision, now} : TranscriptMatchMemo{};
		return same;
	}
} // namespace uam::chat_snapshot
