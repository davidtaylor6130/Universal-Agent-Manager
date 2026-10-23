#include "common/config/voice_input_settings.h"
#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_async.h"
#include "common/platform/platform_speech_output.h"

#include "common/platform/platform_services.h"

#include <cstdlib>
#include <nlohmann/json.hpp>

using namespace uam::query_handler_async;

void UamQueryHandler::HandleStartDictation(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (uam::GetSpeechOutputState().speaking)
	{
		cb->Failure(409, "Stop speech playback before starting voice input.");
		return;
	}
	DictationOptions options;
	options.mode = uam::voice_input::NormalizeMode(m_app.settings.voice_input_mode);
	options.locale = payload.value("locale", "");
	options.dictation_id = payload.value("dictationId", "");
	if (options.dictation_id.empty() || options.dictation_id.size() > 128)
	{
		cb->Failure(400, "Invalid dictation recording ID.");
		return;
	}
	if (options.locale.size() > 64)
	{
		cb->Failure(400, "Dictation locale is too long.");
		return;
	}
	if (options.mode == uam::voice_input::kLocalMode)
	{
		cb->Failure(409, "Local AI transcription is coming soon.");
		return;
	}
	if (options.mode == uam::voice_input::kServerMode)
	{
		std::string validation_error;
		if (!uam::voice_input::ValidateServerSettings(m_app.settings.voice_input_server_base_url,
		                                                m_app.settings.voice_input_server_endpoint,
		                                                m_app.settings.voice_input_server_model,
		                                                m_app.settings.voice_input_api_key_env,
		                                                &validation_error))
		{
			cb->Failure(409, validation_error);
			return;
		}
		options.server_url = uam::voice_input::BuildServerUrl(m_app.settings.voice_input_server_base_url,
		                                                        m_app.settings.voice_input_server_endpoint);
		options.server_model = m_app.settings.voice_input_server_model;
		if (!m_app.settings.voice_input_api_key_env.empty())
		{
			const char* key = std::getenv(m_app.settings.voice_input_api_key_env.c_str());
			if (key == nullptr || *key == '\0')
			{
				cb->Failure(409, "Voice API credential environment variable is not set: " + m_app.settings.voice_input_api_key_env);
				return;
			}
			if (std::string_view(key).size() > 8192 || uam::voice_input::HasControlCharacters(key))
			{
				cb->Failure(409, "Voice API credential contains unsupported characters or exceeds its size limit.");
				return;
			}
			options.server_api_key = key;
		}
	}

	std::string error;
	if (!PlatformServicesFactory::Instance().dictation_service.Start(options, &error))
	{
		cb->Failure(409, error.empty() ? "Failed to start dictation." : error);
		return;
	}

	cb->Success(R"({"started":true})");
}

void UamQueryHandler::HandleStopDictation(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string dictation_id = payload.value("dictationId", "");
	if (dictation_id.empty() || dictation_id.size() > 128)
	{
		cb->Failure(400, "Invalid dictation recording ID.");
		return;
	}
	PlatformServicesFactory::Instance().dictation_service.Stop(dictation_id);
	cb->Success(R"({"stopped":true})");
}

void UamQueryHandler::HandleListSpeechVoices(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& /*payload*/, CefRefPtr<Callback> cb)
{
	RunAsyncCefQuery(m_asyncLifetime, cb, []
	{
		std::string error;
		const std::vector<uam::SpeechVoice> voices = uam::ListSpeechVoices(&error);
		if (!error.empty()) return AsyncFailure(503, error);
		nlohmann::json result = nlohmann::json::array();
		for (const uam::SpeechVoice& voice : voices) result.push_back({{"id", voice.id}, {"name", voice.name}, {"language", voice.language}});
		return AsyncSuccess({{"voices", std::move(result)}});
	});
}

void UamQueryHandler::HandleStartSpeechOutput(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	for (const char* key : {"speechId", "text", "voiceId", "language"})
	{
		if (payload.contains(key) && !payload[key].is_string())
		{
			cb->Failure(400, "Speech IDs, text, voice and language must be strings.");
			return;
		}
	}
	for (const char* key : {"rate", "volume"})
	{
		if (payload.contains(key) && !payload[key].is_number())
		{
			cb->Failure(400, "Speech speed and volume must be numbers.");
			return;
		}
	}
	uam::SpeechOutputOptions options;
	options.speech_id = payload.value("speechId", "");
	options.text = payload.value("text", "");
	options.voice_id = payload.value("voiceId", "");
	options.language = payload.value("language", "");
	options.rate = payload.value("rate", 1.0);
	options.volume = payload.value("volume", 1.0);
	std::string error;
	if (!uam::ValidateSpeechOutputOptions(options, &error))
	{
		cb->Failure(400, error);
		return;
	}
	if (PlatformServicesFactory::Instance().dictation_service.IsRunning())
	{
		cb->Failure(409, "Stop voice input before starting speech playback.");
		return;
	}
	const std::uint64_t request = uam::PrepareSpeechOutputRequest(options.speech_id);
	if (!RunAsyncCefQuery(m_asyncLifetime, cb, [options, request]
	{
		std::string error;
		if (PlatformServicesFactory::Instance().dictation_service.IsRunning())
		{
			(void)uam::StopSpeechOutput(options.speech_id);
			return AsyncFailure(409, "Stop voice input before starting speech playback.");
		}
		if (!uam::StartSpeechOutput(options, request, &error)) return AsyncFailure(503, error.empty() ? "Speech playback could not start." : error);
		return AsyncSuccess({{"started", true}, {"speechId", options.speech_id}});
	}))
	{
		(void)uam::StopSpeechOutput(options.speech_id);
	}
}

void UamQueryHandler::HandleStopSpeechOutput(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!payload.contains("speechId") || !payload["speechId"].is_string())
	{
		cb->Failure(400, "Invalid speech playback ID.");
		return;
	}
	const std::string speech_id = payload["speechId"].get<std::string>();
	if (speech_id.empty() || speech_id.size() > 128)
	{
		cb->Failure(400, "Invalid speech playback ID.");
		return;
	}
	RunAsyncCefQuery(m_asyncLifetime, cb, [speech_id] { return AsyncSuccess({{"stopped", uam::StopSpeechOutput(speech_id)}}); });
}

void UamQueryHandler::HandleGetSpeechOutputState(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& /*payload*/, CefRefPtr<Callback> cb)
{
	const uam::SpeechOutputState state = uam::GetSpeechOutputState();
	cb->Success(nlohmann::json{{"speechId", state.speech_id}, {"speaking", state.speaking}, {"error", state.error}}.dump());
}
