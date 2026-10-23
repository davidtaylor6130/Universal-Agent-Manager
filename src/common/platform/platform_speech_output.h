#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	struct SpeechVoice
	{
		std::string id;
		std::string name;
		std::string language;
	};

	struct SpeechOutputOptions
	{
		std::string speech_id;
		std::string text;
		std::string voice_id;
		std::string language;
		double rate = 1.0;
		double volume = 1.0;
	};

	struct SpeechOutputState
	{
		std::string speech_id;
		bool speaking = false;
		std::string error;
	};

	/// <summary>Validate bounded native synthesis input before allocating audio resources.</summary>
	inline bool ValidateSpeechOutputOptions(const SpeechOutputOptions& options, std::string* error_out)
	{
		std::string error;
		if (options.speech_id.empty() || options.speech_id.size() > 128) error = "Invalid speech playback ID.";
		else if (options.text.empty() || options.text.size() > 65536) error = "Speech text must contain between 1 and 65536 bytes.";
		else if (options.voice_id.size() > 256 || options.language.size() > 64) error = "Invalid speech voice or language.";
		else if (!std::isfinite(options.rate) || options.rate < 0.5 || options.rate > 2.0) error = "Speech speed must be between 0.5 and 2.";
		else if (!std::isfinite(options.volume) || options.volume < 0.0 || options.volume > 1.0) error = "Speech volume must be between 0 and 1.";
		if (error_out) *error_out = error;
		return error.empty();
	}

	/// <summary>Native installed voices. Call synthesis and enumeration from an existing background worker.</summary>
	std::vector<SpeechVoice> ListSpeechVoices(std::string* error_out = nullptr);
	/// <summary>Reserve playback on the CEF thread before offloading synthesis; Stop cancels even a queued request.</summary>
	std::uint64_t PrepareSpeechOutputRequest(std::string_view speech_id);
	bool StartSpeechOutput(const SpeechOutputOptions& options, std::uint64_t request, std::string* error_out = nullptr);
	bool StopSpeechOutput(std::string_view speech_id);
	SpeechOutputState GetSpeechOutputState();
}
