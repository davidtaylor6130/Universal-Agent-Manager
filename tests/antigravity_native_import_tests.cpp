// Synthetic fixtures independently encode the pinned source-observed CLI schema;
// no real profile, provider process, transcript or credential is used.
#if defined(UAM_ANTIGRAVITY_HISTORY_STANDALONE)
#include "antigravity_native_import_test_support.h"
#else
#include "test_harness.h"
#endif
#include "common/provider/antigravity_native_history.h"
#include "common/chat/native_attachment_assets.h"
#include "common/memory/memory_storage_guard.h"
#include "common/platform/private_temp_directory.h"
#include "antigravity_history/legacy_golden.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <sstream>
#if !defined(UAM_ANTIGRAVITY_HISTORY_STANDALONE)
#include "app/startup_native_import_service.h"
#include "common/chat/message_attachment_sources.h"
#endif

#if defined(UAM_ANTIGRAVITY_HISTORY_STANDALONE) || UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
using namespace uam_test;
namespace
{
	constexpr const char* kSession = "3dc412af-b91a-4d9d-82f0-6cd3f991ce2c";
	constexpr const char* kTrajectory = "fe61a3f1-c177-49aa-ab0b-e266e7ec5be6";
	std::string AgWorkspace()
	{
#if defined(_WIN32)
		return "C:/synthetic/native workspace";
#else
		return "/synthetic/native workspace";
#endif
	}
	std::string AgVarint(std::uint64_t value)
	{
		std::string result;
		do
		{
			result += static_cast<char>((value & 127) | (value > 127 ? 128 : 0));
			value >>= 7;
		} while (value);
		return result;
	}
	std::string AgNumber(unsigned field, std::uint64_t value)
	{
		return AgVarint(field * 8) + AgVarint(value);
	}
	std::string AgBytes(unsigned field, std::string_view bytes)
	{
		return AgVarint(field * 8 + 2) + AgVarint(bytes.size()) + std::string(bytes);
	}
	std::string AgStamp()
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
		void Summary(int steps, const std::string& uri)
		{
			sqlite3* raw = nullptr;
			const auto path = temp.root / "conversation_summaries.db";
			UAM_ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
			const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> index(raw, sqlite3_close_v2);
			UAM_ASSERT_EQ(sqlite3_exec(raw, "CREATE TABLE IF NOT EXISTS conversation_summaries(conversation_id TEXT,workspace_uris TEXT,step_count INTEGER,source TEXT,parent_conversation_id TEXT,nesting_depth INTEGER,not_fully_idle INTEGER,battle_id TEXT); DELETE FROM conversation_summaries", nullptr, nullptr, nullptr), SQLITE_OK);
			sqlite3_stmt* statement = nullptr;
			UAM_ASSERT_EQ(sqlite3_prepare_v2(raw, "INSERT INTO conversation_summaries VALUES(?1,?2,?3,'antigravity-cli','',0,0,'')", -1, &statement, nullptr), SQLITE_OK);
			const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> insert(statement, sqlite3_finalize);
			const auto uris = "[\"" + uri + "\"]";
			sqlite3_bind_text(statement, 1, session.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(statement, 2, uris.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(statement, 3, steps);
			UAM_ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
		}
		uam::antigravity::history::Discovery Read()
		{
			return uam::antigravity::history::Discover(temp.root);
		}
	};
	std::string AgCall()
	{
		return AgBytes(1, "native-call-exact") + AgBytes(2, "run_command") + AgBytes(3, "{\"command\":\"synthetic-only\"}");
	}
	void AgMetadataOnlyRows(AgFixture& fixture)
	{
		fixture.Step(0, 14, 19, {}, 4);
		fixture.Step(1, 14, 19, AgBytes(2, "Original request"), 4);
		fixture.Step(2, 15, 20, AgBytes(4, "opaque signature"), 2);
		fixture.Step(3, 14, 19, AgBytes(2, " \n\t"), 4);
		fixture.Step(4, 15, 20, AgBytes(7, AgCall()), 2);
		fixture.Step(5, 15, 20, AgBytes(6, "opaque response metadata"), 2);
		fixture.Step(6, 21, 28, AgBytes(21, AgBytes(1, "Exact call output")), 2, AgCall());
		fixture.Step(7, 15, 20, AgBytes(3, "Reasoning alone stays visible"), 2);
		fixture.Step(8, 15, 20, {}, 2, {}, 6);
	}
} // namespace

UAM_TEST(AntigravityNativeHistoryReadsOriginalWorkspaceUuidReasoningToolsAndInertInputWithoutWritingSource)
{
	AgFixture fixture;
	fixture.User();
	fixture.Step(1, 15, 20, AgBytes(1, "I will inspect it.") + AgBytes(3, "Preserved native reasoning") + AgBytes(7, AgCall()), 2);
	fixture.Step(2, 21, 28, AgBytes(21, AgBytes(1, "synthetic output")), 2, AgCall());
	const auto revision = uam::memory::StorageRevision(fixture.file);
	const auto before = ReadFile(fixture.file);
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats.size(), std::size_t(1));
	const ChatSession& chat = result.chats.front();
	UAM_ASSERT_EQ(chat.native_session_id, std::string(kSession));
	UAM_ASSERT_EQ(chat.id, std::string(kSession));
	UAM_ASSERT_EQ(chat.workspace_directory, uam::paths::Utf8PathString(fs::path(AgWorkspace())));
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t(2));
	UAM_ASSERT_EQ(chat.messages[0].role, MessageRole::User);
	UAM_ASSERT(!chat.messages[0].native_input_id);
	UAM_ASSERT(!chat.messages[0].provider_event_ids.empty());
	UAM_ASSERT(!chat.messages[0].input_write_completed);
	UAM_ASSERT(!chat.messages[0].origin_request_id);
	UAM_ASSERT_EQ(chat.messages[1].thoughts, std::string("Preserved native reasoning"));
	UAM_ASSERT_EQ(chat.messages[1].tool_calls[0].id, std::string("native-call-exact"));
	UAM_ASSERT_EQ(chat.messages[1].tool_calls[0].result_text, std::string("synthetic output"));
	UAM_ASSERT_EQ(chat.messages[1].tool_calls[0].status, std::string("completed"));
	UAM_ASSERT(std::ranges::any_of(chat.messages[1].blocks, [](const auto& block) { return block.type == "native_tool_result"; }));
	UAM_ASSERT(chat.messages[1].model_id.empty());
	UAM_ASSERT_EQ(chat.messages[0].created_at, std::string("2026-10-23T12:00:00.123456789Z"));
	UAM_ASSERT_EQ(uam::memory::StorageRevision(fixture.file), revision);
	UAM_ASSERT_EQ(ReadFile(fixture.file), before);
	UAM_ASSERT(!fs::exists(fixture.file.string() + "-wal"));
	UAM_ASSERT(!fs::exists(fixture.file.string() + "-shm"));
}

UAM_TEST(AntigravityNativeHistoryMcpImagesRetainExactCallAndOriginalBytes)
{
	AgFixture fixture;
	fixture.User();
	fixture.Step(1, 15, 20, AgBytes(7, AgCall()), 2);
	fixture.Step(2, 38, 47, AgBytes(2, AgCall()) + AgBytes(3, "Original MCP result") + AgBytes(5, AgBytes(1, "aGVsbG8=") + AgBytes(2, "image/png")), 2, AgCall());
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats.size(), std::size_t{1});
	const auto& messages = result.chats[0].messages;
	UAM_ASSERT_EQ(messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(messages[1].tool_calls[0].result_text, std::string("Original MCP result"));
	UAM_ASSERT_EQ(messages[1].attachments.size(), std::size_t{1});
	const auto& image = messages[1].attachments[0];
	UAM_ASSERT_EQ(image.native_tool_call_id, std::string("native-call-exact"));
	UAM_ASSERT_EQ(image.source, AttachmentSource::NativeInlinePending);
	UAM_ASSERT_EQ(image.native_source_field, std::string("CortexStepMcpTool.images[0]"));
	UAM_ASSERT(image.native_part_key.find(":2:image:0") != std::string::npos);
	UAM_ASSERT(image.native_inline_payload);
	UAM_ASSERT_EQ(image.native_inline_payload->DecodedSize(), std::size_t{5});
}

UAM_TEST(AntigravityNativeHistoryRefusesMalformedAndUnsupportedToolResultMedia)
{
	for (const auto& extra : {AgBytes(5, AgBytes(1, "%%%%") + AgBytes(2, "image/png")), AgBytes(6, "unsupported media"), AgBytes(99, "unrecognised result")})
	{
		AgFixture fixture;
		fixture.User();
		fixture.Step(1, 15, 20, AgBytes(7, AgCall()), 2);
		fixture.Step(2, 38, 47, AgBytes(3, "Valid text cannot authorize an incomplete prefix") + extra, 2, AgCall());
		const auto result = fixture.Read();
		UAM_ASSERT(result.chats.empty());
		UAM_ASSERT(!result.errors.empty());
	}
	for (const auto field : {4U, 6U, 7U, 9U, 99U})
	{
		AgFixture fixture;
		fixture.User();
		fixture.Step(1, 15, 20, AgBytes(7, AgCall()), 2);
		fixture.Step(2, 132, 140, AgBytes(2, AgBytes(1, "Generic result text") + AgBytes(field, "unsupported original content")), 2, AgCall());
		const auto result = fixture.Read();
		UAM_ASSERT(result.chats.empty());
		UAM_ASSERT(!result.errors.empty());
	}
	for (const auto field : {14U, 15U, 99U})
	{
		AgFixture fixture;
		fixture.User();
		fixture.Step(1, 15, 20, AgBytes(7, AgCall()), 2);
		fixture.Step(2, 8, 14, AgBytes(4, "Visible text cannot replace a binary original") + AgBytes(field, "unhandled original"), 2, AgCall());
		UAM_ASSERT(fixture.Read().chats.empty());
	}
}

UAM_TEST(AntigravityNativeHistoryEmptyToolTextNeverSubstitutesRawNativeEvidence)
{
	for (const int type : {38, 132, 21})
		for (const bool explicit_empty : {false, true})
		{
			AgFixture fixture;
			fixture.User();
			fixture.Step(1, 15, 20, AgBytes(7, AgCall()), 2);
			const auto empty = explicit_empty ? AgBytes(type == 38 ? 3 : 1, "") : std::string{};
			const auto image = AgBytes(5, AgBytes(1, "aGVsbG8=") + AgBytes(2, "image/png"));
			const auto body = type == 38 ? empty + image : AgBytes(type == 132 ? 2 : 21, empty);
			fixture.Step(2, type, type == 38 ? 47 : type == 132 ? 140 : 28, body, 2, AgCall());
			const auto result = fixture.Read();
			UAM_ASSERT(result.errors.empty());
			UAM_ASSERT_EQ(result.chats.size(), std::size_t{1});
			const auto& response = result.chats[0].messages[1];
			UAM_ASSERT(response.tool_calls[0].result_text.empty());
			UAM_ASSERT_EQ(response.tool_calls[0].status, std::string("completed"));
			UAM_ASSERT(std::ranges::any_of(response.blocks, [](const auto& block) { return block.type == "native_tool_result" && block.text.find("payloadHex") != std::string::npos; }));
			if (type == 38)
			{
				UAM_ASSERT_EQ(response.attachments.size(), std::size_t{1});
				UAM_ASSERT_EQ(response.attachments[0].native_tool_call_id, response.tool_calls[0].id);
				UAM_ASSERT_EQ(response.attachments[0].native_inline_payload->DecodedSize(), std::size_t{5});
			}
		}
}

UAM_TEST(AntigravityNativeHistoryRequiresRedundantRenderPresenceAndMatchingValidatedBytes)
{
	AgFixture fixture;
	fixture.User();
	for (const auto& render : {std::string("\x80"), std::string{}})
	{
		fixture.Render(0, render);
		UAM_ASSERT(fixture.Read().chats.empty());
	}
	fixture.Render(0, std::nullopt);
	fixture.Step(1, 15, 20, AgBytes(7, AgCall()), 2);
	const auto step = [&](const std::optional<std::string>& nested, const std::optional<std::string>& column)
	{
		fixture.Sql("DELETE FROM steps WHERE idx=2");
		fixture.Step(2, 132, 140, AgBytes(2, AgBytes(1, "Generic output") + (nested ? AgBytes(3, *nested) : std::string{})), 2, AgCall());
		fixture.Render(2, column);
		return fixture.Read();
	};
	const auto render = AgBytes(1, "Original render title") + AgBytes(2, "Original markdown");
	UAM_ASSERT(step(render, std::nullopt).chats.empty());
	UAM_ASSERT(step(std::nullopt, render).chats.empty());
	UAM_ASSERT(step(render, AgBytes(1, "Different title")).chats.empty());
	UAM_ASSERT(step(std::string("\x80"), std::string("\x80")).chats.empty());
	UAM_ASSERT(step(AgBytes(99, "unknown"), AgBytes(99, "unknown")).chats.empty());
	UAM_ASSERT(step(AgBytes(4, "wrong wire"), AgBytes(4, "wrong wire")).chats.empty());
	UAM_ASSERT(step(AgNumber(4, 2), AgNumber(4, 2)).chats.empty());
	UAM_ASSERT(step(AgBytes(1, "duplicate") + AgBytes(1, "duplicate"), AgBytes(1, "duplicate") + AgBytes(1, "duplicate")).chats.empty());
	UAM_ASSERT(step(AgBytes(10, "unsupported content block"), AgBytes(10, "unsupported content block")).chats.empty());
	const auto empty = step(std::string{}, std::string{});
	UAM_ASSERT(empty.errors.empty());
	UAM_ASSERT_EQ(empty.chats.size(), std::size_t{1});
	const auto valid = step(render, render);
	UAM_ASSERT(valid.errors.empty());
	UAM_ASSERT_EQ(valid.chats.size(), std::size_t{1});
}

UAM_TEST(AntigravityNativeHistoryMetadataOnlyRowsKeepEvidenceAndExactToolCorrelation)
{
	AgFixture fixture;
	AgMetadataOnlyRows(fixture);
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats.size(), std::size_t{1});
	const auto& messages = result.chats[0].messages;
	UAM_ASSERT_EQ(messages.size(), std::size_t{4});
	UAM_ASSERT_EQ(messages[0].content, std::string("Original request"));
	UAM_ASSERT_EQ(messages[1].tool_calls.size(), std::size_t{1});
	UAM_ASSERT_EQ(messages[1].tool_calls[0].result_text, std::string("Exact call output"));
	UAM_ASSERT_EQ(messages[1].tool_calls[0].status, std::string("completed"));
	UAM_ASSERT_EQ(messages[2].thoughts, std::string("Reasoning alone stays visible"));
	UAM_ASSERT(messages[3].interrupted);
	std::size_t metadata = 0;
	for (const auto& message : messages)
		for (const auto& block : message.blocks) metadata += block.type == "native_step_metadata";
	UAM_ASSERT_EQ(metadata, std::size_t{9});
}

UAM_TEST(AntigravityNativeHistoryLegacyDerivesExactRenderPresenceFromAuthenticatedPayload)
{
	for (const bool empty : {false, true})
	{
		AgFixture fixture;
#if defined(_WIN32)
		if (empty) fixture.LegacyBytes(legacy_golden::empty_render_windows);
		else fixture.LegacyBytes(legacy_golden::render_windows);
#else
		if (empty) fixture.LegacyBytes(legacy_golden::empty_render_posix);
		else fixture.LegacyBytes(legacy_golden::render_posix);
#endif
		const auto result = fixture.Read();
		UAM_ASSERT(result.errors.empty());
		UAM_ASSERT_EQ(result.chats.size(), std::size_t{1});
		UAM_ASSERT_EQ(result.chats[0].messages.size(), std::size_t{2});
		UAM_ASSERT_EQ(result.chats[0].messages[1].tool_calls[0].id, std::string("legacy-render-call"));
		UAM_ASSERT_EQ(result.chats[0].messages[1].tool_calls[0].result_text, std::string("Legacy generic output"));
	}
}

UAM_TEST(AntigravityNativeHistoryIncludesCommittedWalWithoutCreatingNativeSharedMemory)
{
	AgFixture fixture;
	fixture.Sql("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0");
	fixture.User("WAL-only native tail");
	const fs::path wal = fixture.file.string() + "-wal", shm = fixture.file.string() + "-shm";
	const auto before = ReadFile(wal), shared = ReadFile(shm);
	const auto revision = uam::memory::StorageRevision(wal);
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats.size(), std::size_t(1));
	UAM_ASSERT_EQ(result.chats[0].messages[0].content, std::string("WAL-only native tail"));
	UAM_ASSERT_EQ(ReadFile(wal), before);
	UAM_ASSERT_EQ(ReadFile(shm), shared);
	UAM_ASSERT_EQ(uam::memory::StorageRevision(wal), revision);
}

UAM_TEST(AntigravityNativeHistoryRejectsForeignIdentityAndAmbiguousWorkspace)
{
	AgFixture fixture;
	fixture.User();
	fixture.Metadata(fixture.workspace, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
	UAM_ASSERT(fixture.Read().chats.empty());
	UAM_ASSERT(!fixture.Read().errors.empty());
	fixture.Metadata(fixture.workspace, kSession, "file:///different/workspace");
	UAM_ASSERT(fixture.Read().chats.empty());
	fixture.Metadata("file://remote-server/private");
	UAM_ASSERT(fixture.Read().chats.empty());
	fixture.Metadata("file:////remote-server/private");
	UAM_ASSERT(fixture.Read().chats.empty());
	fixture.Metadata("file:///bad%00workspace");
	UAM_ASSERT(fixture.Read().chats.empty());
#if !defined(_WIN32)
	fixture.Metadata("file:///C:/foreign/workspace");
	UAM_ASSERT(fixture.Read().chats.empty());
#endif
	fixture.Metadata(fixture.workspace);
	fixture.Sql("UPDATE trajectory_meta SET source=1");
	UAM_ASSERT(fixture.Read().chats.empty());
}

UAM_TEST(AntigravityNativeHistoryRefusesEntirePartialOrUnknownConversationAndRemainsRetryable)
{
	AgFixture fixture;
	fixture.User();
	fixture.Step(1, 15, 20, AgBytes(1, "in progress"), 2, {}, 1);
	const auto partial = fixture.Read();
	UAM_ASSERT(partial.chats.empty());
	UAM_ASSERT(!partial.errors.empty());
	fixture.Sql("DELETE FROM steps WHERE idx=1");
	fixture.Step(2, 15, 20, AgBytes(1, "gap"), 2);
	UAM_ASSERT(fixture.Read().chats.empty());
	fixture.Sql("DELETE FROM steps WHERE idx=2");
	fixture.Step(1, 999, 200, AgBytes(1, "unknown"), 2);
	UAM_ASSERT(fixture.Read().chats.empty());
	fixture.Sql("DELETE FROM steps WHERE idx=1");
	UAM_ASSERT_EQ(fixture.Read().chats.size(), std::size_t(1));
	fixture.Sql("UPDATE steps SET step_payload=CAST(step_payload||X'80' AS BLOB)");
	const auto corrupt = fixture.Read();
	UAM_ASSERT(corrupt.chats.empty());
	UAM_ASSERT(!corrupt.errors.empty());
	UAM_ASSERT(corrupt.errors[0].find("truncated") != std::string::npos);
	fixture.Sql("PRAGMA user_version=2");
	UAM_ASSERT(fixture.Read().chats.empty());
}

UAM_TEST(AntigravityNativeHistoryPreservesInlineOriginalAndRejectsMalformedMedia)
{
	AgFixture fixture;
	fixture.User("Image context", AgBytes(5, AgBytes(1, "aGVsbG8=") + AgBytes(2, "image/png")));
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats.size(), std::size_t(1));
	const auto& attachment = result.chats[0].messages[0].attachments[0];
	UAM_ASSERT_EQ(attachment.source, AttachmentSource::NativeInlinePending);
	UAM_ASSERT(attachment.native_inline_payload);
	UAM_ASSERT_EQ(attachment.size_bytes, std::uintmax_t(5));
	UAM_ASSERT(!attachment.content_digest.empty());
	UAM_ASSERT(attachment.path.empty());
	fixture.Sql("DELETE FROM steps");
	fixture.User("bad image", AgBytes(5, AgBytes(1, "%%%%") + AgBytes(2, "image/png")));
	UAM_ASSERT(fixture.Read().chats.empty());
	UAM_ASSERT(!fixture.Read().errors.empty());
	fixture.Sql("DELETE FROM steps");
	fixture.User("missing MIME", AgBytes(5, AgBytes(1, "aGVsbG8=")));
	UAM_ASSERT(fixture.Read().chats.empty());
}

UAM_TEST(AntigravityNativeHistoryRejectsOrphanDuplicateAndUnfinishedToolCorrelations)
{
	AgFixture fixture;
	fixture.User();
	fixture.Step(1, 21, 28, AgBytes(4, "orphan result"), 2, AgCall());
	UAM_ASSERT(fixture.Read().chats.empty());
	fixture.Sql("DELETE FROM steps WHERE idx=1");
	fixture.Step(1, 15, 20, AgBytes(7, AgCall()), 2);
	UAM_ASSERT(fixture.Read().chats.empty());
	fixture.Step(2, 21, 28, AgBytes(4, "actual native failure") + AgNumber(6, 2), 2, AgCall());
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats[0].messages[1].tool_calls[0].status, std::string("error"));
	fixture.Step(3, 21, 28, AgBytes(4, "duplicate result"), 2, AgCall());
	UAM_ASSERT(fixture.Read().chats.empty());
	fixture.Sql("DELETE FROM steps WHERE idx>0");
	fixture.Step(1, 15, 20, AgBytes(7, AgCall()), 2, {}, 6);
	const auto cancelled = fixture.Read();
	UAM_ASSERT(cancelled.errors.empty());
	UAM_ASSERT(cancelled.chats[0].messages[1].interrupted);
	UAM_ASSERT_EQ(cancelled.chats[0].messages[1].tool_calls[0].status, std::string("cancelled"));
	UAM_ASSERT(cancelled.chats[0].messages[1].tool_calls[0].result_text.empty());
	fixture.Sql("DELETE FROM steps WHERE idx>0");
	const auto no_args = AgBytes(1, "native-no-args") + AgBytes(2, "fixture_tool");
	fixture.Step(1, 15, 20, AgBytes(7, no_args), 2);
	fixture.Step(2, 132, 140, AgBytes(2, AgBytes(1, "Actual no-argument result")), 2, no_args);
	const auto empty_args = fixture.Read();
	UAM_ASSERT(empty_args.errors.empty());
	UAM_ASSERT(empty_args.chats[0].messages[1].tool_calls[0].args_json.empty());
}

UAM_TEST(AntigravityNativeHistoryDoesNotInventHumanContentFromSystemMarkersOrQueryText)
{
	AgFixture fixture;
	fixture.Step(0, 23, 30, AgBytes(4, "setup-only checkpoint"), 5);
	UAM_ASSERT(fixture.Read().chats.empty());
	fixture.Sql("DELETE FROM steps");
	fixture.Step(0, 14, 19, AgBytes(2, "machine context"), 5);
	UAM_ASSERT(fixture.Read().chats.empty());
	fixture.Sql("DELETE FROM steps");
	fixture.User("original human", AgBytes(1, "Original wrapper context"));
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats[0].messages[0].content, std::string("original human"));
	fixture.Sql("DELETE FROM steps");
	fixture.User("one representation", AgBytes(3, AgBytes(1, "conflicting item")));
	UAM_ASSERT(fixture.Read().chats.empty());
}

UAM_TEST(AntigravityNativeHistoryRetainsNativeEditAndContextRecordsWithoutInventingSuccess)
{
	AgFixture fixture;
	fixture.User();
	const std::string call = AgBytes(1, "native-edit") + AgBytes(2, "replace_file_content") + AgBytes(3, "{\"TargetFile\":\"/synthetic/file\"}");
	fixture.Step(1, 15, 20, AgBytes(7, call), 2);
	const auto body = AgBytes(1, AgBytes(1, AgBytes(4, AgBytes(5, "file:///synthetic/file")) + AgBytes(7, "Actual replacement text")));
	fixture.Step(2, 5, 10, body, 2, call);
	fixture.Step(3, 81, 93, AgNumber(1, 1), 5);
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats.size(), std::size_t(1));
	const auto& messages = result.chats[0].messages;
	UAM_ASSERT_EQ(messages.size(), std::size_t(2));
	UAM_ASSERT_EQ(messages[1].role, MessageRole::Assistant);
	UAM_ASSERT(messages[1].tool_calls[0].result_text.find("Actual replacement text") != std::string::npos);
	UAM_ASSERT(messages[1].tool_calls[0].result_text.find("created") == std::string::npos);
	UAM_ASSERT(std::ranges::any_of(messages[1].blocks, [](const auto& block) { return block.type == "native_context_record" && block.text.find("payloadHex") != std::string::npos; }));
}

UAM_TEST(AntigravityNativeHistoryCancellationExactSessionAndSymlinkScope)
{
	AgFixture fixture;
	fixture.User();
	std::stop_source stop;
	stop.request_stop();
	const auto cancelled = uam::antigravity::history::Discover(fixture.temp.root, {}, stop.get_token());
	UAM_ASSERT(cancelled.chats.empty());
	UAM_ASSERT(!cancelled.errors.empty());
	UAM_ASSERT(uam::antigravity::history::Discover(fixture.temp.root, "../outside").chats.empty());
	UAM_ASSERT_EQ(uam::antigravity::history::Discover(fixture.temp.root, kSession).chats.size(), std::size_t(1));
#if !defined(_WIN32)
	fs::rename(fixture.file, fixture.temp.root / "original.db");
	fs::create_symlink(fixture.temp.root / "original.db", fixture.file);
	const auto linked = fixture.Read();
	UAM_ASSERT(linked.chats.empty());
	UAM_ASSERT(!linked.errors.empty());
#endif

}

UAM_TEST(AntigravityNativeHistoryRefusesUnattestedParentContextAndHotRollbackJournal)
{
	AgFixture fixture;
	fixture.User();
	fixture.Sql("INSERT INTO parent_references VALUES(0,X'0801')");
	UAM_ASSERT(fixture.Read().chats.empty());
	UAM_ASSERT(!fixture.Read().errors.empty());
	fixture.Sql("DELETE FROM parent_references");
	UAM_ASSERT_EQ(fixture.Read().chats.size(), std::size_t(1));
	const fs::path journal = fixture.file.string() + "-journal";
	{
		std::ofstream pending(journal);
		pending << "synthetic pending rollback";
	}
	UAM_ASSERT(fixture.Read().chats.empty());
	UAM_ASSERT(!fixture.Read().errors.empty());
}

UAM_TEST(AntigravityNativeHistoryRefusesInheritedMetadataWithoutRelyingOnSummaryIndex)
{
	AgFixture fixture;
	fixture.User();
	UAM_ASSERT(!fs::exists(fixture.temp.root / "conversation_summaries.db"));
	for (const std::string extra : {AgBytes(5, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"), AgNumber(17, 1), AgNumber(16, 1), AgBytes(22, AgBytes(2, "native-battle-parent")), AgNumber(5, 1)})
	{
		fixture.Metadata(fixture.workspace, kSession, {}, extra);
		const auto refused = fixture.Read();
		UAM_ASSERT(refused.chats.empty());
		UAM_ASSERT(!refused.errors.empty());
		fixture.Metadata(fixture.workspace);
		UAM_ASSERT_EQ(fixture.Read().chats.size(), std::size_t(1));
	}
}

UAM_TEST(AntigravityNativeHistoryPreservesSourceQualifiedPlannerIdentityAndOpaqueContextWithoutInputAuthority)
{
	AgFixture fixture;
	fixture.User();
	const auto body = AgBytes(1, "Source-qualified response") + AgBytes(3, "Native reasoning") + AgBytes(15, "native-provider-response-original") + AgBytes(11, AgNumber(1, 1)) + AgBytes(17, AgBytes(1, "type.googleapis.com/synthetic.context") + AgBytes(2, "inert context bytes"));
	fixture.Step(1, 15, 20, body, 2);
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats.size(), std::size_t(1));
	UAM_ASSERT_EQ(result.chats[0].messages[1].content, std::string("Source-qualified response"));
	UAM_ASSERT_EQ(result.chats[0].messages[1].thoughts, std::string("Native reasoning"));
	UAM_ASSERT(!result.chats[0].messages[1].native_input_id && !result.chats[0].messages[1].input_write_completed);
	const auto& blocks = result.chats[0].messages[1].blocks;
	const auto metadata = std::ranges::find_if(blocks, [](const auto& block) { return block.type == "native_response_metadata"; });
	UAM_ASSERT(metadata != blocks.end());
	const auto observed = nlohmann::json::parse(metadata->text);
	UAM_ASSERT_EQ(observed["nativeProviderMessageId"].get<std::string>(), std::string("native-provider-response-original"));
	UAM_ASSERT(observed["nativePlannerPayloadHex"].is_string());
	UAM_ASSERT(!observed.contains("effectiveModel") && !observed.contains("approvalPolicy"));
}

UAM_TEST(AntigravityNativeHistoryReadsAuthenticatedLegacyAndRejectsCorruptionAndSQLiteSupersession)
{
	AgFixture fixture;
	fixture.Legacy();
	const auto bytes = ReadFile(fixture.file);
	const auto revision = uam::memory::StorageRevision(fixture.file);
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats.size(), std::size_t(1));
	const auto& chat = result.chats[0];
	UAM_ASSERT_EQ(chat.native_session_id, std::string(kSession));
	UAM_ASSERT_EQ(chat.workspace_directory, uam::paths::Utf8PathString(fs::path(AgWorkspace())));
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t(2));
	UAM_ASSERT_EQ(chat.messages[0].content, std::string("Legacy synthetic request"));
	UAM_ASSERT_EQ(chat.messages[1].content, std::string("Legacy synthetic response"));
	UAM_ASSERT_EQ(chat.messages[1].thoughts, std::string("Legacy native reasoning"));
	UAM_ASSERT(!chat.messages[0].native_input_id && !chat.messages[0].input_write_completed);
	UAM_ASSERT_EQ(ReadFile(fixture.file), bytes);
	UAM_ASSERT_EQ(uam::memory::StorageRevision(fixture.file), revision);
	UAM_ASSERT_EQ(uam::antigravity::history::Discover(fixture.temp.root, kSession).chats.size(), std::size_t(1));
	const auto write = [&](std::string_view data)
	{
		std::ofstream out(fixture.file, std::ios::binary | std::ios::trunc);
		out.write(data.data(), static_cast<std::streamsize>(data.size()));
		UAM_ASSERT(out.good());
	};
	auto corrupt = bytes;
	corrupt.back() ^= 1;
	write(corrupt);
	UAM_ASSERT(fixture.Read().chats.empty());
	UAM_ASSERT(!fixture.Read().errors.empty());
	write(std::string_view(bytes).substr(0, 20));
	UAM_ASSERT(fixture.Read().chats.empty());
	write(bytes);
	fs::path newer = fixture.file;
	newer.replace_extension(".db");
	const auto superseded = uam::antigravity::history::Discover(fixture.temp.root, kSession, {}, [&]
	{
		std::ofstream database(newer);
		database << "synthetic newly published SQLite";
	});
	UAM_ASSERT(superseded.chats.empty());
	UAM_ASSERT(!superseded.errors.empty());
	const auto preferred = fixture.Read();
	UAM_ASSERT(preferred.chats.empty()); // Never fall back from newer SQLite to old protobuf.
	UAM_ASSERT(!preferred.errors.empty());
}

UAM_TEST(AntigravityNativeHistoryChangedSnapshotRefusesPublicationThenFreshRetryReadsCompleteTail)
{
	AgFixture fixture;
	fixture.User();
	const auto stale = uam::antigravity::history::Discover(fixture.temp.root, kSession, {}, [&] { fixture.Step(1, 15, 20, AgBytes(1, "New committed native tail"), 2); });
	UAM_ASSERT(stale.chats.empty());
	UAM_ASSERT(!stale.errors.empty());
	UAM_ASSERT(stale.errors[0].find("changed") != std::string::npos);
	const auto fresh = fixture.Read();
	UAM_ASSERT(fresh.errors.empty());
	UAM_ASSERT_EQ(fresh.chats.size(), std::size_t(1));
	UAM_ASSERT_EQ(fresh.chats[0].messages.size(), std::size_t(2));
	UAM_ASSERT_EQ(fresh.chats[0].messages.back().content, std::string("New committed native tail"));
}

UAM_TEST(AntigravityNativeHistorySnapshotDirectoriesAreFreshAndOwnerPrivate)
{
	const auto first = uam::platform::CreatePrivateHistoryTempDirectory(), second = uam::platform::CreatePrivateHistoryTempDirectory();
	UAM_ASSERT(first && second);
	UAM_ASSERT(*first != *second);
	struct Cleanup
	{
		fs::path first, second;
		~Cleanup()
		{
			std::error_code ignored;
			fs::remove_all(first, ignored);
			fs::remove_all(second, ignored);
		}
	} cleanup{*first, *second};
#if !defined(_WIN32)
	const auto mode = fs::status(*first).permissions();
	UAM_ASSERT((mode & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
	UAM_ASSERT((mode & fs::perms::owner_all) == fs::perms::owner_all);
#endif

}

UAM_TEST(AntigravityNativeHistoryUsesExactSummaryWorkspaceAndCompleteStepCountWithoutTitleTimeMatching)
{
	AgFixture fixture;
	fixture.Metadata("");
	fixture.Summary(2, fixture.workspace);
	fixture.Step(0, 14, 19, AgBytes(2, "Native 1.1.16 text input"), 0, {}, 3, false);
	fixture.Step(1, 15, 20, AgBytes(1, "Native 1.1.16 response"), 0, {}, 3, false);
	const auto path = fixture.temp.root / "conversation_summaries.db";
	const auto before = ReadFile(path);
	const auto revision = uam::memory::StorageRevision(path);
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats.size(), std::size_t(1));
	UAM_ASSERT_EQ(result.chats[0].workspace_directory, uam::paths::Utf8PathString(fs::path(AgWorkspace())));
	UAM_ASSERT_EQ(result.chats[0].messages.size(), std::size_t(2));
	UAM_ASSERT_EQ(result.chats[0].messages[0].role, MessageRole::User);
	UAM_ASSERT(!result.chats[0].messages[0].native_input_id);
	UAM_ASSERT(!result.chats[0].messages[0].input_write_completed);
	UAM_ASSERT_EQ(ReadFile(path), before);
	UAM_ASSERT_EQ(uam::memory::StorageRevision(path), revision);
	UAM_ASSERT(!fs::exists(path.string() + "-shm"));
	fixture.Summary(3, fixture.workspace);
	UAM_ASSERT(fixture.Read().chats.empty());
	fixture.Metadata(fixture.workspace);
	fixture.Summary(2, "file:///different/original/workspace");
	UAM_ASSERT(fixture.Read().chats.empty());
	fixture.Summary(2, fixture.workspace);
	fixture.Sql("UPDATE trajectory_meta SET cascade_id='aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee'");
	UAM_ASSERT(fixture.Read().chats.empty());
}

UAM_TEST(AntigravityNativeHistoryRefusesWideOrCoercedSQLiteIdentityAndStepIntegers)
{
	for (const char* mutation : {"UPDATE trajectory_meta SET trajectory_type=4294967300", "UPDATE trajectory_meta SET source=4294967313", "UPDATE trajectory_meta SET source='17invalid'", "UPDATE steps SET step_type=4294967310", "UPDATE steps SET status=4294967299", "UPDATE steps SET has_subtrajectory=4294967296", "UPDATE steps SET step_format=4294967296"})
	{
		AgFixture fixture;
		fixture.User();
		fixture.Sql(mutation);
		const auto result = fixture.Read();
		UAM_ASSERT(result.chats.empty());
		UAM_ASSERT(!result.errors.empty());
	}
}

UAM_TEST(AntigravityNativeHistoryPreservesCancelledResultStatusAndOutput)
{
	AgFixture fixture;
	fixture.User();
	fixture.Step(1, 15, 20, AgBytes(7, AgCall()), 2);
	fixture.Step(2, 21, 28, AgBytes(4, "Native partial output before cancellation"), 2, AgCall(), 6);
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats.size(), std::size_t{1});
	const auto& call = result.chats[0].messages[1].tool_calls[0];
	UAM_ASSERT_EQ(call.status, std::string("cancelled"));
	UAM_ASSERT_EQ(call.result_text, std::string("Native partial output before cancellation"));
}

UAM_TEST(AntigravityNativeHistoryRefusesEqualConversationAndTrajectoryIdentityInBothFormats)
{
	AgFixture fixture(kSession, kSession);
	fixture.User();
	UAM_ASSERT(fixture.Read().chats.empty());
	UAM_ASSERT(!fixture.Read().errors.empty());
	fixture.Legacy(true);
	UAM_ASSERT(fixture.Read().chats.empty());
	UAM_ASSERT(!fixture.Read().errors.empty());
}

UAM_TEST(AntigravityNativeHistorySnapshotsSummaryOnceAcrossFiftyConversationsAndPrefiltersWorkspace)
{
	TempDir store("uam-antigravity-fifty-index");
	fs::create_directories(store.root / "conversations");
	for (unsigned i = 0; i < 50; ++i)
	{
		std::ostringstream id;
		id << "3dc412af-b91a-4d9d-82f0-" << std::setfill('0') << std::setw(12) << i;
		AgFixture fixture(id.str());
		fixture.User("Synthetic indexed conversation");
		fixture.Summary(1, fixture.workspace);
		fixture.db.reset();
		fs::copy_file(fixture.file, store.root / "conversations" / fixture.file.filename());
		if (i == 0) fs::copy_file(fixture.temp.root / "conversation_summaries.db", store.root / "conversation_summaries.db");
		else
		{
			sqlite3* raw = nullptr;
			UAM_ASSERT_EQ(sqlite3_open((store.root / "conversation_summaries.db").string().c_str(), &raw), SQLITE_OK);
			const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> db(raw, sqlite3_close_v2);
			const std::string query = "INSERT INTO conversation_summaries SELECT '" + id.str() + "',workspace_uris,step_count,source,parent_conversation_id,nesting_depth,not_fully_idle,battle_id FROM conversation_summaries LIMIT 1";
			UAM_ASSERT_EQ(sqlite3_exec(raw, query.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
		}
	}
	unsigned summaries = 0, conversations = 0;
	const auto result = uam::antigravity::history::Discover(store.root, {}, {}, [&] { ++conversations; }, {}, [&] { ++summaries; });
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(summaries, 1U);
	UAM_ASSERT_EQ(conversations, 50U);
	UAM_ASSERT_EQ(result.chats.size(), std::size_t{50});
	UAM_ASSERT_EQ(result.chats.back().native_session_id, std::string("3dc412af-b91a-4d9d-82f0-000000000049"));
	conversations = summaries = 0;
	const auto filtered = uam::antigravity::history::Discover(store.root, {}, {}, [&] { ++conversations; }, [](const std::string&) { return false; }, [&] { ++summaries; });
	UAM_ASSERT(filtered.errors.empty());
	UAM_ASSERT(filtered.chats.empty());
	UAM_ASSERT_EQ(summaries, 1U);
	UAM_ASSERT_EQ(conversations, 0U);
	// Index changes after the snapshot invalidate every staged result.
	const auto raced = uam::antigravity::history::Discover(store.root, {}, {}, {}, {}, [&] { std::ofstream(store.root / "conversation_summaries.db", std::ios::app).put('x'); });
	UAM_ASSERT(raced.chats.empty());
	UAM_ASSERT(!raced.errors.empty());
	fs::remove(store.root / "conversation_summaries.db");
	for (const auto& file : fs::directory_iterator(store.root / "conversations")) std::ofstream(file.path(), std::ios::trunc) << "refused synthetic database";
	const auto refused = uam::antigravity::history::Discover(store.root);
	UAM_ASSERT_EQ(refused.errors.size(), std::size_t{20});
	UAM_ASSERT_EQ(refused.omitted_error_count, std::size_t{30});
}

#if !defined(UAM_ANTIGRAVITY_HISTORY_STANDALONE) && UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
namespace
{
	void AgWait(uam::StartupNativeImportService& service, uam::AppState& app)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (service.Snapshot().phase == "scanning" || service.ImportPending())
		{
			UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			service.Poll(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	ChatHistorySyncService::LocalHistoryDiscovery AgApplicationDiscovery(const AgFixture& fixture)
	{
		return ChatHistorySyncService().DiscoverProviderChatsForFolder({}, nullptr, {}, 0, false, {}, {}, "antigravity-cli", kSession, fixture.temp.root);
	}
} // namespace

UAM_TEST(SettingsAntigravityNativeDiscoveryAssignmentAndSelectedOriginalPublication)
{
	AgFixture fixture;
	fixture.User("Preserve native image", AgBytes(5, AgBytes(1, "aGVsbG8=") + AgBytes(2, "image/png")));
	fixture.Step(1, 15, 20, AgBytes(7, AgCall()), 2);
	fixture.Step(2, 38, 47, AgBytes(3, "MCP tool image") + AgBytes(5, AgBytes(1, "dG9vbCBvcmlnaW5hbA==") + AgBytes(2, "image/png")), 2, AgCall());
	auto discovery = AgApplicationDiscovery(fixture);
	UAM_ASSERT_EQ(discovery.chats.size(), std::size_t(1));
	uam::AppState app;
	app.data_root = fixture.temp.root / "uam-data";
	uam::StartupNativeImportService service([&](const auto&, std::stop_token) { return AgApplicationDiscovery(fixture); });
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	AgWait(service, app);
	const auto ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.total_count, std::size_t(1));
	UAM_ASSERT(ready.candidates[0].folder_id.empty());
	UAM_ASSERT(!fs::exists(app.data_root));
	UAM_ASSERT(service.AssignWorkspace(app, ready.generation, ready.candidates[0].candidate_id, "Native workspace"));
	UAM_ASSERT(app.folders.empty());
	UAM_ASSERT(!fs::exists(app.data_root));
	UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates[0].candidate_id}, {}));
	AgWait(service, app);
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 1);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t(1));
	UAM_ASSERT_EQ(app.folders.size(), std::size_t(1));
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, app.chats[0].id, true);
	UAM_ASSERT(saved);
	UAM_ASSERT_EQ(saved->workspace_directory, uam::paths::Utf8PathString(fs::path(AgWorkspace())));
	UAM_ASSERT_EQ(saved->native_session_id, std::string(kSession));
	UAM_ASSERT_EQ(saved->messages[0].attachments[0].source, AttachmentSource::OwnedNativeAsset);
	const auto original = uam::native_assets::ReadOwned(app.data_root, saved->id, saved->messages[0].attachments[0].native_asset_key);
	UAM_ASSERT(original.ok);
	UAM_ASSERT_EQ(original.bytes, std::string("hello"));
	UAM_ASSERT_EQ(saved->messages[1].attachments.size(), std::size_t{1});
	const auto& tool_image = saved->messages[1].attachments[0];
	UAM_ASSERT_EQ(tool_image.native_tool_call_id, saved->messages[1].tool_calls[0].id);
	UAM_ASSERT_EQ(tool_image.source, AttachmentSource::OwnedNativeAsset);
	const auto tool_original = uam::native_assets::ReadOwned(app.data_root, saved->id, tool_image.native_asset_key);
	UAM_ASSERT(tool_original.ok);
	UAM_ASSERT_EQ(tool_original.bytes, std::string("tool original"));
	UAM_ASSERT(!saved->messages[0].input_write_completed);
	const auto frontend = uam::StateSerializer::SerializeMessagePage(*saved, 100, {});
	UAM_ASSERT(frontend.dump().find("native_step_metadata") == std::string::npos);
	UAM_ASSERT(std::ranges::any_of(saved->messages[0].blocks, [](const auto& block) { return block.type == "native_step_metadata"; }));
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	AgWait(service, app);
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t(0));
}

UAM_TEST(SettingsAntigravityNativeManyRefusalsRetainOmissionCountAcrossStaging)
{
	AgFixture fixture;
	fixture.db.reset();
	fs::remove(fixture.file);
	for (unsigned i = 0; i < 50; ++i)
	{
		std::ostringstream id;
		id << "3dc412af-b91a-4d9d-82f0-" << std::setfill('0') << std::setw(12) << i;
		std::ofstream(fixture.file.parent_path() / (id.str() + ".db")) << "Synthetic refused database";
	}
	uam::AppState app;
	app.data_root = fixture.temp.root / "uam-data";
	bool include_other_errors = true;
	uam::StartupNativeImportService service([&](const auto&, std::stop_token)
	{
		ChatHistorySyncService::LocalHistoryDiscovery result;
		if (include_other_errors)
			for (unsigned i = 0; i < 7; ++i) result.result.Fail("Earlier synthetic discovery issue " + std::to_string(i));
		auto native = ChatHistorySyncService().DiscoverProviderChatsForFolder({}, nullptr, {}, 0, false, {}, {}, "antigravity-cli", {}, fixture.temp.root);
		result.result.Merge(native.result);
		return result;
	});
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	AgWait(service, app);
	const auto snapshot = service.Snapshot();
	UAM_ASSERT_EQ(snapshot.total_count, std::size_t{0});
	UAM_ASSERT_EQ(snapshot.errors.size(), std::size_t{21});
	UAM_ASSERT(snapshot.errors.back().starts_with("37 additional native history issues"));
	const auto frontend = uam::StateSerializer::SerializeStartupNativeImports(snapshot);
	UAM_ASSERT_EQ(frontend["errors"], nlohmann::json(snapshot.errors));
	UAM_ASSERT(!fs::exists(app.data_root));
	for (const auto& file : fs::directory_iterator(fixture.file.parent_path())) fs::remove(file.path());
	include_other_errors = false;
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	AgWait(service, app);
	UAM_ASSERT(service.Snapshot().errors.empty());
}

UAM_TEST(SettingsAntigravityNativeImageOnlyMcpKeepsOwnedOriginalOutOfFrontendToolText)
{
	for (const bool explicit_empty : {false, true})
	{
		AgFixture fixture;
		fixture.User();
		fixture.Step(1, 15, 20, AgBytes(7, AgCall()), 2);
		// The large literal exceeds the inline tool-text threshold if the original
		// raw-record fallback leaks; the same real deferred-content API is checked.
		const std::string encoded = explicit_empty ? std::string(48000, 'A') : "aGVsbG8=";
		const std::string original_bytes = explicit_empty ? std::string(36000, '\0') : "hello";
		fixture.Step(2, 38, 47, (explicit_empty ? AgBytes(3, "") : std::string{}) + AgBytes(5, AgBytes(1, encoded) + AgBytes(2, "image/png")), 2, AgCall());
		uam::AppState app;
		app.data_root = fixture.temp.root / "uam-data";
		uam::StartupNativeImportService service([&](const auto&, std::stop_token) { return AgApplicationDiscovery(fixture); });
		UAM_ASSERT(service.DiscoverLocalHistory(app));
		AgWait(service, app);
		const auto ready = service.Snapshot();
		UAM_ASSERT_EQ(ready.total_count, std::size_t{1});
		UAM_ASSERT(service.AssignWorkspace(app, ready.generation, ready.candidates[0].candidate_id, "Original workspace"));
		UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates[0].candidate_id}, {}));
		AgWait(service, app);
		UAM_ASSERT_EQ(service.Snapshot().imported_count, 1);
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, app.chats[0].id, true);
		UAM_ASSERT(saved);
		UAM_ASSERT_EQ(saved->messages.size(), std::size_t{2});
		const auto& response = saved->messages[1];
		const auto& call = response.tool_calls[0];
		UAM_ASSERT(call.result_text.empty());
		UAM_ASSERT_EQ(call.status, std::string("completed"));
		UAM_ASSERT_EQ(response.attachments.size(), std::size_t{1});
		const auto& image = response.attachments[0];
		UAM_ASSERT_EQ(image.native_tool_call_id, call.id);
		UAM_ASSERT_EQ(image.source, AttachmentSource::OwnedNativeAsset);
		const auto original = uam::native_assets::ReadOwned(app.data_root, saved->id, image.native_asset_key);
		UAM_ASSERT(original.ok);
		UAM_ASSERT_EQ(original.bytes, original_bytes);
		UAM_ASSERT(std::ranges::any_of(response.blocks, [](const auto& block) { return block.type == "native_tool_result" && block.text.find("payloadHex") != std::string::npos; }));
		const auto text = uam::StateSerializer::ToolCallContentForFrontend(call);
		UAM_ASSERT_EQ(text, call.args_json);
		const auto tool_page = uam::StateSerializer::ToolCallContentPageForFrontend(call, 0);
		UAM_ASSERT_EQ(tool_page["content"], call.args_json);
		UAM_ASSERT_EQ(tool_page["totalBytes"], call.args_json.size());
		const auto page = uam::StateSerializer::SerializeMessagePage(*saved, 100, {});
		for (const auto& data : {text, tool_page.dump(), page.dump()})
		{
			UAM_ASSERT(data.find("payloadHex") == std::string::npos);
			UAM_ASSERT(data.find(encoded) == std::string::npos);
		}
	}
}

UAM_TEST(SettingsAntigravityNativeMetadataRowsDoNotConsumePersistedMessagePages)
{
	AgFixture fixture;
	AgMetadataOnlyRows(fixture);
	uam::AppState app;
	app.data_root = fixture.temp.root / "uam-data";
	uam::StartupNativeImportService service([&](const auto&, std::stop_token) { return AgApplicationDiscovery(fixture); });
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	AgWait(service, app);
	const auto ready = service.Snapshot();
	UAM_ASSERT(service.AssignWorkspace(app, ready.generation, ready.candidates[0].candidate_id, "Original workspace"));
	UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates[0].candidate_id}, {}));
	AgWait(service, app);
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 1);
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, app.chats[0].id, true);
	UAM_ASSERT(saved);
	UAM_ASSERT_EQ(saved->messages.size(), std::size_t{4});
	const auto page = uam::StateSerializer::SerializeMessagePage(*saved, 2, {});
	UAM_ASSERT_EQ(page["messages"].size(), std::size_t{2});
	UAM_ASSERT_EQ(page["totalCount"], 4);
	UAM_ASSERT_EQ(page["startIndex"], 2);
	UAM_ASSERT_EQ(page["userMessageIndices"], nlohmann::json::array({0}));
	UAM_ASSERT(page.dump().find("Reasoning alone stays visible") != std::string::npos);
	UAM_ASSERT(page.dump().find("native_step_metadata") == std::string::npos);
	UAM_ASSERT(saved->messages.back().interrupted);
}

UAM_TEST(SettingsAntigravityNativeHiddenOwnerAndLateTombstonePreventPublication)
{
	AgFixture fixture;
	fixture.User();
	auto discovery = AgApplicationDiscovery(fixture);
	UAM_ASSERT_EQ(discovery.chats.size(), std::size_t(1));
	uam::AppState app;
	app.data_root = fixture.temp.root / "uam-data";
	uam::StartupNativeImportService service([&](const auto&, std::stop_token) { return AgApplicationDiscovery(fixture); });
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	AgWait(service, app);
	const auto ready = service.Snapshot();
	UAM_ASSERT(service.AssignWorkspace(app, ready.generation, ready.candidates[0].candidate_id, "Native workspace"));
	std::vector<std::string> added;
	UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(app.data_root, discovery.chats, added));
	UAM_ASSERT(!service.Decide(app, ready.generation, {ready.candidates[0].candidate_id}, {}));
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(app.folders.empty());
	uam::AppState other;
	other.data_root = fixture.temp.root / "other-uam-data";
	ChatSession hidden = discovery.chats[0];
	hidden.id = "hidden-uam-owner";
	hidden.agent_run_id = "private-worker";
	UAM_ASSERT(ChatRepository::SaveChat(other.data_root, hidden));
	const auto before = ReadFile(AppPaths::UamChatFilePath(other.data_root, hidden.id));
	ChatHistorySyncService().LoadSidebarChats(other);
	UAM_ASSERT(other.chats.empty());
	uam::StartupNativeImportService hidden_service([&](const auto&, std::stop_token) { return AgApplicationDiscovery(fixture); });
	UAM_ASSERT(hidden_service.DiscoverLocalHistory(other));
	AgWait(hidden_service, other);
	UAM_ASSERT_EQ(hidden_service.Snapshot().total_count, std::size_t(0));
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(other.data_root, hidden.id)), before);
}

UAM_TEST(SettingsAntigravityLegacyDiscoveryAssignmentAndSelectedPublication)
{
	AgFixture fixture;
	fixture.Legacy();
	const auto original = ReadFile(fixture.file);
	uam::AppState app;
	app.data_root = fixture.temp.root / "uam-legacy-data";
	uam::StartupNativeImportService service([&](const auto&, std::stop_token) { return AgApplicationDiscovery(fixture); });
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	AgWait(service, app);
	const auto ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.total_count, std::size_t(1));
	UAM_ASSERT(ready.candidates[0].folder_id.empty());
	UAM_ASSERT(!fs::exists(app.data_root));
	UAM_ASSERT(service.AssignWorkspace(app, ready.generation, ready.candidates[0].candidate_id, "Legacy native workspace"));
	UAM_ASSERT(app.folders.empty() && !fs::exists(app.data_root));
	UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates[0].candidate_id}, {}));
	AgWait(service, app);
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 1);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t(1));
	UAM_ASSERT_EQ(app.folders.size(), std::size_t(1));
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, app.chats[0].id, true);
	UAM_ASSERT(saved);
	UAM_ASSERT_EQ(saved->native_session_id, std::string(kSession));
	UAM_ASSERT_EQ(saved->workspace_directory, uam::paths::Utf8PathString(fs::path(AgWorkspace())));
	UAM_ASSERT_EQ(saved->messages[0].content, std::string("Legacy synthetic request"));
	UAM_ASSERT_EQ(saved->messages[1].thoughts, std::string("Legacy native reasoning"));
	UAM_ASSERT(!saved->messages[0].input_write_completed);
	UAM_ASSERT_EQ(ReadFile(fixture.file), original);
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	AgWait(service, app);
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t(0));
}
#endif

#endif // standalone or enabled Antigravity reader
