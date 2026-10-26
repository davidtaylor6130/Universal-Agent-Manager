#include "app/worktree_launch_preparation_service.h"
#include "uam_query_handler.h"
#include "uam_query_handler_async.h"
#include "uam_query_handler_internal.h"
#include "remote/runner_client.h"

#include "app/chat_domain_service.h"
#include "app/chat_search_service.h"
#include "app/chat_lifecycle_service.h"
#include "app/transcript_retention_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "app/runtime_orchestration_internal.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/chat/branch_native_assets.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_ids.h"
#include "common/chat/native_chat_identity.h"
#include "common/chat/native_media_chat_save.h"
#include "common/runtime/native_refresh_lifetime.h"
#include <type_traits>
#include <utility>
#include "common/config/execution_host_config.h"
#include "common/models/app_models.h"
#include "common/paths/path_utils.h"
#include "common/paths/app_paths.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/utils/diagnostic_log.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/terminal/terminal_provider_cli.h"
#include "common/state/app_state.h"
#include "common/platform/platform_services.h"
#include "common/platform/published_directory_confirmation.h"
#include "common/utils/time_utils.h"
#include "common/utils/shell_escape.h"
#include "common/utils/io_utils.h"
#include <chrono>
#include "common/runtime/app_time.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
	const std::string& CompanionBootId()
	{
		static const std::string id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		return id;
	}

	struct NativeChatTranscript
	{
		bool success = false;
		std::vector<Message> messages;
		std::string error;
	};

	struct BranchFromMessageRequest
	{
		std::string chat_id;
		int message_index = -1;
		std::optional<std::string> replacement_content;
		std::optional<std::string> operation_id;
		std::optional<std::vector<std::string>> retained_attachment_ids;
		bool select_chat = true;
		bool regenerate = true;
		std::optional<std::vector<std::string>> retained_markdown_store_files;
	};

	uam::query_handler_async::AsyncCefResult ExecuteBranchFromMessageRequest(uam::AppState& app, CefRefPtr<CefBrowser> browser, const BranchFromMessageRequest& request, const ChatSession* prepared_source = nullptr, const ChatSession* prepared_existing_branch = nullptr, uam::PreparedBranchOriginals* prepared_originals = nullptr)
	{
		std::string branch_id;
		std::string branch_error;
		std::string branch_warning;
		if (!uam::BranchFromMessageAndRetry(app, request.chat_id, request.message_index, request.replacement_content, &branch_id, &branch_error, request.operation_id, &branch_warning, request.retained_attachment_ids, prepared_source, prepared_existing_branch, prepared_originals, request.retained_markdown_store_files, request.select_chat, request.regenerate))
		{
			const bool branch_was_created = !branch_id.empty();
			if (branch_was_created && ChatDomainService().FindChatById(app, branch_id) != nullptr)
			{
				uam::PushStateUpdateIfChanged(browser, app);
			}
			if (branch_was_created && !branch_warning.empty())
				return uam::query_handler_async::AsyncSuccess(nlohmann::json{{"chatId", branch_id}, {"warning", branch_warning}});
			return uam::query_handler_async::AsyncFailure(branch_was_created ? 500 : 409,
			    uam::query_handler_internal::FailureDetailOrFallback(
			        branch_error,
			        branch_was_created ? "Branch regeneration could not start." : "Failed to create message branch."));
		}

		uam::PushStateUpdateIfChanged(browser, app);
		return uam::query_handler_async::AsyncSuccess(nlohmann::json{{"chatId", branch_id}});
	}

	std::string WorkspaceTitle(std::string_view directory)
	{
		std::string value(uam::strings::TrimAsciiView(directory));
		while (value.size() > 1 && (value.back() == '/' || value.back() == '\\')) value.pop_back();
		const std::size_t separator = value.find_last_of("/\\");
		return uam::strings::NonEmptyOrFallback(
		    separator == std::string::npos ? value : value.substr(separator + 1), "Workspace");
	}

	bool WorkspaceMatches(const ExecutionHost& host, std::string_view lhs, std::string_view rhs)
	{
		return host.id == uam::execution_hosts::kLocalHostId
		    ? FolderDirectoryMatches(uam::paths::PathFromUtf8(lhs), uam::paths::PathFromUtf8(rhs))
		    : uam::execution_hosts::RemotePathsMatch(host.platform, lhs, rhs);
	}

	nlohmann::json SerializeChatMessagesResult(
	    const ChatSession& chat, std::string_view known_digest,
	    const std::optional<std::pair<std::size_t, std::optional<std::size_t>>>& page,
	    bool defer_tool_call_content, bool reuse_selected_history = false)
	{
		if (page.has_value())
		{
			nlohmann::json result = uam::StateSerializer::SerializeMessagePage(
			    chat, page->first, page->second, defer_tool_call_content, known_digest, reuse_selected_history);
			result["chatId"] = chat.id;
			if (!result.contains("unchanged")) result["unchanged"] = false;
			return result;
		}
		if (!known_digest.empty() &&
		    known_digest == uam::StateSerializer::MessageDigest(chat, reuse_selected_history))
		{
			return {{"chatId", chat.id}, {"messagesDigest", known_digest},
			        {"unchanged", true}};
		}

		const nlohmann::json serialized = uam::StateSerializer::SerializeSession(chat, reuse_selected_history);
		nlohmann::json result{{"chatId", chat.id},
		                      {"messagesDigest", serialized.value("messagesDigest", "")},
		                      {"unchanged", false}};
		const nlohmann::json* messages =
		    uam::nlohmann_json::FindArrayField(serialized, "messages");
		result["messages"] = messages == nullptr ? nlohmann::json::array() : *messages;
		return result;
	}

	uam::query_handler_async::AsyncCefResult SerializeGuardedChatMessagesResult(
	    const ChatSession& chat, std::string_view known_digest,
	    const std::optional<std::pair<std::size_t, std::optional<std::size_t>>>& page,
	    bool defer_tool_call_content,
	    const std::optional<std::pair<std::string, std::size_t>>& search_target, bool reuse_selected_history = false)
	{
		if (search_target && !uam::ChatSearchTargetMatches(chat, search_target->first, search_target->second, reuse_selected_history))
			return uam::query_handler_async::AsyncFailure(409, "Chat history changed. Search again to select this message.");
		if (search_target && page)
		{
			const std::size_t end = std::min(page->second.value_or(chat.messages.size()), chat.messages.size());
			const std::size_t begin = end > page->first ? end - page->first : 0;
			if (search_target->second < begin || search_target->second >= end)
				return uam::query_handler_async::AsyncFailure(400, "The requested page does not contain the selected search message.");
		}
		return uam::query_handler_async::AsyncSuccess(SerializeChatMessagesResult(chat, known_digest, page, defer_tool_call_content, reuse_selected_history));
	}

	bool ReadMessageIndex(const nlohmann::json& value, std::size_t& result)
	{
		if (!value.is_number_integer() && !value.is_number_unsigned()) return false;
		if (value.is_number_unsigned())
		{
			const std::uint64_t index = value.get<std::uint64_t>();
			if (index > std::numeric_limits<std::size_t>::max()) return false;
			result = static_cast<std::size_t>(index);
			return true;
		}
		const std::int64_t index = value.get<std::int64_t>();
		if (index < 0 || static_cast<std::uint64_t>(index) > std::numeric_limits<std::size_t>::max()) return false;
		result = static_cast<std::size_t>(index);
		return true;
	}
}

// ---------------------------------------------------------------------------
// Chat lifecycle handlers (session CRUD + select)
// Pure chat domain — no cross-dependencies with other handler groups
// ---------------------------------------------------------------------------

void UamQueryHandler::HandleGetInitialState(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string known_boot_id = payload.value("knownBootId", "");
	const std::uint64_t known_revision = payload.value("knownStateRevision", std::numeric_limits<std::uint64_t>::max());
	if (payload.value("summaryOnly", true) && known_boot_id == CompanionBootId() && known_revision == m_app.state_revision)
	{
		cb->Success(nlohmann::json{{"unchanged", true}, {"bootId", CompanionBootId()}, {"stateRevision", m_app.state_revision}}.dump());
		return;
	}
	nlohmann::json state = uam::StateSerializer::Serialize(m_app, payload.value("summaryOnly", true));
	state["bootId"] = CompanionBootId();
	std::string body;
	try
	{
		body = state.dump();
	}
	catch (const nlohmann::json::type_error&)
	{
		// Clipped native text must not stop the UI or phone companion loading. Log once per run: companion reloads repeat it.
		static bool logged = false;
		if (!std::exchange(logged, true)) uam::diagnostics::Write("[getInitialState] State contained invalid UTF-8; replaced it with U+FFFD.");
		body = state.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
	}
	cb->Success(body);
}

// Elapsed time stays outside state fingerprints so a phone timer does not
// invalidate the desktop catalogue or publish a fresh global state every tick.
void UamQueryHandler::HandleGetChatRuntimeMetrics(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	if (!uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Chat no longer exists.")) return;
	const uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id);
	nlohmann::json result{{"chatId", chat_id}, {"processing", session && session->processing}, {"elapsedMs", nullptr}, {"turnSerial", session ? session->turn_serial : 0}};
	if (session && session->processing && session->turn_started_time_s > 0.0)
	{
		result["elapsedMs"] = static_cast<std::int64_t>(std::max(0.0, uam::GetAppTimeSeconds() - session->turn_started_time_s) * 1000.0);
	}
	cb->Success(result.dump());
}

void UamQueryHandler::HandleSelectSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string previous_selected_chat_id = ChatDomainService().SelectedChatId(m_app);
	if (chat_id.empty())
	{
		ChatDomainService().SelectChatById(m_app, "");
		if (!PersistenceCoordinator().SaveSettings(m_app))
		{
			ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
			cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Failed to clear selected chat."));
			return;
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}
	ChatSession* target_chat = uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Selected chat no longer exists.");
	if (target_chat == nullptr)
	{
		return;
	}

	const std::string previous_last_opened_at = target_chat->last_opened_at;
	ChatDomainService().SelectChatById(m_app, chat_id);

	ChatSession* selected_chat = ChatDomainService().SelectedChat(m_app);
	if (selected_chat == nullptr)
	{
		cb->Failure(404, "Selected chat no longer exists.");
		return;
	}

	selected_chat->last_opened_at = uam::time::TimestampNow();
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		selected_chat->last_opened_at = previous_last_opened_at;
		ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
		return;
	}

	if (!m_app.pending_chat_save_at_by_chat_id.contains(chat_id))
	{
		ChatSession metadata;
		metadata.id = selected_chat->id;
		metadata.last_opened_at = selected_chat->last_opened_at;
		metadata.interaction_at = selected_chat->interaction_at;
		metadata.attention_revision = selected_chat->attention_revision;
		metadata.settled_at = selected_chat->settled_at;
		metadata.snoozed_until_ms = selected_chat->snoozed_until_ms;
		const std::filesystem::path data_root = m_app.data_root;
		std::shared_ptr<std::shared_ptr<PreparedChatSave>> prepared = std::make_shared<std::shared_ptr<PreparedChatSave>>();
		if (!uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, nullptr,
		    [data_root, metadata, prepared]()
		    {
			    *prepared = ChatRepository::PrepareLastOpenedAt(data_root, metadata);
			    return uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
		    },
		    [this, data_root, metadata, prepared](uam::query_handler_async::AsyncCefResult&)
		    {
			    ChatSession* current = ChatDomainService().FindChatById(m_app, metadata.id);
			    if (current == nullptr || m_app.data_root != data_root || current->last_opened_at != metadata.last_opened_at ||
			        current->interaction_at != metadata.interaction_at || current->attention_revision != metadata.attention_revision || current->settled_at != metadata.settled_at || current->snoozed_until_ms != metadata.snoozed_until_ms) return;
			    if (m_app.pending_chat_save_at_by_chat_id.contains(metadata.id)) return;
			    if (m_app.worktree_operation_chat_ids.contains(metadata.id) || !ChatRepository::PublishPreparedChatSave(*prepared, true, current))
				    m_app.pending_chat_save_at_by_chat_id[metadata.id] = uam::GetAppTimeSeconds() + 1.0;
		    }, TID_FILE_USER_BLOCKING)) m_app.pending_chat_save_at_by_chat_id[chat_id] = uam::GetAppTimeSeconds() + 1.0;
	}
	m_app.status_line.clear();

	ChatDomainService().SortChatsByRecent(m_app.chats);
	ChatDomainService().SelectChatById(m_app, chat_id);
	cb->Success("{}");
	uam::PushStateUpdateIfChanged(browser, m_app);
}

void UamQueryHandler::HandleSetVisibleChatIds(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!payload.contains("chatIds") || !payload["chatIds"].is_array() || payload["chatIds"].size() > 64)
	{ cb->Failure(400, "A bounded list of visible chat IDs is required."); return; }
	std::vector<std::string> ids;
	for (const nlohmann::json& id : payload["chatIds"])
	{
		if (!id.is_string() || id.get_ref<const std::string&>().size() > 256) { cb->Failure(400, "Visible chat IDs must be bounded strings."); return; }
		ids.push_back(id.get<std::string>());
	}
	m_transcriptRetention.SetVisibleChatIds(ids);
	cb->Success("{}");
}

void UamQueryHandler::HandleReleaseChatMessages(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string id = payload.value("chatId", "");
	const std::optional<std::uint64_t> generation = m_transcriptRetention.BeginRelease(m_app, id);
	if (!generation) { cb->Success("{}"); return; }
	const auto data_root = m_app.data_root;
	auto persisted = std::make_shared<std::optional<ChatSession>>();
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb,
	    [data_root, id, persisted]()
	    {
		    *persisted = ChatRepository::LoadLocalChat(data_root, id, true, nullptr, false);
		    return uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
	    },
	    [this, id, generation, persisted, data_root](uam::query_handler_async::AsyncCefResult&)
	    {
		    const auto released = std::make_shared<std::vector<Message>>();
		    if (m_app.data_root == data_root && persisted->has_value())
			    m_transcriptRetention.CompleteRelease(m_app, id, *generation, **persisted, released.get());
		    // Dispose both large vectors on the managed worker, including rejected releases.
		    uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, nullptr, [released, persisted]()
		    {
			    released->clear();
			    persisted->reset();
			    return uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
		    });
	    });
}

void UamQueryHandler::HandleGetChatMessages(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	m_transcriptRetention.Invalidate(chat_id);
	const std::string known_digest = payload.value("messagesDigest", "");
	const bool defer_tool_call_content = payload.value("deferToolCallContent", false);
	const bool refresh_native = payload.value("refreshNative", false);
	std::optional<std::pair<std::size_t, std::optional<std::size_t>>> page;
	if (payload.contains("limit") || payload.contains("before"))
	{
		if (!payload.contains("limit"))
		{
			cb->Failure(400, "A message limit is required when requesting an older page.");
			return;
		}
		std::size_t limit = 0;
		if (!ReadMessageIndex(payload.at("limit"), limit) || limit == 0 || limit > 200)
		{
			cb->Failure(400, "Message limit must be between 1 and 200.");
			return;
		}
		std::optional<std::size_t> before;
		if (payload.contains("before"))
		{
			std::size_t index = 0;
			if (!ReadMessageIndex(payload.at("before"), index))
			{
				cb->Failure(400, "Message cursor must be a non-negative integer.");
				return;
			}
			before = index;
		}
		page = std::make_pair(limit, before);
	}
	std::optional<std::pair<std::string, std::size_t>> search_target;
	if (payload.contains("expectedMessagesDigest") || payload.contains("targetMessageIndex"))
	{
		std::size_t target_index = 0;
		if (!page || refresh_native || !payload.contains("expectedMessagesDigest") || !payload["expectedMessagesDigest"].is_string() ||
		    payload["expectedMessagesDigest"].get_ref<const std::string&>().empty() || payload["expectedMessagesDigest"].get_ref<const std::string&>().size() > 256 ||
		    !payload.contains("targetMessageIndex") || !ReadMessageIndex(payload["targetMessageIndex"], target_index))
		{
			cb->Failure(400, "Search navigation requires a bounded page, transcript digest and message index.");
			return;
		}
		search_target = std::make_pair(payload["expectedMessagesDigest"].get<std::string>(), target_index);
	}
	ChatSession* chat = uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}
	if (!refresh_native && HandleReadUnconfirmedNativeMediaHistory(chat_id, known_digest, page, defer_tool_call_content, search_target, cb)) return;
	if (!chat->messages_loaded && chat->persisted_message_count > 0 && !refresh_native)
	{
		const auto data_root = m_app.data_root;
		const std::size_t expected_count = chat->persisted_message_count;
		const std::string expected_digest = chat->persisted_messages_digest;
		const ChatSession expected = *chat;
		const uam::AcpSessionState* runtime = uam::FindAcpSessionForChat(m_app, chat_id);
		const bool recover_direct_receipt = !chat->imported_read_only && chat->acp_queue_held &&
		    chat->acp_dispatched_queued_prompt_count == 0 && chat->acp_queued_prompts.size() == 1 &&
		    !chat->acp_queued_prompts.front().prepared_for_delivery &&
		    (chat->acp_queued_prompts.front().direct_admission || chat->acp_queued_prompts.front().existing_user_message_index >= 0) &&
		    !chat->remote_turn_reconnect_pending && !chat->remote_restart_pending && !chat->remote_stop_cleanup_pending &&
		    chat->remote_active_turn_id.empty() && chat->remote_prompt_delivery_id.empty() && chat->remote_pending_requests.empty() &&
		    (runtime == nullptr || (!runtime->processing && runtime->prompt_request_id == 0 && runtime->queued_prompt.empty()));
		const bool recover_cold_unsent = recover_direct_receipt || (!chat->imported_read_only && chat->native_session_id.empty() &&
		    (runtime == nullptr || (!runtime->running && !runtime->processing)));
		const std::weak_ptr<void> lifetime = m_asyncLifetime;
		auto indexed = std::make_shared<std::optional<ChatMessagePage>>();
		auto loaded = std::make_shared<std::optional<ChatSession>>();
		auto tail = std::make_shared<std::shared_ptr<PreparedChatSave>>();
		auto cached_revision = std::make_shared<std::optional<std::string>>();
		auto unsent_revision = std::make_shared<std::optional<std::string>>();
		auto hydrate_unsent = std::make_shared<bool>(false);
		// Opening history must not queue behind background VCS subprocess checks.
		uam::query_handler_async::RunAsyncCefQuery(
		    m_asyncLifetime, cb,
		    [data_root, chat_id, known_digest, page, defer_tool_call_content, search_target, loaded, tail, cached_revision, expected_count, expected_digest, expected, indexed, lifetime, recover_cold_unsent, recover_direct_receipt, hydrate_unsent, unsent_revision]()
		    {
			    *hydrate_unsent = recover_direct_receipt;
			    if (!*hydrate_unsent && page && !page->second && page->first <= 50 && defer_tool_call_content && !search_target)
			    {
				    *cached_revision = ChatRepository::ChatStorageRevision(data_root, chat_id);
				    const std::optional<nlohmann::json> cached = uam::MessageTailCache::Read(data_root, chat_id, expected_count, expected_digest, page->first,
				        recover_cold_unsent ? std::string_view{} : std::string_view(known_digest), &expected);
				    if (cached && *cached_revision == ChatRepository::ChatStorageRevision(data_root, chat_id))
				    {
					    const nlohmann::json* rows = uam::nlohmann_json::FindArrayField(*cached, "messages");
					    *hydrate_unsent = recover_cold_unsent && rows && std::ranges::any_of(*rows, [](const nlohmann::json& row)
					    {
						    const auto flag = row.find("acpPromptNotSent");
						    return flag != row.end() && flag->is_boolean() && flag->get<bool>();
					    });
					    if (!*hydrate_unsent)
					    {
						    nlohmann::json body = *cached;
						    if (!known_digest.empty() && body.value("messagesDigest", "") == known_digest)
						    {
							    body["unchanged"] = true;
							    body.erase("messages");
						    }
						    return uam::query_handler_async::AsyncSuccess(std::move(body));
					    }
				    }
				    cached_revision->reset();
			    }
			    std::string warning;
			    if (page && !*hydrate_unsent)
			    {
				    *indexed = ChatRepository::ReadMessagePage(data_root, expected, page->first, page->second, &warning, [lifetime]() { return lifetime.expired(); });
				    if (!*indexed) return uam::query_handler_async::AsyncFailure(500,
				        uam::query_handler_internal::FailureDetailOrFallback(warning, "Failed to load chat messages."));
				    if (search_target && (search_target->first != (*indexed)->messages_digest ||
				        search_target->second >= (*indexed)->total_count))
					    return uam::query_handler_async::AsyncFailure(409, "Chat history changed. Search again to select this message.");
				    if (search_target && (search_target->second < (*indexed)->start_index ||
				        search_target->second >= (*indexed)->start_index + (*indexed)->chat.messages.size()))
					    return uam::query_handler_async::AsyncFailure(400, "The requested page does not contain the selected search message.");
				    *hydrate_unsent = recover_cold_unsent && std::ranges::any_of((*indexed)->chat.messages,
				        [](const Message& row) { return row.acp_prompt_not_sent; });
				    if (!*hydrate_unsent)
					    return uam::query_handler_async::AsyncSuccess(uam::StateSerializer::SerializeDetachedMessagePage(
					        **indexed, defer_tool_call_content, known_digest, !page->second));
				    indexed->reset();
			    }
			    // Failed first-turn drafts need full native certainty; ordinary cached pages remain detached.
			    if (*hydrate_unsent) *unsent_revision = ChatRepository::ChatStorageRevision(data_root, chat_id);
			    *loaded = recover_direct_receipt
			        ? ChatRepository::LoadLocalChatBounded(data_root, chat_id, 8 * 1024 * 1024)
			        : ChatRepository::LoadLocalChat(data_root, chat_id, true, &warning);
			    if (!*loaded && recover_direct_receipt && page)
			    {
				    *hydrate_unsent = false;
				    *indexed = ChatRepository::ReadMessagePage(data_root, expected, page->first, page->second, &warning, [lifetime]() { return lifetime.expired(); });
				    if (*indexed) return uam::query_handler_async::AsyncSuccess(uam::StateSerializer::SerializeDetachedMessagePage(
				        **indexed, defer_tool_call_content, known_digest, !page->second));
			    }
			    if (!loaded->has_value())
				    return uam::query_handler_async::AsyncFailure(500,
				        uam::query_handler_internal::FailureDetailOrFallback(warning, "Failed to load chat messages."));
			    if (*hydrate_unsent && *unsent_revision != ChatRepository::ChatStorageRevision(data_root, chat_id))
				    return uam::query_handler_async::AsyncFailure(409, "Chat history changed while loading. Retry to refresh it.");
			    *tail = uam::MessageTailCache::Prepare(data_root, **loaded);
			    return SerializeGuardedChatMessagesResult(**loaded, known_digest, page, defer_tool_call_content, search_target);
		    },
		    [this, browser, chat_id, known_digest, page, defer_tool_call_content, search_target, expected_count,
		     expected_digest, loaded, tail, cached_revision, data_root, indexed, expected, hydrate_unsent, unsent_revision, recover_cold_unsent](uam::query_handler_async::AsyncCefResult& response)
		    {
			    bool publish_tail = false;
			    const std::function<void()> complete = [&]()
			    {
				    if (!response.ok) return;
				    ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
				    if (current == nullptr || m_app.data_root != data_root)
				    {
					    response = uam::query_handler_async::AsyncFailure(404, "Chat was removed while its history was loading.");
					    return;
				    }
				    if (current->messages_loaded)
				    {
					    response = SerializeGuardedChatMessagesResult(*current, known_digest, page, defer_tool_call_content, search_target, ChatDomainService().SelectedChatId(m_app) == current->id);
					    return;
				    }
				    if (*indexed)
				    {
					    if (!ChatRepository::MatchesMessagePage(*current, **indexed) ||
					        m_app.pending_chat_save_at_by_chat_id.contains(chat_id) || m_app.worktree_operation_chat_ids.contains(chat_id) ||
					        ChatRepository::ChatStorageRevision(data_root, chat_id) != (*indexed)->storage_revision)
						    response = uam::query_handler_async::AsyncFailure(409, "Chat history changed while loading. Retry to refresh it.");
					    return;
				    }
				    if (cached_revision->has_value())
				    {
					    ChatMessagePage identity;
					    identity.chat = expected; identity.total_count = expected_count;
					    if (!ChatRepository::MatchesMessagePage(*current, identity) ||
					        m_app.pending_chat_save_at_by_chat_id.contains(chat_id) || m_app.worktree_operation_chat_ids.contains(chat_id) ||
					        ChatRepository::ChatStorageRevision(data_root, chat_id) != *cached_revision)
						    response = uam::query_handler_async::AsyncFailure(409, "Chat history changed while loading. Retry to refresh it.");
					    return;
				    }
				    if (*hydrate_unsent && (m_app.pending_chat_save_at_by_chat_id.contains(chat_id) ||
				        m_app.worktree_operation_chat_ids.contains(chat_id) ||
				        *unsent_revision != ChatRepository::ChatStorageRevision(data_root, chat_id)))
				    {
					    response = uam::query_handler_async::AsyncFailure(409, "Chat history changed while loading. Retry to refresh it.");
					    return;
				    }
				    if (!uam::TranscriptRetentionService::CompleteHistoryLoad(
				            *current, std::move(**loaded), expected_count, expected_digest, !page.has_value() || *hydrate_unsent))
					    response = uam::query_handler_async::AsyncFailure(409, "Chat history changed while loading. Retry to refresh it.");
				    else if (uam::SettleStoppedDirectAcpAdmissions(m_app, *current))
				    {
					    // Settlement changes storage ownership; never publish the old prepared tail.
					    response = SerializeGuardedChatMessagesResult(*current, known_digest, page, defer_tool_call_content, search_target);
					    uam::PushStateUpdateIfChanged(browser, m_app);
				    }
				    else if (!m_app.pending_chat_save_at_by_chat_id.contains(chat_id) && !m_app.worktree_operation_chat_ids.contains(chat_id))
					    publish_tail = true;
			    };
			    const std::function<void()> dispose = [&]()
			    {
				    if (!loaded->has_value() && !*tail && !*indexed) return;
				    // Publish revision-guarded cache files and dispose large buffers off the GUI thread.
				    uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, nullptr, [loaded, tail, publish_tail, indexed]()
				    {
					    if (publish_tail) (void)ChatRepository::PublishPreparedChatSave(*tail);
					    tail->reset();
					    loaded->reset();
					    indexed->reset();
					    return uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
				    });
			    };
			    try
			    {
				    complete();
				    const ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
				    if (response.ok && recover_cold_unsent && current && current->messages_loaded)
				    {
					    nlohmann::json body = nlohmann::json::parse(response.body);
					    body["unsentMessageIndices"] = uam::KnownUnsentAcpMessageIndices(m_app, *current);
					    body["recoverableUnsentMessageIndices"] = uam::RecoverableUnsentAcpMessageIndices(m_app, *current);
					    body["totalCount"] = current->messages.size();
					    response.body = body.dump();
				    }
			    }
			    catch (...) { dispose(); throw; }
			    dispose();
		    }, TID_FILE_USER_BLOCKING);
		return;
	}

	std::string hydrate_warning;
	if (!ChatRepository::HydrateChatMessages(m_app.data_root, *chat, &hydrate_warning))
	{
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(hydrate_warning, "Failed to load chat messages."));
		return;
	}

	if (uam::SettleStoppedDirectAcpAdmissions(m_app, *chat)) uam::PushStateUpdateIfChanged(browser, m_app);
	const std::string execution_host_id = uam::strings::NonEmptyOrFallback(
	    uam::strings::Trim(chat->execution_host_id), uam::execution_hosts::kLocalHostId);
	const std::string provider_id =
	    uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat->provider_id);
	const bool hydrate_native_chat =
	    (refresh_native ||
	     (chat->messages.empty() && execution_host_id != uam::execution_hosts::kLocalHostId)) &&
	    !chat->imported_read_only &&
	    !uam::runtime_orch_impl::ChatNeedsTranscriptPreserved(m_app, *chat) &&
	    !uam::strings::IsBlank(chat->native_session_id) &&
	    (provider_id == uam::provider_ids::kOpenCodeCli ||
	     provider_id == uam::provider_ids::kCodexCli ||
	     provider_id == uam::provider_ids::kClaudeCli || provider_id == uam::provider_ids::kCopilotCli || provider_id == uam::provider_ids::kGeminiCli);
	if (!hydrate_native_chat)
	{
		const uam::query_handler_async::AsyncCefResult result = SerializeGuardedChatMessagesResult(*chat, known_digest, page, defer_tool_call_content, search_target, ChatDomainService().SelectedChatId(m_app) == chat->id);
		if (result.ok) cb->Success(result.body);
		else cb->Failure(result.status, result.error);
		return;
	}

	std::optional<ExecutionHost> host_snapshot;
	if (execution_host_id != uam::execution_hosts::kLocalHostId)
	{
		const ExecutionHost* host = uam::execution_hosts::Find(
		    m_app.settings.execution_hosts, execution_host_id);
		if (host == nullptr || !uam::remote::CanAttemptRunnerConnection(*host))
		{
			cb->Failure(409, "The chat's remote helper is not ready.");
			return;
		}
		host_snapshot = *host;
	}

	if (provider_id == uam::provider_ids::kGeminiCli && !host_snapshot)
	{
		HandleRefreshLocalGeminiMedia(browser, *chat, known_digest, page, defer_tool_call_content, cb);
		return;
	}

	if (provider_id == uam::provider_ids::kGeminiCli && host_snapshot)
	{
		HandleRefreshRemoteGeminiMedia(browser, *chat, *host_snapshot, known_digest, page, defer_tool_call_content, cb);
		return;
	}

	const ProviderProfile profile_snapshot = ProviderResolutionService().ProviderForChatOrDefault(m_app, *chat);
	ChatSession chat_snapshot;
	chat_snapshot.id = chat->id;
	chat_snapshot.provider_id = chat->provider_id;
	chat_snapshot.native_session_id = chat->native_session_id;
	chat_snapshot.workspace_directory = chat->workspace_directory;
	chat_snapshot.execution_host_id = chat->execution_host_id;
	chat_snapshot.folder_id = chat->folder_id;
	chat_snapshot.created_at = chat->created_at;
	chat_snapshot.metadata_incarnation = chat->metadata_incarnation;
	chat_snapshot.managed_parent_input_receipts = chat->managed_parent_input_receipts;
	chat_snapshot.managed_parent_input_receipts_present = chat->managed_parent_input_receipts_present;
	const auto history_data_root = m_app.data_root;
	const bool bind_parent_input_receipts = provider_id == uam::provider_ids::kCodexCli;
	const auto history_primary_revision = bind_parent_input_receipts
	    ? ChatRepository::ChatStorageRevision(history_data_root, chat->id) : std::optional<std::string>{};
	if (bind_parent_input_receipts && (!history_primary_revision || *history_primary_revision == "missing"))
	{
		cb->Failure(409, "The saved chat is not ready for a native history refresh.");
		return;
	}
	const std::string original_digest = uam::StateSerializer::MessageDigest(*chat, ChatDomainService().SelectedChatId(m_app) == chat->id);
	const auto previous_request = m_nativeHistoryRequests.find(chat_id);
	if (previous_request != m_nativeHistoryRequests.end()) previous_request->second->request_stop();
	const std::shared_ptr<std::stop_source> request = std::make_shared<std::stop_source>();
	m_nativeHistoryRequests[chat_id] = request;
	auto transcript = std::make_shared<NativeChatTranscript>();
	if (!DispatchNativeMediaRefresh(cb,
	    [chat_snapshot, host_snapshot, provider_id, transcript, request, profile_snapshot,
	     history_data_root, history_primary_revision, bind_parent_input_receipts]()
	    {
		    if (request->stop_requested())
			    return uam::query_handler_async::AsyncFailure(409, "A newer history refresh was requested.");
		    if (bind_parent_input_receipts)
		    {
			    // Only the captured canonical primary may supply history-classification DATA.
			    if (ChatRepository::ChatStorageRevision(history_data_root, chat_snapshot.id) != history_primary_revision)
				    return uam::query_handler_async::AsyncFailure(409, "Saved history changed while loading.");
			    const auto saved = ChatRepository::LoadLocalChat(history_data_root, chat_snapshot.id, true, nullptr, false);
			    if (!saved || saved->id != chat_snapshot.id || saved->provider_id != chat_snapshot.provider_id ||
			        saved->native_session_id != chat_snapshot.native_session_id || saved->execution_host_id != chat_snapshot.execution_host_id ||
			        saved->workspace_directory != chat_snapshot.workspace_directory || saved->created_at != chat_snapshot.created_at ||
			        saved->managed_parent_input_receipts != chat_snapshot.managed_parent_input_receipts ||
			        saved->managed_parent_input_receipts_present != chat_snapshot.managed_parent_input_receipts_present ||
			        ChatRepository::ChatStorageRevision(history_data_root, chat_snapshot.id) != history_primary_revision)
				    return uam::query_handler_async::AsyncFailure(409, "Saved input provenance changed while loading.");
		    }
		    if (!host_snapshot && provider_id == uam::provider_ids::kCodexCli)
		    {
			    std::optional<ChatSession> loaded = ChatHistorySyncService().LoadLocalCodexChildChat(
			        chat_snapshot, &transcript->error);
			    transcript->success = loaded.has_value();
			    if (loaded) transcript->messages = std::move(loaded->messages);
		    }
		    else if (provider_id == uam::provider_ids::kCodexCli)
		    {
			    auto loaded = ChatHistorySyncService().LoadRemoteCodexTranscript(
			        *host_snapshot, chat_snapshot, request->get_token());
			    transcript->success = loaded.success;
			    transcript->messages = std::move(loaded.messages);
			    transcript->error = std::move(loaded.error);
		    }
		    else if (provider_id == uam::provider_ids::kClaudeCli || provider_id == uam::provider_ids::kCopilotCli || provider_id == uam::provider_ids::kGeminiCli)
		    {
			    auto loaded = host_snapshot
			        ? ChatHistorySyncService().LoadRemoteNativeTranscript(*host_snapshot, chat_snapshot, &transcript->error, request->get_token())
			        : ChatHistorySyncService().LoadLocalNativeTranscript(chat_snapshot, &transcript->error, request->get_token());
			    transcript->success = loaded.has_value();
			    if (loaded) transcript->messages = std::move(loaded->messages);
		    }
		    else
		    {
			    auto loaded = host_snapshot
			        ? ChatHistorySyncService().LoadRemoteOpenCodeTranscript(*host_snapshot, chat_snapshot, profile_snapshot, request->get_token())
			        : ChatHistorySyncService().LoadLocalOpenCodeTranscript(chat_snapshot, profile_snapshot, request->get_token());
			    transcript->success = loaded.success;
			    transcript->messages = std::move(loaded.messages);
			    transcript->error = std::move(loaded.error);
		    }
		    return transcript->success
		        ? uam::query_handler_async::AsyncSuccess({{"ok", true}})
		        : uam::query_handler_async::AsyncFailure(
		              502, uam::strings::NonEmptyOrFallback(
		                       transcript->error, "Native chat history is not available yet."));
	    },
		    [this, browser, chat_snapshot, host_snapshot, known_digest, original_digest, request, transcript, page,
		     defer_tool_call_content, search_target, history_data_root, history_primary_revision, bind_parent_input_receipts](
	        uam::query_handler_async::AsyncCefResult& response)
	    {
		    const auto pending = m_nativeHistoryRequests.find(chat_snapshot.id);
		    if (pending == m_nativeHistoryRequests.end() || pending->second != request)
		    {
			    response = uam::query_handler_async::AsyncFailure(409, "A newer history refresh was requested.");
			    return;
		    }
		    m_nativeHistoryRequests.erase(pending);
		    ChatSession* current = ChatDomainService().FindChatById(m_app, chat_snapshot.id);
		    if (current == nullptr)
		    {
			    response = uam::query_handler_async::AsyncFailure(
			        404, "Chat was removed while its history was loading.");
			    return;
		    }
		    if (current->execution_host_id != chat_snapshot.execution_host_id ||
		        current->workspace_directory != chat_snapshot.workspace_directory ||
		        current->native_session_id != chat_snapshot.native_session_id ||
		        current->provider_id != chat_snapshot.provider_id || current->imported_read_only)
		    {
			    response = uam::query_handler_async::AsyncFailure(
			        409, "Chat identity changed while its history was loading.");
			    return;
		    }
		    if (host_snapshot)
		    {
			    const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, host_snapshot->id);
			    if (host == nullptr || host->transport != host_snapshot->transport ||
			        host->ssh_alias != host_snapshot->ssh_alias || host->platform != host_snapshot->platform ||
			        host->runner_directory != host_snapshot->runner_directory ||
			        host->runner_version != host_snapshot->runner_version ||
			        host->runner_protocol_version != host_snapshot->runner_protocol_version)
			    {
				    response = uam::query_handler_async::AsyncFailure(
				        409, "Remote host changed while its history was loading.");
				    return;
			    }
		    }
		    const auto input_snapshot_current = [&]
		    {
			    return !bind_parent_input_receipts || (!request->stop_requested() && m_app.data_root == history_data_root &&
			        ChatDomainService().FindChatById(m_app, chat_snapshot.id) == current &&
			        current->provider_id == chat_snapshot.provider_id && current->native_session_id == chat_snapshot.native_session_id &&
			        current->execution_host_id == chat_snapshot.execution_host_id && current->workspace_directory == chat_snapshot.workspace_directory &&
			        current->metadata_incarnation == chat_snapshot.metadata_incarnation && current->created_at == chat_snapshot.created_at &&
			        current->managed_parent_input_receipts == chat_snapshot.managed_parent_input_receipts &&
			        current->managed_parent_input_receipts_present == chat_snapshot.managed_parent_input_receipts_present &&
			        ChatRepository::ChatStorageRevision(history_data_root, chat_snapshot.id) == history_primary_revision);
		    };
		    if (!input_snapshot_current())
		    {
			    response = uam::query_handler_async::AsyncFailure(409, "Saved input provenance changed while loading.");
			    return;
		    }
		    // Runtime ownership may have started while the native export was running.
		    if (uam::runtime_orch_impl::ChatNeedsTranscriptPreserved(m_app, *current))
		    {
			    response = SerializeGuardedChatMessagesResult(*current, known_digest, page, defer_tool_call_content, search_target, ChatDomainService().SelectedChatId(m_app) == current->id);
			    return;
		    }
		    if (!response.ok) return;
		    std::string hydrate_error;
		    if (!ChatRepository::HydrateChatMessages(m_app.data_root, *current, &hydrate_error))
		    {
			    response = uam::query_handler_async::AsyncFailure(500,
			        uam::strings::NonEmptyOrFallback(hydrate_error, "Failed to reload the current chat history."));
			    return;
		    }
		    if (!input_snapshot_current())
		    {
			    response = uam::query_handler_async::AsyncFailure(409, "Saved input provenance changed while loading.");
			    return;
		    }
		    // A prompt or another import may have completed while the native export was running.
		    if (uam::StateSerializer::MessageDigest(*current, ChatDomainService().SelectedChatId(m_app) == current->id) == original_digest)
		    {
			    if (bind_parent_input_receipts)
			    {
				    ChatSession candidate = *current;
				    if (ChatHistorySyncService().BuildNativeTranscriptCandidate(candidate, std::move(transcript->messages)))
				    {
					    const auto prepared = ChatRepository::PrepareChatSave(history_data_root, candidate, *history_primary_revision);
					    const auto still_owned = [&]
					    {
						    return input_snapshot_current() && !current->imported_read_only &&
						        !uam::runtime_orch_impl::ChatNeedsTranscriptPreserved(m_app, *current) &&
						        uam::StateSerializer::MessageDigest(*current, ChatDomainService().SelectedChatId(m_app) == current->id) == original_digest;
					    };
					    if (!prepared || !ChatRepository::PublishPreparedChatSave(prepared, true, &candidate, still_owned))
					    {
						    response = uam::query_handler_async::AsyncFailure(500, "Failed to save the loaded chat history.");
						    return;
					    }
					    *current = std::move(candidate);
				    }
			    }
			    else if (!ChatHistorySyncService().SaveNativeTranscript(m_app, *current, std::move(transcript->messages)))
			    {
				    response = uam::query_handler_async::AsyncFailure(500, "Failed to save the loaded chat history.");
				    return;
			    }
			    uam::PushStateUpdateIfChanged(browser, m_app);
		    }
		    response = SerializeGuardedChatMessagesResult(*current, known_digest, page, defer_tool_call_content, search_target, ChatDomainService().SelectedChatId(m_app) == current->id);
	    }))
	{
		m_nativeHistoryRequests.erase(chat_id);
		cb->Failure(503, "The background task queue is unavailable.");
	}
}

bool UamQueryHandler::DispatchNativeMediaRefresh(CefRefPtr<Callback> cb, ManagedSubagentWorker worker, ManagedSubagentCompletion completion, CefThreadId thread_id)
{
	if (m_nativeMediaDispatchThreadObserverForTests) m_nativeMediaDispatchThreadObserverForTests(thread_id);
	if (m_nativeMediaRefreshDispatcherForTests)
		return m_nativeMediaRefreshDispatcherForTests(cb, std::move(worker), std::move(completion));
	return CefPostTask(thread_id, new uam::query_handler_async::CefQueryWorkerTask(
	    m_asyncLifetime, cb, std::move(worker), std::move(completion)));
}

void UamQueryHandler::HandleRefreshLocalGeminiMedia(CefRefPtr<CefBrowser> browser, const ChatSession& chat,
    const std::string& known_digest, const std::optional<std::pair<std::size_t, std::optional<std::size_t>>>& page,
    bool defer_tool_call_content, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	struct Work
	{
		std::vector<Message> messages;
		std::optional<ChatSession> candidate;
		std::optional<ChatSession> retired;
		std::unique_ptr<uam::PreparedNativeMediaChatSave> prepared;
		std::string error;
		bool skipped = false;
		~Work() noexcept
		{
			// This destructor runs on the armed cleanup worker, even when a CEF
			// task is discarded or its callback lifetime expires.
			try
			{
				if (prepared && prepared->PrimaryAcknowledged())
					(void)prepared->AcknowledgeOriginals(error);
			}
			catch (...) { /* Attempted originals remain retained on receipt failure. */ }
		}
	};
	struct RequestExit
	{
		std::function<void()> finish;
		bool deferred = false;
		~RequestExit() { if (!deferred) finish(); }
	};

	// Arm disposal and its bounded permit before stopping a prior request or
	// admitting any transcript/payload into an asynchronous task.
	auto work = uam::native_history::TryCreateWorkerOwnedNativeRefresh<Work>();
	if (!work) { cb->Failure(503, "Native history refresh is busy. Retry shortly."); return; }
	const auto root = m_app.data_root;
	const auto revision = ChatRepository::ChatStorageRevision(root, chat.id);
	const auto summary_revision = ChatRepository::ChatSummaryStorageRevision(root, chat.id);
	const auto metadata = ChatRepository::ChatMetadataFingerprint(root, chat);
	if (!root.is_absolute() || !revision || !summary_revision || metadata.empty())
	{ cb->Failure(409, "The current saved chat is not ready for a native refresh."); return; }
	const std::string id = chat.id;
	const auto incarnation = chat.metadata_incarnation;
	const auto birth = chat.created_at;
	const auto native_key = uam::chat_identity::NativeIdentityKeyForHistoryImport(chat);
	const auto resolved = m_app.resolved_native_sessions_by_chat_id.find(id);
	const std::optional<std::string> resolved_session = resolved == m_app.resolved_native_sessions_by_chat_id.end()
	    ? std::nullopt : std::optional<std::string>(resolved->second);
	const auto original_digest = uam::StateSerializer::MessageDigest(chat, false);
	const auto computer_state = std::make_tuple(chat.computer_use_globally_enabled, chat.computer_use_enabled,
	    chat.computer_use_target_kind, chat.computer_use_target_id, chat.computer_use_target_process_id,
	    chat.computer_use_target_title, chat.computer_use_target_input_mode);
	const bool computer_consent = m_app.settings.computer_use_globally_enabled;
	ChatSession parse_identity;
	parse_identity.id = id; parse_identity.provider_id = chat.provider_id;
	parse_identity.native_session_id = chat.native_session_id;
	parse_identity.workspace_directory = chat.workspace_directory;
	parse_identity.execution_host_id = chat.execution_host_id;
	parse_identity.folder_id = chat.folder_id;
	const auto request = std::make_shared<std::stop_source>();
	const auto prior = m_nativeHistoryRequests.find(id);
	if (prior != m_nativeHistoryRequests.end()) prior->second->request_stop();
	m_nativeHistoryRequests[id] = request;
	const auto finish = [this, id, request]()
	{
		const auto found = m_nativeHistoryRequests.find(id);
		if (found != m_nativeHistoryRequests.end() && found->second == request) m_nativeHistoryRequests.erase(found);
	};
	const auto admitted = [this, root, id, incarnation, birth, native_key, resolved_session, metadata,
	    revision, summary_revision, original_digest, computer_state, computer_consent, request]() -> ChatSession*
	{
		const auto pending = m_nativeHistoryRequests.find(id);
		ChatSession* current = ChatDomainService().FindChatById(m_app, id);
		if (request->stop_requested() || pending == m_nativeHistoryRequests.end() || pending->second != request ||
		    m_app.data_root != root || !current || current->metadata_incarnation != incarnation || current->created_at != birth ||
		    !current->messages_loaded || current->imported_read_only || m_workspaceDeletionPending || m_app.deletion_progress ||
		    m_app.pending_chat_save_at_by_chat_id.contains(id) || m_app.pending_chat_save_tasks.contains(id) ||
		    m_app.worktree_operation_chat_ids.contains(id) ||
		    uam::runtime_orch_impl::ChatNeedsTranscriptPreserved(m_app, *current) ||
		    uam::chat_identity::NativeIdentityKeyForHistoryImport(*current) != native_key ||
		    ChatRepository::ChatMetadataFingerprint(root, *current) != metadata ||
		    uam::StateSerializer::MessageDigest(*current, false) != original_digest ||
		    ChatRepository::ChatStorageRevision(root, id) != revision ||
		    ChatRepository::ChatSummaryStorageRevision(root, id) != summary_revision ||
		    m_app.settings.computer_use_globally_enabled != computer_consent ||
		    std::tie(current->computer_use_globally_enabled, current->computer_use_enabled,
		        current->computer_use_target_kind, current->computer_use_target_id, current->computer_use_target_process_id,
		        current->computer_use_target_title, current->computer_use_target_input_mode) != computer_state) return nullptr;
		const auto mapping = m_app.resolved_native_sessions_by_chat_id.find(id);
		const std::optional<std::string> now = mapping == m_app.resolved_native_sessions_by_chat_id.end()
		    ? std::nullopt : std::optional<std::string>(mapping->second);
		if (now != resolved_session) return nullptr;
		for (const auto& other : m_app.chats)
			if (other.id != id && uam::chat_identity::NativeIdentityKeyForHistoryImport(other) == native_key) return nullptr;
		return current;
	};
	if (!admitted()) { finish(); cb->Failure(409, "Chat ownership changed. Retry to refresh its history."); return; }
	bool queued = false;
	try
	{
		queued = DispatchNativeMediaRefresh(cb,
		    [work, parse_identity, request]()
		    {
			    auto loaded = ChatHistorySyncService().LoadLocalNativeTranscript(parse_identity, &work->error, request->get_token());
			    if (request->stop_requested()) return AsyncFailure(409, "A newer history refresh was requested.");
			    if (!loaded) return AsyncFailure(502, uam::strings::NonEmptyOrFallback(work->error, "Native chat history is not available yet."));
			    work->messages = std::move(loaded->messages);
			    return AsyncSuccess({{"ok", true}});
		    },
		    [this, browser, cb, work, root, revision, request, admitted, finish, known_digest, page, defer_tool_call_content](AsyncCefResult& response)
		    {
			    RequestExit exit{finish};
			    ChatSession* current = admitted();
			    if (!current) { response = AsyncFailure(409, "Chat changed while its native history was loading. Retry to refresh it."); return; }
			    if (!response.ok) return;
			    if (work->messages.size() < current->messages.size())
			    {
				    response = SerializeGuardedChatMessagesResult(*current, known_digest, page, defer_tool_call_content, {});
				    return;
			    }
			    // Preserve the complete admitted live metadata, including unsaved
			    // local details. Its retirement belongs to Work, not this closure.
			    work->candidate.emplace(*current);
			    const bool prepared_queued = DispatchNativeMediaRefresh(cb,
			        [work, root, revision, request]()
			        {
				        if (request->stop_requested()) return AsyncFailure(409, "A newer history refresh was requested.");
				        if (!ChatHistorySyncService().BuildNativeTranscriptCandidate(*work->candidate, std::move(work->messages)))
				        { work->skipped = true; return AsyncSuccess({{"ok", true}}); }
				        work->prepared = uam::PreparedNativeMediaChatSave::Prepare(root, std::move(*work->candidate), *revision, work->error, request->get_token());
				        work->candidate.reset();
				        return work->prepared ? AsyncSuccess({{"ok", true}})
				            : AsyncFailure(502, uam::strings::NonEmptyOrFallback(work->error, "Native history could not be prepared without changing saved content."));
			        },
			        [browser, work, admitted, finish, known_digest, page, defer_tool_call_content, this](AsyncCefResult& result)
			        {
				        RequestExit terminal{finish};
				        ChatSession* target = admitted();
				        if (!target) { result = AsyncFailure(409, "Chat changed while native media was being prepared. Retry to refresh it."); return; }
				        if (!result.ok) return;
				        if (!work->skipped)
				        {
					        if (!work->prepared || !work->prepared->Publish(work->error, [&] { return admitted() == target; }))
					        { result = AsyncFailure(500, "Native history publication was not acknowledged; saved content was preserved."); return; }
					        work->retired = work->prepared->TakeCommittedChat();
					        if (!work->retired) { result = AsyncFailure(500, "Native history was saved but could not be adopted. Reload this chat."); return; }
					        static_assert(std::is_nothrow_swappable_v<ChatSession>);
					        // This transient UI signal is independent of durable ownership metadata.
					        work->retired->managed_subagent_revision = target->managed_subagent_revision;
					        std::swap(*target, *work->retired);
					        uam::PushStateUpdateIfChanged(browser, m_app);
				        }
				        result = SerializeGuardedChatMessagesResult(*target, known_digest, page, defer_tool_call_content, {}, ChatDomainService().SelectedChatId(m_app) == target->id);
			        });
			    if (!prepared_queued) { response = AsyncFailure(503, "The background task queue is unavailable."); return; }
			    exit.deferred = true;
			    response.callback_deferred = true;
		    });
	}
	catch (...) { finish(); throw; }
	if (!queued) { finish(); cb->Failure(503, "The background task queue is unavailable."); }
}

namespace
{
	std::string RemoteNativeMediaMetadata(const std::filesystem::path& root, const ChatSession& chat)
	{
		auto metadata = nlohmann::json::parse(ChatRepository::ChatMetadataFingerprint(root, chat), nullptr, false);
		if (!metadata.is_object()) return {};
		if (chat.messages_loaded)
		{
			// Const metadata saves can leave only these derived live caches behind
			// their serialized values; actual message content has a separate witness.
			metadata["validationPersistedCount"] = chat.messages.size();
			metadata["validationPersistedDigest"] = chat.updated_at + ":" + std::to_string(chat.messages.size());
		}
		return metadata.dump();
	}

	std::string RemoteNativeMediaObservationMetadata(const std::filesystem::path& root, const ChatSession& chat)
	{
		auto metadata = nlohmann::json::parse(RemoteNativeMediaMetadata(root, chat), nullptr, false);
		if (!metadata.is_object()) return {};
		// Reopening changes recency; normal eviction changes only loaded/cache
		// fields. Neither grants publication authority. History is checked separately.
		for (const auto* field : {"last_opened_at", "validationMessagesLoaded", "validationPersistedCount", "validationPersistedDigest"}) metadata.erase(field);
		return metadata.dump();
	}

	bool SameRemoteNativeMediaProfile(const ProviderProfile& left, const ProviderProfile& right)
	{
		return std::tie(left.id, left.title, left.execution_mode, left.output_mode, left.interactive_command,
		    left.supports_cli, left.supports_structured, left.structured_protocol, left.supports_interactive,
		    left.supports_resume, left.runtime_flags, left.resume_argument, left.history_adapter,
		    left.prompt_bootstrap, left.prompt_bootstrap_path, left.user_message_types, left.assistant_message_types,
		    left.native_goal_command) ==
		    std::tie(right.id, right.title, right.execution_mode, right.output_mode, right.interactive_command,
		    right.supports_cli, right.supports_structured, right.structured_protocol, right.supports_interactive,
		    right.supports_resume, right.runtime_flags, right.resume_argument, right.history_adapter,
		    right.prompt_bootstrap, right.prompt_bootstrap_path, right.user_message_types, right.assistant_message_types,
		    right.native_goal_command);
	}
}

std::function<ChatSession*()> UamQueryHandler::CaptureUnconfirmedNativeMediaOwner(const std::string& chat_id)
{
	const auto pending = m_unconfirmedNativeMediaHistory.find(chat_id);
	if (pending == m_unconfirmedNativeMediaHistory.end()) return {};
	const auto* live = ChatDomainService().FindChatById(m_app, chat_id);
	if (!live || pending->second.data_root != m_app.data_root || pending->second.incarnation != live->metadata_incarnation)
	{ m_unconfirmedNativeMediaHistory.erase(pending); return {}; }
	const auto same_owner = pending->second.owner;
	const auto root = m_app.data_root;
	const auto observation = pending->second.observation;
	const auto metadata = RemoteNativeMediaMetadata(root, *live);
	const auto digest = uam::StateSerializer::MessageDigest(*live, false);
	// Rebind a fresh exact witness for this read, after validating the retained
	// ownership/history witness. Changes during a read still refuse its completion.
	return [this, same_owner, root, chat_id, observation, metadata, digest]() -> ChatSession*
	{
		const auto pending = m_unconfirmedNativeMediaHistory.find(chat_id);
		auto* current = same_owner();
		if (pending == m_unconfirmedNativeMediaHistory.end() || pending->second.observation != observation || !current ||
		    RemoteNativeMediaMetadata(root, *current) != metadata || uam::StateSerializer::MessageDigest(*current, false) != digest ||
		    m_workspaceDeletionPending || m_app.deletion_progress || m_nativeHistoryRequests.contains(chat_id) ||
		    m_app.pending_chat_save_at_by_chat_id.contains(chat_id) || m_app.pending_chat_save_tasks.contains(chat_id) ||
		    m_app.worktree_operation_chat_ids.contains(chat_id) || uam::runtime_orch_impl::ChatNeedsTranscriptPreserved(m_app, *current)) return nullptr;
		return current;
	};
}

bool UamQueryHandler::HandleReadUnconfirmedNativeMediaHistory(const std::string& chat_id, const std::string& known_digest,
    const std::optional<std::pair<std::size_t, std::optional<std::size_t>>>& page,
    bool defer_tool_call_content, const std::optional<std::pair<std::string, std::size_t>>& search_target, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	const auto admitted = CaptureUnconfirmedNativeMediaOwner(chat_id);
	if (!admitted) return false;
	if (!admitted()) { cb->Failure(409, "Chat is busy. Reopen its saved history when current work has finished."); return true; }
	const auto root = m_app.data_root;
	const auto lifetime = std::weak_ptr<void>(m_asyncLifetime);
	auto observed = uam::native_history::TryCreateWorkerOwnedNativeRefresh<std::optional<ChatMessagePage>>();
	if (!observed) { cb->Failure(503, "Saved history checking is busy. Retry shortly."); return true; }
	const auto bounds = page.value_or(std::make_pair(std::size_t{50}, std::optional<std::size_t>{}));
	const bool queued = DispatchNativeMediaRefresh(cb,
	    [root, chat_id, bounds, known_digest, defer_tool_call_content, search_target, observed, lifetime]()
	    {
		    std::string error;
		    *observed = ChatRepository::ReadSavedPrimaryMessagePage(root, chat_id, bounds.first, bounds.second, &error, [lifetime]() { return lifetime.expired(); });
		    if (!*observed) return AsyncFailure(500, "Saved chat history could not be checked. Reopen the chat before retrying.");
		    if (search_target && (search_target->first != (*observed)->messages_digest || search_target->second < (*observed)->start_index ||
		        search_target->second >= (*observed)->start_index + (*observed)->chat.messages.size()))
			    return AsyncFailure(409, "Chat history changed. Search again to select this message.");
		    // Even identical messages must carry their passive detail binding: the
		    // renderer may still hold the old live descriptors without that binding.
		    auto body = uam::StateSerializer::SerializeDetachedMessagePage(**observed, defer_tool_call_content, known_digest, false);
		    if (body.contains("messages"))
			    for (auto& message : body["messages"])
				    for (const auto* field : {"attachments", "toolCalls"})
					    if (message.contains(field)) for (auto& detail : message[field]) detail["historyDigest"] = (*observed)->messages_digest;
		    return AsyncSuccess(std::move(body));
	    },
	    [this, root, chat_id, admitted, observed](AsyncCefResult& response)
	    {
		    if (!response.ok) return;
		    auto* current = admitted();
		    if (!current || !*observed || m_app.data_root != root ||
		        !ChatRepository::MatchesSavedPrimaryObservation(*current, **observed) ||
		        ChatRepository::ChatStorageRevision(root, chat_id) != (*observed)->storage_revision)
			    response = AsyncFailure(409, "Chat history changed while checking its saved history. Reopen the chat before retrying.");
		    else
		    {
			    auto& pending = m_unconfirmedNativeMediaHistory.at(chat_id);
			    pending.displayed_revision = (*observed)->storage_revision;
			    pending.displayed_digest = (*observed)->messages_digest;
		    }
		    // Passive display only: no live adoption, primary save, original ACK, or warning mutation.
		    // Keep the marker so a subsequent ordinary read cannot return the old live graph.
	    }, TID_FILE_USER_BLOCKING);
	if (!queued) cb->Failure(503, "The background task queue is unavailable.");
	return true;
}

bool UamQueryHandler::HandleReadUnconfirmedNativeMediaDetail(const std::string& chat_id, const nlohmann::json& payload,
    std::optional<std::size_t> message_index, CefRefPtr<Callback> cb,
    std::function<uam::query_handler_async::AsyncCefResult(const ChatMessagePage&)> resolve)
{
	using namespace uam::query_handler_async;
	const auto admitted = CaptureUnconfirmedNativeMediaOwner(chat_id);
	if (!admitted && !payload.contains("historyDigest")) return false;
	const auto pending = m_unconfirmedNativeMediaHistory.find(chat_id);
	const auto digest = payload.find("historyDigest");
	if (!admitted || !admitted() || !message_index || *message_index == std::numeric_limits<std::size_t>::max() ||
	    pending == m_unconfirmedNativeMediaHistory.end() || digest == payload.end() || !digest->is_string() ||
	    digest->get_ref<const std::string&>().size() != 16 || *digest != pending->second.displayed_digest || pending->second.displayed_revision.empty())
	{ cb->Failure(409, "Saved history changed. Reload history before opening its details."); return true; }
	const auto root = m_app.data_root;
	const auto revision = pending->second.displayed_revision, expected_digest = pending->second.displayed_digest;
	const auto lifetime = std::weak_ptr<void>(m_asyncLifetime);
	auto observed = uam::native_history::TryCreateWorkerOwnedNativeRefresh<std::optional<ChatMessagePage>>();
	if (!observed) { cb->Failure(503, "Saved history checking is busy. Retry shortly."); return true; }
	const bool queued = DispatchNativeMediaRefresh(cb,
	    [root, chat_id, index = *message_index, revision, expected_digest, observed, lifetime, resolve = std::move(resolve)]()
	    {
		    if (ChatRepository::ChatStorageRevision(root, chat_id) != revision) return AsyncFailure(409, "Saved history changed. Reload history before opening its details.");
		    *observed = ChatRepository::ReadSavedPrimaryMessagePage(root, chat_id, 1, index + 1, nullptr, [lifetime]() { return lifetime.expired(); });
		    if (!*observed || (*observed)->storage_revision != revision || (*observed)->messages_digest != expected_digest ||
		        (*observed)->start_index != index || (*observed)->chat.messages.size() != 1)
			    return AsyncFailure(409, "Saved history changed. Reload history before opening its details.");
		    return resolve(**observed);
	    },
	    [this, root, chat_id, revision, expected_digest, admitted, observed](AsyncCefResult& response)
	    {
		    if (!response.ok) return;
		    const auto* current = admitted();
		    const auto pending = m_unconfirmedNativeMediaHistory.find(chat_id);
		    if (!current || !*observed || pending == m_unconfirmedNativeMediaHistory.end() ||
		        pending->second.displayed_digest != expected_digest || pending->second.displayed_revision != revision ||
		        !ChatRepository::MatchesSavedPrimaryObservation(*current, **observed) || ChatRepository::ChatStorageRevision(root, chat_id) != revision)
			    response = AsyncFailure(409, "Saved history changed while its details were loading. Reload history before retrying.");
	    }, TID_FILE_USER_BLOCKING);
	if (!queued) cb->Failure(503, "The background task queue is unavailable.");
	return true;
}

void UamQueryHandler::HandleRefreshRemoteGeminiMedia(CefRefPtr<CefBrowser> browser, const ChatSession& chat,
    const ExecutionHost& host, const std::string& known_digest,
    const std::optional<std::pair<std::size_t, std::optional<std::size_t>>>& page,
    bool defer_tool_call_content, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	struct Work
	{
		std::filesystem::path root;
		std::vector<Message> messages;
		std::optional<ChatSession> candidate, retired;
		std::optional<std::tuple<std::string, std::string, int>> saved_lineage;
		std::unique_ptr<uam::PreparedNativeMediaChatSave> prepared;
		std::string error, ack_error;
		bool acknowledgement_finished = false;
		void Acknowledge() noexcept
		{
			if (!prepared || !prepared->PrimaryAcknowledged() || acknowledgement_finished) return;
			try
			{
				if (!prepared->AcknowledgeOriginals(ack_error) && ack_error.empty())
					ack_error = "Original acknowledgement was not confirmed.";
				std::string directory_error;
				const auto confirmation = uam::platform::ConfirmPublishedDirectories(
				    AppPaths::UamChatsRootPath(root), AppPaths::UamChatSummariesRootPath(root), root, directory_error);
				if (confirmation == uam::platform::PublishedDirectoryConfirmation::Failed && ack_error.empty())
					ack_error = "Published history directory durability was not confirmed.";
			}
			catch (...) { ack_error = "Published original acknowledgement was not confirmed."; }
			acknowledgement_finished = true;
		}
		~Work() noexcept { Acknowledge(); } // Armed worker retirement; no app or CEF access.
	};
	struct RequestExit
	{
		std::function<void()> finish;
		bool deferred = false;
		~RequestExit() { if (!deferred) finish(); }
	};

	// Reserve before stopping another request or retaining a transcript/payload.
	auto work = uam::native_history::TryCreateWorkerOwnedNativeRefresh<Work, uam::native_history::NativeRefreshWorkerPool::RemoteRefresh>();
	if (!work) { cb->Failure(503, "Native history refresh is busy. Retry shortly."); return; }
	const auto root = m_app.data_root;
	const auto revision = ChatRepository::ChatStorageRevision(root, chat.id);
	const auto summary_revision = ChatRepository::ChatSummaryStorageRevision(root, chat.id);
	const auto metadata = RemoteNativeMediaMetadata(root, chat);
	const auto profile = ProviderResolutionService().ProviderForChatOrDefault(m_app, chat);
	if (!root.is_absolute() || !revision || *revision == "missing" || !summary_revision || metadata.empty() ||
	    !chat.messages_loaded || chat.imported_read_only || chat.native_session_id.empty() ||
	    !chat.metadata_incarnation || chat.created_at.empty() ||
	    uam::provider_ids::NormalizeCliProviderAliasOrSelf(profile.id) != uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id) ||
	    host.id == "local" || host.id != chat.execution_host_id || host.transport != "ssh" ||
	    host.runner_status != "ready" || !uam::remote::CanAttemptRunnerConnection(host))
	{ cb->Failure(409, "The saved remote chat is not ready for a native refresh."); return; }
	work->root = root;
	const auto id = chat.id;
	const auto incarnation = chat.metadata_incarnation;
	const auto birth = chat.created_at;
	const auto native_key = uam::chat_identity::NativeIdentityKeyForHistoryImport(chat);
	const auto original_digest = uam::StateSerializer::MessageDigest(chat, false);
	const auto lineage = std::make_tuple(chat.parent_chat_id, chat.branch_root_chat_id, chat.branch_from_message_index);
	const auto resolved = m_app.resolved_native_sessions_by_chat_id.find(id);
	const std::optional<std::string> resolved_session = resolved == m_app.resolved_native_sessions_by_chat_id.end()
	    ? std::nullopt : std::optional<std::string>(resolved->second);
	const auto computer_state = std::make_tuple(chat.computer_use_globally_enabled, chat.computer_use_enabled,
	    chat.computer_use_target_kind, chat.computer_use_target_id, chat.computer_use_target_process_id,
	    chat.computer_use_target_title, chat.computer_use_target_input_mode);
	const bool computer_consent = m_app.settings.computer_use_globally_enabled;
	ChatSession parse_identity;
	parse_identity.id = id; parse_identity.provider_id = chat.provider_id;
	parse_identity.native_session_id = chat.native_session_id;
	parse_identity.workspace_directory = chat.workspace_directory;
	parse_identity.execution_host_id = chat.execution_host_id;
	parse_identity.folder_id = chat.folder_id;
	const auto request = std::make_shared<std::stop_source>();
	const auto prior = m_nativeHistoryRequests.find(id);
	if (prior != m_nativeHistoryRequests.end()) prior->second->request_stop();
	m_nativeHistoryRequests[id] = request;
	const auto finish = [this, id, request]()
	{
		const auto found = m_nativeHistoryRequests.find(id);
		if (found != m_nativeHistoryRequests.end() && found->second == request) m_nativeHistoryRequests.erase(found);
	};
	// This identity-only check also serves postcommit response admission. New
	// authored content is serialized from current live state, never an old page.
	const auto same_owner = [this, root, id, incarnation, birth, native_key, resolved_session, host, profile](bool require_loaded = true) -> ChatSession*
	{
		auto* current = ChatDomainService().FindChatById(m_app, id);
		const auto* now_host = uam::execution_hosts::Find(m_app.settings.execution_hosts, host.id);
		if (m_app.data_root != root || !current || current->metadata_incarnation != incarnation || current->created_at != birth ||
		    (require_loaded && !current->messages_loaded) || current->imported_read_only || current->native_session_id.empty() ||
		    uam::chat_identity::NativeIdentityKeyForHistoryImport(*current) != native_key || !now_host ||
		    !uam::execution_hosts::SameConnection(*now_host, host) || now_host->runner_status != "ready" ||
		    !SameRemoteNativeMediaProfile(ProviderResolutionService().ProviderForChatOrDefault(m_app, *current), profile)) return nullptr;
		const auto mapping = m_app.resolved_native_sessions_by_chat_id.find(id);
		const std::optional<std::string> now = mapping == m_app.resolved_native_sessions_by_chat_id.end()
		    ? std::nullopt : std::optional<std::string>(mapping->second);
		if (now != resolved_session || std::ranges::count_if(m_app.chats, [&](const ChatSession& row) { return row.id == id; }) != 1) return nullptr;
		for (const auto& other : m_app.chats)
			if (other.id != id && uam::chat_identity::NativeIdentityKeyForHistoryImport(other) == native_key) return nullptr;
		return current;
	};
	const auto admitted = [this, root, id, revision, summary_revision, metadata, original_digest,
	    computer_state, computer_consent, request, same_owner]() -> ChatSession*
	{
		const auto pending = m_nativeHistoryRequests.find(id);
		auto* current = same_owner();
		if (!current || request->stop_requested() || pending == m_nativeHistoryRequests.end() || pending->second != request ||
		    m_workspaceDeletionPending || m_app.deletion_progress || m_app.pending_chat_save_at_by_chat_id.contains(id) ||
		    m_app.pending_chat_save_tasks.contains(id) || m_app.worktree_operation_chat_ids.contains(id) ||
		    uam::runtime_orch_impl::ChatNeedsTranscriptPreserved(m_app, *current) ||
		    RemoteNativeMediaMetadata(root, *current) != metadata || uam::StateSerializer::MessageDigest(*current, false) != original_digest ||
		    ChatRepository::ChatStorageRevision(root, id) != revision || ChatRepository::ChatSummaryStorageRevision(root, id) != summary_revision ||
		    m_app.settings.computer_use_globally_enabled != computer_consent ||
		    std::tie(current->computer_use_globally_enabled, current->computer_use_enabled,
		        current->computer_use_target_kind, current->computer_use_target_id, current->computer_use_target_process_id,
		        current->computer_use_target_title, current->computer_use_target_input_mode) != computer_state) return nullptr;
		return current;
	};
	if (!admitted()) { finish(); cb->Failure(409, "Remote chat ownership changed. Retry to refresh its history."); return; }
	bool queued = false;
	try
	{
		queued = DispatchNativeMediaRefresh(cb,
		    [work, root, id, revision, summary_revision, metadata, original_digest, lineage, incarnation, parse_identity, host, request, reader = m_remoteNativeHistoryReaderForTests]()
		    {
			    if (request->stop_requested()) return AsyncFailure(409, "A newer history refresh was requested.");
			    if (ChatRepository::ChatStorageRevision(root, id) != revision || ChatRepository::ChatSummaryStorageRevision(root, id) != summary_revision)
				    return AsyncFailure(409, "Saved remote history changed before loading.");
			    work->candidate = ChatRepository::LoadLocalChat(root, id, true, &work->error, false);
			    if (work->candidate)
			    {
				    // Normalize can rewrite legacy lineage in live state only. Compare and
				    // publish the live lineage; the controller verifies the saved alias.
				    auto saved = std::tie(work->candidate->parent_chat_id, work->candidate->branch_root_chat_id, work->candidate->branch_from_message_index);
				    if (saved != lineage) { work->saved_lineage = saved; saved = lineage; }
			    }
			    if (!work->candidate || !work->candidate->messages_loaded || work->candidate->imported_read_only ||
			        RemoteNativeMediaMetadata(root, *work->candidate) != metadata ||
			        uam::StateSerializer::MessageDigest(*work->candidate, false) != original_digest ||
			        ChatRepository::ChatStorageRevision(root, id) != revision || ChatRepository::ChatSummaryStorageRevision(root, id) != summary_revision)
				    return AsyncFailure(409, "The saved remote conversation differs from its current view. Save or reload it before refreshing.");
			    work->candidate->metadata_incarnation = incarnation;
			    auto loaded = reader ? reader(host, parse_identity, &work->error, request->get_token())
			        : ChatHistorySyncService().LoadRemoteNativeTranscript(host, parse_identity, &work->error, request->get_token());
			    if (request->stop_requested()) return AsyncFailure(409, "A newer history refresh was requested.");
			    if (!loaded) return AsyncFailure(502, uam::strings::NonEmptyOrFallback(work->error, "Remote native history is unavailable."));
			    work->messages = std::move(loaded->messages);
			    return AsyncSuccess(nlohmann::json::object());
		    },
		    [this, browser, cb, work, root, revision, summary_revision, request, admitted, same_owner, finish, id, known_digest, page, defer_tool_call_content](AsyncCefResult& response)
		    {
			    RequestExit loaded_exit{finish};
			    auto* current = admitted();
			    if (!current) { response = AsyncFailure(409, "Remote chat changed while its native history was loading. Retry to refresh it."); return; }
			    if (!response.ok) return;
			    if (work->saved_lineage && !ChatBranching::SavedLineageNormalizesToCurrent(m_app.chats, id,
			            std::get<0>(*work->saved_lineage), std::get<1>(*work->saved_lineage), std::get<2>(*work->saved_lineage)))
			    {
				    response = AsyncFailure(409, "The saved remote conversation differs from its current view. Save or reload it before refreshing.");
				    return;
			    }
			    if (!work->candidate || work->messages.size() < work->candidate->messages.size())
			    {
				    response = SerializeGuardedChatMessagesResult(*current, known_digest, page, defer_tool_call_content, {}, ChatDomainService().SelectedChatId(m_app) == id);
				    return; // A readable shorter snapshot never replaces fuller saved history.
			    }
			    const bool preparation_queued = DispatchNativeMediaRefresh(cb,
			        [work, root, id, revision, summary_revision, request]()
			        {
				        if (request->stop_requested()) return AsyncFailure(409, "A newer history refresh was requested.");
				        if (ChatRepository::ChatStorageRevision(root, id) != revision || ChatRepository::ChatSummaryStorageRevision(root, id) != summary_revision)
					        return AsyncFailure(409, "Saved remote history changed before original preparation.");
				        if (!work->candidate || !ChatHistorySyncService().BuildNativeTranscriptCandidate(*work->candidate, std::move(work->messages)))
					        return AsyncFailure(409, "The complete remote conversation could not be prepared; saved history was retained.");
				        work->prepared = uam::PreparedNativeMediaChatSave::Prepare(root, std::move(*work->candidate), *revision, work->error, request->get_token());
				        work->candidate.reset();
				        return work->prepared ? AsyncSuccess(nlohmann::json::object())
				            : AsyncFailure(502, uam::strings::NonEmptyOrFallback(work->error, "Remote originals could not be prepared; saved history was retained."));
			        },
			        [this, browser, cb, work, root, id, request, admitted, same_owner, finish, known_digest, page, defer_tool_call_content](AsyncCefResult& result)
			        {
				        RequestExit prepared_exit{finish};
				        auto* target = admitted();
				        if (!target) { result = AsyncFailure(409, "Remote chat changed while its originals were prepared. Retry to refresh it."); return; }
				        if (!result.ok) return;
				        if (!work->prepared || !work->prepared->Publish(work->error, [&]() { return admitted() == target; }, false, m_nativeMediaPublicationStageGateForTests))
				        {
					        if (work->prepared && work->prepared->PrimaryPublicationState() == ChatPublicationState::Rejected)
						        result = AsyncFailure(409, "The chat or its saved history changed, so this refresh was not saved. Current history was kept. Try refreshing again.");
					        else
					        {
						        const auto metadata = RemoteNativeMediaObservationMetadata(root, *target);
						        const auto digest = uam::StateSerializer::MessageDigest(*target, false);
						        const auto count = target->messages.size();
						        const auto persisted_digest = target->persisted_messages_digest;
						        m_unconfirmedNativeMediaHistory[id] = {root, target->metadata_incarnation, [same_owner, root, metadata, digest, count, persisted_digest]() -> ChatSession*
						        {
							        auto* current = same_owner(false); // Passive display alone tolerates normal eviction.
							        return current && RemoteNativeMediaObservationMetadata(root, *current) == metadata &&
							            (current->messages_loaded ? uam::StateSerializer::MessageDigest(*current, false) == digest :
							                current->persisted_message_count == count && current->persisted_messages_digest == persisted_digest) ? current : nullptr;
						        }};
						        result = AsyncFailure(500, "UAM could not confirm that the refreshed history was saved. Reopen the chat before retrying.");
					        }
					        return;
				        }
				        work->retired = work->prepared->TakeCommittedChat();
				        if (!work->retired) { result = AsyncFailure(500, "Remote history was saved but could not be adopted. Reload this chat."); return; }
				        static_assert(std::is_nothrow_swappable_v<ChatSession>);
				        work->retired->managed_subagent_revision = target->managed_subagent_revision;
				        std::swap(*target, *work->retired); // The old live graph retires on the prearmed worker.
				        m_unconfirmedNativeMediaHistory.erase(id);
				        const auto complete_ack = [this, browser, work, root, id, request, same_owner, finish, known_digest, page, defer_tool_call_content](AsyncCefResult& ack)
				        {
					        RequestExit ack_exit{finish};
					        try
					        {
						        const bool uncertain = !ack.ok || !work->ack_error.empty();
						        const auto pending = m_nativeHistoryRequests.find(id);
						        auto* current = same_owner();
						        if (!current || request->stop_requested() || pending == m_nativeHistoryRequests.end() || pending->second != request)
						        {
							        ack = AsyncFailure(409, "Remote history was already saved, but this view changed before the response. Reopen the chat to see its current history.");
							        uam::PushStateUpdateIfChanged(browser, m_app);
							        return; // Never redraw a cached page, republish or roll back committed history.
						        }
						        ack = SerializeGuardedChatMessagesResult(*current, known_digest, page, defer_tool_call_content, {}, ChatDomainService().SelectedChatId(m_app) == id);
						        if (uncertain && ack.ok)
						        {
							        const std::string warning = "History and attached files were kept, but UAM could not confirm the final save. Reopen the chat to check it.";
							        auto body = nlohmann::json::parse(ack.body);
							        body["warning"] = warning; ack.body = body.dump();
							        if (m_app.data_root == root) m_app.status_line = warning;
						        }
						        uam::PushStateUpdateIfChanged(browser, m_app);
					        }
					        catch (...)
					        {
						        ack = AsyncFailure(500, "Remote history was already saved, but its response could not be prepared. Reopen the chat to see its current history.");
					        }
				        };
				        bool ack_queued = false;
				        try
				        {
					        ack_queued = DispatchNativeMediaRefresh(cb,
					            [work]() { work->Acknowledge(); return work->ack_error.empty() ? AsyncSuccess(nlohmann::json::object()) : AsyncFailure(500, "Original acknowledgement was not confirmed."); },
					            complete_ack);
				        }
				        catch (...) { /* Committed originals remain retained and worker retirement will retry acknowledgement. */ }
				        if (ack_queued)
				        {
					        prepared_exit.deferred = true;
					        result.callback_deferred = true;
				        }
				        else
				        {
					        result = AsyncFailure(503, "Original acknowledgement could not be queued.");
					        complete_ack(result); // Explicit committed success-warning, never a rollback.
				        }
			        });
			    if (!preparation_queued) { response = AsyncFailure(503, "The background task queue is unavailable."); return; }
			    loaded_exit.deferred = true;
			    response.callback_deferred = true;
		    });
	}
	catch (...) { finish(); throw; }
	if (!queued) { finish(); cb->Failure(503, "The background task queue is unavailable."); }
}

void UamQueryHandler::HandleGetToolCallContent(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::optional<int> message_index = uam::nlohmann_json::IntFieldStrict(payload, "messageIndex");
	if (payload.contains("messageIndex") && (!message_index.has_value() || *message_index < 0))
	{
		cb->Failure(400, "A valid message index is required.");
		return;
	}
	const std::string chat_id = payload.value("chatId", "");
	const std::string tool_call_id = payload.value("toolCallId", "");
	std::size_t offset = 0;
	if (const auto found = payload.find("offset"); found != payload.end())
	{
		if (!found->is_number_integer())
		{
			cb->Failure(400, "Tool output offset must be a non-negative integer.");
			return;
		}
		std::uint64_t requested = 0;
		if (found->is_number_unsigned())
		{
			requested = found->get<std::uint64_t>();
		}
		else
		{
			const std::int64_t signed_offset = found->get<std::int64_t>();
			if (signed_offset < 0)
			{
				cb->Failure(400, "Tool output offset must be a non-negative integer.");
				return;
			}
			requested = static_cast<std::uint64_t>(signed_offset);
		}
		if (requested > std::numeric_limits<std::size_t>::max())
		{
			cb->Failure(400, "Tool output offset is too large.");
			return;
		}
		offset = static_cast<std::size_t>(requested);
	}
	ChatSession* chat = uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr || tool_call_id.empty() || tool_call_id.size() > 256)
	{
		if (chat != nullptr)
		{
			cb->Failure(400, "A tool call id is required.");
		}
		return;
	}
	if (HandleReadUnconfirmedNativeMediaDetail(chat_id, payload,
	    message_index ? std::optional<std::size_t>(static_cast<std::size_t>(*message_index)) : std::nullopt, cb,
	    [tool_call_id, offset](const ChatMessagePage& page)
	    {
		    const ToolCall* match = nullptr;
		    for (const auto& tool : page.chat.messages.front().tool_calls)
			    if (tool.id == tool_call_id)
			    {
				    if (match) return uam::query_handler_async::AsyncFailure(409, "Tool call identity is ambiguous in this transcript.");
				    match = &tool;
			    }
		    return match ? uam::query_handler_async::AsyncSuccess(uam::StateSerializer::ToolCallContentPageForFrontend(*match, offset))
			    : uam::query_handler_async::AsyncFailure(404, "Tool call is unavailable in this saved history.");
	    })) return;

	if (const uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id); session != nullptr && !message_index.has_value())
	{
		const auto active_tool = std::ranges::find_if(
		    session->tool_calls,
		    [&](const uam::AcpToolCallState& candidate) { return candidate.id == tool_call_id; });
		if (active_tool != session->tool_calls.end())
		{
			cb->Success(uam::StateSerializer::ToolCallContentPageForFrontend(*active_tool, offset).dump());
			return;
		}
	}

	if (!chat->messages_loaded)
	{
		std::optional<std::size_t> requested_index;
		if (message_index.has_value()) requested_index = static_cast<std::size_t>(*message_index);
		const uam::ToolCallContentIdentity identity =
		    uam::CaptureToolCallContentIdentity(*chat, requested_index);
		const std::shared_ptr<std::optional<uam::ToolCallContentSnapshot>> resolved =
		    std::make_shared<std::optional<uam::ToolCallContentSnapshot>>();
		uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb,
		    [data_root = m_app.data_root, identity, tool_call_id, offset, resolved]()
		    {
			    *resolved = uam::LoadToolCallContentSnapshot(data_root, identity, tool_call_id);
			    if (!*resolved)
				    return uam::query_handler_async::AsyncFailure(404,
				        "Tool call not found or its transcript changed while loading.");
			    return uam::query_handler_async::AsyncSuccess(
			        uam::StateSerializer::ToolCallContentPageForFrontend((*resolved)->tool_call, offset));
		    },
		    [this, chat_id, resolved](uam::query_handler_async::AsyncCefResult& response)
		    {
			    if (!response.ok) return;
			    ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
			    if (!current || !*resolved || !uam::MatchesToolCallContentSnapshot(*current, **resolved))
				    response = uam::query_handler_async::AsyncFailure(409,
				        "Chat history changed while tool output was loading. Retry to refresh it.");
		    });
		return;
	}

	const std::size_t begin = message_index.has_value() ? static_cast<std::size_t>(*message_index) : 0;
	const std::size_t end = message_index.has_value() ? std::min(begin + 1, chat->messages.size()) : chat->messages.size();
	const ToolCall* match = nullptr;
	std::size_t matches = 0;
	for (std::size_t index = begin; index < end; ++index)
	{
		const Message& message = chat->messages[index];
		for (const ToolCall& tool : message.tool_calls)
		{
			if (tool.id == tool_call_id)
			{
				match = &tool;
				++matches;
			}
		}
	}
	if (matches != 1)
	{
		cb->Failure(matches == 0 ? 404 : 409,
		    matches == 0 ? "Tool call not found: " + tool_call_id : "Tool call identity is ambiguous in this transcript.");
		return;
	}
	cb->Success(uam::StateSerializer::ToolCallContentPageForFrontend(
	    *match, offset).dump());
}

namespace
{
	/// Captures workspace ownership without copying a transcript or blocking a live CLI.
	std::string SelectedWorktreeFingerprint(const ChatSession& chat)
	{
		return nlohmann::json::array({chat.id, chat.created_at, chat.folder_id, chat.provider_id,
		    uam::strings::NonEmptyOrFallback(chat.execution_host_id, "local"), chat.workspace_directory,
		    chat.workspace_isolation_kind, chat.workspace_source_directory, chat.workspace_base_ref,
		    chat.workspace_branch_name, chat.workspace_worktree_directory, chat.imported_read_only,
		    chat.agent_run_id, chat.goal_owner_chat_id, chat.temporary_parent_chat_id}).dump();
	}

	bool SelectedWorktreeBusy(const uam::AppState& app, const ChatSession& source)
	{
		for (const ChatSession& chat : app.chats)
		{
			if (chat.id != source.id && (chat.workspace_worktree_directory != source.workspace_worktree_directory ||
			    uam::strings::NonEmptyOrFallback(chat.execution_host_id, "local") != uam::strings::NonEmptyOrFallback(source.execution_host_id, "local"))) continue;
			if (app.worktree_operation_chat_ids.contains(chat.id) || uam::IsWorktreeLaunchPreparationPending(app, chat.id) ||
			    app.git_port_requests.contains(chat.id)) return true;
		}
		return false;
	}

	/// Read-only linked-worktree verification runs only on the CEF background thread.
	bool ValidateSelectedWorktree(const ChatSession& source,
	    const std::string& folder_directory, const std::filesystem::path& data_root)
	{
		const std::filesystem::path worktree = uam::paths::PathFromUtf8(source.workspace_worktree_directory);
		const std::filesystem::path original = uam::paths::PathFromUtf8(source.workspace_source_directory);
		std::error_code error;
		const std::filesystem::path physical = std::filesystem::canonical(worktree, error);
		if (error || uam::paths::IsLinkOrReparsePointNoThrow(worktree)) return false;
		const std::filesystem::path owned_root = std::filesystem::canonical(data_root / "worktrees", error);
		if (error || uam::paths::IsLinkOrReparsePointNoThrow(data_root / "worktrees")) return false;
		const std::filesystem::path relative = physical.lexically_relative(owned_root);
		if (relative.empty() || relative == "." || relative.is_absolute() || *relative.begin() == "..") return false;
		const std::filesystem::path source_path = std::filesystem::canonical(original, error);
		if (error || uam::paths::IsLinkOrReparsePointNoThrow(original)) return false;
		const std::filesystem::path folder_path = std::filesystem::canonical(uam::paths::PathFromUtf8(folder_directory), error);
		if (error || source_path != folder_path) return false;
		const std::filesystem::file_status marker = std::filesystem::symlink_status(worktree / ".git", error);
		if (error || !std::filesystem::is_regular_file(marker) || uam::paths::IsLinkOrReparsePointNoThrow(worktree / ".git")) return false;
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		const std::function<bool(const std::filesystem::path&, const std::string&, std::string&)> query = [deadline](const std::filesystem::path& directory, const std::string& args, std::string& output)
		{
			const int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
			if (remaining <= 0) return false;
			const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
			    "git --no-optional-locks -C " + uam::shell::EscapeArg(uam::paths::Utf8PathString(directory)) + " " + args,
			    std::min(1500, remaining));
			if (!result.ok || result.timed_out || result.canceled || result.exit_code != 0 || result.output_truncated || result.output.size() > 8192) return false;
			output = uam::strings::Trim(result.output);
			return !output.empty();
		};
		std::string common, expected_common, top, branch, admin;
		if (!query(original, "rev-parse --path-format=absolute --git-common-dir", expected_common)) return false;
		if (!query(worktree, "rev-parse --path-format=absolute --git-common-dir", common) ||
		    !query(worktree, "rev-parse --show-toplevel", top) || !query(worktree, "symbolic-ref --short HEAD", branch) ||
		    !query(worktree, "rev-parse --absolute-git-dir", admin) || branch != source.workspace_branch_name) return false;
		const std::filesystem::path actual_common = std::filesystem::canonical(uam::paths::PathFromUtf8(common), error);
		if (error) return false;
		const std::filesystem::path source_common = std::filesystem::canonical(uam::paths::PathFromUtf8(expected_common), error);
		if (error || actual_common != source_common) return false;
		const std::filesystem::path actual_top = std::filesystem::canonical(uam::paths::PathFromUtf8(top), error);
		if (error || actual_top != physical) return false;
		const std::filesystem::path administrative = std::filesystem::canonical(uam::paths::PathFromUtf8(admin), error);
		if (error || administrative.parent_path() != actual_common / "worktrees" || uam::paths::IsLinkOrReparsePointNoThrow(administrative)) return false;
		const std::filesystem::path pointer = administrative / "gitdir";
		if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(pointer, error)) || error ||
		    std::filesystem::file_size(pointer, error) > 8192 || error) return false;
		const std::filesystem::path back_pointer = std::filesystem::canonical(
		    uam::paths::PathFromUtf8(uam::strings::Trim(uam::io::ReadTextFile(pointer))), error);
		if (error) return false;
		const std::filesystem::path expected_pointer = std::filesystem::canonical(worktree / ".git", error);
		return !error && back_pointer == expected_pointer;
	}
}

void UamQueryHandler::HandleCreateSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string title = payload.value("title", "New Chat");
	const std::string requested_folder_id = payload.value("folderId", "");
	const std::string routing_mode = payload.value("routingMode", "explicit");
	const bool selected_worktree = payload.contains("sourceChatId") || payload.contains("expectedWorkspaceDirectory") || payload.contains("expectedExecutionHostId");
	std::string source_chat_id, expected_workspace, expected_host;
	if (selected_worktree)
	{
		for (const char* field : {"sourceChatId", "expectedWorkspaceDirectory", "expectedExecutionHostId"})
		{
			if (!payload.contains(field) || !payload[field].is_string() || payload[field].get_ref<const std::string&>().empty() ||
			    payload[field].get_ref<const std::string&>().size() > 8192)
			{ cb->Failure(400, "Choose an existing chat worktree."); return; }
		}
		source_chat_id = payload["sourceChatId"].get<std::string>();
		expected_workspace = payload["expectedWorkspaceDirectory"].get<std::string>();
		expected_host = payload["expectedExecutionHostId"].get<std::string>();
		if (routing_mode != "explicit" || payload.contains("gitBaseBranch") || requested_folder_id.empty() || expected_host != "local")
		{ cb->Failure(400, "Choose an existing local workspace without automatic routing or a Git base override."); return; }
	}
	std::optional<std::string> git_base_branch_override;
	if (payload.contains("gitBaseBranch"))
	{
		const nlohmann::json* value = uam::nlohmann_json::FindStringField(payload, "gitBaseBranch");
		if (!value || value->get_ref<const std::string&>().size() > 255 ||
		    std::ranges::any_of(value->get_ref<const std::string&>(), [](unsigned char c) { return c < 32 || c == 127; }))
		{ cb->Failure(400, "Choose a valid local Git base branch."); return; }
		git_base_branch_override = uam::strings::Trim(value->get<std::string>());
		if (!m_app.settings.git_enabled || !m_app.settings.git_auto_worktree || routing_mode != "explicit")
		{ cb->Failure(400, "A Git base override requires automatic local worktree creation."); return; }
	}
	if (routing_mode != "explicit" && routing_mode != "automatic")
	{
		cb->Failure(400, "Routing mode must be explicit or automatic.");
		return;
	}

	const ProviderProfile* preferred_provider = uam::query_handler_internal::ResolvePreferredCliProvider(m_app);
	const std::string default_provider_id = uam::query_handler_internal::DefaultNewChatProviderId(m_app, preferred_provider);
	const std::string requested_provider_id = payload.value("providerId", default_provider_id);
	const std::string provider_id = uam::query_handler_internal::ResolveNewChatProviderId(m_app, requested_provider_id, preferred_provider);
	const ChatFolder* routing_source = routing_mode == "automatic" ? ChatDomainService().FindFolderById(m_app, requested_folder_id) : nullptr;
	if (routing_mode == "automatic" && routing_source == nullptr)
	{
		cb->Failure(400, "Automatic routing requires an existing source workspace.");
		return;
	}
	const std::string execution_host_id = routing_source != nullptr ? uam::strings::NonEmptyOrFallback(routing_source->execution_host_id, "local") : uam::strings::Trim(payload.value("executionHostId", "local"));
	const ExecutionHost* execution_host = uam::execution_hosts::Find(m_app.settings.execution_hosts, execution_host_id);
	if (execution_host == nullptr)
	{
		cb->Failure(400, "The selected execution host no longer exists.");
		return;
	}
	if (git_base_branch_override && (execution_host->id != uam::execution_hosts::kLocalHostId || requested_folder_id.empty()))
	{ cb->Failure(400, "Choose an existing local Git workspace for the base override."); return; }
	const std::string previous_selected_chat_id = ChatDomainService().SelectedChatId(m_app);
	std::string requested_workspace = uam::strings::Trim(payload.value("workspaceDirectory", ""));
	std::string auto_created_folder_id;

	std::string target_folder_id;
	if (!requested_folder_id.empty())
	{
		const ChatFolder* folder = ChatDomainService().FindFolderById(m_app, requested_folder_id);
		if (folder == nullptr)
		{
			cb->Failure(400, "The selected workspace folder no longer exists.");
			return;
		}
		const std::string folder_host_id = uam::strings::NonEmptyOrFallback(
		    uam::strings::Trim(folder->execution_host_id), "local");
		if (folder_host_id != execution_host->id)
		{
			cb->Failure(400, "The selected workspace belongs to a different computer.");
			return;
		}
		if (!requested_workspace.empty() &&
		    !WorkspaceMatches(*execution_host, folder->directory, requested_workspace))
		{
			cb->Failure(400, "The workspace path does not match the selected workspace.");
			return;
		}
		target_folder_id = folder->id;
		requested_workspace = folder->directory;
	}
	else if (execution_host->id == uam::execution_hosts::kLocalHostId)
	{
		cb->Failure(400, "A workspace folder is required to create a local chat.");
		return;
	}
	else
	{
		if (!uam::execution_hosts::IsAbsoluteRemotePath(execution_host->platform, requested_workspace))
		{
			cb->Failure(400, "An absolute remote workspace path is required.");
			return;
		}
		const auto existing = std::ranges::find_if(m_app.folders, [&](const ChatFolder& folder) {
			return uam::strings::NonEmptyOrFallback(folder.execution_host_id, "local") == execution_host->id &&
			       WorkspaceMatches(*execution_host, folder.directory, requested_workspace);
		});
		if (existing != m_app.folders.end()) target_folder_id = existing->id;
		else if (!CreateFolder(m_app, WorkspaceTitle(requested_workspace), requested_workspace,
		                       &auto_created_folder_id, execution_host->id))
		{
			cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(
			    m_app.status_line, "Failed to create the remote workspace."));
			return;
		}
		else target_folder_id = auto_created_folder_id;
	}

	const nlohmann::json* payload_defaults = uam::nlohmann_json::FindObjectField(payload, "defaults");
	const std::optional<nlohmann::json> defaults = payload_defaults ? std::optional<nlohmann::json>(*payload_defaults) : std::nullopt;
	const bool select_chat = payload.value("selectChat", true);
	const std::function<void(const ChatSession*)> create = [this, browser, cb, target_folder_id, provider_id,
	    git_base_branch_override, execution_host_id, routing_mode, requested_workspace, title,
	    defaults, select_chat, previous_selected_chat_id, auto_created_folder_id](const ChatSession* validated_source)
	{
		ChatSession chat = ChatDomainService().CreateNewChat(target_folder_id, provider_id, validated_source == nullptr && m_app.settings.git_enabled && m_app.settings.git_auto_worktree, git_base_branch_override);
		chat.execution_host_id = execution_host_id;
		chat.svn_update_pending = validated_source == nullptr && execution_host_id == uam::execution_hosts::kLocalHostId &&
		    m_app.settings.svn_enabled && m_app.settings.svn_update_before_chat;
		chat.routing_mode = routing_mode;
		if (routing_mode == "automatic")
		{
			chat.routing_source_folder_id = target_folder_id;
			chat.routing_source_directory = requested_workspace;
		}
		if (!title.empty())
		{
			chat.title = title;
		}
		if (execution_host_id == uam::execution_hosts::kLocalHostId)
		{
			chat.workspace_directory = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(m_app, chat));
		}
		else
		{
			chat.workspace_directory = requested_workspace;
		}
		if (validated_source)
		{
			chat.workspace_directory = validated_source->workspace_worktree_directory;
			chat.workspace_isolation_kind = validated_source->workspace_isolation_kind;
			chat.workspace_source_directory = validated_source->workspace_source_directory;
			chat.workspace_base_ref = validated_source->workspace_base_ref;
			chat.workspace_branch_name = validated_source->workspace_branch_name;
			chat.workspace_worktree_directory = validated_source->workspace_worktree_directory;
		}
		uam::query_handler_internal::ApplyProviderDefaultsToChat(m_app.settings, chat, defaults ? &*defaults : nullptr);

		m_app.chats.push_back(std::move(chat));

		ChatSession& created_chat = m_app.chats.back();
		const std::string created_chat_id = created_chat.id;
		if (select_chat)
			ChatDomainService().SelectChatById(m_app, created_chat_id);

		ChatHistorySyncService sync;
		if (!sync.SaveChatWithStatus(m_app, created_chat, "", ""))
		{
			const std::string failure = uam::query_handler_internal::FailureDetailOrFallback(
			    m_app.status_line, "Failed to persist new chat.");
			uam::query_handler_internal::RollbackCreatedChat(m_app, created_chat_id, previous_selected_chat_id, false);
			if (!auto_created_folder_id.empty()) (void)DeleteFolderById(m_app, auto_created_folder_id);
			cb->Failure(500, failure);
			return;
		}

		if (!PersistenceCoordinator().SaveSettings(m_app))
		{
			const std::string failure = uam::query_handler_internal::FailureDetailOrFallback(
			    m_app.status_line, "Failed to persist new chat settings.");
			uam::query_handler_internal::RollbackCreatedChat(m_app, created_chat_id, previous_selected_chat_id, true);
			if (!auto_created_folder_id.empty()) (void)DeleteFolderById(m_app, auto_created_folder_id);
			cb->Failure(500, failure);
			return;
		}

		if (const ChatSession* selected = ChatDomainService().SelectedChat(m_app); selected != nullptr && selected->id == created_chat_id && ProviderResolutionService().ChatUsesCliOutput(m_app, *selected))
		{
			uam::MarkSelectedCliTerminalForLaunch(m_app);
		}

		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"chatId", created_chat_id}}.dump());
	};
	if (!selected_worktree) { create(nullptr); return; }
	ChatSession* source = ChatDomainService().FindChatById(m_app, source_chat_id);
	const ChatFolder* folder = ChatDomainService().FindFolderById(m_app, target_folder_id);
	if (!source || !folder || expected_host != uam::execution_hosts::kLocalHostId || execution_host_id != expected_host ||
	    source->folder_id != target_folder_id || uam::strings::NonEmptyOrFallback(source->execution_host_id, "local") != expected_host ||
	    source->workspace_worktree_directory != expected_workspace || !uam::paths::IsGitWorktreeIsolated(*source) ||
	    source->workspace_source_directory.empty() || source->workspace_branch_name.empty() || source->workspace_base_ref.empty() ||
	    source->imported_read_only || !source->agent_run_id.empty() || !source->goal_owner_chat_id.empty() ||
	    !source->temporary_parent_chat_id.empty() || SelectedWorktreeBusy(m_app, *source))
	{ cb->Failure(409, "The selected chat worktree is unavailable or changed."); return; }
	const std::shared_ptr<uam::WorkspaceLeaseGate::Lease> lease = m_app.worktree_retention_gate.TryAcquireMutation();
	if (!lease) { cb->Failure(409, "Workspace cleanup is finishing. Try again shortly."); return; }
	ChatSession metadata;
	metadata.id = source->id;
	metadata.workspace_directory = source->workspace_directory;
	metadata.workspace_isolation_kind = source->workspace_isolation_kind;
	metadata.workspace_source_directory = source->workspace_source_directory;
	metadata.workspace_base_ref = source->workspace_base_ref;
	metadata.workspace_branch_name = source->workspace_branch_name;
	metadata.workspace_worktree_directory = source->workspace_worktree_directory;
	const std::filesystem::path root = m_app.data_root;
	const std::shared_ptr<const std::uint8_t> parent = source->metadata_incarnation;
	const std::string fingerprint = SelectedWorktreeFingerprint(*source);
	const std::string folder_directory = folder->directory;
	const ExecutionHost host_snapshot = *execution_host;
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb,
	    [metadata, folder_directory, root, lease]()
	    {
		if (!ValidateSelectedWorktree(metadata, folder_directory, root))
			return uam::query_handler_async::AsyncFailure(409, "The selected worktree requires an existing Git source and its recorded branch.");
		return uam::query_handler_async::AsyncSuccess("{}");
	    },
	    [this, root, source_chat_id, parent, fingerprint, folder_directory, target_folder_id,
	        host_snapshot, metadata, lease, create](uam::query_handler_async::AsyncCefResult& result)
	    {
		if (!result.ok) return;
		const ChatSession* current = ChatDomainService().FindChatById(m_app, source_chat_id);
		const ChatFolder* folder = ChatDomainService().FindFolderById(m_app, target_folder_id);
		const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, host_snapshot.id);
		if (m_app.data_root != root || !current || current->metadata_incarnation != parent ||
		    fingerprint != SelectedWorktreeFingerprint(*current) || !folder || folder->directory != folder_directory ||
		    uam::strings::NonEmptyOrFallback(folder->execution_host_id, "local") != host_snapshot.id ||
		    !host || *host != host_snapshot || SelectedWorktreeBusy(m_app, *current))
		{ result = uam::query_handler_async::AsyncFailure(409, "The selected chat worktree changed before creation."); return; }
		create(&metadata);
		// Publication owns the callback; the async wrapper must not answer it a second time.
		result.callback_deferred = true;
	    }, TID_FILE_USER_BLOCKING);
}

void UamQueryHandler::HandleRetryFailedMessage(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::optional<int> message_index = uam::nlohmann_json::IntFieldStrict(payload, "messageIndex");
	if (!payload.contains("chatId") || !payload["chatId"].is_string() ||
	    payload["chatId"].get<std::string>().empty() || !message_index.has_value() || *message_index < 0 ||
	    payload.contains("content") || payload.contains("attachments") || payload.contains("markdownStoreFiles"))
	{
		cb->Failure(400, "Retry requires chatId and messageIndex without replacement content.");
		return;
	}
	const std::string chat_id = payload["chatId"].get<std::string>();
	ChatSession* chat = uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Chat no longer exists.");
	if (chat == nullptr) return;
	using namespace uam::query_handler_async;
	const ChatDomainService domain;
	const auto pending = m_app.branch_preparations.find(chat_id);
	if (pending != m_app.branch_preparations.end() && !pending->second.expired())
	{
		cb->Failure(409, "Chat preparation is still running. Wait before retrying the prompt.");
		return;
	}
	if (!chat->messages_loaded)
	{
		const std::shared_ptr<ChatDomainService::ColdHistoryPreparation> preparation = domain.BeginColdHistoryPreparation(m_app, chat_id);
		if (!preparation)
		{
			cb->Failure(409, "Chat history is changing. Retry after it settles.");
			return;
		}
		const bool queued = RunAsyncCefQuery(m_asyncLifetime, cb,
		    [domain, preparation]()
		    {
			    std::string error;
			    return domain.LoadColdHistoryPreparation(*preparation, error)
			        ? AsyncSuccess(nlohmann::json::object())
			        : AsyncFailure(409, uam::query_handler_internal::FailureDetailOrFallback(error, "Chat history changed or preparation was stopped."));
		    },
		    [this, browser, payload, cb, domain, preparation](AsyncCefResult& result)
		    {
			    if (!result.ok)
			    {
				    (void)domain.ConsumeBranchPreparation(m_app, preparation->identity.chat_id, preparation->ownership);
				    return;
			    }
			    if (!domain.CompleteColdHistoryPreparation(m_app, *preparation))
			    {
				    result = AsyncFailure(409, "Chat history changed or preparation was stopped. Retry the prompt.");
				    return;
			    }
			    HandleRetryFailedMessage(browser, payload, cb);
			    result.callback_deferred = true;
		    }, TID_FILE_USER_BLOCKING);
		if (!queued) (void)domain.ConsumeBranchPreparation(m_app, chat_id, preparation->ownership);
		return;
	}
	std::string error;
	if (!uam::RetryFailedAcpMessage(m_app, chat_id, *message_index, &error))
	{
		cb->Failure(409, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	const std::weak_ptr<void> lifetime = m_asyncLifetime;
	const std::filesystem::path data_root = m_app.data_root;
	if (uam::DeferChatSaveResult(m_app, chat_id, [this, lifetime, data_root, browser, cb, chat_id](bool saved)
	{
		if (lifetime.expired()) return;
		uam::PushStateUpdateIfChanged(browser, m_app);
		if (saved && m_app.data_root == data_root) cb->Success(nlohmann::json{{"chatId", chat_id}}.dump());
		else cb->Failure(500, "Prompt was not sent because chat history could not be saved.");
	})) return;
	cb->Success(nlohmann::json{{"chatId", chat_id}}.dump());
}

void UamQueryHandler::HandleBranchFromMessage(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::optional<int> message_index = uam::nlohmann_json::IntFieldStrict(payload, "messageIndex");
	if (!message_index.has_value() || *message_index < 0)
	{
		cb->Failure(400, "A valid message index is required.");
		return;
	}

	if (uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Branch source chat no longer exists.") == nullptr)
	{
		return;
	}
	if ((payload.contains("selectChat") && !payload["selectChat"].is_boolean()) ||
	    (payload.contains("regenerate") && !payload["regenerate"].is_boolean()) ||
	    (payload.contains("content") && !payload["content"].is_string()))
	{
		cb->Failure(400, "selectChat and regenerate must be booleans; content must be a string.");
		return;
	}
	const std::optional<std::string> replacement_content = payload.contains("content")
	                                                      ? std::optional<std::string>(payload.value("content", ""))
	                                                      : std::nullopt;
	if (payload.contains("operationId") && !payload["operationId"].is_string())
	{
		cb->Failure(400, "operationId must be a string.");
		return;
	}
	const std::optional<std::string> operation_id = payload.contains("operationId") ? std::optional<std::string>(payload.value("operationId", "")) : std::nullopt;

	std::optional<std::vector<std::string>> retained_attachment_ids;
	if (payload.contains("retainedAttachmentIds"))
	{
		if (!payload["retainedAttachmentIds"].is_array() || payload["retainedAttachmentIds"].size() > 128)
		{
			cb->Failure(400, "retainedAttachmentIds must be an array of attachment IDs.");
			return;
		}
		retained_attachment_ids.emplace();
		for (const nlohmann::json& attachment_id : payload["retainedAttachmentIds"])
		{
			if (!attachment_id.is_string())
			{
				cb->Failure(400, "retainedAttachmentIds must contain only attachment IDs.");
				return;
			}
			retained_attachment_ids->push_back(attachment_id.get<std::string>());
		}
	}
	std::optional<std::vector<std::string>> retained_markdown_store_files;
	if (payload.contains("retainedMarkdownStoreFiles"))
	{
		const auto& files = payload["retainedMarkdownStoreFiles"];
		if (!files.is_array() || files.size() > 128)
		{
			cb->Failure(400, "retainedMarkdownStoreFiles must be an array of skill file identities.");
			return;
		}
		retained_markdown_store_files.emplace();
		for (const auto& file : files)
		{
			if (!file.is_string() || file.get_ref<const std::string&>().empty() || file.get_ref<const std::string&>().size() > 16384)
			{
				cb->Failure(400, "retainedMarkdownStoreFiles must contain bounded nonempty skill file identities.");
				return;
			}
			retained_markdown_store_files->push_back(file.get<std::string>());
		}
	}
	const BranchFromMessageRequest request{
	    .chat_id = chat_id,
	    .message_index = *message_index,
	    .replacement_content = replacement_content,
	    .operation_id = operation_id,
	    .retained_attachment_ids = retained_attachment_ids,
	    .select_chat = payload.value("selectChat", true),
	    .regenerate = payload.value("regenerate", true),
	    .retained_markdown_store_files = retained_markdown_store_files,
	};
	const ChatDomainService domain_service;
	ChatSession* source = domain_service.FindChatById(m_app, chat_id);
	if (!source)
	{
		cb->Failure(404, "Branch source chat no longer exists.");
		return;
	}
	std::string existing_id;
	if (operation_id && !uam::strings::Trim(*operation_id).empty())
	{
		const auto found = std::ranges::find(m_app.chats, uam::strings::Trim(*operation_id), &ChatSession::branch_operation_id);
		if (found != m_app.chats.end())
			existing_id = found->id;
	}
	const auto pending = m_app.branch_preparations.find(chat_id);
	if (pending != m_app.branch_preparations.end() && !pending->second.expired())
	{
		cb->Failure(409, "Branch preparation is already loading. Wait or stop it before retrying.");
		return;
	}
	const ChatSession* existing = existing_id.empty() ? nullptr : domain_service.FindChatById(m_app, existing_id);
	const bool source_loaded = source->messages_loaded;
	const bool existing_loaded = existing && existing->messages_loaded;
	// Known existing operations are checked for reuse/conflict before any new
	// final owner or original generation. Nonmedia loaded requests keep their route.
	if (source_loaded && (!existing || existing_loaded) && (!existing_id.empty() || !uam::BranchPrefixHasOwnedOriginals(*source, *message_index)))
	{
		const auto result = ExecuteBranchFromMessageRequest(m_app, browser, request);
		if (result.ok)
			cb->Success(result.body);
		else
			cb->Failure(result.status, result.error);
		return;
	}
	struct Work
	{
		std::filesystem::path root;
		std::string source_id, existing_id, source_revision, existing_revision, final_id;
		ChatDomainService::BranchHistoryIdentity source_identity;
		std::optional<ChatDomainService::BranchHistoryIdentity> existing_identity;
		std::shared_ptr<const std::uint8_t> source_incarnation, existing_incarnation;
		std::optional<ChatFolder> folder;
		std::optional<ExecutionHost> host;
		std::string folder_id;
		bool source_loaded = false, existing_loaded = false;
		std::optional<ChatSession> source, existing;
		std::shared_ptr<uam::BranchPreparationOwnership> ownership;
		std::unique_ptr<uam::PreparedBranchOriginals> originals;
		uam::query_handler_async::AsyncCefResult outcome;
		std::string ack_error;
	};
	// Arm before retaining bodies, prepared originals or any filesystem destructor.
	auto work = uam::native_history::TryCreateWorkerOwnedNativeRefresh<Work>();
	if (!work)
	{
		cb->Failure(503, "Branch preparation is busy. Retry shortly.");
		return;
	}
	work->root = m_app.data_root;
	work->source_id = chat_id;
	work->existing_id = existing_id;
	work->source_identity = domain_service.CaptureBranchHistoryIdentity(*source);
	work->source_incarnation = source->metadata_incarnation;
	work->source_loaded = source_loaded;
	work->existing_loaded = existing_loaded;
	work->folder_id = source->folder_id;
	if (const auto* folder = domain_service.FindFolderById(m_app, source->folder_id))
		work->folder = *folder;
	if (const auto* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, source->execution_host_id))
		work->host = *host;
	const auto revision = ChatRepository::ChatStorageRevision(work->root, chat_id);
	if (!revision || *revision == "missing")
	{
		cb->Failure(409, "The saved branch source is unavailable. Save and retry.");
		return;
	}
	work->source_revision = *revision;
	if (source_loaded)
		work->source = *source;
	if (existing)
	{
		work->existing_identity = domain_service.CaptureBranchHistoryIdentity(*existing);
		work->existing_incarnation = existing->metadata_incarnation;
		const auto saved = ChatRepository::ChatStorageRevision(work->root, existing_id);
		if (!saved || *saved == "missing")
		{
			cb->Failure(409, "The existing branch is unavailable. Refresh and retry.");
			return;
		}
		work->existing_revision = *saved;
		if (existing_loaded)
			work->existing = *existing;
	}
	work->ownership = domain_service.BeginBranchPreparation(m_app, chat_id, existing_id);
	if (!work->ownership)
	{
		cb->Failure(409, "Branch preparation is already loading.");
		return;
	}
	const auto finish = [this, work]() { (void)ChatDomainService().ConsumeBranchPreparation(m_app, work->source_id, work->ownership); };
	const auto admitted = [this, work, request]()
	{
		if (work->ownership->cancellation.stop_requested() || m_app.data_root != work->root)
			return false;
		const auto reservation = m_app.branch_preparations.find(work->source_id);
		if (reservation == m_app.branch_preparations.end() || reservation->second.lock() != work->ownership)
			return false;
		const auto blocked = [this](const std::string& id) { return !id.empty() && (m_app.worktree_operation_chat_ids.contains(id) || m_app.pending_chat_save_at_by_chat_id.contains(id) || m_app.pending_chat_save_tasks.contains(id)); };
		if (blocked(work->source_id) || blocked(work->existing_id))
			return false;
		if (m_workspaceDeletionPending && (m_app.worktree_operation_chat_ids.contains(work->source_id) || m_app.worktree_operation_chat_ids.contains(work->existing_id)))
			return false;
		const auto* current = ChatDomainService().FindChatById(m_app, work->source_id);
		if (!current || current->metadata_incarnation != work->source_incarnation || current->folder_id != work->folder_id || !ChatDomainService().MatchesBranchHistoryIdentity(*current, work->source_identity) || (work->source_loaded && !current->messages_loaded) || ChatRepository::ChatStorageRevision(work->root, work->source_id) != std::optional<std::string>(work->source_revision))
			return false;
		if (work->source && !ChatDomainService().MatchesPreparedBranchHistory(*current, work->source_identity, work->source_loaded, &*work->source))
			return false;
		// This opt-in selection owns captured skill snapshots, including prefix rows.
		// The existing metadata guards do not witness an unsaved skill-only edit.
		if (request.retained_markdown_store_files && work->source && current->messages_loaded)
		{
			if (request.message_index < 0 || static_cast<std::size_t>(request.message_index) >= work->source->messages.size() || static_cast<std::size_t>(request.message_index) >= current->messages.size())
				return false;
			for (std::size_t row = 0; row <= static_cast<std::size_t>(request.message_index); ++row)
			{
				const auto& captured = work->source->messages[row];
				const auto& present = current->messages[row];
				if (captured.markdown_store_files != present.markdown_store_files || captured.markdown_store_prompt_blocks != present.markdown_store_prompt_blocks)
					return false;
			}
		}
		if (work->originals && current->messages_loaded && !work->originals->MatchesSource(*current))
			return false;
		const auto* folder = ChatDomainService().FindFolderById(m_app, work->folder_id);
		if (static_cast<bool>(work->folder) != (folder != nullptr) || (folder && (folder->directory != work->folder->directory || folder->execution_host_id != work->folder->execution_host_id)))
			return false;
		const auto* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, current->execution_host_id);
		if (static_cast<bool>(work->host) != (host != nullptr) || (host && !uam::execution_hosts::SameConnection(*host, *work->host)))
			return false;
		if (!work->existing_id.empty())
		{
			const auto* branch = ChatDomainService().FindChatById(m_app, work->existing_id);
			if (!branch || branch->metadata_incarnation != work->existing_incarnation || !work->existing_identity || branch->branch_operation_id != uam::strings::Trim(request.operation_id.value_or("")) || !ChatDomainService().MatchesBranchHistoryIdentity(*branch, *work->existing_identity) || (work->existing_loaded && !branch->messages_loaded) || ChatRepository::ChatStorageRevision(work->root, work->existing_id) != std::optional<std::string>(work->existing_revision))
				return false;
			if (work->existing && !ChatDomainService().MatchesPreparedBranchHistory(*branch, *work->existing_identity, work->existing_loaded, &*work->existing))
				return false;
		}
		return true;
	};
	if (!admitted())
	{
		finish();
		cb->Failure(409, "The branch source is changing or still being saved. Retry after it settles.");
		return;
	}
	const auto commit = [this, browser, cb, request, work, admitted, finish](uam::query_handler_async::AsyncCefResult& response)
	{
		struct Exit
		{
			std::function<void()> finish;
			~Exit()
			{
				finish();
			}
		} exit{finish};
		if (!response.ok)
			return;
		if (!admitted() || !work->source || !work->source->messages_loaded || (work->originals && (!work->originals->MatchesSource(*work->source) || ChatDomainService().FindChatById(m_app, work->final_id) != nullptr || ChatRepository::ChatStorageRevision(work->root, work->final_id) != std::optional<std::string>("missing"))))
		{
			response = uam::query_handler_async::AsyncFailure(409, "The branch source or final owner changed during preparation.");
			return;
		}
		if (!ChatDomainService().ConsumeBranchPreparation(m_app, request.chat_id, work->ownership))
		{
			response = uam::query_handler_async::AsyncFailure(409, "Branch preparation was stopped or replaced.");
			return;
		}
		response = ExecuteBranchFromMessageRequest(m_app, browser, request, work->source_loaded ? nullptr : &*work->source, work->existing_loaded || !work->existing ? nullptr : &*work->existing, work->originals.get());
		if (!work->originals || !work->originals->PrimaryAcknowledged())
			return;
		work->outcome = response;
		const auto complete_ack = [work](uam::query_handler_async::AsyncCefResult& result)
		{
			const bool uncertain = !result.ok || !work->ack_error.empty();
			result = work->outcome;
			if (!uncertain)
				return;
			const std::string warning = " Attached originals were kept, but their save could not be fully confirmed.";
			if (result.ok)
			{
				auto body = nlohmann::json::parse(result.body);
				body["warning"] = body.value("warning", "") + warning;
				result.body = body.dump();
			}
			else
				result.error += warning;
		};
		// Original receipt writes and destruction never run in this UI completion.
		// Unlike RunAsyncCefQuery, a refused post must preserve the real branch result.
		bool ack_queued = false;
		try
		{
			ack_queued = CefPostTask(TID_FILE_BACKGROUND, new uam::query_handler_async::CefQueryWorkerTask(
			                                                  m_asyncLifetime, cb,
			                                                  [work]()
			                                                  {
				                                                  try
				                                                  {
					                                                  if (!work->originals->AcknowledgeOriginals(work->ack_error) && work->ack_error.empty())
						                                                  work->ack_error = "Original acknowledgement was not confirmed.";
				                                                  }
				                                                  catch (...)
				                                                  {
					                                                  work->ack_error = "Original acknowledgement failed.";
				                                                  }
				                                                  return work->ack_error.empty() ? uam::query_handler_async::AsyncSuccess(nlohmann::json::object()) : uam::query_handler_async::AsyncFailure(500, "Original acknowledgement was not confirmed.");
			                                                  },
			                                                  complete_ack));
		}
		catch (...)
		{ /* No callback was transferred; retain the real result below. */
		}
		if (ack_queued)
			response.callback_deferred = true;
		else
		{
			response = uam::query_handler_async::AsyncFailure(503, "Original acknowledgement could not be queued.");
			complete_ack(response);
		}
	};
	const auto after_history = [this, cb, request, work, admitted, finish, commit](uam::query_handler_async::AsyncCefResult& response)
	{
		if (!response.ok)
		{
			finish();
			return;
		}
		if (!admitted() || !work->source || !work->source->messages_loaded)
		{
			finish();
			response = uam::query_handler_async::AsyncFailure(409, "The branch history changed while loading. Retry.");
			return;
		}
		// Existing operations reuse or conflict in domain before any new asset work.
		if (!work->existing_id.empty() || !uam::BranchPrefixHasOwnedOriginals(*work->source, request.message_index))
		{
			commit(response);
			return;
		}
		work->final_id = uam::chat_ids::NewChatId();
		if (!uam::chat_ids::IsSafeStorageChatId(work->final_id) || work->final_id == work->source_id || ChatDomainService().FindChatById(m_app, work->final_id) != nullptr || ChatRepository::ChatStorageRevision(work->root, work->final_id) != std::optional<std::string>("missing"))
		{
			finish();
			response = uam::query_handler_async::AsyncFailure(409, "A fresh branch owner could not be admitted.");
			return;
		}
		bool queued = false;
		try
		{
			queued = uam::query_handler_async::RunAsyncCefQuery(
			    m_asyncLifetime, cb,
			    [work, request]()
			    {
				    if (work->ownership->cancellation.stop_requested() || ChatRepository::ChatStorageRevision(work->root, work->source_id) != std::optional<std::string>(work->source_revision))
					    return uam::query_handler_async::AsyncFailure(409, "The saved branch source changed before original preparation.");
				    std::string error;
				    work->originals = uam::PreparedBranchOriginals::Prepare(work->root, *work->source, work->final_id, request.message_index, request.retained_attachment_ids, error, work->ownership->cancellation.get_token(), request.retained_markdown_store_files);
				    if (!work->originals)
					    return uam::query_handler_async::AsyncFailure(409, error);
				    if (ChatRepository::ChatStorageRevision(work->root, work->source_id) != std::optional<std::string>(work->source_revision))
					    return uam::query_handler_async::AsyncFailure(409, "The saved branch source changed during original preparation.");
				    return uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
			    },
			    commit);
			// A normal false return has already answered the queue failure once.
			response.callback_deferred = true;
		}
		catch (...)
		{
			finish();
			response = uam::query_handler_async::AsyncFailure(503, "Branch original preparation could not be queued.");
			return;
		}
		if (!queued)
			finish(); // RunAsync already reports its single queue failure.
	};
	if (source_loaded && (!existing || existing_loaded))
	{
		auto response = uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
		uam::query_handler_async::CompleteAsyncCefQuery(cb, response, after_history);
		return;
	}
	bool queued = false;
	try
	{
		queued = uam::query_handler_async::RunAsyncCefQuery(
		    m_asyncLifetime, cb,
		    [work, domain_service]()
		    {
			    if (work->ownership->cancellation.stop_requested() || ChatRepository::ChatStorageRevision(work->root, work->source_id) != std::optional<std::string>(work->source_revision))
				    return uam::query_handler_async::AsyncFailure(409, "Branch preparation was cancelled or its source changed.");
			    if (!work->source_loaded)
				    work->source = domain_service.LoadBranchHistoryDetached(work->root, work->source_identity);
			    if (!work->source || !work->source->messages_loaded)
				    return uam::query_handler_async::AsyncFailure(409, "Branch source history could not be loaded.");
			    if (!work->existing_id.empty() && !work->existing_loaded)
			    {
				    if (ChatRepository::ChatStorageRevision(work->root, work->existing_id) != std::optional<std::string>(work->existing_revision))
					    return uam::query_handler_async::AsyncFailure(409, "Existing branch history changed before loading.");
				    work->existing = domain_service.LoadBranchHistoryDetached(work->root, *work->existing_identity);
				    if (!work->existing)
					    return uam::query_handler_async::AsyncFailure(409, "Existing branch history could not be loaded.");
			    }
			    if (work->ownership->cancellation.stop_requested() || ChatRepository::ChatStorageRevision(work->root, work->source_id) != std::optional<std::string>(work->source_revision) || (!work->existing_id.empty() && ChatRepository::ChatStorageRevision(work->root, work->existing_id) != std::optional<std::string>(work->existing_revision)))
				    return uam::query_handler_async::AsyncFailure(409, "Branch history changed during loading.");
			    return uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
		    },
		    after_history);
	}
	catch (...)
	{
		finish();
		cb->Failure(503, "Branch history preparation could not be queued.");
		return;
	}
	if (!queued)
		finish();
}

void UamQueryHandler::SavePassiveChatMetadata(CefRefPtr<CefBrowser> browser, const ChatSession& chat,
    std::optional<std::string> title, std::optional<bool> pinned, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	std::erase_if(m_passiveMetadataSaves, [](const auto& entry) { return entry.second.expired(); });
	if (m_passiveMetadataSaves.contains(chat.id))
	{ cb->Failure(409, "An imported chat metadata save is already pending. Retry when it finishes."); return; }
	struct Work
	{
		ChatSession candidate;
		std::shared_ptr<PreparedChatSave> prepared;
	};
	// Bound retained snapshots and retire abandoned prepared files off the UI thread.
	auto work = uam::native_history::TryCreateWorkerOwnedNativeRefresh<Work>();
	if (!work) { cb->Failure(503, "Chat metadata saving is busy. Retry shortly."); return; }
	const auto root = m_app.data_root;
	const auto id = chat.id;
	const auto incarnation = chat.metadata_incarnation;
	const auto birth = chat.created_at;
	const auto metadata = ChatRepository::ChatMetadataFingerprint(root, chat);
	const auto revision = ChatRepository::ChatStorageRevision(root, id);
	const auto summary_revision = ChatRepository::ChatSummaryStorageRevision(root, id);
	if (!chat.imported_read_only || metadata.empty() || !revision || *revision == "missing" || !summary_revision)
	{ cb->Failure(409, "The saved imported chat is unavailable. Refresh before changing its metadata."); return; }
	const auto request = std::make_shared<const char>(0);
	m_passiveMetadataSaves[id] = request;
	const auto finish = [this, id, request]()
	{
		const auto found = m_passiveMetadataSaves.find(id);
		if (found != m_passiveMetadataSaves.end() && found->second.lock() == request) m_passiveMetadataSaves.erase(found);
	};
	const auto admitted = [this, root, id, incarnation, birth, metadata, revision, summary_revision, request]() -> ChatSession*
	{
		const auto pending = m_passiveMetadataSaves.find(id);
		ChatSession* current = ChatDomainService().FindChatById(m_app, id);
		if (pending == m_passiveMetadataSaves.end() || pending->second.lock() != request || m_app.data_root != root ||
		    !current || !current->imported_read_only || current->metadata_incarnation != incarnation || current->created_at != birth ||
		    m_workspaceDeletionPending || m_app.deletion_progress || m_app.worktree_operation_chat_ids.contains(id) ||
		    m_app.pending_chat_save_at_by_chat_id.contains(id) || m_app.pending_chat_save_tasks.contains(id) ||
		    ChatRepository::ChatMetadataFingerprint(root, *current) != metadata ||
		    ChatRepository::ChatStorageRevision(root, id) != revision ||
		    ChatRepository::ChatSummaryStorageRevision(root, id) != summary_revision) return nullptr;
		return current;
	};
	if (!admitted()) { finish(); cb->Failure(409, "The imported chat changed. Refresh before saving its metadata."); return; }
	try
	{
		work->candidate = chat;
		work->candidate.persisted_message_count = chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count;
		work->candidate.messages.clear();
		work->candidate.messages_loaded = false;
		if (title)
		{
			work->candidate.title = *title;
			work->candidate.updated_at = uam::time::TimestampNow();
		}
		if (pinned) work->candidate.pinned = *pinned;
		const bool queued = DispatchNativeMediaRefresh(cb,
		    [work, root, revision, request]()
		    {
			    work->prepared = ChatRepository::PrepareChatSave(root, work->candidate, *revision);
			    return work->prepared ? AsyncSuccess(nlohmann::json::object())
			        : AsyncFailure(409, "Could not prepare the imported chat metadata. Refresh and retry.");
		    },
		    [this, work, browser, title, pinned, request, finish, admitted](AsyncCefResult& result)
		    {
			    struct Exit { std::function<void()> finish; ~Exit() { finish(); } } exit{finish};
			    if (!result.ok) return;
			    ChatSession* current = admitted();
			    if (!current) { result = AsyncFailure(409, "The imported chat changed while its metadata was saving. Retry."); return; }
			    if (!ChatRepository::PublishPreparedChatSave(work->prepared, true, nullptr, [&]() { return admitted() != nullptr; }))
			    { result = AsyncFailure(409, "The saved imported chat changed before publication. Refresh and retry."); return; }
			    // Apply only this acknowledged mutation, retaining live-only state and viewport ownership.
			    if (title) { current->title = *title; current->updated_at = work->candidate.updated_at; }
			    if (pinned) current->pinned = *pinned;
			    current->acp_queue_full_save_revision = work->prepared->published_queue_full_revision;
			    m_app.status_line = title ? "Chat title updated." : "Chat pin updated.";
			    uam::PushStateUpdateIfChanged(browser, m_app);
		    });
		if (!queued)
		{
			finish();
			cb->Failure(503, "The background task queue is unavailable.");
		}
	}
	catch (...)
	{
		finish();
		throw;
	}
}

void UamQueryHandler::HandleRenameSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string title = payload.value("title", "");

	ChatSession* chat = uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (chat->imported_read_only)
	{
		const auto trimmed = uam::strings::Trim(title);
		if (trimmed.empty()) { cb->Failure(400, "Chat title is required."); return; }
		if (trimmed == chat->title) { cb->Success("{}"); return; }
		SavePassiveChatMetadata(browser, *chat, trimmed, std::nullopt, cb);
		return;
	}

	if (!ChatHistorySyncService().RenameChat(m_app, *chat, title))
	{
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Failed to rename chat: " + chat_id));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatPinned(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool pinned = payload.value("pinned", false);

	ChatSession* chat = uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (payload.contains("expectedPinned") && (!payload["expectedPinned"].is_boolean() || chat->pinned != payload["expectedPinned"].get<bool>()))
	{
		cb->Failure(409, "Chat pin changed. Refresh before undoing.");
		return;
	}

	if (chat->pinned == pinned)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	if (chat->imported_read_only)
	{
		SavePassiveChatMetadata(browser, *chat, std::nullopt, pinned, cb);
		return;
	}

	const bool previous_pinned = chat->pinned;
	chat->pinned = pinned;

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat pin updated.", "Chat pin changed in UI, but failed to save."))
	{
		chat->pinned = previous_pinned;
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Failed to persist chat pin."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleDeleteSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	if (uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id) == nullptr)
	{
		return;
	}

	StartChatDeletion(browser, {chat_id}, true, cb);
}

void UamQueryHandler::HandleDeleteSessions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::vector<std::string> chat_ids;
	const std::string validation_error = uam::query_handler_internal::ParseDeleteChatIds(payload, chat_ids);
	if (!validation_error.empty())
	{
		cb->Failure(400, validation_error);
		return;
	}
	StartChatDeletion(browser, chat_ids, false, cb);
}

void UamQueryHandler::HandleAcknowledgeChatAttention(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string revision = payload.value("attentionRevision", "");
	if (!ChatDomainService().AcknowledgeChatAttention(m_app, chat_id, revision))
	{
		cb->Failure(500, "Could not save chat acknowledgement.");
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleMoveChatWorkspace(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string folder_id = payload.value("folderId", "");
	const ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	const ChatFolder* folder = ChatDomainService().FindFolderById(m_app, folder_id);
	if (chat == nullptr || folder == nullptr) { cb->Failure(404, "The chat or destination workspace no longer exists."); return; }
	const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, chat->execution_host_id);
	if (host == nullptr || host->id == "local" || folder->execution_host_id != chat->execution_host_id)
	{ cb->Failure(400, "Select a workspace on this chat's SSH host."); return; }
	const ExecutionHost observed_host = *host;
	const std::string original_workspace = chat->workspace_directory;
	const std::string destination = folder->directory;
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb,
	    [observed_host, destination]()
	    {
		    uam::remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
		        observed_host);
		    uam::remote::DirectoryListing listing;
		    std::string error;
		    return client.ListDirectories(uam::paths::PathFromUtf8(destination), listing, &error)
		        ? uam::query_handler_async::AsyncSuccess({}) : uam::query_handler_async::AsyncFailure(502, error);
	    },
	    [this, browser, chat_id, folder_id, observed_host, original_workspace, destination](uam::query_handler_async::AsyncCefResult& result)
	    {
		    if (!result.ok) return;
		    ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
		    const ChatFolder* folder = ChatDomainService().FindFolderById(m_app, folder_id);
		    const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, observed_host.id);
		    if (current == nullptr || folder == nullptr || host == nullptr || current->workspace_directory != original_workspace ||
		        current->execution_host_id != observed_host.id || folder->execution_host_id != observed_host.id ||
		        folder->directory != destination || !uam::execution_hosts::SameConnection(*host, observed_host))
		    { result = uam::query_handler_async::AsyncFailure(409, "The chat or workspace changed during remote validation."); return; }
		    if (!ChatHistorySyncService().MoveChatToFolder(m_app, *current, folder_id))
		    { result = uam::query_handler_async::AsyncFailure(409, m_app.status_line); return; }
		    uam::PushStateUpdateIfChanged(browser, m_app);
	    });
}

void UamQueryHandler::HandleSetChatSnoozedUntil(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!payload.contains("chatId") || !payload["chatId"].is_string() || !payload.contains("untilMs") ||
	    !payload["untilMs"].is_number_integer() || payload["untilMs"] < 0 || payload["untilMs"] > 8'640'000'000'000'000LL)
	{
		cb->Failure(400, "Choose a valid chat and wake time.");
		return;
	}
	const std::string chat_id = payload["chatId"].get<std::string>();
	const std::int64_t until_ms = payload["untilMs"].get<std::int64_t>();
	std::string error;
	if (!ChatDomainService().ValidateChatSnooze(m_app, chat_id, until_ms, &error)) { cb->Failure(409, error); return; }
	if (m_snoozeRequests.contains(chat_id) || m_snoozeRequests.size() >= 64)
	{ cb->Failure(409, "A wake time is still being saved. Try again shortly."); return; }
	const ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	ChatSession metadata;
	metadata.id = chat_id;
	metadata.last_opened_at = chat->last_opened_at;
	metadata.interaction_at = chat->interaction_at;
	metadata.attention_revision = chat->attention_revision;
	metadata.settled_at = chat->settled_at;
	const std::int64_t previous_until = chat->snoozed_until_ms;
	metadata.snoozed_until_ms = until_ms;
	const std::string previous_interaction = metadata.interaction_at;
	const std::string previous_settled = metadata.settled_at;
	if (until_ms == 0 && previous_until != 0)
	{
		metadata.interaction_at = uam::time::InteractionTimestampNow();
		metadata.settled_at.clear();
	}
	if (!m_snoozeQueries) m_snoozeQueries = CefThread::CreateThread("uam-chat-snooze");
	if (!m_snoozeQueries) { cb->Failure(503, "The wake-time worker is unavailable."); return; }
	const std::filesystem::path data_root = m_app.data_root;
	const std::shared_ptr<std::shared_ptr<PreparedChatSave>> prepared = std::make_shared<std::shared_ptr<PreparedChatSave>>();
	m_snoozeRequests.insert(chat_id);
	if (!m_snoozeQueries->GetTaskRunner()->PostTask(new uam::query_handler_async::CefQueryWorkerTask(m_asyncLifetime, cb,
	    [data_root, metadata, prepared]()
	    {
		    *prepared = ChatRepository::PrepareLastOpenedAt(data_root, metadata);
		    return *prepared ? uam::query_handler_async::AsyncSuccess({}) : uam::query_handler_async::AsyncFailure(409, "The chat changed or could not be saved. Try again.");
	    },
	    [this, browser, data_root, metadata, prepared, previous_until, previous_interaction, previous_settled](uam::query_handler_async::AsyncCefResult& result)
	    {
		    m_snoozeRequests.erase(metadata.id);
		    if (!result.ok) return;
		    ChatSession* current = ChatDomainService().FindChatById(m_app, metadata.id);
		    std::string validation_error;
		    if (current == nullptr || m_app.data_root != data_root || current->snoozed_until_ms != previous_until ||
		        current->last_opened_at != metadata.last_opened_at || current->interaction_at != previous_interaction ||
		        current->attention_revision != metadata.attention_revision || current->settled_at != previous_settled ||
		        m_workspaceDeletionPending || m_app.worktree_operation_chat_ids.contains(metadata.id) || m_app.pending_chat_save_tasks.contains(metadata.id) ||
		        !ChatDomainService().ValidateChatSnooze(m_app, metadata.id, metadata.snoozed_until_ms, &validation_error))
		    { result = uam::query_handler_async::AsyncFailure(409, validation_error.empty() ? "The chat changed. Its current wake time was kept. Try again." : validation_error); return; }
		    if (!ChatRepository::PublishPreparedChatSave(*prepared, true, current))
		    { result = uam::query_handler_async::AsyncFailure(409, "The chat changed or could not be saved. Try again."); return; }
		    current->snoozed_until_ms = metadata.snoozed_until_ms;
		    current->interaction_at = metadata.interaction_at;
		    current->settled_at = metadata.settled_at;
		    uam::PushStateUpdateIfChanged(browser, m_app);
	    })))
	{
		m_snoozeRequests.erase(chat_id);
		cb->Failure(503, "The wake-time worker is unavailable.");
	}
}

void UamQueryHandler::HandleSetChatSettled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool settled = payload.contains("settled") && payload["settled"].is_boolean() ? payload["settled"].get<bool>() : true;
	if (!ChatDomainService().SetChatSettled(m_app, chat_id, settled))
	{
		cb->Failure(500, "Could not save chat done state.");
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleCreateSideChat(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	const std::string parent_id = payload.value("chatId", "");
	ChatSession* parent = ChatDomainService().FindChatById(m_app, parent_id);
	if (parent == nullptr || !parent->temporary_parent_chat_id.empty() || parent->imported_read_only)
	{
		cb->Failure(409, "Choose an editable main chat to start a side chat.");
		return;
	}
	const std::string key = "side-context:" + parent_id;
	const std::unordered_map<std::string, std::shared_ptr<std::stop_source>>::const_iterator previous = m_nativeHistoryRequests.find(key);
	if (previous != m_nativeHistoryRequests.end())
	{
		previous->second->request_stop();
		m_nativeHistoryRequests.erase(previous);
	}
	const bool context_loaded = parent->messages_loaded;
	std::optional<ChatSession> candidate = uam::BuildTemporarySideChatCandidate(m_app, parent_id, nullptr, !context_loaded);
	if (!candidate)
	{
		cb->Failure(409, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Could not create a side chat."));
		return;
	}
	ChatSession snapshot;
	snapshot.metadata_incarnation = parent->metadata_incarnation;
	snapshot.id = parent->id;
	snapshot.created_at = parent->created_at;
	snapshot.updated_at = parent->updated_at;
	snapshot.provider_id = parent->provider_id;
	snapshot.native_session_id = parent->native_session_id;
	snapshot.execution_host_id = parent->execution_host_id;
	snapshot.folder_id = parent->folder_id;
	snapshot.workspace_directory = parent->workspace_directory;
	snapshot.workspace_isolation_kind = parent->workspace_isolation_kind;
	snapshot.workspace_source_directory = parent->workspace_source_directory;
	snapshot.workspace_base_ref = parent->workspace_base_ref;
	snapshot.workspace_branch_name = parent->workspace_branch_name;
	snapshot.workspace_worktree_directory = parent->workspace_worktree_directory;
	snapshot.imported_read_only = parent->imported_read_only;
	snapshot.messages_loaded = false;
	snapshot.persisted_message_count = parent->persisted_message_count;
	snapshot.persisted_messages_digest = parent->persisted_messages_digest;
	const ChatDomainService::BranchHistoryIdentity identity = ChatDomainService().CaptureBranchHistoryIdentity(*parent);
	const std::string workspace = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(m_app, *parent));
	const std::filesystem::path data_root = m_app.data_root;
	const std::weak_ptr<void> lifetime = m_asyncLifetime;
	const std::shared_ptr<std::stop_source> request = std::make_shared<std::stop_source>();
	m_nativeHistoryRequests[key] = request;
	const std::shared_ptr<std::optional<uam::TemporarySideChatContext>> prepared = std::make_shared<std::optional<uam::TemporarySideChatContext>>();
	const std::shared_ptr<ChatSession> child = std::make_shared<ChatSession>(std::move(*candidate));
	const std::shared_ptr<std::string> receipt = std::make_shared<std::string>();
	const std::function<void()> cleanup = [data_root, child, receipt]()
	{
		// Independent lifetime permits stale-result cleanup after the originating query is cancelled.
		const std::shared_ptr<int> cleanup_lifetime = std::make_shared<int>(0);
		(void)RunAsyncCefQuery(cleanup_lifetime, nullptr, [cleanup_lifetime, data_root, child, receipt]()
		{
			(void)ChatRepository::DeleteTemporaryChatIfRevision(data_root, child->id, child->temporary_parent_chat_id, *receipt);
			return AsyncSuccess({});
		});
	};
	if (!RunAsyncCefQuery(m_asyncLifetime, cb,
	    [data_root, snapshot, context_loaded, child, receipt, request, prepared, lifetime]()
	    {
		    const std::function<bool()> cancelled = [request, lifetime]() { return request->stop_requested() || lifetime.expired(); };
		    if (!context_loaded)
		    {
			    *prepared = uam::PrepareTemporarySideChatContext(data_root, snapshot, cancelled);
			    if (!prepared->has_value()) return AsyncFailure(409, "The main chat context changed or could not be loaded.");
			    child->provider_handoff_context = prepared->value().reference;
		    }
		    if (cancelled()) return AsyncFailure(409, "A newer side chat context was requested.");
		    if (!ChatRepository::SaveChatIfAbsent(data_root, *child, receipt.get())) return AsyncFailure(409, "Could not save the temporary side chat.");
		    if (cancelled())
		    {
			    (void)ChatRepository::DeleteTemporaryChatIfRevision(data_root, child->id, child->temporary_parent_chat_id, *receipt);
			    return AsyncFailure(409, "A newer side chat context was requested.");
		    }
		    if (!ChatRepository::MatchesTemporaryChatSaveReceipt(data_root, child->id, *receipt))
		    {
			    (void)ChatRepository::DeleteTemporaryChatIfRevision(data_root, child->id, child->temporary_parent_chat_id, *receipt);
			    return AsyncFailure(409, "Could not save the temporary side chat.");
		    }
		    return AsyncSuccess({});
	    },
	    [this, browser, parent_id, identity, workspace, snapshot, data_root, context_loaded, key, request, prepared, child, receipt, cleanup](AsyncCefResult& result)
	    {
		    const std::unordered_map<std::string, std::shared_ptr<std::stop_source>>::const_iterator current_request = m_nativeHistoryRequests.find(key);
		    if (current_request == m_nativeHistoryRequests.end() || current_request->second != request || request->stop_requested())
		    { cleanup(); result = AsyncFailure(409, "A newer side chat context was requested."); return; }
		    m_nativeHistoryRequests.erase(current_request);
		    if (!result.ok) return;
		    const ChatSession* current = ChatDomainService().FindChatById(m_app, parent_id);
		    if (m_app.data_root != data_root || current == nullptr || current->metadata_incarnation != snapshot.metadata_incarnation ||
		        current->messages_loaded != context_loaded ||
		        current->folder_id != snapshot.folder_id || !ChatDomainService().MatchesBranchHistoryIdentity(*current, identity) ||
		        uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(m_app, *current)) != workspace ||
		        (!context_loaded && !prepared->has_value()))
		    { cleanup(); result = AsyncFailure(409, "The main chat changed while preparing the side chat. Retry."); return; }
		    if (!uam::AdoptTemporarySideChat(m_app, *child, data_root, *receipt, snapshot.metadata_incarnation, context_loaded ? nullptr : &prepared->value()))
		    { cleanup(); result = AsyncFailure(409, "The main chat changed while preparing the side chat. Retry."); return; }
		    uam::PushStateUpdateIfChanged(browser, m_app);
		    result = AsyncSuccess({{"chatId", child->id}});
	    }))
	{
		m_nativeHistoryRequests.erase(key);
	}
}

void UamQueryHandler::HandleDismissSideChat(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string id = payload.value("chatId", "");
	const ChatSession* side = ChatDomainService().FindChatById(m_app, id);
	if (side == nullptr || side->temporary_parent_chat_id.empty())
	{
		cb->Failure(400, "Choose a temporary side chat to dismiss.");
		return;
	}
	const std::string parent_id = side->temporary_parent_chat_id;
	if (!uam::RequestTemporarySideChatCleanup(m_app, id))
	{
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Could not dismiss the side chat."));
		return;
	}
	if (ChatDomainService().SelectedChatId(m_app) == id)
		ChatDomainService().SelectChatById(m_app, parent_id);
	(void)uam::PollTemporarySideChatCleanup(m_app);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"parentChatId", parent_id}, {"cleanupPending", ChatDomainService().FindChatById(m_app, id) != nullptr}}.dump());
}
