#include "test_harness.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

using namespace uam_test;

namespace
{
	nlohmann::json Inspect(uam::remote::RunnerState& state, const std::string& path, bool missing = false)
	{
		return uam::remote::HandleRunnerRequest({{"id", "inspect"}, {"type", "path.inspect"},
		    {"path", path}, {"allowMissingLeaf", missing}}, "4.11.0", &state);
	}

	fs::path Runner()
	{
		return PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().parent_path() /
#if defined(_WIN32)
		    "uam-runner.exe";
#else
		    "uam-runner";
#endif
	}
}

UAM_TEST(RemotePathInspectionProvesNativeTypesAndOnlyAMissingLeaf)
{
	TempDir temp("uam-path-inspection");
	const fs::path root = fs::canonical(temp.root);
	const fs::path file = root / ".git";
	UAM_ASSERT(uam::io::WriteTextFile(file, "gitdir: another directory\n"));
	uam::remote::RunnerState state;
	const nlohmann::json hello = uam::remote::HandleRunnerRequest({{"id", "hello"}, {"type", "hello"},
	    {"protocolVersion", 3}, {"nonce", "inspection"}}, "4.11.0", &state);
	UAM_ASSERT(hello["capabilities"].value("safePathInspection", false));
	UAM_ASSERT(!uam::remote::HandleRunnerRequest({{"id", "hello"}, {"type", "hello"},
	    {"protocolVersion", 3}, {"nonce", "inspection"}}, "4.11.0")["capabilities"].value("safePathInspection", true));
	const nlohmann::json directory = Inspect(state, uam::paths::Utf8PathString(root));
	UAM_ASSERT(directory.value("ok", false));
	UAM_ASSERT(directory["result"].value("exists", false));
	UAM_ASSERT(directory["result"].value("directory", false));
	UAM_ASSERT(!directory["result"].value("regularFile", true));
	const nlohmann::json marker = Inspect(state, uam::paths::Utf8PathString(file));
	UAM_ASSERT(marker.value("ok", false));
	UAM_ASSERT(marker["result"].value("regularFile", false));
	UAM_ASSERT(!marker["result"].value("directory", true));
	const fs::path absent = root / "fresh-worktree";
	UAM_ASSERT(!Inspect(state, uam::paths::Utf8PathString(absent)).value("ok", true));
	const nlohmann::json missing = Inspect(state, uam::paths::Utf8PathString(absent), true);
	UAM_ASSERT(missing.value("ok", false));
	UAM_ASSERT(!missing["result"].value("exists", true));
	UAM_ASSERT(!missing["result"].value("directory", true));
	UAM_ASSERT(!missing["result"].value("regularFile", true));
	UAM_ASSERT(!Inspect(state, uam::paths::Utf8PathString(absent / "nested"), true).value("ok", true));
	UAM_ASSERT(!Inspect(state, uam::paths::Utf8PathString(file / "child"), true).value("ok", true));
	UAM_ASSERT(!fs::exists(absent));
	UAM_ASSERT_EQ(ReadFile(file), std::string("gitdir: another directory\n"));
}

UAM_TEST(RemotePathInspectionRejectsAmbiguousAndSpecialNativePaths)
{
	TempDir temp("uam-path-inspection-unsafe");
	const fs::path root = fs::canonical(temp.root);
	uam::remote::RunnerState state;
	for (const std::string& path : std::vector<std::string>{"", "relative", "C:relative", "\\\\?\\C:\\device", "\\\\.\\NUL",
	    uam::paths::Utf8PathString(root / "." / "missing"), uam::paths::Utf8PathString(root / ".." / "missing"),
	    uam::paths::Utf8PathString(root / "line\nbreak"), std::string(4097, 'x')})
		UAM_ASSERT(!Inspect(state, path, true).value("ok", true));
	UAM_ASSERT(!uam::remote::HandleRunnerRequest({{"id", "bad"}, {"type", "path.inspect"}, {"path", 7},
	    {"allowMissingLeaf", true}}, "4.11.0", &state).value("ok", true));
	UAM_ASSERT(!uam::remote::HandleRunnerRequest({{"id", "bad"}, {"type", "path.inspect"},
	    {"path", uam::paths::Utf8PathString(root)}, {"allowMissingLeaf", "yes"}}, "4.11.0", &state).value("ok", true));
#if defined(_WIN32)
	for (const std::string& name : {"NUL", "con.txt", "COM1", "LPT9.txt", "trailing.", "trailing ", "file:stream", "wild*card"})
		UAM_ASSERT(!Inspect(state, uam::paths::Utf8PathString(root / name), true).value("ok", true));
	const nlohmann::json drive_root = Inspect(state, uam::paths::Utf8PathString(root.root_path()));
	UAM_ASSERT(drive_root.value("ok", false));
	UAM_ASSERT(drive_root["result"].value("directory", false));
	UAM_ASSERT(uam::execution_hosts::RemotePathsMatch("windows", drive_root["result"].value("path", ""), uam::paths::Utf8PathString(root.root_path())));
	const fs::path child = root / "normal child";
	fs::create_directory(child);
	const nlohmann::json child_proof = Inspect(state, uam::paths::Utf8PathString(child));
	UAM_ASSERT(child_proof.value("ok", false));
	UAM_ASSERT(child_proof["result"].value("exists", false));
	UAM_ASSERT(child_proof["result"].value("directory", false));
	UAM_ASSERT(uam::execution_hosts::RemotePathsMatch("windows", child_proof["result"].value("path", ""), uam::paths::Utf8PathString(child)));
	const fs::path junction = root / "junction";
	UAM_ASSERT(RunTestCommand("cmd.exe /d /c mklink /J " + ShellQuoteForTest(uam::paths::Utf8PathString(junction)) +
	    " " + ShellQuoteForTest(uam::paths::Utf8PathString(root))));
	UAM_ASSERT(!Inspect(state, uam::paths::Utf8PathString(junction), true).value("ok", true));
	UAM_ASSERT(!Inspect(state, uam::paths::Utf8PathString(junction / "absent"), true).value("ok", true));
	fs::remove(junction);
#else
	const fs::path linked = root / "linked";
	fs::create_directory_symlink(root, linked);
	UAM_ASSERT(!Inspect(state, uam::paths::Utf8PathString(linked), true).value("ok", true));
	UAM_ASSERT(!Inspect(state, uam::paths::Utf8PathString(linked / "missing"), true).value("ok", true));
	fs::create_symlink(root / "absent", root / "dangling");
	UAM_ASSERT(!Inspect(state, uam::paths::Utf8PathString(root / "dangling"), true).value("ok", true));
	UAM_ASSERT(mkfifo((root / "fifo").c_str(), 0600) == 0);
	UAM_ASSERT(!Inspect(state, uam::paths::Utf8PathString(root / "fifo")).value("ok", true));
#endif
}

UAM_TEST(RemotePathInspectionClientUsesThePrivateRunnerAndHonorsCancellation)
{
	TempDir temp("uam-path-inspection-client");
	const fs::path root = fs::canonical(temp.root);
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	uam::remote::RunnerClient client(service, {uam::paths::Utf8PathString(Runner()), "bridge-direct"});
	uam::remote::PathInspection proof;
	std::string error;
	UAM_ASSERT(client.InspectPath(uam::paths::Utf8PathString(root), false, proof, &error));
	UAM_ASSERT(proof.exists && proof.directory && !proof.regular_file);
	UAM_ASSERT(client.InspectPath(uam::paths::Utf8PathString(root / "absent"), true, proof, &error));
	UAM_ASSERT(!proof.exists && !proof.directory && !proof.regular_file);
	std::stop_source stop;
	stop.request_stop();
	UAM_ASSERT(!client.InspectPath(uam::paths::Utf8PathString(root), false, proof, &error, stop.get_token()));
	UAM_ASSERT(proof.path.empty());
}

UAM_TEST(RemotePathInspectionRecoveryRequiresTypedOwnedMissingProof)
{
	TempDir temp("uam-path-owned-recovery");
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	uam::remote::RunnerClient client(service, {uam::paths::Utf8PathString(Runner()), "bridge-direct"});
	std::string error;
	UAM_ASSERT(client.Connect(&error));
	uam::remote::ProcessPollResult result;
	bool missing = true;
	UAM_ASSERT(!client.PollProcess("absent", result, &error, {}, nullptr, nullptr, &missing));
	UAM_ASSERT(!missing);
	client.SetProcessControlToken("absent", "retained-original-token");
	UAM_ASSERT(!client.PollProcess("absent", result, &error, {}, nullptr, nullptr, &missing));
	UAM_ASSERT(missing);
	UAM_ASSERT(client.StartProcess("owned", fs::canonical(temp.root), {uam::paths::Utf8PathString(Runner()), "--version"},
	    {}, &error, false, "owned-token"));
	client.SetProcessControlToken("owned", "foreign-token");
	missing = true;
	UAM_ASSERT(!client.PollProcess("owned", result, &error, {}, nullptr, nullptr, &missing));
	UAM_ASSERT(!missing);
	client.SetProcessControlToken("owned", "owned-token");
	UAM_ASSERT(client.StopProcess("owned", &error));
	UAM_ASSERT(client.RemoveProcess("owned", &error));
	client.Disconnect();
	missing = true;
	UAM_ASSERT(!client.PollProcess("absent", result, &error, {}, nullptr, nullptr, &missing));
	UAM_ASSERT(!missing);
}

UAM_TEST(RemoteRetainedCommandExitKeepsThePrivateRunnerRecordUntilSettlement)
{
	TempDir temp("uam-retained-command-exit");
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	uam::remote::RunnerClient client(service, {uam::paths::Utf8PathString(Runner()), "bridge-direct"});
	const ProcessExecutionResult command = client.ExecuteCommand("retained-version", fs::canonical(temp.root),
	    {uam::paths::Utf8PathString(Runner()), "--version"}, 5000, {}, {}, "durable-version-token");
	UAM_ASSERT(command.ok);
	uam::remote::ProcessPollResult observed;
	std::string error;
	UAM_ASSERT(client.PollProcess("retained-version", observed, &error));
	UAM_ASSERT(!observed.running);
	UAM_ASSERT_EQ(observed.exit_code, 0);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "settled.json", "{\"state\":\"completed\"}"));
	UAM_ASSERT(client.RemoveProcess("retained-version", &error));
	client.SetProcessControlToken("retained-version", "durable-version-token");
	bool missing = false;
	UAM_ASSERT(!client.PollProcess("retained-version", observed, &error, {}, nullptr, nullptr, &missing));
	UAM_ASSERT(missing);
}

#if !defined(_WIN32)
UAM_TEST(RemotePathInspectionClientRejectsOldCapabilitiesAndMalformedProofs)
{
	TempDir temp("uam-path-inspection-wire");
	const fs::path script = temp.root / "fixture.py";
	UAM_ASSERT(uam::io::WriteTextFile(script, R"(import json,struct,sys
mode=sys.argv[1]
def read():
 h=sys.stdin.buffer.read(4)
 return json.loads(sys.stdin.buffer.read(struct.unpack('>I',h)[0])) if len(h)==4 else None
def send(v):
 b=json.dumps(v).encode();sys.stdout.buffer.write(struct.pack('>I',len(b))+b);sys.stdout.buffer.flush()
r=read()
send(dict(id=r['id'],ok=True,nonce=r['nonce'],protocolVersion=3,runnerVersion='development',capabilities=dict(processExecution=True,fileCopy=True,computerUse=False,safePathInspection=mode!='old')))
r=read()
if r:
 p=dict(path='/safe',exists=True,directory=True,regularFile=False)
 if mode=='type': p['exists']='true'
 if mode=='both': p['regularFile']=True
 if mode=='missing': p.update(exists=False,directory=False)
 if mode=='relative': p['path']='relative'
 send(dict(id=r['id'],ok=True,result=p))
)"));
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	for (const std::string& mode : {"old", "type", "both", "missing", "relative"})
	{
		uam::remote::RunnerClient client(service, {"python3", script.string(), mode});
		uam::remote::PathInspection proof;
		std::string error;
		UAM_ASSERT(!client.InspectPath("/safe", false, proof, &error));
		UAM_ASSERT(proof.path.empty());
		UAM_ASSERT(error.find(mode == "old" ? "Update this SSH helper" : "invalid") != std::string::npos);
	}
}

UAM_TEST(RemoteRetainedCommandKeepsOwnedSettlementAcrossExitTimeoutAndCancel)
{
	TempDir temp("uam-retained-command-wire");
	const fs::path script = temp.root / "retained.py";
	UAM_ASSERT(uam::io::WriteTextFile(script, R"(import json,struct,sys,os
mode,log,settled=sys.argv[1:]
records={}
def read():
 h=sys.stdin.buffer.read(4)
 return json.loads(sys.stdin.buffer.read(struct.unpack('>I',h)[0])) if len(h)==4 else None
def send(v):
 b=json.dumps(v).encode();sys.stdout.buffer.write(struct.pack('>I',len(b))+b);sys.stdout.buffer.flush()
while True:
 r=read()
 if r is None: break
 with open(log,'a') as f: f.write(json.dumps(r)+'\n')
 t=r['type'];result={};ok=True
 if t=='hello':
  send(dict(id=r['id'],ok=True,nonce=r['nonce'],protocolVersion=3,runnerVersion='development',capabilities=dict(processExecution=True,fileCopy=True,computerUse=False,processOutputAcknowledgement=True)))
  continue
 if t=='process.start': records[r['sessionId']]=r['controlToken'];result={'inputSequence':0}
 elif r.get('sessionId') not in records or records[r['sessionId']]!=r.get('controlToken'): ok=False
 elif t=='process.poll':
  with open(log+'.polled','w') as f: f.write('observed')
  running=mode not in ('exit','default-exit')
  result=dict(running=running,exitCode=0,stdoutBase64='',stderrBase64='',stdoutCursor=0,stderrCursor=0)
 elif t=='process.remove':
  ok=mode.startswith('default') or os.path.exists(settled)
  if ok: del records[r['sessionId']]
 send(dict(id=r['id'],ok=ok,result=result,error={'code':'fixture_guard','message':'settlement not durable'}))
)"));
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	for (const std::string& mode : {"exit", "timeout", "cancel", "default-exit", "default-timeout"})
	{
		const fs::path log = temp.root / (mode + ".jsonl");
		const fs::path settled = temp.root / (mode + ".settled");
		uam::remote::RunnerClient client(service, {"python3", script.string(), mode, log.string(), settled.string()});
		std::string error;
		UAM_ASSERT(client.Connect(&error));
		std::stop_source cancellation;
		std::jthread cancel;
		if (mode == "cancel")
			cancel = std::jthread([&](std::stop_token stop)
			{
				const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
				{
					if (fs::exists(log.string() + ".polled")) { cancellation.request_stop(); return; }
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
			});
		const bool retained = !mode.starts_with("default");
		const ProcessExecutionResult result = client.ExecuteCommand("owned-mutation", fs::canonical(temp.root), {"fixture"},
		    mode == "cancel" ? 5000 : 1000, cancellation.get_token(), {}, retained ? "retained-token" : "");
		if (mode == "exit" || mode == "default-exit") UAM_ASSERT(result.ok);
		else if (mode == "cancel") UAM_ASSERT(result.canceled);
		else UAM_ASSERT(result.timed_out);
		int starts = 0, stops = 0, removes = 0;
		std::istringstream events(ReadFile(log));
		std::string line;
		while (std::getline(events, line))
		{
			const nlohmann::json event = nlohmann::json::parse(line);
			const std::string type = event.value("type", "");
			if (type == "process.start")
			{
				++starts;
				UAM_ASSERT_EQ(event.contains("transientLeaseMs"), !retained);
				if (retained) UAM_ASSERT_EQ(event.value("controlToken", ""), std::string("retained-token"));
			}
			if (type == "process.stop") ++stops;
			if (type == "process.remove") ++removes;
		}
		UAM_ASSERT_EQ(starts, 1);
		UAM_ASSERT_EQ(stops, mode == "default-timeout" ? 1 : 0);
		UAM_ASSERT_EQ(removes, retained ? 0 : 1);
		if (mode == "exit")
		{
			uam::remote::ProcessPollResult observed;
			UAM_ASSERT(client.PollProcess("owned-mutation", observed, &error));
			UAM_ASSERT(!observed.running);
			UAM_ASSERT(!client.RemoveProcess("owned-mutation", &error));
			UAM_ASSERT(uam::io::WriteTextFile(settled, "completed"));
			UAM_ASSERT(client.RemoveProcess("owned-mutation", &error));
		}
	}
}
#endif
