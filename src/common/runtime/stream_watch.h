#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

/// Shared per-turn stream watch (#636). Every provider's structured stream reaches the shared
/// turn-event and tool-call sinks, which feed this detector; the runtime poll decides whether to stop.
/// Memory is fixed per turn (hashes plus short examples) and each observed byte or tool call is O(1).
namespace uam::stream_watch
{
	/// Default for settings `stream_loop_repeat_limit`; 0 turns loop stops off.
	inline constexpr int kDefaultLoopRepeatLimit = 20;
	inline constexpr int kMaxLoopRepeatLimit = 50;
	/// A repeat counts only when the same content recurs within this many distinct segments or tool calls.
	inline constexpr std::size_t kRepeatWindow = 8;
	/// Shorter text/thought segments (words, list markers, code braces) are never loop evidence.
	inline constexpr std::size_t kMinSegmentBytes = 40;
	inline constexpr std::size_t kExampleBytes = 160;
	/// A tool call that ran this long was waiting (a sleep, poll or build), not looping.
	inline constexpr double kMaxLoopToolDurationSeconds = 5.0;
	/// Identical tool calls further apart than this are treated as deliberate polling.
	inline constexpr double kMaxToolRepeatGapSeconds = 60.0;

	/// Settings offer Off, 10, 20 or 50; any other saved value rounds up to the next offered one.
	inline int NormalizeLoopRepeatLimit(int value)
	{
		return value <= 0 ? 0 : value <= 10 ? 10 : value <= 20 ? 20 : kMaxLoopRepeatLimit;
	}

	enum class Channel : std::uint8_t
	{
		Text,
		Thought,
		Tool,
	};

	inline const char* ChannelLabel(Channel channel)
	{
		return channel == Channel::Text ? "text" : channel == Channel::Thought ? "thought" : "tool call";
	}

	inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
	inline std::uint64_t HashByte(std::uint64_t hash, unsigned char byte)
	{
		return (hash ^ byte) * 1099511628211ULL;
	}

	inline void AppendExample(std::string& example, std::string_view text)
	{
		const std::size_t room = kExampleBytes > example.size() ? kExampleBytes - example.size() : 0;
		std::size_t take = std::min(room, text.size());
		// Never split a UTF-8 sequence at the cap.
		if (take < text.size())
			while (take > 0 && (static_cast<unsigned char>(text[take]) & 0xC0) == 0x80) --take;
		example.append(text.substr(0, take));
	}

	class StreamWatch
	{
	public:
		void ObserveText(Channel channel, std::string_view chunk)
		{
			Segment& segment = segments_[channel == Channel::Thought ? 1 : 0];
			for (const char c : chunk)
			{
				const unsigned char byte = static_cast<unsigned char>(c);
				if (c == '\n' || c == '\r')
				{
					EndSegment(channel, segment);
					continue;
				}
				const bool space = c == ' ' || c == '\t';
				if (!space)
				{
					// Normalize whitespace runs and ASCII case so reflowed repeats still match.
					if (segment.pending_space && segment.bytes > 0) Add(segment, ' ', " ");
					Add(segment, static_cast<unsigned char>(byte >= 'A' && byte <= 'Z' ? byte + 32 : byte), std::string_view(&c, 1));
				}
				segment.pending_space = space;
				if (c == '.' || c == '!' || c == '?') EndSegment(channel, segment);
			}
		}

		/// Observe one finished tool call once, with its final result. A negative duration is unknown (the call was
		/// first seen already finished) and, like a call with neither arguments nor a result, is never loop evidence.
		void ObserveTool(std::string_view name, std::string_view args, std::string_view result, double duration_seconds, double now_seconds)
		{
			if (duration_seconds < 0.0 || duration_seconds > kMaxLoopToolDurationSeconds || (args.empty() && result.empty())) return;
			std::uint64_t hash = kFnvOffset;
			for (const std::string_view part : {name, args, result})
			{
				for (const char c : part) hash = HashByte(hash, static_cast<unsigned char>(c));
				hash = HashByte(hash, 0);
			}
			const std::uint32_t count = Push(rings_[2], hash, now_seconds, kMaxToolRepeatGapSeconds);
			if (count > peak_count_)
			{
				peak_count_ = count;
				peak_channel_ = Channel::Tool;
				peak_example_.clear();
				AppendExample(peak_example_, name);
				if (!args.empty())
				{
					AppendExample(peak_example_, " ");
					AppendExample(peak_example_, args);
				}
			}
		}

		std::uint32_t PeakCount() const { return peak_count_; }
		Channel PeakChannel() const { return peak_channel_; }
		const std::string& PeakExample() const { return peak_example_; }

	private:
		struct Entry
		{
			std::uint64_t hash = 0;
			std::uint32_t count = 0;
			double time_s = 0.0;
		};
		struct Ring
		{
			std::array<Entry, kRepeatWindow> entries{};
			std::size_t next = 0;
		};
		struct Segment
		{
			std::uint64_t hash = kFnvOffset;
			std::size_t bytes = 0;
			bool pending_space = false;
			bool example_full = false;
			std::string example;
		};

		static void Add(Segment& segment, unsigned char normalized, std::string_view raw)
		{
			segment.hash = HashByte(segment.hash, normalized);
			++segment.bytes;
			if (segment.example_full) return;
			// Stop at the cap only on a character boundary.
			if (segment.example.size() >= kExampleBytes && (static_cast<unsigned char>(raw.front()) & 0xC0) != 0x80) segment.example_full = true;
			else segment.example.append(raw);
		}

		static std::uint32_t Push(Ring& ring, std::uint64_t hash, double now_seconds, double max_gap_seconds)
		{
			std::uint32_t count = 1;
			for (const Entry& entry : ring.entries)
				if (entry.count > 0 && entry.hash == hash && now_seconds - entry.time_s <= max_gap_seconds)
					count = std::max(count, entry.count + 1);
			ring.entries[ring.next] = {hash, count, now_seconds};
			ring.next = (ring.next + 1) % kRepeatWindow;
			return count;
		}

		void EndSegment(Channel channel, Segment& segment)
		{
			if (segment.bytes >= kMinSegmentBytes)
			{
				// Text has no wall-clock gap rule: a looping stream repeats quickly by nature.
				const std::uint32_t count = Push(rings_[channel == Channel::Thought ? 1 : 0], segment.hash, 0.0, 1.0);
				if (count > peak_count_)
				{
					peak_count_ = count;
					peak_channel_ = channel;
					peak_example_.clear();
					AppendExample(peak_example_, segment.example);
				}
			}
			segment = Segment{};
		}

		std::array<Ring, 3> rings_{};
		std::array<Segment, 2> segments_{};
		std::uint32_t peak_count_ = 0;
		Channel peak_channel_ = Channel::Text;
		std::string peak_example_;
	};
} // namespace uam::stream_watch
