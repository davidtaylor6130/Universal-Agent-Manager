#include "platform_speech_output.h"

#if defined(__APPLE__)
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <mutex>

namespace
{
	std::mutex s_mutex;
	uam::SpeechOutputState s_state;
	AVSpeechSynthesizer* s_synthesizer = nil;
	AVSpeechUtterance* s_utterance = nil;
	std::uint64_t s_generation = 0;
	std::uint64_t s_utteranceGeneration = 0;

	void OnMain(void (^operation)(void))
	{
		if ([NSThread isMainThread]) operation();
		else dispatch_sync(dispatch_get_main_queue(), operation);
	}

	void CompleteSpeech(AVSpeechUtterance* utterance)
	{
		if (utterance != s_utterance) return;
		std::scoped_lock lock(s_mutex);
		if (s_generation != s_utteranceGeneration) return;
		s_state.speaking = false;
		s_utterance = nil;
	}
}

@interface UamSpeechOutputDelegate : NSObject<AVSpeechSynthesizerDelegate>
@end
@implementation UamSpeechOutputDelegate
- (void)speechSynthesizer:(AVSpeechSynthesizer*)synthesizer didFinishSpeechUtterance:(AVSpeechUtterance*)utterance
{
	(void)synthesizer;
	CompleteSpeech(utterance);
}
- (void)speechSynthesizer:(AVSpeechSynthesizer*)synthesizer didCancelSpeechUtterance:(AVSpeechUtterance*)utterance
{
	(void)synthesizer;
	CompleteSpeech(utterance);
}
@end

namespace uam
{
	std::vector<SpeechVoice> ListSpeechVoices(std::string* error_out)
	{
		if (error_out) error_out->clear();
		std::vector<SpeechVoice> voices;
		@autoreleasepool
		{
			for (AVSpeechSynthesisVoice* voice in [AVSpeechSynthesisVoice speechVoices])
			{
				voices.push_back({voice.identifier.UTF8String ?: "", voice.name.UTF8String ?: "", voice.language.UTF8String ?: ""});
			}
		}
		return voices;
	}

	std::uint64_t PrepareSpeechOutputRequest(std::string_view speech_id)
	{
		const std::string id(speech_id);
		__block std::uint64_t request = 0;
		OnMain(^{
			[s_synthesizer stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
			s_utterance = nil;
			std::scoped_lock lock(s_mutex);
			s_state = {id, true, {}};
			request = ++s_generation;
		});
		return request;
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
		{
			std::scoped_lock lock(s_mutex);
			if (request != s_generation || !s_state.speaking || s_state.speech_id != options.speech_id)
			{
				if (error_out) *error_out = "Speech playback was canceled.";
				return false;
			}
		}
		__block std::string error;
		@autoreleasepool
		{
			NSString* text = [[NSString alloc] initWithBytes:options.text.data() length:options.text.size() encoding:NSUTF8StringEncoding];
			AVSpeechSynthesisVoice* voice = nil;
			if (!options.voice_id.empty()) voice = [AVSpeechSynthesisVoice voiceWithIdentifier:[NSString stringWithUTF8String:options.voice_id.c_str()]];
			else if (!options.language.empty()) voice = [AVSpeechSynthesisVoice voiceWithLanguage:[NSString stringWithUTF8String:options.language.c_str()]];
			if (text == nil || ((!options.voice_id.empty() || !options.language.empty()) && voice == nil))
			{
				error = text == nil ? "Speech text is not valid UTF-8." : "The selected system voice or language is unavailable.";
				std::scoped_lock lock(s_mutex);
				if (request == s_generation)
				{
					s_state.speaking = false;
					s_state.error = error;
				}
				if (error_out) *error_out = error;
				return false;
			}
			AVSpeechSynthesizer* prepared_synthesizer = [[AVSpeechSynthesizer alloc] init];
			AVSpeechUtterance* prepared_utterance = [AVSpeechUtterance speechUtteranceWithString:text];
			prepared_utterance.voice = voice;
			prepared_utterance.rate = std::clamp(static_cast<float>(AVSpeechUtteranceDefaultSpeechRate * options.rate), AVSpeechUtteranceMinimumSpeechRate, AVSpeechUtteranceMaximumSpeechRate);
			prepared_utterance.volume = static_cast<float>(options.volume);
			OnMain(^{
				{
					std::scoped_lock lock(s_mutex);
					if (request != s_generation || !s_state.speaking)
					{
						error = "Speech playback was canceled.";
						return;
					}
				}
				static UamSpeechOutputDelegate* delegate = [[UamSpeechOutputDelegate alloc] init];
				s_synthesizer = prepared_synthesizer;
				s_synthesizer.delegate = delegate;
				s_utterance = prepared_utterance;
				s_utteranceGeneration = request;
				[s_synthesizer speakUtterance:s_utterance];
			});
		}
		if (error_out) *error_out = error;
		return error.empty();
	}

	bool StopSpeechOutput(std::string_view speech_id)
	{
		const std::string requested(speech_id);
		__block bool stopped = false;
		OnMain(^{
			{
				std::scoped_lock lock(s_mutex);
				if (requested.empty() || s_state.speech_id != requested) return;
				++s_generation;
				s_state.speaking = false;
				stopped = true;
			}
			s_utterance = nil;
			[s_synthesizer stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
		});
		return stopped;
	}

	SpeechOutputState GetSpeechOutputState()
	{
		std::scoped_lock lock(s_mutex);
		return s_state;
	}
}
#endif
