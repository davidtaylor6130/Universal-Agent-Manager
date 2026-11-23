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
#include "antigravity_history_fixture.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <sstream>
#if !defined(UAM_ANTIGRAVITY_HISTORY_STANDALONE)
#include "app/startup_native_import_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/message_attachment_sources.h"
#endif

#if defined(UAM_ANTIGRAVITY_HISTORY_STANDALONE) || UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
using namespace uam_test;
using namespace uam_test::antigravity_fixture;
namespace
{
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

UAM_TEST(AntigravityNativeHistoryAcceptsEmptySourceAndStillRefusesForeignSource)
{
	// Real Antigravity CLI stores leave `source` empty; the reader must accept that.
	AgFixture fixture;
	fixture.Metadata("");
	fixture.Summary(2, fixture.workspace, "");
	fixture.Step(0, 14, 19, AgBytes(2, "Native empty-source input"), 0, {}, 3, false);
	fixture.Step(1, 15, 20, AgBytes(1, "Native empty-source response"), 0, {}, 3, false);
	const auto result = fixture.Read();
	UAM_ASSERT(result.errors.empty());
	UAM_ASSERT_EQ(result.chats.size(), std::size_t(1));
	UAM_ASSERT_EQ(result.chats[0].messages.size(), std::size_t(2));
	// A non-empty source that is not ours is still foreign and must be refused.
	fixture.Summary(2, fixture.workspace, "some-other-tool");
	const auto foreign = fixture.Read();
	UAM_ASSERT(foreign.chats.empty());
	UAM_ASSERT(!foreign.errors.empty());
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

UAM_TEST(AntigravityNativeHistoryRecordsPlainRefusalReasonAndUnsettledSource)
{
	AgFixture nested;
	nested.User();
	nested.Sql("INSERT INTO parent_references VALUES(0,X'0801')");
	const auto refused = nested.Read();
	UAM_ASSERT(refused.chats.empty());
	UAM_ASSERT_EQ(refused.refusals.size(), std::size_t(1));
	UAM_ASSERT_EQ(refused.refusals[0].session, std::string(kSession));
	UAM_ASSERT_EQ(refused.refusals[0].reason, std::string("This conversation continues another Antigravity conversation, and UAM cannot read that earlier context completely yet."));
	UAM_ASSERT(!refused.refusals[0].unsettled);
	UAM_ASSERT_EQ(refused.errors.size(), std::size_t(1));
	UAM_ASSERT(refused.errors[0].find(kSession) != std::string::npos);

	AgFixture running;
	running.User();
	running.Step(1, 15, 20, AgBytes(1, "Still streaming"), 2, {}, 2);
	const auto unsettled = running.Read();
	UAM_ASSERT(unsettled.chats.empty());
	UAM_ASSERT_EQ(unsettled.refusals.size(), std::size_t(1));
	UAM_ASSERT(unsettled.refusals[0].unsettled);
	UAM_ASSERT_EQ(unsettled.refusals[0].reason, std::string("Antigravity is still working on this conversation. Retry after it finishes."));

	// INTERRUPTED (12) is not an in-progress state: refused with a reason, never silent.
	AgFixture interrupted;
	interrupted.User();
	interrupted.Step(1, 15, 20, AgBytes(1, "Stopped"), 2, {}, 12);
	const auto unknown = interrupted.Read();
	UAM_ASSERT(unknown.chats.empty());
	UAM_ASSERT_EQ(unknown.refusals.size(), std::size_t(1));
	UAM_ASSERT(!unknown.refusals[0].unsettled);
	UAM_ASSERT_EQ(unknown.refusals[0].reason, std::string("This conversation has an Antigravity step state that UAM cannot read yet."));
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

UAM_TEST(SettingsAntigravityNativeSingleOmissionUsesSingularWording)
{
	AgFixture fixture;
	fixture.db.reset();
	fs::remove(fixture.file);
	uam::AppState app;
	app.data_root = fixture.temp.root / "uam-data";
	uam::StartupNativeImportService service([&](const auto&, std::stop_token)
	{
		ChatHistorySyncService::LocalHistoryDiscovery result;
		result.result.omitted_error_count = 1;
		return result;
	});
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	AgWait(service, app);
	const auto snapshot = service.Snapshot();
	UAM_ASSERT_EQ(snapshot.total_count, std::size_t{0});
	UAM_ASSERT_EQ(snapshot.errors.size(), std::size_t{1});
	UAM_ASSERT_EQ(snapshot.errors.back(), "1 additional native history issue was omitted; review discovery before importing.");
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

namespace
{
	// The exact local transcript read used by Terminal-to-Chat refresh, against the
	// documented CLI store under a synthetic HOME.
	struct AgExactReadFixture
	{
		ScopedNativeHistoryHome home;
		TempDir workspace_root{"uam-antigravity-exact-read"};
		AgFixture native;
		ChatSession chat;
		AgExactReadFixture()
		{
			const fs::path workspace = workspace_root.root / "workspace";
			fs::create_directories(workspace);
			std::string uri;
			for (const char c : workspace.generic_string()) uri += c == ' ' ? std::string("%20") : std::string(1, c);
			native.Metadata(uri.front() == '/' ? "file://" + uri : "file:///" + uri);
			native.User("Recorded text");
			chat.id = "saved-UAM-antigravity";
			chat.provider_id = uam::provider_ids::kAntigravityCli;
			chat.native_session_id = kSession;
			chat.workspace_directory = workspace.string();
			chat.execution_host_id = "local";
		}
		std::optional<ChatSession> Load(std::string& error, bool& unsettled)
		{
			native.db.reset();
			const fs::path store = home.directory.root / ".gemini" / "antigravity-cli" / "conversations";
			fs::create_directories(store);
			fs::copy_file(native.file, store / native.file.filename(), fs::copy_options::overwrite_existing);
			return ChatHistorySyncService().LoadLocalNativeTranscript(chat, &error, {}, &unsettled);
		}
	};
} // namespace

UAM_TEST(AntigravityExactLocalTranscriptReadsSettledConversation)
{
	AgExactReadFixture fixture;
	fixture.native.Step(1, 15, 20, AgBytes(1, "Native terminal answer"), 2);
	std::string error;
	bool unsettled = true;
	const auto loaded = fixture.Load(error, unsettled);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(!unsettled);
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(loaded->messages.size(), std::size_t(2));
	UAM_ASSERT_EQ(loaded->messages[1].content, std::string("Native terminal answer"));
}

UAM_TEST(AntigravityExactLocalTranscriptReportsPlainRefusalReason)
{
	AgExactReadFixture fixture;
	fixture.native.Sql("INSERT INTO parent_references VALUES(0,X'0801')");
	std::string error;
	bool unsettled = true;
	UAM_ASSERT(!fixture.Load(error, unsettled));
	UAM_ASSERT(!unsettled);
	UAM_ASSERT_EQ(error, std::string("This conversation continues another Antigravity conversation, and UAM cannot read that earlier context completely yet."));
}

UAM_TEST(AntigravityExactLocalTranscriptMarksUnfinishedConversation)
{
	AgExactReadFixture fixture;
	fixture.native.Step(1, 15, 20, AgBytes(1, "Still streaming"), 2, {}, 2);
	std::string error;
	bool unsettled = false;
	UAM_ASSERT(!fixture.Load(error, unsettled));
	UAM_ASSERT(unsettled);
	UAM_ASSERT_EQ(error, std::string("Antigravity is still working on this conversation. Retry after it finishes."));
}
#endif

#endif // standalone or enabled Antigravity reader
