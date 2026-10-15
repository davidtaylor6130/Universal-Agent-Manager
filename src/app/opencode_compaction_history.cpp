#include "app/opencode_compaction_history.h"

#include "app/chat_domain_service.h"
#include "app/provider_worker_command.h"
#include "app/runtime_orchestration_internal.h"
#include "common/chat/chat_repository.h"
#include "common/config/execution_host_config.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/utils/diagnostic_log.h"
#include "remote/runner_client.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <set>
#include <thread>

namespace uam
{
std::string OpenCodeCompactionProofSql()
{
	// Native compaction can retain an ordinary text-only user parent.
	return "json_extract(m.data,'$.role')='assistant' AND json_extract(m.data,'$.summary')=1 AND "
	    "EXISTS(SELECT 1 FROM message u WHERE u.id=json_extract(m.data,'$.parentID') AND u.session_id=m.session_id "
	    "AND json_extract(u.data,'$.role')='user' AND ("
	    "(json_extract(m.data,'$.agent')='compaction' AND json_extract(m.data,'$.mode')='compaction') OR "
	    "EXISTS(SELECT 1 FROM part c WHERE c.message_id=u.id AND c.session_id=u.session_id "
	    "AND json_extract(c.data,'$.type')='compaction')))";
}

namespace
{
	constexpr std::size_t kPrimaryBytes = 8 * 1024 * 1024;
	constexpr std::size_t kQueryBytes = 4 * 1024 * 1024;
	constexpr std::size_t kPartLimit = 4096;
	constexpr int kQueryTimeoutMs = 30000;

	struct Identity
	{
		std::filesystem::path root;
		std::shared_ptr<const std::uint8_t> incarnation;
		std::string chat, native, workspace, created, updated, digest, process, process_handle;
		std::size_t count = 0;
		const AcpSessionState* runtime = nullptr;
		int turn = 0, assistant = -1;
		bool running = false;
		std::vector<std::string> command;
		std::optional<ExecutionHost> host;
		bool operator==(const Identity& other) const
		{
			return root == other.root && incarnation == other.incarnation && chat == other.chat &&
			    native == other.native && workspace == other.workspace && created == other.created &&
			    updated == other.updated && digest == other.digest && count == other.count &&
			    runtime == other.runtime && process == other.process && process_handle == other.process_handle &&
			    running == other.running && turn == other.turn &&
			    assistant == other.assistant && command == other.command && host.has_value() == other.host.has_value() &&
			    (!host || execution_hosts::SameConnection(*host, *other.host));
		}
	};

	bool Idle(const AppState& app, const ChatSession& chat)
	{
		// An empty held queue is a dispatch policy, not pending work; classification preserves it.
		if (chat.provider_id != provider_ids::kOpenCodeCli || chat.native_session_id.empty() ||
		    chat.imported_read_only || chat.side_cleanup_requested || chat.native_session_reset_pending ||
		    chat.acp_queue_storage_error || !chat.acp_queued_prompts.empty() ||
		    chat.acp_dispatched_queued_prompt_count != 0 || chat.remote_turn_reconnect_pending ||
		    chat.remote_stop_cleanup_pending || chat.remote_restart_pending || chat.remote_source_exit_pending ||
		    !chat.remote_active_turn_id.empty() || !chat.remote_prompt_delivery_id.empty() ||
		    !chat.remote_prompt_delivery_session_id.empty() || !chat.remote_prompt_delivery_payload.empty() ||
		    !chat.remote_pending_requests.empty() || !chat.remote_interaction_responses.empty() ||
		    app.pending_chat_save_tasks.contains(chat.id) || app.worktree_launch_preparations.contains(chat.id) ||
		    runtime_orch_impl::ChatNeedsTranscriptPreserved(app, chat) || ChatHasActiveCliTerminal(app, chat.id)) return false;
		const AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
		if (session && (session->provider_id != chat.provider_id ||
		    (!session->session_id.empty() && session->session_id != chat.native_session_id) ||
		    AcpSessionHasBlockingRuntimeWork(*session) || AcpSessionHasPendingCancel(*session) ||
		    session->manual_compaction_pending || session->context_compaction_in_progress ||
		    session->reconnect_pending || session->recovering_remote_turn || session->recovering_remote_process ||
		    session->local_stop_pending || session->remote_stop_pending || session->remote_stop_unconfirmed ||
		    session->pending_prompt_delivery || session->pending_local_launch || session->pending_local_session_receipt ||
		    session->pending_agent_input || session->pending_queued_history || session->worktree_launch_pending ||
		    session->ignore_session_updates_until_ready || !session->load_history_replay_updates.empty() ||
		    session->model_discovery_only || session->goal_internal_session || session->goal_review_scheduled ||
		    session->turn_checkpoint_commit_pending || session->turn_checkpoint_preflight_pending ||
		    !session->pending_request_methods.empty() || !session->pending_steer_requests.empty())) return false;
		return true;
	}

	bool PortableId(std::string_view value)
	{
		return !value.empty() && value.size() <= 512 && !value.starts_with('-') && execution_hosts::IsPortableId(value);
	}

	std::optional<Identity> Capture(const AppState& app, const ChatSession& chat)
	{
		if (!Idle(app, chat) || !PortableId(chat.native_session_id)) return {};
		const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, chat.provider_id);
		if (!profile) return {};
		Identity identity;
		identity.root = app.data_root; identity.incarnation = chat.metadata_incarnation;
		identity.chat = chat.id; identity.native = chat.native_session_id; identity.created = chat.created_at;
		identity.updated = chat.updated_at; identity.digest = chat.persisted_messages_digest;
		identity.count = chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count;
		if (identity.count == 0) return {};
		identity.workspace = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
		identity.command = provider_runtime_internal::SplitInteractiveCommandOrDefault(*profile, "opencode");
		if (identity.command.empty()) return {};
		if (!paths::IsControllerLocalWorkspace(chat))
		{
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
			if (!host || host->transport != "ssh" || host->runner_protocol_version < 3 ||
			    !remote::CanAttemptRunnerConnection(*host) || !execution_hosts::IsAbsoluteRemotePath(host->platform, identity.workspace)) return {};
			identity.host = *host;
		}
		else if (!paths::PathFromUtf8(identity.workspace).is_absolute()) return {};
		identity.runtime = FindAcpSessionForChat(app, chat.id);
		if (identity.runtime)
		{
			identity.process = identity.runtime->last_process_id;
			identity.process_handle = acp_detail::AcpProcessHandleLabel(*identity.runtime);
			identity.running = identity.runtime->running;
			identity.turn = identity.runtime->turn_serial;
			identity.assistant = identity.runtime->turn_assistant_message_index;
		}
		return identity;
	}

	std::string SqlLiteral(std::string_view value)
	{
		std::string result = "'";
		for (const char character : value)
		{
			result += character;
			if (character == '\'') result += '\'';
		}
		return result + "'";
	}

	struct Checkpoint
	{
		std::size_t message = 0, block = 0;
		std::string id, text, previous_tool;
		nlohmann::json prefix_counts = nlohmann::json::object();
	};

	/// <summary>Captures a completed saved boundary; native SQL must still prove its identity and prefix coverage.</summary>
	std::optional<Checkpoint> SavedCheckpoint(const ChatSession& chat)
	{
		std::optional<Checkpoint> checkpoint;
		nlohmann::json counts = nlohmann::json::object();
		std::string previous_tool;
		for (std::size_t message = 0; message < chat.messages.size(); ++message)
		{
			if (chat.messages[message].role != MessageRole::Assistant) continue;
			for (std::size_t block = 0; block < chat.messages[message].blocks.size(); ++block)
			{
				const MessageBlock& value = chat.messages[message].blocks[block];
				if (value.type == "tool_call") previous_tool = value.tool_call_id;
				if ((value.type != "context_compaction" && value.type != "context_compaction_interrupted") || value.text.empty()) continue;
				const std::string id = value.request_id_json.starts_with("acp-compaction:") ? value.request_id_json.substr(15) : value.request_id_json;
				if (!PortableId(id)) return {};
				counts[id] = counts.value(id, 0) + 1;
				if (value.type == "context_compaction" && value.text.size() <= 64 * 1024 && value.text.find('\0') == std::string::npos &&
				    (previous_tool.empty() || PortableId(previous_tool)))
					checkpoint = Checkpoint{message, block, id, value.text, previous_tool, counts};
			}
		}
		return checkpoint;
	}

	std::string HistorySql(const Identity& identity, const std::optional<Checkpoint>& checkpoint = {})
	{
		std::string prefix;
		if (checkpoint)
		{
			prefix = "candidate AS (SELECT m.time_created created,m.id message_id,p.id part_id FROM message m JOIN part p "
			    "ON p.message_id=m.id AND p.session_id=m.session_id WHERE m.session_id=" + SqlLiteral(identity.native) +
			    " AND m.id=" + SqlLiteral(checkpoint->id) + " AND json_extract(m.data,'$.role')='assistant' AND " +
			    OpenCodeCompactionProofSql() + " AND json_extract(p.data,'$.type')='text' AND json_extract(p.data,'$.text')=" +
			    SqlLiteral(checkpoint->text) + " AND json_type(m.data,'$.finish')='text' "
			    "AND length(CAST(json_extract(m.data,'$.finish') AS BLOB)) BETWEEN 1 AND 128 AND coalesce(json_type(m.data,'$.error'),'null')='null'),"
			    "checkpoint AS (SELECT c.* FROM candidate c WHERE (SELECT count(*) FROM candidate)=1 AND NOT EXISTS("
			    "SELECT m.id FROM message m JOIN part p ON p.message_id=m.id AND p.session_id=m.session_id "
			    "WHERE m.session_id=" + SqlLiteral(identity.native) + " AND json_extract(m.data,'$.role')='assistant' "
			    "AND json_extract(m.data,'$.summary')=1 AND json_extract(p.data,'$.type')='text' "
			    "AND (m.time_created,m.id,p.id)<=(c.created,c.message_id,c.part_id) GROUP BY m.id "
			    "HAVING count(*)!=coalesce((SELECT value FROM json_each(" + SqlLiteral(checkpoint->prefix_counts.dump()) +
			    ") WHERE key=m.id),0)) AND coalesce((SELECT json_extract(p.data,'$.callID') FROM message m JOIN part p "
			    "ON p.message_id=m.id AND p.session_id=m.session_id WHERE m.session_id=" + SqlLiteral(identity.native) +
			    " AND json_extract(m.data,'$.role')='assistant' AND json_extract(p.data,'$.type')='tool' "
			    "AND (m.time_created,m.id,p.id)<(c.created,c.message_id,c.part_id) "
			    "ORDER BY m.time_created DESC,m.id DESC,p.id DESC LIMIT 1),'')=" + SqlLiteral(checkpoint->previous_tool);
			if (!checkpoint->previous_tool.empty())
				prefix += " AND (SELECT count(*) FROM part WHERE session_id=" + SqlLiteral(identity.native) +
				    " AND json_extract(data,'$.type')='tool' AND json_extract(data,'$.callID')=" + SqlLiteral(checkpoint->previous_tool) + ")=1";
			prefix += "),";
		}
		return "WITH " + prefix + "selected AS (SELECT m.id message_id,p.id part_id,json_extract(p.data,'$.type') type,"
		    "CASE WHEN json_extract(p.data,'$.type')='text' THEN json_extract(p.data,'$.text') ELSE NULL END text,"
		    "CASE WHEN json_extract(p.data,'$.type')='tool' THEN json_extract(p.data,'$.callID') ELSE NULL END tool_id,"
		    "coalesce(json_extract(m.data,'$.summary'),0) summary,json_extract(m.data,'$.finish') finish,"
		    "json_type(m.data,'$.error') error_type,CASE WHEN " + OpenCodeCompactionProofSql() + " THEN 1 ELSE 0 END parent_proof,"
		    "NOT EXISTS(SELECT 1 FROM part tail WHERE tail.message_id=m.id AND tail.session_id=m.session_id "
		    "AND tail.id>p.id AND json_extract(tail.data,'$.type') IN ('text','tool')) native_tail,"
		    "CASE WHEN json_extract(m.data,'$.summary')=1 AND json_type(m.data,'$.error')='object' THEN "
		    "NOT EXISTS(SELECT 1 FROM message n JOIN part np ON np.message_id=n.id AND np.session_id=n.session_id "
		    "WHERE n.session_id=m.session_id AND json_extract(n.data,'$.role')='assistant' "
		    "AND json_extract(np.data,'$.type') IN ('text','tool') AND (n.time_created,n.id)>(m.time_created,m.id) "
		    "AND NOT EXISTS(SELECT 1 FROM message fresh WHERE fresh.session_id=m.session_id "
		    "AND json_extract(fresh.data,'$.role')='user' AND (fresh.time_created,fresh.id)>(m.time_created,m.id) "
		    "AND (fresh.time_created,fresh.id)<(n.time_created,n.id) "
		    "AND EXISTS(SELECT 1 FROM part ft WHERE ft.message_id=fresh.id AND ft.session_id=fresh.session_id "
		    "AND json_extract(ft.data,'$.type')='text') "
		    "AND NOT EXISTS(SELECT 1 FROM part fc WHERE fc.message_id=fresh.id AND fc.session_id=fresh.session_id "
		    "AND json_extract(fc.data,'$.type')='compaction'))) ELSE 0 END turn_ended "
		    "FROM message m JOIN part p ON p.message_id=m.id AND p.session_id=m.session_id WHERE m.session_id=" + SqlLiteral(identity.native) +
		    " AND json_extract(m.data,'$.role')='assistant' AND json_extract(p.data,'$.type') IN ('text','tool') " +
		    (checkpoint ? "AND (NOT EXISTS(SELECT 1 FROM checkpoint) OR (m.time_created,m.id,p.id)>(SELECT created,message_id,part_id FROM checkpoint)) " : "") +
		    "ORDER BY m.time_created,m.id,p.id LIMIT " + std::to_string(kPartLimit + 1) +
		    ") SELECT s.id sessionId,s.directory," + (checkpoint ? "(SELECT count(*) FROM checkpoint)" : "0") +
		    " checkpointProof,(SELECT json_group_array(json_object('messageId',message_id,"
		    "'partId',part_id,'type',type,'text',text,'toolId',tool_id,'summary',summary,'parentProof',parent_proof,"
		    "'finish',finish,'errorType',error_type,'nativeTail',native_tail,'turnEnded',turn_ended)) FROM selected) rows FROM session s WHERE s.id=" + SqlLiteral(identity.native);
	}

	ProcessExecutionResult Execute(const OpenCodeCompactionHistoryService::Query& query, std::stop_token stop)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
		const auto remaining = [&]()
		{
			return static_cast<int>(std::clamp<std::int64_t>(
			    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count(), 0, kQueryTimeoutMs));
		};
		if (query.host)
		{
			const ExecutionHost& host = *query.host;
			remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
			    remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version),
			    host.runner_version, host.runner_protocol_version);
			return OpenCodeCompactionHistoryService::CollectBoundedPages(query,
			    [&](const OpenCodeCompactionHistoryService::Query& page, std::stop_token cancellation)
			{
				const std::string nonce = PlatformServicesFactory::Instance().process_service.GenerateUuid();
				if (nonce.empty()) return ProcessExecutionResult{.error = "History metadata query identity is unavailable."};
				return client.ExecuteCommand("compaction-history-" + nonce, page.workspace, page.argv, remaining(), cancellation);
			}, stop);
		}
		return OpenCodeCompactionHistoryService::CollectBoundedPages(query,
		    [&](const OpenCodeCompactionHistoryService::Query& page, std::stop_token cancellation)
		{
			ProviderWorkerInvocation invocation;
			invocation.direct_process = invocation.structured_stdout = true;
			invocation.argv = page.argv;
			return ExecuteProviderWorkerInvocation(invocation, page.workspace, remaining(), cancellation);
		}, stop);
	}

	struct Change
	{
		std::size_t message = 0, block = 0;
		std::string id;
		bool remove = false;
		std::string type = "context_compaction";
	};
	struct NativeText
	{
		std::string text, summary;
		bool verified = true;
		bool interrupted = false;
		bool native_tail = false;
		bool turn_ended = false;
	};
	struct StoredText
	{
		std::size_t message = 0, block = 0;
	};

	std::optional<std::vector<Change>> Classify(ChatSession& chat, const Identity& identity, const ProcessExecutionResult& result, const std::optional<Checkpoint>& checkpoint = {})
	{
		if (!result.ok || result.exit_code != 0 || result.canceled || result.timed_out || result.output_truncated || result.output.size() > kQueryBytes) return {};
		const nlohmann::json output = nlohmann::json::parse(result.output);
		if (!output.is_array() || output.size() != 1 || !output.front().is_object()) return {};
		const nlohmann::json& envelope = output.front();
		if (envelope.at("sessionId") != identity.native || !envelope.at("directory").is_string() || !envelope.at("rows").is_string()) return {};
		const std::string directory = envelope.at("directory").get<std::string>();
		const bool workspace_matches = identity.host ? execution_hosts::RemotePathsMatch(identity.host->platform, directory, identity.workspace)
		    : paths::PathFromUtf8(directory).lexically_normal() == paths::PathFromUtf8(identity.workspace).lexically_normal();
		if (!workspace_matches) return {};
		const nlohmann::json rows = nlohmann::json::parse(envelope.at("rows").get<std::string>());
		if (!rows.is_array() || rows.size() > kPartLimit) return {};
		const int checkpoint_proof = envelope.value("checkpointProof", 0);
		if (checkpoint_proof != 0 && checkpoint_proof != 1) return {};
		const bool resumed = checkpoint_proof == 1;
		if (resumed && !checkpoint) return {};
		std::vector<std::string> native_tools, stored_tools;
		const bool anchored = resumed && !checkpoint->previous_tool.empty();
		if (anchored) { native_tools.push_back(checkpoint->previous_tool); stored_tools.push_back(checkpoint->previous_tool); }
		std::vector<std::vector<NativeText>> native_segments(anchored ? 2 : 1);
		std::vector<std::vector<StoredText>> stored_segments(anchored ? 2 : 1);
		std::set<std::string> parts, tools;
		if (anchored) tools.insert(checkpoint->previous_tool);
		for (const nlohmann::json& row : rows)
		{
			const std::string message = row.at("messageId").get<std::string>();
			const std::string part = row.at("partId").get<std::string>();
			if (!PortableId(message) || !PortableId(part) || !parts.insert(part).second || !row.at("summary").is_number_integer()) return {};
			const int summary = row.at("summary").get<int>();
			if (summary != 0 && summary != 1) return {};
			const std::string type = row.at("type").get<std::string>();
			if (type == "tool")
			{
				const std::string tool = row.at("toolId").get<std::string>();
				if (summary || !PortableId(tool) || !tools.insert(tool).second) return {};
				native_tools.push_back(tool); native_segments.emplace_back();
			}
			else if (type == "text")
			{
				NativeText text{row.at("text").get<std::string>(), ""};
				if (summary)
				{
					const nlohmann::json& finish = row.at("finish");
					const bool parent_proven = !text.text.empty() && row.at("parentProof") == 1;
					text.summary = message;
					text.verified = parent_proven && finish.is_string() && !finish.get_ref<const std::string&>().empty() &&
					    finish.get_ref<const std::string&>().size() <= 128 && (row.at("errorType").is_null() || row.at("errorType") == "null");
					text.interrupted = parent_proven && finish.is_null() && row.at("errorType") == "object";
					text.native_tail = row.contains("nativeTail") && row.at("nativeTail") == 1;
					text.turn_ended = row.contains("turnEnded") && row.at("turnEnded") == 1;
				}
				native_segments.back().push_back(std::move(text));
			}
			else return {};
		}
		tools.clear();
		if (anchored) tools.insert(checkpoint->previous_tool);
		for (std::size_t message = resumed ? checkpoint->message : 0; message < chat.messages.size(); ++message)
		{
			const Message& row = chat.messages[message];
			if (row.role != MessageRole::Assistant) continue;
			if (row.blocks.empty() && (!row.content.empty() || !row.tool_calls.empty())) return {};
			for (std::size_t block = resumed && message == checkpoint->message ? checkpoint->block + 1 : 0; block < row.blocks.size(); ++block)
			{
				const MessageBlock& value = row.blocks[block];
				if (value.type == "tool_call")
				{
					if (!PortableId(value.tool_call_id) || !tools.insert(value.tool_call_id).second) return {};
					stored_tools.push_back(value.tool_call_id); stored_segments.emplace_back();
				}
				else if (value.type == "assistant_text" ||
				    ((value.type == "context_compaction" || value.type == "context_compaction_interrupted") && !value.text.empty()))
					stored_segments.back().push_back({message, block});
			}
		}
		// shortcut: differently chunked summaries stay unchanged until native message IDs are persisted.
		if (native_tools != stored_tools) return {};
		std::vector<Change> changes;
		std::set<std::string> proven_summaries;
		// Each tool-delimited segment is atomic: an interrupted or unmatched later segment
		// must not discard independent, fully proven earlier summaries.
		for (std::size_t segment = 0; segment < native_segments.size(); ++segment)
		{
			const std::vector<NativeText>& native = native_segments[segment];
			if (!std::ranges::any_of(native, [](const NativeText& text) { return !text.summary.empty(); })) continue;
			const std::vector<StoredText>& stored = stored_segments[segment];
			if (native.size() != stored.size()) continue;
			std::vector<Change> segment_changes;
			std::set<std::string> segment_summaries;
			bool matched = true, exact_prefix = true;
			for (std::size_t index = 0; index < native.size(); ++index)
			{
				const StoredText& position = stored[index];
				const Message& assistant = chat.messages[position.message];
				const MessageBlock& block = assistant.blocks[position.block];
				const NativeText& source = native[index];
				if (source.summary.empty())
				{
					if (block.type != "assistant_text" || strings::Trim(block.text) != strings::Trim(source.text))
					{ matched = false; break; }
					exact_prefix = exact_prefix && block.text == source.text;
					continue;
				}
				std::string type = "context_compaction";
				if (!source.verified)
				{
					// A failed native compaction can outlive the ACP stream. Fold only its
					// proven nonempty prefix at the interrupted assistant's terminal output.
					bool terminal = assistant.interrupted && segment > 0 && exact_prefix && source.interrupted &&
					    source.native_tail && source.turn_ended && !block.text.empty() && block.text.size() < source.text.size() &&
					    source.text.starts_with(block.text);
					for (std::size_t following = position.block + 1; terminal && following < assistant.blocks.size(); ++following)
						if (!assistant.blocks[following].text.empty() || !assistant.blocks[following].tool_call_id.empty()) terminal = false;
					for (std::size_t following = position.message + 1; terminal && following < chat.messages.size(); ++following)
					{
						const Message& next = chat.messages[following];
						if (next.role == MessageRole::User && !next.priority_steer && !next.continues_turn) break;
						if (next.role == MessageRole::Assistant && (!next.blocks.empty() || !next.content.empty() || !next.tool_calls.empty())) terminal = false;
					}
					if (!terminal) { matched = false; break; }
					type = "context_compaction_interrupted";
				}
				else if (block.text != source.text) { matched = false; break; }
				const std::string& id = source.summary;
				if (block.type == "context_compaction" || block.type == "context_compaction_interrupted")
				{
					if (block.type != type || (block.request_id_json != id && block.request_id_json != "acp-compaction:" + id))
					{ matched = false; break; }
				}
				else segment_changes.push_back({position.message, position.block, id, false, type});
				segment_summaries.insert(id);
				exact_prefix = exact_prefix && block.text == source.text;
			}
			if (!matched) continue;
			changes.insert(changes.end(), segment_changes.begin(), segment_changes.end());
			proven_summaries.insert(segment_summaries.begin(), segment_summaries.end());
		}
		for (std::size_t message = resumed ? checkpoint->message : 0; message < chat.messages.size(); ++message)
			for (std::size_t block = resumed && message == checkpoint->message ? checkpoint->block + 1 : 0; block < chat.messages[message].blocks.size(); ++block)
			{
				const MessageBlock& value = chat.messages[message].blocks[block];
				const std::string id = value.request_id_json.starts_with("acp-compaction:") ? value.request_id_json.substr(15) : value.request_id_json;
				if (value.type == "context_compaction" && value.text.empty() && proven_summaries.contains(id))
					changes.push_back({message, block, id, true});
			}
		std::ranges::sort(changes, [](const Change& left, const Change& right)
		{ return left.message > right.message || (left.message == right.message && left.block > right.block); });
		for (const Change& change : changes)
		{
			if (change.remove)
			{
				auto& blocks = chat.messages[change.message].blocks;
				blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(change.block));
				continue;
			}
			MessageBlock& block = chat.messages[change.message].blocks[change.block];
			block.type = change.type; block.request_id_json = change.id;
		}
		return changes;
	}
}

ProcessExecutionResult OpenCodeCompactionHistoryService::CollectBoundedPages(const Query& query,
    const ExecuteQuery& execute, std::stop_token stop)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
	const auto failure = [&](std::string error)
	{
		ProcessExecutionResult result;
		result.error = std::move(error);
		result.canceled = stop.stop_requested();
		result.timed_out = !result.canceled && std::chrono::steady_clock::now() >= deadline;
		return result;
	};
	const auto interrupted = [&]() { return stop.stop_requested() || std::chrono::steady_clock::now() >= deadline; };
	if (interrupted()) return failure("Native history query was interrupted.");
	ProcessExecutionResult direct = execute(query, stop);
	if (interrupted()) return failure("Native history query was interrupted.");
	if (!direct.ok || direct.exit_code != 0 || direct.canceled || direct.timed_out || direct.output_truncated || direct.output.size() > kQueryBytes) return direct;
	try
	{
		const nlohmann::json parsed = nlohmann::json::parse(direct.output);
		if (parsed.is_array() && parsed.size() == 1 && parsed.front().is_object() &&
		    parsed.front().at("sessionId").is_string() && parsed.front().at("directory").is_string() &&
		    parsed.front().at("rows").is_string() && nlohmann::json::parse(parsed.front().at("rows").get<std::string>()).is_array()) return direct;
	}
	catch (const nlohmann::json::exception&) {}

	// OpenCode console.log is followed by process.exit; large pipe output can be lost with exit code zero.
	const bool windows = query.host ? (query.host->platform == "windows" || query.host->platform == "Windows") :
#if defined(_WIN32)
	    true;
#else
	    false;
#endif
	const bool linux_host = query.host && (query.host->platform == "linux" || query.host->platform == "Linux");
	const std::size_t chunk_size = windows ? 256 : linux_host ? 8192 : 2048;
	constexpr std::size_t kMaximumPages = 4096;
	std::string rows, session, directory, revision;
	int checkpoint_proof = 0;
	std::size_t total = 0, offset = 1;
	try
	{
		for (std::size_t page_index = 0; page_index < kMaximumPages; ++page_index)
		{
			if (interrupted()) return failure("Native history pages were interrupted.");
			Query page = query;
			page.sql = "SELECT h.sessionId,h.directory," + std::string(query.checkpoint_projection ? "h.checkpointProof" : "0") +
			    " checkpointProof,length(h.rows) total," + std::to_string(offset) +
			    " offset,substr(h.rows," + std::to_string(offset) + "," + std::to_string(chunk_size) + ") chunk,"
			    "json_array((SELECT json_array(count(*),coalesce(max(time_updated),0),coalesce(sum(time_updated),0),max(id)) "
			    "FROM message WHERE session_id=h.sessionId),(SELECT json_array(count(*),coalesce(max(time_updated),0),"
			    "coalesce(sum(time_updated),0),max(id)) FROM part WHERE session_id=h.sessionId)) revision FROM (" + query.sql + ") h";
			const auto sql_argument = std::ranges::find(page.argv, query.sql);
			if (sql_argument == page.argv.end()) return failure("Native history query arguments could not be verified.");
			*sql_argument = page.sql;
			const ProcessExecutionResult result = execute(page, stop);
			if (interrupted()) return failure("Native history pages were interrupted.");
			if (!result.ok || result.exit_code != 0 || result.canceled || result.timed_out || result.output_truncated || result.output.size() > kQueryBytes)
			{
				ProcessExecutionResult failed = failure("Native history page could not be completely read. " + result.error);
				failed.exit_code = result.exit_code;
				failed.canceled = failed.canceled || result.canceled;
				failed.timed_out = failed.timed_out || result.timed_out;
				failed.output_truncated = result.output_truncated;
				return failed;
			}
			const nlohmann::json output = nlohmann::json::parse(result.output);
			if (!output.is_array() || output.size() != 1 || !output.front().is_object()) return failure("Native history page envelope is invalid.");
			const auto& envelope = output.front();
			if (!envelope.at("total").is_number_unsigned() && !envelope.at("total").is_number_integer()) return failure("Native history page size is invalid.");
			const auto length = envelope.at("total").get<std::int64_t>();
			if (length < 0 || length > static_cast<std::int64_t>(kQueryBytes) || envelope.at("offset") != offset)
				return failure("Native history page exceeds its limits or position.");
			const std::string page_session = envelope.at("sessionId").get<std::string>();
			const std::string page_directory = envelope.at("directory").get<std::string>();
			const std::string page_revision = envelope.at("revision").get<std::string>();
			const int page_checkpoint = envelope.value("checkpointProof", 0);
			if (page_checkpoint != 0 && page_checkpoint != 1) return failure("Native history checkpoint proof is invalid.");
			if (page_session.empty() || page_session.size() > 512 || page_directory.empty() || page_directory.size() > 16384 ||
			    page_revision.empty() || page_revision.size() > 4096) return failure("Native history page identity is invalid.");
			if (page_index == 0)
			{
				total = static_cast<std::size_t>(length); session = page_session;
				directory = page_directory; revision = page_revision; checkpoint_proof = page_checkpoint;
				if (total > chunk_size * kMaximumPages) return failure("Native history requires too many pages.");
			}
			else if (total != static_cast<std::size_t>(length) || session != page_session || directory != page_directory || revision != page_revision || checkpoint_proof != page_checkpoint)
				return failure("Native history changed between pages.");
			const std::string chunk = envelope.at("chunk").get<std::string>();
			if (rows.size() + chunk.size() > kQueryBytes) return failure("Native history text exceeded its byte limit.");
			const std::size_t expected = std::min(chunk_size, total - (offset - 1));
			const std::size_t characters = std::ranges::count_if(chunk, [](unsigned char character) { return (character & 0xc0) != 0x80; });
			if (characters != expected) return failure("Native history page text is incomplete.");
			rows += chunk;
			if (offset - 1 + expected == total)
			{
				if (!nlohmann::json::parse(rows).is_array()) return failure("Native history reconstruction is invalid.");
				direct.output = nlohmann::json::array({{{"sessionId", session}, {"directory", directory}, {"checkpointProof", checkpoint_proof}, {"rows", rows}}}).dump();
				if (direct.output.size() > kQueryBytes) return failure("Native history envelope exceeded its byte limit.");
				return direct;
			}
			offset += expected;
		}
	}
	catch (const nlohmann::json::exception&) { return failure("Native history page JSON is incomplete or malformed."); }
	return failure("Native history page limit was exceeded.");
}

struct OpenCodeCompactionHistoryService::State
{
	struct Work
	{
		Identity identity;
		std::string fingerprint;
		bool loaded = false;
		std::size_t persisted_count = 0;
		std::string persisted_digest;
		std::vector<Message> original;
		std::optional<ChatSession> repaired;
		std::vector<Change> changes;
		std::shared_ptr<PreparedChatSave> prepared;
		std::atomic<bool> finished{false};
		std::stop_source stop;
		bool stale = false;
		std::string error;
	};
	ExecuteQuery execute;
	std::map<std::string, Identity> attempted;
	struct Retry
	{
		Identity identity;
		std::string fingerprint;
		int count = 0;
		std::chrono::steady_clock::time_point after;
	};
	std::map<std::string, Retry> retries;
	std::shared_ptr<Work> active;
	std::unique_ptr<std::jthread> worker;
	bool stopped = false;

	explicit State(ExecuteQuery executor) : execute(executor ? std::move(executor) : Execute) {}

	void Start(const AppState& app, const ChatSession& chat, Identity identity)
	{
		auto work = std::make_shared<Work>();
		work->identity = std::move(identity);
		work->fingerprint = ChatRepository::ChatMetadataFingerprint(app.data_root, chat);
		work->loaded = chat.messages_loaded; work->persisted_count = chat.persisted_message_count;
		work->persisted_digest = chat.persisted_messages_digest;
		attempted[chat.id] = work->identity;
		active = work;
		worker = std::make_unique<std::jthread>([work, executor = execute]()
		{
			try
			{
				const auto prepare = [&]()
				{
					const Identity& identity = work->identity;
					const auto revision = ChatRepository::ChatStorageRevision(identity.root, identity.chat);
					const auto summary_revision = ChatRepository::ChatSummaryStorageRevision(identity.root, identity.chat);
					if (!revision || !summary_revision || work->stop.stop_requested()) return;
					auto chat = ChatRepository::LoadLocalChatBounded(identity.root, identity.chat, kPrimaryBytes);
					if (ChatRepository::ChatStorageRevision(identity.root, identity.chat) != revision ||
					    ChatRepository::ChatSummaryStorageRevision(identity.root, identity.chat) != summary_revision)
					{ work->stale = true; return; }
					if (!chat) return;
					chat->messages_loaded = work->loaded;
					chat->persisted_message_count = work->persisted_count; chat->persisted_messages_digest = work->persisted_digest;
					if (ChatRepository::ChatMetadataFingerprint(identity.root, *chat) != work->fingerprint)
					{ work->stale = true; return; }
					chat->messages_loaded = true;
					work->original = chat->messages;
					Query query;
					query.workspace = paths::PathFromUtf8(identity.workspace); query.host = identity.host;
					std::optional<Checkpoint> checkpoint = SavedCheckpoint(*chat);
					query.sql = HistorySql(identity, checkpoint);
					const std::size_t maximum_sql = identity.host && (identity.host->platform == "windows" || identity.host->platform == "Windows") ? 24 * 1024 : 64 * 1024;
					if (query.sql.size() > maximum_sql) { checkpoint.reset(); query.sql = HistorySql(identity); }
					query.checkpoint_projection = checkpoint.has_value();
					query.argv = identity.command;
					query.argv.insert(query.argv.end(), {"db", query.sql, "--format", "json", "--pure"});
					const ProcessExecutionResult result = executor(query, work->stop.get_token());
					if (work->stop.stop_requested()) return;
					if (!result.ok || result.exit_code != 0 || result.canceled || result.timed_out || result.output_truncated || result.output.size() > kQueryBytes)
					{
						std::string error = result.error.substr(0, 512);
						std::replace(error.begin(), error.end(), '\n', ' ');
						std::replace(error.begin(), error.end(), '\r', ' ');
						work->error = "Native history query failed. [chat=" + identity.chat + ", native=" + identity.native +
						    ", exit=" + std::to_string(result.exit_code) + ", timeout=" + std::to_string(result.timed_out) +
						    ", canceled=" + std::to_string(result.canceled) + ", truncated=" + std::to_string(result.output_truncated) +
						    ", outputBytes=" + std::to_string(result.output.size()) + "] " + error;
						return;
					}
					const auto changes = Classify(*chat, identity, result, checkpoint);
					if (!changes)
					{
						work->error = "Native summary metadata did not match the saved transcript. [chat=" + identity.chat + ", native=" + identity.native + "]";
						return;
					}
					if (changes->empty()) return;
					work->changes = *changes;
					work->prepared = ChatRepository::PrepareChatSave(identity.root, *chat, *revision);
					if (!work->prepared) { work->stale = true; work->error = "Reclassified history could not be prepared."; return; }
					work->prepared->expected_summary_revision = *summary_revision;
					work->repaired = std::move(chat);
				};
				prepare();
			}
			catch (const std::exception&) { work->error = "Native history classification could not be verified."; }
			work->finished.store(true, std::memory_order_release);
		});
	}

	bool Publish(AppState& app, Work& work)
	{
		ChatSession* chat = ChatDomainService().FindChatById(app, work.identity.chat);
		const auto owns = [&]()
		{
			const auto current = chat ? Capture(app, *chat) : std::nullopt;
			return current && *current == work.identity && ChatRepository::ChatMetadataFingerprint(app.data_root, *chat) == work.fingerprint &&
			    (!chat->messages_loaded || chat->messages == work.original);
		};
		if (!owns()) { work.stale = true; return false; }
		AcpSessionState* session = FindAcpSessionForChat(app, chat->id);
		if (session && session->turn_assistant_message_index >= 0 && !session->turn_events.empty())
		{
			const std::size_t owner = static_cast<std::size_t>(session->turn_assistant_message_index);
			if (owner >= work.original.size())
			{ work.error = "History classification skipped an invalid retained assistant owner."; return false; }
			const auto& blocks = work.original[owner].blocks;
			if (session->turn_events.size() != blocks.size())
			{ work.error = "History classification skipped retained events with different block counts."; return false; }
			for (std::size_t index = 0; index < blocks.size(); ++index)
			{
				const AcpTurnEventState& event = session->turn_events[index];
				const MessageBlock& block = blocks[index];
				if (event.type != block.type || event.text != block.text || event.tool_call_id != block.tool_call_id ||
				    event.request_id_json != block.request_id_json)
				{ work.error = "History classification skipped retained events that differ from saved blocks."; return false; }
			}
		}
		if (!ChatRepository::PublishPreparedChatSave(work.prepared, true, chat, owns)) { work.stale = true; return false; }
		if (session && session->turn_assistant_message_index >= 0 && !session->turn_events.empty())
		{
			for (const Change& change : work.changes)
				if (change.message == static_cast<std::size_t>(session->turn_assistant_message_index))
				{
					if (change.remove)
					{
						session->turn_events.erase(session->turn_events.begin() + static_cast<std::ptrdiff_t>(change.block));
						continue;
					}
					AcpTurnEventState& event = session->turn_events[change.block];
					event.type = change.type; event.request_id_json = change.id;
				}
		}
		chat->messages = std::move(work.repaired->messages);
		chat->messages_loaded = true;
		chat->persisted_message_count = chat->messages.size();
		chat->persisted_messages_digest = chat->updated_at + ":" + std::to_string(chat->messages.size());
		if (const auto identity = Capture(app, *chat)) attempted[chat->id] = *identity;
		retries.erase(chat->id);
		return true;
	}
};

OpenCodeCompactionHistoryService::OpenCodeCompactionHistoryService(ExecuteQuery execute) : m_state(std::make_unique<State>(std::move(execute))) {}
OpenCodeCompactionHistoryService::~OpenCodeCompactionHistoryService() { Stop(); }

void OpenCodeCompactionHistoryService::Stop()
{
	if (!m_state || m_state->stopped) return;
	m_state->stopped = true;
	if (m_state->active) m_state->active->stop.request_stop();
	if (m_state->worker && m_state->worker->joinable()) m_state->worker->join();
	m_state->worker.reset(); m_state->active.reset();
}

bool OpenCodeCompactionHistoryService::Poll(AppState& app)
{
	if (m_state->stopped) return false;
	bool changed = false;
	if (m_state->active && m_state->active->finished.load(std::memory_order_acquire))
	{
		const auto work = std::move(m_state->active);
		if (m_state->worker && m_state->worker->joinable()) m_state->worker->join();
		m_state->worker.reset();
		if (work->prepared && work->repaired && !work->stop.stop_requested()) changed = m_state->Publish(app, *work);
		if (work->stale)
		{
			auto& retry = m_state->retries[work->identity.chat];
			if (!(retry.identity == work->identity) || retry.fingerprint != work->fingerprint)
			{ retry.identity = work->identity; retry.fingerprint = work->fingerprint; retry.count = 0; }
			if (++retry.count <= 3) m_state->attempted.erase(work->identity.chat);
			retry.after = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
		}
		if (!work->error.empty()) diagnostics::Write("[OpenCode history] " + work->error);
	}
	if (m_state->active) return changed;
	const auto schedule = [&](const ChatSession& chat)
	{
		const auto identity = Capture(app, chat);
		if (!identity) return false;
		const auto retry = m_state->retries.find(chat.id);
		if (retry != m_state->retries.end() && retry->second.identity == *identity &&
		    std::chrono::steady_clock::now() < retry->second.after) return false;
		const auto previous = m_state->attempted.find(chat.id);
		if (previous != m_state->attempted.end() && previous->second == *identity) return false;
		try { m_state->Start(app, chat, *identity); }
		catch (const std::exception&) { diagnostics::Write("[OpenCode history] Metadata worker could not start."); m_state->active.reset(); }
		return true;
	};
	if (app.selected_chat_index >= 0 && app.selected_chat_index < static_cast<int>(app.chats.size()) && schedule(app.chats[app.selected_chat_index])) return changed;
	return changed;
}
}
