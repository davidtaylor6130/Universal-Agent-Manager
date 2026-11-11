#pragma once
// Async bridge infrastructure for handler files that offload work to background threads.
// Include this header (after uam_query_handler.h) in handler files that call RunAsyncCefQuery.

#include "cef/uam_query_handler.h"

#include "include/cef_task.h"

#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace uam::query_handler_async
{

struct AsyncCefResult
{
	bool ok = true;
	int status = 500;
	std::string body;
	std::string error;
	// A completion may hand the callback to a second asynchronous query.
	bool callback_deferred = false;
};

inline AsyncCefResult AsyncSuccess(nlohmann::json body)
{
	return {true, 200, body.dump(), ""};
}

inline AsyncCefResult AsyncFailure(int status, std::string error)
{
	return {false, status, "", std::move(error)};
}

/// <summary>Resolve completion errors before responding; forwarding is deferred only after success.</summary>
template <typename Callback>
void CompleteAsyncCefQuery(Callback callback, AsyncCefResult& result,
                                  const std::function<void(AsyncCefResult&)>& completion = {})
{
	if (completion)
	{
		try
		{
			completion(result);
		}
		catch (const nlohmann::json::exception& ex)
		{
			if (!result.callback_deferred) result = AsyncFailure(400, std::string("Invalid bridge payload: ") + ex.what());
		}
		catch (const std::exception& ex)
		{
			if (!result.callback_deferred) result = AsyncFailure(500, ex.what());
		}
		catch (...)
		{
			if (!result.callback_deferred) result = AsyncFailure(500, "Async bridge completion failed.");
		}
	}
	if (!callback || result.callback_deferred) return;
	if (result.ok)
	{
		callback->Success(result.body);
	}
	else
	{
		callback->Failure(result.status, result.error);
	}
}

class CefQueryCallbackTask : public CefTask
{
  public:
	CefQueryCallbackTask(std::weak_ptr<void> lifetime, CefRefPtr<CefMessageRouterBrowserSide::Callback> callback,
	                     AsyncCefResult result,
	                     std::function<void(AsyncCefResult&)> completion = {})
	    : m_lifetime(std::move(lifetime)), m_callback(std::move(callback)), m_result(std::move(result)), m_completion(std::move(completion))
	{
	}

	void Execute() override
	{
		if (m_lifetime.expired()) return;
		CEF_REQUIRE_UI_THREAD();
		CompleteAsyncCefQuery(m_callback, m_result, m_completion);
	}

  private:
	std::weak_ptr<void> m_lifetime;
	CefRefPtr<CefMessageRouterBrowserSide::Callback> m_callback;
	AsyncCefResult m_result;
	std::function<void(AsyncCefResult&)> m_completion;
	IMPLEMENT_REFCOUNTING(CefQueryCallbackTask);
};

class CefQueryWorkerTask : public CefTask
{
  public:
	CefQueryWorkerTask(std::weak_ptr<void> lifetime, CefRefPtr<CefMessageRouterBrowserSide::Callback> callback,
	                   std::function<AsyncCefResult()> worker,
	                   std::function<void(AsyncCefResult&)> completion = {})
	    : m_lifetime(std::move(lifetime)), m_callback(std::move(callback)), m_worker(std::move(worker)), m_completion(std::move(completion))
	{
	}

	void Execute() override
	{
		if (m_lifetime.expired()) return;
		AsyncCefResult result;
		try
		{
			result = m_worker();
		}
		catch (const std::exception& ex)
		{
			result = AsyncFailure(500, ex.what());
		}
		catch (...)
		{
			result = AsyncFailure(500, "Async bridge request failed.");
		}
		// CEF releases this task after Execute returns; drop worker captures (such as workspace leases) before the completion can run (#647).
		m_worker = nullptr;
		(void)CefPostTask(TID_UI, new CefQueryCallbackTask(m_lifetime, m_callback, std::move(result), std::move(m_completion)));
	}

  private:
	std::weak_ptr<void> m_lifetime;
	CefRefPtr<CefMessageRouterBrowserSide::Callback> m_callback;
	std::function<AsyncCefResult()> m_worker;
	std::function<void(AsyncCefResult&)> m_completion;
	IMPLEMENT_REFCOUNTING(CefQueryWorkerTask);
};

/// <summary>Runs its task once however many lanes it was posted to, so posting it again promotes a task still queued.</summary>
class CefRunOnceTask : public CefTask
{
  public:
	explicit CefRunOnceTask(CefRefPtr<CefTask> task) : m_task(std::move(task)) {}

	void Execute() override
	{
		if (m_started.exchange(true)) return;
		m_task->Execute();
		m_task = nullptr;
	}

  private:
	std::atomic<bool> m_started{false};
	CefRefPtr<CefTask> m_task;
	IMPLEMENT_REFCOUNTING(CefRunOnceTask);
};

/// User requests require immediate results, including when no BrowserView exists.
/// CEF may indefinitely defer BACKGROUND work while the application is idle.
template <typename Worker>
bool RunAsyncCefQuery(std::weak_ptr<void> lifetime, CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, Worker worker)
{
	if (!CefPostTask(TID_FILE_USER_BLOCKING, new CefQueryWorkerTask(std::move(lifetime), callback, std::move(worker))))
	{
		if (callback) callback->Failure(503, "The background task queue is unavailable.");
		return false;
	}
	return true;
}

/// Subprocess and network workers pass a handler-owned runner: CEF runs each shared file lane one task at a
/// time, so a slow host or repository there holds up saved-history reads in every other chat. Completion
/// still runs on the UI thread. A missing runner falls back to the lane.
template <typename Worker, typename Completion = std::function<void(AsyncCefResult&)>>
bool RunAsyncCefQuery(CefRefPtr<CefTaskRunner> runner, std::weak_ptr<void> lifetime, CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, Worker worker, Completion completion = {}, CefThreadId fallback = TID_FILE_USER_BLOCKING)
{
	CefRefPtr<CefTask> task = new CefQueryWorkerTask(std::move(lifetime), callback, std::move(worker), std::move(completion));
	if (runner ? runner->PostTask(task) : CefPostTask(fallback, task)) return true;
	if (callback) callback->Failure(503, "The background task queue is unavailable.");
	return false;
}

template <typename Worker, typename Completion>
bool RunAsyncCefQuery(std::weak_ptr<void> lifetime, CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, Worker worker, Completion completion, CefThreadId thread_id = TID_FILE_USER_BLOCKING, CefRefPtr<CefTask>* promotable = nullptr)
{
	CefRefPtr<CefTask> task = new CefQueryWorkerTask(std::move(lifetime), callback, std::move(worker), std::move(completion));
	if (promotable != nullptr) task = new CefRunOnceTask(task);
	if (!CefPostTask(thread_id, task))
	{
		if (callback) callback->Failure(503, "The background task queue is unavailable.");
		return false;
	}
	if (promotable != nullptr) *promotable = task;
	return true;
}

} // namespace uam::query_handler_async
