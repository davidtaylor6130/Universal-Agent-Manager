#include "test_harness.h"
#include "app/markdown_store_service.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/provider/provider_setup.h"
#include "remote/runner_client.h"
#include "remote/runner_state.h"

#include <sstream>

using namespace uam_test;

namespace
{
	struct AttachedSkillFixture
	{
		TempDir temp{"uam-skill-discovery"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;
		MarkdownStoreService::Entry skill;
		std::string block;
		fs::path output = temp.root / "wire.jsonl";

		AttachedSkillFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
			app.settings.central_provider_configuration.enabled = false;
			app.settings.git_auto_worktree = false;
			app.settings.provider_chat_defaults["gemini-cli"].feature_preference = "provider";
			app.settings.markdown_store_directory = (temp.root / "skills").string();
			std::error_code setup_error;
			fs::create_directories(temp.root / "skills", setup_error);
			if (setup_error) throw TestFailure("Attached skill fixture directory: " + setup_error.message());
			std::string error;
			if (!MarkdownStoreService::IsConfiguredRoot(temp.root / "skills", &error))
				throw TestFailure("Attached skill fixture store: " + error);
			if (!MarkdownStoreService::CreateEntry(temp.root / "skills", {"Private skill", "Private", "", "CAPTURED_PRIVATE_SKILL_BODY\nRead the controlled fixture."}, &skill, &error))
				throw TestFailure("Attached skill fixture entry: " + error);
			UAM_ASSERT_EQ(skill.title, std::string("Private skill"));
			UAM_ASSERT(skill.body.find("CAPTURED_PRIVATE_SKILL_BODY") != std::string::npos);
			UAM_ASSERT(fs::is_regular_file(skill.file_path));
			block = "--- BEGIN ATTACHED SKILL: " + skill.title + " ---\n" + skill.body + "\n--- END ATTACHED SKILL ---";
			ChatSession chat;
			chat.id = "private-attached-skill";
			chat.provider_id = "gemini-cli";
			chat.workspace_directory = fs::canonical(temp.root).string();
			chat.approval_mode.clear();
			chat.memory_enabled = false;
			chat.messages.push_back({MessageRole::User, "Previously saved input"});
			app.chats.push_back(chat);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
			session = owned.get();
			session->chat_id = chat.id;
			session->provider_id = chat.provider_id;
			session->protocol_kind = "gemini-acp";
			session->session_id = "private-warm-session";
			session->current_mode_id = "default";
			const std::vector<std::string> command = StdioCatArgv(output);
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*session, temp.root, command, &error));
			session->running = session->session_ready = session->initialized = true;
			app.acp_sessions.push_back(std::move(owned));
		}

		~AttachedSkillFixture()
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(*session, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*session);
		}

		std::shared_ptr<uam::RemoteContextPreparation> PrepareOnly()
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (session->attached_skill_preparation == nullptr && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::FlushPendingChatSaves(app);
				(void)uam::acp_detail::SendQueuedPromptIfReady(app, *session, app.chats.front());
				std::this_thread::yield();
			}
			UAM_ASSERT(session->attached_skill_preparation != nullptr);
			UAM_ASSERT_EQ(session->prompt_request_id, 0);
			return session->attached_skill_preparation;
		}

		std::string Deliver()
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (session->prompt_request_id == 0 && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::FlushPendingChatSaves(app);
				(void)uam::acp_detail::SendQueuedPromptIfReady(app, *session, app.chats.front());
				std::this_thread::yield();
			}
			UAM_ASSERT(session->prompt_request_id != 0);
			std::string bytes;
			while (bytes.find('\n') == std::string::npos && std::chrono::steady_clock::now() < deadline)
			{ bytes = ReadFile(output); std::this_thread::yield(); }
			UAM_ASSERT_EQ(std::count(bytes.begin(), bytes.end(), '\n'), 1);
			if (bytes.starts_with(uam::remote::kRemoteInputDeliveryPrefix))
			{
				const std::size_t first = bytes.find(' ');
				const std::size_t second = bytes.find(' ', first + 1);
				const std::size_t third = bytes.find(' ', second + 1);
				UAM_ASSERT(first != std::string::npos && second != std::string::npos && third != std::string::npos);
				std::string payload;
				UAM_ASSERT(uam::base64::Decode(bytes.substr(third + 1, bytes.size() - third - 2), payload));
				bytes = std::move(payload);
			}
			const nlohmann::json request = nlohmann::json::parse(bytes);
			UAM_ASSERT_EQ(request.at("method"), nlohmann::json("session/prompt"));
			return request.at("params").at("prompt").at(0).at("text").get<std::string>();
		}
	};
}

UAM_TEST(AttachedSkillLocalDispatchUsesCapturedImmutableReference)
{
	AttachedSkillFixture fixture;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(fixture.app, fixture.app.chats.front().id, "Use the attached skill.", {fixture.skill.file_path.string()}, {}, false, &error));
	UAM_ASSERT(MarkdownStoreService::UpdateEntry(fixture.temp.root / "skills", fixture.skill.file_path.string(), {"Private skill", "Private", "", "MUTATED_LIBRARY_BODY"}, nullptr, &error));
	const std::string wire = fixture.Deliver();
	UAM_ASSERT(wire.find("CAPTURED_PRIVATE_SKILL_BODY") == std::string::npos);
	UAM_ASSERT(wire.find("Private skill") != std::string::npos);
	UAM_ASSERT(wire.find(".UAM/attached-skills/") != std::string::npos);
	UAM_ASSERT(wire.find("denied") != std::string::npos);
	UAM_ASSERT(wire.find("do not use") != std::string::npos);
	UAM_ASSERT(fixture.session->queued_prompt.empty() || fixture.session->queued_prompt.find(fixture.block) != std::string::npos);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.back().markdown_store_prompt_blocks, std::vector<std::string>{fixture.block});
	bool found = false;
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(fixture.temp.root / ".UAM" / "attached-skills"))
		if (entry.path().extension() == ".md") { UAM_ASSERT_EQ(ReadFile(entry.path()), fixture.block); found = true; }
	UAM_ASSERT(found);
}

UAM_TEST(AttachedSkillQuotedIdenticalAuthoredTextIsNeverRewritten)
{
	AttachedSkillFixture fixture;
	std::string error;
	const std::string authored = "Quoted example, preserve exactly:\n" + fixture.block;
	UAM_ASSERT(uam::SendAcpPrompt(fixture.app, fixture.app.chats.front().id, authored, {fixture.skill.file_path.string()}, {}, false, &error));
	const std::string wire = fixture.Deliver();
	UAM_ASSERT(wire.find(authored) != std::string::npos);
	UAM_ASSERT_EQ(CountSubstring(wire, fixture.block), std::size_t{1});
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.back().content, authored);
}

UAM_TEST(AttachedSkillRemoteInstallationRequiresFinalVerification)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	for (const std::string mode : {"cached", "missing-final", "invalid-final", "uploaded", "invalid-initial"})
	{
		TempDir temp("uam-skill-final-verify");
		const fs::path script = temp.root / "verify.py";
		const fs::path requests = temp.root / "requests.log";
		UAM_ASSERT(uam::io::WriteTextFile(script, R"(import base64,json,struct,sys
count=0;uploaded=b'';mode=sys.argv[1]
def read():
 h=sys.stdin.buffer.read(4)
 return json.loads(sys.stdin.buffer.read(struct.unpack('>I',h)[0])) if len(h)==4 else None
def send(v):
 b=json.dumps(v).encode();sys.stdout.buffer.write(struct.pack('>I',len(b))+b);sys.stdout.buffer.flush()
while True:
 r=read()
 if r is None:break
 t=r['type']
 with open(sys.argv[2],'a') as f:f.write(t+'\n')
 if t=='hello':
  send(dict(id=r['id'],ok=True,nonce=r['nonce'],protocolVersion=3,runnerVersion='development',capabilities=dict(processExecution=True,fileCopy=True,computerUse=False,centralProviderConfiguration=True)));continue
 if t=='configuration.prepare':
  count+=1
  missing=[] if mode=='cached' or (count>1 and mode=='uploaded') else ['skill.md']
  if mode=='invalid-initial' or (count>1 and mode=='invalid-final'):missing=None
  send(dict(id=r['id'],ok=True,result=dict(missing=missing)));continue
 assert mode not in ('cached','invalid-initial')
 if t=='file.begin':assert r['size']==len(b'Private captured skill')
 elif t=='file.write':uploaded+=base64.b64decode(r['dataBase64'],validate=True)
 elif t=='file.commit':assert uploaded==b'Private captured skill'
 else:raise AssertionError('Unexpected private verification request: '+t)
 send(dict(id=r['id'],ok=True,result={}))
)"));
		const std::string content = "Private captured skill";
		const nlohmann::json manifest{{"format", 1}, {"files", {{"skill.md", {{"size", static_cast<std::uint64_t>(content.size())}, {"digest", uam::provider_setup::Digest(content)}}}}}};
		const fs::path source = fs::canonical(temp.root) / uam::provider_setup::Digest(manifest.dump());
		std::vector<std::string> missing;
		std::string error;
		UAM_ASSERT(uam::provider_setup::Prepare(source, manifest, missing, error));
		UAM_ASSERT(uam::provider_setup::WriteRuntimeFile(source / "skill.md", content, error, false));
		uam::remote::RunnerClient client(PlatformServicesFactory::Instance().process_service, {"python3", script.string(), mode, requests.string()});
		const bool installed = client.InstallProviderConfiguration(source, source, &error);
		const bool expected = mode == "cached" || mode == "uploaded";
		if (installed != expected) throw TestFailure("Resource verification " + mode + " returned unexpected result: " + error);
		if (!expected) UAM_ASSERT(!error.empty());
		const bool upload_required = mode != "cached" && mode != "invalid-initial";
		const std::string expected_requests = std::string("hello\nconfiguration.prepare\n") +
		    (upload_required ? "file.begin\nfile.write\nfile.commit\nconfiguration.prepare\n" : "");
		const std::string actual_requests = ReadFile(requests);
		if (actual_requests != expected_requests)
			throw TestFailure("Resource verification " + mode + " request mismatch.\nExpected:\n" + expected_requests + "Actual:\n" + actual_requests);
	}
#endif
}

UAM_TEST(AttachedSkillPreparationFailureFallsBackBeforeDispatchOnly)
{
	for (const bool linked : {false, true})
	{
#if defined(_WIN32)
		if (linked) continue;
#endif
		AttachedSkillFixture fixture;
		const std::string sentinel = "Private obstruction, preserve exactly.";
		const fs::path linked_target = fixture.temp.root / "linked-target";
		if (linked)
		{
			fs::create_directory(linked_target);
			UAM_ASSERT(uam::io::WriteTextFile(linked_target / "sentinel", sentinel));
			fs::create_directory_symlink(linked_target, fixture.temp.root / ".UAM");
		}
		else UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / ".UAM", sentinel));
		std::string error;
		UAM_ASSERT(uam::SendAcpPrompt(fixture.app, fixture.app.chats.front().id, "Use captured inline fallback.", {fixture.skill.file_path.string()}, {}, false, &error));
		const std::string wire = fixture.Deliver();
		UAM_ASSERT(wire.find(fixture.block) != std::string::npos);
		UAM_ASSERT(wire.find(".UAM/attached-skills/") == std::string::npos);
		UAM_ASSERT_EQ(ReadFile(linked ? linked_target / "sentinel" : fixture.temp.root / ".UAM"), sentinel);
		if (linked) UAM_ASSERT(!fs::exists(linked_target / "attached-skills"));
		UAM_ASSERT_EQ(fixture.app.chats.front().messages.back().markdown_store_prompt_blocks, std::vector<std::string>{fixture.block});
	}
}

UAM_TEST(AttachedSkillStalePreparationCannotFallBackOrSend)
{
	for (const std::string changed : {"workspace", "configuration", "incarnation", "native", "process", "turn", "root", "provider", "host", "prompt", "span"})
	{
		AttachedSkillFixture fixture;
		std::string error;
		UAM_ASSERT(uam::SendAcpPrompt(fixture.app, fixture.app.chats.front().id, "Retain this authored input.", {fixture.skill.file_path.string()}, {}, false, &error));
		const std::shared_ptr<uam::RemoteContextPreparation> state = fixture.PrepareOnly();
		ChatSession& chat = fixture.app.chats.front();
		if (changed == "workspace") chat.workspace_directory = (fixture.temp.root / "changed-workspace").string();
		if (changed == "configuration") chat.model_id = "private-changed-model";
		if (changed == "incarnation") chat.metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		if (changed == "native") fixture.session->session_id = "private-replacement-native";
		if (changed == "process") fixture.session->last_process_id = "private-replacement-process";
		if (changed == "turn") ++fixture.session->turn_serial;
		if (changed == "root") fixture.app.data_root = fixture.temp.root / "other-data";
		if (changed == "provider") chat.provider_id = "private-replacement-provider";
		if (changed == "host") { ExecutionHost host; host.instruction_text = "Changed local instructions"; fixture.app.settings.execution_hosts = {host}; }
		if (changed == "prompt") fixture.session->queued_prompt += "Changed";
		if (changed == "span") ++fixture.session->queued_skill_spans.front().offset;
		UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, chat));
		UAM_ASSERT(state->cancel.stop_requested());
		UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
		UAM_ASSERT(fixture.session->last_error.find("ownership changed") != std::string::npos);
		UAM_ASSERT_EQ(chat.messages.back().content, std::string("Retain this authored input."));
		UAM_ASSERT_EQ(chat.messages.back().markdown_store_prompt_blocks, std::vector<std::string>{fixture.block});
		UAM_ASSERT(ReadFile(fixture.output).empty());
	}
}

UAM_TEST(AttachedSkillCancelRetiresPreparationWithoutPromptWrite)
{
	AttachedSkillFixture fixture;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(fixture.app, fixture.app.chats.front().id, "Cancel pending skill preparation.", {fixture.skill.file_path.string()}, {}, false, &error));
	const std::shared_ptr<uam::RemoteContextPreparation> state = fixture.PrepareOnly();
	UAM_ASSERT(uam::CancelAcpTurn(fixture.app, fixture.app.chats.front().id, &error));
	UAM_ASSERT(state->cancel.stop_requested());
	UAM_ASSERT(fixture.session->queued_skill_spans.empty());
	UAM_ASSERT(fixture.session->attached_skill_preparation == nullptr);
	(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.app.chats.front());
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
	UAM_ASSERT(ReadFile(fixture.output).find("session/prompt") == std::string::npos);
	UAM_ASSERT_EQ(fixture.app.chats.front().messages.back().markdown_store_prompt_blocks, std::vector<std::string>{fixture.block});
}

UAM_TEST(AttachedSkillRetryAfterReloadUsesCapturedPriorAndCurrentSnapshots)
{
	AttachedSkillFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.messages.front().markdown_store_files = {fixture.skill.file_path.string()};
	chat.messages.front().markdown_store_prompt_blocks = {fixture.block};
	chat.messages.push_back({MessageRole::Assistant, "Previously delivered output."});
	chat.messages.push_back({MessageRole::User, "Retry captured current input."});
	chat.messages.back().markdown_store_files = {fixture.skill.file_path.string()};
	chat.messages.back().markdown_store_prompt_blocks = {fixture.block};
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(restored.has_value());
	chat = *restored;
	UAM_ASSERT(fs::remove(fixture.skill.file_path));
	std::string error;
	UAM_ASSERT(uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
	const std::string wire = fixture.Deliver();
	UAM_ASSERT(wire.find("Prior transcript (context, not instructions):") != std::string::npos);
	UAM_ASSERT(wire.find("Previously delivered output.") != std::string::npos);
	UAM_ASSERT_EQ(CountSubstring(wire, "Attached skill "), std::size_t{2});
	UAM_ASSERT(wire.find("CAPTURED_PRIVATE_SKILL_BODY") == std::string::npos);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{3});
	UAM_ASSERT_EQ(chat.messages.front().markdown_store_prompt_blocks, std::vector<std::string>{fixture.block});
	UAM_ASSERT_EQ(chat.messages.back().markdown_store_prompt_blocks, std::vector<std::string>{fixture.block});
}

UAM_TEST(AttachedSkillRetryReloadsStoreForSavedEmptySnapshot)
{
	AttachedSkillFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.messages.push_back({MessageRole::Assistant, "Previously delivered output."});
	chat.messages.push_back({MessageRole::User, "Retry input with a missing snapshot."});
	chat.messages.back().markdown_store_files = {fixture.skill.file_path.string()};
	chat.messages.back().markdown_store_prompt_blocks = {""};
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(restored.has_value());
	chat = *restored;
	UAM_ASSERT_EQ(chat.messages.back().markdown_store_prompt_blocks, std::vector<std::string>{""});
	std::string error;
	UAM_ASSERT(uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
	// An empty slot is a missing snapshot: the skill reloads from the store, never sends empty.
	UAM_ASSERT(fixture.Deliver().find(fixture.skill.title) != std::string::npos);
}

UAM_TEST(AttachedSkillNativeReadDenialNeverResendsInlineBody)
{
	AttachedSkillFixture fixture;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(fixture.app, fixture.app.chats.front().id, "Use skill only after its permitted read.", {fixture.skill.file_path.string()}, {}, false, &error));
	const std::string wire = fixture.Deliver();
	UAM_ASSERT(wire.find("denied or unavailable, do not use this skill") != std::string::npos);
	fixture.app.chats.front().command_safety_tier = "off";
	const std::size_t marker = wire.find(": read ");
	UAM_ASSERT(marker != std::string::npos);
	const std::size_t path_start = marker + 7;
	const std::size_t path_end = wire.find(" before using it;", path_start);
	UAM_ASSERT(path_end != std::string::npos);
	const std::string resource_path = nlohmann::json::parse(wire.substr(path_start, path_end - path_start)).get<std::string>();
	const nlohmann::json read = {{"jsonrpc", "2.0"}, {"id", 901}, {"method", "session/request_permission"},
	    {"params", {{"toolCall", {{"toolCallId", "private-skill-read"}, {"title", "Read attached skill"}, {"kind", "read"}, {"rawInput", {{"path", resource_path}}}}},
	    {"options", nlohmann::json::array({{{"optionId", "deny"}, {"kind", "reject_once"}}, {{"optionId", "allow"}, {"kind", "allow_once"}}})}}}};
	UAM_ASSERT(uam::ProcessAcpLineForTests(fixture.app, *fixture.session, fixture.app.chats.front(), read.dump()));
	UAM_ASSERT(fixture.session->waiting_for_permission);
	UAM_ASSERT(uam::ResolveAcpPermission(fixture.app, fixture.app.chats.front().id, "901", "deny", false, &error));
	for (int poll = 0; poll < 10; ++poll)
		(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.app.chats.front());
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	std::string bytes;
	while (std::chrono::steady_clock::now() < deadline)
	{
		bytes = ReadFile(fixture.output);
		if (std::count(bytes.begin(), bytes.end(), '\n') >= 2) break;
		std::this_thread::yield();
	}
	UAM_ASSERT_EQ(std::count(bytes.begin(), bytes.end(), '\n'), 2);
	UAM_ASSERT_EQ(CountSubstring(bytes, "session/prompt"), std::size_t{1});
	UAM_ASSERT(bytes.find("CAPTURED_PRIVATE_SKILL_BODY") == std::string::npos);
	const nlohmann::json response = nlohmann::json::parse(bytes.substr(bytes.find('\n') + 1));
	UAM_ASSERT_EQ(response.at("id"), nlohmann::json(901));
	UAM_ASSERT_EQ(response.at("result").at("outcome").at("optionId"), nlohmann::json("deny"));
}

UAM_TEST(AttachedSkillInvalidSpansAndOversizeNeverPublishOrOverwrite)
{
	TempDir temp("uam-skill-span-guards");
	const fs::path workspace = fs::canonical(temp.root);
	const std::string content = "Immutable captured skill";
	const uam::attached_skills::Span valid{0, content.size(), uam::provider_setup::Digest(content)};
	std::string error, wire;
	for (const std::vector<uam::attached_skills::Span>& spans : std::vector<std::vector<uam::attached_skills::Span>>{
	    {{1, content.size(), valid.digest}}, {{0, content.size(), "wrong"}}, {valid, valid}})
	{
		UAM_ASSERT(!uam::attached_skills::Prepare(workspace / "data", ExecutionHost{}, workspace.string(), content, spans, wire, error));
		UAM_ASSERT(!fs::exists(workspace / ".UAM"));
	}
	const std::string oversized(2 * 1024 * 1024 + 1, 's');
	UAM_ASSERT(!uam::attached_skills::Prepare(workspace / "data", ExecutionHost{}, workspace.string(), oversized,
	    {{0, oversized.size(), uam::provider_setup::Digest(oversized)}}, wire, error));
	UAM_ASSERT(!fs::exists(workspace / ".UAM"));
	UAM_ASSERT(uam::attached_skills::Prepare(workspace / "data", ExecutionHost{}, workspace.string(), content, {valid}, wire, error));
	fs::path resource;
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(workspace / ".UAM"))
		if (entry.path().extension() == ".md") resource = entry.path();
	UAM_ASSERT(!resource.empty());
	UAM_ASSERT(uam::io::WriteTextFile(resource, "Foreign replacement, preserve exactly."));
	UAM_ASSERT(!uam::attached_skills::Prepare(workspace / "data", ExecutionHost{}, workspace.string(), content, {valid}, wire, error));
	UAM_ASSERT(wire.empty());
	UAM_ASSERT_EQ(ReadFile(resource), std::string("Foreign replacement, preserve exactly."));
}

UAM_TEST(AttachedSkillWarmSshPublishesCapturedResourcesBeforeExactDelivery)
{
#if !defined(_WIN32) && UAM_ENABLE_SSH
	for (const std::string platform : {"linux", "windows"})
	{
		AttachedSkillFixture fixture;
		const fs::path script = fixture.temp.root / "skill-runner.py";
		const fs::path record = fixture.temp.root / "resources.jsonl";
		const fs::path traceback = fixture.temp.root / "runner-stderr.log";
		UAM_ASSERT(uam::io::WriteTextFile(script, R"(import base64,json,struct,sys
resources={};uploads={}
def managed_folder(path):return path.replace('\\','/').split('/')[-3:-1]==['.UAM','provider-setup']
assert managed_folder('/private/workspace/.UAM/provider-setup/hash')
assert managed_folder('C:\\private workspace\\.UAM\\provider-setup\\hash')
assert not managed_folder('/private/workspace/.UAM/foreign-folder/hash')
def read():
 h=sys.stdin.buffer.read(4)
 return json.loads(sys.stdin.buffer.read(struct.unpack('>I',h)[0])) if len(h)==4 else None
def send(v):
 b=json.dumps(v).encode();sys.stdout.buffer.write(struct.pack('>I',len(b))+b);sys.stdout.buffer.flush()
def digest(b):
 h=int(sys.argv[2])
 for c in b:h=((h^c)*int(sys.argv[3]))&0xffffffffffffffff
 return '%016x'%h
while True:
 r=read()
 if r is None:break
 t=r['type']
 with open(sys.argv[1],'a') as f:f.write(json.dumps(r)+'\n')
 if t=='hello':
  send(dict(id=r['id'],ok=True,nonce=r['nonce'],protocolVersion=3,runnerVersion='development',capabilities=dict(processExecution=True,fileCopy=True,computerUse=False,centralProviderConfiguration=True)));continue
 if t=='configuration.prepare':
  m=r['manifest'];assert set(m)=={'format','files'}
  assert managed_folder(r['directory'])
  missing=[n for n,e in m['files'].items() if n not in resources or len(resources[n])!=e['size'] or digest(resources[n])!=e['digest']]
  send(dict(id=r['id'],ok=True,result=dict(missing=missing)));continue
 if t=='file.begin':uploads[r['uploadId']]=[r['path'],b'',r['size'],r['digest']]
 elif t=='file.write':uploads[r['uploadId']][1]+=base64.b64decode(r['dataBase64'],validate=True)
 elif t=='file.commit':
  p,b,size,d=uploads.pop(r['uploadId']);assert len(b)==size and digest(b)==d
  resources[p.replace('\\','/').rsplit('/',1)[-1]]=b
 elif t=='file.abort':uploads.pop(r['uploadId'],None)
 else:raise AssertionError('Unexpected private runner request: '+t)
 send(dict(id=r['id'],ok=True,result={}))
)"));
		const fs::path ssh = fixture.temp.root / "ssh";
		UAM_ASSERT(uam::io::WriteTextFile(ssh, "#!/bin/sh\nexec python3 " + ShellQuoteForTest(script.string()) + " " + ShellQuoteForTest(record.string()) +
		    " " + std::to_string(uam::hashing::kFnv1a64OffsetBasis) + " " + std::to_string(uam::hashing::kFnv1a64Prime) + " 2>>" + ShellQuoteForTest(traceback.string()) + "\n"));
		fs::permissions(ssh, fs::perms::owner_all);
		ScopedEnvVar search("PATH", fixture.temp.root.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin"));
		ExecutionHost host;
		host.id = "private-skills-ssh"; host.transport = "ssh"; host.ssh_alias = "private-fixture";
		host.platform = platform; host.runner_version = "development"; host.runner_protocol_version = 3;
		host.instruction_text = "";
		fixture.app.settings.execution_hosts = {host};
		ChatSession& chat = fixture.app.chats.front();
		chat.execution_host_id = host.id;
		chat.workspace_directory = platform == "windows" ? "C:\\private skill workspace" : (fs::canonical(fixture.temp.root) / "private-remote-workspace").string();
		fixture.session->process_execution_host_id = host.id;
		fixture.session->remote_runner_protocol_version = 3;
		fixture.session->remote_output_delivery_token = "private-owned-output-token";
		const std::string native = fixture.session->session_id;
		const std::string process = uam::acp_detail::AcpProcessHandleLabel(*fixture.session);
		std::string error;
		if (!uam::SendAcpPrompt(fixture.app, chat.id, "Read the captured skill before use.", {fixture.skill.file_path.string()}, {}, false, &error))
			throw TestFailure("Warm SSH " + platform + " admission: " + error + "\nPrivate requests:\n" + ReadFile(record) + "\nPrivate runner stderr:\n" + ReadFile(traceback));
		const std::string wire = fixture.Deliver();
		if (wire.find("CAPTURED_PRIVATE_SKILL_BODY") != std::string::npos)
		{
			std::string cause;
			for (const uam::AcpDiagnosticEntryState& diagnostic : fixture.session->diagnostics)
				if (diagnostic.reason == "resource_preparation_inline_fallback") cause = diagnostic.message;
			throw TestFailure("Warm SSH " + platform + " unexpectedly used inline body: " + cause + "\nPrivate requests:\n" + ReadFile(record) + "\nPrivate runner stderr:\n" + ReadFile(traceback));
		}
		UAM_ASSERT(wire.find(platform == "windows" ? ".UAM\\\\provider-setup\\\\" : ".UAM/provider-setup/") != std::string::npos);
		UAM_ASSERT_EQ(fixture.session->session_id, native);
		UAM_ASSERT_EQ(uam::acp_detail::AcpProcessHandleLabel(*fixture.session), process);
		UAM_ASSERT(!chat.remote_prompt_delivery_id.empty());
		const std::string delivery_id = chat.remote_prompt_delivery_id;
		const std::string delivery_payload = chat.remote_prompt_delivery_payload;
		UAM_ASSERT(delivery_payload.find("CAPTURED_PRIVATE_SKILL_BODY") == std::string::npos);
		UAM_ASSERT_EQ(nlohmann::json::parse(delivery_payload).at("params").at("prompt").at(0).at("text"), nlohmann::json(wire));
		UAM_ASSERT_EQ(chat.messages.back().markdown_store_prompt_blocks, std::vector<std::string>{fixture.block});
		const std::string requests = ReadFile(record);
		UAM_ASSERT_EQ(CountSubstring(requests, "configuration.prepare"), std::size_t{2});
		UAM_ASSERT(requests.find("process.start") == std::string::npos);
		UAM_ASSERT(requests.find("configuration.apply") == std::string::npos);
		UAM_ASSERT(requests.find("context.prepare") == std::string::npos);
		std::istringstream lines(requests);
		std::string line, uploaded;
		nlohmann::json prepare_request;
		while (std::getline(lines, line))
		{
			const nlohmann::json request = nlohmann::json::parse(line);
			if (request.at("type") == "configuration.prepare") prepare_request = request;
			if (request.at("type") == "file.write")
			{
				std::string chunk;
				UAM_ASSERT(uam::base64::Decode(request.at("dataBase64").get<std::string>(), chunk));
				uploaded += chunk;
			}
		}
		UAM_ASSERT_EQ(uploaded, fixture.block);
		if (platform == "linux")
		{
			// Exercise the actual helper path/manifest authority, not just the framed fake.
			uam::remote::RunnerState runner;
			UAM_ASSERT(runner.HandleProcessRequest(prepare_request).value("ok", false));
			const fs::path directory = uam::paths::PathFromUtf8(prepare_request.at("directory").get<std::string>());
			const std::string name = prepare_request.at("manifest").at("files").begin().key();
			UAM_ASSERT(uam::provider_setup::WriteRuntimeFile(directory / name, uploaded, error, false));
			const nlohmann::json verified = runner.HandleProcessRequest(prepare_request);
			UAM_ASSERT(verified.value("ok", false));
			UAM_ASSERT(verified.at("result").at("missing").empty());
			UAM_ASSERT_EQ(ReadFile(directory / name), fixture.block);
		}
		UAM_ASSERT(MarkdownStoreService::UpdateEntry(fixture.temp.root / "skills", fixture.skill.file_path.string(), {"Private skill", "Private", "", "Changed after dispatch"}, nullptr, &error));
		for (int poll = 0; poll < 10; ++poll) (void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, chat);
		UAM_ASSERT_EQ(chat.remote_prompt_delivery_id, delivery_id);
		UAM_ASSERT_EQ(chat.remote_prompt_delivery_payload, delivery_payload);
		UAM_ASSERT_EQ(ReadFile(record), requests);
	}
#endif
}

UAM_TEST(AttachedSkillRecallAgentAndComputerPrefixesPreserveExactSpans)
{
	AttachedSkillFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.messages.clear();
	chat.memory_enabled = true;
	chat.memory_level = "open";
	fixture.app.settings.memory_recall_budget_bytes = 2048;
	const fs::path category = MemoryService::CategoryPath(MemoryService::LocalMemoryRoot(fixture.temp.root), "Lessons/User_Lessons");
	fs::create_directories(category);
	UAM_ASSERT(uam::io::WriteTextFile(category / "private.md", "## Memory\nRemember PRIVATE_SKILL_RECALL_SENTINEL\n"));
	uam::AcpQueuedUserPromptState queued;
	queued.id = "private-prefix-input";
	queued.revision = 1;
	queued.text = "Preserve this authored quotation: " + fixture.block;
	queued.markdown_store_files = {fixture.skill.file_path.string()};
	queued.markdown_store_prompt_blocks = {fixture.block};
	queued.uam_agent_id = "build";
	queued.uam_agent_definition_hash = "private-prefix-hash";
	queued.uam_agent_definition_snapshot = "PRIVATE_SKILL_AGENT_PREFIX";
	queued.uam_agent_instructions = queued.uam_agent_definition_snapshot;
	queued.uam_agent_execution_capability = "uam-prompt-injected";
#if UAM_ENABLE_COMPUTER_USE
	fixture.app.settings.computer_use_globally_enabled = true;
	chat.computer_use_globally_enabled = true;
	chat.computer_use_enabled = true;
	chat.computer_use_backend = "uam";
	queued.computer_use_mode = true;
#endif
	fixture.session->queued_user_prompts.push_back(queued);
	chat.acp_queued_prompts = {queued};
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	UAM_ASSERT(uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, chat));
	const std::string wire = fixture.Deliver();
	UAM_ASSERT(wire.find("PRIVATE_SKILL_AGENT_PREFIX") != std::string::npos);
	UAM_ASSERT(wire.find("PRIVATE_SKILL_RECALL_SENTINEL") != std::string::npos);
	UAM_ASSERT(wire.find(queued.text) != std::string::npos);
	UAM_ASSERT_EQ(CountSubstring(wire, fixture.block), std::size_t{1});
	UAM_ASSERT_EQ(CountSubstring(wire, "Attached skill "), std::size_t{1});
#if UAM_ENABLE_COMPUTER_USE
	UAM_ASSERT(wire.find("User task:") != std::string::npos);
#endif
	UAM_ASSERT_EQ(chat.messages.back().content, queued.text);
	UAM_ASSERT_EQ(chat.messages.back().markdown_store_prompt_blocks, queued.markdown_store_prompt_blocks);
}
