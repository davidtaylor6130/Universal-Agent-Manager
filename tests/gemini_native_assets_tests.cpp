#include "test_harness.h"
#include "common/provider/gemini_history_loader.h"

#if UAM_ENABLE_RUNTIME_GEMINI_CLI

namespace
{
	using Json = nlohmann::json;

	const std::string kAssetSession = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
	const std::string kAssetStart = "2026-10-16T12:00:00Z";
	const std::string kAssetUpdated = "2026-10-16T12:00:01Z";
	// Original 1x1 RGBA PNG: 70 bytes, with CRC-bearing chunks and embedded NULs.
	const std::string kOriginalPng = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
	// Original RIFF/WAVE: 46 bytes, PCM mono 8-bit/8000Hz, two silence samples.
	const std::string kOriginalWave = "UklGRiYAAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQIAAACAgA==";

	Json AssetMetadata()
	{
		return {{"sessionId", kAssetSession}, {"projectHash", "native-assets-project"}, {"startTime", kAssetStart}, {"lastUpdated", kAssetUpdated}};
	}

	Json AssetRecord(const std::string& id, const std::string& type, Json content)
	{
		return {{"id", id}, {"type", type}, {"timestamp", kAssetUpdated}, {"content", std::move(content)}};
	}

	Json InlineAsset(const std::string& mime, const std::string& original)
	{
		return {{"inlineData", {{"mimeType", mime}, {"data", original}}}};
	}

	Json AssetToolResult(const std::string& output, const std::string& mime, const std::string& original)
	{
		// Published tool result PartListUnion: media is a sibling of functionResponse.
		return Json::array({{{"functionResponse", {{"id", "asset-tool"}, {"name", "read_file"}, {"response", {{"output", output}}}}}}, InlineAsset(mime, original)});
	}

	Json AssetTool(const Json& result)
	{
		return {{"id", "asset-tool"}, {"name", "read_file"}, {"args", {{"path", "argument-only-source.png"}}}, {"status", "success"}, {"timestamp", kAssetUpdated}, {"result", result}};
	}

	void AssertAssetIdentity(const ChatSession& chat, const ProviderProfile& provider)
	{
		UAM_ASSERT_EQ(chat.id, kAssetSession);
		UAM_ASSERT_EQ(chat.native_session_id, kAssetSession);
		UAM_ASSERT_EQ(chat.provider_id, provider.id);
		UAM_ASSERT_EQ(chat.created_at, kAssetStart);
		UAM_ASSERT_EQ(chat.updated_at, kAssetUpdated);
	}

	void AssertPathlessAsset(const MessageAttachment& attachment, const std::string& mime)
	{
		UAM_ASSERT(!attachment.id.empty());
		UAM_ASSERT_EQ(attachment.mime_type, mime);
		UAM_ASSERT(attachment.path.empty());
		UAM_ASSERT(attachment.preview_origin_path.empty());
		UAM_ASSERT(attachment.preview_key.empty());
		UAM_ASSERT(!attachment.copied);
		// A generic display name is allowed; no source filename was recorded in the part.
		UAM_ASSERT(attachment.name.find("argument-only-source") == std::string::npos);
		UAM_ASSERT(attachment.name.find("native-assets-journal") == std::string::npos);
		UAM_ASSERT(attachment.name.find("uri-is-not-a-source-name") == std::string::npos);
	}

	void AssertNoAssetText(const ChatSession& chat)
	{
		const auto assert_text = [](const std::string& text)
		{
			UAM_ASSERT(text.find(kOriginalPng) == std::string::npos);
			UAM_ASSERT(text.find(kOriginalWave) == std::string::npos);
		};
		for (const auto& message : chat.messages)
		{
			assert_text(message.content);
			assert_text(message.thoughts);
			assert_text(message.plan_summary);
			for (const auto& block : message.blocks) assert_text(block.text);
			for (const auto& tool : message.tool_calls)
			{
				assert_text(tool.args_json);
				assert_text(tool.result_text);
			}
		}
	}
}

UAM_TEST(GeminiNativeAssetsRetainInlineOnlyUserDocument)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	auto document = AssetMetadata();
	document["messages"] = Json::array({AssetRecord("inline-user", "user", Json::array({InlineAsset("image/png", kOriginalPng)}))});
	const auto chat = GeminiJsonHistoryStore::ParseDocument(std::move(document), provider);

	// Attachment-only conversation content must not become a rejected empty session.
	UAM_ASSERT(chat.has_value());
	AssertAssetIdentity(*chat, provider);
	UAM_ASSERT_EQ(chat->messages.size(), std::size_t(1));
	const auto& message = chat->messages.front();
	UAM_ASSERT_EQ(message.role, MessageRole::User);
	UAM_ASSERT_EQ(message.created_at, kAssetUpdated);
	UAM_ASSERT(message.content.empty());
	UAM_ASSERT_EQ(message.attachments.size(), std::size_t(1));
	AssertPathlessAsset(message.attachments.front(), "image/png");
	AssertNoAssetText(*chat);
}

UAM_TEST(GeminiNativeAssetsRetainUriOnlyUserWithoutPathAuthority)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	// These are opaque provider references, never inputs to a file/network reader.
	for (const std::string uri : {"gs://native-assets/uri-is-not-a-source-name.pdf", "https://assets.invalid/uri-is-not-a-source-name.pdf", "file:///not-authorized/uri-is-not-a-source-name.pdf"})
	{
		auto document = AssetMetadata();
		document["messages"] = Json::array({AssetRecord("uri-user", "user", Json::array({{{"fileData", {{"fileUri", uri}, {"mimeType", "application/pdf"}}}}}))});
		const auto chat = GeminiJsonHistoryStore::ParseDocument(std::move(document), provider);

		UAM_ASSERT(chat.has_value());
		AssertAssetIdentity(*chat, provider);
		UAM_ASSERT_EQ(chat->messages.size(), std::size_t(1));
		const auto& message = chat->messages.front();
		UAM_ASSERT_EQ(message.role, MessageRole::User);
		UAM_ASSERT(message.content.empty());
		UAM_ASSERT_EQ(message.attachments.size(), std::size_t(1));
		const auto& attachment = message.attachments.front();
		AssertPathlessAsset(attachment, "application/pdf");
		UAM_ASSERT_EQ(attachment.size_bytes, std::uintmax_t(0));
		AssertNoAssetText(*chat);
	}
}

UAM_TEST(GeminiNativeAssetsRetainToolResultInlineSiblingAndText)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	auto document = AssetMetadata();
	auto answer = AssetRecord("tool-answer", "gemini", "");
	answer["model"] = "recorded-native-model";
	answer["toolCalls"] = Json::array({AssetTool(AssetToolResult("Image read successfully", "image/png", kOriginalPng))});
	document["messages"] = Json::array({AssetRecord("text-control", "user", Json::array({{{"text", "Inspect the native result"}}})), std::move(answer)});
	const auto chat = GeminiJsonHistoryStore::ParseDocument(std::move(document), provider);

	UAM_ASSERT(chat.has_value());
	AssertAssetIdentity(*chat, provider);
	UAM_ASSERT_EQ(chat->messages.size(), std::size_t(2));
	UAM_ASSERT_EQ(chat->messages[0].role, MessageRole::User);
	UAM_ASSERT_EQ(chat->messages[0].content, std::string("Inspect the native result"));
	UAM_ASSERT(chat->messages[0].attachments.empty());
	const auto& message = chat->messages[1];
	UAM_ASSERT_EQ(message.role, MessageRole::Assistant);
	UAM_ASSERT(message.content.empty());
	UAM_ASSERT_EQ(message.model_id, std::string("recorded-native-model"));
	UAM_ASSERT_EQ(message.tool_calls.size(), std::size_t(1));
	const auto& tool = message.tool_calls.front();
	UAM_ASSERT_EQ(tool.id, std::string("asset-tool"));
	UAM_ASSERT_EQ(tool.name, std::string("read_file"));
	UAM_ASSERT_EQ(tool.status, std::string("success"));
	UAM_ASSERT_EQ(tool.result_text, std::string("Image read successfully"));
	AssertNoAssetText(*chat);
	UAM_ASSERT_EQ(message.attachments.size(), std::size_t(1));
	AssertPathlessAsset(message.attachments.front(), "image/png");
}

UAM_TEST(GeminiNativeAssetsFoldReplacedAndRemovedPartsBeforeMapping)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	const auto line = [](const Json& value) { return value.dump() + "\n"; };
	auto answer = AssetRecord("tool-answer", "gemini", "");
	answer["toolCalls"] = Json::array({AssetTool(AssetToolResult("Obsolete image result", "image/png", kOriginalPng))});
	const std::string journal = line(AssetMetadata()) +
	    line(AssetRecord("text-control", "user", "Normal text survives")) +
	    line(AssetRecord("replaced", "user", Json::array({{{"text", "Obsolete draft"}}, InlineAsset("image/png", kOriginalPng)}))) +
	    line(answer) +
	    line(AssetRecord("removed", "user", Json::array({InlineAsset("image/png", kOriginalPng)}))) +
	    line({{"$patch", {{"updates", Json::array({
	        {{"id", "replaced"}, {"content", Json::array({{{"text", "Final authored text"}}})}},
	        {{"id", "tool-answer"}, {"toolCalls", Json::array({{{"id", "asset-tool"}, {"result", AssetToolResult("Final audio result", "audio/wav", kOriginalWave)}}})}}
	    })}, {"removeIds", Json::array({"removed"})}}}});

	uam_test::TempDir temp("uam-gemini-native-assets");
	const auto file = temp.root / "native-assets-journal.jsonl";
	UAM_ASSERT(uam::io::WriteTextFile(file, journal));
	const auto chat = GeminiJsonHistoryStore::ParseFile(file, provider);
	UAM_ASSERT(chat.has_value());
	AssertAssetIdentity(*chat, provider);
	UAM_ASSERT_EQ(chat->messages.size(), std::size_t(3));
	UAM_ASSERT_EQ(chat->messages[0].role, MessageRole::User);
	UAM_ASSERT_EQ(chat->messages[0].content, std::string("Normal text survives"));
	UAM_ASSERT(chat->messages[0].attachments.empty());
	UAM_ASSERT_EQ(chat->messages[1].role, MessageRole::User);
	UAM_ASSERT_EQ(chat->messages[1].content, std::string("Final authored text"));
	UAM_ASSERT(chat->messages[1].attachments.empty());
	const auto& message = chat->messages[2];
	UAM_ASSERT_EQ(message.role, MessageRole::Assistant);
	UAM_ASSERT(message.content.empty());
	UAM_ASSERT_EQ(message.tool_calls.size(), std::size_t(1));
	UAM_ASSERT_EQ(message.tool_calls.front().id, std::string("asset-tool"));
	UAM_ASSERT_EQ(message.tool_calls.front().result_text, std::string("Final audio result"));
	AssertNoAssetText(*chat);
	// Only the final tool-result WAV remains; replaced/removed PNGs grant no assets.
	UAM_ASSERT_EQ(message.attachments.size(), std::size_t(1));
	AssertPathlessAsset(message.attachments.front(), "audio/wav");
	UAM_ASSERT_EQ(uam_test::ReadFile(file), journal);
}

UAM_TEST(GeminiNativeAssetsRejectMalformedInlineBesideText)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	for (const std::string encoded : {"AB==", "AA", "data:image/png;base64,AA=="})
	{
		auto document = AssetMetadata();
		document["messages"] = Json::array({AssetRecord("invalid", "user", Json::array({{{"text", "Keep retryable"}}, InlineAsset("image/png", encoded)}))});
		UAM_ASSERT(!GeminiJsonHistoryStore::ParseDocument(std::move(document), provider));
	}
}

UAM_TEST(GeminiNativeAssetsRejectUnknownBinaryShapeBesideText)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	auto document = AssetMetadata();
	document["messages"] = Json::array({AssetRecord("unknown", "user", Json::array({{{"text", "Keep retryable"}}, {{"futureBinary", {{"mimeType", "image/png"}, {"data", kOriginalPng}}}}}))});
	UAM_ASSERT(!GeminiJsonHistoryStore::ParseDocument(std::move(document), provider));
}

UAM_TEST(GeminiNativeAssetsMapNestedToolOutputWithoutBinaryText)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	auto document = AssetMetadata();
	auto answer = AssetRecord("nested-tool", "gemini", "");
	answer["toolCalls"] = Json::array({AssetTool(Json::array({{{"functionResponse", {{"response", {{"output", Json::array({{{"text", "Nested output"}}, InlineAsset("application/pdf", kOriginalPng)})}}}}}}}))});
	document["messages"] = Json::array({std::move(answer)});
	const auto chat = GeminiJsonHistoryStore::ParseDocument(std::move(document), provider);
	UAM_ASSERT(chat);
	AssertNoAssetText(*chat);
	UAM_ASSERT_EQ(chat->messages.front().tool_calls.front().result_text, std::string("Nested output"));
	UAM_ASSERT_EQ(chat->messages.front().attachments.size(), std::size_t(1));
	AssertPathlessAsset(chat->messages.front().attachments.front(), "application/pdf");
}

UAM_TEST(GeminiNativeAssetsCoalesceDisplayDuplicateAndRetainDisplayOnly)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	auto document = AssetMetadata();
	auto message = AssetRecord("display", "user", Json::array({InlineAsset("image/png", kOriginalPng)}));
	message["displayContent"] = Json::array({InlineAsset("image/png", kOriginalPng), InlineAsset("audio/wav", kOriginalWave)});
	document["messages"] = Json::array({std::move(message)});
	const auto chat = GeminiJsonHistoryStore::ParseDocument(std::move(document), provider);
	UAM_ASSERT(chat);
	UAM_ASSERT_EQ(chat->messages.front().attachments.size(), std::size_t(2));
	AssertPathlessAsset(chat->messages.front().attachments[0], "image/png");
	AssertPathlessAsset(chat->messages.front().attachments[1], "audio/wav");
	AssertNoAssetText(*chat);
}

UAM_TEST(GeminiNativeAssetsBoundUriCountAndExactUriLength)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	for (const bool count_overflow : {false, true})
	{
		auto document = AssetMetadata();
		auto parts = Json::array({{{"text", "Keep retryable"}}});
		const std::size_t count = count_overflow ? 257 : 1;
		for (std::size_t index = 0; index < count; ++index)
			parts.push_back({{"fileData", {{"mimeType", "application/pdf"}, {"fileUri", count_overflow ? "opaque:" + std::to_string(index) : std::string(16 * 1024 + 1, 'x')}}}});
		document["messages"] = Json::array({AssetRecord("bounded-uri", "user", std::move(parts))});
		UAM_ASSERT(!GeminiJsonHistoryStore::ParseDocument(std::move(document), provider));
	}
}

namespace
{
	Json ResponseOriginal(const Json& id)
	{
		Json response = {{"name", "read_file"}, {"response", {{"output", Json::array({InlineAsset("image/png", kOriginalPng)})}}}};
		if (!id.is_null()) response["id"] = id;
		return {{"functionResponse", std::move(response)}};
	}

	void AssertUnsupportedMedia(const GeminiHistoryParseResult& parsed)
	{
		UAM_ASSERT_EQ(parsed.status, GeminiHistoryParseStatus::UnsupportedOrInvalid);
		UAM_ASSERT(!parsed.chat);
		UAM_ASSERT(!parsed.error.empty());
	}
}

UAM_TEST(GeminiNativeAssetsRejectMixedMediaAlternatives)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	auto document = AssetMetadata();
	auto mixed = InlineAsset("image/png", kOriginalPng);
	mixed["fileData"] = {{"mimeType", "application/pdf"}, {"fileUri", "opaque:second"}};
	document["messages"] = Json::array({AssetRecord("mixed", "user", Json::array({std::move(mixed)}))});
	AssertUnsupportedMedia(GeminiJsonHistoryStore::ParseDocumentResult(std::move(document), provider));
}

UAM_TEST(GeminiNativeAssetsRejectUnvisitedResponseMediaWithText)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	auto document = AssetMetadata();
	Json response = {{"id", "asset-tool"}, {"name", "read_file"}, {"response", {{"output", "ok"}}}, {"parts", Json::array({InlineAsset("image/png", kOriginalPng)})}};
	document["messages"] = Json::array({AssetRecord("unknown-response-text", "user", Json::array({{{"text", "Keep retryable"}}, {{"functionResponse", std::move(response)}}}))});
	AssertUnsupportedMedia(GeminiJsonHistoryStore::ParseDocumentResult(std::move(document), provider));
}

UAM_TEST(GeminiNativeAssetsRejectUnvisitedResponseMediaAsEmpty)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	auto document = AssetMetadata();
	Json response = {{"id", "asset-tool"}, {"name", "read_file"}, {"parts", Json::array({InlineAsset("image/png", kOriginalPng)})}};
	document["messages"] = Json::array({AssetRecord("unknown-response-empty", "user", Json::array({{{"functionResponse", std::move(response)}}}))});
	AssertUnsupportedMedia(GeminiJsonHistoryStore::ParseDocumentResult(std::move(document), provider));
}

UAM_TEST(GeminiNativeAssetsRetainRecordedResponseIdentityAndKey)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	std::string previous_key;
	for (const std::string id : {"tool-a", "tool-b"})
	{
		auto document = AssetMetadata();
		document["messages"] = Json::array({AssetRecord("recorded-response", "user", Json::array({ResponseOriginal(id)}))});
		const auto parsed = GeminiJsonHistoryStore::ParseDocumentResult(std::move(document), provider);
		UAM_ASSERT_EQ(parsed.status, GeminiHistoryParseStatus::Content);
		UAM_ASSERT(parsed.chat);
		UAM_ASSERT_EQ(parsed.chat->messages.size(), std::size_t(1));
		UAM_ASSERT_EQ(parsed.chat->messages.front().attachments.size(), std::size_t(1));
		const auto& attachment = parsed.chat->messages.front().attachments.front();
		UAM_ASSERT_EQ(attachment.native_tool_call_id, id);
		UAM_ASSERT(!attachment.native_part_key.empty());
		UAM_ASSERT(attachment.native_part_key != previous_key);
		UAM_ASSERT(attachment.native_source_field.find("functionResponse") != std::string::npos);
		AssertPathlessAsset(attachment, "image/png");
		AssertNoAssetText(*parsed.chat);
		previous_key = attachment.native_part_key;
	}
}

UAM_TEST(GeminiNativeAssetsRejectConflictingInheritedResponseIdentity)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	auto document = AssetMetadata();
	auto answer = AssetRecord("conflicting-response", "gemini", "");
	answer["toolCalls"] = Json::array({AssetTool(Json::array({ResponseOriginal("foreign-tool")}))});
	document["messages"] = Json::array({std::move(answer)});
	AssertUnsupportedMedia(GeminiJsonHistoryStore::ParseDocumentResult(std::move(document), provider));
}

UAM_TEST(GeminiNativeAssetsKeepDifferentDisplayResponseIdentities)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	auto document = AssetMetadata();
	auto message = AssetRecord("display-response", "user", Json::array({ResponseOriginal("tool-a")}));
	message["displayContent"] = Json::array({ResponseOriginal("tool-b")});
	document["messages"] = Json::array({std::move(message)});
	const auto parsed = GeminiJsonHistoryStore::ParseDocumentResult(std::move(document), provider);
	UAM_ASSERT_EQ(parsed.status, GeminiHistoryParseStatus::Content);
	UAM_ASSERT(parsed.chat);
	const auto& attachments = parsed.chat->messages.front().attachments;
	UAM_ASSERT_EQ(attachments.size(), std::size_t(2));
	UAM_ASSERT_EQ(attachments[0].native_tool_call_id, std::string("tool-a"));
	UAM_ASSERT_EQ(attachments[1].native_tool_call_id, std::string("tool-b"));
	UAM_ASSERT(attachments[0].native_part_key != attachments[1].native_part_key);
	AssertNoAssetText(*parsed.chat);
}

UAM_TEST(GeminiNativeAssetsBoundRecordedResponseIdentity)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	for (const auto& id : Json::array({std::string(1025, 'a'), 7, false, ""}))
	{
		auto document = AssetMetadata();
		document["messages"] = Json::array({AssetRecord("bounded-response-id", "user", Json::array({ResponseOriginal(id)}))});
		AssertUnsupportedMedia(GeminiJsonHistoryStore::ParseDocumentResult(std::move(document), provider));
	}
}

UAM_TEST(GeminiNativeAssetsScopeAmbiguousResponseIdentityToSnapshot)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	for (const bool duplicate : {false, true})
	{
		std::vector<std::string> previous_keys;
		for (int snapshot = 0; snapshot < 2; ++snapshot)
		{
			auto document = AssetMetadata();
			auto parts = Json::array({ResponseOriginal(duplicate ? Json("duplicate-tool") : Json(nullptr))});
			if (duplicate) parts.push_back(ResponseOriginal("duplicate-tool"));
			document["messages"] = Json::array({AssetRecord("ambiguous-response", "user", std::move(parts))});
			const auto parsed = GeminiJsonHistoryStore::ParseDocumentResult(std::move(document), provider);
			UAM_ASSERT_EQ(parsed.status, GeminiHistoryParseStatus::Content);
			UAM_ASSERT(parsed.chat);
			const auto& attachments = parsed.chat->messages.front().attachments;
			UAM_ASSERT_EQ(attachments.size(), duplicate ? std::size_t(2) : std::size_t(1));
			if (duplicate) UAM_ASSERT(attachments[0].native_part_key != attachments[1].native_part_key);
			std::vector<std::string> keys;
			for (const auto& attachment : attachments)
			{
				UAM_ASSERT_EQ(attachment.native_tool_call_id, duplicate ? std::string("duplicate-tool") : std::string());
				UAM_ASSERT(std::find(previous_keys.begin(), previous_keys.end(), attachment.native_part_key) == previous_keys.end());
				keys.push_back(attachment.native_part_key);
			}
			previous_keys = std::move(keys);
		}
	}
}

UAM_TEST(GeminiNativeAssetsRejectBinaryInIgnoredPartMetadata)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	for (const std::string field : {"thought", "thoughtSignature", "videoMetadata"})
	{
		for (const bool with_text : {false, true})
		{
			auto document = AssetMetadata();
			auto parts = Json::array();
			if (with_text) parts.push_back({{"text", "Keep retryable"}});
			parts.push_back({{field, {{"futureBinary", InlineAsset("image/png", kOriginalPng)}}}});
			document["messages"] = Json::array({AssetRecord("binary-metadata", "user", std::move(parts))});
			AssertUnsupportedMedia(GeminiJsonHistoryStore::ParseDocumentResult(std::move(document), provider));
		}
	}
}

UAM_TEST(GeminiNativeAssetsRejectCapHidingEligibleConversation)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	GeminiJsonHistoryStoreOptions capped;
	capped.max_messages = 1;
	for (const bool inline_only : {false, true})
	{
		auto document = AssetMetadata();
		document["messages"] = Json::array({AssetRecord("setup-prefix", "info", "Setup status"),
		    AssetRecord("eligible-later", "user", inline_only ? Json::array({InlineAsset("image/png", kOriginalPng)}) : Json("Authored conversation"))});
		const auto full = GeminiJsonHistoryStore::ParseDocumentResult(document, provider);
		UAM_ASSERT_EQ(full.status, GeminiHistoryParseStatus::Content);
		UAM_ASSERT(full.chat);
		UAM_ASSERT_EQ(full.chat->messages.size(), std::size_t(2));
		const auto legacy = GeminiJsonHistoryStore::ParseDocument(document, provider, capped);
		UAM_ASSERT(legacy);
		UAM_ASSERT_EQ(legacy->messages.size(), std::size_t(1));
		UAM_ASSERT_EQ(legacy->messages.front().role, MessageRole::System);
		UAM_ASSERT_EQ(legacy->messages.front().content, std::string("Setup status"));
		const auto projected = GeminiJsonHistoryStore::ParseDocumentResult(std::move(document), provider, capped);
		AssertUnsupportedMedia(projected);
		UAM_ASSERT(projected.error.find("projection") != std::string::npos);
	}
}

UAM_TEST(GeminiNativeAssetsKeepSetupOnlyEmptyWithOrdinaryPartMetadata)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	GeminiJsonHistoryStoreOptions capped;
	capped.max_messages = 1;
	auto document = AssetMetadata();
	Json metadata_text = {{"text", "Setup status"}, {"thought", false}, {"thoughtSignature", "recorded-signature"},
	    {"videoMetadata", {{"startOffset", "0s"}, {"endOffset", "1s"}, {"fps", 30}}}};
	document["messages"] = Json::array({AssetRecord("setup-only", "info", Json::array({std::move(metadata_text)})), AssetRecord("setup-later", "info", "Still setup")});
	const auto rich = GeminiJsonHistoryStore::ParseDocumentResult(document, provider, capped);
	UAM_ASSERT_EQ(rich.status, GeminiHistoryParseStatus::ConfirmedEmpty);
	UAM_ASSERT(!rich.chat);
	UAM_ASSERT(rich.error.empty());
	const auto legacy = GeminiJsonHistoryStore::ParseDocument(std::move(document), provider, capped);
	UAM_ASSERT(legacy);
	UAM_ASSERT_EQ(legacy->messages.size(), std::size_t(1));
	UAM_ASSERT_EQ(legacy->messages.front().role, MessageRole::System);
	UAM_ASSERT_EQ(legacy->messages.front().content, std::string("Setup status"));
}

UAM_TEST(GeminiNativeAssetsValidateLaterInvalidMediaBeforeCapDisposition)
{
	const auto provider = ProviderProfileStore::DefaultGeminiProfile();
	GeminiJsonHistoryStoreOptions capped;
	capped.max_messages = 1;
	auto document = AssetMetadata();
	document["messages"] = Json::array({AssetRecord("setup-prefix", "info", "Setup status"),
	    AssetRecord("eligible-later", "user", "Authored conversation"),
	    AssetRecord("invalid-after-cap", "user", Json::array({InlineAsset("image/png", "AB==")}))});
	const auto parsed = GeminiJsonHistoryStore::ParseDocumentResult(document, provider, capped);
	AssertUnsupportedMedia(parsed);
	UAM_ASSERT(parsed.error.find("base64") != std::string::npos);
	UAM_ASSERT(parsed.error.find("projection") == std::string::npos);
	UAM_ASSERT(!GeminiJsonHistoryStore::ParseDocument(std::move(document), provider, capped));
}

#endif
