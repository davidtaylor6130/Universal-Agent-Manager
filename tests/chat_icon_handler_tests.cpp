#include "test_harness.h"
#include "cef/uam_query_handler.h"
#include "common/runtime/acp/acp_session_internal.h"
#if defined(__APPLE__)
#include "common/platform/platform_application_macos.h"
#endif

using namespace uam_test;

/// Executes the real icon handler and the scalar identity seam shared by CEF routing.
struct ChatIconHandlerTestAccess
{
	static void Set(UamQueryHandler& handler, CefRefPtr<UamQueryHandler::Callback> callback,
	    const std::string& chat_id, const std::string& value, int browser = 1, std::string frame = "frame", int64_t query = 1)
	{
		handler.BeginChatIconQuery(callback.get(), browser, frame, query);
		handler.HandleSetCustomIcon(nullptr, {{"targetType", "chat"}, {"targetId", chat_id},
		    {"icon", {{"type", "text"}, {"value", value}}}}, callback);
		handler.m_chatIconQueryIdentities.erase(callback.get());
	}
	static void Cancel(UamQueryHandler& handler, int browser, const std::string& frame, int64_t query)
	{
		handler.CancelChatIconQuery(browser, frame, query);
	}
};

namespace
{
	class RecordingIconCallback final : public UamQueryHandler::Callback
	{
	  public:
		int successes = 0, failures = 0, last_failure = 0;
		void Success(const CefString&) override { ++successes; }
		void Success(const void*, size_t) override { ++successes; }
		void Failure(int code, const CefString&) override { ++failures; last_failure = code; }
	  private:
		IMPLEMENT_REFCOUNTING(RecordingIconCallback);
	};

	ChatSession HandlerChat(const std::string& id)
	{
		ChatSession chat;
		chat.id = id; chat.provider_id = "codex-cli"; chat.title = "Private handler fixture";
		chat.created_at = chat.updated_at = "2026-10-13T00:00:00Z";
		chat.custom_icon = {"text", "O"}; chat.messages.push_back({.role = MessageRole::User, .content = "Retained"});
		return chat;
	}

	void DrainIconHandlerSaves(uam::AppState& app)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!app.pending_chat_save_at_by_chat_id.empty() && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
	}
}

UAM_TEST(ChatIconHandlerLatestFailureRollsBackExactlyOnce)
{
	TempDir temp("uam-icon-handler-failure");
	uam::AppState app; app.data_root = temp.root; app.chats.push_back(HandlerChat("chat"));
	UamQueryHandler handler(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Set(handler, callback, "chat", "A");
	UAM_ASSERT_EQ(app.chats.front().custom_icon.value, std::string("A"));
	uam::ResolvePendingChatSaveResults(app, "chat", false);
	UAM_ASSERT_EQ(app.chats.front().custom_icon.value, std::string("O"));
	UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->last_failure, 500);
	uam::ResolvePendingChatSaveResults(app, "chat", false);
	DrainIconHandlerSaves(app); UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->successes, 0);
}

UAM_TEST(ChatIconHandlerSupersededCancellationCannotRollbackNewerIntent)
{
	TempDir temp("uam-icon-handler-superseded");
	uam::AppState app; app.data_root = temp.root; app.chats.push_back(HandlerChat("chat"));
	UamQueryHandler handler(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> first = new RecordingIconCallback, second = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Set(handler, first, "chat", "A", 1, "frame", 1);
	ChatIconHandlerTestAccess::Set(handler, second, "chat", "B", 1, "frame", 2);
	handler.OnQueryCanceled(nullptr, nullptr, 1); // A different CEF scope cannot cancel either request.
	ChatIconHandlerTestAccess::Cancel(handler, 1, "frame", 1);
	UAM_ASSERT_EQ(app.chats.front().custom_icon.value, std::string("B"));
	DrainIconHandlerSaves(app);
	UAM_ASSERT_EQ(first->successes + first->failures, 0); UAM_ASSERT_EQ(second->successes, 1);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, "chat")->custom_icon.value, std::string("B"));
}

UAM_TEST(ChatIconHandlerMatchingCefCancellationRollsBackWithoutCallback)
{
	TempDir temp("uam-icon-handler-cancel");
	uam::AppState app; app.data_root = temp.root; app.chats.push_back(HandlerChat("chat"));
	UamQueryHandler handler(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Set(handler, callback, "chat", "A", -1, "", 7);
	handler.OnQueryCanceled(nullptr, nullptr, 7);
	UAM_ASSERT_EQ(app.chats.front().custom_icon.value, std::string("O"));
	handler.OnQueryCanceled(nullptr, nullptr, 7); DrainIconHandlerSaves(app);
	UAM_ASSERT_EQ(callback->successes + callback->failures, 0);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, "chat")->custom_icon.value, std::string("O"));
}

UAM_TEST(ChatIconHandlerReusedQueryIdIsScopedByBrowserAndFrame)
{
	TempDir temp("uam-icon-handler-query-scope");
	uam::AppState app; app.data_root = temp.root;
	for (const std::string id : {"one", "two", "three"}) app.chats.push_back(HandlerChat(id));
	UamQueryHandler handler(app, "file:///private/index.html");
	CefRefPtr<RecordingIconCallback> one = new RecordingIconCallback, two = new RecordingIconCallback, three = new RecordingIconCallback;
	ChatIconHandlerTestAccess::Set(handler, one, "one", "A", 1, "f1", 7);
	ChatIconHandlerTestAccess::Set(handler, two, "two", "B", 2, "f1", 7);
	ChatIconHandlerTestAccess::Set(handler, three, "three", "C", 1, "f2", 7);
	ChatIconHandlerTestAccess::Cancel(handler, 1, "f1", 7);
	UAM_ASSERT_EQ(app.chats[0].custom_icon.value, std::string("O"));
	UAM_ASSERT_EQ(app.chats[1].custom_icon.value, std::string("B")); UAM_ASSERT_EQ(app.chats[2].custom_icon.value, std::string("C"));
	DrainIconHandlerSaves(app);
	UAM_ASSERT_EQ(one->successes + one->failures, 0); UAM_ASSERT_EQ(two->successes, 1); UAM_ASSERT_EQ(three->successes, 1);
}

UAM_TEST(ChatIconHandlerRootAndIncarnationChangesProtectReplacement)
{
	for (const bool root_switch : {false, true})
	{
		TempDir temp("uam-icon-handler-owner");
		uam::AppState app; app.data_root = temp.root / "first"; app.chats.push_back(HandlerChat("chat"));
		UamQueryHandler handler(app, "file:///private/index.html");
		CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
		ChatIconHandlerTestAccess::Set(handler, callback, "chat", "A");
		if (root_switch) app.data_root = temp.root / "second";
		else { app.chats.front() = HandlerChat("chat"); app.chats.front().custom_icon = {"text", "A"}; }
		uam::ResolvePendingChatSaveResults(app, "chat", false);
		UAM_ASSERT_EQ(app.chats.front().custom_icon.value, std::string("A"));
		UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->last_failure, 409);
		DrainIconHandlerSaves(app); UAM_ASSERT_EQ(callback->failures, 1);
	}
}

UAM_TEST(ChatIconHandlerDestructionSettlesReceiptWithoutDanglingAccess)
{
	TempDir temp("uam-icon-handler-lifetime");
	uam::AppState app; app.data_root = temp.root; app.chats.push_back(HandlerChat("chat"));
	CefRefPtr<RecordingIconCallback> callback = new RecordingIconCallback;
	{
		UamQueryHandler handler(app, "file:///private/index.html");
		ChatIconHandlerTestAccess::Set(handler, callback, "chat", "A");
	}
	uam::ResolvePendingChatSaveResults(app, "chat", false);
	UAM_ASSERT_EQ(app.chats.front().custom_icon.value, std::string("A"));
	UAM_ASSERT_EQ(callback->failures, 1); UAM_ASSERT_EQ(callback->last_failure, 409);
	DrainIconHandlerSaves(app); UAM_ASSERT_EQ(callback->failures, 1);
}

int main(int argc, char** argv)
{
#if defined(__APPLE__)
	if (const std::optional<int> watchdog_result = uam::platform::RunMacParentDeathWatchdogIfRequested(argc, argv); watchdog_result.has_value())
	{
		return *watchdog_result;
	}
#endif
	std::size_t passed = 0;
	for (const TestCase& test : Registry())
	{
		try { test.fn(); ++passed; std::cout << "PASS " << test.name << '\n'; }
		catch (const std::exception& error) { std::cerr << "FAIL " << test.name << ": " << error.what() << '\n'; }
	}
	std::cout << passed << '/' << Registry().size() << " connected icon cases passed\n";
	return passed == Registry().size() && Registry().size() == 6 ? 0 : 1;
}
