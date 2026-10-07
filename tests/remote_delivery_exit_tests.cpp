#include "test_harness.h"
#include "remote/runner_state.h"

using namespace uam_test;

UAM_TEST(RemoteExitedProviderAcknowledgesOnlyRememberedDeliveryAndRetainsCompletion)
{
	TempDir temp("uam-remote-exited-delivery");
	uam::remote::RunnerState state;
	const std::string session_id = "exited-delivery";
	const std::string token = "exited-delivery-authority";
#if defined(_WIN32)
	const std::vector<std::string> arguments = {
	    "cmd.exe", "/d", "/s", "/c", "set /p line= & echo completed-once& exit /b 0"};
#else
	const std::vector<std::string> arguments = {
	    "/bin/sh", "-c", "IFS= read -r line; printf 'completed-once\\n'; exit 0"};
#endif
	const nlohmann::json started = state.HandleProcessRequest(
	    {{"id", "start"}, {"type", "process.start"}, {"sessionId", session_id},
	     {"controlToken", token}, {"cwd", temp.root.string()}, {"argv", arguments}});
	UAM_ASSERT(started.value("ok", false));
	nlohmann::json delivery = {
	    {"id", "first"}, {"type", "process.write"}, {"sessionId", session_id},
	    {"controlToken", token}, {"dataBase64", uam::base64::Encode("accepted-once\n")},
	    {"deliveryId", "prompt-17.0.1"}, {"inputSequence", std::uint64_t{1}}};
	const nlohmann::json accepted = state.HandleProcessRequest(delivery);
	UAM_ASSERT(accepted.value("ok", false));
	UAM_ASSERT(!accepted["result"].value("duplicate", false));

	const nlohmann::json poll = {
	    {"id", "poll"}, {"type", "process.poll"}, {"sessionId", session_id},
	    {"controlToken", token}, {"acknowledgedOutput", true}};
	nlohmann::json completion;
	bool exited = false;
	for (int attempt = 0; attempt < 200 && !exited; ++attempt)
	{
		completion = state.HandleProcessRequest(poll);
		UAM_ASSERT(completion.value("ok", false));
		exited = !completion["result"].value("running", true);
		if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(exited);
	std::string output;
	UAM_ASSERT(uam::base64::Decode(completion["result"].value("stdoutBase64", ""), output));
	UAM_ASSERT_EQ(uam::strings::Trim(output), std::string("completed-once"));

	// Simulate the lost receipt: the same logical delivery is retried, never a new prompt.
	delivery["id"] = "replay";
	delivery["inputSequence"] = std::uint64_t{2};
	const nlohmann::json replay = state.HandleProcessRequest(delivery);
	UAM_ASSERT(replay.value("ok", false));
	UAM_ASSERT(replay["result"].value("duplicate", false));
	UAM_ASSERT_EQ(replay["result"].value("inputSequence", 0), 1);

	delivery["dataBase64"] = uam::base64::Encode("different\n");
	const nlohmann::json conflict = state.HandleProcessRequest(delivery);
	UAM_ASSERT(!conflict.value("ok", true));
	UAM_ASSERT_EQ(conflict["error"].value("code", ""), std::string("input_conflict"));
	delivery["dataBase64"] = uam::base64::Encode("accepted-once\n");
	delivery["controlToken"] = "wrong-authority";
	const nlohmann::json unauthorized = state.HandleProcessRequest(delivery);
	UAM_ASSERT(!unauthorized.value("ok", true));
	UAM_ASSERT_EQ(unauthorized["error"].value("code", ""), std::string("unauthorized"));
	delivery["controlToken"] = token;
	delivery["deliveryId"] = "new-prompt";
	const nlohmann::json fresh = state.HandleProcessRequest(delivery);
	UAM_ASSERT(!fresh.value("ok", true));
	UAM_ASSERT_EQ(fresh["error"].value("code", ""), std::string("process_exited"));
	delivery["inputSequence"] = std::uint64_t{1};
	const nlohmann::json fresh_same_sequence = state.HandleProcessRequest(delivery);
	UAM_ASSERT(!fresh_same_sequence.value("ok", true));
	UAM_ASSERT_EQ(fresh_same_sequence["error"].value("code", ""), std::string("process_exited"));

	const nlohmann::json retained = state.HandleProcessRequest(poll);
	UAM_ASSERT(retained.value("ok", false));
	UAM_ASSERT_EQ(retained["result"]["stdoutBase64"], completion["result"]["stdoutBase64"]);
	UAM_ASSERT_EQ(retained["result"].value("inputSequence", 0), 1);
	const nlohmann::json acknowledged = state.HandleProcessRequest(
	    {{"id", "ack"}, {"type", "process.ack"}, {"sessionId", session_id},
	     {"controlToken", token}, {"stdoutCursor", retained["result"]["stdoutCursor"]},
	     {"stderrCursor", retained["result"]["stderrCursor"]}});
	UAM_ASSERT(acknowledged.value("ok", false));
	const nlohmann::json drained = state.HandleProcessRequest(poll);
	UAM_ASSERT(drained.value("ok", false));
	UAM_ASSERT(!drained["result"].value("running", true));
	UAM_ASSERT_EQ(drained["result"].value("stdoutBase64", ""), std::string(""));
	UAM_ASSERT_EQ(drained["result"].value("inputSequence", 0), 1);
	const nlohmann::json removed = state.HandleProcessRequest(
	    {{"id", "remove"}, {"type", "process.remove"}, {"sessionId", session_id}, {"controlToken", token}});
	UAM_ASSERT(removed.value("ok", false));
}
