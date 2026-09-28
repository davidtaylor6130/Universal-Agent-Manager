#include "cef/uam_query_handler.h"

#include "common/platform/platform_services.h"

#include <nlohmann/json.hpp>

void UamQueryHandler::HandleStartDictation(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	DictationOptions options;
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
