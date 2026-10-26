#include "app/opencode_compaction_sync_service.h"
#include "app/opencode_compaction_history.h"

#include "app/chat_domain_service.h"
#include "app/provider_worker_command.h"
#include "common/config/execution_host_config.h"
#include "common/paths/workspace_root.h"
#include "common/provider/opencode/opencode_native_identity.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_update_handler.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/hash_utils.h"
#include "remote/runner_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace uam
{

namespace
{
	constexpr std::size_t kPageSize = 128;
	constexpr std::size_t kPendingLimit = 64;
	constexpr std::size_t kMetadataBytes = 512 * 1024;
	constexpr int kQueryTimeoutMs = 30000;
	constexpr int kQueryRetryLimit = 2;
	constexpr std::chrono::seconds kPendingProbeInterval{2};
	constexpr std::chrono::seconds kPendingDeadline{10};
	constexpr std::chrono::seconds kSummaryDeadline{90};
	constexpr std::chrono::milliseconds kStreamPublicationInterval{200};

	struct Cursor
	{
		std::int64_t time = 0;
		std::string id;
		bool operator==(const Cursor&) const = default;
		bool operator<(const Cursor& other) const
		{
			return time < other.time || (time == other.time && id < other.id);
		}
	};

	enum class QueryKind { Baseline, Pending, Page, Text, Identified };
	constexpr const char* kQueryNames[] = {"baseline", "pending", "page", "text", "identified"};

	bool VerifiedVersion(std::string_view version)
	{
		return version == "1.17.20" || version == "1.18.35";
	}

	/// <summary>Warm SSH attachment has no initialize reply; durable process ownership permits read-only schema verification.</summary>
	bool MetadataVersionAvailable(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
	{
		if (VerifiedVersion(session.agent_version)) return true;
		if (!session.agent_version.empty() || paths::IsControllerLocalWorkspace(chat)) return false;
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		const std::string process = acp_detail::AcpProcessHandleLabel(session);
		return host && host->transport == "ssh" && host->runner_protocol_version == 3 &&
		    remote::CanAttemptRunnerConnection(*host) && session.remote_runner_protocol_version == 3 &&
		    session.initialized && session.session_ready && session.running && !session.reconnect_pending &&
		    !session.recovering_remote_process && session.chat_id == chat.id &&
		    !session.session_id.empty() && session.session_id == chat.native_session_id &&
		    session.process_execution_host_id == chat.execution_host_id && chat.remote_process_exists &&
		    !chat.remote_process_control_token.empty() && session.remote_output_delivery_token == chat.remote_process_control_token &&
		    !session.last_process_id.empty() && !process.empty() && process != "0" && process == session.last_process_id &&
		    !chat.remote_restart_pending && !chat.remote_stop_cleanup_pending && !chat.remote_source_exit_pending &&
		    !session.local_stop_pending && !session.remote_stop_pending && !session.remote_stop_unconfirmed &&
		    !session.remote_source_exit_reported && !AcpSessionHasPendingCancel(session);
	}

	/// <summary>Only explicit UTC assistant timestamps can bound adoption of an already running turn.</summary>
	std::optional<std::int64_t> AssistantUtcMilliseconds(std::string_view value)
	{
		if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
		    value[13] != ':' || value[16] != ':' || value.back() != 'Z') return {};
		const auto number = [&](std::size_t offset, std::size_t length) -> int
		{
			int result = 0;
			for (std::size_t index = offset; index < offset + length; ++index)
			{
				if (value[index] < '0' || value[index] > '9') return -1;
				result = result * 10 + value[index] - '0';
			}
			return result;
		};
		const int year = number(0, 4), month = number(5, 2), day = number(8, 2);
		const int hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
		const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
		if (!date.ok() || year < 1970 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return {};
		int milliseconds = 0;
		if (value.size() != 20)
		{
			if (value[19] != '.' || value.size() < 22 || value.size() > 30) return {};
			for (std::size_t index = 20; index + 1 < value.size(); ++index)
			{
				if (value[index] < '0' || value[index] > '9') return {};
				if (index < 23) milliseconds += (value[index] - '0') * (index == 20 ? 100 : index == 21 ? 10 : 1);
			}
		}
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::sys_days{date}.time_since_epoch()).count() +
		    ((hour * 60LL + minute) * 60 + second) * 1000 + milliseconds;
	}

	struct Summary
	{
		std::string id, text, prefix, previous_tool, revision;
		std::int64_t characters = -1, offset = 0, prefix_bytes = 0;
		bool has_text = false, ready = false, requires_tool = false;
		std::chrono::steady_clock::time_point deadline{};
	};

	/// <summary>Recovered OpenCode prompts retain durable RPC ownership even when the runtime prompt ID is unset.</summary>
	bool IsActivePrompt(const AcpSessionState& session, const ChatSession& chat)
	{
		if (session.prompt_request_id != 0) return true;
		if (paths::IsControllerLocalWorkspace(chat) || session.remote_runner_protocol_version != 3 ||
		    !session.recovering_remote_turn || !chat.remote_turn_reconnect_pending || session.turn_serial <= 0 ||
		    chat.remote_turn_serial != session.turn_serial || session.process_execution_host_id != chat.execution_host_id) return false;
		return std::ranges::any_of(chat.remote_pending_requests, [&](const AcpRemotePendingRequestState& request)
		{
			if (request.response_consumed || request.request_id <= 0 || request.method != "session/prompt" ||
			    request.turn_serial != session.turn_serial) return false;
			const std::unordered_map<int, std::string>::const_iterator pending = session.pending_request_methods.find(request.request_id);
			return pending != session.pending_request_methods.end() && pending->second == request.method;
		});
	}

	struct Identity
	{
		std::filesystem::path root;
		std::shared_ptr<const std::uint8_t> incarnation;
		std::string chat_id, created, provider, native, stored_native, process, process_handle, workspace, warm_control_token;
		std::vector<std::string> command;
		std::optional<ExecutionHost> host;
		bool operator==(const Identity& other) const
		{
			return root == other.root && incarnation == other.incarnation && chat_id == other.chat_id &&
			    created == other.created && provider == other.provider && native == other.native && stored_native == other.stored_native &&
			    process == other.process && process_handle == other.process_handle && workspace == other.workspace &&
			    warm_control_token == other.warm_control_token && command == other.command &&
			    host.has_value() == other.host.has_value() && (!host || execution_hosts::SameConnection(*host, *other.host));
		}
	};

	std::optional<Identity> CaptureIdentity(const AppState& app, const AcpSessionState& session,
	    const ChatSession& chat, std::string& error)
	{
		if (!MetadataVersionAvailable(app, session, chat))
		{ error = "Native compaction synchronization requires a verified OpenCode metadata schema."; return {}; }
		if (!session.running || !session.session_ready || session.last_process_id.empty() || session.chat_id != chat.id ||
		    session.local_stop_pending || session.remote_stop_pending || session.remote_stop_unconfirmed || session.remote_source_exit_reported ||
		    session.provider_id != chat.provider_id || chat.imported_read_only || chat.side_cleanup_requested ||
		    !execution_hosts::IsPortableId(session.session_id) || session.session_id.starts_with('-'))
		{ error = "Native compaction session ownership is not ready."; return {}; }
		const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, session.provider_id);
		if (!profile) { error = "Native compaction provider configuration is unavailable."; return {}; }
		Identity identity;
		identity.root = app.data_root; identity.incarnation = chat.metadata_incarnation;
		identity.chat_id = chat.id; identity.created = chat.created_at; identity.provider = session.provider_id;
		identity.native = session.session_id; identity.stored_native = chat.native_session_id;
		identity.process = session.last_process_id; identity.process_handle = acp_detail::AcpProcessHandleLabel(session);
		if (session.agent_version.empty()) identity.warm_control_token = session.remote_output_delivery_token;
		identity.workspace = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
		if (app.provider_model_catalog && !session.model_catalog_scope_key.empty() &&
		    app.provider_model_catalog->CaptureScopeKey(session.provider_id, identity.workspace, chat.execution_host_id) != session.model_catalog_scope_key)
		{ error = "Native compaction configuration no longer matches the running provider process."; return {}; }
		identity.command = provider_runtime_internal::SplitInteractiveCommandOrDefault(*profile, "opencode");
		if (identity.command.empty()) { error = "Native compaction command is unavailable."; return {}; }
		if (!paths::IsControllerLocalWorkspace(chat))
		{
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
			if (!host || host->transport != "ssh" || !remote::CanAttemptRunnerConnection(*host) || host->runner_protocol_version < 3 ||
			    session.process_execution_host_id != host->id || !execution_hosts::IsAbsoluteRemotePath(host->platform, identity.workspace))
			{ error = "Native compaction metadata requires the owned protocol 3 SSH runner."; return {}; }
			identity.host = *host;
		}
		else if (!paths::PathFromUtf8(identity.workspace).is_absolute())
		{ error = "Native compaction workspace is unavailable."; return {}; }
		return identity;
	}

	std::string SqlLiteral(std::string_view value)
	{
		std::string literal = "'";
		for (const char character : value)
		{
			literal.push_back(character);
			if (character == '\'') literal.push_back('\'');
		}
		literal.push_back('\'');
		return literal;
	}

	std::string MetadataSql(const Identity& identity, QueryKind kind, const Cursor& cursor, const std::set<std::string>& pending)
	{
		const std::string session = SqlLiteral(identity.native);
		std::string filter = "m.session_id=" + session;
		if (kind == QueryKind::Baseline)
			filter += " AND 0"; // The indexed high-water cursor excludes every pre-prompt identity, including pending summaries.
		else if (kind == QueryKind::Page)
			filter += " AND (m.time_created,m.id)>(" + std::to_string(cursor.time) + "," + SqlLiteral(cursor.id) + ")";
		else
		{
			filter += " AND m.id IN (";
			bool first = true;
			for (const std::string& id : pending)
			{
				if (!first) filter += ',';
				first = false; filter += SqlLiteral(id);
			}
			filter += ')';
		}
		// Message.data is native info metadata. Transcript text and tool output live in part.data,
		// which this query inspects only for indexed part types and the parent compaction predicate.
		return "WITH selected AS (SELECT m.id,m.time_created,json_extract(m.data,'$.role') role,"
		    "CASE WHEN json_extract(m.data,'$.role')='assistant' THEN json_extract(m.data,'$.summary') ELSE NULL END summary,"
		    "json_extract(m.data,'$.parentID') parent,"
		    "json_extract(m.data,'$.finish') finish,json_type(m.data,'$.error') error_type,"
		    "CASE WHEN json_extract(m.data,'$.summary')=1 THEN (SELECT count(*) FROM part t WHERE t.message_id=m.id "
		    "AND t.session_id=m.session_id AND json_extract(t.data,'$.type')='text') ELSE 0 END text_parts,"
		    "(" + OpenCodeCompactionProofSql() + ") parent_proof FROM message m WHERE " + filter +
		    " ORDER BY m.time_created,m.id LIMIT " + std::to_string(kPageSize + 1) +
		    ") SELECT s.id sessionId,s.directory,"
		    "EXISTS(SELECT 1 FROM sqlite_master WHERE type='index' AND name='message_session_time_created_id_idx') AND "
		    "EXISTS(SELECT 1 FROM sqlite_master WHERE type='index' AND name='part_message_id_id_idx') schemaCompatible,"
		    "coalesce((SELECT time_created FROM message WHERE session_id=s.id ORDER BY time_created DESC,id DESC LIMIT 1),0) upperTime,"
		    "coalesce((SELECT id FROM message WHERE session_id=s.id ORDER BY time_created DESC,id DESC LIMIT 1),'') upperId,"
		    "(SELECT json_group_array(json_object('id',id,'created',time_created,'role',role,'summary',summary,"
		    "'parent',parent,'finish',finish,'errorType',error_type,'parentProof',parent_proof,'textParts',text_parts)) FROM selected) rows "
		    "FROM session s WHERE s.id=" + session;
	}

	std::string SummarySql(const Identity& identity, const Summary& summary, const Cursor& scope)
	{
		// The CLI can exit before draining large stdout. Page the prefix and body together below its pipe bound.
		const int page = identity.host && identity.host->platform == "windows" ? 256 : 8192;
		const std::string bound = std::to_string(kMetadataBytes);
		return "WITH owned AS (SELECT m.id,m.session_id,m.time_created,m.time_updated,m.data FROM message m WHERE m.session_id=" + SqlLiteral(identity.native) +
		    " AND m.id=" + SqlLiteral(summary.id) + " AND json_extract(m.data,'$.role')='assistant' "
		    "AND json_extract(m.data,'$.summary')=1 AND json_type(m.data,'$.finish')='text' AND json_extract(m.data,'$.finish')<>'' "
		    "AND (json_type(m.data,'$.error') IS NULL OR json_type(m.data,'$.error')='null') "
		    "AND (" + OpenCodeCompactionProofSql() + ")), anchor AS ("
		    "SELECT tm.time_created created,tm.id message_id,tp.id part_id,json_extract(tp.data,'$.callID') tool "
		    "FROM owned m JOIN message tm ON tm.session_id=m.session_id JOIN part tp ON tp.message_id=tm.id AND tp.session_id=tm.session_id "
		    "WHERE (tm.time_created,tm.id)<(m.time_created,m.id) AND (tm.time_created,tm.id)>(" +
		    std::to_string(scope.time) + "," + SqlLiteral(scope.id) + ") AND json_extract(tm.data,'$.role')='assistant' "
		    "AND json_extract(tp.data,'$.type')='tool' ORDER BY tm.time_created DESC,tm.id DESC,tp.id DESC LIMIT 1), "
		    "prefix_parts AS (SELECT tm.id message_id,p.id part_id,p.time_updated updated,length(p.data) size,"
		    "json_extract(p.data,'$.text') text,json_type(p.data,'$.text') text_type "
		    "FROM owned m JOIN message tm ON tm.session_id=m.session_id JOIN part p ON p.message_id=tm.id AND p.session_id=tm.session_id "
		    "WHERE json_extract(tm.data,'$.role')='assistant' AND json_extract(p.data,'$.type')='text' "
		    "AND (tm.time_created,tm.id)<(m.time_created,m.id) AND (tm.time_created,tm.id)>(" +
		    std::to_string(scope.time) + "," + SqlLiteral(scope.id) + ") AND (NOT EXISTS(SELECT 1 FROM anchor) OR "
		    "(tm.time_created,tm.id,p.id)>(SELECT created,message_id,part_id FROM anchor)) ORDER BY tm.time_created,tm.id,p.id LIMIT 129), "
		    "body_parts AS (SELECT p.id part_id,p.time_updated updated,length(p.data) size,json_extract(p.data,'$.text') text,"
		    "json_type(p.data,'$.text') text_type FROM owned m JOIN part p ON p.message_id=m.id AND p.session_id=m.session_id "
		    "WHERE json_extract(p.data,'$.type')='text' ORDER BY p.id LIMIT 129), "
		    "bounds AS (SELECT (SELECT count(*) FROM prefix_parts) prefix_count,(SELECT count(*) FROM body_parts) body_count,"
		    "(SELECT coalesce(sum(CASE WHEN text_type='text' THEN length(CAST(text AS BLOB)) ELSE " + bound + "+1 END),0) FROM prefix_parts)+"
		    "(SELECT coalesce(sum(CASE WHEN text_type='text' THEN length(CAST(text AS BLOB)) ELSE " + bound + "+1 END),0) FROM body_parts) bytes), "
		    "prefix AS (SELECT coalesce(group_concat(text,''),'') text,coalesce(group_concat(message_id||':'||part_id||':'||updated||':'||size,'|'),'') revision "
		    "FROM prefix_parts CROSS JOIN bounds WHERE bytes<=" + bound + " AND prefix_count<=128 AND body_count<=128), "
		    "body AS (SELECT coalesce(group_concat(text,''),'') text,coalesce(group_concat(part_id||':'||updated||':'||size,'|'),'') revision "
		    "FROM body_parts CROSS JOIN bounds WHERE bytes<=" + bound + " AND prefix_count<=128 AND body_count<=128), "
		    "snapshot AS (SELECT prefix.text||body.text text,length(CAST(prefix.text AS BLOB)) prefix_bytes,prefix.revision||':'||body.revision revision FROM prefix CROSS JOIN body) "
		    "SELECT s.id sessionId,s.directory,m.time_updated||':'||m.data||':'||b.revision revision,length(b.text) characters,"
		    "b.prefix_bytes prefixBytes,length(substr(b.text,1," + std::to_string(summary.offset) + ")) offset,substr(b.text," +
		    std::to_string(summary.offset + 1) + "," + std::to_string(page) + ") chunk,"
		    "length(substr(b.text," + std::to_string(summary.offset + 1) + "," + std::to_string(page) + ")) chunkCharacters,"
		    "coalesce((SELECT tool FROM anchor),'') previousTool FROM owned m JOIN session s ON s.id=m.session_id CROSS JOIN snapshot b CROSS JOIN bounds "
		    "WHERE bytes<=" + bound + " AND prefix_count<=128 AND body_count<=128";
	}

	/// <summary>Replace complete native-ID summaries, or legacy text at its exactly proven native prefix.</summary>
	template<typename Block> bool RetypeSummaries(std::vector<Block>& blocks, const std::map<Cursor, Summary>& summaries)
	{
		struct Match { std::size_t block, offset, length; std::string id; };
		std::vector<Match> matches;
		bool identity_changed = false;
		for (const std::pair<const Cursor, Summary>& item : summaries)
		{
			const Summary& summary = item.second;
			if (!summary.ready || summary.text.empty()) continue;
			const std::string id = "acp-compaction:" + summary.id;
			if (std::ranges::any_of(blocks, [&](const Block& block) { return block.type == "context_compaction" &&
			    (block.request_id_json == id || block.request_id_json == summary.id) && block.text == summary.text; })) continue;
			const std::string native_identity = "opencode-message:" + summary.id;
			std::size_t first_native = blocks.size(), last_native = 0;
			std::string identified_text;
			bool contiguous = true;
			for (std::size_t index = 0; index < blocks.size(); ++index)
			{
				if ((blocks[index].type != "assistant_text" && blocks[index].type != "context_compaction_summary") ||
				    blocks[index].request_id_json != native_identity || blocks[index].text.empty()) continue;
				if (first_native == blocks.size()) first_native = index;
				else
					for (std::size_t gap = last_native + 1; gap < index; ++gap)
						if (blocks[gap].type != "context_compaction_pending" &&
						    !(blocks[gap].type == "context_compaction_summary" && blocks[gap].text.empty())) contiguous = false;
				last_native = index;
				if (blocks[index].text.size() > summary.text.size() - identified_text.size()) { contiguous = false; break; }
				identified_text += blocks[index].text;
			}
			if (first_native != blocks.size())
			{
				if (contiguous && identified_text == summary.text)
				{
					Block compacted = blocks[first_native];
					compacted.type = "context_compaction"; compacted.text = summary.text; compacted.request_id_json = id;
					for (std::size_t index = last_native; index > first_native; --index)
						if ((blocks[index].type == "assistant_text" || blocks[index].type == "context_compaction_summary") &&
						    blocks[index].request_id_json == native_identity && !blocks[index].text.empty())
							blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(index));
					blocks[first_native] = std::move(compacted);
					identity_changed = true;
				}
				continue; // A partial or mismatched identified stream cannot fall back to matching prose.
			}
		}
		for (const std::pair<const Cursor, Summary>& item : summaries)
		{
			const Summary& summary = item.second;
			if (!summary.ready || summary.text.empty()) continue;
			const std::string id = "acp-compaction:" + summary.id;
			// Empty quarantine receipts are omitted from saved blocks and contain no native text to veto legacy proof.
			if (std::ranges::any_of(blocks, [&](const Block& block)
			{ return (block.request_id_json == "opencode-message:" + summary.id &&
			        !(block.type == "context_compaction_summary" && block.text.empty())) ||
			    (block.type == "context_compaction" && (block.request_id_json == id || block.request_id_json == summary.id) && block.text == summary.text); })) continue;
			if (summary.requires_tool && summary.previous_tool.empty()) continue;
			std::size_t begin = 0;
			if (!summary.previous_tool.empty())
			{
				int anchors = 0;
				for (std::size_t index = 0; index < blocks.size(); ++index)
					if (blocks[index].type == "tool_call" && blocks[index].tool_call_id == summary.previous_tool) { begin = index + 1; ++anchors; }
				if (anchors != 1) continue;
			}
			std::size_t consumed = 0;
			for (std::size_t index = begin; index < blocks.size(); ++index)
			{
				const Block& block = blocks[index];
				if (block.type == "tool_call") break;
				if (block.type != "assistant_text" && block.type != "context_compaction") continue;
				const std::size_t prefix = std::min(summary.prefix.size() - consumed, block.text.size());
				if (block.text.compare(0, prefix, summary.prefix, consumed, prefix) != 0) break;
				consumed += prefix;
				if (consumed != summary.prefix.size() || prefix == block.text.size()) continue;
				// The native prefix proves the exact position; uniqueness in an incomplete ACP stream cannot.
				if (block.type == "assistant_text" && block.request_id_json.empty() && block.text.compare(prefix, summary.text.size(), summary.text) == 0)
					matches.push_back({index, prefix, summary.text.size(), id});
				break;
			}
		}
		std::sort(matches.begin(), matches.end(), [](const Match& left, const Match& right)
		{ return left.block > right.block || (left.block == right.block && left.offset > right.offset); });
		for (std::size_t index = 1; index < matches.size(); ++index)
			if (matches[index].block == matches[index - 1].block && matches[index].offset + matches[index].length > matches[index - 1].offset) return identity_changed;
		bool changed = identity_changed;
		for (const Match& match : matches)
		{
			const Block original = blocks[match.block];
			std::vector<Block> replacement;
			if (match.offset != 0) { Block before = original; before.text.resize(match.offset); replacement.push_back(std::move(before)); }
			Block compacted = original;
			compacted.type = "context_compaction"; compacted.text = original.text.substr(match.offset, match.length); compacted.request_id_json = match.id;
			replacement.push_back(std::move(compacted));
			if (match.offset + match.length < original.text.size())
			{ Block after = original; after.text = original.text.substr(match.offset + match.length); replacement.push_back(std::move(after)); }
			blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(match.block));
			blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(match.block), replacement.begin(), replacement.end());
			changed = true;
		}
		if (changed)
		{
			std::set<std::string> completed;
			for (const Block& block : blocks)
				if (block.type == "context_compaction" && !block.text.empty()) completed.insert(block.request_id_json);
			std::erase_if(blocks, [&](const Block& block)
			{
				return block.type == "context_compaction" && block.text.empty() &&
				    (completed.contains(block.request_id_json) || completed.contains("acp-compaction:" + block.request_id_json));
			});
		}

		return changed;
	}

	ProcessExecutionResult ExecuteNativeQuery(const OpenCodeCompactionSyncService::Query& query, std::stop_token stop)
	{
		if (query.host)
		{
			const ExecutionHost& host = *query.host;
			remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
			    host);
			const std::string nonce = PlatformServicesFactory::Instance().process_service.GenerateUuid();
			if (nonce.empty()) return {.error = "Native compaction query identity could not be created."};
			const std::string id = "compaction-metadata-" + nonce;
			return client.ExecuteCommand(id, query.workspace, query.argv, kQueryTimeoutMs, stop);
		}
		ProviderWorkerInvocation invocation;
		invocation.direct_process = invocation.structured_stdout = true;
		invocation.argv = query.argv;
		return ExecuteProviderWorkerInvocation(invocation, query.workspace, kQueryTimeoutMs, stop);
	}

	std::uint64_t MessageOwnership(const Message& message)
	{
		std::uint64_t hash = hashing::kFnv1a64OffsetBasis;
		for (const std::string* value : {&message.created_at, &message.content, &message.thoughts}) hashing::UpdateFnv1a64WithSeparator(hash, *value);
		for (const MessageBlock& block : message.blocks)
		{
			for (const std::string* value : {&block.type, &block.text, &block.tool_call_id, &block.request_id_json}) hashing::UpdateFnv1a64WithSeparator(hash, *value);
			hashing::UpdateFnv1a64WithSeparator(hash, std::to_string(block.compaction_tokens_before));
			hashing::UpdateFnv1a64WithSeparator(hash, std::to_string(block.compaction_tokens_after));
		}
		return hash;
	}

	bool PortableId(const nlohmann::json& value)
	{
		return value.is_string() && opencode::IsNativeId(value.get_ref<const std::string&>());
	}
}

struct OpenCodeCompactionSyncService::State
{
	struct Owner
	{
		int serial = 0, assistant = -1;
		std::uint64_t message = 0;
		int handoff_serial = 0;
		bool pages_finished = false, live = false, scan_complete = false;
		std::string created;
		std::chrono::steady_clock::time_point pending_deadline{};
		std::chrono::steady_clock::time_point next_pending_probe{};
		std::map<Cursor, Summary> completed;
	};
	struct Context
	{
		Identity identity;
		Cursor cursor, scope;
		std::set<std::string> pending;
		bool baseline = false, adopted = false, identified_probe_due = true;
		int armed_turn = 0, finished_turn = 0, failed_attempt = 0;
		std::optional<Owner> owner;
		std::string error;
		std::set<std::string> native_pending_markers, native_summary_ids, identified, inspected;
		int inspected_turn = 0;
		int native_marker_turn = 0, native_marker_assistant = -1, query_timeouts = 0;
		std::chrono::steady_clock::time_point retry_at{}, next_live_probe{}, next_publication_probe{};
	};
	struct Work
	{
		Identity identity;
		QueryKind kind = QueryKind::Baseline;
		Cursor cursor;
		std::set<std::string> pending, identified;
		std::map<Cursor, Summary> completed;
		std::stop_source cancellation;
		std::atomic<bool> finished{false};
		ProcessExecutionResult result;
		Cursor summary_cursor;
		int owner_serial = 0, owner_assistant = -1;
		std::int64_t duration_ms = 0;
	};
	ExecuteQuery execute;
	std::map<std::string, Context> contexts;
	std::shared_ptr<Work> active;
	std::unique_ptr<std::jthread> worker;
	std::string last_scheduled;
	std::set<std::string> unsupported_diagnostics;
	bool stopped = false;

	explicit State(ExecuteQuery executor) : execute(executor ? std::move(executor) : ExecuteNativeQuery) {}

	bool CheapOwns(const AppState& app, const AcpSessionState& session, const ChatSession& chat, const Context& context) const
	{
		const Identity& identity = context.identity;
		return app.data_root == identity.root && chat.metadata_incarnation == identity.incarnation &&
		    chat.id == identity.chat_id && chat.created_at == identity.created && chat.provider_id == identity.provider &&
		    session.provider_id == identity.provider && chat.native_session_id == identity.stored_native &&
		    session.session_id == identity.native && session.last_process_id == identity.process &&
		    acp_detail::AcpProcessHandleLabel(session) == identity.process_handle && session.running && !AcpSessionHasPendingCancel(session) &&
		    !session.local_stop_pending && !session.remote_stop_pending && !session.remote_stop_unconfirmed && !session.remote_source_exit_reported &&
		    !chat.imported_read_only && !chat.side_cleanup_requested &&
		    (identity.host ? chat.execution_host_id == identity.host->id : paths::IsControllerLocalWorkspace(chat));
	}

	/// <summary>Keep verified native summary identity separate from active status until its exact body is classified.</summary>
	bool QuarantineNativeSummaries(AppState& app, AcpSessionState& session, ChatSession& chat, Context& context,
	    const std::set<std::string>& proven)
	{
		if (!context.owner || !Owns(app, session, chat, context)) return false;
		bool changed = false;
		const bool retained = session.turn_serial == context.owner->serial &&
		    session.turn_assistant_message_index == context.owner->assistant;
		if (!retained)
		{
			if (context.owner->assistant < 0 || context.owner->assistant >= static_cast<int>(chat.messages.size())) return false;
			Message& message = chat.messages[context.owner->assistant];
			for (MessageBlock& block : message.blocks)
				if (block.type == "assistant_text" && block.request_id_json.starts_with("opencode-message:") &&
				    proven.contains(block.request_id_json.substr(17)))
				{ block.type = "context_compaction_summary"; changed = true; }
			if (changed)
			{
				acp_detail::ScheduleChatSave(app, chat, 0.0);
				context.owner->message = MessageOwnership(message);
			}
			return changed;
		}
		for (const std::string& id : proven)
		{
			const std::string identity = "opencode-message:" + id;
			context.native_summary_ids.insert(id);
			for (AcpTurnEventState& event : session.turn_events)
				if (event.type == "assistant_text" && event.request_id_json == identity)
				{ event.type = "context_compaction_summary"; changed = true; }
			if (std::ranges::none_of(session.turn_events, [&](const AcpTurnEventState& event)
			{ return event.type == "context_compaction_summary" && event.request_id_json == identity; }))
			{
				session.turn_events.push_back({.type = "context_compaction_summary", .request_id_json = identity});
				changed = true;
			}
		}
		if (changed && context.owner->assistant >= 0)
		{
			if (acp_detail::SyncCurrentAssistantMessageBlocksFromTurnEvents(chat, session))
				acp_detail::ScheduleChatSave(app, chat, 0.0);
			context.owner->message = MessageOwnership(chat.messages[context.owner->assistant]);
		}
		return changed;
	}

	/// <summary>Collapse only retained text with previously verified native compaction identity after interruption.</summary>
	bool InterruptNativeSummaries(AppState& app, AcpSessionState& session, ChatSession& chat, Context& context,
	    std::set<std::string> interrupted)
	{
		if (!context.owner || session.turn_serial != context.owner->serial ||
		    session.turn_assistant_message_index != context.owner->assistant || session.session_id != context.identity.native ||
		    session.last_process_id != context.identity.process) return false;
		bool changed = false;
		std::vector<AcpTurnEventState> events;
		events.reserve(session.turn_events.size());
		for (const AcpTurnEventState& original : session.turn_events)
		{
			AcpTurnEventState event = original;
			if (event.type == "context_compaction_summary" && event.request_id_json.starts_with("opencode-message:") &&
			    interrupted.contains(event.request_id_json.substr(17)) && context.native_summary_ids.contains(event.request_id_json.substr(17)))
			{
				changed = true;
				if (event.text.empty()) continue;
				event.type = "context_compaction_interrupted";
				event.request_id_json = "acp-compaction:" + event.request_id_json.substr(17);
				if (!events.empty() && events.back().type == event.type && events.back().request_id_json == event.request_id_json)
				{ events.back().text += event.text; continue; }
			}
			events.push_back(std::move(event));
		}
		if (!changed) return false;
		session.turn_events = std::move(events);
		if (context.owner->assistant >= 0 && context.owner->assistant < static_cast<int>(chat.messages.size()) &&
		    chat.messages[context.owner->assistant].role == MessageRole::Assistant)
		{
			if (acp_detail::SyncCurrentAssistantMessageBlocksFromTurnEvents(chat, session))
				acp_detail::ScheduleChatSave(app, chat, 0.0);
			context.owner->message = MessageOwnership(chat.messages[context.owner->assistant]);
		}
		const bool may_receive_output = session.running &&
		    (session.processing || session.prompt_request_id != 0 || AcpSessionIsWaitingForInput(session));
		for (const std::string& id : interrupted)
		{
			if (may_receive_output)
				session.turn_events.push_back({.type = "context_compaction_summary", .request_id_json = "opencode-message:" + id});
			else context.native_summary_ids.erase(id);
		}
		return true;
	}

	/// <summary>Own only pending markers introduced by this exact native turn; ACP markers remain independent.</summary>
	bool SyncNativePending(AppState& app, AcpSessionState& session, ChatSession& chat, Context& context,
	    const std::set<std::string>& unfinished)
	{
		const bool same_marker_owner = session.turn_serial == context.native_marker_turn &&
		    session.turn_assistant_message_index == context.native_marker_assistant &&
		    session.session_id == context.identity.native && session.last_process_id == context.identity.process;
		bool changed = false;
		if (same_marker_owner)
		{
			const std::size_t before = session.turn_events.size();
			std::erase_if(session.turn_events, [&](const AcpTurnEventState& event)
			{
				return event.type == "context_compaction_pending" &&
				    context.native_pending_markers.contains(event.request_id_json) &&
				    !unfinished.contains(event.request_id_json.substr(15));
			});
			changed = before != session.turn_events.size();
		}
		else context.native_pending_markers.clear();
		std::erase_if(context.native_pending_markers, [&](const std::string& id) { return !unfinished.contains(id.substr(15)); });
		if (context.owner && context.owner->live && Owns(app, session, chat, context))
		{
			for (const std::string& id : unfinished)
			{
				const std::string marker = "acp-compaction:" + id;
				if (std::ranges::any_of(session.turn_events, [&](const AcpTurnEventState& event)
				{ return event.request_id_json == marker && (event.type == "context_compaction_pending" || event.type == "context_compaction"); })) continue;
				acp_detail::HandleSessionUpdate(app, session, chat, {{"sessionId", session.session_id}, {"update", {
				    {"sessionUpdate", "compaction_update"}, {"compactionId", id}, {"status", "started"}}}}, nullptr);
				if (std::ranges::any_of(session.turn_events, [&](const AcpTurnEventState& event)
				{ return event.type == "context_compaction_pending" && event.request_id_json == marker; }))
				{
					context.native_pending_markers.insert(marker);
					context.native_marker_turn = session.turn_serial;
					context.native_marker_assistant = session.turn_assistant_message_index;
					changed = true;
				}
			}
		}
		if (changed)
		{
			const bool active = session.running && !AcpSessionHasPendingCancel(session) &&
			    (session.processing || session.prompt_request_id != 0 || AcpSessionIsWaitingForInput(session));
			session.context_compaction_in_progress = active && std::ranges::any_of(session.turn_events,
			    [](const AcpTurnEventState& event) { return event.type == "context_compaction_pending"; });
		}
		return changed;
	}

	Context* FindOrCreate(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string& error)
	{
		const std::optional<Identity> identity = CaptureIdentity(app, session, chat, error);
		if (!identity) return nullptr;
		Context& context = contexts[chat.id];
		if (!(context.identity == *identity))
		{
			if (active && active->identity.chat_id == chat.id) active->cancellation.request_stop();
			if (ChatSession* current = ChatDomainService().FindChatById(app, chat.id))
				(void)SyncNativePending(app, session, *current, context, {});
			context = Context{};
			context.identity = *identity;
		}
		return &context;
	}

	void CaptureOwner(Context& context, const AcpSessionState& session, const ChatSession& chat, bool handoff)
	{
		if (context.owner)
		{
			if (context.owner->live && context.owner->assistant < 0 && session.turn_serial == context.owner->serial)
			{
				const int index = session.turn_assistant_message_index;
				if (index >= 0 && index == session.current_assistant_message_index && index < static_cast<int>(chat.messages.size()) &&
				    chat.messages[index].role == MessageRole::Assistant &&
				    acp_detail::MessageBlocksEqual(chat.messages[index].blocks, acp_detail::MessageBlocksFromTurnEvents(session)))
				{
					context.owner->assistant = index;
					context.owner->message = MessageOwnership(chat.messages[index]);
					context.owner->created = chat.messages[index].created_at;
					if (context.native_marker_turn == session.turn_serial && context.native_marker_assistant < 0 &&
					    session.session_id == context.identity.native && session.last_process_id == context.identity.process)
						context.native_marker_assistant = index;
				}
			}
			if (context.owner->live && session.turn_serial == context.owner->serial && !session.processing && session.prompt_request_id == 0)
			{
				if (session.last_settled_turn_serial != session.turn_serial || session.last_turn_outcome != "completed")
				{
					context.finished_turn = context.armed_turn; context.baseline = false; context.pending.clear(); context.owner.reset();
					if (active && active->identity.chat_id == chat.id) active->cancellation.request_stop();
					return;
				}
				if (context.owner->assistant < 0 || context.owner->assistant >= static_cast<int>(chat.messages.size()))
				{
					context.error = "Native compaction has no unchanged completed assistant owner.";
					return;
				}
				context.owner->live = false;
				// Active probes have no settlement deadline; take a final page before starting that grace period.
				context.owner->pages_finished = context.owner->scan_complete = false;
				context.owner->pending_deadline = context.owner->next_pending_probe = {};
				context.owner->message = MessageOwnership(chat.messages[context.owner->assistant]);
				context.next_live_probe = {};
			}
			if (handoff && (context.owner->serial == session.turn_serial || context.failed_attempt == session.turn_serial))
				context.owner->handoff_serial = session.turn_serial + 1;
			return;
		}
		if (!context.baseline || context.armed_turn == 0 || context.finished_turn == context.armed_turn ||
		    session.turn_serial != context.armed_turn || AcpSessionHasPendingCancel(session)) return;
		const bool live = session.processing && IsActivePrompt(session, chat) && session.queued_prompt.empty() &&
		    session.current_assistant_message_index == session.turn_assistant_message_index;
		if (!live && (session.last_settled_turn_serial != context.armed_turn || session.processing || session.prompt_request_id != 0)) return;
		if (!live && session.last_turn_outcome != "completed")
		{
			context.finished_turn = context.armed_turn;
			context.baseline = false;
			context.pending.clear();
			return;
		}
		const int index = session.turn_assistant_message_index;
		if (live && index < 0)
		{
			// A pre-prompt native baseline owns metadata before ACP lazily creates an assistant.
			if (!context.adopted)
				context.owner = Owner{.serial = session.turn_serial, .handoff_serial = handoff ? session.turn_serial + 1 : 0, .live = true};
			return;
		}
		if (index < 0 || index >= static_cast<int>(chat.messages.size()) || chat.messages[index].role != MessageRole::Assistant)
		{
			context.finished_turn = context.armed_turn;
			context.baseline = false;
			context.pending.clear();
			context.error = "Native compaction has no unchanged completed assistant owner.";
			return;
		}
		if (live && !acp_detail::MessageBlocksEqual(chat.messages[index].blocks, acp_detail::MessageBlocksFromTurnEvents(session))) return;
		context.owner = Owner{.serial = session.turn_serial, .assistant = index,
		    .message = MessageOwnership(chat.messages[index]), .handoff_serial = handoff ? session.turn_serial + 1 : 0,
		    .live = live, .created = chat.messages[index].created_at};
	}

	bool Owns(const AppState& app, const AcpSessionState& session, const ChatSession& chat, const Context& context) const
	{
		if (!CheapOwns(app, session, chat, context)) return false;
		std::string ignored;
		const std::optional<Identity> current = CaptureIdentity(app, session, chat, ignored);
		if (!current || !(*current == context.identity) || AcpSessionHasPendingCancel(session)) return false;
		if (!context.owner) return true;
		const Owner& owner = *context.owner;
		if (owner.assistant < 0)
			return owner.live && context.baseline && !context.adopted && session.turn_serial == owner.serial &&
			    session.processing && IsActivePrompt(session, chat) && session.queued_prompt.empty() &&
			    session.turn_assistant_message_index < 0 && session.current_assistant_message_index < 0;
		if (owner.assistant >= static_cast<int>(chat.messages.size()) ||
		    chat.messages[owner.assistant].role != MessageRole::Assistant || chat.messages[owner.assistant].created_at != owner.created) return false;
		if (owner.live) return session.turn_serial == owner.serial && session.processing && IsActivePrompt(session, chat) &&
		    session.queued_prompt.empty() && session.turn_assistant_message_index == owner.assistant && session.current_assistant_message_index == owner.assistant;
		if (MessageOwnership(chat.messages[owner.assistant]) != owner.message) return false;
		return (session.turn_serial == owner.serial && session.last_settled_turn_serial == owner.serial &&
		        !session.processing && session.prompt_request_id == 0) ||
		    (owner.handoff_serial == session.turn_serial && session.prompt_request_id == 0 && !session.queued_prompt.empty()) ||
		    (context.failed_attempt == session.turn_serial && session.last_settled_turn_serial == context.failed_attempt &&
		        session.last_turn_outcome == "failed" && !session.processing && session.prompt_request_id == 0);
	}

	bool Publish(AppState& app, AcpSessionState& session, ChatSession& chat, Context& context)
	{
		if (!context.owner || !Owns(app, session, chat, context))
		{ context.error = "Native compaction ownership changed before publication."; return false; }
		Owner& owner = *context.owner;
		if (owner.assistant < 0) return false; // Metadata and pending state cannot manufacture an assistant or success divider.
		Message& message = chat.messages[owner.assistant];
		const bool retained = session.turn_serial == owner.serial && session.turn_assistant_message_index == owner.assistant && !session.turn_events.empty();
		if (retained && !acp_detail::MessageBlocksEqual(message.blocks, acp_detail::MessageBlocksFromTurnEvents(session))) return false;
		std::vector<MessageBlock> blocks = message.blocks;
		if (blocks.empty() && !owner.live)
		{
			AcpSessionState restored;
			acp_detail::RestoreTurnEventsFromMessageBlocks(restored, message);
			blocks = acp_detail::MessageBlocksFromTurnEvents(restored);
		}
		bool changed = RetypeSummaries(blocks, owner.completed);
		if (changed && retained)
		{
			std::vector<AcpTurnEventState> events = session.turn_events;
			if (!RetypeSummaries(events, owner.completed)) return false;
			// Updating retained events directly preserves every tool owner and delivery field.
			session.turn_events = std::move(events);
			blocks = acp_detail::MessageBlocksFromTurnEvents(session);
		}
		bool unresolved = false;
		for (const std::pair<const Cursor, Summary>& completed : owner.completed)
		{
			const Summary& summary = completed.second;
			const std::string identity = "acp-compaction:" + summary.id;
			if (summary.has_text)
			{
				if (!std::ranges::any_of(blocks, [&](const MessageBlock& block) { return block.type == "context_compaction" &&
				    (block.request_id_json == identity || block.request_id_json == summary.id) && block.text == summary.text; })) unresolved = true;
				continue;
			}
			// Metadata-only providers retain the existing completed-turn empty-boundary contract.
			if (owner.live) continue;
			const bool exists = std::ranges::any_of(chat.messages, [&](const Message& row)
			{ return std::ranges::any_of(row.blocks, [&](const MessageBlock& block) { return block.type == "context_compaction" &&
			    (block.request_id_json == summary.id || block.request_id_json == identity); }); });
			if (!exists)
			{
				blocks.push_back({"context_compaction", "", "", identity});
				if (retained) session.turn_events.push_back({.type = "context_compaction", .request_id_json = identity});
				changed = true;
			}
		}
		if (changed)
		{
			message.blocks = std::move(blocks);
			acp_detail::ScheduleChatSave(app, chat, 0.0);
		}
		owner.message = MessageOwnership(message);
		context.next_publication_probe = std::chrono::steady_clock::now() + kStreamPublicationInterval;
		// Published IDs remain in the saved projection and behind the native cursor; only unresolved work needs caching.
		std::erase_if(owner.completed, [&](const std::pair<const Cursor, Summary>& item)
		{
			const Summary& summary = item.second;
			const bool published = summary.ready && !summary.text.empty() && !context.pending.contains(summary.id) &&
			    std::ranges::any_of(message.blocks, [&](const MessageBlock& block)
			    { return block.type == "context_compaction" && block.text == summary.text &&
			        (block.request_id_json == summary.id || block.request_id_json == "acp-compaction:" + summary.id); });
			if (published) context.identified.erase(summary.id);
			return published;
		});
		if (!owner.live && owner.scan_complete && !unresolved && context.pending.empty())
		{ context.finished_turn = owner.serial; context.owner.reset(); }
		else if (!owner.live && owner.pending_deadline == std::chrono::steady_clock::time_point{})
			owner.pending_deadline = std::chrono::steady_clock::now() + kPendingDeadline;
		return changed;
	}

	bool Consume(AppState& app, Context& context, const Work& work)
	{
		if (context.owner && context.owner->pending_deadline != std::chrono::steady_clock::time_point{})
			context.owner->pending_deadline += std::chrono::milliseconds(work.duration_ms);
		if (!work.result.ok || work.result.output_truncated || work.result.canceled || work.result.timed_out ||
		    work.result.output.size() > kMetadataBytes)
		{
			const std::string detail = work.result.output.size() > kMetadataBytes ? "metadata exceeded the 512 KiB limit" :
			    !work.result.error.empty() ? work.result.error : work.result.timed_out ? "the query timed out" :
			    work.result.canceled ? "the query was cancelled" : work.result.output_truncated ? "the command output limit was exceeded" :
			    "OpenCode exited with code " + std::to_string(work.result.exit_code);
			const bool retry = work.result.timed_out && !work.result.canceled && !work.result.output_truncated &&
			    work.result.output.size() <= kMetadataBytes && ++context.query_timeouts <= kQueryRetryLimit;
			diagnostics::Write("[OpenCode compaction] chat=" + context.identity.chat_id + " native=" + context.identity.native +
			    " turn=" + std::to_string(work.owner_serial) + " query=" + kQueryNames[static_cast<int>(work.kind)] +
			    " duration_ms=" + std::to_string(work.duration_ms) + " retry=" + std::to_string(retry ? context.query_timeouts : 0) +
			    " error=" + detail);
			if (retry)
			{
				const std::chrono::milliseconds backoff{250 * (1 << (context.query_timeouts - 1))};
				context.retry_at = std::chrono::steady_clock::now() + backoff;
				if (context.owner && context.owner->pending_deadline != std::chrono::steady_clock::time_point{})
					context.owner->pending_deadline += backoff;
			}
			else context.error = "Native compaction metadata query failed: " + detail;
			return false;
		}
		try
		{
			const nlohmann::json result = nlohmann::json::parse(work.result.output);
			if (!result.is_array() || result.size() != 1) throw std::runtime_error("missing native session");
			const nlohmann::json& envelope = result.front();
			const std::string directory = envelope.at("directory").get<std::string>();
			const bool directory_matches = context.identity.host
			    ? execution_hosts::RemotePathsMatch(context.identity.host->platform, directory, context.identity.workspace)
			    : paths::PathFromUtf8(directory).lexically_normal() == paths::PathFromUtf8(context.identity.workspace).lexically_normal();
			if (work.kind == QueryKind::Text)
			{
				if (envelope.at("sessionId") != context.identity.native || !directory_matches || !context.owner) throw std::runtime_error("summary ownership mismatch");
				Summary& summary = context.owner->completed.at(work.summary_cursor);
				const std::int64_t characters = envelope.at("characters").get<std::int64_t>();
				const std::int64_t offset = envelope.at("offset").get<std::int64_t>();
				const std::int64_t count = envelope.at("chunkCharacters").get<std::int64_t>();
				const std::int64_t prefix_bytes = envelope.at("prefixBytes").get<std::int64_t>();
				const std::string revision = envelope.at("revision").get<std::string>();
				const std::string previous_tool = envelope.at("previousTool").get<std::string>();
				const std::string chunk = envelope.at("chunk").get<std::string>();
				if (characters < 0 || characters > static_cast<std::int64_t>(kMetadataBytes) || prefix_bytes < 0 || prefix_bytes > static_cast<std::int64_t>(kMetadataBytes) || offset != summary.offset || count < 0 || count > 8192 ||
				    count > characters - offset || (count == 0 && offset != characters) || summary.text.size() + chunk.size() > kMetadataBytes ||
				    (!previous_tool.empty() && !PortableId(previous_tool)) || (summary.characters >= 0 &&
				        (characters != summary.characters || summary.prefix_bytes != prefix_bytes || summary.revision != revision || summary.previous_tool != previous_tool)))
					throw std::runtime_error("summary text snapshot changed or exceeded its bound");
				summary.characters = characters; summary.prefix_bytes = prefix_bytes; summary.revision = revision; summary.previous_tool = previous_tool;
				summary.text += chunk; summary.offset += count; summary.ready = summary.offset == characters;
				if (summary.ready)
				{
					if (static_cast<std::size_t>(prefix_bytes) > summary.text.size()) throw std::runtime_error("invalid native prefix boundary");
					summary.prefix = summary.text.substr(0, static_cast<std::size_t>(prefix_bytes));
					summary.text.erase(0, static_cast<std::size_t>(prefix_bytes));
				}
				std::size_t total = 0;
				for (const std::pair<const Cursor, Summary>& item : context.owner->completed) total += item.second.text.size() + item.second.prefix.size();
				if (total > 4 * 1024 * 1024) throw std::runtime_error("turn summary text limit exceeded");
				ChatSession* chat = ChatDomainService().FindChatById(app, context.identity.chat_id);
				AcpSessionState* session = FindAcpSessionForChat(app, context.identity.chat_id);
				context.query_timeouts = 0; context.retry_at = {};
				return summary.ready && chat && session ? Publish(app, *session, *chat, context) : false;
			}
			if (envelope.at("sessionId") != context.identity.native || !directory_matches || envelope.at("schemaCompatible") != 1)
				throw std::runtime_error("native session, workspace or schema mismatch");
			const nlohmann::json rows = nlohmann::json::parse(envelope.at("rows").get<std::string>());
			if (!rows.is_array() || rows.size() > kPageSize + 1)
				throw std::runtime_error("metadata page limit exceeded");
			if (work.kind == QueryKind::Baseline)
			{
				if (!rows.empty()) throw std::runtime_error("baseline returned historical message records");
				if (!envelope.at("upperTime").is_number_integer()) throw std::runtime_error("invalid baseline time");
				context.cursor = {envelope.at("upperTime").get<std::int64_t>(), envelope.at("upperId").get<std::string>()};
				if (context.cursor.time < 0 || (!context.cursor.id.empty() && !PortableId(context.cursor.id))) throw std::runtime_error("invalid baseline cursor");
				context.baseline = true;
				context.scope = context.cursor;
				context.query_timeouts = 0; context.retry_at = {};
				return false;
			}
			if (work.kind == QueryKind::Pending) context.pending.clear();
			std::set<std::string> unfinished, proven, failed;
			if (work.kind != QueryKind::Pending)
				for (const std::string& marker : context.native_pending_markers) unfinished.insert(marker.substr(15));
			std::set<std::string> published;
			if (context.owner)
			{
				const ChatSession* chat = ChatDomainService().FindChatById(app, context.identity.chat_id);
				if (chat && context.owner->assistant >= 0 && context.owner->assistant < static_cast<int>(chat->messages.size()))
					for (const MessageBlock& block : chat->messages[context.owner->assistant].blocks)
						if (block.type == "context_compaction" && !block.text.empty())
							published.insert(block.request_id_json.starts_with("acp-compaction:") ? block.request_id_json.substr(15) : block.request_id_json);
			}
			Cursor previous = work.cursor;
			const std::size_t count = std::min(rows.size(), kPageSize);
			for (std::size_t index = 0; index < count; ++index)
			{
				const nlohmann::json& row = rows[index];
				if (!PortableId(row.at("id")) || !row.at("created").is_number_integer()) throw std::runtime_error("invalid metadata identity");
				const Cursor cursor{row.at("created").get<std::int64_t>(), row.at("id").get<std::string>()};
				if (work.kind == QueryKind::Identified)
				{
					if (!work.identified.contains(cursor.id)) throw std::runtime_error("unexpected native message identity");
					context.identified.erase(cursor.id);
					context.inspected.insert(cursor.id);
				}
				if (cursor.time < 0 || (work.kind == QueryKind::Page && !(previous < cursor))) throw std::runtime_error("unordered metadata cursor");
				previous = cursor;
				if (work.kind == QueryKind::Page) context.cursor = cursor;
				if (row.at("role") != "assistant") continue;
				const nlohmann::json& summary = row.at("summary");
				if (!summary.is_null() && !summary.is_number_integer()) throw std::runtime_error("invalid summary metadata");
				if (summary != 1) continue;
				if (!PortableId(row.at("parent"))) throw std::runtime_error("invalid compaction parent");
				const nlohmann::json& finish = row.at("finish");
				if (!finish.is_null() && (!finish.is_string() || finish.get_ref<const std::string&>().size() > 128)) throw std::runtime_error("invalid native finish");
				const bool no_error = row.at("errorType").is_null() || row.at("errorType") == "null";
				const bool finished = finish.is_string() && !finish.get_ref<const std::string&>().empty();
				if (row.at("parentProof") == 1)
				{
					proven.insert(cursor.id);
					if (!no_error) failed.insert(cursor.id);
				}
				unfinished.erase(cursor.id);
				if (!finished && no_error && row.at("parentProof") == 1)
				{
					context.pending.insert(cursor.id);
					unfinished.insert(cursor.id);
				}
				else if (finished && no_error && row.at("parentProof") == 1 && context.owner)
				{
					if (published.contains(cursor.id)) continue; // Adoption never requeues an already typed native identity.
					const bool has_text = row.value("textParts", 0) > 0;
					if (context.owner->live && !has_text) context.pending.insert(cursor.id);
					else context.owner->completed.try_emplace(cursor, Summary{.id = cursor.id, .has_text = has_text, .requires_tool = context.adopted});
				}
				if (context.pending.size() > kPendingLimit || (context.owner && context.owner->completed.size() > kPendingLimit))
					throw std::runtime_error("compaction metadata limit exceeded");
			}
			if (!context.owner) throw std::runtime_error("settled owner was replaced");
			context.query_timeouts = 0; context.retry_at = {};
			ChatSession* current_chat = ChatDomainService().FindChatById(app, context.identity.chat_id);
			AcpSessionState* current_session = FindAcpSessionForChat(app, context.identity.chat_id);
			bool state_changed = false;
			if (current_chat && current_session)
			{
				state_changed = QuarantineNativeSummaries(app, *current_session, *current_chat, context, proven);
				state_changed = InterruptNativeSummaries(app, *current_session, *current_chat, context, failed) || state_changed;
				state_changed = SyncNativePending(app, *current_session, *current_chat, context,
				    context.owner->live ? unfinished : std::set<std::string>{}) || state_changed;
			}
			if (work.kind == QueryKind::Identified)
			{
				context.identified_probe_due = false;
				context.next_live_probe = std::chrono::steady_clock::now() + kPendingProbeInterval;
				return (current_chat && current_session ? Publish(app, *current_session, *current_chat, context) : false) || state_changed;
			}
			if (work.kind == QueryKind::Pending)
			{
				context.owner->scan_complete = false;
				context.owner->pages_finished = false;
				if (context.owner->live) context.next_live_probe = std::chrono::steady_clock::now() + kPendingProbeInterval;
				return state_changed;
			}
			context.identified_probe_due = true;
			context.owner->scan_complete = rows.size() <= kPageSize;
			if (context.owner->live)
			{
				context.owner->pages_finished = rows.size() <= kPageSize && !context.pending.empty();
				context.next_live_probe = rows.size() > kPageSize ? std::chrono::steady_clock::time_point{} : std::chrono::steady_clock::now() + kPendingProbeInterval;
				ChatSession* chat = ChatDomainService().FindChatById(app, context.identity.chat_id);
				AcpSessionState* session = FindAcpSessionForChat(app, context.identity.chat_id);
				return (chat && session ? Publish(app, *session, *chat, context) : false) || state_changed;
			}
			if (rows.size() > kPageSize) return state_changed;
			if (!context.pending.empty())
			{
				context.owner->pages_finished = true;
				const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
				if (context.owner->pending_deadline == std::chrono::steady_clock::time_point{})
					context.owner->pending_deadline = now + kPendingDeadline;
				context.owner->next_pending_probe = now + kPendingProbeInterval;
				return false;
			}
			ChatSession* chat = ChatDomainService().FindChatById(app, context.identity.chat_id);
			AcpSessionState* session = FindAcpSessionForChat(app, context.identity.chat_id);
			if (!chat || !session) throw std::runtime_error("settled chat was removed");
			return Publish(app, *session, *chat, context);
		}
		catch (const std::exception& exception)
		{
			context.cursor = work.cursor;
			context.pending = work.pending;
			if (context.owner) context.owner->completed = work.completed;
			context.error = std::string("Native compaction metadata could not be verified: ") + exception.what();
			return false;
		}
	}

	bool QueryAwaited(const Context& context) const
	{
		return (active && active->identity == context.identity &&
		    (!context.owner || active->owner_serial == context.owner->serial)) ||
		    std::chrono::steady_clock::now() < context.retry_at;
	}

	void Schedule()
	{
		if (stopped || active || contexts.empty()) return;
		std::map<std::string, Context>::iterator current = contexts.upper_bound(last_scheduled);
		for (std::size_t scanned = 0; scanned < contexts.size(); ++scanned)
		{
			if (current == contexts.end()) current = contexts.begin();
			Context& context = current->second;
			if (context.owner && !context.owner->live && context.owner->pages_finished && !QueryAwaited(context))
			{
				const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
				if (now >= context.owner->pending_deadline)
					context.error = "Native compaction metadata remained pending after its completed turn. The next prompt was not sent; retry after checking the provider.";
				else if (now < context.owner->next_pending_probe) { ++current; continue; }
			}
			if (std::chrono::steady_clock::now() < context.retry_at) { ++current; continue; }
			if (context.error.empty() && (!context.baseline || context.owner))
			{
				std::shared_ptr<Work> work = std::make_shared<Work>();
				work->identity = context.identity;
				work->cursor = context.cursor;
				work->pending = context.pending;
				work->identified = context.identified;
				if (context.owner)
				{
					work->owner_serial = context.owner->serial; work->owner_assistant = context.owner->assistant;
					work->completed = context.owner->completed;
				}
				work->kind = !context.baseline ? QueryKind::Baseline : (!context.identified.empty() && context.identified_probe_due) ? QueryKind::Identified :
				    ((!context.pending.empty() && context.owner->pages_finished) ? QueryKind::Pending : QueryKind::Page);
				const Summary* text = nullptr;
				if (context.owner)
					for (std::pair<const Cursor, Summary>& item : context.owner->completed)
						if (item.second.has_text && !item.second.ready)
						{
							if (item.second.deadline == std::chrono::steady_clock::time_point{}) item.second.deadline = std::chrono::steady_clock::now() + kSummaryDeadline;
							if (std::chrono::steady_clock::now() >= item.second.deadline) { context.error = "Native compaction summary text exceeded its 90 second deadline."; break; }
							work->kind = QueryKind::Text; work->summary_cursor = item.first; text = &item.second; break;
						}
				if (!context.error.empty()) { ++current; continue; }
				if (context.owner && context.owner->live && !text && std::chrono::steady_clock::now() < context.next_live_probe) { ++current; continue; }
				Query query;
				query.workspace = paths::PathFromUtf8(context.identity.workspace); query.host = context.identity.host;
				query.sql = text ? SummarySql(context.identity, *text, context.scope) : MetadataSql(context.identity, work->kind, context.cursor, work->kind == QueryKind::Identified ? work->identified : work->pending);
				query.argv = context.identity.command;
				query.argv.insert(query.argv.end(), {"db", query.sql, "--format", "json", "--pure"});
				last_scheduled = current->first;
				active = work;
				try
				{
					worker = std::make_unique<std::jthread>([work, query = std::move(query), executor = execute](std::stop_token stop)
					{
						std::stop_callback cancellation(stop, [work] { work->cancellation.request_stop(); });
						const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
						try { work->result = executor(query, work->cancellation.get_token()); }
						catch (const std::exception& exception) { work->result.error = exception.what(); }
						catch (...) { work->result.error = "Metadata worker failed."; }
						work->duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
						work->finished.store(true, std::memory_order_release);
					});
				}
				catch (const std::exception&) { context.error = "Native compaction metadata worker could not start."; active.reset(); }
				return;
			}
			++current;
		}
	}
};

OpenCodeCompactionSyncService::OpenCodeCompactionSyncService(ExecuteQuery execute)
    : m_state(std::make_unique<State>(execute))
{
	OpenCodeCompactionHistoryService::ExecuteQuery history_execute;
	if (execute)
		history_execute = [execute](const OpenCodeCompactionHistoryService::Query& query, std::stop_token stop)
		{
			return execute(Query{query.argv, query.workspace, query.host, query.sql}, stop);
		};
	m_history = std::make_unique<OpenCodeCompactionHistoryService>(std::move(history_execute));
}
OpenCodeCompactionSyncService::~OpenCodeCompactionSyncService() { Stop(); }

void OpenCodeCompactionSyncService::Stop()
{
	if (m_history) m_history->Stop();
	if (!m_state) return;
	m_state->stopped = true;
	if (m_state->active) m_state->active->cancellation.request_stop();
	m_state->worker.reset();
	m_state->active.reset();
	m_state->contexts.clear();
}

void OpenCodeCompactionSyncService::ObserveBeforeTurnReset(AppState& app, AcpSessionState& session, const ChatSession& chat)
{
	if (session.provider_id != provider_ids::kOpenCodeCli || session.model_discovery_only || m_state->stopped) return;
	std::string error;
	State::Context* context = m_state->FindOrCreate(app, session, chat, error);
	if (context)
	{
		if (!context->error.empty() && context->failed_attempt == session.turn_serial &&
		    session.last_settled_turn_serial == session.turn_serial && session.last_turn_outcome == "failed" &&
		    !session.processing && session.prompt_request_id == 0 && !AcpSessionHasPendingCancel(session) &&
		    m_state->Owns(app, session, chat, *context))
		{
			if (m_state->active && m_state->active->identity.chat_id == chat.id) m_state->active->cancellation.request_stop();
			context->error.clear();
			if (!context->owner)
			{
				context->baseline = false;
				context->pending.clear();
			}
			if (context->owner && context->owner->pending_deadline != std::chrono::steady_clock::time_point{})
			{
				context->owner->pending_deadline = std::chrono::steady_clock::now() + kPendingDeadline;
				context->owner->next_pending_probe = std::chrono::steady_clock::now();
			}
		}
		m_state->CaptureOwner(*context, session, chat, true);
	}
	m_state->Schedule();
}

OpenCodeCompactionGateState OpenCodeCompactionSyncService::BeforeDispatch(AppState& app, AcpSessionState& session, ChatSession& chat, std::string& error)
{
	if (session.provider_id != provider_ids::kOpenCodeCli || session.model_discovery_only) return OpenCodeCompactionGateState::NotApplicable;
	if (m_state->stopped) { error = "Native compaction synchronization is stopping."; return OpenCodeCompactionGateState::Failed; }
	const ExecutionHost* host = paths::IsControllerLocalWorkspace(chat) ? nullptr : execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
	if (!MetadataVersionAvailable(app, session, chat) || (host && host->runner_protocol_version < 3))
	{
		const std::string scope = chat.execution_host_id + ":" + session.agent_version + ":" + (host ? std::to_string(host->runner_protocol_version) : "local");
		if (m_state->unsupported_diagnostics.insert(scope).second)
			diagnostics::Write("[OpenCode compaction] Native metadata reconciliation is unavailable for " + scope + "; the existing provider chat remains supported.");
		return OpenCodeCompactionGateState::NotApplicable;
	}
	(void)Poll(app);
	State::Context* context = m_state->FindOrCreate(app, session, chat, error);
	if (!context) return OpenCodeCompactionGateState::Failed;
	if (!context->error.empty())
	{
		context->failed_attempt = session.turn_serial;
		error = context->error;
		return OpenCodeCompactionGateState::Failed;
	}
	m_state->Schedule();
	if (!context->baseline || context->owner) return OpenCodeCompactionGateState::Pending;
	if (context->armed_turn != session.turn_serial) context->scope = context->cursor;
	context->armed_turn = session.turn_serial;
	context->adopted = false;
	context->failed_attempt = 0;
	return OpenCodeCompactionGateState::Ready;
}

bool OpenCodeCompactionSyncService::Poll(AppState& app)
{
	if (m_state->stopped) return false;
	bool changed = m_history && m_history->Poll(app);
	for (const std::unique_ptr<AcpSessionState>& candidate : app.acp_sessions)
	{
		if (!candidate || candidate->provider_id != provider_ids::kOpenCodeCli || candidate->model_discovery_only ||
		    !candidate->processing || !candidate->queued_prompt.empty() ||
		    m_state->contexts.contains(candidate->chat_id)) continue;
		ChatSession* chat = ChatDomainService().FindChatById(app, candidate->chat_id);
		const int assistant = candidate->turn_assistant_message_index;
		if (!chat || !IsActivePrompt(*candidate, *chat) || assistant < 0 || assistant >= static_cast<int>(chat->messages.size()) ||
		    candidate->current_assistant_message_index != assistant || chat->messages[assistant].role != MessageRole::Assistant ||
		    !acp_detail::MessageBlocksEqual(chat->messages[assistant].blocks, acp_detail::MessageBlocksFromTurnEvents(*candidate))) continue;
		const std::optional<std::int64_t> lower = AssistantUtcMilliseconds(chat->messages[assistant].created_at);
		if (!lower) continue;
		std::string ignored;
		State::Context* context = m_state->FindOrCreate(app, *candidate, *chat, ignored);
		if (!context) continue;
		context->baseline = context->adopted = true; context->armed_turn = candidate->turn_serial;
		context->cursor = context->scope = Cursor{*lower, ""};
	}
	for (std::map<std::string, State::Context>::iterator current = m_state->contexts.begin(); current != m_state->contexts.end();)
	{
		State::Context& context = current->second;
		if (context.error.empty() && context.owner && !context.owner->live && !m_state->QueryAwaited(context) && context.owner->pending_deadline != std::chrono::steady_clock::time_point{} &&
		    std::chrono::steady_clock::now() >= context.owner->pending_deadline)
		{
			context.error = "Native compaction metadata remained pending after its completed turn. The next prompt was not sent; retry after checking the provider.";
			if (m_state->active && m_state->active->identity.chat_id == current->first) m_state->active->cancellation.request_stop();
			diagnostics::Write("[OpenCode compaction] " + context.error);
		}
		ChatSession* chat = ChatDomainService().FindChatById(app, current->first);
		AcpSessionState* session = FindAcpSessionForChat(app, current->first);
		if (!chat || !session || !m_state->CheapOwns(app, *session, *chat, current->second))
		{
			if (m_state->active && m_state->active->identity.chat_id == current->first) m_state->active->cancellation.request_stop();
			if (chat && session)
			{
				if (AcpSessionHasPendingCancel(*session) || !session->running || session->remote_source_exit_reported ||
				    session->local_stop_pending || session->remote_stop_pending || session->remote_stop_unconfirmed)
					changed = m_state->InterruptNativeSummaries(app, *session, *chat, context, context.native_summary_ids) || changed;
				changed = m_state->SyncNativePending(app, *session, *chat, context, {}) || changed;
			}
			current = m_state->contexts.erase(current);
			continue;
		}
		if ((!context.error.empty() && !session->processing && !IsActivePrompt(*session, *chat)) ||
		    (session->last_settled_turn_serial == session->turn_serial && session->last_turn_outcome == "failed" && !session->processing))
			changed = m_state->InterruptNativeSummaries(app, *session, *chat, context, context.native_summary_ids) || changed;
		m_state->CaptureOwner(current->second, *session, *chat, false);
		if (context.owner && context.owner->live && m_state->Owns(app, *session, *chat, context))
		{
			if (context.inspected_turn != session->turn_serial)
			{
				context.identified.clear(); context.inspected.clear(); context.inspected_turn = session->turn_serial;
			}
			std::set<std::string> published;
			if (context.owner->assistant >= 0)
				for (const MessageBlock& block : chat->messages[context.owner->assistant].blocks)
					if (block.type == "context_compaction" && !block.text.empty())
						published.insert(block.request_id_json.starts_with("acp-compaction:") ? block.request_id_json.substr(15) : block.request_id_json);
			for (const AcpTurnEventState& event : session->turn_events)
			{
				if (context.identified.size() >= kPendingLimit) break;
				if ((event.type != "assistant_text" && event.type != "context_compaction_summary") ||
				    !event.request_id_json.starts_with("opencode-message:")) continue;
				const std::string id = event.request_id_json.substr(17);
				if (PortableId(id) && !context.inspected.contains(id) && !published.contains(id)) context.identified.insert(id);
			}
		}
		if (!context.error.empty() || !context.owner || !context.owner->live || !m_state->Owns(app, *session, *chat, context))
			changed = m_state->SyncNativePending(app, *session, *chat, context, {}) || changed;
		if (context.error.empty() && context.owner && context.owner->live && !context.owner->completed.empty() &&
		    std::chrono::steady_clock::now() >= context.next_publication_probe)
		{
			context.next_publication_probe = std::chrono::steady_clock::now() + kStreamPublicationInterval;
			if (context.owner->assistant >= 0 && context.owner->assistant < static_cast<int>(chat->messages.size()) &&
			    context.owner->message != MessageOwnership(chat->messages[context.owner->assistant]))
				changed = m_state->Publish(app, *session, *chat, context) || changed;
		}
		++current;
	}
	if (m_state->active && m_state->active->finished.load(std::memory_order_acquire))
	{
		const std::shared_ptr<State::Work> work = std::move(m_state->active);
		// finished is published before the worker's stop callback is destroyed. Joining first
		// prevents jthread destruction from cancelling a successfully completed query.
		if (m_state->worker && m_state->worker->joinable()) m_state->worker->join();
		m_state->worker.reset();
		const std::map<std::string, State::Context>::iterator current = m_state->contexts.find(work->identity.chat_id);
		if (current != m_state->contexts.end() && current->second.error.empty() && current->second.identity == work->identity && !work->cancellation.stop_requested())
		{
			ChatSession* chat = ChatDomainService().FindChatById(app, current->first);
			AcpSessionState* session = FindAcpSessionForChat(app, current->first);
			if (!chat || !session || !m_state->Owns(app, *session, *chat, current->second)) m_state->contexts.erase(current);
			else if (work->owner_serial != 0 && (!current->second.owner || current->second.owner->serial != work->owner_serial ||
			    (current->second.owner->assistant != work->owner_assistant &&
			        !(work->owner_assistant < 0 && current->second.owner->assistant >= 0 && !current->second.adopted))))
			{ /* The old turn's worker cannot publish into a replacement owner. */ }
			else
			{
				changed = m_state->Consume(app, current->second, *work) || changed;
				if (!current->second.error.empty()) diagnostics::Write("[OpenCode compaction] " + current->second.error);
			}
		}
	}
	m_state->Schedule();
	return changed;
}

}
