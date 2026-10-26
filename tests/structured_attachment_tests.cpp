#include "test_harness.h"
#include "common/provider/structured_attachments.h"
#include "common/chat/native_attachment_assets.h"

namespace
{
	std::vector<MessageAttachment> UnsupportedRecordedInputs()
	{
		std::vector<MessageAttachment> result;
		for (const auto source : {AttachmentSource::ProviderUri, AttachmentSource::OwnedNativeAsset,
		         AttachmentSource::NativeInlinePending, static_cast<AttachmentSource>(999)})
		{
			MessageAttachment value; value.id = "recorded"; value.name = "Recorded file";
			value.kind = "file"; value.mime_type = "application/pdf"; value.source = source;
			result.push_back(value);
			value.path = "sentinel.pdf"; result.push_back(value);
		}
		for (int field = 0; field != 8; ++field)
		{
			MessageAttachment value; value.id = "mixed"; value.name = "Mixed file";
			value.kind = "file"; value.path = "sentinel.pdf";
			if (field == 0) value.source_uri = "provider://opaque";
			if (field == 1) value.origin_execution_host_id = "recorded-host";
			if (field == 2) value.native_part_key = "recorded-part";
			if (field == 3) value.native_tool_call_id = "recorded-tool";
			if (field == 4) value.native_source_field = "content/0";
			if (field == 5) value.native_asset_key = "recorded-key";
			if (field == 6) value.content_digest = "recorded-digest";
			if (field == 7)
			{
				std::string error;
				value.native_inline_payload = uam::native_assets::MakeInlinePayload(std::string("eA=="),
				    "application/octet-stream", std::make_shared<uam::native_assets::PayloadBudget>(), error);
				UAM_ASSERT(value.native_inline_payload && error.empty());
			}
			result.push_back(std::move(value));
		}
		return result;
	}
}

UAM_TEST(StructuredNativeInputRejectsRecordedSourcesBeforeProtocolSkips)
{
	for (const std::string protocol : {"claude-code-stream-json", "codex-app-server", "gemini-acp",
	         "opencode-acp", "copilot-acp", "copilot-sdk", "antigravity-stream-json", "unknown-native"})
	for (const bool remote : {false, true})
	for (const auto& attachment : UnsupportedRecordedInputs())
	{
		uam::AcpSessionState session; session.protocol_kind = protocol; session.prompt_image_supported = true;
		nlohmann::json request{{"params", {{"prompt", nlohmann::json::array()}, {"input", nlohmann::json::array()}}},
		    {"message", {{"content", nlohmann::json::array()}}}};
		const auto original = request; std::string error;
		UAM_ASSERT(!uam::structured_attachments::AppendNative(request, session, {attachment},
		    "/workspace", "linux", remote, error));
		UAM_ASSERT(!error.empty()); UAM_ASSERT_EQ(request, original);
	}
}

UAM_TEST(StructuredImageInputRejectsRecordedAndMixedSourcesBeforeAnyRead)
{
	const MessageAttachment legacy{.id = "legacy", .name = "clean.png", .kind = "image",
	    .mime_type = "image/png", .path = "clean.png"};
	for (auto image : UnsupportedRecordedInputs())
	{
		image.kind = "image"; image.mime_type = "image/png";
		int reads = 0; std::string error;
		const auto prepared = uam::claude_input::BuildWithReader("Inspect", {legacy, image}, error, false, {},
		    [&reads](const MessageAttachment&, std::string& bytes, std::size_t, std::string&)
		    { ++reads; bytes = "image fixture"; return true; });
		UAM_ASSERT(prepared.is_null()); UAM_ASSERT(!error.empty()); UAM_ASSERT_EQ(reads, 0);
	}
	int reads = 0; std::string error;
	const auto prepared = uam::claude_input::BuildWithReader("Inspect", {legacy}, error, false, {},
	    [&reads](const MessageAttachment&, std::string& bytes, std::size_t, std::string&)
	    { ++reads; bytes = "image fixture"; return true; });
	UAM_ASSERT(!prepared.is_null()); UAM_ASSERT(error.empty()); UAM_ASSERT_EQ(reads, 1);
}

UAM_TEST(StructuredInputRejectsPathlessLegacyBeforeProtocolSkipsOrReads)
{
	for (const std::string path : {std::string(), std::string(" \t\r\n "), std::string("bad\0path", 8)})
	{
		MessageAttachment attachment{.id = "malformed-legacy", .name = "Unavailable file", .kind = "file", .path = path};
		for (const std::string protocol : {"claude-code-stream-json", "codex-app-server", "gemini-acp",
		         "opencode-acp", "copilot-acp", "copilot-sdk", "antigravity-stream-json", "unknown-native"})
		for (const bool remote : {false, true})
		{
			uam::AcpSessionState session;
			session.protocol_kind = protocol;
			nlohmann::json request{{"params", {{"prompt", nlohmann::json::array({{{"type", "text"}, {"text", "Inspect"}}})}, {"input", nlohmann::json::array()}}}};
			const auto before = request;
			std::string error;
			UAM_ASSERT(!uam::structured_attachments::AppendNative(request, session, {attachment}, "/workspace", "linux", remote, error));
			UAM_ASSERT(error.find("invalid path") != std::string::npos);
			UAM_ASSERT_EQ(request, before);
		}
		attachment.kind = "image";
		attachment.mime_type = "image/png";
		int reads = 0;
		std::string error;
		const auto prepared = uam::claude_input::BuildWithReader("Inspect", {attachment}, error, false, {},
		    [&reads](const MessageAttachment&, std::string&, std::size_t, std::string&) { ++reads; return true; });
		UAM_ASSERT(prepared.is_null());
		UAM_ASSERT(error.find("invalid path") != std::string::npos);
		UAM_ASSERT_EQ(reads, 0);
	}
}

UAM_TEST(AcpNativeAdmissionRejectsPathlessLegacyWithoutQueueOrHistoryMutation)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	uam_test::TempDir temp("uam-pathless-legacy-admission");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
	ChatSession chat;
	chat.id = "pathless-legacy-owner";
	chat.provider_id = "gemini-cli";
	chat.workspace_directory = temp.root.string();
	chat.messages.push_back(Message{MessageRole::User, "Existing history"});
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	const auto primary = AppPaths::UamChatFilePath(app.data_root, chat.id);
	const auto before = uam_test::ReadFile(primary);
	auto owned = std::make_unique<uam::AcpSessionState>();
	owned->chat_id = chat.id;
	owned->provider_id = chat.provider_id;
	owned->running = owned->processing = true;
	owned->prompt_request_id = 71;
	auto* session = owned.get();
	app.acp_sessions.push_back(std::move(owned));
	for (const std::string path : {std::string(), std::string(" \t\r\n "), std::string("bad\0path", 8)})
	for (const std::string text : {"New authored request", ""})
	for (const bool steer : {false, true})
	{
		MessageAttachment attachment{.id = "malformed-legacy", .name = "Unavailable file", .kind = "file", .path = path};
		std::string error;
		const bool accepted = steer ? uam::SteerAcpPrompt(app, chat.id, text, {}, {attachment}, false, &error)
		    : uam::SendAcpPrompt(app, chat.id, text, {}, {attachment}, false, &error);
		UAM_ASSERT(!accepted);
		UAM_ASSERT(error.find("invalid path") != std::string::npos);
		UAM_ASSERT(session->queued_user_prompts.empty());
		UAM_ASSERT(app.chats.front().acp_queued_prompts.empty());
		UAM_ASSERT_EQ(app.chats.front().messages, chat.messages);
		UAM_ASSERT_EQ(session->prompt_request_id, 71);
		UAM_ASSERT_EQ(uam_test::ReadFile(primary), before);
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
	}
#endif
}

UAM_TEST(StructuredAttachmentsUseOnlyCurrentTurnAndPreserveBatchedImages)
{
	ChatSession chat;
	for (int index = 0; index < 4; ++index)
	{
		Message message;
		message.role = index == 2 ? MessageRole::Assistant : MessageRole::User;
		message.attachments.push_back(MessageAttachment{.id = std::to_string(index), .name = std::to_string(index) + ".png", .kind = "image", .path = std::to_string(index) + ".png"});
		chat.messages.push_back(message);
	}
	uam::AcpSessionState session;
	session.turn_first_user_message_index = 1;
	session.turn_user_message_index = 3;
	const auto attachments = uam::structured_attachments::CurrentTurn(chat, session);
	UAM_ASSERT_EQ(attachments.size(), std::size_t(2));
	UAM_ASSERT_EQ(attachments[0].id, std::string("1"));
	UAM_ASSERT_EQ(attachments[1].id, std::string("3"));
	chat.messages[1].acp_prompt_discarded = true;
	UAM_ASSERT_EQ(uam::structured_attachments::CurrentTurn(chat, session).size(), std::size_t(1));
	session.turn_first_user_message_index = -1;
	session.turn_user_message_index = -1;
	UAM_ASSERT(uam::structured_attachments::CurrentTurn(chat, session).empty());
}

UAM_TEST(StructuredAttachmentPathsBelongToProviderHostAndEscapeFileUris)
{
	std::string error;
	UAM_ASSERT_EQ(uam::structured_attachments::ProviderPath("/srv/project", ".UAM/a b#c.png", "linux", true, error), std::string("/srv/project/.UAM/a b#c.png"));
	UAM_ASSERT_EQ(uam::structured_attachments::ProviderPath("C:\\work", ".UAM\\pic.png", "windows", true, error), std::string("C:\\work\\.UAM\\pic.png"));
	UAM_ASSERT_EQ(uam::structured_attachments::FileUri("/srv/a b#c%20.png"), std::string("file:///srv/a%20b%23c%2520.png"));
	UAM_ASSERT_EQ(uam::structured_attachments::FileUri("C:\\work\\a b.png"), std::string("file:///C:/work/a%20b.png"));
	UAM_ASSERT_EQ(uam::structured_attachments::FileUri("\\\\server\\share\\a b.png"), std::string("file://server/share/a%20b.png"));
	UAM_ASSERT(uam::structured_attachments::ProviderPath("relative", "picture.png", "linux", true, error).empty());
	UAM_ASSERT(!error.empty());
}

UAM_TEST(StructuredAcpPreparedImagesUseNativeBlocksWithoutDuplicateResources)
{
	uam::AcpSessionState session;
	session.protocol_kind = uam::provider_profile_constants::kProtocolGeminiAcp;
	session.prompt_image_supported = true;
	nlohmann::json request = {{"params", {{"prompt", nlohmann::json::array({{{"type", "text"}, {"text", "Inspect this"}}})}}}};
	const nlohmann::json prepared = {{"message", {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "Inspect this"}}, {{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", "image/png"}, {"data", "prepared-image-bytes"}}}}})}}}};
	uam::structured_attachments::AppendPreparedAcpImages(request, prepared);
	std::string error;
	UAM_ASSERT(uam::structured_attachments::AppendNative(request, session, {{.name = "a space.png", .kind = "image", .path = "a space.png"}}, "/workspace", "", false, error));
	UAM_ASSERT_EQ(request["params"]["prompt"].size(), std::size_t(2));
	UAM_ASSERT_EQ(request["params"]["prompt"][1], nlohmann::json({{"type", "image"}, {"mimeType", "image/png"}, {"data", "prepared-image-bytes"}}));
}

UAM_TEST(StructuredCopilotAttachmentsUseSdkFileReferencesWithDisplayNames)
{
	uam::AcpSessionState session;
	session.protocol_kind = "copilot-sdk";
	nlohmann::json request = {{"params", {{"sessionId", "native-session"}, {"prompt", "Inspect this"}}}};
	std::string error;
	UAM_ASSERT(uam::structured_attachments::AppendNative(request, session, {{.name = "Diagram.png", .kind = "image", .path = "diagram.png"}}, "/srv/project", "linux", true, error));
	UAM_ASSERT_EQ(request["params"]["attachments"][0], nlohmann::json({{"type", "file"}, {"path", "/srv/project/diagram.png"}, {"displayName", "Diagram.png"}}));
	UAM_ASSERT(uam::structured_attachments::AppendNative(request, session, {{.name = "Source", .kind = "directory", .path = "src"}}, "/srv/project", "linux", true, error));
	UAM_ASSERT_EQ(request["params"]["attachments"][1], nlohmann::json({{"type", "directory"}, {"path", "/srv/project/src"}, {"displayName", "Source"}}));
}

UAM_TEST(StructuredGeminiFilesWithEscapedPathsKeepExistingLiteralReferences)
{
	uam::AcpSessionState session;
	session.protocol_kind = uam::provider_profile_constants::kProtocolGeminiAcp;
	nlohmann::json request = {{"params", {{"prompt", nlohmann::json::array({{{"type", "text"}, {"text", "Referenced files:\n- a space#%.txt"}}})}}}};
	const auto original = request;
	std::string error;
	UAM_ASSERT(uam::structured_attachments::AppendNative(request, session, {{.name = "a space#%.txt", .kind = "file", .path = "a space#%.txt"}}, "/workspace", "linux", true, error));
	UAM_ASSERT_EQ(request, original);
}

UAM_TEST(StructuredAcpCapabilityNegotiationClearsStaleImageSupport)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("gemini-cli");
	uam::AcpSessionState session;
	runtime.OnAcpInitializeResult(session, {{"agentCapabilities", {{"promptCapabilities", {{"image", true}}}}}});
	UAM_ASSERT(session.prompt_image_supported);
	for (const nlohmann::json invalid : {nlohmann::json("true"), nlohmann::json(1), nlohmann::json::object(), nlohmann::json::array(), nlohmann::json(nullptr), nlohmann::json(false)})
	{
		session.prompt_image_supported = true;
		runtime.OnAcpInitializeResult(session, {{"agentCapabilities", {{"promptCapabilities", {{"image", invalid}}}}}});
		UAM_ASSERT(!session.prompt_image_supported);
	}
	session.prompt_image_supported = true;
	runtime.OnAcpInitializeResult(session, nlohmann::json::object());
	UAM_ASSERT(!session.prompt_image_supported);
#endif
}

UAM_TEST(StructuredAttachmentsUseNativeCodexImagesOnLocalAndSshHosts)
{
	uam::AcpSessionState session;
	session.protocol_kind = uam::provider_profile_constants::kProtocolCodexAppServer;
	const std::vector<MessageAttachment> attachments{{.id = "image", .name = "Screenshot.png", .kind = "image", .mime_type = "image/png", .path = ".UAM/screen.png"}, {.id = "file", .name = "notes.txt", .kind = "file", .path = "notes.txt"}};
#if defined(_WIN32)
	// Local hosts resolve native local paths; only the SSH host uses the remote platform's POSIX spelling.
	const std::string local_workspace = "C:\\workspace", local_image = "C:\\workspace\\.UAM\\screen.png";
#else
	const std::string local_workspace = "/workspace", local_image = "/workspace/.UAM/screen.png";
#endif
	for (const bool remote : {false, true})
	{
		nlohmann::json request = {{"params", {{"input", nlohmann::json::array({{{"type", "text"}, {"text", "Inspect this"}}})}}}};
		std::string error;
		UAM_ASSERT(uam::structured_attachments::AppendNative(request, session, attachments, remote ? "/workspace" : local_workspace, "linux", remote, error));
		UAM_ASSERT_EQ(request["params"]["input"].size(), std::size_t(2));
		UAM_ASSERT_EQ(request["params"]["input"][1]["type"], nlohmann::json("localImage"));
		UAM_ASSERT_EQ(request["params"]["input"][1]["path"], nlohmann::json(remote ? "/workspace/.UAM/screen.png" : local_image));
	}
}

UAM_TEST(StructuredAcpAttachmentsRequireAdvertisedVisionAndKeepResourceIdentity)
{
	uam::AcpSessionState session;
	session.protocol_kind = uam::provider_profile_constants::kProtocolGeminiAcp;
	const std::vector<MessageAttachment> attachments{{.id = "image", .name = "Screenshot.png", .kind = "image", .mime_type = "image/png", .path = "screen.png"}};
	nlohmann::json request = {{"params", {{"prompt", nlohmann::json::array({{{"type", "text"}, {"text", "Inspect this"}}})}}}};
	const nlohmann::json original = request;
	std::string error;
	UAM_ASSERT(!uam::structured_attachments::AppendNative(request, session, attachments, "/workspace", "linux", true, error));
	UAM_ASSERT_EQ(request, original);
	UAM_ASSERT(error.find("image") != std::string::npos);
	session.prompt_image_supported = true;
	UAM_ASSERT(uam::structured_attachments::AppendNative(request, session, attachments, "/workspace", "linux", true, error));
	UAM_ASSERT_EQ(request["params"]["prompt"][1]["type"], nlohmann::json("resource_link"));
	UAM_ASSERT_EQ(request["params"]["prompt"][1]["uri"], nlohmann::json("file:///workspace/screen.png"));
	UAM_ASSERT_EQ(request["params"]["prompt"][1]["name"], nlohmann::json("Screenshot.png"));
	UAM_ASSERT_EQ(request["params"]["prompt"][1]["mimeType"], nlohmann::json("image/png"));
}

UAM_TEST(StructuredAttachmentsRejectUnsupportedNativeVisionWithoutDroppingImages)
{
	const std::vector<MessageAttachment> attachments{{.id = "image", .name = "Screenshot.png", .kind = "image", .path = "screen.png"}};
	uam::AcpSessionState session;
	session.protocol_kind = uam::provider_profile_constants::kProtocolClaudeCodeStreamJson;
	nlohmann::json request = {{"type", "user"}};
	std::string error;
	UAM_ASSERT(!uam::structured_attachments::AppendNative(request, session, attachments, "/workspace", "linux", true, error));
	UAM_ASSERT(error.find("SSH") != std::string::npos);
	UAM_ASSERT(uam::structured_attachments::AppendNative(request, session, attachments, "/workspace", "linux", false, error));
	session.protocol_kind = "unknown-native-protocol";
	UAM_ASSERT(!uam::structured_attachments::AppendNative(request, session, attachments, "/workspace", "linux", false, error));
}
