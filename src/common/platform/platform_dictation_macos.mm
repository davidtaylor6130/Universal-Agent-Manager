#include "platform_services.h"
#include "common/config/voice_input_settings.h"

#if defined(__APPLE__)

#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>

#include <algorithm>
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
		[m_uploadTask cancel];
		[m_audioEngine stop];
		RemoveInputTap();
		if (m_audioUrl != nil) [[NSFileManager defaultManager] removeItemAtURL:m_audioUrl error:nil];
		std::fill(m_options.server_api_key.begin(), m_options.server_api_key.end(), '\0');
	}

	bool Start(const DictationOptions& options, std::string* error_out = nullptr) override
	{
		if (options.mode == uam::voice_input::kLocalMode)
		{
			if (error_out) *error_out = "Local AI transcription is coming soon.";
			return false;
		}
		if (options.mode != uam::voice_input::kSystemMode && options.mode != uam::voice_input::kServerMode)
		{
			if (error_out) *error_out = "Unsupported voice input mode.";
			return false;
		}
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
			m_options = options;
			m_uploading = false;
			generation = ++m_generation;
		}

		dispatch_async(dispatch_get_main_queue(), ^{
			if (options.mode == uam::voice_input::kServerMode)
			{
				RequestMicrophonePermission(generation, options.locale);
			}
			else
			{
				RequestSpeechPermission(generation, options.locale);
			}
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
			if (m_options.mode == uam::voice_input::kServerMode)
			{
				if (!m_uploading)
				{
					m_uploading = true;
					UploadRecording(generation);
				}
				return;
			}
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
				BeginAudio(generation, locale);
				return;
			case AVAuthorizationStatusNotDetermined:
				[AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
					dispatch_async(dispatch_get_main_queue(), ^{
						if (granted)
						{
							BeginAudio(generation, locale);
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

	void BeginAudio(std::uint64_t generation, const std::string& locale)
	{
		if (m_options.mode == uam::voice_input::kServerMode)
		{
			BeginServerCapture(generation);
			return;
		}
		BeginRecognition(generation, locale);
	}

	void BeginServerCapture(std::uint64_t generation)
	{
		if (!IsCurrent(generation)) return;
		m_audioEngine = [[AVAudioEngine alloc] init];
		AVAudioInputNode* input_node = m_audioEngine.inputNode;
		AVAudioFormat* format = [input_node outputFormatForBus:0];
		if (format.sampleRate <= 0 || format.channelCount == 0)
		{
			Fail(generation, "No microphone input is available.");
			return;
		}

		NSString* filename = [NSString stringWithFormat:@"uam-voice-%@.wav", NSUUID.UUID.UUIDString];
		m_audioUrl = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:filename]];
		NSError* file_error = nil;
		m_audioFile = [[AVAudioFile alloc] initForWriting:m_audioUrl settings:format.settings error:&file_error];
		if (m_audioFile == nil)
		{
			Fail(generation, file_error.localizedDescription.UTF8String ?: "Failed to create voice recording.");
			return;
		}

		AVAudioFile* audio_file = m_audioFile;
		[input_node installTapOnBus:0 bufferSize:1024 format:format block:^(AVAudioPCMBuffer* buffer, AVAudioTime* /*when*/) {
			NSError* write_error = nil;
			[audio_file writeFromBuffer:buffer error:&write_error];
			if (write_error != nil || audio_file.length > static_cast<AVAudioFramePosition>(format.sampleRate * 600.0))
			{
				const std::string message = write_error != nil ? (write_error.localizedDescription.UTF8String ?: "Failed to record microphone audio.") : "Voice recording is limited to 10 minutes.";
				dispatch_async(dispatch_get_main_queue(), ^{ Fail(generation, message); });
			}
		}];
		m_tapInstalled = true;
		[m_audioEngine prepare];
		NSError* start_error = nil;
		if (![m_audioEngine startAndReturnError:&start_error])
		{
			Fail(generation, start_error.localizedDescription.UTF8String ?: "Failed to start microphone capture.");
		}
	}

	void UploadRecording(std::uint64_t generation)
	{
		m_audioFile = nil;
		NSData* audio = [NSData dataWithContentsOfURL:m_audioUrl];
		if (audio.length == 0 || audio.length > 25 * 1024 * 1024)
		{
			Fail(generation, audio.length == 0 ? "No microphone audio was recorded." : "Voice recording exceeds the 25 MB upload limit.");
			return;
		}

		NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:m_options.server_url.c_str()]];
		if (url == nil)
		{
			Fail(generation, "Voice server URL is invalid.");
			return;
		}
		NSString* boundary = [NSString stringWithFormat:@"uam-%@", NSUUID.UUID.UUIDString];
		NSMutableData* body = [NSMutableData data];
		auto append = [body](NSString* value) { [body appendData:[value dataUsingEncoding:NSUTF8StringEncoding]]; };
		append([NSString stringWithFormat:@"--%@\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n", boundary, m_options.server_model.c_str()]);
		append([NSString stringWithFormat:@"--%@\r\nContent-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\nContent-Type: audio/wav\r\n\r\n", boundary]);
		[body appendData:audio];
		append([NSString stringWithFormat:@"\r\n--%@--\r\n", boundary]);

		NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
		request.HTTPMethod = @"POST";
		request.HTTPBody = body;
		request.timeoutInterval = 90;
		[request setValue:[NSString stringWithFormat:@"multipart/form-data; boundary=%@", boundary] forHTTPHeaderField:@"Content-Type"];
		if (!m_options.server_api_key.empty())
		{
			[request setValue:[NSString stringWithFormat:@"Bearer %s", m_options.server_api_key.c_str()] forHTTPHeaderField:@"Authorization"];
		}

		m_uploadTask = [[NSURLSession sharedSession] dataTaskWithRequest:request completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
			dispatch_async(dispatch_get_main_queue(), ^{
				[[NSFileManager defaultManager] removeItemAtURL:m_audioUrl error:nil];
				if (!IsCurrent(generation)) return;
				if (error != nil)
				{
					Fail(generation, error.localizedDescription.UTF8String ?: "Voice transcription request failed.");
					return;
				}
				const NSInteger status = [(NSHTTPURLResponse*)response statusCode];
				if (status < 200 || status >= 300 || data.length > 1024 * 1024)
				{
					Fail(generation, status < 200 || status >= 300 ? "Voice server rejected the transcription request." : "Voice server response is too large.");
					return;
				}
				std::string transcript;
				std::string parse_error;
				const std::string response_text(static_cast<const char*>(data.bytes), data.length);
				if (!uam::voice_input::ParseTranscriptResponse(response_text, transcript, &parse_error))
				{
					Fail(generation, parse_error);
					return;
				}
				Enqueue({DictationEventType::Final, std::move(transcript)});
				Complete(generation);
			});
		}];
		[m_uploadTask resume];
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
		m_audioFile = nil;
		[m_uploadTask cancel];
		m_uploadTask = nil;
		if (m_audioUrl != nil)
		{
			[[NSFileManager defaultManager] removeItemAtURL:m_audioUrl error:nil];
			m_audioUrl = nil;
		}
		m_stopRequested = false;
		m_uploading = false;
		std::fill(m_options.server_api_key.begin(), m_options.server_api_key.end(), '\0');
		m_options = {};
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
	AVAudioFile* m_audioFile = nil;
	NSURL* m_audioUrl = nil;
	NSURLSessionDataTask* m_uploadTask = nil;
	DictationOptions m_options;
	bool m_uploading = false;
};

IPlatformDictationService& GetMacDictationService()
{
	static MacDictationService instance;
	return instance;
}

} // namespace uam::platform_macos_impl

#endif // defined(__APPLE__)
