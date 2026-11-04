#include "test_harness.h"

using namespace uam_test;

namespace
{
	constexpr const char* kFirstInputId = "117e40c2-20d1-46a4-98bd-6bbf6dfc9b80";
	constexpr const char* kSecondInputId = "d56f02b6-5a86-4ad0-b68d-7be18b936325";

	Message AuthoredRefreshInput(const char* native_input_id, const char* origin_request_id)
	{
		Message row{MessageRole::User, "Repeat exactly this authored request.", "2026-10-21T15:00:00Z"};
		row.provider = "codex-cli";
		row.native_input_id = native_input_id;
		row.origin_request_id = origin_request_id;
		// Requires the separately proposed Message field; this test does not mint
		// runtime delivery authority or exercise a provider/write producer.
		row.input_write_completed = true;
		return row;
	}

	Message ExportedInputWithoutUamFacts()
	{
		Message row{MessageRole::User, "Repeat exactly this authored request.", "2026-10-21T15:00:00Z"};
		row.provider = "codex-cli";
		return row;
	}

	Message NativeAnswer(const std::string& text, const std::string& timestamp)
	{
		Message row{MessageRole::Assistant, text, timestamp};
		row.provider = "codex-cli";
		return row;
	}

	ChatSession SavedAuthoredCandidate()
	{
		ChatSession candidate;
		candidate.id = "provenance-native-refresh-candidate";
		candidate.provider_id = "codex-cli";
		candidate.native_session_id = "recorded-native-history-session";
		candidate.messages_loaded = true;
		candidate.updated_at = "2026-10-21T15:00:03Z";
		candidate.messages = {
			AuthoredRefreshInput(kFirstInputId, "first-authored-send"),
			NativeAnswer("First answer.", "2026-10-21T15:00:01Z"),
			AuthoredRefreshInput(kSecondInputId, "second-authored-send"),
			NativeAnswer("Second answer.", "2026-10-21T15:00:03Z")};
		candidate.persisted_message_count = candidate.messages.size();
		candidate.persisted_messages_digest = "retained-canonical-history-digest";
		return candidate;
	}
}

UAM_TEST(InputProvenanceNativeRefreshCanonicalPrefixRetainsAuthoredFacts)
{
	ChatSession candidate = SavedAuthoredCandidate();
	const auto before = candidate.messages;
	const std::vector<Message> native = {
		ExportedInputWithoutUamFacts(),
		NativeAnswer("First answer.", "2026-10-21T15:00:01Z"),
		ExportedInputWithoutUamFacts(),
		NativeAnswer("Second answer.", "2026-10-21T15:00:03Z"),
		NativeAnswer("New native history is still admitted.", "2026-10-21T15:00:04Z")};
	UAM_ASSERT(native.size() > before.size());
	UAM_ASSERT(!native.front().native_input_id && !native.front().origin_request_id);
	UAM_ASSERT(!native.front().input_write_completed);

	UAM_ASSERT(ChatHistorySyncService().BuildNativeTranscriptCandidate(candidate, native));
	UAM_ASSERT_EQ(candidate.messages.size(), native.size());
	UAM_ASSERT_EQ(candidate.messages.back().content, native.back().content);
	UAM_ASSERT_EQ(candidate.updated_at, std::string("2026-10-21T15:00:04Z"));
	UAM_ASSERT_EQ(candidate.persisted_message_count, native.size());
	for (const std::size_t index : {std::size_t{0}, std::size_t{2}})
	{
		UAM_ASSERT(candidate.messages[index].native_input_id == before[index].native_input_id);
		UAM_ASSERT(candidate.messages[index].origin_request_id == before[index].origin_request_id);
		UAM_ASSERT(candidate.messages[index].input_write_completed);
	}
	UAM_ASSERT(candidate.messages[0].native_input_id != candidate.messages[2].native_input_id);
	UAM_ASSERT(candidate.messages[0].origin_request_id != candidate.messages[2].origin_request_id);
}

UAM_TEST(InputProvenanceNativeRefreshAmbiguousRegroupingRefusesWithoutMutation)
{
	ChatSession candidate = SavedAuthoredCandidate();
	const ChatSession before = candidate;
	// The two saved authored rows deliberately have identical text AND time,
	// but distinct UAM-native input IDs. Neither exported row has that identity.
	UAM_ASSERT_EQ(before.messages[0].content, before.messages[2].content);
	UAM_ASSERT_EQ(before.messages[0].created_at, before.messages[2].created_at);
	UAM_ASSERT(before.messages[0].native_input_id != before.messages[2].native_input_id);
	// Native assistant regrouping moves BOTH authored inputs beyond all saved
	// indexes. No same-position prefix copy or unique text/time match can rescue
	// them; provider_event_ids are absent rather than fabricated for this test.
	const std::vector<Message> native = {
		NativeAnswer("Regrouped response A.", "2026-10-21T15:00:01Z"),
		NativeAnswer("Regrouped response B.", "2026-10-21T15:00:02Z"),
		NativeAnswer("Regrouped response C.", "2026-10-21T15:00:03Z"),
		NativeAnswer("Regrouped response D.", "2026-10-21T15:00:04Z"),
		ExportedInputWithoutUamFacts(), ExportedInputWithoutUamFacts()};
	UAM_ASSERT(native.size() > before.messages.size());
	for (const auto& row : native)
	{
		UAM_ASSERT(!row.native_input_id && !row.origin_request_id);
		UAM_ASSERT(!row.input_write_completed);
		UAM_ASSERT(row.provider_event_ids.empty());
	}

	UAM_ASSERT(!ChatHistorySyncService().BuildNativeTranscriptCandidate(candidate, native));
	// A rejected replacement must leave the original full candidate intact,
	// including the distinct per-input facts and its saved pagination metadata.
	UAM_ASSERT_EQ(candidate.messages, before.messages);
	UAM_ASSERT_EQ(candidate.messages_loaded, before.messages_loaded);
	UAM_ASSERT_EQ(candidate.persisted_message_count, before.persisted_message_count);
	UAM_ASSERT_EQ(candidate.persisted_messages_digest, before.persisted_messages_digest);
	UAM_ASSERT_EQ(candidate.updated_at, before.updated_at);
	UAM_ASSERT_EQ(candidate.id, before.id);
	UAM_ASSERT_EQ(candidate.provider_id, before.provider_id);
	UAM_ASSERT_EQ(candidate.native_session_id, before.native_session_id);
}

UAM_TEST(InputProvenanceNativeRefreshRejectsContradictoryProtectedFacts)
{
	for (int field = 0; field < 3; ++field)
	{
		ChatSession candidate = SavedAuthoredCandidate();
		// The third arm distinguishes a positive incoming write claim from an
		// absent/false native-export projection; false alone is not negative proof.
		if (field == 2) candidate.messages.front().input_write_completed = false;
		const ChatSession before = candidate;
		auto incoming = before.messages;
		if (field == 0) incoming.front().native_input_id = kSecondInputId;
		if (field == 1) incoming.front().origin_request_id = "foreign-origin-on-same-slot";
		if (field == 2) incoming.front().input_write_completed = true;
		incoming.push_back(NativeAnswer("New export suffix.", "2026-10-21T15:00:04Z"));
		UAM_ASSERT(!ChatHistorySyncService().BuildNativeTranscriptCandidate(candidate, incoming));
		UAM_ASSERT_EQ(candidate.messages, before.messages);
		UAM_ASSERT_EQ(candidate.updated_at, before.updated_at);
		UAM_ASSERT_EQ(candidate.persisted_message_count, before.persisted_message_count);
		UAM_ASSERT_EQ(candidate.persisted_messages_digest, before.persisted_messages_digest);
	}
}

UAM_TEST(InputProvenancePreparedOverlayRetainsCanonicalHistoryOnAmbiguousRegrouping)
{
	const ChatSession local = SavedAuthoredCandidate();
	ChatSession incoming = local;
	incoming.messages = {
		NativeAnswer("Regrouped A.", "2026-10-21T15:00:01Z"),
		NativeAnswer("Regrouped B.", "2026-10-21T15:00:02Z"),
		NativeAnswer("Regrouped C.", "2026-10-21T15:00:03Z"),
		NativeAnswer("Regrouped D.", "2026-10-21T15:00:04Z"),
		ExportedInputWithoutUamFacts(), ExportedInputWithoutUamFacts()};
	incoming.updated_at = "2026-10-21T15:00:05Z";
	incoming.persisted_message_count = incoming.messages.size();
	incoming.persisted_messages_digest = "external-regrouped-history-digest";
	UAM_ASSERT(incoming.messages.size() > local.messages.size());
	ChatHistorySyncService().OverlayPreparedLocalHistory(local, incoming);
	UAM_ASSERT_EQ(incoming.messages, local.messages);
	UAM_ASSERT_EQ(incoming.messages_loaded, local.messages_loaded);
	UAM_ASSERT_EQ(incoming.persisted_message_count, local.persisted_message_count);
	UAM_ASSERT_EQ(incoming.persisted_messages_digest, local.persisted_messages_digest);
	UAM_ASSERT_EQ(incoming.updated_at, local.updated_at);
	UAM_ASSERT_EQ(incoming.native_session_id, local.native_session_id);
}

UAM_TEST(InputProvenanceNativeRefreshKeepsLegacyRegroupingAvailable)
{
	ChatSession candidate = SavedAuthoredCandidate();
	for (auto& message : candidate.messages)
	{
		message.native_input_id.reset();
		message.origin_request_id.reset();
		message.input_write_completed = false;
	}
	const std::vector<Message> incoming = {
		NativeAnswer("Legacy regrouped A.", "2026-10-21T15:00:01Z"),
		NativeAnswer("Legacy regrouped B.", "2026-10-21T15:00:02Z"),
		NativeAnswer("Legacy regrouped C.", "2026-10-21T15:00:03Z"),
		NativeAnswer("Legacy regrouped D.", "2026-10-21T15:00:04Z"),
		ExportedInputWithoutUamFacts(), ExportedInputWithoutUamFacts()};
	UAM_ASSERT(incoming.size() > candidate.messages.size());
	UAM_ASSERT(ChatHistorySyncService().BuildNativeTranscriptCandidate(candidate, incoming));
	UAM_ASSERT_EQ(candidate.messages, incoming);
	UAM_ASSERT_EQ(candidate.persisted_message_count, incoming.size());
}

UAM_TEST(InputProvenanceNativeRefreshRejectsAppendedUnownedWriteFacts)
{
	for (int claim = 0; claim < 3; ++claim)
	{
		ChatSession candidate = SavedAuthoredCandidate();
		const ChatSession before = candidate;
		auto incoming = before.messages;
		Message external{MessageRole::User, "New native history row.", "2026-10-21T15:00:04Z"};
		if (claim == 0)
		{
			external.native_input_id = "d6a66392-489f-40ca-b835-52dc18162e68";
			external.input_write_completed = true;
		}
		if (claim == 1) external.origin_request_id = "external-origin-without-canonical-owner";
		if (claim == 2) external.native_input_id = "d6a66392-489f-40ca-b835-52dc18162e68";
		incoming.push_back(external);
		UAM_ASSERT(!ChatHistorySyncService().BuildNativeTranscriptCandidate(candidate, incoming));
		UAM_ASSERT_EQ(candidate.messages, before.messages);
		UAM_ASSERT_EQ(candidate.messages_loaded, before.messages_loaded);
		UAM_ASSERT_EQ(candidate.persisted_message_count, before.persisted_message_count);
		UAM_ASSERT_EQ(candidate.persisted_messages_digest, before.persisted_messages_digest);
		UAM_ASSERT_EQ(candidate.updated_at, before.updated_at);
	}
}

UAM_TEST(InputProvenanceNativeRefreshRejectsAppendedDuplicateInputIdentity)
{
	ChatSession candidate = SavedAuthoredCandidate();
	const ChatSession before = candidate;
	auto incoming = before.messages;
	// Copying a genuine saved ID to a new slot is still not authored-input
	// ownership. No cross-row ID lookup may turn this duplicate into a receipt.
	Message duplicate{MessageRole::User, "Another native row.", "2026-10-21T15:00:04Z"};
	duplicate.native_input_id = before.messages.front().native_input_id;
	duplicate.origin_request_id = before.messages.front().origin_request_id;
	duplicate.input_write_completed = true;
	incoming.push_back(duplicate);
	UAM_ASSERT(!ChatHistorySyncService().BuildNativeTranscriptCandidate(candidate, incoming));
	UAM_ASSERT_EQ(candidate.messages, before.messages);
	UAM_ASSERT_EQ(candidate.messages_loaded, before.messages_loaded);
	UAM_ASSERT_EQ(candidate.persisted_message_count, before.persisted_message_count);
	UAM_ASSERT_EQ(candidate.persisted_messages_digest, before.persisted_messages_digest);
	UAM_ASSERT_EQ(candidate.updated_at, before.updated_at);
}

UAM_TEST(InputProvenanceNativeRefreshRejectsFactsInjectedIntoLegacySlot)
{
	for (const bool origin_only : {false, true})
	{
		ChatSession candidate = SavedAuthoredCandidate();
		for (auto& row : candidate.messages)
		{
			row.native_input_id.reset();
			row.origin_request_id.reset();
			row.input_write_completed = false;
		}
		const ChatSession before = candidate;
		auto incoming = before.messages;
		if (origin_only) incoming.front().origin_request_id = "not-a-saved-legacy-origin";
		else
		{
			incoming.front().native_input_id = kFirstInputId;
			incoming.front().input_write_completed = true;
		}
		UAM_ASSERT(!ChatHistorySyncService().BuildNativeTranscriptCandidate(candidate, incoming));
		UAM_ASSERT_EQ(candidate.messages, before.messages);
		UAM_ASSERT_EQ(candidate.messages_loaded, before.messages_loaded);
		UAM_ASSERT_EQ(candidate.persisted_message_count, before.persisted_message_count);
		UAM_ASSERT_EQ(candidate.persisted_messages_digest, before.persisted_messages_digest);
		UAM_ASSERT_EQ(candidate.updated_at, before.updated_at);
	}
}
