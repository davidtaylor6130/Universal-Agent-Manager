#include "platform_speech_output.h"

#if defined(_WIN32)
#include <winerror.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cstdint>
#include <mutex>
#include <utility>

namespace uam
{
	namespace
	{
		using winrt::Windows::Media::Playback::MediaPlayer;
		std::mutex s_mutex;
		SpeechOutputState s_state;
		MediaPlayer s_player{nullptr};
		std::uint64_t s_generation = 0;

		class SpeechApartment
		{
		  public:
			SpeechApartment()
			{
				try
				{
					winrt::init_apartment(winrt::apartment_type::multi_threaded);
					m_initialized = true;
				}
				catch (const winrt::hresult_error& error)
				{
					if (error.code() != RPC_E_CHANGED_MODE) throw;
				}
			}
			~SpeechApartment()
			{
				if (m_initialized) winrt::uninit_apartment();
			}
		  private:
			bool m_initialized = false;
		};

		void CompleteSpeech(std::uint64_t generation, std::string error = {})
		{
			MediaPlayer completed{nullptr};
			{
				std::scoped_lock lock(s_mutex);
				if (generation != s_generation) return;
				s_state.speaking = false;
				s_state.error = error.substr(0, 1024);
				completed = std::move(s_player);
			}
		}
	}

	std::vector<SpeechVoice> ListSpeechVoices(std::string* error_out)
	{
		if (error_out) error_out->clear();
		std::vector<SpeechVoice> voices;
		try
		{
			const SpeechApartment apartment;
			for (const winrt::Windows::Media::SpeechSynthesis::VoiceInformation& voice : winrt::Windows::Media::SpeechSynthesis::SpeechSynthesizer::AllVoices())
			{
				voices.push_back({winrt::to_string(voice.Id()), winrt::to_string(voice.DisplayName()), winrt::to_string(voice.Language())});
			}
		}
		catch (const winrt::hresult_error& error)
		{
			if (error_out) *error_out = winrt::to_string(error.message());
		}
		return voices;
	}

	std::uint64_t PrepareSpeechOutputRequest(std::string_view speech_id)
	{
		std::scoped_lock lock(s_mutex);
		s_state = {std::string(speech_id), true, {}};
		return ++s_generation;
	}

	bool StartSpeechOutput(const SpeechOutputOptions& options, std::uint64_t request, std::string* error_out)
	{
		std::string validation_error;
		if (!ValidateSpeechOutputOptions(options, &validation_error))
		{
			std::scoped_lock lock(s_mutex);
			if (request == s_generation && s_state.speech_id == options.speech_id)
			{
				s_state.speaking = false;
				s_state.error = validation_error;
			}
			if (error_out) *error_out = validation_error;
			return false;
		}
		const std::uint64_t generation = request;
		MediaPlayer previous{nullptr};
		{
			std::scoped_lock lock(s_mutex);
			if (request != s_generation || !s_state.speaking || s_state.speech_id != options.speech_id)
			{
				if (error_out) *error_out = "Speech playback was canceled.";
				return false;
			}
			previous = std::move(s_player);
		}
		try
		{
			const SpeechApartment apartment;
			if (previous) previous.Close();
			using namespace winrt::Windows::Media::SpeechSynthesis;
			SpeechSynthesizer synthesizer;
			if (!options.voice_id.empty() || !options.language.empty())
			{
				bool found = false;
				for (const winrt::Windows::Media::SpeechSynthesis::VoiceInformation& voice : SpeechSynthesizer::AllVoices())
				{
					if ((!options.voice_id.empty() && winrt::to_string(voice.Id()) == options.voice_id) ||
					    (options.voice_id.empty() && winrt::to_string(voice.Language()) == options.language))
					{
						synthesizer.Voice(voice);
						found = true;
						break;
					}
				}
				if (!found)
				{
					const std::string error = "The selected system voice or language is unavailable.";
					CompleteSpeech(generation, error);
					if (error_out) *error_out = error;
					return false;
				}
			}
			const SpeechSynthesisStream stream = synthesizer.SynthesizeTextToStreamAsync(winrt::to_hstring(options.text)).get();
			MediaPlayer player;
			player.Volume(options.volume);
			player.PlaybackSession().PlaybackRate(options.rate);
			player.MediaEnded([generation](const MediaPlayer&, const winrt::Windows::Foundation::IInspectable&) { CompleteSpeech(generation); });
			player.MediaFailed([generation](const MediaPlayer&, const winrt::Windows::Media::Playback::MediaPlayerFailedEventArgs& args) {
				CompleteSpeech(generation, "Speech playback failed: " + winrt::to_string(args.ErrorMessage()));
			});
			player.Source(winrt::Windows::Media::Core::MediaSource::CreateFromStream(stream, stream.ContentType()));
			{
				std::scoped_lock lock(s_mutex);
				if (generation != s_generation || !s_state.speaking)
				{
					if (error_out) *error_out = "Speech playback was canceled.";
					return false;
				}
				s_player = player;
			}
			player.Play();
			return true;
		}
		catch (const winrt::hresult_error& error)
		{
			const std::string message = "Speech playback failed: " + winrt::to_string(error.message());
			CompleteSpeech(generation, message);
			if (error_out) *error_out = message;
			return false;
		}
	}

	bool StopSpeechOutput(std::string_view speech_id)
	{
		MediaPlayer player{nullptr};
		{
			std::scoped_lock lock(s_mutex);
			if (speech_id.empty() || speech_id != s_state.speech_id) return false;
			++s_generation;
			s_state.speaking = false;
			player = std::move(s_player);
		}
		if (player)
		{
			try
			{
				const SpeechApartment apartment;
				player.Close();
			}
			catch (const winrt::hresult_error&) {}
		}
		return true;
	}

	SpeechOutputState GetSpeechOutputState()
	{
		std::scoped_lock lock(s_mutex);
		return s_state;
	}
}
#endif
