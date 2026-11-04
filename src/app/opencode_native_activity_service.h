#pragma once

#include "common/provider/opencode_native_http.h"
#include "common/models/app_models.h"

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace uam
{

	struct AppState;
	struct AcpSessionState;
	struct OpenCodeNativeCompanion
	{
		opencode_native::Endpoint endpoint;
		struct State;
		std::shared_ptr<State> state;
	};

	enum class OpenCodeNativeGateState { NotApplicable, Pending, Ready, Failed };

	/// Owns bounded native HTTP work; session destruction and UI polling never join network workers.
	class OpenCodeNativeActivityService
	{
	  public:
		struct Query
		{
			opencode_native::Endpoint endpoint;
			std::string workspace, process_id, control_token;
			std::optional<ExecutionHost> host;
			nlohmann::json operation;
		};
		using ExecuteQuery = std::function<bool(const Query&, nlohmann::json&, std::string&, std::stop_token)>;
		explicit OpenCodeNativeActivityService(ExecuteQuery execute = {});
		~OpenCodeNativeActivityService();
		static std::shared_ptr<OpenCodeNativeCompanion> CreateCompanion(opencode_native::Endpoint endpoint);
		static std::optional<bool> TryWritePermissionResponse(AcpSessionState& session, const nlohmann::json& message, std::string* error);
		OpenCodeNativeGateState BeforeDispatch(AppState& app, AcpSessionState& session, ChatSession& chat, std::string& error);
		static OpenCodeNativeGateState PrepareSavedImageRetry(AcpSessionState&, const ChatSession& effective_settings, const std::vector<MessageAttachment>&, std::string& error);
		static nlohmann::json SavedImageRetryMetadata(const AcpSessionState&, const ChatSession& effective_settings);
		bool Poll(AppState& app);
		/// Pending native futures; observe before Stop clears cancelled jobs.
		bool HasPendingWork() const noexcept;
		void Stop();
		/// Exact native ancestry and workspace proof, also used by bounded fixture tests.
		static bool OwnsSession(const std::string& root, const std::string& workspace, const std::string& candidate,
		    const nlohmann::json& sessions, std::string_view platform = {});
		static bool TaskApprovalEnabled(const nlohmann::json& session);

	  private:
		struct State;
		std::unique_ptr<State> m_state;
	};
}
