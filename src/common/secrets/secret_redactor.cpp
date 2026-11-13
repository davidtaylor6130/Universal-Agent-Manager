#include "common/secrets/secret_redactor.h"

#include "common/utils/base64.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace uam::secrets
{
	namespace
	{
		struct Pattern
		{
			std::string text;
			std::uint64_t owner = 0;
		};

		// ponytail: 256 values, oldest dropped first; a session never asks for nearly that many secrets.
		constexpr std::size_t kMaxValues = 256;

		std::mutex g_mutex;
		std::atomic<bool> g_active{false};
		std::vector<Pattern> g_patterns; // Longest first, so a longer form is replaced before any shorter form inside it.
		std::deque<std::uint64_t> g_owners;
		std::uint64_t g_nextOwner = 1;

		// Volatile writes so the compiler cannot drop the wipe as a dead store.
		void Wipe(std::string& value)
		{
			volatile char* bytes = value.data();
			for (std::size_t index = 0; index < value.size(); ++index) bytes[index] = '\0';
			value.clear();
		}

		std::string JsonEscaped(std::string_view value)
		{
			const std::string quoted = nlohmann::json(std::string(value)).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
			return quoted.substr(1, quoted.size() - 2);
		}

		std::string PercentEncoded(std::string_view value)
		{
			constexpr char kHex[] = "0123456789ABCDEF";
			std::string encoded;
			for (const char character : value)
			{
				const unsigned char byte = static_cast<unsigned char>(character);
				if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~') encoded.push_back(character);
				else
				{
					encoded.push_back('%');
					encoded.push_back(kHex[byte >> 4U]);
					encoded.push_back(kHex[byte & 0x0FU]);
				}
			}
			return encoded;
		}

		std::vector<std::string> PatternsFor(std::string_view value)
		{
			std::vector<std::string> patterns{std::string(value), JsonEscaped(value), PercentEncoded(value)};
			// The value can start at any byte offset of a larger encoded blob; keep only the characters its bits fully decide.
			for (std::size_t prefix = 0; prefix < 3; ++prefix)
			{
				std::string padded(prefix, '\0');
				padded.append(value);
				std::string encoded = uam::base64::Encode(padded);
				Wipe(padded);
				const std::size_t begin = (8 * prefix + 5) / 6;
				const std::size_t end = 8 * (prefix + value.size()) / 6;
				if (end > begin && end - begin >= 8)
				{
					std::string standard = encoded.substr(begin, end - begin);
					std::string url_safe = standard;
					std::ranges::replace(url_safe, '+', '-');
					std::ranges::replace(url_safe, '/', '_');
					patterns.push_back(std::move(standard));
					patterns.push_back(std::move(url_safe));
				}
				Wipe(encoded);
			}
			std::vector<std::string> unique;
			for (std::string& pattern : patterns)
			{
				if (std::ranges::find(unique, pattern) == unique.end()) unique.push_back(std::move(pattern));
				else Wipe(pattern);
			}
			return unique;
		}

		void RemoveOwner(std::uint64_t owner)
		{
			for (Pattern& pattern : g_patterns)
				if (pattern.owner == owner) Wipe(pattern.text);
			std::erase_if(g_patterns, [owner](const Pattern& pattern) { return pattern.owner == owner; });
		}

		// ponytail: naive find per pattern, O(text x patterns); fine for log lines and stream chunks, add Aho-Corasick if it profiles hot.
		bool ScrubLocked(std::string& text)
		{
			bool changed = false;
			for (const Pattern& pattern : g_patterns)
			{
				for (std::size_t found = text.find(pattern.text); found != std::string::npos; found = text.find(pattern.text, found + kRedaction.size()))
				{
					text.replace(found, pattern.text.size(), kRedaction);
					changed = true;
				}
			}
			return changed;
		}

		// Start of the longest suffix that is a proper prefix of some pattern; text.size() when none.
		std::size_t HeldSuffixStart(const std::string& text)
		{
			std::size_t best = text.size();
			for (const Pattern& pattern : g_patterns)
			{
				const std::size_t longest = std::min(pattern.text.size() - 1, text.size());
				for (std::size_t start = text.size() - longest; start < best; ++start)
				{
					if (text[start] == pattern.text[0] && text.compare(start, text.size() - start, pattern.text, 0, text.size() - start) == 0)
					{
						best = start;
						break;
					}
				}
			}
			return best;
		}
	}

	bool Register(std::string_view value)
	{
		if (value.size() < kMinScrubbedBytes) return false;
		std::vector<std::string> patterns = PatternsFor(value);
		std::lock_guard lock(g_mutex);
		if (std::ranges::any_of(g_patterns, [value](const Pattern& pattern) { return pattern.text == value; }))
		{
			for (std::string& pattern : patterns) Wipe(pattern);
			return true;
		}
		if (g_owners.size() >= kMaxValues)
		{
			RemoveOwner(g_owners.front());
			g_owners.pop_front();
		}
		const std::uint64_t owner = g_nextOwner++;
		g_owners.push_back(owner);
		for (std::string& pattern : patterns)
		{
			const auto position = std::ranges::find_if(g_patterns, [&pattern](const Pattern& existing) { return existing.text.size() < pattern.size(); });
			g_patterns.insert(position, Pattern{std::move(pattern), owner});
		}
		g_active.store(true, std::memory_order_release);
		return true;
	}

	bool Active() noexcept
	{
		return g_active.load(std::memory_order_acquire);
	}

	bool ScrubInPlace(std::string& text)
	{
		if (!Active()) return false;
		std::lock_guard lock(g_mutex);
		return ScrubLocked(text);
	}

	std::string Scrub(std::string_view text)
	{
		std::string scrubbed(text);
		ScrubInPlace(scrubbed);
		return scrubbed;
	}

	bool ScrubTail(std::string& text, std::size_t window)
	{
		if (!Active()) return false;
		if (text.size() <= window) return ScrubInPlace(text);
		const std::size_t start = text.size() - window;
		std::string tail = text.substr(start);
		if (!ScrubInPlace(tail)) return false;
		text.replace(start, std::string::npos, tail);
		return true;
	}

	std::size_t MaxPatternBytes()
	{
		std::lock_guard lock(g_mutex);
		return g_patterns.empty() ? 0 : g_patterns.front().text.size(); // Longest first.
	}

	bool ScrubJsonStrings(nlohmann::json& value)
	{
		if (!Active()) return false;
		if (value.is_string()) return ScrubInPlace(value.get_ref<std::string&>());
		bool changed = false;
		if (value.is_object() || value.is_array())
			for (nlohmann::json& child : value) changed = ScrubJsonStrings(child) || changed;
		return changed;
	}

	std::string StreamFilter::Push(std::string_view chunk)
	{
		m_held.append(chunk);
		std::string out;
		if (!Active())
		{
			out.swap(m_held);
			return out;
		}
		std::lock_guard lock(g_mutex);
		ScrubLocked(m_held);
		const std::size_t keep = HeldSuffixStart(m_held);
		out = m_held.substr(0, keep);
		m_held.erase(0, keep);
		return out;
	}

	std::string StreamFilter::Flush()
	{
		std::string out;
		out.swap(m_held);
		ScrubInPlace(out);
		return out;
	}

	void ResetForTests()
	{
		std::lock_guard lock(g_mutex);
		for (Pattern& pattern : g_patterns) Wipe(pattern.text);
		g_patterns.clear();
		g_owners.clear();
		g_active.store(false, std::memory_order_release);
	}
}
