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
