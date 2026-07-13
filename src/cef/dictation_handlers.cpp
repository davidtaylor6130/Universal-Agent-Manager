#include "cef/uam_query_handler.h"

#include "common/platform/platform_services.h"

#include <nlohmann/json.hpp>

void UamQueryHandler::HandleStartDictation(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string locale = payload.value("locale", "");
	if (locale.size() > 64)
	{
		cb->Failure(400, "Dictation locale is too long.");
		return;
	}

	std::string error;
	if (!PlatformServicesFactory::Instance().dictation_service.Start(locale, &error))
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
