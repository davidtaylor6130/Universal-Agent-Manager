#include "platform_services.h"

#if defined(__APPLE__)

#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>

#include <cstdint>
#include <mutex>
#include <utility>

namespace uam::platform_macos_impl
{

class MacDictationService final : public IPlatformDictationService
{
  public:
	~MacDictationService() override
	{
		[m_audioEngine stop];
		RemoveInputTap();
	}

	bool Start(const DictationOptions& options, std::string* error_out = nullptr) override
	{
		std::uint64_t generation = 0;
		{
			std::scoped_lock lock(m_mutex);
			if (m_running)
			{
				if (error_out != nullptr)
				{
					*error_out = "Dictation is already running.";
				}
				return false;
			}

			m_running = true;
			generation = ++m_generation;
		}

		const std::string locale = options.locale;
		dispatch_async(dispatch_get_main_queue(), ^{
			RequestSpeechPermission(generation, locale);
		});
		return true;
	}

	void Stop() override
	{
		std::uint64_t generation = 0;
		{
			std::scoped_lock lock(m_mutex);
			if (!m_running)
			{
				return;
			}
			generation = m_generation;
		}

		dispatch_async(dispatch_get_main_queue(), ^{
			if (!IsCurrent(generation))
			{
				return;
			}
			m_stopRequested = true;
			[m_audioEngine stop];
			RemoveInputTap();
			[m_request endAudio];
			if (m_task == nil)
			{
				Complete(generation);
			}
		});
	}

	bool IsRunning() const override
	{
		std::scoped_lock lock(m_mutex);
		return m_running;
	}

	std::vector<DictationEvent> PollEvents() override
	{
		std::scoped_lock lock(m_mutex);
		std::vector<DictationEvent> events;
		events.swap(m_events);
		return events;
	}

  private:
	void RequestSpeechPermission(std::uint64_t generation, const std::string& locale)
	{
		switch ([SFSpeechRecognizer authorizationStatus])
		{
			case SFSpeechRecognizerAuthorizationStatusAuthorized:
				RequestMicrophonePermission(generation, locale);
				return;
			case SFSpeechRecognizerAuthorizationStatusNotDetermined:
				[SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus status) {
					dispatch_async(dispatch_get_main_queue(), ^{
						if (status == SFSpeechRecognizerAuthorizationStatusAuthorized)
						{
							RequestMicrophonePermission(generation, locale);
						}
						else
						{
							Fail(generation, "Speech recognition permission was denied.");
						}
					});
				}];
				return;
			default:
				Fail(generation, "Speech recognition permission was denied.");
				return;
		}
	}

	void RequestMicrophonePermission(std::uint64_t generation, const std::string& locale)
	{
		switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
		{
			case AVAuthorizationStatusAuthorized:
				BeginRecognition(generation, locale);
				return;
			case AVAuthorizationStatusNotDetermined:
				[AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
					dispatch_async(dispatch_get_main_queue(), ^{
						if (granted)
						{
							BeginRecognition(generation, locale);
						}
						else
						{
							Fail(generation, "Microphone permission was denied.");
						}
					});
				}];
				return;
			default:
				Fail(generation, "Microphone permission was denied.");
				return;
		}
	}

	void BeginRecognition(std::uint64_t generation, const std::string& locale)
	{
		if (!IsCurrent(generation))
		{
			return;
		}

		NSString* locale_identifier = locale.empty() ? nil : [NSString stringWithUTF8String:locale.c_str()];
		m_recognizer = locale_identifier == nil ? [[SFSpeechRecognizer alloc] init] : [[SFSpeechRecognizer alloc] initWithLocale:[[NSLocale alloc] initWithLocaleIdentifier:locale_identifier]];
		if (m_recognizer == nil || !m_recognizer.available)
		{
			Fail(generation, "Speech recognition is unavailable for the selected language.");
			return;
		}

		m_audioEngine = [[AVAudioEngine alloc] init];
		m_request = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
		m_request.shouldReportPartialResults = YES;
		m_stopRequested = false;

		AVAudioInputNode* input_node = m_audioEngine.inputNode;
		AVAudioFormat* format = [input_node outputFormatForBus:0];
		if (format.sampleRate <= 0 || format.channelCount == 0)
		{
			Fail(generation, "No microphone input is available.");
			return;
		}

		SFSpeechAudioBufferRecognitionRequest* request = m_request;
		[input_node installTapOnBus:0 bufferSize:1024 format:format block:^(AVAudioPCMBuffer* buffer, AVAudioTime* /*when*/) {
			[request appendAudioPCMBuffer:buffer];
		}];
		m_tapInstalled = true;
		[m_audioEngine prepare];

		m_task = [m_recognizer recognitionTaskWithRequest:m_request resultHandler:^(SFSpeechRecognitionResult* result, NSError* error) {
			dispatch_async(dispatch_get_main_queue(), ^{
				if (!IsCurrent(generation))
				{
					return;
				}
				if (error != nil)
				{
					if (m_stopRequested)
					{
						Complete(generation);
					}
					else
					{
						Fail(generation, error.localizedDescription.UTF8String ?: "Speech recognition failed.");
					}
					return;
				}

				if (result == nil)
				{
					return;
				}

				const char* utf8 = result.bestTranscription.formattedString.UTF8String;
				const std::string text = utf8 == nullptr ? std::string() : std::string(utf8);
				if (!text.empty())
				{
					Enqueue({result.final ? DictationEventType::Final : DictationEventType::Interim, text});
				}
				if (result.final)
				{
					Complete(generation);
				}
			});
		}];

		NSError* start_error = nil;
		if (![m_audioEngine startAndReturnError:&start_error])
		{
			Fail(generation, start_error.localizedDescription.UTF8String ?: "Failed to start microphone capture.");
		}
	}

	bool IsCurrent(std::uint64_t generation) const
	{
		std::scoped_lock lock(m_mutex);
		return m_running && m_generation == generation;
	}

	void Enqueue(DictationEvent event)
	{
		std::scoped_lock lock(m_mutex);
		m_events.push_back(std::move(event));
	}

	void RemoveInputTap()
	{
		if (m_tapInstalled)
		{
			[m_audioEngine.inputNode removeTapOnBus:0];
			m_tapInstalled = false;
		}
	}

	void ReleaseRecognitionObjects()
	{
		[m_audioEngine stop];
		RemoveInputTap();
		m_task = nil;
		m_request = nil;
		m_audioEngine = nil;
		m_recognizer = nil;
		m_stopRequested = false;
	}

	void Complete(std::uint64_t generation)
	{
		if (!IsCurrent(generation))
		{
			return;
		}
		ReleaseRecognitionObjects();
		{
			std::scoped_lock lock(m_mutex);
			m_running = false;
			m_events.push_back({DictationEventType::End, {}});
		}
	}

	void Fail(std::uint64_t generation, std::string message)
	{
		if (!IsCurrent(generation))
		{
			return;
		}
		ReleaseRecognitionObjects();
		{
			std::scoped_lock lock(m_mutex);
			m_running = false;
			m_events.push_back({DictationEventType::Error, std::move(message)});
			m_events.push_back({DictationEventType::End, {}});
		}
	}

	mutable std::mutex m_mutex;
	std::vector<DictationEvent> m_events;
	std::uint64_t m_generation = 0;
	bool m_running = false;
	bool m_stopRequested = false;
	bool m_tapInstalled = false;
	SFSpeechRecognizer* m_recognizer = nil;
	SFSpeechAudioBufferRecognitionRequest* m_request = nil;
	SFSpeechRecognitionTask* m_task = nil;
	AVAudioEngine* m_audioEngine = nil;
};

IPlatformDictationService& GetMacDictationService()
{
	static MacDictationService instance;
	return instance;
}

} // namespace uam::platform_macos_impl

#endif // defined(__APPLE__)
