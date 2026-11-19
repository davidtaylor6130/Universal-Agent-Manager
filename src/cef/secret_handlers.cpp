#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_async.h"

#include "app/secret_request_service.h"
#include "app/uam_control_service.h"
#include "cef/cef_push.h"
#include "common/secrets/secret_keychain.h"
#include "common/secrets/secret_redactor.h"
#include "common/utils/diagnostic_log.h"

#include <algorithm>
#include <memory>

namespace
{
	/// Overwrites the bytes before the buffer is released.
	void Wipe(std::string& value)
	{
		std::fill(value.begin(), value.end(), '\0');
		value.clear();
	}
}

/// The typed secret goes from the card straight to a read-once drop; it never enters chat state or a reply.
void UamQueryHandler::HandleSubmitSecretRequest(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	const std::string chat_id = payload.value("chatId", "");
	const std::string request_id = payload.value("requestId", "");
	const bool use_saved = payload.value("useSaved", false) == true;
	const bool save = !use_saved && payload.value("save", false) == true;
	auto value = std::make_shared<std::string>();
	if (const auto found = payload.find("value"); found != payload.end() && found->is_string()) *value = found->get<std::string>();
	if (!value->empty() && value->back() == '\n') value->pop_back();
	if (!value->empty() && value->back() == '\r') value->pop_back();
	if ((!use_saved && value->empty()) || value->size() > uam::secrets::kMaxSecretBytes ||
	    std::ranges::any_of(*value, [](char ch) { return ch == '\0' || ch == '\n' || ch == '\r'; }))
	{
		Wipe(*value);
		cb->Failure(400, "Enter a single-line secret of at most 8192 bytes.");
		return;
	}
	uam::SecretDeliveryTarget target;
	std::string error;
	if (!uam::UamControlService::BeginSecretDelivery(m_app, chat_id, request_id, target, &error))
	{
		Wipe(*value);
		cb->Failure(409, error);
		return;
	}
	if (const std::string refusal = uam::SecretRequestService::SaveRefusal(target, save, use_saved); !refusal.empty())
	{
		Wipe(*value);
		uam::secrets::DropResult refused;
		refused.error = refusal;
		(void)uam::UamControlService::FinishSecretDelivery(m_app, chat_id, request_id, refused);
		cb->Failure(400, refusal);
		return;
	}
	// Registered before anything can echo it, so every later sink scrubs it.
	(void)uam::secrets::Register(*value);
	const bool save_here = save;
	auto saved = std::make_shared<bool>(false);
	auto drop = std::make_shared<uam::secrets::DropResult>();
	uam::PushStateUpdateIfChanged(browser, m_app);
	// SSH delivery and a Keychain prompt can wait; they run on a slow-work thread keyed by host, off the shared file lanes.
	const CefRefPtr<CefTaskRunner> runner = SlowQueryRunner("secret:" + (target.remote_host.has_value() ? target.remote_host->id : std::string("local")));
	const bool queued = RunAsyncCefQuery(runner, m_asyncLifetime, cb,
	    [target, value, use_saved, save_here, saved, drop]()
	    {
		    std::string failure;
		    if (use_saved)
		    {
			    std::optional<std::string> loaded = uam::secrets::keychain::Load(target.keychain_account, &failure);
			    if (!loaded.has_value())
			    {
				    drop->error = failure.empty() ? "No saved secret was found. Type it instead." : "The saved secret could not be read: " + failure;
				    return AsyncSuccess(nlohmann::json::object());
			    }
			    *value = std::move(*loaded);
			    Wipe(*loaded);
			    (void)uam::secrets::Register(*value);
		    }
		    if (save_here && !uam::secrets::keychain::Save(target.keychain_account, *value, &failure))
		    {
			    Wipe(*value);
			    drop->error = "The secret could not be saved: " + failure + " Turn off Save and submit again.";
			    return AsyncSuccess(nlohmann::json::object());
		    }
		    *saved = save_here;
		    *drop = uam::SecretRequestService::Deliver(target, *value);
		    Wipe(*value);
		    return AsyncSuccess(nlohmann::json::object());
	    },
	    [this, browser, chat_id, request_id, target, saved, drop](AsyncCefResult& result)
	    {
		    if (*saved) (void)uam::SecretRequestService::RecordKeychainAccount(m_app.data_root, target.keychain_account, true);
		    std::string finish_error;
		    const bool delivered = uam::UamControlService::FinishSecretDelivery(m_app, chat_id, request_id, *drop, &finish_error);
		    // Outcome only: never the value, its length or the host's error text.
		    uam::diagnostics::Write(std::string("[secret_request] chat=") + chat_id + " host=" +
		        (target.remote_host.has_value() ? target.remote_host->id : std::string("local")) + " outcome=" + (delivered ? "delivered" : "failed"));
		    uam::PushStateUpdateIfChanged(browser, m_app);
		    if (!delivered) result = AsyncFailure(409, finish_error);
	    });
	if (!queued)
	{
		Wipe(*value);
		uam::secrets::DropResult failed;
		failed.error = "The secret could not be delivered. Try again.";
		(void)uam::UamControlService::FinishSecretDelivery(m_app, chat_id, request_id, failed);
	}
}

void UamQueryHandler::HandleForgetSavedSecret(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string request_id = payload.value("requestId", "");
	const std::optional<std::string> account = uam::UamControlService::SecretKeychainAccount(m_app, chat_id, request_id);
	if (!account.has_value())
	{
		cb->Failure(409, "This secret request is no longer pending.");
		return;
	}
	using namespace uam::query_handler_async;
	// The Keychain can show a prompt; never on the UI thread.
	(void)RunAsyncCefQuery(SlowQueryRunner(std::string("secret:local")), m_asyncLifetime, cb,
	    [account = *account]()
	    {
		    std::string error;
		    return uam::secrets::keychain::Forget(account, &error) ? AsyncSuccess(nlohmann::json::object())
		                                                           : AsyncFailure(500, "The saved secret could not be removed: " + error);
	    },
	    [this, browser, account = *account](AsyncCefResult& result)
	    {
		    if (result.ok) (void)uam::SecretRequestService::RecordKeychainAccount(m_app.data_root, account, false);
		    uam::PushStateUpdateIfChanged(browser, m_app);
	    });
}
