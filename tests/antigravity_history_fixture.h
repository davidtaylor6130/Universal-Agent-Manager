#pragma once

// Synthetic Antigravity CLI SQLite conversations shared by the parser fixtures and
// the CEF native-refresh handler cases. Each fixture independently encodes the
// pinned source-observed CLI schema; no real profile, provider process,
// transcript or credential is used. Include after the test harness.
#include "common/provider/antigravity_native_history.h"
#include "antigravity_history/legacy_golden.h"
#include <array>
#include <memory>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <string_view>

namespace uam_test::antigravity_fixture
{
	inline constexpr const char* kSession = "3dc412af-b91a-4d9d-82f0-6cd3f991ce2c";
	inline constexpr const char* kTrajectory = "fe61a3f1-c177-49aa-ab0b-e266e7ec5be6";
	inline std::string AgWorkspace()
	{
#if defined(_WIN32)
		return "C:/synthetic/native workspace";
#else
		return "/synthetic/native workspace";
#endif
	}
	inline std::string AgVarint(std::uint64_t value)
	{
		std::string result;
		do
		{
			result += static_cast<char>((value & 127) | (value > 127 ? 128 : 0));
			value >>= 7;
		} while (value);
		return result;
	}
	inline std::string AgNumber(unsigned field, std::uint64_t value)
	{
		return AgVarint(field * 8) + AgVarint(value);
	}
	inline std::string AgBytes(unsigned field, std::string_view bytes)
	{
		return AgVarint(field * 8 + 2) + AgVarint(bytes.size()) + std::string(bytes);
	}
	inline std::string AgStamp()
	{
		return AgNumber(1, 1792756800) + AgNumber(2, 123456789);
	}
	struct AgFixture
	{
		TempDir temp{"uam-antigravity-history-fixture"};
		fs::path file;
		std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> db{nullptr, sqlite3_close_v2};
		std::string workspace = "file:///synthetic/native%20workspace";
		std::string session, trajectory;
		explicit AgFixture(std::string session_id = kSession, std::string trajectory_id = kTrajectory) : session(std::move(session_id)), trajectory(std::move(trajectory_id))
		{
#if defined(_WIN32)
			workspace = "file:///C:/synthetic/native%20workspace";
#endif
			fs::create_directories(temp.root / "conversations");
			file = temp.root / "conversations" / (session + ".db");
			sqlite3* raw = nullptr;
			UAM_ASSERT_EQ(sqlite3_open(file.string().c_str(), &raw), SQLITE_OK);
			db.reset(raw);
			Sql("PRAGMA user_version=1; CREATE TABLE trajectory_meta(trajectory_id TEXT,cascade_id TEXT,trajectory_type INTEGER,source INTEGER); CREATE TABLE trajectory_metadata_blob(id TEXT PRIMARY KEY,data BLOB); CREATE TABLE parent_references(idx INTEGER PRIMARY KEY,data BLOB); CREATE TABLE steps(idx INTEGER PRIMARY KEY,step_type INTEGER,status INTEGER,has_subtrajectory INTEGER,metadata BLOB,error_details BLOB,step_payload BLOB,step_format INTEGER,permissions BLOB,task_details BLOB,render_info BLOB)");
			Sql(("INSERT INTO trajectory_meta VALUES('" + trajectory + "','" + session + "',4,17)").c_str());
			Metadata(workspace);
		}
		void Sql(const char* text)
		{
			UAM_ASSERT_EQ(sqlite3_exec(db.get(), text, nullptr, nullptr, nullptr), SQLITE_OK);
		}
		void Metadata(const std::string& uri, const std::string& id = {}, const std::string& second = {}, const std::string& extra = {})
		{
			const std::string data = AgBytes(1, AgBytes(1, uri) + AgBytes(2, second.empty() ? uri : second)) + AgBytes(2, AgStamp()) + AgBytes(6, id.empty() ? session : id) + AgBytes(7, uri) + extra;
			sqlite3_stmt* raw = nullptr;
			UAM_ASSERT_EQ(sqlite3_prepare_v2(db.get(), "INSERT OR REPLACE INTO trajectory_metadata_blob VALUES('main',?1)", -1, &raw, nullptr), SQLITE_OK);
			const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
			UAM_ASSERT_EQ(sqlite3_bind_blob(raw, 1, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT), SQLITE_OK);
			UAM_ASSERT_EQ(sqlite3_step(raw), SQLITE_DONE);
		}
		void Legacy(bool equal_identity = false)
		{
#if defined(_WIN32)
			const auto& bytes = equal_identity ? legacy_golden::equal_windows : legacy_golden::windows;
#else
			const auto& bytes = equal_identity ? legacy_golden::equal_posix : legacy_golden::posix;
#endif
			LegacyBytes(bytes);
		}
		template <std::size_t N> void LegacyBytes(const std::array<unsigned char, N>& bytes)
		{
			db.reset();
			UAM_ASSERT(fs::remove(file));
			file.replace_extension(".pb");
			std::ofstream out(file, std::ios::binary);
			out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			UAM_ASSERT(out.good());
		}
		void Step(int index, int type, unsigned body_field, const std::string& body, int source, const std::string& call = {}, int status = 3, bool metadata_present = true)
		{
			const std::string meta = AgBytes(1, AgStamp()) + AgNumber(3, source) + (call.empty() ? "" : AgBytes(4, call)) + AgBytes(20, AgBytes(1, trajectory) + AgNumber(2, index) + AgBytes(4, session));
			const std::string payload = AgNumber(1, type) + AgNumber(4, status) + (metadata_present ? AgBytes(5, meta) : "") + AgBytes(body_field, body);
			sqlite3_stmt* raw = nullptr;
			UAM_ASSERT_EQ(sqlite3_prepare_v2(db.get(), "INSERT INTO steps VALUES(?1,?2,?3,0,?4,X'',?5,0,NULL,NULL,NULL)", -1, &raw, nullptr), SQLITE_OK);
			const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
			sqlite3_bind_int(raw, 1, index);
			sqlite3_bind_int(raw, 2, type);
			sqlite3_bind_int(raw, 3, status);
			if (metadata_present)
				sqlite3_bind_blob(raw, 4, meta.data(), static_cast<int>(meta.size()), SQLITE_TRANSIENT);
			else
				sqlite3_bind_null(raw, 4);
			sqlite3_bind_blob(raw, 5, payload.data(), static_cast<int>(payload.size()), SQLITE_TRANSIENT);
			UAM_ASSERT_EQ(sqlite3_step(raw), SQLITE_DONE);
		}
		void User(const std::string& text = "Read the synthetic fixture", const std::string& extra = {})
		{
			Step(0, 14, 19, AgBytes(2, text) + AgBytes(3, AgBytes(1, text)) + extra, 4);
		}
		void Render(int index, const std::optional<std::string>& bytes)
		{
			sqlite3_stmt* raw = nullptr;
			UAM_ASSERT_EQ(sqlite3_prepare_v2(db.get(), "UPDATE steps SET render_info=?1 WHERE idx=?2", -1, &raw, nullptr), SQLITE_OK);
			const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
			if (bytes) sqlite3_bind_blob(raw, 1, bytes->data(), static_cast<int>(bytes->size()), SQLITE_TRANSIENT);
			else sqlite3_bind_null(raw, 1);
			sqlite3_bind_int(raw, 2, index);
			UAM_ASSERT_EQ(sqlite3_step(raw), SQLITE_DONE);
		}
		void Summary(int steps, const std::string& uri, const std::string& source = "antigravity-cli")
		{
			sqlite3* raw = nullptr;
			const auto path = temp.root / "conversation_summaries.db";
			UAM_ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
			const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> index(raw, sqlite3_close_v2);
			UAM_ASSERT_EQ(sqlite3_exec(raw, "CREATE TABLE IF NOT EXISTS conversation_summaries(conversation_id TEXT,workspace_uris TEXT,step_count INTEGER,source TEXT,parent_conversation_id TEXT,nesting_depth INTEGER,not_fully_idle INTEGER,battle_id TEXT); DELETE FROM conversation_summaries", nullptr, nullptr, nullptr), SQLITE_OK);
			sqlite3_stmt* statement = nullptr;
			UAM_ASSERT_EQ(sqlite3_prepare_v2(raw, "INSERT INTO conversation_summaries VALUES(?1,?2,?3,?4,'',0,0,'')", -1, &statement, nullptr), SQLITE_OK);
			const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> insert(statement, sqlite3_finalize);
			const auto uris = "[\"" + uri + "\"]";
			sqlite3_bind_text(statement, 1, session.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(statement, 2, uris.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(statement, 3, steps);
			sqlite3_bind_text(statement, 4, source.c_str(), -1, SQLITE_TRANSIENT);
			UAM_ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
		}
		uam::antigravity::history::Discovery Read()
		{
			return uam::antigravity::history::Discover(temp.root);
		}
	};
} // namespace uam_test::antigravity_fixture
