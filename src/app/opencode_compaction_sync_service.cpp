#include "app/opencode_compaction_sync_service.h"

#include "app/chat_domain_service.h"
#include "app/provider_worker_command.h"
#include "common/config/execution_host_config.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_session_internal.h"
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
	constexpr int kQueryTimeoutMs = 5000;
	constexpr std::chrono::seconds kPendingProbeInterval{2};
	constexpr std::chrono::seconds kPendingDeadline{10};

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

	enum class QueryKind { Baseline, Pending, Page };

	struct Identity
	{
		std::filesystem::path root;
		std::shared_ptr<const std::uint8_t> incarnation;
		std::string chat_id, created, provider, native, stored_native, process, process_handle, workspace;
		std::vector<std::string> command;
		std::optional<ExecutionHost> host;
		bool operator==(const Identity& other) const
		{
			return root == other.root && incarnation == other.incarnation && chat_id == other.chat_id &&
			    created == other.created && provider == other.provider && native == other.native && stored_native == other.stored_native &&
			    process == other.process && process_handle == other.process_handle && workspace == other.workspace && command == other.command &&
			    host.has_value() == other.host.has_value() && (!host || execution_hosts::SameConnection(*host, *other.host));
		}
	};

	std::optional<Identity> CaptureIdentity(const AppState& app, const AcpSessionState& session,
	    const ChatSession& chat, std::string& error)
	{
		if (session.agent_version != "1.18.35")
		{ error = "Native compaction synchronization requires verified OpenCode 1.18.35 metadata."; return {}; }
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
		// which this query inspects only for the indexed parent compaction type predicate.
		return "WITH selected AS (SELECT m.id,m.time_created,json_extract(m.data,'$.role') role,"
		    "CASE WHEN json_extract(m.data,'$.role')='assistant' THEN json_extract(m.data,'$.summary') ELSE NULL END summary,"
		    "json_extract(m.data,'$.parentID') parent,"
		    "json_extract(m.data,'$.finish') finish,json_type(m.data,'$.error') error_type,"
		    "CASE WHEN json_extract(m.data,'$.summary')=1 THEN EXISTS(SELECT 1 FROM message u WHERE "
		    "u.id=json_extract(m.data,'$.parentID') AND u.session_id=m.session_id AND json_extract(u.data,'$.role')='user' "
		    "AND EXISTS(SELECT 1 FROM part p WHERE p.message_id=u.id AND p.session_id=u.session_id AND "
		    "json_extract(p.data,'$.type')='compaction')) ELSE 0 END parent_proof FROM message m WHERE " + filter +
		    " ORDER BY m.time_created,m.id LIMIT " + std::to_string(kPageSize + 1) +
		    ") SELECT s.id sessionId,s.directory,"
		    "EXISTS(SELECT 1 FROM sqlite_master WHERE type='index' AND name='message_session_time_created_id_idx') AND "
		    "EXISTS(SELECT 1 FROM sqlite_master WHERE type='index' AND name='part_message_id_id_idx') schemaCompatible,"
		    "coalesce((SELECT time_created FROM message WHERE session_id=s.id ORDER BY time_created DESC,id DESC LIMIT 1),0) upperTime,"
		    "coalesce((SELECT id FROM message WHERE session_id=s.id ORDER BY time_created DESC,id DESC LIMIT 1),'') upperId,"
		    "(SELECT json_group_array(json_object('id',id,'created',time_created,'role',role,'summary',summary,"
		    "'parent',parent,'finish',finish,'errorType',error_type,'parentProof',parent_proof)) FROM selected) rows "
		    "FROM session s WHERE s.id=" + session;
	}

	ProcessExecutionResult ExecuteNativeQuery(const OpenCodeCompactionSyncService::Query& query, std::stop_token stop)
	{
		if (query.host)
		{
			const ExecutionHost& host = *query.host;
			remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
			    remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version),
			    host.runner_version, host.runner_protocol_version);
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
		return value.is_string() && value.get_ref<const std::string&>().size() <= 512 &&
		    execution_hosts::IsPortableId(value.get_ref<const std::string&>()) && !value.get_ref<const std::string&>().starts_with('-');
	}
}

struct OpenCodeCompactionSyncService::State
{
	struct Owner
	{
		int serial = 0, assistant = -1;
		std::uint64_t message = 0;
		int handoff_serial = 0;
		bool pages_finished = false;
		std::chrono::steady_clock::time_point pending_deadline{};
		std::chrono::steady_clock::time_point next_pending_probe{};
		std::map<Cursor, std::string> completed;
	};
	struct Context
	{
		Identity identity;
		Cursor cursor;
		std::set<std::string> pending;
		bool baseline = false;
		int armed_turn = 0, finished_turn = 0, failed_attempt = 0;
		std::optional<Owner> owner;
		std::string error;
	};
	struct Work
	{
		Identity identity;
		QueryKind kind = QueryKind::Baseline;
		Cursor cursor;
		std::set<std::string> pending;
		std::map<Cursor, std::string> completed;
		std::stop_source cancellation;
		std::atomic<bool> finished{false};
		ProcessExecutionResult result;
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

	Context* FindOrCreate(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string& error)
	{
		const std::optional<Identity> identity = CaptureIdentity(app, session, chat, error);
		if (!identity) return nullptr;
		Context& context = contexts[chat.id];
		if (!(context.identity == *identity))
		{
			if (active && active->identity.chat_id == chat.id) active->cancellation.request_stop();
			context = Context{};
			context.identity = *identity;
		}
		return &context;
	}

	void CaptureOwner(Context& context, const AcpSessionState& session, const ChatSession& chat, bool handoff)
	{
		if (context.owner)
		{
			if (handoff && (context.owner->serial == session.turn_serial || context.failed_attempt == session.turn_serial))
				context.owner->handoff_serial = session.turn_serial + 1;
			return;
		}
		if (!context.baseline || context.armed_turn == 0 || context.finished_turn == context.armed_turn ||
		    session.turn_serial != context.armed_turn || session.last_settled_turn_serial != context.armed_turn ||
		    session.processing || session.prompt_request_id != 0 || AcpSessionHasPendingCancel(session)) return;
		if (session.last_turn_outcome != "completed")
		{
			context.finished_turn = context.armed_turn;
			context.baseline = false;
			context.pending.clear();
			return;
		}
		const int index = session.turn_assistant_message_index;
		if (index < 0 || index >= static_cast<int>(chat.messages.size()) || chat.messages[index].role != MessageRole::Assistant)
		{
			context.finished_turn = context.armed_turn;
			context.baseline = false;
			context.pending.clear();
			context.error = "Native compaction has no unchanged completed assistant owner.";
			return;
		}
		context.owner = Owner{.serial = session.turn_serial, .assistant = index,
		    .message = MessageOwnership(chat.messages[index]), .handoff_serial = handoff ? session.turn_serial + 1 : 0};
	}

	bool Owns(const AppState& app, const AcpSessionState& session, const ChatSession& chat, const Context& context) const
	{
		if (!CheapOwns(app, session, chat, context)) return false;
		std::string ignored;
		const std::optional<Identity> current = CaptureIdentity(app, session, chat, ignored);
		if (!current || !(*current == context.identity) || AcpSessionHasPendingCancel(session)) return false;
		if (!context.owner) return true;
		const Owner& owner = *context.owner;
		if (owner.assistant < 0 || owner.assistant >= static_cast<int>(chat.messages.size()) ||
		    chat.messages[owner.assistant].role != MessageRole::Assistant || MessageOwnership(chat.messages[owner.assistant]) != owner.message) return false;
		return (session.turn_serial == owner.serial && session.last_settled_turn_serial == owner.serial &&
		        !session.processing && session.prompt_request_id == 0) ||
		    (owner.handoff_serial == session.turn_serial && session.prompt_request_id == 0 && !session.queued_prompt.empty()) ||
		    (context.failed_attempt == session.turn_serial && session.last_settled_turn_serial == context.failed_attempt &&
		        session.last_turn_outcome == "failed" && !session.processing && session.prompt_request_id == 0);
	}

	bool Publish(AppState& app, AcpSessionState& session, ChatSession& chat, Context& context)
	{
		if (!Owns(app, session, chat, context))
		{ context.error = "Native compaction ownership changed before publication."; return false; }
		Owner& owner = *context.owner;
		bool changed = false;
		for (const std::pair<const Cursor, std::string>& completed : owner.completed)
		{
			const std::string& id = completed.second;
			const std::string identity = "acp-compaction:" + id;
			const bool exists = std::ranges::any_of(chat.messages, [&](const Message& message)
			{
				return std::ranges::any_of(message.blocks, [&](const MessageBlock& block)
				{ return block.type == "context_compaction" && (block.request_id_json == id || block.request_id_json == identity); });
			});
			if (exists) continue;
			Message& message = chat.messages[owner.assistant];
			if (message.blocks.empty())
			{
				AcpSessionState restored;
				acp_detail::RestoreTurnEventsFromMessageBlocks(restored, message);
				message.blocks = acp_detail::MessageBlocksFromTurnEvents(restored);
			}
			message.blocks.push_back({"context_compaction", "", "", identity});
			changed = true;
		}
		context.finished_turn = owner.serial;
		context.owner.reset();
		if (changed) acp_detail::ScheduleChatSave(app, chat, 0.0);
		return changed;
	}

	bool Consume(AppState& app, Context& context, const Work& work)
	{
		if (!work.result.ok || work.result.output_truncated || work.result.canceled || work.result.timed_out ||
		    work.result.output.size() > kMetadataBytes)
		{
			const std::string detail = work.result.output.size() > kMetadataBytes ? "metadata exceeded the 512 KiB limit" :
			    !work.result.error.empty() ? work.result.error : work.result.timed_out ? "the query timed out" :
			    work.result.canceled ? "the query was cancelled" : work.result.output_truncated ? "the command output limit was exceeded" :
			    "OpenCode exited with code " + std::to_string(work.result.exit_code);
			context.error = "Native compaction metadata query failed: " + detail;
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
				return false;
			}
			if (work.kind == QueryKind::Pending) context.pending.clear();
			Cursor previous = work.cursor;
			const std::size_t count = std::min(rows.size(), kPageSize);
			for (std::size_t index = 0; index < count; ++index)
			{
				const nlohmann::json& row = rows[index];
				if (!PortableId(row.at("id")) || !row.at("created").is_number_integer()) throw std::runtime_error("invalid metadata identity");
				const Cursor cursor{row.at("created").get<std::int64_t>(), row.at("id").get<std::string>()};
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
				if (!finished && no_error && row.at("parentProof") == 1) context.pending.insert(cursor.id);
				else if (finished && no_error && row.at("parentProof") == 1 && context.owner)
					context.owner->completed.emplace(cursor, cursor.id);
				if (context.pending.size() > kPendingLimit || (context.owner && context.owner->completed.size() > kPendingLimit))
					throw std::runtime_error("compaction metadata limit exceeded");
			}
			if (!context.owner) throw std::runtime_error("settled owner was replaced");
			if (work.kind == QueryKind::Pending) { context.owner->pages_finished = false; return false; }
			if (rows.size() > kPageSize) return false;
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

	void Schedule()
	{
		if (stopped || active || contexts.empty()) return;
		std::map<std::string, Context>::iterator current = contexts.upper_bound(last_scheduled);
		for (std::size_t scanned = 0; scanned < contexts.size(); ++scanned)
		{
			if (current == contexts.end()) current = contexts.begin();
			Context& context = current->second;
			if (context.owner && context.owner->pages_finished)
			{
				const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
				if (now >= context.owner->pending_deadline)
					context.error = "Native compaction metadata remained pending after its completed turn. The next prompt was not sent; retry after checking the provider.";
				else if (now < context.owner->next_pending_probe) { ++current; continue; }
			}
			if (context.error.empty() && (!context.baseline || context.owner))
			{
				std::shared_ptr<Work> work = std::make_shared<Work>();
				work->identity = context.identity;
				work->cursor = context.cursor;
				work->pending = context.pending;
				if (context.owner) work->completed = context.owner->completed;
				work->kind = !context.baseline ? QueryKind::Baseline :
				    (context.owner->pages_finished && !context.pending.empty() ? QueryKind::Pending : QueryKind::Page);
				Query query;
				query.workspace = paths::PathFromUtf8(context.identity.workspace); query.host = context.identity.host;
				query.sql = MetadataSql(context.identity, work->kind, context.cursor, context.pending);
				query.argv = context.identity.command;
				query.argv.insert(query.argv.end(), {"db", query.sql, "--format", "json", "--pure"});
				last_scheduled = current->first;
				active = work;
				try
				{
					worker = std::make_unique<std::jthread>([work, query = std::move(query), executor = execute](std::stop_token stop)
					{
						std::stop_callback cancellation(stop, [work] { work->cancellation.request_stop(); });
						try { work->result = executor(query, work->cancellation.get_token()); }
						catch (const std::exception& exception) { work->result.error = exception.what(); }
						catch (...) { work->result.error = "Metadata worker failed."; }
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

OpenCodeCompactionSyncService::OpenCodeCompactionSyncService(ExecuteQuery execute) : m_state(std::make_unique<State>(std::move(execute))) {}
OpenCodeCompactionSyncService::~OpenCodeCompactionSyncService() { Stop(); }

void OpenCodeCompactionSyncService::Stop()
{
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
	if (session.agent_version != "1.18.35" || (host && host->runner_protocol_version < 3))
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
	context->armed_turn = session.turn_serial;
	context->failed_attempt = 0;
	return OpenCodeCompactionGateState::Ready;
}

bool OpenCodeCompactionSyncService::Poll(AppState& app)
{
	if (m_state->stopped) return false;
	bool changed = false;
	for (std::map<std::string, State::Context>::iterator current = m_state->contexts.begin(); current != m_state->contexts.end();)
	{
		State::Context& context = current->second;
		if (context.error.empty() && context.owner && context.owner->pending_deadline != std::chrono::steady_clock::time_point{} &&
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
			current = m_state->contexts.erase(current);
			continue;
		}
		m_state->CaptureOwner(current->second, *session, *chat, false);
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
			else
			{
				changed = m_state->Consume(app, current->second, *work);
				if (!current->second.error.empty()) diagnostics::Write("[OpenCode compaction] " + current->second.error);
			}
		}
	}
	m_state->Schedule();
	return changed;
}

}
