#include "test_harness.h"
#include "common/config/voice_input_settings.h"

UAM_TEST(VoiceInputSettingsPersistValidateAndParseServerResponses)
{
	uam_test::TempDir temp("uam-voice-settings");
	const std::filesystem::path settings_file = temp.root / "settings.txt";
	AppSettings settings;
	settings.voice_input_mode = "server";
	settings.voice_input_server_base_url = "https://voice.example.test/";
	settings.voice_input_server_endpoint = "/v1/audio/transcriptions";
	settings.voice_input_server_model = "whisper-large-v3";
	settings.voice_input_api_key_env = "UAM_VOICE_API_KEY";
	UAM_ASSERT(SettingsStore::Save(settings_file, settings));

	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(settings_file, loaded).loaded);
	UAM_ASSERT_EQ(loaded.voice_input_mode, std::string("server"));
	UAM_ASSERT_EQ(loaded.voice_input_server_base_url, std::string("https://voice.example.test/"));
	UAM_ASSERT_EQ(loaded.voice_input_server_model, std::string("whisper-large-v3"));
	UAM_ASSERT_EQ(loaded.voice_input_api_key_env, std::string("UAM_VOICE_API_KEY"));
	UAM_ASSERT(uam::io::ReadTextFile(settings_file).find("actual-secret") == std::string::npos);
	const nlohmann::json frontend = uam::settings_frontend_json::SerializeCommonSettingsFields(loaded);
	UAM_ASSERT_EQ(frontend.value("voiceInputMode", ""), std::string("server"));
	UAM_ASSERT_EQ(frontend.value("voiceInputApiKeyEnv", ""), std::string("UAM_VOICE_API_KEY"));
	UAM_ASSERT(frontend["voiceInputCapabilities"]["local"].value("supported", true) == false);
#if defined(__APPLE__)
	UAM_ASSERT(frontend["voiceInputCapabilities"]["server"].value("supported", false));
#else
	UAM_ASSERT(!frontend["voiceInputCapabilities"]["server"].value("supported", true));
#endif

	std::string error;
	UAM_ASSERT(uam::voice_input::HasControlCharacters("credential\r\nInjected: header"));
	UAM_ASSERT(!uam::voice_input::HasControlCharacters("fixture-bearer-token"));
	UAM_ASSERT(uam::voice_input::ValidateServerSettings("https://voice.example.test", "/v1/audio/transcriptions", "whisper-1", "OPENAI_API_KEY", &error));
	UAM_ASSERT(uam::voice_input::ValidateServerSettings("http://localhost:8000", "/v1/audio/transcriptions", "whisper-1", "", &error));
	UAM_ASSERT(!uam::voice_input::ValidateServerSettings("http://voice.example.test", "/v1/audio/transcriptions", "whisper-1", "OPENAI_API_KEY", &error));
	UAM_ASSERT(!uam::voice_input::ValidateServerSettings("https://user:secret@voice.example.test", "/v1/audio/transcriptions", "whisper-1", "OPENAI_API_KEY", &error));
	UAM_ASSERT(!uam::voice_input::ValidateServerSettings("https://voice.example.test", "v1/audio/transcriptions", "whisper-1", "OPENAI_API_KEY", &error));
	UAM_ASSERT(!uam::voice_input::ValidateServerSettings("https://voice.example.test", "/v1/audio/transcriptions", "whisper-1", "bad-name", &error));
	UAM_ASSERT(!uam::voice_input::ValidateServerSettings("https://voice.example.test", "/v1/audio\r\nInjected: header", "whisper-1", "", &error));
	UAM_ASSERT(!uam::voice_input::ValidateServerSettings("https://voice.example.test", "/v1/audio/transcriptions", "whisper\r\n--multipart", "", &error));
	UAM_ASSERT(!uam::voice_input::ValidateServerSettings(std::string("https://voice.example.test\0evil", 31), "/v1/audio/transcriptions", "whisper-1", "", &error));
	UAM_ASSERT_EQ(uam::voice_input::BuildServerUrl("https://voice.example.test/", "/v1/audio/transcriptions"), std::string("https://voice.example.test/v1/audio/transcriptions"));

	std::string transcript;
	UAM_ASSERT(uam::voice_input::ParseTranscriptResponse(R"({"text":" hello world "})", transcript, &error));
	UAM_ASSERT_EQ(transcript, std::string("hello world"));
	UAM_ASSERT(!uam::voice_input::ParseTranscriptResponse(R"({"error":"bad request"})", transcript, &error));
	UAM_ASSERT(transcript.empty());
	UAM_ASSERT(!uam::voice_input::ParseTranscriptResponse(R"({"text":""})", transcript, &error));
	UAM_ASSERT(!uam::voice_input::ParseTranscriptResponse(std::string(1024 * 1024 + 1, 'x'), transcript, &error));
}

UAM_TEST(VoiceRecordingReadRejectsBeforeAllocation)
{
	uam_test::TempDir temp("uam-voice-recording-bounds");
	const std::filesystem::path recording = temp.root / "recording.wav";
	std::string bytes = "stale";
	std::string error;
	UAM_ASSERT(!uam::voice_input::ReadRecording(recording, bytes, error));
	UAM_ASSERT(bytes.empty());
	UAM_ASSERT(uam::io::WriteTextFile(recording, ""));
	UAM_ASSERT(!uam::voice_input::ReadRecording(recording, bytes, error));
	UAM_ASSERT(uam::io::WriteTextFile(recording, std::string(uam::voice_input::kMaxRecordingBytes, 'a')));
	UAM_ASSERT(uam::voice_input::ReadRecording(recording, bytes, error));
	UAM_ASSERT_EQ(bytes.size(), uam::voice_input::kMaxRecordingBytes);
	UAM_ASSERT(error.empty());
	std::filesystem::resize_file(recording, uam::voice_input::kMaxRecordingBytes + 1);
	UAM_ASSERT(!uam::voice_input::ReadRecording(recording, bytes, error));
	UAM_ASSERT(bytes.empty());
	UAM_ASSERT(!error.empty());
}

UAM_TEST(VoiceResponseTransferBoundsAndRecordingLifetime)
{
	std::string bytes;
	std::string error;
	const std::string limit(uam::voice_input::kMaxResponseBytes, 'x');
	uam::voice_input::ResponseBuffer exact;
	UAM_ASSERT(exact.AcceptLength(uam::voice_input::kMaxResponseBytes));
	UAM_ASSERT(exact.Append(limit));
	UAM_ASSERT(exact.TakeForRecording(7, 7, true, bytes, error));
	UAM_ASSERT_EQ(bytes, limit);
	UAM_ASSERT(!exact.Append("late"));
	UAM_ASSERT(!exact.TakeForRecording(7, 7, true, bytes, error));
	UAM_ASSERT(bytes.empty());
	for (const std::int64_t declared : {-1LL, 1LL})
	{
		uam::voice_input::ResponseBuffer oversized;
		UAM_ASSERT(oversized.AcceptLength(declared));
		UAM_ASSERT(oversized.Append(limit));
		UAM_ASSERT(!oversized.Append("x"));
		UAM_ASSERT(!oversized.TakeForRecording(7, 7, true, bytes, error));
		UAM_ASSERT(bytes.empty());
		UAM_ASSERT(!error.empty());
	}
	uam::voice_input::ResponseBuffer advertised;
	UAM_ASSERT(!advertised.AcceptLength(uam::voice_input::kMaxResponseBytes + 1));
	UAM_ASSERT(!advertised.Append("x"));
	UAM_ASSERT(!advertised.TakeForRecording(7, 7, true, bytes, error));
	UAM_ASSERT(!error.empty());
	uam::voice_input::ResponseBuffer canceled;
	UAM_ASSERT(canceled.Append("old result"));
	canceled.Cancel();
	UAM_ASSERT(!canceled.Append("late"));
	UAM_ASSERT(!canceled.TakeForRecording(7, 7, true, bytes, error));
	UAM_ASSERT(bytes.empty());
	UAM_ASSERT(error.empty());
	uam::voice_input::ResponseBuffer stale;
	UAM_ASSERT(stale.Append("old result"));
	UAM_ASSERT(!stale.TakeForRecording(7, 8, true, bytes, error));
	UAM_ASSERT(!stale.TakeForRecording(7, 7, true, bytes, error));
	UAM_ASSERT(bytes.empty());
	uam::voice_input::ResponseBuffer newer;
	UAM_ASSERT(newer.Append("new result"));
	UAM_ASSERT(newer.TakeForRecording(8, 8, true, bytes, error));
	UAM_ASSERT_EQ(bytes, std::string("new result"));
	uam::voice_input::ResponseBuffer stopped;
	UAM_ASSERT(stopped.Append("result"));
	UAM_ASSERT(!stopped.TakeForRecording(8, 8, false, bytes, error));
	UAM_ASSERT(bytes.empty());
}
