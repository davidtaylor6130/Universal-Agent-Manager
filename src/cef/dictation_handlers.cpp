#include "cef/uam_query_handler.h"

#include "common/config/voice_input_settings.h"
#include "common/platform/platform_services.h"

#include <cstdlib>
#include <nlohmann/json.hpp>

void UamQueryHandler::HandleStartDictation(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	DictationOptions options;
	options.mode = uam::voice_input::NormalizeMode(m_app.settings.voice_input_mode);
	options.locale = payload.value("locale", "");
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

void UamQueryHandler::HandleStopDictation(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& /*payload*/, CefRefPtr<Callback> cb)
{
	PlatformServicesFactory::Instance().dictation_service.Stop();
	cb->Success(R"({"stopped":true})");
}
