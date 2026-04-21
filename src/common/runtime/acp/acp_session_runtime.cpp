#include "common/runtime/acp/acp_session_runtime.h"

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "common/chat/chat_repository.h"
#include "common/platform/platform_services.h"
#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <iostream>
#include <sstream>
#include <utility>

namespace uam
{
namespace
{
	constexpr std::size_t kMaxRecentStderrBytes = 16 * 1024;
	constexpr const char* kAcpLifecycleStarting = "starting";
	constexpr const char* kAcpLifecycleReady = "ready";
	constexpr const char* kAcpLifecycleProcessing = "processing";
	constexpr const char* kAcpLifecycleWaitingPermission = "waitingPermission";
	constexpr const char* kAcpLifecycleWaitingUserInput = "waitingUserInput";
		constexpr const char* kAcpLifecycleStopped = "stopped";
		constexpr const char* kAcpLifecycleError = "error";
		constexpr std::size_t kMinAssistantReplayPrefixBytes = 32;
		constexpr std::size_t kMaxAcpDiagnosticEntries = 80;
		constexpr std::size_t kMaxAcpDiagnosticFieldBytes = 4096;
		constexpr std::size_t kMaxAcpDiagnosticDetailBytes = 8192;
		constexpr std::size_t kMaxAcpLogFieldBytes = 512;
		constexpr const char* kProtocolGeminiAcp = "gemini-acp";
		constexpr const char* kProtocolCodexAppServer = "codex-app-server";

			void CompletePromptTurn(AcpSessionState& session, const char* lifecycle_state);
			void FailAcpTurnOrSession(AcpSessionState& session, const std::string& message);
			void MarkAcpChatUnseenIfBackground(AppState& app, const ChatSession& chat);
			void SaveChatQuietly(AppState& app, const ChatSession& chat);
			std::string JsonDiagnosticStringValue(const nlohmann::json& object, const char* key);
			std::string JsonDiagnosticStringValueOr(const nlohmann::json& object, const char* key, const std::string& fallback);
			bool JsonBooleanValueOr(const nlohmann::json& object, const char* key, bool fallback);
			nlohmann::json JsonObjectValue(const nlohmann::json& object, const char* key);
			nlohmann::json JsonArrayValue(const nlohmann::json& object, const char* key);
			std::string CodexTurnErrorMessage(const nlohmann::json& error);
			std::string CodexTurnErrorDetails(const AcpSessionState& session, const nlohmann::json& params, const nlohmann::json& error);
			std::string FormatAcpFailureMessage(const AcpSessionState& session,
			                                    const std::string& method,
			                                    const std::string& request_id,
			                                    bool has_code,
			                                    int code,
			                                    const std::string& message,
			                                    bool has_detail);
			bool SyncAcpToolCallsToAssistantMessage(ChatSession& chat, AcpSessionState& session, bool create_if_missing);

		bool IsCodexSession(const AcpSessionState& session)
		{
			return session.protocol_kind == kProtocolCodexAppServer || session.provider_id == "codex-cli";
		}

		const char* RuntimeDisplayName(const AcpSessionState& session)
		{
			return IsCodexSession(session) ? "Codex app-server" : "Gemini ACP";
		}

		std::string MessageProviderId(const AcpSessionState& session)
		{
			return session.provider_id.empty() ? std::string(provider_build_config::FirstEnabledProviderId()) : session.provider_id;
		}

	std::string TimestampNow()
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t tt = std::chrono::system_clock::to_time_t(now);
		std::tm tm_snapshot{};
#if defined(_WIN32)
		localtime_s(&tm_snapshot, &tt);
#else
		localtime_r(&tt, &tm_snapshot);
#endif
		std::ostringstream out;
			out << std::put_time(&tm_snapshot, "%Y-%m-%dT%H:%M:%S.000Z");
			return out.str();
		}

		std::string CapDiagnosticString(const std::string& value, const std::size_t max_bytes)
		{
			if (value.size() <= max_bytes)
			{
				return value;
			}

			std::ostringstream out;
			out << value.substr(0, max_bytes) << "... [truncated " << (value.size() - max_bytes) << " bytes]";
			return out.str();
		}

		std::string SanitizeLogField(const std::string& value)
		{
			std::string sanitized = CapDiagnosticString(value, kMaxAcpLogFieldBytes);
			for (char& ch : sanitized)
			{
				if (ch == '\n' || ch == '\r' || ch == '\t')
				{
					ch = ' ';
				}
			}
			return sanitized;
		}

		std::string QuoteLogField(const std::string& value)
		{
			std::string quoted;
			quoted.reserve(value.size() + 2);
			quoted.push_back('"');
			for (const char ch : SanitizeLogField(value))
			{
				if (ch == '"' || ch == '\\')
				{
					quoted.push_back('\\');
				}
				quoted.push_back(ch);
			}
			quoted.push_back('"');
			return quoted;
		}

		std::string JsonRpcIdToDiagnosticString(const nlohmann::json& id)
		{
			if (id.is_null())
			{
				return "";
			}
			if (id.is_string())
			{
				return id.get<std::string>();
			}
			return id.dump();
		}

		std::string AcpProcessHandleLabel(const AcpSessionState& session)
		{
	#if defined(_WIN32)
			if (session.process_info.dwProcessId != 0)
			{
				return std::to_string(static_cast<unsigned long long>(session.process_info.dwProcessId));
			}
	#elif defined(__APPLE__)
			if (session.child_pid > 0)
			{
				return std::to_string(static_cast<long long>(session.child_pid));
			}
	#endif
			return session.last_process_id.empty() ? std::string("0") : session.last_process_id;
		}

		void AppendAcpDiagnostic(AcpSessionState& session,
		                         const std::string& event,
		                         const std::string& reason,
		                         const std::string& method = "",
		                         const std::string& request_id = "",
		                         const bool has_code = false,
		                         const int code = 0,
		                         const std::string& message = "",
		                         const std::string& detail = "")
		{
			AcpDiagnosticEntryState entry;
			entry.time = TimestampNow();
			entry.event = CapDiagnosticString(event, kMaxAcpDiagnosticFieldBytes);
			entry.reason = CapDiagnosticString(reason, kMaxAcpDiagnosticFieldBytes);
			entry.method = CapDiagnosticString(method, kMaxAcpDiagnosticFieldBytes);
			entry.request_id = CapDiagnosticString(request_id, kMaxAcpDiagnosticFieldBytes);
			entry.has_code = has_code;
			entry.code = code;
			entry.message = CapDiagnosticString(message, kMaxAcpDiagnosticFieldBytes);
			entry.detail = CapDiagnosticString(detail, kMaxAcpDiagnosticDetailBytes);
			entry.lifecycle_state = session.lifecycle_state;

			std::ostringstream out;
			out << "[acp-diag]"
			    << " event=" << entry.event
			    << " reason=" << entry.reason
			    << " chat_id=" << session.chat_id
			    << " session_id=" << session.session_id
			    << " process_id=" << AcpProcessHandleLabel(session)
			    << " lifecycle_state=" << entry.lifecycle_state;
			if (!entry.method.empty())
			{
				out << " method=" << entry.method;
			}
			if (!entry.request_id.empty())
			{
				out << " request_id=" << entry.request_id;
			}
			if (entry.has_code)
			{
				out << " code=" << entry.code;
			}
			if (!entry.message.empty())
			{
				out << " message=" << QuoteLogField(entry.message);
			}
			if (!entry.detail.empty())
			{
				out << " detail=" << QuoteLogField(entry.detail);
			}
			out << " t=" << entry.time;
			std::cerr << out.str() << std::endl;

			session.diagnostics.push_back(std::move(entry));
			if (session.diagnostics.size() > kMaxAcpDiagnosticEntries)
			{
				session.diagnostics.erase(session.diagnostics.begin(), session.diagnostics.begin() + static_cast<std::ptrdiff_t>(session.diagnostics.size() - kMaxAcpDiagnosticEntries));
			}
		}

		std::string RecentStderrTail(const AcpSessionState& session)
		{
			return CapDiagnosticString(session.recent_stderr, kMaxAcpDiagnosticDetailBytes);
		}

	nlohmann::json BuildInitializeRequest(const int request_id)
	{
		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "initialize"},
			{"params", {
				{"protocolVersion", 1},
				{"clientCapabilities", nlohmann::json::object()},
				{"clientInfo", {
					{"name", "universal-agent-manager"},
					{"title", "Universal Agent Manager"},
					{"version", "1.0.1"},
				}},
			}},
		};
	}

	nlohmann::json BuildCodexInitializeRequest(const int request_id)
	{
		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "initialize"},
			{"params", {
					{"clientInfo", {
						{"name", "universal-agent-manager"},
						{"title", "Universal Agent Manager"},
						{"version", "1.0.1"},
					}},
					{"capabilities", {
						{"experimentalApi", true},
					}},
				}},
			};
		}

	nlohmann::json BuildCodexInitializedNotification()
	{
		return {
			{"jsonrpc", "2.0"},
			{"method", "initialized"},
		};
	}

	nlohmann::json BuildCodexModelListRequest(const int request_id)
	{
		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "model/list"},
			{"params", nlohmann::json::object()},
		};
	}

	nlohmann::json BuildNewSessionRequest(const int request_id, const std::string& cwd)
	{
		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "session/new"},
			{"params", {
				{"cwd", cwd},
				{"mcpServers", nlohmann::json::array()},
			}},
		};
	}

	nlohmann::json BuildLoadSessionRequest(const int request_id, const std::string& session_id, const std::string& cwd)
	{
		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "session/load"},
			{"params", {
				{"sessionId", session_id},
				{"cwd", cwd},
				{"mcpServers", nlohmann::json::array()},
			}},
		};
	}

	nlohmann::json BuildCodexThreadStartRequest(const int request_id, const ChatSession& chat, const std::string& cwd)
	{
		nlohmann::json params = {
			{"cwd", cwd},
			{"approvalPolicy", "on-request"},
			{"sandbox", "workspace-write"},
			{"serviceName", "universal-agent-manager"},
			{"experimentalRawEvents", false},
			{"persistExtendedHistory", true},
		};

		const std::string model_id = Trim(chat.model_id);
		if (!model_id.empty())
		{
			params["model"] = model_id;
		}

		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "thread/start"},
			{"params", std::move(params)},
		};
	}

	nlohmann::json BuildCodexThreadResumeRequest(const int request_id, const ChatSession& chat, const std::string& cwd)
	{
		nlohmann::json params = {
			{"threadId", chat.native_session_id},
			{"cwd", cwd},
			{"approvalPolicy", "on-request"},
			{"sandbox", "workspace-write"},
			{"persistExtendedHistory", true},
		};

		const std::string model_id = Trim(chat.model_id);
		if (!model_id.empty())
		{
			params["model"] = model_id;
		}

		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "thread/resume"},
			{"params", std::move(params)},
		};
	}

	std::string ValidCodexResumeId(const ChatSession& chat)
	{
		return uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
	}

	std::string ValidGeminiResumeId(const ChatSession& chat)
	{
		return NativeSessionLinkService().HasRealNativeSessionId(chat) ? Trim(chat.native_session_id) : std::string{};
	}

	nlohmann::json BuildGeminiSessionSetupRequest(const int request_id, const ChatSession& chat, const std::string& cwd, const bool load_session_supported)
	{
		const std::string resume_id = ValidGeminiResumeId(chat);
		if (!resume_id.empty() && load_session_supported)
		{
			return BuildLoadSessionRequest(request_id, resume_id, cwd);
		}

		return BuildNewSessionRequest(request_id, cwd);
	}

	std::string LowerAsciiCopy(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch)
		{
			return static_cast<char>(std::tolower(ch));
		});
		return value;
	}

	bool GeminiErrorLooksLikeInvalidSessionId(const std::string& error_message, const std::string& error_data)
	{
		const std::string text = LowerAsciiCopy(error_message + "\n" + error_data);
		return text.find("invalid session identifier") != std::string::npos ||
		       text.find("use --list-sessions") != std::string::npos;
	}

	nlohmann::json BuildCodexSessionSetupRequest(const int request_id, const ChatSession& chat, const std::string& cwd)
	{
		const std::string resume_id = ValidCodexResumeId(chat);
		if (resume_id.empty())
		{
			return BuildCodexThreadStartRequest(request_id, chat, cwd);
		}

		ChatSession resume_chat = chat;
		resume_chat.native_session_id = resume_id;
		return BuildCodexThreadResumeRequest(request_id, resume_chat, cwd);
	}

	nlohmann::json BuildPromptRequest(const int request_id, const std::string& session_id, const std::string& text)
	{
		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "session/prompt"},
			{"params", {
				{"sessionId", session_id},
				{"prompt", nlohmann::json::array({
					{
						{"type", "text"},
						{"text", text},
					},
				})},
			}},
		};
	}

	nlohmann::json BuildCodexTurnStartRequest(const int request_id, const std::string& thread_id, const std::string& text, const ChatSession& chat, const std::string& active_model_id)
	{
		nlohmann::json params = {
			{"threadId", thread_id},
			{"input", nlohmann::json::array({
				{
					{"type", "text"},
					{"text", text},
					{"text_elements", nlohmann::json::array()},
				},
			})},
		};

		const std::string model_id = Trim(chat.model_id);
		const std::string collaboration_model_id = model_id.empty() ? Trim(active_model_id) : model_id;
		if (!model_id.empty())
		{
			params["model"] = model_id;
		}

		const std::string requested_mode_id = Trim(chat.approval_mode) == "plan" ? "plan" : "default";
		if (!collaboration_model_id.empty())
		{
			params["collaborationMode"] = {
				{"mode", requested_mode_id},
				{"settings", {
					{"model", collaboration_model_id},
					{"reasoning_effort", nullptr},
					{"developer_instructions", nullptr},
				}},
			};
		}

		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "turn/start"},
			{"params", std::move(params)},
		};
	}

		nlohmann::json BuildCancelNotification(const std::string& session_id)
		{
			return {
				{"jsonrpc", "2.0"},
				{"method", "session/cancel"},
			{"params", {
				{"sessionId", session_id},
				}},
			};
		}

		nlohmann::json BuildCodexTurnInterruptRequest(const int request_id, const std::string& thread_id, const std::string& turn_id)
		{
			return {
				{"jsonrpc", "2.0"},
				{"id", request_id},
				{"method", "turn/interrupt"},
				{"params", {
					{"threadId", thread_id},
					{"turnId", turn_id},
				}},
			};
		}

		nlohmann::json BuildSetModeRequest(const int request_id, const std::string& session_id, const std::string& mode_id)
		{
			return {
				{"jsonrpc", "2.0"},
				{"id", request_id},
				{"method", "session/set_mode"},
				{"params", {
					{"sessionId", session_id},
					{"modeId", mode_id},
				}},
			};
		}

		nlohmann::json BuildSetModelRequest(const int request_id, const std::string& session_id, const std::string& model_id)
		{
			return {
				{"jsonrpc", "2.0"},
				{"id", request_id},
				{"method", "session/set_model"},
				{"params", {
					{"sessionId", session_id},
					{"modelId", model_id},
				}},
			};
		}

		std::string LaunchApprovalMode(const ChatSession& chat)
		{
			const std::string mode = Trim(chat.approval_mode);
			return (mode == "default" || mode == "plan") ? mode : "";
		}

		std::vector<std::string> BuildAcpLaunchArgv(const ChatSession& chat)
		{
			if (Trim(chat.provider_id) == "codex-cli")
			{
				return {"codex", "app-server", "--listen", "stdio://"};
			}

			std::vector<std::string> argv = {"gemini", "--acp"};
			const std::string approval_mode = LaunchApprovalMode(chat);
			if (!approval_mode.empty())
			{
				argv.push_back("--approval-mode");
				argv.push_back(approval_mode);
			}
			const std::string model_id = Trim(chat.model_id);
			if (!model_id.empty())
			{
			argv.push_back("--model");
			argv.push_back(model_id);
		}
		return argv;
	}

	std::string JoinAcpArgvForDiagnostics(const std::vector<std::string>& argv)
	{
		std::ostringstream out;
		for (std::size_t i = 0; i < argv.size(); ++i)
		{
			if (i > 0)
			{
				out << ' ';
			}
			out << argv[i];
		}
		return out.str();
	}

	std::string BuildAcpLaunchDetail(const std::filesystem::path& workspace_root, const ChatSession& chat)
	{
		const std::vector<std::string> argv = BuildAcpLaunchArgv(chat);
		return "cwd=" + (workspace_root.empty() ? std::filesystem::current_path().string() : workspace_root.string()) +
		       ", argv=" + JoinAcpArgvForDiagnostics(argv) +
		       ", nativeSessionId=" + chat.native_session_id;
	}

		int NextAcpRequestId(AcpSessionState& session, const std::string& method)
		{
			const int id = session.next_request_id++;
			session.pending_request_methods[id] = method;
			return id;
		}

		std::string AcpMessageMethodForDiagnostics(const nlohmann::json& message)
		{
			return message.is_object() ? message.value("method", "") : "";
		}

		std::string AcpMessageRequestIdForDiagnostics(const nlohmann::json& message)
		{
			if (!message.is_object() || !message.contains("id"))
			{
				return "";
			}
			return JsonRpcIdToDiagnosticString(message["id"]);
		}

		std::string PromptLengthDetail(const nlohmann::json& params)
		{
			const nlohmann::json prompt = params.value("prompt", nlohmann::json::array());
			std::size_t prompt_chars = 0;
			if (prompt.is_array())
			{
				for (const nlohmann::json& part : prompt)
				{
					if (part.is_object() && part.contains("text") && part["text"].is_string())
					{
						prompt_chars += part["text"].get<std::string>().size();
					}
				}
			}
			std::ostringstream out;
			out << "sessionId=" << params.value("sessionId", "") << ", prompt_chars=" << prompt_chars;
			return out.str();
		}

		std::string AcpMessageDetailForDiagnostics(const nlohmann::json& message)
		{
			if (!message.is_object())
			{
				return "payload is not an object";
			}

			const std::string method = message.value("method", "");
			const nlohmann::json params = message.value("params", nlohmann::json::object());
			if (method == "initialize")
			{
				return "protocolVersion=" + std::to_string(params.value("protocolVersion", 0));
			}
			if (method == "session/new")
			{
				return "cwd=" + params.value("cwd", "");
			}
			if (method == "session/load")
			{
				return "sessionId=" + params.value("sessionId", "") + ", cwd=" + params.value("cwd", "");
			}
			if (method == "session/prompt")
			{
				return PromptLengthDetail(params);
			}
			if (method == "turn/start")
			{
				std::size_t input_chars = 0;
				const nlohmann::json input = params.value("input", nlohmann::json::array());
				if (input.is_array())
				{
					for (const nlohmann::json& part : input)
					{
						if (part.is_object() && part.contains("text") && part["text"].is_string())
						{
							input_chars += part["text"].get<std::string>().size();
						}
					}
				}
				std::ostringstream out;
				out << "threadId=" << JsonDiagnosticStringValue(params, "threadId") << ", input_chars=" << input_chars;
				const std::string model = JsonDiagnosticStringValue(params, "model");
				if (!model.empty())
				{
					out << ", model=" << model;
				}
				const nlohmann::json collaboration_mode = params.value("collaborationMode", nlohmann::json::object());
				if (collaboration_mode.is_object())
				{
					out << ", collaborationMode=" << JsonDiagnosticStringValue(collaboration_mode, "mode");
					const nlohmann::json settings = collaboration_mode.value("settings", nlohmann::json::object());
					const std::string collaboration_model = JsonDiagnosticStringValue(settings, "model");
					if (!collaboration_model.empty())
					{
						out << ", collaborationModel=" << collaboration_model;
					}
				}
				return out.str();
			}
				if (method == "session/cancel")
				{
					return "sessionId=" + params.value("sessionId", "");
				}
				if (method == "session/set_mode")
				{
					return "sessionId=" + params.value("sessionId", "") + ", modeId=" + params.value("modeId", "");
				}
				if (method == "session/set_model")
				{
					return "sessionId=" + params.value("sessionId", "") + ", modelId=" + params.value("modelId", "");
				}
				if (message.contains("error"))
				{
				const nlohmann::json error = message.value("error", nlohmann::json::object());
				std::ostringstream out;
				out << "error_code=" << error.value("code", 0) << ", error_message=" << error.value("message", "");
				return out.str();
			}
			if (message.contains("result"))
			{
				return "response_result=" + CapDiagnosticString(message.value("result", nlohmann::json(nullptr)).dump(), kMaxAcpDiagnosticFieldBytes);
			}
			return "";
		}

		bool WriteAcpMessage(AcpSessionState& session, const nlohmann::json& message, std::string* error_out = nullptr)
		{
			std::string line = message.dump();
			line.push_back('\n');

			const std::string method = AcpMessageMethodForDiagnostics(message);
			const std::string request_id = AcpMessageRequestIdForDiagnostics(message);
			const std::string detail = AcpMessageDetailForDiagnostics(message);
			std::string write_error;
			if (!PlatformServicesFactory::Instance().process_service.WriteToStdioProcess(session, line.data(), line.size(), &write_error))
			{
				const std::string runtime_name = RuntimeDisplayName(session);
				session.last_error = write_error.empty() ? ("Failed to write message to " + runtime_name + ".") : ("Failed to write message to " + runtime_name + ": " + write_error);
				session.lifecycle_state = kAcpLifecycleError;
				AppendAcpDiagnostic(session, "write", "write_failed", method, request_id, false, 0, session.last_error, detail);
				if (error_out != nullptr)
				{
					*error_out = session.last_error;
				}
				return false;
			}

			AppendAcpDiagnostic(session, "write", "sent", method, request_id, false, 0, "", detail);
			return true;
		}

	bool SendInitialize(AcpSessionState& session, std::string* error_out = nullptr)
	{
		const int id = NextAcpRequestId(session, "initialize");
		session.initialize_request_id = id;
		return WriteAcpMessage(session, IsCodexSession(session) ? BuildCodexInitializeRequest(id) : BuildInitializeRequest(id), error_out);
	}

	std::string ContentTextFromJson(const nlohmann::json& content)
	{
		if (content.is_string())
		{
			return content.get<std::string>();
		}

		if (content.is_object())
		{
			const std::string type = content.value("type", "");
			if (type == "text" && content.contains("text") && content["text"].is_string())
			{
				return content["text"].get<std::string>();
			}

			if (content.contains("text") && content["text"].is_string())
			{
				return content["text"].get<std::string>();
			}

			if (content.contains("content"))
			{
				return ContentTextFromJson(content["content"]);
			}
		}

		if (content.is_array())
		{
			std::ostringstream out;
			bool first = true;
			for (const nlohmann::json& item : content)
			{
				const std::string piece = uam::strings::Trim(ContentTextFromJson(item));
				if (piece.empty())
				{
					continue;
				}
				if (!first)
				{
					out << '\n';
				}
				out << piece;
				first = false;
			}
			return out.str();
		}

			return "";
		}

	bool StartsWith(const std::string& value, const std::string& prefix)
	{
		return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
	}

		std::string StripLeadingLineBreaks(std::string value)
		{
			while (!value.empty() && (value.front() == '\n' || value.front() == '\r'))
			{
				value.erase(value.begin());
		}
			return value;
		}

		bool StartsWithLineBreak(const std::string& value)
		{
			return !value.empty() && (value.front() == '\n' || value.front() == '\r');
		}

		void AppendThoughtText(std::string& target, const std::string& chunk, const bool starts_new_block)
		{
			if (chunk.empty())
			{
				return;
			}

			if (!target.empty() && starts_new_block)
			{
				target += "\n\n";
			}
			target += chunk;
		}

		void RememberAssistantReplayPrefixes(AcpSessionState& session, const ChatSession& chat, const int turn_user_message_index)
		{
			session.assistant_replay_prefixes.clear();
			const int exclusive_end = std::min(turn_user_message_index, static_cast<int>(chat.messages.size()));
		for (int i = 0; i < exclusive_end; ++i)
		{
			const Message& message = chat.messages[static_cast<std::size_t>(i)];
			if (message.role != MessageRole::Assistant)
			{
				continue;
			}

			const std::string trimmed = uam::strings::Trim(message.content);
			if (trimmed.empty())
			{
				continue;
			}

			session.assistant_replay_prefixes.push_back(message.content);
			if (trimmed != message.content)
			{
				session.assistant_replay_prefixes.push_back(trimmed);
			}
		}

		std::sort(session.assistant_replay_prefixes.begin(), session.assistant_replay_prefixes.end(), [](const std::string& lhs, const std::string& rhs) {
			return lhs.size() > rhs.size();
		});
			session.assistant_replay_prefixes.erase(std::unique(session.assistant_replay_prefixes.begin(), session.assistant_replay_prefixes.end()), session.assistant_replay_prefixes.end());
		}

		void RememberLoadHistoryReplayUpdates(AcpSessionState& session, const ChatSession& chat, const int turn_user_message_index)
		{
			session.load_history_replay_updates.clear();
			const int exclusive_end = std::min(turn_user_message_index, static_cast<int>(chat.messages.size()));
			for (int i = 0; i < exclusive_end; ++i)
			{
				const Message& message = chat.messages[static_cast<std::size_t>(i)];
				if (message.role == MessageRole::User)
				{
					if (!uam::strings::Trim(message.content).empty())
					{
						AcpReplayUpdateState replay;
						replay.session_update = "user_message_chunk";
						replay.text = message.content;
						session.load_history_replay_updates.push_back(std::move(replay));
					}
					continue;
				}

				if (message.role != MessageRole::Assistant)
				{
					continue;
				}

				if (!uam::strings::Trim(message.thoughts).empty())
				{
					AcpReplayUpdateState replay;
					replay.session_update = "agent_thought_chunk";
					replay.text = message.thoughts;
					session.load_history_replay_updates.push_back(std::move(replay));
				}

				if (!uam::strings::Trim(message.content).empty())
				{
					AcpReplayUpdateState replay;
					replay.session_update = "agent_message_chunk";
					replay.text = message.content;
					session.load_history_replay_updates.push_back(std::move(replay));
				}

				for (const ToolCall& tool_call : message.tool_calls)
				{
					AcpReplayUpdateState replay;
					replay.session_update = "tool_call";
					replay.tool_call_id = tool_call.id;
					replay.title = tool_call.name;
					session.load_history_replay_updates.push_back(std::move(replay));
				}
			}
		}

		std::string StripKnownAssistantReplayPrefix(const AcpSessionState& session, const std::string& text)
		{
			for (const std::string& prefix : session.assistant_replay_prefixes)
			{
				if (text == prefix)
				{
					return "";
				}

				if (StartsWith(text, prefix))
				{
					const std::string suffix = text.substr(prefix.size());
					if (prefix.size() >= kMinAssistantReplayPrefixBytes || StartsWithLineBreak(suffix))
					{
						return StripLeadingLineBreaks(suffix);
					}
				}
			}

			return text;
	}

		std::string AssistantDeltaForIncomingText(const AcpSessionState& session, const std::string& current_assistant_text, const std::string& incoming_text)
		{
			std::string candidate = StripKnownAssistantReplayPrefix(session, incoming_text);
			if (candidate.empty())
		{
			return "";
		}

		if (!current_assistant_text.empty())
		{
			if (candidate == current_assistant_text)
			{
				return "";
			}

			if (StartsWith(candidate, current_assistant_text))
			{
				return candidate.substr(current_assistant_text.size());
			}
		}

			return candidate;
		}

		bool ReplayUpdateTypesCompatible(const std::string& expected, const std::string& incoming)
		{
			if (expected == incoming)
			{
				return true;
			}
			return (expected == "tool_call" && incoming == "tool_call_update") ||
			       (expected == "tool_call_update" && incoming == "tool_call");
		}

		bool ReplayToolUpdateMatches(const AcpReplayUpdateState& expected, const nlohmann::json& update, const std::string& update_type)
		{
			if (!ReplayUpdateTypesCompatible(expected.session_update, update_type))
			{
				return false;
			}

			const std::string incoming_id = update.value("toolCallId", "");
			if (!expected.tool_call_id.empty() && !incoming_id.empty())
			{
				return expected.tool_call_id == incoming_id;
			}

			const std::string incoming_title = update.value("title", "");
			return !expected.title.empty() && !incoming_title.empty() && expected.title == incoming_title;
		}

		bool ReplayTextUpdateMatches(const AcpReplayUpdateState& expected,
		                             const std::string& update_type,
		                             const std::string& incoming_text,
		                             std::string& live_suffix)
		{
			live_suffix.clear();
			if (!ReplayUpdateTypesCompatible(expected.session_update, update_type) || incoming_text.empty())
			{
				return false;
			}

			if (expected.text == incoming_text)
			{
				return true;
			}

			if (StartsWith(expected.text, incoming_text))
			{
				return true;
			}

			if (StartsWith(incoming_text, expected.text))
			{
				const std::string suffix = incoming_text.substr(expected.text.size());
				if (StartsWithLineBreak(suffix))
				{
					live_suffix = StripLeadingLineBreaks(suffix);
					return true;
				}
			}

			return false;
		}

		bool TryConsumeLoadHistoryReplayUpdate(AcpSessionState& session,
		                                       const nlohmann::json& update,
		                                       const std::string& update_type,
		                                       const std::string& incoming_text,
		                                       std::string& live_text)
		{
			live_text = incoming_text;
			if (session.load_history_replay_updates.empty() || !update.is_object())
			{
				return false;
			}

			for (std::size_t i = 0; i < session.load_history_replay_updates.size(); ++i)
			{
				AcpReplayUpdateState& expected = session.load_history_replay_updates[i];
				if (!ReplayUpdateTypesCompatible(expected.session_update, update_type))
				{
					continue;
				}

				if (update_type == "tool_call" || update_type == "tool_call_update")
				{
					if (!ReplayToolUpdateMatches(expected, update, update_type))
					{
						continue;
					}
					session.load_history_replay_updates.erase(session.load_history_replay_updates.begin(), session.load_history_replay_updates.begin() + static_cast<std::ptrdiff_t>(i) + 1);
					live_text.clear();
					return true;
				}

				std::string suffix;
				if (!ReplayTextUpdateMatches(expected, update_type, incoming_text, suffix))
				{
					continue;
				}

				if (StartsWith(expected.text, incoming_text) && expected.text != incoming_text)
				{
					expected.text = StripLeadingLineBreaks(expected.text.substr(incoming_text.size()));
					const std::ptrdiff_t erase_count = static_cast<std::ptrdiff_t>(i) + (expected.text.empty() ? 1 : 0);
					if (erase_count > 0)
					{
						session.load_history_replay_updates.erase(session.load_history_replay_updates.begin(), session.load_history_replay_updates.begin() + erase_count);
					}
					live_text.clear();
					return true;
				}

				session.load_history_replay_updates.erase(session.load_history_replay_updates.begin(), session.load_history_replay_updates.begin() + static_cast<std::ptrdiff_t>(i) + 1);
				live_text = suffix;
				return true;
			}

			return false;
		}

		std::string JsonRpcIdToStableString(const nlohmann::json& id)
		{
		if (id.is_null())
		{
			return "";
		}
		return id.dump();
	}

	nlohmann::json StableStringToJsonRpcId(const std::string& request_id_json)
	{
		try
		{
			return nlohmann::json::parse(request_id_json);
		}
		catch (...)
		{
			return request_id_json;
		}
	}

	int JsonRpcNumericId(const nlohmann::json& id)
	{
		if (id.is_number_integer())
		{
			return id.get<int>();
		}
		if (id.is_string())
		{
			try
			{
				return std::stoi(id.get<std::string>());
			}
			catch (...)
			{
				return 0;
			}
		}
		return 0;
	}

	void AppendRecentStderr(AcpSessionState& session, const std::string& chunk)
	{
		session.recent_stderr += chunk;
		if (session.recent_stderr.size() > kMaxRecentStderrBytes)
		{
			session.recent_stderr.erase(0, session.recent_stderr.size() - kMaxRecentStderrBytes);
		}
	}

	void ResetAcpRuntimeState(AcpSessionState& session)
	{
		session.initialized = false;
		session.session_ready = false;
		session.load_session_supported = false;
		session.processing = false;
		session.waiting_for_permission = false;
		session.waiting_for_user_input = false;
		session.next_request_id = 1;
		session.initialize_request_id = 0;
		session.session_setup_request_id = 0;
		session.prompt_request_id = 0;
		session.cancel_request_id = 0;
		session.current_assistant_message_index = -1;
		session.turn_user_message_index = -1;
		session.turn_assistant_message_index = -1;
		session.turn_serial = 0;
		session.queued_prompt.clear();
		session.ignore_session_updates_until_ready = false;
		session.codex_resume_fallback_attempted = false;
		session.gemini_resume_fallback_attempted = false;
			session.stdout_buffer.clear();
			session.stderr_buffer.clear();
			session.recent_stderr.clear();
			session.last_error.clear();
			session.has_last_exit_code = false;
			session.last_exit_code = 0;
			session.last_process_id.clear();
			session.assistant_replay_prefixes.clear();
			session.load_history_replay_updates.clear();
			session.diagnostics.clear();
			session.pending_assistant_thoughts.clear();
			session.agent_name.clear();
		session.agent_title.clear();
		session.agent_version.clear();
		session.pending_request_methods.clear();
		session.tool_calls.clear();
		session.plan_entries.clear();
		session.plan_summary.clear();
		session.codex_agent_message_text_by_item_id.clear();
		session.codex_last_agent_message_item_id.clear();
		session.codex_streamed_reasoning_keys.clear();
		session.codex_last_reasoning_section.clear();
		session.turn_events.clear();
		session.codex_turn_id.clear();
		session.pending_permission = AcpPendingPermissionState{};
		session.pending_user_input = AcpPendingUserInputState{};
	}

	AcpSessionState& EnsureAcpSessionForChat(AppState& app, const ChatSession& chat)
	{
		const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
		if (AcpSessionState* existing = FindAcpSessionForChat(app, chat.id); existing != nullptr)
		{
			existing->provider_id = provider.id;
			existing->protocol_kind = provider.structured_protocol.empty() ? kProtocolGeminiAcp : provider.structured_protocol;
			return *existing;
		}

		auto session = std::make_unique<AcpSessionState>();
		session->chat_id = chat.id;
		session->provider_id = provider.id;
		session->protocol_kind = provider.structured_protocol.empty() ? kProtocolGeminiAcp : provider.structured_protocol;
		app.acp_sessions.push_back(std::move(session));
		return *app.acp_sessions.back();
	}

	bool StartAcpProcessForChat(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string* error_out)
	{
		if (session.running)
		{
			return true;
		}

		PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
		ResetAcpRuntimeState(session);
		session.chat_id = chat.id;
		const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
		session.provider_id = provider.id;
		session.protocol_kind = provider.structured_protocol.empty() ? kProtocolGeminiAcp : provider.structured_protocol;
		const std::string codex_resume_id = IsCodexSession(session) ? ValidCodexResumeId(chat) : std::string{};
		const std::string gemini_resume_id = IsCodexSession(session) ? std::string{} : ValidGeminiResumeId(chat);
		session.session_id = IsCodexSession(session) ? codex_resume_id : gemini_resume_id;
		session.codex_thread_id = codex_resume_id;
			session.lifecycle_state = kAcpLifecycleStarting;

			std::string startup_error;
			const std::filesystem::path workspace_root = ResolveWorkspaceRootPath(app, chat);
			const std::vector<std::string> launch_argv = BuildAcpLaunchArgv(chat);
			const std::string launch_detail = BuildAcpLaunchDetail(workspace_root, chat);
			AppendAcpDiagnostic(session, "process_launch", "starting", "", "", false, 0, "", launch_detail);
			if (!PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, workspace_root, launch_argv, &startup_error))
			{
				session.lifecycle_state = kAcpLifecycleError;
				session.last_error = startup_error.empty() ? ("Failed to start " + std::string(RuntimeDisplayName(session)) + " process.") : startup_error;
				AppendAcpDiagnostic(session, "process_launch", "start_failed", "", "", false, 0, session.last_error, launch_detail);
				if (error_out != nullptr)
				{
					*error_out = session.last_error;
				}
				return false;
			}

			session.running = true;
			session.last_process_id = AcpProcessHandleLabel(session);
			AppendAcpDiagnostic(session, "process_launch", "started", "", "", false, 0, "", launch_detail);
			if (!SendInitialize(session, error_out))
			{
				PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true);
				session.running = false;
			return false;
		}

		return true;
	}

	bool SendSessionSetupIfReady(AppState& app, AcpSessionState& session, ChatSession& chat)
	{
		if (!session.running || !session.initialized || session.session_ready || session.session_setup_request_id != 0)
		{
			return false;
		}

		const std::filesystem::path workspace_root = ResolveWorkspaceRootPath(app, chat);
		const std::string cwd = workspace_root.empty() ? std::filesystem::current_path().string() : workspace_root.string();
		if (IsCodexSession(session))
		{
			const std::string raw_resume_id = Trim(chat.native_session_id);
			const std::string resume_id = ValidCodexResumeId(chat);
			if (!raw_resume_id.empty() && resume_id.empty())
			{
				AppendAcpDiagnostic(session, "session_setup", "codex_invalid_resume_id_ignored", "", "", false, 0, "Ignoring invalid Codex thread id and starting a new thread.", "nativeSessionId=" + raw_resume_id);
				chat.native_session_id.clear();
				SaveChatQuietly(app, chat);
			}
			const bool can_resume = !resume_id.empty();
			const int id = NextAcpRequestId(session, can_resume ? "thread/resume" : "thread/start");
			session.session_setup_request_id = id;
			session.ignore_session_updates_until_ready = false;
			session.lifecycle_state = kAcpLifecycleStarting;
			session.session_id = can_resume ? resume_id : "";
			session.codex_thread_id = session.session_id;
			ChatSession setup_chat = chat;
			setup_chat.native_session_id = resume_id;
			const bool written = WriteAcpMessage(session, can_resume ? BuildCodexThreadResumeRequest(id, setup_chat, cwd) : BuildCodexThreadStartRequest(id, setup_chat, cwd));
			if (!written)
			{
				session.session_setup_request_id = 0;
				FailAcpTurnOrSession(session, session.last_error.empty() ? "Failed to create Codex app-server thread." : session.last_error);
				MarkAcpChatUnseenIfBackground(app, chat);
			}
			return written;
		}

		const std::string raw_resume_id = Trim(chat.native_session_id);
		const std::string resume_id = ValidGeminiResumeId(chat);
		if (!raw_resume_id.empty() && resume_id.empty())
		{
			AppendAcpDiagnostic(session, "session_setup", "gemini_invalid_resume_id_ignored", "", "", false, 0, "Ignoring invalid Gemini session id and starting a new session.", "nativeSessionId=" + raw_resume_id);
			chat.native_session_id.clear();
			SaveChatQuietly(app, chat);
		}

		const bool can_load = !resume_id.empty() && session.load_session_supported;
		const int id = NextAcpRequestId(session, can_load ? "session/load" : "session/new");
		session.session_setup_request_id = id;
		session.ignore_session_updates_until_ready = can_load;
		session.lifecycle_state = kAcpLifecycleStarting;
		session.session_id = can_load ? resume_id : "";

		if (can_load)
		{
			const bool written = WriteAcpMessage(session, BuildLoadSessionRequest(id, resume_id, cwd));
			if (!written)
			{
				session.session_setup_request_id = 0;
				FailAcpTurnOrSession(session, session.last_error.empty() ? "Failed to load Gemini ACP session." : session.last_error);
				MarkAcpChatUnseenIfBackground(app, chat);
			}
			return written;
		}

		const bool written = WriteAcpMessage(session, BuildNewSessionRequest(id, cwd));
		if (!written)
		{
			session.session_setup_request_id = 0;
			FailAcpTurnOrSession(session, session.last_error.empty() ? "Failed to create Gemini ACP session." : session.last_error);
			MarkAcpChatUnseenIfBackground(app, chat);
		}
		return written;
	}

	bool RetryGeminiSessionNewAfterInvalidLoad(AppState& app,
	                                           AcpSessionState& session,
	                                           ChatSession& chat,
	                                           const std::string& method,
	                                           const std::string& request_id,
	                                           const bool has_code,
	                                           const int code,
	                                           const std::string& error_message,
	                                           const std::string& error_data,
	                                           const std::string& detail_text,
	                                           const std::string& formatted_error)
	{
		if (IsCodexSession(session) ||
		    method != "session/load" ||
		    session.gemini_resume_fallback_attempted ||
		    !GeminiErrorLooksLikeInvalidSessionId(error_message, error_data))
		{
			return false;
		}

		session.gemini_resume_fallback_attempted = true;
		session.session_setup_request_id = 0;
		session.session_id.clear();
		chat.native_session_id.clear();
		SaveChatQuietly(app, chat);
		AppendAcpDiagnostic(session, "response", "gemini_invalid_resume_id_retry_new", method, request_id, has_code, code, "Gemini rejected the stored session id. Starting a new session instead.", detail_text);

		const std::filesystem::path workspace_root = ResolveWorkspaceRootPath(app, chat);
		const std::string cwd = workspace_root.empty() ? std::filesystem::current_path().string() : workspace_root.string();
		const int retry_id = NextAcpRequestId(session, "session/new");
		session.session_setup_request_id = retry_id;
		session.ignore_session_updates_until_ready = false;
		session.lifecycle_state = kAcpLifecycleStarting;

		if (!WriteAcpMessage(session, BuildNewSessionRequest(retry_id, cwd)))
		{
			session.pending_request_methods.erase(retry_id);
			session.session_setup_request_id = 0;
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
			FailAcpTurnOrSession(session, session.last_error.empty() ? formatted_error : session.last_error);
			SaveChatQuietly(app, chat);
			MarkAcpChatUnseenIfBackground(app, chat);
		}

		return true;
	}

	bool SendQueuedPromptIfReady(AcpSessionState& session, const ChatSession& chat)
	{
		if (!session.running || !session.session_ready || !session.processing || session.waiting_for_permission || session.waiting_for_user_input || session.prompt_request_id != 0 || session.queued_prompt.empty() || session.session_id.empty())
		{
			return false;
		}

		const int id = NextAcpRequestId(session, IsCodexSession(session) ? "turn/start" : "session/prompt");
		const std::string prompt = session.queued_prompt;
		session.prompt_request_id = id;
		session.lifecycle_state = kAcpLifecycleProcessing;
		const nlohmann::json request = IsCodexSession(session)
			? BuildCodexTurnStartRequest(id, session.session_id, prompt, chat, session.current_model_id)
			: BuildPromptRequest(id, session.session_id, prompt);
		if (!WriteAcpMessage(session, request))
		{
			session.prompt_request_id = 0;
			CompletePromptTurn(session, kAcpLifecycleError);
			return true;
		}

		session.queued_prompt.clear();
		return true;
	}

	void SaveChatQuietly(AppState& app, const ChatSession& chat)
	{
		(void)ChatRepository::SaveChat(app.data_root, chat);
	}

	bool MessageBlocksEqual(const std::vector<MessageBlock>& lhs, const std::vector<MessageBlock>& rhs)
	{
		if (lhs.size() != rhs.size())
		{
			return false;
		}
		for (std::size_t i = 0; i < lhs.size(); ++i)
		{
			if (lhs[i].type != rhs[i].type ||
			    lhs[i].text != rhs[i].text ||
			    lhs[i].tool_call_id != rhs[i].tool_call_id ||
			    lhs[i].request_id_json != rhs[i].request_id_json)
			{
				return false;
			}
		}
		return true;
	}

	bool PersistableTurnEvent(const AcpTurnEventState& event)
	{
		if (event.type == "assistant_text" || event.type == "thought")
		{
			return !event.text.empty();
		}
		if (event.type == "tool_call")
		{
			return !event.tool_call_id.empty();
		}
		if (event.type == "permission_request")
		{
			return !event.request_id_json.empty();
		}
		if (event.type == "user_input_request")
		{
			return !event.request_id_json.empty();
		}
		return event.type == "plan";
	}

	std::vector<MessageBlock> MessageBlocksFromTurnEvents(const AcpSessionState& session)
	{
		std::vector<MessageBlock> blocks;
		blocks.reserve(session.turn_events.size());
		for (const AcpTurnEventState& event : session.turn_events)
		{
			if (!PersistableTurnEvent(event))
			{
				continue;
			}

			if ((event.type == "assistant_text" || event.type == "thought") &&
			    !blocks.empty() &&
			    blocks.back().type == event.type)
			{
				blocks.back().text += event.text;
				continue;
			}

			if ((event.type == "tool_call" && std::any_of(blocks.begin(), blocks.end(), [&](const MessageBlock& block) {
				    return block.type == "tool_call" && block.tool_call_id == event.tool_call_id;
			    })) ||
			    (event.type == "permission_request" && std::any_of(blocks.begin(), blocks.end(), [&](const MessageBlock& block) {
				    return block.type == "permission_request" && block.request_id_json == event.request_id_json;
			    })) ||
			    (event.type == "user_input_request" && std::any_of(blocks.begin(), blocks.end(), [&](const MessageBlock& block) {
				    return block.type == "user_input_request" && block.request_id_json == event.request_id_json;
			    })))
			{
				continue;
			}

			MessageBlock block;
			block.type = event.type;
			block.text = event.text;
			block.tool_call_id = event.tool_call_id;
			block.request_id_json = event.request_id_json;
			blocks.push_back(std::move(block));
		}
		return blocks;
	}

	bool SyncMessageBlocksFromTurnEvents(Message& message, const AcpSessionState& session)
	{
		if (session.turn_events.empty())
		{
			return false;
		}

		std::vector<MessageBlock> blocks = MessageBlocksFromTurnEvents(session);
		if (blocks.empty() || MessageBlocksEqual(message.blocks, blocks))
		{
			return false;
		}

		message.blocks = std::move(blocks);
		return true;
	}

	bool SyncCurrentAssistantMessageBlocksFromTurnEvents(ChatSession& chat, AcpSessionState& session)
	{
		if (session.current_assistant_message_index < 0 ||
		    session.current_assistant_message_index >= static_cast<int>(chat.messages.size()) ||
		    chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].role != MessageRole::Assistant)
		{
			return false;
		}

		if (SyncMessageBlocksFromTurnEvents(chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)], session))
		{
			chat.updated_at = TimestampNow();
			return true;
		}
		return false;
	}

	void AppendAssistantTextTurnEvent(AcpSessionState& session, const std::string& chunk)
	{
		if (!session.turn_events.empty() && session.turn_events.back().type == "assistant_text")
		{
			session.turn_events.back().text += chunk;
			return;
		}

		AcpTurnEventState event;
		event.type = "assistant_text";
		event.text = chunk;
		session.turn_events.push_back(std::move(event));
	}

		bool AppendThoughtTurnEvent(AcpSessionState& session, const std::string& chunk)
		{
			if (chunk.empty())
			{
				return false;
			}

			if (!session.turn_events.empty() && session.turn_events.back().type == "thought")
			{
				session.turn_events.back().text += chunk;
				return false;
			}

			AcpTurnEventState event;
			event.type = "thought";
			event.text = chunk;
			session.turn_events.push_back(std::move(event));
			return true;
		}

	bool HasTurnToolEvent(const AcpSessionState& session, const std::string& tool_call_id)
	{
		return std::any_of(session.turn_events.begin(), session.turn_events.end(), [&](const AcpTurnEventState& event) {
			return event.type == "tool_call" && event.tool_call_id == tool_call_id;
		});
	}

	void AppendToolTurnEventIfNeeded(AcpSessionState& session, const std::string& tool_call_id)
	{
		if (tool_call_id.empty() || HasTurnToolEvent(session, tool_call_id))
		{
			return;
		}

		AcpTurnEventState event;
		event.type = "tool_call";
		event.tool_call_id = tool_call_id;
		session.turn_events.push_back(std::move(event));
	}

	void AppendPermissionTurnEventIfNeeded(AcpSessionState& session, const std::string& request_id_json, const std::string& tool_call_id)
	{
		if (request_id_json.empty())
		{
			return;
		}

		const bool exists = std::any_of(session.turn_events.begin(), session.turn_events.end(), [&](const AcpTurnEventState& event) {
			return event.type == "permission_request" && event.request_id_json == request_id_json;
		});
		if (exists)
		{
			return;
		}

		if (!tool_call_id.empty())
		{
			AppendToolTurnEventIfNeeded(session, tool_call_id);
		}

		AcpTurnEventState event;
		event.type = "permission_request";
		event.request_id_json = request_id_json;
		event.tool_call_id = tool_call_id;
		session.turn_events.push_back(std::move(event));
	}

	void AppendUserInputTurnEventIfNeeded(AcpSessionState& session, const std::string& request_id_json, const std::string& item_id)
	{
		if (request_id_json.empty())
		{
			return;
		}

		const bool exists = std::any_of(session.turn_events.begin(), session.turn_events.end(), [&](const AcpTurnEventState& event) {
			return event.type == "user_input_request" && event.request_id_json == request_id_json;
		});
		if (exists)
		{
			return;
		}

		AcpTurnEventState event;
		event.type = "user_input_request";
		event.request_id_json = request_id_json;
		event.tool_call_id = item_id;
		session.turn_events.push_back(std::move(event));
	}

	void AppendPlanTurnEventIfNeeded(AcpSessionState& session)
	{
		const bool exists = std::any_of(session.turn_events.begin(), session.turn_events.end(), [](const AcpTurnEventState& event) {
			return event.type == "plan";
		});
		if (exists)
		{
			return;
		}

		AcpTurnEventState event;
		event.type = "plan";
		session.turn_events.push_back(std::move(event));
	}

		void CompletePromptTurn(AcpSessionState& session, const char* lifecycle_state)
		{
			session.prompt_request_id = 0;
			session.processing = false;
			session.waiting_for_permission = false;
			session.waiting_for_user_input = false;
			session.queued_prompt.clear();
			session.current_assistant_message_index = -1;
			session.codex_turn_id.clear();
			session.load_history_replay_updates.clear();
			session.pending_assistant_thoughts.clear();
			session.pending_permission = AcpPendingPermissionState{};
			session.pending_user_input = AcpPendingUserInputState{};
			session.lifecycle_state = lifecycle_state;
		}

	void FailAcpTurnOrSession(AcpSessionState& session, const std::string& message)
	{
		session.last_error = message;
		if (session.processing || session.waiting_for_permission || session.prompt_request_id != 0 || !session.queued_prompt.empty())
		{
			CompletePromptTurn(session, kAcpLifecycleError);
			return;
		}

		session.lifecycle_state = kAcpLifecycleError;
	}

		void MarkAcpChatUnseenIfBackground(AppState& app, const ChatSession& chat)
		{
			const ChatSession* selected = ChatDomainService().SelectedChat(app);
			if (selected != nullptr && selected->id == chat.id)
		{
			return;
		}

			app.chats_with_unseen_updates.insert(chat.id);
		}

		Message& EnsureAssistantMessage(ChatSession& chat, AcpSessionState& session)
		{
			if (session.current_assistant_message_index >= 0 &&
			    session.current_assistant_message_index < static_cast<int>(chat.messages.size()) &&
			    chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].role == MessageRole::Assistant)
			{
				return chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)];
			}

			Message message;
			message.role = MessageRole::Assistant;
			message.provider = MessageProviderId(session);
			message.created_at = TimestampNow();
			chat.messages.push_back(std::move(message));
			session.current_assistant_message_index = static_cast<int>(chat.messages.size()) - 1;
			session.turn_assistant_message_index = session.current_assistant_message_index;
			return chat.messages.back();
		}

		bool AppendThoughtChunk(ChatSession& chat, AcpSessionState& session, const std::string& chunk)
		{
			if (chunk.empty())
			{
				return false;
			}

			const bool starts_new_block = AppendThoughtTurnEvent(session, chunk);
			if (session.current_assistant_message_index >= 0 &&
			    session.current_assistant_message_index < static_cast<int>(chat.messages.size()) &&
			    chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].role == MessageRole::Assistant)
			{
				Message& message = chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)];
				AppendThoughtText(message.thoughts, chunk, starts_new_block);
				(void)SyncMessageBlocksFromTurnEvents(message, session);
				chat.updated_at = TimestampNow();
				return true;
			}

			AppendThoughtText(session.pending_assistant_thoughts, chunk, starts_new_block);
			return false;
		}

			void AppendAssistantChunk(ChatSession& chat, AcpSessionState& session, const std::string& chunk)
			{
				if (chunk.empty())
				{
					return;
			}

		std::string current_assistant_text;
		if (session.current_assistant_message_index >= 0 &&
		    session.current_assistant_message_index < static_cast<int>(chat.messages.size()) &&
		    chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].role == MessageRole::Assistant)
		{
			current_assistant_text = chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].content;
		}

		const std::string delta = AssistantDeltaForIncomingText(session, current_assistant_text, chunk);
		if (delta.empty())
		{
			return;
		}

		if (session.current_assistant_message_index < 0 ||
		    session.current_assistant_message_index >= static_cast<int>(chat.messages.size()) ||
		    chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].role != MessageRole::Assistant)
		{
			(void)EnsureAssistantMessage(chat, session);
		}

			Message& message = chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)];
			if (!session.pending_assistant_thoughts.empty())
			{
				AppendThoughtText(message.thoughts, session.pending_assistant_thoughts, !message.thoughts.empty());
				session.pending_assistant_thoughts.clear();
			}
				message.content += delta;
				(void)SyncAcpToolCallsToAssistantMessage(chat, session, false);
				chat.updated_at = TimestampNow();
				if (session.turn_assistant_message_index < 0)
				{
				session.turn_assistant_message_index = session.current_assistant_message_index;
		}
			AppendAssistantTextTurnEvent(session, delta);
			(void)SyncCurrentAssistantMessageBlocksFromTurnEvents(chat, session);
		}

		ToolCall PersistedToolCallFromAcpToolCall(const AcpToolCallState& tool_call)
		{
			ToolCall persisted;
			persisted.id = tool_call.id;
			persisted.name = !tool_call.title.empty() ? tool_call.title : (!tool_call.kind.empty() ? tool_call.kind : tool_call.id);
			persisted.status = tool_call.status;
			persisted.result_text = tool_call.content;
			return persisted;
		}

		bool UpsertPersistedToolCall(std::vector<ToolCall>& tool_calls, const AcpToolCallState& tool_call)
		{
			if (tool_call.id.empty())
			{
				return false;
			}

			const ToolCall persisted = PersistedToolCallFromAcpToolCall(tool_call);
			for (ToolCall& existing : tool_calls)
			{
				if (existing.id != persisted.id)
				{
					continue;
				}

				if (existing.name == persisted.name &&
				    existing.args_json == persisted.args_json &&
				    existing.result_text == persisted.result_text &&
				    existing.status == persisted.status)
				{
					return false;
				}

				existing = persisted;
				return true;
			}

			tool_calls.push_back(persisted);
			return true;
		}

		bool SyncAcpToolCallsToAssistantMessage(ChatSession& chat, AcpSessionState& session, const bool create_if_missing)
		{
			if (session.tool_calls.empty())
			{
				return false;
			}

			if (session.current_assistant_message_index < 0 ||
			    session.current_assistant_message_index >= static_cast<int>(chat.messages.size()) ||
			    chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].role != MessageRole::Assistant)
			{
				if (!create_if_missing)
				{
					return false;
				}

				(void)EnsureAssistantMessage(chat, session);
			}

			Message& message = chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)];
			bool changed = false;
			for (const AcpToolCallState& tool_call : session.tool_calls)
			{
				changed |= UpsertPersistedToolCall(message.tool_calls, tool_call);
			}
			changed |= SyncMessageBlocksFromTurnEvents(message, session);
			if (changed)
			{
				chat.updated_at = TimestampNow();
			}
			return changed;
		}

		MessagePlanEntry PersistedPlanEntryFromAcpPlanEntry(const AcpPlanEntryState& entry)
		{
			MessagePlanEntry persisted;
			persisted.content = entry.content;
			persisted.priority = entry.priority;
			persisted.status = entry.status;
			return persisted;
		}

		std::vector<MessagePlanEntry> PersistedPlanEntriesFromAcpPlanEntries(const std::vector<AcpPlanEntryState>& entries)
		{
			std::vector<MessagePlanEntry> persisted;
			persisted.reserve(entries.size());
			for (const AcpPlanEntryState& entry : entries)
			{
				persisted.push_back(PersistedPlanEntryFromAcpPlanEntry(entry));
			}
			return persisted;
		}

		bool MessagePlanEntriesEqual(const std::vector<MessagePlanEntry>& lhs, const std::vector<MessagePlanEntry>& rhs)
		{
			if (lhs.size() != rhs.size())
			{
				return false;
			}
			for (std::size_t i = 0; i < lhs.size(); ++i)
			{
				if (lhs[i].content != rhs[i].content ||
				    lhs[i].priority != rhs[i].priority ||
				    lhs[i].status != rhs[i].status)
				{
					return false;
				}
			}
			return true;
		}

		bool SyncAcpPlanToAssistantMessage(ChatSession& chat, AcpSessionState& session, const bool create_if_missing)
		{
			if (uam::strings::Trim(session.plan_summary).empty() && session.plan_entries.empty())
			{
				return false;
			}

			if (session.current_assistant_message_index < 0 ||
			    session.current_assistant_message_index >= static_cast<int>(chat.messages.size()) ||
			    chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].role != MessageRole::Assistant)
			{
				if (!create_if_missing)
				{
					return false;
				}
				(void)EnsureAssistantMessage(chat, session);
			}

			Message& message = chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)];
			const std::vector<MessagePlanEntry> persisted_entries = PersistedPlanEntriesFromAcpPlanEntries(session.plan_entries);
			bool changed = false;
			if (message.plan_summary != session.plan_summary ||
			    !MessagePlanEntriesEqual(message.plan_entries, persisted_entries))
			{
				message.plan_summary = session.plan_summary;
				message.plan_entries = persisted_entries;
				changed = true;
			}
			changed |= SyncMessageBlocksFromTurnEvents(message, session);
			if (changed)
			{
				chat.updated_at = TimestampNow();
			}
			return changed;
		}

		AcpToolCallState& UpsertToolCall(AcpSessionState& session, const std::string& id)
		{
		for (AcpToolCallState& tool_call : session.tool_calls)
		{
			if (tool_call.id == id)
			{
				return tool_call;
			}
		}

		AcpToolCallState tool_call;
		tool_call.id = id;
		session.tool_calls.push_back(std::move(tool_call));
		return session.tool_calls.back();
	}

		void HandleSessionUpdate(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& params)
		{
			const nlohmann::json update = params.value("update", nlohmann::json::object());
			if (!update.is_object())
			{
				return;
			}

			std::string update_type = update.value("sessionUpdate", "");
			if (update_type.empty() && update.value("thought", false))
			{
				update_type = "agent_thought_chunk";
			}
			if (update_type.empty() && update.contains("toolCallId"))
			{
				update_type = "tool_call_update";
				}
				const std::string content_text = ContentTextFromJson(update.value("content", nlohmann::json::object()));
				std::string live_text;
				if (update_type == "current_mode_update")
				{
					const std::string current_mode_id = update.value("currentModeId", "");
					if (!current_mode_id.empty())
					{
						session.current_mode_id = current_mode_id;
					}
					return;
				}
				if (session.ignore_session_updates_until_ready)
				{
					(void)TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text);
				return;
			}

			const bool active_turn = session.processing || session.waiting_for_permission || session.waiting_for_user_input || session.prompt_request_id != 0;
			if (!active_turn)
			{
				return;
			}

			if (update_type == "user_message_chunk")
			{
				(void)TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text);
				return;
			}

			if (update_type == "agent_thought_chunk" || update.value("thought", false))
			{
				live_text = content_text;
				if (TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text) && live_text.empty())
				{
					return;
				}

				if (AppendThoughtChunk(chat, session, live_text))
				{
					SaveChatQuietly(app, chat);
				}
				return;
			}

			if (update_type == "agent_message_chunk")
			{
				live_text = content_text;
				if (TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text) && live_text.empty())
				{
					return;
				}

				AppendAssistantChunk(chat, session, live_text);
				SaveChatQuietly(app, chat);
				return;
			}

			if (update_type == "tool_call" || update.contains("toolCallId"))
			{
				if (TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text))
				{
					return;
				}

				const std::string id = update.value("toolCallId", "");
				if (!id.empty())
			{
				AcpToolCallState& tool_call = UpsertToolCall(session, id);
				tool_call.title = update.value("title", tool_call.title);
				tool_call.kind = update.value("kind", tool_call.kind.empty() ? "other" : tool_call.kind);
				tool_call.status = update.value("status", tool_call.status.empty() ? "pending" : tool_call.status);
					if (update.contains("content"))
					{
						tool_call.content = ContentTextFromJson(update["content"]);
					}
					AppendToolTurnEventIfNeeded(session, id);
					if (SyncAcpToolCallsToAssistantMessage(chat, session, false))
					{
						SaveChatQuietly(app, chat);
					}
				}
				return;
			}

		if (update_type == "plan" && update.contains("entries") && update["entries"].is_array())
		{
			session.plan_summary = JsonDiagnosticStringValueOr(update, "summary", JsonDiagnosticStringValue(update, "explanation"));
			session.plan_entries.clear();
			for (const nlohmann::json& entry : update["entries"])
			{
				if (!entry.is_object())
				{
					continue;
				}
				AcpPlanEntryState plan_entry;
				plan_entry.content = entry.value("content", "");
				plan_entry.priority = entry.value("priority", "");
				plan_entry.status = entry.value("status", "");
				session.plan_entries.push_back(std::move(plan_entry));
			}
			AppendPlanTurnEventIfNeeded(session);
			if (SyncAcpPlanToAssistantMessage(chat, session, true))
			{
				SaveChatQuietly(app, chat);
			}
		}
	}

	void SendJsonRpcError(AcpSessionState& session, const nlohmann::json& id, const int code, const std::string& message)
	{
		nlohmann::json response;
		response["jsonrpc"] = "2.0";
		response["id"] = id;
		response["error"] = {
			{"code", code},
			{"message", message},
		};
		(void)WriteAcpMessage(session, response);
	}

	bool SendPermissionResponse(AcpSessionState& session,
	                            const std::string& request_id_json,
	                            const std::string& option_id,
	                            const bool cancelled,
	                            std::string* error_out = nullptr)
	{
		nlohmann::json response;
		response["jsonrpc"] = "2.0";
		response["id"] = StableStringToJsonRpcId(request_id_json);
		if (IsCodexSession(session))
		{
			const std::string kind = session.pending_permission.provider_request_kind;
			const bool deny = cancelled || option_id == "decline" || option_id == "cancelled";
			if (kind == "codex-command")
			{
				response["result"] = {{"decision", cancelled ? "cancel" : (deny ? "decline" : (option_id.empty() ? "accept" : option_id))}};
			}
			else if (kind == "codex-file")
			{
				response["result"] = {{"decision", cancelled ? "cancel" : (deny ? "decline" : (option_id.empty() ? "accept" : option_id))}};
			}
			else if (kind == "codex-permissions")
			{
				nlohmann::json permissions = nlohmann::json::object();
				if (!deny && !session.pending_permission.codex_approval_payload_json.empty())
				{
					try
					{
						const nlohmann::json payload = nlohmann::json::parse(session.pending_permission.codex_approval_payload_json);
						if (payload.contains("permissions"))
						{
							permissions = payload["permissions"];
						}
					}
					catch (...)
					{
						permissions = nlohmann::json::object();
					}
				}
				response["result"] = {
					{"permissions", permissions},
					{"scope", "session"},
				};
			}
			else
			{
				response["result"] = nlohmann::json::object();
			}
			return WriteAcpMessage(session, response, error_out);
		}

		if (cancelled)
		{
			response["result"] = {{"outcome", {{"outcome", "cancelled"}}}};
		}
		else
		{
			response["result"] = {{"outcome", {{"outcome", "selected"}, {"optionId", option_id}}}};
		}

		return WriteAcpMessage(session, response, error_out);
	}

	nlohmann::json BuildCodexUserInputResponse(const std::string& request_id_json, const std::map<std::string, std::vector<std::string>>& answers)
	{
		nlohmann::json answer_map = nlohmann::json::object();
		for (const auto& [question_id, values] : answers)
		{
			if (question_id.empty())
			{
				continue;
			}

			nlohmann::json answer_values = nlohmann::json::array();
			for (const std::string& value : values)
			{
				answer_values.push_back(value);
			}
			answer_map[question_id] = {{"answers", std::move(answer_values)}};
		}

		return {
			{"jsonrpc", "2.0"},
			{"id", StableStringToJsonRpcId(request_id_json)},
			{"result", {
				{"answers", std::move(answer_map)},
			}},
		};
	}

	bool SendCodexUserInputResponse(AcpSessionState& session,
	                                const std::string& request_id_json,
	                                const std::map<std::string, std::vector<std::string>>& answers,
	                                std::string* error_out = nullptr)
	{
		return WriteAcpMessage(session, BuildCodexUserInputResponse(request_id_json, answers), error_out);
	}

	void HandlePermissionRequest(AcpSessionState& session, const nlohmann::json& message)
	{
		const nlohmann::json params = message.value("params", nlohmann::json::object());
		const nlohmann::json tool_call = params.value("toolCall", nlohmann::json::object());

		AcpPendingPermissionState pending;
		pending.request_id_json = JsonRpcIdToStableString(message.value("id", nlohmann::json(nullptr)));
		pending.tool_call_id = tool_call.value("toolCallId", "");
		pending.title = tool_call.value("title", "Permission required");
		pending.kind = tool_call.value("kind", "other");
		pending.status = tool_call.value("status", "pending");
		if (tool_call.contains("content"))
		{
			pending.content = ContentTextFromJson(tool_call["content"]);
		}

		const nlohmann::json options = params.value("options", nlohmann::json::array());
		if (options.is_array())
		{
			for (const nlohmann::json& option : options)
			{
				if (!option.is_object())
				{
					continue;
				}
				AcpPermissionOptionState parsed;
				parsed.id = option.value("optionId", "");
				parsed.name = option.value("name", parsed.id);
				parsed.kind = option.value("kind", "");
				if (!parsed.id.empty())
				{
					pending.options.push_back(std::move(parsed));
				}
			}
		}

		if (!pending.tool_call_id.empty())
		{
			AcpToolCallState& tracked_tool_call = UpsertToolCall(session, pending.tool_call_id);
			tracked_tool_call.title = pending.title;
			tracked_tool_call.kind = pending.kind;
			tracked_tool_call.status = pending.status;
			tracked_tool_call.content = pending.content;
		}
		AppendPermissionTurnEventIfNeeded(session, pending.request_id_json, pending.tool_call_id);

		session.pending_permission = std::move(pending);
		session.waiting_for_permission = true;
		session.processing = true;
		session.lifecycle_state = kAcpLifecycleWaitingPermission;
	}

	std::string CodexItemTitle(const nlohmann::json& item)
	{
		const std::string type = JsonDiagnosticStringValueOr(item, "type", "tool");
		if (type == "commandExecution")
		{
			const std::string command = JsonDiagnosticStringValue(item, "command");
			return command.empty() ? "Command" : command;
		}
		if (type == "fileChange")
		{
			return "File changes";
		}
		if (type == "mcpToolCall")
		{
			return JsonDiagnosticStringValueOr(item, "tool", "MCP tool");
		}
		if (type == "dynamicToolCall")
		{
			return JsonDiagnosticStringValueOr(item, "tool", "Tool");
		}
		return type;
	}

	std::string CodexItemContent(const nlohmann::json& item)
	{
		const std::string type = JsonDiagnosticStringValue(item, "type");
		if (type == "commandExecution")
		{
			return JsonDiagnosticStringValue(item, "aggregatedOutput");
		}
		if (type == "agentMessage")
		{
			return JsonDiagnosticStringValue(item, "text");
		}
		if (type == "plan")
		{
			return JsonDiagnosticStringValue(item, "text");
		}
		if (type == "fileChange" || type == "mcpToolCall" || type == "dynamicToolCall")
		{
			return item.dump();
		}
		return "";
	}

	std::string CodexReasoningPartText(const nlohmann::json& value)
	{
		if (value.is_string())
		{
			return value.get<std::string>();
		}
		if (value.is_object())
		{
			return JsonDiagnosticStringValue(value, "text");
		}
		return value.is_null() ? "" : value.dump();
	}

	std::string CodexReasoningKey(const std::string& item_id, const std::string& section, const int index)
	{
		if (item_id.empty())
		{
			return "";
		}
		if (index < 0)
		{
			return item_id + "\n" + section;
		}
		return item_id + "\n" + section + "\n" + std::to_string(index);
	}

	int JsonIntValueOr(const nlohmann::json& object, const char* key, const int fallback)
	{
		if (!object.is_object())
		{
			return fallback;
		}
		const auto it = object.find(key);
		if (it == object.end() || !it->is_number_integer())
		{
			return fallback;
		}
		return it->get<int>();
	}

	bool CodexReasoningWasStreamed(const AcpSessionState& session, const std::string& item_id, const std::string& section, const int index)
	{
		if (item_id.empty())
		{
			return false;
		}
		const std::string wildcard_key = CodexReasoningKey(item_id, section, -1);
		const std::string indexed_key = CodexReasoningKey(item_id, section, index);
		return session.codex_streamed_reasoning_keys.find(wildcard_key) != session.codex_streamed_reasoning_keys.end() ||
		       session.codex_streamed_reasoning_keys.find(indexed_key) != session.codex_streamed_reasoning_keys.end();
	}

	std::string CodexCompletedReasoningSectionText(const AcpSessionState& session,
	                                              const nlohmann::json& item,
	                                              const char* key,
	                                              const std::string& item_id,
	                                              const std::string& section)
	{
		if (!item.is_object())
		{
			return "";
		}
		const auto it = item.find(key);
		if (it == item.end() || it->is_null())
		{
			return "";
		}
		if (!it->is_array())
		{
			if (CodexReasoningWasStreamed(session, item_id, section, 0))
			{
				return "";
			}
			return JsonDiagnosticStringValue(item, key);
		}

		std::ostringstream out;
		bool first = true;
		for (std::size_t i = 0; i < it->size(); ++i)
		{
			if (CodexReasoningWasStreamed(session, item_id, section, static_cast<int>(i)))
			{
				continue;
			}
			const std::string text = CodexReasoningPartText((*it)[i]);
			if (text.empty())
			{
				continue;
			}
			if (!first)
			{
				out << '\n';
			}
			out << text;
			first = false;
		}
		return out.str();
	}

	bool AppendCodexReasoningThought(ChatSession& chat,
	                                 AcpSessionState& session,
	                                 const std::string& item_id,
	                                 const std::string& section,
	                                 const std::string& text,
	                                 const int index,
	                                 const bool streamed)
	{
		if (text.empty())
		{
			return false;
		}

		if (streamed && !item_id.empty())
		{
			session.codex_streamed_reasoning_keys.insert(CodexReasoningKey(item_id, section, index));
		}

		std::string chunk = text;
		if (session.codex_last_reasoning_section != section)
		{
			chunk = (session.codex_last_reasoning_section.empty() ? "### " : "\n\n### ") + section + "\n" + text;
			session.codex_last_reasoning_section = section;
		}

		(void)EnsureAssistantMessage(chat, session);
		return AppendThoughtChunk(chat, session, chunk);
	}

	bool HandleCodexCompletedReasoningItem(ChatSession& chat, AcpSessionState& session, const nlohmann::json& item)
	{
		const std::string item_id = JsonDiagnosticStringValue(item, "id");
		bool changed = false;
		const std::string raw_content = CodexCompletedReasoningSectionText(session, item, "content", item_id, "Reasoning");
		if (!raw_content.empty())
		{
			changed |= AppendCodexReasoningThought(chat, session, item_id, "Reasoning", raw_content, -1, false);
		}

		const std::string summary = CodexCompletedReasoningSectionText(session, item, "summary", item_id, "Summary");
		if (!summary.empty())
		{
			changed |= AppendCodexReasoningThought(chat, session, item_id, "Summary", summary, -1, false);
		}
		return changed;
	}

	std::string CodexStreamedAgentMessageDelta(AcpSessionState& session, const std::string& item_id, const std::string& delta)
	{
		if (delta.empty())
		{
			return "";
		}
		if (!item_id.empty())
		{
			session.codex_agent_message_text_by_item_id[item_id] += delta;
		}
		return delta;
	}

	std::string CodexCompletedAgentMessageDelta(AcpSessionState& session, const std::string& item_id, const std::string& text)
	{
		if (text.empty())
		{
			return "";
		}
		if (item_id.empty())
		{
			return text;
		}

		std::string& streamed_text = session.codex_agent_message_text_by_item_id[item_id];
		if (streamed_text.empty())
		{
			streamed_text = text;
			return text;
		}
		if (text == streamed_text || StartsWith(streamed_text, text))
		{
			return "";
		}
		if (StartsWith(text, streamed_text))
		{
			const std::string suffix = text.substr(streamed_text.size());
			streamed_text = text;
			return suffix;
		}

		streamed_text = text;
		return text;
	}

	bool CurrentAssistantMessageHasContent(const ChatSession& chat, const AcpSessionState& session)
	{
		return session.current_assistant_message_index >= 0 &&
		       session.current_assistant_message_index < static_cast<int>(chat.messages.size()) &&
		       chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].role == MessageRole::Assistant &&
		       !chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].content.empty();
	}

	void AppendCodexAgentMessageText(ChatSession& chat, AcpSessionState& session, const std::string& item_id, const std::string& delta)
	{
		if (delta.empty())
		{
			return;
		}

		std::string chunk = delta;
		if (!item_id.empty() &&
		    !session.codex_last_agent_message_item_id.empty() &&
		    session.codex_last_agent_message_item_id != item_id &&
		    CurrentAssistantMessageHasContent(chat, session) &&
		    !StartsWithLineBreak(delta))
		{
			chunk = "\n\n" + delta;
		}
		if (!item_id.empty())
		{
			session.codex_last_agent_message_item_id = item_id;
		}
		AppendAssistantChunk(chat, session, chunk);
	}

	void RemoveCodexPlanDeltaEntryForItem(AcpSessionState& session, const std::string& item_id)
	{
		if (item_id.empty())
		{
			return;
		}
		session.plan_entries.erase(std::remove_if(session.plan_entries.begin(), session.plan_entries.end(), [&](const AcpPlanEntryState& entry) {
			return entry.priority == item_id;
		}), session.plan_entries.end());
	}

	void HandleCodexToolItem(AcpSessionState& session, ChatSession& chat, const nlohmann::json& item)
	{
		const std::string item_id = JsonDiagnosticStringValue(item, "id");
		const std::string type = JsonDiagnosticStringValue(item, "type");
		if (item_id.empty())
		{
			return;
		}
		if (type == "agentMessage")
		{
			const std::string content = CodexItemContent(item);
			if (!content.empty())
			{
				AppendCodexAgentMessageText(chat, session, item_id, CodexCompletedAgentMessageDelta(session, item_id, content));
			}
			return;
		}
		if (type == "reasoning")
		{
			(void)HandleCodexCompletedReasoningItem(chat, session, item);
			return;
		}
		if (type == "plan")
		{
			session.plan_summary = CodexItemContent(item);
			RemoveCodexPlanDeltaEntryForItem(session, item_id);
			AppendPlanTurnEventIfNeeded(session);
			(void)SyncAcpPlanToAssistantMessage(chat, session, true);
			return;
		}
		if (type != "commandExecution" && type != "fileChange" && type != "mcpToolCall" && type != "dynamicToolCall")
		{
			return;
		}

		AcpToolCallState& tool_call = UpsertToolCall(session, item_id);
		tool_call.title = CodexItemTitle(item);
		tool_call.kind = type;
		tool_call.status = JsonDiagnosticStringValueOr(item, "status", tool_call.status.empty() ? "pending" : tool_call.status);
		const std::string content = CodexItemContent(item);
		if (!content.empty())
		{
			tool_call.content = content;
		}
		AppendToolTurnEventIfNeeded(session, item_id);
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, false);
	}

	void HandleCodexPendingPermission(AcpSessionState& session, const nlohmann::json& message, const std::string& kind)
	{
		const nlohmann::json params = JsonObjectValue(message, "params");
		AcpPendingPermissionState pending;
		pending.request_id_json = JsonRpcIdToStableString(message.value("id", nlohmann::json(nullptr)));
		pending.provider_request_method = JsonDiagnosticStringValue(message, "method");
		pending.provider_request_kind = kind;
		pending.codex_approval_payload_json = params.dump();
		pending.tool_call_id = JsonDiagnosticStringValueOr(params, "itemId", pending.request_id_json);
		pending.status = "pending";

		if (kind == "codex-command")
		{
			pending.title = "Command approval";
			pending.kind = "commandExecution";
			pending.content = JsonDiagnosticStringValueOr(params, "command", JsonDiagnosticStringValue(params, "reason"));
			const nlohmann::json decisions = JsonArrayValue(params, "availableDecisions");
			if (decisions.is_array())
			{
				for (const nlohmann::json& decision : decisions)
				{
					if (!decision.is_string())
					{
						continue;
					}
					const std::string id = decision.get<std::string>();
					pending.options.push_back(AcpPermissionOptionState{id, id == "acceptForSession" ? "Allow for session" : (id == "accept" ? "Allow" : (id == "decline" ? "Deny" : id)), "decision"});
				}
			}
		}
		else if (kind == "codex-file")
		{
			pending.title = "File change approval";
			pending.kind = "fileChange";
			pending.content = JsonDiagnosticStringValueOr(params, "reason", JsonDiagnosticStringValue(params, "grantRoot"));
		}
		else
		{
			pending.title = "Permission approval";
			pending.kind = "permissions";
			pending.content = JsonDiagnosticStringValue(params, "reason");
		}

		if (pending.options.empty())
		{
			pending.options.push_back(AcpPermissionOptionState{"accept", "Allow", "decision"});
			pending.options.push_back(AcpPermissionOptionState{"decline", "Deny", "decision"});
		}
		pending.options.push_back(AcpPermissionOptionState{"cancelled", "Cancel", "cancel"});

		if (!pending.tool_call_id.empty())
		{
			AcpToolCallState& tracked_tool_call = UpsertToolCall(session, pending.tool_call_id);
			tracked_tool_call.title = pending.title;
			tracked_tool_call.kind = pending.kind;
			tracked_tool_call.status = pending.status;
			tracked_tool_call.content = pending.content;
		}
		AppendPermissionTurnEventIfNeeded(session, pending.request_id_json, pending.tool_call_id);
		session.pending_permission = std::move(pending);
		session.waiting_for_permission = true;
		session.processing = true;
		session.lifecycle_state = kAcpLifecycleWaitingPermission;
	}

	std::string CodexUserInputContent(const AcpPendingUserInputState& pending)
	{
		std::ostringstream out;
		bool first = true;
		for (const AcpUserInputQuestionState& question : pending.questions)
		{
			if (!first)
			{
				out << "\n\n";
			}
			if (!question.header.empty())
			{
				out << question.header << "\n";
			}
			out << question.question;
			first = false;
		}
		return out.str();
	}

	void HandleCodexUserInputRequest(AcpSessionState& session, const nlohmann::json& message)
	{
		const nlohmann::json params = JsonObjectValue(message, "params");
		AcpPendingUserInputState pending;
		pending.request_id_json = JsonRpcIdToStableString(message.value("id", nlohmann::json(nullptr)));
		pending.item_id = JsonDiagnosticStringValue(params, "itemId");
		pending.status = "pending";

		const nlohmann::json questions = JsonArrayValue(params, "questions");
		if (questions.is_array())
		{
			for (const nlohmann::json& question_json : questions)
			{
				if (!question_json.is_object())
				{
					continue;
				}

				AcpUserInputQuestionState question;
				question.id = JsonDiagnosticStringValue(question_json, "id");
				question.header = JsonDiagnosticStringValue(question_json, "header");
				question.question = JsonDiagnosticStringValue(question_json, "question");
				question.is_other = JsonBooleanValueOr(question_json, "isOther", false);
				question.is_secret = JsonBooleanValueOr(question_json, "isSecret", false);

				const nlohmann::json options = JsonArrayValue(question_json, "options");
				if (options.is_array())
				{
					for (const nlohmann::json& option_json : options)
					{
						if (!option_json.is_object())
						{
							continue;
						}

						AcpUserInputOptionState option;
						option.label = JsonDiagnosticStringValue(option_json, "label");
						option.description = JsonDiagnosticStringValue(option_json, "description");
						if (!option.label.empty() || !option.description.empty())
						{
							question.options.push_back(std::move(option));
						}
					}
				}

				if (!question.id.empty())
				{
					pending.questions.push_back(std::move(question));
				}
			}
		}

		if (!pending.item_id.empty())
		{
			AcpToolCallState& tracked_tool_call = UpsertToolCall(session, pending.item_id);
			tracked_tool_call.title = "User input";
			tracked_tool_call.kind = "userInput";
			tracked_tool_call.status = pending.status;
			tracked_tool_call.content = CodexUserInputContent(pending);
		}

		AppendUserInputTurnEventIfNeeded(session, pending.request_id_json, pending.item_id);
		session.pending_user_input = std::move(pending);
		session.waiting_for_user_input = true;
		session.processing = true;
		session.lifecycle_state = kAcpLifecycleWaitingUserInput;
	}

	void HandleCodexMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
	{
		const std::string method = JsonDiagnosticStringValue(message, "method");
		const nlohmann::json params = JsonObjectValue(message, "params");

		if (method == "turn/started")
		{
			const nlohmann::json turn = JsonObjectValue(params, "turn");
			if (turn.is_object())
			{
				session.codex_turn_id = JsonDiagnosticStringValueOr(turn, "id", session.codex_turn_id);
			}
			session.lifecycle_state = kAcpLifecycleProcessing;
			return;
		}
		if (method == "turn/completed")
		{
			const nlohmann::json turn = JsonObjectValue(params, "turn");
			const nlohmann::json error = JsonObjectValue(turn, "error");
			const std::string turn_status = JsonDiagnosticStringValue(turn, "status");
			if ((error.is_object() && !error.empty()) || turn_status == "failed")
			{
				nlohmann::json error_params = {
					{"willRetry", false},
					{"threadId", JsonDiagnosticStringValue(params, "threadId")},
					{"turnId", JsonDiagnosticStringValue(params, "turnId")},
				};
				if (JsonDiagnosticStringValue(error_params, "turnId").empty() && turn.is_object())
				{
					error_params["turnId"] = JsonDiagnosticStringValue(turn, "id");
				}
				const nlohmann::json normalized_error = error.is_object() ? error : nlohmann::json::object();
				const std::string error_message = CodexTurnErrorMessage(normalized_error);
				const std::string detail = CodexTurnErrorDetails(session, error_params, normalized_error);
				AppendAcpDiagnostic(session, "notification", "codex_turn_completed_error", method, "", false, 0, error_message, detail);
				(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
				FailAcpTurnOrSession(session, FormatAcpFailureMessage(session, method, "", false, 0, error_message, !detail.empty()));
				SaveChatQuietly(app, chat);
				MarkAcpChatUnseenIfBackground(app, chat);
				return;
			}
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
			CompletePromptTurn(session, kAcpLifecycleReady);
			SaveChatQuietly(app, chat);
			MarkAcpChatUnseenIfBackground(app, chat);
			return;
		}
		if (method == "item/agentMessage/delta")
		{
			const std::string item_id = JsonDiagnosticStringValue(params, "itemId");
			AppendCodexAgentMessageText(chat,
			                            session,
			                            item_id,
			                            CodexStreamedAgentMessageDelta(session, item_id, JsonDiagnosticStringValue(params, "delta")));
			SaveChatQuietly(app, chat);
			return;
		}
		if (method == "item/reasoning/textDelta")
		{
			if (AppendCodexReasoningThought(chat,
			                               session,
			                               JsonDiagnosticStringValue(params, "itemId"),
			                               "Reasoning",
			                               JsonDiagnosticStringValue(params, "delta"),
			                               JsonIntValueOr(params, "contentIndex", -1),
			                               true))
			{
				SaveChatQuietly(app, chat);
			}
			return;
		}
		if (method == "item/reasoning/summaryTextDelta")
		{
			if (AppendCodexReasoningThought(chat,
			                               session,
			                               JsonDiagnosticStringValue(params, "itemId"),
			                               "Summary",
			                               JsonDiagnosticStringValue(params, "delta"),
			                               JsonIntValueOr(params, "summaryIndex", -1),
			                               true))
			{
				SaveChatQuietly(app, chat);
			}
			return;
		}
		if (method == "item/reasoning/summaryPartAdded")
		{
			return;
		}
		if (method == "item/plan/delta")
		{
			const std::string item_id = JsonDiagnosticStringValue(params, "itemId");
			if (item_id.empty())
			{
				return;
			}
			AcpPlanEntryState* entry = nullptr;
			for (AcpPlanEntryState& existing : session.plan_entries)
			{
				if (existing.priority == item_id)
				{
					entry = &existing;
					break;
				}
			}
			if (entry == nullptr)
			{
				AcpPlanEntryState created;
				created.priority = item_id;
				created.status = "pending";
				session.plan_entries.push_back(std::move(created));
				entry = &session.plan_entries.back();
			}
			entry->content += JsonDiagnosticStringValue(params, "delta");
			AppendPlanTurnEventIfNeeded(session);
			(void)SyncAcpPlanToAssistantMessage(chat, session, true);
			SaveChatQuietly(app, chat);
			return;
		}
		if (method == "turn/plan/updated")
		{
			session.plan_summary = JsonDiagnosticStringValue(params, "explanation");
			const nlohmann::json plan = JsonArrayValue(params, "plan");
			if (plan.is_array())
			{
				session.plan_entries.clear();
				for (const nlohmann::json& step : plan)
				{
					if (!step.is_object())
					{
						continue;
					}
					AcpPlanEntryState entry;
					entry.content = JsonDiagnosticStringValue(step, "step");
					entry.status = JsonDiagnosticStringValue(step, "status");
					session.plan_entries.push_back(std::move(entry));
				}
			}
			AppendPlanTurnEventIfNeeded(session);
			if (SyncAcpPlanToAssistantMessage(chat, session, true))
			{
				SaveChatQuietly(app, chat);
			}
			return;
		}
		if (method == "item/started" || method == "item/completed")
		{
			HandleCodexToolItem(session, chat, JsonObjectValue(params, "item"));
			SaveChatQuietly(app, chat);
			return;
		}
		if (method == "item/commandExecution/outputDelta" || method == "command/exec/outputDelta" || method == "item/fileChange/outputDelta")
		{
			const std::string item_id = JsonDiagnosticStringValue(params, "itemId");
			if (!item_id.empty())
			{
				AcpToolCallState& tool_call = UpsertToolCall(session, item_id);
				if (tool_call.title.empty())
				{
					tool_call.title = method.find("fileChange") != std::string::npos ? "File changes" : "Command output";
				}
				if (tool_call.kind.empty())
				{
					tool_call.kind = method.find("fileChange") != std::string::npos ? "fileChange" : "commandExecution";
				}
				if (tool_call.status.empty())
				{
					tool_call.status = "running";
				}
				tool_call.content += JsonDiagnosticStringValue(params, "delta");
				AppendToolTurnEventIfNeeded(session, item_id);
				(void)SyncAcpToolCallsToAssistantMessage(chat, session, false);
				SaveChatQuietly(app, chat);
			}
			return;
		}
		if (method == "item/commandExecution/requestApproval")
		{
			HandleCodexPendingPermission(session, message, "codex-command");
			return;
		}
		if (method == "item/fileChange/requestApproval")
		{
			HandleCodexPendingPermission(session, message, "codex-file");
			return;
		}
		if (method == "item/permissions/requestApproval")
		{
			HandleCodexPendingPermission(session, message, "codex-permissions");
			return;
		}
		if (method == "item/tool/requestUserInput")
		{
			HandleCodexUserInputRequest(session, message);
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, false);
			(void)SyncCurrentAssistantMessageBlocksFromTurnEvents(chat, session);
			SaveChatQuietly(app, chat);
			return;
		}
		if (method == "error")
		{
			const nlohmann::json error = JsonObjectValue(params, "error");
			const std::string error_message = CodexTurnErrorMessage(error);
			const std::string detail = CodexTurnErrorDetails(session, params, error);
			const bool will_retry = JsonBooleanValueOr(params, "willRetry", false);
			AppendAcpDiagnostic(session, "notification", will_retry ? "codex_turn_error_retrying" : "codex_turn_error", method, "", false, 0, error_message, detail);
			if (will_retry)
			{
				session.lifecycle_state = kAcpLifecycleProcessing;
				return;
			}
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
			FailAcpTurnOrSession(session, FormatAcpFailureMessage(session, "turn", "", false, 0, error_message, !detail.empty()));
			SaveChatQuietly(app, chat);
			MarkAcpChatUnseenIfBackground(app, chat);
			return;
		}
		if (method == "thread/started" || method == "thread/status/changed" || method == "serverRequest/resolved" || method == "thread/name/updated" || method == "thread/tokenUsage/updated" || method == "account/rateLimits/updated" || method == "configWarning" || method == "deprecationNotice")
		{
			return;
		}

		if (message.contains("id"))
		{
			AppendAcpDiagnostic(session, "request", "unsupported_method", method, JsonRpcIdToStableString(message["id"]), true, -32601, "UAM Codex app-server client does not implement method: " + method);
			SendJsonRpcError(session, message["id"], -32601, "UAM Codex app-server client does not implement method: " + method);
		}
	}

		void HandleAcpRequest(AppState&, AcpSessionState& session, const nlohmann::json& message)
		{
			const std::string method = message.value("method", "");
			if (method == "session/update")
			{
			return;
		}

		if (method == "session/request_permission")
		{
			HandlePermissionRequest(session, message);
			return;
		}

			if (message.contains("id"))
			{
				AppendAcpDiagnostic(session, "request", "unsupported_method", method, JsonRpcIdToStableString(message["id"]), true, -32601, "UAM ACP client does not implement method: " + method);
				SendJsonRpcError(session, message["id"], -32601, "UAM ACP client does not implement method: " + method);
			}
		}

		std::string PendingRequestSummary(const AcpSessionState& session)
		{
			if (session.pending_request_methods.empty())
			{
				return "";
			}

			std::ostringstream out;
			bool first = true;
			for (const auto& entry : session.pending_request_methods)
			{
				if (!first)
				{
					out << ", ";
				}
				out << entry.first << ":" << entry.second;
				first = false;
			}
			return out.str();
		}

		std::string ErrorDataForDiagnostics(const nlohmann::json& error)
		{
			if (!error.is_object() || !error.contains("data"))
			{
				return "";
			}
			return CapDiagnosticString(error["data"].dump(), kMaxAcpDiagnosticDetailBytes);
		}

		std::string JsonDiagnosticStringValue(const nlohmann::json& object, const char* key)
		{
			if (!object.is_object())
			{
				return "";
			}
			const auto it = object.find(key);
			if (it == object.end() || it->is_null())
			{
				return "";
			}
			if (it->is_string())
			{
				return it->get<std::string>();
			}
			if (it->is_boolean())
			{
				return it->get<bool>() ? "true" : "false";
			}
			if (it->is_number_integer() || it->is_number_unsigned() || it->is_number_float())
			{
				return it->dump();
			}
			return CapDiagnosticString(it->dump(), kMaxAcpDiagnosticDetailBytes);
		}

		std::string JsonDiagnosticStringValueOr(const nlohmann::json& object, const char* key, const std::string& fallback)
		{
			const std::string value = JsonDiagnosticStringValue(object, key);
			return value.empty() ? fallback : value;
		}

		bool JsonBooleanValueOr(const nlohmann::json& object, const char* key, const bool fallback)
		{
			if (!object.is_object())
			{
				return fallback;
			}
			const auto it = object.find(key);
			if (it == object.end() || it->is_null())
			{
				return fallback;
			}
			if (it->is_boolean())
			{
				return it->get<bool>();
			}
			if (it->is_string())
			{
				const std::string value = it->get<std::string>();
				if (value == "true")
				{
					return true;
				}
				if (value == "false")
				{
					return false;
				}
			}
			return fallback;
		}

		nlohmann::json JsonObjectValue(const nlohmann::json& object, const char* key)
		{
			if (!object.is_object())
			{
				return nlohmann::json::object();
			}
			const auto it = object.find(key);
			if (it == object.end() || !it->is_object())
			{
				return nlohmann::json::object();
			}
			return *it;
		}

		nlohmann::json JsonArrayValue(const nlohmann::json& object, const char* key)
		{
			if (!object.is_object())
			{
				return nlohmann::json::array();
			}
			const auto it = object.find(key);
			if (it == object.end() || !it->is_array())
			{
				return nlohmann::json::array();
			}
			return *it;
		}

		std::string CodexTurnErrorMessage(const nlohmann::json& error)
		{
			if (!error.is_object())
			{
				return "Codex app-server error.";
			}
			const std::string message = JsonDiagnosticStringValue(error, "message");
			return message.empty() ? "Codex app-server error." : message;
		}

		std::string CodexTurnErrorDetails(const AcpSessionState& session, const nlohmann::json& params, const nlohmann::json& error)
		{
			std::ostringstream detail;
			bool has_detail = false;
			const auto append_line = [&](const std::string& line)
			{
				if (line.empty())
				{
					return;
				}
				if (has_detail)
				{
					detail << "\n";
				}
				detail << line;
				has_detail = true;
			};

			if (params.contains("willRetry") && params["willRetry"].is_boolean())
			{
				append_line(std::string("willRetry=") + (params["willRetry"].get<bool>() ? "true" : "false"));
			}
			const std::string thread_id = JsonDiagnosticStringValue(params, "threadId");
			if (!thread_id.empty())
			{
				append_line("threadId=" + thread_id);
			}
			const std::string turn_id = JsonDiagnosticStringValue(params, "turnId");
			if (!turn_id.empty())
			{
				append_line("turnId=" + turn_id);
			}
			if (error.is_object())
			{
				const std::string additional_details = JsonDiagnosticStringValue(error, "additionalDetails");
				if (!additional_details.empty())
				{
					append_line("additionalDetails=" + additional_details);
				}
				if (error.contains("codexErrorInfo") && !error["codexErrorInfo"].is_null())
				{
					append_line("codexErrorInfo=" + CapDiagnosticString(error["codexErrorInfo"].dump(), kMaxAcpDiagnosticDetailBytes));
				}
			}
			if (!session.recent_stderr.empty())
			{
				append_line("stderr_tail=" + RecentStderrTail(session));
			}
			return detail.str();
		}

			std::string FormatAcpFailureMessage(const AcpSessionState& session,
			                                    const std::string& method,
			                                    const std::string& request_id,
			                                    const bool has_code,
			                                    const int code,
		                                    const std::string& message,
		                                    const bool has_detail)
		{
			std::ostringstream out;
			out << RuntimeDisplayName(session) << " " << (method.empty() ? "request" : method) << " failed";
			if (!request_id.empty() || has_code)
			{
				out << " (";
				bool first = true;
				if (!request_id.empty())
				{
					out << "id=" << request_id;
					first = false;
				}
				if (has_code)
				{
					if (!first)
					{
						out << ", ";
					}
					out << "code=" << code;
				}
				out << ")";
			}
			out << ": " << (message.empty() ? (std::string(RuntimeDisplayName(session)) + " request failed.") : message);
			if (has_detail)
			{
				out << " See diagnostics/stderr details.";
				}
				return out.str();
			}

			void UpdateAcpModesFromJson(AcpSessionState& session, const nlohmann::json& modes)
			{
				if (!modes.is_object())
				{
					return;
				}

				if (const nlohmann::json available_modes = modes.value("availableModes", nlohmann::json::array()); available_modes.is_array())
				{
					session.available_modes.clear();
					for (const nlohmann::json& mode : available_modes)
					{
						if (!mode.is_object())
						{
							continue;
						}

						AcpModeState parsed;
						parsed.id = mode.value("id", "");
						parsed.name = mode.value("name", parsed.id);
						parsed.description = mode.value("description", "");
						if (!parsed.id.empty())
						{
							session.available_modes.push_back(std::move(parsed));
						}
					}
				}

				const std::string current_mode_id = modes.value("currentModeId", "");
				if (!current_mode_id.empty())
				{
					session.current_mode_id = current_mode_id;
				}
			}

			void UpdateAcpModelsFromJson(AcpSessionState& session, const nlohmann::json& models)
			{
				if (!models.is_object())
				{
					return;
				}

				if (const nlohmann::json available_models = models.value("availableModels", nlohmann::json::array()); available_models.is_array())
				{
					session.available_models.clear();
					for (const nlohmann::json& model : available_models)
					{
						if (!model.is_object())
						{
							continue;
						}

						AcpModelState parsed;
						parsed.id = model.value("modelId", "");
						parsed.name = model.value("name", parsed.id);
						parsed.description = model.value("description", "");
						if (!parsed.id.empty())
						{
							session.available_models.push_back(std::move(parsed));
						}
					}
				}

				const std::string current_model_id = models.value("currentModelId", "");
				if (!current_model_id.empty())
				{
					session.current_model_id = current_model_id;
				}
			}

			void HandleAcpResponse(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
			{
			const std::string request_id = JsonRpcIdToStableString(message.value("id", nlohmann::json(nullptr)));
			const int id = JsonRpcNumericId(message.value("id", nlohmann::json(nullptr)));
			if (id == 0)
			{
				AppendAcpDiagnostic(session, "response", "ignored_invalid_id", "", request_id, false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
				return;
			}

			std::string method;
			if (const auto it = session.pending_request_methods.find(id); it != session.pending_request_methods.end())
		{
			method = it->second;
			session.pending_request_methods.erase(it);
		}
		if (session.prompt_request_id != 0 && id == session.prompt_request_id)
		{
			method = IsCodexSession(session) ? "turn/start" : "session/prompt";
		}

			if (message.contains("error"))
			{
				const nlohmann::json error = message["error"];
				const bool has_code = error.is_object() && error.contains("code") && error["code"].is_number_integer();
				const int code = has_code ? error["code"].get<int>() : 0;
				const std::string default_error = std::string(RuntimeDisplayName(session)) + " request failed.";
				const std::string error_message = error.is_object() ? error.value("message", default_error) : default_error;
				const std::string error_data = ErrorDataForDiagnostics(error);
				std::ostringstream detail;
				bool has_detail = false;
				if (!error_data.empty())
				{
					detail << "error.data=" << error_data;
					has_detail = true;
				}
				if (!session.recent_stderr.empty())
				{
					if (has_detail)
					{
						detail << "\n";
					}
					detail << "stderr_tail=" << RecentStderrTail(session);
					has_detail = true;
				}
				const std::string pending_summary = PendingRequestSummary(session);
				if (!pending_summary.empty())
				{
					if (has_detail)
					{
						detail << "\n";
					}
					detail << "pending_requests=" << pending_summary;
					has_detail = true;
				}
					const std::string detail_text = detail.str();
					const std::string formatted_error = FormatAcpFailureMessage(session, method, request_id, has_code, code, error_message, !detail_text.empty());
					AppendAcpDiagnostic(session, "response", "jsonrpc_error", method, request_id, has_code, code, error_message, detail_text);
					if (IsCodexSession(session) &&
					    method == "thread/resume" &&
					    has_code &&
					    code == -32600 &&
					    uam::codex::ErrorLooksLikeInvalidThreadId(error_message) &&
					    !session.codex_resume_fallback_attempted)
					{
						session.codex_resume_fallback_attempted = true;
						session.session_setup_request_id = 0;
						session.session_id.clear();
						session.codex_thread_id.clear();
						chat.native_session_id.clear();
						SaveChatQuietly(app, chat);
						AppendAcpDiagnostic(session, "response", "codex_invalid_resume_id_retry_start", method, request_id, has_code, code, "Codex rejected the stored thread id. Starting a new thread instead.", detail_text);

						const std::filesystem::path workspace_root = ResolveWorkspaceRootPath(app, chat);
						const std::string cwd = workspace_root.empty() ? std::filesystem::current_path().string() : workspace_root.string();
						const int retry_id = NextAcpRequestId(session, "thread/start");
						session.session_setup_request_id = retry_id;
						session.lifecycle_state = kAcpLifecycleStarting;
						if (!WriteAcpMessage(session, BuildCodexThreadStartRequest(retry_id, chat, cwd)))
						{
							session.pending_request_methods.erase(retry_id);
							session.session_setup_request_id = 0;
							(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
							FailAcpTurnOrSession(session, session.last_error.empty() ? formatted_error : session.last_error);
							SaveChatQuietly(app, chat);
							MarkAcpChatUnseenIfBackground(app, chat);
						}
						return;
					}
					if (RetryGeminiSessionNewAfterInvalidLoad(app, session, chat, method, request_id, has_code, code, error_message, error_data, detail_text, formatted_error))
					{
						return;
					}
						if (method == "session/prompt" || session.processing || session.waiting_for_permission || session.waiting_for_user_input || !session.queued_prompt.empty())
						{
						(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
						FailAcpTurnOrSession(session, formatted_error);
						SaveChatQuietly(app, chat);
						MarkAcpChatUnseenIfBackground(app, chat);
					}
				else
				{
					session.last_error = formatted_error;
					session.lifecycle_state = kAcpLifecycleError;
				}
				return;
		}

		const nlohmann::json result = message.value("result", nlohmann::json(nullptr));
		if (method == "initialize" || method == "thread/start" || method == "thread/resume" || method == "turn/start" || method == "session/new" || method == "session/load")
		{
			AppendAcpDiagnostic(session, "response", "jsonrpc_result", method, request_id, false, 0, "", "result=" + CapDiagnosticString(result.dump(), kMaxAcpDiagnosticDetailBytes));
		}
		if (method == "initialize")
		{
			session.initialize_request_id = 0;
			session.initialized = true;
			session.lifecycle_state = kAcpLifecycleStarting;
			if (IsCodexSession(session))
			{
				session.agent_name = "codex";
				session.agent_title = "Codex";
				if (result.is_object())
				{
					session.agent_version = result.value("userAgent", "");
				}
				session.load_session_supported = true;
				(void)WriteAcpMessage(session, BuildCodexInitializedNotification());
				const int model_list_id = NextAcpRequestId(session, "model/list");
				(void)WriteAcpMessage(session, BuildCodexModelListRequest(model_list_id));
				return;
			}
			if (result.is_object())
			{
				const nlohmann::json agent_info = result.value("agentInfo", nlohmann::json::object());
				if (agent_info.is_object())
				{
					session.agent_name = agent_info.value("name", "");
					session.agent_title = agent_info.value("title", "");
					session.agent_version = agent_info.value("version", "");
				}
				const nlohmann::json agent_capabilities = result.value("agentCapabilities", nlohmann::json::object());
				if (agent_capabilities.is_object())
				{
					session.load_session_supported = agent_capabilities.value("loadSession", false);
				}
			}
			return;
		}

				if (method == "model/list")
				{
					if (result.is_object())
					{
						const nlohmann::json data = result.value("data", nlohmann::json::array());
						if (data.is_array())
						{
							session.available_models.clear();
							std::vector<std::string> seen_model_ids;
							for (const nlohmann::json& model : data)
							{
								if (!model.is_object() || model.value("hidden", false))
								{
									continue;
								}

								const auto first_string = [&model](std::initializer_list<const char*> keys) -> std::string
								{
									for (const char* key : keys)
									{
										if (model.contains(key) && model[key].is_string())
										{
											const std::string value = Trim(model[key].get<std::string>());
											if (!value.empty())
											{
												return value;
											}
										}
									}
									return "";
								};

								AcpModelState parsed;
								parsed.id = first_string({"id", "model", "slug", "modelId"});
								if (parsed.id.empty())
								{
									continue;
								}

								const bool is_default = model.value("isDefault", false);
								const std::string visibility = first_string({"visibility"});
								if (!visibility.empty() && visibility != "list" && !is_default)
								{
									continue;
								}
								if (std::find(seen_model_ids.begin(), seen_model_ids.end(), parsed.id) != seen_model_ids.end())
								{
									continue;
								}

								parsed.name = first_string({"displayName", "display_name", "name"});
								if (parsed.name.empty())
								{
									parsed.name = parsed.id;
								}
								parsed.description = first_string({"description"});
								if (!parsed.id.empty())
								{
									if (is_default)
									{
										session.current_model_id = parsed.id;
									}
									seen_model_ids.push_back(parsed.id);
									session.available_models.push_back(std::move(parsed));
								}
							}

							const std::string explicit_current_model = Trim(result.value("currentModelId", result.value("model", "")));
							if (!explicit_current_model.empty())
							{
								session.current_model_id = explicit_current_model;
							}
						}
					}
					return;
				}

					if (method == "thread/start" || method == "thread/resume")
					{
						session.session_setup_request_id = 0;
						std::string returned_thread_id;
						if (result.is_object())
						{
							const nlohmann::json thread = result.value("thread", nlohmann::json::object());
							if (thread.is_object())
							{
								returned_thread_id = thread.value("id", "");
							}
							session.current_model_id = result.value("model", session.current_model_id);
						}
						if (uam::codex::IsValidThreadId(returned_thread_id))
						{
							session.codex_thread_id = returned_thread_id;
							session.session_id = session.codex_thread_id;
						}
						else
						{
							session.codex_thread_id.clear();
							session.session_id.clear();
						}
						if (!session.session_id.empty() && chat.native_session_id != session.session_id)
						{
							chat.native_session_id = session.session_id;
					}
					session.available_modes = {
						AcpModeState{"default", "Default", "Use Codex default collaboration mode."},
						AcpModeState{"plan", "Plan", "Ask Codex to plan before implementing."},
					};
					session.current_mode_id = chat.approval_mode.empty() ? "default" : chat.approval_mode;
					session.session_ready = !session.session_id.empty();
						session.lifecycle_state = session.session_ready ? kAcpLifecycleReady : kAcpLifecycleError;
						if (!session.session_ready)
						{
							const std::string detail = "result=" + CapDiagnosticString(result.dump(), kMaxAcpDiagnosticDetailBytes) + (session.recent_stderr.empty() ? "" : "\nstderr_tail=" + RecentStderrTail(session));
							session.last_error = FormatAcpFailureMessage(session, method, request_id, false, 0, "Codex app-server did not return a valid thread id.", true);
							AppendAcpDiagnostic(session, "response", "missing_thread_id", method, request_id, false, 0, session.last_error, detail);
						}
					SaveChatQuietly(app, chat);
					return;
				}

				if (method == "turn/start")
				{
					session.prompt_request_id = 0;
					if (result.is_object())
					{
						const nlohmann::json turn = result.value("turn", nlohmann::json::object());
						if (turn.is_object())
						{
							session.codex_turn_id = turn.value("id", session.codex_turn_id);
						}
					}
					session.lifecycle_state = kAcpLifecycleProcessing;
					return;
				}

				if (method == "session/new")
				{
					session.session_setup_request_id = 0;
					if (result.is_object())
					{
						session.session_id = result.value("sessionId", session.session_id);
						UpdateAcpModesFromJson(session, result.value("modes", nlohmann::json::object()));
						UpdateAcpModelsFromJson(session, result.value("models", nlohmann::json::object()));
					}
					if (!session.session_id.empty() && chat.native_session_id != session.session_id)
					{
				chat.native_session_id = session.session_id;
			}
			session.session_ready = !session.session_id.empty();
				session.lifecycle_state = session.session_ready ? kAcpLifecycleReady : kAcpLifecycleError;
				if (!session.session_ready)
				{
					const std::string detail = "result=" + CapDiagnosticString(result.dump(), kMaxAcpDiagnosticDetailBytes) + (session.recent_stderr.empty() ? "" : "\nstderr_tail=" + RecentStderrTail(session));
					session.last_error = FormatAcpFailureMessage(session, method, request_id, false, 0, std::string(RuntimeDisplayName(session)) + " did not return a session id.", true);
					AppendAcpDiagnostic(session, "response", "missing_session_id", method, request_id, false, 0, session.last_error, detail);
				}
				SaveChatQuietly(app, chat);
				return;
		}

				if (method == "session/load")
				{
					session.session_setup_request_id = 0;
					if (result.is_object())
					{
						UpdateAcpModesFromJson(session, result.value("modes", nlohmann::json::object()));
						UpdateAcpModelsFromJson(session, result.value("models", nlohmann::json::object()));
					}
					session.session_ready = true;
					session.ignore_session_updates_until_ready = false;
					session.lifecycle_state = kAcpLifecycleReady;
					return;
				}

			if (method == "session/prompt")
			{
				(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
				CompletePromptTurn(session, kAcpLifecycleReady);
				SaveChatQuietly(app, chat);
				MarkAcpChatUnseenIfBackground(app, chat);
			return;
		}

				if (method == "session/cancel")
				{
					session.cancel_request_id = 0;
					return;
				}

				if (method == "session/set_mode" || method == "session/set_model")
				{
					return;
				}

			AppendAcpDiagnostic(session, "response", "unknown_request_id", method, request_id, false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
		}

	bool ProcessAcpLine(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line)
	{
		const std::string trimmed = uam::strings::Trim(line);
		if (trimmed.empty())
		{
			return false;
		}

		nlohmann::json message;
		try
		{
			message = nlohmann::json::parse(trimmed);
			}
			catch (const std::exception& ex)
			{
				const std::string error_message = std::string("Invalid JSON from ") + RuntimeDisplayName(session) + ": " + ex.what();
				AppendAcpDiagnostic(session, "parse", "invalid_json", "", "", false, 0, error_message, CapDiagnosticString(trimmed, kMaxAcpDiagnosticDetailBytes));
				FailAcpTurnOrSession(session, error_message);
				MarkAcpChatUnseenIfBackground(app, chat);
				return true;
			}

		if (message.contains("method"))
		{
			const std::string method = JsonDiagnosticStringValue(message, "method");
			if (IsCodexSession(session))
			{
				try
				{
					HandleCodexMessage(app, session, chat, message);
				}
				catch (const std::exception& ex)
				{
					const std::string error_message = std::string("Codex app-server message handling failed: ") + ex.what();
					AppendAcpDiagnostic(session, "parse", "codex_message_parse_error", method, "", false, 0, error_message, CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
					FailAcpTurnOrSession(session, error_message);
					MarkAcpChatUnseenIfBackground(app, chat);
				}
			}
			else if (method == "session/update")
			{
				HandleSessionUpdate(app, session, chat, message.value("params", nlohmann::json::object()));
			}
			else
			{
				HandleAcpRequest(app, session, message);
			}
			return true;
		}

			if (message.contains("id"))
			{
				HandleAcpResponse(app, session, chat, message);
				return true;
			}

			AppendAcpDiagnostic(session, "message", "ignored_without_method_or_id", "", "", false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
			return false;
		}

	bool DrainStdout(AppState& app, AcpSessionState& session, ChatSession& chat)
	{
		bool changed = false;
			std::array<char, 8192> buffer{};
			while (true)
			{
				std::string read_error;
				const std::ptrdiff_t read_bytes = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStdout(session, buffer.data(), buffer.size(), &read_error);
				if (read_bytes > 0)
				{
					session.stdout_buffer.append(buffer.data(), static_cast<std::size_t>(read_bytes));
				std::size_t newline_pos = std::string::npos;
				while ((newline_pos = session.stdout_buffer.find('\n')) != std::string::npos)
				{
					std::string line = session.stdout_buffer.substr(0, newline_pos);
					session.stdout_buffer.erase(0, newline_pos + 1);
					changed = ProcessAcpLine(app, session, chat, line) || changed;
				}
				continue;
			}

			if (read_bytes == -2)
			{
				break;
			}

			if (read_bytes == 0)
			{
					break;
				}

				const std::string message = read_error.empty() ? ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stdout.") : ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stdout: " + read_error);
				AppendAcpDiagnostic(session, "read", "stdout_read_failed", "", "", false, 0, message);
				FailAcpTurnOrSession(session, message);
				MarkAcpChatUnseenIfBackground(app, chat);
				changed = true;
				break;
		}
		return changed;
	}

	bool DrainStderr(AcpSessionState& session)
	{
		bool changed = false;
			std::array<char, 4096> buffer{};
			while (true)
			{
				std::string read_error;
				const std::ptrdiff_t read_bytes = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStderr(session, buffer.data(), buffer.size(), &read_error);
				if (read_bytes > 0)
				{
					AppendRecentStderr(session, std::string(buffer.data(), static_cast<std::size_t>(read_bytes)));
				changed = true;
				continue;
			}

			if (read_bytes == -2 || read_bytes == 0)
			{
					break;
				}

				const std::string message = read_error.empty() ? ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stderr.") : ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stderr: " + read_error);
				AppendAcpDiagnostic(session, "read", "stderr_read_failed", "", "", false, 0, message);
				changed = true;
				break;
			}
			return changed;
		}

		void MarkAcpProcessExited(AcpSessionState& session, const bool has_exit_code = false, const int exit_code = 0)
		{
			if (has_exit_code)
			{
				session.has_last_exit_code = true;
				session.last_exit_code = exit_code;
			}
			const bool active_turn = session.processing || session.waiting_for_permission || session.waiting_for_user_input || session.prompt_request_id != 0 || !session.queued_prompt.empty();
			std::ostringstream detail;
			bool has_detail = false;
			if (has_exit_code)
			{
				detail << "exit_code=" << exit_code;
				has_detail = true;
			}
			if (!session.recent_stderr.empty())
			{
				if (has_detail)
				{
					detail << "\n";
				}
				detail << "stderr_tail=" << RecentStderrTail(session);
				has_detail = true;
			}
			const std::string pending_summary = PendingRequestSummary(session);
			if (!pending_summary.empty())
			{
				if (has_detail)
				{
					detail << "\n";
				}
				detail << "pending_requests=" << pending_summary;
			}
			AppendAcpDiagnostic(session, "process_exit", active_turn ? "active_turn" : "idle", "", "", has_exit_code, exit_code, "", detail.str());
			session.running = false;
			session.initialized = false;
			session.session_ready = false;
			if (active_turn)
			{
				const std::string message = session.last_error.empty() ? (std::string(RuntimeDisplayName(session)) + " process exited during an active turn.") : session.last_error;
				FailAcpTurnOrSession(session, message);
		}
		else
		{
			session.lifecycle_state = kAcpLifecycleStopped;
		}
		session.processing = false;
		session.waiting_for_permission = false;
		session.waiting_for_user_input = false;
			session.prompt_request_id = 0;
			session.cancel_request_id = 0;
			session.current_assistant_message_index = -1;
			session.pending_assistant_thoughts.clear();
			session.pending_permission = AcpPendingPermissionState{};
			session.pending_user_input = AcpPendingUserInputState{};
		PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
	}
} // namespace

AcpSessionState* FindAcpSessionForChat(AppState& app, const std::string& chat_id)
{
	for (auto& session : app.acp_sessions)
	{
		if (session != nullptr && session->chat_id == chat_id)
		{
			return session.get();
		}
	}
	return nullptr;
}

const AcpSessionState* FindAcpSessionForChat(const AppState& app, const std::string& chat_id)
{
	for (const auto& session : app.acp_sessions)
	{
		if (session != nullptr && session->chat_id == chat_id)
		{
			return session.get();
		}
	}
	return nullptr;
}

bool SendAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, std::string* error_out)
{
	const std::string prompt = uam::strings::Trim(text);
	if (prompt.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Prompt is empty.";
		}
		return false;
	}

	const int chat_index = ChatDomainService().FindChatIndexById(app, chat_id);
	if (chat_index < 0)
	{
		if (error_out != nullptr)
		{
			*error_out = "Chat not found: " + chat_id;
		}
		return false;
	}

	ChatSession& chat = app.chats[static_cast<std::size_t>(chat_index)];
	AcpSessionState& session = EnsureAcpSessionForChat(app, chat);
	if (session.processing)
	{
		if (error_out != nullptr)
		{
			*error_out = std::string(RuntimeDisplayName(session)) + " is already processing this chat.";
		}
		return false;
	}

	if (!StartAcpProcessForChat(app, session, chat, error_out))
	{
		return false;
	}

	ChatDomainService().AddMessageWithAnalytics(chat, MessageRole::User, prompt, MessageProviderId(session), 0, 0, 0, 0, false);
	SaveChatQuietly(app, chat);

	session.queued_prompt = prompt;
	session.processing = true;
	session.waiting_for_permission = false;
	session.waiting_for_user_input = false;
	session.current_assistant_message_index = -1;
	session.turn_user_message_index = static_cast<int>(chat.messages.size()) - 1;
		session.turn_assistant_message_index = -1;
		session.turn_serial += 1;
		RememberAssistantReplayPrefixes(session, chat, session.turn_user_message_index);
		RememberLoadHistoryReplayUpdates(session, chat, session.turn_user_message_index);
		session.pending_assistant_thoughts.clear();
	session.tool_calls.clear();
	session.plan_entries.clear();
	session.plan_summary.clear();
	session.codex_agent_message_text_by_item_id.clear();
	session.codex_last_agent_message_item_id.clear();
	session.codex_streamed_reasoning_keys.clear();
	session.codex_last_reasoning_section.clear();
	session.turn_events.clear();
	session.pending_permission = AcpPendingPermissionState{};
	session.pending_user_input = AcpPendingUserInputState{};
	session.last_error.clear();
	session.lifecycle_state = session.session_ready ? kAcpLifecycleProcessing : kAcpLifecycleStarting;

	if (session.session_ready)
	{
		(void)SendQueuedPromptIfReady(session, chat);
	}

	return true;
}

bool CancelAcpTurn(AppState& app, const std::string& chat_id, std::string* error_out)
{
	AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
	if (session == nullptr || !session->running)
	{
		return true;
	}

	const std::string pending_permission_request_id = session->pending_permission.request_id_json;
	if (!pending_permission_request_id.empty())
	{
		(void)SendPermissionResponse(*session, pending_permission_request_id, "", true, error_out);
	}
	const std::string pending_user_input_request_id = session->pending_user_input.request_id_json;
	if (!pending_user_input_request_id.empty())
	{
		(void)SendCodexUserInputResponse(*session, pending_user_input_request_id, {}, error_out);
	}

	session->queued_prompt.clear();
	session->processing = false;
	session->waiting_for_permission = false;
	session->waiting_for_user_input = false;
		session->pending_permission = AcpPendingPermissionState{};
		session->pending_user_input = AcpPendingUserInputState{};
		session->current_assistant_message_index = -1;
		session->pending_assistant_thoughts.clear();
		session->lifecycle_state = session->session_ready ? kAcpLifecycleReady : kAcpLifecycleStopped;

	if (IsCodexSession(*session) && !session->session_id.empty() && !session->codex_turn_id.empty())
	{
		const int id = NextAcpRequestId(*session, "turn/interrupt");
		session->cancel_request_id = id;
		if (!WriteAcpMessage(*session, BuildCodexTurnInterruptRequest(id, session->session_id, session->codex_turn_id), error_out))
		{
			session->pending_request_methods.erase(id);
			session->cancel_request_id = 0;
			return false;
		}
	}
	else if (!session->session_id.empty())
	{
		if (!WriteAcpMessage(*session, BuildCancelNotification(session->session_id), error_out))
		{
			return false;
		}
	}

	return true;
}

	bool StopAcpSession(AppState& app, const std::string& chat_id)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr)
	{
		return true;
	}

	if (session->running)
	{
		PlatformServicesFactory::Instance().process_service.StopStdioProcess(*session, true);
	}

	session->running = false;
	session->initialized = false;
	session->session_ready = false;
	session->processing = false;
	session->waiting_for_permission = false;
	session->waiting_for_user_input = false;
	session->lifecycle_state = kAcpLifecycleStopped;
	session->queued_prompt.clear();
	session->prompt_request_id = 0;
	session->cancel_request_id = 0;
	session->current_assistant_message_index = -1;
	session->turn_user_message_index = -1;
		session->turn_assistant_message_index = -1;
		session->turn_events.clear();
		session->assistant_replay_prefixes.clear();
		session->load_history_replay_updates.clear();
		session->pending_assistant_thoughts.clear();
		session->pending_permission = AcpPendingPermissionState{};
		session->pending_user_input = AcpPendingUserInputState{};
		PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*session);
		return true;
	}

	bool SetAcpSessionMode(AppState& app, const std::string& chat_id, const std::string& mode_id, std::string* error_out)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr || !session->running)
		{
			return true;
		}
		if (session->processing || session->waiting_for_permission || session->waiting_for_user_input || !session->queued_prompt.empty() || session->prompt_request_id != 0 || session->cancel_request_id != 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "Cannot change structured runtime mode while " + std::string(RuntimeDisplayName(*session)) + " is busy.";
			}
			return false;
		}
		if (!session->session_ready || session->session_id.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "ACP session is not ready.";
			}
			return false;
		}
		if (IsCodexSession(*session))
		{
			session->current_mode_id = mode_id;
			return true;
		}

		const int id = NextAcpRequestId(*session, "session/set_mode");
		if (!WriteAcpMessage(*session, BuildSetModeRequest(id, session->session_id, mode_id), error_out))
		{
			session->pending_request_methods.erase(id);
			return false;
		}
		session->current_mode_id = mode_id;
		return true;
	}

	bool SetAcpSessionModel(AppState& app, const std::string& chat_id, const std::string& model_id, std::string* error_out)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr || !session->running)
		{
			return true;
		}
		if (session->processing || session->waiting_for_permission || session->waiting_for_user_input || !session->queued_prompt.empty() || session->prompt_request_id != 0 || session->cancel_request_id != 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "Cannot change structured runtime model while " + std::string(RuntimeDisplayName(*session)) + " is busy.";
			}
			return false;
		}
		if (!session->session_ready || session->session_id.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "ACP session is not ready.";
			}
			return false;
		}
		if (IsCodexSession(*session))
		{
			session->current_model_id = model_id;
			return true;
		}

		const int id = NextAcpRequestId(*session, "session/set_model");
		if (!WriteAcpMessage(*session, BuildSetModelRequest(id, session->session_id, model_id), error_out))
		{
			session->pending_request_methods.erase(id);
			return false;
		}
		session->current_model_id = model_id;
		return true;
	}

bool ResolveAcpPermission(AppState& app,
	                          const std::string& chat_id,
                          const std::string& request_id_json,
                          const std::string& option_id,
                          const bool cancelled,
                          std::string* error_out)
{
	AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
	if (session == nullptr || !session->running)
	{
		if (error_out != nullptr)
		{
			*error_out = "ACP session is not running.";
		}
		return false;
	}

	if (session->pending_permission.request_id_json != request_id_json)
	{
		if (error_out != nullptr)
		{
			*error_out = "ACP permission request is no longer active.";
		}
		return false;
	}

	if (!SendPermissionResponse(*session, request_id_json, option_id, cancelled, error_out))
	{
		return false;
	}

	session->pending_permission = AcpPendingPermissionState{};
	session->waiting_for_permission = false;
	session->lifecycle_state = session->processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
	return true;
}

bool ResolveAcpUserInput(AppState& app,
                         const std::string& chat_id,
                         const std::string& request_id_json,
                         const std::map<std::string, std::vector<std::string>>& answers,
                         std::string* error_out)
{
	AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
	if (session == nullptr || !session->running)
	{
		if (error_out != nullptr)
		{
			*error_out = "ACP session is not running.";
		}
		return false;
	}

	if (session->pending_user_input.request_id_json != request_id_json)
	{
		if (error_out != nullptr)
		{
			*error_out = "ACP user input request is no longer active.";
		}
		return false;
	}

	if (!SendCodexUserInputResponse(*session, request_id_json, answers, error_out))
	{
		return false;
	}

	session->pending_user_input = AcpPendingUserInputState{};
	session->waiting_for_user_input = false;
	session->lifecycle_state = session->processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
	return true;
}

bool PollAllAcpSessions(AppState& app)
{
	bool changed = false;
	for (auto& session_ptr : app.acp_sessions)
	{
		if (session_ptr == nullptr)
		{
			continue;
		}

		AcpSessionState& session = *session_ptr;
		if (!session.running)
		{
			continue;
		}

			const int chat_index = ChatDomainService().FindChatIndexById(app, session.chat_id);
			if (chat_index < 0)
			{
				PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true);
				MarkAcpProcessExited(session, false, 0);
				changed = true;
				continue;
			}

		ChatSession& chat = app.chats[static_cast<std::size_t>(chat_index)];
		changed = DrainStderr(session) || changed;
		changed = DrainStdout(app, session, chat) || changed;

		if (SendSessionSetupIfReady(app, session, chat))
		{
			changed = true;
		}

		if (SendQueuedPromptIfReady(session, chat))
		{
			if (!session.last_error.empty() && session.lifecycle_state == kAcpLifecycleError)
			{
				MarkAcpChatUnseenIfBackground(app, chat);
			}
			changed = true;
		}

			int exit_code = 0;
			if (PlatformServicesFactory::Instance().process_service.PollStdioProcessExited(session, &exit_code))
			{
				MarkAcpProcessExited(session, true, exit_code);
				if (!session.last_error.empty())
				{
					MarkAcpChatUnseenIfBackground(app, chat);
			}
			changed = true;
		}
	}

	return changed;
}

void FastStopAcpSessionsForExit(AppState& app)
{
	for (auto& session : app.acp_sessions)
	{
		if (session != nullptr)
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(*session, true);
			session->running = false;
			session->lifecycle_state = kAcpLifecycleStopped;
		}
	}
	app.acp_sessions.clear();
}

std::vector<std::string> BuildAcpLaunchArgvForTests(const ChatSession& chat)
{
	return BuildAcpLaunchArgv(chat);
}

std::string BuildAcpLaunchDetailForTests(const std::filesystem::path& workspace_root, const ChatSession& chat)
{
	return BuildAcpLaunchDetail(workspace_root, chat);
}

std::string BuildAcpInitializeRequestForTests(const int request_id)
{
	return BuildInitializeRequest(request_id).dump();
}

std::string BuildAcpNewSessionRequestForTests(const int request_id, const std::string& cwd)
{
	return BuildNewSessionRequest(request_id, cwd).dump();
}

std::string BuildGeminiSessionSetupRequestForTests(const int request_id, const ChatSession& chat, const std::string& cwd, const bool load_session_supported)
{
	return BuildGeminiSessionSetupRequest(request_id, chat, cwd, load_session_supported).dump();
}

	std::string BuildAcpPromptRequestForTests(const int request_id, const std::string& session_id, const std::string& text)
	{
		return BuildPromptRequest(request_id, session_id, text).dump();
	}

	std::string BuildAcpSetModeRequestForTests(const int request_id, const std::string& session_id, const std::string& mode_id)
	{
		return BuildSetModeRequest(request_id, session_id, mode_id).dump();
	}

	std::string BuildAcpSetModelRequestForTests(const int request_id, const std::string& session_id, const std::string& model_id)
	{
		return BuildSetModelRequest(request_id, session_id, model_id).dump();
	}

	std::string BuildCodexInitializeRequestForTests(const int request_id)
	{
		return BuildCodexInitializeRequest(request_id).dump();
	}

	std::string BuildCodexInitializedNotificationForTests()
	{
		return BuildCodexInitializedNotification().dump();
	}

	std::string BuildCodexModelListRequestForTests(const int request_id)
	{
		return BuildCodexModelListRequest(request_id).dump();
	}

	std::string BuildCodexSessionSetupRequestForTests(const int request_id, const ChatSession& chat, const std::string& cwd)
	{
		return BuildCodexSessionSetupRequest(request_id, chat, cwd).dump();
	}

	std::string BuildCodexThreadStartRequestForTests(const int request_id, const ChatSession& chat, const std::string& cwd)
	{
		return BuildCodexThreadStartRequest(request_id, chat, cwd).dump();
	}

	std::string BuildCodexThreadResumeRequestForTests(const int request_id, const ChatSession& chat, const std::string& cwd)
	{
		return BuildCodexThreadResumeRequest(request_id, chat, cwd).dump();
	}

	std::string BuildCodexTurnStartRequestForTests(const int request_id, const std::string& thread_id, const std::string& text, const ChatSession& chat, const std::string& active_model_id)
	{
		return BuildCodexTurnStartRequest(request_id, thread_id, text, chat, active_model_id).dump();
	}

	std::string BuildCodexTurnInterruptRequestForTests(const int request_id, const std::string& thread_id, const std::string& turn_id)
	{
		return BuildCodexTurnInterruptRequest(request_id, thread_id, turn_id).dump();
	}

	std::string BuildCodexUserInputResponseForTests(const std::string& request_id_json, const std::map<std::string, std::vector<std::string>>& answers)
	{
		return BuildCodexUserInputResponse(request_id_json, answers).dump();
	}

	bool ProcessAcpLineForTests(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line)
	{
	return ProcessAcpLine(app, session, chat, line);
	}

	bool IsValidCodexThreadIdForTests(const std::string& thread_id)
	{
		return uam::codex::IsValidThreadId(thread_id);
	}

} // namespace uam
