#include "test_harness.h"

using namespace uam_test;

UAM_TEST(ClaudeDiscoveryPreservesOnlyReportedModelTokenCapacities)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	uam::AcpSessionState session;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("claude-cli");
	runtime.OnAcpInitializeResult(session, {{"models", {{{"value", "fixture-claude"}, {"contextWindow", 200000}, {"maxOutputTokens", 64000}}, {{"value", "unknown-claude"}}, {{"value", "invalid-claude"}, {"contextWindow", -1}, {"maxOutputTokens", "64000"}}}}});
	UAM_ASSERT_EQ(session.available_models.size(), std::size_t{3});
	UAM_ASSERT_EQ(session.available_models[0].context_window_tokens.value_or(-1), std::int64_t{200000});
	UAM_ASSERT_EQ(session.available_models[0].max_output_tokens.value_or(-1), std::int64_t{64000});
	UAM_ASSERT(!session.available_models[1].context_window_tokens.has_value());
	UAM_ASSERT(!session.available_models[1].max_output_tokens.has_value());
	UAM_ASSERT(!session.available_models[2].context_window_tokens.has_value());
	UAM_ASSERT(!session.available_models[2].max_output_tokens.has_value());
#endif
}

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
#include "common/provider/claude_preinput_observation.h"

namespace
{
	struct ProcessFreeClaudeObservation
	{
		TempDir temp{"uam-cold-claude-injected-observation"};
		uam::AppState app;
		ChatSession chat;
		uam::AcpSessionState session;
		std::string process = "injected-original-claude-generation";
		std::shared_ptr<const char> writer = std::make_shared<const char>(0);
		std::shared_ptr<uam::claude_preinput::InjectedTransportForTests> transport;
		std::vector<nlohmann::json> controls;
		ProcessFreeClaudeObservation()
		{
			app.data_root = temp.root;
			chat.id = "injected-observer-owner"; chat.created_at = "2026-10-23T00:00:00.000Z";
			chat.provider_id = "claude-cli"; chat.execution_host_id = "local";
			chat.native_session_id = "b3509391-8a4f-43e1-9d87-dd196b3a1697";
			chat.model_id = "claude-sonnet-4-6"; chat.approval_mode = "default"; chat.command_safety_tier = "off";
			chat.workspace_directory = temp.root.string();
			session.chat_id = chat.id; session.provider_id = chat.provider_id; session.session_id = chat.native_session_id;
			session.protocol_kind = "claude-code-stream-json"; session.process_execution_host_id = "local";
			session.current_model_id = "unprojected-model"; session.current_mode_id = "unprojected-mode";
			session.running = true; // Injected runtime DATA; no OS handle or process is created.
			transport = std::make_shared<uam::claude_preinput::InjectedTransportForTests>();
			transport->process = [&] { return process; };
			transport->writer = [&]() -> std::shared_ptr<const void> { return writer; };
			transport->write_control = [&](const nlohmann::json& request) { controls.push_back(request); return true; };
			UAM_ASSERT(uam::claude_preinput::ArmWithInjectedTransportForTests(app, session, chat, transport));
		}
		void Respond(nlohmann::json payload)
		{
			nlohmann::json frame = {{"type", "control_response"}, {"response", {{"subtype", "success"},
			    {"request_id", controls.back().at("request_id")}, {"response", std::move(payload)}}}};
			if (controls.back().at("request").at("subtype") == "initialize")
			{
				frame["response"]["pending_permission_requests"] = nlohmann::json::array();
				frame["response"]["pending_user_dialog_requests"] = nlohmann::json::array();
			}
			UAM_ASSERT(uam::claude_preinput::BeforeSharedDispatch(app, session, chat, &frame));
		}
		std::shared_ptr<const uam::claude_preinput::NativeWitness> Observe()
		{
			UAM_ASSERT(!uam::claude_preinput::CurrentWitness(app, session, chat));
			Respond({{"claude_code_version", "2.1.302"}, {"current_permission_mode", "default"}});
			Respond({{"sections", {{{"title", "Session"}, {"rows", {{{"label", "Session ID"}, {"value", chat.native_session_id}}}}}}}});
			Respond({{"applied", {{"model", chat.model_id}}}});
			const auto witness = uam::claude_preinput::CurrentWitness(app, session, chat);
			UAM_ASSERT(witness && witness->Injected() && witness->Current(app, session, chat));
			UAM_ASSERT(!session.stdin_writer && session.pending_request_methods.empty() && controls.size() == 3);
			return witness;
		}
	};
}

UAM_TEST(ClaudeColdInjectedObserverHandoffRetainsOriginalNativeFences)
{
	for (const std::string fault : {"none", "epoch", "writer", "process", "stop", "model", "late-frame"})
	{
		ProcessFreeClaudeObservation fixture;
		const auto witness = fixture.Observe();
		UAM_ASSERT(!fixture.session.initialized && !fixture.session.session_ready);
		UAM_ASSERT(witness->Handoff(fixture.app, fixture.session, fixture.chat));
		UAM_ASSERT(fixture.session.initialized && fixture.session.session_ready);
		UAM_ASSERT_EQ(fixture.session.current_model_id, fixture.chat.model_id);
		const auto epoch = fixture.session.permission_review_incarnation;
		const auto writer = fixture.writer;
		const auto model = fixture.chat.model_id;
		if (fault == "epoch") fixture.session.permission_review_incarnation = std::make_shared<const char>(0);
		if (fault == "writer") fixture.writer = std::make_shared<const char>(0);
		if (fault == "process") fixture.process = "injected-replacement-generation";
		if (fault == "stop") fixture.session.local_stop_pending = true;
		if (fault == "model") fixture.chat.model_id = "foreign-model";
		const nlohmann::json late = {{"type", "assistant"}, {"session_id", fixture.chat.native_session_id}, {"message", {{"role", "assistant"}, {"content", nlohmann::json::array()}}}};
		if (fault == "late-frame") UAM_ASSERT(uam::claude_preinput::BeforeSharedDispatch(fixture.app, fixture.session, fixture.chat, &late));
		if (fault == "none")
		{
			UAM_ASSERT(witness->InputAttempted(fixture.app, fixture.session, fixture.chat));
			UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat), uam::claude_preinput::State::InputAttempted);
			UAM_ASSERT(!uam::claude_preinput::BeforeSharedDispatch(fixture.app, fixture.session, fixture.chat, &late));
			UAM_ASSERT(!witness->InputAttempted(fixture.app, fixture.session, fixture.chat));
		}
		else
		{
			UAM_ASSERT(!witness->Current(fixture.app, fixture.session, fixture.chat));
			fixture.session.permission_review_incarnation = epoch; fixture.writer = writer; fixture.chat.model_id = model;
			fixture.session.local_stop_pending = false; fixture.process = "injected-original-claude-generation";
			UAM_ASSERT(!witness->Current(fixture.app, fixture.session, fixture.chat));
			UAM_ASSERT(!uam::claude_preinput::CurrentWitness(fixture.app, fixture.session, fixture.chat));
		}
		UAM_ASSERT_EQ(fixture.controls.size(), std::size_t{3});
		UAM_ASSERT(!fixture.session.stdin_writer && fixture.chat.messages.empty() && fixture.app.pending_chat_save_tasks.empty());
	}
}

UAM_TEST(ClaudeColdInjectedObservationRefusesBeforeAnyReadinessProjection)
{
	ProcessFreeClaudeObservation fixture;
	fixture.Respond({{"claude_code_version", "2.1.302"}, {"current_permission_mode", "default"}});
	fixture.Respond({{"sections", {{{"title", "Session"}, {"rows", {{{"label", "Session ID"}, {"value", fixture.chat.native_session_id}}}}}}}});
	UAM_ASSERT(!uam::claude_preinput::CurrentWitness(fixture.app, fixture.session, fixture.chat));
	fixture.Respond({{"applied", {{"model", "foreign-model"}}}});
	UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat), uam::claude_preinput::State::Refused);
	UAM_ASSERT(!uam::claude_preinput::CurrentWitness(fixture.app, fixture.session, fixture.chat));
	UAM_ASSERT(!fixture.session.initialized && !fixture.session.session_ready && !fixture.session.stdin_writer);
	UAM_ASSERT_EQ(fixture.session.current_model_id, std::string("unprojected-model"));
	UAM_ASSERT_EQ(fixture.controls.size(), std::size_t{3});

	ProcessFreeClaudeObservation handoff;
	const auto witness = handoff.Observe();
	int identity_reads = 0;
	handoff.transport->process = [&]
	{
		return ++identity_reads == 1 ? handoff.process : std::string("injected-drift-at-readiness-handoff");
	};
	UAM_ASSERT(!witness->Handoff(handoff.app, handoff.session, handoff.chat));
	UAM_ASSERT(!handoff.session.initialized && !handoff.session.session_ready);
	UAM_ASSERT(!witness->Current(handoff.app, handoff.session, handoff.chat));
	UAM_ASSERT_EQ(handoff.controls.size(), std::size_t{3});
}

UAM_TEST(ClaudeColdForeignObserverCannotMutateOrReleaseEitherRuntime)
{
	for (const std::string phase : {"initialize", "observed", "input-attempted"})
	{
		for (const std::string api : {"state", "dispatch", "consume"})
		{
			ProcessFreeClaudeObservation fixture;
			if (phase != "initialize")
			{
				const auto witness = fixture.Observe();
				if (phase == "input-attempted")
				{
					UAM_ASSERT(witness->Handoff(fixture.app, fixture.session, fixture.chat));
					UAM_ASSERT(witness->InputAttempted(fixture.app, fixture.session, fixture.chat));
				}
			}
			const auto original_state = uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat);
			const auto original_pending = fixture.session.pending_request_methods;
			uam::AcpSessionState foreign;
			foreign.claude_preinput_observation = fixture.session.claude_preinput_observation;
			foreign.native_pre_dispatch_runtime = fixture.session.native_pre_dispatch_runtime;
			foreign.initialize_request_id = 1;
			foreign.pending_request_methods[1] = "initialize";
			foreign.pending_request_methods[3] = "claude/preinput_get_settings";
			const auto foreign_pending = foreign.pending_request_methods;
			const nlohmann::json frame = {{"type", "assistant"}};
			if (api == "state")
			{
				UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, foreign, fixture.chat), uam::claude_preinput::State::Refused);
			}
			if (api == "dispatch") UAM_ASSERT(uam::claude_preinput::BeforeSharedDispatch(fixture.app, foreign, fixture.chat, &frame));
			if (api == "consume") UAM_ASSERT(uam::claude_preinput::Consume(fixture.app, foreign, fixture.chat, frame));
			UAM_ASSERT(foreign.pending_request_methods == foreign_pending);
			UAM_ASSERT_EQ(foreign.initialize_request_id, 1);
			UAM_ASSERT(!foreign.initialized && !foreign.session_ready && !foreign.stdin_writer);
			UAM_ASSERT(fixture.session.pending_request_methods == original_pending);
			UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat), original_state);
		}
	}
}

UAM_TEST(ClaudeColdRetiredObserverCannotRetireReplacementControlIds)
{
	for (const std::string drift : {"epoch", "writer", "process"})
	{
		ProcessFreeClaudeObservation fixture;
		const auto original_epoch = fixture.session.permission_review_incarnation;
		const auto original_writer = fixture.writer;
		const auto original_process = fixture.process;
		fixture.session.pending_request_methods[3] = "claude/preinput_get_settings";
		const auto replacement_pending = fixture.session.pending_request_methods;
		if (drift == "epoch") fixture.session.permission_review_incarnation = std::make_shared<const char>(0);
		if (drift == "writer") fixture.writer = std::make_shared<const char>(0);
		if (drift == "process") fixture.process = "injected-replacement-generation";
		UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat), uam::claude_preinput::State::Refused);
		UAM_ASSERT(fixture.session.pending_request_methods == replacement_pending);
		UAM_ASSERT_EQ(fixture.session.initialize_request_id, 1);
		fixture.session.permission_review_incarnation = original_epoch;
		fixture.writer = original_writer;
		fixture.process = original_process;
		UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat), uam::claude_preinput::State::Refused);
		UAM_ASSERT(fixture.session.pending_request_methods == replacement_pending);
		UAM_ASSERT_EQ(fixture.session.initialize_request_id, 1);
		UAM_ASSERT_EQ(fixture.controls.size(), std::size_t{1});
		UAM_ASSERT(!fixture.session.initialized && !fixture.session.session_ready && !fixture.session.stdin_writer);
	}
}

#endif


#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
#include "common/provider/claude_preinput_observation.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_polling.h"
#include "common/chat/chat_repository.h"
#include "common/utils/io_utils.h"
#include <type_traits>

static_assert(!std::is_default_constructible_v<uam::claude_preinput::NativeWitness>);
static_assert(!std::is_copy_constructible_v<uam::claude_preinput::NativeWitness>);
static_assert(!std::is_convertible_v<uam::claude_preinput::NativeTuple, uam::claude_preinput::NativeWitness>);

namespace
{
	// Actual private stdio process, synthetic native peer only. No CLI/account/network,
	// SSH/helper or installed-native availability claim. No pre-user system/init exists.
	struct ClaudePreinputPeerFixture
	{
		TempDir temp{"uam-claude-preinput-peer"};
		uam::AppState app;
		uam::AcpSessionState session;
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("claude-cli");
		const std::string native = "b3509391-8a4f-43e1-9d87-dd196b3a1697";
		const std::string selected_model = "claude-sonnet-4-6";
		std::vector<std::string> argv;
		std::string buffer, primary_before;
		std::size_t decoded_lines = 0;
		std::chrono::steady_clock::time_point deadline;
		std::vector<Message> messages_before;
		fs::path primary;
		ChatSession& chat() { return app.chats.front(); }

		explicit ClaudePreinputPeerFixture(const std::string& variant)
		{
			app.data_root = temp.root;
			ChatSession selected;
			selected.id = "private-preinput-query-owner"; selected.created_at = "2026-10-19T00:00:00.000Z";
			selected.provider_id = "claude-cli"; selected.execution_host_id = "local";
			selected.native_session_id = native; selected.model_id = selected_model;
			selected.approval_mode = "default"; selected.command_safety_tier = "off";
			selected.workspace_directory = temp.root.string(); selected.branch_root_chat_id = selected.id;
			selected.messages = {{MessageRole::User, "Retained original authored row", "2026-10-18T23:00:00.000Z"}};
			app.chats.push_back(selected); messages_before = chat().messages;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat()));
			primary = app.data_root / "chats" / (chat().id + ".json"); primary_before = ReadFile(primary);
			session.chat_id = chat().id; session.provider_id = chat().provider_id; session.session_id = native;
			session.process_execution_host_id = "local"; session.protocol_kind = "claude-code-stream-json";
			session.current_model_id = "unprojected-current-model"; session.current_mode_id = "unprojected-current-mode";
			const auto peer = temp.root / "peer.py";
			UAM_ASSERT(uam::io::WriteTextFile(peer, R"PEER(import json, os, pathlib, sys
variant, trace, native, model, release = sys.argv[1:]
trace, release = pathlib.Path(trace), pathlib.Path(release)
def emit(value):
    print(json.dumps(value), flush=True)
for line in sys.stdin:
    with trace.open('a') as out:
        out.write(line)
        out.flush()
        os.fsync(out.fileno())
    request = json.loads(line)
    if request.get('type') != 'control_request':
        (trace.parent / 'unexpected-input').write_text('refused')
        sys.exit(93)
    rid, subtype = request['request_id'], request['request']['subtype']
    assert isinstance(rid, str)
    assert subtype in ['initialize', 'get_status', 'get_settings']
    if subtype == 'initialize':
        if variant.startswith('usage-'):
            emit(dict(method='session/update', params=dict(sessionId=native, update=dict(sessionUpdate='usage_update', used=1, size=100))))
        if variant == 'malformed-under-owner-rejection':
            print('{private-malformed-native-frame', flush=True)
        payload = dict(claude_code_version='2.1.302', current_permission_mode='default', commands=[], models=[], pid=os.getpid())
        if variant == 'wrong-version': payload['claude_code_version'] = '2.1.301'
        if variant == 'missing-version': payload.pop('claude_code_version')
        if variant == 'wrong-mode': payload['current_permission_mode'] = 'plan'
        if variant == 'missing-mode': payload.pop('current_permission_mode')
        if variant == 'mode-type': payload['current_permission_mode'] = ['default']
    elif subtype == 'get_status':
        rows = [dict(label='Session ID', value=native)]
        if variant == 'foreign-id': rows[0]['value'] = '985a2388-728b-4ba7-87b6-e66efc9c5d0a'
        if variant == 'duplicate-id': rows.append(dict(rows[0]))
        if variant == 'missing-id': rows = [dict(label='Cloud session ID', value=native)]
        payload = dict(sections=[dict(title='Session', rows=rows)])
    else:
        payload = dict(applied=dict(model=model))
        if variant == 'wrong-model': payload['applied']['model'] = 'claude-opus-4-6'
        if variant == 'missing-model': payload['applied'].pop('model')
    response = dict(type='control_response', response=dict(subtype='success', request_id=rid, response=payload))
    if subtype == 'initialize':
        response['response']['pending_permission_requests'] = []
        response['response']['pending_user_dialog_requests'] = []
        if variant == 'pending-permission': response['response']['pending_permission_requests'] = [dict(request_id='foreign')]
        if variant == 'out-of-order': response['response']['request_id'] = str(int(rid) + 1)
        if variant == 'id-type': response['response']['request_id'] = int(rid)
        if variant == 'native-request': emit(dict(type='control_request', request_id='native', request=dict(subtype='hook_callback')))
        if variant == 'error': response['response']['subtype'] = 'error'
        if variant == 'unknown-success-shape': response['response']['subtype'] = 'pending'
        if variant == 'child-scope': response['parent_tool_use_id'] = 'foreign-child'
        if variant == 'foreign-envelope': response['session_id'] = '985a2388-728b-4ba7-87b6-e66efc9c5d0a'
        if variant == 'early-turn': emit(dict(type='assistant', session_id=native, message=dict(role='assistant', content=[])))
        if variant == 'early-init': emit(dict(type='system', subtype='init', session_id=native, model=model, permissionMode='default'))
    emit(response)
    if subtype == 'initialize' and variant == 'duplicate': emit(response)
    if subtype == 'get_settings' and variant == 'late-turn':
        import time
        while not release.exists(): time.sleep(.005)
        (trace.parent / 'late-turn-emitted').write_text('private-peer')
        emit(dict(type='assistant', session_id=native, message=dict(role='assistant', content=[])))
)PEER"));
			argv = {"/usr/bin/python3", "-u", peer.string(), variant, (temp.root / "stdin.jsonl").string(), native, selected_model, (temp.root / "release-turn").string()};
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, temp.root, argv));
			session.running = true;
		}
		~ClaudePreinputPeerFixture() { PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true); }

		void Begin()
		{
			deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			// No Poll/handler between actual initialization write and observation arm.
			// The first write creates the real platform writer; none is fabricated.
			UAM_ASSERT(uam::acp_detail::SendInitialize(session, nullptr));
			UAM_ASSERT(uam::claude_preinput::Arm(app, session, chat()));
			UAM_ASSERT(!uam::claude_preinput::CurrentTuple(app, session, chat()));
			UAM_ASSERT(!uam::claude_preinput::CurrentWitness(app, session, chat()));
		}
		void Pump()
		{
			char bytes[8192];
			for (;;)
			{
				const auto count = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStdout(session, bytes, sizeof(bytes));
				if (count == -2) break; // Documented nonblocking would-block; retain the absolute wait deadline.
				UAM_ASSERT(count >= 0); if (count == 0) break;
				buffer.append(bytes, static_cast<std::size_t>(count)); UAM_ASSERT(buffer.size() <= 1024 * 1024);
				for (auto end = buffer.find('\n'); end != std::string::npos; end = buffer.find('\n'))
				{
					const auto line = buffer.substr(0, end); buffer.erase(0, end + 1);
					UAM_ASSERT(uam::acp_detail::ProcessAcpLine(app, session, chat(), line, nullptr));
					++decoded_lines;
				}
			}
		}
		void Wait(uam::claude_preinput::State expected)
		{
			// One absolute fixture budget, also for late-turn observation; the native
			// observer's separate 25s expiry cannot satisfy a 20s refusal oracle.
			while (std::chrono::steady_clock::now() < deadline)
			{
				Pump();
				const auto state = uam::claude_preinput::CurrentState(app, session, chat());
				if (state == expected) return;
				UAM_ASSERT(state != uam::claude_preinput::State::Refused);
				std::this_thread::yield();
			}
			UAM_ASSERT(false);
		}
		void WaitDecoded(std::size_t count)
		{
			while (decoded_lines < count && std::chrono::steady_clock::now() < deadline) { Pump(); std::this_thread::yield(); }
			UAM_ASSERT(decoded_lines >= count);
		}
		void AssertUnprojected()
		{
			UAM_ASSERT(!session.initialized && !session.session_ready && !session.processing);
			UAM_ASSERT_EQ(session.current_model_id, std::string("unprojected-current-model"));
			UAM_ASSERT_EQ(session.current_mode_id, std::string("unprojected-current-mode"));
			UAM_ASSERT_EQ(session.session_id, native); UAM_ASSERT_EQ(chat().native_session_id, native);
			UAM_ASSERT_EQ(chat().model_id, selected_model); UAM_ASSERT_EQ(chat().approval_mode, std::string("default"));
			UAM_ASSERT_EQ(chat().messages, messages_before); UAM_ASSERT_EQ(ReadFile(primary), primary_before);
			UAM_ASSERT(!session.provider_usage.context_usage.available && session.provider_usage.context_usage.updated_at_ms == 0 &&
			    session.provider_usage.context_usage.used_tokens == 0 && session.provider_usage.context_usage.window_tokens == 0);
			UAM_ASSERT(app.pending_chat_save_tasks.empty()); UAM_ASSERT(!fs::exists(temp.root / "unexpected-input"));
			std::istringstream input(ReadFile(temp.root / "stdin.jsonl")); std::string line;
			const std::vector<std::string> expected = {"initialize", "get_status", "get_settings"}; std::size_t index = 0;
			while (std::getline(input, line))
			{
				const auto envelope = nlohmann::json::parse(line);
				UAM_ASSERT_EQ(envelope.at("type"), nlohmann::json("control_request"));
				const auto subtype = envelope.at("request").at("subtype").get<std::string>();
				UAM_ASSERT(index < expected.size()); UAM_ASSERT_EQ(subtype, expected[index++]);
			}
			if (uam::claude_preinput::CurrentState(app, session, chat()) == uam::claude_preinput::State::Observed) UAM_ASSERT_EQ(index, std::size_t{3});
		}
	};
}

UAM_TEST(ClaudePreinputQueriesRetainOnlySourceQualifiedNativeTupleWithoutTurnProjection)
{
	ClaudePreinputPeerFixture fixture("healthy"); fixture.Begin(); fixture.Wait(uam::claude_preinput::State::Observed);
	const auto observed = uam::claude_preinput::CurrentTuple(fixture.app, fixture.session, fixture.chat());
	UAM_ASSERT(observed); UAM_ASSERT_EQ(observed->NativeId(), fixture.native); UAM_ASSERT_EQ(observed->Model(), fixture.selected_model);
	UAM_ASSERT_EQ(observed->Mode(), std::string("default")); UAM_ASSERT_EQ(observed->Version(), std::string("2.1.302"));
	UAM_ASSERT(fixture.session.pending_request_methods.empty()); fixture.AssertUnprojected();
}

UAM_TEST(ClaudePreinputQueriesRefuseMalformedForeignAndEarlyTurnWithoutFallback)
{
	for (const std::string variant : {"wrong-version", "missing-version", "wrong-mode", "missing-mode", "mode-type", "pending-permission", "out-of-order", "id-type", "native-request", "error",
	    "unknown-success-shape", "child-scope", "foreign-envelope", "early-turn", "early-init", "duplicate", "foreign-id", "duplicate-id", "missing-id", "wrong-model", "missing-model"})
	{
		ClaudePreinputPeerFixture fixture(variant); fixture.Begin(); fixture.Wait(uam::claude_preinput::State::Refused);
		UAM_ASSERT(!uam::claude_preinput::CurrentTuple(fixture.app, fixture.session, fixture.chat()));
		UAM_ASSERT(!uam::claude_preinput::Arm(fixture.app, fixture.session, fixture.chat()));
		fixture.Pump(); UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat()), uam::claude_preinput::State::Refused);
		fixture.AssertUnprojected();
	}
}

UAM_TEST(ClaudePreinputObservedTupleRevokesForLateTurnActualStopAndReplacement)
{
	for (const std::string variant : {"late-turn", "stop", "replacement"})
	{
		ClaudePreinputPeerFixture fixture(variant); fixture.Begin(); fixture.Wait(uam::claude_preinput::State::Observed);
		const auto witness = uam::claude_preinput::CurrentWitness(fixture.app, fixture.session, fixture.chat());
		UAM_ASSERT(witness && witness->Current(fixture.app, fixture.session, fixture.chat()));
		if (variant == "late-turn")
		{
			UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "release-turn", "release")); fixture.Wait(uam::claude_preinput::State::Refused);
			UAM_ASSERT(fs::exists(fixture.temp.root / "late-turn-emitted"));
		}
		else
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(fixture.session, true);
			if (variant == "replacement") UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(fixture.session, fixture.temp.root, fixture.argv));
		}
		UAM_ASSERT(!uam::claude_preinput::CurrentTuple(fixture.app, fixture.session, fixture.chat()));
		UAM_ASSERT(!witness->Current(fixture.app, fixture.session, fixture.chat()));
		UAM_ASSERT_EQ(witness->Tuple().NativeId(), fixture.native); // Retained DATA survives loss of current proof.
		UAM_ASSERT(!uam::claude_preinput::CurrentWitness(fixture.app, fixture.session, fixture.chat()));
		UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat()), uam::claude_preinput::State::Refused);
		UAM_ASSERT(!uam::claude_preinput::Arm(fixture.app, fixture.session, fixture.chat())); fixture.AssertUnprojected();
	}
}

UAM_TEST(ClaudePreinputWitnessRequiresOriginalObserverAndDoesNotRetainItsLifetime)
{
	for (const bool reattach : {false, true})
	{
		ClaudePreinputPeerFixture fixture("healthy");
		UAM_ASSERT(!uam::claude_preinput::CurrentWitness(fixture.app, fixture.session, fixture.chat()));
		fixture.Begin(); fixture.Wait(uam::claude_preinput::State::Observed);
		const auto witness = uam::claude_preinput::CurrentWitness(fixture.app, fixture.session, fixture.chat());
		UAM_ASSERT(witness && witness->Current(fixture.app, fixture.session, fixture.chat()));
		const auto tuple = witness->Tuple(); // Freely copied DATA cannot manufacture a witness.
		UAM_ASSERT_EQ(tuple.NativeId(), fixture.native); UAM_ASSERT_EQ(tuple.Model(), fixture.selected_model);
		auto retained = reattach ? fixture.session.claude_preinput_observation : nullptr;
		const std::weak_ptr<uam::claude_preinput::Observation> observer = fixture.session.claude_preinput_observation;
		fixture.session.claude_preinput_observation.reset();
		UAM_ASSERT_EQ(observer.expired(), !reattach);
		UAM_ASSERT(!witness->Current(fixture.app, fixture.session, fixture.chat()));
		if (reattach)
		{
			fixture.session.claude_preinput_observation = std::move(retained);
			UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat()), uam::claude_preinput::State::Observed);
			UAM_ASSERT(!witness->Current(fixture.app, fixture.session, fixture.chat())); // Refused handle cannot revive.
		}
		fixture.AssertUnprojected();
	}
}

UAM_TEST(ClaudePreinputWitnessCannotAdoptCopiedOwnerOrTouchForeignRequests)
{
	ClaudePreinputPeerFixture fixture("healthy"); fixture.Begin(); fixture.Wait(uam::claude_preinput::State::Observed);
	const auto witness = uam::claude_preinput::CurrentWitness(fixture.app, fixture.session, fixture.chat());
	UAM_ASSERT(witness && witness->Current(fixture.app, fixture.session, fixture.chat()));
	uam::AcpSessionState other;
	other.chat_id = fixture.session.chat_id; other.provider_id = fixture.session.provider_id;
	other.session_id = fixture.session.session_id; other.protocol_kind = fixture.session.protocol_kind;
	other.claude_preinput_observation = fixture.session.claude_preinput_observation;
	other.native_pre_dispatch_runtime = fixture.session.native_pre_dispatch_runtime;
	other.initialize_request_id = 777; other.pending_request_methods.emplace(777, "foreign-control");
	UAM_ASSERT(!uam::claude_preinput::CurrentWitness(fixture.app, other, fixture.chat()));
	UAM_ASSERT(!witness->Current(fixture.app, other, fixture.chat()));
	UAM_ASSERT_EQ(other.initialize_request_id, 777);
	UAM_ASSERT_EQ(other.pending_request_methods.at(777), std::string("foreign-control"));
	UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat()), uam::claude_preinput::State::Observed);
	UAM_ASSERT(!witness->Current(fixture.app, fixture.session, fixture.chat()));
	fixture.AssertUnprojected();
}

UAM_TEST(ClaudePreinputWitnessRefusesOwnerDriftWithoutRevivalFromRestoredMetadata)
{
	for (const std::string field : {"root", "incarnation", "epoch", "model", "token"})
	{
		ClaudePreinputPeerFixture fixture("healthy"); fixture.Begin(); fixture.Wait(uam::claude_preinput::State::Observed);
		const auto witness = uam::claude_preinput::CurrentWitness(fixture.app, fixture.session, fixture.chat());
		UAM_ASSERT(witness && witness->Current(fixture.app, fixture.session, fixture.chat()));
		const auto incarnation = fixture.chat().metadata_incarnation;
		const auto epoch = fixture.session.permission_review_incarnation;
		if (field == "root")
		{
			fixture.app.data_root /= "foreign-root";
		}
		else if (field == "incarnation")
		{
			fixture.chat().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		}
		else if (field == "epoch")
		{
			fixture.session.permission_review_incarnation = std::make_shared<const char>(0);
		}
		else if (field == "model")
		{
			fixture.chat().model_id = "foreign-model";
		}
		else
		{
			fixture.session.remote_output_delivery_token = "foreign-token";
		}
		UAM_ASSERT(!witness->Current(fixture.app, fixture.session, fixture.chat()));
		fixture.app.data_root = fixture.temp.root; fixture.chat().metadata_incarnation = incarnation;
		fixture.session.permission_review_incarnation = epoch; fixture.chat().model_id = fixture.selected_model;
		fixture.session.remote_output_delivery_token.clear();
		UAM_ASSERT(!witness->Current(fixture.app, fixture.session, fixture.chat()));
		UAM_ASSERT(!uam::claude_preinput::CurrentWitness(fixture.app, fixture.session, fixture.chat()));
		UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat()), uam::claude_preinput::State::Refused);
		fixture.AssertUnprojected();
	}
}

UAM_TEST(ClaudePreinputQueriesRefuseUnsettledSetupAndSelectedOwnerDrift)
{
	for (const std::string control : {"fast", "reasoning", "model-drift", "mode-drift"})
	{
		ClaudePreinputPeerFixture fixture("healthy");
		if (control == "fast" || control == "reasoning")
		{
			if (control == "fast") { fixture.chat().service_tier = "fast"; fixture.chat().service_tier_explicit = true; }
			else fixture.chat().reasoning_effort = "high";
			UAM_ASSERT(uam::acp_detail::SendInitialize(fixture.session, nullptr));
			UAM_ASSERT(!uam::claude_preinput::Arm(fixture.app, fixture.session, fixture.chat()));
			UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat()), uam::claude_preinput::State::Refused);
			fixture.Pump(); fixture.AssertUnprojected();
			UAM_ASSERT_EQ(fixture.chat().service_tier, control == "fast" ? std::string("fast") : std::string{});
			UAM_ASSERT_EQ(fixture.chat().reasoning_effort, control == "reasoning" ? std::string("high") : std::string{});
		}
		else
		{
			fixture.Begin(); fixture.Wait(uam::claude_preinput::State::Observed);
			if (control == "model-drift") fixture.chat().model_id = "different-authored-model";
			else fixture.chat().approval_mode = "plan";
			UAM_ASSERT(!uam::claude_preinput::CurrentTuple(fixture.app, fixture.session, fixture.chat()));
			UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat()), uam::claude_preinput::State::Refused);
			UAM_ASSERT_EQ(fixture.chat().model_id, control == "model-drift" ? std::string("different-authored-model") : fixture.selected_model);
			UAM_ASSERT_EQ(fixture.chat().approval_mode, control == "mode-drift" ? std::string("plan") : std::string("default"));
			UAM_ASSERT_EQ(fixture.chat().messages, fixture.messages_before); UAM_ASSERT_EQ(ReadFile(fixture.primary), fixture.primary_before);
			UAM_ASSERT(!fixture.session.initialized && !fixture.session.session_ready);
		}
		UAM_ASSERT(!uam::claude_preinput::Arm(fixture.app, fixture.session, fixture.chat()));
	}
}

UAM_TEST(ClaudePreinputDispatcherRefusesSharedUsageBeforeAnyProjectionOrRepair)
{
	for (const std::string variant : {"usage-then-healthy", "usage-owner-drift"})
	{
		ClaudePreinputPeerFixture fixture(variant); fixture.Begin();
		const IProviderRuntime* const captured = fixture.session.native_pre_dispatch_runtime;
		UAM_ASSERT(captured != nullptr);
		if (variant == "usage-owner-drift")
		{
			fixture.chat().provider_id = "codex-cli";
			fixture.session.provider_id = "codex-cli";
		}
		// Real peer emits usage first then the actual pending initialize response.
		// Both raw frames traverse ProcessAcpLine, even after the first is refused.
		fixture.WaitDecoded(2);
		const auto trace = ReadFile(fixture.temp.root / "stdin.jsonl");
		UAM_ASSERT_EQ(static_cast<std::size_t>(std::count(trace.begin(), trace.end(), '\n')), std::size_t{1});
		UAM_ASSERT_EQ(fixture.session.native_pre_dispatch_runtime, captured);
		UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat()), uam::claude_preinput::State::Refused);
		UAM_ASSERT(!uam::claude_preinput::CurrentTuple(fixture.app, fixture.session, fixture.chat()));
		UAM_ASSERT(!uam::claude_preinput::Arm(fixture.app, fixture.session, fixture.chat()));
		fixture.AssertUnprojected();
		if (variant == "usage-owner-drift") UAM_ASSERT_EQ(fixture.chat().provider_id, std::string("codex-cli"));
	}
}

UAM_TEST(ClaudePreinputDispatcherMalformedOwnershipRefusalCannotRepairLiveTransport)
{
	ClaudePreinputPeerFixture fixture("malformed-under-owner-rejection"); fixture.Begin();
	const std::weak_ptr<uam::platform::AsyncByteWriter> writer = fixture.session.stdin_writer;
	const std::string actual_process = uam::acp_detail::AcpProcessHandleLabel(fixture.session);
	fixture.session.last_process_id = "deliberately-different-recorded-process";
	fixture.WaitDecoded(2); // malformed raw frame, then real pending initialize reply
	const auto trace = ReadFile(fixture.temp.root / "stdin.jsonl");
	UAM_ASSERT_EQ(static_cast<std::size_t>(std::count(trace.begin(), trace.end(), '\n')), std::size_t{1});
	UAM_ASSERT(!fixture.session.transport_ownership_error.empty());
	UAM_ASSERT_EQ(fixture.session.lifecycle_state, std::string(uam::acp_detail::kAcpLifecycleError));
	UAM_ASSERT(fixture.session.running && !writer.expired() && writer.lock() == fixture.session.stdin_writer);
	UAM_ASSERT_EQ(uam::acp_detail::AcpProcessHandleLabel(fixture.session), actual_process);
	UAM_ASSERT(!PlatformServicesFactory::Instance().process_service.PollStdioProcessExited(fixture.session));
	UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat()), uam::claude_preinput::State::Refused);
	UAM_ASSERT(!uam::claude_preinput::CurrentTuple(fixture.app, fixture.session, fixture.chat()));
	UAM_ASSERT(!uam::claude_preinput::Arm(fixture.app, fixture.session, fixture.chat())); fixture.AssertUnprojected();
}

UAM_TEST(ClaudePreinputObservedTupleRejectsLatchedOwnershipFaultWithoutAnotherFrame)
{
	ClaudePreinputPeerFixture fixture("healthy"); fixture.Begin(); fixture.Wait(uam::claude_preinput::State::Observed);
	fixture.session.last_process_id = "deliberately-different-recorded-process";
	uam::acp_detail::InvalidateAcpTransport(fixture.app, fixture.session, fixture.chat(), "Private observed transport fault.");
	UAM_ASSERT(fixture.session.running && fixture.session.stdin_writer);
	UAM_ASSERT(!fixture.session.transport_ownership_error.empty());
	UAM_ASSERT_EQ(fixture.session.lifecycle_state, std::string(uam::acp_detail::kAcpLifecycleError));
	// No new decoded-frame refusal can mask the freshness check in this arm.
	UAM_ASSERT(!uam::claude_preinput::CurrentTuple(fixture.app, fixture.session, fixture.chat()));
	UAM_ASSERT_EQ(uam::claude_preinput::CurrentState(fixture.app, fixture.session, fixture.chat()), uam::claude_preinput::State::Refused);
	fixture.AssertUnprojected();
}

UAM_TEST(AcpPredispatchAbsentPreservesSharedUsageAcrossAllProviders)
{
	for (const char* provider : uam::provider_ids::kAllCliProviderIds)
	{
		uam::AppState app; uam::AcpSessionState session; ChatSession chat;
		chat.id = "ordinary-context-only"; chat.provider_id = provider;
		chat.native_session_id = "b3509391-8a4f-43e1-9d87-dd196b3a1697";
		session.chat_id = chat.id; session.provider_id = provider; session.session_id = chat.native_session_id;
		UAM_ASSERT(session.native_pre_dispatch_runtime == nullptr);
		const auto frame = nlohmann::json{{"method", "session/update"}, {"params", {{"sessionId", chat.native_session_id},
		    {"update", {{"sessionUpdate", "usage_update"}, {"used", 1}, {"size", 100}}}}}};
		UAM_ASSERT(uam::acp_detail::ProcessAcpLine(app, session, chat, frame.dump(), nullptr));
		UAM_ASSERT(session.provider_usage.context_usage.available);
		UAM_ASSERT_EQ(session.provider_usage.context_usage.used_tokens, std::int64_t{1});
		UAM_ASSERT_EQ(session.provider_usage.context_usage.window_tokens, std::int64_t{100});
		UAM_ASSERT(!session.initialized && !session.session_ready && chat.messages.empty());
	}
}

#endif
