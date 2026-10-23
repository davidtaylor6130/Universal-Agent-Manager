#include "test_harness.h"
#include "app/transcript_retention_service.h"
#include "common/runtime/native_refresh_lifetime.h"
#include "common/runtime/terminal/terminal_launch.h"
#include "common/chat/native_attachment_assets.h"
#include "common/chat/native_media_chat_save.h"
#include "common/utils/base64.h"
#include "app/native_agent_history_service.h"
#include "common/provider/gemini_history_journal.h"
#include "common/provider/gemini_history_loader.h"
#include "common/provider/native_provider_agent_history.h"
#include "remote/native_history_snapshot.h"

#include <fstream>
#include <future>

using namespace uam_test;

namespace
{
	using Json = nlohmann::json;
	const std::string kParent = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
	const std::string kChild = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
	std::string Line(const Json& value) { return value.dump() + "\n"; }
	Json Header(const std::string& id = kParent)
	{
		return {{"sessionId", id}, {"projectHash", "project"}, {"startTime", "2026-10-16T12:00:00Z"}, {"lastUpdated", "2026-10-16T12:00:01Z"}};
	}
	std::string Journal()
	{
		return Line(Header()) + Line({{"id", "u"}, {"type", "user"}, {"content", "Native question"}}) +
		    Line({{"id", "a"}, {"type", "gemini"}, {"content", "Old answer"}, {"model", "native-model"},
		        {"thoughts", Json::array({"Native reasoning"})}, {"toolCalls", Json::array({{{"id", "t"}, {"name", "read_file"}, {"status", "success"}, {"result", "old"}}})}}) +
		    Line({{"$patch", {{"id", "a"}, {"content", "Current answer"}, {"toolCalls", Json::array({{{"id", "t"}, {"result", "Current tool result"}}})}}}});
	}
}

UAM_TEST(GeminiJournalFoldsReplacementPatchOrderRemovalAndRewind)
{
	auto text = Journal() + Line({{"id", "temporary"}, {"type", "gemini"}, {"content", "Never restore"}}) +
	    Line({{"$patch", {{"updates", Json::array({{{"id", "u"}, {"content", "Updated question"}}})}, {"orderIds", Json::array({"a", "u", "a"})}}}}) +
	    Line({{"$patch", {{"removeIds", Json::array({"temporary"})}}}});
	auto folded = uam::gemini::FoldHistoryJournal(text);
	UAM_ASSERT(folded);
	UAM_ASSERT_EQ((*folded)["messages"].size(), std::size_t(2));
	UAM_ASSERT_EQ((*folded)["messages"][0]["id"].get<std::string>(), std::string("a"));
	UAM_ASSERT_EQ((*folded)["messages"][0]["toolCalls"][0]["result"].get<std::string>(), std::string("Current tool result"));
	UAM_ASSERT_EQ((*folded)["messages"][1]["content"].get<std::string>(), std::string("Updated question"));
	text += Line({{"$rewindTo", "u"}});
	folded = uam::gemini::FoldHistoryJournal(text);
	UAM_ASSERT(folded && (*folded)["messages"].size() == 1);
	text += Line({{"id", "u"}, {"type", "user"}, {"content", "New branch"}});
	folded = uam::gemini::FoldHistoryJournal(text);
	UAM_ASSERT(folded && (*folded)["messages"].size() == 2 && (*folded)["messages"][1]["content"] == "New branch");
	folded = uam::gemini::FoldHistoryJournal(text + Line({{"$rewindTo", "unknown"}}));
	UAM_ASSERT(folded && (*folded)["messages"].empty());
}

UAM_TEST(GeminiJournalReplacesSnapshotsAndRejectsForeignIdentityOrInteriorCorruption)
{
	const auto replacement = Line({{"$set", {{"messages", Json::array({{{"id", "new"}, {"type", "gemini"}, {"content", "Only current content"}}})}, {"lastUpdated", "recorded"}}}});
	auto folded = uam::gemini::FoldHistoryJournal(Journal() + replacement);
	UAM_ASSERT(folded && (*folded)["messages"].size() == 1 && (*folded)["messages"][0]["id"] == "new");
	UAM_ASSERT(uam::gemini::FoldHistoryJournal(Journal() + "{\"unfinished\":"));
	UAM_ASSERT(!uam::gemini::FoldHistoryJournal(Journal() + "broken\n"));
	UAM_ASSERT(!uam::gemini::FoldHistoryJournal(Journal() + Line({{"$set", {{"sessionId", kChild}}}})));
	UAM_ASSERT(!uam::gemini::FoldHistoryJournal(Journal() + Line({{"$set", {{"projectHash", "foreign"}}}})));
	const auto metadata = uam::gemini::FoldHistoryJournal(Journal() + replacement, false);
	UAM_ASSERT(metadata && !metadata->contains("messages") && (*metadata)["lastUpdated"] == "recorded");
}

UAM_TEST(GeminiJournalStreamingFoldRetainsLatestBeyondSixtyFourMiB)
{
	TempDir temp("uam-gemini-journal-long-log");
	const auto file = temp.root / "session.jsonl";
	{
		std::ofstream output(file, std::ios::binary);
		UAM_ASSERT(output.good());
		output << Line(Header());
		const std::string obsolete = Line({{"id", "answer"}, {"type", "gemini"}, {"content", std::string(2 * 1024 * 1024, 'x')}});
		for (int index = 0; index < 33; ++index) output << obsolete;
		output << Line({{"id", "answer"}, {"type", "gemini"}, {"content", "Current compact record"}});
	}
	UAM_ASSERT(fs::file_size(file) > 64U * 1024U * 1024U);
	UAM_ASSERT(!uam::gemini::FoldHistoryJournalFile(file, 64U * 1024U * 1024U));
	const auto folded = uam::gemini::FoldHistoryJournalFile(file);
	UAM_ASSERT(folded && (*folded)["messages"].size() == 1 && (*folded)["messages"][0]["content"] == "Current compact record");
}

UAM_TEST(GeminiJournalRetainsEveryCurrentMessageWithoutImplicitCountCap)
{
	TempDir temp("uam-gemini-journal-many-messages");
	const auto file = temp.root / "session.jsonl";
	{
		std::ofstream output(file, std::ios::binary);
		output << Line(Header());
		for (int index = 0; index < 10003; ++index)
			output << Line({{"id", std::to_string(index)}, {"type", "gemini"}, {"content", "Retained " + std::to_string(index)}});
	}
	const auto folded = uam::gemini::FoldHistoryJournalFile(file);
	UAM_ASSERT(folded && (*folded)["messages"].size() == 10003 && (*folded)["messages"][10002]["content"] == "Retained 10002");
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	const auto full = GeminiJsonHistoryStore::ParseFile(file, ProviderProfileStore::DefaultGeminiProfile());
	UAM_ASSERT(full && full->messages.size() == 10003 && full->messages.back().content == "Retained 10002");
	const auto capped = GeminiJsonHistoryStore::ParseFile(file, ProviderProfileStore::DefaultGeminiProfile(), {0, 3});
	UAM_ASSERT(capped && capped->messages.size() == 3);
#endif
}

UAM_TEST(GeminiJournalHistoryDiscoveryPrefersCurrentSiblingAndKeepsLegacyCompatibility)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	TempDir temp("uam-gemini-journal-discovery");
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / (kParent + ".json"), Json{{"sessionId", kParent}, {"messages", Json::array({{{"type", "user"}, {"content", "Obsolete"}}})}}.dump()));
	const auto current = temp.root / (kParent + ".jsonl");
	UAM_ASSERT(uam::io::WriteTextFile(current, Journal()));
	const auto chats = GeminiJsonHistoryStore::Load(temp.root, provider);
	UAM_ASSERT_EQ(chats.size(), std::size_t(1));
	UAM_ASSERT_EQ(chats.front().messages.size(), std::size_t(2));
	UAM_ASSERT_EQ(chats.front().messages.back().content, std::string("Current answer"));
	UAM_ASSERT_EQ(chats.front().messages.back().thoughts, std::string("Native reasoning"));
	UAM_ASSERT_EQ(chats.front().messages.back().tool_calls.front().result_text, std::string("Current tool result"));
	UAM_ASSERT_EQ(ChatHistorySyncService().FindNativeSessionFilePath(temp.root, kParent).value(), current);
	UAM_ASSERT(!GeminiJsonHistoryStore::ParseFile(current, provider, {1, 0}));
	const auto legacy = temp.root / "legacy.jsonl";
	const auto legacy_text = Json{{"sessionId", "legacy"}, {"messages", Json::array({{{"type", "user"}, {"content", "Legacy named JSONL"}}})}}.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(legacy, legacy_text));
	UAM_ASSERT(GeminiJsonHistoryStore::ParseFile(legacy, provider));
	UAM_ASSERT_EQ(ChatHistorySyncService().FindNativeSessionFilePath(temp.root, "legacy").value(), legacy);
#endif
}

UAM_TEST(GeminiSavedNativeAgentIdUsesExactHostModelAndReportedChildState)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	const auto text = Line(Header()) + Line({{"id", "answer"}, {"type", "gemini"}, {"content", "Result"}, {"model", "actual-native-model"},
	    {"toolCalls", Json::array({{{"id", "delegation"}, {"name", "delegate_to_agent"}, {"status", "success"}, {"agentId", kChild},
	        {"resultDisplay", {{"agentName", "Reviewer"}, {"state", "completed"}}}, {"result", "Child result"}}})}});
	const auto parsed = GeminiJsonHistoryStore::ParseText(text, ProviderProfileStore::DefaultGeminiProfile(), {0, 0, "actual-ssh"});
	UAM_ASSERT(parsed && parsed->messages.size() == 1);
	UAM_ASSERT_EQ(parsed->messages.front().model_id, std::string("actual-native-model"));
	const auto& tool = parsed->messages.front().tool_calls.front();
	UAM_ASSERT(tool.is_sub_agent && tool.sub_agent_children.size() == 1);
	const auto reference = uam::native_agents::ParseHistoryId(tool.sub_agent_id);
	UAM_ASSERT(reference && reference->agent_id == kChild && reference->parent_session_id == kParent && reference->execution_host_id == "actual-ssh");
	UAM_ASSERT_EQ(tool.sub_agent_children.front().status, std::string("completed"));
	UAM_ASSERT_EQ(tool.sub_agent_title, std::string("Reviewer"));
#endif
}

UAM_TEST(GeminiCurrentJsonlParentAndChildSnapshotsPreserveExactIdentity)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	TempDir temp("uam-gemini-journal-snapshots");
	ScopedEnvVar home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
	const auto workspace = uam::paths::Utf8PathString(temp.root / "workspace");
	fs::create_directories(uam::paths::PathFromUtf8(workspace));
	const auto chats = temp.root / "gemini" / ".gemini" / "tmp" / "project" / "chats";
	UAM_ASSERT(uam::io::WriteTextFile(chats.parent_path() / ".project_root", workspace));
	const auto parent_file = chats / ("session-2026-10-16T12-00-" + kParent.substr(0, 8) + ".jsonl");
	UAM_ASSERT(uam::io::WriteTextFile(parent_file, Journal()));
	UAM_ASSERT(uam::io::WriteTextFile(fs::path(parent_file.string().substr(0, parent_file.string().size() - 1)), "legacy sibling no longer current"));
	auto child_metadata = Header(kChild); child_metadata["kind"] = "subagent"; child_metadata["parentSessionId"] = kParent; child_metadata["directories"] = Json::array({workspace});
	const auto child_text = Line(child_metadata) + Line({{"id", "child-a"}, {"type", "gemini"}, {"content", "Exact current child"}, {"thoughts", Json::array({"Child reasoning"})}});
	UAM_ASSERT(uam::io::WriteTextFile(chats / kParent / (kChild + ".jsonl"), child_text));
	uam::remote::native_history::Snapshot snapshot;
	std::string error;
	UAM_ASSERT(uam::remote::native_history::Load("gemini-cli", kParent, workspace, snapshot, error));
	UAM_ASSERT_EQ(snapshot.text, Journal());
	UAM_ASSERT(uam::remote::native_history::Load("gemini-cli", kParent, workspace, snapshot, error, kChild));
	UAM_ASSERT_EQ(snapshot.text, child_text);
	UAM_ASSERT(!uam::remote::native_history::GeminiChildIdentity(child_text, "foreign", kChild, workspace));
	const auto reference = uam::native_agents::BuildHistoryId("gemini-cli", kParent, kChild, "local");
	ChatSession parent; parent.provider_id = "gemini-cli"; parent.native_session_id = kParent; parent.workspace_directory = workspace;
	const auto loaded = uam::native_agents::LoadHistory(parent, reference, std::nullopt, &error);
	UAM_ASSERT(loaded && loaded->imported_read_only && loaded->native_session_id.empty());
	UAM_ASSERT_EQ(loaded->messages.front().content, std::string("Exact current child"));
	UAM_ASSERT_EQ(loaded->messages.front().thoughts, std::string("Child reasoning"));
#endif
}

UAM_TEST(GeminiJournalRejectsIdentityTypeClearingAndReassignment)
{
	TempDir temp("uam-gemini-identity-types");
	const auto file = temp.root / "identity.jsonl";
	for (const std::string key : {"sessionId", "projectHash", "parentSessionId"})
	{
		auto header = Header(kChild); header["parentSessionId"] = kParent;
		const auto initial = Line(header) + Line({{"id", "u"}, {"type", "user"}, {"content", "Original conversation"}});
		for (const auto& invalid : Json::array({nullptr, 7, false, Json::object(), Json::array(), ""}))
		{
			for (const bool metadata_only : {false, true})
			{
				const auto text = initial + Line({{"$set", {{key, invalid}}}}) + Line({{"$set", {{key, header[key]}}}});
				UAM_ASSERT(!uam::gemini::FoldHistoryJournal(text, !metadata_only));
				UAM_ASSERT(uam::io::WriteTextFile(file, text));
				UAM_ASSERT(!uam::gemini::ReadHistoryDocumentFile(file, 0, !metadata_only));
			}
			auto invalid_header = header; invalid_header[key] = invalid;
			UAM_ASSERT(!uam::gemini::FoldHistoryJournal(initial + Line(invalid_header)));
		}
		UAM_ASSERT(uam::gemini::FoldHistoryJournal(initial + Line({{"$set", {{key, header[key]}}}})));
	}
}

UAM_TEST(GeminiNativeThoughtSummaryObjectsPreserveReasoningOnlyMessages)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	const auto text = Line(Header()) + Line({{"id", "thought"}, {"type", "gemini"}, {"content", ""}, {"model", "native-thinking-model"},
	    {"thoughts", Json::array({{{"subject", "Inspect source"}, {"description", "Check the journal mutation order"}, {"timestamp", "2026-10-16T12:00:00Z"}},
	        {{"subject", ""}, {"description", "Preserve the final branch"}}})}});
	const auto chat = GeminiJsonHistoryStore::ParseText(text, ProviderProfileStore::DefaultGeminiProfile());
	UAM_ASSERT(chat && chat->messages.size() == 1);
	UAM_ASSERT(chat->messages.front().content.empty());
	UAM_ASSERT_EQ(chat->messages.front().thoughts, std::string("Inspect source\nCheck the journal mutation order\nPreserve the final branch"));
	UAM_ASSERT_EQ(chat->messages.front().model_id, std::string("native-thinking-model"));
#endif
}

UAM_TEST(GeminiNativeFunctionResponsesPreserveOriginalAndPatchedOutputAndError)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	const auto response = [](const std::string& id, Json value)
	{
		return Json::array({{{"functionResponse", {{"id", id}, {"name", "read_file"}, {"response", std::move(value)}}}}});
	};
	const auto initial = Line(Header()) + Line({{"id", "a"}, {"type", "gemini"}, {"content", ""}, {"toolCalls", Json::array({
	    {{"id", "output"}, {"name", "read_file"}, {"status", "success"}, {"result", response("output", {{"output", "Original file contents"}})}},
	    {{"id", "error"}, {"name", "read_file"}, {"status", "error"}, {"result", response("error", {{"error", "Original error"}})}}})}});
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	const auto before = GeminiJsonHistoryStore::ParseText(initial, provider);
	UAM_ASSERT(before && before->messages.size() == 1 && before->messages.front().tool_calls.size() == 2);
	UAM_ASSERT_EQ(before->messages.front().tool_calls[0].result_text, std::string("Original file contents"));
	UAM_ASSERT_EQ(before->messages.front().tool_calls[1].result_text, std::string("Original error"));
	const auto patched = initial + Line({{"$patch", {{"updates", Json::array({{{"id", "a"}, {"toolCalls", Json::array({
	    {{"id", "output"}, {"result", response("output", {{"output", "Current file contents"}, {"error", "Partial warning"}})}},
	    {{"id", "error"}, {"result", response("error", {{"error", {{"message", "Current error"}}}})}}})}}})}}}});
	TempDir temp("uam-gemini-function-response");
	const auto file = temp.root / "native.jsonl";
	UAM_ASSERT(uam::io::WriteTextFile(file, patched));
	const auto after = GeminiJsonHistoryStore::ParseFile(file, provider);
	UAM_ASSERT(after && after->messages.size() == 1 && after->messages.front().tool_calls.size() == 2);
	UAM_ASSERT_EQ(after->messages.front().tool_calls[0].result_text, std::string("Current file contents\nPartial warning"));
	UAM_ASSERT_EQ(after->messages.front().tool_calls[1].result_text, std::string("Current error"));
#endif
}

UAM_TEST(GeminiCompactLegacyDocumentsKeepIdlessMessagesUnderEitherExtension)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	TempDir temp("uam-gemini-compact-legacy");
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	const auto legacy = Json{{"sessionId", kParent}, {"projectHash", "project"}, {"messages", Json::array({
	    {{"type", "user"}, {"content", "Keep the legacy question"}}, {{"type", "gemini"}, {"content", "Keep the legacy answer"}}})}};
	for (const auto suffix : {".json", ".jsonl"})
	for (const int indent : {-1, 2})
	{
		const auto file = temp.root / (kParent + suffix);
		UAM_ASSERT(uam::io::WriteTextFile(file, legacy.dump(indent)));
		const auto chat = GeminiJsonHistoryStore::ParseFile(file, provider);
		UAM_ASSERT(chat && chat->messages.size() == 2);
		UAM_ASSERT_EQ(chat->messages.front().content, std::string("Keep the legacy question"));
		UAM_ASSERT_EQ(chat->messages.back().content, std::string("Keep the legacy answer"));
		const auto metadata = uam::gemini::ReadHistoryMetadataFile(file);
		UAM_ASSERT(metadata && (*metadata)["sessionId"] == kParent);
		UAM_ASSERT(!metadata->contains("messages"));
	}
	// A following mutation makes this a journal, not an isolated legacy snapshot.
	const auto journal = Line(Header()) + Line({{"id", "u"}, {"type", "user"}, {"content", "Removed"}}) + Line({{"$rewindTo", "u"}});
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "rewound.jsonl", journal));
	UAM_ASSERT(!GeminiJsonHistoryStore::ParseFile(temp.root / "rewound.jsonl", provider));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "corrupt.jsonl", Journal() + "broken\n"));
	UAM_ASSERT(!GeminiJsonHistoryStore::ParseFile(temp.root / "corrupt.jsonl", provider));
#endif
}

UAM_TEST(GeminiJournalOpenedStreamBoundaryIncludesHeldReplacement)
{
	TempDir temp("uam-gemini-held-open");
	const auto file = temp.root / "session.jsonl";
	const auto replacement = temp.root / "replacement.jsonl";
	UAM_ASSERT(uam::io::WriteTextFile(file, Journal()));
	const auto complete = Journal() + Line({{"id", "last"}, {"type", "gemini"}, {"content", "The entire larger replacement must survive"}});
	UAM_ASSERT(uam::io::WriteTextFile(replacement, complete));
	std::promise<void> at_open, replaced;
	auto release = replaced.get_future();
	auto waiting = at_open.get_future();
	auto reader = std::async(std::launch::async, [&]
	{
		return uam::gemini::FoldHistoryJournalFile(file, 0, true, [&](const fs::path& requested, std::ifstream& input)
		{
			at_open.set_value();
			release.wait();
			return uam::io::OpenReadFile(requested, input);
		});
	});
	waiting.wait();
	std::error_code backup_error, replace_error;
	fs::rename(file, temp.root / "old.jsonl", backup_error);
	fs::rename(replacement, file, replace_error);
	replaced.set_value(); // Release before any assertion can throw.
	const auto folded = reader.get();
	UAM_ASSERT(!backup_error && !replace_error);
	UAM_ASSERT(folded && (*folded)["messages"].size() == 3);
	UAM_ASSERT_EQ((*folded)["messages"][2]["content"].get<std::string>(), std::string("The entire larger replacement must survive"));
	UAM_ASSERT(!uam::gemini::FoldHistoryJournalFile(file, complete.size() - 1));
}

UAM_TEST(GeminiLocalExactChildStreamsLargeParentAndChildWithoutSnapshotCap)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	TempDir temp("uam-gemini-large-local-child");
	ScopedEnvVar home("GEMINI_CLI_HOME", uam::paths::Utf8PathString(temp.root / "gemini"));
	const auto workspace = uam::paths::Utf8PathString(temp.root / "workspace");
	fs::create_directories(uam::paths::PathFromUtf8(workspace));
	const auto chats = temp.root / "gemini" / ".gemini" / "tmp" / "project" / "chats";
	UAM_ASSERT(uam::io::WriteTextFile(chats.parent_path() / ".project_root", workspace));
	const auto parent_file = chats / (kParent + ".jsonl");
	const auto child_file = chats / kParent / (kChild + ".jsonl");
	fs::create_directories(child_file.parent_path());
	const auto write_large = [&](const fs::path& path, const Json& header, const std::string& current)
	{
		std::ofstream output;
		// Fixtures use the same native path normalization as production readers.
		fs::path native; std::string path_error;
		UAM_ASSERT(uam::io::NativeFilePath(path, native, path_error));
		output.open(native, std::ios::binary);
		UAM_ASSERT(output.good());
		output << Line(header);
		const auto obsolete = Line({{"id", "answer"}, {"type", "gemini"}, {"content", std::string(2 * 1024 * 1024, 'x')}});
		for (int index = 0; index < 33; ++index) output << obsolete;
		output << Line({{"id", "answer"}, {"type", "gemini"}, {"content", current}});
		output.close();
		UAM_ASSERT(output.good());
		UAM_ASSERT(fs::file_size(path) > 64U * 1024U * 1024U);
	};
	write_large(parent_file, Header(), "Current parent");
	auto child_header = Header(kChild); child_header["kind"] = "subagent"; child_header["parentSessionId"] = kParent; child_header["directories"] = Json::array({workspace});
	write_large(child_file, child_header, "Current child beyond obsolete append history");
	ChatSession parent; parent.provider_id = "gemini-cli"; parent.native_session_id = kParent; parent.workspace_directory = workspace;
	const auto reference = uam::native_agents::BuildHistoryId("gemini-cli", kParent, kChild, "local");
	std::string error;
	const auto loaded = uam::native_agents::LoadHistory(parent, reference, std::nullopt, &error);
	UAM_ASSERT(loaded && loaded->messages.size() == 1 && loaded->imported_read_only && loaded->native_session_id.empty());
	UAM_ASSERT_EQ(loaded->messages.front().content, std::string("Current child beyond obsolete append history"));
	UAM_ASSERT_EQ(loaded->execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(loaded->parent_chat_id, kParent);
	// The SSH transport retains its independent bounded snapshot contract.
	uam::remote::native_history::Snapshot snapshot;
	UAM_ASSERT(!uam::remote::native_history::Load("gemini-cli", kParent, workspace, snapshot, error, kChild));
	UAM_ASSERT(uam::io::WriteTextFile(parent_file, Journal()));
	UAM_ASSERT(!uam::remote::native_history::Load("gemini-cli", kParent, workspace, snapshot, error, kChild));
	const auto small_child = Line(child_header) + Line({{"id", "answer"}, {"type", "gemini"}, {"content", "Bounded SSH child"}});
	UAM_ASSERT(uam::io::WriteTextFile(child_file, small_child));
	UAM_ASSERT(uam::remote::native_history::Load("gemini-cli", kParent, workspace, snapshot, error, kChild));
	UAM_ASSERT_EQ(snapshot.text, small_child);
	std::stop_source cancelled; cancelled.request_stop();
	UAM_ASSERT(!uam::gemini::LoadNativeAgentHistory({"gemini-cli", kParent, kChild, "local"}, workspace, &error, cancelled.get_token()));
#endif
}

UAM_TEST(GeminiLocalExactChildRejectsForeignOwnershipAndCorruptCurrentJournal)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	TempDir temp("uam-gemini-child-attestation");
	ScopedEnvVar home("GEMINI_CLI_HOME", uam::paths::Utf8PathString(temp.root / "gemini"));
	const auto workspace = uam::paths::Utf8PathString(temp.root / "workspace");
	fs::create_directories(uam::paths::PathFromUtf8(workspace));
	const auto chats = temp.root / "gemini" / ".gemini" / "tmp" / "project" / "chats";
	UAM_ASSERT(uam::io::WriteTextFile(chats.parent_path() / ".project_root", workspace));
	UAM_ASSERT(uam::io::WriteTextFile(chats / (kParent + ".jsonl"), Journal()));
	const auto child_file = chats / kParent / (kChild + ".jsonl");
	auto header = Header(kChild); header["kind"] = "subagent"; header["parentSessionId"] = kParent; header["directories"] = Json::array({workspace});
	const auto child = [&](const Json& metadata)
	{
		return Line(metadata) + Line({{"id", "a"}, {"type", "gemini"}, {"content", "Owned child"}});
	};
	const uam::native_agents::HistoryReference reference{"gemini-cli", kParent, kChild, "local"};
	std::string error;
	UAM_ASSERT(uam::io::WriteTextFile(child_file, child(header)));
	UAM_ASSERT(uam::gemini::LoadNativeAgentHistory(reference, workspace, &error));
	UAM_ASSERT(uam::io::WriteTextFile(chats / kParent / (kChild + ".json"), child(header)));
	for (const auto& mutation : Json::array({Json{{"sessionId", kParent}}, Json{{"parentSessionId", "foreign"}},
	    Json{{"parentSessionId", nullptr}}, Json{{"kind", "main"}}, Json{{"directories", Json::array({workspace + "-foreign"})}}}))
	{
		auto foreign = header;
		for (auto item = mutation.begin(); item != mutation.end(); ++item) foreign[item.key()] = item.value();
		UAM_ASSERT(uam::io::WriteTextFile(child_file, child(foreign)));
		UAM_ASSERT(!uam::gemini::LoadNativeAgentHistory(reference, workspace, &error));
	}
	UAM_ASSERT(uam::io::WriteTextFile(child_file, child(header) + "corrupt\n"));
	UAM_ASSERT(!uam::gemini::LoadNativeAgentHistory(reference, workspace, &error));
	UAM_ASSERT(uam::io::WriteTextFile(child_file, child(header) + "{\"incomplete\":"));
	UAM_ASSERT(uam::gemini::LoadNativeAgentHistory(reference, workspace, &error));
	UAM_ASSERT(!uam::gemini::LoadNativeAgentHistory(reference, workspace + "-foreign", &error));
	auto foreign_host = reference; foreign_host.execution_host_id = "ssh-other";
	UAM_ASSERT(!uam::gemini::LoadNativeAgentHistory(foreign_host, workspace, &error));
	auto foreign_parent = Header(kChild);
	UAM_ASSERT(uam::io::WriteTextFile(chats / (kParent + ".jsonl"), Line(foreign_parent)));
	UAM_ASSERT(!uam::gemini::LoadNativeAgentHistory(reference, workspace, &error));
#endif
}

UAM_TEST(GeminiJournalNormalizedReadersPreserveUnicodeBeyondMaxPath)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	TempDir temp("uam-gemini-unicode-path");
	auto directory = temp.root;
	for (int index = 0; index < 12; ++index) directory /= uam::paths::PathFromUtf8("Gemini-étude-会話-" + std::to_string(index));
	const auto file = directory / "session.jsonl";
	UAM_ASSERT(uam::paths::Utf8PathString(file).size() > 260);
	UAM_ASSERT(uam::io::WriteTextFile(file, Journal()));
	const auto document = uam::gemini::FoldHistoryJournalFile(file);
	UAM_ASSERT(document && (*document)["messages"].size() == 2);
	const auto metadata = uam::gemini::ReadHistoryMetadataFile(file);
	UAM_ASSERT(metadata && (*metadata)["sessionId"] == kParent && !metadata->contains("messages"));
	const auto chat = GeminiJsonHistoryStore::ParseFile(file, ProviderProfileStore::DefaultGeminiProfile());
	UAM_ASSERT(chat && chat->messages.size() == 2 && chat->messages.back().content == "Current answer");
#endif
}

namespace
{
	struct GeminiOwnershipFixture
	{
		TempDir temp{"uam-gemini-published-ownership"};
		const fs::path profile = temp.root / "profile";
		ScopedEnvVar home{"GEMINI_CLI_HOME", uam::paths::Utf8PathString(profile)};
		const fs::path storage = profile / ".gemini";
		const fs::path project = storage / "tmp" / "project";
		const fs::path chats = project / "chats";
		const std::string workspace = uam::paths::Utf8PathString(temp.root / "workspace");
		std::string child_text;
		GeminiOwnershipFixture()
		{
			fs::create_directories(uam::paths::PathFromUtf8(workspace));
			UAM_ASSERT(uam::io::WriteTextFile(storage / "projects.json", Json{{"projects", {{workspace, "project"}}}}.dump()));
			UAM_ASSERT(uam::io::WriteTextFile(project / ".project_root", workspace));
			UAM_ASSERT(uam::io::WriteTextFile(chats / (kParent + ".jsonl"), Journal()));
			auto header = Header(kChild); header["kind"] = "subagent"; header["directories"] = Json::array({workspace});
			// The actual published header has no optional parentSessionId.
			child_text = Line(header) + Line({{"id", "answer"}, {"type", "gemini"}, {"content", "Exact published child"}});
			UAM_ASSERT(uam::io::WriteTextFile(chats / kParent / (kChild + ".jsonl"), child_text));
		}
		ChatSession Parent() const
		{
			ChatSession parent; parent.provider_id = "gemini-cli"; parent.native_session_id = kParent; parent.workspace_directory = workspace;
			return parent;
		}
		std::optional<ChatSession> LocalChild() const
		{
			return uam::native_agents::LoadHistory(Parent(), uam::native_agents::BuildHistoryId("gemini-cli", kParent, kChild, "local"), std::nullopt, nullptr);
		}
		bool Helper(bool child = true) const
		{
			uam::remote::native_history::Snapshot snapshot;
			std::string error;
			return uam::remote::native_history::Load("gemini-cli", kParent, workspace, snapshot, error, child ? kChild : "");
		}
	};

	void CreateGeminiDirectoryAlias(const fs::path& target, const fs::path& alias)
	{
#if defined(_WIN32)
		UAM_ASSERT(RunTestCommand("cmd.exe /d /c mklink /J " + ShellQuoteForTest(uam::paths::Utf8PathString(alias)) +
		    " " + ShellQuoteForTest(uam::paths::Utf8PathString(target))));
#else
		std::error_code error;
		fs::create_directory_symlink(target, alias, error);
		UAM_ASSERT(!error);
#endif
	}
}

UAM_TEST(GeminiPublishedConfiguredHomeFindsLocalAndHelperHistory)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiOwnershipFixture fixture;
	ScopedEnvVar os_home("HOME", "");
	ScopedEnvVar os_profile("USERPROFILE", "");
	UAM_ASSERT_EQ(AppPaths::GeminiHomePath(), fixture.storage);
	const auto local = fixture.LocalChild();
	UAM_ASSERT(local && local->messages.front().content == "Exact published child" && local->parent_chat_id == kParent);
	UAM_ASSERT(fixture.Helper() && fixture.Helper(false));
	std::string error;
	const auto sources = DiscoverGeminiTmpChatSources(&error);
	UAM_ASSERT(error.empty() && sources.size() == 1 && sources.front().chats_dir == fixture.chats);
	const auto parent = ChatHistorySyncService().LoadLocalNativeTranscript(fixture.Parent(), &error);
	UAM_ASSERT(parent && parent->messages.back().content == "Current answer");
	// A plausible old-layout decoy must never become an ambiguous fallback.
	UAM_ASSERT(uam::io::WriteTextFile(fixture.profile / "tmp" / "project" / ".project_root", fixture.workspace));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.profile / "tmp" / "project" / "chats" / (kParent + ".jsonl"), Journal()));
	fs::remove_all(fixture.storage);
	UAM_ASSERT(!fixture.LocalChild() && !fixture.Helper() && !fixture.Helper(false));
#endif
}

UAM_TEST(GeminiPublishedDefaultHomeIgnoresUndocumentedStorageOverride)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiOwnershipFixture fixture;
	ScopedEnvVar configured("GEMINI_CLI_HOME", "");
	ScopedEnvVar home("HOME", uam::paths::Utf8PathString(fixture.profile));
	ScopedEnvVar profile("USERPROFILE", uam::paths::Utf8PathString(fixture.profile));
	ScopedEnvVar legacy_override("GEMINI_HOME", uam::paths::Utf8PathString(fixture.temp.root / "decoy"));
	UAM_ASSERT_EQ(AppPaths::GeminiHomePath(), fixture.storage);
	UAM_ASSERT(fixture.LocalChild() && fixture.Helper() && fixture.Helper(false));
	const auto parent = ChatHistorySyncService().LoadLocalNativeTranscript(fixture.Parent());
	UAM_ASSERT(parent && parent->native_session_id == kParent);
#endif
}

UAM_TEST(GeminiConflictingRegistryAndMarkerRejectLocalAndHelperHistory)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiOwnershipFixture fixture;
	const auto foreign = fixture.workspace + "-other";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.project / ".project_root", foreign));
	auto header = Header(kChild); header["kind"] = "subagent"; header["directories"] = Json::array({foreign, fixture.workspace});
	UAM_ASSERT(uam::io::WriteTextFile(fixture.chats / kParent / (kChild + ".jsonl"), Line(header) + Line({{"id", "a"}, {"type", "gemini"}, {"content", "Foreign owner"}})));
	UAM_ASSERT(!fixture.LocalChild() && !fixture.Helper() && !fixture.Helper(false));
	std::string error;
	UAM_ASSERT(DiscoverGeminiTmpChatSources(&error).empty());
	UAM_ASSERT(error.find("conflict") != std::string::npos);
	UAM_ASSERT(!ChatHistorySyncService().LoadLocalNativeTranscript(fixture.Parent()));
#endif
}

UAM_TEST(GeminiAbsentAndUnreadableMarkersHaveDistinctRegistryFallback)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiOwnershipFixture fixture;
	const auto marker = fixture.project / ".project_root";
	fs::remove(marker);
	UAM_ASSERT(uam::gemini::ReadHistoryOwnershipFile(fixture.storage, fs::path("tmp") / "project" / ".project_root", 16384).state == uam::gemini::HistoryPathState::Missing);
	UAM_ASSERT(fixture.LocalChild() && fixture.Helper() && fixture.Helper(false));
	std::string error;
	UAM_ASSERT(DiscoverGeminiTmpChatSources(&error).size() == 1 && error.empty());
	// An existing directory is portably unreadable as a marker, even for root.
	fs::create_directory(marker);
	UAM_ASSERT(uam::gemini::ReadHistoryOwnershipFile(fixture.storage, fs::path("tmp") / "project" / ".project_root", 16384).state == uam::gemini::HistoryPathState::Unreadable);
	UAM_ASSERT(!fixture.LocalChild() && !fixture.Helper() && !fixture.Helper(false));
	UAM_ASSERT(DiscoverGeminiTmpChatSources(&error).empty() && !error.empty());
	fs::remove(marker);
	fs::remove(fixture.storage / "projects.json");
	UAM_ASSERT(!fixture.LocalChild() && !fixture.Helper(false));
#endif
}

UAM_TEST(GeminiParentDirectoryAliasesCannotAttestAnotherParentsChild)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiOwnershipFixture fixture;
	const auto other = fixture.chats / "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
	fs::rename(fixture.chats / kParent, other);
	CreateGeminiDirectoryAlias(other, fixture.chats / kParent);
	UAM_ASSERT(fixture.Helper(false));
	UAM_ASSERT(!fixture.LocalChild() && !fixture.Helper());
	fs::remove(fixture.chats / kParent);
#endif
}

UAM_TEST(GeminiChildFileAliasesCannotAttestAnotherParentsChild)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiOwnershipFixture fixture;
	const auto original = fixture.chats / kParent / (kChild + ".jsonl");
	const auto other = fixture.chats / "cccccccc-cccc-4ccc-8ccc-cccccccccccc" / (kChild + ".jsonl");
	UAM_ASSERT(uam::io::WriteTextFile(other, fixture.child_text));
	fs::remove(original);
	std::error_code error;
	fs::create_symlink(other, original, error);
	UAM_ASSERT(!error);
	UAM_ASSERT(fixture.Helper(false));
	UAM_ASSERT(!fixture.LocalChild() && !fixture.Helper());
	fs::remove(original);
#endif
}

UAM_TEST(GeminiProjectChatAliasesCannotCrossCanonicalProjectOwnership)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiOwnershipFixture fixture;
	const auto other_project = fixture.storage / "tmp" / "other-project";
	fs::create_directories(other_project);
	fs::rename(fixture.chats, other_project / "chats");
	CreateGeminiDirectoryAlias(other_project / "chats", fixture.chats);
	UAM_ASSERT(!fixture.LocalChild() && !fixture.Helper() && !fixture.Helper(false));
	UAM_ASSERT(!ChatHistorySyncService().LoadLocalNativeTranscript(fixture.Parent()));
	fs::remove(fixture.chats);
#endif
}

UAM_TEST(GeminiCanonicalProviderRootKeepsOrdinaryNativeHeaderReadable)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiOwnershipFixture fixture;
	const auto actual = fixture.temp.root / "canonical-storage";
	fs::rename(fixture.storage, actual);
	CreateGeminiDirectoryAlias(actual, fixture.storage);
	const auto local = fixture.LocalChild();
	UAM_ASSERT(local && local->parent_chat_id == kParent && local->messages.front().content == "Exact published child");
	UAM_ASSERT(fixture.Helper() && fixture.Helper(false));
	UAM_ASSERT(ChatHistorySyncService().LoadLocalNativeTranscript(fixture.Parent()));
	fs::remove(fixture.storage);
#endif
}

UAM_TEST(GeminiCanceledParentReadsAndDiscoveryPreserveAuthoritativeHistory)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiOwnershipFixture fixture;
	const auto file = fixture.chats / (kParent + ".jsonl");
	const auto original = uam::io::ReadTextFile(file);
	std::stop_source stop;
	stop.request_stop();
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	UAM_ASSERT(!GeminiJsonHistoryStore::ParseFile(file, provider, {}, stop.get_token()));
	UAM_ASSERT(GeminiJsonHistoryStore::Load(fixture.chats, provider, {}, stop.get_token()).empty());
	UAM_ASSERT(!uam::gemini::ReadHistoryMetadataFile(file, stop.get_token()));
	UAM_ASSERT(!ChatHistorySyncService().FindNativeSessionFilePath(fixture.chats, kParent, stop.get_token()));
	UAM_ASSERT(DiscoverGeminiTmpChatSources(nullptr, stop.get_token()).empty());
	UAM_ASSERT(!uam::remote::native_history::VisitGeminiParentFiles(kParent, fixture.workspace, [](const auto&, const auto&) { return true; }, stop.get_token()));
	auto parent = fixture.Parent(); parent.messages.push_back({MessageRole::User, "Authoritative saved UAM history"});
	UAM_ASSERT(!ChatHistorySyncService().LoadLocalNativeTranscript(parent, nullptr, stop.get_token()));
	UAM_ASSERT(parent.messages.size() == 1 && parent.messages.front().content == "Authoritative saved UAM history");
	UAM_ASSERT_EQ(uam::io::ReadTextFile(file), original);
	// Hold a real opened stream, then cancel before the initial bounded read.
	std::stop_source after_open;
	const auto interrupted = uam::gemini::detail::ReadHistoryFile(file, 0, true, true, [&](const fs::path& requested, std::ifstream& input)
	{
		const bool opened = uam::io::OpenReadFile(requested, input);
		after_open.request_stop();
		return opened;
	}, after_open.get_token());
	UAM_ASSERT(!interrupted);
#endif
}

namespace
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	struct GeminiParentFileAliasFixture
	{
		GeminiOwnershipFixture owned;
		const std::string other_id = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
		const std::string other_workspace = uam::paths::Utf8PathString(owned.temp.root / "other-workspace");
		const fs::path other_project = owned.storage / "tmp" / "other-project";
		const fs::path other_file = other_project / "chats" / ("session-2026-10-16T12-00-" + other_id.substr(0, 8) + ".jsonl");
		const fs::path alias = owned.chats / other_file.filename();
		GeminiParentFileAliasFixture()
		{
			fs::create_directories(uam::paths::PathFromUtf8(other_workspace));
			UAM_ASSERT(uam::io::WriteTextFile(owned.storage / "projects.json", Json{{"projects", {{owned.workspace, "project"}, {other_workspace, "other-project"}}}}.dump()));
			UAM_ASSERT(uam::io::WriteTextFile(other_project / ".project_root", other_workspace));
			auto header = Header(other_id); header["projectHash"] = "other-project";
			UAM_ASSERT(uam::io::WriteTextFile(other_file, Line(header) + Line({{"id", "answer"}, {"type", "gemini"}, {"content", "Other project's private parent"}})));
			std::error_code error;
			fs::create_symlink(other_file, alias, error);
			UAM_ASSERT(!error);
			UAM_ASSERT(fs::is_symlink(alias) && fs::canonical(alias) == fs::canonical(other_file));
			// The selected chats directory itself passes the existing directory fence.
			UAM_ASSERT(uam::gemini::HistoryDirectoryInProject(owned.storage, fs::path("tmp") / "project" / "chats"));
			std::string discovery_error;
			const auto sources = DiscoverGeminiTmpChatSources(&discovery_error);
			UAM_ASSERT(discovery_error.empty() && sources.size() == 2);
			UAM_ASSERT(std::ranges::any_of(sources, [&](const auto& source)
			{
				return source.chats_dir == owned.chats && source.folder_directory == owned.workspace;
			}));
			// Standalone caller-supplied parsing is intentionally a different boundary.
			const auto standalone = GeminiJsonHistoryStore::ParseFile(alias, ProviderProfileStore::DefaultGeminiProfile());
			UAM_ASSERT(standalone && standalone->native_session_id == other_id && standalone->messages.size() == 1);
			UAM_ASSERT_EQ(standalone->messages.front().content, std::string("Other project's private parent"));
		}
		ChatFolder Folder(const std::string& workspace) const
		{
			ChatFolder folder; folder.id = "selected-folder"; folder.directory = workspace; folder.execution_host_id = "local";
			return folder;
		}
		ChatSession OtherParent(const std::string& workspace) const
		{
			auto parent = owned.Parent(); parent.native_session_id = other_id; parent.workspace_directory = workspace; parent.execution_host_id = "local";
			return parent;
		}
	};

	struct GeminiTwoProjectOwnershipFixture
	{
		GeminiOwnershipFixture owned;
		const std::string other_workspace = uam::paths::Utf8PathString(owned.temp.root / "other-workspace");
		const fs::path other_project = owned.storage / "tmp" / "other-project";
		std::string other_parent_text;
		std::string other_child_text;
		GeminiTwoProjectOwnershipFixture()
		{
			fs::create_directories(uam::paths::PathFromUtf8(other_workspace));
			UAM_ASSERT(uam::io::WriteTextFile(owned.storage / "projects.json", Json{{"projects", {{owned.workspace, "project"}, {other_workspace, "other-project"}}}}.dump()));
			UAM_ASSERT(uam::io::WriteTextFile(other_project / ".project_root", other_workspace));
			auto parent_header = Header(); parent_header["projectHash"] = "other-project";
			other_parent_text = Line(parent_header) + Line({{"id", "answer"}, {"type", "gemini"}, {"content", "Other project parent"}});
			UAM_ASSERT(uam::io::WriteTextFile(other_project / "chats" / (kParent + ".jsonl"), other_parent_text));
			auto header = Header(kChild); header["projectHash"] = "other-project"; header["kind"] = "subagent"; header["directories"] = Json::array({other_workspace, owned.workspace});
			// Same parent/child IDs deliberately exist in both projects: workspace ownership must select them.
			other_child_text = Line(header) + Line({{"id", "answer"}, {"type", "gemini"}, {"content", "Other project child"}});
			UAM_ASSERT(uam::io::WriteTextFile(other_project / "chats" / kParent / (kChild + ".jsonl"), other_child_text));
		}
		std::optional<ChatSession> LocalChild(const std::string& workspace, std::string* error) const
		{
			auto parent = owned.Parent(); parent.workspace_directory = workspace; parent.execution_host_id = "local";
			return uam::native_agents::LoadHistory(parent, uam::native_agents::BuildHistoryId("gemini-cli", kParent, kChild, "local"), std::nullopt, error);
		}
		void AssertReadable(const std::string& workspace, const std::string& parent_text, const std::string& child_text, const std::string& content) const
		{
			std::string local_error, parent_error, child_error;
			const auto local = LocalChild(workspace, &local_error);
			uam::remote::native_history::Snapshot parent_snapshot, child_snapshot;
			// Exercise all three routes before an assertion can stop the case.
			const bool parent_ok = uam::remote::native_history::Load("gemini-cli", kParent, workspace, parent_snapshot, parent_error);
			const bool child_ok = uam::remote::native_history::Load("gemini-cli", kParent, workspace, child_snapshot, child_error, kChild);
			UAM_ASSERT(local && local_error.empty());
			UAM_ASSERT_EQ(local->provider_id, std::string("gemini-cli"));
			UAM_ASSERT_EQ(local->workspace_directory, workspace);
			UAM_ASSERT_EQ(local->execution_host_id, std::string("local"));
			UAM_ASSERT_EQ(local->parent_chat_id, kParent);
			UAM_ASSERT_EQ(local->id, uam::native_agents::BuildHistoryId("gemini-cli", kParent, kChild, "local"));
			UAM_ASSERT(local->imported_read_only && local->native_session_id.empty() && local->messages.size() == 1);
			UAM_ASSERT_EQ(local->messages.front().content, content);
			UAM_ASSERT(parent_ok && parent_error.empty() && parent_snapshot.ready);
			UAM_ASSERT_EQ(parent_snapshot.provider, std::string("gemini-cli"));
			UAM_ASSERT_EQ(parent_snapshot.session_id, kParent);
			UAM_ASSERT(parent_snapshot.agent_id.empty());
			UAM_ASSERT_EQ(parent_snapshot.workspace, workspace);
			UAM_ASSERT_EQ(parent_snapshot.text, parent_text);
			UAM_ASSERT(child_ok && child_error.empty() && child_snapshot.ready);
			UAM_ASSERT_EQ(child_snapshot.provider, std::string("gemini-cli"));
			UAM_ASSERT_EQ(child_snapshot.session_id, kParent);
			UAM_ASSERT_EQ(child_snapshot.agent_id, kChild);
			UAM_ASSERT_EQ(child_snapshot.workspace, workspace);
			UAM_ASSERT_EQ(child_snapshot.text, child_text);
		}
		void AssertRejected(const std::string& workspace) const
		{
			std::string local_error, parent_error, child_error;
			const auto local = LocalChild(workspace, &local_error);
			uam::remote::native_history::Snapshot parent_snapshot, child_snapshot;
			const bool parent_ok = uam::remote::native_history::Load("gemini-cli", kParent, workspace, parent_snapshot, parent_error);
			const bool child_ok = uam::remote::native_history::Load("gemini-cli", kParent, workspace, child_snapshot, child_error, kChild);
			UAM_ASSERT(!local && !local_error.empty());
			UAM_ASSERT(!parent_ok && !parent_error.empty() && !parent_snapshot.ready);
			UAM_ASSERT(!child_ok && !child_error.empty() && !child_snapshot.ready);
		}
		void AssertGlobalUnreadableRegistryRejected() const
		{
			const auto registry = owned.storage / "projects.json";
			const auto original = uam::io::ReadTextFile(registry);
			fs::remove(registry);
			fs::create_directory(registry); // Deterministically unreadable as a file, including as root.
			UAM_ASSERT(uam::gemini::ReadHistoryOwnershipFile(owned.storage, "projects.json", 1024 * 1024).state == uam::gemini::HistoryPathState::Unreadable);
			AssertRejected(owned.workspace);
			AssertRejected(other_workspace);
			std::string error;
			UAM_ASSERT(DiscoverGeminiTmpChatSources(&error).empty());
			UAM_ASSERT(error.find("registry") != std::string::npos);
			fs::remove(registry);
			UAM_ASSERT(uam::io::WriteTextFile(registry, original));
		}
	};

	void AssertGeminiUnrelatedProjectErrorIsScoped(bool conflicting)
	{
		GeminiTwoProjectOwnershipFixture fixture;
		fixture.AssertReadable(fixture.owned.workspace, Journal(), fixture.owned.child_text, "Exact published child");
		fixture.AssertReadable(fixture.other_workspace, fixture.other_parent_text, fixture.other_child_text, "Other project child");
		fixture.AssertGlobalUnreadableRegistryRejected();
		const auto marker = fixture.other_project / ".project_root";
		if (conflicting)
		{
			const auto contrary_workspace = fixture.other_workspace + "-contrary";
			fs::create_directories(uam::paths::PathFromUtf8(contrary_workspace));
			UAM_ASSERT(uam::io::WriteTextFile(marker, contrary_workspace));
		}
		else
		{
			fs::remove(marker);
			fs::create_directory(marker);
			UAM_ASSERT(uam::gemini::ReadHistoryOwnershipFile(fixture.owned.storage, fs::path("tmp") / "other-project" / ".project_root", 16384).state == uam::gemini::HistoryPathState::Unreadable);
		}
		fixture.AssertRejected(fixture.other_workspace);
		std::string error;
		const auto sources = DiscoverGeminiTmpChatSources(&error);
		UAM_ASSERT(!error.empty()); // Successful exact reads must not erase the ownership defect diagnostic.
		UAM_ASSERT(error.find(conflicting ? "conflict" : "marker") != std::string::npos);
		UAM_ASSERT(sources.size() == 1 && sources.front().folder_directory == fixture.owned.workspace && sources.front().chats_dir == fixture.owned.chats);
		fixture.AssertReadable(fixture.owned.workspace, Journal(), fixture.owned.child_text, "Exact published child");
	}
#endif
}

UAM_TEST(GeminiParentFileAliasCannotCrossProjectDuringFolderDiscovery)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiParentFileAliasFixture fixture;
	const ChatHistorySyncService service;
	const auto other = service.DiscoverProviderChatsForFolder(fixture.Folder(fixture.other_workspace), nullptr, {}, 0, false, {}, {}, "gemini-cli");
	UAM_ASSERT(other.result.success && other.chats.size() == 1 && other.chats.front().native_session_id == fixture.other_id);
	UAM_ASSERT_EQ(other.chats.front().workspace_directory, fixture.other_workspace);
	UAM_ASSERT_EQ(other.chats.front().messages.front().content, std::string("Other project's private parent"));
	const auto selected = service.DiscoverProviderChatsForFolder(fixture.Folder(fixture.owned.workspace), nullptr, {}, 0, false, {}, {}, "gemini-cli");
	UAM_ASSERT(std::ranges::none_of(selected.chats, [&](const auto& chat) { return chat.native_session_id == fixture.other_id; }));
	UAM_ASSERT_EQ(selected.chats.size(), std::size_t(1));
	UAM_ASSERT_EQ(selected.chats.front().native_session_id, kParent);
	UAM_ASSERT_EQ(selected.chats.front().provider_id, std::string("gemini-cli"));
	UAM_ASSERT_EQ(selected.chats.front().workspace_directory, fixture.owned.workspace);
	UAM_ASSERT_EQ(selected.chats.front().folder_id, std::string("selected-folder"));
	UAM_ASSERT_EQ(selected.chats.front().messages.back().content, std::string("Current answer"));
#endif
}

UAM_TEST(GeminiParentFileAliasCannotCrossProjectDuringLocalRefresh)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiParentFileAliasFixture fixture;
	const ChatHistorySyncService service;
	std::string error;
	const auto real = service.LoadLocalNativeTranscript(fixture.OtherParent(fixture.other_workspace), &error);
	UAM_ASSERT(real && real->native_session_id == fixture.other_id && real->workspace_directory == fixture.other_workspace);
	UAM_ASSERT_EQ(real->messages.front().content, std::string("Other project's private parent"));
	auto selected = fixture.OtherParent(fixture.owned.workspace);
	selected.messages.push_back({MessageRole::User, "Authoritative saved A history"});
	const auto original_file = uam::io::ReadTextFile(fixture.other_file);
	const auto refreshed = service.LoadLocalNativeTranscript(selected, &error);
	UAM_ASSERT(!refreshed && !error.empty());
	UAM_ASSERT(selected.messages.size() == 1 && selected.messages.front().content == "Authoritative saved A history");
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.other_file), original_file);
	const auto healthy = service.LoadLocalNativeTranscript(fixture.owned.Parent());
	UAM_ASSERT(healthy && healthy->native_session_id == kParent && healthy->messages.back().content == "Current answer");
#endif
}

UAM_TEST(GeminiUnreadableOtherProjectKeepsHealthyExactHistoryReadable)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	AssertGeminiUnrelatedProjectErrorIsScoped(false);
#endif
}

UAM_TEST(GeminiConflictingOtherProjectKeepsHealthyExactHistoryReadable)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	AssertGeminiUnrelatedProjectErrorIsScoped(true);
#endif
}

UAM_TEST(GeminiUnregisteredUnreadableProjectKeepsHealthyExactHistoryReadable)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiTwoProjectOwnershipFixture fixture;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.owned.storage / "projects.json", Json{{"projects", {{fixture.owned.workspace, "project"}}}}.dump()));
	const auto marker = fixture.other_project / ".project_root";
	fs::remove(marker);
	fs::create_directory(marker);
	const auto discovery = uam::gemini::DiscoverHistoryProjectsWithDiagnostics();
	UAM_ASSERT(!discovery.Fatal() && discovery.projects.size() == 1 && discovery.diagnostics.size() == 1);
	UAM_ASSERT(discovery.diagnostics.front().scope == uam::gemini::HistoryDiagnosticScope::Project);
	UAM_ASSERT_EQ(discovery.diagnostics.front().project, fs::path("tmp") / "other-project");
	UAM_ASSERT(discovery.diagnostics.front().workspaces.empty());
	UAM_ASSERT(discovery.DiagnosticText().find("marker") != std::string::npos);
	fixture.AssertRejected(fixture.other_workspace);
	fixture.AssertReadable(fixture.owned.workspace, Journal(), fixture.owned.child_text, "Exact published child");
#endif
}

UAM_TEST(GeminiRejectedProjectOwnershipCannotHideBehindHealthyWorkspace)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiTwoProjectOwnershipFixture fixture;
	fixture.AssertReadable(fixture.owned.workspace, Journal(), fixture.owned.child_text, "Exact published child");
	// B's registry says B, but its marker also claims A: the diagnostic affects both.
	UAM_ASSERT(uam::io::WriteTextFile(fixture.other_project / ".project_root", fixture.owned.workspace));
	const auto discovery = uam::gemini::DiscoverHistoryProjectsWithDiagnostics();
	UAM_ASSERT(!discovery.Fatal() && discovery.projects.size() == 1 && discovery.diagnostics.size() == 1);
	UAM_ASSERT(!discovery.CanReadWorkspace(fixture.owned.workspace) && !discovery.CanReadWorkspace(fixture.other_workspace));
	UAM_ASSERT_EQ(discovery.diagnostics.front().workspaces.size(), std::size_t(2));
	UAM_ASSERT(std::ranges::find(discovery.diagnostics.front().workspaces, fixture.owned.workspace) != discovery.diagnostics.front().workspaces.end());
	UAM_ASSERT(std::ranges::find(discovery.diagnostics.front().workspaces, fixture.other_workspace) != discovery.diagnostics.front().workspaces.end());
	fixture.AssertRejected(fixture.owned.workspace);
	fixture.AssertRejected(fixture.other_workspace);
	std::string error;
	UAM_ASSERT(DiscoverGeminiTmpChatSources(&error).empty() && error.find("conflict") != std::string::npos);
	UAM_ASSERT(!ChatHistorySyncService().LoadLocalNativeTranscript(fixture.owned.Parent()));
#endif
}

UAM_TEST(GeminiDuplicateWorkspaceProjectsRejectExactHistory)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiTwoProjectOwnershipFixture fixture;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.owned.storage / "projects.json", Json{{"projects", {{fixture.owned.workspace, "project"}}}}.dump()));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.other_project / ".project_root", fixture.owned.workspace));
	const auto discovery = uam::gemini::DiscoverHistoryProjectsWithDiagnostics();
	UAM_ASSERT(!discovery.Fatal() && discovery.projects.size() == 2);
	UAM_ASSERT(!discovery.CanReadWorkspace(fixture.owned.workspace));
	UAM_ASSERT(discovery.DiagnosticText().find("More than one") != std::string::npos);
	fixture.AssertRejected(fixture.owned.workspace);
	fixture.AssertRejected(fixture.other_workspace);
	std::string error;
	UAM_ASSERT(DiscoverGeminiTmpChatSources(&error).empty() && error.find("More than one") != std::string::npos);
	UAM_ASSERT(!ChatHistorySyncService().LoadLocalNativeTranscript(fixture.owned.Parent()));
#endif
}

UAM_TEST(GeminiCustomHistoryCallerKeepsStandaloneAliasCompatibility)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiOwnershipFixture fixture;
	const auto custom = fixture.temp.root / "custom" / "chats";
	fs::create_directories(custom);
	const auto source = fixture.temp.root / "caller-supplied.jsonl";
	UAM_ASSERT(uam::io::WriteTextFile(source, Journal()));
	const auto alias = custom / "caller-alias.jsonl";
	std::error_code error;
	fs::create_symlink(source, alias, error);
	UAM_ASSERT(!error);
	fs::remove(fixture.storage / "projects.json");
	fs::create_directory(fixture.storage / "projects.json");
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	const auto parsed = GeminiJsonHistoryStore::ParseFile(alias, provider);
	UAM_ASSERT(parsed && parsed->native_session_id == kParent && parsed->messages.back().content == "Current answer");
	const auto standalone = GeminiJsonHistoryStore::Load(custom, provider);
	UAM_ASSERT(standalone.size() == 1 && standalone.front().native_session_id == kParent && standalone.front().messages.back().content == "Current answer");
	const auto runtime = LoadGeminiJsonHistoryForRuntime(custom, provider, {});
	UAM_ASSERT(runtime.size() == 1 && runtime.front().native_session_id == kParent && runtime.front().messages.back().content == "Current answer");
#endif
}

namespace
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	void AssertGeminiRelatedLocalImportsKeepAttestedWorkspace(bool targeted)
	{
		std::vector<std::string> misattributed_workspaces;
		for (const bool requested_is_ancestor : {true, false})
		{
			GeminiOwnershipFixture fixture;
			const auto owner = uam::paths::PathFromUtf8(fixture.workspace);
			const auto requested = requested_is_ancestor ? owner.parent_path() : owner / "nested-requested-workspace";
			fs::create_directories(requested);
			const auto requested_text = uam::paths::Utf8PathString(requested);
			UAM_ASSERT(!FolderDirectoryMatches(requested, owner));
			std::string discovery_error;
			const auto sources = DiscoverGeminiTmpChatSources(&discovery_error);
			UAM_ASSERT(discovery_error.empty() && sources.size() == 1 && sources.front().folder_directory == fixture.workspace);
			// No exact A project exists: this is the real related-directory fallback to B.
			const auto related = AppPaths::ResolveGeminiProjectTmpDir(requested);
			UAM_ASSERT(related && *related == fixture.project);
			const ChatHistorySyncService service;
			const auto attested = service.LoadNativeSessionChats(fixture.chats, ProviderProfileStore::DefaultGeminiProfile());
			UAM_ASSERT(attested.size() == 1 && attested.front().native_session_id == kParent);
			UAM_ASSERT_EQ(attested.front().workspace_directory, fixture.workspace);
			const auto owner_key = uam::chat_identity::NativeIdentityKeyForHistoryImport(attested.front());
			auto wrong_identity = attested.front(); wrong_identity.workspace_directory = requested_text;
			const auto wrong_key = uam::chat_identity::NativeIdentityKeyForHistoryImport(wrong_identity);
			UAM_ASSERT(owner_key != wrong_key);
			const auto parent_file = fixture.chats / (kParent + ".jsonl");
			const auto parent_bytes = uam::io::ReadTextFile(parent_file);
			const auto child_file = fixture.chats / kParent / (kChild + ".jsonl");
			const auto child_bytes = uam::io::ReadTextFile(child_file);
			const auto registry_bytes = uam::io::ReadTextFile(fixture.storage / "projects.json");
			const auto marker_bytes = uam::io::ReadTextFile(fixture.project / ".project_root");
			const auto target = targeted ? kParent : std::string{};
			// Exact B and equivalent B/. remain importable through this same product route.
			for (const bool equivalent : {false, true})
			{
				uam::AppState exact;
				exact.data_root = fixture.temp.root / (equivalent ? "equivalent-owner-data" : "exact-owner-data");
				exact.provider_profiles = ProviderProfileStore::BuiltInProfiles();
				ChatFolder folder; folder.id = "owner-folder"; folder.title = "Native B"; folder.execution_host_id = "local";
				folder.directory = uam::paths::Utf8PathString(equivalent ? owner / "." : owner);
				exact.folders.push_back(folder);
				UAM_ASSERT(ChatFolderStore::Save(exact.data_root, exact.folders));
				const auto imported = service.ImportAllNativeChatsToLocal(exact, false, target);
				UAM_ASSERT(imported.success && imported.imported_count == 1 && imported.total_count == 1);
				const auto saved = ChatRepository::LoadLocalChats(exact.data_root);
				UAM_ASSERT(saved.size() == 1 && saved.front().native_session_id == kParent);
				UAM_ASSERT_EQ(saved.front().provider_id, std::string("gemini-cli"));
				UAM_ASSERT_EQ(saved.front().folder_id, folder.id);
				UAM_ASSERT(FolderDirectoryMatches(uam::paths::PathFromUtf8(saved.front().workspace_directory), owner));
				UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(saved.front()), owner_key);
				const auto transcript = ChatRepository::LoadLocalChat(exact.data_root, saved.front().id, true);
				UAM_ASSERT(transcript && transcript->messages.size() == 2 && transcript->messages.back().content == "Current answer");
			}
			uam::AppState app;
			app.data_root = fixture.temp.root / "related-request-data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatFolder folder; folder.id = "requested-folder"; folder.title = "Existing A"; folder.directory = requested_text; folder.execution_host_id = "local";
			app.folders.push_back(folder);
			UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
			ChatSession existing;
			existing.id = "chat-existing-a"; existing.provider_id = "gemini-cli"; existing.native_session_id = "existing-a-native";
			existing.title = "Keep existing A"; existing.folder_id = folder.id; existing.workspace_directory = requested_text; existing.execution_host_id = "local";
			existing.messages.push_back({MessageRole::User, "Existing authored A history"});
			app.chats.push_back(existing);
			app.resolved_native_sessions_by_chat_id[existing.id] = existing.native_session_id;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, existing));
			const auto existing_file = AppPaths::UamChatFilePath(app.data_root, existing.id);
			const auto existing_bytes = uam::io::ReadTextFile(existing_file);
			UAM_ASSERT(!existing_bytes.empty());
			const auto result = service.ImportAllNativeChatsToLocal(app, false, target);
			const auto saved = ChatRepository::LoadLocalChats(app.data_root);
			// Rejection and a correctly B-owned import are both valid dispositions;
			// a display folder cannot change transcript or native identity ownership.
			UAM_ASSERT(result.imported_count >= 0 && result.imported_count <= 1);
			UAM_ASSERT(saved.size() == std::size_t(1 + result.imported_count));
			for (const auto& chat : saved)
			{
				const auto key = uam::chat_identity::NativeIdentityKeyForHistoryImport(chat);
				if (key == wrong_key) misattributed_workspaces.push_back(requested_text);
				if (chat.native_session_id != kParent) continue;
				if (key != owner_key || !FolderDirectoryMatches(uam::paths::PathFromUtf8(chat.workspace_directory), owner))
					misattributed_workspaces.push_back(chat.workspace_directory);
				const auto transcript = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
				UAM_ASSERT(transcript && transcript->messages.size() == 2 && transcript->messages.back().content == "Current answer");
			}
			UAM_ASSERT_EQ(uam::io::ReadTextFile(existing_file), existing_bytes);
			const auto retained = ChatRepository::LoadLocalChat(app.data_root, existing.id, true);
			UAM_ASSERT(retained && retained->messages.size() == 1 && retained->messages.front().content == "Existing authored A history");
			UAM_ASSERT_EQ(retained->workspace_directory, requested_text);
			UAM_ASSERT_EQ(retained->native_session_id, existing.native_session_id);
			UAM_ASSERT_EQ(retained->title, existing.title);
			const auto retained_live = std::ranges::find_if(app.chats, [&](const auto& chat) { return chat.id == existing.id; });
			UAM_ASSERT(retained_live != app.chats.end());
			UAM_ASSERT_EQ(retained_live->provider_id, existing.provider_id);
			UAM_ASSERT_EQ(retained_live->title, existing.title);
			UAM_ASSERT_EQ(retained_live->folder_id, existing.folder_id);
			UAM_ASSERT_EQ(retained_live->workspace_directory, existing.workspace_directory);
			UAM_ASSERT_EQ(retained_live->native_session_id, existing.native_session_id);
			UAM_ASSERT_EQ(retained_live->execution_host_id, existing.execution_host_id);
			UAM_ASSERT(retained_live->messages.size() == 1 && retained_live->messages.front().role == MessageRole::User);
			UAM_ASSERT_EQ(retained_live->messages.front().content, existing.messages.front().content);
			UAM_ASSERT(app.chats.size() >= 1 && app.chats.size() <= std::size_t(1 + result.imported_count));
			UAM_ASSERT(std::ranges::count_if(app.chats, [&](const auto& chat) { return chat.id == existing.id; }) == 1);
			for (const auto& chat : app.chats)
			{
				if (chat.id == existing.id) continue;
				UAM_ASSERT_EQ(chat.provider_id, std::string("gemini-cli"));
				UAM_ASSERT_EQ(chat.native_session_id, kParent);
				const auto key = uam::chat_identity::NativeIdentityKeyForHistoryImport(chat);
				if (key == wrong_key || key != owner_key || !FolderDirectoryMatches(uam::paths::PathFromUtf8(chat.workspace_directory), owner))
					misattributed_workspaces.push_back(chat.workspace_directory);
				UAM_ASSERT(std::ranges::any_of(saved, [&](const auto& persisted) { return persisted.id == chat.id; }));
			}
			const auto retained_folder = std::ranges::find_if(app.folders, [&](const auto& candidate) { return candidate.id == folder.id; });
			UAM_ASSERT(retained_folder != app.folders.end());
			UAM_ASSERT_EQ(retained_folder->title, folder.title);
			UAM_ASSERT_EQ(retained_folder->directory, folder.directory);
			UAM_ASSERT_EQ(retained_folder->execution_host_id, folder.execution_host_id);
			UAM_ASSERT(app.folders.size() >= 1 && app.folders.size() <= std::size_t(1 + result.imported_count));
			UAM_ASSERT(std::ranges::count_if(app.folders, [&](const auto& candidate) { return candidate.id == folder.id; }) == 1);
			for (const auto& candidate : app.folders)
			{
				if (candidate.id == folder.id) continue;
				UAM_ASSERT(!candidate.id.empty() && FolderDirectoryMatches(uam::paths::PathFromUtf8(candidate.directory), owner));
			}
			const auto retained_mapping = app.resolved_native_sessions_by_chat_id.find(existing.id);
			UAM_ASSERT(retained_mapping != app.resolved_native_sessions_by_chat_id.end() && retained_mapping->second == existing.native_session_id);
			UAM_ASSERT(app.resolved_native_sessions_by_chat_id.size() >= 1 && app.resolved_native_sessions_by_chat_id.size() <= std::size_t(1 + result.imported_count));
			for (const auto& [chat_id, native_id] : app.resolved_native_sessions_by_chat_id)
			{
				if (chat_id == existing.id) continue;
				UAM_ASSERT_EQ(native_id, kParent);
				const auto mapped = std::ranges::find_if(saved, [&](const auto& chat) { return chat.id == chat_id; });
				UAM_ASSERT(mapped != saved.end() && mapped->provider_id == "gemini-cli" && mapped->native_session_id == kParent);
				auto identity = *mapped; identity.native_session_id = native_id;
				const auto key = uam::chat_identity::NativeIdentityKeyForHistoryImport(identity);
				if (key == wrong_key || key != owner_key || !FolderDirectoryMatches(uam::paths::PathFromUtf8(identity.workspace_directory), owner))
					misattributed_workspaces.push_back(identity.workspace_directory);
			}
			UAM_ASSERT(!fs::exists(app.data_root / "native-import-tombstones.json"));
			UAM_ASSERT_EQ(uam::io::ReadTextFile(parent_file), parent_bytes);
			UAM_ASSERT_EQ(uam::io::ReadTextFile(child_file), child_bytes);
			UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.storage / "projects.json"), registry_bytes);
			UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.project / ".project_root"), marker_bytes);
		}
		// Defer this assertion so both ancestor/descendant product traces run in RED.
		UAM_ASSERT(misattributed_workspaces.empty());
	}
#endif
}

UAM_TEST(GeminiTargetedLocalImportCannotRelabelRelatedNativeWorkspace)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	AssertGeminiRelatedLocalImportsKeepAttestedWorkspace(true);
#endif
}

UAM_TEST(GeminiBulkLocalImportCannotRelabelRelatedNativeWorkspace)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	AssertGeminiRelatedLocalImportsKeepAttestedWorkspace(false);
#endif
}

UAM_TEST(GeminiDiscoveryImportsKeepUnrelatedNativeChatAndDraftUnlinked)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	for (const bool targeted : {false, true})
	{
		GeminiOwnershipFixture fixture;
		const ChatHistorySyncService service;
		uam::AppState app; app.data_root = fixture.temp.root / "discovery-preservation"; app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		const auto unrelated = fixture.temp.root / "unrelated-workspace"; fs::create_directories(unrelated);
		ChatFolder owner_folder; owner_folder.id = "owner-b"; owner_folder.title = "Owner B"; owner_folder.directory = fixture.workspace; owner_folder.execution_host_id = "local";
		ChatFolder unrelated_folder; unrelated_folder.id = "unrelated-a"; unrelated_folder.title = "Unrelated A"; unrelated_folder.directory = uam::paths::Utf8PathString(unrelated); unrelated_folder.execution_host_id = "local";
		app.folders = {unrelated_folder, owner_folder}; UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
		const auto folders_before = ChatFolderStore::Serialize(app.folders);
		ChatSession existing; existing.id = "chat-unrelated-a"; existing.provider_id = "gemini-cli"; existing.native_session_id = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
		existing.title = "Retained unrelated A"; existing.workspace_directory = unrelated_folder.directory; existing.folder_id = unrelated_folder.id; existing.execution_host_id = "local";
		existing.messages.push_back({MessageRole::User, "Unrelated authored A history"});
		ChatSession draft; draft.id = "chat-unlinked-draft"; draft.provider_id = "gemini-cli"; draft.workspace_directory = fixture.workspace; draft.folder_id = owner_folder.id; draft.execution_host_id = "local";
		draft.title = "Unlinked local intent"; draft.messages.push_back({MessageRole::User, "Native question"});
		const auto native = service.LoadNativeSessionChats(fixture.chats, ProviderProfileStore::DefaultGeminiProfile());
		UAM_ASSERT(native.size() == 1);
		const auto inferred = NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(draft, native);
		UAM_ASSERT(inferred && *inferred == kParent); // Ensure old link inference would be exercised.
		app.chats = {existing, draft}; app.resolved_native_sessions_by_chat_id[existing.id] = existing.native_session_id;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, existing) && ChatRepository::SaveChat(app.data_root, draft));
		const auto existing_file = AppPaths::UamChatFilePath(app.data_root, existing.id); const auto existing_bytes = uam::io::ReadTextFile(existing_file);
		const auto draft_file = AppPaths::UamChatFilePath(app.data_root, draft.id); const auto draft_bytes = uam::io::ReadTextFile(draft_file);
		const auto source_file = fixture.chats / (kParent + ".jsonl"); const auto source_bytes = uam::io::ReadTextFile(source_file);
		const auto result = service.ImportAllNativeChatsByDiscovery(app, false, targeted ? kParent : "");
		UAM_ASSERT(result.success && result.imported_count == 1);
		const auto saved = ChatRepository::LoadLocalChats(app.data_root); UAM_ASSERT_EQ(saved.size(), std::size_t(3));
		const auto imported = std::ranges::find_if(saved, [&](const auto& chat) { return chat.native_session_id == kParent; });
		UAM_ASSERT(imported != saved.end() && imported->workspace_directory == fixture.workspace && imported->folder_id == owner_folder.id);
		UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(*imported), uam::chat_identity::NativeIdentityKeyForHistoryImport(native.front()));
		const auto retained_live = std::ranges::find_if(app.chats, [&](const auto& chat) { return chat.id == existing.id; });
		UAM_ASSERT(retained_live != app.chats.end() && retained_live->messages.size() == 1 && retained_live->messages.front().content == "Unrelated authored A history");
		UAM_ASSERT_EQ(retained_live->native_session_id, existing.native_session_id); UAM_ASSERT_EQ(retained_live->workspace_directory, existing.workspace_directory);
		const auto draft_live = std::ranges::find_if(app.chats, [&](const auto& chat) { return chat.id == draft.id; });
		UAM_ASSERT(draft_live != app.chats.end() && draft_live->native_session_id.empty() && draft_live->messages.size() == 1 && draft_live->messages.front().content == "Native question");
		UAM_ASSERT(!app.resolved_native_sessions_by_chat_id.contains(draft.id));
		UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.at(existing.id), existing.native_session_id);
		UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), folders_before);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(existing_file), existing_bytes); UAM_ASSERT_EQ(uam::io::ReadTextFile(draft_file), draft_bytes);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(source_file), source_bytes);
	}
#endif
}

UAM_TEST(GeminiPartialImportsRespectScopedTombstonesAndRetainCatalogue)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	for (const bool by_discovery : {false, true})
	for (const bool targeted : {false, true})
	{
		GeminiOwnershipFixture fixture;
		const ChatHistorySyncService service;
		uam::AppState app; app.data_root = fixture.temp.root / "tombstone-preservation"; app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatFolder folder; folder.id = "owner-folder"; folder.directory = fixture.workspace; folder.execution_host_id = "local"; app.folders.push_back(folder);
		UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
		const auto unrelated = fixture.temp.root / "unrelated"; fs::create_directories(unrelated);
		ChatSession existing = fixture.Parent(); existing.id = "chat-same-native-id-other-owner"; existing.workspace_directory = uam::paths::Utf8PathString(unrelated);
		existing.messages.push_back({MessageRole::User, "Keep this different workspace's native identity"}); app.chats.push_back(existing);
		app.resolved_native_sessions_by_chat_id[existing.id] = kParent;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, existing));
		const auto existing_file = AppPaths::UamChatFilePath(app.data_root, existing.id); const auto existing_bytes = uam::io::ReadTextFile(existing_file);
		const auto native = service.LoadNativeSessionChats(fixture.chats, ProviderProfileStore::DefaultGeminiProfile()); UAM_ASSERT(native.size() == 1);
		UAM_ASSERT(uam::chat_identity::NativeIdentityKeyForHistoryImport(existing) != uam::chat_identity::NativeIdentityKeyForHistoryImport(native.front()));
		std::vector<std::string> keys; UAM_ASSERT(service.AddNativeImportTombstones(app.data_root, native, keys) && keys.size() == 1);
		const auto tombstone_file = app.data_root / "native-import-tombstones.json"; const auto tombstone_bytes = uam::io::ReadTextFile(tombstone_file);
		const auto source_file = fixture.chats / (kParent + ".jsonl"); const auto source_bytes = uam::io::ReadTextFile(source_file);
		const auto target = targeted ? kParent : std::string{};
		const auto result = by_discovery ? service.ImportAllNativeChatsByDiscovery(app, false, target) : service.ImportAllNativeChatsToLocal(app, false, target);
		UAM_ASSERT(result.success && result.imported_count == 0);
		const auto saved = ChatRepository::LoadLocalChats(app.data_root); UAM_ASSERT(saved.size() == 1 && saved.front().id == existing.id);
		const auto retained = std::ranges::find_if(app.chats, [&](const auto& chat) { return chat.id == existing.id; });
		UAM_ASSERT(retained != app.chats.end() && retained->messages.size() == 1 && retained->workspace_directory == existing.workspace_directory && retained->native_session_id == kParent);
		UAM_ASSERT_EQ(retained->messages.front().content, existing.messages.front().content);
		UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.at(existing.id), kParent);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(existing_file), existing_bytes); UAM_ASSERT_EQ(uam::io::ReadTextFile(tombstone_file), tombstone_bytes);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(source_file), source_bytes);
	}
#endif
}

UAM_TEST(GeminiPartialImportsPreserveUnsavedLiveTailAndLocalMetadata)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	for (const bool by_discovery : {false, true})
	for (const bool pending_save : {false, true})
	{
		GeminiOwnershipFixture fixture;
		const ChatHistorySyncService service;
		uam::AppState app; app.data_root = fixture.temp.root / "live-tail-preservation"; app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatFolder folder; folder.id = "owner-folder"; folder.directory = fixture.workspace; folder.execution_host_id = "local"; app.folders.push_back(folder);
		UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
		const auto native = service.LoadNativeSessionChats(fixture.chats, ProviderProfileStore::DefaultGeminiProfile()); UAM_ASSERT(native.size() == 1);
		auto local = native.front(); local.id = "chat-live-local-owner"; local.folder_id = folder.id; local.title = "Keep local title"; local.pinned = true;
		local.messages.push_back({MessageRole::User, "Saved authored tail"}); local.updated_at = "2026-10-16T12:10:00Z";
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, local));
		const auto local_file = AppPaths::UamChatFilePath(app.data_root, local.id); const auto local_bytes = uam::io::ReadTextFile(local_file);
		local.messages.push_back({MessageRole::User, "Unsaved live tail"}); app.chats.push_back(local); app.resolved_native_sessions_by_chat_id[local.id] = kParent;
		if (pending_save) app.pending_chat_save_at_by_chat_id[local.id] = 1.0;
		const auto source_file = fixture.chats / (kParent + ".jsonl"); const auto source_bytes = uam::io::ReadTextFile(source_file);
		const auto result = by_discovery ? service.ImportAllNativeChatsByDiscovery(app, false, kParent) : service.ImportAllNativeChatsToLocal(app, false, kParent);
		UAM_ASSERT(result.success && result.imported_count == 0);
		const auto retained = std::ranges::find_if(app.chats, [&](const auto& chat) { return chat.id == local.id; });
		UAM_ASSERT(retained != app.chats.end() && retained->messages.size() == 4 && retained->messages.back().content == "Unsaved live tail");
		UAM_ASSERT_EQ(retained->title, local.title); UAM_ASSERT(retained->pinned);
		UAM_ASSERT_EQ(retained->workspace_directory, fixture.workspace); UAM_ASSERT_EQ(retained->native_session_id, kParent);
		UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.at(local.id), kParent);
		UAM_ASSERT_EQ(app.pending_chat_save_at_by_chat_id.contains(local.id), pending_save);
		const auto persisted = ChatRepository::LoadLocalChat(app.data_root, local.id, true);
		UAM_ASSERT(persisted && persisted->messages.size() == 3 && persisted->messages.back().content == "Saved authored tail" && persisted->pinned && persisted->title == local.title);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(local_file), local_bytes); UAM_ASSERT_EQ(uam::io::ReadTextFile(source_file), source_bytes);
	}
#endif
}

namespace
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	struct GeminiTerminalSyncConsumerFixture
	{
		GeminiOwnershipFixture owned;
		uam::AppState app;
		ChatSession local;
		ChatSession unrelated;
		std::vector<std::pair<fs::path, std::string>> unchanged_files;
		std::string local_bytes;
		std::string folders_before;

		explicit GeminiTerminalSyncConsumerFixture(std::size_t saved_count)
		{
			app.data_root = owned.temp.root / "terminal-sync-consumer";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatFolder folder; folder.id = "owner-folder"; folder.title = "Native owner"; folder.directory = owned.workspace; folder.execution_host_id = "local";
			app.folders.push_back(folder);
			UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
			folders_before = ChatFolderStore::Serialize(app.folders);
			const auto native = ChatHistorySyncService().LoadNativeSessionChats(owned.chats, ProviderProfileStore::DefaultGeminiProfile());
			UAM_ASSERT(native.size() == 1 && native.front().messages.size() == 2);
			UAM_ASSERT_EQ(native.front().native_session_id, kParent);
			UAM_ASSERT_EQ(native.front().workspace_directory, owned.workspace);
			UAM_ASSERT_EQ(native.front().messages.back().content, std::string("Current answer"));
			local = native.front(); local.id = "chat-terminal-sync-owner"; local.folder_id = folder.id; local.execution_host_id = "local";
			local.title = "Keep local title"; local.pinned = true;
			UAM_ASSERT(saved_count == 1 || saved_count == 3);
			if (saved_count == 1) local.messages.resize(1);
			else local.messages.push_back({MessageRole::User, "Saved authored tail"});
			local.persisted_message_count = local.messages.size();
			local.updated_at = saved_count == 1 ? "2026-10-16T12:00:00Z" : "2026-10-16T12:10:00Z";
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, local));
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, local.id, true);
			UAM_ASSERT(saved && saved->messages_loaded && saved->messages.size() == saved_count);
			local = *saved; // Live and disk agree exactly before either regression's trigger.
			local_bytes = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, local.id));
			const auto other_workspace = owned.temp.root / "unrelated-workspace";
			fs::create_directories(other_workspace);
			unrelated.id = "chat-terminal-sync-unrelated"; unrelated.provider_id = "gemini-cli";
			unrelated.native_session_id = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
			unrelated.workspace_directory = uam::paths::Utf8PathString(other_workspace); unrelated.execution_host_id = "local";
			unrelated.title = "Keep unrelated history"; unrelated.pinned = true;
			unrelated.messages.push_back({MessageRole::User, "Unrelated authored history"});
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, unrelated));
			app.chats = {unrelated, local};
			app.resolved_native_sessions_by_chat_id[local.id] = kParent;
			app.resolved_native_sessions_by_chat_id[unrelated.id] = unrelated.native_session_id;
			ChatDomainService().SelectChatById(app, local.id);
			app.composer_text = "Keep selected draft";
			for (const auto& file : {owned.chats / (kParent + ".jsonl"), owned.chats / kParent / (kChild + ".jsonl"),
			    owned.storage / "projects.json", owned.project / ".project_root", AppPaths::UamChatFilePath(app.data_root, unrelated.id)})
			{
				const auto bytes = uam::io::ReadTextFile(file);
				UAM_ASSERT(!bytes.empty());
				unchanged_files.emplace_back(file, bytes);
			}
			UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty() && app.acp_sessions.empty() && app.worktree_operation_chat_ids.empty() && app.cli_terminals.empty());
		}

		void AssertIdentityAndUnrelatedHistoryPreserved() const
		{
			UAM_ASSERT_EQ(app.chats.size(), std::size_t(2));
			UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), local.id);
			UAM_ASSERT_EQ(app.composer_text, std::string("Keep selected draft"));
			const auto retained = std::ranges::find_if(app.chats, [&](const auto& chat) { return chat.id == local.id; });
			UAM_ASSERT(retained != app.chats.end());
			UAM_ASSERT_EQ(retained->provider_id, local.provider_id); UAM_ASSERT_EQ(retained->native_session_id, kParent);
			UAM_ASSERT_EQ(retained->workspace_directory, owned.workspace); UAM_ASSERT_EQ(retained->execution_host_id, std::string("local"));
			UAM_ASSERT_EQ(retained->folder_id, local.folder_id); UAM_ASSERT_EQ(retained->title, local.title); UAM_ASSERT_EQ(retained->pinned, local.pinned);
			UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(*retained), uam::chat_identity::NativeIdentityKeyForHistoryImport(local));
			const auto other = std::ranges::find_if(app.chats, [&](const auto& chat) { return chat.id == unrelated.id; });
			UAM_ASSERT(other != app.chats.end());
			UAM_ASSERT_EQ(other->title, unrelated.title); UAM_ASSERT(other->pinned);
			UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(*other), uam::chat_identity::NativeIdentityKeyForHistoryImport(unrelated));
			auto hydrated_other = *other; // An unrelated sidebar row may legitimately stay lazy.
			UAM_ASSERT(ChatRepository::HydrateChatMessages(app.data_root, hydrated_other));
			UAM_ASSERT(hydrated_other.messages.size() == 1 && hydrated_other.messages.front().content == unrelated.messages.front().content);
			UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.size(), std::size_t(2));
			UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.at(local.id), kParent);
			UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.at(unrelated.id), unrelated.native_session_id);
			UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), folders_before);
			for (const auto& [file, bytes] : unchanged_files) UAM_ASSERT_EQ(uam::io::ReadTextFile(file), bytes);
		}
	};
#endif
}


namespace
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	void AwaitGeminiTerminalSavedAndLiveCompletion(GeminiTerminalSyncConsumerFixture& fixture, bool poll_terminals = false)
	{
		const auto complete = [&]
		{
			const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.local.id, true);
			const auto* live = ChatDomainService().SelectedChat(fixture.app);
			return !ChatHistorySyncService().TerminalNativeMediaPending(fixture.app) && saved && saved->messages_loaded && saved->messages.size() == 2 &&
			       saved->messages.front().content == "Native question" && saved->messages.back().content == "Current answer" &&
			       live != nullptr && live->messages_loaded && live->messages == saved->messages;
		};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (!complete() && std::chrono::steady_clock::now() < deadline)
		{
			// Direct still-running metadata fixtures drain only the actual history
			// service. Real EOF consumers use public app polling after Stop/handle checks.
			if (poll_terminals) (void)uam::PollAllCliTerminals(nullptr, fixture.app, false);
			else (void)ChatHistorySyncService().PollTerminalNativeMedia(fixture.app);
			std::this_thread::yield();
		}
		UAM_ASSERT(complete());
		UAM_ASSERT(fixture.app.native_chat_refresh_error_status.empty() && fixture.app.native_chat_refresh_error_target_id.empty());
	}
#endif
}

UAM_TEST(GeminiTerminalFinalSyncPersistsCompletedJournalWhileStillRunning)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	// Run the stopped control first; stop/exit callers actually sync before clearing running.
	for (const bool running : {false, true})
	{
		GeminiTerminalSyncConsumerFixture fixture(1);
		auto& app = fixture.app;
		auto terminal = std::make_unique<uam::CliTerminalState>();
		terminal->terminal_id = "fixture-terminal"; terminal->frontend_chat_id = fixture.local.id; terminal->attached_chat_id = fixture.local.id;
		terminal->attached_session_id = kParent; terminal->running = running;
		terminal->turn_state = uam::CliTerminalTurnState::Idle;
		terminal->lifecycle_state = running ? uam::CliTerminalLifecycleState::Idle : uam::CliTerminalLifecycleState::Stopped;
		app.cli_terminals.push_back(std::move(terminal)); // Default platform handles remain invalid; no process is launched.
		UAM_ASSERT_EQ(uam::CliTerminalSyncTargetId(*app.cli_terminals.front()), kParent);
		UAM_ASSERT_EQ(uam::ChatHasActiveCliTerminal(app, fixture.local.id), running);
		UAM_ASSERT(!uam::ChatHasBusyCliTerminal(app, fixture.local.id));
		UAM_ASSERT(!app.cli_terminals.front()->generation_in_progress);
		// This is the public consumer used by close, StopAll and FastStopForExit.
		uam::SyncCliTerminalToNativeHistory(app, *app.cli_terminals.front());
		AwaitGeminiTerminalSavedAndLiveCompletion(fixture);
		fixture.AssertIdentityAndUnrelatedHistoryPreserved();
		UAM_ASSERT_EQ(app.cli_terminals.front()->running, running);
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		UAM_ASSERT(saved && saved->messages.size() == 2);
		UAM_ASSERT_EQ(saved->messages.front().content, std::string("Native question"));
		UAM_ASSERT_EQ(saved->messages.back().content, std::string("Current answer"));
		UAM_ASSERT_EQ(saved->messages.back().thoughts, std::string("Native reasoning"));
		UAM_ASSERT_EQ(saved->messages.back().model_id, std::string("native-model"));
		UAM_ASSERT(saved->messages.back().tool_calls.size() == 1);
		UAM_ASSERT_EQ(saved->messages.back().tool_calls.front().result_text, std::string("Current tool result"));
		UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(*saved), uam::chat_identity::NativeIdentityKeyForHistoryImport(fixture.local));
		const auto* selected = ChatDomainService().SelectedChat(app);
		UAM_ASSERT(selected != nullptr && selected->messages_loaded && selected->messages.size() == 2);
		UAM_ASSERT_EQ(selected->messages.back().content, saved->messages.back().content);
	}
#endif
}

UAM_TEST(GeminiTerminalSyncReloadPreservesSelectedUnsavedLiveTail)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	// The explicit pending-save control precedes the unflagged live/disk mismatch.
	for (const bool pending_save : {true, false})
	{
		GeminiTerminalSyncConsumerFixture fixture(3);
		auto& app = fixture.app;
		auto* selected = ChatDomainService().SelectedChat(app);
		UAM_ASSERT(selected != nullptr && selected->messages.size() == 3);
		selected->messages.push_back({MessageRole::User, "Unsaved live tail"});
		if (pending_save) app.pending_chat_save_at_by_chat_id[fixture.local.id] = 1.0;
		UAM_ASSERT(selected->messages_loaded && selected->messages.size() == 4);
		UAM_ASSERT_EQ(app.pending_chat_save_at_by_chat_id.contains(fixture.local.id), pending_save);
		UAM_ASSERT(app.cli_terminals.empty() && app.acp_sessions.empty() && app.worktree_operation_chat_ids.empty());
		// Also the natural-exit caller's public sync consumer: import, sidebar reload, selection hydration.
		UAM_ASSERT(uam::SyncChatsFromNative(app, kParent, true));
		fixture.AssertIdentityAndUnrelatedHistoryPreserved();
		UAM_ASSERT_EQ(app.pending_chat_save_at_by_chat_id.contains(fixture.local.id), pending_save);
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		UAM_ASSERT(saved && saved->messages.size() == 3 && saved->messages.back().content == "Saved authored tail");
		UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), fixture.local_bytes);
		selected = ChatDomainService().SelectedChat(app); // The wrapper may have replaced/reordered the catalogue.
		UAM_ASSERT(selected != nullptr && selected->messages_loaded && selected->messages.size() == 4);
		UAM_ASSERT_EQ(selected->messages[0].content, std::string("Native question"));
		UAM_ASSERT_EQ(selected->messages[1].content, std::string("Current answer"));
		UAM_ASSERT_EQ(selected->messages[2].content, std::string("Saved authored tail"));
		UAM_ASSERT_EQ(selected->messages[3].content, std::string("Unsaved live tail"));
	}
#endif
}

// R6 regression-only append. Keep the original 39 cases above byte-for-byte intact.
#if defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	struct GeminiR6TerminalCleanup
	{
		uam::AppState& app;
		~GeminiR6TerminalCleanup()
		{
			for (auto& terminal : app.cli_terminals)
				if (terminal) uam::StopCliTerminal(*terminal, false, uam::CliTerminalStopMode::FastExit);
		}
	};

	uam::CliTerminalState& AddGeminiR6OwnedTerminal(GeminiTerminalSyncConsumerFixture& fixture, bool busy = true)
	{
		auto terminal = std::make_unique<uam::CliTerminalState>();
		terminal->terminal_id = "r6-owned-terminal";
		terminal->frontend_chat_id = fixture.local.id;
		terminal->attached_chat_id = fixture.local.id;
		terminal->attached_session_id = kParent;
		terminal->native_activity_provider_id = "gemini-cli";
		terminal->running = true;
		terminal->startup_time_s = uam::GetAppTimeSeconds();
		terminal->lifecycle_state = busy ? uam::CliTerminalLifecycleState::Busy : uam::CliTerminalLifecycleState::Idle;
		terminal->turn_state = busy ? uam::CliTerminalTurnState::Busy : uam::CliTerminalTurnState::Idle;
		terminal->generation_in_progress = busy;
		fixture.app.cli_terminals.push_back(std::move(terminal));
		return *fixture.app.cli_terminals.back();
	}

	void AssertGeminiR6LiveRow(const uam::AppState& app, const ChatSession& expected)
	{
		const auto* live = ChatDomainService().FindChatById(app, expected.id);
		UAM_ASSERT(live != nullptr && live->messages_loaded);
		UAM_ASSERT(live->messages == expected.messages); // Includes attachments, reasoning, tools and interruption state.
		UAM_ASSERT_EQ(live->id, expected.id);
		UAM_ASSERT_EQ(live->provider_id, expected.provider_id);
		UAM_ASSERT_EQ(live->native_session_id, expected.native_session_id);
		UAM_ASSERT_EQ(live->workspace_directory, expected.workspace_directory);
		UAM_ASSERT_EQ(live->execution_host_id, expected.execution_host_id);
		UAM_ASSERT_EQ(live->folder_id, expected.folder_id);
		UAM_ASSERT_EQ(live->title, expected.title);
		UAM_ASSERT_EQ(live->pinned, expected.pinned);
		UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(*live), uam::chat_identity::NativeIdentityKeyForHistoryImport(expected));
	}

	void AssertGeminiR6ContextAndSources(const GeminiTerminalSyncConsumerFixture& fixture)
	{
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t(2));
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), fixture.local.id);
		UAM_ASSERT_EQ(fixture.app.composer_text, std::string("Keep selected draft"));
		UAM_ASSERT_EQ(ChatFolderStore::Serialize(fixture.app.folders), fixture.folders_before);
		UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.worktree_operation_chat_ids.empty());
		UAM_ASSERT_EQ(ChatRepository::LoadLocalChats(fixture.app.data_root).size(), std::size_t(2));
		for (const auto& [file, bytes] : fixture.unchanged_files) UAM_ASSERT_EQ(uam::io::ReadTextFile(file), bytes);
	}

	void AssertGeminiR6PrimaryDeferred(const GeminiTerminalSyncConsumerFixture& fixture)
	{
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.local.id, true);
		UAM_ASSERT(saved && saved->messages.size() == 1 && saved->messages.front().content == "Native question");
		UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.local.id)), fixture.local_bytes);
	}

	void AssertGeminiR6CompletePublication(const GeminiTerminalSyncConsumerFixture& fixture)
	{
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.local.id, true);
		UAM_ASSERT(saved && saved->messages.size() == 2);
		UAM_ASSERT_EQ(saved->messages.front().content, std::string("Native question"));
		const auto& answer = saved->messages.back();
		UAM_ASSERT(saved->messages.front().role == MessageRole::User && answer.role == MessageRole::Assistant);
		UAM_ASSERT_EQ(answer.content, std::string("Current answer"));
		UAM_ASSERT_EQ(answer.thoughts, std::string("Native reasoning"));
		UAM_ASSERT_EQ(answer.model_id, std::string("native-model"));
		UAM_ASSERT(answer.tool_calls.size() == 1);
		UAM_ASSERT_EQ(answer.tool_calls.front().id, std::string("t"));
		UAM_ASSERT_EQ(answer.tool_calls.front().name, std::string("read_file"));
		UAM_ASSERT_EQ(answer.tool_calls.front().result_text, std::string("Current tool result"));
		UAM_ASSERT_EQ(answer.tool_calls.front().status, std::string("success"));
		UAM_ASSERT_EQ(saved->id, fixture.local.id);
		UAM_ASSERT_EQ(saved->provider_id, fixture.local.provider_id);
		UAM_ASSERT_EQ(saved->native_session_id, kParent);
		UAM_ASSERT_EQ(saved->workspace_directory, fixture.owned.workspace);
		UAM_ASSERT_EQ(saved->execution_host_id, std::string("local"));
		UAM_ASSERT_EQ(saved->folder_id, fixture.local.folder_id);
		UAM_ASSERT_EQ(saved->title, fixture.local.title);
		UAM_ASSERT_EQ(saved->pinned, fixture.local.pinned);
		UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(*saved), uam::chat_identity::NativeIdentityKeyForHistoryImport(fixture.local));
		AssertGeminiR6LiveRow(fixture.app, *saved);
		const auto* selected = ChatDomainService().SelectedChat(fixture.app);
		UAM_ASSERT(selected != nullptr && selected->id == saved->id && selected->messages == saved->messages);
	}

#if defined(_WIN32) || defined(__APPLE__)
	void GiveGeminiR6TerminalClosedWritePipe(uam::CliTerminalState& terminal)
	{
		// The caller's scope guard already owns cleanup through StopCliTerminal.
#if defined(_WIN32)
		HANDLE reader = INVALID_HANDLE_VALUE, writer = INVALID_HANDLE_VALUE;
		UAM_ASSERT(CreatePipe(&reader, &writer, nullptr, 0));
		terminal.pipe_output = reader;
		terminal.pipe_input = writer;
		UAM_ASSERT(CloseHandle(writer));
		terminal.pipe_input = INVALID_HANDLE_VALUE;
		DWORD flags = 0;
		UAM_ASSERT(GetHandleInformation(terminal.pipe_output, &flags));
		UAM_ASSERT(GetFileType(terminal.pipe_output) == FILE_TYPE_PIPE);
#else
		int descriptors[2] = {-1, -1};
		UAM_ASSERT(pipe(descriptors) == 0);
		terminal.master_fd = descriptors[0];
		const int closed = close(descriptors[1]);
		UAM_ASSERT(closed == 0);
		const int flags = fcntl(terminal.master_fd, F_GETFL);
		UAM_ASSERT(flags >= 0 && (flags & O_ACCMODE) == O_RDONLY);
#endif
		// This queries handle admission only: do not consume EOF before public PollCliTerminal.
		UAM_ASSERT(PlatformServicesFactory::Instance().terminal_runtime.HasReadableTerminalOutputHandle(terminal));
	}
#endif
#endif
}

UAM_TEST(GeminiTerminalPollEofPublishesFinalJournalInsideRefreshInterval)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(_WIN32) || defined(__APPLE__))
	GeminiTerminalSyncConsumerFixture fixture(1);
	auto& app = fixture.app;
	GeminiR6TerminalCleanup cleanup{app};
	auto& terminal = AddGeminiR6OwnedTerminal(fixture);
	GiveGeminiR6TerminalClosedWritePipe(terminal);
	UAM_ASSERT(app.cli_terminals.size() == 1 && app.cli_terminals.front().get() == &terminal);
	UAM_ASSERT(uam::ChatHasBusyCliTerminal(app, fixture.local.id) && terminal.running && terminal.generation_in_progress);
	UAM_ASSERT(terminal.lifecycle_state == uam::CliTerminalLifecycleState::Busy && terminal.turn_state == uam::CliTerminalTurnState::Busy);
	UAM_ASSERT_EQ(terminal.frontend_chat_id, fixture.local.id);
	UAM_ASSERT_EQ(terminal.attached_chat_id, fixture.local.id);
	UAM_ASSERT_EQ(terminal.attached_session_id, kParent);
	AssertGeminiR6PrimaryDeferred(fixture);
	const auto mappings = app.resolved_native_sessions_by_chat_id;
	terminal.last_sync_time_s = uam::GetAppTimeSeconds();
	const double last_sync = terminal.last_sync_time_s;
	UAM_ASSERT(uam::GetAppTimeSeconds() - last_sync < uam::kCliNativeHistoryRefreshIntervalSeconds);
	// The real platform reader sees EOF here, before the later process-exit probe.
	UAM_ASSERT(uam::PollCliTerminal(nullptr, app, terminal, true));
	UAM_ASSERT(!terminal.running && !terminal.should_launch && !terminal.generation_in_progress);
	UAM_ASSERT(terminal.lifecycle_state == uam::CliTerminalLifecycleState::Stopped && terminal.turn_state == uam::CliTerminalTurnState::Idle);
	UAM_ASSERT(!PlatformServicesFactory::Instance().terminal_runtime.HasReadableTerminalOutputHandle(terminal));
	UAM_ASSERT_EQ(terminal.last_error, std::string("Provider terminal exited."));
	UAM_ASSERT_EQ(terminal.last_sync_time_s, last_sync);
	UAM_ASSERT(app.native_chat_load_tasks.empty()); // No periodic/async refresh can explain this publication.
	UAM_ASSERT(!uam::PollCliTerminal(nullptr, app, terminal, true)); // Stopped admission cannot rescue a missed final sync.
	AwaitGeminiTerminalSavedAndLiveCompletion(fixture, true);
	AssertGeminiR6ContextAndSources(fixture);
	AssertGeminiR6LiveRow(app, fixture.unrelated);
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
	AssertGeminiR6CompletePublication(fixture); // Intended R6 RED: primary must contain both messages.
#endif
}

UAM_TEST(GeminiTerminalGenericRefreshDefersUntilVerifiedFinalConsumer)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	for (const bool busy : {false, true})
	{
		GeminiTerminalSyncConsumerFixture fixture(1);
		auto& app = fixture.app;
		GeminiR6TerminalCleanup cleanup{app};
		auto& terminal = AddGeminiR6OwnedTerminal(fixture, busy);
		const auto mappings = app.resolved_native_sessions_by_chat_id;
		UAM_ASSERT(uam::ChatHasActiveCliTerminal(app, fixture.local.id));
		UAM_ASSERT_EQ(uam::ChatHasBusyCliTerminal(app, fixture.local.id), busy);
		const auto direct = ChatHistorySyncService().ImportAllNativeChatsToLocal(app, false, kParent);
		UAM_ASSERT(direct.success && direct.imported_count == 0 && direct.published_chat_ids.empty());
		AssertGeminiR6PrimaryDeferred(fixture);
		AssertGeminiR6LiveRow(app, fixture.local);
		AssertGeminiR6LiveRow(app, fixture.unrelated);
		AssertGeminiR6ContextAndSources(fixture);
		UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
		UAM_ASSERT(uam::SyncChatsFromNative(app, kParent, true));
		AssertGeminiR6PrimaryDeferred(fixture);
		AssertGeminiR6LiveRow(app, fixture.local);
		AssertGeminiR6LiveRow(app, fixture.unrelated);
		AssertGeminiR6ContextAndSources(fixture);
		UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
		UAM_ASSERT(terminal.running && terminal.generation_in_progress == busy);
		// Identical running state; only the explicit app-owned final witness changes the outcome.
		(void)uam::SyncChatsFromFinalCliTerminal(app, terminal);
		AwaitGeminiTerminalSavedAndLiveCompletion(fixture);
		AssertGeminiR6CompletePublication(fixture);
		AssertGeminiR6LiveRow(app, fixture.unrelated);
		AssertGeminiR6ContextAndSources(fixture);
		UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
		UAM_ASSERT(terminal.running && terminal.generation_in_progress == busy);
	}
#endif
}

UAM_TEST(GeminiTerminalFinalConsumerRejectsUnverifiedOwnershipWithoutMutation)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	enum class InvalidWitness { Detached, FrontendMismatch, AttachedMismatch, NativeMismatch, StaleMapping, Ambiguous, ForeignProvider, CompetingTerminal };
	for (const auto invalid : {InvalidWitness::Detached, InvalidWitness::FrontendMismatch, InvalidWitness::AttachedMismatch,
	    InvalidWitness::NativeMismatch, InvalidWitness::StaleMapping, InvalidWitness::Ambiguous, InvalidWitness::ForeignProvider, InvalidWitness::CompetingTerminal})
	{
		GeminiTerminalSyncConsumerFixture fixture(1);
		auto& app = fixture.app;
		GeminiR6TerminalCleanup cleanup{app};
		auto& terminal = AddGeminiR6OwnedTerminal(fixture);
		uam::CliTerminalState detached;
		detached.terminal_id = terminal.terminal_id; detached.frontend_chat_id = terminal.frontend_chat_id;
		detached.attached_chat_id = terminal.attached_chat_id; detached.attached_session_id = terminal.attached_session_id;
		detached.native_activity_provider_id = terminal.native_activity_provider_id; detached.startup_time_s = terminal.startup_time_s;
		detached.running = true; detached.generation_in_progress = true;
		detached.lifecycle_state = uam::CliTerminalLifecycleState::Busy; detached.turn_state = uam::CliTerminalTurnState::Busy;
		const uam::CliTerminalState* witness = &terminal;
		switch (invalid)
		{
		case InvalidWitness::Detached: witness = &detached; break;
		case InvalidWitness::FrontendMismatch: terminal.frontend_chat_id = fixture.unrelated.id; break;
		case InvalidWitness::AttachedMismatch: terminal.attached_chat_id = fixture.unrelated.id; break;
		case InvalidWitness::NativeMismatch: terminal.attached_session_id = fixture.unrelated.native_session_id; break;
		case InvalidWitness::StaleMapping: app.resolved_native_sessions_by_chat_id[fixture.local.id] = fixture.unrelated.native_session_id; break;
		case InvalidWitness::Ambiguous: terminal.native_session_discovery_ambiguous = true; break;
		case InvalidWitness::ForeignProvider: terminal.native_activity_provider_id = "codex-cli"; break;
		case InvalidWitness::CompetingTerminal:
		{
			auto& competitor = AddGeminiR6OwnedTerminal(fixture);
			competitor.terminal_id = "r6-competing-terminal";
			break;
		}
		}
		const auto mappings = app.resolved_native_sessions_by_chat_id;
		std::vector<std::string> metadata;
		for (const auto& chat : app.chats) metadata.push_back(ChatRepository::ChatMetadataFingerprint(app.data_root, chat));
		const auto frontend = terminal.frontend_chat_id, attached = terminal.attached_chat_id, native = terminal.attached_session_id;
		UAM_ASSERT(!uam::SyncChatsFromFinalCliTerminal(app, *witness));
		AssertGeminiR6PrimaryDeferred(fixture);
		AssertGeminiR6LiveRow(app, fixture.local);
		AssertGeminiR6LiveRow(app, fixture.unrelated);
		AssertGeminiR6ContextAndSources(fixture);
		UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
		for (std::size_t index = 0; index < app.chats.size(); ++index)
			UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, app.chats[index]), metadata[index]);
		UAM_ASSERT_EQ(terminal.frontend_chat_id, frontend); UAM_ASSERT_EQ(terminal.attached_chat_id, attached); UAM_ASSERT_EQ(terminal.attached_session_id, native);
		UAM_ASSERT(terminal.running && terminal.generation_in_progress && terminal.lifecycle_state == uam::CliTerminalLifecycleState::Busy);
		UAM_ASSERT_EQ(terminal.native_session_discovery_ambiguous, invalid == InvalidWitness::Ambiguous);
	}
#endif
}

UAM_TEST(GeminiTerminalFinalConsumerScopesSameNativeIdToWorkspace)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiTerminalSyncConsumerFixture fixture(1);
	auto& app = fixture.app;
	GeminiR6TerminalCleanup cleanup{app};
	// A distinct persisted/live owner and running terminal share P only in a different workspace.
	fixture.unrelated.native_session_id = kParent;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, fixture.unrelated));
	*ChatDomainService().FindChatById(app, fixture.unrelated.id) = fixture.unrelated;
	app.resolved_native_sessions_by_chat_id[fixture.unrelated.id] = kParent;
	for (auto& [file, bytes] : fixture.unchanged_files)
		if (file == AppPaths::UamChatFilePath(app.data_root, fixture.unrelated.id)) bytes = uam::io::ReadTextFile(file);
	auto& terminal = AddGeminiR6OwnedTerminal(fixture);
	auto& other = AddGeminiR6OwnedTerminal(fixture);
	other.terminal_id = "r6-other-workspace-terminal";
	other.frontend_chat_id = fixture.unrelated.id; other.attached_chat_id = fixture.unrelated.id;
	UAM_ASSERT_EQ(other.attached_session_id, terminal.attached_session_id);
	UAM_ASSERT(uam::chat_identity::NativeIdentityKeyForHistoryImport(fixture.local) != uam::chat_identity::NativeIdentityKeyForHistoryImport(fixture.unrelated));
	const auto mappings = app.resolved_native_sessions_by_chat_id;
	(void)uam::SyncChatsFromFinalCliTerminal(app, terminal);
	AwaitGeminiTerminalSavedAndLiveCompletion(fixture);
	AssertGeminiR6CompletePublication(fixture);
	AssertGeminiR6LiveRow(app, fixture.unrelated);
	AssertGeminiR6ContextAndSources(fixture);
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
	UAM_ASSERT(terminal.running && other.running && other.generation_in_progress);
	UAM_ASSERT_EQ(other.attached_chat_id, fixture.unrelated.id);
#endif
}

UAM_TEST(GeminiTerminalDeferredReloadPreservesUnsavedAttachmentsMetadataAndUnrelatedLiveRow)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	enum class UnsavedChange { AttachmentOnly, TitleOnly, PinOnly };
	for (const auto change : {UnsavedChange::AttachmentOnly, UnsavedChange::TitleOnly, UnsavedChange::PinOnly})
	{
		GeminiTerminalSyncConsumerFixture fixture(1);
		auto& app = fixture.app;
		GeminiR6TerminalCleanup cleanup{app};
		auto& terminal = AddGeminiR6OwnedTerminal(fixture);
		auto* selected = ChatDomainService().SelectedChat(app);
		UAM_ASSERT(selected != nullptr && selected->messages.size() == 1);
		if (change == UnsavedChange::AttachmentOnly)
		{
			MessageAttachment attachment; attachment.id = "r6-unsaved-attachment"; attachment.name = "local-note.txt";
			attachment.kind = "file"; attachment.mime_type = "text/plain"; attachment.path = uam::paths::Utf8PathString(fixture.owned.temp.root / attachment.name);
			selected->messages.front().attachments.push_back(attachment);
		}
		else if (change == UnsavedChange::TitleOnly) selected->title = "Unsaved title only";
		else selected->pinned = false;
		const ChatSession expected_local = *selected;
		auto* unrelated = ChatDomainService().FindChatById(app, fixture.unrelated.id);
		UAM_ASSERT(unrelated != nullptr);
		unrelated->messages.push_back({MessageRole::User, "Unrelated unsaved live tail"});
		unrelated->title = "Unrelated unsaved title"; unrelated->pinned = false;
		const ChatSession expected_unrelated = *unrelated;
		const auto mappings = app.resolved_native_sessions_by_chat_id;
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
		// Exercise both public wrappers: a deferred final target grants no sidebar adoption receipt.
		UAM_ASSERT(uam::SyncChatsFromFinalCliTerminal(app, terminal));
		AssertGeminiR6PrimaryDeferred(fixture);
		AssertGeminiR6LiveRow(app, expected_local);
		AssertGeminiR6LiveRow(app, expected_unrelated);
		AssertGeminiR6ContextAndSources(fixture);
		UAM_ASSERT(uam::SyncChatsFromNative(app, kParent, true));
		AssertGeminiR6PrimaryDeferred(fixture);
		AssertGeminiR6LiveRow(app, expected_local);
		AssertGeminiR6LiveRow(app, expected_unrelated);
		AssertGeminiR6ContextAndSources(fixture);
		UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
		UAM_ASSERT(terminal.running && terminal.generation_in_progress);
	}
#endif
}

// R6 generic-refresh follow-up: preserve the complete 44-case prefix above.
UAM_TEST(GeminiTerminalGenericRefreshRecognizesEquivalentSavedSelfRoot)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiTerminalSyncConsumerFixture fixture(1);
	auto& app = fixture.app;
	GeminiR6TerminalCleanup cleanup{app};
	auto& terminal = AddGeminiR6OwnedTerminal(fixture);
	UAM_ASSERT(fixture.local.parent_chat_id.empty() && fixture.local.branch_from_message_index == -1);
	UAM_ASSERT_EQ(fixture.local.branch_root_chat_id, kParent);
	const auto mappings = app.resolved_native_sessions_by_chat_id;
	UAM_ASSERT(uam::SyncChatsFromNative(app, kParent, true));
	AssertGeminiR6PrimaryDeferred(fixture);
	const auto* live = ChatDomainService().SelectedChat(app);
	auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
	UAM_ASSERT(live != nullptr && saved && live->id == saved->id && live->messages == saved->messages);
	UAM_ASSERT(live->parent_chat_id.empty() && saved->parent_chat_id.empty());
	UAM_ASSERT(live->branch_from_message_index == -1 && saved->branch_from_message_index == -1);
	UAM_ASSERT_EQ(live->branch_root_chat_id, fixture.local.id);
	UAM_ASSERT_EQ(saved->branch_root_chat_id, kParent);
	UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(*live), uam::chat_identity::NativeIdentityKeyForHistoryImport(*saved));
	// Diagnose the exact saved/live serialization difference after the real public reload.
	saved->persisted_message_count = live->persisted_message_count;
	saved->persisted_messages_digest = live->persisted_messages_digest;
	const auto live_metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, *live);
	UAM_ASSERT(!live_metadata.empty() && live_metadata != ChatRepository::ChatMetadataFingerprint(app.data_root, *saved));
	saved->branch_root_chat_id = live->branch_root_chat_id;
	UAM_ASSERT_EQ(live_metadata, ChatRepository::ChatMetadataFingerprint(app.data_root, *saved));
	AssertGeminiR6ContextAndSources(fixture);
	AssertGeminiR6LiveRow(app, fixture.local);
	AssertGeminiR6LiveRow(app, fixture.unrelated);
	// A same-owner self-root alias cannot keep the completed native answer deferred.
	(void)uam::SyncChatsFromFinalCliTerminal(app, terminal);
	AwaitGeminiTerminalSavedAndLiveCompletion(fixture);
	AssertGeminiR6CompletePublication(fixture);
	AssertGeminiR6ContextAndSources(fixture);
	AssertGeminiR6LiveRow(app, fixture.unrelated);
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
	UAM_ASSERT(terminal.running && terminal.generation_in_progress);
#endif
}

UAM_TEST(GeminiTerminalFinalImportStillDefersAuthoredChangesAfterRootNormalization)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	enum class AuthoredChange { Parent, Root, Index, Title, Pin, Attachment, Tail, SavedForeignRoot };
	for (const auto change : {AuthoredChange::Parent, AuthoredChange::Root, AuthoredChange::Index, AuthoredChange::Title,
	    AuthoredChange::Pin, AuthoredChange::Attachment, AuthoredChange::Tail, AuthoredChange::SavedForeignRoot})
	{
		GeminiTerminalSyncConsumerFixture fixture(1);
		auto& app = fixture.app;
		GeminiR6TerminalCleanup cleanup{app};
		auto& terminal = AddGeminiR6OwnedTerminal(fixture);
		if (change == AuthoredChange::SavedForeignRoot)
		{
			// Global catalogue normalization must not authorize an arbitrary persisted root.
			fixture.local.branch_root_chat_id = fixture.unrelated.id;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, fixture.local));
			*ChatDomainService().FindChatById(app, fixture.local.id) = fixture.local;
			fixture.local_bytes = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id));
		}
		UAM_ASSERT(uam::SyncChatsFromNative(app, kParent, true));
		AssertGeminiR6PrimaryDeferred(fixture);
		auto* live = ChatDomainService().SelectedChat(app);
		UAM_ASSERT(live != nullptr && live->parent_chat_id.empty() && live->branch_from_message_index == -1);
		UAM_ASSERT_EQ(live->branch_root_chat_id, fixture.local.id);
		switch (change)
		{
		case AuthoredChange::Parent: live->parent_chat_id = fixture.unrelated.id; break;
		case AuthoredChange::Root: live->branch_root_chat_id = fixture.unrelated.id; break;
		case AuthoredChange::Index: live->branch_from_message_index = 0; break;
		case AuthoredChange::Title: live->title = "Unsaved title after normalization"; break;
		case AuthoredChange::Pin: live->pinned = false; break;
		case AuthoredChange::Attachment:
		{
			MessageAttachment attachment; attachment.id = "unsaved-after-normalization"; attachment.name = "draft.txt";
			attachment.kind = "file"; attachment.mime_type = "text/plain";
			attachment.path = uam::paths::Utf8PathString(fixture.owned.temp.root / attachment.name);
			live->messages.front().attachments.push_back(attachment);
			break;
		}
		case AuthoredChange::Tail: live->messages.push_back({MessageRole::User, "Unsaved tail after normalization"}); break;
		case AuthoredChange::SavedForeignRoot: break;
		}
		const ChatSession expected_live = *live;
		const auto expected_metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, expected_live);
		UAM_ASSERT(!expected_metadata.empty());
		const auto mappings = app.resolved_native_sessions_by_chat_id;
		// Use the real public final importer to isolate publication from the separate
		// sidebar Normalize step, which intentionally repairs display branch fields.
		const auto result = ChatHistorySyncService().ImportAllNativeChatsToLocal(app, false, kParent, &terminal);
		UAM_ASSERT(result.success && result.total_count == 1 && result.imported_count == 0 && result.published_chat_ids.empty());
		AssertGeminiR6PrimaryDeferred(fixture);
		AssertGeminiR6LiveRow(app, expected_live);
		live = ChatDomainService().SelectedChat(app);
		UAM_ASSERT(live != nullptr);
		UAM_ASSERT_EQ(live->parent_chat_id, expected_live.parent_chat_id);
		UAM_ASSERT_EQ(live->branch_root_chat_id, expected_live.branch_root_chat_id);
		UAM_ASSERT_EQ(live->branch_from_message_index, expected_live.branch_from_message_index);
		UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *live), expected_metadata);
		AssertGeminiR6ContextAndSources(fixture);
		AssertGeminiR6LiveRow(app, fixture.unrelated);
		UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
		UAM_ASSERT(terminal.running && terminal.generation_in_progress);
	}
#endif
}

// R7 consumer regressions. Preserve all 46 cases and 109,517 bytes above.
#if defined(__APPLE__)
#include <poll.h>
#include <sys/wait.h>
#include <termios.h>
#include <util.h>
#endif

namespace
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	std::vector<ChatSession> LoadGeminiR7Snapshot(const GeminiTerminalSyncConsumerFixture& fixture)
	{
		auto snapshot = ChatHistorySyncService().LoadNativeSessionChats(fixture.owned.chats, ProviderProfileStore::DefaultGeminiProfile());
		UAM_ASSERT(snapshot.size() == 1 && snapshot.front().messages.size() == 2);
		UAM_ASSERT_EQ(snapshot.front().native_session_id, kParent);
		UAM_ASSERT_EQ(snapshot.front().workspace_directory, fixture.owned.workspace);
		UAM_ASSERT_EQ(snapshot.front().messages.back().content, std::string("Current answer"));
		return snapshot;
	}

	ChatSession AddGeminiR7UnrelatedLiveTail(GeminiTerminalSyncConsumerFixture& fixture)
	{
		auto* other = ChatDomainService().FindChatById(fixture.app, fixture.unrelated.id);
		UAM_ASSERT(other != nullptr && other->messages_loaded && other->messages.size() == 1);
		other->messages.push_back({MessageRole::User, "Unrelated unsaved R7 tail"});
		other->title = "Unrelated unsaved R7 title";
		other->pinned = false;
		return *other;
	}

	ChatSession AddGeminiR7SelectedLiveTail(GeminiTerminalSyncConsumerFixture& fixture)
	{
		auto* selected = ChatDomainService().SelectedChat(fixture.app);
		UAM_ASSERT(selected != nullptr && selected->messages_loaded && selected->messages.size() == 3);
		selected->messages.push_back({MessageRole::User, "Unsaved live R7 tail"});
		return *selected;
	}

	void AssertGeminiR7SavedThreePreserved(const GeminiTerminalSyncConsumerFixture& fixture)
	{
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.local.id, true);
		UAM_ASSERT(saved && saved->messages.size() == 3 && saved->messages == fixture.local.messages);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.local.id)), fixture.local_bytes);
	}

#if defined(__APPLE__)
	struct GeminiR7Fd
	{
		int value = -1;
		~GeminiR7Fd() { Close(); }
		void Close() { if (value >= 0) { (void)close(value); value = -1; } }
		int Release() { const int result = value; value = -1; return result; }
	};

	void MakeGeminiR7Pipe(GeminiR7Fd& reader, GeminiR7Fd& writer)
	{
		int descriptors[2] = {-1, -1};
		UAM_ASSERT(pipe(descriptors) == 0);
		reader.value = descriptors[0]; writer.value = descriptors[1];
	}

	struct GeminiR7TerminalCleanup
	{
		uam::CliTerminalState& terminal;
		~GeminiR7TerminalCleanup() { uam::StopCliTerminal(terminal, false, uam::CliTerminalStopMode::FastExit); }
	};

	struct GeminiR7TaskCleanup
	{
		uam::AppState& app;
		~GeminiR7TaskCleanup() { uam::platform::ResetAsyncNativeChatLoadTasks(app.native_chat_load_tasks); }
	};

	// An actual test-owned child waits on a control pipe; it never execs a CLI/provider.
	// Its private process group and still-waitable PID belong to public Stop cleanup.
	struct GeminiR7MacChild
	{
		uam::CliTerminalState& terminal;
		GeminiR7Fd release_writer;
		explicit GeminiR7MacChild(uam::CliTerminalState& current, bool real_pty = false, const std::string& output = {}) : terminal(current)
		{
			GeminiR7Fd release_reader, ready_reader, ready_writer, output_reader, output_writer;
			MakeGeminiR7Pipe(release_reader, release_writer);
			MakeGeminiR7Pipe(ready_reader, ready_writer);
			if (real_pty)
			{
				int master = -1, slave = -1;
				UAM_ASSERT(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
				output_reader.value = master; output_writer.value = slave;
				struct termios attributes{};
				UAM_ASSERT(tcgetattr(slave, &attributes) == 0);
				cfmakeraw(&attributes);
				UAM_ASSERT(tcsetattr(slave, TCSANOW, &attributes) == 0);
			}
			else MakeGeminiR7Pipe(output_reader, output_writer);
			const int flags = fcntl(output_reader.value, F_GETFL);
			UAM_ASSERT(flags >= 0 && fcntl(output_reader.value, F_SETFL, flags | O_NONBLOCK) == 0);
			terminal.master_fd = output_reader.Release();
			const char* bytes = output.data();
			const std::size_t size = output.size();
			const pid_t child = fork();
			if (child == 0)
			{
				// Only async-signal-safe syscalls after fork; inherited C++ guards never run.
				(void)close(terminal.master_fd); (void)close(release_writer.value); (void)close(ready_reader.value);
				if (setpgid(0, 0) != 0) _exit(90);
				std::size_t offset = 0;
				while (offset < size)
				{
					const ssize_t written = write(output_writer.value, bytes + offset, size - offset);
					if (written < 0 && errno == EINTR) continue;
					if (written <= 0) _exit(91);
					offset += static_cast<std::size_t>(written);
				}
				const char ready = 'R';
				if (write(ready_writer.value, &ready, 1) != 1) _exit(92);
				char release = 0;
				ssize_t received = -1;
				do { received = read(release_reader.value, &release, 1); } while (received < 0 && errno == EINTR);
				_exit(received >= 0 ? 0 : 93);
			}
			UAM_ASSERT(child > 0);
			terminal.child_pid = child; // Register ownership before any parent-side assertion can fail.
			release_reader.Close(); ready_writer.Close(); output_writer.Close();
			struct pollfd ready{ready_reader.value, POLLIN, 0};
			int polled = -1;
			do { polled = poll(&ready, 1, 2000); } while (polled < 0 && errno == EINTR);
			UAM_ASSERT(polled == 1 && (ready.revents & POLLIN) != 0);
			char byte = 0;
			UAM_ASSERT(read(ready_reader.value, &byte, 1) == 1 && byte == 'R');
			UAM_ASSERT(getpgid(child) == child);
			AssertRunning();
			UAM_ASSERT(PlatformServicesFactory::Instance().terminal_runtime.HasReadableTerminalOutputHandle(terminal));
		}
		void AssertRunning() const
		{
			siginfo_t status{};
			UAM_ASSERT(terminal.child_pid > 0 && waitid(P_PID, static_cast<id_t>(terminal.child_pid), &status, WEXITED | WNOHANG | WNOWAIT) == 0);
			UAM_ASSERT(status.si_pid == 0);
		}
		void ReleaseAndConfirmExit()
		{
			release_writer.Close();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (std::chrono::steady_clock::now() < deadline)
			{
				siginfo_t status{};
				UAM_ASSERT(waitid(P_PID, static_cast<id_t>(terminal.child_pid), &status, WEXITED | WNOHANG | WNOWAIT) == 0);
				if (status.si_pid == terminal.child_pid)
				{
					UAM_ASSERT(status.si_code == CLD_EXITED && status.si_status == 0);
					return; // Leave this exact child unreaped for the real platform probe/Stop.
				}
				std::this_thread::yield();
			}
			UAM_ASSERT(false && "test-owned child exit confirmation timed out");
		}
	};

	void CompleteGeminiR7Task(uam::AppState& app, const GeminiTerminalSyncConsumerFixture& fixture,
	    const std::vector<ChatSession>& snapshot, const std::string& digest, const std::string& error = {})
	{
		// Same completed-task boundary as NativeTerminalRefreshRetriesTheSameSnapshotAfterStorageRecovers.
		uam::platform::ResetAsyncNativeChatLoadTasks(app.native_chat_load_tasks);
		auto& task = uam::AsyncNativeChatLoadTaskFor(app, "gemini-cli", fixture.owned.chats);
		task.running = true;
		task.state = std::make_shared<uam::platform::AsyncNativeChatLoadTask::State>();
		task.state->chats = snapshot; task.state->snapshot_digest = digest; task.state->error = error;
		task.state->completed.store(true, std::memory_order_release);
	}
#endif
#endif
}

UAM_TEST(GeminiLoadedSnapshotPreservesUnsavedTargetAndUnrelatedLiveTails)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiTerminalSyncConsumerFixture fixture(3);
	const ChatSession expected_local = AddGeminiR7SelectedLiveTail(fixture);
	const ChatSession expected_other = AddGeminiR7UnrelatedLiveTail(fixture);
	const auto mappings = fixture.app.resolved_native_sessions_by_chat_id;
	auto snapshot = LoadGeminiR7Snapshot(fixture);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty() && fixture.app.cli_terminals.empty());
	(void)uam::SyncChatsFromLoadedNative(fixture.app, std::move(snapshot), kParent, true);
	const auto* selected = ChatDomainService().SelectedChat(fixture.app);
	UAM_ASSERT(selected != nullptr && selected->messages_loaded && selected->messages.size() == 4);
	AssertGeminiR6LiveRow(fixture.app, expected_local);
	AssertGeminiR6LiveRow(fixture.app, expected_other);
	AssertGeminiR7SavedThreePreserved(fixture);
	AssertGeminiR6ContextAndSources(fixture);
	UAM_ASSERT(fixture.app.resolved_native_sessions_by_chat_id == mappings);
#endif
}

UAM_TEST(GeminiLoadedSnapshotPublishesSafeTargetWithoutReplacingUnrelatedLiveWork)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiTerminalSyncConsumerFixture fixture(1);
	const ChatSession expected_other = AddGeminiR7UnrelatedLiveTail(fixture);
	const auto mappings = fixture.app.resolved_native_sessions_by_chat_id;
	auto snapshot = LoadGeminiR7Snapshot(fixture);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty() && fixture.app.cli_terminals.empty());
	UAM_ASSERT(uam::SyncChatsFromLoadedNative(fixture.app, std::move(snapshot), kParent, true));
	AssertGeminiR6CompletePublication(fixture);
	const auto* other = ChatDomainService().FindChatById(fixture.app, expected_other.id);
	UAM_ASSERT(other != nullptr && other->messages_loaded && other->messages.size() == 2);
	AssertGeminiR6LiveRow(fixture.app, expected_other);
	AssertGeminiR6ContextAndSources(fixture);
	UAM_ASSERT(fixture.app.resolved_native_sessions_by_chat_id == mappings);
#endif
}

UAM_TEST(GeminiPeriodicCompletedSnapshotPreservesUnflaggedLiveWorkAfterReadRetry)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
	GeminiTerminalSyncConsumerFixture fixture(3);
	auto& app = fixture.app;
	const ChatSession expected_local = AddGeminiR7SelectedLiveTail(fixture);
	const ChatSession expected_other = AddGeminiR7UnrelatedLiveTail(fixture);
	const auto mappings = app.resolved_native_sessions_by_chat_id;
	auto& terminal = AddGeminiR6OwnedTerminal(fixture);
	GeminiR7TerminalCleanup cleanup{terminal};
	GeminiR7MacChild child(terminal);
	GeminiR7TaskCleanup task_cleanup{app};
	const auto snapshot = LoadGeminiR7Snapshot(fixture);
	const auto digest = uam::NativeHistorySnapshotDigest(snapshot);
	terminal.last_native_history_snapshot_digest = "r7-previous-snapshot";
	UAM_ASSERT(PlatformServicesFactory::Instance().terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh());
	CompleteGeminiR7Task(app, fixture, snapshot, digest, "R7 temporary snapshot read failure");
	terminal.last_sync_time_s = -100.0;
	(void)uam::PollCliTerminal(nullptr, app, terminal, true);
	UAM_ASSERT_EQ(terminal.last_native_history_snapshot_digest, std::string("r7-previous-snapshot"));
	AssertGeminiR6LiveRow(app, expected_local); AssertGeminiR6LiveRow(app, expected_other);
	AssertGeminiR7SavedThreePreserved(fixture);
	child.AssertRunning();
	CompleteGeminiR7Task(app, fixture, snapshot, digest);
	terminal.last_sync_time_s = -100.0;
	(void)uam::PollCliTerminal(nullptr, app, terminal, true);
	const auto* selected = ChatDomainService().SelectedChat(app);
	UAM_ASSERT(selected != nullptr && selected->messages_loaded && selected->messages.size() == 4);
	AssertGeminiR6LiveRow(app, expected_local); AssertGeminiR6LiveRow(app, expected_other);
	AssertGeminiR7SavedThreePreserved(fixture);
	AssertGeminiR6ContextAndSources(fixture);
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
	UAM_ASSERT(terminal.running && terminal.output_cursor == 0);
	child.AssertRunning();
#endif
}

UAM_TEST(GeminiPeriodicSnapshotSaveFailureRetainsDigestForSameSnapshotRetry)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
	GeminiTerminalSyncConsumerFixture fixture(1);
	auto& app = fixture.app;
	const auto real_root = app.data_root;
	const auto mappings = app.resolved_native_sessions_by_chat_id;
	const auto snapshot = LoadGeminiR7Snapshot(fixture);
	const auto digest = uam::NativeHistorySnapshotDigest(snapshot);
	// Preserve the existing retry fixture's standalone observer: it grants no app-owned final witness.
	uam::CliTerminalState terminal;
	terminal.terminal_id = "r7-retry-observer"; terminal.frontend_chat_id = fixture.local.id;
	terminal.attached_chat_id = fixture.local.id; terminal.attached_session_id = kParent;
	terminal.running = true; terminal.lifecycle_state = uam::CliTerminalLifecycleState::Idle;
	GeminiR7TerminalCleanup cleanup{terminal};
	GeminiR7MacChild child(terminal);
	GeminiR7TaskCleanup task_cleanup{app};
	terminal.last_native_history_snapshot_digest = "r7-before-save-failure";
	const auto blocked_root = fixture.owned.temp.root / "r7-blocked-data-root";
	UAM_ASSERT(uam::io::WriteTextFile(blocked_root, "not a directory"));
	app.data_root = blocked_root;
	CompleteGeminiR7Task(app, fixture, snapshot, digest);
	terminal.last_sync_time_s = -100.0;
	(void)uam::PollCliTerminal(nullptr, app, terminal, true);
	UAM_ASSERT_EQ(terminal.last_native_history_snapshot_digest, std::string("r7-before-save-failure"));
	UAM_ASSERT(!app.native_chat_refresh_error_status.empty());
	AssertGeminiR6LiveRow(app, fixture.local); AssertGeminiR6LiveRow(app, fixture.unrelated);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(real_root, fixture.local.id)), fixture.local_bytes);
	child.AssertRunning();
	app.data_root = real_root;
	CompleteGeminiR7Task(app, fixture, snapshot, digest); // Same exact snapshot/digest, now writable.
	terminal.last_sync_time_s = -100.0;
	(void)uam::PollCliTerminal(nullptr, app, terminal, true);
	UAM_ASSERT_EQ(terminal.last_native_history_snapshot_digest, digest);
	UAM_ASSERT(app.native_chat_refresh_error_status.empty());
	AssertGeminiR6CompletePublication(fixture);
	AssertGeminiR6ContextAndSources(fixture);
	fixture.AssertIdentityAndUnrelatedHistoryPreserved(); // An unchanged unrelated saved row may stay lazy.
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
	UAM_ASSERT(terminal.running && terminal.output_cursor == 0);
	child.AssertRunning();
#endif
}

UAM_TEST(GeminiTerminalRealPtySmallOutputThenEofPublishesFinalHistory)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
	GeminiTerminalSyncConsumerFixture fixture(1);
	auto& app = fixture.app;
	auto& terminal = AddGeminiR6OwnedTerminal(fixture);
	GeminiR7TerminalCleanup cleanup{terminal};
	std::string output(512, 'x'); output.replace(0, 13, "R7 PTY prefix"); output.replace(498, 14, "R7 PTY suffix!");
	GeminiR7MacChild child(terminal, true, output);
	struct pollfd ready{terminal.master_fd, POLLIN, 0};
	UAM_ASSERT(poll(&ready, 1, 2000) == 1 && (ready.revents & POLLIN) != 0);
	const auto mappings = app.resolved_native_sessions_by_chat_id;
	terminal.last_sync_time_s = uam::GetAppTimeSeconds();
	(void)uam::PollCliTerminal(nullptr, app, terminal, true);
	UAM_ASSERT(terminal.running && terminal.output_cursor == output.size());
	UAM_ASSERT_EQ(terminal.recent_output_bytes, output); // Exact bytes at the real forwarding boundary, before Stop clears it.
	child.AssertRunning();
	AssertGeminiR6PrimaryDeferred(fixture);
	child.ReleaseAndConfirmExit(); // Real child exited; no reaping or fake PID/exit state.
	UAM_ASSERT(uam::PollCliTerminal(nullptr, app, terminal, true));
	UAM_ASSERT(!terminal.running && terminal.lifecycle_state == uam::CliTerminalLifecycleState::Stopped);
	UAM_ASSERT_EQ(terminal.output_cursor, static_cast<std::uint64_t>(output.size()));
	UAM_ASSERT(!PlatformServicesFactory::Instance().terminal_runtime.HasReadableTerminalOutputHandle(terminal));
	AwaitGeminiTerminalSavedAndLiveCompletion(fixture, true);
	AssertGeminiR6CompletePublication(fixture);
	AssertGeminiR6ContextAndSources(fixture);
	AssertGeminiR6LiveRow(app, fixture.unrelated);
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
#endif
}

// R7 loaded-publication follow-up: preserve the complete 51-case prefix above.
UAM_TEST(GeminiLoadedPublicationDefersActiveOwnerUntilSameSnapshotCanRetry)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiTerminalSyncConsumerFixture fixture(1);
	auto& app = fixture.app;
	GeminiR6TerminalCleanup cleanup{app};
	auto& terminal = AddGeminiR6OwnedTerminal(fixture);
	const auto expected_other = AddGeminiR7UnrelatedLiveTail(fixture);
	const auto snapshot = LoadGeminiR7Snapshot(fixture);
	const auto mappings = app.resolved_native_sessions_by_chat_id;
	const auto deferred = ChatHistorySyncService().ImportLoadedNativeChatsToLocal(app, snapshot, kParent);
	UAM_ASSERT(!deferred.success && deferred.errors.empty() && deferred.imported_count == 0 && deferred.published_chat_ids.empty());
	UAM_ASSERT(!uam::SyncChatsFromLoadedNative(app, snapshot, kParent, true));
	AssertGeminiR6PrimaryDeferred(fixture);
	AssertGeminiR6LiveRow(app, fixture.local); AssertGeminiR6LiveRow(app, expected_other);
	AssertGeminiR6ContextAndSources(fixture);
	UAM_ASSERT(terminal.running && terminal.generation_in_progress);
	// Public Stop changes the actual owned terminal state; no final import is invoked here.
	uam::StopCliTerminal(terminal, false, uam::CliTerminalStopMode::FastExit);
	UAM_ASSERT(!terminal.running && !uam::ChatHasActiveCliTerminal(app, fixture.local.id));
	UAM_ASSERT(uam::SyncChatsFromLoadedNative(app, snapshot, kParent, true));
	AssertGeminiR6CompletePublication(fixture);
	AssertGeminiR6LiveRow(app, expected_other);
	AssertGeminiR6ContextAndSources(fixture);
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
#endif
}

UAM_TEST(GeminiLoadedPublicationRetainsTombstonesAndRejectsForeignScope)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	enum class Boundary { Tombstoned, UnreadableDeletion, ForeignWorkspace, StaleMapping };
	for (const auto boundary : {Boundary::Tombstoned, Boundary::UnreadableDeletion, Boundary::ForeignWorkspace, Boundary::StaleMapping})
	{
		GeminiTerminalSyncConsumerFixture fixture(1);
		auto& app = fixture.app;
		const auto expected_other = AddGeminiR7UnrelatedLiveTail(fixture);
		auto snapshot = LoadGeminiR7Snapshot(fixture);
		if (boundary == Boundary::Tombstoned)
		{
			std::vector<std::string> keys;
			UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(app.data_root, snapshot, keys) && keys.size() == 1);
		}
		else if (boundary == Boundary::UnreadableDeletion)
			UAM_ASSERT(uam::io::WriteTextFile(app.data_root / "native-import-tombstones.json", "{"));
		else if (boundary == Boundary::ForeignWorkspace)
			snapshot.front().workspace_directory = fixture.unrelated.workspace_directory;
		else app.resolved_native_sessions_by_chat_id[fixture.local.id] = fixture.unrelated.native_session_id;
		const auto mappings = app.resolved_native_sessions_by_chat_id;
		const bool disposed = uam::SyncChatsFromLoadedNative(app, snapshot, kParent, true);
		UAM_ASSERT_EQ(disposed, boundary == Boundary::Tombstoned);
		AssertGeminiR6PrimaryDeferred(fixture);
		AssertGeminiR6LiveRow(app, fixture.local); AssertGeminiR6LiveRow(app, expected_other);
		AssertGeminiR6ContextAndSources(fixture);
		UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
		if (boundary == Boundary::Tombstoned)
		{
			std::string error;
			UAM_ASSERT(ChatHistorySyncService().FilterDeletedNativeHistory(app.data_root, snapshot, &error) && snapshot.empty() && error.empty());
		}
	}
#endif
}

UAM_TEST(GeminiLoadedPublicationDoesNotInferUnlinkedDraftOwnership)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	GeminiTerminalSyncConsumerFixture fixture(1);
	auto& app = fixture.app;
	const auto snapshot = LoadGeminiR7Snapshot(fixture);
	ChatSession draft;
	draft.id = "chat-r7-unlinked-draft"; draft.provider_id = "gemini-cli";
	draft.execution_host_id = "local"; draft.workspace_directory = fixture.owned.workspace; draft.folder_id = fixture.local.folder_id;
	draft.title = "Unlinked R7 intent"; draft.messages.push_back({MessageRole::User, "Native question"});
	UAM_ASSERT(NativeSessionLinkService().IsLocalDraftChatId(draft.id));
	const auto inferred = NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(draft, snapshot);
	UAM_ASSERT(inferred && *inferred == kParent); // Establish the old inference path's positive producer shape.
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, draft));
	const auto draft_path = AppPaths::UamChatFilePath(app.data_root, draft.id);
	const auto draft_bytes = uam::io::ReadTextFile(draft_path);
	app.chats.push_back(draft);
	const auto mappings = app.resolved_native_sessions_by_chat_id;
	UAM_ASSERT(uam::SyncChatsFromLoadedNative(app, snapshot, kParent, true));
	AssertGeminiR6CompletePublication(fixture);
	const auto* retained = ChatDomainService().FindChatById(app, draft.id);
	UAM_ASSERT(retained != nullptr && retained->native_session_id.empty() && retained->messages == draft.messages);
	UAM_ASSERT_EQ(retained->title, draft.title);
	UAM_ASSERT(!app.resolved_native_sessions_by_chat_id.contains(draft.id));
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t(3));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChats(app.data_root).size(), std::size_t(3));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(draft_path), draft_bytes);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), fixture.local.id);
	UAM_ASSERT_EQ(app.composer_text, std::string("Keep selected draft"));
	UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), fixture.folders_before);
	for (const auto& [file, bytes] : fixture.unchanged_files) UAM_ASSERT_EQ(uam::io::ReadTextFile(file), bytes);
#endif
}


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	const std::string kTerminalOriginalPng = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
	struct GeminiTerminalOriginalFixture : GeminiTerminalSyncConsumerFixture
	{
		std::vector<std::pair<std::size_t, MessageAttachment>> parts;
		std::string original;
		std::string expected_answer = "Current answer";

		GeminiTerminalOriginalFixture() : GeminiTerminalSyncConsumerFixture(1)
		{
			UAM_ASSERT(uam::base64::Decode(kTerminalOriginalPng, original) && !original.empty());
			const Json image{{"inlineData", {{"mimeType", "image/png"}, {"data", kTerminalOriginalPng}}}};
			const Json user_parts = Json::array({{{"text", "Native question"}}, image});
			const Json tool_result = Json::array({{{"functionResponse", {{"id", "t"}, {"name", "read_file"}, {"response", {{"output", "Current tool result"}}}}}}, image});
			const std::string bytes = Journal() +
			    Line({{"$patch", {{"id", "u"}, {"content", user_parts}}}}) +
			    Line({{"$patch", {{"id", "a"}, {"toolCalls", Json::array({{{"id", "t"}, {"result", tool_result}}})}}}});
			const auto path = owned.chats / (kParent + ".jsonl");
			UAM_ASSERT(uam::io::WriteTextFile(path, bytes));
			// The trigger is a real new provider journal. Preserve these bytes from now on.
			for (auto& [file, expected] : unchanged_files) if (file == path) expected = bytes;
			auto snapshot = Snapshot();
			for (std::size_t row = 0; row < snapshot.front().messages.size(); ++row)
				for (const auto& attachment : snapshot.front().messages[row].attachments)
				{
					UAM_ASSERT_EQ(attachment.source, AttachmentSource::NativeInlinePending);
					UAM_ASSERT(attachment.native_inline_payload && attachment.path.empty() && attachment.native_asset_key.empty());
					auto expected = attachment;
					expected.native_inline_payload.reset(); // Do not retain a second payload lease in expectations.
					parts.emplace_back(row, std::move(expected));
				}
			UAM_ASSERT_EQ(parts.size(), std::size_t{2});
			UAM_ASSERT(parts.front().second.native_tool_call_id.empty());
			UAM_ASSERT_EQ(parts.back().second.native_tool_call_id, std::string("t"));
			UAM_ASSERT(parts.front().second.native_part_key != parts.back().second.native_part_key);
		}

		std::vector<ChatSession> Snapshot() const
		{
			auto snapshot = ChatHistorySyncService().LoadNativeSessionChats(owned.chats, ProviderProfileStore::DefaultGeminiProfile());
			UAM_ASSERT_EQ(snapshot.size(), std::size_t{1});
			UAM_ASSERT_EQ(snapshot.front().messages.size(), std::size_t{2});
			UAM_ASSERT_EQ(snapshot.front().native_session_id, kParent);
			UAM_ASSERT_EQ(snapshot.front().workspace_directory, owned.workspace);
			return snapshot;
		}

		void AdvanceAnswer(const std::string& content)
		{
			UAM_ASSERT(content != expected_answer);
			const auto file = owned.chats / (kParent + ".jsonl");
			const auto bytes = uam::io::ReadTextFile(file) + Line({{"$patch", {{"id", "a"}, {"content", content}}}});
			UAM_ASSERT(uam::io::WriteTextFile(file, bytes));
			expected_answer = content;
			// A genuine new native journal patch is the refresh trigger. From this
			// point every operation must leave its complete native bytes unchanged.
			for (auto& [path, expected] : unchanged_files) if (path == file) expected = bytes;
			const auto snapshot = Snapshot();
			UAM_ASSERT_EQ(snapshot.front().messages.back().content, expected_answer);
			UAM_ASSERT(!HasPublished()); // Stored originals alone cannot satisfy this attempt.
		}

		bool HasPublished() const
		{
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, local.id, true);
			const auto* selected = ChatDomainService().SelectedChat(app);
			if (!saved || !saved->messages_loaded || saved->messages.size() != 2 ||
			    saved->messages.back().content != expected_answer || selected == nullptr || !selected->messages_loaded || selected->messages != saved->messages) return false;
			std::size_t count = 0;
			for (const auto& message : saved->messages)
				for (const auto& attachment : message.attachments)
				{
					if (attachment.source != AttachmentSource::OwnedNativeAsset || attachment.native_asset_key.empty() || attachment.native_inline_payload) return false;
					++count;
				}
			return count == parts.size();
		}

		void RequestLoaded()
		{
			// Keep the existing bool's completed-publication meaning: pending admission
			// need not return true and a true result cannot acknowledge unsaved work.
			const bool completed = uam::SyncChatsFromLoadedNative(app, Snapshot(), kParent, true);
			if (completed) UAM_ASSERT(HasPublished());
		}

		void CloseAfterFinalCapture()
		{
			auto terminal = std::make_unique<uam::CliTerminalState>();
			terminal->terminal_id = "owned-original-final-terminal";
			terminal->frontend_chat_id = terminal->attached_chat_id = local.id;
			terminal->attached_session_id = kParent;
			terminal->running = true;
			terminal->turn_state = uam::CliTerminalTurnState::Idle;
			terminal->lifecycle_state = uam::CliTerminalLifecycleState::Idle;
			app.cli_terminals.push_back(std::move(terminal));
			UAM_ASSERT_EQ(app.cli_terminals.size(), std::size_t{1});
			UAM_ASSERT_EQ(uam::CliTerminalSyncTargetId(*app.cli_terminals.front()), kParent);
			UAM_ASSERT(uam::ChatHasActiveCliTerminal(app, local.id));
			UAM_ASSERT(!uam::ChatHasBusyCliTerminal(app, local.id));
			// The actual close caller captures its final witness before clearing and
			// erasing this app-owned terminal. No pointer is retained by the test.
			// Platform process handles are invalid, as in existing final-sync controls.
			UAM_ASSERT(uam::StopAndEraseCliTerminalForChat(app, local.id, true));
			UAM_ASSERT(app.cli_terminals.empty());
		}

		template <class Predicate> void DrainUntil(Predicate complete)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			while (!complete() && std::chrono::steady_clock::now() < deadline)
			{
				// Existing real application polling entry; the production repair must
				// drain its app-owned media coordinator even with no terminal rows.
				(void)uam::PollAllCliTerminals(nullptr, app, false);
				std::this_thread::yield();
			}
			UAM_ASSERT(complete());
		}

		void AwaitPublished()
		{
			DrainUntil([&] { return HasPublished() && !ChatHistorySyncService().TerminalNativeMediaPending(app); });
			UAM_ASSERT(app.native_chat_refresh_error_status.empty());
		}

		std::vector<std::string> AssertPublished() const
		{
			AssertIdentityAndUnrelatedHistoryPreserved();
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, local.id, true);
			UAM_ASSERT(saved && saved->messages_loaded && saved->messages.size() == 2);
			UAM_ASSERT_EQ(saved->messages.front().content, std::string("Native question"));
			UAM_ASSERT_EQ(saved->messages.back().content, expected_answer);
			UAM_ASSERT_EQ(saved->messages.back().tool_calls.size(), std::size_t{1});
			UAM_ASSERT_EQ(saved->messages.back().tool_calls.front().id, std::string("t"));
			std::size_t count = 0;
			for (const auto& message : saved->messages) count += message.attachments.size();
			UAM_ASSERT_EQ(count, parts.size());
			std::vector<std::string> keys;
			for (const auto& [row, expected] : parts)
			{
				const auto& attachments = saved->messages[row].attachments;
				const auto found = std::ranges::find_if(attachments, [&](const auto& value) { return value.id == expected.id; });
				UAM_ASSERT(found != attachments.end());
				UAM_ASSERT_EQ(found->source, AttachmentSource::OwnedNativeAsset);
				UAM_ASSERT(found->path.empty() && !found->native_inline_payload);
				UAM_ASSERT_EQ(found->name, expected.name);
				UAM_ASSERT_EQ(found->kind, expected.kind);
				UAM_ASSERT_EQ(found->native_part_key, expected.native_part_key);
				UAM_ASSERT_EQ(found->native_tool_call_id, expected.native_tool_call_id);
				UAM_ASSERT_EQ(found->native_source_field, expected.native_source_field);
				UAM_ASSERT_EQ(found->origin_execution_host_id, expected.origin_execution_host_id);
				UAM_ASSERT_EQ(found->mime_type, expected.mime_type);
				UAM_ASSERT_EQ(found->content_digest, expected.content_digest);
				UAM_ASSERT_EQ(found->size_bytes, expected.size_bytes);
				UAM_ASSERT(uam::native_assets::IsKeyForOwner(local.id, found->native_asset_key));
				UAM_ASSERT(!uam::native_assets::IsKeyForOwner(kParent, found->native_asset_key));
				const auto read = uam::native_assets::ReadOwned(app.data_root, local.id, found->native_asset_key);
				UAM_ASSERT(read.ok && read.error.empty());
				UAM_ASSERT_EQ(read.bytes, original);
				keys.push_back(found->native_asset_key);
			}
			const auto* selected = ChatDomainService().SelectedChat(app);
			UAM_ASSERT(selected && selected->messages_loaded && selected->messages == saved->messages);
			return keys;
		}

		void PublishExistingOwnedControl()
		{
			auto snapshot = Snapshot();
			ChatSession candidate = std::move(snapshot.front());
			ChatHistorySyncService().OverlayPreparedLocalHistory(local, candidate);
			UAM_ASSERT(candidate.id == local.id && candidate.native_session_id == kParent);
			const auto revision = ChatRepository::ChatStorageRevision(app.data_root, local.id);
			UAM_ASSERT(revision && *revision != "missing");
			// Existing-owned control setup uses the real already-verified original publisher
			// entirely on its required worker context, never a forged key or saved receipt.
			auto committed = std::async(std::launch::async, [root = app.data_root, candidate = std::move(candidate), revision = *revision]() mutable
			{
				std::string error;
				auto prepared = uam::PreparedNativeMediaChatSave::Prepare(root, std::move(candidate), revision, error);
				UAM_ASSERT(prepared && error.empty());
				UAM_ASSERT(prepared->Publish(error) && prepared->PrimaryAcknowledged());
				UAM_ASSERT(prepared->AcknowledgeOriginals(error) && error.empty());
				return prepared->TakeCommittedChat();
			}).get();
			UAM_ASSERT(committed);
			*ChatDomainService().FindChatById(app, local.id) = *committed;
			local = *committed;
			local_bytes = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, local.id));
		}
	};
}

UAM_TEST(GeminiTerminalLoadedInlineOriginalsCompleteThroughPublicPoll)
{
	GeminiTerminalOriginalFixture fixture;
	fixture.RequestLoaded(); // Exactly one initial admission, never repeatedly queue until green.
	fixture.AwaitPublished();
	const auto keys = fixture.AssertPublished();
	fixture.AdvanceAnswer("Loaded follow-up answer");
	fixture.RequestLoaded();
	fixture.AwaitPublished();
	UAM_ASSERT_EQ(fixture.AssertPublished(), keys);
}

UAM_TEST(GeminiTerminalFinalInlineOriginalsCompleteAfterOwnedTerminalErase)
{
	GeminiTerminalOriginalFixture fixture;
	fixture.CloseAfterFinalCapture();
	UAM_ASSERT(fixture.app.cli_terminals.empty());
	fixture.AwaitPublished();
	UAM_ASSERT(fixture.app.cli_terminals.empty()); // Completion cannot recreate terminal ownership.
	const auto keys = fixture.AssertPublished();
	fixture.AdvanceAnswer("Post-close follow-up answer");
	fixture.RequestLoaded(); // A later ordinary refresh reuses the same immutable originals.
	fixture.AwaitPublished();
	UAM_ASSERT_EQ(fixture.AssertPublished(), keys);
	UAM_ASSERT(fixture.app.cli_terminals.empty());
}

UAM_TEST(GeminiTerminalOriginalsKeepTextAndExistingOwnedControls)
{
	{
		GeminiTerminalSyncConsumerFixture fixture(1);
		auto snapshot = ChatHistorySyncService().LoadNativeSessionChats(fixture.owned.chats, ProviderProfileStore::DefaultGeminiProfile());
		UAM_ASSERT(snapshot.size() == 1 && snapshot.front().messages.size() == 2);
		UAM_ASSERT(uam::SyncChatsFromLoadedNative(fixture.app, std::move(snapshot), kParent, true));
		fixture.AssertIdentityAndUnrelatedHistoryPreserved();
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.local.id, true);
		UAM_ASSERT(saved && saved->messages.size() == 2 && saved->messages.back().content == "Current answer");
	}
	{
		GeminiTerminalOriginalFixture fixture;
		fixture.PublishExistingOwnedControl();
		const auto keys = fixture.AssertPublished();
		fixture.AdvanceAnswer("Existing-owned follow-up answer");
		fixture.RequestLoaded();
		fixture.AwaitPublished();
		UAM_ASSERT_EQ(fixture.AssertPublished(), keys);
	}
}

UAM_TEST(GeminiTerminalOriginalPublicationFailurePreservesSourceUntilFreshRequest)
{
	GeminiTerminalOriginalFixture fixture;
	const auto before = fixture.local.messages;
	const auto store = fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory);
	UAM_ASSERT(!fs::exists(store));
	UAM_ASSERT(uam::io::WriteTextFile(store, "Private original-store obstruction"));
	fixture.RequestLoaded();
	// The real public failure status, rather than elapsed time or a fabricated
	// future, witnesses a refused attempt. The private journal remains unchanged.
	fixture.DrainUntil([&] { return fixture.app.native_chat_refresh_error_target_id == kParent && !fixture.app.native_chat_refresh_error_status.empty(); });
	UAM_ASSERT(!fixture.HasPublished());
	UAM_ASSERT(fs::is_regular_file(store));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(store), std::string("Private original-store obstruction"));
	fixture.AssertIdentityAndUnrelatedHistoryPreserved();
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.local.id)), fixture.local_bytes);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChat(fixture.app)->messages, before);
	UAM_ASSERT(fs::remove(store));
	fixture.RequestLoaded(); // Real fresh parse/admission; never reuse a fabricated receipt.
	fixture.AwaitPublished();
	(void)fixture.AssertPublished();
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
namespace
{
	// Built before fork; the child only appends these exact native records from
	// its actual owned stop signal handler, using preopened private descriptors.
	constexpr char kGeminiPostStopJournalPatch[] = R"stop({"$patch":{"id":"u","content":[{"text":"Native question"},{"inlineData":{"mimeType":"image/png","data":"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="}}]}}
{"$patch":{"id":"a","content":"Answer written only after owned stop signal","toolCalls":[{"id":"t","result":[{"functionResponse":{"id":"t","name":"read_file","response":{"output":"Post-stop tool result"}}},{"inlineData":{"mimeType":"image/png","data":"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="}}]}]}}
)stop";
	volatile sig_atomic_t gGeminiPostStopJournalFd = -1;
	volatile sig_atomic_t gGeminiPostStopMarkerFd = -1;

	void WriteGeminiJournalOnOwnedStop(int signal_number)
	{
		std::size_t offset = 0;
		while (offset < sizeof(kGeminiPostStopJournalPatch) - 1)
		{
			const ssize_t count = write(gGeminiPostStopJournalFd, kGeminiPostStopJournalPatch + offset, sizeof(kGeminiPostStopJournalPatch) - 1 - offset);
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0) _exit(94);
			offset += static_cast<std::size_t>(count);
		}
		const char marker = signal_number == SIGHUP ? 'H' : 'T';
		ssize_t count = -1;
		do { count = write(gGeminiPostStopMarkerFd, &marker, 1); } while (count < 0 && errno == EINTR);
		_exit(count == 1 ? 0 : 95);
	}

	struct GeminiPostStopMacChild
	{
		GeminiR7Fd marker_reader;
		pid_t pid = -1;
		GeminiPostStopMacChild(uam::CliTerminalState& terminal, const fs::path& native_journal)
		{
			GeminiR7Fd marker_writer, ready_reader, ready_writer, journal_writer, pty_master, pty_slave;
			MakeGeminiR7Pipe(marker_reader, marker_writer);
			MakeGeminiR7Pipe(ready_reader, ready_writer);
			journal_writer.value = open(native_journal.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
			UAM_ASSERT(journal_writer.value >= 0);
			int master = -1, slave = -1;
			UAM_ASSERT(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
			pty_master.value = master; pty_slave.value = slave;
			const int flags = fcntl(master, F_GETFL);
			UAM_ASSERT(flags >= 0 && fcntl(master, F_SETFL, flags | O_NONBLOCK) == 0);
			terminal.master_fd = pty_master.Release();
			pid = fork();
			if (pid == 0)
			{
				// Async-signal-safe syscalls only after fork; inherited C++ guards do not run.
				(void)close(terminal.master_fd); (void)close(marker_reader.value); (void)close(ready_reader.value);
				if (setpgid(0, 0) != 0) _exit(90);
				gGeminiPostStopJournalFd = journal_writer.value;
				gGeminiPostStopMarkerFd = marker_writer.value;
				struct sigaction action{};
				action.sa_handler = WriteGeminiJournalOnOwnedStop;
				if (sigfillset(&action.sa_mask) != 0 || sigaction(SIGHUP, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) _exit(91);
				sigset_t unblocked{};
				if (sigemptyset(&unblocked) != 0 || sigprocmask(SIG_SETMASK, &unblocked, nullptr) != 0) _exit(93);
				const char ready = 'R';
				if (write(ready_writer.value, &ready, 1) != 1) _exit(92);
				for (;;) (void)pause();
			}
			if (pid > 0) terminal.child_pid = pid; // Register exact ownership before assertions.
			UAM_ASSERT(pid > 0);
			marker_writer.Close(); ready_writer.Close(); journal_writer.Close(); pty_slave.Close();
			struct pollfd ready{ready_reader.value, POLLIN, 0};
			int polled = -1;
			do { polled = poll(&ready, 1, 2000); } while (polled < 0 && errno == EINTR);
			UAM_ASSERT(polled == 1 && (ready.revents & POLLIN) != 0);
			char byte = 0;
			UAM_ASSERT(read(ready_reader.value, &byte, 1) == 1 && byte == 'R');
			UAM_ASSERT(getpgid(pid) == pid);
			siginfo_t status{};
			UAM_ASSERT(waitid(P_PID, static_cast<id_t>(pid), &status, WEXITED | WNOHANG | WNOWAIT) == 0 && status.si_pid == 0);
			UAM_ASSERT(PlatformServicesFactory::Instance().terminal_runtime.HasReadableTerminalOutputHandle(terminal));
		}

		void AssertStopPatchWrittenAndReaped() const
		{
			struct pollfd marker{marker_reader.value, POLLIN, 0};
			int polled = -1;
			do { polled = poll(&marker, 1, 2000); } while (polled < 0 && errno == EINTR);
			UAM_ASSERT(polled == 1 && (marker.revents & POLLIN) != 0);
			char byte = 0;
			UAM_ASSERT(read(marker_reader.value, &byte, 1) == 1 && byte == 'H');
			int status = 0;
			errno = 0;
			UAM_ASSERT(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
		}
	};
}

UAM_TEST(GeminiTerminalFinalOriginalsUseJournalWrittenDuringOwnedNormalStop)
{
	GeminiTerminalSyncConsumerFixture fixture(1);
	auto& app = fixture.app;
	GeminiR6TerminalCleanup terminal_cleanup{app};
	GeminiR7TaskCleanup read_cleanup{app};
	auto& terminal = AddGeminiR6OwnedTerminal(fixture, false);
	const auto native_file = fixture.owned.chats / (kParent + ".jsonl");
	const auto before_native = uam::io::ReadTextFile(native_file);
	UAM_ASSERT_EQ(before_native, Journal()); // No inline originals or final text exist yet.
	GeminiPostStopMacChild child(terminal, native_file);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(native_file), before_native);
	UAM_ASSERT(uam::ChatHasActiveCliTerminal(app, fixture.local.id));
	UAM_ASSERT_EQ(uam::CliTerminalSyncTargetId(terminal), kParent);
	const auto complete = [&]
	{
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		const auto* live = ChatDomainService().SelectedChat(app);
		if (ChatHistorySyncService().TerminalNativeMediaPending(app) || !saved || !saved->messages_loaded || saved->messages.size() != 2 ||
		    saved->messages.back().content != "Answer written only after owned stop signal" ||
		    live == nullptr || !live->messages_loaded || live->messages != saved->messages) return false;
		std::size_t count = 0;
		for (const auto& message : saved->messages)
			for (const auto& attachment : message.attachments)
			{
				if (attachment.source != AttachmentSource::OwnedNativeAsset || attachment.native_asset_key.empty() || attachment.native_inline_payload) return false;
				++count;
			}
		return count == 2;
	};
	UAM_ASSERT(!complete());
	// Actual public final-capture -> normal platform stop/reap. Normal stop allows
	// HUP settlement; FastExit's consecutive HUP/TERM/KILL cannot promise it.
	uam::StopAllCliTerminals(app, false);
	child.AssertStopPatchWrittenAndReaped();
	UAM_ASSERT(!terminal.running && terminal.child_pid == -1 && terminal.master_fd == -1);
	UAM_ASSERT_EQ(terminal.attached_chat_id, fixture.local.id);
	UAM_ASSERT_EQ(terminal.attached_session_id, kParent);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(native_file), before_native + kGeminiPostStopJournalPatch);
	// Retained identity routes the already-stopped row through actual public
	// erase without another sync. No test-created replacement witness is used.
	UAM_ASSERT(uam::StopAndEraseCliTerminalForChat(app, fixture.local.id, false));
	UAM_ASSERT(app.cli_terminals.empty());
	UAM_ASSERT(!complete()); // No owner poll has admitted publication/adoption.
	for (auto& [file, expected] : fixture.unchanged_files) if (file == native_file) expected = before_native + kGeminiPostStopJournalPatch;
	const auto snapshot = ChatHistorySyncService().LoadNativeSessionChats(fixture.owned.chats, ProviderProfileStore::DefaultGeminiProfile());
	UAM_ASSERT(snapshot.size() == 1 && snapshot.front().messages.size() == 2);
	UAM_ASSERT_EQ(snapshot.front().native_session_id, kParent);
	UAM_ASSERT_EQ(snapshot.front().messages.back().content, std::string("Answer written only after owned stop signal"));
	std::vector<std::pair<std::size_t, MessageAttachment>> parts;
	for (std::size_t row = 0; row < snapshot.front().messages.size(); ++row)
		for (const auto& attachment : snapshot.front().messages[row].attachments)
		{
			UAM_ASSERT(attachment.source == AttachmentSource::NativeInlinePending && attachment.native_inline_payload);
			auto expected = attachment; expected.native_inline_payload.reset();
			parts.emplace_back(row, std::move(expected));
		}
	UAM_ASSERT_EQ(parts.size(), std::size_t{2});
	UAM_ASSERT(parts.front().second.native_tool_call_id.empty() && parts.back().second.native_tool_call_id == "t");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (!complete() && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::PollAllCliTerminals(nullptr, app, false);
		std::this_thread::yield();
	}
	UAM_ASSERT(complete());
	UAM_ASSERT(app.cli_terminals.empty() && app.native_chat_refresh_error_status.empty());
	fixture.AssertIdentityAndUnrelatedHistoryPreserved();
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
	UAM_ASSERT(saved && saved->messages.back().tool_calls.size() == 1 && saved->messages.back().tool_calls.front().id == "t");
	UAM_ASSERT_EQ(saved->messages.front().content, std::string("Native question"));
	UAM_ASSERT_EQ(saved->native_session_id, kParent); UAM_ASSERT_EQ(saved->provider_id, fixture.local.provider_id);
	UAM_ASSERT_EQ(saved->workspace_directory, fixture.owned.workspace); UAM_ASSERT_EQ(saved->execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(saved->messages.back().tool_calls.front().result_text, std::string("Post-stop tool result"));
	std::string original;
	UAM_ASSERT(uam::base64::Decode(kTerminalOriginalPng, original));
	for (const auto& [row, expected] : parts)
	{
		const auto& attachments = saved->messages[row].attachments;
		const auto found = std::ranges::find_if(attachments, [&](const auto& attachment) { return attachment.id == expected.id; });
		UAM_ASSERT(found != attachments.end() && found->source == AttachmentSource::OwnedNativeAsset);
		UAM_ASSERT(found->path.empty() && !found->native_inline_payload);
		UAM_ASSERT_EQ(found->name, expected.name); UAM_ASSERT_EQ(found->kind, expected.kind);
		UAM_ASSERT_EQ(found->native_part_key, expected.native_part_key); UAM_ASSERT_EQ(found->native_tool_call_id, expected.native_tool_call_id);
		UAM_ASSERT_EQ(found->native_source_field, expected.native_source_field); UAM_ASSERT_EQ(found->origin_execution_host_id, expected.origin_execution_host_id);
		UAM_ASSERT_EQ(found->mime_type, expected.mime_type); UAM_ASSERT_EQ(found->content_digest, expected.content_digest); UAM_ASSERT_EQ(found->size_bytes, expected.size_bytes);
		UAM_ASSERT(uam::native_assets::IsKeyForOwner(fixture.local.id, found->native_asset_key));
		UAM_ASSERT(!uam::native_assets::IsKeyForOwner(kParent, found->native_asset_key));
		const auto read = uam::native_assets::ReadOwned(app.data_root, fixture.local.id, found->native_asset_key);
		UAM_ASSERT(read.ok && read.error.empty()); UAM_ASSERT_EQ(read.bytes, original);
	}
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(GeminiTerminalPendingOriginalsCannotAdoptAcrossNewerSavedAuthority)
{
	enum class Change { Birth, NativeBinding, StorageOnly };
	for (const auto change : {Change::Birth, Change::NativeBinding, Change::StorageOnly})
	{
		GeminiTerminalOriginalFixture fixture;
		auto& app = fixture.app;
		const auto before_revision = ChatRepository::ChatStorageRevision(app.data_root, fixture.local.id);
		UAM_ASSERT(before_revision && *before_revision != "missing");
		UAM_ASSERT(app.native_chat_refresh_error_status.empty());
		// New inline bytes require worker preparation; this is pending, never a
		// completed-save acknowledgement. No owner poll occurs before the change.
		UAM_ASSERT(!uam::SyncChatsFromLoadedNative(app, fixture.Snapshot(), kParent, true));
		UAM_ASSERT(app.native_chat_refresh_error_status.empty());
		UAM_ASSERT(!fixture.HasPublished());

		auto newer = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		UAM_ASSERT(newer && newer->messages_loaded && newer->messages.size() == 1);
		newer->messages.push_back({MessageRole::User, "Newer authoritative work survives pending native originals"});
		if (change == Change::Birth)
		{
			newer->created_at = "2031-04-05T06:07:08Z";
			UAM_ASSERT(newer->created_at != fixture.local.created_at);
		}
		if (change == Change::NativeBinding) newer->native_session_id = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *newer));
		newer = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		UAM_ASSERT(newer && newer->messages_loaded && newer->messages.size() == 2);
		if (change == Change::Birth) UAM_ASSERT_EQ(newer->created_at, std::string("2031-04-05T06:07:08Z"));
		if (change == Change::NativeBinding) UAM_ASSERT_EQ(newer->native_session_id, std::string("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"));
		const auto after_revision = ChatRepository::ChatStorageRevision(app.data_root, fixture.local.id);
		UAM_ASSERT(after_revision && *after_revision != *before_revision);
		auto* live = ChatDomainService().FindChatById(app, fixture.local.id);
		UAM_ASSERT(live != nullptr);
		if (change != Change::StorageOnly) *live = *newer;
		// StorageOnly models another authoritative writer while the selected
		// in-memory projection still contains the old one-message row.
		else UAM_ASSERT_EQ(live->messages.size(), std::size_t{1});
		if (change == Change::NativeBinding) app.resolved_native_sessions_by_chat_id[fixture.local.id] = newer->native_session_id;
		const auto expected_live_messages = live->messages;
		const auto expected_live_metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, *live);
		const auto expected_mappings = app.resolved_native_sessions_by_chat_id;
		const auto expected_primary = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id));
		UAM_ASSERT(!expected_primary.empty() && expected_primary != fixture.local_bytes);

		// Publication is owner-poll admitted. An already-prepared worker still
		// cannot acquire authority after the actual saved/live owner transition.
		fixture.DrainUntil([&] { return app.native_chat_refresh_error_target_id == kParent && !app.native_chat_refresh_error_status.empty(); });
		UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), expected_primary);
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		UAM_ASSERT(saved && saved->messages == newer->messages);
		UAM_ASSERT_EQ(saved->created_at, newer->created_at);
		UAM_ASSERT_EQ(saved->native_session_id, newer->native_session_id);
		live = ChatDomainService().FindChatById(app, fixture.local.id);
		UAM_ASSERT(live != nullptr && live->messages == expected_live_messages);
		UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *live), expected_live_metadata);
		for (const auto& message : saved->messages) UAM_ASSERT(message.attachments.empty());
		for (const auto& message : live->messages) UAM_ASSERT(message.attachments.empty());
		UAM_ASSERT(app.resolved_native_sessions_by_chat_id == expected_mappings);
		UAM_ASSERT_EQ(app.chats.size(), std::size_t{2});
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), fixture.local.id);
		UAM_ASSERT_EQ(app.composer_text, std::string("Keep selected draft"));
		UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), fixture.folders_before);
		for (const auto& [file, bytes] : fixture.unchanged_files) UAM_ASSERT_EQ(uam::io::ReadTextFile(file), bytes);
		UAM_ASSERT(app.cli_terminals.empty());
	}
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
UAM_TEST(GeminiTerminalFinalPriorityBlocksParsersUntilDurableSettlementThenReopens)
{
	GeminiTerminalSyncConsumerFixture fixture(1);
	auto& app = fixture.app;
	GeminiR6TerminalCleanup terminal_cleanup{app};
	GeminiR7TaskCleanup read_cleanup{app};
	auto& terminal = AddGeminiR6OwnedTerminal(fixture, false);
	const auto native_file = fixture.owned.chats / (kParent + ".jsonl");
	const auto before_native = uam::io::ReadTextFile(native_file);
	UAM_ASSERT_EQ(before_native, Journal()); // No inline originals or final text exist yet.
	GeminiPostStopMacChild child(terminal, native_file);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(native_file), before_native);
	UAM_ASSERT(uam::ChatHasActiveCliTerminal(app, fixture.local.id));
	UAM_ASSERT_EQ(uam::CliTerminalSyncTargetId(terminal), kParent);
	const auto complete = [&]
	{
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		const auto* live = ChatDomainService().SelectedChat(app);
		if (ChatHistorySyncService().TerminalNativeMediaPending(app) || !saved || !saved->messages_loaded || saved->messages.size() != 2 ||
		    saved->messages.back().content != "Answer written only after owned stop signal" ||
		    live == nullptr || !live->messages_loaded || live->messages != saved->messages) return false;
		std::size_t count = 0;
		for (const auto& message : saved->messages)
			for (const auto& attachment : message.attachments)
			{
				if (attachment.source != AttachmentSource::OwnedNativeAsset || attachment.native_asset_key.empty() || attachment.native_inline_payload) return false;
				++count;
			}
		return count == 2 && app.native_chat_refresh_error_status.empty();
	};
	UAM_ASSERT(!complete());
	// The actual captured final receipt has priority before parser allocation.
	// Never call TaskFor here: that helper itself would insert an empty task.
	uam::SyncCliTerminalToNativeHistory(app, terminal, true);
	UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
	UAM_ASSERT(app.native_chat_load_tasks.empty());
	const auto profile = ProviderProfileStore::DefaultGeminiProfile();
	UAM_ASSERT(!uam::StartAsyncNativeChatLoadForTerminal(app, profile, fixture.owned.chats));
	UAM_ASSERT(app.native_chat_load_tasks.empty());
	UAM_ASSERT_EQ(uam::io::ReadTextFile(native_file), before_native);
	siginfo_t before_stop{};
	UAM_ASSERT(waitid(P_PID, static_cast<id_t>(child.pid), &before_stop, WEXITED | WNOHANG | WNOWAIT) == 0 && before_stop.si_pid == 0);
	// Actual public final-capture -> normal platform stop/reap. Normal stop allows
	// HUP settlement; FastExit's consecutive HUP/TERM/KILL cannot promise it.
	uam::StopAllCliTerminals(app, false);
	child.AssertStopPatchWrittenAndReaped();
	UAM_ASSERT(!terminal.running && terminal.child_pid == -1 && terminal.master_fd == -1);
	UAM_ASSERT_EQ(terminal.attached_chat_id, fixture.local.id);
	UAM_ASSERT_EQ(terminal.attached_session_id, kParent);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(native_file), before_native + kGeminiPostStopJournalPatch);
	// Retained identity routes the already-stopped row through actual public
	// erase without another sync. No test-created replacement witness is used.
	UAM_ASSERT(uam::StopAndEraseCliTerminalForChat(app, fixture.local.id, false));
	UAM_ASSERT(app.cli_terminals.empty());
	UAM_ASSERT(!complete()); // No owner poll has admitted publication/adoption.
	for (auto& [file, expected] : fixture.unchanged_files) if (file == native_file) expected = before_native + kGeminiPostStopJournalPatch;
	const auto snapshot = ChatHistorySyncService().LoadNativeSessionChats(fixture.owned.chats, ProviderProfileStore::DefaultGeminiProfile());
	UAM_ASSERT(snapshot.size() == 1 && snapshot.front().messages.size() == 2);
	UAM_ASSERT_EQ(snapshot.front().native_session_id, kParent);
	UAM_ASSERT_EQ(snapshot.front().messages.back().content, std::string("Answer written only after owned stop signal"));
	std::vector<std::pair<std::size_t, MessageAttachment>> parts;
	for (std::size_t row = 0; row < snapshot.front().messages.size(); ++row)
		for (const auto& attachment : snapshot.front().messages[row].attachments)
		{
			UAM_ASSERT(attachment.source == AttachmentSource::NativeInlinePending && attachment.native_inline_payload);
			auto expected = attachment; expected.native_inline_payload.reset();
			parts.emplace_back(row, std::move(expected));
		}
	UAM_ASSERT_EQ(parts.size(), std::size_t{2});
	UAM_ASSERT(parts.front().second.native_tool_call_id.empty() && parts.back().second.native_tool_call_id == "t");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (!complete() && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::PollAllCliTerminals(nullptr, app, false);
		std::this_thread::yield();
	}
	UAM_ASSERT(complete());
	UAM_ASSERT(app.cli_terminals.empty() && app.native_chat_refresh_error_status.empty());
	fixture.AssertIdentityAndUnrelatedHistoryPreserved();
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
	UAM_ASSERT(saved && saved->messages.back().tool_calls.size() == 1 && saved->messages.back().tool_calls.front().id == "t");
	UAM_ASSERT_EQ(saved->messages.front().content, std::string("Native question"));
	UAM_ASSERT_EQ(saved->native_session_id, kParent); UAM_ASSERT_EQ(saved->provider_id, fixture.local.provider_id);
	UAM_ASSERT_EQ(saved->workspace_directory, fixture.owned.workspace); UAM_ASSERT_EQ(saved->execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(saved->messages.back().tool_calls.front().result_text, std::string("Post-stop tool result"));
	std::string original;
	UAM_ASSERT(uam::base64::Decode(kTerminalOriginalPng, original));
	for (const auto& [row, expected] : parts)
	{
		const auto& attachments = saved->messages[row].attachments;
		const auto found = std::ranges::find_if(attachments, [&](const auto& attachment) { return attachment.id == expected.id; });
		UAM_ASSERT(found != attachments.end() && found->source == AttachmentSource::OwnedNativeAsset);
		UAM_ASSERT(found->path.empty() && !found->native_inline_payload);
		UAM_ASSERT_EQ(found->name, expected.name); UAM_ASSERT_EQ(found->kind, expected.kind);
		UAM_ASSERT_EQ(found->native_part_key, expected.native_part_key); UAM_ASSERT_EQ(found->native_tool_call_id, expected.native_tool_call_id);
		UAM_ASSERT_EQ(found->native_source_field, expected.native_source_field); UAM_ASSERT_EQ(found->origin_execution_host_id, expected.origin_execution_host_id);
		UAM_ASSERT_EQ(found->mime_type, expected.mime_type); UAM_ASSERT_EQ(found->content_digest, expected.content_digest); UAM_ASSERT_EQ(found->size_bytes, expected.size_bytes);
		UAM_ASSERT(uam::native_assets::IsKeyForOwner(fixture.local.id, found->native_asset_key));
		UAM_ASSERT(!uam::native_assets::IsKeyForOwner(kParent, found->native_asset_key));
		const auto read = uam::native_assets::ReadOwned(app.data_root, fixture.local.id, found->native_asset_key);
		UAM_ASSERT(read.ok && read.error.empty()); UAM_ASSERT_EQ(read.bytes, original);
	}
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
	UAM_ASSERT(app.native_chat_load_tasks.empty());
	const auto committed_primary = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id));
	const auto committed_live_messages = ChatDomainService().SelectedChat(app)->messages;
	UAM_ASSERT(uam::StartAsyncNativeChatLoadForTerminal(app, profile, fixture.owned.chats));
	auto& fresh_task = uam::AsyncNativeChatLoadTaskFor(app, profile.id, fixture.owned.chats);
	UAM_ASSERT(fresh_task.running && fresh_task.worker && fresh_task.state);
	auto fresh_state = fresh_task.state;
	const auto parse_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (!fresh_state->completed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < parse_deadline) std::this_thread::yield();
	UAM_ASSERT(fresh_state->completed.load(std::memory_order_acquire));
	UAM_ASSERT(fresh_state->error.empty() && fresh_state->chats.size() == 1);
	const auto& fresh = fresh_state->chats.front();
	UAM_ASSERT_EQ(fresh.native_session_id, kParent); UAM_ASSERT_EQ(fresh.provider_id, profile.id);
	UAM_ASSERT_EQ(fresh.workspace_directory, fixture.owned.workspace);
	UAM_ASSERT_EQ(fresh.messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(fresh.messages.back().content, std::string("Answer written only after owned stop signal"));
	UAM_ASSERT(fresh.messages == snapshot.front().messages);
	for (const auto& [row, expected] : parts)
	{
		const auto& attachments = fresh.messages[row].attachments;
		const auto found = std::ranges::find_if(attachments, [&](const auto& value) { return value.id == expected.id; });
		UAM_ASSERT(found != attachments.end() && *found == expected);
		UAM_ASSERT(found->source == AttachmentSource::NativeInlinePending && found->native_inline_payload);
	}
	UAM_ASSERT(!fresh_state->snapshot_digest.empty());
	UAM_ASSERT_EQ(fresh_state->snapshot_digest, uam::NativeHistorySnapshotDigest(fresh_state->chats));
	fresh_state.reset(); // The real map/worker still owns cleanup; no copied raw snapshot is reimported.
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), committed_primary);
	UAM_ASSERT(ChatDomainService().SelectedChat(app)->messages == committed_live_messages);
	fixture.AssertIdentityAndUnrelatedHistoryPreserved();

}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(GeminiTerminalBorrowedOriginalsDeferAtCapacityWithoutConsumingSnapshot)
{
	GeminiTerminalOriginalFixture fixture;
	auto& app = fixture.app;
	const auto snapshot = fixture.Snapshot(); // Select the real borrowed const-snapshot overload.
	const auto before = snapshot;
	const auto before_digest = uam::NativeHistorySnapshotDigest(snapshot);
	const auto assert_borrowed_unchanged = [&]
	{
		UAM_ASSERT_EQ(snapshot.size(), before.size());
		UAM_ASSERT_EQ(uam::NativeHistorySnapshotDigest(snapshot), before_digest);
		for (std::size_t row = 0; row < snapshot.size(); ++row)
		{
			UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(snapshot[row]), uam::chat_identity::NativeIdentityKeyForHistoryImport(before[row]));
			UAM_ASSERT(snapshot[row].messages == before[row].messages);
			for (std::size_t message = 0; message < snapshot[row].messages.size(); ++message)
				for (std::size_t part = 0; part < snapshot[row].messages[message].attachments.size(); ++part)
				{
					const auto& attachment = snapshot[row].messages[message].attachments[part];
					UAM_ASSERT(attachment.source == AttachmentSource::NativeInlinePending && attachment.native_inline_payload);
					UAM_ASSERT(attachment.native_inline_payload == before[row].messages[message].attachments[part].native_inline_payload);
				}
		}
	};
	// Real lifetime leases occupy the shared documented capacity. They contain
	// no fabricated parser state, prepared files or completion/publication receipt.
	struct CapacityLease {};
	const auto acquire = []
	{
		std::shared_ptr<CapacityLease> lease;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		do
		{
			lease = uam::native_history::TryCreateWorkerOwnedNativeRefresh<CapacityLease>();
			if (!lease) std::this_thread::yield();
		} while (!lease && std::chrono::steady_clock::now() < deadline);
		UAM_ASSERT(lease);
		return lease;
	};
	auto first = acquire();
	auto second = acquire();
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(app.native_chat_refresh_error_status.empty());
	UAM_ASSERT(!uam::SyncChatsFromLoadedNative(app, snapshot, kParent, true));
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
	UAM_ASSERT(app.native_chat_refresh_error_status.empty() && app.native_chat_load_tasks.empty());
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), fixture.local_bytes);
	UAM_ASSERT(ChatDomainService().SelectedChat(app)->messages == fixture.local.messages);
	assert_borrowed_unchanged();
	fixture.AssertIdentityAndUnrelatedHistoryPreserved();
	first.reset(); second.reset(); // Real asynchronous lease retirement releases capacity.

	bool admitted = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	do
	{
		// This is the explicit capacity-admission retry control. Once a real job
		// exists, no further Sync call is permitted or needed for its completion.
		UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
		UAM_ASSERT(!uam::SyncChatsFromLoadedNative(app, snapshot, kParent, true));
		admitted = ChatHistorySyncService().TerminalNativeMediaPending(app);
		UAM_ASSERT(app.native_chat_refresh_error_status.empty());
		if (!admitted) std::this_thread::yield();
	} while (!admitted && std::chrono::steady_clock::now() < deadline);
	UAM_ASSERT(admitted);
	assert_borrowed_unchanged();
	fixture.AwaitPublished(); // Actual saved/live originals plus ACK/fsync settlement.
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	(void)fixture.AssertPublished();
	assert_borrowed_unchanged();
}

UAM_TEST(GeminiTerminalFailedNewLaunchRevokesPendingOriginalsBeforeOwnerPoll)
{
	GeminiTerminalOriginalFixture fixture;
	auto& app = fixture.app;
	GeminiR6TerminalCleanup cleanup{app};
	auto* live = ChatDomainService().FindChatById(app, fixture.local.id);
	UAM_ASSERT(live != nullptr);
	// This durable closing intent exists before media capture: changing metadata
	// after capture must not be what makes the actual launch-attempt test pass.
	live->side_cleanup_requested = true;
	live->title = "Preserve newer closing intent";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *live));
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
	UAM_ASSERT(saved && saved->side_cleanup_requested && saved->messages == fixture.local.messages);
	*live = *saved;
	fixture.local = *saved;
	fixture.local_bytes = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id));
	const auto expected_messages = live->messages;
	const auto expected_metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, *live);
	const auto mappings = app.resolved_native_sessions_by_chat_id;
	const auto snapshot = fixture.Snapshot();
	UAM_ASSERT(!uam::SyncChatsFromLoadedNative(app, snapshot, kParent, true));
	UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(app.native_chat_refresh_error_status.empty());
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), fixture.local_bytes);

	// No process is launched or fabricated. This stopped metadata terminal makes
	// a real public launch attempt, which revokes before its closing prerequisite.
	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->terminal_id = "failed-new-launch-originals";
	terminal->frontend_chat_id = terminal->attached_chat_id = fixture.local.id;
	terminal->attached_session_id = kParent;
	app.cli_terminals.push_back(std::move(terminal));
	auto& attempted = *app.cli_terminals.back();
	const auto previous_generation = attempted.native_history_generation;
	UAM_ASSERT(previous_generation && !attempted.running);
	UAM_ASSERT(!uam::StartCliTerminalForChat(app, attempted, *live, 24, 80));
	UAM_ASSERT(!attempted.running && attempted.lifecycle_state == uam::CliTerminalLifecycleState::Disabled);
	UAM_ASSERT(attempted.native_history_generation && attempted.native_history_generation != previous_generation);
	UAM_ASSERT(previous_generation->revoked.load(std::memory_order_acquire));
	UAM_ASSERT(!PlatformServicesFactory::Instance().terminal_runtime.HasReadableTerminalOutputHandle(attempted));
	UAM_ASSERT(app.acp_sessions.empty() && app.native_chat_load_tasks.empty());
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), fixture.local_bytes);
	UAM_ASSERT(live->messages == expected_messages && live->side_cleanup_requested);
	UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *live), expected_metadata);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (ChatHistorySyncService().TerminalNativeMediaPending(app) && std::chrono::steady_clock::now() < deadline)
	{
		(void)ChatHistorySyncService().PollTerminalNativeMedia(app);
		std::this_thread::yield();
	}
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(app.native_chat_refresh_error_status.empty());
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), fixture.local_bytes);
	live = ChatDomainService().FindChatById(app, fixture.local.id);
	UAM_ASSERT(live != nullptr && live->side_cleanup_requested && live->messages == expected_messages);
	UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *live), expected_metadata);
	for (const auto& message : live->messages) UAM_ASSERT(message.attachments.empty());
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
	fixture.AssertIdentityAndUnrelatedHistoryPreserved();
	UAM_ASSERT(!attempted.running && attempted.lifecycle_state == uam::CliTerminalLifecycleState::Disabled);
}
#endif

// Completion-aware compatibility and exit-primitive controls. This packet does
// not claim the whole CEF Quit journey or attest filtered malformed native data.
#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(GeminiTerminalFinalEmptyOrAbsentSourceSettlesWithoutChangingSavedHistory)
{
	for (const bool absent : {false, true})
	{
		GeminiTerminalSyncConsumerFixture fixture(1);
		auto& app = fixture.app;
		GeminiR6TerminalCleanup terminal_cleanup{app};
		const auto native_file = fixture.owned.chats / (kParent + ".jsonl");
		std::string source_bytes;
		if (absent)
		{
			UAM_ASSERT(fs::remove(native_file));
			std::erase_if(fixture.unchanged_files, [&](const auto& entry) { return entry.first == native_file; });
			UAM_ASSERT(!fs::exists(native_file));
		}
		else
		{
			// A single header is read as a legacy document and lacks a required
			// message collection; it must not attest a confirmed empty history.
			UAM_ASSERT(uam::io::WriteTextFile(native_file, Line(Header())));
			const auto header_only = GeminiJsonHistoryStore::ParseFileResult(native_file, ProviderProfileStore::DefaultGeminiProfile());
			UAM_ASSERT(header_only.status == GeminiHistoryParseStatus::UnsupportedOrInvalid && !header_only.chat && !header_only.error.empty());
			// An actual second journal record explicitly replaces history with
			// an empty collection. The rich parser must confirm that final state.
			source_bytes = Line(Header()) + Line({{"$set", {{"messages", Json::array()}}}});
			UAM_ASSERT(uam::io::WriteTextFile(native_file, source_bytes));
			const auto parsed = GeminiJsonHistoryStore::ParseFileResult(native_file, ProviderProfileStore::DefaultGeminiProfile());
			UAM_ASSERT(parsed.status == GeminiHistoryParseStatus::ConfirmedEmpty && !parsed.chat && parsed.error.empty());
			for (auto& [file, expected] : fixture.unchanged_files) if (file == native_file) expected = source_bytes;
		}
		const auto snapshot = ChatHistorySyncService().LoadNativeSessionChats(fixture.owned.chats, ProviderProfileStore::DefaultGeminiProfile());
		UAM_ASSERT(snapshot.empty()); // Neither source disposition grants new history.
		const auto primary_before = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id));
		const auto saved_before = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		const auto* live_before = ChatDomainService().SelectedChat(app);
		UAM_ASSERT(saved_before && live_before && live_before->messages == saved_before->messages && saved_before->messages.size() == 1);
		const auto expected_messages = live_before->messages;
		const auto expected_metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, *live_before);
		const auto expected_birth = live_before->metadata_incarnation;
		const auto expected_mappings = app.resolved_native_sessions_by_chat_id;
		UAM_ASSERT(!expected_metadata.empty() && app.native_chat_refresh_error_status.empty());
		(void)AddGeminiR6OwnedTerminal(fixture, false);
		// This real public close captures the existing UAM/native owner before
		// stopping/erasing the metadata-only fixture's invalid platform handles.
		UAM_ASSERT(uam::StopAndEraseCliTerminalForChat(app, fixture.local.id, true));
		UAM_ASSERT(app.cli_terminals.empty());
		UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaPending(app));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (ChatHistorySyncService().TerminalNativeMediaPending(app) && std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::PollAllCliTerminals(nullptr, app, false);
			std::this_thread::yield();
		}
		UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
		UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
		UAM_ASSERT(app.native_chat_refresh_error_status.empty() && app.native_chat_refresh_error_target_id.empty());
		UAM_ASSERT(app.cli_terminals.empty() && app.pending_chat_save_at_by_chat_id.empty());
		UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), primary_before);
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		const auto* live = ChatDomainService().SelectedChat(app);
		UAM_ASSERT(saved && live && saved->messages == expected_messages && live->messages == expected_messages);
		UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *saved), ChatRepository::ChatMetadataFingerprint(app.data_root, *saved_before));
		UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *live), expected_metadata);
		UAM_ASSERT(live->metadata_incarnation == expected_birth);
		UAM_ASSERT(app.resolved_native_sessions_by_chat_id == expected_mappings);
		fixture.AssertIdentityAndUnrelatedHistoryPreserved(); // Includes draft/folder/native-source controls.
		if (absent) UAM_ASSERT(!fs::exists(native_file));
		else UAM_ASSERT_EQ(uam::io::ReadTextFile(native_file), source_bytes);
	}
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
UAM_TEST(GeminiTerminalFastExitSettlementPublishesOriginalsWithoutOrdinaryPolling)
{
	GeminiTerminalOriginalFixture fixture;
	auto& app = fixture.app;
	GeminiR6TerminalCleanup terminal_cleanup{app};
	GeminiR7TaskCleanup read_cleanup{app};
	auto& terminal = AddGeminiR6OwnedTerminal(fixture, false);
	GeminiR7MacChild child(terminal, true); // Real private PTY child; never execs a provider.
	child.AssertRunning();
	const pid_t owned_pid = terminal.child_pid;
	UAM_ASSERT(owned_pid > 0 && terminal.running);
	const auto native_file = fixture.owned.chats / (kParent + ".jsonl");
	const auto native_before = uam::io::ReadTextFile(native_file);
	UAM_ASSERT(!native_before.empty() && !fixture.HasPublished());
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	// Exact service sequence used after ordinary application polling has stopped.
	// FastExit makes no promise that a native signal handler can append content;
	// the real private source already contains both retained inline originals.
	uam::FastStopCliTerminalsForExit(app);
	UAM_ASSERT(!terminal.running && terminal.child_pid == -1 && terminal.master_fd == -1);
	UAM_ASSERT(terminal.attached_chat_id.empty() && terminal.attached_session_id.empty() && terminal.frontend_chat_id.empty() && terminal.terminal_id.empty());
	int status = 0;
	errno = 0;
	UAM_ASSERT(waitpid(owned_pid, &status, WNOHANG) == -1 && errno == ECHILD);
	UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
	UAM_ASSERT(!fixture.HasPublished());
	// No PollAllCliTerminals/PollCliTerminal/AwaitPublished after shutdown intent:
	// the explicit exit-settlement primitive must own actual publication + ACK.
	ChatHistorySyncService().SettleTerminalNativeMediaForExit(app);
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
	UAM_ASSERT(fixture.HasPublished() && app.native_chat_refresh_error_status.empty() && app.native_chat_refresh_error_target_id.empty());
	UAM_ASSERT(app.cli_terminals.size() == 1 && app.cli_terminals.front().get() == &terminal && !terminal.running);
	UAM_ASSERT(terminal.attached_chat_id.empty() && terminal.attached_session_id.empty() && terminal.frontend_chat_id.empty() && terminal.terminal_id.empty());
	(void)fixture.AssertPublished(); // Exact saved+live message/tool associations and owned original bytes.
	UAM_ASSERT_EQ(uam::io::ReadTextFile(native_file), native_before);
}
#endif

// Claude R1 P2 regressions: final import rebases metadata, while newer authored
// work/identity stays authoritative; exit never waits on a still-running owner.
#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	void AssertTerminalFinalMetadataPreserved(const ChatSession& actual, const ChatSession& expected)
	{
		UAM_ASSERT_EQ(actual.id, expected.id);
		UAM_ASSERT_EQ(actual.created_at, expected.created_at);
		UAM_ASSERT_EQ(actual.provider_id, expected.provider_id);
		UAM_ASSERT_EQ(actual.native_session_id, expected.native_session_id);
		UAM_ASSERT_EQ(actual.execution_host_id, expected.execution_host_id);
		UAM_ASSERT_EQ(actual.workspace_directory, expected.workspace_directory);
		UAM_ASSERT_EQ(actual.model_id, expected.model_id);
		UAM_ASSERT_EQ(actual.reasoning_effort, expected.reasoning_effort);
		UAM_ASSERT_EQ(actual.last_opened_at, expected.last_opened_at);
		UAM_ASSERT_EQ(actual.interaction_at, expected.interaction_at);
		UAM_ASSERT_EQ(actual.attention_revision, expected.attention_revision);
		UAM_ASSERT_EQ(actual.settled_at, expected.settled_at);
		UAM_ASSERT_EQ(actual.snoozed_until_ms, expected.snoozed_until_ms);
		UAM_ASSERT_EQ(actual.title, expected.title);
		UAM_ASSERT_EQ(actual.folder_id, expected.folder_id);
		UAM_ASSERT_EQ(actual.pinned, expected.pinned);
	}
}

UAM_TEST(GeminiTerminalFinalOriginalsRebaseMetadataWithoutOverridingNewWorkOrIdentity)
{
	enum class Change { ModelMetadata, SidebarMetadata, AuthoredMessage, NativeBinding, Birth };
	for (const auto change : {Change::ModelMetadata, Change::SidebarMetadata, Change::AuthoredMessage, Change::NativeBinding, Change::Birth})
	{
		GeminiTerminalOriginalFixture fixture;
		auto& app = fixture.app;
		const auto captured_birth = ChatDomainService().SelectedChat(app)->metadata_incarnation;
		const auto initial_primary = fixture.local_bytes;
		fixture.CloseAfterFinalCapture(); // One actual public final capture/stop/erase, no owner poll yet.
		UAM_ASSERT(app.cli_terminals.empty());
		UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaPending(app));
		UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
		UAM_ASSERT(!fixture.HasPublished() && app.native_chat_refresh_error_status.empty());
		auto* live = ChatDomainService().FindChatById(app, fixture.local.id);
		UAM_ASSERT(live && live->messages == fixture.local.messages && live->metadata_incarnation == captured_birth);
		if (change == Change::ModelMetadata)
		{
			// Mirrors a successful model-option save after its sync-requested stop.
			live->model_id = "newer-user-selected-model";
			live->reasoning_effort = "high";
			live->updated_at = "2026-10-16T13:00:00Z";
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *live));
		}
		else if (change == Change::SidebarMetadata)
		{
			// The real sidebar metadata writer changes the summary, not the primary.
			live->last_opened_at = "2026-10-17T12:00:00Z";
			live->interaction_at = "2026-10-17T12:00:00.000Z";
			live->attention_revision = "post-capture-attention";
			live->settled_at = "2026-10-17T11:59:00.000Z";
			live->snoozed_until_ms = 1792251000000LL;
			UAM_ASSERT(ChatRepository::SaveLastOpenedAt(app.data_root, *live));
			UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), initial_primary);
		}
		else
		{
			// Independent negative controls: native/birth drift does not borrow a
			// newer-message difference to make rejection pass accidentally.
			if (change == Change::AuthoredMessage)
			{
				Message newer{MessageRole::User, "New authoritative work after the terminal closed"};
				newer.created_at = "2026-10-16T14:00:00Z";
				live->messages.push_back(std::move(newer));
				live->updated_at = "2026-10-16T14:00:00Z";
			}
			if (change == Change::NativeBinding) live->native_session_id = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
			if (change == Change::Birth) live->created_at = "2031-04-05T06:07:08Z";
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *live));
			if (change == Change::NativeBinding) app.resolved_native_sessions_by_chat_id[live->id] = live->native_session_id;
		}
		const ChatSession expected = *live;
		const auto saved_change = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		const auto summary_change = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, fixture.local.id);
		UAM_ASSERT(saved_change && summary_change && saved_change->messages == expected.messages);
		AssertTerminalFinalMetadataPreserved(*saved_change, expected);
		AssertTerminalFinalMetadataPreserved(*summary_change, expected);
		UAM_ASSERT(live->metadata_incarnation == captured_birth); // Metadata edits did not recreate the live owner.
		const auto metadata_after_change = ChatRepository::ChatMetadataFingerprint(app.data_root, *live);
		const auto primary_after_change = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id));
		const auto mappings_after_change = app.resolved_native_sessions_by_chat_id;
		UAM_ASSERT(!metadata_after_change.empty());
		if (change == Change::ModelMetadata || change == Change::SidebarMetadata)
		{
			fixture.AwaitPublished(); // Only the existing job is polled; never a second Sync request.
			(void)fixture.AssertPublished();
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
			const auto summary = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, fixture.local.id);
			const auto* selected = ChatDomainService().SelectedChat(app);
			UAM_ASSERT(saved && summary && selected);
			for (const ChatSession* actual : {&*saved, &*summary, selected})
			{
				AssertTerminalFinalMetadataPreserved(*actual, expected);
				if (change == Change::ModelMetadata) UAM_ASSERT_EQ(actual->updated_at, expected.updated_at);
				else UAM_ASSERT(actual->updated_at >= expected.updated_at); // Untouched history time may advance with the final answer.
			}
			UAM_ASSERT(selected->metadata_incarnation == captured_birth);
			UAM_ASSERT_EQ(saved->messages.back().model_id, std::string("native-model")); // Historical response model is not rewritten.
			UAM_ASSERT(app.native_chat_refresh_error_status.empty() && app.native_chat_refresh_error_target_id.empty());
		}
		else
		{
			fixture.DrainUntil([&] { return !ChatHistorySyncService().TerminalNativeMediaPending(app); });
			UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), primary_after_change);
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
			live = ChatDomainService().FindChatById(app, fixture.local.id);
			UAM_ASSERT(saved && live && saved->messages == expected.messages && live->messages == expected.messages);
			AssertTerminalFinalMetadataPreserved(*saved, expected);
			AssertTerminalFinalMetadataPreserved(*live, expected);
			UAM_ASSERT_EQ(saved->updated_at, expected.updated_at);
			UAM_ASSERT_EQ(live->updated_at, expected.updated_at);
			UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *live), metadata_after_change);
			UAM_ASSERT(live->metadata_incarnation == captured_birth);
			for (const auto& message : live->messages) UAM_ASSERT(message.attachments.empty());
			UAM_ASSERT(app.chats.size() == 2 && ChatDomainService().SelectedChatId(app) == fixture.local.id);
			UAM_ASSERT_EQ(app.composer_text, std::string("Keep selected draft"));
			UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), fixture.folders_before);
			for (const auto& [file, bytes] : fixture.unchanged_files) UAM_ASSERT_EQ(uam::io::ReadTextFile(file), bytes);
		}
		UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings_after_change);
		UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app) && app.cli_terminals.empty());
	}
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
UAM_TEST(GeminiTerminalExitSettlementReleasesUnstoppedFinalWithoutWaitingForDeadline)
{
	GeminiTerminalOriginalFixture fixture;
	auto& app = fixture.app;
	GeminiR6TerminalCleanup terminal_cleanup{app};
	GeminiR7TaskCleanup read_cleanup{app};
	auto& terminal = AddGeminiR6OwnedTerminal(fixture, false);
	GeminiR7MacChild child(terminal, true);
	child.AssertRunning();
	const pid_t owned_pid = terminal.child_pid;
	const auto original_generation = terminal.native_history_generation;
	const auto primary_before = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id));
	const auto expected_messages = ChatDomainService().SelectedChat(app)->messages;
	const auto expected_metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, *ChatDomainService().SelectedChat(app));
	const auto mappings = app.resolved_native_sessions_by_chat_id;
	UAM_ASSERT(owned_pid > 0 && original_generation && !original_generation->stopped.load(std::memory_order_acquire));
	uam::SyncCliTerminalToNativeHistory(app, terminal, true); // Genuine final wait; deliberately no Stop before exit settlement.
	UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaPending(app) && ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
	UAM_ASSERT(!fixture.HasPublished() && app.native_chat_refresh_error_status.empty());
	child.AssertRunning();
	const auto started = std::chrono::steady_clock::now();
	ChatHistorySyncService().SettleTerminalNativeMediaForExit(app);
	const auto elapsed = std::chrono::steady_clock::now() - started;
	UAM_ASSERT(elapsed < std::chrono::seconds(2)); // A no-progress receipt must not burn the existing 15-second work deadline.
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app) && !ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
	child.AssertRunning(); // The settlement primitive must not fabricate stop evidence or send provider input.
	UAM_ASSERT(terminal.running && terminal.child_pid == owned_pid && terminal.native_history_generation == original_generation);
	UAM_ASSERT(!original_generation->stopped.load(std::memory_order_acquire));
	UAM_ASSERT_EQ(terminal.attached_chat_id, fixture.local.id);
	UAM_ASSERT_EQ(terminal.attached_session_id, kParent);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), primary_before);
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
	const auto* live = ChatDomainService().SelectedChat(app);
	UAM_ASSERT(saved && live && saved->messages == expected_messages && live->messages == expected_messages);
	UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *live), expected_metadata);
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
	UAM_ASSERT(!fixture.HasPublished());
	fixture.AssertIdentityAndUnrelatedHistoryPreserved(); // Includes exact source bytes and unsent draft.
	// Only now perform actual owned normal cleanup. No synthetic process state,
	// ordinary Poll, second final capture or full CEF Quit can rescue the oracle.
	uam::StopCliTerminal(terminal, false, uam::CliTerminalStopMode::Graceful);
	UAM_ASSERT(!terminal.running && terminal.child_pid == -1 && terminal.master_fd == -1);
	int status = 0;
	errno = 0;
	UAM_ASSERT(waitpid(owned_pid, &status, WNOHANG) == -1 && errno == ECHILD);
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(GeminiTerminalFinalSameCountOriginalsSurviveNewModelAndOrganizationMetadata)
{
	for (const bool organization : {false, true})
	{
		GeminiTerminalOriginalFixture fixture;
		auto& app = fixture.app;
		auto* live = ChatDomainService().FindChatById(app, fixture.local.id);
		UAM_ASSERT(live && live->messages.size() == 1);
		// Persist a real older2-message native projection. The current provider
		// source has the SAME count, but changed assistant text plus user/tool PNG.
		Message old_answer{MessageRole::Assistant, "Previously saved native answer"};
		old_answer.provider = "gemini-cli";
		old_answer.model_id = "native-model";
		live->messages.push_back(std::move(old_answer));
		live->updated_at = "2026-10-16T12:00:00Z";
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *live));
		const auto baseline = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		UAM_ASSERT(baseline && baseline->messages.size() == 2 && baseline->messages.back().content == "Previously saved native answer");
		for (const auto& message : baseline->messages) UAM_ASSERT(message.attachments.empty());
		*live = *baseline;
		fixture.local = *baseline;
		fixture.local_bytes = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id));
		const auto original_messages = live->messages;
		const auto original_birth = live->metadata_incarnation;
		const auto source = fixture.Snapshot();
		UAM_ASSERT(source.size() == 1 && source.front().messages.size() == original_messages.size());
		UAM_ASSERT_EQ(source.front().messages.back().content, fixture.expected_answer);
		UAM_ASSERT(source.front().updated_at > live->updated_at && source.front().messages != original_messages);
		UAM_ASSERT(!fixture.HasPublished());
		fixture.CloseAfterFinalCapture(); // One public final capture, no owner poll before the actual metadata save.
		UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaPending(app) && ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
		UAM_ASSERT(app.cli_terminals.empty() && app.native_chat_refresh_error_status.empty());
		live = ChatDomainService().FindChatById(app, fixture.local.id);
		UAM_ASSERT(live && live->metadata_incarnation == original_birth && live->messages == original_messages);
		if (organization)
		{
			live->title = "Newest organized terminal chat";
			live->pinned = false;
			live->custom_icon = {"text", "R"};
		}
		else
		{
			live->model_id = "newer-user-selected-model";
			live->reasoning_effort = "high";
		}
		live->updated_at = "2026-10-16T13:00:00Z"; // New UAM metadata time exceeds native12:00:01, without new authored text.
		UAM_ASSERT(live->updated_at > source.front().updated_at);
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *live));
		const auto saved_change = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		UAM_ASSERT(saved_change && saved_change->messages == original_messages && saved_change->messages.size() == 2);
		UAM_ASSERT_EQ(saved_change->updated_at, live->updated_at);
		if (organization)
		{
			UAM_ASSERT_EQ(saved_change->title, std::string("Newest organized terminal chat"));
			UAM_ASSERT(!saved_change->pinned && saved_change->custom_icon == live->custom_icon);
		}
		else UAM_ASSERT_EQ(saved_change->model_id, std::string("newer-user-selected-model"));
		const ChatSession expected = *live;
		fixture.local = expected; // Generic identity helper must compare the actual current title/pin expectation.
		const auto mappings = app.resolved_native_sessions_by_chat_id;
		UAM_ASSERT(!fixture.HasPublished()); // Same count/old2-message primary cannot satisfy final publication.
		fixture.AwaitPublished(); // No retry Sync: the admitted final must publish, adopt and ACK its exact current originals.
		(void)fixture.AssertPublished();
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		const auto summary = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, fixture.local.id);
		const auto* selected = ChatDomainService().SelectedChat(app);
		UAM_ASSERT(saved && summary && selected && selected->metadata_incarnation == original_birth);
		for (const ChatSession* actual : {&*saved, &*summary, selected})
		{
			UAM_ASSERT_EQ(actual->model_id, expected.model_id);
			UAM_ASSERT_EQ(actual->reasoning_effort, expected.reasoning_effort);
			UAM_ASSERT_EQ(actual->updated_at, expected.updated_at);
			UAM_ASSERT_EQ(actual->title, expected.title);
			UAM_ASSERT_EQ(actual->pinned, expected.pinned);
			UAM_ASSERT(actual->custom_icon == expected.custom_icon);
			UAM_ASSERT_EQ(actual->created_at, expected.created_at);
			UAM_ASSERT_EQ(actual->provider_id, expected.provider_id);
			UAM_ASSERT_EQ(actual->native_session_id, expected.native_session_id);
			UAM_ASSERT_EQ(actual->workspace_directory, expected.workspace_directory);
		}
		UAM_ASSERT_EQ(saved->messages.back().model_id, std::string("native-model"));
		UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
		UAM_ASSERT(app.cli_terminals.empty() && !ChatHistorySyncService().TerminalNativeMediaPending(app));
		UAM_ASSERT(app.native_chat_refresh_error_status.empty() && app.native_chat_refresh_error_target_id.empty());
	}
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(GeminiTerminalPendingFinalBlocksWorkspaceMoveWithoutMutation)
{
	GeminiTerminalOriginalFixture fixture;
	auto& app = fixture.app;
	const auto destination = fixture.owned.temp.root / "pending-final-move-destination";
	fs::create_directories(destination);
	ChatFolder target;
	target.id = "pending-final-target-folder";
	target.title = "Different private workspace";
	target.directory = uam::paths::Utf8PathString(destination);
	target.execution_host_id = "local";
	app.folders.push_back(target);
	UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
	fixture.folders_before = ChatFolderStore::Serialize(app.folders);
	const auto settings_file = AppPaths::SettingsFilePath(app.data_root);
	UAM_ASSERT(SettingsStore::Save(settings_file, app.settings));
	const auto settings_before = uam::io::ReadTextFile(settings_file);
	UAM_ASSERT(!settings_before.empty());
	fixture.CloseAfterFinalCapture(); // Actual final capture/stop/erase, no owner poll.
	UAM_ASSERT(app.cli_terminals.empty());
	UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
	UAM_ASSERT(!fixture.HasPublished() && app.native_chat_refresh_error_status.empty());
	auto* live = ChatDomainService().FindChatById(app, fixture.local.id);
	UAM_ASSERT(live && live->messages_loaded && live->workspace_directory != target.directory);
	const auto messages_before = live->messages;
	const auto metadata_before = ChatRepository::ChatMetadataFingerprint(app.data_root, *live);
	const auto mappings_before = app.resolved_native_sessions_by_chat_id;
	const auto selected_before = ChatDomainService().SelectedChatId(app);
	const auto draft_before = app.composer_text;
	const auto folders_before = ChatFolderStore::Serialize(app.folders);
	const auto pending_before = app.move_chat_pending_id;
	const auto original_folder_before = app.move_chat_original_folder_id;
	const auto original_workspace_before = app.move_chat_original_workspace;
	const auto target_folder_before = app.move_chat_target_folder_id;
	const auto target_workspace_before = app.move_chat_target_workspace;
	const auto warning_before = app.move_chat_show_missing_session_warning;
	// Tiny private fixture only: capture every source/primary/settings byte and
	// file membership so a failed later rebuild cannot masquerade as a barrier.
	const auto private_files = [&]
	{
		std::map<fs::path, std::string> files;
		for (const auto& entry : fs::recursive_directory_iterator(fixture.owned.temp.root))
			if (entry.is_regular_file()) files.emplace(entry.path(), uam::io::ReadTextFile(entry.path()));
		return files;
	};
	const auto files_before = private_files();
	UAM_ASSERT(!files_before.empty());
	// A no-op selection of the current folder remains valid while final is pending.
	UAM_ASSERT(ChatHistorySyncService().MoveChatToFolder(app, *live, live->folder_id));
	const bool moved = ChatHistorySyncService().MoveChatToFolder(app, *live, target.id);
	// Assert side-effect refusal before the return/status oracle: R6 can refuse
	// at a later native rebuild after already mutating this move bookkeeping.
	UAM_ASSERT_EQ(app.move_chat_pending_id, pending_before);
	UAM_ASSERT_EQ(app.move_chat_original_folder_id, original_folder_before);
	UAM_ASSERT_EQ(app.move_chat_original_workspace, original_workspace_before);
	UAM_ASSERT_EQ(app.move_chat_target_folder_id, target_folder_before);
	UAM_ASSERT_EQ(app.move_chat_target_workspace, target_workspace_before);
	UAM_ASSERT_EQ(app.move_chat_show_missing_session_warning, warning_before);
	UAM_ASSERT(private_files() == files_before);
	UAM_ASSERT(!moved);
	UAM_ASSERT(app.status_line.find("history") != std::string::npos && app.status_line.find("Retry") != std::string::npos);
	live = ChatDomainService().FindChatById(app, fixture.local.id);
	UAM_ASSERT(live && live->messages == messages_before);
	UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *live), metadata_before);
	UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), folders_before);
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings_before);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), selected_before);
	UAM_ASSERT_EQ(app.composer_text, draft_before);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(settings_file), settings_before);
	const auto serialized_after = fixture.owned.temp.root / "move-settings-after-assertion.txt";
	UAM_ASSERT(SettingsStore::Save(serialized_after, app.settings));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(serialized_after), settings_before); // In-memory settings also unchanged.
	UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaPending(app) && app.cli_terminals.empty());
	fixture.AwaitPublished(); // Drain only the original final, never recapture or request another import.
	UAM_ASSERT_EQ(fixture.AssertPublished().size(), std::size_t{2}); // Actual owned PNG bytes/associations and selected live state.
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
	UAM_ASSERT(app.cli_terminals.empty() && app.native_chat_refresh_error_status.empty());
	UAM_ASSERT_EQ(app.move_chat_pending_id, pending_before);
	UAM_ASSERT_EQ(app.move_chat_original_folder_id, original_folder_before);
	UAM_ASSERT_EQ(app.move_chat_original_workspace, original_workspace_before);
	UAM_ASSERT_EQ(app.move_chat_target_folder_id, target_folder_before);
	UAM_ASSERT_EQ(app.move_chat_target_workspace, target_workspace_before);
	UAM_ASSERT_EQ(app.move_chat_show_missing_session_warning, warning_before);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(settings_file), settings_before);
	// Cross-workspace media migration after settlement is a separate contract:
	// do not call it here or claim that this pending barrier proves it succeeds.
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(GeminiTerminalColdFinalCannotRebaseSameCountSameTimeAuthoredPrimary)
{
	GeminiTerminalOriginalFixture fixture;
	auto& app = fixture.app;
	auto* live = ChatDomainService().FindChatById(app, fixture.local.id);
	UAM_ASSERT(live && live->messages_loaded && live->messages.size() == 1);
	Message old_answer{MessageRole::Assistant, "Previously saved plain answer"};
	old_answer.provider = "gemini-cli";
	old_answer.model_id = "native-model";
	live->messages.push_back(std::move(old_answer));
	live->updated_at = "2026-10-16T12:00:00Z";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *live));
	const auto baseline = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
	UAM_ASSERT(baseline && baseline->messages_loaded && baseline->messages.size() == 2);
	for (const auto& message : baseline->messages) UAM_ASSERT(message.attachments.empty());
	*live = *baseline;
	fixture.local = *baseline;
	fixture.local_bytes = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id));
	const auto native = fixture.Snapshot();
	UAM_ASSERT(native.front().messages.size() == baseline->messages.size() && fixture.parts.size() == 2);
	UAM_ASSERT(native.front().updated_at > baseline->updated_at && native.front().messages != baseline->messages);

	// Use the supported release service BEFORE final capture; never manufacture
	// a cold owner by toggling messages_loaded or clearing its message vector.
	const auto draft = app.composer_text;
	ChatDomainService().SelectChatById(app, fixture.unrelated.id);
	uam::TranscriptRetentionService retention;
	retention.SetVisibleChatIds({fixture.unrelated.id});
	const auto release = retention.BeginRelease(app, fixture.local.id);
	UAM_ASSERT(release.has_value());
	UAM_ASSERT(retention.CompleteRelease(app, fixture.local.id, *release, *baseline));
	ChatDomainService().SelectChatById(app, fixture.local.id); // Domain selection does not hydrate.
	app.composer_text = draft;
	live = ChatDomainService().FindChatById(app, fixture.local.id);
	UAM_ASSERT(live && !live->messages_loaded && live->messages.empty() && live->persisted_message_count == 2);
	const ChatSession cold = *live;
	const auto cold_metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, cold);
	const auto before_revision = ChatRepository::ChatStorageRevision(app.data_root, cold.id);
	const auto mappings = app.resolved_native_sessions_by_chat_id;
	UAM_ASSERT(before_revision && *before_revision != "missing" && !cold_metadata.empty());
	fixture.CloseAfterFinalCapture(); // Actual capture/stop/erase; no owner poll yet.
	UAM_ASSERT(app.cli_terminals.empty() && ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaFinalPending(app) && app.native_chat_refresh_error_status.empty());

	// A genuine storage-only authored rewrite advances the primary revision but
	// deliberately preserves the weak timestamp/count-derived cached facts.
	// The cold live owner is not updated, hydrated or replaced by this save.
	ChatSession newer = *baseline;
	newer.messages.front().content = "New authoritative stored question";
	newer.messages.back().content = "New authoritative stored answer";
	UAM_ASSERT(newer.messages.size() == native.front().messages.size() && newer.updated_at == cold.updated_at);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, newer));
	const auto rewritten = ChatRepository::LoadLocalChat(app.data_root, cold.id, true);
	const auto after_revision = ChatRepository::ChatStorageRevision(app.data_root, cold.id);
	UAM_ASSERT(rewritten && after_revision && after_revision != before_revision);
	UAM_ASSERT(rewritten->messages == newer.messages && rewritten->messages != baseline->messages);
	UAM_ASSERT_EQ(rewritten->updated_at, cold.updated_at);
	UAM_ASSERT_EQ(rewritten->persisted_message_count, cold.persisted_message_count);
	UAM_ASSERT_EQ(rewritten->persisted_messages_digest, cold.persisted_messages_digest);
	AssertTerminalFinalMetadataPreserved(*rewritten, *baseline); // Compare persisted fields, not loaded-vs-cold validation flags.
	const auto primary = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, cold.id));
	const auto summary_revision = ChatRepository::ChatSummaryStorageRevision(app.data_root, cold.id);
	UAM_ASSERT(primary != fixture.local_bytes && summary_revision.has_value());
	live = ChatDomainService().FindChatById(app, cold.id);
	UAM_ASSERT(live && !live->messages_loaded && live->messages.empty());
	UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *live), cold_metadata);

	// Drain ONLY the existing real final receipt. There is no second Sync,
	// fabricated completion/future, or replacement job that can repair a refusal.
	fixture.DrainUntil([&] { return !ChatHistorySyncService().TerminalNativeMediaPending(app); });
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, cold.id)), primary);
	UAM_ASSERT(ChatRepository::ChatStorageRevision(app.data_root, cold.id) == after_revision);
	UAM_ASSERT(ChatRepository::ChatSummaryStorageRevision(app.data_root, cold.id) == summary_revision);
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, cold.id, true);
	live = ChatDomainService().FindChatById(app, cold.id);
	UAM_ASSERT(saved && live && saved->messages == rewritten->messages && saved->messages.size() == 2);
	for (const auto& message : saved->messages) UAM_ASSERT(message.attachments.empty()); // No original association adopted.
	AssertTerminalFinalMetadataPreserved(*saved, *rewritten);
	UAM_ASSERT_EQ(saved->updated_at, rewritten->updated_at);
	UAM_ASSERT(!live->messages_loaded && live->messages.empty());
	UAM_ASSERT(live->metadata_incarnation == cold.metadata_incarnation);
	UAM_ASSERT_EQ(live->persisted_message_count, cold.persisted_message_count);
	UAM_ASSERT_EQ(live->persisted_messages_digest, cold.persisted_messages_digest);
	UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *live), cold_metadata);
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
	UAM_ASSERT(!app.native_chat_refresh_error_status.empty());
	UAM_ASSERT_EQ(app.native_chat_refresh_error_target_id, kParent);
	UAM_ASSERT(!fixture.HasPublished());
	fixture.AssertIdentityAndUnrelatedHistoryPreserved(); // Exact source bytes, unrelated primary, identity, folders and draft.
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(GeminiTerminalColdFinalDefersMetadataSaveUntilOriginalsSettle)
{
	for (const bool rename : {false, true})
	{
		GeminiTerminalOriginalFixture fixture;
		auto& app = fixture.app;
		auto* live = ChatDomainService().FindChatById(app, fixture.local.id);
		UAM_ASSERT(live && live->messages_loaded);
		const auto birth = live->metadata_incarnation;
		auto cold = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, false);
		UAM_ASSERT(cold && !cold->messages_loaded && cold->messages.empty() && cold->persisted_message_count == 1);
		// Actual repository metadata load, preserving the existing in-memory owner
		// incarnation across hydration state; this is not execution authority.
		cold->metadata_incarnation = birth;
		*live = std::move(*cold);
		const auto cold_metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, *live);
		const auto primary = AppPaths::UamChatFilePath(app.data_root, fixture.local.id);
		const auto primary_before = uam::io::ReadTextFile(primary);
		const auto revision_before = ChatRepository::ChatStorageRevision(app.data_root, fixture.local.id);
		UAM_ASSERT(revision_before && *revision_before != "missing");
		fixture.CloseAfterFinalCapture();
		UAM_ASSERT(app.cli_terminals.empty() && ChatHistorySyncService().TerminalNativeMediaPending(app));
		UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
		UAM_ASSERT(!fixture.HasPublished() && app.native_chat_refresh_error_status.empty());
		live = ChatDomainService().FindChatById(app, fixture.local.id);
		UAM_ASSERT(live && !live->messages_loaded && live->messages.empty() && live->metadata_incarnation == birth);
		const std::string requested_title = "Rename after cold final settles";
		if (rename)
		{
			UAM_ASSERT(!ChatHistorySyncService().RenameChat(app, *live, requested_title));
			UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *live), cold_metadata);
		}
		else
		{
			const auto previous_model = live->model_id;
			const auto previous_effort = live->reasoning_effort;
			const auto previous_tier = live->service_tier;
			const auto previous_explicit = live->service_tier_explicit;
			const auto previous_updated = live->updated_at;
			live->model_id = "cold-user-selected-model";
			live->reasoning_effort = "high";
			live->service_tier.clear();
			live->service_tier_explicit = false;
			live->updated_at = "2026-10-16T13:00:00Z";
			UAM_ASSERT(!ChatHistorySyncService().SaveChatWithStatus(app, *live, "model saved", "model save failed"));
			// Mirrors the actual HandleSetChatModel failure branch's exact five
			// assignments. This public service case is not a CEF callback test.
			live->model_id = previous_model;
			live->reasoning_effort = previous_effort;
			live->service_tier = previous_tier;
			live->service_tier_explicit = previous_explicit;
			live->updated_at = previous_updated;
			UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *live), cold_metadata);
		}
		UAM_ASSERT(app.status_line.find("history") != std::string::npos && app.status_line.find("Retry") != std::string::npos);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(primary), primary_before);
		UAM_ASSERT(ChatRepository::ChatStorageRevision(app.data_root, fixture.local.id) == revision_before);
		UAM_ASSERT(app.native_chat_refresh_error_status.empty() && app.native_chat_refresh_error_target_id.empty());
		fixture.AssertIdentityAndUnrelatedHistoryPreserved();
		fixture.AwaitPublished(); // Exactly the original captured final, no retry import or forced hydration.
		const auto keys = fixture.AssertPublished();
		UAM_ASSERT_EQ(keys.size(), std::size_t{2});
		UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app) && app.cli_terminals.empty());
		live = ChatDomainService().FindChatById(app, fixture.local.id);
		UAM_ASSERT(live && live->messages_loaded && live->metadata_incarnation == birth);
		if (rename)
		{
			UAM_ASSERT(ChatHistorySyncService().RenameChat(app, *live, requested_title));
			fixture.local.title = requested_title;
		}
		else
		{
			live->model_id = "cold-user-selected-model";
			live->reasoning_effort = "high";
			live->service_tier.clear();
			live->service_tier_explicit = false;
			live->updated_at = "2026-10-16T13:00:00Z";
			UAM_ASSERT(ChatHistorySyncService().SaveChatWithStatus(app, *live, "model saved", "model save failed"));
		}
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, fixture.local.id, true);
		UAM_ASSERT(saved && saved->messages == live->messages && saved->metadata_incarnation != 0);
		if (rename) UAM_ASSERT_EQ(saved->title, requested_title);
		else
		{
			UAM_ASSERT_EQ(saved->model_id, std::string("cold-user-selected-model"));
			UAM_ASSERT_EQ(saved->reasoning_effort, std::string("high"));
			UAM_ASSERT_EQ(saved->updated_at, std::string("2026-10-16T13:00:00Z"));
		}
		UAM_ASSERT_EQ(fixture.AssertPublished(), keys); // Fresh metadata save retained exact owned original bytes/keys.
		UAM_ASSERT(app.native_chat_refresh_error_status.empty() && !ChatHistorySyncService().TerminalNativeMediaPending(app));
	}
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
namespace
{
	// Real private Gemini journals and real PTY children. The independent image
	// owner has no terminal; the other two owners occupy actual periodic parses.
	struct GeminiTerminalParserCapacityFixture : GeminiTerminalOriginalFixture
	{
		std::array<fs::path, 2> parser_dirs;
		std::array<std::string, 2> parser_ids;
		std::array<std::string, 2> native_ids{
		    "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"};
		std::array<std::string, 2> workspaces;
		std::array<std::weak_ptr<uam::platform::AsyncNativeChatLoadTask::State>, 2> observed;
		std::vector<std::pair<fs::path, std::string>> parser_files;

		GeminiTerminalParserCapacityFixture()
		{
			Json registry{{"projects", {{owned.workspace, "project"}}}};
			for (std::size_t i = 0; i < parser_dirs.size(); ++i)
			{
				const auto suffix = std::to_string(i);
				const auto project_name = "pool-project-" + suffix;
				const auto workspace = owned.temp.root / ("pool-workspace-" + suffix);
				fs::create_directories(workspace);
				workspaces[i] = uam::paths::Utf8PathString(workspace);
				registry["projects"][workspaces[i]] = project_name;
				const auto project = owned.storage / "tmp" / project_name;
				parser_dirs[i] = project / "chats";
				UAM_ASSERT(uam::io::WriteTextFile(project / ".project_root", workspaces[i]));
				auto header = Header(native_ids[i]); header["projectHash"] = project_name;
				const auto journal = Line(header) +
				    Line({{"id", "pool-user-" + suffix}, {"type", "user"}, {"content", "Pool question " + suffix}}) +
				    Line({{"id", "pool-answer-" + suffix}, {"type", "gemini"}, {"content", "Pool answer " + suffix}});
				UAM_ASSERT(uam::io::WriteTextFile(parser_dirs[i] / (native_ids[i] + ".jsonl"), journal));
			}
			const auto registry_bytes = registry.dump();
			UAM_ASSERT(uam::io::WriteTextFile(owned.storage / "projects.json", registry_bytes));
			for (auto& [file, bytes] : unchanged_files)
				if (file == owned.storage / "projects.json") bytes = registry_bytes;
			for (std::size_t i = 0; i < parser_dirs.size(); ++i)
			{
				const auto suffix = std::to_string(i);
				ChatFolder folder; folder.id = "pool-folder-" + suffix; folder.title = "Parser workspace " + suffix;
				folder.directory = workspaces[i]; folder.execution_host_id = "local";
				app.folders.push_back(folder);
				auto source = ChatHistorySyncService().LoadNativeSessionChats(parser_dirs[i], ProviderProfileStore::DefaultGeminiProfile());
				UAM_ASSERT(source.size() == 1 && source.front().messages_loaded && source.front().messages.size() == 2);
				UAM_ASSERT_EQ(source.front().native_session_id, native_ids[i]);
				UAM_ASSERT_EQ(source.front().workspace_directory, workspaces[i]);
				auto owner = std::move(source.front());
				owner.id = parser_ids[i] = "chat-pool-parser-owner-" + suffix;
				owner.folder_id = folder.id; owner.execution_host_id = "local";
				UAM_ASSERT(ChatRepository::SaveChat(app.data_root, owner));
				app.resolved_native_sessions_by_chat_id[owner.id] = native_ids[i];
				app.chats.push_back(std::move(owner));
				for (const auto& file : {parser_dirs[i] / (native_ids[i] + ".jsonl"),
				    parser_dirs[i].parent_path() / ".project_root", AppPaths::UamChatFilePath(app.data_root, parser_ids[i])})
					parser_files.emplace_back(file, uam::io::ReadTextFile(file));
			}
			UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
			folders_before = ChatFolderStore::Serialize(app.folders);
			UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), local.id);
			AddParserTerminals();
		}

		void AddParserTerminals()
		{
			UAM_ASSERT(app.cli_terminals.empty());
			for (std::size_t i = 0; i < parser_ids.size(); ++i)
			{
				auto terminal = std::make_unique<uam::CliTerminalState>();
				terminal->terminal_id = "pool-private-terminal-" + std::to_string(i);
				terminal->frontend_chat_id = terminal->attached_chat_id = parser_ids[i];
				terminal->attached_session_id = native_ids[i];
				terminal->native_activity_provider_id = "gemini-cli";
				terminal->running = true;
				terminal->startup_time_s = uam::GetAppTimeSeconds();
				terminal->last_sync_time_s = -100.0;
				terminal->turn_state = uam::CliTerminalTurnState::Idle;
				terminal->lifecycle_state = uam::CliTerminalLifecycleState::Idle;
				app.cli_terminals.push_back(std::move(terminal));
			}
		}

		uam::platform::AsyncNativeChatLoadTask* FindParser(std::size_t i)
		{
			for (auto& [key, task] : app.native_chat_load_tasks)
				if (task.provider_id_snapshot == "gemini-cli" && task.chats_dir_snapshot == parser_dirs[i].string()) return &task;
			return nullptr;
		}

		void PollAndObserveActualParsers()
		{
			UAM_ASSERT(app.cli_terminals.size() == 2);
			UAM_ASSERT(PlatformServicesFactory::Instance().terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh());
			for (std::size_t i = 0; i < parser_ids.size(); ++i)
			{
				const auto admission_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
				while ((FindParser(i) == nullptr || !FindParser(i)->state) && std::chrono::steady_clock::now() < admission_deadline)
				{
					auto* terminal = uam::FindCliTerminalForChat(app, parser_ids[i]);
					UAM_ASSERT(terminal && terminal->running);
					terminal->last_sync_time_s = -100.0;
					(void)uam::PollCliTerminal(nullptr, app, *terminal, true);
					std::this_thread::yield();
				}
				auto* task = FindParser(i);
				UAM_ASSERT(task && task->running && task->worker && task->state);
				auto state = task->state;
				const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
				while (!state->completed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < completion_deadline)
					std::this_thread::yield();
				UAM_ASSERT(state->completed.load(std::memory_order_acquire));
				UAM_ASSERT(state->worker_retirement_owned && state->error.empty() && state->chats.size() == 1);
				const auto& parsed = state->chats.front();
				UAM_ASSERT_EQ(parsed.native_session_id, native_ids[i]);
				UAM_ASSERT_EQ(parsed.workspace_directory, workspaces[i]);
				UAM_ASSERT(parsed.messages_loaded && parsed.messages.size() == 2);
				UAM_ASSERT_EQ(parsed.messages.back().content, "Pool answer " + std::to_string(i));
				UAM_ASSERT(!state->snapshot_digest.empty());
				UAM_ASSERT_EQ(state->snapshot_digest, uam::NativeHistorySnapshotDigest(state->chats));
				observed[i] = state; // No test-owned strong State survives this method.
			}
			UAM_ASSERT_EQ(app.native_chat_load_tasks.size(), std::size_t{2});
			UAM_ASSERT(!observed[0].expired() && !observed[1].expired());
		}

		void AssertIndependentOriginals() const
		{
			UAM_ASSERT(HasPublished());
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, local.id, true);
			const auto* selected = ChatDomainService().SelectedChat(app);
			UAM_ASSERT(saved && selected && selected->messages == saved->messages);
			UAM_ASSERT_EQ(saved->id, local.id); UAM_ASSERT_EQ(saved->native_session_id, kParent);
			UAM_ASSERT_EQ(saved->workspace_directory, owned.workspace); UAM_ASSERT_EQ(saved->title, local.title);
			UAM_ASSERT_EQ(saved->folder_id, local.folder_id); UAM_ASSERT_EQ(saved->pinned, local.pinned);
			for (const auto& [row, expected] : parts)
			{
				const auto& attachments = saved->messages[row].attachments;
				const auto found = std::ranges::find_if(attachments, [&](const auto& a) { return a.id == expected.id; });
				UAM_ASSERT(found != attachments.end() && found->source == AttachmentSource::OwnedNativeAsset && !found->native_inline_payload);
				UAM_ASSERT(uam::native_assets::IsKeyForOwner(local.id, found->native_asset_key));
				const auto bytes = uam::native_assets::ReadOwned(app.data_root, local.id, found->native_asset_key);
				UAM_ASSERT(bytes.ok && bytes.error.empty()); UAM_ASSERT_EQ(bytes.bytes, original);
				auto normalized = *found; normalized.source = AttachmentSource::NativeInlinePending; normalized.native_asset_key.clear();
				UAM_ASSERT(normalized == expected); // Every stable part association/descriptor remains exact.
			}
			UAM_ASSERT_EQ(app.composer_text, std::string("Keep selected draft"));
			UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), folders_before);
			for (const auto& [file, bytes] : unchanged_files) UAM_ASSERT_EQ(uam::io::ReadTextFile(file), bytes);
			for (const auto& [file, bytes] : parser_files) UAM_ASSERT_EQ(uam::io::ReadTextFile(file), bytes);
		}
	};
}

UAM_TEST(GeminiTerminalParsersLeaveIndependentOriginalPublicationCapacity)
{
	GeminiTerminalParserCapacityFixture fixture;
	auto& app = fixture.app;
	GeminiR7TaskCleanup read_cleanup{app};
	GeminiR6TerminalCleanup terminal_cleanup{app};
	GeminiR7MacChild first(*app.cli_terminals[0], true);
	GeminiR7MacChild second(*app.cli_terminals[1], true);
	first.AssertRunning(); second.AssertRunning();
	fixture.PollAndObserveActualParsers();
	UAM_ASSERT(!uam::ChatHasActiveCliTerminal(app, fixture.local.id));
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	fixture.RequestLoaded(); // Independent third chat, actual inline-PNG refresh admission.
	UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
	UAM_ASSERT_EQ(app.native_chat_load_tasks.size(), std::size_t{2});
	UAM_ASSERT(!fixture.observed[0].expired() && !fixture.observed[1].expired());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while ((!fixture.HasPublished() || ChatHistorySyncService().TerminalNativeMediaPending(app)) && std::chrono::steady_clock::now() < deadline)
	{
		// Deliberately do not Poll/consume the terminal parsers to free capacity.
		(void)ChatHistorySyncService().PollTerminalNativeMedia(app);
		std::this_thread::yield();
	}
	UAM_ASSERT(fixture.HasPublished() && !ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(app.native_chat_refresh_error_status.empty());
	fixture.AssertIndependentOriginals();
	UAM_ASSERT_EQ(app.native_chat_load_tasks.size(), std::size_t{2});
	UAM_ASSERT(!fixture.observed[0].expired() && !fixture.observed[1].expired());
	first.AssertRunning(); second.AssertRunning(); // No hidden Stop/consume can rescue admission.
}

UAM_TEST(GeminiTerminalErasedOwnersRetireCompletedParsersWithoutFinalCapture)
{
	GeminiTerminalParserCapacityFixture fixture;
	auto& app = fixture.app;
	GeminiR7TaskCleanup read_cleanup{app};
	GeminiR6TerminalCleanup terminal_cleanup{app};
	GeminiR7MacChild first(*app.cli_terminals[0], true);
	GeminiR7MacChild second(*app.cli_terminals[1], true);
	fixture.PollAndObserveActualParsers();
	first.AssertRunning(); second.AssertRunning();
	const std::array<pid_t, 2> pids{app.cli_terminals[0]->child_pid, app.cli_terminals[1]->child_pid};
	const auto old_states = fixture.observed;
	const auto primary_before = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id));
	for (const auto& id : fixture.parser_ids)
		UAM_ASSERT(uam::StopAndEraseCliTerminalForChat(app, id, false)); // Same public stop seam used by delete/provider switch.
	UAM_ASSERT(app.cli_terminals.empty());
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
	for (const auto pid : pids)
	{
		int status = 0; errno = 0;
		UAM_ASSERT(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD); // Actual owned child reap, no invented stop state.
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while ((!app.native_chat_load_tasks.empty() || !old_states[0].expired() || !old_states[1].expired()) && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::PollAllCliTerminals(nullptr, app, false); // Empty terminal vector must still retire orphan results.
		std::this_thread::yield();
	}
	UAM_ASSERT(app.native_chat_load_tasks.empty());
	UAM_ASSERT(old_states[0].expired() && old_states[1].expired());
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(app.native_chat_refresh_error_status.empty());
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), primary_before);
	for (const auto& [file, bytes] : fixture.unchanged_files) UAM_ASSERT_EQ(uam::io::ReadTextFile(file), bytes);
	for (const auto& [file, bytes] : fixture.parser_files) UAM_ASSERT_EQ(uam::io::ReadTextFile(file), bytes);
	fixture.AddParserTerminals();
	GeminiR7MacChild fresh_first(*app.cli_terminals[0], true);
	GeminiR7MacChild fresh_second(*app.cli_terminals[1], true);
	// Weak expiry alone is not a worker-destructor receipt. Both fresh actual
	// parse admissions and exact completed snapshots also prove quota reclamation.
	fixture.PollAndObserveActualParsers();
	fresh_first.AssertRunning(); fresh_second.AssertRunning();
	UAM_ASSERT(old_states[0].expired() && old_states[1].expired());
	UAM_ASSERT_EQ(app.native_chat_load_tasks.size(), std::size_t{2});
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
UAM_TEST(GeminiTerminalSharedParserSurvivesOneOwnerAndRetiresAfterLastStop)
{
	GeminiTerminalParserCapacityFixture fixture;
	auto& app = fixture.app;
	GeminiR7TaskCleanup read_cleanup{app};
	GeminiR6TerminalCleanup terminal_cleanup{app};
	// Two distinct saved native owners and two actual PTYs share one actual
	// provider/workspace/history key. Move the second genuine journal into that
	// project before parsing, keeping only one scoped copy of its native UUID.
	const auto old_second_file = fixture.parser_dirs[1] / (fixture.native_ids[1] + ".jsonl");
	const auto shared_file = fixture.parser_dirs[0] / (fixture.native_ids[1] + ".jsonl");
	auto header = Header(fixture.native_ids[1]); header["projectHash"] = "pool-project-0";
	const auto journal = Line(header) +
	    Line({{"id", "pool-user-1"}, {"type", "user"}, {"content", "Pool question 1"}}) +
	    Line({{"id", "pool-answer-1"}, {"type", "gemini"}, {"content", "Pool answer 1"}});
	UAM_ASSERT(uam::io::WriteTextFile(shared_file, journal));
	UAM_ASSERT(fs::remove(old_second_file));
	fixture.parser_dirs[1] = fixture.parser_dirs[0];
	fixture.workspaces[1] = fixture.workspaces[0];
	auto* second_owner = ChatDomainService().FindChatById(app, fixture.parser_ids[1]);
	const auto* first_owner = ChatDomainService().FindChatById(app, fixture.parser_ids[0]);
	UAM_ASSERT(first_owner && second_owner);
	second_owner->workspace_directory = first_owner->workspace_directory;
	second_owner->folder_id = first_owner->folder_id;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *second_owner));
	const auto selected_primary = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id));
	const auto first_primary = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.parser_ids[0]));
	const auto second_primary = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.parser_ids[1]));
	GeminiR7MacChild first(*app.cli_terminals[0], true);
	GeminiR7MacChild second(*app.cli_terminals[1], true);
	first.AssertRunning(); second.AssertRunning();
	const auto first_pid = app.cli_terminals[0]->child_pid;
	const auto second_pid = app.cli_terminals[1]->child_pid;
	const auto admission_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while ((fixture.FindParser(0) == nullptr || !fixture.FindParser(0)->state) && std::chrono::steady_clock::now() < admission_deadline)
	{
		auto* terminal = uam::FindCliTerminalForChat(app, fixture.parser_ids[0]);
		UAM_ASSERT(terminal && terminal->running);
		terminal->last_sync_time_s = -100.0;
		(void)uam::PollCliTerminal(nullptr, app, *terminal, true);
		std::this_thread::yield();
	}
	auto* task = fixture.FindParser(0);
	UAM_ASSERT(task && task->running && task->worker && task->state);
	auto state = task->state;
	const auto completed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (!state->completed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < completed_deadline)
		std::this_thread::yield();
	UAM_ASSERT(state->completed.load(std::memory_order_acquire) && state->worker_retirement_owned && state->error.empty());
	UAM_ASSERT_EQ(state->chats.size(), std::size_t{2});
	for (std::size_t i = 0; i < fixture.native_ids.size(); ++i)
	{
		const auto found = std::ranges::find_if(state->chats, [&](const auto& row) { return row.native_session_id == fixture.native_ids[i]; });
		UAM_ASSERT(found != state->chats.end() && found->messages_loaded && found->messages.size() == 2);
		UAM_ASSERT_EQ(found->workspace_directory, fixture.workspaces[0]);
		UAM_ASSERT_EQ(found->messages.back().content, "Pool answer " + std::to_string(i));
	}
	UAM_ASSERT_EQ(state->snapshot_digest, uam::NativeHistorySnapshotDigest(state->chats));
	const auto* identity = state.get();
	const auto cohort = app.terminal_native_history_load_generation;
	std::weak_ptr<uam::platform::AsyncNativeChatLoadTask::State> observed = state;
	state.reset(); // Only the real task owns this completed snapshot.
	UAM_ASSERT_EQ(app.native_chat_load_tasks.size(), std::size_t{1});
	// Only the first terminal was polled. The survivor is a real same-key owner
	// even before its first refresh tick, and must not be cancelled by this stop.
	UAM_ASSERT(uam::StopAndEraseCliTerminalForChat(app, fixture.parser_ids[0], false));
	UAM_ASSERT_EQ(app.cli_terminals.size(), std::size_t{1});
	int status = 0; errno = 0;
	UAM_ASSERT(waitpid(first_pid, &status, WNOHANG) == -1 && errno == ECHILD);
	auto* survivor = uam::FindCliTerminalForChat(app, fixture.parser_ids[1]);
	UAM_ASSERT(survivor && survivor->running);
	// Current timestamps prevent an unrelated ordinary consume/replacement from
	// obscuring the owner's pre-loop sweep. No fabricated future is used.
	survivor->last_sync_time_s = uam::GetAppTimeSeconds();
	survivor->last_polled_time_s = uam::GetAppTimeSeconds();
	(void)uam::PollAllCliTerminals(nullptr, app, false);
	task = fixture.FindParser(0);
	UAM_ASSERT(task && task->state && task->state.get() == identity && task->worker);
	UAM_ASSERT(!task->worker->get_stop_token().stop_requested());
	UAM_ASSERT(!observed.expired() && app.terminal_native_history_load_generation == cohort);
	second.AssertRunning();
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(uam::StopAndEraseCliTerminalForChat(app, fixture.parser_ids[1], false));
	UAM_ASSERT(app.cli_terminals.empty());
	status = 0; errno = 0;
	UAM_ASSERT(waitpid(second_pid, &status, WNOHANG) == -1 && errno == ECHILD);
	const auto retirement_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while ((!app.native_chat_load_tasks.empty() || !observed.expired()) && std::chrono::steady_clock::now() < retirement_deadline)
	{
		(void)uam::PollAllCliTerminals(nullptr, app, false);
		std::this_thread::yield();
	}
	UAM_ASSERT(app.native_chat_load_tasks.empty() && observed.expired());
	UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
	UAM_ASSERT(app.native_chat_refresh_error_status.empty());
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.local.id)), selected_primary);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.parser_ids[0])), first_primary);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, fixture.parser_ids[1])), second_primary);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(shared_file), journal); UAM_ASSERT(!fs::exists(old_second_file));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), fixture.local.id);
	UAM_ASSERT_EQ(app.composer_text, std::string("Keep selected draft"));
}
#endif


namespace
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	struct NativeTimestampPrivateTimezone
	{
		std::unique_ptr<ScopedEnvVar> zone = std::make_unique<ScopedEnvVar>("TZ", "UTC0");
		static void Reload()
		{
#if defined(_WIN32)
			_tzset();
#else
			tzset();
#endif
		}
		NativeTimestampPrivateTimezone()
		{
			Reload();
		}
		~NativeTimestampPrivateTimezone()
		{
			zone.reset();
			Reload();
		}
	};

	void CheckGeminiUntargetedTimestampImport(bool native_is_newer)
	{
		// This process-private, non-DST zone makes these two historical instants
		// unambiguous. It is NOT a policy for interpreting old local metadata.
		NativeTimestampPrivateTimezone timezone;
		GeminiOwnershipFixture fixture;
		const auto private_home = fixture.temp.root / "os-home";
		fs::create_directories(private_home);
		ScopedEnvVar os_home("HOME", uam::paths::Utf8PathString(private_home));
		ScopedEnvVar os_profile("USERPROFILE", uam::paths::Utf8PathString(private_home));
		const auto noon = std::chrono::sys_days{std::chrono::year{2026} / 10 / 16} + std::chrono::hours{12};
		const auto local_epoch = std::chrono::system_clock::to_time_t(noon);
		const auto native_epoch = local_epoch + (native_is_newer ? 3600 : -3600);
		UAM_ASSERT(native_is_newer ? native_epoch > local_epoch : native_epoch < local_epoch);
		const auto local_stamp = uam::time::detail::FormatTimestamp(uam::time::detail::LocalTimeSnapshot(local_epoch), uam::time::kLocalDisplayTimestampFormat);
		const auto native_stamp = uam::time::detail::FormatTimestamp(uam::time::detail::UtcTimeSnapshot(native_epoch), uam::time::kIsoUtcTimestampFormat);
		UAM_ASSERT_EQ(local_stamp, std::string("2026-10-16 12:00:00"));
		UAM_ASSERT_EQ(native_stamp, native_is_newer ? std::string("2026-10-16T13:00:00.000Z") : std::string("2026-10-16T11:00:00.000Z"));
		const auto native_file = fixture.chats / (kParent + ".jsonl");
		const auto write_journal = [&](const std::string& stamp, const std::string& answer)
		{
			auto header = Header();
			header["startTime"] = "2026-10-16T09:00:00.000Z";
			header["lastUpdated"] = stamp;
			const auto bytes = Line(header) + Line({{"id", "timestamp-question"}, {"type", "user"}, {"timestamp", "2026-10-16T09:00:00.000Z"}, {"content", "Same timestamp question"}}) + Line({{"id", "timestamp-answer"}, {"type", "gemini"}, {"timestamp", stamp}, {"content", answer}});
			UAM_ASSERT(uam::io::WriteTextFile(native_file, bytes));
		};
		write_journal("2026-10-16T09:01:00.000Z", "Initial imported answer");
		uam::AppState app;
		app.data_root = fixture.temp.root / "timestamp-import-data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatFolder folder;
		folder.id = "timestamp-folder";
		folder.title = "Timestamp workspace";
		folder.directory = fixture.workspace;
		folder.execution_host_id = "local";
		app.folders.push_back(folder);
		UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
		const auto first = ChatHistorySyncService().ImportAllNativeChatsToLocal(app, false);
		UAM_ASSERT(first.success && first.total_count == 1 && first.imported_count == 1 && first.published_chat_ids.size() == 1);
		auto imported = ChatRepository::LoadLocalChat(app.data_root, first.published_chat_ids.front(), true);
		UAM_ASSERT(imported && imported->messages_loaded && imported->messages.size() == 2);
		UAM_ASSERT_EQ(imported->messages.back().content, std::string("Initial imported answer"));
		// A genuine imported owner remains eligible for untargeted reconciliation;
		// fabricating a chat-* owner would instead hit the owned-session exclusion.
		UAM_ASSERT(!uam::strings::StartsWith(imported->id, "chat-"));
		UAM_ASSERT_EQ(imported->native_session_id, kParent);
		UAM_ASSERT_EQ(imported->workspace_directory, fixture.workspace);
		imported->messages.back().content = "Newer saved UAM answer";
		imported->updated_at = local_stamp;
		imported->title = "Preserve UAM title";
		imported->pinned = true;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *imported));
		imported = ChatRepository::LoadLocalChat(app.data_root, imported->id, true);
		UAM_ASSERT(imported && imported->updated_at == local_stamp && imported->messages.size() == 2);
		const ChatSession saved_before = *imported;
		ChatSession unrelated;
		unrelated.id = "chat-timestamp-unrelated";
		unrelated.provider_id = "gemini-cli";
		unrelated.title = "Unrelated authored history";
		unrelated.created_at = "2026-10-15T09:00:00.000Z";
		unrelated.updated_at = unrelated.created_at;
		unrelated.messages.push_back({MessageRole::User, "Keep unrelated answer"});
		unrelated.workspace_directory = uam::paths::Utf8PathString(fixture.temp.root / "other-workspace");
		fs::create_directories(uam::paths::PathFromUtf8(unrelated.workspace_directory));
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, unrelated));
		app.chats = {unrelated, saved_before};
		ChatDomainService().SelectChatById(app, saved_before.id);
		app.composer_text = "Keep timestamp draft";
		app.resolved_native_sessions_by_chat_id[saved_before.id] = kParent;
		UAM_ASSERT(app.cli_terminals.empty() && app.acp_sessions.empty() && app.pending_chat_save_at_by_chat_id.empty() && app.worktree_operation_chat_ids.empty());
		write_journal(native_stamp, native_is_newer ? "Newer native answer" : "Older native answer");
		const auto native = ChatHistorySyncService().LoadNativeSessionChats(fixture.chats, ProviderProfileStore::DefaultGeminiProfile());
		UAM_ASSERT(native.size() == 1 && native.front().messages_loaded && native.front().messages.size() == saved_before.messages.size());
		UAM_ASSERT_EQ(native.front().native_session_id, kParent);
		UAM_ASSERT_EQ(native.front().workspace_directory, fixture.workspace);
		UAM_ASSERT_EQ(native.front().updated_at, native_stamp);
		UAM_ASSERT_EQ(native.front().messages.back().content, native_is_newer ? std::string("Newer native answer") : std::string("Older native answer"));
		for (const auto& message : native.front().messages)
			UAM_ASSERT(message.attachments.empty());
		std::vector<std::pair<fs::path, std::string>> unchanged;
		for (const auto& path : {native_file, fixture.storage / "projects.json", fixture.project / ".project_root", fixture.chats / kParent / (kChild + ".jsonl"), AppPaths::UamChatFilePath(app.data_root, unrelated.id), AppPaths::UamChatSummaryFilePath(app.data_root, unrelated.id)})
		{
			const auto bytes = uam::io::ReadTextFile(path);
			UAM_ASSERT(!bytes.empty());
			unchanged.emplace_back(path, bytes);
		}
		const auto primary = AppPaths::UamChatFilePath(app.data_root, saved_before.id);
		const auto summary = AppPaths::UamChatSummaryFilePath(app.data_root, saved_before.id);
		const auto primary_before = uam::io::ReadTextFile(primary), summary_before = uam::io::ReadTextFile(summary);
		const auto folders_before = ChatFolderStore::Serialize(app.folders);
		// Actual journal read -> untargeted import -> saved publication -> sidebar
		// merge/selected hydration. Empty target deliberately retains age checks.
		UAM_ASSERT(uam::SyncChatsFromNative(app, "", true));
		const auto saved_after = ChatRepository::LoadLocalChat(app.data_root, saved_before.id, true);
		const auto* live = ChatDomainService().SelectedChat(app);
		UAM_ASSERT(saved_after && saved_after->messages_loaded && live && live->messages_loaded);
		UAM_ASSERT_EQ(saved_after->messages.size(), std::size_t{2});
		UAM_ASSERT_EQ(live->messages, saved_after->messages);
		if (native_is_newer)
		{
			UAM_ASSERT_EQ(saved_after->messages.back().content, std::string("Newer native answer"));
			UAM_ASSERT_EQ(saved_after->updated_at, native_stamp);
			UAM_ASSERT(uam::io::ReadTextFile(primary) != primary_before);
		}
		else
		{
			UAM_ASSERT_EQ(saved_after->messages, saved_before.messages);
			UAM_ASSERT_EQ(live->messages, saved_before.messages);
			UAM_ASSERT_EQ(saved_after->updated_at, local_stamp);
			UAM_ASSERT_EQ(uam::io::ReadTextFile(primary), primary_before);
			UAM_ASSERT_EQ(uam::io::ReadTextFile(summary), summary_before);
		}
		for (const auto* current : {&*saved_after, live})
		{
			UAM_ASSERT_EQ(current->id, saved_before.id);
			UAM_ASSERT_EQ(current->native_session_id, kParent);
			UAM_ASSERT_EQ(current->provider_id, saved_before.provider_id);
			UAM_ASSERT_EQ(current->execution_host_id, saved_before.execution_host_id);
			UAM_ASSERT_EQ(current->workspace_directory, fixture.workspace);
			UAM_ASSERT_EQ(current->folder_id, folder.id);
			UAM_ASSERT_EQ(current->title, saved_before.title);
			UAM_ASSERT(current->pinned);
		}
		UAM_ASSERT_EQ(app.chats.size(), std::size_t{2});
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), saved_before.id);
		UAM_ASSERT_EQ(app.composer_text, std::string("Keep timestamp draft"));
		UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.at(saved_before.id), kParent);
		UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), folders_before);
		UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
		UAM_ASSERT(app.native_chat_refresh_error_status.empty() && app.native_chat_refresh_error_target_id.empty());
		for (const auto& [path, bytes] : unchanged)
		{
			const std::string current_bytes = uam::io::ReadTextFile(path);
			if (current_bytes != bytes)
			{
				throw TestFailure("Timestamp import changed expected-stable file: " +
				    uam::paths::Utf8PathString(path.lexically_relative(fixture.temp.root)) +
				    " (before bytes=" + std::to_string(bytes.size()) +
				    ", after bytes=" + std::to_string(current_bytes.size()) + ")");
			}
			UAM_ASSERT_EQ(uam::io::ReadTextFile(path), bytes);
		}
	}
#endif
} // namespace

UAM_TEST(GeminiUntargetedImportAcceptsNewerIsoThanLocalDisplayHistory)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	CheckGeminiUntargetedTimestampImport(true);
#endif
}

UAM_TEST(GeminiUntargetedImportPreservesNewerLocalDisplayThanIsoHistory)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	CheckGeminiUntargetedTimestampImport(false);
#endif
}

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	const std::string kUnknownStableNativeMessageTime = "2026-10-16T09:01:00Z";

	struct UnknownHistoryOwnedOriginal
	{
		std::size_t row = 0;
		MessageRole role;
		std::string message_created_at;
		MessageAttachment attachment;
		std::string bytes;
	};

	std::vector<UnknownHistoryOwnedOriginal> CaptureUnknownHistoryOriginals(const GeminiTerminalOriginalFixture& fixture, const ChatSession& chat)
	{
		std::vector<UnknownHistoryOwnedOriginal> originals;
		for (std::size_t row = 0; row < chat.messages.size(); ++row)
		{
			const auto& message = chat.messages[row];
			for (const auto& attachment : message.attachments)
			{
				UAM_ASSERT_EQ(attachment.source, AttachmentSource::OwnedNativeAsset);
				const auto original = uam::native_assets::ReadOwned(fixture.app.data_root, chat.id, attachment.native_asset_key);
				UAM_ASSERT(original.ok && original.error.empty());
				UAM_ASSERT_EQ(original.bytes, fixture.original);
				originals.push_back({row, message.role, message.created_at, attachment, original.bytes});
			}
		}
		UAM_ASSERT_EQ(originals.size(), std::size_t{2});
		return originals;
	}

	std::vector<ChatSession> WriteUnknownHistoryJournal(GeminiTerminalOriginalFixture& fixture, bool native_unknown, bool grow)
	{
		const auto file = fixture.owned.chats / (kParent + ".jsonl");
		const std::string stamp = native_unknown ? "recorded" : "2026-10-16T12:00:01Z";
		std::string bytes = uam::io::ReadTextFile(file) + Line({{"$set", {{"lastUpdated", stamp}}}}) +
		    Line({{"$patch", {{"id", "a"}, {"content", "Different source answer under unknown chronology"}}}});
		if (grow)
		{
			bytes += Line({{"id", "unknown-extra-user"}, {"type", "user"}, {"timestamp", stamp}, {"content", "Additional genuine native row"}});
		}
		UAM_ASSERT(uam::io::WriteTextFile(file, bytes));
		for (auto& [path, expected] : fixture.unchanged_files)
		{
			if (path == file)
			{
				expected = bytes;
			}
		}
		auto native = ChatHistorySyncService().LoadNativeSessionChats(fixture.owned.chats, ProviderProfileStore::DefaultGeminiProfile());
		UAM_ASSERT(native.size() == 1 && native.front().messages_loaded);
		UAM_ASSERT_EQ(native.front().messages.size(), grow ? std::size_t{3} : std::size_t{2});
		UAM_ASSERT_EQ(native.front().updated_at, stamp);
		UAM_ASSERT_EQ(native.front().messages[1].content, std::string("Different source answer under unknown chronology"));
		UAM_ASSERT_EQ(native.front().native_session_id, kParent);
		UAM_ASSERT_EQ(native.front().workspace_directory, fixture.owned.workspace);
		UAM_ASSERT_EQ(native.front().messages[0].created_at, kUnknownStableNativeMessageTime);
		UAM_ASSERT_EQ(native.front().messages[1].created_at, kUnknownStableNativeMessageTime);
		return native;
	}

	ChatSession SaveUnknownHistoryBaseline(GeminiTerminalOriginalFixture& fixture, bool saved_unknown)
	{
		// Establish explicit native row times before the initial real publication.
		// Changing only header lastUpdated later must not change row identity via
		// the loader's legitimate missing-row-timestamp fallback.
		const auto file = fixture.owned.chats / (kParent + ".jsonl");
		std::string bytes = uam::io::ReadTextFile(file);
		const auto folded = uam::gemini::FoldHistoryJournal(bytes);
		UAM_ASSERT(folded && folded->contains("messages") && (*folded)["messages"].is_array());
		UAM_ASSERT_EQ((*folded)["messages"].size(), std::size_t{2});
		// Timestamp is not a supported $patch field. Append complete current
		// same-ID native records: Put replaces them without changing row order.
		// Copy the actual folded objects so all content/tool/original parts survive.
		for (const std::string id : {"u", "a"})
		{
			bool found = false;
			for (const auto& record : (*folded)["messages"])
			{
				if (record.value("id", std::string{}) != id)
				{
					continue;
				}
				UAM_ASSERT_EQ(record.value("type", std::string{}), id == "u" ? std::string("user") : std::string("gemini"));
				auto replacement = record;
				replacement["timestamp"] = kUnknownStableNativeMessageTime;
				bytes += Line(replacement);
				found = true;
			}
			UAM_ASSERT(found);
		}
		UAM_ASSERT(uam::io::WriteTextFile(file, bytes));
		for (auto& [path, expected] : fixture.unchanged_files)
		{
			if (path == file)
			{
				expected = bytes;
			}
		}
		const auto explicit_rows = fixture.Snapshot();
		UAM_ASSERT(kUnknownStableNativeMessageTime != explicit_rows.front().updated_at);
		UAM_ASSERT_EQ(explicit_rows.front().messages[0].created_at, kUnknownStableNativeMessageTime);
		UAM_ASSERT_EQ(explicit_rows.front().messages[1].created_at, kUnknownStableNativeMessageTime);
		fixture.PublishExistingOwnedControl(); // Real worker original publisher + primary/original ACK.
		(void)fixture.AssertPublished();
		auto* live = ChatDomainService().FindChatById(fixture.app, fixture.local.id);
		UAM_ASSERT(live && live->messages_loaded && live->messages.size() == 2);
		live->updated_at = saved_unknown ? "recorded" : "2026-10-16T12:00:00Z";
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *live));
		const auto baseline = ChatRepository::LoadLocalChat(fixture.app.data_root, live->id, true);
		UAM_ASSERT(baseline && baseline->messages == live->messages && baseline->updated_at == live->updated_at);
		*live = *baseline;
		fixture.local = *baseline;
		fixture.local_bytes = uam::io::ReadTextFile(AppPaths::UamChatFilePath(fixture.app.data_root, live->id));
		return *baseline;
	}

	void AssertUnknownHistoryOriginalsPreserved(const GeminiTerminalOriginalFixture& fixture, const std::vector<UnknownHistoryOwnedOriginal>& originals)
	{
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.local.id, true);
		UAM_ASSERT(saved && saved->messages_loaded);
		std::size_t attachment_count = 0;
		for (const auto& message : saved->messages)
		{
			attachment_count += message.attachments.size();
		}
		UAM_ASSERT_EQ(attachment_count, std::size_t{2});
		for (const auto& original : originals)
		{
			UAM_ASSERT(original.row < saved->messages.size());
			const auto& message = saved->messages[original.row];
			UAM_ASSERT_EQ(message.role, original.role);
			UAM_ASSERT_EQ(message.created_at, original.message_created_at);
			const auto found = std::ranges::find(message.attachments, original.attachment.id, &MessageAttachment::id);
			UAM_ASSERT(found != message.attachments.end());
			UAM_ASSERT_EQ(found->source, AttachmentSource::OwnedNativeAsset);
			UAM_ASSERT_EQ(found->native_asset_key, original.attachment.native_asset_key);
			UAM_ASSERT_EQ(found->native_part_key, original.attachment.native_part_key);
			UAM_ASSERT_EQ(found->native_tool_call_id, original.attachment.native_tool_call_id);
			UAM_ASSERT_EQ(found->native_source_field, original.attachment.native_source_field);
			UAM_ASSERT_EQ(found->origin_execution_host_id, original.attachment.origin_execution_host_id);
			UAM_ASSERT_EQ(found->content_digest, original.attachment.content_digest);
			UAM_ASSERT_EQ(found->size_bytes, original.attachment.size_bytes);
			UAM_ASSERT_EQ(found->mime_type, original.attachment.mime_type);
			UAM_ASSERT_EQ(found->name, original.attachment.name);
			UAM_ASSERT_EQ(found->kind, original.attachment.kind);
			UAM_ASSERT(found->path.empty() && !found->native_inline_payload);
			const auto read = uam::native_assets::ReadOwned(fixture.app.data_root, fixture.local.id, found->native_asset_key);
			UAM_ASSERT(read.ok && read.error.empty());
			UAM_ASSERT_EQ(read.bytes, original.bytes);
		}
		fixture.AssertIdentityAndUnrelatedHistoryPreserved();
	}
}

UAM_TEST(GeminiDiscoveredUnknownChronologyPreservesOwnedHistoryWithCountGrowthControl)
{
	for (const bool native_unknown : {false, true})
	{
		for (const bool grow : {false, true})
		{
			GeminiTerminalOriginalFixture fixture;
			const auto baseline = SaveUnknownHistoryBaseline(fixture, !native_unknown);
			const auto originals = CaptureUnknownHistoryOriginals(fixture, baseline);
			auto native = WriteUnknownHistoryJournal(fixture, native_unknown, grow);
			UAM_ASSERT(native.front().messages != baseline.messages);
			const auto source_messages = native.front().messages;
			const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, baseline.id);
			const auto summary = AppPaths::UamChatSummaryFilePath(fixture.app.data_root, baseline.id);
			const auto primary_before = uam::io::ReadTextFile(primary), summary_before = uam::io::ReadTextFile(summary);
			ChatHistorySyncService::LocalHistoryDiscovery discovery;
			discovery.chats = std::move(native); // Actual reader output; no overlay or eligibility mutation.
			const auto admitted = ChatHistorySyncService().ImportDiscoveredProviderChatsBatch(fixture.app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::RegisteredWorkspacesOnly);
			UAM_ASSERT(admitted.success && admitted.imported_count == 0 && discovery.chats.empty() && discovery.preparation.valid());
			UAM_ASSERT_EQ(discovery.preparation.wait_for(std::chrono::seconds(15)), std::future_status::ready);
			{
				const auto prepared = discovery.preparation.get();
				UAM_ASSERT(prepared && prepared->chats.size() == 1);
				const auto& candidate = prepared->chats.front();
				UAM_ASSERT(candidate.expected_live && candidate.expected_live->messages == baseline.messages);
				UAM_ASSERT(candidate.error.empty() && !candidate.confirmed_empty);
				UAM_ASSERT_EQ(candidate.skipped, !grow);
				if (!grow)
				{
					UAM_ASSERT(!candidate.files && !candidate.native_media);
					UAM_ASSERT_EQ(candidate.chat.messages, baseline.messages);
				}
			}
			ChatHistorySyncService::ImportResult result;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			while (discovery.Pending())
			{
				UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
				result.Merge(ChatHistorySyncService().ImportDiscoveredProviderChatsBatch(fixture.app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::RegisteredWorkspacesOnly));
				std::this_thread::yield();
			}
			UAM_ASSERT(result.success && result.confirmed_empty_native_keys.empty());
			UAM_ASSERT_EQ(result.imported_count, grow ? 1 : 0);
			const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, baseline.id, true);
			UAM_ASSERT(saved && saved->messages_loaded);
			if (grow)
			{
				UAM_ASSERT_EQ(saved->messages.size(), source_messages.size());
				UAM_ASSERT_EQ(saved->messages[1].content, source_messages[1].content);
				UAM_ASSERT_EQ(saved->messages.back().content, source_messages.back().content);
			}
			else
			{
				UAM_ASSERT_EQ(saved->messages, baseline.messages);
				UAM_ASSERT_EQ(saved->updated_at, baseline.updated_at);
				UAM_ASSERT_EQ(uam::io::ReadTextFile(primary), primary_before);
				UAM_ASSERT_EQ(uam::io::ReadTextFile(summary), summary_before);
			}
			AssertUnknownHistoryOriginalsPreserved(fixture, originals);
		}
	}
}

UAM_TEST(GeminiFinalUnknownConversationChronologyCannotBorrowValidMetadataTime)
{
	for (const bool native_unknown : {false, true})
	{
		for (const bool grow : {false, true})
		{
			GeminiTerminalOriginalFixture fixture;
			const auto baseline = SaveUnknownHistoryBaseline(fixture, !native_unknown);
			const auto originals = CaptureUnknownHistoryOriginals(fixture, baseline);
			const auto source = WriteUnknownHistoryJournal(fixture, native_unknown, grow);
			UAM_ASSERT(source.front().messages != baseline.messages);
			fixture.CloseAfterFinalCapture(); // Actual final capture/stop/erase, before any owner poll.
			UAM_ASSERT(fixture.app.cli_terminals.empty());
			UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaFinalPending(fixture.app));
			auto* live = ChatDomainService().FindChatById(fixture.app, baseline.id);
			UAM_ASSERT(live && live->messages == baseline.messages);
			// This known metadata time is BEFORE the valid native source. If the
			// final override were lost, the saved-Unknown arm would wrongly admit it.
			live->updated_at = "2026-10-16T11:59:59Z";
			live->model_id = "explicit-current-metadata-model";
			UAM_ASSERT_EQ(live->persisted_message_count, baseline.persisted_message_count);
			UAM_ASSERT_EQ(live->persisted_messages_digest, baseline.persisted_messages_digest);
			UAM_ASSERT(ChatHistorySyncService().SaveChatWithStatus(fixture.app, *live,
			    "Private fixture warm metadata saved.", "Private fixture warm metadata save failed."));
			// A normal warm metadata save preserves the captured cache witness;
			// an independent disk read is an oracle, not an instruction to adopt it.
			UAM_ASSERT_EQ(live->persisted_message_count, baseline.persisted_message_count);
			UAM_ASSERT_EQ(live->persisted_messages_digest, baseline.persisted_messages_digest);
			const auto saved_metadata = ChatRepository::LoadLocalChat(fixture.app.data_root, live->id, true);
			UAM_ASSERT(saved_metadata && saved_metadata->messages == baseline.messages);
			UAM_ASSERT_EQ(saved_metadata->updated_at, std::string("2026-10-16T11:59:59Z"));
			UAM_ASSERT_EQ(saved_metadata->model_id, std::string("explicit-current-metadata-model"));
			UAM_ASSERT_EQ(live->updated_at, saved_metadata->updated_at);
			UAM_ASSERT_EQ(live->model_id, saved_metadata->model_id);
			fixture.local = *saved_metadata;
			const auto expected_metadata = *saved_metadata;
			fixture.DrainUntil([&] { return !ChatHistorySyncService().TerminalNativeMediaPending(fixture.app); });
			UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaFinalPending(fixture.app));
			if (!fixture.app.native_chat_refresh_error_status.empty())
			{
				std::cerr << "Unknown final fixture: native_unknown=" << native_unknown << " grow=" << grow
				          << " status=" << fixture.app.native_chat_refresh_error_status
				          << " target=" << fixture.app.native_chat_refresh_error_target_id
				          << " captured_count=" << baseline.persisted_message_count
				          << " captured_digest=" << baseline.persisted_messages_digest
				          << " reloaded_count=" << expected_metadata.persisted_message_count
				          << " reloaded_digest=" << expected_metadata.persisted_messages_digest << std::endl;
			}
			UAM_ASSERT(fixture.app.native_chat_refresh_error_status.empty());
			const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, baseline.id, true);
			const auto* current = ChatDomainService().FindChatById(fixture.app, baseline.id);
			UAM_ASSERT(saved && current && saved->messages_loaded && current->messages_loaded);
			UAM_ASSERT_EQ(current->messages, saved->messages);
			if (grow)
			{
				UAM_ASSERT_EQ(saved->messages.size(), source.front().messages.size());
				UAM_ASSERT_EQ(saved->messages[1].content, source.front().messages[1].content);
				UAM_ASSERT_EQ(saved->messages.back().content, source.front().messages.back().content);
			}
			else
			{
				UAM_ASSERT_EQ(saved->messages, baseline.messages);
				UAM_ASSERT_EQ(saved->updated_at, expected_metadata.updated_at);
			}
			const auto summary = ChatRepository::LoadLocalChatSummaryReadOnly(fixture.app.data_root, baseline.id);
			UAM_ASSERT(summary);
			AssertTerminalFinalMetadataPreserved(*summary, expected_metadata);
			AssertTerminalFinalMetadataPreserved(*saved, expected_metadata);
			AssertTerminalFinalMetadataPreserved(*current, expected_metadata);
			AssertUnknownHistoryOriginalsPreserved(fixture, originals);
		}
	}
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
namespace
{
	struct NativeOverlayPrivateReadDenial
	{
		fs::path primary;
		fs::perms original;
		explicit NativeOverlayPrivateReadDenial(const fs::path& path) : primary(path), original(fs::status(path).permissions())
		{
			fs::permissions(primary, fs::perms::none, fs::perm_options::replace);
		}
		~NativeOverlayPrivateReadDenial()
		{
			std::error_code error;
			fs::permissions(primary, original, fs::perm_options::replace, error);
		}
	};

	void CheckNativeOverlayFailedHydration(bool fuller_saved)
	{
		GeminiOwnershipFixture fixture;
		fs::permissions(fixture.temp.root, fs::perms::owner_all, fs::perm_options::replace);
		UAM_ASSERT_EQ(fs::status(fixture.temp.root).permissions(), fs::perms::owner_all);
		const auto private_home = fixture.temp.root / "hydration-os-home";
		fs::create_directories(private_home);
		ScopedEnvVar os_home("HOME", uam::paths::Utf8PathString(private_home));
		ScopedEnvVar os_profile("USERPROFILE", uam::paths::Utf8PathString(private_home));
		uam::AppState app;
		app.data_root = fixture.temp.root / "hydration-data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatFolder folder;
		folder.id = "hydration-folder";
		folder.title = "Private hydration workspace";
		folder.directory = fixture.workspace;
		folder.execution_host_id = "local";
		app.folders.push_back(folder);
		UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
		const auto initial = ChatHistorySyncService().ImportAllNativeChatsToLocal(app, false);
		UAM_ASSERT(initial.success && initial.total_count == 1 && initial.imported_count == 1 && initial.published_chat_ids.size() == 1);
		auto saved = ChatRepository::LoadLocalChat(app.data_root, initial.published_chat_ids.front(), true);
		UAM_ASSERT(saved && saved->messages_loaded && saved->messages.size() == 2);
		UAM_ASSERT(!uam::strings::StartsWith(saved->id, "chat-"));
		UAM_ASSERT_EQ(saved->native_session_id, kParent);
		UAM_ASSERT_EQ(saved->workspace_directory, fixture.workspace);
		saved->messages.back().content = "Preserve actual saved answer";
		if (fuller_saved) saved->messages.push_back({MessageRole::User, "Preserve actual additional saved turn"});
		saved->updated_at = fuller_saved ? "2026-10-16T11:00:00Z" : "2026-10-16T14:00:00Z";
		saved->title = "Keep hydration owner title";
		saved->pinned = true;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *saved));
		saved = ChatRepository::LoadLocalChat(app.data_root, saved->id, true);
		UAM_ASSERT(saved && saved->messages_loaded && saved->messages.size() == (fuller_saved ? 3U : 2U));
		const auto cold_before = ChatRepository::LoadLocalChat(app.data_root, saved->id, false);
		UAM_ASSERT(cold_before && !cold_before->messages_loaded && cold_before->messages.empty() && cold_before->persisted_message_count == saved->messages.size());
		UAM_ASSERT_EQ(cold_before->updated_at, saved->updated_at);
		app.chats = {*saved};
		ChatDomainService().SelectChatById(app, saved->id);
		app.composer_text = "Keep hydration draft";
		app.resolved_native_sessions_by_chat_id[saved->id] = kParent;
		UAM_ASSERT(app.cli_terminals.empty() && app.acp_sessions.empty() && app.pending_chat_save_at_by_chat_id.empty() && app.worktree_operation_chat_ids.empty());
		auto header = Header();
		header["startTime"] = "2026-10-16T09:00:00Z";
		header["lastUpdated"] = "2026-10-16T13:00:00Z";
		const auto native_path = fixture.chats / (kParent + ".jsonl");
		const auto native_bytes = Line(header) +
		    Line({{"id", "hydration-user"}, {"type", "user"}, {"timestamp", "2026-10-16T09:00:00Z"}, {"content", "Actual native question"}}) +
		    Line({{"id", "hydration-answer"}, {"type", "gemini"}, {"timestamp", "2026-10-16T13:00:00Z"}, {"content", "Actual older or shorter native answer"}});
		UAM_ASSERT(uam::io::WriteTextFile(native_path, native_bytes));
		const auto profile = ProviderProfileStore::DefaultGeminiProfile();
		auto incoming = ChatHistorySyncService().LoadNativeSessionChats(fixture.chats, profile);
		UAM_ASSERT(incoming.size() == 1 && incoming.front().messages_loaded && incoming.front().messages.size() == 2);
		UAM_ASSERT_EQ(incoming.front().native_session_id, kParent);
		UAM_ASSERT_EQ(incoming.front().workspace_directory, fixture.workspace);
		UAM_ASSERT_EQ(incoming.front().updated_at, std::string("2026-10-16T13:00:00Z"));
		UAM_ASSERT_EQ(incoming.front().messages.back().content, std::string("Actual older or shorter native answer"));
		const auto native_messages = incoming.front().messages;
		const auto primary = AppPaths::UamChatFilePath(app.data_root, saved->id);
		const auto summary = AppPaths::UamChatSummaryFilePath(app.data_root, saved->id);
		const auto primary_mode = fs::status(primary).permissions();
		const auto primary_size = fs::file_size(primary);
		const auto primary_time = fs::last_write_time(primary);
		const auto primary_revision = ChatRepository::ChatStorageRevision(app.data_root, saved->id);
		const auto summary_revision = ChatRepository::ChatSummaryStorageRevision(app.data_root, saved->id);
		struct RetainedFile
		{
			fs::path path;
			std::string bytes;
			fs::perms mode;
		};
		std::vector<RetainedFile> retained;
		for (const auto& root : {app.data_root, fixture.storage})
		{
			for (const auto& entry : fs::recursive_directory_iterator(root))
			{
				if (!entry.is_regular_file()) continue;
				std::string bytes;
				UAM_ASSERT(uam::io::TryReadTextFile(entry.path(), bytes));
				retained.push_back({entry.path(), bytes, entry.status().permissions()});
			}
		}
		{
			NativeOverlayPrivateReadDenial denied(primary);
			UAM_ASSERT(fs::is_regular_file(primary) && fs::file_size(primary) == primary_size && fs::last_write_time(primary) == primary_time);
			UAM_ASSERT_EQ(fs::status(primary).permissions(), fs::perms::none);
			std::string inaccessible, readable_summary;
			UAM_ASSERT(!uam::io::TryReadTextFile(primary, inaccessible)); // A root/ACL override is a failed prerequisite, never a product RED.
			UAM_ASSERT(uam::io::TryReadTextFile(summary, readable_summary) && !readable_summary.empty());
			const auto genuine_cold_rows = ChatRepository::LoadLocalChatSummaries(app.data_root);
			UAM_ASSERT(genuine_cold_rows.size() == 1 && genuine_cold_rows.front().id == saved->id);
			UAM_ASSERT(!genuine_cold_rows.front().messages_loaded && genuine_cold_rows.front().messages.empty() && genuine_cold_rows.front().persisted_message_count == saved->messages.size());
			UAM_ASSERT_EQ(genuine_cold_rows.front().updated_at, saved->updated_at);
			auto failed_hydration = genuine_cold_rows.front();
			std::string warning;
			UAM_ASSERT(!ChatRepository::HydrateChatMessages(app.data_root, failed_hydration, &warning));
			UAM_ASSERT(!warning.empty() && !failed_hydration.messages_loaded && failed_hydration.messages.empty());
			// No prepared summaries argument: the real caller chooses/loads its own
			// cold catalogue and attempts actual full hydration under this denial.
			(void)ChatHistorySyncService().OverlayLocalHistory(app, incoming, false);
		}
		// Restore and prove all private evidence BEFORE the intended feature oracle.
		UAM_ASSERT_EQ(fs::status(primary).permissions(), primary_mode);
		UAM_ASSERT_EQ(fs::file_size(primary), primary_size);
		UAM_ASSERT_EQ(fs::last_write_time(primary), primary_time);
		const auto assert_retained = [&]()
		{
			for (const auto& file : retained)
			{
				std::string current;
				UAM_ASSERT(uam::io::TryReadTextFile(file.path, current));
				UAM_ASSERT_EQ(current, file.bytes);
				UAM_ASSERT_EQ(fs::status(file.path).permissions(), file.mode);
			}
		};
		assert_retained();
		UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(app.data_root, saved->id), primary_revision);
		UAM_ASSERT_EQ(ChatRepository::ChatSummaryStorageRevision(app.data_root, saved->id), summary_revision);
		const auto saved_after = ChatRepository::LoadLocalChat(app.data_root, saved->id, true);
		UAM_ASSERT(saved_after && saved_after->messages_loaded);
		UAM_ASSERT_EQ(saved_after->messages, saved->messages);
		UAM_ASSERT_EQ(saved_after->updated_at, saved->updated_at);
		UAM_ASSERT_EQ(app.chats.front().messages, saved->messages); // Detached older native data was never adopted as the live owner.
		UAM_ASSERT_EQ(app.composer_text, std::string("Keep hydration draft"));
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), saved->id);
		UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.at(saved->id), kParent);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(native_path), native_bytes);
		// Actual fresh read after restoring access proves normal saved precedence
		// still hydrates successfully, without reusing the failed input vector.
		auto retry = ChatHistorySyncService().LoadNativeSessionChats(fixture.chats, profile);
		UAM_ASSERT(retry.size() == 1 && retry.front().messages_loaded && retry.front().messages == native_messages);
		const auto retry_locals = ChatHistorySyncService().OverlayLocalHistory(app, retry, false);
		UAM_ASSERT(retry_locals.size() == 1 && retry_locals.front().messages_loaded);
		UAM_ASSERT_EQ(retry.front().messages, saved->messages);
		UAM_ASSERT_EQ(retry.front().updated_at, saved->updated_at);
		assert_retained();
		// Intended first feature oracle: failed hydration cannot turn actual full
		// native content into a falsely loaded empty conversation. This call does
		// not request the optional error: no retry/refusal shape is asserted, and
		// a rejected/removed candidate alone is not claimed visible recovery.
		for (const auto& candidate : incoming)
		{
			UAM_ASSERT(!candidate.messages_loaded || !candidate.messages.empty());
		}
	}
}

UAM_TEST(NativeOverlayFailedHydrationCannotEraseKnownNewerSavedHistory)
{
	CheckNativeOverlayFailedHydration(false);
}

UAM_TEST(NativeOverlayFailedHydrationCannotEraseFullerSavedHistory)
{
	CheckNativeOverlayFailedHydration(true);
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
namespace
{
	struct NativeOverlayPropagationFixture
	{
		GeminiOwnershipFixture native;
		ScopedEnvVar os_home{"HOME", uam::paths::Utf8PathString(native.temp.root / "propagation-os-home")};
		ScopedEnvVar os_profile{"USERPROFILE", uam::paths::Utf8PathString(native.temp.root / "propagation-os-home")};
		uam::AppState app;
		ChatSession saved, unrelated;
		fs::path primary;
		std::string native_bytes;
		struct RetainedFile
		{
			fs::path path;
			std::string bytes;
			fs::perms mode;
		};
		std::vector<RetainedFile> retained;

		NativeOverlayPropagationFixture()
		{
			fs::permissions(native.temp.root, fs::perms::owner_all, fs::perm_options::replace);
			UAM_ASSERT_EQ(fs::status(native.temp.root).permissions(), fs::perms::owner_all);
			fs::create_directories(native.temp.root / "propagation-os-home");
			app.data_root = native.temp.root / "propagation-data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatFolder folder;
			folder.id = "propagation-folder";
			folder.title = "Private propagation workspace";
			folder.directory = native.workspace;
			folder.execution_host_id = "local";
			app.folders.push_back(folder);
			UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
			const auto imported = ChatHistorySyncService().ImportAllNativeChatsToLocal(app, false);
			UAM_ASSERT(imported.success && imported.imported_count == 1 && imported.published_chat_ids.size() == 1);
			auto loaded = ChatRepository::LoadLocalChat(app.data_root, imported.published_chat_ids.front(), true);
			UAM_ASSERT(loaded && loaded->messages_loaded && loaded->messages.size() == 2);
			UAM_ASSERT(!uam::strings::StartsWith(loaded->id, "chat-"));
			loaded->title = "Keep public propagation title";
			loaded->pinned = true;
			loaded->updated_at = "2026-10-16T14:00:00Z";
			loaded->messages.back().content = "Keep saved newer propagation answer";
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *loaded));
			loaded = ChatRepository::LoadLocalChat(app.data_root, loaded->id, true);
			UAM_ASSERT(loaded && loaded->messages_loaded && loaded->messages.size() == 2);
			saved = *loaded;
			unrelated = ChatDomainService().CreateNewChat(folder.id, "codex-cli");
			unrelated.workspace_directory = native.workspace;
			unrelated.messages.push_back({MessageRole::User, "Keep unrelated propagation history"});
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, unrelated));
			const auto real_unrelated = ChatRepository::LoadLocalChat(app.data_root, unrelated.id, true);
			UAM_ASSERT(real_unrelated && real_unrelated->messages_loaded);
			unrelated = *real_unrelated;
			app.chats = {unrelated, saved};
			ChatDomainService().SelectChatById(app, saved.id);
			app.composer_text = "Keep public propagation draft";
			app.resolved_native_sessions_by_chat_id[saved.id] = kParent;
			primary = AppPaths::UamChatFilePath(app.data_root, saved.id);
			UAM_ASSERT(app.cli_terminals.empty() && app.acp_sessions.empty() && app.pending_chat_save_at_by_chat_id.empty() && app.worktree_operation_chat_ids.empty());
			WriteNative("2026-10-16T13:00:00Z", "Older actual propagation native answer");
			const auto parsed = ReadNative();
			UAM_ASSERT(parsed.size() == 1 && parsed.front().messages_loaded && parsed.front().messages.size() == 2);
			UAM_ASSERT_EQ(parsed.front().native_session_id, kParent);
			UAM_ASSERT_EQ(parsed.front().workspace_directory, native.workspace);
			UAM_ASSERT_EQ(parsed.front().updated_at, std::string("2026-10-16T13:00:00Z"));
			for (const auto& root : {app.data_root, native.storage})
			{
				for (const auto& entry : fs::recursive_directory_iterator(root))
				{
					if (!entry.is_regular_file()) continue;
					std::string bytes;
					UAM_ASSERT(uam::io::TryReadTextFile(entry.path(), bytes));
					retained.push_back({entry.path(), bytes, entry.status().permissions()});
				}
			}
		}

		void WriteNative(const std::string& timestamp, const std::string& answer)
		{
			auto header = Header();
			header["startTime"] = "2026-10-16T09:00:00Z";
			header["lastUpdated"] = timestamp;
			native_bytes = Line(header) +
			    Line({{"id", "propagation-user"}, {"type", "user"}, {"timestamp", "2026-10-16T09:00:00Z"}, {"content", "Actual propagation question"}}) +
			    Line({{"id", "propagation-answer"}, {"type", "gemini"}, {"timestamp", timestamp}, {"content", answer}});
			UAM_ASSERT(uam::io::WriteTextFile(native.chats / (kParent + ".jsonl"), native_bytes));
		}

		std::vector<ChatSession> ReadNative() const
		{
			return ChatHistorySyncService().LoadNativeSessionChats(native.chats, ProviderProfileStore::DefaultGeminiProfile());
		}

		void AssertDeniedPrerequisites() const
		{
			std::string inaccessible;
			UAM_ASSERT(fs::is_regular_file(primary) && !uam::io::TryReadTextFile(primary, inaccessible));
			const auto summary = ChatRepository::LoadLocalChat(app.data_root, saved.id, false);
			UAM_ASSERT(summary && summary->id == saved.id && !summary->messages_loaded && summary->messages.empty() && summary->persisted_message_count == 2);
			UAM_ASSERT_EQ(summary->updated_at, saved.updated_at);
			auto attempted = *summary;
			std::string warning;
			UAM_ASSERT(!ChatRepository::HydrateChatMessages(app.data_root, attempted, &warning) && !warning.empty());
		}

		void AssertOriginalFiles() const
		{
			for (const auto& file : retained)
			{
				std::string bytes;
				UAM_ASSERT(uam::io::TryReadTextFile(file.path, bytes));
				UAM_ASSERT_EQ(bytes, file.bytes);
				UAM_ASSERT_EQ(fs::status(file.path).permissions(), file.mode);
			}
		}

		void AssertCatalogueRetained() const
		{
			UAM_ASSERT_EQ(app.chats.size(), std::size_t{2});
			for (const auto* expected : {&saved, &unrelated})
			{
				const auto* current = ChatDomainService().FindChatById(app, expected->id);
				UAM_ASSERT(current && current->messages_loaded);
				UAM_ASSERT_EQ(current->messages, expected->messages);
				UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(app.data_root, *current), ChatRepository::ChatMetadataFingerprint(app.data_root, *expected));
			}
			UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), saved.id);
			UAM_ASSERT_EQ(app.composer_text, std::string("Keep public propagation draft"));
			UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.at(saved.id), kParent);
		}

		void AssertHealthyDisplayRetry() const
		{
			const auto* live = ChatDomainService().FindChatById(app, saved.id);
			UAM_ASSERT(live && live->messages_loaded && live->messages.size() == 2);
			UAM_ASSERT_EQ(live->messages.back().content, std::string("Fresh actual propagation answer"));
			UAM_ASSERT_EQ(live->updated_at, std::string("2026-10-16T15:00:00Z"));
			UAM_ASSERT_EQ(live->id, saved.id);
			UAM_ASSERT_EQ(live->native_session_id, kParent);
			UAM_ASSERT_EQ(live->workspace_directory, native.workspace);
			UAM_ASSERT_EQ(live->title, saved.title);
			UAM_ASSERT(live->pinned);
			UAM_ASSERT_EQ(app.composer_text, std::string("Keep public propagation draft"));
			UAM_ASSERT_EQ(uam::io::ReadTextFile(native.chats / (kParent + ".jsonl")), native_bytes);
		}

		void AssertHealthySavedRetry() const
		{
			AssertHealthyDisplayRetry();
			const auto durable = ChatRepository::LoadLocalChat(app.data_root, saved.id, true);
			const auto* live = ChatDomainService().FindChatById(app, saved.id);
			UAM_ASSERT(durable && durable->messages_loaded && live);
			UAM_ASSERT_EQ(live->messages, durable->messages);
			UAM_ASSERT_EQ(durable->updated_at, std::string("2026-10-16T15:00:00Z"));
			UAM_ASSERT_EQ(durable->title, saved.title);
			UAM_ASSERT(durable->pinned);
		}
	};
}

UAM_TEST(NativeOverlayHydrationFailureSurfacesThroughActualSyncAndRetries)
{
	NativeOverlayPropagationFixture fixture;
	UAM_ASSERT(fixture.app.status_line.empty() && fixture.app.native_chat_refresh_error_status.empty());
	bool synchronized = true;
	{
		NativeOverlayPrivateReadDenial denied(fixture.primary);
		fixture.AssertDeniedPrerequisites();
		synchronized = uam::SyncChatsFromNative(fixture.app, "", true);
	}
	fixture.AssertOriginalFiles();
	fixture.AssertCatalogueRetained();
	UAM_ASSERT(!synchronized);
	UAM_ASSERT(!fixture.app.native_chat_refresh_error_status.empty());
	UAM_ASSERT_EQ(fixture.app.status_line, fixture.app.native_chat_refresh_error_status);
	fixture.WriteNative("2026-10-16T15:00:00Z", "Fresh actual propagation answer");
	UAM_ASSERT(uam::SyncChatsFromNative(fixture.app, "", true));
	fixture.AssertHealthySavedRetry();
	UAM_ASSERT(fixture.app.native_chat_refresh_error_status.empty() && fixture.app.native_chat_refresh_error_target_id.empty());
}

UAM_TEST(NativeOverlayHydrationFailurePreservesApplyCatalogueAndRetries)
{
	NativeOverlayPropagationFixture fixture;
	UAM_ASSERT(fixture.app.status_line.empty());
	auto incoming = fixture.ReadNative();
	UAM_ASSERT(incoming.size() == 1 && incoming.front().messages_loaded && incoming.front().messages.size() == 2);
	{
		NativeOverlayPrivateReadDenial denied(fixture.primary);
		fixture.AssertDeniedPrerequisites();
		ChatHistorySyncService().ApplyLocalOverrides(fixture.app, incoming, false);
	}
	fixture.AssertOriginalFiles();
	fixture.AssertCatalogueRetained();
	UAM_ASSERT(!fixture.app.status_line.empty());
	fixture.WriteNative("2026-10-16T15:00:00Z", "Fresh actual propagation answer");
	// Actual display-only retry: no Sync preconditioning may adopt these messages
	// first and mask whether ApplyLocalOverrides itself reconciles fresh input.
	incoming = fixture.ReadNative();
	UAM_ASSERT(incoming.size() == 1 && incoming.front().messages_loaded && incoming.front().messages.size() == 2);
	UAM_ASSERT_EQ(incoming.front().messages.back().content, std::string("Fresh actual propagation answer"));
	ChatHistorySyncService().ApplyLocalOverrides(fixture.app, incoming, false);
	fixture.AssertHealthyDisplayRetry();
	const auto still_saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.saved.id, true);
	UAM_ASSERT(still_saved && still_saved->messages_loaded);
	UAM_ASSERT_EQ(still_saved->messages, fixture.saved.messages);
	UAM_ASSERT_EQ(still_saved->updated_at, fixture.saved.updated_at);
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	void CheckGeminiTerminalFinalTimestampMaximum(bool native_is_newer)
	{
		// Historical instants in this private UTC0 process are unambiguous. This
		// fixture makes no claim to reconstruct the timezone of old real records.
		NativeTimestampPrivateTimezone timezone;
		const auto metadata_time = std::chrono::sys_days{std::chrono::year{2026} / 10 / 16} + std::chrono::hours{14};
		const auto metadata_epoch = std::chrono::system_clock::to_time_t(metadata_time);
		const auto conversation_epoch = metadata_epoch - 3 * 3600;
		const auto created_epoch = conversation_epoch - 3600;
		const auto native_epoch = metadata_epoch + (native_is_newer ? 3600 : -3600);
		UAM_ASSERT(created_epoch < conversation_epoch && conversation_epoch < native_epoch);
		UAM_ASSERT(native_is_newer ? native_epoch > metadata_epoch : native_epoch < metadata_epoch);
		const auto iso_stamp = [](std::time_t instant)
		{
			return uam::time::detail::FormatTimestamp(uam::time::detail::UtcTimeSnapshot(instant), uam::time::kIsoUtcTimestampFormat);
		};
		const auto created_stamp = iso_stamp(created_epoch);
		const auto conversation_stamp = iso_stamp(conversation_epoch);
		const auto native_stamp = iso_stamp(native_epoch);
		const auto metadata_stamp = uam::time::detail::FormatTimestamp(uam::time::detail::LocalTimeSnapshot(metadata_epoch), uam::time::kLocalDisplayTimestampFormat);
		UAM_ASSERT_EQ(created_stamp, std::string("2026-10-16T10:00:00.000Z"));
		UAM_ASSERT_EQ(conversation_stamp, std::string("2026-10-16T11:00:00.000Z"));
		UAM_ASSERT_EQ(metadata_stamp, std::string("2026-10-16 14:00:00"));
		UAM_ASSERT_EQ(native_stamp, native_is_newer ? std::string("2026-10-16T15:00:00.000Z") : std::string("2026-10-16T13:00:00.000Z"));

		GeminiTerminalOriginalFixture fixture;
		auto& app = fixture.app;
		const auto native_file = fixture.owned.chats / (kParent + ".jsonl");
		const auto original_header = Line(Header());
		const auto media_journal = uam::io::ReadTextFile(native_file);
		const auto plain_journal = Journal();
		UAM_ASSERT(media_journal.starts_with(original_header) && plain_journal.starts_with(original_header));
		auto header = Header();
		header["startTime"] = created_stamp;
		header["lastUpdated"] = conversation_stamp;
		// Derive the older saved graph from the REAL reader, not a constructed
		// transcript or later metadata timestamp masquerading as conversation time.
		const auto old_bytes = Line(header) + plain_journal.substr(original_header.size()) +
		    Line({{"$patch", {{"id", "u"}, {"timestamp", created_stamp}}}}) +
		    Line({{"$patch", {{"id", "a"}, {"timestamp", conversation_stamp}, {"content", "Previously saved native answer"}}}});
		UAM_ASSERT(uam::io::WriteTextFile(native_file, old_bytes));
		const auto old_source = fixture.Snapshot();
		UAM_ASSERT_EQ(old_source.front().updated_at, conversation_stamp);
		UAM_ASSERT_EQ(old_source.front().created_at, created_stamp);
		UAM_ASSERT_EQ(old_source.front().messages.back().content, std::string("Previously saved native answer"));
		for (const auto& message : old_source.front().messages) UAM_ASSERT(message.attachments.empty());
		auto* live = ChatDomainService().FindChatById(app, fixture.local.id);
		UAM_ASSERT(live && live->messages_loaded && live->messages.size() == 1);
		UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(*live), uam::chat_identity::NativeIdentityKeyForHistoryImport(old_source.front()));
		live->messages = old_source.front().messages;
		live->created_at = old_source.front().created_at;
		live->updated_at = old_source.front().updated_at;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *live));
		const auto baseline = ChatRepository::LoadLocalChat(app.data_root, live->id, true);
		UAM_ASSERT(baseline && baseline->messages_loaded && baseline->messages == old_source.front().messages);
		UAM_ASSERT_EQ(baseline->updated_at, conversation_stamp);
		*live = *baseline;
		fixture.local = *baseline;
		fixture.local_bytes = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, live->id));
		const auto captured_messages = live->messages;
		const auto captured_birth = live->metadata_incarnation;

		header["lastUpdated"] = native_stamp;
		const auto new_bytes = Line(header) + media_journal.substr(original_header.size()) +
		    Line({{"$patch", {{"id", "u"}, {"timestamp", created_stamp}}}}) +
		    Line({{"$patch", {{"id", "a"}, {"timestamp", native_stamp}}}});
		UAM_ASSERT(uam::io::WriteTextFile(native_file, new_bytes));
		for (auto& [file, expected] : fixture.unchanged_files) if (file == native_file) expected = new_bytes;
		const auto source = fixture.Snapshot();
		UAM_ASSERT_EQ(source.front().updated_at, native_stamp);
		UAM_ASSERT_EQ(source.front().messages.size(), captured_messages.size());
		UAM_ASSERT_EQ(source.front().messages.back().content, fixture.expected_answer);
		UAM_ASSERT(source.front().messages != captured_messages && !fixture.HasPublished());
		for (const auto& [row, expected] : fixture.parts)
		{
			const auto& attachments = source.front().messages[row].attachments;
			const auto found = std::ranges::find_if(attachments, [&](const auto& value) { return value.id == expected.id; });
			UAM_ASSERT(found != attachments.end() && found->source == AttachmentSource::NativeInlinePending);
			UAM_ASSERT(found->native_inline_payload && found->native_part_key == expected.native_part_key);
		}

		// Actual close captures original conversation11 BEFORE genuine metadata14
		// save. Existing fixture handles are invalid: this is not an OS-exit test.
		UAM_ASSERT_EQ(live->updated_at, conversation_stamp);
		fixture.CloseAfterFinalCapture();
		UAM_ASSERT(app.cli_terminals.empty() && ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
		UAM_ASSERT(ChatHistorySyncService().TerminalNativeMediaPending(app));
		live = ChatDomainService().FindChatById(app, baseline->id);
		UAM_ASSERT(live && live->metadata_incarnation == captured_birth && live->messages == captured_messages);
		live->model_id = "newest-terminal-metadata-model";
		live->reasoning_effort = "high";
		live->updated_at = metadata_stamp;
		UAM_ASSERT(ChatHistorySyncService().SaveChatWithStatus(app, *live,
		    "Private terminal metadata saved.", "Private terminal metadata save failed."));
		const auto saved_metadata = ChatRepository::LoadLocalChat(app.data_root, live->id, true);
		const auto summary_metadata = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, live->id);
		UAM_ASSERT(saved_metadata && summary_metadata && saved_metadata->messages == captured_messages);
		UAM_ASSERT_EQ(saved_metadata->updated_at, metadata_stamp);
		UAM_ASSERT_EQ(summary_metadata->updated_at, metadata_stamp);
		UAM_ASSERT_EQ(saved_metadata->model_id, live->model_id);
		const ChatSession expected = *saved_metadata;
		fixture.local = expected;
		const auto mappings = app.resolved_native_sessions_by_chat_id;
		UAM_ASSERT(!fixture.HasPublished());
		fixture.AwaitPublished(); // Only existing public Poll; no repeated Sync or injected completion.
		const auto keys = fixture.AssertPublished(); // Saved/live new graph and exact user/tool PNG originals FIRST.
		UAM_ASSERT_EQ(keys.size(), std::size_t{2});
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, expected.id, true);
		const auto summary = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, expected.id);
		const auto* selected = ChatDomainService().SelectedChat(app);
		UAM_ASSERT(saved && summary && selected && selected->metadata_incarnation == captured_birth);
		UAM_ASSERT_EQ(saved->messages.back().created_at, native_stamp);
		UAM_ASSERT(saved->messages != captured_messages && saved->messages == selected->messages);
		UAM_ASSERT(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, expected.id)) != fixture.local_bytes);
		UAM_ASSERT(app.resolved_native_sessions_by_chat_id == mappings);
		UAM_ASSERT(app.cli_terminals.empty() && !ChatHistorySyncService().TerminalNativeMediaPending(app));
		UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaFinalPending(app));
		UAM_ASSERT(app.native_chat_refresh_error_status.empty() && app.native_chat_refresh_error_target_id.empty());
		for (const ChatSession* actual : {&*saved, &*summary, selected})
		{
			AssertTerminalFinalMetadataPreserved(*actual, expected);
			UAM_ASSERT(actual->custom_icon == expected.custom_icon);
		}
		// Content admission/owned bytes, identity, metadata and final ACK have all
		// passed. Only this maximum differs between native13 and native15 cases.
		const auto& expected_maximum = native_is_newer ? native_stamp : metadata_stamp;
		for (const ChatSession* actual : {&*saved, &*summary, selected})
			UAM_ASSERT_EQ(actual->updated_at, expected_maximum);
	}
}

UAM_TEST(GeminiTerminalFinalKeepsNewerSavedTimestampWhilePublishingNativeOriginals)
{
	CheckGeminiTerminalFinalTimestampMaximum(false);
}

UAM_TEST(GeminiTerminalFinalAdvancesTimestampForNewerNativeOriginals)
{
	CheckGeminiTerminalFinalTimestampMaximum(true);
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI
namespace
{
	void CheckGeminiDuplicateNativeTimestampImport(bool mixed_offset)
	{
		GeminiOwnershipFixture owned;
		const auto private_home = owned.temp.root / "dedup-os-home";
		fs::create_directories(private_home);
		ScopedEnvVar os_home("HOME", uam::paths::Utf8PathString(private_home));
		ScopedEnvVar os_profile("USERPROFILE", uam::paths::Utf8PathString(private_home));
		ScopedEnvVar data_home("UAM_DATA_DIR", uam::paths::Utf8PathString(owned.temp.root / "dedup-data"));
		const auto provider = ProviderProfileStore::DefaultGeminiProfile();
		const std::string older_stamp = mixed_offset ? "2026-10-16T14:00:00+02:00" : "2026-10-16T12:00:00Z";
		const std::string newer_stamp = "2026-10-16T13:00:00Z";
		const std::string older_answer = "Older native duplicate answer at noon UTC";
		const std::string newer_answer = "Newer native duplicate answer at thirteen UTC";
		const auto journal = [&](const std::string& stamp, const std::string& answer)
		{
			auto header = Header();
			header["startTime"] = "2026-10-16T09:00:00Z";
			header["lastUpdated"] = stamp;
			return Line(header) +
			    Line({{"id", "dedup-u"}, {"type", "user"}, {"timestamp", "2026-10-16T09:00:00Z"}, {"content", "Actual duplicate native question"}}) +
			    Line({{"id", "dedup-a"}, {"type", "gemini"}, {"timestamp", stamp}, {"content", answer}});
		};
		const auto older_file = owned.chats / (kParent + ".jsonl");
		const auto newer_file = owned.chats / ("session-replica-" + kParent + ".jsonl");
		const auto older_bytes = journal(older_stamp, older_answer);
		const auto newer_bytes = journal(newer_stamp, newer_answer);
		UAM_ASSERT(uam::io::WriteTextFile(older_file, older_bytes));
		UAM_ASSERT(uam::io::WriteTextFile(newer_file, newer_bytes));

		// Genuine raw reader prerequisite: both files are readable and attested
		// in the same native project before the APP catalogue deduplicates them.
		const auto raw = LoadGeminiJsonHistoryForRuntime(owned.chats, provider, {});
		UAM_ASSERT_EQ(raw.size(), std::size_t{2});
		const auto older = std::ranges::find_if(raw, [&](const auto& chat)
		{
			return chat.messages_loaded && chat.messages.size() == 2 && chat.messages.back().content == older_answer;
		});
		const auto newer = std::ranges::find_if(raw, [&](const auto& chat)
		{
			return chat.messages_loaded && chat.messages.size() == 2 && chat.messages.back().content == newer_answer;
		});
		UAM_ASSERT(older != raw.end() && newer != raw.end());
		for (const ChatSession* source : {&*older, &*newer})
		{
			UAM_ASSERT_EQ(source->native_session_id, kParent);
			UAM_ASSERT_EQ(source->provider_id, provider.id);
			UAM_ASSERT_EQ(source->workspace_directory, owned.workspace);
			UAM_ASSERT_EQ(source->execution_host_id, std::string("local"));
			UAM_ASSERT_EQ(source->messages.front().content, std::string("Actual duplicate native question"));
			UAM_ASSERT_EQ(source->messages.front().role, MessageRole::User);
			UAM_ASSERT_EQ(source->messages.back().role, MessageRole::Assistant);
		}
		UAM_ASSERT_EQ(older->id, newer->id);
		UAM_ASSERT_EQ(older->updated_at, older_stamp);
		UAM_ASSERT_EQ(newer->updated_at, newer_stamp);
		UAM_ASSERT_EQ(older->messages.back().created_at, older_stamp);
		UAM_ASSERT_EQ(newer->messages.back().created_at, newer_stamp);
		UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(*older), uam::chat_identity::NativeIdentityKeyForHistoryImport(*newer));
		// 14:00+02:00 and 12:00Z are the same noon instant; both are strictly
		// before 13:00Z. No legacy-local/TZ interpretation is involved here.

		uam::AppState app;
		app.data_root = owned.temp.root / "dedup-data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatFolder folder;
		folder.id = "dedup-native-folder";
		folder.title = "Private duplicate native workspace";
		folder.directory = owned.workspace;
		folder.execution_host_id = "local";
		app.folders.push_back(folder);
		UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
		auto unrelated = ChatDomainService().CreateNewChat(folder.id, "codex-cli");
		unrelated.created_at = "2026-10-16T08:00:00Z";
		unrelated.updated_at = "2026-10-16T10:00:00Z";
		unrelated.workspace_directory = owned.workspace;
		unrelated.messages = {{MessageRole::User, "Unrelated selected duplicate-import draft context"}};
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, unrelated));
		const auto unrelated_saved = ChatRepository::LoadLocalChat(app.data_root, unrelated.id, true);
		UAM_ASSERT(unrelated_saved && unrelated_saved->messages_loaded);
		app.chats = {*unrelated_saved};
		ChatDomainService().SelectChatById(app, unrelated.id);
		app.composer_text = "Preserve unrelated duplicate-import draft";
		const auto folders_before = ChatFolderStore::Serialize(app.folders);
		std::vector<std::pair<fs::path, std::string>> unchanged;
		for (const auto& file : {older_file, newer_file, owned.storage / "projects.json", owned.project / ".project_root",
		    owned.chats / kParent / (kChild + ".jsonl"), AppPaths::UamChatFilePath(app.data_root, unrelated.id),
		    AppPaths::UamChatSummaryFilePath(app.data_root, unrelated.id)})
		{
			std::string bytes;
			UAM_ASSERT(uam::io::TryReadTextFile(file, bytes));
			unchanged.emplace_back(file, std::move(bytes));
		}
		UAM_ASSERT(app.cli_terminals.empty() && app.acp_sessions.empty() && app.pending_chat_save_at_by_chat_id.empty());
		UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));

		const std::string selected_before_import = ChatDomainService().SelectedChatId(app);
		UAM_ASSERT_EQ(selected_before_import, unrelated.id);
		// Actual public importer invokes APP native-catalogue dedup before local
		// overlay and persistence. First import has no preexisting owner winner.
		const auto result = ChatHistorySyncService().ImportAllNativeChatsToLocal(app, false, kParent);
		UAM_ASSERT(result.success && result.errors.empty() && result.imported_count == 1 && result.published_chat_ids.size() == 1);
		const auto published_id = result.published_chat_ids.front();
		ChatHistorySyncService().MergeSidebarChatsPreservingCurrent(app, result.published_chat_ids);
		// Mirror the real public sync caller: merge rows, then finalize stable selection.
		UAM_ASSERT(uam::FinalizeChatSyncSelection(app, selected_before_import, kParent, true));
		auto* live = ChatDomainService().FindChatById(app, published_id);
		UAM_ASSERT(live && ChatRepository::HydrateChatMessages(app.data_root, *live));
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, published_id, true);
		UAM_ASSERT(saved && saved->messages_loaded && live->messages_loaded);
		UAM_ASSERT_EQ(saved->messages.size(), std::size_t{2});
		UAM_ASSERT_EQ(saved->messages, live->messages);
		UAM_ASSERT_EQ(saved->native_session_id, kParent);
		UAM_ASSERT_EQ(saved->provider_id, provider.id);
		UAM_ASSERT_EQ(saved->workspace_directory, owned.workspace);
		UAM_ASSERT_EQ(saved->execution_host_id, std::string("local"));
		UAM_ASSERT_EQ(saved->folder_id, folder.id);
		UAM_ASSERT_EQ(app.chats.size(), std::size_t{2});
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), unrelated.id);
		UAM_ASSERT_EQ(app.composer_text, std::string("Preserve unrelated duplicate-import draft"));
		UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), folders_before);
		const auto other = ChatRepository::LoadLocalChat(app.data_root, unrelated.id, true);
		UAM_ASSERT(other && other->messages == unrelated_saved->messages);
		for (const auto& [file, expected] : unchanged)
		{
			UAM_ASSERT_EQ(uam::io::ReadTextFile(file), expected);
		}
		UAM_ASSERT(!ChatHistorySyncService().TerminalNativeMediaPending(app));
		// First intended feature oracle follows real readable sources, public
		// persistence/live adoption, identity and source-preservation controls.
		UAM_ASSERT_EQ(saved->messages.back().content, newer_answer);
		UAM_ASSERT_EQ(live->messages.back().content, newer_answer);
		UAM_ASSERT_EQ(saved->updated_at, newer_stamp);
		UAM_ASSERT_EQ(live->updated_at, newer_stamp);
	}
}

UAM_TEST(GeminiDuplicateNativeImportChoosesKnownNewerOffsetJournal)
{
	CheckGeminiDuplicateNativeTimestampImport(true);
}

UAM_TEST(GeminiDuplicateNativeImportKeepsSameFormatNewerControl)
{
	CheckGeminiDuplicateNativeTimestampImport(false);
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI
namespace
{
	void CheckDuplicateTimestampBoundary(const std::string& existing_updated, const std::string& candidate_updated,
	    const std::string& existing_created, const std::string& candidate_created, bool replace)
	{
		// Domain policy controls only: these are not claimed as native-reader receipts.
		ChatSession existing;
		existing.id = kParent;
		existing.provider_id = "gemini-cli";
		existing.native_session_id = kParent;
		existing.workspace_directory = "/private-duplicate-domain-boundary";
		existing.updated_at = existing_updated;
		existing.created_at = existing_created;
		existing.messages = {{MessageRole::User, "Duplicate boundary question"}, {MessageRole::Assistant, "Existing duplicate graph"}};
		ChatSession candidate = existing;
		candidate.updated_at = candidate_updated;
		candidate.created_at = candidate_created;
		candidate.messages.back().content = "Candidate duplicate graph";
		candidate.linked_files = {"candidate-context.md"};
		const auto result = ChatDomainService().DeduplicateChatsById({existing, candidate});
		UAM_ASSERT_EQ(result.size(), std::size_t{1});
		UAM_ASSERT_EQ(result.front().messages, replace ? candidate.messages : existing.messages);
		UAM_ASSERT_EQ(result.front().updated_at, replace ? candidate_updated : existing_updated);
		UAM_ASSERT_EQ(result.front().created_at, replace ? candidate_created : existing_created);
		UAM_ASSERT_EQ(result.front().linked_files, replace ? candidate.linked_files : existing.linked_files);
		UAM_ASSERT_EQ(result.front().native_session_id, existing.native_session_id);
		UAM_ASSERT_EQ(result.front().workspace_directory, existing.workspace_directory);
	}
}

UAM_TEST(ChatDomainDuplicateTimestampTiesKeepExistingMetadataPrecedence)
{
	// Equal update instants reach the existing created-time tie-break, not spelling order.
	CheckDuplicateTimestampBoundary("2026-10-16T13:00:00Z", "2026-10-16T15:00:00+02:00",
	    "2026-10-16T11:00:00Z", "2026-10-16T12:00:00+02:00", false);
	CheckDuplicateTimestampBoundary("2026-10-16T13:00:00Z", "2026-10-16T15:00:00+02:00",
	    "2026-10-16T11:00:00Z", "2026-10-16T12:00:00Z", true);
	// Equal known creation instants reach established metadata richness despite different spelling.
	CheckDuplicateTimestampBoundary("2026-10-16T13:00:00Z", "2026-10-16T13:00:00Z",
	    "2026-10-16T11:00:00Z", "2026-10-16T13:00:00+02:00", true);
	// Identical absent creation values remain an existing non-chronological tie.
	CheckDuplicateTimestampBoundary("2026-10-16T13:00:00Z", "2026-10-16T13:00:00Z", "", "", true);
	// Exact equal values supply no new recency claim; preserve the original metadata tie policy.
	CheckDuplicateTimestampBoundary("", "", "", "", true);
	CheckDuplicateTimestampBoundary("unknown-update", "unknown-update",
	    "2026-10-16T11:00:00Z", "2026-10-16T11:00:00Z", true);
}

UAM_TEST(ChatDomainDuplicateChangedUnknownTimesPreserveExistingGraph)
{
	// A differing unknown update value supplies no recency authority, on either side.
	CheckDuplicateTimestampBoundary("2026-10-16T13:00:00Z", "unknown-update",
	    "2026-10-16T11:00:00Z", "2026-10-16T12:00:00Z", false);
	CheckDuplicateTimestampBoundary("unknown-update", "2026-10-16T13:00:00Z",
	    "2026-10-16T11:00:00Z", "2026-10-16T12:00:00Z", false);
	// A differing unknown creation value cannot turn known-equal update time into a richness bypass.
	CheckDuplicateTimestampBoundary("2026-10-16T13:00:00Z", "2026-10-16T13:00:00Z",
	    "2026-10-16T11:00:00Z", "unknown-created", false);
}
#endif
