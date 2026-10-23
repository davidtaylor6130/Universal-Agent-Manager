#include "cef/uam_query_handler.h"

#include "app/agent_run_ledger.h"
#include "app/managed_subagent_control_service.h"
#include <algorithm>
#include "app/managed_subagent_followup_service.h"
#include "common/utils/uuid.h"
#include "common/utils/string_utils.h"
#include "cef/cef_push.h"
#include "cef/uam_query_handler_async.h"

#include <optional>

namespace
{
	constexpr std::size_t kMaxControlScopes = 8;

	std::optional<std::string> Id(const nlohmann::json& payload, const char* key, bool required)
	{
		if (!payload.is_object()) return std::nullopt;
		const auto item = payload.find(key);
		if (item == payload.end()) return required ? std::nullopt : std::optional<std::string>{""};
		if (!item->is_string()) return std::nullopt;
		const auto value = item->get<std::string>();
		if (value.size() > 160 || value.find('\0') != std::string::npos || (required && value.empty())) return std::nullopt;
		return value;
	}

	std::optional<std::size_t> Count(const nlohmann::json& payload, const char* key,
	    std::size_t fallback, std::size_t maximum)
	{
		const auto item = payload.find(key);
		if (item == payload.end()) return fallback;
		if (!item->is_number_integer() || (!item->is_number_unsigned() && item->get<int64_t>() < 0)) return std::nullopt;
		const auto value = item->get<uint64_t>();
		return value <= maximum ? std::optional<std::size_t>{static_cast<std::size_t>(value)} : std::nullopt;
	}
}


bool UamQueryHandler::DispatchManagedSubagentQuery(CefRefPtr<Callback> cb,
    ManagedSubagentWorker worker, ManagedSubagentCompletion completion)
{
	if (m_managedSubagentDispatcherForTests)
		return m_managedSubagentDispatcherForTests(cb, std::move(worker), std::move(completion));
	return uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, std::move(worker), std::move(completion));
}

void UamQueryHandler::HandleListManagedSubagents(CefRefPtr<CefBrowser> /*browser*/,
    const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	const auto chat_id = Id(payload, "chatId", true);
	const auto run_id = Id(payload, "runId", false);
	if (!chat_id || !run_id)
	{
		cb->Failure(400, "Managed subagent chat and run identifiers are invalid.");
		return;
	}
	const auto offset = Count(payload, "offset", 0, uam::ManagedSubagentControlService::MaxLedgerRuns);
	const auto limit = Count(payload, "limit", 50, uam::ManagedSubagentControlService::MaxPageSize);
	if (!offset || !limit || *limit == 0)
	{
		cb->Failure(400, "Managed subagent pagination is invalid.");
		return;
	}
	const std::string key = "list:" + *chat_id;
	if (m_managedSubagentQueries.contains(key) || m_managedSubagentQueries.size() >= kMaxControlScopes)
	{
		cb->Failure(409, "A bounded managed subagent query is already pending; retry shortly.");
		return;
	}
	auto snapshot = std::make_shared<uam::ManagedSubagentSnapshot>();
	std::string error;
	if (!uam::ManagedSubagentControlService::Capture(m_app, *chat_id, snapshot.get(), &error, *run_id, *offset, *limit))
	{
		cb->Failure(409, error);
		return;
	}
	auto read = std::make_shared<uam::ManagedSubagentDetachedRead>();
	const auto operation = std::make_shared<const char>(0);
	m_managedSubagentQueries.emplace(key, operation);
	if (!DispatchManagedSubagentQuery(cb,
	    [snapshot, read]()
	    {
		    *read = uam::ManagedSubagentControlService::ReadDetached(*snapshot);
		    return read->ready ? AsyncSuccess({}) : AsyncFailure(409, read->error);
	    },
	    [this, chat_id = *chat_id, run_id = *run_id, key, operation, snapshot, read](AsyncCefResult& result)
	    {
		    const auto pending = m_managedSubagentQueries.find(key);
		    if (pending == m_managedSubagentQueries.end() || pending->second != operation)
		    {
			    result = AsyncFailure(409, "Managed subagent query was replaced.");
			    return;
		    }
		    m_managedSubagentQueries.erase(pending);
		    if (!result.ok) return;
		    if (!uam::ManagedSubagentControlService::MatchesSnapshot(m_app, *snapshot))
		    {
			    result = AsyncFailure(409, "Managed subagent ownership changed while reading; refresh the listing.");
			    return;
		    }
		    auto& page = read->page;
		    if (!m_managedSubagentScopes.contains(chat_id) && m_managedSubagentScopes.size() >= kMaxControlScopes)
			    m_managedSubagentScopes.erase(m_managedSubagentScopes.begin());
		    const auto previous = m_managedSubagentScopes.find(chat_id);
		    const std::string scope_id = previous != m_managedSubagentScopes.end() && previous->second.second == page.scope
			    ? previous->second.first : uam::AgentRunLedger::NewRunId();
		    m_managedSubagentScopes.insert_or_assign(chat_id, std::make_pair(scope_id, std::move(page.scope)));
		    auto response = uam::ManagedSubagentControlService::ToJson(page);
		    response["chatId"] = chat_id;
		    response["controlScopeId"] = scope_id;
		    if (!run_id.empty())
			    for (auto& row : response["runs"])
				    if (row["runId"] == run_id && !row.value("lastReported", false))
					    if (const auto hint = ManagedFollowupHint(chat_id, run_id)) row["followup"] = *hint;
		    result = AsyncSuccess(std::move(response));
	    })) m_managedSubagentQueries.erase(key);
}

namespace
{
	const char* FollowupCode(uam::ManagedFollowupCode code)
	{
		using enum uam::ManagedFollowupCode;
		switch (code)
		{
		case None: return "none";
		case NotImplemented: return "not_implemented";
		case InvalidRequest: return "invalid_request";
		case LegacyIdentityUnverified: return "legacy_identity_unverified";
		case ScopeChanged: return "scope_changed";
		case Busy: return "busy";
		case CancelPending: return "cancel_pending";
		case PermissionDenied: return "permission_denied";
		case QueueFull: return "queue_full";
		case ReceiptLimit: return "receipt_limit";
		case TranscriptUnavailable: return "transcript_unavailable";
		case ModelIdentityUnverified: return "model_identity_unverified";
		case NativeResumeUnavailable: return "native_resume_unavailable";
		case PersistenceFailed: return "persistence_failed";
		case DeliveryUnknown: return "delivery_unknown";
		case IdempotencyConflict: return "idempotency_conflict";
		}
		return "not_implemented";
	}
}

std::optional<nlohmann::json> UamQueryHandler::ManagedFollowupHint(const std::string& root_id, const std::string& run_id, const ChatSession* transcript) const
{
	if (m_app.agent_runs.size() > uam::ManagedSubagentControlService::MaxLedgerRuns || std::ranges::count(m_app.agent_runs, run_id, &AgentRun::id) != 1) return std::nullopt;
	const auto found = std::ranges::find(m_app.agent_runs, run_id, &AgentRun::id);
	if (found == m_app.agent_runs.end() || found->root_chat_id != root_id || !found->managed_continuation) return std::nullopt;
	const auto& value = *found->managed_continuation;
	if (transcript)
	{
		if (transcript->id != found->transcript_chat_id || transcript->agent_run_id != run_id || transcript->provider_id != found->provider_id || transcript->managed_followup_incarnation != value.owner.transcript_birth_id || (value.execution && (transcript->native_session_id != value.execution->native_session_id || transcript->execution_host_id != value.execution->execution_host_id || transcript->workspace_directory != value.execution->workspace_directory))) return std::nullopt;
	}
	const auto state = uam::ManagedSubagentFollowupService::GetState(m_app, root_id, run_id);
	return nlohmann::json{{"canSend", state.can_send}, {"code", FollowupCode(state.code)}, {"currentTurnId", state.current_turn_id}, {"currentTurnNumber", state.current_turn_number}};
}

void UamQueryHandler::HandleSubmitManagedSubagentFollowup(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const auto fail = [&](int code, const std::string& error)
	{
		// A callback may throw after observing a reply. Never attempt a second reply.
		try { cb->Failure(code, error); } catch (...) {}
	};
	const auto root_id = Id(payload, "chatId", true);
	const auto run_id = Id(payload, "runId", true);
	const auto scope_id = Id(payload, "controlScopeId", true);
	const auto after = Id(payload, "afterTurnId", true);
	const auto client = Id(payload, "clientRequestId", true);
	if (!root_id || !run_id || !scope_id || !after || !client || !payload.is_object() || payload.size() != 6 || !payload.contains("text") || !payload["text"].is_string() || !uam::uuid::IsCanonicalUuid(*run_id) || !uam::uuid::IsCanonicalUuid(*after) || !uam::uuid::IsCanonicalUuid(*client))
	{
		fail(400, "Managed follow-up requires exactly its six text and identity fields.");
		return;
	}
	const auto text = payload["text"].get<std::string>();
	if (text.empty() || text.size() > 32U * 1024U || text.find('\0') != std::string::npos || uam::strings::IsBlank(text))
	{
		fail(400, "Managed follow-up text must be nonblank and at most 32 KiB.");
		return;
	}
	const auto admitted_scope = m_managedSubagentScopes.find(*root_id);
	if (admitted_scope == m_managedSubagentScopes.end() || admitted_scope->second.first != *scope_id)
	{
		fail(409, "Managed subagent listing changed; refresh before submitting follow-up.");
		return;
	}
	const auto original_scope = admitted_scope->second.second;
	std::string scope_error;
	if (!uam::ManagedSubagentControlService::ValidateFollowupScope(m_app, *root_id, *run_id, original_scope, &scope_error))
	{
		fail(409, scope_error);
		return;
	}
	const std::string key = "followup:" + *root_id + ":" + *run_id;
	if (m_managedSubagentQueries.contains(key) || m_managedSubagentQueries.size() >= kMaxControlScopes)
	{
		fail(409, "A bounded managed subagent operation is already pending; retry shortly.");
		return;
	}
	auto operation = std::make_shared<ManagedFollowupOperation>();
	const auto query = m_managedFollowupQueryIdentities.find(cb.get());
	if (query != m_managedFollowupQueryIdentities.end())
	{
		const auto prior = m_managedFollowupQueryOperations.find(query->second);
		if (prior != m_managedFollowupQueryOperations.end() && !prior->second.expired())
		{
			fail(409, "This managed follow-up query already owns an operation.");
			return;
		}
		operation->query_identity = query->second;
		m_managedFollowupQueryOperations.insert_or_assign(query->second, operation);
	}
	const auto invocation = std::make_shared<const char>(0);
	m_managedSubagentQueries.emplace(key, invocation);
	uam::ManagedFollowupRequest request;
	request.after_turn_id = *after;
	request.client_request_id = *client;
	request.text = text; // Exact authored bytes; Native/None/retry-empty are server defaults.
	const std::weak_ptr<void> lifetime = m_asyncLifetime;
	const auto complete = [this, lifetime, cb, root_id = *root_id, run_id = *run_id, key, invocation, operation, original_scope, request](const uam::ManagedFollowupSubmitResult& result) -> bool
	{
		if (lifetime.expired() || operation->settled) return false;
		const auto owns_invocation = [&]
		{
			const auto current = m_managedSubagentQueries.find(key);
			return current != m_managedSubagentQueries.end() && current->second == invocation;
		};
		const auto retire = [&]
		{
			if (owns_invocation()) m_managedSubagentQueries.erase(key);
			if (operation->query_identity)
			{
				const auto current = m_managedFollowupQueryOperations.find(*operation->query_identity);
				if (current != m_managedFollowupQueryOperations.end() && current->second.lock() == operation) m_managedFollowupQueryOperations.erase(current);
			}
		};
		operation->settled = true; // Includes refusal, cancellation and a throwing callback.
		if (operation->cancelled || !owns_invocation()) { retire(); return false; }
		try
		{
			if (!uam::ManagedSubagentControlService::ValidateFollowupCompletionScope(m_app, root_id, run_id, original_scope))
			{
				try { cb->Failure(409, "Managed follow-up ownership changed before receipt delivery."); } catch (...) {}
				if (!lifetime.expired()) retire();
				return false;
			}
			nlohmann::json acceptance = nullptr;
			if (result.accepted)
			{
				const auto run = std::ranges::find(m_app.agent_runs, run_id, &AgentRun::id);
				bool valid = result.code == uam::ManagedFollowupCode::None && result.acceptance && run != m_app.agent_runs.end() && run->managed_continuation;
				if (valid)
				{
					const auto& value = *result.acceptance;
					const auto& managed = *run->managed_continuation;
					const auto turn = std::ranges::find(managed.turns, value.turn_id, &uam::ManagedRunTurnReceipt::turn_id);
					valid = value.root_chat_id == root_id && value.run_id == run_id && value.transcript_chat_id == run->transcript_chat_id && value.client_request_id == request.client_request_id && value.turn_number > 1 && uam::uuid::IsCanonicalUuid(value.turn_id) && value.status_at_admission == uam::ManagedRunTurnStatus::Queued && turn != managed.turns.end();
					if (valid)
						valid = turn->turn_number == value.turn_number && turn->client_request_id == request.client_request_id && turn->after_turn_id == request.after_turn_id && turn->text == request.text && turn->delivery_id == value.turn_id && turn->continuity == uam::ManagedRunContinuity::Native && turn->recovery_decision == uam::ManagedRunRecoveryDecision::None && turn->retry_of_turn_id.empty() && turn->principal.kind == uam::ManagedRunPrincipalKind::UserRoot && turn->principal.root_chat_id == root_id && turn->principal.root_birth_id == managed.owner.root_birth_id && turn->principal.parent_run_id.empty() && turn->principal.parent_transcript_birth_id.empty() && (result.replayed || (managed.current_turn_id == value.turn_id && managed.current_turn_number == value.turn_number && turn->status == uam::ManagedRunTurnStatus::Queued && !turn->dispatch_attempted));
					if (valid)
						acceptance = {{"rootChatId", value.root_chat_id}, {"runId", value.run_id}, {"transcriptChatId", value.transcript_chat_id}, {"clientRequestId", value.client_request_id}, {"turnId", value.turn_id}, {"turnNumber", value.turn_number}, {"statusAtAdmission", "queued"}};
				}
				if (!valid)
				{
					try { cb->Failure(409, "Managed follow-up acceptance identity could not be confirmed."); } catch (...) {}
					if (!lifetime.expired()) retire();
					return false;
				}
			}
			else if (result.acceptance || result.replayed || result.code == uam::ManagedFollowupCode::None)
			{
				try { cb->Failure(409, "Managed follow-up classification could not be confirmed."); } catch (...) {}
				if (!lifetime.expired()) retire();
				return false;
			}
			cb->Success(nlohmann::json{{"followup", {{"accepted", result.accepted}, {"replayed", result.replayed}, {"code", FollowupCode(result.code)}, {"acceptance", std::move(acceptance)}}}}.dump());
			// Success is IPC callback invocation, not evidence of renderer/network receipt.
			if (lifetime.expired()) return false;
			const bool acknowledged = !operation->cancelled && owns_invocation() && uam::ManagedSubagentControlService::ValidateFollowupCompletionScope(m_app, root_id, run_id, original_scope);
			retire();
			return acknowledged;
		}
		catch (...)
		{
			if (!lifetime.expired()) retire();
			return false; // In particular, never send Failure after a throwing Success.
		}
	};
	uam::ManagedFollowupCode rejection = uam::ManagedFollowupCode::None;
	try
	{
		if (uam::ManagedSubagentFollowupService::TrySubmit(m_app, *root_id, *run_id, request, complete, &rejection) == uam::ManagedFollowupAdmission::Rejected)
		{
			uam::ManagedFollowupSubmitResult result;
			result.code = rejection;
			(void)complete(result);
		}
	}
	catch (...)
	{
		uam::ManagedFollowupSubmitResult result;
		result.code = uam::ManagedFollowupCode::DeliveryUnknown;
		(void)complete(result); // Any later real completion is settled and cannot ACK input.
	}
}

void UamQueryHandler::HandleCancelManagedSubagent(CefRefPtr<CefBrowser> browser,
    const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	const auto chat_id = Id(payload, "chatId", true);
	const auto run_id = Id(payload, "runId", true);
	const auto scope_id = Id(payload, "controlScopeId", true);
	if (!chat_id || !run_id || !scope_id)
	{
		cb->Failure(400, "Managed subagent cancellation identifiers are invalid.");
		return;
	}
	const auto scope = m_managedSubagentScopes.find(*chat_id);
	if (scope == m_managedSubagentScopes.end() || scope->second.first != *scope_id)
	{
		cb->Failure(409, "Managed subagent listing changed; refresh before stopping this child.");
		return;
	}
	const std::string key = "cancel:" + *chat_id + ":" + *run_id;
	if (m_managedSubagentQueries.contains(key) || m_managedSubagentQueries.size() >= kMaxControlScopes)
	{
		cb->Failure(409, "A bounded managed subagent query is already pending; retry shortly.");
		return;
	}
	auto snapshot = std::make_shared<uam::ManagedSubagentSnapshot>();
	std::string error;
	if (!uam::ManagedSubagentControlService::Capture(m_app, *chat_id, snapshot.get(), &error, *run_id, 0, 1))
	{
		cb->Failure(409, error);
		return;
	}
	auto read = std::make_shared<uam::ManagedSubagentDetachedRead>();
	const auto operation = std::make_shared<const char>(0);
	const auto original_scope = scope->second.second;
	m_managedSubagentQueries.emplace(key, operation);
	if (!DispatchManagedSubagentQuery(cb,
	    [snapshot, read]()
	    {
		    *read = uam::ManagedSubagentControlService::ReadDetached(*snapshot, false);
		    return read->ready ? AsyncSuccess({}) : AsyncFailure(409, read->error);
	    },
	    [this, browser, chat_id = *chat_id, run_id = *run_id, scope_id = *scope_id,
	     original_scope, key, operation, snapshot, read](AsyncCefResult& result)
	    {
		    const auto pending = m_managedSubagentQueries.find(key);
		    if (pending == m_managedSubagentQueries.end() || pending->second != operation)
		    {
			    result = AsyncFailure(409, "Managed subagent cancellation query was replaced.");
			    return;
		    }
		    m_managedSubagentQueries.erase(pending);
		    if (!result.ok) return;
		    const auto current_scope = m_managedSubagentScopes.find(chat_id);
		    if (current_scope == m_managedSubagentScopes.end() || current_scope->second.first != scope_id)
		    {
			    result = AsyncFailure(409, "Managed subagent listing changed during cancellation.");
			    return;
		    }
		    std::string error;
		    const bool cancelled = uam::ManagedSubagentControlService::CancelPrepared(
			    m_app, chat_id, run_id, original_scope, *snapshot, *read, &error);
		    uam::PushStateUpdateIfChanged(browser, m_app);
		    result = cancelled ? AsyncSuccess({{"runId", run_id}, {"ok", true}}) : AsyncFailure(409, error);
	    })) m_managedSubagentQueries.erase(key);
}
