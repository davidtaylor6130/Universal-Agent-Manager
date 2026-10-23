#include "test_harness.h"
#include "common/paths/workspace_file_reader.h"
#include "common/provider/structured_attachments.h"

using namespace uam_test;

std::optional<int> RunRemoteAttachmentFixture(int argc, char** argv)
{
	if (argc != 3 || std::string_view(argv[1]) != "--uam-test-attachment-reader") return std::nullopt;
	const std::string mode = argv[2];
	const std::string content(512 * 1024 + 7, '\0');
	uam::remote::RunnerState state;
	nlohmann::json request;
	while (uam::remote::ReadFrame(std::cin, request) == uam::remote::FrameReadResult::Ok)
	{
		nlohmann::json response;
		if (request.value("type", "") == "hello")
		{
			response = uam::remote::HandleRunnerRequest(request, "development", &state);
			if (mode == "old-helper") response["capabilities"].erase("workspaceAttachmentRead");
		}
		else
		{
			const std::size_t offset = request.value("offset", std::size_t{0});
			const std::string_view chunk = std::string_view(content).substr(offset, 512 * 1024);
			nlohmann::json result{{"dataBase64", uam::base64::Encode(chunk)}, {"path", "image.png"}, {"offset", offset},
			    {"totalBytes", content.size()}, {"fingerprint", uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(content))}};
			if (mode == "wrong-hash") result["fingerprint"] = "0000000000000000";
			if (mode == "oversize") result["totalBytes"] = 25 * 1024 * 1024 + 1;
			if (mode == "bad-base64") result["dataBase64"] = "!not-base64!";
			if (offset > 0)
			{
				if (mode == "changed-path") result["path"] = "other.png";
				if (mode == "changed-size") result["totalBytes"] = content.size() + 1;
				if (mode == "duplicate-offset") result["offset"] = 0;
				if (mode == "changed-fingerprint") result["fingerprint"] = "0000000000000000";
			}
			response = {{"id", request["id"]}, {"ok", true}, {"result", std::move(result)}};
		}
		if (!uam::remote::WriteFrame(std::cout, response)) return 1;
		std::cout.flush();
	}
	return 0;
}

UAM_TEST(RemoteImageAttachmentClientRejectsOldHelpersCorruptAndMixedSnapshots)
{
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const std::string executable = uam::paths::Utf8PathString(service.ResolveCurrentExecutablePath());
	for (const std::string mode : {"old-helper", "wrong-hash", "oversize", "bad-base64", "changed-path", "changed-size", "duplicate-offset", "changed-fingerprint"})
	{
		uam::remote::RunnerClient client(service, {executable, "--uam-test-attachment-reader", mode});
		std::string bytes, error;
		UAM_ASSERT(!client.ReadWorkspaceAttachment("/workspace", "image.png", bytes, 25 * 1024 * 1024, &error));
		UAM_ASSERT(bytes.empty());
		UAM_ASSERT(!error.empty());
		if (mode == "old-helper") UAM_ASSERT(error.find("Update") != std::string::npos);
	}
}

UAM_TEST(RemoteImageAttachmentReadsBinaryWithinWorkspaceWithoutWeakeningTextPreview)
{
	TempDir temp("uam-remote-binary-attachment");
	const fs::path workspace = temp.root / "workspace";
	fs::create_directory(workspace);
	const std::string content("\x89PNG\0\xff\x80", 7);
	UAM_ASSERT(uam::io::WriteBinaryFile(workspace / "image with spaces.png", content));
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / "outside.png", content));
	UAM_ASSERT(!uam::workspace_files::Read(workspace, "image with spaces.png").ok);
	const auto binary = uam::workspace_files::Read(workspace, "image with spaces.png", true);
	UAM_ASSERT(binary.ok);
	UAM_ASSERT_EQ(binary.text, content);
	UAM_ASSERT(!uam::workspace_files::Read(workspace, "../outside.png", true).ok);
	UAM_ASSERT(!uam::workspace_files::Read(workspace, "missing.png", true).ok);
	std::error_code error;
	fs::create_symlink(temp.root / "outside.png", workspace / "outside-link.png", error);
	if (!error) UAM_ASSERT(!uam::workspace_files::Read(workspace, "outside-link.png", true).ok);
	UAM_ASSERT(uam::io::WriteBinaryFile(workspace / "oversize.png", std::string(25 * 1024 * 1024 + 1, 'x')));
	UAM_ASSERT(!uam::workspace_files::Read(workspace, "oversize.png", true).ok);
}

UAM_TEST(RemoteImageAttachmentChunksPreserveBinaryAndRejectChangedSnapshots)
{
	TempDir temp("uam-remote-image-chunks");
	uam::remote::RunnerState state;
	const nlohmann::json hello = uam::remote::HandleRunnerRequest({{"id", "hello"}, {"type", "hello"},
	    {"protocolVersion", uam::remote::kRunnerProtocolVersion}, {"nonce", "image"}}, "4.11.0", &state);
	UAM_ASSERT(hello["capabilities"].value("workspaceAttachmentRead", false));
	std::string content(1024 * 1024 + 17, '\0');
	content[512 * 1024 - 1] = static_cast<char>(0xff);
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / "image.png", content));
	nlohmann::json request = {{"id", "image"}, {"type", "workspace.attachment.read"},
	    {"workspace", uam::paths::Utf8PathString(temp.root)}, {"path", "image.png"}};
	const nlohmann::json first = state.HandleProcessRequest(request);
	UAM_ASSERT(first.value("ok", false));
	UAM_ASSERT(first.dump().size() < uam::remote::kMaxRunnerFrameBytes);
	std::string decoded;
	UAM_ASSERT(uam::base64::Decode(first["result"].value("dataBase64", ""), decoded));
	UAM_ASSERT_EQ(decoded, content.substr(0, 512 * 1024));
	request["offset"] = decoded.size();
	request["fingerprint"] = first["result"]["fingerprint"];
	const auto second = state.HandleProcessRequest(request);
	UAM_ASSERT(second.value("ok", false));
	UAM_ASSERT(second.dump().size() < uam::remote::kMaxRunnerFrameBytes);
	UAM_ASSERT(uam::base64::Decode(second["result"].value("dataBase64", ""), decoded));
	UAM_ASSERT_EQ(decoded, content.substr(512 * 1024, 512 * 1024));
	content.back() = 'x';
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / "image.png", content));
	UAM_ASSERT(!state.HandleProcessRequest(request).value("ok", true));
	request.erase("fingerprint");
	request["offset"] = -1;
	UAM_ASSERT(!state.HandleProcessRequest(request).value("ok", true));
}

UAM_TEST(RemoteImageAttachmentClientReassemblesBinaryAndRetainsByteLimits)
{
	TempDir temp("uam-remote-image-client");
	ScopedEnvVar data_root("UAM_DATA_DIR", (temp.root / "data").string());
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const fs::path runner = service.ResolveCurrentExecutablePath().parent_path() /
#if defined(_WIN32)
	    "uam-runner.exe";
#else
	    "uam-runner";
#endif
	uam::remote::RunnerClient client(service, {uam::paths::Utf8PathString(runner), "bridge-direct"});
	std::string content(1024 * 1024 + 5, '\0');
	content[512 * 1024] = static_cast<char>(0xff);
	UAM_ASSERT(uam::io::WriteBinaryFile(temp.root / "image.png", content));
	std::string bytes, error;
	UAM_ASSERT(client.ReadWorkspaceAttachment(uam::paths::Utf8PathString(temp.root), "image.png", bytes, 25 * 1024 * 1024, &error));
	UAM_ASSERT_EQ(bytes, content);
	UAM_ASSERT(!client.ReadWorkspaceAttachment(uam::paths::Utf8PathString(temp.root), "image.png", bytes, 512 * 1024, &error));
	UAM_ASSERT(bytes.empty());
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(!client.ReadWorkspaceAttachment(uam::paths::Utf8PathString(temp.root), "missing.png", bytes, 25 * 1024 * 1024, &error));
	UAM_ASSERT(bytes.empty());
}

UAM_TEST(RemoteImageInputUsesInjectedBytesAndRejectsBadImagesBeforeSend)
{
	std::string png;
	UAM_ASSERT(uam::base64::Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==", png));
	const std::vector<MessageAttachment> images{{.name = "remote.png", .kind = "image", .mime_type = "image/png", .path = "C:\\remote\\image.png"}};
	std::string error;
	int reads = 0;
	const auto build = [&images, &error, &reads](const std::string& bytes, std::stop_token cancel = {})
	{
		return uam::claude_input::BuildWithReader("Describe the image", images, error, true, cancel,
		    [&bytes, &reads](const MessageAttachment& image, std::string& output, std::size_t max_bytes, std::string&)
		    {
			    ++reads;
			    UAM_ASSERT_EQ(image.path, std::string("C:\\remote\\image.png"));
			    UAM_ASSERT(max_bytes >= bytes.size());
			    output = bytes;
			    return true;
		    });
	};
	const auto prepared = build(png);
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(prepared["message"]["content"][1]["source"]["data"], nlohmann::json(uam::base64::Encode(png)));
	const auto bounded = uam::claude_input::BuildWithReader("Describe the image", images, error, false, {},
	    [&png](const MessageAttachment&, std::string& bytes, std::size_t, std::string&) { bytes = png; return true; }, 512);
	UAM_ASSERT(!bounded.is_null() && bounded.dump().size() + 1 <= 512);
	UAM_ASSERT(uam::claude_input::BuildWithReader(std::string(2 * 1024 * 1024, 'x'), images, error, true, {},
	    [&png](const MessageAttachment&, std::string& bytes, std::size_t, std::string&) { bytes = png; return true; }, 2 * 1024 * 1024 - 16 * 1024).is_null());
	UAM_ASSERT(error.find("SSH") != std::string::npos);
	UAM_ASSERT(build("not an image").is_null());
	UAM_ASSERT(!error.empty());
	std::stop_source cancel;
	cancel.request_stop();
	const int before = reads;
	UAM_ASSERT(build(png, cancel.get_token()).is_null());
	UAM_ASSERT_EQ(reads, before);
}

UAM_TEST(RemoteImageCapabilityPersistsForOwnedProcessesAndClearsLocalImports)
{
	TempDir temp("uam-remote-image-capability");
	ChatSession chat;
	chat.id = "remote-vision";
	chat.provider_id = "gemini-cli";
	chat.execution_host_id = "ssh-test";
	chat.remote_process_exists = true;
	chat.remote_process_control_token = "private-process-token";
	chat.remote_prompt_image_supported = true;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	auto loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded && loaded->remote_prompt_image_supported);
	auto summary = ChatRepository::LoadLocalChatSummaryReadOnly(temp.root, chat.id);
	UAM_ASSERT(summary && summary->remote_prompt_image_supported);
	UAM_ASSERT(ChatRepository::HydrateChatMessages(temp.root, *summary));
	UAM_ASSERT(summary->remote_prompt_image_supported);
	chat.execution_host_id = "local";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded && !loaded->remote_prompt_image_supported);
	chat.execution_host_id = "ssh-test";
	chat.imported_read_only = true;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded && !loaded->remote_prompt_image_supported);
}

UAM_TEST(RemoteImageNegotiationSavesCapabilityOnlyForItsOwnedRemoteProcess)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	for (const bool owned : {false, true})
	{
		TempDir temp("uam-remote-image-negotiation");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "remote-vision";
		chat.provider_id = "gemini-cli";
		chat.execution_host_id = "ssh-test";
		chat.remote_process_exists = owned;
		chat.remote_process_control_token = "private-process-token";
		app.chats.push_back(chat);
		uam::AcpSessionState session;
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.protocol_kind = uam::provider_profile_constants::kProtocolGeminiAcp;
		session.process_execution_host_id = chat.execution_host_id;
		session.remote_output_delivery_token = chat.remote_process_control_token;
		session.pending_request_methods[1] = "initialize";
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
		    R"({"id":1,"result":{"agentCapabilities":{"promptCapabilities":{"image":true}}}})"));
		UAM_ASSERT(session.prompt_image_supported);
		UAM_ASSERT_EQ(app.chats.front().remote_prompt_image_supported, owned);
		if (owned)
		{
			const auto saved = ChatRepository::LoadLocalChat(temp.root, chat.id);
			UAM_ASSERT(saved && saved->remote_prompt_image_supported);
		}
	}
#endif
}
