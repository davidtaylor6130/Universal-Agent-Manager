#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"

#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/chat/message_attachment_json.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/utils/base64.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// ACP handlers (prompt, attachments, permissions, user input, clipboard)
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;

namespace
{
	namespace attachment_fields = uam::message_attachment_json;
	namespace attachment_frontend_fields = uam::message_attachment_json::frontend;

	constexpr std::size_t kMaxClipboardTextBytes = 1024 * 1024;
	constexpr std::uintmax_t kMaxAttachmentBytes = 25ull * 1024ull * 1024ull;

#if defined(_WIN32)
	std::wstring WideFromUtf8(const std::string& value)
	{
		if (value.empty())
		{
			return std::wstring();
		}
		const int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
		if (wide_len <= 0)
		{
			return std::wstring();
		}
		std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), wide.data(), wide_len) <= 0)
		{
			return std::wstring();
		}
		return wide;
	}
#endif

#if defined(__APPLE__)
	bool WriteAllToFileDescriptor(int fd, const std::string& text, std::string* error_out)
	{
		std::size_t offset = 0;
		while (offset < text.size())
		{
			const ssize_t written = write(fd, text.data() + offset, text.size() - offset);
			if (written > 0)
			{
				offset += static_cast<std::size_t>(written);
				continue;
			}

			if (written < 0 && errno == EINTR)
			{
				continue;
			}

			if (error_out != nullptr)
			{
				*error_out = written == 0 ? "Failed to write clipboard text: write made no progress." : "Failed to write clipboard text: " + std::string(std::strerror(errno)) + ".";
			}
			return false;
		}

		return true;
	}

	bool WaitForClipboardProcess(const pid_t pid, std::string* error_out)
	{
		int status = 0;
		while (waitpid(pid, &status, 0) < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			if (error_out != nullptr)
			{
				*error_out = "Failed waiting for pbcopy: " + std::string(std::strerror(errno)) + ".";
			}
			return false;
		}

		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		{
			return true;
		}

		if (error_out != nullptr)
		{
			if (WIFEXITED(status))
			{
				*error_out = "pbcopy exited with status " + std::to_string(WEXITSTATUS(status)) + ".";
			}
			else if (WIFSIGNALED(status))
			{
				*error_out = "pbcopy terminated by signal " + std::to_string(WTERMSIG(status)) + ".";
			}
			else
			{
				*error_out = "pbcopy ended without a normal exit status.";
			}
		}

		return false;
	}
#endif

	bool WriteNativeClipboardText(const std::string& text, std::string* error_out)
	{
#if defined(__APPLE__)
		int stdin_pipe[2] = {-1, -1};
		if (pipe(stdin_pipe) != 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to create pbcopy pipe: " + std::string(std::strerror(errno)) + ".";
			}
			return false;
		}

		const pid_t pid = fork();
		if (pid < 0)
		{
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			if (error_out != nullptr)
			{
				*error_out = "Failed to launch pbcopy: " + std::string(std::strerror(errno)) + ".";
			}
			return false;
		}

		if (pid == 0)
		{
			close(stdin_pipe[1]);
			if (dup2(stdin_pipe[0], STDIN_FILENO) < 0)
			{
				_exit(126);
			}
			close(stdin_pipe[0]);
			execl("/usr/bin/pbcopy", "pbcopy", static_cast<char*>(nullptr));
			_exit(127);
		}

		close(stdin_pipe[0]);
		std::string write_error;
		const bool write_ok = WriteAllToFileDescriptor(stdin_pipe[1], text, &write_error);
		close(stdin_pipe[1]);

		std::string wait_error;
		const bool wait_ok = WaitForClipboardProcess(pid, &wait_error);

		if (!write_ok || !wait_ok)
		{
			if (error_out != nullptr)
			{
				*error_out = !write_ok ? write_error : wait_error;
			}
			return false;
		}

		return true;
#elif defined(_WIN32)
		const std::wstring wide = WideFromUtf8(text);
		if (wide.empty() && !text.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "Clipboard text is not valid UTF-8.";
			}
			return false;
		}
		if (!OpenClipboard(nullptr))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to open the Windows clipboard.";
			}
			return false;
		}

		if (!EmptyClipboard())
		{
			CloseClipboard();
			if (error_out != nullptr)
			{
				*error_out = "Failed to clear the Windows clipboard.";
			}
			return false;
		}

		const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
		HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
		if (memory == nullptr)
		{
			CloseClipboard();
			if (error_out != nullptr)
			{
				*error_out = "Failed to allocate Windows clipboard memory.";
			}
			return false;
		}

		void* locked = GlobalLock(memory);
		if (locked == nullptr)
		{
			GlobalFree(memory);
			CloseClipboard();
			if (error_out != nullptr)
			{
				*error_out = "Failed to lock Windows clipboard memory.";
			}
			return false;
		}
		std::memcpy(locked, wide.c_str(), bytes);
		GlobalUnlock(memory);

		if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
		{
			GlobalFree(memory);
			CloseClipboard();
			if (error_out != nullptr)
			{
				*error_out = "Failed to set Windows clipboard text.";
			}
			return false;
		}
		CloseClipboard();
		return true;
#else
		if (error_out != nullptr)
		{
			*error_out = "Native clipboard writes are not implemented for this platform.";
		}
		return false;
#endif
	}

	std::string SafeAttachmentName(std::string value, const std::string& fallback)
	{
		std::string out;
		out.reserve(value.size());
		for (const unsigned char ch : value)
		{
			if (uam::strings::IsAsciiAlnum(ch) || ch == '.' || ch == '-' || ch == '_')
			{
				out.push_back(static_cast<char>(ch));
			}
			else if (ch == ' ')
			{
				out.push_back('-');
			}
		}
		if (out.empty() || out == "." || out == "..")
		{
			out = fallback;
		}
		return out.substr(0, 120);
	}

	std::string AttachmentId()
	{
		return "att-" + uam::time::SystemEpochMicrosecondsTokenNow();
	}

	std::string PathForPrompt(const std::filesystem::path& workspace_root, const std::filesystem::path& path)
	{
		const std::filesystem::path final_path = uam::paths::AbsolutePathNoThrow(path);
		if (const std::optional<std::filesystem::path> relative = uam::paths::RelativePathIfInsideRoot(workspace_root, final_path))
		{
			return uam::paths::PortablePathString(*relative);
		}
		return uam::paths::PortablePathString(final_path);
	}

	nlohmann::json AttachmentToJson(const MessageAttachment& attachment)
	{
		return {
		    {std::string(attachment_fields::kIdField), attachment.id},
		    {std::string(attachment_fields::kNameField), attachment.name},
		    {std::string(attachment_fields::kKindField), attachment.kind},
		    {std::string(attachment_frontend_fields::kMimeTypeField), attachment.mime_type},
		    {std::string(attachment_fields::kPathField), attachment.path},
		    {std::string(attachment_frontend_fields::kSizeBytesField), attachment.size_bytes},
		    {std::string(attachment_fields::kCopiedField), attachment.copied},
		};
	}

	std::string NormalizeStagedAttachmentKind(std::string_view requested_kind)
	{
		requested_kind = uam::strings::TrimAsciiView(requested_kind);
		if (requested_kind == attachment_frontend_fields::kDirectoryKind)
		{
			return std::string(attachment_frontend_fields::kDirectoryKind);
		}
		if (requested_kind == attachment_frontend_fields::kImageKind)
		{
			return std::string(attachment_frontend_fields::kImageKind);
		}

		return std::string(attachment_frontend_fields::kFileKind);
	}

	std::vector<MessageAttachment> ParseStagedAttachments(const nlohmann::json& payload)
	{
		std::vector<MessageAttachment> attachments;
		const nlohmann::json* attachment_items = uam::nlohmann_json::FindArrayField(payload, attachment_frontend_fields::kAttachmentsField);
		if (attachment_items == nullptr)
		{
			return attachments;
		}
		for (const nlohmann::json& item : *attachment_items)
		{
			if (!item.is_object())
			{
				continue;
			}
			MessageAttachment attachment;
			attachment.id = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kIdField});
			attachment.name = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kNameField});
			attachment.kind = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kKindField, attachment_frontend_fields::kMimeTypeField});
			attachment.mime_type = uam::nlohmann_json::TrimmedStringValue(item, {attachment_frontend_fields::kMimeTypeInputField, attachment_frontend_fields::kMimeTypeField});
			if (attachment.kind == attachment.mime_type || uam::strings::Contains(attachment.kind, '/'))
			{
				attachment.kind = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kKindField});
			}
			attachment.path = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kPathField});
			attachment.size_bytes = item.value(std::string(attachment_frontend_fields::kSizeBytesField), static_cast<std::uintmax_t>(0));
			attachment.copied = item.value(std::string(attachment_fields::kCopiedField), false);
			if (attachment.path.empty())
			{
				continue;
			}
			if (attachment.id.empty())
			{
				attachment.id = AttachmentId();
			}
			if (attachment.name.empty())
			{
				attachment.name = std::filesystem::path(attachment.path).filename().string();
			}
			if (attachment.kind.empty())
			{
				attachment.kind = "file";
			}
			attachments.push_back(std::move(attachment));
		}
		return attachments;
	}
} // namespace

void UamQueryHandler::HandleSendAcpPrompt(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string text = payload.value("text", "");
	const int missing_chat_status = chat_id.empty() || text.empty() ? 400 : 404;
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id, missing_chat_status);
	if (chat == nullptr)
	{
		return;
	}

	std::string hydrate_warning;
	if (!ChatRepository::HydrateChatMessages(m_app.data_root, *chat, &hydrate_warning))
	{
		cb->Failure(500, FailureDetailOrFallback(hydrate_warning, "Failed to load chat messages."));
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	const std::vector<std::string> markdown_store_files = uam::nlohmann_json::StringArrayField(payload, "markdownStoreFiles");

	std::string error;
	const std::vector<MessageAttachment> attachments = ParseStagedAttachments(payload);
	const bool goal_mode = payload.value("goalMode", false);
	const std::string goal_id = AcpPromptGoalIdFromPayload(payload);
	const bool steer_now = payload.value("steerNow", false);
	const bool sent = steer_now
	                    ? uam::SteerAcpPrompt(m_app, chat_id, text, markdown_store_files, attachments, goal_mode, &error, goal_id)
	                    : uam::SendAcpPrompt(m_app, chat_id, text, markdown_store_files, attachments, goal_mode, &error, goal_id);
	if (!sent)
	{
		cb->Failure(chat_id.empty() || text.empty() ? 400 : 500, FailureDetailOrFallback(error, "Failed to send ACP prompt."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleDiscoverProviderModels(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (!ChatProviderAvailableOrFail(m_app, *chat, cb)) return;
	if (m_app.provider_model_catalog == nullptr)
	{
		cb->Failure(500, "Provider model catalog is unavailable.");
		return;
	}
	const bool should_start = m_app.provider_model_catalog->BeginDiscoveryIfMissing(chat->provider_id);
	if (!should_start)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"started", false}, {"pending", m_app.provider_model_catalog->IsDiscoveryPending(chat->provider_id)}}.dump());
		return;
	}
	std::string error;
	if (!uam::StartAcpModelDiscovery(m_app, chat->id, &error))
	{
		m_app.provider_model_catalog->RememberRefreshFailure(chat->provider_id, FailureDetailOrFallback(error, "Provider model discovery failed to start."));
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(500, FailureDetailOrFallback(error, "Provider model discovery failed to start."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"started", true}, {"pending", true}}.dump());
}

void UamQueryHandler::HandleStageChatAttachments(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	(void)browser;
	const std::string chat_id = payload.value("chatId", "");
	if (chat_id.empty())
	{
		cb->Failure(400, "Missing chat id.");
		return;
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	const nlohmann::json* items = uam::nlohmann_json::FindArrayField(payload, attachment_frontend_fields::kItemsField);
	if (items == nullptr)
	{
		cb->Failure(400, "Attachment staging requires an items array.");
		return;
	}

	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(m_app, *chat);
	if (workspace_root.empty())
	{
		cb->Failure(400, "Chat has no workspace directory.");
		return;
	}

	std::error_code ec;
	if (!uam::paths::CreateDirectoriesNoThrow(workspace_root, &ec))
	{
		cb->Failure(500, "Failed to create workspace directory.");
		return;
	}

	const std::filesystem::path attachment_root = workspace_root / ".UAM" / "attachments" / chat_id;
	if (!uam::paths::CreateDirectoriesNoThrow(attachment_root, &ec))
	{
		cb->Failure(500, "Failed to create attachment directory.");
		return;
	}

	auto staged = nlohmann::json::array();
	std::size_t index = 0;
	for (const nlohmann::json& item : *items)
	{
		if (!item.is_object())
		{
			continue;
		}

		const std::string requested_kind = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kKindField});
		const std::string mime_type = uam::nlohmann_json::TrimmedStringValue(item, {attachment_frontend_fields::kMimeTypeInputField});
		const std::string source_path_text = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kPathField});
		const std::string attachment_kind = NormalizeStagedAttachmentKind(requested_kind);
		const bool is_directory = attachment_kind == attachment_frontend_fields::kDirectoryKind;
		MessageAttachment attachment;
		attachment.id = uam::nlohmann_json::TrimmedStringValue(item, {attachment_fields::kIdField});
		if (attachment.id.empty())
		{
			attachment.id = AttachmentId();
		}
		const std::string fallback_name = source_path_text.empty() ? "attachment" : std::filesystem::path(source_path_text).filename().string();
		attachment.name = SafeAttachmentName(uam::strings::TrimOrFallback(uam::nlohmann_json::StringViewOrEmpty(item, attachment_fields::kNameField), fallback_name), "attachment");
		attachment.kind = attachment_kind;
		attachment.mime_type = mime_type;

		if (is_directory)
		{
			if (source_path_text.empty())
			{
				cb->Failure(400, "Directory attachments require a filesystem path.");
				return;
			}
			const std::filesystem::path source_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(source_path_text);
			if (!uam::paths::IsDirectoryNoThrow(source_path))
			{
				cb->Failure(400, "Directory attachment does not exist: " + source_path_text);
				return;
			}
			attachment.path = PathForPrompt(workspace_root, source_path);
			attachment.copied = false;
			staged.push_back(AttachmentToJson(attachment));
			continue;
		}

		std::string bytes;
		if (const nlohmann::json* data_base64 = uam::nlohmann_json::FindStringField(item, attachment_frontend_fields::kDataBase64Field); data_base64 != nullptr)
		{
			if (!uam::base64::Decode(data_base64->get_ref<const std::string&>(), bytes))
			{
				cb->Failure(400, "Attachment data is not valid base64.");
				return;
			}
			if (bytes.size() > kMaxAttachmentBytes)
			{
				cb->Failure(413, "Attachment is larger than the 25 MB limit.");
				return;
			}
		}
		else if (!source_path_text.empty())
		{
			const std::filesystem::path source_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(source_path_text);
			if (!uam::paths::IsRegularFileNoThrow(source_path))
			{
				cb->Failure(400, "File attachment does not exist: " + source_path_text);
				return;
			}
			const std::optional<std::uintmax_t> source_size = uam::paths::FileSizeNoThrow(source_path);
			if (!source_size || *source_size > kMaxAttachmentBytes)
			{
				cb->Failure(413, "Attachment is larger than the 25 MB limit.");
				return;
			}
			if (!uam::io::TryReadBinaryFile(source_path, bytes))
			{
				cb->Failure(500, "Failed to read attachment: " + source_path_text);
				return;
			}
			if (bytes.size() > kMaxAttachmentBytes)
			{
				cb->Failure(413, "Attachment is larger than the 25 MB limit.");
				return;
			}
		}
		else
		{
			cb->Failure(400, "File attachments require data or a filesystem path.");
			return;
		}

		const std::string prefix = uam::time::SystemEpochMicrosecondsTokenNow() + "-" + std::to_string(index++);
		const std::filesystem::path target = attachment_root / (prefix + "-" + attachment.name);
		if (!uam::io::WriteBinaryFile(target, bytes))
		{
			cb->Failure(500, "Failed to write attachment.");
			return;
		}
		attachment.path = PathForPrompt(workspace_root, target);
		attachment.size_bytes = bytes.size();
		attachment.copied = true;
		staged.push_back(AttachmentToJson(attachment));
	}

	cb->Success(nlohmann::json{{"attachments", staged}}.dump());
}

void UamQueryHandler::HandleCancelAcpTurn(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");

	std::string error;
	if (!uam::CancelAcpTurn(m_app, chat_id, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to cancel ACP turn."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleResolveAcpPermission(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string request_id = payload.value("requestId", "");
	const std::string option_id = payload.value("optionId", "");
	const bool cancelled = payload.value("cancelled", false) || option_id == uam::acp_permissions::kCancelledOptionId;

	std::string error;
	if (!uam::ResolveAcpPermission(m_app, chat_id, request_id, option_id, cancelled, &error))
	{
		cb->Failure(409, FailureDetailOrFallback(error, "Failed to resolve ACP permission request."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleResolveAcpUserInput(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string request_id = payload.value("requestId", "");
	std::map<std::string, std::vector<std::string>> answers;

	const nlohmann::json* raw_answers = uam::nlohmann_json::FindObjectField(payload, "answers");
	if (raw_answers == nullptr)
	{
		cb->Failure(400, "ACP user input answers must be an object.");
		return;
	}

	for (auto it = raw_answers->begin(); it != raw_answers->end(); ++it)
	{
		if (it.key().empty())
		{
			continue;
		}

		answers[it.key()] = uam::nlohmann_json::StringListValue(it.value());
	}

	std::string error;
	if (!uam::ResolveAcpUserInput(m_app, chat_id, request_id, answers, &error))
	{
		cb->Failure(409, FailureDetailOrFallback(error, "Failed to resolve ACP user input request."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleStopAcpSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	uam::StopAcpSession(m_app, chat_id);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleWriteClipboardText(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string text = payload.value("text", "");
	if (text.empty())
	{
		cb->Failure(400, "Clipboard text is empty.");
		return;
	}
	if (text.size() > kMaxClipboardTextBytes)
	{
		cb->Failure(413, "Clipboard text is too large.");
		return;
	}

	std::string error;
	if (!WriteNativeClipboardText(text, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to write clipboard text."));
		return;
	}

	cb->Success(R"({"copied":true})");
}
