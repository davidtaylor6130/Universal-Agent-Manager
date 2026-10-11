#include "common/usage/antigravity_history_scan.h"
#include "common/memory/memory_storage_guard.h"
#include "common/paths/path_utils.h"
#include <chrono>
#include <sqlite3.h>

namespace uam::usage
{
	namespace
	{
		using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
		std::optional<std::string> Revision(const std::filesystem::path& path)
		{
			std::filesystem::path wal = path; wal += "-wal";
			const std::optional<std::string> main = memory::StorageRevision(path), sidecar = memory::StorageRevision(wal);
			return main && *main != "missing" && sidecar ? std::optional<std::string>{*main + "/" + *sidecar} : std::nullopt;
		}
		struct Budget
		{
			const std::function<bool()>& cancelled;
			std::chrono::steady_clock::time_point deadline;
		};
		int Stop(void* context)
		{
			const Budget& budget = *static_cast<const Budget*>(context);
			return (budget.cancelled && budget.cancelled()) || std::chrono::steady_clock::now() >= budget.deadline;
		}
	}
	struct AntigravityHistoryScan::State
	{
		std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database{nullptr, sqlite3_close_v2};
		std::string revision;
		std::int64_t fallback_ms = 0;
		std::optional<std::int64_t> trajectory_ms;
		std::size_t phase = 0, invalid_rows = 0, rows = 0, bytes = 0;
		std::int64_t cursor = 0;
		bool saw_schema = false;
		std::map<std::int64_t, std::string> models;
		std::vector<AntigravityUsageCandidate> candidates, generations;
	};
	AntigravityHistoryScan::AntigravityHistoryScan() = default;
	AntigravityHistoryScan::~AntigravityHistoryScan() = default;

	ScanResult AntigravityHistoryScan::Refresh(const std::filesystem::path& path, const std::function<bool()>& cancelled)
	{
		ScanResult result;
		if (cancelled && cancelled()) { m_state.reset(); result.cancelled = true; return result; }
		const std::optional<std::string> revision = Revision(path);
		if (!revision) { m_state.reset(); result.error = "Could not inspect Antigravity history."; return result; }
		if (m_path != path) { m_state.reset(); m_revision.clear(); m_candidates.clear(); m_invalidRows = 0; m_path = path; }
		if (!m_state && m_revision == *revision) { result.complete = true; return result; }
		const std::function<ScanResult(const char*)> fail = [&](const char* error) { m_state.reset(); result.cancelled = cancelled && cancelled(); if (!result.cancelled) result.error = error; return result; };
		if (!m_state)
		{
			m_state = std::make_unique<State>(); m_state->revision = *revision;
			std::error_code error;
			const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, error);
			if (error) return fail("Could not inspect Antigravity history time.");
#if defined(_WIN32)
			m_state->fallback_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::clock_cast<std::chrono::system_clock>(modified).time_since_epoch()).count();
#else
			m_state->fallback_ms = std::chrono::duration_cast<std::chrono::milliseconds>(decltype(modified)::clock::to_sys(modified).time_since_epoch()).count();
#endif
			std::filesystem::path native; std::string path_error;
			if (!io::NativeFilePath(path, native, path_error)) return fail("Could not resolve Antigravity history path.");
			const std::u8string utf8 = native.u8string(); const std::string filename(reinterpret_cast<const char*>(utf8.data()), utf8.size());
			sqlite3* raw = nullptr;
			const int opened = sqlite3_open_v2(filename.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
			m_state->database.reset(raw);
			if (opened != SQLITE_OK) return fail("Could not open Antigravity history read-only.");
			sqlite3_busy_timeout(raw,50); sqlite3_limit(raw,SQLITE_LIMIT_LENGTH,8*1024*1024);
			if (sqlite3_exec(raw,"PRAGMA query_only=ON; BEGIN",nullptr,nullptr,nullptr)!=SQLITE_OK) return fail("Could not start Antigravity history snapshot.");
		}
		State& state = *m_state;
		Budget budget{cancelled,std::chrono::steady_clock::now()+std::chrono::milliseconds(250)};
		sqlite3_progress_handler(state.database.get(),1000,Stop,&budget);
		const char* table = state.phase == 0 ? "trajectory_metadata_blob" : state.phase == 1 ? "gen_metadata" : "steps";
		sqlite3_stmt* raw_probe = nullptr;
		if (sqlite3_prepare_v2(state.database.get(),"SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",-1,&raw_probe,nullptr)!=SQLITE_OK) return fail("Could not inspect Antigravity history schema.");
		Statement probe(raw_probe,sqlite3_finalize); sqlite3_bind_text(raw_probe,1,table,-1,SQLITE_STATIC);
		const int present = sqlite3_step(raw_probe);
		if (present == SQLITE_DONE) { ++state.phase; state.cursor = 0; }
		else if (present != SQLITE_ROW) return fail("Could not inspect Antigravity history schema.");
		else
		{
			if (state.phase != 0) state.saw_schema = true;
			const std::string query = "SELECT rowid," + std::string(state.phase == 0 ? "0,data" : state.phase == 1 ? "idx,data" : "idx,metadata") + " FROM " + table +
			    " WHERE rowid>?1" + (state.phase == 2 ? " AND metadata IS NOT NULL" : "") + " ORDER BY rowid LIMIT 512";
			sqlite3_stmt* raw = nullptr;
			if (sqlite3_prepare_v2(state.database.get(),query.c_str(),-1,&raw,nullptr)!=SQLITE_OK) return fail("Antigravity history schema is unsupported.");
			Statement statement(raw,sqlite3_finalize); sqlite3_bind_int64(raw,1,state.cursor);
			std::size_t batch_rows = 0;
			int status;
			while ((status=sqlite3_step(raw))==SQLITE_ROW)
			{
				if (cancelled && cancelled()) { statement.reset(); probe.reset(); m_state.reset(); result.cancelled=true; return result; }
				state.cursor=sqlite3_column_int64(raw,0); ++batch_rows;
				if (++state.rows > 100000) return fail("Antigravity history exceeds the row limit.");
				const int size=sqlite3_column_bytes(raw,2);
				result.bytes_read += std::max(0,size)+1;
				if (sqlite3_column_type(raw,1)!=SQLITE_INTEGER || sqlite3_column_int64(raw,1)<0 || sqlite3_column_type(raw,2)!=SQLITE_BLOB || size<=0 || size>8*1024*1024)
				{ ++state.invalid_rows; continue; }
				const std::string_view blob(static_cast<const char*>(sqlite3_column_blob(raw,2)),static_cast<std::size_t>(size));
				if (state.phase==0)
				{
					std::optional<std::int64_t> timestamp;
					if (!DecodeAntigravityTrajectoryTimestamp(blob,timestamp)) ++state.invalid_rows;
					else if (!state.trajectory_ms) state.trajectory_ms=timestamp;
				}
				else
				{
					const std::int64_t index=sqlite3_column_int64(raw,1);
					const std::u8string stem=path.stem().u8string(); const std::string session(reinterpret_cast<const char*>(stem.data()),stem.size());
					const std::string model=state.models.contains(index) ? state.models.at(index) : "";
					std::optional<AntigravityMetadata> metadata=DecodeAntigravityMetadata(blob,session,index,state.phase==2,state.fallback_ms,state.trajectory_ms,model);
					if (!metadata) { ++state.invalid_rows; continue; }
					if (state.phase==1)
					{
						std::size_t model_bytes=metadata->model.size();
						const std::map<std::int64_t,std::string>::iterator existing=state.models.find(index);
						if (existing!=state.models.end()) state.bytes-=existing->second.size();
						else model_bytes+=sizeof(std::pair<const std::int64_t,std::string>)+32;
						if (model_bytes>64*1024*1024-state.bytes) return fail("Antigravity model lookup exceeds the accounting limit.");
						state.bytes+=model_bytes; state.models[index]=metadata->model;
					}
					for (AntigravityUsageCandidate& candidate : metadata->candidates)
					{
						std::size_t weight=sizeof(candidate)+candidate.record.model_id.size()+candidate.record.session_id.size()+candidate.record.dedupe_key.size()+candidate.location_id.size();
						for (const std::string& alias : candidate.aliases) weight+=alias.size();
						if (state.candidates.size()+state.generations.size()==100000 || weight>64*1024*1024-state.bytes) return fail("Antigravity history exceeds the accounting limit.");
						state.bytes+=weight;
						if (state.phase==1) state.generations.push_back(std::move(candidate));
						else state.candidates.push_back(std::move(candidate));
					}
				}
				if (result.bytes_read>=32*1024*1024 || std::chrono::steady_clock::now()>=budget.deadline) break;
			}
			if (status!=SQLITE_ROW && status!=SQLITE_DONE) return fail("Could not finish Antigravity history batch.");
			if (status==SQLITE_DONE && batch_rows<512) { ++state.phase; state.cursor=0; }
		}
		// Do not retain a callback pointing at this stack frame between worker batches.
		sqlite3_progress_handler(state.database.get(),0,nullptr,nullptr);
		if (state.phase==3)
		{
			if (!state.saw_schema) return fail("Antigravity history schema is unsupported.");
			// Prefer explicit step model names while keeping generation rows available for alias merging.
			state.candidates.insert(state.candidates.end(),std::make_move_iterator(state.generations.begin()),std::make_move_iterator(state.generations.end()));
			m_revision=state.revision; m_candidates=std::move(state.candidates); m_invalidRows=state.invalid_rows;
			m_state.reset(); result.reset=true; result.complete=true;
		}
		return result;
	}
}
