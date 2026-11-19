#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

// Process-wide scrubber for secrets the user typed into UAM. Values stay in memory only, for the process lifetime.
namespace uam::secrets
{
	inline constexpr std::size_t kMinScrubbedBytes = 6;
	inline constexpr std::size_t kMaxSecretBytes = 8192;
	inline constexpr std::string_view kRedaction = "[redacted secret]";

	/// Registers the value and its JSON-escaped, percent-encoded and base64 forms. Thread-safe.
	/// False, and nothing registered, when the value is shorter than kMinScrubbedBytes.
	bool Register(std::string_view value);
	/// Cheap check: true once any value is registered.
	bool Active() noexcept;
	/// Replaces every registered pattern with kRedaction; true when the text changed.
	bool ScrubInPlace(std::string& text);
	std::string Scrub(std::string_view text);
	/// Scrubs only the last `window` bytes, for text that grew at its end; window must cover the new bytes plus MaxPatternBytes().
	bool ScrubTail(std::string& text, std::size_t window);
	std::size_t MaxPatternBytes();
	/// Scrubs every string value of parsed JSON, so structure, numbers and escapes stay valid.
	bool ScrubJsonStrings(nlohmann::json& value);

	/// Scrubs chunked text. Never emits a whole pattern, even one split across chunks: it holds back only
	/// a suffix that is a proper prefix of some pattern until the next Push or Flush.
	class StreamFilter
	{
	  public:
		std::string Push(std::string_view chunk);
		/// Emits the held remainder, scrubbed.
		std::string Flush();

	  private:
		std::string m_held;
	};

	/// Zeroes and forgets every registered value.
	void ResetForTests();
}
